/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_DEPTH_ENCODE_H
#define M9_FAA_GRAPH_DEPTH_ENCODE_H
#include "m9_faa_graph_depth.h"
struct m9x_faa_depth_stage_image {
 u32 hz[20];
 u32 rectangle[10];
};
#define M9_FAA_DEPTH_CLEAR_BYTES 872u
#define M9_FAA_DEPTH_CLEAR_SETUP_BYTES 832u
struct m9x_faa_depth_clear_image {
 u32 word[M9_FAA_DEPTH_CLEAR_BYTES / 4];
};
#define M9_FAA_ATTACHMENT_CLEAR_CAPACITY 1508u
/* 1024 primary logical extent769 pages, at most97 page-group words. */
struct m9x_faa_attachment_clear_image {
 u32 word[M9_FAA_ATTACHMENT_CLEAR_CAPACITY / 4];
 u32 bytes;
 /* Color END, HZ END twice, depth setup END, depth rectangle END.
  * The subsequent sampled-clear producer contributes its own final END. */
 u32 end_bytes[5];
};
/* Typed b5030 bodies; no publication or completion is implied. The full
 * bound-clear prefix must install its source-required state around them. */
int m9x_faa_depth_stages_encode(const struct m9x_faa_depth_views *,u32,
 struct m9x_faa_depth_stage_image *);
/* Specialized b5630 setup + b5030 rectangle. Fixed40004000 transition
 * control is distinct from typed bound owner addresses; no pages are mapped.
 * Source END occurs after setup832, then after rectangle872. */
int m9x_faa_depth_clear_encode(const struct m9x_faa_depth_views *,u32,u32,
 struct m9x_faa_depth_clear_image *);
int m9x_faa_attachment_clear_encode(const struct m9x_faa_depth_views *,u32,u32,
 struct m9x_faa_attachment_clear_image *);
/* Matching retained COLOR4 first attachment clear; epoch is live queue state. */
int m9x_faa_color_attachment_clear_encode(const struct m9x_faa_depth_views *,u32,u32,
 struct m9x_faa_attachment_clear_image *);
#endif
