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
	p[0] = word; p[1] = word >> 8; p[2] = word >> 16; p[3] = word >> 24;
}

/* Original b5e70 page-run grammar, with device+1c bit0 clear. This bounded
 * TARGET clear selects the current 320x240 primary's 80 logical pages.
 * Explicit masks are generated for each live aligned group of eight pages;
 * no captured page index or native-placement-independent count is reused. */
static int clear_target(u32 base, u64 previous, u8 image[128], u32 *bytes)
{
	static const u32 prefix[] = {
		0x45458fe6, 0x04000001, 0x80000600, 0x45929190,
		0, 0, 0xffffffff, 0x45454570, 0,
		0x4545ace6, 2, 0, 0x45e62b2a, 0x454545e6, 0x0000ffff,
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
	const struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS],
	u32 init_bytes, u32 secondary_cursor, u64 previous_unlock_point,
	const struct m9x_faa_color_input *input, u32 clear_flags,
	const struct m9x_faa_color_next_image *output)
{
	/* The full four-stage encoder has exactly these four secondary-domain
	 * pointers in primary. Removing its already-retired HEAD moves each by64. */
	static const u32 secondary_pointer[] = {4, 8, 52, 56};
	u8 clear[M9_FAA_COLOR_NEXT_CLEAR_CAPACITY];
	u32 clear_bytes = 0, secondary_bytes, i;
	u64 epoch;
	int ret;

	if (!owner || !input || !output || !output->clear_length ||
	    clear_flags > 1 || (secondary_cursor & 3) ||
	    (previous_unlock_point & 15) || (u32)previous_unlock_point < 0x1b0 ||
	    previous_unlock_point > ~(u64)0 - 0xe0 ||
	    input->layout.program_count != 4 || input->declaration_count < 3 ||
	    input->declaration_count > 31 || !(input->declaration_count & 1))
		return -EINVAL;
	secondary_bytes = 2664 + ((input->declaration_count + 3) / 4) * 24;
	/* Retain the vendor pool's512-byte guard and never wrap a live cursor.
	 * Validate before the delegated encoder can touch any caller output. */
	if (secondary_cursor > 0x3fe00 || secondary_bytes > 0x3fe00 - secondary_cursor ||
	    secondary_cursor > owner[29].bytes ||
	    secondary_bytes > owner[29].bytes - secondary_cursor ||
	    (u64)owner[29].base + secondary_cursor + secondary_bytes > U32_MAX)
		return -ERANGE;
	if (clear_flags) {
		if (!output->clear || owner[1].bytes < 0x50000)
			return -EINVAL;
		ret = clear_target(owner[1].base, previous_unlock_point, clear, &clear_bytes);
		if (ret)
			return ret;
		if (output->clear_capacity < clear_bytes)
			return -ERANGE;
	}
	/* Source repeated DRAW omits the initial HEAD epoch. Optional CLEAR
	 * contributes a genuine preceding post and epoch; neither is a receipt. */
	epoch = previous_unlock_point - 0x10 + (clear_flags ? 0x10 : 0);
	ret = m9x_faa_color_four_encode(blob, bytes, owner, init_bytes, epoch,
				      input, &output->image);
	if (ret)
		return ret;
	memmove(output->image.primary, output->image.primary + 64,
		M9_FAA_COLOR_NEXT_PRIMARY_BYTES);
	for (i = 0; i < ARRAY_SIZE(secondary_pointer); i++) {
		u8 *p = output->image.primary + secondary_pointer[i];

		/* Cursor alignment preserves the source start/end pointer tags. */
		put(p, get(p) + secondary_cursor);
	}
	if (clear_bytes)
		memcpy(output->clear, clear, clear_bytes);
	*output->clear_length = clear_bytes;
	return 0;
}
