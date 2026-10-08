/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_GRAPH_DUAL_BATCH_INPUT_H
#define M9_FAA_GRAPH_DUAL_BATCH_INPUT_H
#include "m9_faa_graph_frame_batch_input.h"

#define M9_FAA_DUAL_BATCH_MAX_DRAWS 3u

/* Copies every caller-owned byte before any graph preparation. This source
 * profile reuses one immutable program/declaration and one texture pair.
 */
int m9x_faa_dual_batch_copy(const struct drm_parhelia_m9_faa_graph_dual_batch
				    *args,
			    struct m9x_faa_owned_color_batch **result);
/* Version 2 retained later DRAWs: same program and texture pair, own data.
 * Version 3: each DRAW also names its own texture pair.
 */
int m9x_faa_dual_later_batch_copy(const struct drm_parhelia_m9_faa_graph_dual_later
					  *args,
				  struct m9x_faa_owned_color_batch **result);
#endif
