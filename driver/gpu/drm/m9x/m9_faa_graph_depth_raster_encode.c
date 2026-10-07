// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include "m9_faa_graph_depth_raster_encode.h"

static bool depth_raster_overlap(const void *a, unsigned long na,
				const void *b, unsigned long nb)
{
	unsigned long x = (unsigned long)a, y = (unsigned long)b, xe, ye;
	return check_add_overflow(x, na, &xe) ||
	       check_add_overflow(y, nb, &ye) || (x < ye && y < xe);
}

int m9x_faa_depth_raster_encode(const struct m9x_faa_depth_raster_input *in,
			      struct m9x_faa_depth_raster_output *out)
{
	/* Original bound-depth indirect dispatches and seven END boundaries.
	 * These are command grammar offsets, independent of owner placement. */
	static const u32 ranges[] = {4244, 4248, 4292, 4296};
	static const u32 ends[] = {4212, 4212, 4240, 4300, 7860, 7860, 7872};
	u32 i, limit;
	int ret;

	if (!in || !out ||
	    in->streams.primary_bytes != M9_FAA_DEPTH_DRAW_PRIMARY_BYTES ||
	    memcmp(in->streams.range_at, ranges, sizeof(ranges)) ||
	    (in->primary_cursor & 3) ||
	    check_add_overflow(in->primary_cursor,
		M9_FAA_DEPTH_DRAW_PRIMARY_BYTES, &limit))
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(ends); i++)
		if (in->end_bytes[i] != in->primary_cursor + ends[i])
			return -EINVAL;
	/* Neither stream writer may overwrite its descriptors or END result. */
	if (depth_raster_overlap(out, sizeof(*out), in, sizeof(*in)) ||
	    depth_raster_overlap(out, sizeof(*out), in->streams.primary,
		in->streams.primary_bytes) ||
	    depth_raster_overlap(out, sizeof(*out), in->streams.secondary,
		in->streams.secondary_bytes) ||
	    depth_raster_overlap(out, sizeof(*out), out->streams.primary,
		out->streams.primary_capacity) ||
	    depth_raster_overlap(out, sizeof(*out), out->streams.secondary,
		out->streams.secondary_capacity) ||
	    depth_raster_overlap(out, sizeof(*out), out->streams.secondary_bytes,
		sizeof(*out->streams.secondary_bytes)))
		return -EINVAL;
	ret = m9x_faa_raster_encode(&in->streams, &out->streams);
	if (ret)
		return ret;
	memcpy(out->end_bytes, in->end_bytes, sizeof(out->end_bytes));
	return 0;
}
