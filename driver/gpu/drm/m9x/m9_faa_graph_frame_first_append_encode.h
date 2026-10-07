/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_FRAME_FIRST_APPEND_ENCODE_H
#define M9_FAA_GRAPH_FRAME_FIRST_APPEND_ENCODE_H
#include "m9_faa_graph_color_encode.h"

#define M9_FAA_FIRST_APPEND_SECONDARY_BYTES 2700u
#define M9_FAA_FIRST_APPEND_ENDS 14u

struct m9x_faa_frame_first_append_image {
	u8 *primary, *secondary;
	u32 primary_capacity, secondary_capacity;
	u32 end_bytes[M9_FAA_FIRST_APPEND_ENDS];
	u32 primary_bytes;
	u64 final_point;
};

/* Source-only FIRST bound-depth COLOR4 batch continuation. Input primary is
 * the entire same-context image through the old pending normal Lock. The
 * immutable first_secondary and newly encoded extra_first_secondary are
 * from trusted live COLOR4 encoders with identical program/declaration/state
 * but disjoint caller-owned constant/geometry ranges. No GPU post or fence
 * transition occurs here. Original two-output N=2/3 controls are exact;
 * live one-output/FF/count variants remain subject to hardware proof. */
int m9x_faa_frame_first_append_encode(
	const struct m9x_faa_graph_draw_owner owner[31],
	u32 init_bytes, u64 init_point, u32 old_draw_count,
	u32 primary_cursor, u32 secondary_cursor,
	const u32 old_end_bytes[M9_FAA_FIRST_APPEND_ENDS],
	u64 old_final_point,
	const u8 *old_primary, u32 old_primary_bytes,
	const u8 *first_secondary, u32 first_secondary_bytes,
	const u8 *extra_first_secondary, u32 extra_first_bytes,
	const struct m9x_faa_color_input *extra_input, bool same_program,
	struct m9x_faa_frame_first_append_image *out);
#endif
