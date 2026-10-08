/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_FIRST_DEPTH_TWO_ENCODE_H
#define M9_FAA_GRAPH_FIRST_DEPTH_TWO_ENCODE_H
#include "m9_faa_graph_first_depth_encode.h"
/* Genuine two-vector FIRST has its own secondary grammar and INIT+110
 * Lock frontier. Scratch is CPU-owned; all output owners remain caller-held. */
int m9x_faa_first_depth_two_encode(const u8 *sample,size_t sample_bytes,
 const u8 *first,size_t first_bytes,
 const struct m9x_faa_graph_draw_owner *owner,const struct m9x_faa_depth_views *,
 u32 init_bytes,u64 init_point,const struct m9x_faa_color_input *,
 u8 *primary,size_t primary_capacity,u8 *secondary,size_t secondary_capacity,
 u32 *primary_bytes,u32 *secondary_bytes,u32 *ends,
 const struct m9x_faa_color_image *first_scratch,
 struct m9x_faa_first_depth_scratch *prefix_scratch);
#endif
