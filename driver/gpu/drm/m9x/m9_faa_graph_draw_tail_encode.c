// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* Returned public DRAW followed by independent genuine 23b80 finish.
 * RE 7c7f2742 joins both source posts and stops before the GPU status read.
 * The immutable template is a fixed 320x240 red control, not arbitrary GL
 * state. Every address, width/height field and completion value is generated
 * from live arguments. No unknown GPU-produced bytes are initialized here.
 */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <crypto/sha2.h>

#include "m9_faa_graph_draw_tail_encode.h"

static const u8 tail_sha256[SHA256_DIGEST_SIZE] = {
	0x55, 0x14, 0xd2, 0x47, 0xa2, 0x7f, 0x96, 0xfc, 0x04, 0x39, 0x70,
	0xe7, 0xb8, 0x10, 0x4e, 0x28, 0xac, 0x8b, 0x8a, 0x03, 0xe2, 0x31,
	0xce, 0x78, 0x9c, 0x9a, 0x26, 0xa0, 0xe1, 0xf8, 0x2d, 0x25,
};

static const struct {
	u16 offset, owner;
	u32 addend, encoder, tag;
} tail_fixups[] = {
	{ 796, 8, 0, 0x24930, 1 },
	{ 800, 8, 20, 0x24909, 0 },
	{ 828, 1, 0, 0x24930, 1 },
	{ 844, 4, 0, 0x24930, 1 },
	{ 860, 5, 0, 0x24930, 1 },
	{ 876, 25, 0, 0x24930, 1 },
	{ 888, 22, 0, 0x24930, 1 },
	{ 920, 2, 0, 0x24930, 1 },
	{ 924, 3, 0, 0x24930, 1 },
	/* Fixed diagnostic companion layout, validated by the graph profile. */
	{ 1068, 23, 0x107800, 0x24930, 1 },
	{ 1072, 23, 0x316800, 0x24930, 1 },
	{ 1088, 23, 0x20f000, 0x24930, 1 },
	{ 1108, 26, 0, 0x24886, 1 },
	{ 1132, 28, 0, 0x248c8, 1 },
	{ 1136, 28, 32, 0x248c8, 1 },
	{ 1308, 12, 0, 0x24886, 1 },
	{ 1340, 28, 256, 0x248c8, 1 },
};

static void tail_put32(u8 *p, u32 word)
{
	p[0] = word;
	p[1] = word >> 8;
	p[2] = word >> 16;
	p[3] = word >> 24;
}

bool m9x_faa_graph_draw_tail_blob_valid(const u8 *blob, size_t bytes)
{
	u8 digest[SHA256_DIGEST_SIZE];

	if (!blob || bytes != M9_FAA_DRAW_TAIL_BLOB_BYTES)
		return false;
	sha256(blob, bytes, digest);
	return !memcmp(digest, tail_sha256, sizeof(digest));
}

int m9x_faa_graph_draw_tail_encode(const u8 *blob, size_t bytes,
				   const struct m9x_faa_graph_draw_owner
					   owner[M9_FAA_DRAW_SOURCE_OWNERS],
				   u32 live_primary_bytes, u32 width,
				   u32 height, u64 prefix_point,
				   const struct m9x_faa_graph_draw_tail_image
					   *image)
{
	u32 values[ARRAY_SIZE(tail_fixups)], i, address;
	u32 secondary_end = M9_FAA_DRAW_TAIL_SECONDARY_OFFSET +
			    M9_FAA_DRAW_TAIL_SECONDARY_BYTES;

	if (!blob || bytes != M9_FAA_DRAW_TAIL_BLOB_BYTES || !owner || !image ||
	    !image->secondary || !image->primary ||
	    image->secondary_bytes < M9_FAA_DRAW_TAIL_SECONDARY_BYTES ||
	    image->primary_bytes < M9_FAA_DRAW_TAIL_PRIMARY_BYTES ||
	    width != 320 || height != 240 || (prefix_point & 15) ||
	    (u32)prefix_point < 0x110 || (u32)prefix_point > U32_MAX - 0x30 ||
	    live_primary_bytes < 4968 || (live_primary_bytes & 3) ||
	    owner[27].bytes < M9_FAA_DRAW_TAIL_PRIMARY_BYTES ||
	    live_primary_bytes >
		    owner[27].bytes - M9_FAA_DRAW_TAIL_PRIMARY_BYTES ||
	    owner[29].bytes < secondary_end)
		return -EINVAL;
	if (!m9x_faa_graph_draw_tail_blob_valid(blob, bytes))
		return -EINVAL;
	if (m9x_faa_graph_draw_owners_validate(owner))
		return -ERANGE;
	/* All admission completes before touching either destination. */
	for (i = 0; i < ARRAY_SIZE(tail_fixups); i++) {
		const struct m9x_faa_graph_draw_owner *src =
			&owner[tail_fixups[i].owner];
		u32 addend = tail_fixups[i].addend;

		if (addend > src->bytes || src->bytes - addend < 4 ||
		    check_add_overflow(src->base, addend, &address) ||
		    (address & 3))
			return -ERANGE;
		switch (tail_fixups[i].encoder) {
		case 0x24886:
			values[i] = (address & 0xfffff000) |
				    tail_fixups[i].tag | (!address ? 4 : 0);
			break;
		case 0x24909:
			values[i] = address;
			break;
		case 0x248c8:
		case 0x24930:
			values[i] = address | tail_fixups[i].tag;
			break;
		default:
			return -EINVAL;
		}
	}
	memcpy(image->secondary, blob, M9_FAA_DRAW_TAIL_SECONDARY_BYTES);
	memcpy(image->primary, blob + M9_FAA_DRAW_TAIL_SECONDARY_BYTES,
	       M9_FAA_DRAW_TAIL_PRIMARY_BYTES);
	for (i = 0; i < ARRAY_SIZE(tail_fixups); i++)
		tail_put32(image->secondary + tail_fixups[i].offset -
				   M9_FAA_DRAW_TAIL_SECONDARY_OFFSET,
			   values[i]);
	tail_put32(image->secondary + 1012 - M9_FAA_DRAW_TAIL_SECONDARY_OFFSET,
		   height << 16 | width);
	tail_put32(image->secondary + 1020 - M9_FAA_DRAW_TAIL_SECONDARY_OFFSET,
		   height << 16 | width);
	tail_put32(image->secondary + 1368 - M9_FAA_DRAW_TAIL_SECONDARY_OFFSET,
		   ((u32)prefix_point + 0x30) | 1);
	tail_put32(image->primary + 4, (owner[29].base + 664) | 1);
	tail_put32(image->primary + 8, (owner[29].base + 1356) | 2);
	tail_put32(image->primary + 16, (owner[29].base + 1356) | 1);
	tail_put32(image->primary + 20, (owner[29].base + 1372) | 2);
	return 0;
}
