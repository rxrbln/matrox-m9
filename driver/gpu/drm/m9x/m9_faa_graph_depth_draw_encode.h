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

bool m9x_faa_depth_draw_input_valid(const struct m9x_faa_color_input *);

/* Source-bounded, post-attach-and-clear suffix only. Caller owns the paired
 * attachment preparation, all six graph fences, publication and wait. */
int m9x_faa_depth_draw_encode(const u8 *blob, size_t bytes,
	const struct m9x_faa_graph_draw_owner owner[M9_FAA_DEPTH_DRAW_OWNERS],
	const struct m9x_faa_depth_views *views,
	u32 primary_cursor, u32 secondary_cursor, u64 previous_unlock_point,
	u32 clear_flags,
	const struct m9x_faa_color_input *input,
	u32 geometry_header_offset, u32 vertex_record_offset,
	struct m9x_faa_depth_draw_image *output);

/* Private COLOR4 continuation candidate. The first-stage image is rebuilt
 * from live input and authenticated first firmware into caller scratch;
 * neither scratch commands nor this suffix are posted by the encoder. The
 * caller supplies the retained queue's actual next epoch, not a captured
 * source point or an inferred shader-dependent increment. */
#define M9_FAA_DEPTH_COLOR_SECONDARY_BYTES 2468u
int m9x_faa_depth_color_encode(const u8 *depth_blob, size_t depth_bytes,
	const u8 *first_blob, size_t first_bytes,
	const struct m9x_faa_graph_draw_owner owner[M9_FAA_DEPTH_DRAW_OWNERS],
	const struct m9x_faa_depth_views *views,
	u32 init_bytes, u32 primary_cursor, u32 secondary_cursor,
	u64 live_next_epoch, u32 clear_flags,
	const struct m9x_faa_color_input *input,
	u32 geometry_header_offset, u32 vertex_record_offset,
	struct m9x_faa_depth_draw_image *output,
	struct m9x_faa_color_image *first_scratch);
#endif
