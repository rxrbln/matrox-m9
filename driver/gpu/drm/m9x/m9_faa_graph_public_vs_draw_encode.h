/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_PUBLIC_VS_DRAW_ENCODE_H
#define M9_FAA_GRAPH_PUBLIC_VS_DRAW_ENCODE_H
#include "m9_faa_graph_draw_encode.h"
#define M9_FAA_VS_DRAW_PRIMARY_BYTES 60u
#define M9_FAA_VS_DRAW_SECONDARY_BYTES 2688u
#define M9_FAA_VS_DRAW_TEMPLATE_BYTES 2748u
struct m9x_faa_public_vs_draw_image {
 u8 *primary, *secondary;
 u32 primary_bytes, secondary_bytes;
};
struct m9x_faa_public_vs_draw_endpoints {u32 final_end;u64 observed_point;};
bool m9x_faa_public_vs_draw_template_valid(const u8 *blob, size_t bytes);
int m9x_faa_public_vs_draw_encode(const u8 *blob, size_t bytes,
 const struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS],
 u32 live_primary_bytes, u64 head_point,
 const struct m9x_faa_public_vs_draw_image *out,
 struct m9x_faa_public_vs_draw_endpoints *ends);
#endif
