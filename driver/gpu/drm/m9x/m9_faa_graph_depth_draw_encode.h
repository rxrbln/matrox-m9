/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_DEPTH_DRAW_ENCODE_H
#define M9_FAA_GRAPH_DEPTH_DRAW_ENCODE_H
#include "m9_faa_graph_color_encode.h"
#include "m9_faa_graph_depth.h"

#define M9_FAA_DEPTH_DRAW_OWNERS 34u
#define M9_FAA_DEPTH_DRAW_PRIMARY_BYTES 7872u
#define M9_FAA_DEPTH_DRAW_SECONDARY_BYTES 2444u
#define M9_FAA_DEPTH_DRAW_TEMPLATE_BYTES 10316u
#define M9_FAA_DEPTH_DRAW_ENDS 7u

struct m9x_faa_depth_draw_image {
	u8 *primary, *secondary;
	u32 primary_capacity, secondary_capacity;
	u32 end_bytes[M9_FAA_DEPTH_DRAW_ENDS];
	u64 final_point;
};

/* Source-bounded, post-attach-and-clear suffix only. Caller owns the paired
 * attachment preparation, all six graph fences, publication and wait. */
int m9x_faa_depth_draw_encode(const u8 *blob, size_t bytes,
	const struct m9x_faa_graph_draw_owner owner[M9_FAA_DEPTH_DRAW_OWNERS],
	const struct m9x_faa_depth_views *views,
	u32 primary_cursor, u32 secondary_cursor, u64 previous_unlock_point,
	const struct m9x_faa_color_input *input,
	u32 previous_geometry_offset, u32 next_geometry_offset,
	struct m9x_faa_depth_draw_image *output);
#endif
