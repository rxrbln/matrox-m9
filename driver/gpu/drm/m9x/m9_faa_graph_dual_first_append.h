/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_DUAL_FIRST_APPEND_H
#define M9_FAA_GRAPH_DUAL_FIRST_APPEND_H
#include "m9_faa_graph_dual_first_encode.h"

#define M9_FAA_DUAL_APPEND_SECONDARY_BYTES 2868u

struct m9x_faa_dual_append_image {
	u8 *primary, *secondary;
	u32 primary_capacity, secondary_capacity;
	u32 primary_bytes, secondary_bytes;
	u32 end_bytes[M9_FAA_DUAL_FIRST_ENDS];
	u64 final_point;
};

/* CPU-only composition of an additional source-qualified dual-texture DRAW
 * before the same normal Lock. Both secondary inputs must be freshly built
 * by m9x_faa_dual_first_encode for the retained program and texture pair;
 * the extra one may bind a disjoint live constant/geometry range. No GPU
 * point is treated as completed by this constructor. Culling is separately
 * gated until the source's 24-byte insertion has a live producer. */
int m9x_faa_dual_first_append_encode(
	const struct m9x_faa_graph_draw_owner owner[31],
	u32 init_bytes, u64 init_point, u32 old_draw_count,
	u32 primary_cursor, u32 secondary_cursor,
	const u32 old_end_bytes[M9_FAA_DUAL_FIRST_ENDS], u64 old_final_point,
	const u8 *old_primary, u32 old_primary_bytes,
	const u8 *first_secondary, const u8 *extra_first_secondary,
	bool programs_and_textures_retained,
	struct m9x_faa_dual_append_image *out);
#endif
