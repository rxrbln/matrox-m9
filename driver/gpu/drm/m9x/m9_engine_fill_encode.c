// SPDX-License-Identifier: GPL-2.0-only
// Copyright 2026 René Rebe <rene@exactco.de>
#include <linux/array_size.h>
#include <linux/errno.h>
#include <linux/math.h>
#include "m9_engine_fill_encode.h"
#include "m9_engine_2d_state.h"
#include "m9_hw_encode.h"

/*
 * Colour-target copy of the vendor resource unlock (MxMD.dll 0x55a40 ->
 * 0x1b650, emulated 320x240x32, primary 0x434..0x116c): tiled source in
 * unit 0, linear target in b0/b1, f0 280c040c, ef aa, launched by fc with
 * the negated extent; the upload variant of the same block uses 080c040c.
 */
#define M9_FILL_COPY_F0		0x280c040cu
#define M9_FILL_COPY_EF		0x000000aau
#define M9_FILL_FORMAT_RAW32	0x78u
#define M9_FILL_KIND_TILED	0x21u
#define M9_FILL_KIND_LINEAR	0x00u
#define M9_FILL_LAYERS		1u
/*
 * What that block sets beyond the shared 2-D context: no depth target,
 * the multisample pair as sequence0 and Mesa's preprocessing leave it,
 * and the guard band in c4 as well as c5.
 */
#define M9_FILL_AD		0x00000020u
#define M9_FILL_CLIP_MAX	0x7fff7fffu
#define M9_FILL_GUARD		0x40004000u
#define M9_FILL_TAG_LOCAL	1u
#define M9_FILL_TAG_BUS		2u
/*
 * e6=1 + e3 token|1, then 43 on it: the readback's serialisation (DdBlt
 * capture 0x1114..0x113c); the unlock emits the same e3 pair unwaited.
 */
#define M9_FILL_E6_TOKEN	0x00000001u
#define M9_FILL_E6_BANK		0x04000001u
/* Completion range of every ordinary job (m9_triangle_external footer). */
#define M9_FILL_E6_FOOTER	0x00000002u
/* The 0x600 bank sequence0 leaves and no ordinary job restates. */
#define M9_FILL_BANK_STATE	0x80000600u
#define M9_FILL_SEQ0_BANK0	0x007ff000u
#define M9_FILL_SEQ0_BANK2	0xffffffffu
#define M9_FILL_TOKEN_STEP	0x10u
/* Every captured b0 and 78 address is page aligned. */
#define M9_FILL_ALIGN		0x1000u
#define M9_FILL_HEAD_WORDS	(2 + 2 * 5)
#define M9_FILL_COPY_WORDS	(4 + 5 + 3 + 3 + 5 + 2 + 5 + 20 + 2)
#define M9_FILL_TAIL_WORDS	(7 + 2 + 4 + 2)

u32 m9x_engine_fill_capacity(u32 launches)
{
	return M9_FILL_HEAD_WORDS + m9x_engine_2d_state_words() +
	       M9_FILL_COPY_WORDS + 5 + M9_FILL_TAIL_WORDS +
	       (launches ? launches - 1 : 0) * M9X_ENGINE_FILL_LAUNCH_WORDS;
}

static u32 m9_fill_extent(u32 rows, u32 span)
{
	return ((0u - rows) & 0xffffu) << 16 | ((0u - span) & 0xffffu);
}

static u32 m9_fill_token(u32 *w, u32 token, bool wait)
{
	u32 n = 0;

	w[n++] = M9_HW_HDR2(0xe6, 0xe3);
	w[n++] = M9_FILL_E6_TOKEN;
	w[n++] = token | 1;
	if (wait) {
		w[n++] = M9_HW_HDR1(0x43);
		w[n++] = token;
	}
	return n;
}

static bool m9_fill_request_valid(const struct m9x_engine_fill_request *r,
				  u32 row_bytes)
{
	return r->span >= 32 && r->span <= M9X_ENGINE_FILL_MAX_SPAN &&
	       !(r->span % 32) && r->bytes && !(r->bytes % row_bytes) &&
	       r->dest && !(r->dest % M9_FILL_ALIGN) &&
	       (u64)r->dest + r->bytes <= 0x100000000ULL &&
	       r->source && !(r->source % M9_FILL_ALIGN) &&
	       r->source_rows && r->source_rows <= M9X_ENGINE_FILL_MAX_ROWS &&
	       r->fence_bus && !(r->fence_bus & 3) && !(r->token & 0xf);
}

int m9x_engine_fill_encode(const struct m9x_engine_fill_request *r,
			   u32 *words, u32 capacity,
			   struct m9x_engine_fill_plan *plan)
{
	u32 row_bytes, rows, launches, step, n = 0, i, done, t1, t2, final;
	u32 d1, used;

	if (!r || !words || !plan)
		return -EINVAL;
	row_bytes = r->span * 4;
	if (!m9_fill_request_valid(r, row_bytes))
		return -EINVAL;
	rows = r->bytes / row_bytes;
	step = r->source_rows;
	launches = DIV_ROUND_UP(rows, step);
	/* A later launch rebases b0, which has to stay page aligned. */
	if (launches > 1 && ((u64)step * row_bytes) % M9_FILL_ALIGN)
		return -EINVAL;
	if (capacity < m9x_engine_fill_capacity(launches))
		return -ENOSPC;
	t1 = r->token;
	t2 = t1 + M9_FILL_TOKEN_STEP;
	if ((u64)t1 + (2 + (u64)launches) * M9_FILL_TOKEN_STEP > 0xfffffff0u)
		return -ERANGE;
	final = t1 + (2 + launches) * M9_FILL_TOKEN_STEP;
	d1 = M9_FILL_FORMAT_RAW32 << 25 | M9_FILL_KIND_LINEAR << 16 | r->span;

	/* Writeback owner of every |1 token, as the completion footer binds it. */
	words[n++] = M9_HW_HDR1(0x1b);
	words[n++] = r->fence_bus | M9_FILL_TAG_BUS;
	/* Every job posted before retires before any state changes. */
	n += m9_fill_token(words + n, t1, true);
	n += m9_fill_token(words + n, t2, true);
	if (m9x_engine_2d_state(words + n, capacity - n, t2, &used))
		return -ENOSPC;
	n += used;
	words[n++] = M9_HW_HDR3(0xf0, 0xef, 0xd7);
	words[n++] = M9_FILL_COPY_F0;
	words[n++] = M9_FILL_COPY_EF;
	words[n++] = 0;
	words[n++] = M9_HW_HDR(0xec, 0xf2, 0xf3, 0xf4);
	words[n++] = 0;
	words[n++] = 0;
	words[n++] = 0;
	words[n++] = m9_fill_extent(rows < step ? rows : step, r->span);
	words[n++] = M9_HW_HDR2(0xb0, 0xb1);
	words[n++] = r->dest | M9_FILL_TAG_LOCAL;
	words[n++] = d1;
	words[n++] = M9_HW_HDR2(0xb8, 0xb9);
	words[n++] = 0;
	words[n++] = 0;
	words[n++] = M9_HW_HDR(0xad, 0xad, 0xad, 0xad);
	for (i = 0; i < 4; i++)
		words[n++] = M9_FILL_AD;
	words[n++] = M9_HW_HDR1(0xaf);
	words[n++] = 0;
	words[n++] = M9_HW_HDR(0xf5, 0xf6, 0xc4, 0xc5);
	words[n++] = 0;
	words[n++] = M9_FILL_CLIP_MAX;
	words[n++] = M9_FILL_GUARD;
	words[n++] = M9_FILL_GUARD;
	/* Unit 0 samples the zeroed source, packet for packet as the unlock. */
	words[n++] = M9_HW_HDR1(0x70);
	words[n++] = 0;
	words[n++] = M9_HW_HDR3(0x78, 0x79, 0x7a);
	words[n++] = r->source | M9_FILL_TAG_LOCAL;
	words[n++] = M9_FILL_FORMAT_RAW32 << 25 | M9_FILL_KIND_TILED << 16 |
		     r->span;
	words[n++] = M9_FILL_LAYERS << 16 | r->source_rows;
	words[n++] = M9_HW_HDR3(0x7c, 0x7d, 0x87);
	words[n++] = 0;
	words[n++] = 0;
	words[n++] = 0;
	words[n++] = M9_HW_HDR(0x82, 0x83, 0x84, 0x85);
	words[n++] = 0;
	words[n++] = 0;
	words[n++] = 0;
	words[n++] = 0;
	words[n++] = M9_HW_HDR(0x80, 0x81, 0x7e, 0x7f);
	words[n++] = 0;
	words[n++] = 0;
	words[n++] = 0;
	words[n++] = 0;
	words[n++] = M9_HW_HDR1(0xf0);
	words[n++] = M9_FILL_COPY_F0;
	for (i = 0, done = 0; i < launches; i++) {
		u32 count = rows - done < step ? rows - done : step;
		u32 extent = m9_fill_extent(count, r->span);

		if (i) {
			words[n++] = M9_HW_HDR3(0xb0, 0xb1, 0xf4);
			words[n++] = (r->dest + done * row_bytes) |
				     M9_FILL_TAG_LOCAL;
			words[n++] = d1;
			words[n++] = extent;
		}
		words[n++] = M9_HW_HDR1(0xfc);
		words[n++] = extent;
		n += m9_fill_token(words + n, t1 + (2 + i) * M9_FILL_TOKEN_STEP,
				   false);
		done += count;
	}
	words[n++] = M9_HW_HDR(0xe6, 0x8f, 0x90, 0x91);
	words[n++] = M9_FILL_E6_BANK;
	words[n++] = M9_FILL_BANK_STATE;
	words[n++] = M9_FILL_SEQ0_BANK0;
	words[n++] = 0;
	words[n++] = M9_HW_HDR1(0x92);
	words[n++] = M9_FILL_SEQ0_BANK2;
	/* The ordinary completion range: the next job starts after the copy. */
	words[n++] = M9_HW_HDR1(0x1b);
	words[n++] = r->fence_bus | M9_FILL_TAG_BUS;
	words[n++] = M9_HW_HDR3(0xe6, 0xe6, 0xe3);
	words[n++] = M9_HW_E6_STEP;
	words[n++] = M9_FILL_E6_FOOTER;
	words[n++] = final | 1;
	words[n++] = M9_HW_HDR1(0x43);
	words[n++] = final;

	plan->dwords = n;
	plan->launches = launches;
	plan->rows = rows;
	plan->final = final;
	return 0;
}
