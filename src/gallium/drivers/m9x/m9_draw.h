/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_DRAW_H
#define M9_DRAW_H

#include "m9_batch.h"
#include "m9_draw_params.h"
#include "m9_texture.h"

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
   uint32_t depth_control;    /* raster a8 of this pass */
   uint32_t a9, ae;           /* coverage words, which differ on a record pass */
   unsigned route_count;
   struct m9_pass_route routes[M9_TEXTURE_MAX_UNITS];
   /* Application units this pass binds, as indices into the draw's unit
    * array; M9_PASS_UNIT_PLACEHOLDER binds the 1x1 opaque black texture a
    * record-writing pass still needs in slot 0. */
   unsigned unit_count;
   uint8_t unit_index[M9_TEXTURE_MAX_UNITS];
};

#define M9_PASS_UNIT_PLACEHOLDER 0xffu

/* One two-pass job: shared pass state, GEM handles and its raster draws. */
struct m9_draw_job {
   const struct m9_draw_params *params;
   const struct m9_vertex_decl *decl;
   uint32_t source;        /* DMA32 source records */
   uint32_t system;        /* DMA32 8d/8e constants and 6c record */
   uint32_t program;       /* VRAM c2 pass-through program */
   uint32_t fragment_db;   /* VRAM fixed-fragment database */
   uint32_t transient;     /* VRAM, M9_TRANSIENT_VIEWS stream views */
   uint32_t cpu_streams;   /* DMA32, the views the CPU writes */
   uint32_t color, depth, aux;
   uint32_t be;
   uint32_t source_vertices;  /* padded record count, multiple of four */
   uint32_t index;            /* DMA32 raster indices, contiguous */
   uint32_t index_base;       /* byte offset of the indices in that object */
   uint32_t index_count;      /* 16-bit indices stored there */
   const struct m9_draw_record *draws;
   unsigned draw_count;
   uint32_t depth_control;    /* raster a8 */
   uint32_t depth_ef;         /* raster ef depth bits */
   uint32_t color_mask;       /* raster ab */
   bool blend_dest;           /* the fragment program blends the destination */
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
   /* Raster texture units, texel pair order. */
   const struct m9_texture_unit *units;
   unsigned unit_count;
   /* Fragment passes.  A zero count keeps the single raster pass driven by
    * fs/units above; any other count replaces it, and every pass but the
    * last renders into record. */
   const struct m9_raster_pass *passes;
   unsigned pass_count;
   uint32_t record;           /* VRAM inter-pass record surface */
   /* Placeholder texture a record-writing pass binds in slot 0. */
   const struct m9_texture_unit *placeholder;
};

unsigned m9_draw_job_dwords(unsigned draw_count);
/* Dwords a declaration of this many words costs beyond the pass-through five
 * the base job already counts. */
unsigned m9_draw_decl_dwords(unsigned decl_count);
unsigned m9_draw_job_relocs(unsigned draw_count, unsigned unit_count);
unsigned m9_draw_units_dwords(unsigned unit_count);
unsigned m9_draw_job_dwords_fs(unsigned draw_count, unsigned unit_count);
/* Dwords and relocations one extra raster pass of a split shader costs. */
unsigned m9_draw_pass_dwords(unsigned draw_count, unsigned unit_count,
                             unsigned route_count);
unsigned m9_draw_pass_relocs(unsigned draw_count, unsigned unit_count,
                             unsigned route_count);
/* Dependency steps one raster pass advances; the kernel caps the value. */
#define M9_PASS_DEPENDENCY_STEP 0x20u
int m9_batch_emit_draw_job(struct m9_batch *batch,
                           const struct m9_draw_job *job);

#endif
