/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_PRIMARY_POOL_H
#define M9_FAA_PRIMARY_POOL_H

#include "m9_faa_secondary_pool.h"

/* Primary and counted-secondary owners obey the same distinct-slot and
 * full-64 retirement rules. Only the source packet and END differ. */
typedef struct m9x_faa_secondary_pool m9x_faa_primary_pool;
typedef struct m9x_faa_secondary_pool_transition m9x_faa_primary_transition;

struct m9x_faa_primary_pool_image {
	u32 old_link[4];
	u32 new_end, old_next;
	u64 pool_point;
};

/* Use m9x_faa_secondary_pool_init/bind/advance/observe_raw/slot_retired for
 * live mapped primary owners. This mode-0 planner appends the source link at
 * the old cursor. Its END is the selected owner's base, not old_next. */
int m9x_faa_primary_pool_plan(m9x_faa_primary_pool *pool,
	u64 global_last_submitted, u64 pool_origin, u64 fence_bus,
	const struct m9x_faa_secondary_pool_observation *observed,
	struct m9x_faa_primary_pool_image *image,
	m9x_faa_primary_transition *transition);

/* Abort only before any END post; after a post, retain both owners and use
 * the posted transition. On ambiguous post failure quarantine the pool. */
static inline int m9x_faa_primary_pool_abort(m9x_faa_primary_pool *pool,
	const m9x_faa_primary_transition *transition)
{
	return m9x_faa_secondary_pool_abort(pool, transition);
}
static inline int m9x_faa_primary_pool_posted(m9x_faa_primary_pool *pool,
	const m9x_faa_primary_transition *transition)
{
	return m9x_faa_secondary_pool_posted(pool, transition);
}

#endif
