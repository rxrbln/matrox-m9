// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* Source-qualified repeated red draw after the same graph's Unlock. The 692-B
 * source template contains opcodes and fixed state only; all address,
 * timeline and vertex input words are filled from live retained state.
 */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <crypto/sha2.h>

#include "m9_faa_graph_next_encode.h"

static const u8 next_sha256[SHA256_DIGEST_SIZE] = {
	0xe2, 0xa6, 0x45, 0xa1, 0x2d, 0xfe, 0x80, 0x14, 0x91, 0x09, 0xb5,
	0x68, 0x7e, 0x05, 0xfe, 0x04, 0xed, 0xcf, 0x4d, 0x55, 0x78, 0x6a,
	0xe5, 0x1c, 0x0f, 0x0f, 0x78, 0x3e, 0x18, 0xe4, 0xd8, 0x5d,
};

struct next_fixup {
	u16 dst, src;
	u32 addend, encoder;
};

/* Destinations are relative to the retained secondary cursor. Every source is a
 * retained live owner; the source index namespace remains the 31-owner
 * post-INIT/Unlock namespace.
 */
static const struct next_fixup next_fixups[] = {
	{ 76, 23, 0x000000, 0x24930 },	{ 92, 23, 0x107800, 0x24930 },
	{ 108, 23, 0x20f000, 0x24930 }, { 124, 23, 0x316800, 0x24930 },
	{ 288, 28, 0x0001d0, 0x24930 }, { 308, 26, 0x000020, 0x24886 },
	{ 312, 26, 0x000020, 0x248ab }, { 332, 28, 0x000020, 0x248c8 },
	{ 336, 28, 0x000088, 0x248c8 }, { 544, 12, 0x000000, 0x24886 },
	{ 576, 28, 0x000100, 0x248c8 },
};

static const u16 next_tracker_offsets[] = { 16, 64, 620, 648, 664 };
static const u16 next_direct_offsets[] = { 4, 8, 16, 20 };
static const u16 next_direct_addends[] = { 0, 608, 608, 668 };
static const u8 next_direct_tags[] = { 1, 2, 1, 2 };

static void next_put32(u8 *p, u32 word)
{
	p[0] = word;
	p[1] = word >> 8;
	p[2] = word >> 16;
	p[3] = word >> 24;
}

static bool next_range_overlap(const void *a, size_t na, const void *b,
			       size_t nb)
{
	unsigned long x = (unsigned long)a, y = (unsigned long)b;
	unsigned long ex, ey;

	if (check_add_overflow(x, na, &ex) || check_add_overflow(y, nb, &ey))
		return true;
	return x < ey && y < ex;
}

/* Only a bounded 320x240 XYZRHW triangle is admitted. Comparing positive
 * finite IEEE-754 encodings as integers avoids kernel floating point.
 */
static bool next_vertices_valid(const u32 vertices[15])
{
	u32 i;

	for (i = 0; i < 3; i++) {
		const u32 *v = &vertices[5 * i];

		if ((v[0] & 0x80000000) || v[0] > 0x43a00000 ||
		    (v[1] & 0x80000000) || v[1] > 0x43700000 ||
		    (v[2] & 0x80000000) || v[2] > 0x3f800000 ||
		    (v[3] & 0x80000000) || !v[3] ||
		    (v[3] & 0x7f800000) == 0x7f800000)
			return false;
	}
	return true;
}

bool m9x_faa_graph_next_blob_valid(const u8 *blob, size_t bytes)
{
	u8 digest[SHA256_DIGEST_SIZE];

	if (!blob || bytes != M9_FAA_NEXT_BLOB_BYTES)
		return false;
	sha256(blob, bytes, digest);
	return !memcmp(digest, next_sha256, sizeof(digest));
}

int m9x_faa_graph_next_encode_at(const u8 *blob, size_t bytes,
				 const struct m9x_faa_graph_draw_owner
					 owner[M9_FAA_DRAW_SOURCE_OWNERS],
				 const struct m9x_faa_graph_next_cursor *cursor,
				 u64 unlock_point, const u32 vertices[15],
				 const struct m9x_faa_graph_next_image *image)
{
	u32 fixup_words[ARRAY_SIZE(next_fixups)];
	u32 direct_words[ARRAY_SIZE(next_direct_offsets)];
	u32 address, i;

	if (!cursor || !owner || !image || !vertices || !image->primary ||
	    !image->secondary || !image->vertices ||
	    image->primary_bytes < M9_FAA_NEXT_PRIMARY_BYTES ||
	    image->secondary_bytes < M9_FAA_NEXT_SECONDARY_BYTES ||
	    image->vertex_bytes < M9_FAA_NEXT_VERTEX_BYTES ||
	    cursor->primary < 8644 || (cursor->primary & 3) ||
	    cursor->secondary < M9_FAA_NEXT_SECONDARY_OFFSET ||
	    (cursor->secondary & 3) ||
	    cursor->secondary >
		    M9_FAA_NEXT_SECONDARY_GUARD - M9_FAA_NEXT_SECONDARY_BYTES ||
	    cursor->vertices < M9_FAA_NEXT_VERTEX_OFFSET ||
	    (cursor->vertices & 15) || (u32)unlock_point < 0x190 ||
	    (u32)unlock_point > U32_MAX - 0x60 || (unlock_point & 15) ||
	    !next_vertices_valid(vertices) ||
	    !m9x_faa_graph_next_blob_valid(blob, bytes))
		return -EINVAL;
	if (m9x_faa_graph_draw_owners_validate(owner))
		return -ERANGE;
	if (owner[27].bytes < cursor->primary ||
	    owner[27].bytes - cursor->primary < M9_FAA_NEXT_PRIMARY_BYTES ||
	    cursor->secondary > owner[29].bytes ||
	    owner[29].bytes - cursor->secondary <
		    M9_FAA_NEXT_SECONDARY_BYTES + 4 ||
	    cursor->vertices > owner[28].bytes ||
	    owner[28].bytes - cursor->vertices < M9_FAA_NEXT_VERTEX_STRIDE)
		return -ERANGE;
	/* No destination can corrupt the authenticated template, live input,
	 * or another destination. All refusals leave every output untouched.
	 */
	if (next_range_overlap(image->primary, M9_FAA_NEXT_PRIMARY_BYTES,
			       image->secondary, M9_FAA_NEXT_SECONDARY_BYTES) ||
	    next_range_overlap(image->primary, M9_FAA_NEXT_PRIMARY_BYTES,
			       image->vertices, M9_FAA_NEXT_VERTEX_BYTES) ||
	    next_range_overlap(image->secondary, M9_FAA_NEXT_SECONDARY_BYTES,
			       image->vertices, M9_FAA_NEXT_VERTEX_BYTES) ||
	    next_range_overlap(image->primary, M9_FAA_NEXT_PRIMARY_BYTES, blob,
			       bytes) ||
	    next_range_overlap(image->secondary, M9_FAA_NEXT_SECONDARY_BYTES,
			       blob, bytes) ||
	    next_range_overlap(image->vertices, M9_FAA_NEXT_VERTEX_BYTES, blob,
			       bytes) ||
	    next_range_overlap(image->primary, M9_FAA_NEXT_PRIMARY_BYTES,
			       vertices, M9_FAA_NEXT_VERTEX_BYTES) ||
	    next_range_overlap(image->secondary, M9_FAA_NEXT_SECONDARY_BYTES,
			       vertices, M9_FAA_NEXT_VERTEX_BYTES) ||
	    next_range_overlap(image->vertices, M9_FAA_NEXT_VERTEX_BYTES,
			       vertices, M9_FAA_NEXT_VERTEX_BYTES))
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(next_fixups); i++) {
		const struct next_fixup *f = &next_fixups[i];
		const struct m9x_faa_graph_draw_owner *src = &owner[f->src];

		u32 addend = f->dst == 288 ? cursor->vertices : f->addend;

		if (addend > src->bytes || src->bytes - addend < 4 ||
		    check_add_overflow(src->base, addend, &address) ||
		    (address & 3))
			return -ERANGE;
		switch (f->encoder) {
		case 0x24930:
		case 0x248c8:
			fixup_words[i] = address | 1;
			break;
		case 0x24886:
			fixup_words[i] = (address & 0xfffff000) | 1 |
					 (!address ? 4 : 0);
			break;
		case 0x248ab:
			fixup_words[i] = (address & 0xfff) >> 3;
			break;
		default:
			return -EINVAL;
		}
	}
	for (i = 0; i < ARRAY_SIZE(next_direct_offsets); i++) {
		u32 addend = cursor->secondary + next_direct_addends[i];

		if (addend > owner[29].bytes || owner[29].bytes - addend < 4 ||
		    check_add_overflow(owner[29].base, addend, &address) ||
		    (address & 3))
			return -ERANGE;
		direct_words[i] = address | next_direct_tags[i];
	}
	memcpy(image->primary, blob, M9_FAA_NEXT_PRIMARY_BYTES);
	memcpy(image->secondary, blob + M9_FAA_NEXT_PRIMARY_BYTES,
	       M9_FAA_NEXT_SECONDARY_BYTES);
	for (i = 0; i < ARRAY_SIZE(next_fixups); i++)
		next_put32(image->secondary + next_fixups[i].dst,
			   fixup_words[i]);
	for (i = 0; i < ARRAY_SIZE(next_tracker_offsets); i++)
		next_put32(image->secondary + next_tracker_offsets[i],
			   ((u32)unlock_point + (i + 1) * 0x10) | 1);
	for (i = 0; i < ARRAY_SIZE(next_direct_offsets); i++)
		next_put32(image->primary + next_direct_offsets[i],
			   direct_words[i]);
	for (i = 0; i < 15; i++)
		next_put32(image->vertices + 4 * i, vertices[i]);
	return 0;
}

/* Preserve the original bounded second-frame encoder API for source tests. */
int m9x_faa_graph_next_encode(const u8 *blob, size_t bytes,
			      const struct m9x_faa_graph_draw_owner
				      owner[M9_FAA_DRAW_SOURCE_OWNERS],
			      u32 live_primary_bytes, u64 unlock_point,
			      const u32 vertices[15],
			      const struct m9x_faa_graph_next_image *image)
{
	const struct m9x_faa_graph_next_cursor cursor = {
		live_primary_bytes,
		M9_FAA_NEXT_SECONDARY_OFFSET,
		M9_FAA_NEXT_VERTEX_OFFSET,
	};
	return m9x_faa_graph_next_encode_at(blob, bytes, owner, &cursor,
					    unlock_point, vertices, image);
}
