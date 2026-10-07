/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/string.h>
#include "m9_faa_secondary_pool.h"

static bool m9_faa_pool_owner_valid(
	const struct m9x_faa_counted_secondary_owner *owner)
{
	return owner && owner->identity && owner->mapped &&
		owner->bytes >= 12 && !(owner->gpu_base & 3) &&
		owner->gpu_base <= U32_MAX - owner->bytes;
}

int m9x_faa_pool_timeline_init(struct m9x_faa_pool_timeline *timeline,
	u32 generation, u64 initial_point)
{
	if (!timeline || timeline->initialized || (initial_point & 15))
		return -EINVAL;
	*timeline = (struct m9x_faa_pool_timeline) {
		.submitted_point = initial_point,
		.completed_point = initial_point,
		.generation = generation,
		.initialized = true,
	};
	return 0;
}

int m9x_faa_secondary_pool_init(struct m9x_faa_secondary_pool *pool,
	struct m9x_faa_pool_timeline *timeline,
	u32 generation, const struct m9x_faa_counted_secondary_owner *initial,
	u32 cursor)
{
	struct m9x_faa_secondary_pool next = { };

	if (!pool || pool->initialized || !timeline || !timeline->initialized ||
	    timeline->generation != generation || timeline->quarantined ||
	    timeline->planned || !m9_faa_pool_owner_valid(initial) ||
	    initial->cpu_refs != 1 || (cursor & 3) || cursor > initial->bytes)
		return -EINVAL;
	next.slot[0].owner = *initial;
	next.slot[0].bound = true;
	next.generation = generation;
	next.initialized = true;
	next.cursor = cursor;
	next.timeline = timeline;
	*pool = next;
	return 0;
}

int m9x_faa_secondary_pool_bind(struct m9x_faa_secondary_pool *pool,
	const struct m9x_faa_counted_secondary_owner *selected)
{
	const struct m9x_faa_counted_secondary_owner *old;
	u64 old_end, new_end;

	if (!pool || !pool->initialized || pool->planned || pool->quarantined ||
	    !pool->timeline || pool->timeline->planned ||
	    pool->timeline->quarantined ||
	    pool->slot[1].bound || !m9_faa_pool_owner_valid(selected) ||
	    selected->cpu_refs)
		return -EINVAL;
	old = &pool->slot[0].owner;
	old_end = (u64)old->gpu_base + old->bytes;
	new_end = (u64)selected->gpu_base + selected->bytes;
	if (selected->identity == old->identity ||
	    ((u64)selected->gpu_base < old_end && old->gpu_base < new_end))
		return -EINVAL;
	pool->slot[1].owner = *selected;
	pool->slot[1].bound = true;
	return 0;
}

int m9x_faa_secondary_pool_advance(struct m9x_faa_secondary_pool *pool,
	u32 expected_cursor, u32 bytes)
{
	if (!pool || !pool->initialized || pool->quarantined || pool->planned ||
	    !pool->timeline || pool->timeline->planned ||
	    pool->timeline->quarantined ||
	    pool->cursor != expected_cursor || (bytes & 3) ||
	    bytes > pool->slot[pool->active].owner.bytes - pool->cursor)
		return -EINVAL;
	pool->cursor += bytes;
	return 0;
}

int m9x_faa_secondary_pool_observe_raw(struct m9x_faa_secondary_pool *pool,
	u32 raw20, u64 global_completed, u32 generation, bool resources_idle,
	struct m9x_faa_secondary_pool_observation *observed)
{
	struct m9x_faa_secondary_pool_observation next;
	u64 span;
	u32 delta;

	if (!pool || !pool->initialized || pool->quarantined || !pool->timeline ||
	    pool->timeline->quarantined || !observed ||
	    generation != pool->generation ||
	    pool->timeline->submitted_point < pool->timeline->completed_point)
		return -EINVAL;
	span = pool->timeline->submitted_point - pool->timeline->completed_point;
	if (span >= (1ULL << 31))
		return -ERANGE;
	delta = (raw20 & ~15u) - (u32)pool->timeline->completed_point;
	if (delta > span)
		return -EAGAIN;
	next.pool_point = pool->timeline->completed_point + delta;
	next.global_point = global_completed;
	next.generation = generation;
	next.resource_queries_retired = resources_idle;
	pool->timeline->completed_point = next.pool_point;
	*observed = next;
	return 0;
}

int m9x_faa_secondary_pool_plan(struct m9x_faa_secondary_pool *pool,
	u32 range_start, u32 parent_cursor, u32 parent_capacity,
	u64 global_origin, u64 pool_origin, u64 fence_bus,
	const struct m9x_faa_secondary_pool_observation *observed,
	struct m9x_faa_counted_secondary_image *image,
	struct m9x_faa_secondary_pool_transition *transition)
{
	struct m9x_faa_counted_secondary_request request;
	struct m9x_faa_counted_secondary_image next;
	struct m9x_faa_secondary_pool_transition token;
	u32 selected;
	int ret;

	if (!pool || !pool->initialized || pool->quarantined || pool->planned ||
	    !pool->timeline || pool->timeline->planned ||
	    pool->timeline->quarantined ||
	    !observed || !image || !transition ||
	    observed->generation != pool->generation ||
	    observed->pool_point != pool->timeline->completed_point ||
	    pool_origin != pool->timeline->submitted_point ||
	    range_start != pool->cursor ||
	    pool->cursor > U32_MAX - 12)
		return -EINVAL;
	selected = pool->active ^ 1;
	if (!pool->slot[selected].bound)
		return -ENOSPC;
	memset(&request, 0, sizeof(request));
	request.old = pool->slot[pool->active].owner;
	request.selected = pool->slot[selected].owner;
	request.range_start = range_start;
	request.range_end = pool->cursor + 12;
	request.parent_cursor = parent_cursor;
	request.parent_capacity = parent_capacity;
	request.global_origin = global_origin;
	request.pool_origin = pool_origin;
	request.observed_pool_point = observed->pool_point;
	request.observed_global_point = observed->global_point;
	request.fence_bus = fence_bus;
	request.generation = pool->generation;
	request.observed_generation = observed->generation;
	request.mode = 1;
	request.resource_queries_retired = observed->resource_queries_retired;
	ret = m9x_faa_counted_secondary_encode(&request, &next);
	if (ret)
		return ret;
	if (pool->serial == U64_MAX)
		return -ERANGE;
	token.serial = pool->serial + 1;
	token.old_identity = request.old.identity;
	token.selected_identity = request.selected.identity;
	token.secondary_point = next.secondary_point;
	token.pool_point = next.pool_point;
	token.generation = pool->generation;
	token.old_cursor = pool->cursor;
	token.parent_next = next.parent_next;
	pool->pending = token;
	pool->planned = true;
	pool->timeline->planned = true;
	*image = next;
	*transition = token;
	return 0;
}

static bool m9_faa_pool_token_matches(const struct m9x_faa_secondary_pool *pool,
	const struct m9x_faa_secondary_pool_transition *transition)
{
	const struct m9x_faa_secondary_pool_transition *p;

	if (!pool || !transition || !pool->initialized || !pool->planned ||
	    pool->quarantined || !pool->timeline ||
	    !pool->timeline->planned || pool->timeline->quarantined)
		return false;
	p = &pool->pending;
	return p->serial == transition->serial &&
		p->old_identity == transition->old_identity &&
		p->selected_identity == transition->selected_identity &&
		p->secondary_point == transition->secondary_point &&
		p->pool_point == transition->pool_point &&
		p->generation == transition->generation &&
		p->old_cursor == transition->old_cursor &&
		p->parent_next == transition->parent_next;
}

int m9x_faa_secondary_pool_abort(struct m9x_faa_secondary_pool *pool,
	const struct m9x_faa_secondary_pool_transition *transition)
{
	if (!m9_faa_pool_token_matches(pool, transition))
		return -ESTALE;
	pool->planned = false;
	pool->timeline->planned = false;
	memset(&pool->pending, 0, sizeof(pool->pending));
	return 0;
}

int m9x_faa_secondary_pool_posted(struct m9x_faa_secondary_pool *pool,
	const struct m9x_faa_secondary_pool_transition *transition)
{
	u32 old, selected;

	if (!m9_faa_pool_token_matches(pool, transition))
		return -ESTALE;
	old = pool->active;
	selected = old ^ 1;
	pool->slot[old].owner.last_pool_use = transition->pool_point;
	pool->slot[old].owner.last_global_use = transition->secondary_point;
	pool->slot[old].owner.cpu_refs = 0;
	pool->slot[selected].owner.cpu_refs = 1;
	pool->active = selected;
	pool->cursor = 0;
	pool->serial = transition->serial;
	pool->timeline->submitted_point = transition->pool_point;
	pool->timeline->planned = false;
	pool->planned = false;
	memset(&pool->pending, 0, sizeof(pool->pending));
	return 0;
}

bool m9x_faa_secondary_pool_slot_retired(
	const struct m9x_faa_secondary_pool *pool, u32 index,
	const struct m9x_faa_secondary_pool_observation *observed)
{
	const struct m9x_faa_counted_secondary_owner *owner;

	if (!pool || !observed || !pool->initialized || pool->quarantined ||
	    !pool->timeline || pool->timeline->quarantined ||
	    pool->planned ||
	    index >= M9_FAA_SECONDARY_POOL_SLOTS || !pool->slot[index].bound ||
	    pool->active == index || observed->generation != pool->generation ||
	    observed->pool_point != pool->timeline->completed_point ||
	    !observed->resource_queries_retired)
		return false;
	owner = &pool->slot[index].owner;
	return !owner->cpu_refs &&
		owner->last_pool_use <= observed->pool_point &&
		owner->last_global_use <= observed->global_point;
}

void m9x_faa_secondary_pool_quarantine(struct m9x_faa_secondary_pool *pool)
{
	if (pool && pool->initialized)
		pool->timeline->quarantined = true;
	if (pool && pool->initialized)
		pool->quarantined = true;
}
