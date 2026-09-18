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
#include "m9_vs_texture.h"
#include "m9_vs_source.h"
#include "m9_passthrough.h"
#include "m9_state.h"
#include "nir.h"
#include "nir_builder.h"
#include "tgsi/tgsi_from_mesa.h"
#include "util/u_debug.h"
#include "util/u_debug_cb.h"
#include "util/format/u_format.h"
#include "util/u_draw.h"
#include "util/u_framebuffer.h"
#include "util/u_helpers.h"
#include "util/u_inlines.h"
#include "util/os_time.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_pack_color.h"
#include "util/u_prim.h"
#include "util/u_surface.h"
#include "util/u_transfer.h"
#include "util/u_upload_mgr.h"

/* Source record: x, y bottom-up, z in ortho object space, r, g, b, a. */
/* x, y, z, w, r, g, b, a: the w slot carries 1/w_clip for the setup engine. */
#define M9_RECORD_FLOATS 8u

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
   M9_WARN_WRAP = 1u << 11,
   M9_WARN_SUBMIT = 1u << 12,
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

/* CPU views a split shader may need: one per interpolated input and one per
 * distinct texture coordinate of every pass. */
#define M9_FS_MAX_EXTRA (M9_TRANSIENT_VIEWS - M9_TRANSIENT_CPU_VIEW)

/* Streams and units of one raster pass of a fragment shader. */
struct m9_fs_pass_streams {
   unsigned coord_count;               /* distinct texture coordinate streams */
   uint8_t coord_location[M9_FS_MAX_TEXTURES];
   uint8_t coord_component[M9_FS_MAX_TEXTURES];  /* slot half the stream carries */
   int8_t coord_extra[M9_FS_MAX_TEXTURES];     /* CPU view of a coordinate stream, -1 for COL0 */
   uint8_t texture_coord[M9_FS_MAX_TEXTURES];  /* coordinate stream per texel pair */
   int8_t varying_extra[M9_FS_MAX_VARYINGS];   /* CPU view of an interpolated input */
   /* Job unit pool entry of each texel slot; M9_PASS_UNIT_PLACEHOLDER binds
    * the opaque-black texture a record-writing pass without one still needs,
    * and a slot fed by a record binds no texture at all. */
   uint8_t unit[M9_FS_MAX_TEXTURES];
};

/* A fragment state specialised on blend state and inlined uniform values. */
struct m9_fs_variant {
   struct m9_fs_blend blend;
   uint32_t inline_values[MAX_INLINABLE_UNIFORMS];
   uint8_t swizzle[PIPE_MAX_SAMPLERS][4];  /* view swizzle per sampler, applied in NIR */
   uint32_t depth_texture;             /* samplers bound to a depth surface */
   bool ok;
   const char *reason;                 /* why the variant is not drawable */
   /* The raster passes the shader runs as; one for everything the engine
    * takes in a single pass. */
   struct m9_fs_passes passes;
   unsigned entry[M9_FS_MAX_PASSES];   /* c2 slot each pass enters at */
   unsigned record_dwords;             /* stride of the inter-pass record */
   uint8_t record_offset[M9_FS_MAX_PASSES];  /* each producer's slot start */
   struct m9_resource c2;              /* fragment slots, then vertex program */
   /* The same fragment slots with a compiled vertex program behind them. */
   struct m9_resource native_c2;
   uint64_t native_c2_serial;
   unsigned vertex_entry;
   unsigned extra_count;               /* CPU view locations: varyings, then coordinates */
   uint8_t extra_location[M9_FS_MAX_EXTRA];
   struct m9_fs_pass_streams stream[M9_FS_MAX_PASSES];
   /* Application texture slots of every pass, in binding order. */
   unsigned slot_count;
   uint8_t slot_pass[M9_DRAW_MAX_UNITS], slot_index[M9_DRAW_MAX_UNITS];
   /* Command budget of the job this variant builds: every pass emits its
    * own preprocessing and raster range. */
   unsigned relocs_base, relocs_draw, dwords_base, dwords_draw;
};

/* Words and relocations the passes of a variant cost for this many draws. */
static unsigned
m9_variant_relocs(const struct m9_fs_variant *v, unsigned draws)
{
   return v->relocs_base + draws * v->relocs_draw;
}

static unsigned
m9_variant_dwords(const struct m9_fs_variant *v, unsigned draws)
{
   return v->dwords_base + (draws - 1) * v->dwords_draw;
}

/* The pass that writes the colour; it carries the blend the state asked for. */
static inline const struct m9_fs_program *
m9_fs_final(const struct m9_fs_variant *v)
{
   return &v->passes.pass[v->passes.count - 1];
}

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
m9_skip_report(struct m9_context *ctx)
{
   fprintf(stderr, "m9x: context %u: %u seen, %u skipped, %u reasons\n",
           ctx->context_id, ctx->submitted_draws, ctx->skipped_draws,
           ctx->skip_reasons);
   fprintf(stderr, "m9x: window %u reads %" PRIu64 " KiB, %u writes %" PRIu64
           " KiB, map usage %#x, read usage %#x, explicit reads %u\n",
           ctx->window_reads, ctx->window_read_bytes >> 10, ctx->window_writes,
           ctx->window_write_bytes >> 10, ctx->map_usage_seen,
           ctx->window_read_usage, ctx->window_reads_explicit);
   for (unsigned i = 0; i < ctx->skip_reasons; i++)
      fprintf(stderr, "m9x: context %u: %u draws skipped: %s\n",
              ctx->context_id, ctx->skip_reason[i].draws,
              ctx->skip_reason[i].reason);
}

/* Warnings are printed once per reason; the tally keeps the draw count. */
static bool
m9_skip_reason(struct m9_context *ctx, const char *reason)
{
   unsigned i;

   for (i = 0; i < ctx->skip_reasons; i++)
      if (ctx->skip_reason[i].reason == reason) {
         ctx->skip_reason[i].draws++;
         return false;
      }
   if (ctx->skip_reasons < M9_MAX_SKIP_REASONS) {
      ctx->skip_reason[ctx->skip_reasons].reason = reason;
      ctx->skip_reason[ctx->skip_reasons++].draws = 1;
   }
   return false;
}

/* M9X_DEBUG_TEXTURES traces the level state of every sampled texture and
 * M9X_TEXTURE_DUMP writes each uploaded level 0 out as a PPM. */
static bool m9_debug_textures;
static const char *m9_texture_dump;

/* M9X_PROFILE=N reports where the frame went every N frames.  The phases
 * nest: draw contains emit, emit contains submit, submit contains the
 * varying gather, the record copy and the submit ioctl. */
enum m9_prof {
   M9_PROF_DRAW, M9_PROF_EMIT, M9_PROF_SUBMIT, M9_PROF_VARYING,
   M9_PROF_RECORDS, M9_PROF_IOCTL, M9_PROF_TARGET, M9_PROF_CLEAR,
   M9_PROF_TRANSFER, M9_PROF_COUNT,
};

static const char *const m9_prof_name[M9_PROF_COUNT] = {
   "draw", "emit", "submit", "varying", "records", "ioctl", "target",
   "clear", "transfer",
};

static unsigned m9_profile_period;
/* M9X_JOB_DRAWS ends a job after that many draws: the frame time against the
 * job count weighs what one job costs. */
static unsigned m9_job_draws;
/* M9X_NATIVE_VS=0 keeps every vertex shader in the draw module.  On by
 * default since the whole hardware fixture corpus passes either way
 * (m9-mesa-vs 14/14, m9-mesa-glsl 90/90, 2026-09-17). */
static bool m9_native_vs;

/* M9X_PASS_LOOP emits even a one-pass shader through the multi-pass loop. */
static bool m9_pass_loop;
/* M9X_SPLIT_STAGE isolates where a split job stops the engine, one binding
 * at a time.  1 leaves the record unbound and the routes idle, 2 binds the
 * record and keeps the routes idle, 3 is the real schedule, 4 binds the
 * record object with an ordinary colour descriptor, 5 with the captured
 * two-dword producer descriptor, 6 with the record format over the
 * colour target's row span and 7 with the colour descriptor and the plain
 * colour-pass state words, which leaves the bound object as the only
 * difference from stage 1. */
static unsigned m9_split_stage;
/* M9X_SPLIT_PREPROCESS runs the preprocessing range ahead of every pass. */
static bool m9_split_preprocess;
/* M9X_SPLIT_DEPTH: 1 the caller's depth state in the first raster pass only,
 * 0 no depth at all in a split job, 2 the schedule that stops the engine. */
static unsigned m9_split_depth;
/* M9X_SPLIT_SUBMIT=0 puts every pass of a split shader in one submission. */
static bool m9_split_submit;
/* M9X_SPLIT_PASSES=N emits only the first N passes of a split shader. */
static unsigned m9_split_passes;
/* M9X_RECORD_DUMP names a file the inter-pass record is written to after
 * every split job, so the slots the engine touched can be read back and
 * the record's granularity measured rather than assumed. */
static const char *m9_record_dump;
/* Raster draws one split job restates.  Two of them stop the engine
 * (MEASURED, docs/parhelia-m9-fragment-pass-hardware.md), and one draw
 * also bounds the record: its fragment counter then covers a single
 * draw's fragments rather than a whole job's.  M9X_SPLIT_DRAWS raises
 * it so the limit can be re-measured. */
static unsigned m9_split_draws;
/* M9X_SPLIT_COVERAGE=0 keeps the plain coverage words on a record pass. */
static bool m9_split_coverage;
/* M9X_RECORD_89 replaces index 0x89 of a record pass, whose low twelve bits
 * are one three-bit render format class per bound target. */
static unsigned m9_record_89;
/* M9X_TRANSITION=0 drops the vendor's inter-draw engine transition from a
 * split job, which separates a fault in that block from one in the record. */
static bool m9_transition;
/* M9X_SPLIT=0 refuses the multi-pass schedule instead of emitting it.  It is
 * on: with each pass placed in its sequence at index ef the engine takes the
 * inter-pass record as a render target, and a five-fetch shader renders
 * pixel-exact (MEASURED). */
static bool m9_split_enable;
/* M9X_INDEXED=0 falls back to one record per index. */
static bool m9_indexed_draws = true;
static uint64_t m9_prof_ns[M9_PROF_COUNT];
static uint64_t m9_prof_hits[M9_PROF_COUNT];
static uint64_t m9_prof_records, m9_prof_jobs, m9_prof_varying_bytes;
static uint64_t m9_prof_draws, m9_prof_split[8];
static uint64_t m9_prof_indexed, m9_prof_referenced;
static uint64_t m9_prof_refused, m9_prof_skipped, m9_prof_relocs;
/* M9X_NO_CLEAR drops the clear quad; M9X_CLEAR_REPEAT draws it that many
 * extra times, which weighs one screen of fill with the frame still correct. */
static bool m9_no_clear;
static unsigned m9_clear_repeat;
static uint64_t m9_prof_frames, m9_prof_wall;

static inline uint64_t
m9_prof_begin(void)
{
   return m9_profile_period ? os_time_get_nano() : 0;
}

static inline void
m9_prof_end(enum m9_prof phase, uint64_t start)
{
   if (m9_profile_period) {
      m9_prof_ns[phase] += os_time_get_nano() - start;
      m9_prof_hits[phase]++;
   }
}

/* Called once per frame with the wall time of the whole frame. */
static void
m9_prof_frame(void)
{
   static uint64_t last;
   uint64_t now;
   double f;

   if (!m9_profile_period)
      return;
   now = os_time_get_nano();
   if (last)
      m9_prof_wall += now - last;
   last = now;
   if (++m9_prof_frames < m9_profile_period)
      return;
   f = (double)m9_prof_frames;
   fprintf(stderr, "m9x: %.0f frames %.2f ms/frame: ", f,
           m9_prof_wall / f / 1e6);
   for (unsigned i = 0; i < M9_PROF_COUNT; i++)
      fprintf(stderr, "%s %.2f ", m9_prof_name[i], m9_prof_ns[i] / f / 1e6);
   fprintf(stderr, "| jobs %.0f draws %.0f records %.0f varying KiB %.1f "
           "refused %.0f skipped %.0f relocs %.0f "
           "indexed %.0f referenced %.0f splits target %.0f program %.0f "
           "depth %.0f mask %.0f clip %.0f uniforms %.0f vertices %.0f "
           "stream %.0f\n",
           m9_prof_jobs / f, m9_prof_draws / f, m9_prof_records / f,
           m9_prof_varying_bytes / f / 1024.0, m9_prof_refused / f,
           m9_prof_skipped / f, (double)m9_prof_relocs, m9_prof_indexed / f,
           m9_prof_referenced / f, m9_prof_split[0] / f,
           m9_prof_split[1] / f, m9_prof_split[2] / f, m9_prof_split[3] / f,
           m9_prof_split[4] / f, m9_prof_split[5] / f, m9_prof_split[6] / f,
           m9_prof_split[7] / f);
   memset(m9_prof_ns, 0, sizeof(m9_prof_ns));
   memset(m9_prof_hits, 0, sizeof(m9_prof_hits));
   m9_prof_records = m9_prof_jobs = m9_prof_varying_bytes = 0;
   m9_prof_draws = m9_prof_indexed = m9_prof_referenced = 0;
   m9_prof_refused = m9_prof_skipped = m9_prof_relocs = 0;
   memset(m9_prof_split, 0, sizeof(m9_prof_split));
   m9_prof_frames = 0;
   m9_prof_wall = 0;
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
   /* At most 64 in both dimensions the ordinary tiled descriptor addresses
    * the surface wrongly - the content comes back band structured - while
    * the small 2-D kind reproduces it exactly, which is the same kind the
    * sampler binds for that size. */
   bool small = resource->width <= 64 && resource->height <= 64;
   struct drm_parhelia_m9_surface_access access = {
      .handle = resource->handle,
      .op = op,
      .descriptor0 = small ?
         M9_TEXTURE_CODE_BGRA << 24 | M9_TEXTURE_KIND_2D << 16 |
         resource->width : m9_surface_desc0(&desc),
      .descriptor1 = small ? 1u << 16 | resource->height :
                             m9_surface_desc1(&desc),
      .flags = small ? PARHELIA_M9_ACCESS_TEXTURE : 0,
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
   /* Every level is allocated with a 32-aligned row span, so that span is
    * what the window descriptor must carry: a 16-wide level lives in the
    * first 16 columns of a 32-wide surface.  Passing the texel width instead
    * left narrow levels with no window shape at all and their draws were
    * dropped. */
   uint32_t span = (lvl->width + 31) & ~31u;
   bool small = span <= 64 && lvl->height <= 64;
   size_t bytes = (size_t)box->width * box->height * 4;
   uint8_t *rows = MALLOC(bytes);
   struct drm_parhelia_m9_surface_access access = {
      .handle = resource->handle,
      .op = write ? PARHELIA_M9_ACCESS_WRITE : PARHELIA_M9_ACCESS_READ,
      /* The kernel window takes the small kind with both dimensions up to
       * 64, and the large kind only with a 32-aligned span. */
      .descriptor0 = (uint32_t)resource->texture_code << 24 |
                     (small ? M9_TEXTURE_KIND_2D :
                              M9_TEXTURE_KIND_RENDER) << 16 | span,
      .descriptor1 = 1u << 16 | lvl->height,
      .surface_width = span,
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
   if (!ret) {
      if (write) {
         ctx->window_writes++;
         ctx->window_write_bytes += bytes;
      } else {
         ctx->window_reads++;
         ctx->window_read_bytes += bytes;
      }
   }
   if (!ret && ioctl(ctx->screen->fd, DRM_IOCTL_PARHELIA_M9_SURFACE_ACCESS,
                     &access)) {
      ret = -errno;
      if (ctx->debug)
         fprintf(stderr, "m9x: window %s level %u %ux%u span %u off %#" PRIx64
                 " box %d,%d %ux%u d0 %08x d1 %08x: %s\n",
                 write ? "store" : "fetch", level, lvl->width, lvl->height,
                 span, (uint64_t)lvl->offset, box->x, box->y, box->width,
                 box->height, access.descriptor0, access.descriptor1,
                 strerror(-ret));
   }
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
m9_transfer_map_locked(struct pipe_context *pctx, struct pipe_resource *pres,
                       unsigned level, unsigned usage,
                       const struct pipe_box *box, struct pipe_transfer **out)
{
   struct m9_context *ctx = m9_context(pctx);
   struct m9_resource *resource = (struct m9_resource *)pres;
   struct m9_transfer *transfer;
   uint64_t offset = 0, last;
   unsigned width, height, stride;
   /* Layers of this level: cube faces, volume slices, one for a plane. */
   unsigned layers = resource && level < resource->texture.levels ?
                     resource->texture.faces *
                     resource->texture.level[level].depth : 1;

   if (!resource || !resource->map ||
       ((usage & PIPE_MAP_WRITE) && !resource->map_writable) ||
       !box || !out || box->x < 0 ||
       box->y < 0 || box->z < 0 || box->width < 0 || box->height < 0 ||
       box->depth < 1 || (unsigned)box->z + box->depth > layers ||
       level > pres->last_level) {
      if (ctx->debug && resource && box)
         fprintf(stderr, "m9x: map refused: %s %ux%u target %u level %u/%u "
                 "box %d,%d,%d %dx%dx%d usage %#x map %p writable %d kind %u\n",
                 util_format_short_name(pres->format), pres->width0,
                 pres->height0, pres->target, level, pres->last_level,
                 box->x, box->y, box->z, box->width, box->height, box->depth,
                 usage, resource->map, resource->map_writable, resource->kind);
      return NULL;
   }
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
      ctx->map_usage_seen |= usage;
      /* Storage is read and written through the box alone, so a write-only
       * map never observes the old content: the read back is dead work. */
      bool preserve = (usage & PIPE_MAP_READ) ||
                      (!(usage & (PIPE_MAP_DISCARD_RANGE |
                                  PIPE_MAP_DISCARD_WHOLE_RESOURCE)) &&
                       !(usage & PIPE_MAP_WRITE));

      stride = box->width * util_format_get_blocksize(pres->format);
      transfer->staging = box->width && box->height ?
                          MALLOC((size_t)stride * box->height * box->depth) :
                          NULL;
      if (preserve) {
         ctx->window_read_usage |= usage;
         ctx->window_reads_explicit += !!(usage & PIPE_MAP_READ);
      }
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
                      (!(usage & (PIPE_MAP_DISCARD_RANGE |
                                  PIPE_MAP_DISCARD_WHOLE_RESOURCE)) &&
                       !(usage & PIPE_MAP_WRITE));

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
         if (ctx->debug)
            fprintf(stderr, "m9x: map beyond BO: %s %ux%u level %u box %d,%d "
                    "%dx%d offset %#" PRIx64 " last %#" PRIx64 " size %#"
                    PRIx64 " kind %u\n", util_format_short_name(pres->format),
                    pres->width0, pres->height0, level, box->x, box->y,
                    box->width, box->height, offset, last, resource->size,
                    resource->kind);
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
                                 (uintptr_t)stride * box->height;
   *out = &transfer->base;
   return transfer->staging ? transfer->staging :
          (uint8_t *)resource->map + offset;
}

static void *
m9_transfer_map(struct pipe_context *pctx, struct pipe_resource *pres,
                unsigned level, unsigned usage, const struct pipe_box *box,
                struct pipe_transfer **out)
{
   uint64_t t_transfer = m9_prof_begin();
   void *map = m9_transfer_map_locked(pctx, pres, level, usage, box, out);

   m9_prof_end(M9_PROF_TRANSFER, t_transfer);
   return map;
}

static void
m9_transfer_unmap(struct pipe_context *pctx, struct pipe_transfer *ptransfer)
{
   struct m9_transfer *transfer = (struct m9_transfer *)ptransfer;

   if (transfer->staging && m9_res(ptransfer->resource)->texture.levels) {
      struct m9_resource *resource = m9_res(ptransfer->resource);
      uint32_t bit = 1u << ptransfer->level;

      if (ptransfer->usage & PIPE_MAP_WRITE) {
         int store = m9_texture_access(m9_context(pctx), resource,
                                       ptransfer->level, true, &ptransfer->box,
                                       transfer->staging, ptransfer->stride);

         /* M9X_TEXTURE_DUMP=dir writes each uploaded level 0 as a PPM, so
          * the content a surface samples can be looked at directly. */
         if (m9_texture_dump && !ptransfer->level) {
            const uint8_t *p = transfer->staging;
            char path[512];
            FILE *f;

            snprintf(path, sizeof(path), "%s/tex%03u_%ux%u.ppm",
                     m9_texture_dump, resource->handle, ptransfer->box.width,
                     ptransfer->box.height);
            f = fopen(path, "wb");
            if (f) {
               fprintf(f, "P6\n%u %u\n255\n", ptransfer->box.width,
                       ptransfer->box.height);
               for (unsigned y = 0; y < ptransfer->box.height; y++)
                  for (unsigned x = 0; x < ptransfer->box.width; x++) {
                     const uint8_t *t = p + (size_t)y * ptransfer->stride +
                                        (size_t)x * 4;

                     fwrite((uint8_t[]){ t[0], t[1], t[2] }, 1, 3, f);
                  }
               fclose(f);
            }
         }

         if (store) {
            resource->texture_unplaced |= bit;
            m9_warn_once(m9_context(pctx), M9_WARN_TRANSFER,
                         "texture level cannot be stored: no captured "
                         "placement and no kernel texture window");
         } else {
            resource->texture_unplaced &= ~bit;
            resource->levels_written |= bit;
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

/* Swap in the other set of per-job arenas; see m9_context's declaration. */
static void
m9_arena_rotate(struct m9_context *ctx)
{
   struct m9_resource tmp;

#define ARENA_SWAP(a, b) do { tmp = (a); (a) = (b); (b) = tmp; } while (0)
   ARENA_SWAP(ctx->source, ctx->source_alt);
   ARENA_SWAP(ctx->system, ctx->system_alt);
   ARENA_SWAP(ctx->index, ctx->index_alt);
   ARENA_SWAP(ctx->cpu_streams, ctx->cpu_streams_alt);
#undef ARENA_SWAP
}

static int
m9_arena_reserve(struct m9_context *ctx, struct m9_resource *arena,
                 uint64_t bytes)
{
   uint64_t size;

   if (bytes <= arena->size)
      return 0;
   size = MAX2((bytes + 0xfff) & ~0xfffull, 2 * arena->size);
   m9_bo_destroy(ctx->screen, arena);
   return m9_bo_create(ctx->screen, size, 0, M9_RESOURCE_LINEAR_BUFFER,
                       arena);
}

static bool
m9_varying_is_color(unsigned location)
{
   return location == VARYING_SLOT_COL0 || location == VARYING_SLOT_COL1;
}

/* Transient view 0 is the preprocessing pass's own varying target.  The
 * captured pass-through program puts the primary colour there; a native
 * vertex program puts its one varying there, whatever its slot. */
static bool
m9_varying_from_pass(const struct m9_job_key *key, unsigned location)
{
   if (key->vs)
      return key->vs->has_varying && key->vs->varying_location == location;
   return location == VARYING_SLOT_COL0;
}

/* The per-fragment record a split shader hands its values through.  Its
 * granularity is not decoded, so m9_record_bytes covers one record per
 * covered pixel rounded out to whole 64-pixel blocks; the object is grown
 * rather than reallocated per job. */
static bool
m9_record_grow(struct m9_context *ctx, unsigned dwords, const char **reason)
{
   struct m9_screen *screen = ctx->screen;
   uint64_t bytes;
   unsigned want;
   bool ok = true;

   simple_mtx_lock(&screen->record_lock);
   want = MAX2(dwords, screen->record_dwords);
   bytes = m9_record_bytes(ctx->params.width, ctx->params.height, want);
   if (!bytes) {
      *reason = "the inter-pass record has no size";
      ok = false;
   } else if (!screen->record.handle || screen->record.size < bytes ||
              screen->record_dwords < dwords) {
      bytes = MAX2(bytes, screen->record.size);
      /* The old object goes first: two records of this size do not fit
       * beside each other. */
      m9_bo_destroy(screen, &screen->record);
      if (m9_bo_create(screen, bytes, PARHELIA_M9_BO_VRAM,
                       M9_RESOURCE_LINEAR_COLOR, &screen->record)) {
         /* Name the size: a record is tens of MiB and which stride asked
          * for it is the difference between a driver fault and the
          * hardware's own cost. */
         fprintf(stderr, "m9x: inter-pass record of %u MiB for %ux%u stride "
                 "%u refused\n", (unsigned)(bytes >> 20),
                 ctx->params.width, ctx->params.height, want);
         *reason = "cannot allocate the inter-pass record";
         screen->record_dwords = 0;
         ok = false;
      } else {
         screen->record_dwords = want;
      }
   }
   simple_mtx_unlock(&screen->record_lock);
   return ok;
}

/* Raster constants, CPU varying views and per-pass state of a native
 * fragment job.  A shader the engine runs in one pass produces one entry,
 * which the emitter drives through its single-pass path unchanged. */
static int
m9_job_native(struct m9_context *ctx, const struct m9_job_key *key,
              unsigned records, unsigned index_bytes,
              struct m9_raster_pass *out)
{
   const struct m9_fs_variant *fs = key->variant;
   const float *extra = ctx->extra.data;
   unsigned cpu_streams = key->vs ? 0 : fs->extra_count;
   uint64_t const_offset = M9_RASTER_CONST_OFFSET;
   uint64_t t_varying;
   int ret;

   /* The spans of the passes lie consecutively in one arena, as the
    * captured two-pass and five-sampler schedules lay them out. */
   memcpy((uint8_t *)ctx->system.map + M9_RASTER_CONST_OFFSET,
          ctx->job_consts.data, ctx->job_consts.size);
   t_varying = m9_prof_begin();
   for (unsigned j = 0; j < cpu_streams; j++) {
      uint8_t *view = (uint8_t *)ctx->cpu_streams.map +
                      j * M9_TRANSIENT_VIEW_BYTES;

      for (unsigned r = 0; r < records; r++)
         memcpy(view + r * M9_TRANSIENT_VERTEX_BYTES,
                extra + (r * fs->extra_count + j) * 4, 4 * sizeof(float));
      /* Each pass draws its own copy of the geometry, so its varyings are
       * repeated behind the first set. */
      for (unsigned i = 1; i < fs->passes.count; i++)
         memcpy(view + (uint64_t)i * records * M9_TRANSIENT_VERTEX_BYTES,
                view, (uint64_t)records * M9_TRANSIENT_VERTEX_BYTES);
   }
   m9_prof_varying_bytes += (uint64_t)records * cpu_streams *
                            fs->passes.count * M9_TRANSIENT_VERTEX_BYTES;
   m9_prof_end(M9_PROF_VARYING, t_varying);

   for (unsigned i = 0; i < fs->passes.count; i++) {
      const struct m9_fs_program *p = &fs->passes.pass[i];
      const struct m9_fs_pass_streams *st = &fs->stream[i];
      uint32_t views[M9_FS_MAX_VARYINGS], coords[M9_FS_MAX_TEXTURES];
      bool flat[M9_FS_MAX_VARYINGS];
      unsigned consts = util_dynarray_num_elements(&p->consts,
                                                   struct m9_fs_const);
      struct m9_raster_pass *pass = &out[i];

      memset(pass, 0, sizeof(*pass));
      /* A varying the preprocessing pass itself writes comes from stream
       * view 0; everything else the CPU gathers into a view of its own. */
      for (unsigned k = 0; k < p->varying_count; k++) {
         flat[k] = m9_varying_is_color(p->varyings[k].location);
         views[k] = st->varying_extra[k] < 0 ||
                    m9_varying_from_pass(key, p->varyings[k].location) ? 0 :
                    M9_TRANSIENT_CPU_VIEW + st->varying_extra[k];
      }
      for (unsigned c = 0; c < st->coord_count; c++)
         coords[c] = st->coord_extra[c] < 0 ||
                     m9_varying_from_pass(key, st->coord_location[c]) ? 0 :
                     M9_TRANSIENT_CPU_VIEW + st->coord_extra[c];
      ret = m9_raster_fs_init_textured(&pass->fs, fs->vertex_entry,
                                       m9_fs_raster_88(p), consts,
                                       p->varying_count, flat, views,
                                       st->coord_count, coords,
                                       p->texture_count, st->texture_coord);
      if (ret)
         return ret;
      pass->entry = fs->entry[i];
      pass->vertex_offset = i * records;
      pass->index_offset = i * index_bytes;
      pass->const_offset = const_offset;
      const_offset += 8ull * consts;
      pass->writes_record = p->writes_record && m9_split_stage != 1;
      /* Every producer owns a four-dword slot of the record, so a pass
       * reads the slots of the passes before it while writing its own. */
      /* Stage 4 binds the record object with an ordinary colour
       * descriptor, which separates the 0x77 record format from the
       * allocation it names. */
      pass->record_desc = !pass->writes_record ? 0 :
                          m9_split_stage == 4 ||
                          m9_split_stage == 7 ? ctx->params.color_desc :
                          m9_split_stage == 5 ?
                          m9_record_desc(M9_RECORD_PAIR, 2, 0) :
                          m9_split_stage == 6 ?
                          m9_record_desc(M9_RECORD_SLOT, fs->record_dwords,
                                         fs->record_offset[i]) |
                          (ctx->params.color_desc & 0xffffu) :
                          m9_record_desc(M9_RECORD_SLOT, fs->record_dwords,
                                         fs->record_offset[i]);
      /* Only the first pass of a split sequence drives the depth unit.
       * Leaving the test on in a continuation pass stops the engine
       * (MEASURED: conv3nowrited and conv3d both time out, conv3notestd
       * renders), and the captured schedules restate a8 with the write bit
       * cleared after the first pass, so the sequence evaluates depth once
       * and the passes behind it inherit that coverage.  M9X_SPLIT_DEPTH
       * selects the schedule: 1 the first pass only, 0 no depth at all,
       * 2 the schedule that stops the engine. */
      bool pass_depth = m9_split_depth == 0 ? fs->passes.count < 2 : !i;

      pass->depth_control = pass_depth ? key->depth_control :
                            m9_depth_control(false, false, PIPE_FUNC_LESS);
      pass->depth_ef = pass_depth ? key->depth_ef : 0;
      if (m9_split_depth == 2 && i) {
         pass->depth_control = key->depth_control & ~0x2u;
         pass->depth_ef = key->depth_ef & ~0x8000u;
      }
      /* Bits 10:9 of ef place a raster pass in its sequence: 1 the first of
       * several, 2 a middle one, 3 the last.  Every single-pass draw leaves
       * them clear (CAPTURED, 0x200/0x600 in all seven two-pass cases and
       * 0x200/0x400/0x400/0x400/0x600 in the five-sampler schedule). */
      pass->sequence_ef = fs->passes.count < 2 ? 0 :
                          !i ? 0x200u :
                          i + 1 < fs->passes.count ? 0x400u : 0x600u;
      /* Index 0x89 holds one three-bit render format class per bound target
       * at bit 3*s: class 4 for the record and 0 for a colour target
       * (CAPTURED, fe007ffc against fe007ff8 in both schedules). */
      /* Stage 7 keeps the colour pass's own state words, so only the
       * object bound at b0 separates it from an ordinary draw. */
      bool record_state = pass->writes_record && m9_split_stage != 7;

      pass->r89 = m9_record_89 && pass->writes_record ? m9_record_89 :
                  record_state ? 0xfe007ffcu : 0xfe207ff8u;
      pass->a9 = record_state && m9_split_coverage ? 0x88ffff00u :
                                                     0x08ffff00u;
      pass->ae = record_state && m9_split_coverage ? 0x32444442u :
                                                     0x22444442u;
      pass->unit_count = p->texture_count;
      for (unsigned k = 0; k < M9_TEXTURE_MAX_UNITS; k++)
         pass->routes[k].word = 0x100;
      for (unsigned k = 0; k < p->texture_count; k++) {
         unsigned src;

         pass->unit_index[k] = st->unit[k];
         if (m9_split_stage == 1 || m9_split_stage == 2 ||
             m9_split_stage == 4)
            continue;
         /* A coordinate route redirects where this unit samples and leaves
          * its texture and sampler alone: the record's first dword is the
          * descriptor's own offset and the second is named in the word
          * (CAPTURED, docs/parhelia-m9-fragment-pass-routes.md). */
         if (p->texcoord_source[k] != M9_FS_ROUTE_NONE) {
            src = fs->record_offset[p->texcoord_source[k]];
            pass->routes[k].word =
               m9_record_route(true, k, src + 1, M9_RECORD_ABSENT, 0);
            pass->routes[k].desc = m9_record_desc(M9_RECORD_PAIR,
                                                  fs->record_dwords, src);
            pass->route_count = MAX2(pass->route_count, k + 1);
            continue;
         }
         if (p->texture_source[k] == M9_FS_ROUTE_NONE)
            continue;
         /* A routed value arrives in the texel registers of its own slot,
          * so route index and texel slot are the same k. */
         src = fs->record_offset[p->texture_source[k]];
         pass->routes[k].word =
            m9_record_route(false, k, src + 1, src + 2, src + 3);
         pass->routes[k].desc = m9_record_desc(M9_RECORD_SLOT,
                                               fs->record_dwords, src);
         pass->route_count = MAX2(pass->route_count, k + 1);
      }
      /* A record-writing pass that samples nothing still binds unit 0:
       * setting 88 bit 20 without one stops the engine. */
      if (!p->texture_count) {
         pass->unit_index[0] = M9_PASS_UNIT_PLACEHOLDER;
         pass->unit_count = 1;
      }
   }
   return 0;
}

/* Job words, relocations, fragment slots and constants for offline decode. */
static void
m9_dump_job(struct m9_context *ctx, const struct m9_fs_variant *fs)
{
   const struct m9_batch *b = &ctx->batch;
   /* M9X_DUMP_FROM / M9X_DUMP_COUNT select a window of jobs by their running
    * number, so a long run leaves one decodable stretch from mid-game. */
   unsigned seq = ctx->dump_seq++;
   FILE *out;

   if (seq < ctx->dump_from || seq >= ctx->dump_from + ctx->dump_count)
      return;
   out = fopen(ctx->dump_path, seq == ctx->dump_from ? "w" : "a");
   if (!out)
      return;
   fprintf(out, "JOB %u dwords %u relocs %u\n", seq,
           b->command_count, b->reloc_count);
   for (unsigned i = 0; i < b->command_count; i++)
      fprintf(out, "%08x%c", b->command[i], i % 8 == 7 ? '\n' : ' ');
   fputc('\n', out);
   for (unsigned i = 0; i < b->reloc_count; i++)
      fprintf(out, "RELOC dword %u handle %u offset %#" PRIx64 " kind %u\n",
              b->relocs[i].command_dword, b->relocs[i].handle,
              (uint64_t)b->relocs[i].target_offset, b->relocs[i].kind);
   /* The kernel compares each ref's flags against the union of its relocs,
    * and every target offset against the object size. */
   for (unsigned i = 0; i < b->bo_count; i++) {
      uint64_t size = 0;

      if (b->bos[i].handle == ctx->command.handle) size = ctx->command.size;
      else if (b->bos[i].handle == ctx->system.handle) size = ctx->system.size;
      else if (b->bos[i].handle == ctx->program.handle) size = ctx->program.size;
      else if (b->bos[i].handle == ctx->fragment_db.handle) size = ctx->fragment_db.size;
      else if (b->bos[i].handle == ctx->transient.handle) size = ctx->transient.size;
      else if (b->bos[i].handle == ctx->cpu_streams.handle) size = ctx->cpu_streams.size;
      else if (b->bos[i].handle == ctx->be.handle) size = ctx->be.size;
      else if (b->bos[i].handle == ctx->source.handle) size = ctx->source.size;
      else if (b->bos[i].handle == ctx->index.handle) size = ctx->index.size;
      fprintf(out, "BO handle %u flags %u size %#" PRIx64 "\n",
              b->bos[i].handle, b->bos[i].flags, size);
   }
   if (fs) {
      const float *k = ctx->job_consts.data;
      unsigned base = 0;

      for (unsigned pi = 0; pi < fs->passes.count; pi++) {
         const struct m9_fs_program *p = &fs->passes.pass[pi];
         const uint32_t *w = p->slots.data;

         for (unsigned i = 0; i < m9_fs_slot_count(p); i++)
            fprintf(out, "SLOT %u %08x %08x\n", base + i, w[2 * i],
                    w[2 * i + 1]);
         base += m9_fs_slot_count(p);
      }
      for (unsigned i = 0; i < ctx->job_consts.size / sizeof(float); i++)
         fprintf(out, "CONST %u %g\n", i, k[i]);
   }
   fclose(out);
}

static int
m9_submit_job(struct m9_context *ctx)
{
   unsigned draws = util_dynarray_num_elements(&ctx->draws,
                                               struct m9_draw_record);
   unsigned records = util_dynarray_num_elements(&ctx->records, float) /
                      ctx->record_floats;
   const struct m9_job_key *key = &ctx->job_key;
   uint32_t vs_decl[M9_VS_MAX_DECL];
   unsigned vs_decl_count = 0, vs_const_pairs = 0;
   const struct m9_fs_state *fs = key->fs;
   const struct m9_draw_record *first_draw = ctx->draws.data;
   struct m9_raster_pass passes[M9_FS_MAX_PASSES];
   struct m9_texture_unit placeholder;
   unsigned pass_count = fs ? key->variant->passes.count : 0;
   struct m9_draw_job job;
   uint64_t point = 0;
   uint64_t t_submit;
   bool built;
   int ret;

   if (!draws)
      return 0;
   t_submit = m9_prof_begin();
   m9_prof_jobs++;
   m9_prof_records += records;
   /* One clear of the 35 KiB batch per job: the build below is its only
    * writer, so clearing it again on the way out was dead work. */
   m9_batch_init(&ctx->batch);
   /* M9X_ONLY_FIRST / M9X_ONLY_AFTER drop every job of a frame outside that
    * range, which isolates the draws a screen region comes from. */
   if (ctx->frame_jobs++ < ctx->only_after ||
       ctx->frame_jobs > ctx->only_first) {
      util_dynarray_foreach(&ctx->job_textures, struct pipe_resource *, held)
         pipe_resource_reference(held, NULL);
      util_dynarray_clear(&ctx->job_textures);
      ctx->job_unit_dwords = 0;
      util_dynarray_clear(&ctx->records);
      util_dynarray_clear(&ctx->indices);
      util_dynarray_clear(&ctx->draws);
      util_dynarray_clear(&ctx->extra);
      return 0;
   }
   unsigned index_count = util_dynarray_num_elements(&ctx->indices, uint16_t);
   /* Copies of the geometry the job preprocesses: one a raster pass. */
   unsigned copies = MAX2(pass_count, 1u);
   uint64_t index_base = 0;

   /* Before a single byte of this job is written. */
   m9_arena_rotate(ctx);
   ret = ctx->lost ? -EIO :
         m9_arena_reserve(ctx, &ctx->source,
                          (uint64_t)records * copies * ctx->record_floats *
                          sizeof(float));
   if (!ret && ctx->indices.size)
      ret = m9_arena_reserve(ctx, &ctx->index, ctx->indices.size * copies);
   if (!ret) {
      uint64_t t_records = m9_prof_begin();

      /* A split shader draws its geometry once a pass, each from its own
       * copy of the preprocessed records. */
      for (unsigned i = 0; i < copies; i++)
         memcpy((uint8_t *)ctx->source.map + i * ctx->records.size,
                ctx->records.data, ctx->records.size);
      for (unsigned i = 0; i < (ctx->indices.size ? copies : 0); i++) {
         uint16_t *out = (uint16_t *)ctx->index.map + i * index_count;
         const uint16_t *in = ctx->indices.data;

         for (unsigned k = 0; k < index_count; k++)
            out[k] = in[k] + i * records;
      }
      memcpy(ctx->system.map, ctx->params.system_prefix,
             sizeof(ctx->params.system_prefix));
      memcpy((uint8_t *)ctx->system.map + M9_LINKED_OFFSET,
             ctx->params.linked_prefix, sizeof(ctx->params.linked_prefix));
      m9_prof_end(M9_PROF_RECORDS, t_records);
      if (key->vs) {
         /* The compiled program replaces the transform: its own constants
          * take the matrix pairs, and the tail applies the viewport in
          * force rather than the target's own. */
         m9_draw_params_viewport(&ctx->params, key->viewport_scale,
                                 key->viewport_bias, ctx->system.map);
         vs_decl_count = m9_vs_decl_words(key->vs, vs_decl,
                                          ARRAY_SIZE(vs_decl));
         vs_const_pairs =
            m9_vs_const_span(key->vs, ctx->job_vs_consts.data,
                             util_dynarray_num_elements(&ctx->job_vs_consts,
                                                        float),
                             (uint32_t *)((uint8_t *)ctx->system.map +
                                          M9_VS_CONST_OFFSET),
                             2 * M9_VS_CONST_MAX_PAIRS);
         if (!vs_decl_count || !vs_const_pairs)
            ret = -EINVAL;
      }
      if (fs) {
         const char *why = NULL;

         /* A split shader needs the record before its first pass runs. */
         if (pass_count > 1 &&
             !m9_record_grow(ctx, key->variant->record_dwords, &why)) {
            m9_warn_once(ctx, M9_WARN_MEMORY, why);
            m9_skip_reason(ctx, why);
            ret = -EINVAL;
         } else {
            ret = m9_job_native(ctx, key, records,
                                index_count * sizeof(uint16_t), passes);
         }
      }
      job = (struct m9_draw_job) {
         .params = &ctx->params,
         .decl = &ctx->decl,
         .source = ctx->source.handle,
         .index = ctx->index.handle,
         .system = ctx->system.handle,
         .program = ctx->program.handle,
         .fragment_db = ctx->fragment_db.handle,
         .transient = ctx->transient.handle,
         .cpu_streams = ctx->cpu_streams.handle,
         .color = m9_res(ctx->color)->handle,
         .depth = key->depth->handle,
         .aux = key->aux->handle,
         .be = ctx->be.handle,
         .source_vertices = records * copies,
         .index_base = (uint32_t)index_base,
         .index_count = index_count * copies,
         .draws = ctx->draws.data,
         .draw_count = draws,
         .depth_control = key->depth_control,
         .depth_ef = key->depth_ef,
         .color_mask = key->color_mask,
         .clip_min = key->clip_min,
         .clip_max = key->clip_max,
         /* One pass is emitted through the single-pass path, so the
          * captured stream is unchanged; more become a pass loop.
          * M9X_PASS_LOOP drives a one-pass shader through the loop too,
          * which separates a fault in the loop from one in the record. */
         .fs = pass_count == 1 && !m9_pass_loop ? &passes[0].fs : NULL,
         .passes = pass_count > 1 || (pass_count && m9_pass_loop) ?
                   passes : NULL,
         .pass_count = pass_count > 1 || (pass_count && m9_pass_loop) ?
                       pass_count : 0,
         .units = first_draw->unit_count ? first_draw->units : NULL,
         .unit_count = first_draw->unit_count,
      };
      if (fs) {
         job.program = key->variant->c2.handle;
         job.blend_dest = m9_fs_final(key->variant)->blend_dest;
      }
      if (pass_count > 1 || (pass_count && m9_pass_loop)) {
         /* The unit pool of a split job is the key's, not a draw's: its
          * passes index the pool and a draw cannot restate it. */
         job.units = key->unit_count ? key->units : NULL;
         job.unit_count = key->unit_count;
         job.record = ctx->screen->record.handle;
         /* M9X_SPLIT_PREPROCESS=0 keeps one preprocessing range for the
          * whole job, which is what the vendor emits; the default runs one
          * ahead of every raster pass. */
         job.preprocess_per_pass = m9_split_preprocess;
         if (pass_count > 1 && m9_split_submit)
            job.pass_count = 1;
         m9_texture_unit_placeholder(&placeholder,
                                     ctx->placeholder_texture.handle);
         job.placeholder = &placeholder;
         /* The vendor's inter-draw engine transition, which a single-pass
          * job never carries so the captured stream stays byte for byte. */
         job.transition = m9_transition;
      }
      if (key->vs) {
         job.program = key->vs_program;
         job.vs_decl = vs_decl;
         job.vs_decl_count = vs_decl_count;
         job.vs_record_bytes = key->vs->record_bytes;
         job.vs_const_pairs = vs_const_pairs;
      }
      if (!ret)
         ret = m9_batch_emit_draw_job(&ctx->batch, &job);
   }
   built = !ret;
   if (!ret) {
      uint64_t t_ioctl;

      ctx->batch.sealed = true;
      if (ctx->dump_path)
         m9_dump_job(ctx, fs ? key->variant : NULL);
      t_ioctl = m9_prof_begin();
      ret = ctx->dry_run ?
            m9_batch_validate(ctx->screen, &ctx->command, &ctx->batch,
                              ctx->context_id) :
            m9_batch_submit(ctx->screen, &ctx->command, &ctx->batch,
                            ctx->context_id, &point);
      /* A second raster pass inside one submission stalls the engine
       * (MEASURED): the vendor separates its passes with an engine
       * transition the kernel does not accept.  Every pass after the
       * first is therefore submitted on its own, which the transport
       * serialises against the one before it. */
      unsigned emit = m9_split_passes ? MIN2(m9_split_passes, copies) :
                      copies;

      for (unsigned pi = 1; !ret && m9_split_submit && pi < emit; pi++) {
         m9_batch_init(&ctx->batch);
         job.passes = &passes[pi];
         job.pass_count = 1;
         ret = m9_batch_emit_draw_job(&ctx->batch, &job);
         if (ret)
            break;
         ctx->batch.sealed = true;
         if (ctx->dump_path)
            m9_dump_job(ctx, key->variant);
         ret = ctx->dry_run ?
               m9_batch_validate(ctx->screen, &ctx->command, &ctx->batch,
                                 ctx->context_id) :
               m9_batch_submit(ctx->screen, &ctx->command, &ctx->batch,
                               ctx->context_id, &point);
      }
      m9_prof_end(M9_PROF_IOCTL, t_ioctl);
   }
   if (ret == -EINVAL) {
      /* A refused job is one unsupported state, not a broken context. */
      const char *who = built ? "the kernel refused a job; those draws are "
                                "skipped" :
                                "a job could not be built; those draws are "
                                "skipped";

      m9_warn_once(ctx, M9_WARN_SUBMIT, who);
      m9_skip_reason(ctx, who);
      ctx->skipped_draws++;
      m9_prof_refused++;
      m9_prof_skipped += draws;
      /* Name the refused job so the failing validator check is identifiable. */
      if (ctx->debug)
         fprintf(stderr, "m9x: refused job (%s): words %u relocs %u bos %u "
                 "records %u draws %u units %u fs %d blend_dest %d "
                 "indices %u base %#x first %u+%u\n",
                 built ? "kernel" : "build",
                 ctx->batch.command_count, ctx->batch.reloc_count,
                 ctx->batch.bo_count, records, draws, key->unit_count,
                 !!fs, fs ? (int)m9_fs_final(key->variant)->blend_dest : -1,
                 index_count, (unsigned)index_base, first_draw->index_first,
                 first_draw->index_count);
   } else if (ret) {
      if (!ctx->lost)
         fprintf(stderr, "m9x: two-pass job failed (%s); context lost\n",
                 strerror(-ret));
      ctx->lost = true;
   } else {
      ctx->last_point = point;
      ctx->record_written |= pass_count > 1;
      m9_prof_relocs = MAX2(m9_prof_relocs, ctx->batch.reloc_count);
      if (ctx->debug)
         fprintf(stderr, "m9x: job %u records %u draws a8 %08x ef %08x "
                 "ab %08x clip %08x/%08x -> %#" PRIx64 "\n", records, draws,
                 key->depth_control, key->depth_ef, key->color_mask,
                 key->clip_min, key->clip_max, point);
   }
   util_dynarray_foreach(&ctx->job_textures, struct pipe_resource *, held)
      pipe_resource_reference(held, NULL);
   util_dynarray_clear(&ctx->job_textures);
   ctx->job_unit_dwords = 0;
   util_dynarray_clear(&ctx->records);
   util_dynarray_clear(&ctx->indices);
   util_dynarray_clear(&ctx->draws);
   util_dynarray_clear(&ctx->extra);
   m9_prof_end(M9_PROF_SUBMIT, t_submit);
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
               const struct util_dynarray *consts,
               const struct util_dynarray *vs_consts, unsigned record_floats,
               unsigned vertices, uint32_t *first, float **extra)
{
   unsigned padded = m9_draw_preprocess_vertices(vertices);
   unsigned extra_count = key->variant && !key->vs ?
                          key->variant->extra_count : 0;
   unsigned dwords;
   unsigned draws = util_dynarray_num_elements(&ctx->draws,
                                               struct m9_draw_record);
   unsigned records = draws ?
                      util_dynarray_num_elements(&ctx->records, float) /
                      ctx->record_floats : 0;
   float *out;

   /* A split shader preprocesses one copy of the geometry a pass. */
   unsigned copies = key->variant && key->fs ? key->variant->passes.count : 1;

   if (padded * copies > m9_draw_max_source_vertices() || !record_floats)
      return NULL;
   /* The bound textures are restated per draw, so they need not end a job;
    * only their extra stream words have to fit the command budget. */
   unsigned passes = copies;
   bool units_differ = key->unit_count != ctx->job_key.unit_count ||
                       memcmp(key->units, ctx->job_key.units,
                              key->unit_count * sizeof(key->units[0]));
   unsigned unit_dwords = ctx->job_unit_dwords +
                          (draws && units_differ && passes == 1 ?
                           m9_draw_units_dwords(key->unit_count) : 0);

   dwords = unit_dwords +
            (key->fs ?
             (passes > 1 ? m9_variant_dwords(key->variant, draws + 1) :
              m9_draw_job_dwords_fs(draws + 1, key->unit_count)) :
             m9_draw_job_dwords(draws + 1)) +
            m9_draw_decl_dwords(key->vs ? 2 * key->vs->attr_count + 1 : 5);
   if (draws) {
      bool state = memcmp(key, &ctx->job_key,
                          offsetof(struct m9_job_key, units)) != 0;

      if (state && (ctx->debug || m9_profile_period)) {
         const struct m9_job_key *o = &ctx->job_key;

         ctx->split_target += key->depth != o->depth || key->aux != o->aux;
         ctx->split_program += key->fs != o->fs || key->variant != o->variant;
         ctx->split_depth += key->depth_control != o->depth_control ||
                             key->depth_ef != o->depth_ef;
         ctx->split_mask += key->color_mask != o->color_mask;
         ctx->split_clip += key->clip_min != o->clip_min ||
                            key->clip_max != o->clip_max;
         m9_prof_split[0] += key->depth != o->depth || key->aux != o->aux;
         m9_prof_split[1] += key->fs != o->fs || key->variant != o->variant;
         m9_prof_split[2] += key->depth_control != o->depth_control ||
                             key->depth_ef != o->depth_ef;
         m9_prof_split[3] += key->color_mask != o->color_mask;
         m9_prof_split[4] += key->clip_min != o->clip_min ||
                             key->clip_max != o->clip_max;
      }
      bool uniforms = (key->fs && m9_consts_differ(consts, &ctx->job_consts)) ||
                      (key->vs && m9_consts_differ(vs_consts,
                                                   &ctx->job_vs_consts)) ||
                      record_floats != ctx->record_floats;
      bool vertices_full = (records + padded) * copies >
                           m9_draw_max_source_vertices();
      unsigned relocs = passes > 1 ?
                        m9_variant_relocs(key->variant, draws + 1) :
                        m9_draw_job_relocs(draws + 1, key->unit_count);
      /* A split shader binds one unit pool for the whole job, so a draw
       * that needs another set starts a job of its own. */
      bool stream_full = dwords > M9_MAX_COMMAND_DWORDS ||
                         relocs > M9_MAX_RELOCS ||
                         (passes > 1 && units_differ) ||
                         (passes > 1 && draws >= m9_split_draws) ||
                         (m9_job_draws && draws >= m9_job_draws);

      if (state || uniforms || vertices_full || stream_full) {
         m9_prof_split[5] += !state && uniforms;
         m9_prof_split[6] += !state && !uniforms && vertices_full;
         m9_prof_split[7] += !state && !uniforms && !vertices_full;
         ctx->split_state += state;
         ctx->split_uniforms += !state && uniforms;
         ctx->split_vertices += !state && !uniforms && vertices_full;
         ctx->split_stream += !state && !uniforms && !vertices_full;
         m9_submit_job(ctx);
         records = 0;
         unit_dwords = 0;
      }
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
      util_dynarray_clear(&ctx->job_vs_consts);
      if (key->vs && vs_consts->size &&
          !util_dynarray_grow_bytes(&ctx->job_vs_consts, 1, vs_consts->size))
         return NULL;
      if (key->vs && vs_consts->size)
         memcpy(ctx->job_vs_consts.data, vs_consts->data, vs_consts->size);
      ctx->record_floats = record_floats;
   }
   ctx->job_key = *key;
   ctx->job_unit_dwords = unit_dwords;
   for (unsigned k = 0; k < key->unit_count; k++) {
      struct pipe_resource **held =
         util_dynarray_grow(&ctx->job_textures, struct pipe_resource *, 1);

      if (!held)
         return NULL;
      *held = NULL;
      pipe_resource_reference(held, key->unit_res[k]);
   }
   *extra = NULL;
   if (extra_count) {
      *extra = util_dynarray_grow(&ctx->extra, float,
                                  padded * 4 * extra_count);
      if (!*extra)
         return NULL;
   }
   out = util_dynarray_grow(&ctx->records, float, padded * record_floats);
   if (!out)
      m9_warn_once(ctx, M9_WARN_MEMORY, "out of memory for source records");
   *first = records;
   return out;
}

static void
m9_job_commit(struct m9_context *ctx, float *out, float *extra,
              unsigned vertices, uint32_t first, uint32_t index_first,
              uint32_t index_count, enum m9_topology topology,
              uint32_t cull, bool lines, bool flat)
{
   unsigned padded = m9_draw_preprocess_vertices(vertices);
   unsigned extra_floats = 4 * (ctx->job_key.variant && !ctx->job_key.vs ?
                                ctx->job_key.variant->extra_count : 0);
   unsigned floats = ctx->record_floats;
   struct m9_draw_record *draw;

   /* Pad to the next four-record boundary with the last vertex. */
   for (unsigned i = vertices; i < padded; i++) {
      memcpy(out + i * floats, out + (vertices - 1) * floats,
             floats * sizeof(float));
      if (extra)
         memcpy(extra + i * extra_floats, extra + (vertices - 1) * extra_floats,
                extra_floats * sizeof(float));
   }
   m9_prof_draws++;
   draw = util_dynarray_grow(&ctx->draws, struct m9_draw_record, 1);
   if (!draw) {
      /* Shrinking never reallocates; drop the orphaned records. */
      ctx->records.size = (size_t)first * floats * sizeof(float);
      ctx->extra.size = (size_t)first * extra_floats * sizeof(float);
      ctx->indices.size = (size_t)index_first * sizeof(uint16_t);
      m9_warn_once(ctx, M9_WARN_MEMORY, "out of memory for draw records");
      return;
   }
   /* M9X_DEBUG_TEXTURES: the screen extent a draw covers and the texture it
    * samples, so a region of the image can be traced back to its texture. */
   if (m9_debug_textures && !ctx->job_key.vs) {
      float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;

      /* Records hold window coordinates premultiplied by 1/w. */
      for (unsigned i = 0; i < vertices; i++) {
         const float *r = out + i * M9_RECORD_FLOATS;
         float iw = r[3] != 0.0f ? 1.0f / r[3] : 0.0f;
         float x = r[0] * iw, y = ctx->params.height - r[1] * iw;

         x0 = MIN2(x0, x); x1 = MAX2(x1, x);
         y0 = MIN2(y0, y); y1 = MAX2(y1, y);
      }
      fprintf(stderr, "m9x: draw %.0f,%.0f-%.0f,%.0f tex %u/%u units %u\n",
              x0, y0, x1, y1, ctx->job_key.unit_count ?
              ctx->job_key.units[0].handle : 0,
              ctx->job_key.unit_count > 1 ? ctx->job_key.units[1].handle : 0,
              ctx->job_key.unit_count);
   }
   *draw = (struct m9_draw_record) {
      .start = first,
      .count = vertices,
      .index_first = index_first,
      .index_count = index_count,
      .topology = topology,
      .cull = cull,
      .lines = lines,
      .flat = flat,
      /* A draw restates the units of a single-pass shader; a split one
       * binds one pool for the whole job, which the job carries. */
      .unit_count = MIN2(ctx->job_key.unit_count, M9_TEXTURE_MAX_UNITS),
   };
   memcpy(draw->units, ctx->job_key.units,
          draw->unit_count * sizeof(draw->units[0]));
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
      return m9_skip_reason(ctx, "the scissor rectangle is empty; "
                            "draw skipped");
   m9_scissor_words(minx, miny, maxx, maxy, &key->clip_min, &key->clip_max);
   return true;
}

/* One coefficient of a coordinate transform against constant buffer 0. */
static float
m9_coord_term(const struct m9_context *ctx, const struct m9_fs_coord_term *t)
{
   const struct pipe_constant_buffer *cb = &ctx->fs_constants;
   const uint8_t *data = cb->user_buffer ? cb->user_buffer :
                         cb->buffer ? m9_res(cb->buffer)->map : NULL;
   float v = 0.0f;

   if (t->offset == M9_FS_UNIFORM_ZERO || !data ||
       (size_t)t->offset + sizeof(float) > cb->buffer_size)
      return t->literal;
   memcpy(&v, data + cb->buffer_offset + t->offset, sizeof(v));
   if (t->reciprocal) {
      if (v == 0.0f)
         return t->literal;
      v = 1.0f / v;
   }
   return t->literal + v * t->factor;
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
   for (unsigned pi = 0; pi < fs->passes.count; pi++)
   util_dynarray_foreach(&fs->passes.pass[pi].consts, struct m9_fs_const, k) {
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
               return m9_skip_reason(ctx, "fragment constants outside the "
                                     "bound constant buffer; draw skipped");
            }
            memcpy(&v, data + k->offset[lane], sizeof(v));
            break;
         case M9_FS_CONST_FRAG_Y:
            v = lane ? (float)ctx->params.height - 1.0f : 0.0f;
            break;
         case M9_FS_CONST_ALPHA_REF:
            if ((size_t)k->offset[0] + sizeof(float) > size) {
               m9_warn_once(ctx, M9_WARN_FS, "alpha reference outside the "
                            "bound constant buffer; draw skipped");
               return m9_skip_reason(ctx, "alpha reference outside the "
                                     "bound constant buffer; draw skipped");
            }
            memcpy(&v, data + k->offset[0], sizeof(v));
            v *= 256.0f;
            break;
         }
         util_dynarray_append_typed(out, float, v);
      }
   }
   return true;
}

/* Unit words of a sampler view, or the reason it cannot be sampled. */
static struct m9_context *m9_debug_ctx;

static const char *
m9_texture_bind_view(const struct pipe_sampler_view *view,
                     struct m9_texture_bind *bind)
{
   struct m9_resource *res;
   unsigned last;

   static unsigned res_debug_once;

   if (!view || !view->texture)
      return "no texture on a sampled unit; draw skipped";
   if (!res_debug_once++ && debug_get_bool_option("M9X_DEBUG", false))
      fprintf(stderr, "m9x: sampling %s %ux%u kind %u tiled %d levels %u\n",
              util_format_short_name(view->texture->format),
              m9_res(view->texture)->width, m9_res(view->texture)->height,
              m9_res(view->texture)->kind, m9_res(view->texture)->tiled,
              m9_res(view->texture)->texture.levels);
   res = m9_res(view->texture);
   if (view->target != PIPE_TEXTURE_2D && view->target != PIPE_TEXTURE_RECT &&
       view->target != PIPE_TEXTURE_CUBE && view->target != PIPE_TEXTURE_3D)
      return "only plane, cube and volume sampling is implemented; "
             "draw skipped";
   /* View swizzles are lowered into the fragment variant (m9_ctx_variant).
    * A depth view names the sampleable part of a depth image, so compare
    * the descriptor code rather than the pipe format. */
   if (view->format != res->base.format) {
      struct m9_format_info a, b;

      if (!m9_format_info(view->format, &a) ||
          !m9_format_info(res->base.format, &b) || a.code != b.code)
         return "sampler view format differs from the texture; draw skipped";
   }
   /* The vendor re-lays the chain from the base level; not implemented. */
   if (view->u.tex.first_level)
      return "a base level above 0 needs a re-based texture copy; "
             "draw skipped";
   last = MIN2(view->u.tex.last_level, res->base.last_level);
   /* The tail page packs every level of 16 texels or less into one captured
    * placement, which only matches the chain it was captured from: on other
    * shapes those levels sample nothing.  Stop at the last level with a
    * placement of its own until the tail's own contract is measured. */
   if (res->texture.levels)
      while (last && res->texture.level[last].in_tail)
         last--;
   if (res->texture.levels) {
      if (res->texture_unplaced & ((2u << last) - 1))
         return "texture levels beyond the captured 32x16 tail need the "
                "kernel texture window; draw skipped";
      /* A level nothing ever wrote samples whatever the object was
       * allocated with, which is why such a texture comes out black. */
      if (m9_debug_textures && !res->bind_logged) {
         ((struct m9_resource *)res)->bind_logged = true;
         fprintf(stderr, "m9x: bind texture %u %ux%u %s levels %u written "
                 "%#x kind %u\n", res->handle, res->width, res->height,
                 util_format_short_name(res->base.format),
                 res->base.last_level + 1, res->levels_written, res->kind);
      }
      /* The storage kind must be the one the level was written through:
       * the small kind up to 64 in both dimensions, the ordinary tiled kind
       * above it.  Reading 128x128 and larger textures as the small kind
       * returned a single constant texel. */
      *bind = (struct m9_texture_bind) {
         .code = res->texture_code,
         .kind = m9_texture_bind_kind(&res->texture, res->width, res->height),
         .width = res->width, .height = res->height,
         .depth = res->texture.kind == M9_TEXTURE_KIND_3D ? res->depth : 1,
         .last_level = res->base.last_level, .view_last_level = last,
      };
      return NULL;
   }
   /* A depth surface samples through the descriptor the raster pass binds
    * it with; shadowmap_d24 binds the very same word in both roles
    * (CAPTURED, captures/m9x-vendor-openbits-20260917). */
   if (res->kind == M9_RESOURCE_LINEAR_DEPTH && res->format) {
      bool small = res->width <= 64 && res->height <= 64;

      *bind = (struct m9_texture_bind) {
         .code = (uint8_t)(res->format << 1),
         .kind = small ? M9_TEXTURE_KIND_2D : M9_TEXTURE_KIND_RENDER,
         .width = small ? res->width : (res->width + 31) & ~31u,
         .height = res->height, .depth = 1,
      };
      return NULL;
   }
   /* Captured 79=c8210080 FBO textures: ordinary tiled render targets.  The
    * descriptor carries the 32-aligned row span; other widths sample through
    * scaled coordinates (m9_job_textures). */
   if (res->tiled && res->kind == M9_RESOURCE_LINEAR_COLOR &&
       (res->base.format == PIPE_FORMAT_B8G8R8A8_UNORM ||
        res->base.format == PIPE_FORMAT_B8G8R8X8_UNORM)) {
      /* The small family is addressed by the 2-D kind, like a small texture:
       * reading it as the ordinary tiled kind returns only the first 16x16
       * texels, which is 1/16 of a 64x64 surface and 1/4 of a 32x32 one. */
      bool small = res->width <= 64 && res->height <= 64;

      *bind = (struct m9_texture_bind) {
         .code = 0xcc,
         .kind = small ? M9_TEXTURE_KIND_2D : M9_TEXTURE_KIND_RENDER,
         .width = small ? res->width : (res->width + 31) & ~31u,
         .height = res->height, .depth = 1,
      };
      return NULL;
   }
   return "texture is not stored in a captured sampler layout; draw skipped";
}

static bool
m9_job_textures(struct m9_context *ctx, const struct m9_fs_variant *fs,
                struct m9_job_key *key)
{
   /* One binding per texture slot of every raster pass, in the order the
    * variant pooled them; a slot a record feeds binds nothing. */
   for (unsigned u = 0; u < fs->slot_count; u++) {
      const struct m9_fs_program *p = &fs->passes.pass[fs->slot_pass[u]];
      unsigned k = fs->slot_index[u];
      unsigned slot = p->textures[k];
      struct pipe_sampler_view *view = slot < PIPE_MAX_SAMPLERS ?
                                       ctx->fs_views[slot] : NULL;
      const struct pipe_sampler_state *sampler = slot < PIPE_MAX_SAMPLERS ?
                                                 ctx->fs_samplers[slot] : NULL;
      struct m9_texture_bind bind;
      const char *reason;

      /* An incomplete unit samples the opaque black placeholder. */
      if (!view || !view->texture) {
         if (m9_debug_textures)
            fprintf(stderr, "m9x: unit %u slot %u has no view: placeholder "
                    "(handle %u), textures %u\n", u, slot,
                    ctx->placeholder_texture.handle, p->texture_count);
         m9_texture_unit_placeholder(&key->units[u],
                                     ctx->placeholder_texture.handle);
         key->unit_scale[u][0] = key->unit_scale[u][1] = 1.0f;
         for (unsigned i = 0; i < 3; i++) {
            key->coord_scale[u][i] = 1.0f;
            key->coord_bias[u][i] = 0.0f;
         }
         continue;
      }
      reason = m9_texture_bind_view(view, &bind);

      if (reason && view && view->texture && ctx->debug)
         fprintf(stderr, "m9x: unit %u view %s on %s swizzle %u%u%u%u: %s\n",
                 u, util_format_short_name(view->format),
                 util_format_short_name(view->texture->format),
                 view->swizzle_r, view->swizzle_g, view->swizzle_b,
                 view->swizzle_a, reason);
      if (!reason && !sampler)
         reason = "sampled unit has no sampler state; draw skipped";
      /* The comparison needs no compensation: a depth surface the engine
       * rendered compares against the plain window depth, exactly, for
       * every one of the eight functions (MEASURED, m9-mesa-glsl
       * shadow:FUNC).  The vendor ICD's complemented readback is not what
       * this pipeline stores. */
      if (!reason && m9_texture_unit_init(&key->units[u],
                                          m9_res(view->texture)->handle,
                                          &bind, sampler))
         reason = "sampler wrap mode is not implemented; draw skipped";
      if (reason) {
         m9_warn_once(ctx, M9_WARN_TEXTURE, reason);
         return m9_skip_reason(ctx, reason);
      }
      key->unit_res[u] = view->texture;
      for (unsigned i = 0; i < 3; i++) {
         key->coord_scale[u][i] =
            m9_coord_term(ctx, &p->texcoord_affine[k].scale[i]);
         key->coord_bias[u][i] =
            m9_coord_term(ctx, &p->texcoord_affine[k].bias[i]);
      }
      key->unit_scale[u][0] = (float)m9_res(view->texture)->width / bind.width;
      key->unit_scale[u][1] = (float)m9_res(view->texture)->height / bind.height;
      /* Every wrap mode agrees inside the content; beyond it the hardware
       * wraps or clamps at the aligned span, including its padding. */
      if (key->unit_scale[u][0] != 1.0f &&
          sampler->wrap_s != PIPE_TEX_WRAP_CLAMP_TO_EDGE &&
          sampler->wrap_s != PIPE_TEX_WRAP_CLAMP_TO_BORDER &&
          sampler->wrap_s != PIPE_TEX_WRAP_CLAMP)
         m9_warn_once(ctx, M9_WARN_WRAP, "render targets of unaligned width "
                      "wrap at their 32-aligned span; samples outside the "
                      "content read padding");
      for (unsigned j = 0; !reason && j < u; j++)
         if (fs->slot_pass[j] == fs->slot_pass[u] &&
             fs->stream[fs->slot_pass[j]].texture_coord[fs->slot_index[j]] ==
             fs->stream[fs->slot_pass[u]].texture_coord[k] &&
             memcmp(key->unit_scale[j], key->unit_scale[u],
                    sizeof(key->unit_scale[u])))
            reason = "textures of different spans share a coordinate; "
                     "draw skipped";
      if (reason) {
         m9_warn_once(ctx, M9_WARN_TEXTURE, reason);
         return m9_skip_reason(ctx, reason);
      }
   }
   key->unit_count = fs->slot_count;
   if (!fs->slot_count) {
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
      { PIPE_BLENDFACTOR_DST_COLOR, M9_FS_DST_COLOR },
      { PIPE_BLENDFACTOR_INV_DST_COLOR, M9_FS_INV_DST_COLOR },
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
   case PIPE_BLENDFACTOR_DST_COLOR: return PIPE_BLENDFACTOR_DST_ALPHA;
   case PIPE_BLENDFACTOR_INV_DST_COLOR: return PIPE_BLENDFACTOR_INV_DST_ALPHA;
   default: return f;
   }
}

/* ZERO * source + ONE * destination leaves the channel as it was. */
static bool
m9_blend_keeps_dst(enum pipe_blendfactor src, enum pipe_blendfactor dst)
{
   return src == PIPE_BLENDFACTOR_ZERO && dst == PIPE_BLENDFACTOR_ONE;
}

/* Blend state as lowered factors and the channels the draw writes, or the
 * reason draws are skipped.  One program factor pair drives all four
 * channels, so a differing alpha lane is only supported where it keeps the
 * destination: that lane is masked off in the raster state instead. */
static const char *
m9_blend_key(const struct pipe_blend_state *blend, struct m9_fs_blend *out,
             unsigned *colormask)
{
   const struct pipe_rt_blend_state *rt = blend ? &blend->rt[0] : NULL;
   enum pipe_blendfactor src, dst;

   if (!rt || !rt->blend_enable)
      return NULL;
   if (rt->rgb_func != PIPE_BLEND_ADD || rt->alpha_func != PIPE_BLEND_ADD)
      return "blend equations other than ADD are not implemented; "
             "draw skipped";
   src = rt->rgb_src_factor;
   dst = rt->rgb_dst_factor;
   if (m9_alpha_equivalent(src) != m9_alpha_equivalent(rt->alpha_src_factor) ||
       m9_alpha_equivalent(dst) != m9_alpha_equivalent(rt->alpha_dst_factor)) {
      if (m9_blend_keeps_dst(rt->alpha_src_factor, rt->alpha_dst_factor)) {
         if (colormask)
            *colormask &= ~PIPE_MASK_A;
      } else if (m9_blend_keeps_dst(src, dst)) {
         if (colormask)
            *colormask &= ~PIPE_MASK_RGB;
         src = m9_alpha_equivalent(rt->alpha_src_factor);
         dst = m9_alpha_equivalent(rt->alpha_dst_factor);
      } else {
         return "separate blend factors are only implemented where one lane "
                "keeps the destination; draw skipped";
      }
   }
   if (!m9_blend_factor(src, &out->src) || !m9_blend_factor(dst, &out->dst))
      return "constant blend factors are not implemented; draw skipped";
   return NULL;
}

static bool
m9_fs_variant_upload(struct m9_context *ctx, struct m9_fs_variant *v,
                     const char **reason)
{
   const unsigned vs_slots = ARRAY_SIZE(m9_passthrough_c2_program) / 2 - 4;
   unsigned slots = 0;
   uint64_t bytes;

   /* One image holds the fragment program of every raster pass back to
    * back, each entered at its own c3, then the vertex program. */
   for (unsigned i = 0; i < v->passes.count; i++) {
      v->entry[i] = slots;
      slots += m9_fs_slot_count(&v->passes.pass[i]);
   }
   bytes = align64(8ull * (slots + vs_slots), 0x1000);
   if (bytes > M9_PROGRAM_MAX_BYTES) {
      *reason = "program exceeds the largest c2 image the driver allocates";
      return false;
   }
   if (m9_bo_create(ctx->screen, bytes, PARHELIA_M9_BO_VRAM,
                    M9_RESOURCE_SYSTEM_STATE, &v->c2)) {
      *reason = "cannot allocate the c2 image";
      return false;
   }
   memset(v->c2.map, 0, v->c2.size);
   for (unsigned i = 0; i < v->passes.count; i++)
      memcpy((uint8_t *)v->c2.map + 8 * v->entry[i],
             v->passes.pass[i].slots.data,
             8 * m9_fs_slot_count(&v->passes.pass[i]));
   memcpy((uint8_t *)v->c2.map + 8 * slots, m9_passthrough_c2_program + 8,
          8 * vs_slots);
   v->vertex_entry = slots;
   return true;
}

/* CPU view order, pass by pass: non-colour varyings, then the pass's own
 * distinct coordinate streams.  Passes share no stream: each interpolates
 * what it reads, and a value handed on between them travels through the
 * record instead. */
static bool
m9_fs_streams(struct m9_fs_variant *v)
{
   for (unsigned i = 0; i < v->passes.count; i++) {
      const struct m9_fs_program *p = &v->passes.pass[i];
      struct m9_fs_pass_streams *st = &v->stream[i];
      /* Two vec2 coordinate sets are packed into one varying slot, so a
       * stream is a slot *half*: keying it by location alone gave both
       * textures the same coordinates, and the second unit sampled where
       * the first did. */
      uint8_t c_owner[M9_FS_MAX_TEXTURES] = { 0 };

      for (unsigned k = 0; k < p->varying_count; k++) {
         st->varying_extra[k] = -1;
         if (p->varyings[k].location == VARYING_SLOT_COL0)
            continue;
         if (v->extra_count >= M9_FS_MAX_EXTRA)
            return false;
         st->varying_extra[k] = v->extra_count;
         v->extra_location[v->extra_count++] = p->varyings[k].location;
      }
      for (unsigned t = 0; t < p->texture_count; t++) {
         unsigned half, c = 0;

         /* A slot an earlier pass feeds through the record interpolates
          * nothing and binds no texture of its own. */
         if (p->texture_source[t] != M9_FS_ROUTE_NONE) {
            st->texture_coord[t] = M9_RASTER_COORD_NONE;
            st->unit[t] = M9_PASS_UNIT_NONE;
            continue;
         }
         /* A unit the record steers samples its own texture and declares no
          * coordinate of its own. */
         if (p->texcoord_source[t] != M9_FS_ROUTE_NONE) {
            st->texture_coord[t] = M9_RASTER_COORD_NONE;
            if (v->slot_count >= M9_DRAW_MAX_UNITS)
               return false;
            st->unit[t] = v->slot_count;
            v->slot_pass[v->slot_count] = i;
            v->slot_index[v->slot_count++] = t;
            continue;
         }
         half = p->texcoord_components[t] / 2;
         /* A unit that transforms its coordinate needs a stream of its own:
          * sharing one would sample both units at the same place. */
         while (c < st->coord_count &&
                (st->coord_location[c] != p->texcoords[t] ||
                 st->coord_component[c] != half ||
                 !m9_fs_coord_affine_same(&p->texcoord_affine[c_owner[c]],
                                         &p->texcoord_affine[t])))
            c++;
         if (c == st->coord_count) {
            c_owner[c] = t;
            st->coord_extra[c] = -1;
            st->coord_component[c] = half;
            st->coord_location[st->coord_count++] = p->texcoords[t];
            if (p->texcoords[t] != VARYING_SLOT_COL0) {
               if (v->extra_count >= M9_FS_MAX_EXTRA)
                  return false;
               st->coord_extra[c] = v->extra_count;
               v->extra_location[v->extra_count++] = p->texcoords[t];
            }
         }
         st->texture_coord[t] = c;
         if (v->slot_count >= M9_DRAW_MAX_UNITS)
            return false;
         st->unit[t] = v->slot_count;
         v->slot_pass[v->slot_count] = i;
         v->slot_index[v->slot_count++] = t;
      }
   }
   return true;
}

/* A plain fetch of a depth surface returns the window depth in the texel's
 * second scalar, not the first; a comparing fetch returns its result in the
 * first (MEASURED, m9-mesa-glsl depthchan).  Splat the second one over the
 * result, which is what every GL_DEPTH_TEXTURE_MODE then selects from. */
static bool
m9_depth_channel_instr(nir_builder *b, nir_instr *instr, void *data)
{
   uint32_t mask = *(const uint32_t *)data;
   nir_tex_instr *tex;
   nir_def *depth;

   if (instr->type != nir_instr_type_tex)
      return false;
   tex = nir_instr_as_tex(instr);
   if (tex->is_shadow || tex->def.num_components < 2 ||
       tex->sampler_index >= 32 || !(mask & BITFIELD_BIT(tex->sampler_index)))
      return false;
   b->cursor = nir_after_instr(instr);
   depth = nir_channel(b, &tex->def, 1);
   nir_def_rewrite_uses_after(&tex->def,
                              nir_vec4(b, depth, depth, depth, depth));
   return true;
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
          !memcmp((*it)->inline_values, want->inline_values, inline_bytes) &&
          (*it)->depth_texture == want->depth_texture &&
          !memcmp((*it)->swizzle, want->swizzle, sizeof(want->swizzle))) {
         *reason = (*it)->reason;
         return (*it)->ok ? *it : NULL;
      }
   }
   /* A shader specialised on a uniform can be asked for one variant per
    * value, so the cache is capped and a further one is refused. */
   if (util_dynarray_num_elements(&fs->variants, struct m9_fs_variant *) >=
       M9_FS_MAX_VARIANTS) {
      *reason = "more shader variants than the cache holds";
      return NULL;
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
   memcpy(v->swizzle, want->swizzle, sizeof(v->swizzle));
   v->depth_texture = want->depth_texture;
   if (inline_bytes)
      m9_fs_inline_uniforms(clone, v->inline_values);
   {
      nir_lower_tex_options tex = { 0 };

      for (unsigned s = 0; s < PIPE_MAX_SAMPLERS; s++) {
         if (v->swizzle[s][0] == PIPE_SWIZZLE_X &&
             v->swizzle[s][1] == PIPE_SWIZZLE_Y &&
             v->swizzle[s][2] == PIPE_SWIZZLE_Z &&
             v->swizzle[s][3] == PIPE_SWIZZLE_W)
            continue;
         /* PIPE_SWIZZLE_ZERO/ONE are nir_lower_tex's 4 and 5 */
         tex.swizzle_result |= BITFIELD_BIT(s);
         memcpy(tex.swizzles[s], v->swizzle[s], 4);
      }
      if (tex.swizzle_result)
         NIR_PASS(_, clone, nir_lower_tex, &tex);
   }
   /* After the view swizzle: nir_lower_tex rewrites every use of the fetch,
    * this one included, so a channel pick before it reads the swizzled
    * value instead of the texel. */
   if (v->depth_texture)
      NIR_PASS(_, clone, nir_shader_instructions_pass, m9_depth_channel_instr,
               nir_metadata_control_flow, &v->depth_texture);
   if (ctx->debug)
      nir_print_shader(clone, stderr);
   v->ok = m9_fs_compile_passes(clone, &v->blend, &v->passes, &v->reason);
   if (v->ok && v->passes.count > 1 && !m9_split_enable) {
      m9_fs_passes_fini(&v->passes);
      v->reason = "the multi-pass schedule is not enabled (M9X_SPLIT)";
      v->ok = false;
   }
   if (!v->ok && ctx->debug)
      fprintf(stderr, "m9x: variant refused: %s\n", v->reason);
   ralloc_free(clone);
   if (v->ok) {
      /* Every pass but the last hands a four-dword slot to the ones after
       * it, so the record's stride is four dwords a producer.  Narrowing a
       * one-pair producer to the two dwords the captures show stops the
       * engine (MEASURED), so the width stays four. */
      v->record_dwords = 4 * (v->passes.count - 1);
      for (unsigned i = 0; i + 1 < v->passes.count; i++)
         v->record_offset[i] = 4 * i;
      if (!m9_fs_streams(v)) {
         v->reason = "more CPU varying streams or texture slots than a job "
                     "binds";
         v->ok = false;
      } else if (v->extra_count > M9_FS_MAX_EXTRA) {
         v->reason = "more CPU varying streams than transient views";
         v->ok = false;
      }
      /* A varying on view 0 costs a raster stream without costing a CPU
       * view, so the check above does not cover a pass's own limit;
       * without this the job builds and its draws are dropped instead of
       * the shader falling back. */
      for (unsigned i = 0; v->ok && i < v->passes.count; i++)
         if (!m9_raster_fs_layout_ok(v->passes.pass[i].varying_count,
                                     v->stream[i].coord_count,
                                     v->passes.pass[i].texture_count)) {
            v->reason = "more raster streams than the pass has";
            v->ok = false;
         }
      if (v->ok) {
         for (unsigned i = 0; i < v->passes.count; i++) {
            const struct m9_fs_program *p = &v->passes.pass[i];
            unsigned units = MAX2(m9_fs_sampler_count(p), 1u);
            unsigned routes = p->texture_count - m9_fs_sampler_count(p);

            v->relocs_base += m9_draw_job_relocs(0, units) + routes;
            v->relocs_draw += units + 2;
            v->dwords_base += m9_draw_pass_dwords(1, units, routes);
            /* Plus the engine transition around each restated draw, the
             * same fourteen dwords m9_draw_pass_dwords budgets. */
            v->dwords_draw += M9_JOB_DRAW_DWORDS + 14;
         }
         if (m9_variant_relocs(v, 1) > M9_MAX_RELOCS) {
            v->reason = "the raster passes of this shader need more "
                        "relocations than one submission carries";
            v->ok = false;
         } else if (m9_variant_dwords(v, 1) > M9_MAX_COMMAND_DWORDS) {
            v->reason = "the raster passes of this shader need more "
                        "command words than one submission carries";
            v->ok = false;
         }
      }
      if (v->ok)
         v->ok = m9_fs_variant_upload(ctx, v, &v->reason);
      if (!v->ok)
         m9_fs_passes_fini(&v->passes);
   }
   slot = util_dynarray_grow(&fs->variants, struct m9_fs_variant *, 1);
   if (!slot) {
      if (v->ok) {
         m9_bo_destroy(ctx->screen, &v->c2);
         m9_fs_passes_fini(&v->passes);
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
m9_ctx_variant(struct m9_context *ctx, const char **reason,
               unsigned *colormask)
{
   struct m9_fs_variant want = { .blend = { M9_FS_ONE, M9_FS_ZERO } };

   *reason = m9_blend_key(ctx->blend, &want.blend, colormask);
   if (*reason)
      return NULL;
   memcpy(want.inline_values, ctx->fs_inline, sizeof(want.inline_values));
   for (unsigned s = 0; s < PIPE_MAX_SAMPLERS; s++) {
      const struct pipe_sampler_view *view = ctx->fs_views[s];

      if (view && BITSET_TEST(ctx->fs->nir->info.textures_used, s)) {
         if (view->texture &&
             util_format_is_depth_or_stencil(view->texture->format))
            want.depth_texture |= BITFIELD_BIT(s);
         want.swizzle[s][0] = view->swizzle_r;
         want.swizzle[s][1] = view->swizzle_g;
         want.swizzle[s][2] = view->swizzle_b;
         want.swizzle[s][3] = view->swizzle_a;
      } else {
         for (unsigned c = 0; c < 4; c++)
            want.swizzle[s][c] = PIPE_SWIZZLE_X + c;
      }
   }
   return m9_fs_variant(ctx, ctx->fs, &want, reason);
}

static bool
m9_job_key_current(struct m9_context *ctx, struct m9_job_key *key)
{
   const struct pipe_depth_stencil_alpha_state *dsa = ctx->dsa;
   const struct pipe_blend_state *blend = ctx->blend;
   const struct pipe_rasterizer_state *rast = ctx->rasterizer;
   struct m9_resource *zs;
   unsigned colormask;
   bool test, write;

   if (ctx->lost)
      return m9_skip_reason(ctx, "the context is lost; draw skipped");
   if (!ctx->params_valid)
      return m9_skip_reason(ctx, "no valid raster parameters; draw skipped");
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
   key->depth = zs ? zs : ctx->target_depth;
   key->aux = zs ? zs->hz : ctx->target_aux;
   key->depth_control = m9_depth_control(test, write,
                                         dsa ? dsa->depth_func : PIPE_FUNC_LESS);
   key->depth_ef = m9_depth_ef(test, write);
   colormask = blend ? blend->rt[0].colormask : PIPE_MASK_RGBA;
   key->color_mask = m9_color_mask(colormask);
   if (ctx->fs && ctx->fs->native && !ctx->fs->passthrough) {
      const char *reason;

      key->fs = ctx->fs;
      key->variant = m9_ctx_variant(ctx, &reason, &colormask);
      if (!key->variant) {
         m9_warn_once(ctx, M9_WARN_BLEND, reason);
         return m9_skip_reason(ctx, reason);
      }
      key->color_mask = m9_color_mask(colormask);
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
      v = m9_ctx_variant(r->ctx, &reason, NULL);
   if (v) {
      unsigned offset = r->has_color ? 8 : 4;

      for (unsigned j = 0; j < v->extra_count; j++) {
         unsigned location = v->extra_location[j];
         unsigned name, index;
         int output;

         /* The draw module scans shader outputs with need_texcoord set. */
         tgsi_get_gl_varying_semantic(location, true, &name, &index);
         output = draw_find_shader_output(r->ctx->draw, name, index);
         if (r->ctx->debug)
            fprintf(stderr, "m9x: CPU varying %u location %u semantic %u/%u "
                    "draw output %d\n", j, location, name, index, output);
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

   /* The captured ortho program flips window y and maps z = 1 - 2 z_win.
    * The draw module already divided by w and left 1/w_clip in position w,
    * so the window coordinates are multiplied back out: the program's own
    * divide restores them and the setup engine keeps 1/w for
    * perspective-correct varyings. */
   const float w = v[3] > 0.0f ? 1.0f / v[3] : 1.0f;

   out[0] = v[0] * w;
   out[1] = ((float)r->ctx->params.height - v[1]) * w;
   out[2] = (1.0f - 2.0f * v[2]) * w;
   out[3] = w;
   for (unsigned c = 0; c < 4; c++)
      out[4 + c] = r->has_color ? CLAMP(v[4 + c], 0.0f, 1.0f) : 1.0f;
}

/* Extra slots whose coordinate m9_scale_coords divides by a projector. */
static uint32_t
m9_proj_slots(const struct m9_job_key *key)
{
   const struct m9_fs_variant *v = key->variant;
   uint32_t mask = 0;

   for (unsigned u = 0; v && u < v->slot_count; u++) {
      const struct m9_fs_program *p = &v->passes.pass[v->slot_pass[u]];
      const struct m9_fs_pass_streams *st = &v->stream[v->slot_pass[u]];
      unsigned k = v->slot_index[u];
      int j = st->coord_extra[st->texture_coord[k]];

      if (j >= 0 && p->texcoord_projective[k])
         mask |= BITFIELD_BIT(j);
   }
   return mask;
}

/* Vertices one primitive of this topology owns alone, or zero for a strip,
 * fan or polygon: those share vertices, so a value that is constant over
 * every primitive of the run is constant over the whole run. */
static unsigned
m9_topology_group(enum m9_topology topology)
{
   switch (topology) {
   case M9_TOPOLOGY_LINES: return 2;
   case M9_TOPOLOGY_TRIANGLES: return 3;
   case M9_TOPOLOGY_QUADS: return 4;
   default: return 0;
   }
}

/* Projective coordinates are divided per vertex: exact while q is constant
 * over a primitive, which m9_proj_constant checks before the job is built.
 * Render-target coordinates address the aligned span. */
static void
m9_scale_coords(const struct m9_job_key *key, float *extra)
{
   const struct m9_fs_variant *v = key->variant;
   uint32_t divided = 0, folded = 0, scaled = 0;

   uint32_t packed = 0;

   for (unsigned u = 0; u < v->slot_count; u++) {
      const struct m9_fs_program *p = &v->passes.pass[v->slot_pass[u]];
      const struct m9_fs_pass_streams *st = &v->stream[v->slot_pass[u]];
      unsigned k = v->slot_index[u];
      int j = st->coord_extra[st->texture_coord[k]];
      unsigned c = p->texcoord_components[k];

      if (j < 0)
         continue;
      /* A stream carries one pair; anything but the low one is moved down,
       * since the program reads its coordinate attribute as xy. */
      if (c && !(packed & BITFIELD_BIT(j))) {
         extra[4 * j] = extra[4 * j + c];
         extra[4 * j + 1] = extra[4 * j + c + 1];
         packed |= BITFIELD_BIT(j);
      }
      if (p->texcoord_affine[k].present && !(folded & BITFIELD_BIT(j))) {
         unsigned n = p->texcoord_shadow[k] || p->texcoord_volume[k] ? 3 : 2;

         for (unsigned i = 0; i < n; i++)
            extra[4 * j + i] = extra[4 * j + i] * key->coord_scale[u][i] +
                               key->coord_bias[u][i];
         folded |= BITFIELD_BIT(j);
      }
      if (p->texcoord_projective[k] && !(divided & BITFIELD_BIT(j))) {
         float q = extra[4 * j + 3];

         if (q != 0.0f && q != 1.0f) {
            extra[4 * j] /= q;
            extra[4 * j + 1] /= q;
            if (p->texcoord_shadow[k] || p->texcoord_volume[k])
               extra[4 * j + 2] /= q;
            extra[4 * j + 3] = 1.0f;
         }
         divided |= BITFIELD_BIT(j);
      }
      /* The stream is shared, so its scale is applied once like every other
       * step here: two units on one coordinate scaled it twice. */
      if (!(scaled & BITFIELD_BIT(j))) {
         extra[4 * j] *= key->unit_scale[u][0];
         extra[4 * j + 1] *= key->unit_scale[u][1];
         scaled |= BITFIELD_BIT(j);
      }
   }
}

static inline uint32_t
m9_index_at(const void *indices, unsigned index_size, unsigned i)
{
   return index_size == 4 ? ((const uint32_t *)indices)[i] :
                            ((const uint16_t *)indices)[i];
}

/* The projector divide below is per vertex, which is exact only while the
 * projector is constant over a primitive: the engine interpolates s/q and
 * t/q, and a q that varies makes that differ from the S/Q the fetch needs.
 * MEASURED: a projector that quadruples across a quad puts 75373 of 76800
 * pixels at the wrong texel (fixture projtexz), so a draw that breaks the
 * precondition is refused instead of drawn.  The comparison carries a
 * relative tolerance because a projector a vertex shader recomputes per
 * vertex is only bitwise equal when the arithmetic is. */
#define M9_PROJ_TOLERANCE 1.0e-6f

static const char m9_proj_reason[] =
   "a projective texture coordinate whose projector varies over a primitive "
   "is divided per vertex, not per pixel; draw skipped";

static bool
m9_proj_constant(const struct m9_job_key *key, enum m9_topology topology,
                 const float *extra, unsigned extra_count,
                 const void *indices, unsigned index_size, unsigned offset,
                 uint32_t lo, unsigned count)
{
   uint32_t mask = m9_proj_slots(key);
   unsigned span = m9_topology_group(topology);

   if (!mask || !extra_count)
      return true;
   if (!span)
      span = count;
   for (unsigned base = 0; base + span <= count; base += span) {
      u_foreach_bit(j, mask) {
         unsigned i0 = indices ?
                       m9_index_at(indices, index_size, offset + base) - lo :
                       base;
         float q = extra[(size_t)i0 * 4 * extra_count + 4 * j + 3];
         float tol = M9_PROJ_TOLERANCE * MAX2(fabsf(q), 1.0f);

         for (unsigned k = 1; k < span; k++) {
            unsigned i = indices ?
                         m9_index_at(indices, index_size,
                                     offset + base + k) - lo :
                         base + k;

            if (fabsf(extra[(size_t)i * 4 * extra_count + 4 * j + 3] - q) >
                tol)
               return false;
         }
      }
   }
   return true;
}


/* Lowest and highest vertex a run of indices refers to. */
static void
m9_index_range(const void *indices, unsigned index_size, unsigned first,
               unsigned count, uint32_t *lo, uint32_t *hi)
{
   uint32_t a = UINT32_MAX, b = 0;

   for (unsigned i = 0; i < count; i++) {
      uint32_t v = m9_index_at(indices, index_size, first + i);

      a = MIN2(a, v);
      b = MAX2(b, v);
   }
   *lo = a;
   *hi = b;
}

/* An index list preprocesses the vertices it spans once and rasterises them
 * through a stored index run, instead of one record per index. */
static void
m9_render_append_indexed(struct m9_vbuf_render *r, const struct m9_job_key *key,
                         enum m9_topology topology, bool lines, uint32_t cull,
                         const void *indices, unsigned index_size,
                         unsigned count)
{
   struct m9_context *ctx = r->ctx;
   const unsigned max = m9_draw_max_source_vertices();
   /* A list splits only on a whole primitive. */
   unsigned grain = topology == M9_TOPOLOGY_TRIANGLES ? 3 :
                    topology == M9_TOPOLOGY_LINES ? 2 : count;

   for (unsigned done = 0; done < count;) {
      unsigned n = count - done, vertices;
      uint32_t lo, hi, first, index_first;
      float *out, *extra;
      uint16_t *words;

      m9_index_range(indices, index_size, done, n, &lo, &hi);
      /* Halve the run until the vertices it spans fit one job's records. */
      while (hi - lo + 1 > max) {
         n = n / 2 / grain * grain;
         if (!n)
            return;
         m9_index_range(indices, index_size, done, n, &lo, &hi);
      }
      vertices = hi - lo + 1;
      out = m9_job_reserve(ctx, key, &ctx->key_consts, NULL, M9_RECORD_FLOATS,
                           vertices, &first, &extra);
      if (!out || (extra && r->extra_count != key->variant->extra_count))
         return;
      /* An odd run is padded to a whole dword so every run stays aligned,
       * and M9_INDEX_TAIL halves beyond that repeat the last index. */
      index_first = util_dynarray_num_elements(&ctx->indices, uint16_t);
      words = util_dynarray_grow(&ctx->indices, uint16_t,
                                 n + (n & 1) + M9_INDEX_TAIL);
      if (!words) {
         ctx->records.size = (size_t)first * M9_RECORD_FLOATS * sizeof(float);
         if (extra)
            ctx->extra.size = (size_t)first * 4 * r->extra_count * sizeof(float);
         m9_warn_once(ctx, M9_WARN_MEMORY, "out of memory for raster indices");
         return;
      }
      for (unsigned v = 0; v < vertices; v++)
         m9_render_record(r, lo + v, out + v * M9_RECORD_FLOATS,
                          extra ? extra + v * 4 * r->extra_count : NULL);
      if (extra &&
          !m9_proj_constant(key, topology, extra, r->extra_count, indices,
                            index_size, done, lo, n)) {
         ctx->records.size = (size_t)first * M9_RECORD_FLOATS * sizeof(float);
         ctx->extra.size = (size_t)first * 4 * r->extra_count * sizeof(float);
         ctx->indices.size = (size_t)index_first * sizeof(uint16_t);
         m9_warn_once(ctx, M9_WARN_FS, m9_proj_reason);
         return;
      }
      for (unsigned v = 0; extra && v < vertices; v++)
         m9_scale_coords(key, extra + v * 4 * r->extra_count);
      for (unsigned i = 0; i < n; i++)
         words[i] = first +
                    m9_index_at(indices, index_size, done + i) - lo;
      for (unsigned i = n; i < n + (n & 1) + M9_INDEX_TAIL; i++)
         words[i] = words[n - 1];
      m9_prof_indexed += n;
      m9_prof_referenced += vertices;
      m9_job_commit(ctx, out, extra, vertices, first, index_first, n, topology,
                    cull, lines,
                    ctx->rasterizer && ctx->rasterizer->flatshade);
      done += n;
   }
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

   if (indices && m9_indexed_draws) {
      m9_render_append_indexed(r, key, topology, lines, cull, indices,
                               index_size, count);
      return;
   }
   /* Native connected primitives are bounded by max_vertex_buffer_bytes;
    * lists split on any multiple of six. */
   if (count > max && !list)
      return;
   for (unsigned done = 0; done < count;) {
      unsigned n = MIN2(count - done, max);
      uint32_t first;
      float *extra;
      float *out = m9_job_reserve(r->ctx, key, &r->ctx->key_consts, NULL,
                                  M9_RECORD_FLOATS, n, &first, &extra);

      if (!out)
         return;
      if (extra && r->extra_count != key->variant->extra_count)
         return;
      for (unsigned i = 0; i < n; i++) {
         unsigned index = !indices ? start + done + i :
                          m9_index_at(indices, index_size, done + i);

         m9_render_record(r, index, out + i * M9_RECORD_FLOATS,
                          extra ? extra + i * 4 * r->extra_count : NULL);
      }
      /* The records are already in primitive order here, indexed or not. */
      if (extra &&
          !m9_proj_constant(key, topology, extra, r->extra_count, NULL, 0, 0,
                            0, n)) {
         r->ctx->records.size =
            (size_t)first * M9_RECORD_FLOATS * sizeof(float);
         r->ctx->extra.size =
            (size_t)first * 4 * r->extra_count * sizeof(float);
         m9_warn_once(r->ctx, M9_WARN_FS, m9_proj_reason);
         return;
      }
      for (unsigned i = 0; extra && i < n; i++)
         m9_scale_coords(key, extra + i * 4 * r->extra_count);
      m9_job_commit(r->ctx, out, extra, n, first, 0, 0, topology, cull, lines,
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
   uint64_t t_emit = m9_prof_begin();
   uint32_t cull = 0;

   if (!count)
      return;
   if (!m9_job_key_current(ctx, &key)) {
      /* Every inner path records its own reason; this names the rest. */
      m9_skip_reason(ctx, "the job state is unsupported; draw skipped");
      ctx->skipped_draws++;
      return;
   }
   /* A killed client never destroys its context, so report as we go. */
   if (ctx->debug && ++ctx->submitted_draws % 200 == 0)
      fprintf(stderr, "m9x: %u draws submitted, %u skipped\n",
              ctx->submitted_draws, ctx->skipped_draws);
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
   m9_prof_end(M9_PROF_EMIT, t_emit);
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

/* Native vertex path: a covered vertex shader runs as the preprocessing
 * program over the raw attributes, so the CPU neither transforms nor clips.
 * The appended tail divides by w, applies the viewport in force and writes
 * the four streams the raster pass reads; the one varying it carries is
 * stream 0, which is why only a primary-colour varying and no CPU-written
 * texture coordinate can be served this way. */

/* Fragment slots of the captured pass-through image; the vertex program
 * starts behind them, which is why its preprocessing entry is 4. */
#define M9_PASSTHROUGH_FS_SLOTS 4u

/* Vertex buffers of one native draw, mapped as the draw module maps them. */
struct m9_native_vs {
   struct m9_vs_source_layout layout;
   const uint8_t *map[PIPE_MAX_ATTRIBS];
   unsigned size[PIPE_MAX_ATTRIBS];
   unsigned buffer_count;
   const void *indices;       /* NULL runs a numeric record range */
   unsigned index_size;
   int index_bias;
   uint32_t first_vertex, vertex_count;   /* the range the records cover */
   enum m9_topology topology;
   uint32_t cull;
   bool lines, flat;
};

/* Constant pairs the compiled program names, resolved against constant
 * buffer 0: two floats per user constant, in program order. */
static bool
m9_vs_resolve_consts(struct m9_context *ctx, const struct m9_vs_program *p,
                     struct util_dynarray *out)
{
   const struct pipe_constant_buffer *cb = &ctx->vs_constants[0];
   const uint8_t *data = cb->user_buffer ? cb->user_buffer :
                         cb->buffer ? m9_res(cb->buffer)->map : NULL;
   size_t size = data ? cb->buffer_size : 0;

   if (data)
      data += cb->buffer_offset;
   util_dynarray_clear(out);
   util_dynarray_foreach(&p->consts, struct m9_vs_const, k) {
      for (unsigned lane = 0; lane < 2; lane++) {
         float v = 0.0f;

         if (k->kind == M9_VS_CONST_UNIFORM &&
             k->offset[lane] != M9_VS_UNIFORM_ZERO) {
            if ((size_t)k->offset[lane] + sizeof(float) > size)
               return m9_skip_reason(ctx, "vertex constants outside the "
                                     "bound constant buffer; draw skipped");
            memcpy(&v, data + k->offset[lane], sizeof(v));
         } else if (k->kind == M9_VS_CONST_LITERAL) {
            v = k->value[lane];
         }
         util_dynarray_append_typed(out, float, v);
      }
   }
   return true;
}

/* The c2 image a native preprocessing job binds: the fragment slots the
 * raster pass enters, with the compiled vertex program behind them instead
 * of the captured one.  One image per fragment variant, rebuilt when a
 * different vertex program binds. */
static uint32_t
m9_native_vs_c2(struct m9_context *ctx, struct m9_fs_variant *v)
{
   const struct m9_vs_state *vs = ctx->vs;
   struct m9_resource *image = v ? &v->native_c2 : &ctx->vs_c2;
   uint64_t *serial = v ? &v->native_c2_serial : &ctx->vs_c2_serial;
   const uint32_t *fs_slots = v ? m9_fs_final(v)->slots.data :
                                  m9_passthrough_c2_program;
   unsigned fs_slots_count = v ? m9_fs_slot_count(m9_fs_final(v)) :
                                 M9_PASSTHROUGH_FS_SLOTS;
   unsigned entry = 0;
   uint64_t bytes = align64(8ull * (fs_slots_count +
                                    m9_vs_slot_count(&vs->program)), 0x1000);

   if (image->handle && *serial == vs->serial)
      return image->handle;
   if (bytes > M9_PROGRAM_MAX_BYTES)
      return 0;
   /* A different vertex program binding behind the same fragment slots can
    * need a longer image than the one already cached. */
   if (image->handle && image->size < bytes)
      m9_bo_destroy(ctx->screen, image);
   if (!image->handle &&
       m9_bo_create(ctx->screen, bytes, PARHELIA_M9_BO_VRAM,
                    M9_RESOURCE_SYSTEM_STATE, image))
      return 0;
   memset(image->map, 0, image->size);
   if (!m9_vs_c2_image(&vs->program, fs_slots, fs_slots_count, image->map,
                       image->size / sizeof(uint32_t), &entry) ||
       entry != fs_slots_count)
      return 0;
   *serial = vs->serial;
   return image->handle;
}

static uint32_t
m9_native_index_at(const void *indices, unsigned size, unsigned i)
{
   return size == 4 ? ((const uint32_t *)indices)[i] :
          size == 2 ? ((const uint16_t *)indices)[i] :
                      ((const uint8_t *)indices)[i];
}

static void
m9_native_index_range(const void *indices, unsigned size, unsigned first,
                      unsigned count, uint32_t *lo, uint32_t *hi)
{
   uint32_t a = UINT32_MAX, b = 0;

   for (unsigned i = 0; i < count; i++) {
      uint32_t v = m9_native_index_at(indices, size, first + i);

      a = MIN2(a, v);
      b = MAX2(b, v);
   }
   *lo = a;
   *hi = b;
}

/* What the fragment shader reads, before any variant of it is compiled: a
 * texture or a second varying needs CPU-written streams the preprocessing
 * pass cannot fill.  Resolving the source layout and the job key for a draw
 * that will fall back anyway is what this avoids. */
static bool
m9_native_vs_fs_cheap(const struct m9_context *ctx)
{
   const struct shader_info *info;

   if (!ctx->fs || !ctx->fs->nir)
      return false;
   info = &ctx->fs->nir->info;
   return BITSET_IS_EMPTY(info->textures_used) &&
          util_bitcount64(info->inputs_read &
                          ~(VARYING_BIT_POS | VARYING_BIT_FACE |
                            VARYING_BIT_PNTC)) <= 1;
}

/* Can this draw run the compiled vertex program?  NULL when it can, and a
 * static reason when the draw module has to keep it.  Everything the emit
 * path then needs is resolved here, so that path cannot fail on state. */
static const char *
m9_native_vs_resolve(struct m9_context *ctx, const struct pipe_draw_info *info,
                     const struct pipe_draw_start_count_bias *draw,
                     struct m9_native_vs *out)
{
   const struct pipe_rasterizer_state *rast = ctx->rasterizer;
   const struct m9_vs_program *p;
   const char *layout_reason = "the vertex elements do not feed the "
                               "declared attributes";
   unsigned in_pv, reduced;
   int64_t low;

   if (!ctx->vs || !ctx->vs->native)
      return ctx->vs && ctx->vs->reason ? ctx->vs->reason :
             "no native vertex program";
   if (!ctx->velems)
      return "no vertex elements are bound";
   p = &ctx->vs->program;
   if (p->attr_count > M9_VS_HW_ATTRIBUTES)
      return "more preprocessing attributes than the declaration can name";
   if (info->instance_count != 1 || info->start_instance)
      return "instanced draws are not preprocessed natively";
   if (info->primitive_restart)
      return "primitive restart is not preprocessed natively";
   in_pv = rast && rast->flatshade && rast->flatshade_first ? PV_FIRST :
                                                              PV_LAST;
   if (!m9_native_topology(info->mode, in_pv, &out->topology))
      return "the primitive needs the draw module's index translation";
   reduced = u_reduced_prim(info->mode);
   out->cull = 0;
   if (reduced == MESA_PRIM_TRIANGLES && rast) {
      if (rast->cull_face == PIPE_FACE_FRONT_AND_BACK)
         return "both faces are culled";
      out->cull = m9_cull_ef(rast->cull_face, rast->front_ccw);
   }
   out->lines = reduced == MESA_PRIM_LINES;
   out->flat = rast && rast->flatshade;
   if (rast && (rast->rasterizer_discard || rast->clip_plane_enable ||
                rast->clip_halfz || !rast->depth_clip_near ||
                !rast->depth_clip_far || rast->offset_tri ||
                rast->offset_line || rast->offset_point ||
                rast->poly_stipple_enable ||
                rast->fill_front != PIPE_POLYGON_MODE_FILL ||
                rast->fill_back != PIPE_POLYGON_MODE_FILL))
      return "a rasterizer state the hardware pass does not carry";
   if (out->lines && rast &&
       (rast->line_width > 1.0f || rast->line_smooth ||
        rast->line_stipple_enable))
      return "wide, smooth or stippled lines run through the draw module";
   if (!m9_vs_source_layout_init(p, ctx->velems->elements, ctx->velems->count,
                                 ctx->vertex_buffers, ctx->num_vertex_buffers,
                                 &out->layout, &layout_reason))
      return layout_reason;
   out->buffer_count = ctx->num_vertex_buffers;
   for (unsigned i = 0; i < out->buffer_count && i < PIPE_MAX_ATTRIBS; i++) {
      const struct pipe_vertex_buffer *vb = &ctx->vertex_buffers[i];

      /* A user buffer has no size the driver knows; the draw module reads
       * it under the same contract. */
      out->map[i] = vb->is_user_buffer ? vb->buffer.user :
                    vb->buffer.resource ? m9_res(vb->buffer.resource)->map :
                                          NULL;
      out->size[i] = vb->is_user_buffer || !vb->buffer.resource ? UINT_MAX :
                     vb->buffer.resource->width0;
   }
   out->indices = NULL;
   out->index_size = info->index_size;
   out->index_bias = draw->index_bias;
   if (info->index_size) {
      const uint8_t *base = info->has_user_indices ? info->index.user :
                            info->index.resource ?
                            m9_res(info->index.resource)->map : NULL;
      uint32_t lo, hi;

      if (!base)
         return "the index buffer is not mapped";
      out->indices = base + (size_t)draw->start * info->index_size;
      if (!info->has_user_indices &&
          (uint64_t)(draw->start + draw->count) * info->index_size >
          info->index.resource->width0)
         return "the index run leaves the index buffer";
      m9_native_index_range(out->indices, info->index_size, 0, draw->count,
                            &lo, &hi);
      low = (int64_t)lo + draw->index_bias;
      if (low < 0)
         return "a biased index is negative";
      out->first_vertex = (uint32_t)low;
      out->vertex_count = hi - lo + 1;
   } else {
      out->first_vertex = draw->start;
      out->vertex_count = draw->count;
   }
   if (!m9_vs_source_in_range(&out->layout, out->size, out->buffer_count,
                              out->first_vertex, out->vertex_count))
      return "the draw reads outside the bound vertex buffers";
   return NULL;
}

/* What the raster pass of a native preprocessing job can consume.  The pass
 * writes one varying stream and nothing fills the CPU views, so a fragment
 * program may read that one varying and no texture coordinate. */
static const char *
m9_native_vs_raster_ok(const struct m9_job_key *key,
                       const struct m9_vs_program *p)
{
   const struct m9_fs_program *fp;

   /* The pass-through colour image reads the primary colour from stream 0. */
   if (!key->fs)
      return p->has_varying && p->varying_location == VARYING_SLOT_COL0 ?
             NULL : "the pass-through fragment image needs a primary colour";
   fp = m9_fs_final(key->variant);
   if (key->variant->passes.count > 1)
      return "the fragment program runs as several raster passes";
   if (fp->texture_count)
      return "the fragment program samples a texture";
   if (!fp->varying_count)
      return NULL;
   if (fp->varying_count > 1)
      return "the fragment program reads more than one varying";
   return p->has_varying && p->varying_location == fp->varyings[0].location ?
          NULL : "the vertex program does not write the varying read";
}

static bool
m9_native_vs_append(struct m9_context *ctx, const struct m9_job_key *key,
                    const struct m9_native_vs *n, unsigned count)
{
   const unsigned max = m9_draw_max_source_vertices();
   unsigned floats = n->layout.record_dwords;
   bool list = n->topology == M9_TOPOLOGY_LINES ||
               n->topology == M9_TOPOLOGY_TRIANGLES;
   unsigned grain = n->topology == M9_TOPOLOGY_TRIANGLES ? 3 :
                    n->topology == M9_TOPOLOGY_LINES ? 2 : count;

   if (!n->indices) {
      if (count > max && !list)
         return false;
      for (unsigned done = 0; done < count;) {
         unsigned k = MIN2(count - done, max / grain * grain);
         uint32_t first;
         float *extra, *out;

         out = m9_job_reserve(ctx, key, &ctx->key_consts, &ctx->key_vs_consts,
                              floats, k, &first, &extra);
         if (!out ||
             !m9_vs_source_records(&n->layout, n->map, n->size,
                                   n->buffer_count, n->first_vertex + done, k,
                                   out))
            return true;
         m9_job_commit(ctx, out, NULL, k, first, 0, 0, n->topology, n->cull,
                       n->lines, n->flat);
         done += k;
      }
      return true;
   }
   for (unsigned done = 0; done < count;) {
      unsigned k = count - done, vertices;
      uint32_t lo, hi, first, index_first;
      float *extra, *out;
      uint16_t *words;

      m9_native_index_range(n->indices, n->index_size, done, k, &lo, &hi);
      /* Halve the run until the vertices it spans fit one job's records. */
      while (hi - lo + 1 > max) {
         k = k / 2 / grain * grain;
         if (!k)
            return true;
         m9_native_index_range(n->indices, n->index_size, done, k, &lo, &hi);
      }
      vertices = hi - lo + 1;
      out = m9_job_reserve(ctx, key, &ctx->key_consts, &ctx->key_vs_consts,
                           floats, vertices, &first, &extra);
      if (!out ||
          !m9_vs_source_records(&n->layout, n->map, n->size, n->buffer_count,
                                lo + n->index_bias, vertices, out))
         return true;
      index_first = util_dynarray_num_elements(&ctx->indices, uint16_t);
      words = util_dynarray_grow(&ctx->indices, uint16_t,
                                 k + (k & 1) + M9_INDEX_TAIL);
      if (!words) {
         ctx->records.size = (size_t)first * floats * sizeof(float);
         m9_warn_once(ctx, M9_WARN_MEMORY, "out of memory for raster indices");
         return true;
      }
      for (unsigned i = 0; i < k; i++)
         words[i] = first +
                    m9_native_index_at(n->indices, n->index_size, done + i) -
                    lo;
      for (unsigned i = k; i < k + (k & 1) + M9_INDEX_TAIL; i++)
         words[i] = words[k - 1];
      m9_prof_indexed += k;
      m9_prof_referenced += vertices;
      m9_job_commit(ctx, out, NULL, vertices, first, index_first, k,
                    n->topology, n->cull, n->lines, n->flat);
      done += k;
   }
   return true;
}

/* True when the draw was taken by the native path; false leaves it to the
 * draw module with no change in behaviour. */
static bool
m9_native_vs_draw(struct m9_context *ctx, const struct pipe_draw_info *info,
                  const struct pipe_draw_start_count_bias *draw)
{
   struct m9_native_vs n;
   struct m9_job_key key;
   const char *reason;

   if (!m9_native_vs || !ctx->vs || !ctx->vs->native ||
       !m9_native_vs_fs_cheap(ctx)) {
      if (m9_native_vs)
         ctx->native_vs_fallbacks++;
      return false;
   }
   reason = m9_native_vs_resolve(ctx, info, draw, &n);
   if (reason || !m9_job_key_current(ctx, &key)) {
      if (!reason) {
         /* The state is unsupported for either path; m9_job_key_current
          * has named it. */
         ctx->skipped_draws++;
         return true;
      }
      ctx->native_vs_fallbacks++;
      if (ctx->debug && ctx->native_vs_fallbacks % 1000 == 1)
         fprintf(stderr, "m9x: native vertex fallback: %s\n", reason);
      return false;
   }
   reason = m9_native_vs_raster_ok(&key, &ctx->vs->program);
   if (reason) {
      ctx->native_vs_fallbacks++;
      if (ctx->debug && ctx->native_vs_fallbacks % 1000 == 1)
         fprintf(stderr, "m9x: native vertex fallback: %s\n", reason);
      return false;
   }
   key.vs = &ctx->vs->program;
   key.vs_program = m9_native_vs_c2(ctx, (struct m9_fs_variant *)key.variant);
   if (!key.vs_program) {
      ctx->native_vs_fallbacks++;
      return false;
   }
   for (unsigned i = 0; i < 3; i++) {
      key.viewport_scale[i] = ctx->viewport.scale[i];
      key.viewport_bias[i] = ctx->viewport.translate[i];
   }
   if (!m9_vs_resolve_consts(ctx, key.vs, &ctx->key_vs_consts)) {
      ctx->skipped_draws++;
      return true;
   }
   if (!m9_native_vs_append(ctx, &key, &n, draw->count)) {
      ctx->native_vs_fallbacks++;
      return false;
   }
   ctx->native_vs_draws++;
   return true;
}

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
   m9_vs_texture_refresh(pctx);
   /* Count at the entry point: a draw dropped here never reaches the
    * vbuf render, and a killed client never destroys its context. */
   if (ctx->debug && ++ctx->submitted_draws % 200 == 0) {
      fprintf(stderr, "m9x: %u draws seen, %u dropped before submission\n",
              ctx->submitted_draws, ctx->skipped_draws);
      m9_skip_report(ctx);
   }
   if (!num_draws || indirect || !draws[0].count || !info->instance_count ||
       ctx->lost || !ctx->params_valid) {
      m9_skip_reason(ctx, !num_draws || !draws[0].count ||
                     !info->instance_count ? "the draw is empty; skipped" :
                     indirect ? "indirect draws are not implemented; skipped" :
                     ctx->lost ? "the context is lost; draw skipped" :
                     "no valid raster parameters; draw skipped");
      ctx->skipped_draws++;
      if (ctx->debug && ctx->skipped_draws % 200 == 0)
         fprintf(stderr, "m9x: draw dropped: draws %u indirect %d count %u "
                 "instances %u lost %d params %d\n", num_draws, !!indirect,
                 num_draws ? draws[0].count : 0, info->instance_count,
                 ctx->lost, ctx->params_valid);
      return;
   }
   if (ctx->fs && (ctx->fs->passthrough || ctx->fs->native) &&
       m9_native_vs_draw(ctx, info, &draws[0]))
      return;
   if (!ctx->fs || (!ctx->fs->passthrough && !ctx->fs->native)) {
      m9_skip_reason(ctx, ctx->fs && ctx->fs->reason ? ctx->fs->reason :
                     "no fragment shader is bound; draw skipped");
      ctx->skipped_draws++;
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
   uint64_t t_draw = m9_prof_begin();

   draw_prepare_shader_outputs(ctx->draw);
   draw_vbo(ctx->draw, info, drawid_offset, NULL, draws, 1, 0);
   draw_flush(ctx->draw);
   m9_prof_end(M9_PROF_DRAW, t_draw);
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
   key.depth = zs ? zs : ctx->target_depth;
   key.aux = zs ? zs->hz : ctx->target_aux;
   key.depth_control = m9_depth_control(depth, depth, PIPE_FUNC_ALWAYS);
   key.depth_ef = m9_depth_ef(depth, depth);
   key.color_mask = m9_color_mask(colormask);
   key.clip_min = ctx->params.clip_min;
   key.clip_max = ctx->params.clip_max;
   if (scissor && !m9_scissor_key(ctx, scissor, &key))
      return;
   out = m9_job_reserve(ctx, &key, NULL, NULL, M9_RECORD_FLOATS, 4, &first,
                        &extra);
   if (!out)
      return;
   for (unsigned i = 0; i < 4; i++) {
      float *rec = out + i * M9_RECORD_FLOATS;

      rec[0] = corner[i][0];
      rec[1] = corner[i][1];
      rec[2] = 1.0f - 2.0f * (float)CLAMP(z, 0.0, 1.0);
      rec[3] = 1.0f;
      for (unsigned c = 0; c < 4; c++)
         rec[4 + c] = CLAMP(rgba[c], 0.0f, 1.0f);
   }
   m9_job_commit(ctx, out, NULL, 4, first, 0, 0, M9_TOPOLOGY_QUADS, 0, false,
                 true);
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
m9_clear_locked(struct pipe_context *pctx, unsigned buffers,
                uint32_t color_clear_mask, uint8_t stencil_clear_mask,
                const struct pipe_scissor_state *scissor,
                const union pipe_color_union *color, double depth,
                unsigned stencil)
{
   static const float zero[4];
   struct m9_context *ctx = m9_context(pctx);
   struct pipe_resource *cbuf = ctx->framebuffer.nr_cbufs ?
                                ctx->framebuffer.cbufs[0].texture : NULL;
   struct m9_resource *zs = m9_context_depth(ctx);
   bool full = m9_scissor_full(ctx, scissor);

   bool colour = (buffers & PIPE_CLEAR_COLOR0) && cbuf;

   if (!ctx->params_valid || m9_no_clear)
      return;
   /* One quad writes both attachments: a clear of colour and depth is what
    * a frame starts with, and two full-screen quads is twice the fill. */
   if (colour && zs && (buffers & PIPE_CLEAR_DEPTH) &&
       !(buffers & PIPE_CLEAR_STENCIL)) {
      m9_clear_quad(ctx, full ? NULL : scissor, color_clear_mask, color->f,
                    true, depth);
      return;
   }
   /* The quad clear rasterises in tiled space and covers the drawable, not
    * the whole object: a dword fill of the object costs three megabytes of
    * uncached writes per frame and reaches past the drawable into the rest
    * of a shared screen surface. */
   if (colour)
      m9_clear_quad(ctx, full ? NULL : scissor, color_clear_mask,
                    color->f, false, 0);
   if (!(buffers & PIPE_CLEAR_DEPTHSTENCIL))
      return;
   if (!zs) {
      m9_warn_once(ctx, M9_WARN_CLEAR, "depth clear needs a D24S8 attachment "
                   "of the target size");
      return;
   }
   /* The quad clear runs on the GPU; only stencil still needs the CPU. */
   if (!(buffers & PIPE_CLEAR_STENCIL) && (buffers & PIPE_CLEAR_DEPTH)) {
      /* The quad writes depth through the engine, which keeps the
       * hierarchical buffer with it; only a CPU depth write has to reset it
       * by hand.  Doing it here cost a stall and a megabyte of uncached
       * writes per frame. */
      m9_clear_quad(ctx, full ? NULL : scissor, 0, zero, true, depth);
   } else if (full && (!(buffers & PIPE_CLEAR_STENCIL) ||
                       stencil_clear_mask == 0xff)) {
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
m9_clear(struct pipe_context *pctx, unsigned buffers,
         uint32_t color_clear_mask, uint8_t stencil_clear_mask,
         const struct pipe_scissor_state *scissor,
         const union pipe_color_union *color, double depth, unsigned stencil)
{
   uint64_t t_clear = m9_prof_begin();

   for (unsigned i = 0; i <= m9_clear_repeat; i++)
      m9_clear_locked(pctx, buffers, color_clear_mask, stencil_clear_mask,
                      scissor, color, depth, stencil);
   m9_prof_end(M9_PROF_CLEAR, t_clear);
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
      /* A mip chain follows level 0 in the same object, so a full clear of
       * the object would wipe the chain with it. */
      uint64_t bytes = resource->texture.levels ?
                       resource->texture.level[0].face_bytes : resource->size;
      uint32_t *words = resource->map;

      for (uint64_t i = 0; i < bytes / 4; i++)
         words[i] = uc.ui[0];
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

/* Unscaled colour blit with format conversion through CPU transfers. */
static bool
m9_blit_convert(struct pipe_context *pctx, const struct pipe_blit_info *info)
{
   const struct pipe_box *s = &info->src.box, *d = &info->dst.box;
   struct pipe_transfer *st = NULL, *dt = NULL;
   uint8_t *src, *dst;
   bool ok;

   unsigned blocksize;
   uint8_t *scaled = NULL;

   if (info->mask != PIPE_MASK_RGBA || info->scissor_enable ||
       info->render_condition_enable || info->alpha_blend ||
       s->depth != 1 || d->depth != 1 || s->width <= 0 || s->height <= 0 ||
       d->width <= 0 || d->height <= 0 || info->swizzle_enable)
      return false;
   src = pipe_texture_map(pctx, info->src.resource, info->src.level, s->z,
                          PIPE_MAP_READ, s->x, s->y, s->width, s->height, &st);
   dst = src ? pipe_texture_map(pctx, info->dst.resource, info->dst.level, d->z,
                                PIPE_MAP_WRITE, d->x, d->y, d->width,
                                d->height, &dt) : NULL;
   blocksize = util_format_get_blocksize(info->dst.format);
   if (src && dst && (s->width != d->width || s->height != d->height)) {
      /* Nearest sampling: translate the source, then pick its texels. */
      scaled = MALLOC((size_t)s->width * s->height * blocksize);
      ok = scaled &&
           util_format_translate(info->dst.format, scaled,
                                 s->width * blocksize, 0, 0, info->src.format,
                                 src, st->stride, 0, 0, s->width, s->height);
      for (int y = 0; ok && y < d->height; y++) {
         int sy = (int)(((int64_t)y * s->height + s->height / 2) / d->height);

         for (int x = 0; x < d->width; x++) {
            int sx = (int)(((int64_t)x * s->width + s->width / 2) / d->width);

            memcpy(dst + (size_t)y * dt->stride + (size_t)x * blocksize,
                   scaled + ((size_t)MIN2(sy, s->height - 1) * s->width +
                             MIN2(sx, s->width - 1)) * blocksize, blocksize);
         }
      }
   } else {
      ok = src && dst &&
           util_format_translate(info->dst.format, dst, dt->stride, 0, 0,
                                 info->src.format, src, st->stride, 0, 0,
                                 s->width, s->height);
   }
   FREE(scaled);
   if (dt)
      pipe_texture_unmap(pctx, dt);
   if (st)
      pipe_texture_unmap(pctx, st);
   return ok;
}

static void
m9_blit(struct pipe_context *pctx, const struct pipe_blit_info *info)
{
   if (!util_try_blit_via_copy_region(pctx, info, false) &&
       !m9_blit_convert(pctx, info))
      m9_warn_once(m9_context(pctx), M9_WARN_BLIT, "scaled, scissored or "
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

   if (shader == MESA_SHADER_VERTEX)
      m9_vs_texture_states(pctx, start, count, states);
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

   if (shader == MESA_SHADER_VERTEX)
      m9_vs_texture_views(pctx, start, count, unbind_trailing, views);
   if (shader != MESA_SHADER_FRAGMENT)
      return;
   for (unsigned i = 0; i < count + unbind_trailing &&
                        start + i < PIPE_MAX_SAMPLERS; i++)
      pipe_sampler_view_reference(&ctx->fs_views[start + i],
                                  views && i < count ? views[i] : NULL);
}

/* Program identity for the cached c2 images; never zero, never reused. */
static uint64_t m9_vs_serial;

static unsigned
m9_vs_type_size(const struct glsl_type *type, bool bindless)
{
   return glsl_count_vec4_slots(type, false, bindless);
}

/* Compile the native preprocessing program from a clone: the shader the
 * state tracker handed over belongs to the draw module, which still runs
 * every vertex.  Nothing reads the result yet -- binding it needs the
 * source records and the varying stream to come from the vertex program
 * (docs/parhelia-m9-native-vertex-backend.md section 8). */
static void
m9_vs_native_init(struct m9_vs_state *vs, struct nir_shader *src)
{
   nir_shader *nir = nir_shader_clone(NULL, src);
   unsigned location = 0;

   if (!nir)
      return;
   if (!nir->info.io_lowered) {
      nir_foreach_shader_in_variable(var, nir)
         var->data.driver_location = location++;
      location = 0;
      nir_foreach_shader_out_variable(var, nir)
         var->data.driver_location = location++;
      NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
               m9_vs_type_size, (nir_lower_io_options)0);
   }
   /* Mesa fuses no multiply and add on its own; the backend folds the
    * product the hardware's multiply-add can take, and this pass first
    * gives it the shapes it recognises. */
   NIR_PASS(_, nir, nir_opt_algebraic_late);
   NIR_PASS(_, nir, nir_opt_dce);
   vs->native = m9_vs_compile(nir, &vs->program, &vs->reason);
   if (getenv("M9X_DEBUG"))
      fprintf(stderr, "m9x vs: %s\n",
              vs->native ? "native" : vs->reason ? vs->reason : "unsupported");
   ralloc_free(nir);
}

static void *
m9_create_vs_state(struct pipe_context *pctx,
                   const struct pipe_shader_state *templ)
{
   struct m9_context *ctx = m9_context(pctx);
   struct m9_vs_state *vs = CALLOC_STRUCT(m9_vs_state);

   if (!vs)
      return NULL;
   if (templ->type == PIPE_SHADER_IR_NIR && templ->ir.nir)
      m9_vs_native_init(vs, templ->ir.nir);
   vs->serial = ++m9_vs_serial;
   vs->draw = draw_create_vertex_shader(ctx->draw, templ);
   if (!vs->draw) {
      if (vs->native)
         m9_vs_program_fini(&vs->program);
      FREE(vs);
      return NULL;
   }
   return vs;
}

static void
m9_bind_vs_state(struct pipe_context *pctx, void *state)
{
   struct m9_context *ctx = m9_context(pctx);
   struct m9_vs_state *vs = state;

   ctx->vs = vs;
   draw_bind_vertex_shader(ctx->draw, vs ? vs->draw : NULL);
}

static void
m9_delete_vs_state(struct pipe_context *pctx, void *state)
{
   struct m9_context *ctx = m9_context(pctx);
   struct m9_vs_state *vs = state;

   if (ctx->vs == vs)
      ctx->vs = NULL;
   draw_delete_vertex_shader(ctx->draw, vs->draw);
   if (vs->native)
      m9_vs_program_fini(&vs->program);
   FREE(vs);
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
   struct m9_fs_variant opaque = { .blend = { M9_FS_ONE, M9_FS_ZERO } };

   for (unsigned s = 0; s < PIPE_MAX_SAMPLERS; s++)
      for (unsigned c = 0; c < 4; c++)
         opaque.swizzle[s][c] = PIPE_SWIZZLE_X + c;
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
         m9_bo_destroy(ctx->screen, &(*it)->native_c2);
         m9_fs_passes_fini(&(*it)->passes);
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

   m9_context(pctx)->velems = velems;
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

/* The depth and HZ pair for a target of this size, allocated once and kept.
 * Depth testing is refused outright when the target has no attachment of its
 * own, so the content is never read and a reused pair needs no refill; only
 * a fresh one is filled, the way a fresh one always was. */
static bool
m9_target_depth_bind(struct m9_context *ctx, uint32_t w, uint32_t h,
                     uint64_t bytes)
{
   struct m9_target_depth *entry = NULL, **oldest = NULL;
   uint64_t total = 0;

   util_dynarray_foreach(&ctx->target_depths, struct m9_target_depth *, it) {
      total += (*it)->depth.size + (*it)->aux.size;
      if ((*it)->width == w && (*it)->height == h)
         entry = *it;
      else if (!oldest || (*it)->used < (*oldest)->used)
         oldest = it;
   }
   if (!entry) {
      /* Release the size used longest ago before adding another, so a
       * context that walks through many sizes does not grow without end. */
      while (oldest && total + bytes + m9_hz_bytes(w, h) >
             M9_TARGET_DEPTH_BUDGET) {
         struct m9_target_depth *evict = *oldest;

         total -= evict->depth.size + evict->aux.size;
         m9_bo_destroy(ctx->screen, &evict->depth);
         m9_bo_destroy(ctx->screen, &evict->aux);
         *oldest = *util_dynarray_pop_ptr(&ctx->target_depths,
                                          struct m9_target_depth *);
         FREE(evict);
         oldest = NULL;
         util_dynarray_foreach(&ctx->target_depths,
                               struct m9_target_depth *, it)
            if (!oldest || (*it)->used < (*oldest)->used)
               oldest = it;
      }
      entry = CALLOC_STRUCT(m9_target_depth);
      if (!entry ||
          m9_bo_create(ctx->screen, bytes, PARHELIA_M9_BO_VRAM,
                       M9_RESOURCE_LINEAR_DEPTH, &entry->depth) ||
          m9_bo_create(ctx->screen, m9_hz_bytes(w, h), PARHELIA_M9_BO_VRAM,
                       M9_RESOURCE_LINEAR_DEPTH, &entry->aux)) {
         if (entry) {
            m9_bo_destroy(ctx->screen, &entry->depth);
            m9_bo_destroy(ctx->screen, &entry->aux);
            FREE(entry);
         }
         m9_warn_once(ctx, M9_WARN_MEMORY, "cannot allocate target depth");
         return false;
      }
      entry->width = entry->depth.width = w;
      entry->height = entry->depth.height = h;
      m9_fill_dwords(&entry->depth, 0xffffffff);
      m9_fill_dwords(&entry->aux, M9_HZ_CLEAR);
      util_dynarray_append_typed(&ctx->target_depths,
                                 struct m9_target_depth *, entry);
   }
   entry->used = ++ctx->target_clock;
   ctx->target_depth = &entry->depth;
   ctx->target_aux = &entry->aux;
   return true;
}

static void
m9_update_target(struct m9_context *ctx)
{
   const struct pipe_surface *cbuf = &ctx->framebuffer.cbufs[0];
   struct m9_resource *color = ctx->framebuffer.nr_cbufs && cbuf->texture ?
                               m9_res(cbuf->texture) : NULL;
   struct m9_render_layout layout;
   uint64_t t_target = m9_prof_begin();
   uint32_t w, h;

   ctx->params_valid = false;
   pipe_resource_reference(&ctx->color, NULL);
   if (!color)
      return;
   w = color->width;
   h = color->height;
   /* A mipmapped image holds its level 0 in the render surface layout, so it
    * is a target as it stands; only the base level can be rendered into. */
   if (cbuf->level || !(color->tiled || m9_texture_renders_level0(color)) ||
       m9_context_set_framebuffer(ctx, color, NULL, w, h) ||
       m9_draw_params_init(&ctx->params, w, h, color->format) ||
       !m9_render_surface_layout(w, h, 4, &layout)) {
      m9_warn_once(ctx, M9_WARN_TARGET, "render target is not a tiled "
                   "A8R8G8B8 surface; draws skipped");
      return;
   }
   /* Depth-less targets still bind a depth and HZ BO of their size. */
   if (!m9_target_depth_bind(ctx, w, h, layout.size))
      return;
   ctx->params_valid = true;
   m9_prof_end(M9_PROF_TARGET, t_target);
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
   if (!start && count)
      m9_context(pctx)->viewport = vp[0];
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
   m9_prof_frame();
   if (ctx->debug && ++ctx->flush_count % 300 == 0)
      fprintf(stderr, "m9x: after %u frames job splits: state %u uniforms %u "
              "vertices %u stream %u | target %u program %u depth %u mask %u "
              "clip %u\n", ctx->flush_count, ctx->split_state,
              ctx->split_uniforms, ctx->split_vertices, ctx->split_stream,
              ctx->split_target, ctx->split_program, ctx->split_depth,
              ctx->split_mask, ctx->split_clip);
   ctx->frame_jobs = 0;
   m9_context_sync(ctx);
   if (m9_record_dump && ctx->record_written && ctx->screen->record.map) {
      const struct m9_resource *rec = &ctx->screen->record;
      FILE *f = fopen(m9_record_dump, "wb");

      if (f) {
         fwrite(rec->map, 1, rec->size, f);
         fclose(f);
      }
      fprintf(stderr, "m9x: record %u bytes stride %u target %ux%u -> %s\n",
              (unsigned)rec->size, ctx->screen->record_dwords,
              ctx->params.width, ctx->params.height, m9_record_dump);
      ctx->record_written = false;
   }
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
   /* The second set of the two arenas that are sized once rather than
    * grown per job; source and index are reserved on demand. */
   if (!ret)
      ret = m9_bo_create(screen, M9_SYSTEM_BYTES, 0, M9_RESOURCE_SYSTEM_STATE,
                         &ctx->system_alt);
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
      ret = m9_bo_create(screen,
                         (M9_TRANSIENT_VIEWS - M9_TRANSIENT_CPU_VIEW) *
                         M9_TRANSIENT_VIEW_BYTES, 0,
                         M9_RESOURCE_LINEAR_BUFFER, &ctx->cpu_streams);
   if (!ret)
      ret = m9_bo_create(screen,
                         (M9_TRANSIENT_VIEWS - M9_TRANSIENT_CPU_VIEW) *
                         M9_TRANSIENT_VIEW_BYTES, 0,
                         M9_RESOURCE_LINEAR_BUFFER, &ctx->cpu_streams_alt);
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
   /* Opaque black: an incomplete GL texture samples (0, 0, 0, 1). */
   m9_fill_dwords(&ctx->placeholder_texture, 0xff000000u);
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

   if (ctx->debug)
      m9_skip_report(ctx);
   m9_vs_texture_fini(pctx);
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
   m9_batch_fini(&ctx->batch);
   m9_command_bo_destroy(screen, &ctx->command);
   m9_bo_destroy(screen, &ctx->system);
   m9_bo_destroy(screen, &ctx->program);
   m9_bo_destroy(screen, &ctx->fragment_db);
   m9_bo_destroy(screen, &ctx->transient);
   m9_bo_destroy(screen, &ctx->cpu_streams);
   m9_bo_destroy(screen, &ctx->be);
   m9_bo_destroy(screen, &ctx->source);
   m9_bo_destroy(screen, &ctx->index);
   m9_bo_destroy(screen, &ctx->system_alt);
   m9_bo_destroy(screen, &ctx->cpu_streams_alt);
   m9_bo_destroy(screen, &ctx->source_alt);
   m9_bo_destroy(screen, &ctx->index_alt);
   util_dynarray_foreach(&ctx->target_depths, struct m9_target_depth *, it) {
      m9_bo_destroy(screen, &(*it)->depth);
      m9_bo_destroy(screen, &(*it)->aux);
      FREE(*it);
   }
   util_dynarray_fini(&ctx->target_depths);
   ctx->target_depth = ctx->target_aux = NULL;
   m9_bo_destroy(screen, &ctx->placeholder_texture);
   m9_bo_destroy(screen, &ctx->vs_c2);
   if (ctx->debug)
      fprintf(stderr, "m9x: job splits: state %u uniforms %u vertices %u "
              "stream %u\n", ctx->split_state, ctx->split_uniforms,
              ctx->split_vertices, ctx->split_stream);
   if (ctx->debug || m9_native_vs)
      fprintf(stderr, "m9x: native vertex draws %u, draw module %u\n",
              ctx->native_vs_draws, ctx->native_vs_fallbacks);
   util_dynarray_foreach(&ctx->job_textures, struct pipe_resource *, held)
      pipe_resource_reference(held, NULL);
   util_dynarray_fini(&ctx->job_textures);
   util_dynarray_fini(&ctx->records);
   util_dynarray_fini(&ctx->indices);
   util_dynarray_fini(&ctx->draws);
   util_dynarray_fini(&ctx->extra);
   util_dynarray_fini(&ctx->key_consts);
   util_dynarray_fini(&ctx->job_consts);
   util_dynarray_fini(&ctx->key_vs_consts);
   util_dynarray_fini(&ctx->job_vs_consts);
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
   m9_profile_period = debug_get_num_option("M9X_PROFILE", 0);
   m9_job_draws = debug_get_num_option("M9X_JOB_DRAWS", 0);
   m9_indexed_draws = debug_get_bool_option("M9X_INDEXED", true);
   m9_native_vs = debug_get_bool_option("M9X_NATIVE_VS", true);
   m9_no_clear = debug_get_bool_option("M9X_NO_CLEAR", false);
   m9_clear_repeat = debug_get_num_option("M9X_CLEAR_REPEAT", 0);
   ctx->dump_path = debug_get_option("M9X_DUMP", NULL);
   m9_debug_textures = debug_get_bool_option("M9X_DEBUG_TEXTURES", false);
   m9_texture_dump = debug_get_option("M9X_TEXTURE_DUMP", NULL);
   m9_debug_ctx = ctx;
   m9_pass_loop = debug_get_bool_option("M9X_PASS_LOOP", false);
   m9_split_stage = debug_get_num_option("M9X_SPLIT_STAGE", 3);
   m9_split_preprocess = debug_get_bool_option("M9X_SPLIT_PREPROCESS", false);
   m9_split_depth = debug_get_num_option("M9X_SPLIT_DEPTH", 1);
   m9_split_submit = debug_get_bool_option("M9X_SPLIT_SUBMIT", true);
   m9_split_passes = debug_get_num_option("M9X_SPLIT_PASSES", 0);
   m9_record_dump = debug_get_option("M9X_RECORD_DUMP", NULL);
   m9_split_draws = debug_get_num_option("M9X_SPLIT_DRAWS", 1);
   m9_record_89 = debug_get_num_option("M9X_RECORD_89", 0);
   m9_split_coverage = debug_get_bool_option("M9X_SPLIT_COVERAGE", true);
   m9_transition = debug_get_bool_option("M9X_TRANSITION", true);
   m9_split_enable = debug_get_bool_option("M9X_SPLIT", true);
   ctx->dump_from = debug_get_num_option("M9X_DUMP_FROM", 0);
   ctx->dump_count = debug_get_num_option("M9X_DUMP_COUNT", 4096);
   ctx->only_first = debug_get_num_option("M9X_ONLY_FIRST", UINT_MAX);
   ctx->only_after = debug_get_num_option("M9X_ONLY_AFTER", 0);
   ctx->dry_run = debug_get_bool_option("M9X_DRYRUN", false);
   util_dynarray_init(&ctx->records, NULL);
   util_dynarray_init(&ctx->target_depths, NULL);
   util_dynarray_init(&ctx->indices, NULL);
   util_dynarray_init(&ctx->draws, NULL);
   util_dynarray_init(&ctx->extra, NULL);
   util_dynarray_init(&ctx->key_consts, NULL);
   util_dynarray_init(&ctx->job_consts, NULL);

   m9_vs_texture_init(&ctx->base);
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

   ctx->record_floats = M9_RECORD_FLOATS;
   if (m9_vertex_decl_init(&ctx->decl, 4, 4) ||
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
