// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifdef M9_FAA_SHADOW_HOST_TEST
#include <string.h>
#else
#include <linux/string.h>
#include <linux/unaligned.h>
#endif
#include "m9_faa_shadow.h"
#include "m9_hw_encode.h"

#ifdef M9_FAA_SHADOW_HOST_TEST
static u32 get_unaligned_le32(const void *p)
{
	u32 v;

	memcpy(&v, p, 4);
	return v;
}

static void put_unaligned_le32(u32 v, void *p)
{
	memcpy(p, &v, 4);
}
#endif

/* Ordinary-writable state without write side effects: no tags, GO, waits,
 * query controls, links or transition pairs.
 */
bool m9x_faa_shadow_restorable(u8 index)
{
	switch (index) {
	case 0x1e: case 0x2c: case 0x2d: case 0x43: case 0x44:
	case 0xa7: case 0xc6: case 0xe3: case 0xe6:
		return false;
	default:
		return m9x_submit_index_writable(index);
	}
}

static bool m9_shadow_draw_header(u32 header)
{
	u32 launch = header & 0x60000000, base = header & ~0x60000000;

	return (launch == 0x20000000 || launch == 0x60000000) &&
	       (base == 0x03000001 || base == 0x06000001 ||
		base == 0x07000001);
}

/* Packets of up to four indices, terminated by 0x45, each followed by its
 * values; a draw header carries two words. Links are followed once.
 */
static void m9_shadow_walk(struct m9x_faa_shadow *s, const u8 *stream,
			   u32 bytes, bool dirty, m9x_faa_shadow_resolve resolve,
			   void *ctx, int depth)
{
	u32 words = bytes / 4, i = 0, k, n;

	while (i < words) {
		u32 header = get_unaligned_le32(stream + 4 * i);
		u8 index[4];

		if (m9_shadow_draw_header(header)) {
			i += 3;
			continue;
		}
		for (k = 0; k < 4; k++)
			index[k] = header >> (8 * k);
		for (n = 0; n < 4 && index[n] != M9_HW_IDX_END; n++)
			;
		for (k = n; k < 4 && index[k] == M9_HW_IDX_END; k++)
			;
		if (k != 4 || i + n >= words) {
			i++;
			continue;
		}
		if (resolve && depth && n == 2 && index[0] == 0x2c &&
		    index[1] == 0x2d) {
			u32 start = get_unaligned_le32(stream + 4 * i + 4) & ~3u;
			u32 end = get_unaligned_le32(stream + 4 * i + 8) & ~3u;
			const u8 *linked = end > start ?
				resolve(ctx, start, end - start) : NULL;

			if (linked)
				m9_shadow_walk(s, linked, end - start, dirty,
					       resolve, ctx, depth - 1);
			else
				s->unresolved++;
		}
		for (k = 0; k < n; k++) {
			u8 r = index[k];

			if (!m9x_faa_shadow_restorable(r))
				continue;
			if (dirty) {
				s->dirty[r / 32] |= 1u << (r % 32);
			} else {
				s->value[r] = get_unaligned_le32(stream +
								 4 * (i + 1 + k));
				s->written[r / 32] |= 1u << (r % 32);
				s->dirty[r / 32] &= ~(1u << (r % 32));
			}
		}
		i += 1 + n;
	}
}

void m9x_faa_shadow_record(struct m9x_faa_shadow *s, const u8 *stream,
			   u32 bytes, m9x_faa_shadow_resolve resolve,
			   void *ctx)
{
	m9_shadow_walk(s, stream, bytes, false, resolve, ctx, 1);
}

void m9x_faa_shadow_mark(struct m9x_faa_shadow *s, const u8 *stream,
			 u32 bytes, m9x_faa_shadow_resolve resolve, void *ctx)
{
	m9_shadow_walk(s, stream, bytes, true, resolve, ctx, 1);
}

static void m9_shadow_put(u8 *out, u32 *at, u32 header, const u32 *values,
			  u32 n)
{
	u32 k;

	put_unaligned_le32(header, out + 4 * (*at)++);
	for (k = 0; k < n; k++)
		put_unaligned_le32(values[k], out + 4 * (*at)++);
}

/* Packets restoring every dirty index to the FAA value, or zero where the
 * FAA streams never set it. Returns the words written, 0 when nothing is
 * dirty or the packets would not fit in @capacity words; the dirty set is
 * then kept.
 */
u32 m9x_faa_shadow_restore(struct m9x_faa_shadow *s, u8 *out, u32 capacity)
{
	u32 at = 0, header = 0, values[4], n = 0, i;

	for (i = 0; i < 256; i++) {
		if (!(s->dirty[i / 32] & 1u << (i % 32)))
			continue;
		if (n == 4) {
			if (at + 5 > capacity)
				return 0;
			m9_shadow_put(out, &at, header, values, n);
			n = 0;
		}
		if (!n)
			header = M9_HW_HDR(M9_HW_IDX_END, M9_HW_IDX_END,
					   M9_HW_IDX_END, M9_HW_IDX_END);
		header = (header & ~(0xffu << (8 * n))) | i << (8 * n);
		values[n++] = s->written[i / 32] & 1u << (i % 32) ?
			      s->value[i] : 0;
	}
	if (n) {
		if (at + 1 + n > capacity)
			return 0;
		m9_shadow_put(out, &at, header, values, n);
	}
	memset(s->dirty, 0, sizeof(s->dirty));
	return at;
}
