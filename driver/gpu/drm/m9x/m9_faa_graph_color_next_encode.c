// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include "m9_faa_graph_color_next_encode.h"

static u32 get(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static void put(u8 *p, u32 word)
{
	p[0] = word;
	p[1] = word >> 8;
	p[2] = word >> 16;
	p[3] = word >> 24;
}

/* Original b5e70 page-run grammar, with device+1c bit0 clear. This bounded
 * TARGET clear selects the current 320x240 primary's 80 logical pages.
 * Explicit masks are generated for each live aligned group of eight pages;
 * no captured page index or native-placement-independent count is reused.
 */
static int clear_target(u32 base, u64 previous, u8 image[128], u32 *bytes)
{
	static const u32 prefix[] = {
		0x45458fe6, 0x04000001, 0x80000600, 0x45929190, 0,
		0,	    0xffffffff, 0x45454570, 0,		0x4545ace6,
		2,	    0,		0x45e62b2a, 0x454545e6, 0x0000ffff,
	};
	u32 page = base >> 12, end, n = 0, at = 64, i;

	if ((base & 4095) || (u64)base + 0x50000 > 0x40000000u)
		return -ERANGE;
	end = page + 80;
	while (page < end) {
		u32 group = page & ~7u;
		u32 last = group + 8 < end ? group + 8 : end;
		u32 mask = ((1u << (last - page)) - 1) << (page & 7);

		put(image + at, ((group | 0xd00000u) << 5) | mask);
		at += 4;
		n++;
		page = last;
	}
	for (i = 0; i < ARRAY_SIZE(prefix); i++)
		put(image + i * 4, prefix[i]);
	put(image + 60, 0x20000000 | (n - 1));
	put(image + at, 0x4545e3e6);
	put(image + at + 4, 1);
	put(image + at + 8, ((u32)previous + 0x10) | 1);
	put(image + at + 12, 0x454545e6);
	put(image + at + 16, 1);
	*bytes = at + 20;
	return 0;
}

int m9x_faa_color_next_encode(const u8 *blob, size_t bytes,
			      const struct m9x_faa_graph_draw_owner
				      owner[M9_FAA_DRAW_SOURCE_OWNERS],
			      u32 init_bytes, u32 secondary_cursor,
			      u64 previous_unlock_point,
			      const struct m9x_faa_color_input *input,
			      u32 clear_flags,
			      const struct m9x_faa_color_next_image *output)
{
	/* Original post-Unlock COLOR4 source f4166819. These are semantic
	 * packet spans in the validated initial encoder, not captured bytes.
	 * The repeat drops first-only GPU43 and setup groups and copies the
	 * live late raster state into the early stage's new state block.
	 */
	static const struct {
		u16 first, last, next;
	} span[] = {
		{ 0, 44, 0 },	      { 60, 436, 64 },
		{ 2428, 2536, 440 },  { 444, 736, 548 },
		{ 748, 844, 840 },    { 944, 1212, 936 },
		{ 1236, 1348, 1204 }, { 1360, 1440, 1316 },
		{ 1516, 1536, 1396 }, { 1556, 1772, 1416 },
		{ 1796, 1884, 1632 }, { 1896, 2712, 1720 },
	};
	static const u32 pointer_at[] = { 4, 8, 52, 56 };
	static const u32 pointer_end[] = { 0, 1676, 1676, 2536 };
	static const u32 branch_at[] = { 28, 808, 1284 };
	static const u32 branch_end[] = { 764, 1248, 1664 };
	struct m9x_faa_color_image first;
	u8 clear[M9_FAA_COLOR_NEXT_CLEAR_CAPACITY];
	u32 clear_bytes = 0, secondary_bytes, first_bytes, i;
	u8 *secondary;
	u64 epoch;
	int extra, ret;

	if (!owner || !input || !output || !output->clear_length ||
	    !output->image.secondary || !output->image.secondary_length ||
	    !output->secondary_scratch ||
	    output->secondary_scratch == output->image.secondary ||
	    clear_flags > 1 || (secondary_cursor & 3) ||
	    (previous_unlock_point & 15) ||
	    (u32)previous_unlock_point < 0x1b0 ||
	    previous_unlock_point > ~(u64)0 - 0xe0 ||
	    input->layout.program_count != 4 || input->declaration_count < 3 ||
	    input->declaration_count > 17 || !(input->declaration_count & 1))
		return -EINVAL;
	extra = ((int)((input->declaration_count + 3) / 4) - 2) * 24;
	secondary_bytes = 2536 + extra;
	/* Keep the vendor pool's 512-byte guard and validate before touching
	 * any destination. The delegated encoder validates its larger scratch.
	 */
	if (secondary_cursor > 0x3fe00 ||
	    secondary_bytes > 0x3fe00 - secondary_cursor ||
	    secondary_cursor > owner[29].bytes ||
	    secondary_bytes > owner[29].bytes - secondary_cursor ||
	    (u64)owner[29].base + secondary_cursor + secondary_bytes >
		    U32_MAX ||
	    output->image.secondary_bytes < secondary_bytes)
		return -ERANGE;
	if (clear_flags) {
		if (!output->clear || owner[1].bytes < 0x50000)
			return -EINVAL;
		ret = clear_target(owner[1].base, previous_unlock_point, clear,
				   &clear_bytes);
		if (ret)
			return ret;
		if (output->clear_capacity < clear_bytes)
			return -ERANGE;
	}
	/* The repeat omits HEAD's epoch. Optional CLEAR contributes a genuine
	 * preceding post/epoch. Neither expected token invents a GPU receipt.
	 */
	epoch = previous_unlock_point - 0x10 + (clear_flags ? 0x10 : 0);
	first = output->image;
	first.secondary = output->secondary_scratch;
	first.secondary_bytes = output->secondary_scratch_capacity;
	first.secondary_length = &first_bytes;
	ret = m9x_faa_color_four_encode(blob, bytes, owner, init_bytes, epoch,
					input, &first);
	if (ret)
		return ret;

	/* Omit the already-retired HEAD and first-only final GPU43 pair. */
	memmove(first.primary, first.primary + 64, 3604);
	memmove(first.primary + 3604, first.primary + 3676, 20);
	for (i = 0; i < ARRAY_SIZE(pointer_at); i++) {
		u32 end = pointer_end[i];

		if (end)
			end += extra;
		put(first.primary + pointer_at[i],
		    (owner[29].base + secondary_cursor + end) |
			    (i & 1 ? 2 : 1));
	}
	secondary = output->image.secondary;
	for (i = 0; i < ARRAY_SIZE(span); i++) {
		u32 begin = span[i].first, end = span[i].last,
		    next = span[i].next;

		if (begin >= 576)
			begin += extra;
		if (end >= 576)
			end += extra;
		if (next >= 680)
			next += extra;
		memcpy(secondary + next, first.secondary + begin, end - begin);
	}
	/* Original repeat E3/E6 packet order is distinct from the initial one. */
	put(secondary + 44, 3);
	put(secondary + 48, 0x45e3e6e6);
	put(secondary + 52, 1);
	put(secondary + 56, 2);
	put(secondary + 60, get(first.secondary + 56));
	/* Original f98b3..f98ce transforms the live first-stage mask;
	 * its route and all packed carry descriptors remain live-derived.
	 */
	put(secondary + 308,
	    ((get(first.secondary + 304) & 0xff1fffff) | 0x00100000) &
		    0xfffffffc);
	for (i = 0; i < ARRAY_SIZE(branch_at); i++) {
		u32 at = branch_at[i], end = branch_end[i];

		if (at >= 680)
			at += extra;
		if (end >= 680)
			end += extra;
		put(secondary + at, 0x49d00000 | ((end - at) / 4));
	}
	if (clear_bytes)
		memcpy(output->clear, clear, clear_bytes);
	*output->clear_length = clear_bytes;
	*output->image.secondary_length = secondary_bytes;
	return 0;
}
