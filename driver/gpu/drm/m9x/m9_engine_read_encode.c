// SPDX-License-Identifier: GPL-2.0-only
// Copyright 2026 René Rebe <rene@exactco.de>
#include <linux/array_size.h>
#include <linux/errno.h>
#include <linux/string.h>
#include "m9_engine_read_encode.h"
#include "m9_engine_2d_state.h"
#include "m9_hw_encode.h"

/* MxMD.dll 0xb1621..0xb1634: copy control of a 16 or 32 bpp source. */
#define M9_READ_COPY_F0		0x280c1c0cu
/* Source view code the vendor gives an A8R8G8B8 target (0xb1456, 79[31:25]). */
#define M9_READ_FORMAT_A8R8G8B8	0x66u
#define M9_READ_FORMAT_RAW32	0x78u
#define M9_READ_KIND_MASK	0x01ff0000u
#define M9_READ_KIND_TILED	0x00210000u
/* Ring row quantum in pixels (+0x1a8), chunk alignment (+0x1b4), 0xb14f3. */
#define M9_READ_PITCH_QUANTUM	8u
#define M9_READ_RING_ALIGN	128u
#define M9_READ_CHUNK_ROWS	32u
#define M9_READ_E6_TOKEN	0x00000001u
#define M9_READ_E6_BANK		0x04000001u
#define M9_READ_BANK_CONST	0xc0000600u
#define M9_READ_BANK_STATE	0x80000600u
#define M9_READ_TAG_LOCAL	1u
#define M9_READ_TAG_BUS		2u
#define M9_READ_TOKEN_STEP	0x10u
/* Two serialising tokens precede the chunks (blt capture 0x1114..0x113c). */
#define M9_READ_FIRST_CHUNK	(3 * M9_READ_TOKEN_STEP)
/* Ordinary context values sequence0 leaves (m9_seq0_framed_payload). */
#define M9_READ_SEQ0_B7		0xcc010100u
#define M9_READ_SEQ0_BANK0	0x007ff000u
#define M9_READ_SEQ0_BANK2	0xffffffffu


static u32 m9_min(u32 a, u32 b)
{
	return a < b ? a : b;
}

static u32 m9_round_up(u32 value, u32 quantum)
{
	return (value + quantum - 1) / quantum * quantum;
}

static bool m9_ring_valid(const struct m9x_engine_read_ring *ring, u32 pitch)
{
	return ring->bus && !(ring->bus % M9_READ_RING_ALIGN) &&
	       ring->bytes >= pitch &&
	       (u64)ring->bus + ring->bytes <= 0x100000000ULL;
}

static bool m9_request_valid(const struct m9x_engine_read_request *r)
{
	u32 d0 = r->descriptor0, d1 = r->descriptor1;

	return r->source && !(r->source & 0xff) &&
	       d0 >> 25 == M9_READ_FORMAT_A8R8G8B8 &&
	       (d0 & M9_READ_KIND_MASK) == M9_READ_KIND_TILED &&
	       !(d1 & 0xf0000000u) && ((d1 >> 16) & 0xfffu) == 1 &&
	       r->width && r->height &&
	       (u64)r->x + r->width <= (d0 & 0xffffu) &&
	       (u64)r->y + r->height <= (d1 & 0xffffu) &&
	       r->fence_bus && !(r->fence_bus & 3) && !(r->token & 0xf);
}

int m9x_engine_read_encode(const struct m9x_engine_read_request *r,
			   u32 *words, u32 capacity,
			   struct m9x_engine_read_chunk *chunks, u32 max_chunks,
			   struct m9x_engine_read_plan *plan)
{
	u32 fixed = 2 + 4 + 2 * 5 + 5 + 3 + 4 + 3, cursor[M9X_ENGINE_READ_RINGS];
	u32 limit, pitch_px, pitch, row = 0, n = 0, k, i, t0, final;

	if (!r || !words || !chunks || !plan || !m9_request_valid(r))
		return -EINVAL;
	pitch_px = m9_round_up(r->width, M9_READ_PITCH_QUANTUM);
	pitch = pitch_px * 4;
	for (i = 0; i < M9X_ENGINE_READ_RINGS; i++)
		if (!m9_ring_valid(&r->ring[i], pitch))
			return -EINVAL;
	if (r->state)
		fixed += m9x_engine_2d_state_words() + 7;
	if (capacity < fixed + M9X_ENGINE_READ_CHUNK_WORDS || !max_chunks)
		return -ENOSPC;
	t0 = r->token;
	/* Room for one chunk below the last 32-bit target. */
	if (t0 > 0xfffffff0u - M9_READ_FIRST_CHUNK - M9_READ_TOKEN_STEP)
		return -ERANGE;
	limit = (capacity - fixed) / M9X_ENGINE_READ_CHUNK_WORDS;
	limit = m9_min(limit, max_chunks);
	limit = m9_min(limit, (0xfffffff0u - t0 - M9_READ_FIRST_CHUNK) /
		       M9_READ_TOKEN_STEP);

	/* Chunks alternate between the rings and pack each one (0xb1742). */
	memset(cursor, 0, sizeof(cursor));
	for (k = 0; k < limit && row < r->height; k++) {
		const struct m9x_engine_read_ring *ring =
			&r->ring[k % M9X_ENGINE_READ_RINGS];
		u32 *at = &cursor[k % M9X_ENGINE_READ_RINGS];
		u32 rows = m9_min(M9_READ_CHUNK_ROWS, ring->bytes / pitch);
		u32 offset = m9_round_up(*at, M9_READ_RING_ALIGN);

		rows = m9_min(rows, r->height - row);
		if (offset > ring->bytes || ring->bytes - offset < rows * pitch)
			break;
		chunks[k] = (struct m9x_engine_read_chunk) {
			.ring = k % M9X_ENGINE_READ_RINGS, .offset = offset,
			.row = row, .rows = rows,
		};
		*at = offset + rows * pitch;
		row += rows;
	}
	if (!k)
		return -ENOSPC;
	final = t0 + M9_READ_FIRST_CHUNK + k * M9_READ_TOKEN_STEP;

	/* Writeback owner of every |1 token, as the completion footer binds it. */
	words[n++] = M9_HW_HDR1(0x1b);
	words[n++] = r->fence_bus | M9_READ_TAG_BUS;
	if (r->state) {
		u32 used;

		if (m9x_engine_2d_state(words + n, capacity - n, t0, &used))
			return -ENOSPC;
		n += used;
	}
	words[n++] = M9_HW_HDR3(0xe6, 0x8f, 0x93);
	words[n++] = M9_READ_E6_BANK;
	words[n++] = M9_READ_BANK_CONST;
	words[n++] = 0xffffffffu;
	for (i = 1; i <= 2; i++) {
		words[n++] = M9_HW_HDR2(0xe6, 0xe3);
		words[n++] = M9_READ_E6_TOKEN;
		words[n++] = (t0 + i * M9_READ_TOKEN_STEP) | 1;
		words[n++] = M9_HW_HDR1(0x43);
		words[n++] = t0 + i * M9_READ_TOKEN_STEP;
	}
	words[n++] = M9_HW_HDR(0xf0, 0x78, 0x79, 0x7a);
	words[n++] = M9_READ_COPY_F0;
	words[n++] = r->source | M9_READ_TAG_LOCAL;
	words[n++] = M9_READ_FORMAT_RAW32 << 25 | (r->descriptor0 & 0x01ffffffu);
	words[n++] = r->descriptor1;
	/* Linear destination: format of the view, its layout bit 23, pitch. */
	words[n++] = M9_HW_HDR2(0xf3, 0xb7);
	words[n++] = 0;
	words[n++] = M9_READ_FORMAT_RAW32 << 25 |
		     (r->descriptor0 & 0x00800000u) | pitch_px;
	for (i = 0; i < k; i++) {
		const struct m9x_engine_read_chunk *c = &chunks[i];

		words[n++] = M9_HW_HDR3(0xb6, 0xf2, 0xfc);
		words[n++] = (r->ring[c->ring].bus + c->offset) | M9_READ_TAG_BUS;
		words[n++] = (r->y + c->row) << 16 | r->x;
		words[n++] = c->rows << 16 | r->width;
		words[n++] = M9_HW_HDR2(0xe6, 0xe3);
		words[n++] = M9_READ_E6_TOKEN;
		words[n++] = (t0 + M9_READ_FIRST_CHUNK + i * M9_READ_TOKEN_STEP) |
			     1;
	}
	/* Leave no system address and no copy origin in the ordinary context. */
	words[n++] = M9_HW_HDR3(0xf2, 0xb6, 0xb7);
	words[n++] = 0;
	words[n++] = 0;
	words[n++] = M9_READ_SEQ0_B7;
	if (r->state) {
		words[n++] = M9_HW_HDR(0xe6, 0x8f, 0x90, 0x91);
		words[n++] = M9_READ_E6_BANK;
		words[n++] = M9_READ_BANK_STATE;
		words[n++] = M9_READ_SEQ0_BANK0;
		words[n++] = 0;
		words[n++] = M9_HW_HDR1(0x92);
		words[n++] = M9_READ_SEQ0_BANK2;
	}
	words[n++] = M9_HW_HDR2(0xe6, 0xe3);
	words[n++] = M9_HW_E6_STEP;
	words[n++] = final | 1;

	plan->dwords = n;
	plan->chunks = k;
	plan->rows = row;
	plan->pitch = pitch;
	plan->final = final;
	return 0;
}
