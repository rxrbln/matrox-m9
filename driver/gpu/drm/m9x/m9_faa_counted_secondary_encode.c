/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/errno.h>
#include <linux/limits.h>
#include "m9_faa_counted_secondary_encode.h"

int m9x_faa_counted_secondary_encode(
	const struct m9x_faa_counted_secondary_request *r,
	struct m9x_faa_counted_secondary_image *image)
{
	struct m9x_faa_counted_secondary_image next;
	u32 secondary_point, pool_point;

	if (!r || !image || r->mode != 1 || r->flags ||
	    r->generation != r->observed_generation ||
	    !r->old.identity || !r->selected.identity ||
	    r->old.identity == r->selected.identity ||
	    !r->old.mapped || !r->selected.mapped ||
	    !r->resource_queries_retired ||
	    r->old.cpu_refs != 1 || r->selected.cpu_refs ||
	    r->selected.last_pool_use > r->observed_pool_point ||
	    r->selected.last_global_use > r->observed_global_point ||
	    (r->range_start | r->range_end | r->parent_cursor |
	     r->old.gpu_base | r->selected.gpu_base) & 3 ||
	    r->range_start >= r->range_end ||
	    r->range_end - r->range_start < 12 ||
	    r->range_end > r->old.bytes ||
	    r->selected.bytes < 12 ||
	    r->parent_cursor > r->parent_capacity ||
	    r->parent_capacity - r->parent_cursor < 24 ||
	    (r->global_origin & 15) || (r->pool_origin & 15) ||
	    r->global_origin > U64_MAX - 16 ||
	    r->pool_origin > U64_MAX - 16 ||
	    (u32)r->global_origin > U32_MAX - 16 ||
	    (u32)r->pool_origin > U32_MAX - 16 ||
	    !r->fence_bus || (r->fence_bus & 15) ||
	    r->fence_bus > U32_MAX - 0x24 ||
	    r->old.gpu_base > U32_MAX - r->old.bytes ||
	    r->selected.gpu_base > U32_MAX - r->selected.bytes)
		return -EINVAL;

	secondary_point = (u32)r->global_origin + 16;
	pool_point = (u32)r->pool_origin + 16;
	next.old_footer[0] = 0x4545e3e6;
	next.old_footer[1] = 1;
	next.old_footer[2] = secondary_point | 1;
	next.parent[0] = 0x45452d2c;
	next.parent[1] = (r->old.gpu_base + r->range_start) | 1;
	next.parent[2] = r->old.gpu_base + r->range_end;
	next.parent[3] = 0x4545421a;
	next.parent[4] = ((u32)r->fence_bus + 0x20) | 2;
	next.parent[5] = pool_point | 1;
	next.secondary_point = r->global_origin + 16;
	next.pool_point = r->pool_origin + 16;
	next.parent_next = r->parent_cursor + 24;
	*image = next;
	return 0;
}
