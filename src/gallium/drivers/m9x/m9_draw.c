/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Two-pass job: the preprocessing pass runs the captured pass-through
 * program over the source records into four transient streams, the raster
 * pass consumes them into the render target with one or more draws.
 * Size and state dependent words come from m9_draw_params.
 */
#include "m9_draw.h"
#include "m9_faa.h"

#include <errno.h>
#include <string.h>

#include "util/macros.h"
#include "util/u_debug.h"

#define V(i, v) { .index = (i), .value = (v) }
#define R(i, h, o, k) { .index = (i), .handle = (h), .offset = (o), \
                        .kind = PARHELIA_M9_RELOC_##k }
#define VIEW(n) ((uint64_t)(n) * m9_transient_view_bytes())
#define EMIT(...) do { \
   const struct m9_packet_value packet_[] = { __VA_ARGS__ }; \
   ret = m9_batch_emit_packet(batch, packet_, ARRAY_SIZE(packet_)); \
   if (ret) \
      goto fail; \
} while (0)

/* Dwords of a single-draw job; each restated raster draw adds
 * M9_JOB_DRAW_DWORDS, which m9_draw.h states. */
#define M9_JOB_BASE_DWORDS 233u
/* The second preprocessing draw without its stream targets and its
 * declaration, which m9_draw_vs_extra_dwords adds. */
#define M9_JOB_EXTRA_DWORDS 95u

unsigned
m9_draw_job_dwords(unsigned draw_count)
{
   return draw_count ?
          M9_JOB_BASE_DWORDS + M9_JOB_DRAW_DWORDS * (draw_count - 1) : 0;
}

/* Upper bound of the kernel's framed size of a job: a restated draw is two
 * packets and a draw header, and every other packet holds at least one value. */
unsigned
m9_draw_framed_dwords(unsigned dwords, unsigned restated_draws)
{
   unsigned draw = M9_JOB_DRAW_DWORDS * restated_draws;
   unsigned rest = dwords > draw ? dwords - draw : 0;

   return dwords + 2 * restated_draws + (rest + 1) / 2;
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
   /* Three more for the index-30 write a consuming pass carries, and up to
    * fourteen a draw for the engine transition around it: an eight-dword
    * epilogue and a six-dword prologue. */
   return m9_draw_job_dwords_fs(draw_count, unit_count) + 5 * route_count + 3 +
          14 * draw_count;
}

unsigned
m9_draw_pass_relocs(unsigned draw_count, unsigned unit_count,
                    unsigned route_count)
{
   return m9_draw_job_relocs(draw_count, unit_count) + route_count;
}

/* A colour attachment past the first costs one address/descriptor packet in
 * every raster pass; a job with no explicit pass count still has one. */
unsigned
m9_draw_color_dwords(unsigned color_count, unsigned pass_count)
{
   unsigned extra = color_count > 1 ? color_count - 1 : 0;

   return extra * M9_DRAW_COLOR_DWORDS * MAX2(pass_count, 1u);
}

unsigned
m9_draw_color_relocs(unsigned color_count, unsigned pass_count)
{
   unsigned extra = color_count > 1 ? color_count - 1 : 0;

   return extra * MAX2(pass_count, 1u);
}

/* Five three-bit fields; the bound targets fill them from bit 0 and every
 * field a target does not claim reads 7.  The fifth is 7 in every captured
 * value, one to four targets alike. */
uint32_t
m9_raster_89(uint32_t base, const uint8_t *color_class, unsigned color_count)
{
   uint32_t word = base & ~0x7fffu;

   if (!color_count)
      return base;
   for (unsigned s = 0; s < 5; s++) {
      unsigned cls = s < color_count && s < M9_DRAW_MAX_COLOR ?
                     color_class[s] & 7u : 7u;

      word |= cls << (3 * s);
   }
   return word;
}

uint32_t
m9_color_mask_pair(const uint32_t *mask, unsigned count, unsigned first)
{
   uint32_t low = first < count ? mask[first] & 0xffffu : 0xffffu;
   uint32_t high = first + 1 < count ? mask[first + 1] & 0xffffu : 0xffffu;

   return low | high << 16;
}

unsigned
m9_draw_units_dwords(unsigned unit_count)
{
   /* One restated unit block inside a job: the 74 group, the bound units and
    * the zero 70+unit packets for the units left unbound. */
   return 10 + 18 * unit_count;
}

/* Dependency steps the second preprocessing draw claims: index e3 advances
 * by 0x10 on every write, and that draw's range writes it twice, exactly as
 * the fused one does. */
static uint32_t
m9_extra_dependency(const struct m9_draw_job *job)
{
   if (!job->vs_extra_entry)
      return 0;
   return job->vs_gather_entry ? 0x40u : 0x20u;
}

/* Dependency token a pass starts at; its raster range begins two steps in
 * when the pass preprocesses for itself, and at the same place otherwise. */
static uint32_t
m9_pass_dependency(const struct m9_draw_job *job, unsigned pass)
{
   return pass * (job->preprocess_per_pass ?
                  M9_PASS_DEPENDENCY_FULL + m9_extra_dependency(job) :
                  M9_PASS_DEPENDENCY_STEP);
}

/* One step past the last dependency a job's ranges raise: what a record
 * emitted behind the job has to wait for. */
uint32_t
m9_draw_job_final_dependency(const struct m9_draw_job *job)
{
   unsigned last = job->pass_count ? job->pass_count - 1 : 0;

   return m9_pass_dependency(job, last) + m9_extra_dependency(job) + 0x50;
}

/* The three shapes a preprocessing draw of one job takes. */
enum m9_pre_kind {
   M9_PRE_HEAD,     /* the tail-free head, writes the varyings it carries */
   M9_PRE_GATHER,   /* route-fed, de-interleaves the head's record */
   M9_PRE_FUSED,    /* the job's own pass, with the appended tail */
};

static int m9_emit_preprocess(struct m9_batch *batch,
                              const struct m9_draw_job *job,
                              const uint32_t *decl, unsigned decl_count,
                              uint32_t record_bytes, unsigned const_pairs,
                              uint32_t base, enum m9_pre_kind kind);

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
         /* A unit the record steers names its route in its own selector
          * word rather than reading an interpolated stream: low byte
          * 0x1c + k and bit 12, against 0x18 for an interpolated
          * coordinate (CAPTURED, the five-sampler schedule's 0004111c and
          * xv-uyvy's 0004111d/0004111e). */
         if (i < pass->route_count &&
             (pass->routes[i].word & 0xffu) == 0x10u + 4 * i) {
            struct m9_texture_unit routed = *u;

            routed.w70 = (routed.w70 & ~0xffu) | 1u << 12 | (0x1cu + i);
            ret = m9_batch_emit_texture_unit(batch, i, &routed);
         } else {
            ret = m9_batch_emit_texture_unit(batch, i, u);
         }
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
m9_stream_offset(const struct m9_draw_job *job,
                 const struct m9_raster_pass *pass, unsigned view)
{
   return m9_stream_is_cpu(job, view) ?
          VIEW(view - M9_TRANSIENT_CPU_VIEW) +
          (pass ? pass->cpu_stream_offset : 0) : VIEW(view);
}

#define STREAM_RELOC(job, pass, slot, view)                                \
   { .index = (slot), .handle = m9_stream_handle(job, view),               \
     .offset = m9_stream_offset(job, pass, view),                          \
     .kind = m9_stream_is_cpu(job, view) ? PARHELIA_M9_RELOC_SYSTEM :      \
             PARHELIA_M9_RELOC_STREAM }

/* Raster ef enables every colour target past the first, one bit every two
 * from bit 19 (CAPTURED: 0, 0x00080000, 0x00280000 and 0x00a80000 for one,
 * two, three and four attachments).  Target 0 is always enabled and claims
 * no bit.  docs/parhelia-m9-mrt-routing.md */
/*
 * CAPTURED (docs/parhelia-m9-gl15-ff-capture.md): ef[7:0] is 0x55 with
 * dithering off and 0x00 with it on.  MEASURED (fixture giga10clear): with
 * dithering on a 10-bit target spreads a colour that falls between two
 * levels over both of them -- green 682/1023 came back 681 on half the
 * surface and 682 on the other half, and the two-bit alpha likewise.  Ten
 * bits already resolve what dithering exists to hide, so a GigaColor target
 * takes the off word, as the vendor's own Parhelia path does.
 */
/* Index ae with this job's AA mode edited in; an inactive job keeps the
 * captured word. */
static uint32_t
m9_job_ae(const struct m9_draw_job *job, uint32_t base)
{
   return job->faa.set ? (base & job->faa.edit.ae_and) | job->faa.edit.ae_or : base;
}

static uint32_t
m9_job_ef(const struct m9_draw_job *job, uint32_t base)
{
   return job->faa.set ? (base & job->faa.edit.ef_and) | job->faa.edit.ef_or : base;
}

static uint32_t
m9_job_f0(const struct m9_draw_job *job, uint32_t base)
{
   return job->faa.set ? (base & job->faa.edit.f0_and) | job->faa.edit.f0_or : base;
}

static uint32_t
m9_dither_ef(uint32_t color_desc)
{
   uint8_t code = color_desc >> 25;

   return code == M9_COLOR_FORMAT_A2B10G10R10 ||
          code == M9_COLOR_FORMAT_A2R10G10B10 ? 0x55u : 0;
}

static uint32_t
m9_color_enable(unsigned color_count)
{
   uint32_t ef = 0;

   for (unsigned s = 1; s < color_count; s++)
      ef |= 0x00080000u << (2 * (s - 1));
   return ef;
}

/* Raster 89 of one pass: a record-writing pass keeps the word it carries,
 * and a colour pass packs one render format class per bound target. */
static uint32_t
m9_job_r89(const struct m9_draw_job *job, const struct m9_raster_pass *pass,
           uint32_t base)
{
   if ((pass && pass->writes_record) || job->color_count < 2)
      return base;
   return m9_raster_89(base, job->color_class, job->color_count);
}

/* Streams, program entry and constant span of a native raster pass. */
static int
m9_emit_raster_fs(struct m9_batch *batch, const struct m9_draw_job *job,
                  const struct m9_raster_pass *pass)
{
   const struct m9_raster_fs *fs = pass ? &pass->fs : job->fs;
   uint64_t base = pass ? pass->const_offset : M9_RASTER_CONST_OFFSET;
   unsigned entry = pass ? pass->entry : 0;
   int ret = 0;

   EMIT(V(0x88, fs->r88),
        V(0x89, m9_job_r89(job, pass, pass && pass->r89 ? pass->r89 :
                                      fs->r89 ? fs->r89 : 0xfe207ff8)),
        V(0x8a, 0), V(0xc1, 0));
   for (unsigned pair = 0; 2 * pair < fs->stream_count; pair++) {
      unsigned s = 2 * pair;

      if (s + 1 < fs->stream_count) {
         EMIT(STREAM_RELOC(job, pass, s, fs->stream_view[s]),
              STREAM_RELOC(job, pass, s + 1, fs->stream_view[s + 1]),
              V(0x10 + pair, fs->stride[pair]));
      } else {
         EMIT(STREAM_RELOC(job, pass, s, fs->stream_view[s]),
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

/*
 * The engine transition the vendor puts around every draw of a native
 * fragment job (CAPTURED, 343 prologues and 230 epilogues over
 * captures/m9x-vendor-glsl-programs-20260915 and
 * captures/m9x-vendor-openbits-20260917; decoded in
 * docs/parhelia-m9-fragment-pass-splitting.md section 6).  The epilogue
 * closes the draw that has ended, the prologue opens the block of the next.
 */
#define M9_TRANSITION_END      0x38000000u  /* end the block that has drawn */
#define M9_TRANSITION_PUBLISH  0x40004000u  /* e6 and both 44 of the pair */
#define M9_TRANSITION_RECORD   0x0c000000u  /* only behind a record writer */
#define M9_TRANSITION_OPEN     0x60600060u  /* a range's first block */
#define M9_TRANSITION_RESUME   0x604fff9fu  /* a continuation block */
#define M9_TRANSITION_FIRST    0x2c000002u  /* second word of the first */
#define M9_TRANSITION_NEXT     0x2c000000u  /* second word of a continuation */
#define M9_TRANSITION_LENGTH   0x49d00000u  /* | the block length in dwords */
#define M9_TRANSITION_START    0x08000000u  /* start the block */

/* Close the block a draw has just ended.  The record word follows a draw
 * that wrote the inter-pass record and no other (CAPTURED: present behind
 * every one of the eight ee190004 producers, absent behind every 68002000
 * stream target). */
static int
m9_emit_transition_end(struct m9_batch *batch, bool record)
{
   int ret = 0;

   EMIT(V(0xe6, M9_TRANSITION_END));
   EMIT(V(0xe6, M9_TRANSITION_PUBLISH), V(0x44, M9_TRANSITION_PUBLISH),
        V(0x44, M9_TRANSITION_PUBLISH));
   if (record)
      EMIT(V(0xe6, M9_TRANSITION_RECORD));
fail:
   return ret;
}

/* Open the block of the draw about to be emitted, and hand back where its
 * length word sits so m9_transition_length can fill it in. */
static int
m9_emit_transition_start(struct m9_batch *batch, bool first, uint32_t *at)
{
   int ret = 0;

   *at = batch->command_count + 3;
   EMIT(V(0xe6, first ? M9_TRANSITION_OPEN : M9_TRANSITION_RESUME),
        V(0xe6, first ? M9_TRANSITION_FIRST : M9_TRANSITION_NEXT),
        V(0xe6, M9_TRANSITION_LENGTH));
   EMIT(V(0xe6, M9_TRANSITION_START));
fail:
   return ret;
}

/*
 * The prologue's length word counts the dwords of the block it opens, from
 * the prologue's own packet header through the draw packet that ends it,
 * less one; a draw that closes its range stops at the draw packet's header
 * instead, because the range close bounds it (DERIVED, and exact for all
 * 343 captured prologues -- tools/m9-command-decode.py over both capture
 * sets).  Captured lengths run 0x80..0xcc with bits 15:8 clear, so the
 * field is read as the low sixteen bits.
 */
static void
m9_transition_length(struct m9_batch *batch, uint32_t at, uint32_t draw_at,
                     bool more)
{
   uint32_t length = draw_at - (at - 3) +
                     (more ? batch->command_count - draw_at - 1 : 0);

   batch->command[at] = M9_TRANSITION_LENGTH | (length & 0xffffu);
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
   uint32_t dep = m9_pass_dependency(job, index) + m9_extra_dependency(job) +
                  0x20;
   /* A record-writing pass never blends: the destination it would read is
    * the record, not the application target. */
   bool blend = job->blend_dest && !(pass && pass->writes_record);
   bool logic = blend && job->logic_op;
   /* Passes that share a preprocessing range follow each other inside one
    * raster range, so only the first of them opens that range. */
   bool opens = !index || job->preprocess_per_pass;
   uint32_t block = 0;
   int ret = 0;

   /* A continuation pass closes the pass before it, which wrote the record
    * whenever that pass was not the last. */
   if (job->transition && !opens) {
      ret = m9_emit_transition_end(batch, job->passes[index - 1].writes_record);
      if (ret)
         goto fail;
   }
   if (job->transition) {
      ret = m9_emit_transition_start(batch, opens, &block);
      if (ret)
         goto fail;
   }
   EMIT(V(0xe6, 3), V(0xe3, dep));
   /* The vendor opens a range with two dependency steps and the 0x42000000
    * control, and a continuation block inside it with one step and neither
    * (CAPTURED, both raster draws of all seven two-pass cases). */
   if (opens) {
      EMIT(V(0xe6, 3));
      EMIT(V(0xe6, 3), V(0xe3, dep + 0x10));
   }
   EMIT(V(0x43, dep + 0x10));
   if (opens)
      EMIT(V(0xe6, 0x42000000));
   uint32_t depth_ef = pass ? pass->depth_ef : job->depth_ef;

   EMIT(V(0xf0, m9_job_f0(job, m9_raster_class(d0->lines, blend))),
        V(0xef, m9_job_ef(job,
                d0->cull | depth_ef | (pass ? pass->sequence_ef : 0) |
                /* ef[7:0] is 0xaa under a logic op (CAPTURED, logic_*) */
                (logic ? 0xaau : m9_dither_ef(p->color_desc)) |
                (pass && pass->writes_record ? 0 :
                 m9_color_enable(job->color_count)))),
        V(0xd7, m9_raster_d7(d0->topology)));
   EMIT(V(0x22, m9_job_raster_control(job, pass, d0)));
   EMIT(V(0xe6, 3), V(0xe3, dep + 0x20));
   if (pass && pass->writes_record) {
      EMIT(R(0xb0, job->record, 0, STREAM), V(0xb1, pass->record_desc));
   } else {
      EMIT(R(0xb0, job->color, job->color_offset, COLOR),
           V(0xb1, p->color_desc));
      /* Target s takes the pair at b0 + 2s with its own descriptor
       * (CAPTURED, one to four attachments). */
      for (unsigned s = 1; s < job->color_count; s++)
         EMIT(R(0xb0 + 2 * s, job->color_extra[s - 1], 0, COLOR),
              V(0xb1 + 2 * s, job->color_extra_desc[s - 1]));
   }
   if (job->faa.set && job->faa.companion)
      EMIT(R(0xb8, job->faa.companion, 0, DEPTH),
           V(0xb9, job->faa.companion_desc));
   else
      EMIT(R(0xb8, job->depth, 0, DEPTH), V(0xb9, p->depth_desc));
   EMIT(R(0xba, job->aux, 0, DEPTH), V(0xbb, p->aux_desc));
   EMIT(R(0xbe, job->be, 0, STREAM));
   EMIT(V(0xa8, pass ? pass->depth_control : job->depth_control),
        V(0xa9, pass ? pass->a9 :
                job->stencil ? job->stencil_a9 : M9_STENCIL_A9_OFF),
        V(0xaa, pass ? pass->aa :
                job->stencil ? job->stencil_aa : M9_STENCIL_AA_OFF));
   /* ab carries targets 0 and 1, ac targets 2 and 3 (CAPTURED: one target
    * leaves ab's upper half stale, four write ffffffff in both). */
   EMIT(V(0xab, pass && pass->writes_record ? 0xffffffffu : job->color_mask),
        V(0xac, pass && pass->writes_record ? 0xffffffffu :
                job->color_count > 2 ? job->color_mask_hi : 0xffffffffu),
        V(0xae, m9_job_ae(job, pass ? pass->ae : 0x22444442u)));
   /* RESET_FRAG_PTR, byte for byte what the vendor emits: e6 = 3 twice, then
    * a1 = a3 = a4 = 0 (STATIC, MxMD.dll 0xfabc7).  No address goes to any of
    * a0..a6 in any vendor image, so this is a control, not a binding. */
   if (job->faa.set && job->faa.bind_reset) {
      EMIT(V(0xe6, 3), V(0xe6, 3));
      EMIT(V(0xa1, 0), V(0xa3, 0), V(0xa4, 0));
   }
   /* The store, bound as the vendor's emitter binds it. */
   if (job->faa.set && job->faa.store_bind && job->faa.store &&
       job->faa.store_aux)
      EMIT(R(0xbc, job->faa.store, 0, FRAGMENT),
           R(0xbd, job->faa.store_aux, 0, FRAGMENT));
   /* The store's address at whatever index a measurement names, for the
    * search that found bc/bd and for the ones still open. */
   if (job->faa.set && job->faa.bind_index >= 0 && job->faa.store) {
      if (job->faa.bind_desc >= 0)
         EMIT(R((uint8_t)job->faa.bind_index, job->faa.store, 0, FRAGMENT),
              V((uint8_t)(job->faa.bind_index + 1),
                (uint32_t)job->faa.bind_desc));
      else
         EMIT(R((uint8_t)job->faa.bind_index, job->faa.store, 0, FRAGMENT));
   }
   /* The captured stream carries the multisample pair in preprocessing
    * alone; restating it here tests whether the raster range needs it. */
   if (job->faa.set && job->faa.raster) {
      EMIT(V(0xad, job->faa.ad), V(0xad, job->faa.ad), V(0xad, job->faa.ad),
           V(0xad, job->faa.ad));
      EMIT(V(0xaf, job->faa.af));
   }
   EMIT(V(0xf5, job->clip_min), V(0xf6, job->clip_max), V(0xc4, job->clip_min),
        V(0xc5, job->clip_max));
   EMIT(V(0xf1, 0));
   if (job->fs || pass) {
      ret = m9_emit_raster_fs(batch, job, pass);
      if (ret)
         goto fail;
   } else {
      EMIT(V(0x88, 0x0100dff0), V(0x89, m9_job_r89(job, pass, 0xfe207ff8)),
           V(0x8a, 0), V(0xc1, 0));
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
   if (job->split_30 && pass ? pass->pass_index != 0 : index != 0)
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
      uint32_t draw_at;

      /* The vendor's transition sits around every draw, not only around a
       * pass, so a restated draw closes the block before it and opens its
       * own (CAPTURED, three draws of one preprocessing range). */
      if (job->transition && i) {
         ret = m9_emit_transition_end(batch, pass && pass->writes_record);
         if (ret)
            goto fail;
         ret = m9_emit_transition_start(batch, false, &block);
         if (ret)
            goto fail;
      }
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
              V(0xef, d->cull | depth_ef |
                      (logic ? 0xaau : m9_dither_ef(p->color_desc))),
              V(0xd7, m9_raster_d7(d->topology)), V(0xf4, 0));
      }
      draw_at = batch->command_count;
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
      /* A block is the last of its range when no draw and no further pass
       * follows it inside that range. */
      if (job->transition)
         m9_transition_length(batch, block, draw_at,
                              i + 1 < job->draw_count ||
                              (!job->preprocess_per_pass &&
                               index + 1 < job->pass_count));
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
       job->color_count > M9_DRAW_MAX_COLOR ||
       !job->draws || !job->draw_count || job->source_vertices % 4 ||
       job->source_vertices > m9_draw_max_source_vertices() ||
       job->unit_count > (job->pass_count ? M9_DRAW_MAX_UNITS :
                                           M9_TEXTURE_MAX_UNITS) ||
       (job->unit_count && ((!job->fs && !job->pass_count) || !job->units)))
      return false;
   for (unsigned i = 0; i < job->unit_count; i++)
      if (!job->units[i].handle)
         return false;
   for (unsigned s = 1; s < job->color_count; s++)
      if (!job->color_extra[s - 1])
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
   /* The second preprocessing draw runs the same declaration and the same
    * source record, so it only stands with a native vertex program. */
   if (job->vs_extra_entry &&
       (!job->vs_decl_count || !job->vs_extra_targets ||
        job->vs_extra_targets > M9_VS_EXTRA_VARYINGS))
      return false;
   for (unsigned t = 0; t < job->vs_extra_targets; t++)
      if (job->vs_extra_view[t] >= M9_TRANSIENT_VIEWS)
         return false;
   /* View 0 is target 0's, so it is the sentinel for an unbound target 3. */
   if (job->vs_fused_view &&
       (!job->vs_decl_count || job->vs_fused_view >= M9_TRANSIENT_VIEWS))
      return false;
   /* The packed record spans one view a varying, above the views the
    * gather de-interleaves into. */
   if (job->vs_gather_entry &&
       (!job->vs_extra_entry ||
        job->vs_record_view < M9_TRANSIENT_VS_RECORD_VIEW ||
        job->vs_record_view + job->vs_extra_targets > M9_TRANSIENT_VIEWS))
      return false;
   if (job->pass_count) {
      /* Every pass but the last renders into the record, so a split job
       * needs one; the dependency the last pass requests must stay inside
       * the kernel's window. */
      if (!job->passes || job->fs ||
          m9_pass_dependency(job, job->pass_count) +
          m9_extra_dependency(job) + 0x40 > PARHELIA_M9_DEPENDENCY_LIMIT)
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
         uint32_t base = m9_pass_dependency(job, pi);

         /* Varyings 1..4 come from a draw of their own, ahead of the fused
          * pass: the appended tail owns three of the four output targets
          * and scratches the register behind the fourth. */
         if (job->vs_extra_entry) {
            ret = m9_emit_preprocess(batch, job, decl, decl_count,
                                     record_bytes, const_pairs, base,
                                     M9_PRE_HEAD);
            if (!ret && job->vs_gather_entry)
               ret = m9_emit_preprocess(batch, job, decl, decl_count,
                                        record_bytes, const_pairs,
                                        base + 0x20, M9_PRE_GATHER);
            if (ret)
               goto fail;
         }
         ret = m9_emit_preprocess(batch, job, decl, decl_count, record_bytes,
                                  const_pairs,
                                  base + m9_extra_dependency(job),
                                  M9_PRE_FUSED);
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

/* Index ef of a preprocessing pass.  Bits 11:9 state the draw's place in
 * the preprocessing chain and bit 15 enables the clip-code output, and the
 * two move together: the only draw of a chain is 4, the first of several 1,
 * a later source-record-fed one 2, a route-fed one 3, and the last of
 * several 7, with bit 15 set exactly for the values that carry bit 11.
 * That holds in 628 of 628 captured preprocessing passes across six capture
 * sets with no exception (docs/parhelia-m9-varying-transport-re.md
 * section 4).  Bits 19/21/23 enable targets 1..3; target 0 is always on. */
#define M9_PRE_ROLE_FIRST  0x00000200u
#define M9_PRE_ROLE_MID    0x00000600u
#define M9_PRE_ROLE_ONLY   0x00000800u
#define M9_PRE_ROLE_LAST   0x00000e00u
#define M9_PRE_CLIP_CODE   0x00008000u

static uint32_t
m9_preprocess_ef(unsigned targets, uint32_t role)
{
   uint32_t ef = role;

   for (unsigned t = 1; t < targets; t++)
      ef |= 0x00080000u << (2 * (t - 1));
   return ef;
}

unsigned
m9_draw_vs_extra_dwords(unsigned target_count, unsigned decl_count)
{
   /* Each draw repeats the preprocessing block without its 0xb8 clip-code
    * pair, with one address/descriptor packet a target; the gather adds a
    * four-dword route packet a target on top of its own block. */
   unsigned one = M9_JOB_EXTRA_DWORDS + 3 * target_count +
                  m9_draw_decl_dwords(decl_count) + 10;

   return 2 * one + 4 * target_count;
}

unsigned
m9_draw_vs_extra_relocs(unsigned target_count)
{
   /* One a bound target, plus the source record, the program, the two
    * constant-span ends, the fragment database and the linked record --
    * for the head and for the gather, and the gather names the record once
    * more behind every route. */
   return 2 * (target_count + 6) + target_count;
}

/* Preprocessing pass: two dependency steps from base.  The extra form runs
 * the tail-free head, binding one stream target a varying and no clip code;
 * everything else -- the declaration, the source record, the constant span
 * and the linked record -- is the fused pass's own. */
static int
m9_emit_preprocess(struct m9_batch *batch, const struct m9_draw_job *job,
                   const uint32_t *decl, unsigned decl_count,
                   uint32_t record_bytes, unsigned const_pairs, uint32_t base,
                   enum m9_pre_kind kind)
{
   bool extra = kind != M9_PRE_FUSED;
   bool packed = m9_vs_record_mode() >= 2 && job->vs_gather_entry;
   unsigned targets = extra ? job->vs_extra_targets : 0;
   int ret = 0;

   EMIT(V(0xe6, 3), V(0xe3, base));
   EMIT(V(0xe6, 3));
   /* The AA mode edit belongs to the raster range: it is lowered from the
    * surface shadow, and a preprocessing pass binds no render target. */
   EMIT(V(0xf0, 0xf0000400),
        V(0xef, kind == M9_PRE_HEAD ?
                   m9_preprocess_ef(targets, M9_PRE_ROLE_FIRST) :
                kind == M9_PRE_GATHER ?
                   m9_preprocess_ef(targets, M9_PRE_ROLE_MID) :
                        m9_preprocess_ef(3, job->vs_extra_entry ?
                                         M9_PRE_ROLE_LAST :
                                         M9_PRE_ROLE_ONLY) |
                        M9_PRE_CLIP_CODE),
        V(0xd7, 0x000050ae));
   EMIT(V(0x22, m9_preprocess_control(decl_count)));
   EMIT(V(0xe6, 3), V(0xe3, base + 0x10));
   if (kind == M9_PRE_HEAD) {
      /* The captured chain writes one packed record, every target of the
       * draw at its own dword offset of the same object. */
      for (unsigned t = 0; t < targets; t++)
         EMIT(R(0xb0 + 2 * t, job->transient,
                VIEW(packed ? job->vs_record_view : job->vs_extra_view[t]),
                STREAM),
              V(0xb1 + 2 * t, m9_vs_extra_desc(t, targets)));
   } else if (kind == M9_PRE_GATHER) {
      for (unsigned t = 0; t < targets; t++)
         EMIT(R(0xb0 + 2 * t, job->transient, VIEW(job->vs_extra_view[t]),
                STREAM),
              V(0xb1 + 2 * t, 0x68002000));
   } else {
      /* Output target 1 carries the second varying instead of the clip
       * position, which the raster stage then does not declare; every
       * other word of the pass is the three-target shape's own. */
      EMIT(R(0xb0, job->transient, VIEW(0), STREAM), V(0xb1, 0x68002000));
      EMIT(R(0xb2, job->transient,
             VIEW(job->vs_fused_view ? job->vs_fused_view : 3u), STREAM),
           V(0xb3, 0x68002000));
      EMIT(R(0xb4, job->transient, VIEW(1), STREAM), V(0xb5, 0x68002000));
      EMIT(R(0xb8, job->transient, VIEW(2), STREAM), V(0xb9, 0xb8002000));
      EMIT(V(0xba, 0x00000020), V(0xbb, 0xf0010040));
   }
   EMIT(V(0xbe, 3));
   uint32_t faa_ad = job->faa.set ? job->faa.ad : M9_FAA_AD_OFF;

   EMIT(V(0xad, faa_ad), V(0xad, faa_ad), V(0xad, faa_ad), V(0xad, faa_ad));
   EMIT(V(0xaf, job->faa.set ? job->faa.af : M9_FAA_AF_OFF));
   EMIT(V(0xa8, 0x1f), V(0xa9, 0), V(0xaa, 0));
   EMIT(V(0xab, 0xffffffff), V(0xac, 0xffffffff),
        V(0xae, m9_job_ae(job, 0x22444442u)));
   EMIT(V(0xf5, 0), V(0xf6, 0x7fff7fff), V(0xc4, 0), V(0xc5, 0x7fff7fff));
   /* Bits 27:24 are the unary count of declared source attributes; the
    * constant two of the captured pass-through made a third attribute read
    * a slot the pass never declared (MEASURED, a clean A/B on the M9128,
    * docs/parhelia-m9-vertex-output-target-re.md).  A declaration holds two
    * words an attribute plus the terminator.
    *
    * Bits 15:0 are a register-pair mask, and the fused pass's captured
    * 0xbf00 leaves pair 14 -- registers 0x1c/0x1d -- clear, because its
    * appended tail only scratches them.  It is the same word when the pass
    * carries a second varying: that varying takes output pair 1, which is
    * already an enabled output, and the two tail slots it displaces write
    * the same scratch pair.  The tail-free head writes 0x1d as target 3, and
    * with 0xbf00 carried over the engine stopped on every four-target draw
    * and retired none of them (MEASURED: fixture vary5 failed with
    * "two-pass job failed (Device or resource busy)" while the same draw
    * with one, two or three targets rendered exactly).  Every captured head
    * pass declares a run of pairs up to 15 instead -- 0xffff, 0xfffe,
    * 0xfffc, 0xfff8, 0xfff0, 0xffe0 -- so the extra draw claims them all,
    * which is the captured 0x0100ffff. */
   EMIT(V(0x88, (BITFIELD_MASK((decl_count - 1) / 2) << 24) |
                (extra ? 0x0000ffffu : 0x0000bf00u)),
        V(0x89, 0xfe201e92), V(0x8a, 0), V(0xc1, 0));
   EMIT(R(0x00, job->source, 0, SYSTEM), V(0x01, 0xf2fe014f),
        V(0x10, record_bytes));
   /* The vertex program of a split shader sits behind every fragment
    * pass, so its entry comes from the passes when fs is unset. */
   EMIT(R(0xc2, job->program, 0, PROGRAM),
        V(0xc3, kind == M9_PRE_HEAD ? job->vs_extra_entry :
                kind == M9_PRE_GATHER ? job->vs_gather_entry :
                job->fs ? job->fs->vertex_entry :
                job->pass_count ? job->passes[0].fs.vertex_entry : 4),
        V(0x8f, 0));
   EMIT(V(0x8c, 0x00500000 | const_pairs << 8),
        R(0x8d, job->system, M9_VS_CONST_OFFSET, SYSTEM),
        R(0x8e, job->system, M9_VS_CONST_OFFSET + 8 * const_pairs, SYSTEM),
        V(0x8f, 0));
   ret = m9_emit_units(batch, job, NULL, NULL, 0);
   if (ret)
      goto fail;
   /* The gather reads the head's record: one route a varying, each naming
    * the record and the field the draw's own output pair takes.  These
    * restate the idle words m9_emit_units just wrote. */
   if (kind == M9_PRE_GATHER)
      for (unsigned t = 0; t < targets; t++)
         EMIT(V(0x74 + t, m9_vs_gather_route(t)),
              R(0x78, job->transient, VIEW(job->vs_record_view), TEXTURE),
              V(0x79, m9_vs_extra_desc(t, targets)));
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
   /* M9X_VS_EXTRA_BREAK=0 keeps both preprocessing draws inside one range.
    * The vendor separates its own preprocessing draws with an engine
    * transition the kernel refuses, so which barrier belongs between them
    * is undecided (docs/parhelia-m9-vertex-output-target-re.md section 4). */
   if (!ret && !(extra && !debug_get_bool_option("M9X_VS_EXTRA_BREAK", true)))
      ret = m9_batch_emit_range_break(batch);
fail:
   return ret;
}
