/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_DRAW_H
#define M9_DRAW_H

#include "m9_batch.h"
#include "m9_draw_params.h"
#include "m9_faa.h"
#include "m9_texture.h"
#include "m9_vs_compile.h"

/* One raster draw; start is a four-record boundary of the source.  A draw
 * with index_count reads that many 16-bit indices from the source object at
 * index_first instead of walking the records in order, so a shared vertex is
 * preprocessed once. */
struct m9_draw_record {
   uint32_t start, count;
   uint32_t index_first, index_count;
   enum m9_topology topology;
   uint32_t cull;          /* ef cull bits */
   bool lines;
   bool flat;              /* raster 22 bit 14 */
   /* Texture units this draw samples; restated in the stream whenever they
    * differ from the preceding draw, so one job spans texture changes. */
   struct m9_texture_unit units[M9_TEXTURE_MAX_UNITS];
   unsigned unit_count;
};

/* A four-aligned preprocessing run with its own constant binding. Runs
 * partition the source in order; raster draws retain their original starts. */
struct m9_vs_const_run {
   uint32_t first, count;
   uint32_t const_offset, const_pairs;
};

/* One value carried from a producing raster pass to a later one through a
 * per-fragment record in memory: the selector emitted at 0x74 + k and the
 * consumer descriptor emitted at 0x79 behind it.
 * docs/parhelia-m9-fragment-pass-routes.md */
struct m9_pass_route {
   uint32_t word;             /* 0x74 + k */
   uint32_t desc;             /* 0x79 */
};

/* One raster pass of a fragment shader split across several of them.  A pass
 * that is not the last renders into the record surface instead of the colour
 * target and hands its values on through the routes the next passes bind. */
struct m9_raster_pass {
   struct m9_raster_fs fs;
   unsigned entry;            /* raster c3: this pass's program slot */
   uint64_t const_offset;     /* byte offset of its constant span */
   bool writes_record;        /* binds the record at b0 instead of the target */
   uint32_t record_desc;      /* b1 of a record-writing pass */
   /* Preprocessed records this pass draws from: the raster stage consumes
    * what preprocessing produced, so every pass reads a copy of its own. */
   uint32_t vertex_offset;    /* records before this pass's copy */
   uint32_t index_offset;     /* bytes before its copy of the index run */
   /* Bytes ahead of a CPU-gathered stream view for this pass.  The gather
    * is written once at the tail of the view and each pass's base is pulled
    * back by its own vertex_offset, so one copy serves every pass. */
   uint32_t cpu_stream_offset;
   uint32_t depth_control;    /* raster a8 of this pass */
   uint32_t depth_ef;         /* raster ef depth bits of this pass */
   uint32_t sequence_ef;      /* raster ef bits 10:9: this pass's place */
   unsigned pass_index;       /* its place in the sequence, across submissions */
   uint32_t a9, ae;           /* coverage words, which differ on a record pass */
   uint32_t aa;               /* second stencil face */
   uint32_t r89;              /* raster 89: a render format class per target */
   unsigned route_count;
   struct m9_pass_route routes[M9_TEXTURE_MAX_UNITS];
   /* What each of the pass's texture slots binds, slot by slot: an index
    * into the job's unit array, M9_PASS_UNIT_PLACEHOLDER for the 1x1
    * opaque black texture a record-writing pass without one still needs,
    * or M9_PASS_UNIT_NONE for a slot whose texel a route fills. */
   unsigned unit_count;       /* slots described, the rest are cleared */
   uint8_t unit_index[M9_TEXTURE_MAX_UNITS];
};

/* Colour attachments the raster stage binds: b0/b1 through b6/b7. */
#define M9_DRAW_MAX_COLOR 4u

#define M9_PASS_UNIT_PLACEHOLDER 0xffu
#define M9_PASS_UNIT_NONE        0xfeu

/* One two-pass job: shared pass state, GEM handles and its raster draws. */
struct m9_draw_job {
   const struct m9_draw_params *params;
   const struct m9_vertex_decl *decl;
   /* Preprocessing source stream 0 of the ten the engine has: slots 00..09
    * take a SYSTEM relocation and a byte stride in the packed words 10..14,
    * so a later zero-copy path can bind an application vertex buffer per
    * attribute instead of one packed record (m9_draw_params.c builds the
    * raster side that way, and index 23 bits 19:16 name the stream). */
   uint32_t source;        /* DMA32 source records */
   uint32_t system;        /* DMA32 8d/8e constants and 6c record */
   uint32_t program;       /* VRAM c2 pass-through program */
   uint32_t fragment_db;   /* VRAM fixed-fragment database */
   uint32_t transient;     /* VRAM, M9_TRANSIENT_VIEWS stream views */
   uint32_t cpu_streams;   /* DMA32, the views the CPU writes */
   uint32_t color, depth, aux;
   /* Byte offset of the colour level inside job->color's object. */
   uint64_t color_offset;
   /* Colour attachments 1..3, each with its own descriptor, bound at
    * b2/b3 .. b6/b7 behind job->color's b0/b1.  A count of zero or one
    * leaves the job at the single target job->color names.
    * docs/parhelia-m9-mrt-routing.md */
   uint32_t color_extra[M9_DRAW_MAX_COLOR - 1];
   uint32_t color_extra_desc[M9_DRAW_MAX_COLOR - 1];
   unsigned color_count;
   uint32_t be;
   uint32_t source_vertices;  /* padded record count, multiple of four */
   uint32_t index;            /* DMA32 raster indices, contiguous */
   uint32_t index_base;       /* byte offset of the indices in that object */
   uint32_t index_count;      /* 16-bit indices stored there */
   const struct m9_draw_record *draws;
   unsigned draw_count;
   uint32_t depth_control;    /* raster a8 */
   uint32_t depth_ef;         /* raster ef depth bits */
   bool depth_bias;           /* native triangle D24 offset, raster only */
   uint32_t depth_bias_units, depth_bias_factor;
   /* Raster a9/aa stencil faces; unset emits the test-off words. */
   bool stencil;
   uint32_t stencil_a9, stencil_aa;
   uint32_t color_mask;       /* raster ab: target 0 low, target 1 high */
   uint32_t color_mask_hi;    /* raster ac: target 2 low, target 3 high */
   /* Render format class of each bound target, packed into raster 89 by
    * m9_raster_89; a zero count keeps the single-target word. */
   uint8_t color_class[M9_DRAW_MAX_COLOR];
   bool blend_dest;           /* the fragment program blends the destination */
   bool logic_op;             /* ... through the logic-op tail */
   uint32_t clip_min, clip_max;
   /* Native fragment program; NULL keeps the pass-through colour image. */
   const struct m9_raster_fs *fs;
   /* Native preprocessing declaration: a compiled vertex program declares
    * its own attributes, more words than the pass-through record and its
    * own stride.  A zero count keeps decl's four words. */
   const uint32_t *vs_decl;
   unsigned vs_decl_count;    /* index-23 words, terminator included */
   uint32_t vs_record_bytes;
   unsigned vs_const_pairs;   /* 0 keeps the captured twenty-two */
   const struct m9_vs_const_run *vs_const_runs;
   unsigned vs_const_run_count;
   /* Transient view the fused pass's own fourth output target writes; zero
    * leaves that target unbound, because view 0 is always target 0's.  The
    * target is index 0xb6 and its register pair 0x1d/0x1f, which the
    * appended tail leaves once m9_vs_emit moves its scratch to r1e. */
   uint32_t vs_fused_view;
   /* Second preprocessing draw: the tail-free head that writes varyings
    * 2..4.  It runs ahead of the fused pass in a range of its own, binding
    * one stream target a varying and no clip code.  A zero entry leaves the
    * job at the single preprocessing draw.
    * docs/parhelia-m9-vertex-output-target-re.md section 4 */
   unsigned vs_extra_entry;   /* program slot the draw enters at c3 */
   unsigned vs_extra_targets; /* 1..4 stream targets it binds */
   uint32_t vs_extra_view[M9_VS_EXTRA_VARYINGS];  /* transient view each takes */
   /* The route-fed draw between the head and the fused pass: it reads the
    * head's packed record through 0x74 + k and writes each varying out to
    * the stream view above, which is what every captured chain does.  Zero
    * leaves the head writing those views itself. */
   unsigned vs_gather_entry;
   /* Nonzero selects the bounded fully route-fed multi-stream chain. */
   unsigned vs_producer_entry;
   uint16_t vs_producer_mask, vs_extra_mask;
   /* Explicitly supplied ordinary native mask; zero is a valid value. */
   bool vs_fused_mask_set;
   uint16_t vs_fused_mask;
   uint32_t vs_record_view;   /* transient view the packed record starts at */
   /* One direct-input, level-zero vertex sampler. Only the ordinary source
    * producer binds it; the raster range restores its own units. */
   const struct m9_texture_unit *vs_unit;
   /* Optional second direct-XY unit; its raw result occupies r04/r06. */
   const struct m9_texture_unit *vs_unit_second;
   /* Raster texture units, texel pair order. */
   const struct m9_texture_unit *units;
   unsigned unit_count;
   /* Fragment passes.  A zero count keeps the single raster pass driven by
    * fs/units above; any other count replaces it, and every pass but the
    * last renders into record. */
   const struct m9_raster_pass *passes;
   unsigned pass_count;
   uint32_t record;           /* VRAM inter-pass record surface */
   bool split_30;             /* index 30 follows the pass's own place */
   /* Run the preprocessing pass again ahead of every raster pass: the
    * raster stage drains the streams it produced, so a second pass over
    * the same job finds nothing left unless they are produced again. */
   bool preprocess_per_pass;
   /* Placeholder texture a record-writing pass binds in slot 0. */
   const struct m9_texture_unit *placeholder;
   /* Emit the vendor's inter-draw engine transition around every raster
    * draw of a split job.  A single-pass job never carries it, so the
    * captured stream stays byte for byte what it was. */
   bool transition;
   /* Multisample pair: ad four times and af once, built by m9_faa.c.  A job
    * that leaves set clear emits the off form the captured stream carries. */
   struct {
      bool set;
      /* Complete surface-owned graph, admitted separately from raw probes. */
      bool graph;
      /* Actual selector9 descriptor of the independently owned primary. */
      uint32_t primary_desc;
      bool raster;   /* restate the pair in the raster range as well */
      uint32_t ad, af;
      /* ae bits 6:4, ef bits 31:29 and two f0 flags are edited rather than
       * replaced, so a pass keeps the rest of its own word. */
      struct m9_faa_edits edit;
      /* The fragment store and the index its address is written to.  A
       * negative index binds nothing, which is the default. */
      bool store_bind;
      int bind_index;
      int64_t bind_desc;
      bool bind_reset;
      uint32_t store, store_aux;
      /* Bound at b8/b9 in place of the depth attachment; zero keeps it. */
      uint32_t companion, companion_desc;
      /* FAA's own HZ attachment, independent of ordinary depth metadata. */
      uint32_t hz, hz_desc;
   } faa;
};

/* Dwords a restated raster draw costs beyond the first. */
#define M9_JOB_DRAW_DWORDS 11u
#define M9_DEPTH_BIAS_EF (1u << 26)
#define M9_DEPTH_BIAS_DWORDS 3u
/* Captured D0 units are divided by 2^24-1; D1 is the raw slope factor. */
bool m9_draw_depth_bias(float factor, float units,
                         uint32_t *units_bits, uint32_t *factor_bits);
unsigned m9_draw_job_dwords(unsigned draw_count);
unsigned m9_draw_framed_dwords(unsigned dwords, unsigned restated_draws);
/* Dwords a declaration of this many words costs beyond the pass-through five
 * the base job already counts. */
unsigned m9_draw_decl_dwords(unsigned decl_count);
/* Dwords and relocations the second preprocessing draw costs, per raster
 * pass that preprocesses for itself. */
unsigned m9_draw_vs_extra_dwords(unsigned target_count, unsigned decl_count);
unsigned m9_draw_vs_extra_relocs(unsigned target_count);
unsigned m9_draw_vs_producer_dwords(unsigned decl_count);
unsigned m9_draw_vs_producer_relocs(void);
unsigned m9_draw_job_relocs(unsigned draw_count, unsigned unit_count);
/* Dwords and relocations each colour attachment past the first costs: one
 * address/descriptor packet and its COLOR relocation, per raster pass. */
#define M9_DRAW_COLOR_DWORDS 3u
unsigned m9_draw_color_dwords(unsigned color_count, unsigned pass_count);
unsigned m9_draw_color_relocs(unsigned color_count, unsigned pass_count);
/* Raster 89: the render format class of every bound target packed three bits
 * at a time from bit 0, with 7 in each unbound field.  base supplies the bits
 * above 14, which no capture varies.  A count of zero returns base unchanged.
 * docs/parhelia-m9-mrt-routing.md */
uint32_t m9_preprocess_89(unsigned targets, bool packed, bool clip);
uint32_t m9_raster_89(uint32_t base, const uint8_t *color_class,
                      unsigned color_count);
/* Raster ab and ac: one sixteen-bit write mask a target, ab holding targets
 * 0 and 1 and ac targets 2 and 3.  m9_color_mask builds one target's word. */
uint32_t m9_color_mask_pair(const uint32_t *mask, unsigned count,
                            unsigned first);
unsigned m9_draw_units_dwords(unsigned unit_count);
unsigned m9_draw_job_dwords_fs(unsigned draw_count, unsigned unit_count);
/* Dwords and relocations one extra raster pass of a split shader costs. */
unsigned m9_draw_pass_dwords(unsigned draw_count, unsigned unit_count,
                             unsigned route_count);
unsigned m9_draw_pass_relocs(unsigned draw_count, unsigned unit_count,
                             unsigned route_count);
/* Dependency steps one pass advances.  Index e3 advances by 0x10 on every
 * write across the whole submission, so a raster range claims three steps
 * and a preprocessing range of its own two more; the kernel caps the value
 * at PARHELIA_M9_DEPENDENCY_LIMIT. */
#define M9_PASS_DEPENDENCY_STEP 0x30u
#define M9_PASS_DEPENDENCY_FULL 0x50u
uint32_t m9_draw_job_final_dependency(const struct m9_draw_job *job);
int m9_batch_emit_draw_job(struct m9_batch *batch,
                           const struct m9_draw_job *job);

#endif
