/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/errno.h>
#include <linux/limits.h>
#include "m9_faa_primary_pool.h"

int m9x_faa_primary_pool_plan(m9x_faa_primary_pool *pool,
	u64 global_last_submitted, u64 pool_origin, u64 fence_bus,
	const struct m9x_faa_secondary_pool_observation *observed,
	struct m9x_faa_primary_pool_image *image,
	m9x_faa_primary_transition *transition)
{
	const struct m9x_faa_counted_secondary_owner *old, *selected;
	struct m9x_faa_primary_pool_image next;
	m9x_faa_primary_transition token;
	u32 selected_index, point;

	if (!pool || !pool->initialized || pool->quarantined || pool->planned ||
	    !observed || !image || !transition ||
	    observed->generation != pool->generation ||
	    observed->pool_point != pool->completed_pool_point ||
	    pool_origin != pool->submitted_pool_point ||
	    !observed->resource_queries_retired ||
	    (global_last_submitted & 15) ||
	    global_last_submitted > observed->global_point ||
	    pool->cursor > U32_MAX - 16 || pool->serial == U64_MAX ||
	    (pool_origin & 15) || pool_origin > U64_MAX - 16 ||
	    (u32)pool_origin > U32_MAX - 16 ||
	    !fence_bus || (fence_bus & 15) || fence_bus > U32_MAX - 0x24)
		return -EINVAL;
	selected_index = pool->active ^ 1;
	if (!pool->slot[selected_index].bound)
		return -ENOSPC;
	old = &pool->slot[pool->active].owner;
	selected = &pool->slot[selected_index].owner;
	if (!old->mapped || !selected->mapped ||
	    old->identity == selected->identity ||
	    old->cpu_refs != 1 || selected->cpu_refs ||
	    pool->cursor > old->bytes || old->bytes - pool->cursor < 16 ||
	    selected->last_pool_use > observed->pool_point ||
	    selected->last_global_use > observed->global_point ||
	    (old->gpu_base & 3) || (selected->gpu_base & 3) ||
	    old->gpu_base > U32_MAX - old->bytes ||
	    selected->gpu_base > U32_MAX - selected->bytes)
		return -EINVAL;
	point = (u32)pool_origin + 16;
	next.old_link[0] = 0x453e421a;
	next.old_link[1] = ((u32)fence_bus + 0x20) | 2;
	next.old_link[2] = point | 1;
	next.old_link[3] = selected->gpu_base | 1;
	next.new_end = selected->gpu_base;
	next.old_next = pool->cursor + 16;
	next.pool_point = pool_origin + 16;
	token.serial = pool->serial + 1;
	token.old_identity = old->identity;
	token.selected_identity = selected->identity;
	/* The primary-mode source emits no new global E3. */
	token.secondary_point = global_last_submitted;
	token.pool_point = next.pool_point;
	token.generation = pool->generation;
	token.old_cursor = pool->cursor;
	token.parent_next = next.new_end;
	pool->pending = token;
	pool->planned = true;
	*image = next;
	*transition = token;
	return 0;
}
