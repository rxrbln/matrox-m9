// SPDX-License-Identifier: GPL-2.0-only
/* M9128 retained FAA code-page placement and C2 selector lowering. */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include "m9_faa_graph_program_arena.h"

/* f5778 emits the live page pointer with selector13 and the entry with
 * selector14. Declaration groups after byte 576 shift the later C2 pairs. */
static const u16 page_at[] = { 2340, 348, 976, 1588 };
static const u16 entry_at[] = { 2344, 352, 980, 1592 };
static const u16 two_page_at[] = { 1080, 304 };
static const u16 two_entry_at[] = { 1084, 308 };

static u32 site(const u16 *at, u32 k, int extra, bool two)
{
	u32 threshold = two ? 508 : 576;
	return at[k] >= threshold ? (u32)((int)at[k] + extra) : at[k];
}

static u32 get(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 |
	       (u32)p[3] << 24;
}

static void put(u8 *p, u32 v)
{
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static bool overlaps(const void *a, u32 an, const void *b, u32 bn)
{
	unsigned long x = (unsigned long)a, y = (unsigned long)b;

	return x <= y ? y - x < an : x - y < bn;
}

static int program_arena_encode(
	const struct m9x_faa_graph_draw_owner *owner,
	const struct m9x_faa_program_arena_job *job, u32 count,
	u8 *code_page, u32 code_capacity, u32 *slot_offset, bool two)
{
	const u16 *pages = two ? two_page_at : page_at;
	const u16 *entries = two ? two_entry_at : entry_at;
	u32 stages = two ? 2 : 4;
	u32 slot[M9_FAA_PROGRAM_ARENA_JOBS], end = 0, i, j, k;

	if (!owner || !job || !code_page || !slot_offset || !count ||
	    count > M9_FAA_PROGRAM_ARENA_JOBS ||
	    (owner->base & (M9_FAA_PROGRAM_PAGE_BYTES - 1)) ||
	    owner->bytes < M9_FAA_PROGRAM_PAGE_BYTES ||
	    code_capacity < M9_FAA_PROGRAM_PAGE_BYTES ||
	    overlaps(code_page, M9_FAA_PROGRAM_PAGE_BYTES, job,
		     count * sizeof(*job)) ||
	    overlaps(slot_offset, count * sizeof(*slot_offset), code_page,
		     M9_FAA_PROGRAM_PAGE_BYTES) ||
	    overlaps(slot_offset, count * sizeof(*slot_offset), job,
		     count * sizeof(*job)))
		return -EINVAL;
	for (i = 0; i < count; i++) {
		const struct m9x_faa_program_arena_job *q = &job[i];
		int extra;
		u32 expected_bytes;

		if (!q->program || !q->secondary || !q->program_bytes ||
		    (q->program_bytes & 7) ||
		    q->program_bytes > M9_FAA_PROGRAM_PAGE_BYTES ||
		    q->declaration_count < 3 || q->declaration_count > 17 ||
		    !(q->declaration_count & 1) ||
		    overlaps(code_page, M9_FAA_PROGRAM_PAGE_BYTES,
			     q->program, q->program_bytes) ||
		    overlaps(code_page, M9_FAA_PROGRAM_PAGE_BYTES,
			     q->secondary, q->secondary_bytes) ||
		    overlaps(slot_offset, count * sizeof(*slot_offset),
			     q->program, q->program_bytes) ||
		    overlaps(slot_offset, count * sizeof(*slot_offset),
			     q->secondary, q->secondary_bytes))
			return -EINVAL;
		extra = ((int)((q->declaration_count + 3) / 4) - (two ? 1 : 2)) * 24;
		expected_bytes = (two ? 1352 : 2712) + extra;
		if (q->secondary_bytes != expected_bytes ||
		    q->entry_bytes[0] ||
		    (two ? (q->entry_bytes[2] || q->entry_bytes[3]) :
		     (q->entry_bytes[3] > q->program_bytes ||
		      q->program_bytes - q->entry_bytes[3] != 200)))
			return -EINVAL;
		for (k = 0; k < stages; k++) {
			u32 p = site(pages, k, extra, two);
			u32 e = site(entries, k, extra, two);
			if ((q->entry_bytes[k] & 7) ||
			    q->entry_bytes[k] >= q->program_bytes ||
			    (k && q->entry_bytes[k] <= q->entry_bytes[k-1]) ||
			    get(q->secondary + p) != (owner->base | 1) ||
			    get(q->secondary + e) != q->entry_bytes[k] / 8)
				return -EINVAL;
		}
		for (j = 0; j < i; j++)
			if (overlaps(q->secondary, q->secondary_bytes,
				     job[j].secondary, job[j].secondary_bytes))
				return -EINVAL;
		for (j = 0; j < i; j++)
			if (job[j].program_bytes == q->program_bytes &&
			    !memcmp(job[j].program, q->program, q->program_bytes))
				break;
		if (j < i) {
			slot[i] = slot[j];
		} else {
			if (end > M9_FAA_PROGRAM_PAGE_BYTES - q->program_bytes)
				return -ERANGE;
			slot[i] = end;
			end += q->program_bytes;
		}
		for (k = 0; k < stages; k++)
			if ((slot[i] + q->entry_bytes[k]) / 8 >=
			    M9_FAA_PROGRAM_PAGE_BYTES / 8)
				return -ERANGE;
	}
	for (i = 0; i < count; i++) {
		const struct m9x_faa_program_arena_job *q = &job[i];

		if (i == 0 || slot[i] != slot[i-1])
			memcpy(code_page + slot[i], q->program, q->program_bytes);
		for (k = 0; k < stages; k++) {
			int extra = ((int)((q->declaration_count + 3) / 4) -
				     (two ? 1 : 2)) * 24;
			u32 e = site(entries, k, extra, two);
			put(q->secondary + e, (slot[i] + q->entry_bytes[k]) / 8);
		}
		slot_offset[i] = slot[i];
	}
	return 0;
}

int m9x_faa_program_arena_encode(
 const struct m9x_faa_graph_draw_owner *owner,
 const struct m9x_faa_program_arena_job *job, u32 count,
 u8 *code_page, u32 code_capacity, u32 *slot_offset)
{
 return program_arena_encode(owner, job, count, code_page, code_capacity,
                             slot_offset, false);
}

int m9x_faa_program_arena_two_encode(
 const struct m9x_faa_graph_draw_owner *owner,
 const struct m9x_faa_program_arena_job *job, u32 count,
 u8 *code_page, u32 code_capacity, u32 *slot_offset)
{
 return program_arena_encode(owner, job, count, code_page, code_capacity,
                             slot_offset, true);
}
