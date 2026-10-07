/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_COUNTED_SECONDARY_ENCODE_H
#define M9_FAA_COUNTED_SECONDARY_ENCODE_H

#include <linux/types.h>

/* Original M9128 mode-1, flags-0 counted-pool close. These are live retained
 * owner identities and mapped GPU addresses, never captured addresses. */
struct m9x_faa_counted_secondary_owner {
	u64 identity;
	u64 last_pool_use;
	u64 last_global_use;
	u32 gpu_base;
	u32 bytes;
	u32 cpu_refs;
	bool mapped;
};

struct m9x_faa_counted_secondary_request {
	struct m9x_faa_counted_secondary_owner old, selected;
	u32 range_start, range_end;
	u32 parent_cursor, parent_capacity;
	u64 global_origin, pool_origin;
	u64 observed_pool_point, observed_global_point;
	u64 fence_bus;
	u32 generation, observed_generation;
	u32 mode, flags;
	bool resource_queries_retired;
};

struct m9x_faa_counted_secondary_image {
	u32 old_footer[3];
	u32 parent[6];
	u64 secondary_point, pool_point;
	u32 parent_next;
};

/* Pure admission and encoding. The caller must authenticate the graph and
 * queue, reserve distinct owners, write the old footer/parent in source order,
 * attach fences, post the parent END, and retain both owners through actual
 * retirement or quarantine. Success here is not a GPU completion receipt. */
int m9x_faa_counted_secondary_encode(
	const struct m9x_faa_counted_secondary_request *request,
	struct m9x_faa_counted_secondary_image *image);

#endif
