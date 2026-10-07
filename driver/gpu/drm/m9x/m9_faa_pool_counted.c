/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/string.h>
#include "m9_faa_pool_counted.h"

static int owner_bounds(const struct m9x_faa_counted_owner *o, u32 generation)
{
 if (!o->token || o->generation != generation) return -ESTALE;
 if (!o->base || !o->bytes || ((o->base | o->bytes) & 4095) ||
     (o->cursor & 3) || o->cursor > o->bytes ||
     (u64)o->base + o->bytes > 0x100000000ULL) return -ERANGE;
 return 0;
}

static bool separate(const struct m9x_faa_counted_owner *a,
                     const struct m9x_faa_counted_owner *b)
{
 return a->token != b->token &&
  ((u64)a->base + a->bytes <= b->base ||
   (u64)b->base + b->bytes <= a->base);
}

int m9x_faa_counted_init(struct m9x_faa_counted_pool *pool,
 const struct m9x_faa_counted_ops *ops, void *context,
 const struct m9x_faa_counted_owner *parent,
 const struct m9x_faa_counted_owner *secondary,
 u64 counter_bus, u64 origin, u64 idle_point,
 const struct m9x_faa_counted_receipt *receipt)
{
 struct m9x_faa_counted_pool next;
 int ret;
 if (!pool || !ops || !parent || !secondary || !receipt ||
     !ops->validate || !ops->retain || !ops->release || !ops->write ||
     !ops->publish_barrier || !ops->post) return -EINVAL;
 if (pool->initialized) return -EBUSY;
 if (!origin || (origin & 15) || (u32)origin != (receipt->raw_pool20 & ~15u) ||
     !idle_point || (idle_point & 15) || receipt->completed_use < idle_point ||
     counter_bus > 0xffffffffULL || (counter_bus & 4095) != 0x20)
  return -EINVAL;
 ret = owner_bounds(parent, receipt->generation);
 if (!ret) ret = owner_bounds(secondary, receipt->generation);
 if (ret) return ret;
 if (!separate(parent, secondary) || parent->last_use > receipt->completed_use ||
     secondary->last_use > receipt->completed_use || parent->last_pool > origin ||
     secondary->last_pool > origin) return -EBUSY;
 ret = ops->validate(context, parent);
 if (!ret) ret = ops->validate(context, secondary);
 if (ret) return ret;
 ret = ops->retain(context, parent->token);
 if (ret) return ret;
 ret = ops->retain(context, secondary->token);
 if (ret) { ops->release(context, parent->token); return ret; }
 next = (struct m9x_faa_counted_pool){
  .parent = *parent, .active = *secondary, .ops = ops, .context = context,
  .generation = receipt->generation, .counter_bus = counter_bus,
  .submitted_pool = origin, .observed_pool = origin,
  .completed_use = receipt->completed_use, .frontier = idle_point,
  .initialized = true };
 *pool = next;
 return 0;
}

int m9x_faa_counted_rotate(struct m9x_faa_counted_pool *pool,
 const struct m9x_faa_counted_input *in)
{
 struct m9x_faa_counted_receipt receipt = { };
 struct m9x_faa_counted_owner old;
 u32 secondary[3], parent[6], parent_end, secondary_end;
 u64 point;
 bool posted = false;
 int ret;
 if (!pool || !in || !pool->initialized) return -EINVAL;
 if (pool->quarantined) return -EIO;
 if (in->generation != pool->generation) return -ESTALE;
 if (pool->retiring.token || pool->completed_use < pool->frontier ||
     in->idle_point != pool->frontier) return -EBUSY;
 /* Current qualified native seam is authenticated idle/Unlock: there can
  * be no unposted shader/draw bytes. Close only the freshly written footer,
  * never execute an old consumed range again to obtain a pool receipt. */
 if (in->range_start != pool->active.cursor || in->next.cursor)
  return -EINVAL;
 if (pool->frontier > ~(u64)0 - 16 ||
     pool->submitted_pool > ~(u64)0 - 16) return -EOVERFLOW;
 if (in->finish_point != pool->frontier + 16) return -EINVAL;
 ret = owner_bounds(&pool->parent, pool->generation);
 if (!ret) ret = owner_bounds(&pool->active, pool->generation);
 if (!ret) ret = owner_bounds(&in->next, pool->generation);
 if (ret) return ret;
 if (!separate(&pool->parent, &in->next) ||
     !separate(&pool->active, &in->next) ||
     in->next.last_use > pool->completed_use ||
     in->next.last_pool > pool->observed_pool) return -EBUSY;
 /* Leave at least one DWORD after each end, as original guard accounting
  * does. END cannot alias the next allocation's first byte. */
 if ((u64)pool->parent.cursor + 24 >= pool->parent.bytes ||
     (u64)pool->active.cursor + 12 >= pool->active.bytes) return -ENOSPC;
 point = pool->submitted_pool + 16;
 if (point - pool->observed_pool >= 0x100000000ULL) return -EOVERFLOW;
 ret = pool->ops->validate(pool->context, &pool->parent);
 if (!ret) ret = pool->ops->validate(pool->context, &pool->active);
 if (!ret) ret = pool->ops->validate(pool->context, &in->next);
 if (ret) return ret;
 ret = pool->ops->retain(pool->context, in->next.token);
 if (ret) return ret;
 secondary_end = pool->active.cursor + 12;
 parent_end = pool->parent.cursor + 24;
 secondary[0] = 0x4545e3e6; secondary[1] = 1;
 secondary[2] = (u32)in->finish_point | 1;
 parent[0] = 0x45452d2c;
 parent[1] = (pool->active.base + in->range_start) | 1;
 /* mode1 flags0: original24c73 gives end-tag0, not draw-range tag2. */
 parent[2] = pool->active.base + secondary_end;
 parent[3] = 0x4545421a;
 parent[4] = (u32)pool->counter_bus | 2;
 parent[5] = (u32)point | 1;
 ret = pool->ops->write(pool->context, pool->active.token,
                         pool->active.cursor, secondary, 3);
 if (!ret) ret = pool->ops->write(pool->context, pool->parent.token,
                                 pool->parent.cursor, parent, 6);
 if (ret) goto unposted;
 pool->ops->publish_barrier(pool->context);
 ret = pool->ops->post(pool->context, pool->parent.base + parent_end,
                      in->finish_point, &posted, &receipt);
 if (!posted) { if (!ret) ret = -EIO; goto unposted; }
 /* END made the appended words live even on timeout/failure. Preserve all
  * owners and cursors, and never offer this old range to another writer. */
 old = pool->active;
 old.cursor = secondary_end; old.last_pool = point;
 old.last_use = in->finish_point;
 pool->retiring = old; pool->active = in->next;
 pool->parent.cursor = parent_end; pool->parent.last_use = in->finish_point;
 pool->submitted_pool = point; pool->frontier = in->finish_point;
 if (!ret && (receipt.generation != pool->generation ||
              receipt.completed_use < pool->frontier)) ret = -EIO;
 if (ret) { pool->quarantined = true; return ret; }
 if (receipt.completed_use > pool->completed_use)
  pool->completed_use = receipt.completed_use;
 /* A returned global finish precedes the parent's pool-counter packet.
  * Do not interpret post()'s raw20, or success, as pool retirement. */
 return 0;
unposted:
 pool->ops->release(pool->context, in->next.token);
 return ret;
}

int m9x_faa_counted_observe(struct m9x_faa_counted_pool *pool,
 const struct m9x_faa_counted_receipt *receipt)
{
 u64 observed;
 void *token;
 if (!pool || !receipt || !pool->initialized) return -EINVAL;
 if (pool->quarantined) return -EIO;
 if (receipt->generation != pool->generation) return -ESTALE;
 if (receipt->completed_use < pool->completed_use) return -EAGAIN;
 /* Only the low pool lane20 has a proved GPU producer. Its unique full64
  * interpretation is bounded by this manager's observed/submitted window,
  * never lane24 or a uint32 completion comparison. */
 /* Original4e332 discards low marker bits before extending its software
  * high word. A GPU-written raw21 is the same logical pool point20. */
 observed = (pool->observed_pool & ~0xffffffffULL) |
            (receipt->raw_pool20 & ~15u);
 if (observed < pool->observed_pool) {
  if (observed > ~(u64)0 - 0x100000000ULL) return -EOVERFLOW;
  observed += 0x100000000ULL;
 }
 if (observed > pool->submitted_pool) return -ERANGE;
 pool->observed_pool = observed;
 pool->completed_use = receipt->completed_use;
 if (pool->retiring.token && observed >= pool->retiring.last_pool &&
     receipt->completed_use >= pool->retiring.last_use) {
  token = pool->retiring.token;
  memset(&pool->retiring, 0, sizeof(pool->retiring));
  pool->ops->release(pool->context, token);
 }
 return 0;
}

int m9x_faa_counted_idle_update(struct m9x_faa_counted_pool *pool,
 u32 parent_cursor, u32 secondary_cursor, u64 unlock_point,
 const struct m9x_faa_counted_receipt *receipt)
{
 struct m9x_faa_counted_owner parent, secondary;
 int ret;
 if (!pool || !receipt || !pool->initialized) return -EINVAL;
 if (pool->quarantined) return -EIO;
 if (receipt->generation != pool->generation) return -ESTALE;
 if (pool->retiring.token || receipt->completed_use < pool->completed_use ||
     receipt->completed_use < unlock_point) return -EBUSY;
 if ((unlock_point & 15) || unlock_point < pool->frontier ||
     parent_cursor < pool->parent.cursor ||
     secondary_cursor < pool->active.cursor) return -EINVAL;
 if (unlock_point == pool->frontier &&
     (parent_cursor != pool->parent.cursor ||
      secondary_cursor != pool->active.cursor)) return -EINVAL;
 parent = pool->parent; secondary = pool->active;
 parent.cursor = parent_cursor; secondary.cursor = secondary_cursor;
 ret = owner_bounds(&parent, pool->generation);
 if (!ret) ret = owner_bounds(&secondary, pool->generation);
 if (!ret) ret = pool->ops->validate(pool->context, &parent);
 if (!ret) ret = pool->ops->validate(pool->context, &secondary);
 if (ret) return ret;
 parent.last_use = secondary.last_use = unlock_point;
 pool->parent = parent; pool->active = secondary;
 pool->frontier = unlock_point; pool->completed_use = receipt->completed_use;
 return 0;
}

int m9x_faa_counted_fini(struct m9x_faa_counted_pool *pool, bool dma_stopped)
{
 if (!pool || !pool->initialized) return -EINVAL;
 if (!dma_stopped && (pool->quarantined || pool->retiring.token ||
                     pool->completed_use < pool->frontier)) return -EBUSY;
 if (pool->retiring.token) pool->ops->release(pool->context, pool->retiring.token);
 pool->ops->release(pool->context, pool->active.token);
 pool->ops->release(pool->context, pool->parent.token);
 memset(pool, 0, sizeof(*pool));
 return 0;
}
