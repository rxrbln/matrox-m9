/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_SAMPLE_CLEAR_ENCODE_H
#define M9_FAA_GRAPH_SAMPLE_CLEAR_ENCODE_H
#include "m9_faa_graph_depth.h"
#include "m9_faa_graph_draw_encode.h"
#define M9_FAA_SAMPLE_CLEAR_TEMPLATE_BYTES 3456u
#define M9_FAA_SAMPLE_CLEAR_MAX_GROUPS 97u
#define M9_FAA_SAMPLE_CLEAR_CAPACITY (3456u + 4u * M9_FAA_SAMPLE_CLEAR_MAX_GROUPS + 20u)
/* Last b5e70 of the full bound-attachment clear. Requires the preceding
 * HZ/depth setup and retained live owner identities; emits no GPU receipt. */
int m9x_faa_sample_clear_encode(const u8 *blob, size_t bytes,
 const struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS],
 const struct m9x_faa_depth_views *views, u64 base_point,
 u8 *primary, size_t capacity, u32 *length);
#endif
