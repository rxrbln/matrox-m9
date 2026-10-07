/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_POOL_COUNTED_H
#define M9_FAA_POOL_COUNTED_H

/* Original249a0(mode1, flags0),25370,24060,24b10,24d60. Native
 * adapters hold transport/file locks and the graph's BO reservations. Owner
 * tokens are real retained native allocations, never captured pointers.
 * This closes ONLY an unposted secondary range; it does not reset a primary,
 * program, constants, or already posted range. One retiring range is enough
 * for a two-pool producer: observe/reclaim before another rotation.
 */
struct m9x_faa_counted_owner {
 void *token;
 u32 base, bytes, cursor, generation;
 u64 last_use, last_pool;
};
struct m9x_faa_counted_receipt {
 u32 generation, raw_pool20;
 u64 completed_use;
};
struct m9x_faa_counted_ops {
 /* Must verify current mapping identity, flags30, extent and ownership. */
 int (*validate)(void *, const struct m9x_faa_counted_owner *);
 int (*retain)(void *, void *);
 /* Transfer this reference to native reclamation outside the transport
  * lock when necessary. The adapter may not unmap/free it prematurely. */
 void (*release)(void *, void *);
 int (*write)(void *, void *, u32, const u32 *, u32);
 void (*publish_barrier)(void *);
 /* Set posted at the actual END write even on failure. A successful return
  * includes a genuine full64 global receipt. raw20 is never a submission
  * receipt; pool observation is separately authenticated by observe(). */
 int (*post)(void *, u32, u64, bool *, struct m9x_faa_counted_receipt *);
};
struct m9x_faa_counted_pool {
 struct m9x_faa_counted_owner parent, active, retiring;
 const struct m9x_faa_counted_ops *ops;
 void *context;
 u32 generation;
 u64 counter_bus, submitted_pool, observed_pool, completed_use, frontier;
 bool initialized, quarantined;
};
struct m9x_faa_counted_input {
 struct m9x_faa_counted_owner next;
 u32 generation, range_start;
 u64 idle_point, finish_point;
};

int m9x_faa_counted_init(struct m9x_faa_counted_pool *,
 const struct m9x_faa_counted_ops *, void *,
 const struct m9x_faa_counted_owner *, const struct m9x_faa_counted_owner *,
 u64, u64, u64, const struct m9x_faa_counted_receipt *);
int m9x_faa_counted_rotate(struct m9x_faa_counted_pool *,
 const struct m9x_faa_counted_input *);
/* After a real application Unlock receipt, advance only the same retained
 * allocations' append cursors/frontier. The native caller authenticates the
 * receipt against its graph/token, not a user-provided counter. */
int m9x_faa_counted_idle_update(struct m9x_faa_counted_pool *, u32, u32, u64,
 const struct m9x_faa_counted_receipt *);
int m9x_faa_counted_observe(struct m9x_faa_counted_pool *,
 const struct m9x_faa_counted_receipt *);
/* dma_stopped is a real native teardown prerequisite, not an error fallback
 * to invent. Without it, all posts and pool stamps must genuinely retire. */
int m9x_faa_counted_fini(struct m9x_faa_counted_pool *, bool dma_stopped);
#endif
