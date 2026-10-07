/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_SECONDARY_POOL_H
#define M9_FAA_SECONDARY_POOL_H

#include "m9_faa_counted_secondary_encode.h"

/* The native caller owns the actual DMA/WC allocation and GPUVA mapping.
 * This registry never frees an owner; both allocations remain pinned until
 * DMA stop or a separately qualified retirement/release path. */
#define M9_FAA_SECONDARY_POOL_SLOTS 2
struct m9x_faa_secondary_pool_slot {
	struct m9x_faa_counted_secondary_owner owner;
	bool bound;
};

struct m9x_faa_secondary_pool_transition {
	u64 serial;
	u64 old_identity, selected_identity;
	u64 secondary_point, pool_point;
	u32 generation, old_cursor, parent_next;
};

/* Primary, secondary, code and constant rotations share one M9128 +0x20
 * platform timeline. Keep it separate from the global +0x00 fence. */
struct m9x_faa_pool_timeline {
	u64 submitted_point, completed_point;
	u32 generation;
	bool initialized, planned, quarantined;
};

struct m9x_faa_secondary_pool {
	struct m9x_faa_secondary_pool_slot slot[M9_FAA_SECONDARY_POOL_SLOTS];
	struct m9x_faa_secondary_pool_transition pending;
	struct m9x_faa_pool_timeline *timeline;
	u64 serial;
	u32 generation, active, cursor;
	bool initialized, planned, quarantined;
};

struct m9x_faa_secondary_pool_observation {
	u64 global_point, pool_point;
	u32 generation;
	bool resource_queries_retired;
};

int m9x_faa_pool_timeline_init(struct m9x_faa_pool_timeline *timeline,
	u32 generation, u64 initial_point);
int m9x_faa_secondary_pool_init(struct m9x_faa_secondary_pool *pool,
	struct m9x_faa_pool_timeline *timeline,
	u32 generation, const struct m9x_faa_counted_secondary_owner *initial,
	u32 cursor);
int m9x_faa_secondary_pool_bind(struct m9x_faa_secondary_pool *pool,
	const struct m9x_faa_counted_secondary_owner *selected);
int m9x_faa_secondary_pool_advance(struct m9x_faa_secondary_pool *pool,
	u32 expected_cursor, u32 bytes);
/* raw20 is a genuine READ_ONCE of the retained coherent counter page at
 * +0x20; global_completed is the requester's separately authenticated
 * global completion. This function never reads or seeds hardware itself. */
int m9x_faa_secondary_pool_observe_raw(struct m9x_faa_secondary_pool *pool,
	u32 raw20, u64 global_completed, u32 generation, bool resources_idle,
	struct m9x_faa_secondary_pool_observation *observed);
int m9x_faa_secondary_pool_plan(struct m9x_faa_secondary_pool *pool,
	u32 range_start, u32 parent_cursor, u32 parent_capacity,
	u64 global_origin, u64 pool_origin, u64 fence_bus,
	const struct m9x_faa_secondary_pool_observation *observed,
	struct m9x_faa_counted_secondary_image *image,
	struct m9x_faa_secondary_pool_transition *transition);
int m9x_faa_secondary_pool_abort(struct m9x_faa_secondary_pool *pool,
	const struct m9x_faa_secondary_pool_transition *transition);
int m9x_faa_secondary_pool_posted(struct m9x_faa_secondary_pool *pool,
	const struct m9x_faa_secondary_pool_transition *transition);
bool m9x_faa_secondary_pool_slot_retired(
	const struct m9x_faa_secondary_pool *pool, u32 index,
	const struct m9x_faa_secondary_pool_observation *observed);
void m9x_faa_secondary_pool_quarantine(struct m9x_faa_secondary_pool *pool);

#endif
