/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/errno.h>
#include <linux/overflow.h>
#include "m9_faa_graph_frame_arena.h"

static int align128(u32 value, u32 *aligned)
{
	u32 rounded;

	if (check_add_overflow(value, 127u, &rounded))
		return -ERANGE;
	*aligned = rounded & ~127u;
	return 0;
}

int m9x_faa_frame_input_plan(u32 owner28_bytes, u32 occupied_end,
	bool changed_program, bool changed_cb0, u32 cb_bytes,
	u32 vertex_count, u32 record_stride,
	struct m9x_faa_frame_input_plan *result)
{
	struct m9x_faa_frame_input_plan next = {0};
	u32 after_cb, bytes;
	int ret;

	if (!result || changed_program || !vertex_count ||
	    vertex_count > 12288 || vertex_count % 3 ||
	    (record_stride != 16 && record_stride != 32) ||
	    (changed_cb0 ? (!cb_bytes || cb_bytes > 16384 || (cb_bytes & 7)) :
	                   cb_bytes != 0))
		return -EINVAL;
	if (occupied_end > owner28_bytes ||
	    check_mul_overflow(vertex_count, record_stride, &bytes))
		return -ERANGE;
	ret = align128(occupied_end, &next.constants_offset);
	if (ret)
		return ret;
	next.prior_end = occupied_end;
	next.constants_bytes = cb_bytes;
	if (check_add_overflow(next.constants_offset, cb_bytes, &after_cb) ||
	    after_cb > owner28_bytes)
		return -ERANGE;
	if (changed_cb0) {
		ret = align128(after_cb, &next.vertices_offset);
		if (ret)
			return ret;
	} else {
		next.vertices_offset = next.constants_offset;
	}
	next.vertices_bytes = bytes;
	if (check_add_overflow(next.vertices_offset, bytes, &next.next_end) ||
	    next.next_end > owner28_bytes)
		return -ERANGE;
	*result = next;
	return 0;
}
