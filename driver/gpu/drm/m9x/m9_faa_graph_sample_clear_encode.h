/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_SAMPLE_CLEAR_ENCODE_H
#define M9_FAA_GRAPH_SAMPLE_CLEAR_ENCODE_H
#include "m9_faa_graph_depth.h"
#include "m9_faa_graph_draw_encode.h"
#define M9_FAA_SAMPLE_CLEAR_TEMPLATE_BYTES 3456u
#define M9_FAA_SAMPLE_CLEAR_CAPACITY 3520u
/* Last b5e70 of the full bound-attachment clear. Requires the preceding
 * HZ/depth setup and admitted SYSTEM root pages; this emits no GPU receipt. */
int m9x_faa_sample_clear_encode(const u8 *blob, size_t bytes,
 const struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS],
 const struct m9x_faa_depth_views *views, u64 base_point,
 u8 *primary, size_t capacity, u32 *length);
#endif
