// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* Genuine second returned-DRAW raster tail and independent 23b80 finish.
 * The source strips every live address, extent and tracker from this fixed
 * 320x240 control. No captured GPU address or future status enters output. */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <crypto/sha2.h>

#include "m9_faa_graph_next_finish_encode.h"

static const u8 finish_sha256[SHA256_DIGEST_SIZE] = {
	0x4f, 0xc2, 0x7c, 0x24, 0xdb, 0x1c, 0xf1, 0x13,
	0x3e, 0xe3, 0x25, 0xba, 0x8d, 0x8b, 0x0b, 0x25,
	0xb7, 0xbc, 0x71, 0x5b, 0x4f, 0xa1, 0x0e, 0xb5,
	0x40, 0xf5, 0xfb, 0xd2, 0x60, 0xd3, 0xf0, 0xe9,
};

struct finish_fixup {
	u16 dst, owner;
	u32 addend, encoder, tag;
};

/* Destination offsets are relative to secondary owner29 offset 2040.
 * Encoders are the original M9128 producer PCs, not captured values. */
static const struct finish_fixup finish_fixups[] = {
	{ 132,  8, 0x000000, 0x24930, 1 },
	{ 136,  8, 0x000014, 0x24909, 0 },
	{ 164,  1, 0x000000, 0x24930, 1 },
	{ 180,  4, 0x000000, 0x24930, 1 },
	{ 196,  5, 0x000000, 0x24930, 1 },
	{ 212, 25, 0x000000, 0x24930, 1 },
	{ 224, 22, 0x000000, 0x24930, 1 },
	{ 256,  2, 0x000000, 0x24930, 1 },
	{ 260,  3, 0x000000, 0x24930, 1 },
	{ 404, 23, 0x107800, 0x24930, 1 },
	{ 408, 23, 0x316800, 0x24930, 1 },
	{ 424, 23, 0x20f000, 0x24930, 1 },
	{ 444, 26, 0x000000, 0x24886, 1 },
	{ 468, 28, 0x000000, 0x248c8, 1 },
	{ 472, 28, 0x000020, 0x248c8, 1 },
	{ 644, 12, 0x000000, 0x24886, 1 },
	{ 676, 28, 0x000100, 0x248c8, 1 },
};

static const u16 finish_direct_dst[] = { 4, 8, 16, 20 };
static const u16 finish_direct_src[] = { 2040, 2732, 2732, 2748 };
static const u8 finish_direct_tag[] = { 1, 2, 1, 2 };
static const u16 finish_tracker_dst[] = { 56, 152, 704 };

static void finish_put32(u8 *p, u32 word)
{
	p[0] = word;
	p[1] = word >> 8;
	p[2] = word >> 16;
	p[3] = word >> 24;
}

static bool finish_overlap(const void *a, size_t na, const void *b, size_t nb)
{
	unsigned long x = (unsigned long)a, y = (unsigned long)b;
	unsigned long ex, ey;

	if (check_add_overflow(x, na, &ex) ||
	    check_add_overflow(y, nb, &ey))
		return true;
	return x < ey && y < ex;
}

bool m9x_faa_graph_next_finish_blob_valid(const u8 *blob, size_t bytes)
{
	u8 digest[SHA256_DIGEST_SIZE];

	if (!blob || bytes != M9_FAA_NEXT_FINISH_BLOB_BYTES)
		return false;
	sha256(blob, bytes, digest);
	return !memcmp(digest, finish_sha256, sizeof(digest));
}

int m9x_faa_graph_next_finish_encode(const u8 *blob, size_t bytes,
	const struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS],
	u32 live_primary_bytes, u64 prefix_point,
	const struct m9x_faa_graph_next_finish_image *image)
{
	u32 values[ARRAY_SIZE(finish_fixups)];
	u32 direct[ARRAY_SIZE(finish_direct_dst)];
	u32 address, i;

	if (!owner || !image || !image->primary || !image->secondary ||
	    image->primary_bytes < M9_FAA_NEXT_FINISH_PRIMARY_BYTES ||
	    image->secondary_bytes < M9_FAA_NEXT_FINISH_SECONDARY_BYTES ||
	    live_primary_bytes < 8668 || (live_primary_bytes & 3) ||
	    (u32)prefix_point < 0x1e0 ||
	    (u32)prefix_point > U32_MAX - 0x40 ||
	    (prefix_point & 15) ||
	    !m9x_faa_graph_next_finish_blob_valid(blob, bytes))
		return -EINVAL;
	if (m9x_faa_graph_draw_owners_validate(owner))
		return -ERANGE;
	if (owner[27].bytes < live_primary_bytes ||
	    owner[27].bytes - live_primary_bytes < M9_FAA_NEXT_FINISH_PRIMARY_BYTES ||
	    owner[29].bytes < M9_FAA_NEXT_FINISH_SECONDARY_OFFSET +
		M9_FAA_NEXT_FINISH_SECONDARY_BYTES)
		return -ERANGE;
	if (finish_overlap(image->primary, M9_FAA_NEXT_FINISH_PRIMARY_BYTES,
			   image->secondary, M9_FAA_NEXT_FINISH_SECONDARY_BYTES) ||
	    finish_overlap(image->primary, M9_FAA_NEXT_FINISH_PRIMARY_BYTES,
			   blob, bytes) ||
	    finish_overlap(image->secondary, M9_FAA_NEXT_FINISH_SECONDARY_BYTES,
			   blob, bytes))
		return -EINVAL;
	/* All owner ranges, encodings and endpoints are checked before a single
	 * output byte changes. The graph profile fixes 320x240; its two size
	 * words are rebuilt below, never copied as captured parameters. */
	for (i = 0; i < ARRAY_SIZE(finish_fixups); i++) {
		const struct finish_fixup *f = &finish_fixups[i];
		const struct m9x_faa_graph_draw_owner *src = &owner[f->owner];

		if (f->addend > src->bytes || src->bytes - f->addend < 4 ||
		    check_add_overflow(src->base, f->addend, &address) ||
		    (address & 3))
			return -ERANGE;
		switch (f->encoder) {
		case 0x24930:
		case 0x248c8:
			values[i] = address | f->tag;
			break;
		case 0x24909:
			values[i] = address;
			break;
		case 0x24886:
			values[i] = (address & 0xfffff000) | f->tag |
				(!address ? 4 : 0);
			break;
		default:
			return -EINVAL;
		}
	}
	for (i = 0; i < ARRAY_SIZE(finish_direct_dst); i++) {
		u32 offset = finish_direct_src[i];

		if (offset > owner[29].bytes || owner[29].bytes - offset < 4 ||
		    check_add_overflow(owner[29].base, offset, &address) ||
		    (address & 3))
			return -ERANGE;
		direct[i] = address | finish_direct_tag[i];
	}
	memcpy(image->primary, blob, M9_FAA_NEXT_FINISH_PRIMARY_BYTES);
	memcpy(image->secondary, blob + M9_FAA_NEXT_FINISH_PRIMARY_BYTES,
	       M9_FAA_NEXT_FINISH_SECONDARY_BYTES);
	for (i = 0; i < ARRAY_SIZE(finish_fixups); i++)
		finish_put32(image->secondary + finish_fixups[i].dst, values[i]);
	for (i = 0; i < ARRAY_SIZE(finish_direct_dst); i++)
		finish_put32(image->primary + finish_direct_dst[i], direct[i]);
	finish_put32(image->secondary + 348, 240u << 16 | 320u);
	finish_put32(image->secondary + 356, 240u << 16 | 320u);
	for (i = 0; i < ARRAY_SIZE(finish_tracker_dst); i++)
		finish_put32(image->secondary + finish_tracker_dst[i],
			     ((u32)prefix_point + (i + 1) * 0x10) | 1);
	return 0;
}
