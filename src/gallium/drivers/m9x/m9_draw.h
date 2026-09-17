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

/* One two-pass job: shared pass state, GEM handles and its raster draws. */
struct m9_draw_job {
   const struct m9_draw_params *params;
   const struct m9_vertex_decl *decl;
   uint32_t source;        /* DMA32 source records */
   uint32_t system;        /* DMA32 8d/8e constants and 6c record */
   uint32_t program;       /* VRAM c2 pass-through program */
   uint32_t fragment_db;   /* VRAM fixed-fragment database */
   uint32_t transient;     /* VRAM, M9_TRANSIENT_VIEWS stream views */
   uint32_t color, depth, aux;
   uint32_t be;
   uint32_t source_vertices;  /* padded record count, multiple of four */
   uint32_t index_base;       /* byte offset of the indices in the source */
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
   /* Raster texture units, texel pair order. */
   const struct m9_texture_unit *units;
   unsigned unit_count;
};

unsigned m9_draw_job_dwords(unsigned draw_count);
unsigned m9_draw_job_relocs(unsigned draw_count, unsigned unit_count);
unsigned m9_draw_units_dwords(unsigned unit_count);
unsigned m9_draw_job_dwords_fs(unsigned draw_count, unsigned unit_count);
int m9_batch_emit_draw_job(struct m9_batch *batch,
                           const struct m9_draw_job *job);

#endif
