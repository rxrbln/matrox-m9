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
#include <string.h>

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

/* Dwords of a single-draw job; each restated raster draw adds
 * M9_JOB_DRAW_DWORDS, which m9_draw.h states. */
#define M9_JOB_BASE_DWORDS 233u

unsigned
m9_draw_job_dwords(unsigned draw_count)
{
   return draw_count ?
          M9_JOB_BASE_DWORDS + M9_JOB_DRAW_DWORDS * (draw_count - 1) : 0;
}

unsigned
m9_draw_decl_dwords(unsigned decl_count)
{
   /* Four words a packet, one header each; the base job counts the
    * pass-through declaration's two packets. */
   return 5 * ((MAX2(decl_count, 1u) + 3) / 4) - 10;
}

unsigned
m9_draw_job_dwords_fs(unsigned draw_count, unsigned unit_count)
{
   /* Streams reach five packets of 19 dwords where the base has 8, and the
    * declaration four packets of five where the base has two.  A bound unit
    * replaces its zero 70+unit packet by 15 dwords, 20 with the
    * placeholder's cleared 82..85. */
   return m9_draw_job_dwords(draw_count) + 21 + 18 * unit_count;
}

/* Relocations a job emits: the two passes bind about thirty objects, every
 * restated unit block one more each, and an indexed draw a start and an end. */
unsigned
m9_draw_job_relocs(unsigned draw_count, unsigned unit_count)
{
   return 30 + unit_count + draw_count * (unit_count + 2);
}

/* A raster pass of a split shader repeats the whole raster range: its state,
 * its units and routes, and every draw of the job. */
unsigned
m9_draw_pass_dwords(unsigned draw_count, unsigned unit_count,
                    unsigned route_count)
{
   /* Three more for the index-30 write a consuming pass carries. */
   return m9_draw_job_dwords_fs(draw_count, unit_count) + 5 * route_count + 3;
}

unsigned
m9_draw_pass_relocs(unsigned draw_count, unsigned unit_count,
                    unsigned route_count)
{
   return m9_draw_job_relocs(draw_count, unit_count) + route_count;
}

unsigned
m9_draw_units_dwords(unsigned unit_count)
{
   /* One restated unit block inside a job: the 74 group, the bound units and
    * the zero 70+unit packets for the units left unbound. */
   return 10 + 18 * unit_count;
}

/* Dependency token a pass starts at; its raster range begins two steps in
 * when the pass preprocesses for itself, and at the same place otherwise. */
static uint32_t
m9_pass_dependency(const struct m9_draw_job *job, unsigned pass)
{
   return pass * (job->preprocess_per_pass ? M9_PASS_DEPENDENCY_FULL :
                                             M9_PASS_DEPENDENCY_STEP);
}

static int m9_emit_preprocess(struct m9_batch *batch,
                              const struct m9_draw_job *job,
                              const uint32_t *decl, unsigned decl_count,
                              uint32_t record_bytes, unsigned const_pairs,
                              uint32_t base);

/* The descriptor-only operands 74..77: a bound route selects the record and
 * names its base and consumer descriptor behind it, an idle one carries the
 * captured 0x100.  Ordering follows the vendor's own five-pass block, which
 * emits 74/78/79 then 75/78/79 (parhelia-m9-five-sampler-schedule-re.md). */
static int
m9_emit_routes(struct m9_batch *batch, const struct m9_draw_job *job,
               const struct m9_raster_pass *pass)
{
   unsigned routes = pass ? pass->route_count : 0;
   int ret = 0;

   for (unsigned k = 0; k < routes; k++) {
      /* A slot no record feeds carries the captured idle word and no
       * descriptor of its own. */
      if (!pass->routes[k].desc) {
         EMIT(V(0x74 + k, 0x100));
         continue;
      }
      EMIT(V(0x74 + k, pass->routes[k].word));
      EMIT(R(0x78, job->record, 0, TEXTURE), V(0x79, pass->routes[k].desc));
   }
   for (unsigned k = routes; k < 4; k++)
      EMIT(V(0x74 + k, 0x100));
fail:
   return ret;
}

static int
m9_emit_units(struct m9_batch *batch, const struct m9_draw_job *job,
              const struct m9_raster_pass *pass,
              const struct m9_texture_unit *units, unsigned unit_count)
{
   int ret = m9_emit_routes(batch, job, pass);
   unsigned bound = 0;

   if (ret)
      return ret;
   if (pass) {
      /* A pass names the application units it samples; a record-writing one
       * still binds the placeholder, whose slot the engine reads. */
      for (unsigned i = 0; i < pass->unit_count; i++) {
         uint8_t sel = pass->unit_index[i];
         const struct m9_texture_unit *u;

         /* A slot a route fills carries no texture of its own. */
         if (sel == M9_PASS_UNIT_NONE) {
            EMIT(V(0x70 + i, 0));
            continue;
         }
         u = sel == M9_PASS_UNIT_PLACEHOLDER ? job->placeholder :
             sel < unit_count ? &units[sel] : NULL;
         if (!u)
            return -EINVAL;
         ret = m9_batch_emit_texture_unit(batch, i, u);
         if (ret)
            return ret;
      }
      bound = pass->unit_count;
   } else {
      for (unsigned i = 0; i < unit_count; i++) {
         ret = m9_batch_emit_texture_unit(batch, i, &units[i]);
         if (ret)
            return ret;
      }
      bound = unit_count;
   }
   for (unsigned i = bound; i < 4; i++)
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
                      const struct m9_raster_pass *pass,
                      const struct m9_draw_record *d)
{
   const struct m9_raster_fs *fs = pass ? &pass->fs : job->fs;

   return fs ? m9_raster_control_decl(d->flat, d->topology, fs->decl_count) :
               m9_raster_control(d->flat, d->topology);
}

/* A view the CPU fills lives in system memory and is fetched through the
 * compact page window; the ones the preprocessing pass renders into stay
 * local.  The engine takes a stream from either address class -- the class
 * is in the address tag, not in the slot -- which is what the vendor's own
 * capture does, binding slot 00 to a system record arena in one job and to
 * the local transient arena in another. */
static bool
m9_stream_is_cpu(const struct m9_draw_job *job, unsigned view)
{
   return view >= M9_TRANSIENT_CPU_VIEW && job->cpu_streams;
}

static uint32_t
m9_stream_handle(const struct m9_draw_job *job, unsigned view)
{
   return m9_stream_is_cpu(job, view) ? job->cpu_streams : job->transient;
}

static uint64_t
m9_stream_offset(const struct m9_draw_job *job, unsigned view)
{
   return m9_stream_is_cpu(job, view) ?
          VIEW(view - M9_TRANSIENT_CPU_VIEW) : VIEW(view);
}

#define STREAM_RELOC(job, slot, view)                                      \
   { .index = (slot), .handle = m9_stream_handle(job, view),               \
     .offset = m9_stream_offset(job, view),                                \
     .kind = m9_stream_is_cpu(job, view) ? PARHELIA_M9_RELOC_SYSTEM :      \
             PARHELIA_M9_RELOC_STREAM }

/* Streams, program entry and constant span of a native raster pass. */
static int
m9_emit_raster_fs(struct m9_batch *batch, const struct m9_draw_job *job,
                  const struct m9_raster_pass *pass)
{
   const struct m9_raster_fs *fs = pass ? &pass->fs : job->fs;
   uint64_t base = pass ? pass->const_offset : M9_RASTER_CONST_OFFSET;
   unsigned entry = pass ? pass->entry : 0;
   int ret = 0;

   EMIT(V(0x88, fs->r88), V(0x89, pass && pass->r89 ? pass->r89 : 0xfe207ff8),
        V(0x8a, 0), V(0xc1, 0));
   for (unsigned pair = 0; 2 * pair < fs->stream_count; pair++) {
      unsigned s = 2 * pair;

      if (s + 1 < fs->stream_count) {
         EMIT(STREAM_RELOC(job, s, fs->stream_view[s]),
              STREAM_RELOC(job, s + 1, fs->stream_view[s + 1]),
              V(0x10 + pair, fs->stride[pair]));
      } else {
         EMIT(STREAM_RELOC(job, s, fs->stream_view[s]),
              V(0x10 + pair, fs->stride[pair]));
      }
   }
   EMIT(R(0xc2, job->program, 0, PROGRAM), V(0xc3, entry), V(0x8f, 0));
   EMIT(V(0x8c, 0x00500000 | fs->const_count << 8),
        R(0x8d, job->system, base, SYSTEM),
        R(0x8e, job->system, base + 8 * fs->const_count, SYSTEM), V(0x8f, 0));
fail:
   return ret;
}

/* Index-23 words in packets of four, the tail padded with zero. */
static int
m9_emit_decl(struct m9_batch *batch, const uint32_t *words, unsigned count)
{
   unsigned padded = (count + 3) & ~3u;
   int ret = 0;

   for (unsigned i = 0; i < padded; i += 4) {
      uint32_t v[4];

      for (unsigned j = 0; j < 4; j++)
         v[j] = i + j < count ? words[i + j] : 0;
      EMIT(V(0x23, v[0]), V(0x23, v[1]), V(0x23, v[2]), V(0x23, v[3]));
   }
fail:
   return ret;
}

/* One raster range: its state, its units and routes, and every draw of the
 * job.  A split shader repeats this per pass, advancing the dependency the
 * vendor advances by 0x20 a pass. */
static int
m9_emit_raster_range(struct m9_batch *batch, const struct m9_draw_job *job,
                     const struct m9_raster_pass *pass, unsigned index,
                     bool last)
{
   const struct m9_draw_params *p = job->params;
   const struct m9_draw_record *d0 = &job->draws[0];
   uint32_t dep = m9_pass_dependency(job, index) + 0x20;
   /* A record-writing pass never blends: the destination it would read is
    * the record, not the application target. */
   bool blend = job->blend_dest && !(pass && pass->writes_record);
   int ret = 0;

   EMIT(V(0xe6, 3), V(0xe3, dep));
   EMIT(V(0xe6, 3));
   EMIT(V(0xe6, 3), V(0xe3, dep + 0x10));
   EMIT(V(0x43, dep + 0x10));
   EMIT(V(0xe6, 0x42000000));
   uint32_t depth_ef = pass ? pass->depth_ef : job->depth_ef;

   EMIT(V(0xf0, m9_raster_class(d0->lines, blend)), V(0xef, d0->cull | depth_ef),
        V(0xd7, m9_raster_d7(d0->topology)));
   EMIT(V(0x22, m9_job_raster_control(job, pass, d0)));
   EMIT(V(0xe6, 3), V(0xe3, dep + 0x20));
   if (pass && pass->writes_record)
      EMIT(R(0xb0, job->record, 0, STREAM), V(0xb1, pass->record_desc));
   else
      EMIT(R(0xb0, job->color, 0, COLOR), V(0xb1, p->color_desc));
   EMIT(R(0xb8, job->depth, 0, DEPTH), V(0xb9, p->depth_desc));
   EMIT(R(0xba, job->aux, 0, DEPTH), V(0xbb, p->aux_desc));
   EMIT(R(0xbe, job->be, 0, STREAM));
   EMIT(V(0xa8, pass ? pass->depth_control : job->depth_control),
        V(0xa9, pass ? pass->a9 : 0x08ffff00u), V(0xaa, 0x00ffff00));
   EMIT(V(0xab, pass && pass->writes_record ? 0xffffffffu : job->color_mask),
        V(0xac, 0xffffffff), V(0xae, pass ? pass->ae : 0x22444442u));
   EMIT(V(0xf5, job->clip_min), V(0xf6, job->clip_max), V(0xc4, job->clip_min),
        V(0xc5, job->clip_max));
   EMIT(V(0xf1, 0));
   if (job->fs || pass) {
      ret = m9_emit_raster_fs(batch, job, pass);
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
   ret = m9_emit_units(batch, job, pass, job->units, job->unit_count);
   if (ret)
      goto fail;
   EMIT(V(0xec, 0), V(0xf4, 0), V(0xea, 0), V(0xeb, 0));
   /* Index 30 is written by a pass that consumes a record, and by no
    * other raster pass (CAPTURED, both schedules). */
   if (index)
      EMIT(V(0x30, 0));
   EMIT(V(0x28, 0));
   if (job->fs || pass) {
      const struct m9_raster_fs *rfs = pass ? &pass->fs : job->fs;

      ret = m9_emit_decl(batch, rfs->decl, rfs->decl_count);
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
         /* A split shader binds one unit pool for the whole job: its passes
          * index that pool, so a draw cannot restate units of its own. */
         if (!job->pass_count &&
             (d->unit_count != job->draws[i - 1].unit_count ||
              memcmp(d->units, job->draws[i - 1].units,
                     d->unit_count * sizeof(d->units[0])))) {
            ret = m9_emit_units(batch, job, pass, d->units, d->unit_count);
            if (ret)
               goto fail;
         }
         EMIT(V(0x22, m9_job_raster_control(job, pass, d)),
              V(0x6a, 0x25010000));
         EMIT(V(0xf0, m9_raster_class(d->lines, blend)),
              V(0xef, d->cull | depth_ef),
              V(0xd7, m9_raster_d7(d->topology)), V(0xf4, 0));
      }
      ret = d->index_count ?
            m9_batch_emit_draw16(batch, job->index,
                                 job->index_base +
                                 (pass ? pass->index_offset : 0) +
                                 (uint64_t)d->index_first * sizeof(uint16_t),
                                 d->index_count, 0x20000000) :
            m9_batch_emit_draw_numeric(batch,
                                       d->start +
                                       (pass ? pass->vertex_offset : 0),
                                       d->count, 0x20000000);
      if (ret)
         goto fail;
   }
   return last ? 0 : m9_batch_emit_range_break(batch);
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
       job->unit_count > (job->pass_count ? M9_DRAW_MAX_UNITS :
                                           M9_TEXTURE_MAX_UNITS) ||
       (job->unit_count && ((!job->fs && !job->pass_count) || !job->units)))
      return false;
   for (unsigned i = 0; i < job->unit_count; i++)
      if (!job->units[i].handle)
         return false;
   if (job->index_count && job->index_base % 4)
      return false;
   if (job->vs_decl_count &&
       (!job->vs_decl || !job->vs_record_bytes || job->vs_record_bytes % 4 ||
        job->vs_decl_count > M9_RASTER_MAX_DECL))
      return false;
   if (job->vs_const_pairs &&
       (job->vs_const_pairs > M9_VS_CONST_MAX_PAIRS ||
        job->vs_const_pairs > 0x7f))
      return false;
   if (job->pass_count) {
      /* Every pass but the last renders into the record, so a split job
       * needs one; the dependency the last pass requests must stay inside
       * the kernel's window. */
      if (!job->passes || job->fs ||
          m9_pass_dependency(job, job->pass_count) + 0x40 >
          PARHELIA_M9_DEPENDENCY_LIMIT)
         return false;
      for (unsigned i = 0; i < job->pass_count; i++) {
         const struct m9_raster_pass *pass = &job->passes[i];

         if ((pass->writes_record && !job->record) ||
             (pass->writes_record && (!job->record || !pass->record_desc)) ||
             /* The descriptor-only operands 74..77 are their own bank: the
              * five-sampler schedule's final pass binds three of them
              * alongside full slot zero, so they do not share the four. */
             pass->route_count > M9_TEXTURE_MAX_UNITS ||
             pass->unit_count > M9_TEXTURE_MAX_UNITS)
            return false;
         if (pass->route_count && !job->record)
            return false;
         for (unsigned k = 0; k < pass->unit_count; k++) {
            uint8_t sel = pass->unit_index[k];

            if (sel == M9_PASS_UNIT_PLACEHOLDER) {
               if (!job->placeholder || !job->placeholder->handle)
                  return false;
            } else if (sel != M9_PASS_UNIT_NONE && sel >= job->unit_count) {
               return false;
            }
         }
      }
   }
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
   uint32_t pass_decl[5], record_bytes;
   const uint32_t *decl;
   unsigned decl_count, const_pairs;
   int ret;

   if (!batch || !job || batch->sealed || !m9_draw_job_valid(job))
      return -EINVAL;
   p = job->params;
   d0 = &job->draws[0];
   if (job->vs_decl_count) {
      decl = job->vs_decl;
      decl_count = job->vs_decl_count;
      record_bytes = job->vs_record_bytes;
   } else {
      memcpy(pass_decl, job->decl->words, sizeof(job->decl->words));
      pass_decl[4] = M9_DECL_END;
      decl = pass_decl;
      decl_count = ARRAY_SIZE(pass_decl);
      record_bytes = job->decl->record_bytes;
   }
   const_pairs = job->vs_const_pairs ? job->vs_const_pairs :
                                       M9_VS_CONST_PAIRS;
   m9_batch_checkpoint(batch, &checkpoint);
   for (unsigned pi = 0, n = MAX2(job->pass_count, 1u); pi < n; pi++) {
      /* The raster stage drains what preprocessing produced, so a pass
       * that runs again preprocesses again; a single-pass job emits
       * exactly the captured stream. */
      /* A range break separates a raster pass from the preprocessing range
       * of the next one; passes that share a preprocessing range follow
       * each other inside a single raster range, as the vendor's own
       * five-pass schedule does. */
      bool brk = job->preprocess_per_pass && pi + 1 < n;

      if (!pi || job->preprocess_per_pass) {
         ret = m9_emit_preprocess(batch, job, decl, decl_count, record_bytes,
                                  const_pairs, m9_pass_dependency(job, pi));
         if (ret)
            goto fail;
      }
      ret = m9_emit_raster_range(batch, job,
                                 job->pass_count ? &job->passes[pi] : NULL,
                                 pi, !brk);
      if (ret)
         goto fail;
   }
   return 0;
fail:
   m9_batch_rollback(batch, &checkpoint);
   return ret;
}

/* Preprocessing pass: two dependency steps from base. */
static int
m9_emit_preprocess(struct m9_batch *batch, const struct m9_draw_job *job,
                   const uint32_t *decl, unsigned decl_count,
                   uint32_t record_bytes, unsigned const_pairs, uint32_t base)
{
   int ret = 0;

   EMIT(V(0xe6, 3), V(0xe3, base));
   EMIT(V(0xe6, 3));
   EMIT(V(0xf0, 0xf0000400), V(0xef, 0x00288800), V(0xd7, 0x000050ae));
   EMIT(V(0x22, m9_preprocess_control(decl_count)));
   EMIT(V(0xe6, 3), V(0xe3, base + 0x10));
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
   /* Bits 27:24 are the unary count of declared source attributes; the
    * constant two of the captured pass-through made a third attribute read
    * a slot the pass never declared (MEASURED, a clean A/B on the M9128,
    * docs/parhelia-m9-vertex-output-target-re.md).  A declaration holds two
    * words an attribute plus the terminator. */
   EMIT(V(0x88, (BITFIELD_MASK((decl_count - 1) / 2) << 24) | 0x0000bf00u),
        V(0x89, 0xfe201e92), V(0x8a, 0), V(0xc1, 0));
   EMIT(R(0x00, job->source, 0, SYSTEM), V(0x01, 0xf2fe014f),
        V(0x10, record_bytes));
   /* The vertex program of a split shader sits behind every fragment
    * pass, so its entry comes from the passes when fs is unset. */
   EMIT(R(0xc2, job->program, 0, PROGRAM),
        V(0xc3, job->fs ? job->fs->vertex_entry :
                job->pass_count ? job->passes[0].fs.vertex_entry : 4),
        V(0x8f, 0));
   EMIT(V(0x8c, 0x00500000 | const_pairs << 8),
        R(0x8d, job->system, M9_VS_CONST_OFFSET, SYSTEM),
        R(0x8e, job->system, M9_VS_CONST_OFFSET + 8 * const_pairs, SYSTEM),
        V(0x8f, 0));
   ret = m9_emit_units(batch, job, NULL, NULL, 0);
   if (ret)
      goto fail;
   EMIT(V(0xec, 0), V(0xf4, 0x00010004), V(0xea, 0), V(0xeb, 0x46000000));
   EMIT(V(0x30, 0));
   EMIT(V(0x28, 0));
   ret = m9_emit_decl(batch, decl, decl_count);
   if (ret)
      goto fail;
   ret = m9_emit_linked(batch, job);
   if (ret)
      goto fail;
   EMIT(V(0x19, 0), V(0x18, 0x7fffffff));
   /* One covering preprocessing draw serves every raster draw. */
   ret = m9_batch_emit_draw_numeric(batch, 0, job->source_vertices,
                                    0x20000000);
   if (!ret)
      ret = m9_batch_emit_range_break(batch);
fail:
   return ret;
}
