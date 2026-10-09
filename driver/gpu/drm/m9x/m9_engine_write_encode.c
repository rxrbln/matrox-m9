// SPDX-License-Identifier: GPL-2.0-only
// Copyright 2026 René Rebe <rene@exactco.de>
#include <linux/array_size.h>
#include <linux/errno.h>
#include "m9_engine_write_encode.h"
#include "m9_engine_2d_state.h"
#include "m9_hw_encode.h"

/*
 * The vendor upload block (emulated memup 320x240 and 256x256 levels 0..8,
 * primary 0x434..0x116c) is the resource unlock's colour-target rectangle
 * with source and target swapped: the linear shadow at unit 0 as a kind 00
 * raw32 surface, the tiled target at b0/b1, f0 080c040c, the origins in
 * f2/f3 and fc launching with the negated extent.
 */
#define M9_WRITE_COPY_F0	0x080c040cu
#define M9_WRITE_COPY_EF	0x000000aau
#define M9_WRITE_FORMAT_RAW32	0x78u
#define M9_WRITE_KIND_LINEAR	0x00u
#define M9_WRITE_KIND_SMALL	0x01u
#define M9_WRITE_KIND_LARGE	0x21u
#define M9_WRITE_LAYERS		1u
/* Descriptor bits below the format code the target keeps (b1 = f0210140). */
#define M9_WRITE_VIEW_MASK	0x00ffffffu
#define M9_WRITE_SPAN_QUANTUM	32u
#define M9_WRITE_MAX_SPAN	2048u
#define M9_WRITE_AD		0x00000020u
#define M9_WRITE_CLIP_MAX	0x7fff7fffu
#define M9_WRITE_GUARD		0x40004000u
#define M9_WRITE_TAG_LOCAL	1u
#define M9_WRITE_TAG_BUS	2u
#define M9_WRITE_E6_TOKEN	0x00000001u
#define M9_WRITE_E6_BANK	0x04000001u
#define M9_WRITE_BANK_STATE	0x80000600u
#define M9_WRITE_SEQ0_BANK0	0x007ff000u
#define M9_WRITE_SEQ0_BANK2	0xffffffffu
#define M9_WRITE_TOKEN_STEP	0x10u
/* Two serialising tokens and the launch's precede the final target. */
#define M9_WRITE_TOKENS		3u
#define M9_WRITE_PAGE		0x1000u
#define M9_WRITE_DEST_ALIGN	0x100u
#define M9_WRITE_COPY_WORDS	(4 + 5 + 3 + 3 + 5 + 2 + 5 + 2 + 4 + 4 + 5 + \
				 5 + 2 + 2)
#define M9_WRITE_TAIL_WORDS	(5 + 4 + 5 + 7 + 3)

u32 m9x_engine_write_words(void)
{
	return 2 + 2 * 5 + m9x_engine_2d_state_words() + M9_WRITE_COPY_WORDS +
	       M9_WRITE_TAIL_WORDS;
}

/* Packed 32-bit colour codes: RGBA8/RGBX8, A8R8G8B8 and the 10-bit pair. */
static bool m9_write_code_packed32(u32 code)
{
	return code == 0x64 || code == 0x66 || code == 0x68 || code == 0x6a;
}

static bool m9_write_request_valid(const struct m9x_engine_write_request *r)
{
	u32 d0 = r->descriptor0, d1 = r->descriptor1;
	u32 kind = (d0 >> 16) & 0xff, span = d0 & 0xffff;

	return r->dest && !(r->dest % M9_WRITE_DEST_ALIGN) &&
	       m9_write_code_packed32(d0 >> 25) &&
	       (kind == M9_WRITE_KIND_SMALL || kind == M9_WRITE_KIND_LARGE) &&
	       span && span <= M9_WRITE_MAX_SPAN &&
	       !(span % M9_WRITE_SPAN_QUANTUM) &&
	       !(d1 & 0xf0000000u) && ((d1 >> 16) & 0xfffu) == 1 &&
	       r->width && r->height &&
	       (u64)r->x + r->width <= span &&
	       (u64)r->y + r->height <= (d1 & 0xffffu) &&
	       r->source && !(r->source % M9_WRITE_PAGE) &&
	       (u64)r->source + r->source_bytes <= 0x100000000ULL &&
	       r->fence_bus && !(r->fence_bus & 3) && !(r->token & 0xf);
}

static u32 m9_write_extent(u32 rows, u32 width)
{
	return ((0u - rows) & 0xffffu) << 16 | ((0u - width) & 0xffffu);
}

static u32 m9_write_token(u32 *w, u32 token, bool wait)
{
	u32 n = 0;

	w[n++] = M9_HW_HDR2(0xe6, 0xe3);
	w[n++] = M9_WRITE_E6_TOKEN;
	w[n++] = token | 1;
	if (wait) {
		w[n++] = M9_HW_HDR1(0x43);
		w[n++] = token;
	}
	return n;
}

int m9x_engine_write_encode(const struct m9x_engine_write_request *r,
			    u32 *words, u32 capacity,
			    struct m9x_engine_write_plan *plan)
{
	u32 n = 0, i, pitch, held, rows, extent, used, t0, final;

	if (!r || !words || !plan || !m9_write_request_valid(r))
		return -EINVAL;
	/* The source is shaped like the target, as the vendor shadow is. */
	pitch = (r->descriptor0 & 0xffff) * 4;
	/* 7a states the rows the staging holds, as for the vendor shadow. */
	held = r->source_bytes / pitch;
	if (!held)
		return -EINVAL;
	if (held > M9X_ENGINE_WRITE_MAX_ROWS)
		held = M9X_ENGINE_WRITE_MAX_ROWS;
	rows = held < r->height ? held : r->height;
	if (capacity < m9x_engine_write_words())
		return -ENOSPC;
	t0 = r->token;
	if (t0 > 0xfffffff0u - M9_WRITE_TOKENS * M9_WRITE_TOKEN_STEP)
		return -ERANGE;
	final = t0 + M9_WRITE_TOKENS * M9_WRITE_TOKEN_STEP;
	extent = m9_write_extent(rows, r->width);

	/* Writeback owner of every |1 token. */
	words[n++] = M9_HW_HDR1(0x1b);
	words[n++] = r->fence_bus | M9_WRITE_TAG_BUS;
	/* Every job posted before retires before any state changes. */
	n += m9_write_token(words + n, t0, true);
	n += m9_write_token(words + n, t0 + M9_WRITE_TOKEN_STEP, true);
	if (m9x_engine_2d_state(words + n, capacity - n,
				t0 + M9_WRITE_TOKEN_STEP, &used))
		return -ENOSPC;
	n += used;
	words[n++] = M9_HW_HDR3(0xf0, 0xef, 0xd7);
	words[n++] = M9_WRITE_COPY_F0;
	words[n++] = M9_WRITE_COPY_EF;
	words[n++] = 0;
	/* Source origin 0: the batch is staged from the image's first byte. */
	words[n++] = M9_HW_HDR(0xec, 0xf2, 0xf3, 0xf4);
	words[n++] = 0;
	words[n++] = 0;
	words[n++] = r->y << 16 | r->x;
	words[n++] = extent;
	words[n++] = M9_HW_HDR2(0xb0, 0xb1);
	words[n++] = r->dest | M9_WRITE_TAG_LOCAL;
	words[n++] = M9_WRITE_FORMAT_RAW32 << 25 |
		     (r->descriptor0 & M9_WRITE_VIEW_MASK);
	words[n++] = M9_HW_HDR2(0xb8, 0xb9);
	words[n++] = 0;
	words[n++] = 0;
	words[n++] = M9_HW_HDR(0xad, 0xad, 0xad, 0xad);
	for (i = 0; i < 4; i++)
		words[n++] = M9_WRITE_AD;
	words[n++] = M9_HW_HDR1(0xaf);
	words[n++] = 0;
	words[n++] = M9_HW_HDR(0xf5, 0xf6, 0xc4, 0xc5);
	words[n++] = 0;
	words[n++] = M9_WRITE_CLIP_MAX;
	words[n++] = M9_WRITE_GUARD;
	words[n++] = M9_WRITE_GUARD;
	/* Unit 0 reads the staged rows through their class-1 leaves. */
	words[n++] = M9_HW_HDR1(0x70);
	words[n++] = 0;
	words[n++] = M9_HW_HDR3(0x78, 0x79, 0x7a);
	words[n++] = r->source | M9_WRITE_TAG_LOCAL;
	words[n++] = M9_WRITE_FORMAT_RAW32 << 25 | M9_WRITE_KIND_LINEAR << 16 |
		     (r->descriptor0 & 0xffff);
	words[n++] = M9_WRITE_LAYERS << 16 | held;
	words[n++] = M9_HW_HDR3(0x7c, 0x7d, 0x87);
	words[n++] = 0;
	words[n++] = 0;
	words[n++] = 0;
	words[n++] = M9_HW_HDR(0x82, 0x83, 0x84, 0x85);
	for (i = 0; i < 4; i++)
		words[n++] = 0;
	words[n++] = M9_HW_HDR(0x80, 0x81, 0x7e, 0x7f);
	for (i = 0; i < 4; i++)
		words[n++] = 0;
	words[n++] = M9_HW_HDR1(0xf0);
	words[n++] = M9_WRITE_COPY_F0;
	words[n++] = M9_HW_HDR1(0xfc);
	words[n++] = extent;
	/* The copy retires before unit 0 and the origins are cleared. */
	n += m9_write_token(words + n, t0 + 2 * M9_WRITE_TOKEN_STEP, true);
	words[n++] = M9_HW_HDR3(0x78, 0x79, 0x7a);
	words[n++] = 0;
	words[n++] = 0;
	words[n++] = 0;
	words[n++] = M9_HW_HDR(0xec, 0xf2, 0xf3, 0xf4);
	for (i = 0; i < 4; i++)
		words[n++] = 0;
	words[n++] = M9_HW_HDR(0xe6, 0x8f, 0x90, 0x91);
	words[n++] = M9_WRITE_E6_BANK;
	words[n++] = M9_WRITE_BANK_STATE;
	words[n++] = M9_WRITE_SEQ0_BANK0;
	words[n++] = 0;
	words[n++] = M9_HW_HDR1(0x92);
	words[n++] = M9_WRITE_SEQ0_BANK2;
	words[n++] = M9_HW_HDR2(0xe6, 0xe3);
	words[n++] = M9_HW_E6_STEP;
	words[n++] = final | 1;

	plan->dwords = n;
	plan->rows = rows;
	plan->pitch = pitch;
	plan->final = final;
	return 0;
}
