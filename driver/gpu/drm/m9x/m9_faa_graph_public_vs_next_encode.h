/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_PUBLIC_VS_NEXT_ENCODE_H
#define M9_FAA_GRAPH_PUBLIC_VS_NEXT_ENCODE_H
#include "m9_faa_graph_draw_encode.h"
#define M9_FAA_VS_NEXT_PRIMARY_BYTES 60u
#define M9_FAA_VS_NEXT_SECONDARY_BYTES 2548u
#define M9_FAA_VS_NEXT_VERTEX_BYTES 96u
#define M9_FAA_VS_NEXT_VERTEX_STRIDE 128u
#define M9_FAA_VS_NEXT_TEMPLATE_BYTES 2608u
struct m9x_faa_public_vs_next_image {u8 *primary,*secondary;u32 primary_bytes,secondary_bytes;};
bool m9x_faa_public_vs_next_blob_valid(const u8 *blob,size_t bytes);
int m9x_faa_public_vs_next_encode(const u8 *blob,size_t bytes,
 const struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS],
 u32 primary_cursor,u32 secondary_cursor,u32 vertex_cursor,u64 unlock_point,
 u32 fs_begin,u32 fs_end,const struct m9x_faa_public_vs_next_image *out);
int m9x_faa_public_vs_next_vertices_valid(const u32 vertices[24]);
#endif
