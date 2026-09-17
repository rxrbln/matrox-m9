/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Two-pass job: the preprocessing pass runs the captured pass-through
 * program over the source records into four transient streams, the raster
 * pass consumes them into the render target with one or more draws.
 * Size and state dependent words come from m9_draw_params.
 */
#include "m9_draw.h"

#include <errno.h>

#include "util/macros.h"

#define V(i, v) { .index = (i), .value = (v) }
#define R(i, h, o, k) { .index = (i), .handle = (h), .offset = (o), \
                        .kind = PARHELIA_M9_RELOC_##k }
#define VIEW(n) ((uint64_t)(n) * M9_TRANSIENT_VIEW_BYTES)
#define EMIT(...) do { \
   const struct m9_packet_value packet_[] = { __VA_ARGS__ }; \
   ret = m9_batch_emit_packet(batch, packet_, ARRAY_SIZE(packet_)); \
   if (ret) \
      goto fail; \
} while (0)

/* Dwords of a single-draw job and of each restated raster draw. */
#define M9_JOB_BASE_DWORDS 233u
#define M9_JOB_DRAW_DWORDS 11u

unsigned
m9_draw_job_dwords(unsigned draw_count)
{
   return draw_count ?
          M9_JOB_BASE_DWORDS + M9_JOB_DRAW_DWORDS * (draw_count - 1) : 0;
}

unsigned
m9_draw_job_dwords_fs(unsigned draw_count, unsigned unit_count)
{
   /* A fifth stream packet; the declaration stays within two packets.  A
    * bound unit replaces its zero 70+unit packet by 15 dwords, 20 with the
    * placeholder's cleared 82..85. */
   return m9_draw_job_dwords(draw_count) + 4 + 18 * unit_count;
}

/* Relocations a job emits: the two passes bind about thirty objects, every
 * restated unit block one more each, and an indexed draw a start and an end. */
unsigned
m9_draw_job_relocs(unsigned draw_count, unsigned unit_count)
{
   return 30 + unit_count + draw_count * (unit_count + 2);
}

unsigned
m9_draw_units_dwords(unsigned unit_count)
{
   /* One restated unit block inside a job: the 74 group, the bound units and
    * the zero 70+unit packets for the units left unbound. */
   return 10 + 18 * unit_count;
}

static int
m9_emit_units(struct m9_batch *batch, const struct m9_texture_unit *units,
              unsigned unit_count)
{
   int ret = 0;

   for (unsigned i = 0; i < 4; i++)
      EMIT(V(0x74 + i, 0x100));
   for (unsigned i = 0; i < unit_count; i++) {
      ret = m9_batch_emit_texture_unit(batch, i, &units[i]);
      if (ret)
         goto fail;
   }
   for (unsigned i = unit_count; i < 4; i++)
      EMIT(V(0x70 + i, 0));
fail:
   return ret;
}

static int
m9_emit_linked(struct m9_batch *batch, const struct m9_draw_job *job)
{
   int ret = 0;

   EMIT(R(0x69, job->fragment_db, 0, RESOURCE_69));
   EMIT(V(0x6a, 0x25010000));
   EMIT(V(0x68, 0), V(0x6b, 0x1030),
        R(0x6c, job->system, M9_LINKED_OFFSET, RESOURCE_6C));
fail:
   return ret;
}

static uint32_t
m9_job_raster_control(const struct m9_draw_job *job,
                      const struct m9_draw_record *d)
{
   return job->fs ? m9_raster_control_decl(d->flat, d->topology,
                                           job->fs->decl_count) :
                    m9_raster_control(d->flat, d->topology);
}

/* Streams, program entry and constant span of a native raster pass. */
static int
m9_emit_raster_fs(struct m9_batch *batch, const struct m9_draw_job *job)
{
   const struct m9_raster_fs *fs = job->fs;
   uint64_t base = M9_RASTER_CONST_OFFSET;
   int ret = 0;

   EMIT(V(0x88, fs->r88), V(0x89, 0xfe207ff8), V(0x8a, 0), V(0xc1, 0));
   for (unsigned pair = 0; pair < 3 && 2 * pair < fs->stream_count; pair++) {
      unsigned s = 2 * pair;

      if (s + 1 < fs->stream_count) {
         EMIT(R(s, job->transient, VIEW(fs->stream_view[s]), STREAM),
              R(s + 1, job->transient, VIEW(fs->stream_view[s + 1]), STREAM),
              V(0x10 + pair, fs->stride[pair]));
      } else {
         EMIT(R(s, job->transient, VIEW(fs->stream_view[s]), STREAM),
              V(0x10 + pair, fs->stride[pair]));
      }
   }
   EMIT(R(0xc2, job->program, 0, PROGRAM), V(0xc3, 0), V(0x8f, 0));
   EMIT(V(0x8c, 0x00500000 | fs->const_count << 8),
        R(0x8d, job->system, base, SYSTEM),
        R(0x8e, job->system, base + 8 * fs->const_count, SYSTEM), V(0x8f, 0));
fail:
   return ret;
}

static int
m9_emit_raster_decl(struct m9_batch *batch, const struct m9_raster_fs *fs)
{
   unsigned words = (fs->decl_count + 3) & ~3u;
   int ret = 0;

   for (unsigned i = 0; i < words; i += 4) {
      uint32_t v[4];

      for (unsigned j = 0; j < 4; j++)
         v[j] = i + j < fs->decl_count ? fs->decl[i + j] : 0;
      EMIT(V(0x23, v[0]), V(0x23, v[1]), V(0x23, v[2]), V(0x23, v[3]));
   }
fail:
   return ret;
}

static bool
m9_draw_job_valid(const struct m9_draw_job *job)
{
   if (!job->params || !job->decl || !job->source || !job->system ||
       !job->program || !job->fragment_db || !job->transient ||
       !job->color || !job->depth || !job->aux || !job->be ||
       !job->draws || !job->draw_count || job->source_vertices % 4 ||
       job->source_vertices > m9_draw_max_source_vertices() ||
       job->unit_count > M9_TEXTURE_MAX_UNITS ||
       (job->unit_count && (!job->fs || !job->units)))
      return false;
   for (unsigned i = 0; i < job->unit_count; i++)
      if (!job->units[i].handle)
         return false;
   if (job->index_count && job->index_base % 4)
      return false;
   for (unsigned i = 0; i < job->draw_count; i++) {
      const struct m9_draw_record *d = &job->draws[i];

      if (d->start % 4 || !d->count || d->start > job->source_vertices ||
          d->count > job->source_vertices - d->start ||
          !m9_topology_valid(d->topology))
         return false;
      /* An indexed draw reads a dword-aligned run inside the stored list. */
      if (d->index_count &&
          (d->index_first % 2 || d->index_first > job->index_count ||
           d->index_count > job->index_count - d->index_first))
         return false;
   }
   return true;
}

int
m9_batch_emit_draw_job(struct m9_batch *batch, const struct m9_draw_job *job)
{
   const struct m9_draw_params *p;
   const struct m9_draw_record *d0;
   struct m9_batch_checkpoint checkpoint;
   int ret;

   if (!batch || !job || batch->sealed || !m9_draw_job_valid(job))
      return -EINVAL;
   p = job->params;
   d0 = &job->draws[0];
   m9_batch_checkpoint(batch, &checkpoint);

   /* Preprocessing pass: dependency steps 0 and 1. */
   EMIT(V(0xe6, 3), V(0xe3, 0x00));
   EMIT(V(0xe6, 3));
   EMIT(V(0xf0, 0xf0000400), V(0xef, 0x00288800), V(0xd7, 0x000050ae));
   EMIT(V(0x22, 0x0008040e));
   EMIT(V(0xe6, 3), V(0xe3, 0x10));
   EMIT(R(0xb0, job->transient, VIEW(0), STREAM), V(0xb1, 0x68002000));
   EMIT(R(0xb2, job->transient, VIEW(3), STREAM), V(0xb3, 0x68002000));
   EMIT(R(0xb4, job->transient, VIEW(1), STREAM), V(0xb5, 0x68002000));
   EMIT(R(0xb8, job->transient, VIEW(2), STREAM), V(0xb9, 0xb8002000));
   EMIT(V(0xba, 0x00000020), V(0xbb, 0xf0010040));
   EMIT(V(0xbe, 3));
   EMIT(V(0xad, 0x20), V(0xad, 0x20), V(0xad, 0x20), V(0xad, 0x20));
   EMIT(V(0xaf, 0));
   EMIT(V(0xa8, 0x1f), V(0xa9, 0), V(0xaa, 0));
   EMIT(V(0xab, 0xffffffff), V(0xac, 0xffffffff), V(0xae, 0x22444442));
   EMIT(V(0xf5, 0), V(0xf6, 0x7fff7fff), V(0xc4, 0), V(0xc5, 0x7fff7fff));
   EMIT(V(0x88, 0x0300bf00), V(0x89, 0xfe201e92), V(0x8a, 0), V(0xc1, 0));
   EMIT(R(0x00, job->source, 0, SYSTEM), V(0x01, 0xf2fe014f),
        V(0x10, job->decl->record_bytes));
   EMIT(R(0xc2, job->program, 0, PROGRAM),
        V(0xc3, job->fs ? job->fs->vertex_entry : 4), V(0x8f, 0));
   EMIT(V(0x8c, 0x00501600), R(0x8d, job->system, 0x10, SYSTEM),
        R(0x8e, job->system, 0xc0, SYSTEM), V(0x8f, 0));
   ret = m9_emit_units(batch, NULL, 0);
   if (ret)
      goto fail;
   EMIT(V(0xec, 0), V(0xf4, 0x00010004), V(0xea, 0), V(0xeb, 0x46000000));
   EMIT(V(0x30, 0));
   EMIT(V(0x28, 0));
   EMIT(V(0x23, job->decl->words[0]), V(0x23, job->decl->words[1]),
        V(0x23, job->decl->words[2]), V(0x23, job->decl->words[3]));
   EMIT(V(0x23, 0x00004b08), V(0x23, 0), V(0x23, 0), V(0x23, 0));
   ret = m9_emit_linked(batch, job);
   if (ret)
      goto fail;
   EMIT(V(0x19, 0), V(0x18, 0x7fffffff));
   /* One covering preprocessing draw serves every raster draw. */
   ret = m9_batch_emit_draw_numeric(batch, 0, job->source_vertices,
                                    0x20000000);
   if (!ret)
      ret = m9_batch_emit_range_break(batch);
   if (ret)
      goto fail;

   /* Raster pass: dependency steps 2 to 4, streams read as [b0,b4,b8,b2]. */
   EMIT(V(0xe6, 3), V(0xe3, 0x20));
   EMIT(V(0xe6, 3));
   EMIT(V(0xe6, 3), V(0xe3, 0x30));
   EMIT(V(0x43, 0x30));
   EMIT(V(0xe6, 0x42000000));
   EMIT(V(0xf0, m9_raster_class(d0->lines, job->blend_dest)), V(0xef, d0->cull | job->depth_ef),
        V(0xd7, m9_raster_d7(d0->topology)));
   EMIT(V(0x22, m9_job_raster_control(job, d0)));
   EMIT(V(0xe6, 3), V(0xe3, 0x40));
   EMIT(R(0xb0, job->color, 0, COLOR), V(0xb1, p->color_desc));
   EMIT(R(0xb8, job->depth, 0, DEPTH), V(0xb9, p->depth_desc));
   EMIT(R(0xba, job->aux, 0, DEPTH), V(0xbb, p->aux_desc));
   EMIT(R(0xbe, job->be, 0, STREAM));
   EMIT(V(0xa8, job->depth_control), V(0xa9, 0x08ffff00),
        V(0xaa, 0x00ffff00));
   EMIT(V(0xab, job->color_mask), V(0xac, 0xffffffff), V(0xae, 0x22444442));
   EMIT(V(0xf5, job->clip_min), V(0xf6, job->clip_max), V(0xc4, job->clip_min),
        V(0xc5, job->clip_max));
   EMIT(V(0xf1, 0));
   if (job->fs) {
      ret = m9_emit_raster_fs(batch, job);
      if (ret)
         goto fail;
   } else {
      EMIT(V(0x88, 0x0100dff0), V(0x89, 0xfe207ff8), V(0x8a, 0), V(0xc1, 0));
      EMIT(R(0x00, job->transient, VIEW(0), STREAM),
           R(0x01, job->transient, VIEW(1), STREAM), V(0x10, 0x00100010));
      EMIT(R(0x02, job->transient, VIEW(2), STREAM),
           R(0x03, job->transient, VIEW(3), STREAM), V(0x11, 0x00100006));
      EMIT(R(0xc2, job->program, 0, PROGRAM), V(0xc3, 0), V(0x8f, 0));
      EMIT(V(0x8c, 0x00500200), R(0x8d, job->system, 0, SYSTEM),
           R(0x8e, job->system, 0x10, SYSTEM), V(0x8f, 0));
   }
   ret = m9_emit_units(batch, job->units, job->unit_count);
   if (ret)
      goto fail;
   EMIT(V(0xec, 0), V(0xf4, 0), V(0xea, 0), V(0xeb, 0));
   EMIT(V(0x28, 0));
   if (job->fs) {
      ret = m9_emit_raster_decl(batch, job->fs);
      if (ret)
         goto fail;
   } else {
      /* Flat-form color links serve both shadings; 22 bit 14 selects per draw. */
      EMIT(V(0x23, 0x80100d88), V(0x23, 0x801006b6), V(0x23, 0x00010688),
           V(0x23, 0x00c231b6));
      EMIT(V(0x23, 0x00d30688), V(0x23, 0), V(0x23, 0), V(0x23, 0));
   }
   ret = m9_emit_linked(batch, job);
   if (ret)
      goto fail;
   for (unsigned i = 0; i < job->draw_count; i++) {
      const struct m9_draw_record *d = &job->draws[i];

      /* Later draws restate topology, shading, class and cull, and their
       * texture units whenever the binding moved on. */
      if (i) {
         if (d->unit_count != job->draws[i - 1].unit_count ||
             memcmp(d->units, job->draws[i - 1].units,
                    d->unit_count * sizeof(d->units[0]))) {
            ret = m9_emit_units(batch, d->units, d->unit_count);
            if (ret)
               goto fail;
         }
         EMIT(V(0x22, m9_job_raster_control(job, d)),
              V(0x6a, 0x25010000));
         EMIT(V(0xf0, m9_raster_class(d->lines, job->blend_dest)),
              V(0xef, d->cull | job->depth_ef),
              V(0xd7, m9_raster_d7(d->topology)), V(0xf4, 0));
      }
      ret = d->index_count ?
            m9_batch_emit_draw16(batch, job->index,
                                 job->index_base +
                                 (uint64_t)d->index_first * sizeof(uint16_t),
                                 d->index_count, 0x20000000) :
            m9_batch_emit_draw_numeric(batch, d->start, d->count, 0x20000000);
      if (ret)
         goto fail;
   }
   return 0;
fail:
   m9_batch_rollback(batch, &checkpoint);
   return ret;
}
