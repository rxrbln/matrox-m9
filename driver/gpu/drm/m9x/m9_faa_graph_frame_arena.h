/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_FRAME_ARENA_H
#define M9_FAA_GRAPH_FRAME_ARENA_H
#include <linux/types.h>

/* Pure input-placement transaction for a second bound COLOR4 DRAW before
 * final Lock. No command is posted and no owner is modified by this helper.
 * A changed program is deliberately excluded: the source control retains
 * owner26 code and does not establish a second immutable program slot. */
struct m9x_faa_frame_input_plan {
	u32 prior_end;
	u32 constants_offset;
	u32 constants_bytes;
	u32 vertices_offset;
	u32 vertices_bytes;
	u32 next_end;
};

int m9x_faa_frame_input_plan(u32 owner28_bytes, u32 occupied_end,
	bool changed_program, bool changed_cb0, u32 cb_bytes,
	u32 vertex_count, u32 record_stride,
	struct m9x_faa_frame_input_plan *result);
#endif
