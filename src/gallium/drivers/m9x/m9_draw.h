/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_DRAW_H
#define M9_DRAW_H

#include "m9_batch.h"
#include "m9_draw_params.h"

/* One raster draw; start is a four-record boundary of the source. */
struct m9_draw_record {
   uint32_t start, count;
   enum m9_topology topology;
   uint32_t cull;          /* ef cull bits */
   bool lines;
   bool flat;              /* raster 22 bit 14 */
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
   const struct m9_draw_record *draws;
   unsigned draw_count;
   uint32_t depth_control;    /* raster a8 */
   uint32_t depth_ef;         /* raster ef depth bits */
   uint32_t color_mask;       /* raster ab */
   uint32_t clip_min, clip_max;
   /* Native fragment program; NULL keeps the pass-through colour image. */
   const struct m9_raster_fs *fs;
};

unsigned m9_draw_job_dwords(unsigned draw_count);
unsigned m9_draw_job_dwords_fs(unsigned draw_count);
int m9_batch_emit_draw_job(struct m9_batch *batch,
                           const struct m9_draw_job *job);

#endif
