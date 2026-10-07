// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Bounded red first draw after a real graph INIT.  Source: signed RE plan
 * 8dda0abf2776d1e057ce74fa2bf0801d73c32f44e2c542fbfe6f2c9bbe69bca8.
 * Source-known bytes are copied from the separately authenticated blob;
 * address and timeline fields are generated from live retained owners.
 * No captured pointer, primary prefix or unknown constants gap is replayed.
 */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <crypto/sha2.h>

#include "m9_faa_graph_draw_encode.h"

#define SOURCE_PRIMARY_BYTES 4880u
#define SOURCE_PRIMARY_BLOB 117648u
#define SOURCE_PROGRAM_BLOB 117408u
#define SOURCE_CONSTANTS0_BLOB 122616u
#define SOURCE_CONSTANTS1_BLOB 122752u
#define SOURCE_SECONDARY_BLOB 122940u
#define SOURCE_INIT_POINT 0xb0u

static const u8 draw_blob_sha256[SHA256_DIGEST_SIZE] = {
	0xd7, 0x26, 0xfb, 0xa3, 0x04, 0x00, 0x23, 0x80,
	0x25, 0x84, 0xdc, 0x8e, 0xa0, 0xb5, 0xc3, 0xf7,
	0xd5, 0xa8, 0x56, 0xd8, 0xa5, 0xbd, 0xc3, 0xdf,
	0xd2, 0xe2, 0xe5, 0x8f, 0x90, 0x63, 0x0d, 0x16,
};

static const struct {
	u32 bytes;
	bool virtual;
} draw_owner_spec[M9_FAA_DRAW_SOURCE_OWNERS] = {
#define L(bytes) { bytes, false }
#define V(bytes) { bytes, true }
	L(0x100), L(0x50000), L(0x136080), L(0x13080),
	L(0x98000), L(0x8000), V(0x50000), L(0x8000),
	L(0x80), L(0x8000), L(0x8000), L(0x20080),
	L(0x1d080), L(0x1080), L(0x1080), L(0x8000),
	L(0x8000), L(0x8000), L(0x8000), L(0x40080),
	L(0x5080), L(0x80), L(0x580), L(0x460080),
	L(0x408000), L(0x100080), L(0x1080), V(0x40000),
	V(0x40000), V(0x40000), L(0x80000),
#undef L
#undef V
};

struct m9_draw_fixup {
	u16 dst, offset, src;
	u32 addend, encoder, tag, captured;
};

/* The first four INIT fixups were already published and are excluded.
 * All rows below are source new bytes: primary[4880,4968) or secondary. */
static const struct m9_draw_fixup draw_fixups[] = {
	{ 27, 4896, 8, 0, 0x24930, 1, 0x1161001 },
	{ 29, 72, 23, 0, 0x24930, 1, 0x122a001 },
	{ 29, 88, 23, 0x107800, 0x24930, 1, 0x1331801 },
	{ 29, 104, 23, 0x20f000, 0x24930, 1, 0x1439001 },
	{ 29, 120, 23, 0x316800, 0x24930, 1, 0x1540801 },
	{ 29, 284, 28, 0x180, 0x24930, 1, 0x400c2181 },
	{ 29, 304, 26, 0x20, 0x24886, 1, 0x1b9a001 },
	{ 29, 308, 26, 0x20, 0x248ab, 1, 0x4 },
	{ 29, 328, 28, 0x20, 0x248c8, 1, 0x400c2021 },
	{ 29, 332, 28, 0x88, 0x248c8, 1, 0x400c2089 },
	{ 29, 540, 12, 0, 0x24886, 1, 0x119a001 },
	{ 29, 572, 28, 0x100, 0x248c8, 1, 0x400c2101 },
	{ 27, 4948, 29, 0, 0x256f1, 1, 0x40042001 },
	{ 27, 4952, 29, 0x25c, 0x256f1, 2, 0x4004225e },
	{ 27, 4960, 29, 0x25c, 0x256f1, 1, 0x4004225d },
	{ 27, 4964, 29, 0x298, 0x256f1, 2, 0x4004229a },
};

struct m9_draw_tracker {
	u16 dst, offset;
	u32 captured;
};

static const struct m9_draw_tracker draw_trackers[] = {
	{ 27, 4888, 0xc1 },
	{ 29, 12, 0xd1 },
	{ 29, 60, 0xe1 },
	{ 29, 616, 0xf1 },
	{ 29, 644, 0x101 },
	{ 29, 660, 0x111 },
};

static u32 draw_get32(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 |
	       (u32)p[2] << 16 | (u32)p[3] << 24;
}

static void draw_put32(u8 *p, u32 value)
{
	p[0] = value;
	p[1] = value >> 8;
	p[2] = value >> 16;
	p[3] = value >> 24;
}

static u32 draw_fnv(const u8 *blob, size_t bytes)
{
	u32 hash = 0x811c9dc5;
	size_t i;

	for (i = 0; i < bytes; i++)
		hash = (hash ^ blob[i]) * 0x01000193;
	return hash;
}

bool m9x_faa_graph_draw_blob_valid(const u8 *blob, size_t blob_bytes)
{
	u8 digest[SHA256_DIGEST_SIZE];

	if (!blob || blob_bytes != M9_FAA_DRAW_BLOB_BYTES ||
	    draw_fnv(blob, blob_bytes) != M9_FAA_DRAW_BLOB_FNV)
		return false;
	sha256(blob, blob_bytes, digest);
	return !memcmp(digest, draw_blob_sha256, sizeof(digest));
}

static u32 draw_encode_address(u32 encoder, u32 address, u32 tag)
{
	switch (encoder) {
	case 0x24930:
	case 0x256f1:
		return address | tag;
	case 0x24886:
		return (address & 0xfffff000) | tag | (!address ? 4 : 0);
	case 0x248ab:
		return ((address & 0xfff) >> 3) & 0xfffff;
	case 0x248c8:
		return (address & ~3u) | tag;
	default:
		return 0;
	}
}

int m9x_faa_graph_draw_owners_validate(
	const struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS])
{
	u32 i;

	if (!owner)
		return -EINVAL;
	for (i = 0; i < M9_FAA_DRAW_SOURCE_OWNERS; i++) {
		u64 end = (u64)owner[i].base + owner[i].bytes;

		if (!IS_ALIGNED(owner[i].base, PAGE_SIZE) ||
		    !IS_ALIGNED(owner[i].bytes, PAGE_SIZE) ||
		    owner[i].bytes < draw_owner_spec[i].bytes ||
		    (draw_owner_spec[i].virtual ?
		     owner[i].base < 0x40000000u || end > 0x41000000u :
		     end > 0x40000000u))
			return -ERANGE;
	}
	return 0;
}

static bool draw_source_word(u16 dst, u16 offset, u32 *blob_offset,
			     u32 *image_offset)
{
	if (dst == 27 && offset >= SOURCE_PRIMARY_BYTES &&
	    offset <= SOURCE_PRIMARY_BYTES + M9_FAA_DRAW_PRIMARY_SUFFIX_BYTES - 4) {
		*blob_offset = SOURCE_PRIMARY_BLOB + offset;
		*image_offset = offset - SOURCE_PRIMARY_BYTES;
		return true;
	}
	if (dst == 29 && offset <= M9_FAA_DRAW_SECONDARY_BYTES - 4) {
		*blob_offset = SOURCE_SECONDARY_BLOB + offset;
		*image_offset = offset;
		return true;
	}
	return false;
}

int m9x_faa_graph_draw_encode(const u8 *blob, size_t blob_bytes,
		const struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS],
		u32 live_init_primary_bytes, u64 init_point,
		const struct m9x_faa_graph_draw_image *image)
{
	u32 source_offset, image_offset, address, value, init_low;
	u32 i;

	if (!blob || !owner || !image || !image->program || !image->constants ||
	    !image->secondary || !image->primary_suffix ||
	    blob_bytes != M9_FAA_DRAW_BLOB_BYTES ||
	    image->program_bytes < M9_FAA_DRAW_PROGRAM_BYTES ||
	    image->constants_bytes < M9_FAA_DRAW_CONSTANTS_BYTES ||
	    image->secondary_bytes < M9_FAA_DRAW_SECONDARY_BYTES ||
	    image->primary_suffix_bytes < M9_FAA_DRAW_PRIMARY_SUFFIX_BYTES ||
	    live_init_primary_bytes < SOURCE_PRIMARY_BYTES ||
	    live_init_primary_bytes > owner[27].bytes ||
	    owner[27].bytes - live_init_primary_bytes <
		M9_FAA_DRAW_PRIMARY_SUFFIX_BYTES ||
	    owner[26].bytes < M9_FAA_DRAW_PROGRAM_BYTES ||
	    owner[28].bytes < M9_FAA_DRAW_CONSTANTS_BYTES ||
	    owner[29].bytes < M9_FAA_DRAW_SECONDARY_BYTES ||
	    (u32)init_point < SOURCE_INIT_POINT ||
	    (u32)init_point > U32_MAX - 0x60 ||
	    SOURCE_SECONDARY_BLOB + M9_FAA_DRAW_SECONDARY_BYTES > blob_bytes ||
	    SOURCE_PRIMARY_BLOB + SOURCE_PRIMARY_BYTES +
		M9_FAA_DRAW_PRIMARY_SUFFIX_BYTES > blob_bytes ||
	    SOURCE_CONSTANTS1_BLOB + 188 > blob_bytes ||
	    !m9x_faa_graph_draw_blob_valid(blob, blob_bytes))
		return -EINVAL;
	init_low = (u32)init_point;
	if (m9x_faa_graph_draw_owners_validate(owner))
		return -ERANGE;
	/* Preflight all source words, live extents and encoded addresses before
	 * writing any destination. A source change cannot leave a partial draw. */
	for (i = 0; i < ARRAY_SIZE(draw_fixups); i++) {
		const struct m9_draw_fixup *f = &draw_fixups[i];
		const struct m9x_faa_graph_draw_owner *src = &owner[f->src];

		if (!draw_source_word(f->dst, f->offset, &source_offset,
				      &image_offset) ||
		    f->addend > src->bytes || src->bytes - f->addend < 4 ||
		    check_add_overflow(src->base, f->addend, &address) ||
		    (f->encoder == 0x248ab ? !IS_ALIGNED(address, 8) :
		     !IS_ALIGNED(address, 4)) ||
		    (f->encoder != 0x248ab && f->tag != 1 && f->tag != 2) ||
		    draw_get32(blob + source_offset) != f->captured ||
		    !draw_encode_address(f->encoder, address, f->tag))
			return -ERANGE;
	}
	for (i = 0; i < ARRAY_SIZE(draw_trackers); i++) {
		const struct m9_draw_tracker *t = &draw_trackers[i];

		if (!draw_source_word(t->dst, t->offset, &source_offset,
				      &image_offset) ||
		    draw_get32(blob + source_offset) != t->captured ||
		    (t->captured & ~15u) < SOURCE_INIT_POINT)
			return -EINVAL;
	}
	memcpy(image->program, blob + SOURCE_PROGRAM_BLOB,
	       M9_FAA_DRAW_PROGRAM_BYTES);
	memcpy(image->constants, blob + SOURCE_CONSTANTS0_BLOB, 136);
	memcpy(image->constants + 256, blob + SOURCE_CONSTANTS1_BLOB, 188);
	memcpy(image->secondary, blob + SOURCE_SECONDARY_BLOB,
	       M9_FAA_DRAW_SECONDARY_BYTES);
	memcpy(image->primary_suffix,
	       blob + SOURCE_PRIMARY_BLOB + SOURCE_PRIMARY_BYTES,
	       M9_FAA_DRAW_PRIMARY_SUFFIX_BYTES);
	for (i = 0; i < ARRAY_SIZE(draw_fixups); i++) {
		const struct m9_draw_fixup *f = &draw_fixups[i];
		u8 *dst;

		address = owner[f->src].base + f->addend;
		value = draw_encode_address(f->encoder, address, f->tag);
		draw_source_word(f->dst, f->offset, &source_offset, &image_offset);
		dst = f->dst == 27 ? image->primary_suffix : image->secondary;
		draw_put32(dst + image_offset, value);
	}
	for (i = 0; i < ARRAY_SIZE(draw_trackers); i++) {
		const struct m9_draw_tracker *t = &draw_trackers[i];
		u8 *dst;

		value = (init_low + (t->captured & ~15u) -
			 SOURCE_INIT_POINT) | (t->captured & 15u);
		draw_source_word(t->dst, t->offset, &source_offset,
				 &image_offset);
		dst = t->dst == 27 ? image->primary_suffix : image->secondary;
		draw_put32(dst + image_offset, value);
	}
	return 0;
}
