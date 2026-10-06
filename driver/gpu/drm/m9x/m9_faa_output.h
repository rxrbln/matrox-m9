/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_OUTPUT_H
#define M9_FAA_OUTPUT_H

/* Actual 8b020 -> 8c4a0: admitted kind1, 32-bit 320x256 backing,
 * platform table24=2/table28=1, z=0, page-aligned CPU DMA alias.
 * This computes the address only; it does not read or initialize pixels.
 * The visible frame is 320x240. The extra16rows remain allocation padding.
 */
static inline int m9_faa_output_offset(u32 x, u32 y, u32 *offset)
{
	u32 first, second, upper, lower;

	if (!offset || x >= 320 || y >= 256)
		return -EINVAL;
	first = ((((x ^ y) << 2) ^ x) & 0x80) ^ ((y & 8) << 4);
	second = (y << 1) ^ x;
	second = (second << 1) ^ x;
	second = (second << 1) ^ x;
	upper = (((first << 1) | (second & 0x80)) << 3) |
		((y >> 5) * (320u << 7) + ((x & ~31u) << 7));
	lower = (((y << 3) ^ x) & 0x20) ^ ((y & 8) << 2);
	lower |= ((y << 2) ^ x) & 0x10;
	lower |= ((y & 3) << 1) | (x & 8);
	lower = ((lower << 2) | (x & 7)) << 2;
	*offset = upper | lower;
	return 0;
}

#endif
