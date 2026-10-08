/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_GRAPH_FRAME_BATCH_INPUT_H
#define M9_FAA_GRAPH_FRAME_BATCH_INPUT_H
#include "m9x_drm.h"
#include "m9_faa_graph_color_encode.h"
#include "m9_faa_graph_batch_limits.h"
struct m9x_faa_owned_color_draw {
 struct m9x_faa_color_input input;
 u8 *upload;
 u32 bytes,raster_flags;
 /* Dual later version 3: this DRAW's own texture pair. */
 struct drm_parhelia_m9_faa_texture_view texture[2];
	/* Dual later version 3: PARHELIA_M9_FAA_PROGRAM_DUAL or _SINGLE. */
	u32 program;
};
struct m9x_faa_owned_color_batch {
 u32 count,flags,clear_flags;
 struct m9x_faa_owned_color_draw draw[M9_FAA_FRAME_BATCH_MAX_DRAWS];
};
/* The caller holds no graph BO reservations. No upload pointer survives this
 * transaction; all code/CB/raw vertex/declaration bytes become kernel-owned.
 * It cannot issue commands or authorize a retirement receipt. */
int m9x_faa_color_batch_copy(const struct drm_parhelia_m9_faa_graph_color_frame_batch *,
 struct m9x_faa_owned_color_batch **);
void m9x_faa_color_batch_free(struct m9x_faa_owned_color_batch *);
#endif
