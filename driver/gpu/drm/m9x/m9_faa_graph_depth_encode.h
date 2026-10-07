/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_DEPTH_ENCODE_H
#define M9_FAA_GRAPH_DEPTH_ENCODE_H
#include "m9_faa_graph_depth.h"
struct m9x_faa_depth_stage_image {
 u32 hz[20];
 u32 rectangle[10];
};
/* Typed b5030 bodies; no publication or completion is implied. The full
 * bound-clear prefix must install its source-required state around them. */
int m9x_faa_depth_stages_encode(const struct m9x_faa_depth_views *,u32,
 struct m9x_faa_depth_stage_image *);
#endif
