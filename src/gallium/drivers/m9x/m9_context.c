/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * M9 rendering context.  The Gallium draw module transforms vertices on the
 * CPU and hands window-space primitives to a vbuf render, which batches them
 * into two-pass pass-through jobs (m9_draw.c) rasterized by the hardware.
 * Fragment shaders run as native programs from m9_fs_compile; the verified
 * interpolated-color pass-through keeps its captured image.
 */
#include "m9_screen.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include "draw/draw_context.h"
#include "draw/draw_vbuf.h"
#include "draw/draw_vertex.h"
#include "indices/u_indices.h"
#include "m9_batch.h"
#include "m9_draw.h"
#include "m9_format.h"
#include "m9_fs_compile.h"
#include "m9_internal.h"
#include "m9_passthrough.h"
#include "m9_state.h"
#include "nir.h"
#include "tgsi/tgsi_from_mesa.h"
#include "util/u_debug.h"
#include "util/u_debug_cb.h"
#include "util/format/u_format.h"
#include "util/u_draw.h"
#include "util/u_framebuffer.h"
#include "util/u_helpers.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_pack_color.h"
#include "util/u_prim.h"
#include "util/u_surface.h"
#include "util/u_transfer.h"
#include "util/u_upload_mgr.h"

/* Source record: x, y bottom-up, z in ortho object space, r, g, b, a. */
#define M9_RECORD_FLOATS 7u

enum m9_warning {
   M9_WARN_TARGET = 1u << 0,
   M9_WARN_FS = 1u << 1,
   M9_WARN_BLEND = 1u << 2,
   M9_WARN_DEPTH = 1u << 3,
   M9_WARN_STENCIL = 1u << 4,
   M9_WARN_POINTS = 1u << 5,
   M9_WARN_BLIT = 1u << 6,
   M9_WARN_CLEAR = 1u << 7,
   M9_WARN_TRANSFER = 1u << 8,
   M9_WARN_MEMORY = 1u << 9,
   M9_WARN_TEXTURE = 1u << 10,
};

struct m9_transfer {
   struct pipe_transfer base;
   uint8_t *staging;  /* de-tiled rows of a tiled render target */
};

struct m9_fs_state {
   struct draw_fragment_shader *draw;
   nir_shader *nir;
   bool passthrough;
   bool native, warned;
   const char *reason;
   struct util_dynarray variants;      /* struct m9_fs_variant * */
};

/* A fragment state specialised on blend state and inlined uniform values. */
struct m9_fs_variant {
   struct m9_fs_blend blend;
   uint32_t inline_values[MAX_INLINABLE_UNIFORMS];
   bool ok;
   const char *reason;                 /* why the variant is not drawable */
   struct m9_fs_program program;
   struct m9_resource c2;              /* fragment slots, then vertex program */
   unsigned vertex_entry;
   unsigned extra_count;               /* CPU view locations: varyings, then coordinates */
   uint8_t extra_location[M9_FS_MAX_VARYINGS + M9_FS_MAX_TEXTURES];
   unsigned coord_count;               /* distinct texture coordinate streams */
   uint8_t coord_location[M9_FS_MAX_TEXTURES];
   uint8_t texture_coord[M9_FS_MAX_TEXTURES];  /* coordinate stream per texel pair */
};

struct m9_velems {
   unsigned count;
   struct pipe_vertex_element elements[PIPE_MAX_ATTRIBS];
};

struct m9_vbuf_render {
   struct vbuf_render base;
   struct m9_context *ctx;
   struct vertex_info vinfo;
   enum mesa_prim prim;
   uint8_t *vertices;
   size_t capacity;
   uint16_t vertex_size;
   bool has_color;
   unsigned extra_count;
   int extra_offset[M9_FS_MAX_VARYINGS + M9_FS_MAX_TEXTURES];  /* float offset, -1 if unwritten */
   struct util_dynarray indices;
};

static int m9_submit_job(struct m9_context *ctx);

static inline struct m9_context *
m9_context(struct pipe_context *pctx)
{
   return (struct m9_context *)pctx;
}

static inline struct m9_resource *
m9_res(struct pipe_resource *pres)
{
   return (struct m9_resource *)pres;
}

static void
m9_warn_once(struct m9_context *ctx, uint32_t bit, const char *message)
{
   if (ctx->warned & bit)
      return;
   ctx->warned |= bit;
   fprintf(stderr, "m9x: %s\n", message);
}

static void
m9_context_sync(struct m9_context *ctx)
{
   if (ctx->draw)
      draw_flush(ctx->draw);
   m9_submit_job(ctx);
}

static void
m9_fill_dwords(struct m9_resource *resource, uint32_t value)
{
   uint32_t *words = resource->map;

   for (uint64_t i = 0; i < resource->size / 4; i++)
      words[i] = value;
}

/* Transfers */

static int
m9_surface_access(struct m9_context *ctx, const struct m9_resource *resource,
                  uint32_t op, const struct pipe_box *box, uint8_t *data)
{
   const struct m9_surface_desc desc = {
      .row_span = resource->row_span, .height = resource->height,
      .layers = 1, .layout = 1, .address_offset = true,
      .format = resource->format,
   };
   struct drm_parhelia_m9_surface_access access = {
      .handle = resource->handle,
      .op = op,
      .descriptor0 = m9_surface_desc0(&desc),
      .descriptor1 = m9_surface_desc1(&desc),
      .surface_width = resource->width,
      .x = box->x,
      .y = box->y,
      .width = box->width,
      .height = box->height,
      .data_stride = box->width * 4,
      .data_ptr = (uintptr_t)data,
      .data_size = (uint64_t)box->width * box->height * 4,
   };

   return ioctl(ctx->screen->fd, DRM_IOCTL_PARHELIA_M9_SURFACE_ACCESS,
                &access) ? -errno : 0;
}

static int
m9_texture_window(struct m9_context *ctx, const struct m9_resource *resource,
                  unsigned level, bool write, const struct pipe_box *box,
                  uint8_t *data, unsigned stride)
{
   const struct m9_texture_level *lvl = &resource->texture.level[level];
   size_t bytes = (size_t)box->width * box->height * 4;
   uint8_t *rows = MALLOC(bytes);
   struct drm_parhelia_m9_surface_access access = {
      .handle = resource->handle,
      .op = write ? PARHELIA_M9_ACCESS_WRITE : PARHELIA_M9_ACCESS_READ,
      .descriptor0 = (uint32_t)resource->texture_code << 24 |
                     M9_TEXTURE_KIND_2D << 16 | lvl->width,
      .descriptor1 = 1u << 16 | lvl->height,
      .surface_width = lvl->width,
      .x = box->x,
      .y = box->y,
      .width = box->width,
      .height = box->height,
      .data_stride = box->width * 4,
      .data_ptr = (uintptr_t)rows,
      .data_size = bytes,
      .flags = PARHELIA_M9_ACCESS_TEXTURE,
      .offset = (uint32_t)lvl->offset,
   };
   int ret = rows && lvl->offset <= UINT32_MAX ? 0 : -ENOMEM;

   if (!ret && write &&
       !util_format_translate(resource->texture_storage, rows, box->width * 4,
                              0, 0, resource->base.format, data, stride, 0, 0,
                              box->width, box->height))
      ret = -EINVAL;
   if (!ret && ioctl(ctx->screen->fd, DRM_IOCTL_PARHELIA_M9_SURFACE_ACCESS,
                     &access))
      ret = -errno;
   if (!ret && !write &&
       !util_format_translate(resource->base.format, data, stride, 0, 0,
                              resource->texture_storage, rows, box->width * 4,
                              0, 0, box->width, box->height))
      ret = -EINVAL;
   FREE(rows);
   return ret;
}

/* Rows in the texture's pipe format to and from tiled texture storage. */
static int
m9_texture_access(struct m9_context *ctx, struct m9_resource *resource,
                  unsigned level, bool write, const struct pipe_box *box,
                  uint8_t *data, unsigned stride)
{
   const struct m9_texture_layout *layout = &resource->texture;

   if (!m9_texture_level_placed(layout, level))
      return m9_texture_window(ctx, resource, level, write, box, data, stride);
   return write ?
      m9_texture_store(layout, resource->map, resource->size,
                       resource->texture_storage, level, 0, box,
                       resource->base.format, data, stride) :
      m9_texture_fetch(layout, resource->map, resource->size,
                       resource->texture_storage, level, 0, box,
                       resource->base.format, data, stride);
}

static uint64_t
m9_transfer_offset(const struct m9_resource *resource, unsigned level,
                   const struct pipe_box *box, unsigned *stride)
{
   if (resource->base.target == PIPE_BUFFER) {
      *stride = 0;
      return box->x;
   }
   if (resource->kind == M9_RESOURCE_LINEAR_TEXTURE) {
      const struct m9_mip_level *mip = &resource->mip.level[level];

      if (mip->in_tail) {
         *stride = 32 * resource->cpp;
         return mip->offset +
                ((uint64_t)(mip->tail_y + box->y) * 32 +
                 mip->tail_x + box->x) * resource->cpp;
      }
      *stride = mip->row_span * resource->cpp;
      return mip->offset +
             ((uint64_t)box->y * mip->row_span + box->x) * resource->cpp;
   }
   *stride = resource->row_span * resource->cpp;
   return ((uint64_t)box->y * resource->row_span + box->x) * resource->cpp;
}

static void *
m9_transfer_map(struct pipe_context *pctx, struct pipe_resource *pres,
                unsigned level, unsigned usage, const struct pipe_box *box,
                struct pipe_transfer **out)
{
   struct m9_context *ctx = m9_context(pctx);
   struct m9_resource *resource = (struct m9_resource *)pres;
   struct m9_transfer *transfer;
   uint64_t offset = 0, last;
   unsigned width, height, stride;

   if (!resource || !resource->map ||
       ((usage & PIPE_MAP_WRITE) && !resource->map_writable) ||
       !box || !out || box->x < 0 ||
       box->y < 0 || box->z || box->width < 0 || box->height < 0 ||
       box->depth != 1 || level > pres->last_level)
      return NULL;
   width = pres->target == PIPE_BUFFER ? pres->width0 :
           (pres->width0 >> level ? pres->width0 >> level : 1);
   height = pres->target == PIPE_BUFFER ? 1 :
            (pres->height0 >> level ? pres->height0 >> level : 1);
   if ((unsigned)box->x > width || (unsigned)box->width > width - box->x ||
       (unsigned)box->y > height || (unsigned)box->height > height - box->y)
      return NULL;
   /* Pending jobs may still write render and depth targets. */
   if (pres->target != PIPE_BUFFER && !(usage & PIPE_MAP_UNSYNCHRONIZED))
      m9_context_sync(ctx);
   transfer = CALLOC_STRUCT(m9_transfer);
   if (!transfer)
      return NULL;
   if (resource->texture.levels) {
      bool preserve = (usage & PIPE_MAP_READ) ||
                      !(usage & (PIPE_MAP_DISCARD_RANGE |
                                 PIPE_MAP_DISCARD_WHOLE_RESOURCE));

      stride = box->width * util_format_get_blocksize(pres->format);
      transfer->staging = box->width && box->height ?
                          MALLOC((size_t)stride * box->height) : NULL;
      if (!transfer->staging ||
          (preserve && m9_texture_access(ctx, resource, level, false, box,
                                         transfer->staging, stride))) {
         m9_warn_once(ctx, M9_WARN_TRANSFER, "texture storage read failed");
         FREE(transfer->staging);
         FREE(transfer);
         return NULL;
      }
   } else if (resource->tiled) {
      bool preserve = (usage & PIPE_MAP_READ) ||
                      !(usage & (PIPE_MAP_DISCARD_RANGE |
                                 PIPE_MAP_DISCARD_WHOLE_RESOURCE));

      /* Tiled storage is only reachable as rows through the kernel. */
      stride = box->width * 4;
      transfer->staging = box->width && box->height ?
                          MALLOC((size_t)stride * box->height) : NULL;
      if (!transfer->staging ||
          (preserve && m9_surface_access(ctx, resource,
                                         PARHELIA_M9_ACCESS_READ, box,
                                         transfer->staging))) {
         m9_warn_once(ctx, M9_WARN_TRANSFER,
                      "de-tiled render target access failed");
         FREE(transfer->staging);
         FREE(transfer);
         return NULL;
      }
   } else {
      offset = m9_transfer_offset(resource, level, box, &stride);
      last = offset;
      if (pres->target == PIPE_BUFFER)
         last += box->width;
      else if (box->width && box->height)
         last += (uint64_t)(box->height - 1) * stride +
                 (uint64_t)box->width * resource->cpp;
      if (last > resource->size) {
         FREE(transfer);
         return NULL;
      }
   }
   pipe_resource_reference(&transfer->base.resource, pres);
   transfer->base.level = level;
   transfer->base.usage = usage;
   transfer->base.box = *box;
   transfer->base.stride = stride;
   transfer->base.layer_stride = pres->target == PIPE_BUFFER ? 0 :
                                 (uintptr_t)stride * height;
   *out = &transfer->base;
   return transfer->staging ? transfer->staging :
          (uint8_t *)resource->map + offset;
}

static void
m9_transfer_unmap(struct pipe_context *pctx, struct pipe_transfer *ptransfer)
{
   struct m9_transfer *transfer = (struct m9_transfer *)ptransfer;

   if (transfer->staging && m9_res(ptransfer->resource)->texture.levels) {
      struct m9_resource *resource = m9_res(ptransfer->resource);
      uint32_t bit = 1u << ptransfer->level;

      if (ptransfer->usage & PIPE_MAP_WRITE) {
         if (m9_texture_access(m9_context(pctx), resource, ptransfer->level,
                               true, &ptransfer->box, transfer->staging,
                               ptransfer->stride)) {
            resource->texture_unplaced |= bit;
            m9_warn_once(m9_context(pctx), M9_WARN_TRANSFER,
                         "texture level cannot be stored: no captured "
                         "placement and no kernel texture window");
         } else {
            resource->texture_unplaced &= ~bit;
         }
      }
      FREE(transfer->staging);
   } else if (transfer->staging) {
      if ((ptransfer->usage & PIPE_MAP_WRITE) &&
          m9_surface_access(m9_context(pctx), m9_res(ptransfer->resource),
                            PARHELIA_M9_ACCESS_WRITE, &ptransfer->box,
                            transfer->staging))
         m9_warn_once(m9_context(pctx), M9_WARN_TRANSFER,
                      "de-tiled render target write failed");
      FREE(transfer->staging);
   } else if (ptransfer->usage & PIPE_MAP_WRITE) {
      __sync_synchronize();
   }
   pipe_resource_reference(&ptransfer->resource, NULL);
   FREE(transfer);
}

/* Two-pass jobs */

static int
m9_source_reserve(struct m9_context *ctx, uint64_t bytes)
{
   uint64_t size;

   if (bytes <= ctx->source.size)
      return 0;
   size = MAX2((bytes + 0xfff) & ~0xfffull, 2 * ctx->source.size);
   m9_bo_destroy(ctx->screen, &ctx->source);
   return m9_bo_create(ctx->screen, size, 0, M9_RESOURCE_LINEAR_BUFFER,
                       &ctx->source);
}

static bool
m9_varying_is_color(unsigned location)
{
   return location == VARYING_SLOT_COL0 || location == VARYING_SLOT_COL1;
}

/* Raster constants and CPU varying views of a native fragment job. */
static int
m9_job_native(struct m9_context *ctx, const struct m9_fs_variant *fs,
              unsigned records, struct m9_raster_fs *raster)
{
   const struct m9_fs_program *p = &fs->program;
   const float *extra = ctx->extra.data;
   uint32_t views[M9_FS_MAX_VARYINGS], coords[M9_FS_MAX_TEXTURES];
   bool flat[M9_FS_MAX_VARYINGS];
   unsigned cpu = 0;

   memcpy((uint8_t *)ctx->system.map + M9_RASTER_CONST_OFFSET,
          ctx->job_consts.data, ctx->job_consts.size);
   for (unsigned k = 0; k < p->varying_count; k++) {
      flat[k] = m9_varying_is_color(p->varyings[k].location);
      views[k] = p->varyings[k].location == VARYING_SLOT_COL0 ? 0 :
                 M9_TRANSIENT_CPU_VIEW + cpu++;
   }
   for (unsigned c = 0; c < fs->coord_count; c++)
      coords[c] = fs->coord_location[c] == VARYING_SLOT_COL0 ? 0 :
                  M9_TRANSIENT_CPU_VIEW + cpu++;
   for (unsigned j = 0; j < fs->extra_count; j++) {
      uint8_t *view = (uint8_t *)ctx->transient.map +
                      (M9_TRANSIENT_CPU_VIEW + j) * M9_TRANSIENT_VIEW_BYTES;

      for (unsigned r = 0; r < records; r++)
         memcpy(view + r * M9_TRANSIENT_VERTEX_BYTES,
                extra + (r * fs->extra_count + j) * 4, 4 * sizeof(float));
   }
   return m9_raster_fs_init_textured(raster, fs->vertex_entry,
                                     m9_fs_raster_88(p),
                                     util_dynarray_num_elements(&p->consts,
                                                                struct m9_fs_const),
                                     p->varying_count, flat, views,
                                     fs->coord_count, coords,
                                     p->texture_count, fs->texture_coord);
}

/* Job words, relocations, fragment slots and constants for offline decode. */
static void
m9_dump_job(struct m9_context *ctx, const struct m9_fs_variant *fs)
{
   const struct m9_batch *b = &ctx->batch;
   FILE *out = fopen(ctx->dump_path, "a");

   if (!out)
      return;
   fprintf(out, "JOB %u dwords %u relocs %u\n", ctx->dump_seq++,
           b->command_count, b->reloc_count);
   for (unsigned i = 0; i < b->command_count; i++)
      fprintf(out, "%08x%c", b->command[i], i % 8 == 7 ? '\n' : ' ');
   fputc('\n', out);
   for (unsigned i = 0; i < b->reloc_count; i++)
      fprintf(out, "RELOC dword %u handle %u offset %#" PRIx64 " kind %u\n",
              b->relocs[i].command_dword, b->relocs[i].handle,
              (uint64_t)b->relocs[i].target_offset, b->relocs[i].kind);
   if (fs) {
      const uint32_t *w = fs->program.slots.data;
      const float *k = ctx->job_consts.data;

      for (unsigned i = 0; i < m9_fs_slot_count(&fs->program); i++)
         fprintf(out, "SLOT %u %08x %08x\n", i, w[2 * i], w[2 * i + 1]);
      for (unsigned i = 0; i < ctx->job_consts.size / sizeof(float); i++)
         fprintf(out, "CONST %u %g\n", i, k[i]);
   }
   fclose(out);
}

static int
m9_submit_job(struct m9_context *ctx)
{
   unsigned records = util_dynarray_num_elements(&ctx->records, float) /
                      M9_RECORD_FLOATS;
   unsigned draws = util_dynarray_num_elements(&ctx->draws,
                                               struct m9_draw_record);
   const struct m9_job_key *key = &ctx->job_key;
   const struct m9_fs_state *fs = key->fs;
   struct m9_raster_fs raster;
   struct m9_draw_job job;
   uint64_t point = 0;
   int ret;

   if (!draws)
      return 0;
   ret = ctx->lost ? -EIO :
         m9_source_reserve(ctx, (uint64_t)records * M9_RECORD_FLOATS *
                                sizeof(float));
   if (!ret) {
      memcpy(ctx->source.map, ctx->records.data, ctx->records.size);
      memcpy(ctx->system.map, ctx->params.system_prefix,
             sizeof(ctx->params.system_prefix));
      memcpy((uint8_t *)ctx->system.map + M9_LINKED_OFFSET,
             ctx->params.linked_prefix, sizeof(ctx->params.linked_prefix));
      if (fs)
         ret = m9_job_native(ctx, key->variant, records, &raster);
      job = (struct m9_draw_job) {
         .params = &ctx->params,
         .decl = &ctx->decl,
         .source = ctx->source.handle,
         .system = ctx->system.handle,
         .program = ctx->program.handle,
         .fragment_db = ctx->fragment_db.handle,
         .transient = ctx->transient.handle,
         .color = m9_res(ctx->color)->handle,
         .depth = key->depth->handle,
         .aux = key->aux->handle,
         .be = ctx->be.handle,
         .source_vertices = records,
         .draws = ctx->draws.data,
         .draw_count = draws,
         .depth_control = key->depth_control,
         .depth_ef = key->depth_ef,
         .color_mask = key->color_mask,
         .clip_min = key->clip_min,
         .clip_max = key->clip_max,
         .fs = fs ? &raster : NULL,
         .units = key->unit_count ? key->units : NULL,
         .unit_count = key->unit_count,
      };
      if (fs) {
         job.program = key->variant->c2.handle;
         job.blend_dest = key->variant->program.blend_dest;
      }
      m9_batch_init(&ctx->batch);
      if (!ret)
         ret = m9_batch_emit_draw_job(&ctx->batch, &job);
   }
   if (!ret) {
      ctx->batch.sealed = true;
      if (ctx->dump_path)
         m9_dump_job(ctx, fs ? key->variant : NULL);
      ret = ctx->dry_run ?
            m9_batch_validate(ctx->screen, &ctx->command, &ctx->batch,
                              ctx->context_id) :
            m9_batch_submit(ctx->screen, &ctx->command, &ctx->batch,
                            ctx->context_id, &point);
   }
   if (ret) {
      if (!ctx->lost)
         fprintf(stderr, "m9x: two-pass job failed (%s); context lost\n",
                 strerror(-ret));
      ctx->lost = true;
   } else {
      ctx->last_point = point;
      if (ctx->debug)
         fprintf(stderr, "m9x: job %u records %u draws a8 %08x ef %08x "
                 "ab %08x clip %08x/%08x -> %#" PRIx64 "\n", records, draws,
                 key->depth_control, key->depth_ef, key->color_mask,
                 key->clip_min, key->clip_max, point);
   }
   util_dynarray_clear(&ctx->records);
   util_dynarray_clear(&ctx->draws);
   util_dynarray_clear(&ctx->extra);
   m9_batch_init(&ctx->batch);
   return ret;
}

/* Space for vertices source records of one draw in a job with this key. */
static bool
m9_consts_differ(const struct util_dynarray *a, const struct util_dynarray *b)
{
   return a->size != b->size || (a->size && memcmp(a->data, b->data, a->size));
}

static float *
m9_job_reserve(struct m9_context *ctx, const struct m9_job_key *key,
               const struct util_dynarray *consts, unsigned vertices,
               uint32_t *first, float **extra)
{
   unsigned padded = m9_draw_preprocess_vertices(vertices);
   unsigned extra_count = key->variant ? key->variant->extra_count : 0;
   unsigned dwords;
   unsigned records = util_dynarray_num_elements(&ctx->records, float) /
                      M9_RECORD_FLOATS;
   unsigned draws = util_dynarray_num_elements(&ctx->draws,
                                               struct m9_draw_record);
   float *out;

   if (padded > m9_draw_max_source_vertices())
      return NULL;
   dwords = key->fs ? m9_draw_job_dwords_fs(draws + 1, key->unit_count) :
                      m9_draw_job_dwords(draws + 1);
   if (draws && (memcmp(key, &ctx->job_key, sizeof(*key)) ||
                 (key->fs && m9_consts_differ(consts, &ctx->job_consts)) ||
                 records + padded > m9_draw_max_source_vertices() ||
                 dwords > M9_MAX_COMMAND_DWORDS)) {
      m9_submit_job(ctx);
      records = 0;
   }
   if (ctx->lost)
      return NULL;
   if (!draws || records == 0) {
      util_dynarray_clear(&ctx->job_consts);
      if (key->fs && consts->size &&
          !util_dynarray_grow_bytes(&ctx->job_consts, 1, consts->size))
         return NULL;
      if (key->fs && consts->size)
         memcpy(ctx->job_consts.data, consts->data, consts->size);
   }
   ctx->job_key = *key;
   for (unsigned k = 0; k < M9_TEXTURE_MAX_UNITS; k++)
      pipe_resource_reference(&ctx->textures[k], k < key->unit_count ?
                              key->unit_res[k] : NULL);
   *extra = NULL;
   if (extra_count) {
      *extra = util_dynarray_grow(&ctx->extra, float,
                                  padded * 4 * extra_count);
      if (!*extra)
         return NULL;
   }
   out = util_dynarray_grow(&ctx->records, float, padded * M9_RECORD_FLOATS);
   if (!out)
      m9_warn_once(ctx, M9_WARN_MEMORY, "out of memory for source records");
   *first = records;
   return out;
}

static void
m9_job_commit(struct m9_context *ctx, float *out, float *extra,
              unsigned vertices, uint32_t first, enum m9_topology topology,
              uint32_t cull, bool lines, bool flat)
{
   unsigned padded = m9_draw_preprocess_vertices(vertices);
   unsigned extra_floats = 4 * (ctx->job_key.variant ?
                                ctx->job_key.variant->extra_count : 0);
   struct m9_draw_record *draw;

   /* Pad to the next four-record boundary with the last vertex. */
   for (unsigned i = vertices; i < padded; i++) {
      memcpy(out + i * M9_RECORD_FLOATS, out + (vertices - 1) * M9_RECORD_FLOATS,
             M9_RECORD_FLOATS * sizeof(float));
      if (extra)
         memcpy(extra + i * extra_floats, extra + (vertices - 1) * extra_floats,
                extra_floats * sizeof(float));
   }
   draw = util_dynarray_grow(&ctx->draws, struct m9_draw_record, 1);
   if (!draw) {
      /* Shrinking never reallocates; drop the orphaned records. */
      ctx->records.size = (size_t)first * M9_RECORD_FLOATS * sizeof(float);
      ctx->extra.size = (size_t)first * extra_floats * sizeof(float);
      m9_warn_once(ctx, M9_WARN_MEMORY, "out of memory for draw records");
      return;
   }
   *draw = (struct m9_draw_record) {
      .start = first,
      .count = vertices,
      .topology = topology,
      .cull = cull,
      .lines = lines,
      .flat = flat,
   };
}

static struct m9_resource *
m9_context_depth(struct m9_context *ctx)
{
   const struct pipe_surface *zsbuf = &ctx->framebuffer.zsbuf;
   struct m9_resource *zs = zsbuf->texture ? m9_res(zsbuf->texture) : NULL;

   return zs && zs->hz && !zsbuf->level && zs->width == ctx->params.width &&
          zs->height == ctx->params.height ? zs : NULL;
}

static bool
m9_scissor_key(struct m9_context *ctx, const struct pipe_scissor_state *s,
               struct m9_job_key *key)
{
   uint32_t minx = MIN2(s->minx, ctx->params.width);
   uint32_t miny = MIN2(s->miny, ctx->params.height);
   uint32_t maxx = MIN2(s->maxx, ctx->params.width);
   uint32_t maxy = MIN2(s->maxy, ctx->params.height);

   if (minx >= maxx || miny >= maxy)
      return false;
   m9_scissor_words(minx, miny, maxx, maxy, &key->clip_min, &key->clip_max);
   return true;
}

/* Raster constant span values for the current constant buffer 0. */
static bool
m9_fs_resolve_consts(struct m9_context *ctx, const struct m9_fs_variant *fs,
                     struct util_dynarray *out)
{
   const struct pipe_constant_buffer *cb = &ctx->fs_constants;
   const uint8_t *data = cb->user_buffer ? cb->user_buffer :
                         cb->buffer ? m9_res(cb->buffer)->map : NULL;
   size_t size = data ? cb->buffer_size : 0;

   if (data)
      data += cb->buffer_offset;
   util_dynarray_clear(out);
   util_dynarray_foreach(&fs->program.consts, struct m9_fs_const, k) {
      for (unsigned lane = 0; lane < 2; lane++) {
         float v = 0.0f;

         switch (k->kind) {
         case M9_FS_CONST_LITERAL:
            v = k->value[lane];
            break;
         case M9_FS_CONST_UNIFORM:
            if (k->offset[lane] == M9_FS_UNIFORM_ZERO)
               break;
            if ((size_t)k->offset[lane] + sizeof(float) > size) {
               m9_warn_once(ctx, M9_WARN_FS, "fragment constants outside "
                            "the bound constant buffer; draw skipped");
               return false;
            }
            memcpy(&v, data + k->offset[lane], sizeof(v));
            break;
         case M9_FS_CONST_FRAG_Y:
            v = lane ? (float)ctx->params.height - 1.0f : 0.0f;
            break;
         }
         util_dynarray_append_typed(out, float, v);
      }
   }
   return true;
}

/* Unit words of a sampler view, or the reason it cannot be sampled. */
static const char *
m9_texture_bind_view(const struct pipe_sampler_view *view,
                     struct m9_texture_bind *bind)
{
   struct m9_resource *res;
   unsigned last;

   if (!view || !view->texture)
      return "no texture on a sampled unit; draw skipped";
   res = m9_res(view->texture);
   if (view->target != PIPE_TEXTURE_2D && view->target != PIPE_TEXTURE_RECT)
      return "only 2-D sampling is implemented; draw skipped";
   if (view->format != res->base.format ||
       view->swizzle_r != PIPE_SWIZZLE_X ||
       view->swizzle_g != PIPE_SWIZZLE_Y ||
       view->swizzle_b != PIPE_SWIZZLE_Z ||
       view->swizzle_a != PIPE_SWIZZLE_W)
      return "sampler view format or swizzle differs from the texture; "
             "draw skipped";
   /* The vendor re-lays the chain from the base level; not implemented. */
   if (view->u.tex.first_level)
      return "a base level above 0 needs a re-based texture copy; "
             "draw skipped";
   last = MIN2(view->u.tex.last_level, res->base.last_level);
   if (res->texture.levels) {
      if (res->texture_unplaced & ((2u << last) - 1))
         return "texture levels beyond the captured 32x16 tail need the "
                "kernel texture window; draw skipped";
      *bind = (struct m9_texture_bind) {
         .code = res->texture_code, .kind = M9_TEXTURE_KIND_2D,
         .width = res->width, .height = res->height, .depth = 1,
         .last_level = res->base.last_level, .view_last_level = last,
      };
      return NULL;
   }
   /* Captured 79=c8210080 FBO textures: ordinary tiled render targets. */
   if (res->tiled && res->kind == M9_RESOURCE_LINEAR_COLOR &&
       res->base.format == PIPE_FORMAT_B8G8R8A8_UNORM && !(res->width % 32) &&
       (res->width > 64 || res->height > 64)) {
      *bind = (struct m9_texture_bind) {
         .code = 0xcc, .kind = M9_TEXTURE_KIND_RENDER,
         .width = res->width, .height = res->height, .depth = 1,
      };
      return NULL;
   }
   return "texture is not stored in a captured sampler layout; draw skipped";
}

static bool
m9_job_textures(struct m9_context *ctx, const struct m9_fs_variant *fs,
                struct m9_job_key *key)
{
   const struct m9_fs_program *p = &fs->program;

   for (unsigned k = 0; k < p->texture_count; k++) {
      unsigned slot = p->textures[k];
      struct pipe_sampler_view *view = slot < PIPE_MAX_SAMPLERS ?
                                       ctx->fs_views[slot] : NULL;
      const struct pipe_sampler_state *sampler = slot < PIPE_MAX_SAMPLERS ?
                                                 ctx->fs_samplers[slot] : NULL;
      struct m9_texture_bind bind;
      const char *reason = m9_texture_bind_view(view, &bind);

      if (!reason && !sampler)
         reason = "sampled unit has no sampler state; draw skipped";
      if (!reason && m9_texture_unit_init(&key->units[k],
                                          m9_res(view->texture)->handle,
                                          &bind, sampler))
         reason = "sampler wrap mode is not implemented; draw skipped";
      if (reason) {
         m9_warn_once(ctx, M9_WARN_TEXTURE, reason);
         return false;
      }
      key->unit_res[k] = view->texture;
   }
   key->unit_count = p->texture_count;
   if (!p->texture_count) {
      m9_texture_unit_placeholder(&key->units[0], ctx->placeholder_texture.handle);
      key->unit_count = 1;
   }
   return true;
}

static bool
m9_blend_factor(enum pipe_blendfactor f, uint8_t *out)
{
   static const struct { enum pipe_blendfactor pipe; uint8_t m9; } map[] = {
      { PIPE_BLENDFACTOR_ZERO, M9_FS_ZERO },
      { PIPE_BLENDFACTOR_ONE, M9_FS_ONE },
      { PIPE_BLENDFACTOR_SRC_ALPHA, M9_FS_SRC_ALPHA },
      { PIPE_BLENDFACTOR_INV_SRC_ALPHA, M9_FS_INV_SRC_ALPHA },
      { PIPE_BLENDFACTOR_DST_ALPHA, M9_FS_DST_ALPHA },
      { PIPE_BLENDFACTOR_INV_DST_ALPHA, M9_FS_INV_DST_ALPHA },
      { PIPE_BLENDFACTOR_SRC_COLOR, M9_FS_SRC_COLOR },
      { PIPE_BLENDFACTOR_INV_SRC_COLOR, M9_FS_INV_SRC_COLOR },
   };

   for (unsigned i = 0; i < ARRAY_SIZE(map); i++) {
      if (map[i].pipe == f) {
         *out = map[i].m9;
         return true;
      }
   }
   return false;
}

/* The alpha lane of SRC_COLOR is SRC_ALPHA. */
static enum pipe_blendfactor
m9_alpha_equivalent(enum pipe_blendfactor f)
{
   switch (f) {
   case PIPE_BLENDFACTOR_SRC_COLOR: return PIPE_BLENDFACTOR_SRC_ALPHA;
   case PIPE_BLENDFACTOR_INV_SRC_COLOR: return PIPE_BLENDFACTOR_INV_SRC_ALPHA;
   default: return f;
   }
}

/* Blend state as lowered factors, or the reason draws are skipped. */
static const char *
m9_blend_key(const struct pipe_blend_state *blend, struct m9_fs_blend *out)
{
   const struct pipe_rt_blend_state *rt = blend ? &blend->rt[0] : NULL;

   if (!rt || !rt->blend_enable)
      return NULL;
   if (rt->rgb_func != PIPE_BLEND_ADD || rt->alpha_func != PIPE_BLEND_ADD)
      return "blend equations other than ADD are not implemented; "
             "draw skipped";
   if (m9_alpha_equivalent(rt->rgb_src_factor) !=
          m9_alpha_equivalent(rt->alpha_src_factor) ||
       m9_alpha_equivalent(rt->rgb_dst_factor) !=
          m9_alpha_equivalent(rt->alpha_dst_factor) ||
       !m9_blend_factor(rt->rgb_src_factor, &out->src) ||
       !m9_blend_factor(rt->rgb_dst_factor, &out->dst))
      return "separate or constant blend factors are not implemented; "
             "draw skipped";
   return NULL;
}

static bool
m9_fs_variant_upload(struct m9_context *ctx, struct m9_fs_variant *v,
                     const char **reason)
{
   const unsigned vs_slots = ARRAY_SIZE(m9_passthrough_c2_program) / 2 - 4;
   unsigned slots = m9_fs_slot_count(&v->program);

   if ((slots + vs_slots) * 8 > M9_PROGRAM_BYTES) {
      *reason = "program exceeds the c2 image";
      return false;
   }
   if (m9_bo_create(ctx->screen, M9_PROGRAM_BYTES, PARHELIA_M9_BO_VRAM,
                    M9_RESOURCE_SYSTEM_STATE, &v->c2)) {
      *reason = "cannot allocate the c2 image";
      return false;
   }
   /* Fragment slots first, the captured vertex program behind them. */
   memset(v->c2.map, 0, v->c2.size);
   memcpy(v->c2.map, v->program.slots.data, 8 * slots);
   memcpy((uint8_t *)v->c2.map + 8 * slots, m9_passthrough_c2_program + 8,
          8 * vs_slots);
   v->vertex_entry = slots;
   return true;
}

/* CPU view order: non-colour varyings, then distinct coordinate streams. */
static void
m9_fs_streams(struct m9_fs_variant *v)
{
   const struct m9_fs_program *p = &v->program;

   for (unsigned k = 0; k < p->varying_count; k++)
      if (p->varyings[k].location != VARYING_SLOT_COL0)
         v->extra_location[v->extra_count++] = p->varyings[k].location;
   for (unsigned t = 0; t < p->texture_count; t++) {
      unsigned c = 0;

      while (c < v->coord_count && v->coord_location[c] != p->texcoords[t])
         c++;
      if (c == v->coord_count) {
         v->coord_location[v->coord_count++] = p->texcoords[t];
         if (p->texcoords[t] != VARYING_SLOT_COL0)
            v->extra_location[v->extra_count++] = p->texcoords[t];
      }
      v->texture_coord[t] = c;
   }
}

/* Program of fs for a blend state and inlined uniform values, compiled on
 * first use; refused variants stay cached with their reason. */
static const struct m9_fs_variant *
m9_fs_variant(struct m9_context *ctx, struct m9_fs_state *fs,
              const struct m9_fs_variant *want, const char **reason)
{
   const size_t inline_bytes = fs->nir->info.num_inlinable_uniforms *
                               sizeof(uint32_t);
   struct m9_fs_variant *v, **slot;
   nir_shader *clone;

   util_dynarray_foreach(&fs->variants, struct m9_fs_variant *, it) {
      if ((*it)->blend.src == want->blend.src &&
          (*it)->blend.dst == want->blend.dst &&
          !memcmp((*it)->inline_values, want->inline_values, inline_bytes)) {
         *reason = (*it)->reason;
         return (*it)->ok ? *it : NULL;
      }
   }
   *reason = "out of memory";
   v = CALLOC_STRUCT(m9_fs_variant);
   clone = v ? nir_shader_clone(NULL, fs->nir) : NULL;
   if (!clone) {
      FREE(v);
      return NULL;
   }
   v->blend = want->blend;
   memcpy(v->inline_values, want->inline_values, inline_bytes);
   if (inline_bytes)
      m9_fs_inline_uniforms(clone, v->inline_values);
   v->ok = m9_fs_compile(clone, &v->blend, &v->program, &v->reason);
   ralloc_free(clone);
   if (v->ok) {
      m9_fs_streams(v);
      if (v->extra_count > M9_TRANSIENT_VIEWS - M9_TRANSIENT_CPU_VIEW) {
         v->reason = "more CPU varying streams than transient views";
         v->ok = false;
      } else {
         v->ok = m9_fs_variant_upload(ctx, v, &v->reason);
      }
      if (!v->ok)
         m9_fs_program_fini(&v->program);
   }
   slot = util_dynarray_grow(&fs->variants, struct m9_fs_variant *, 1);
   if (!slot) {
      if (v->ok) {
         m9_bo_destroy(ctx->screen, &v->c2);
         m9_fs_program_fini(&v->program);
      }
      FREE(v);
      return NULL;
   }
   *slot = v;
   *reason = v->reason;
   return v->ok ? v : NULL;
}

/* Variant of the bound fragment state for the current blend and inlined
 * uniform values. */
static const struct m9_fs_variant *
m9_ctx_variant(struct m9_context *ctx, const char **reason)
{
   struct m9_fs_variant want = { .blend = { M9_FS_ONE, M9_FS_ZERO } };

   *reason = m9_blend_key(ctx->blend, &want.blend);
   if (*reason)
      return NULL;
   memcpy(want.inline_values, ctx->fs_inline, sizeof(want.inline_values));
   return m9_fs_variant(ctx, ctx->fs, &want, reason);
}

static bool
m9_job_key_current(struct m9_context *ctx, struct m9_job_key *key)
{
   const struct pipe_depth_stencil_alpha_state *dsa = ctx->dsa;
   const struct pipe_blend_state *blend = ctx->blend;
   const struct pipe_rasterizer_state *rast = ctx->rasterizer;
   struct m9_resource *zs;
   bool test, write;

   if (ctx->lost || !ctx->params_valid)
      return false;
   zs = m9_context_depth(ctx);
   test = dsa && dsa->depth_enabled;
   if (test && !zs) {
      m9_warn_once(ctx, M9_WARN_DEPTH,
                   "depth test needs a D24S8 attachment of the target size; "
                   "drawing without depth");
      test = false;
   }
   write = test && dsa->depth_writemask;
   if (dsa && (dsa->stencil[0].enabled || dsa->alpha_enabled))
      m9_warn_once(ctx, M9_WARN_STENCIL, "stencil and alpha test are not "
                   "implemented");
   memset(key, 0, sizeof(*key));
   key->depth = zs ? zs : &ctx->target_depth;
   key->aux = zs ? zs->hz : &ctx->target_aux;
   key->depth_control = m9_depth_control(test, write,
                                         dsa ? dsa->depth_func : PIPE_FUNC_LESS);
   key->depth_ef = m9_depth_ef(test, write);
   key->color_mask = m9_color_mask(blend ? blend->rt[0].colormask :
                                           PIPE_MASK_RGBA);
   if (ctx->fs && ctx->fs->native && !ctx->fs->passthrough) {
      const char *reason;

      key->fs = ctx->fs;
      key->variant = m9_ctx_variant(ctx, &reason);
      if (!key->variant) {
         m9_warn_once(ctx, M9_WARN_BLEND, reason);
         return false;
      }
      if (!m9_fs_resolve_consts(ctx, key->variant, &ctx->key_consts) ||
          !m9_job_textures(ctx, key->variant, key))
         return false;
   } else if (blend && blend->rt[0].blend_enable) {
      m9_warn_once(ctx, M9_WARN_BLEND, "blending needs a native fragment "
                   "program; drawing opaque");
   }
   if (rast && rast->scissor)
      return m9_scissor_key(ctx, &ctx->scissor, key);
   key->clip_min = ctx->params.clip_min;
   key->clip_max = ctx->params.clip_max;
   return true;
}

/* vbuf render */

static inline struct m9_vbuf_render *
m9_vbuf_render(struct vbuf_render *render)
{
   return (struct m9_vbuf_render *)render;
}

static const struct vertex_info *
m9_render_get_vertex_info(struct vbuf_render *render)
{
   struct m9_vbuf_render *r = m9_vbuf_render(render);
   int position = draw_find_shader_output(r->ctx->draw,
                                          TGSI_SEMANTIC_POSITION, 0);
   int color = draw_find_shader_output(r->ctx->draw, TGSI_SEMANTIC_COLOR, 0);
   const struct m9_fs_state *fs = r->ctx->fs;
   const struct m9_fs_variant *v = NULL;
   const char *reason;

   memset(&r->vinfo, 0, sizeof(r->vinfo));
   draw_emit_vertex_attr(&r->vinfo, EMIT_4F, MAX2(position, 0));
   r->has_color = color >= 0;
   if (r->has_color)
      draw_emit_vertex_attr(&r->vinfo, EMIT_4F, color);
   r->extra_count = 0;
   if (fs && fs->native && !fs->passthrough)
      v = m9_ctx_variant(r->ctx, &reason);
   if (v) {
      unsigned offset = r->has_color ? 8 : 4;

      for (unsigned j = 0; j < v->extra_count; j++) {
         unsigned location = v->extra_location[j];
         unsigned name, index;
         int output;

         tgsi_get_gl_varying_semantic(location,
                                      r->ctx->screen->base.caps.tgsi_texcoord,
                                      &name, &index);
         output = draw_find_shader_output(r->ctx->draw, name, index);
         r->extra_offset[j] = output >= 0 ? (int)offset : -1;
         if (output >= 0) {
            draw_emit_vertex_attr(&r->vinfo, EMIT_4F, output);
            offset += 4;
         }
      }
      r->extra_count = v->extra_count;
   }
   draw_compute_vertex_size(&r->vinfo);
   return &r->vinfo;
}

static bool
m9_render_allocate_vertices(struct vbuf_render *render, uint16_t vertex_size,
                            uint16_t count)
{
   struct m9_vbuf_render *r = m9_vbuf_render(render);
   size_t bytes = (size_t)vertex_size * count;

   if (bytes > r->capacity) {
      uint8_t *vertices = realloc(r->vertices, bytes);

      if (!vertices)
         return false;
      r->vertices = vertices;
      r->capacity = bytes;
   }
   r->vertex_size = vertex_size;
   return true;
}

static void *
m9_render_map_vertices(struct vbuf_render *render)
{
   return m9_vbuf_render(render)->vertices;
}

static void
m9_render_unmap_vertices(struct vbuf_render *render, uint16_t min_index,
                         uint16_t max_index)
{
   (void)render;
   (void)min_index;
   (void)max_index;
}

static void
m9_render_set_primitive(struct vbuf_render *render, enum mesa_prim prim)
{
   m9_vbuf_render(render)->prim = prim;
}

static void
m9_render_record(const struct m9_vbuf_render *r, unsigned index, float *out,
                 float *extra)
{
   const float *v = (const float *)(r->vertices + (size_t)index * r->vertex_size);
   static const float missing[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

   for (unsigned j = 0; extra && j < r->extra_count; j++)
      memcpy(extra + 4 * j, r->extra_offset[j] >= 0 ?
             v + r->extra_offset[j] : missing, 4 * sizeof(float));

   /* The captured ortho program flips window y and maps z = 1 - 2 z_win. */
   out[0] = v[0];
   out[1] = (float)r->ctx->params.height - v[1];
   out[2] = 1.0f - 2.0f * v[2];
   for (unsigned c = 0; c < 4; c++)
      out[3 + c] = r->has_color ? CLAMP(v[4 + c], 0.0f, 1.0f) : 1.0f;
}

static void
m9_render_append(struct m9_vbuf_render *r, const struct m9_job_key *key,
                 enum m9_topology topology, bool lines, uint32_t cull,
                 const void *indices, unsigned index_size, unsigned start,
                 unsigned count)
{
   const unsigned max = m9_draw_max_source_vertices();
   bool list = topology == M9_TOPOLOGY_LINES ||
               topology == M9_TOPOLOGY_TRIANGLES;

   /* Native connected primitives are bounded by max_vertex_buffer_bytes;
    * lists split on any multiple of six. */
   if (count > max && !list)
      return;
   for (unsigned done = 0; done < count;) {
      unsigned n = MIN2(count - done, max);
      uint32_t first;
      float *extra;
      float *out = m9_job_reserve(r->ctx, key, &r->ctx->key_consts, n, &first,
                                  &extra);

      if (!out)
         return;
      if (extra && r->extra_count != key->variant->extra_count)
         return;
      for (unsigned i = 0; i < n; i++) {
         unsigned index = !indices ? start + done + i :
                          index_size == 4 ?
                          ((const uint32_t *)indices)[done + i] :
                          ((const uint16_t *)indices)[done + i];

         m9_render_record(r, index, out + i * M9_RECORD_FLOATS,
                          extra ? extra + i * 4 * r->extra_count : NULL);
      }
      m9_job_commit(r->ctx, out, extra, n, first, topology, cull, lines,
                    r->ctx->rasterizer && r->ctx->rasterizer->flatshade);
      done += n;
   }
}

static bool
m9_native_topology(enum mesa_prim prim, unsigned in_pv,
                   enum m9_topology *topology)
{
   switch (prim) {
   case MESA_PRIM_LINES: *topology = M9_TOPOLOGY_LINES; break;
   case MESA_PRIM_LINE_STRIP: *topology = M9_TOPOLOGY_LINE_STRIP; break;
   case MESA_PRIM_TRIANGLES: *topology = M9_TOPOLOGY_TRIANGLES; break;
   case MESA_PRIM_TRIANGLE_STRIP: *topology = M9_TOPOLOGY_TRIANGLE_STRIP; break;
   case MESA_PRIM_TRIANGLE_FAN: *topology = M9_TOPOLOGY_TRIANGLE_FAN; break;
   case MESA_PRIM_QUADS: *topology = M9_TOPOLOGY_QUADS; break;
   case MESA_PRIM_QUAD_STRIP: *topology = M9_TOPOLOGY_QUAD_STRIP; break;
   case MESA_PRIM_POLYGON: *topology = M9_TOPOLOGY_POLYGON; return true;
   default: return false;
   }
   /* Hardware flat shading provokes with the last vertex. */
   return in_pv == PV_LAST;
}

static void *
m9_render_scratch(struct m9_vbuf_render *r, size_t bytes)
{
   util_dynarray_clear(&r->indices);
   return util_dynarray_grow_bytes(&r->indices, 1, bytes);
}

static void
m9_render_emit(struct m9_vbuf_render *r, const uint16_t *indices,
               unsigned start, unsigned count)
{
   struct m9_context *ctx = r->ctx;
   const struct pipe_rasterizer_state *rast = ctx->rasterizer;
   const unsigned lists = BITFIELD_BIT(MESA_PRIM_LINES) |
                          BITFIELD_BIT(MESA_PRIM_TRIANGLES);
   enum mesa_prim prim = r->prim, out_prim = r->prim;
   unsigned in_pv = rast && rast->flatshade && rast->flatshade_first ?
                    PV_FIRST : PV_LAST;
   unsigned out_size = 0, out_count = count;
   enum m9_topology topology;
   const void *list = NULL;
   struct m9_job_key key;
   uint32_t cull = 0;

   if (!count || !m9_job_key_current(ctx, &key))
      return;
   if (u_reduced_prim(prim) == MESA_PRIM_POINTS) {
      m9_warn_once(ctx, M9_WARN_POINTS, "unexpanded points are not drawn");
      return;
   }
   if (u_reduced_prim(prim) == MESA_PRIM_TRIANGLES && rast) {
      if (rast->cull_face == PIPE_FACE_FRONT_AND_BACK)
         return;
      cull = m9_cull_ef(rast->cull_face, rast->front_ccw);
   }
   if (indices || !m9_native_topology(prim, in_pv, &topology)) {
      void *buffer;

      if (indices) {
         u_translate_func translate;

         if (u_index_translator(lists, prim, 2, count, in_pv, PV_LAST,
                                PR_DISABLE, &out_prim, &out_size, &out_count,
                                &translate) == U_TRANSLATE_ERROR)
            return;
         buffer = m9_render_scratch(r, (size_t)out_count * out_size);
         if (!buffer)
            return;
         translate(indices, 0, count, out_count, 0, buffer);
      } else {
         u_generate_func generate;

         if (u_index_generator(lists, prim, start, count, in_pv, PV_LAST,
                               &out_prim, &out_size, &out_count,
                               &generate) == U_TRANSLATE_ERROR)
            return;
         buffer = m9_render_scratch(r, (size_t)out_count * out_size);
         if (!buffer)
            return;
         generate(start, out_count, buffer);
      }
      list = buffer;
      topology = out_prim == MESA_PRIM_LINES ? M9_TOPOLOGY_LINES :
                                               M9_TOPOLOGY_TRIANGLES;
   }
   m9_render_append(r, &key, topology,
                    u_reduced_prim(out_prim) == MESA_PRIM_LINES, cull, list,
                    out_size, start, out_count);
}

static void
m9_render_draw_elements(struct vbuf_render *render, const uint16_t *indices,
                        unsigned count)
{
   m9_render_emit(m9_vbuf_render(render), indices, 0, count);
}

static void
m9_render_draw_arrays(struct vbuf_render *render, unsigned start,
                      unsigned count)
{
   m9_render_emit(m9_vbuf_render(render), NULL, start, count);
}

static void
m9_render_release_vertices(struct vbuf_render *render)
{
   (void)render;
}

static void
m9_render_destroy(struct vbuf_render *render)
{
   struct m9_vbuf_render *r = m9_vbuf_render(render);

   util_dynarray_fini(&r->indices);
   free(r->vertices);
   FREE(r);
}

static struct m9_vbuf_render *
m9_render_create(struct m9_context *ctx)
{
   struct m9_vbuf_render *r = CALLOC_STRUCT(m9_vbuf_render);

   if (!r)
      return NULL;
   r->ctx = ctx;
   /* A draw's vertices must fit the transient stream views. */
   r->base.max_indices = m9_draw_max_source_vertices();
   r->base.max_vertex_buffer_bytes =
      m9_draw_max_source_vertices() * M9_TRANSIENT_VERTEX_BYTES;
   r->base.get_vertex_info = m9_render_get_vertex_info;
   r->base.allocate_vertices = m9_render_allocate_vertices;
   r->base.map_vertices = m9_render_map_vertices;
   r->base.unmap_vertices = m9_render_unmap_vertices;
   r->base.set_primitive = m9_render_set_primitive;
   r->base.draw_elements = m9_render_draw_elements;
   r->base.draw_arrays = m9_render_draw_arrays;
   r->base.release_vertices = m9_render_release_vertices;
   r->base.destroy = m9_render_destroy;
   util_dynarray_init(&r->indices, NULL);
   return r;
}

/* Drawing */

static void
m9_draw_vbo(struct pipe_context *pctx, const struct pipe_draw_info *info,
            unsigned drawid_offset,
            const struct pipe_draw_indirect_info *indirect,
            const struct pipe_draw_start_count_bias *draws,
            unsigned num_draws)
{
   struct m9_context *ctx = m9_context(pctx);
   bool indexed = info->index_size;

   if (num_draws > 1) {
      util_draw_multi(pctx, info, drawid_offset, indirect, draws, num_draws);
      return;
   }
   if (!num_draws || indirect || !draws[0].count || !info->instance_count ||
       ctx->lost || !ctx->params_valid)
      return;
   if (!ctx->fs || (!ctx->fs->passthrough && !ctx->fs->native)) {
      if (ctx->fs && !ctx->fs->warned) {
         fprintf(stderr, "m9x: fragment shader not supported by the native "
                 "backend (%s); draws skipped\n", ctx->fs->reason);
         ctx->fs->warned = true;
      }
      return;
   }
   for (unsigned i = 0; i < ctx->num_vertex_buffers; i++) {
      const struct pipe_vertex_buffer *vb = &ctx->vertex_buffers[i];
      const void *map = vb->is_user_buffer ? vb->buffer.user :
                        vb->buffer.resource ?
                        m9_res(vb->buffer.resource)->map : NULL;

      draw_set_mapped_vertex_buffer(ctx->draw, i, map,
                                    vb->is_user_buffer || !vb->buffer.resource ?
                                    ~(size_t)0 : vb->buffer.resource->width0);
   }
   if (indexed)
      draw_set_indexes(ctx->draw, info->has_user_indices ? info->index.user :
                       m9_res(info->index.resource)->map, info->index_size,
                       info->has_user_indices ? ~0u :
                       info->index.resource->width0);
   for (unsigned i = 0; i < PIPE_MAX_CONSTANT_BUFFERS; i++) {
      const struct pipe_constant_buffer *cb = &ctx->vs_constants[i];
      const uint8_t *data = cb->user_buffer ? cb->user_buffer :
                            cb->buffer ? m9_res(cb->buffer)->map : NULL;

      draw_set_mapped_constant_buffer(ctx->draw, MESA_SHADER_VERTEX, i,
                                      data ? data + cb->buffer_offset : NULL,
                                      data ? cb->buffer_size : 0);
   }
   draw_prepare_shader_outputs(ctx->draw);
   draw_vbo(ctx->draw, info, drawid_offset, NULL, draws, 1, 0);
   draw_flush(ctx->draw);
   for (unsigned i = 0; i < ctx->num_vertex_buffers; i++)
      draw_set_mapped_vertex_buffer(ctx->draw, i, NULL, 0);
   if (indexed)
      draw_set_indexes(ctx->draw, NULL, 0, 0);
}

/* Clears */

static bool
m9_scissor_full(const struct m9_context *ctx,
                const struct pipe_scissor_state *s)
{
   return !s || (!s->minx && !s->miny && s->maxx >= ctx->params.width &&
                 s->maxy >= ctx->params.height);
}

/* The vendor rectangle-draw clear: a full-target quad inside the scissor. */
static void
m9_clear_quad(struct m9_context *ctx, const struct pipe_scissor_state *scissor,
              unsigned colormask, const float *rgba, bool depth, double z)
{
   const float w = ctx->params.width, h = ctx->params.height;
   const float corner[4][2] = { { 0, 0 }, { w, 0 }, { w, h }, { 0, h } };
   struct m9_resource *zs = m9_context_depth(ctx);
   struct m9_job_key key;
   uint32_t first;
   float *out, *extra;

   if (ctx->lost || !ctx->params_valid)
      return;
   depth = depth && zs;
   memset(&key, 0, sizeof(key));
   key.depth = zs ? zs : &ctx->target_depth;
   key.aux = zs ? zs->hz : &ctx->target_aux;
   key.depth_control = m9_depth_control(depth, depth, PIPE_FUNC_ALWAYS);
   key.depth_ef = m9_depth_ef(depth, depth);
   key.color_mask = m9_color_mask(colormask);
   key.clip_min = ctx->params.clip_min;
   key.clip_max = ctx->params.clip_max;
   if (scissor && !m9_scissor_key(ctx, scissor, &key))
      return;
   out = m9_job_reserve(ctx, &key, NULL, 4, &first, &extra);
   if (!out)
      return;
   for (unsigned i = 0; i < 4; i++) {
      float *rec = out + i * M9_RECORD_FLOATS;

      rec[0] = corner[i][0];
      rec[1] = corner[i][1];
      rec[2] = 1.0f - 2.0f * (float)CLAMP(z, 0.0, 1.0);
      for (unsigned c = 0; c < 4; c++)
         rec[3 + c] = CLAMP(rgba[c], 0.0f, 1.0f);
   }
   m9_job_commit(ctx, out, NULL, 4, first, M9_TOPOLOGY_QUADS, 0, false, true);
}

static void
m9_clear_depth_cpu(struct m9_resource *zs, unsigned buffers, double depth,
                   unsigned stencil)
{
   uint32_t z = (uint32_t)(CLAMP(depth, 0.0, 1.0) * 0xffffff + 0.5) << 8;
   uint32_t *words = zs->map;

   /* D24S8: depth in bits 31:8, stencil in the low byte. */
   for (uint64_t i = 0; i < zs->size / 4; i++) {
      uint32_t v = words[i];

      if (buffers & PIPE_CLEAR_DEPTH)
         v = (v & 0xff) | z;
      if (buffers & PIPE_CLEAR_STENCIL)
         v = (v & ~0xffu) | (stencil & 0xff);
      words[i] = v;
   }
   if ((buffers & PIPE_CLEAR_DEPTH) && zs->hz)
      m9_fill_dwords(zs->hz, M9_HZ_CLEAR);
}

static void
m9_clear(struct pipe_context *pctx, unsigned buffers,
         uint32_t color_clear_mask, uint8_t stencil_clear_mask,
         const struct pipe_scissor_state *scissor,
         const union pipe_color_union *color, double depth, unsigned stencil)
{
   static const float zero[4];
   struct m9_context *ctx = m9_context(pctx);
   struct pipe_resource *cbuf = ctx->framebuffer.nr_cbufs ?
                                ctx->framebuffer.cbufs[0].texture : NULL;
   struct m9_resource *zs = m9_context_depth(ctx);
   bool full = m9_scissor_full(ctx, scissor);

   if (!ctx->params_valid)
      return;
   m9_context_sync(ctx);
   if ((buffers & PIPE_CLEAR_COLOR0) && cbuf) {
      if (full && (color_clear_mask & PIPE_MASK_RGBA) == PIPE_MASK_RGBA) {
         union util_color uc;

         /* Tiling permutes pixels inside the BO: fill every dword. */
         util_pack_color(color->f, cbuf->format, &uc);
         m9_fill_dwords(m9_res(cbuf), uc.ui[0]);
      } else {
         m9_clear_quad(ctx, full ? NULL : scissor, color_clear_mask,
                       color->f, false, 0);
      }
   }
   if (!(buffers & PIPE_CLEAR_DEPTHSTENCIL))
      return;
   if (!zs) {
      m9_warn_once(ctx, M9_WARN_CLEAR, "depth clear needs a D24S8 attachment "
                   "of the target size");
      return;
   }
   if (full && (!(buffers & PIPE_CLEAR_STENCIL) || stencil_clear_mask == 0xff)) {
      m9_context_sync(ctx);
      m9_clear_depth_cpu(zs, buffers, depth, stencil);
   } else if (buffers & PIPE_CLEAR_DEPTH) {
      m9_clear_quad(ctx, full ? NULL : scissor, 0, zero, true, depth);
      if (buffers & PIPE_CLEAR_STENCIL)
         m9_warn_once(ctx, M9_WARN_CLEAR, "partial stencil clears are not "
                      "implemented");
   }
}

static void
m9_clear_render_target(struct pipe_context *pctx, struct pipe_surface *dst,
                       const union pipe_color_union *color,
                       unsigned dstx, unsigned dsty,
                       unsigned width, unsigned height,
                       bool render_condition_enabled)
{
   struct m9_context *ctx = m9_context(pctx);
   struct m9_resource *resource = m9_res(dst->texture);
   union util_color uc;

   (void)render_condition_enabled;
   if (dst->level || resource->cpp != 4 || !resource->map || !width || !height)
      return;
   m9_context_sync(ctx);
   util_pack_color(color->f, dst->format, &uc);
   if (!dstx && !dsty && width >= resource->width &&
       height >= resource->height) {
      m9_fill_dwords(resource, uc.ui[0]);
   } else if (resource->tiled) {
      const struct pipe_box box = {
         .x = dstx, .y = dsty, .width = width, .height = height, .depth = 1,
      };
      uint32_t *rows = MALLOC((size_t)width * height * 4);

      for (size_t i = 0; rows && i < (size_t)width * height; i++)
         rows[i] = uc.ui[0];
      if (!rows || m9_surface_access(ctx, resource, PARHELIA_M9_ACCESS_WRITE,
                                     &box, (uint8_t *)rows))
         m9_warn_once(ctx, M9_WARN_CLEAR, "partial render target clear failed");
      FREE(rows);
   } else {
      m9_resource_clear_linear(resource, uc.ui[0], dstx, dsty, width, height);
   }
}

static void
m9_clear_depth_stencil(struct pipe_context *pctx, struct pipe_surface *dst,
                       unsigned clear_flags, double depth, unsigned stencil,
                       unsigned dstx, unsigned dsty,
                       unsigned width, unsigned height,
                       bool render_condition_enabled)
{
   struct m9_context *ctx = m9_context(pctx);
   struct m9_resource *resource = m9_res(dst->texture);

   (void)render_condition_enabled;
   if (dst->level || !resource->map)
      return;
   m9_context_sync(ctx);
   if (dstx || dsty || width < resource->width || height < resource->height ||
       resource->format != M9_DEPTH_FORMAT_D24S8) {
      m9_warn_once(ctx, M9_WARN_CLEAR, "partial or non-D24S8 depth clears "
                   "are not implemented");
      return;
   }
   m9_clear_depth_cpu(resource, clear_flags, depth, stencil);
}

static void
m9_blit(struct pipe_context *pctx, const struct pipe_blit_info *info)
{
   if (!util_try_blit_via_copy_region(pctx, info, false))
      m9_warn_once(m9_context(pctx), M9_WARN_BLIT, "scaled, converting or "
                   "filtered blits are not implemented");
}

static void
m9_flush_resource(struct pipe_context *pctx, struct pipe_resource *resource)
{
   (void)pctx;
   (void)resource;
}

/* State objects */

static void *
m9_create_blend_state(struct pipe_context *pctx,
                      const struct pipe_blend_state *state)
{
   (void)pctx;
   return mem_dup(state, sizeof(*state));
}

static void
m9_bind_blend_state(struct pipe_context *pctx, void *state)
{
   m9_context(pctx)->blend = state;
}

static void *
m9_create_dsa_state(struct pipe_context *pctx,
                    const struct pipe_depth_stencil_alpha_state *state)
{
   (void)pctx;
   return mem_dup(state, sizeof(*state));
}

static void
m9_bind_dsa_state(struct pipe_context *pctx, void *state)
{
   m9_context(pctx)->dsa = state;
}

static void *
m9_create_rasterizer_state(struct pipe_context *pctx,
                           const struct pipe_rasterizer_state *state)
{
   (void)pctx;
   return mem_dup(state, sizeof(*state));
}

static void
m9_bind_rasterizer_state(struct pipe_context *pctx, void *state)
{
   struct m9_context *ctx = m9_context(pctx);

   /* draw may rebind its own state; set it before recording ours. */
   draw_set_rasterizer_state(ctx->draw, state, state);
   ctx->rasterizer = state;
}

static void *
m9_create_sampler_state(struct pipe_context *pctx,
                        const struct pipe_sampler_state *state)
{
   (void)pctx;
   return mem_dup(state, sizeof(*state));
}

static void
m9_bind_sampler_states(struct pipe_context *pctx, mesa_shader_stage shader,
                       unsigned start, unsigned count, void **states)
{
   struct m9_context *ctx = m9_context(pctx);

   if (shader != MESA_SHADER_FRAGMENT)
      return;
   for (unsigned i = 0; i < count && start + i < PIPE_MAX_SAMPLERS; i++)
      ctx->fs_samplers[start + i] = states ? states[i] : NULL;
}

static void
m9_delete_state(struct pipe_context *pctx, void *state)
{
   (void)pctx;
   FREE(state);
}

static struct pipe_sampler_view *
m9_create_sampler_view(struct pipe_context *pctx, struct pipe_resource *texture,
                       const struct pipe_sampler_view *templ)
{
   struct pipe_sampler_view *view = CALLOC_STRUCT(pipe_sampler_view);

   if (!view)
      return NULL;
   *view = *templ;
   pipe_reference_init(&view->reference, 1);
   view->texture = NULL;
   pipe_resource_reference(&view->texture, texture);
   view->context = pctx;
   return view;
}

static void
m9_sampler_view_destroy(struct pipe_context *pctx,
                        struct pipe_sampler_view *view)
{
   (void)pctx;
   pipe_resource_reference(&view->texture, NULL);
   FREE(view);
}

static void
m9_set_sampler_views(struct pipe_context *pctx, mesa_shader_stage shader,
                     unsigned start, unsigned count, unsigned unbind_trailing,
                     struct pipe_sampler_view **views)
{
   struct m9_context *ctx = m9_context(pctx);

   if (shader != MESA_SHADER_FRAGMENT)
      return;
   for (unsigned i = 0; i < count + unbind_trailing &&
                        start + i < PIPE_MAX_SAMPLERS; i++)
      pipe_sampler_view_reference(&ctx->fs_views[start + i],
                                  views && i < count ? views[i] : NULL);
}

static void *
m9_create_vs_state(struct pipe_context *pctx,
                   const struct pipe_shader_state *templ)
{
   return draw_create_vertex_shader(m9_context(pctx)->draw, templ);
}

static void
m9_bind_vs_state(struct pipe_context *pctx, void *state)
{
   draw_bind_vertex_shader(m9_context(pctx)->draw, state);
}

static void
m9_delete_vs_state(struct pipe_context *pctx, void *state)
{
   draw_delete_vertex_shader(m9_context(pctx)->draw, state);
}

/* True when def is the unmodified four-component primary color input. */
static bool
m9_fs_is_primary_color(nir_def *def, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);

   if (depth > 8 || def->num_components != 4)
      return false;
   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
      nir_variable *var;

      switch (intr->intrinsic) {
      case nir_intrinsic_load_input:
      case nir_intrinsic_load_interpolated_input:
         return nir_intrinsic_io_semantics(intr).location == VARYING_SLOT_COL0 &&
                !nir_intrinsic_component(intr);
      case nir_intrinsic_load_deref:
         var = nir_intrinsic_get_var(intr, 0);
         return var && var->data.mode == nir_var_shader_in &&
                var->data.location == VARYING_SLOT_COL0;
      default:
         return false;
      }
   }
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);

      if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
         for (unsigned c = 0; c < 4; c++)
            if (alu->src[0].swizzle[c] != c)
               return false;
         return m9_fs_is_primary_color(alu->src[0].src.ssa, depth + 1);
      }
      if (alu->op == nir_op_vec4) {
         nir_def *src = alu->src[0].src.ssa;

         for (unsigned c = 0; c < 4; c++)
            if (alu->src[c].src.ssa != src || alu->src[c].swizzle[0] != c)
               return false;
         return m9_fs_is_primary_color(src, depth + 1);
      }
   }
   return false;
}

/* The raster pass interpolates vertex color only: accept exactly that. */
static bool
m9_fs_passthrough(nir_shader *nir)
{
   bool written = false;

   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            nir_intrinsic_instr *intr;
            nir_variable *var;
            unsigned location;

            if (instr->type == nir_instr_type_deref ||
                instr->type == nir_instr_type_load_const ||
                instr->type == nir_instr_type_alu)
               continue;
            if (instr->type != nir_instr_type_intrinsic)
               return false;
            intr = nir_instr_as_intrinsic(instr);
            switch (intr->intrinsic) {
            case nir_intrinsic_load_input:
            case nir_intrinsic_load_interpolated_input:
            case nir_intrinsic_load_barycentric_pixel:
            case nir_intrinsic_load_barycentric_centroid:
            case nir_intrinsic_load_barycentric_sample:
            case nir_intrinsic_load_deref:
               break;
            case nir_intrinsic_store_output:
               location = nir_intrinsic_io_semantics(intr).location;
               if ((location != FRAG_RESULT_COLOR &&
                    location != FRAG_RESULT_DATA0) ||
                   !m9_fs_is_primary_color(intr->src[0].ssa, 0))
                  return false;
               written = true;
               break;
            case nir_intrinsic_store_deref:
               var = nir_intrinsic_get_var(intr, 0);
               if (!var || var->data.mode != nir_var_shader_out ||
                   (var->data.location != FRAG_RESULT_COLOR &&
                    var->data.location != FRAG_RESULT_DATA0) ||
                   !m9_fs_is_primary_color(intr->src[1].ssa, 0))
                  return false;
               written = true;
               break;
            default:
               return false;
            }
         }
      }
   }
   return written;
}

static void
m9_fs_native_init(struct m9_context *ctx, struct m9_fs_state *fs)
{
   const struct m9_fs_variant opaque = { .blend = { M9_FS_ONE, M9_FS_ZERO } };

   util_dynarray_init(&fs->variants, NULL);
   /* Shaders specialised on inlined uniforms compile per value set. */
   fs->native = fs->nir->info.num_inlinable_uniforms ||
                m9_fs_variant(ctx, fs, &opaque, &fs->reason);
}

static void *
m9_create_fs_state(struct pipe_context *pctx,
                   const struct pipe_shader_state *templ)
{
   struct m9_fs_state *fs = CALLOC_STRUCT(m9_fs_state);

   if (!fs)
      return NULL;
   if (templ->type == PIPE_SHADER_IR_NIR) {
      fs->nir = templ->ir.nir;
      fs->passthrough = m9_fs_passthrough(fs->nir);
      if (!fs->passthrough)
         m9_fs_native_init(m9_context(pctx), fs);
   }
   fs->draw = draw_create_fragment_shader(m9_context(pctx)->draw, templ);
   if (!fs->draw) {
      ralloc_free(fs->nir);
      FREE(fs);
      return NULL;
   }
   return fs;
}

static void
m9_bind_fs_state(struct pipe_context *pctx, void *state)
{
   struct m9_context *ctx = m9_context(pctx);
   struct m9_fs_state *fs = state;

   ctx->fs = fs;
   draw_bind_fragment_shader(ctx->draw, fs ? fs->draw : NULL);
}

static void
m9_delete_fs_state(struct pipe_context *pctx, void *state)
{
   struct m9_context *ctx = m9_context(pctx);
   struct m9_fs_state *fs = state;

   if (ctx->job_key.fs == fs)
      m9_context_sync(ctx);
   util_dynarray_foreach(&fs->variants, struct m9_fs_variant *, it) {
      if ((*it)->ok) {
         m9_bo_destroy(ctx->screen, &(*it)->c2);
         m9_fs_program_fini(&(*it)->program);
      }
      FREE(*it);
   }
   util_dynarray_fini(&fs->variants);
   draw_delete_fragment_shader(ctx->draw, fs->draw);
   ralloc_free(fs->nir);
   FREE(fs);
}

static void *
m9_create_vertex_elements_state(struct pipe_context *pctx, unsigned count,
                                const struct pipe_vertex_element *elements)
{
   struct m9_velems *velems;

   (void)pctx;
   if (count > PIPE_MAX_ATTRIBS)
      return NULL;
   velems = CALLOC_STRUCT(m9_velems);
   if (!velems)
      return NULL;
   velems->count = count;
   memcpy(velems->elements, elements, count * sizeof(*elements));
   return velems;
}

static void
m9_bind_vertex_elements_state(struct pipe_context *pctx, void *state)
{
   struct m9_velems *velems = state;

   draw_set_vertex_elements(m9_context(pctx)->draw,
                            velems ? velems->count : 0,
                            velems ? velems->elements : NULL);
}

static void
m9_set_vertex_buffers(struct pipe_context *pctx, unsigned count,
                      const struct pipe_vertex_buffer *buffers)
{
   struct m9_context *ctx = m9_context(pctx);

   util_set_vertex_buffers_count(ctx->vertex_buffers, &ctx->num_vertex_buffers,
                                 buffers, count);
   draw_set_vertex_buffers(ctx->draw, count, buffers);
}

static void
m9_set_constant_buffer(struct pipe_context *pctx, mesa_shader_stage shader,
                       uint index, const struct pipe_constant_buffer *cb)
{
   if (shader == MESA_SHADER_VERTEX && index < PIPE_MAX_CONSTANT_BUFFERS)
      util_copy_constant_buffer(&m9_context(pctx)->vs_constants[index], cb);
   else if (shader == MESA_SHADER_FRAGMENT && index == 0)
      util_copy_constant_buffer(&m9_context(pctx)->fs_constants, cb);
}

static void
m9_set_inlinable_constants(struct pipe_context *pctx, mesa_shader_stage shader,
                           uint num_values, uint32_t *values)
{
   if (shader == MESA_SHADER_FRAGMENT)
      memcpy(m9_context(pctx)->fs_inline, values,
             MIN2(num_values, MAX_INLINABLE_UNIFORMS) * sizeof(*values));
}

static void
m9_update_target(struct m9_context *ctx)
{
   const struct pipe_surface *cbuf = &ctx->framebuffer.cbufs[0];
   struct m9_resource *color = ctx->framebuffer.nr_cbufs && cbuf->texture ?
                               m9_res(cbuf->texture) : NULL;
   struct m9_render_layout layout;
   uint32_t w, h;

   ctx->params_valid = false;
   pipe_resource_reference(&ctx->color, NULL);
   if (!color)
      return;
   w = color->width;
   h = color->height;
   if (cbuf->level || !color->tiled ||
       m9_context_set_framebuffer(ctx, color, NULL, w, h) ||
       m9_draw_params_init(&ctx->params, w, h, color->format) ||
       !m9_render_surface_layout(w, h, 4, &layout)) {
      m9_warn_once(ctx, M9_WARN_TARGET, "render target is not a tiled "
                   "A8R8G8B8 surface; draws skipped");
      return;
   }
   /* Depth-less targets still bind a depth and HZ BO of their size. */
   if (!ctx->target_depth.handle || ctx->target_depth.width != w ||
       ctx->target_depth.height != h) {
      m9_bo_destroy(ctx->screen, &ctx->target_depth);
      m9_bo_destroy(ctx->screen, &ctx->target_aux);
      if (m9_bo_create(ctx->screen, layout.size, PARHELIA_M9_BO_VRAM,
                       M9_RESOURCE_LINEAR_DEPTH, &ctx->target_depth) ||
          m9_bo_create(ctx->screen, m9_hz_bytes(w, h), PARHELIA_M9_BO_VRAM,
                       M9_RESOURCE_LINEAR_DEPTH, &ctx->target_aux)) {
         m9_bo_destroy(ctx->screen, &ctx->target_depth);
         m9_bo_destroy(ctx->screen, &ctx->target_aux);
         m9_warn_once(ctx, M9_WARN_MEMORY, "cannot allocate target depth");
         return;
      }
      ctx->target_depth.width = w;
      ctx->target_depth.height = h;
      m9_fill_dwords(&ctx->target_depth, 0xffffffff);
      m9_fill_dwords(&ctx->target_aux, M9_HZ_CLEAR);
   }
   ctx->params_valid = true;
}

static void
m9_set_framebuffer_state(struct pipe_context *pctx,
                         const struct pipe_framebuffer_state *fb)
{
   struct m9_context *ctx = m9_context(pctx);

   m9_context_sync(ctx);
   util_copy_framebuffer_state(&ctx->framebuffer, fb);
   draw_set_zs_format(ctx->draw, fb->zsbuf.texture ? fb->zsbuf.format :
                                                     PIPE_FORMAT_NONE);
   m9_update_target(ctx);
}

static void
m9_set_viewport_states(struct pipe_context *pctx, unsigned start,
                       unsigned count, const struct pipe_viewport_state *vp)
{
   draw_set_viewport_states(m9_context(pctx)->draw, start, count, vp);
}

static void
m9_set_scissor_states(struct pipe_context *pctx, unsigned start,
                      unsigned count, const struct pipe_scissor_state *s)
{
   if (!start && count)
      m9_context(pctx)->scissor = s[0];
}

static void
m9_set_clip_state(struct pipe_context *pctx, const struct pipe_clip_state *clip)
{
   draw_set_clip_state(m9_context(pctx)->draw, clip);
}

static void
m9_set_blend_color(struct pipe_context *pctx,
                   const struct pipe_blend_color *color)
{
   (void)pctx;
   (void)color;
}

static void
m9_set_stencil_ref(struct pipe_context *pctx, const struct pipe_stencil_ref ref)
{
   (void)pctx;
   (void)ref;
}

static void
m9_set_sample_mask(struct pipe_context *pctx, unsigned mask)
{
   (void)pctx;
   (void)mask;
}

static void
m9_set_polygon_stipple(struct pipe_context *pctx,
                       const struct pipe_poly_stipple *stipple)
{
   (void)pctx;
   (void)stipple;
}

/* Context */

static void
m9_context_flush(struct pipe_context *pctx,
                 struct pipe_fence_handle **out_fence, unsigned flags)
{
   struct m9_context *ctx = m9_context(pctx);
   struct pipe_fence_handle *fence = NULL;

   (void)flags;
   m9_context_sync(ctx);
   if (out_fence) {
      if (!ctx->lost)
         fence = m9_fence_create(ctx->last_point, true);
      pctx->screen->fence_reference(pctx->screen, out_fence, fence);
      if (fence)
         pctx->screen->fence_reference(pctx->screen, &fence, NULL);
   }
}

static int
m9_context_init_fixed(struct m9_context *ctx)
{
   struct m9_screen *screen = ctx->screen;
   uint64_t db_bytes = MAX2((screen->fragment_db_size + 0xfff) & ~0xfffull,
                            M9_FRAGMENT_DB_MIN_BYTES);
   int ret;

   ret = m9_bo_create(screen, M9_SYSTEM_BYTES, 0, M9_RESOURCE_SYSTEM_STATE,
                      &ctx->system);
   if (!ret)
      ret = m9_bo_create(screen, M9_PROGRAM_BYTES, PARHELIA_M9_BO_VRAM,
                         M9_RESOURCE_SYSTEM_STATE, &ctx->program);
   if (!ret)
      ret = m9_bo_create(screen, db_bytes, PARHELIA_M9_BO_VRAM,
                         M9_RESOURCE_SYSTEM_STATE, &ctx->fragment_db);
   if (!ret)
      ret = m9_bo_create(screen, M9_TRANSIENT_VIEWS * M9_TRANSIENT_VIEW_BYTES,
                         PARHELIA_M9_BO_VRAM, M9_RESOURCE_SYSTEM_STATE,
                         &ctx->transient);
   if (!ret)
      ret = m9_bo_create(screen, M9_BE_BYTES, PARHELIA_M9_BO_VRAM,
                         M9_RESOURCE_SYSTEM_STATE, &ctx->be);
   if (!ret)
      ret = m9_bo_create(screen, 4096, 0, M9_RESOURCE_LINEAR_BUFFER,
                         &ctx->source);
   if (!ret)
      ret = m9_bo_create(screen, M9_TEXTURE_TAIL_BYTES, PARHELIA_M9_BO_VRAM,
                         M9_RESOURCE_LINEAR_TEXTURE, &ctx->placeholder_texture);
   if (ret)
      return ret;
   memset(ctx->placeholder_texture.map, 0, ctx->placeholder_texture.size);
   memset(ctx->program.map, 0, ctx->program.size);
   memcpy(ctx->program.map, m9_passthrough_c2_program,
          sizeof(m9_passthrough_c2_program));
   memset(ctx->fragment_db.map, 0, ctx->fragment_db.size);
   memcpy(ctx->fragment_db.map, screen->fragment_db, screen->fragment_db_size);
   memset(ctx->be.map, 0, ctx->be.size);
   return 0;
}

static void
m9_context_destroy(struct pipe_context *pctx)
{
   struct m9_context *ctx = m9_context(pctx);
   struct m9_screen *screen = ctx->screen;

   if (ctx->draw) {
      m9_context_sync(ctx);
      draw_destroy(ctx->draw);
      ctx->draw = NULL;
   }
   if (ctx->base.stream_uploader)
      u_upload_destroy(ctx->base.stream_uploader);
   util_unreference_framebuffer_state(&ctx->framebuffer);
   util_set_vertex_buffers_count(ctx->vertex_buffers, &ctx->num_vertex_buffers,
                                 NULL, 0);
   for (unsigned i = 0; i < PIPE_MAX_CONSTANT_BUFFERS; i++)
      pipe_resource_reference(&ctx->vs_constants[i].buffer, NULL);
   pipe_resource_reference(&ctx->fs_constants.buffer, NULL);
   for (unsigned i = 0; i < PIPE_MAX_SAMPLERS; i++)
      pipe_sampler_view_reference(&ctx->fs_views[i], NULL);
   m9_context_state_cleanup(ctx);
   m9_command_bo_destroy(screen, &ctx->command);
   m9_bo_destroy(screen, &ctx->system);
   m9_bo_destroy(screen, &ctx->program);
   m9_bo_destroy(screen, &ctx->fragment_db);
   m9_bo_destroy(screen, &ctx->transient);
   m9_bo_destroy(screen, &ctx->be);
   m9_bo_destroy(screen, &ctx->source);
   m9_bo_destroy(screen, &ctx->target_depth);
   m9_bo_destroy(screen, &ctx->target_aux);
   m9_bo_destroy(screen, &ctx->placeholder_texture);
   util_dynarray_fini(&ctx->records);
   util_dynarray_fini(&ctx->draws);
   util_dynarray_fini(&ctx->extra);
   util_dynarray_fini(&ctx->key_consts);
   util_dynarray_fini(&ctx->job_consts);
   FREE(ctx);
}

struct pipe_context *
m9_context_create(struct pipe_screen *pscreen, void *priv, unsigned flags)
{
   struct m9_screen *screen = (struct m9_screen *)pscreen;
   struct m9_vbuf_render *render;
   struct draw_stage *stage;
   struct m9_context *ctx;

   if (flags & PIPE_CONTEXT_COMPUTE_ONLY)
      return NULL;
   if (!screen->execute_submit || !screen->fragment_db) {
      fprintf(stderr, "m9x: rendering needs kernel user submit "
              "(m9_user_submit=1) and firmware m9x/rsfragptrs-normal.bin\n");
      return NULL;
   }
   ctx = CALLOC_STRUCT(m9_context);
   if (!ctx)
      return NULL;
   m9_context_state_init(ctx, screen);
   ctx->base.priv = priv;
   ctx->debug = debug_get_bool_option("M9X_DEBUG", false);
   ctx->dump_path = debug_get_option("M9X_DUMP", NULL);
   ctx->dry_run = debug_get_bool_option("M9X_DRYRUN", false);
   util_dynarray_init(&ctx->records, NULL);
   util_dynarray_init(&ctx->draws, NULL);
   util_dynarray_init(&ctx->extra, NULL);
   util_dynarray_init(&ctx->key_consts, NULL);
   util_dynarray_init(&ctx->job_consts, NULL);

   ctx->base.destroy = m9_context_destroy;
   ctx->base.flush = m9_context_flush;
   ctx->base.draw_vbo = m9_draw_vbo;
   ctx->base.clear = m9_clear;
   ctx->base.clear_render_target = m9_clear_render_target;
   ctx->base.clear_depth_stencil = m9_clear_depth_stencil;
   ctx->base.blit = m9_blit;
   ctx->base.resource_copy_region = util_resource_copy_region;
   ctx->base.flush_resource = m9_flush_resource;
   ctx->base.create_blend_state = m9_create_blend_state;
   ctx->base.bind_blend_state = m9_bind_blend_state;
   ctx->base.delete_blend_state = m9_delete_state;
   ctx->base.create_depth_stencil_alpha_state = m9_create_dsa_state;
   ctx->base.bind_depth_stencil_alpha_state = m9_bind_dsa_state;
   ctx->base.delete_depth_stencil_alpha_state = m9_delete_state;
   ctx->base.create_rasterizer_state = m9_create_rasterizer_state;
   ctx->base.bind_rasterizer_state = m9_bind_rasterizer_state;
   ctx->base.delete_rasterizer_state = m9_delete_state;
   ctx->base.create_sampler_state = m9_create_sampler_state;
   ctx->base.bind_sampler_states = m9_bind_sampler_states;
   ctx->base.delete_sampler_state = m9_delete_state;
   ctx->base.create_sampler_view = m9_create_sampler_view;
   ctx->base.sampler_view_destroy = m9_sampler_view_destroy;
   ctx->base.sampler_view_release = u_default_sampler_view_release;
   ctx->base.set_sampler_views = m9_set_sampler_views;
   ctx->base.create_vs_state = m9_create_vs_state;
   ctx->base.bind_vs_state = m9_bind_vs_state;
   ctx->base.delete_vs_state = m9_delete_vs_state;
   ctx->base.create_fs_state = m9_create_fs_state;
   ctx->base.bind_fs_state = m9_bind_fs_state;
   ctx->base.delete_fs_state = m9_delete_fs_state;
   ctx->base.create_vertex_elements_state = m9_create_vertex_elements_state;
   ctx->base.bind_vertex_elements_state = m9_bind_vertex_elements_state;
   ctx->base.delete_vertex_elements_state = m9_delete_state;
   ctx->base.set_vertex_buffers = m9_set_vertex_buffers;
   ctx->base.set_constant_buffer = m9_set_constant_buffer;
   ctx->base.set_inlinable_constants = m9_set_inlinable_constants;
   ctx->base.set_framebuffer_state = m9_set_framebuffer_state;
   ctx->base.set_viewport_states = m9_set_viewport_states;
   ctx->base.set_scissor_states = m9_set_scissor_states;
   ctx->base.set_clip_state = m9_set_clip_state;
   ctx->base.set_blend_color = m9_set_blend_color;
   ctx->base.set_stencil_ref = m9_set_stencil_ref;
   ctx->base.set_sample_mask = m9_set_sample_mask;
   ctx->base.set_polygon_stipple = m9_set_polygon_stipple;
   ctx->base.buffer_map = m9_transfer_map;
   ctx->base.buffer_unmap = m9_transfer_unmap;
   ctx->base.texture_map = m9_transfer_map;
   ctx->base.texture_unmap = m9_transfer_unmap;
   ctx->base.transfer_flush_region = u_default_transfer_flush_region;
   ctx->base.buffer_subdata = u_default_buffer_subdata;
   ctx->base.texture_subdata = u_default_texture_subdata;
   ctx->base.clear_buffer = u_default_clear_buffer;
   ctx->base.set_debug_callback = u_default_set_debug_callback;
   ctx->base.resource_release = u_default_resource_release;

   if (m9_vertex_decl_init(&ctx->decl, 3, 4) ||
       m9_command_bo_create(screen, &ctx->command) ||
       m9_context_init_fixed(ctx))
      goto fail;
   ctx->draw = draw_create(&ctx->base);
   if (!ctx->draw)
      goto fail;
   render = m9_render_create(ctx);
   if (!render)
      goto fail;
   stage = draw_vbuf_stage(ctx->draw, &render->base);
   if (!stage) {
      m9_render_destroy(&render->base);
      goto fail;
   }
   draw_set_rasterize_stage(ctx->draw, stage);
   draw_set_render(ctx->draw, &render->base);
   /* Points become two triangles; the native point path is unverified. */
   draw_wide_point_threshold(ctx->draw, 0.0f);
   draw_set_constant_buffer_stride(ctx->draw, sizeof(float));
   ctx->base.stream_uploader = u_upload_create_default(&ctx->base);
   if (!ctx->base.stream_uploader)
      goto fail;
   ctx->base.const_uploader = ctx->base.stream_uploader;
   return &ctx->base;
fail:
   m9_context_destroy(&ctx->base);
   return NULL;
}
