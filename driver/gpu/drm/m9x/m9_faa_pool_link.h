/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_POOL_LINK_H
#define M9_FAA_POOL_LINK_H

/* Address-free producer state supplied by the retained native owner manager.
 * No captured address is used. The caller must validate mapping identities,
 * authentic idle graph/Unlock, and observed full64 points under transport and
 * owner reservations before publication. A successful plan is NOT a receipt.
 */
struct m9x_faa_pool_owner {
 u32 base, bytes, cursor, reserved_bytes, generation;
 u32 cpu_refs, pending_refs, system_flags30;
 u64 last_use, last_pool;
};
struct m9x_faa_pool_link_input {
 struct m9x_faa_pool_owner old, next;
 u32 generation;
 u64 pool_counter_bus, pool_origin, completed_use, completed_pool;
};
struct m9x_faa_pool_link_plan {
 u32 words[4], old_offset, old_end, next_guard, end_address;
 u64 point;
};

/* Original24060/241f2 primary-mode link, followed by original24530 END at
 * the selected pool base. Distinct secondary/code modes are not supported.
 * No command is posted, no owner/refcount is changed, and all errors preserve
 * output. Do not rewind the outgoing primary: its future real pool receipt
 * must retire this planned point before it may be selected again.
 */
static inline int m9x_faa_pool_link_plan(
 const struct m9x_faa_pool_link_input *in,
 struct m9x_faa_pool_link_plan *out)
{
 struct m9x_faa_pool_link_plan plan;
 const struct m9x_faa_pool_owner *old, *next;
 u64 old_end, next_end, point;
 if (!in || !out) return -EINVAL;
 old=&in->old;next=&in->next;
 if (old->generation!=in->generation || next->generation!=in->generation ||
     old->system_flags30!=1 || next->system_flags30!=1) return -ESTALE;
 if (!old->base || !next->base || !old->bytes || !next->bytes ||
     ((old->base|next->base|old->bytes|next->bytes)&4095) ||
     (old->cursor&3) || next->cursor ||
     (old->reserved_bytes&3) || (next->reserved_bytes&3) ||
     old->reserved_bytes>=old->bytes || next->reserved_bytes>=next->bytes)
  return -EINVAL;
 old_end=(u64)old->base+old->bytes;next_end=(u64)next->base+next->bytes;
 if (old_end>0x100000000ULL || next_end>0x100000000ULL ||
     old->cursor>old->bytes-old->reserved_bytes ||
     (u64)old->cursor+16>old->bytes ||
     !(old_end<=next->base || next_end<=old->base)) return -ERANGE;
 if (old->cpu_refs!=1 || next->cpu_refs || old->pending_refs || next->pending_refs ||
     old->last_use>in->completed_use || next->last_use>in->completed_use ||
     old->last_pool>in->completed_pool || next->last_pool>in->completed_pool)
  return -EBUSY;
 if (!in->pool_origin || (in->pool_origin&15) ||
     in->completed_pool>in->pool_origin ||
     in->pool_counter_bus>0xffffffffULL ||
     (in->pool_counter_bus&4095)!=0x20) return -EINVAL;
 if (in->pool_origin>~(u64)0-16) return -EOVERFLOW;
 point=in->pool_origin+16;
 plan=(struct m9x_faa_pool_link_plan){
  .words={0x453e421a,(u32)in->pool_counter_bus|2,(u32)point|1,next->base|1},
  .old_offset=old->cursor,.old_end=old->cursor+16,
  .next_guard=next->bytes-next->reserved_bytes,
  .end_address=next->base,.point=point};
 *out=plan;
 return 0;
}
#endif
