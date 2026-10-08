// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* Source-bounded original public-VS continuation. No GPU output is decoded. */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <crypto/sha2.h>
#include "m9_cmd.h"
#include "m9_faa_graph_public_vs_follow_encode.h"

static const u8 template_sha[32] = {
	0x87, 0xc0, 0xd4, 0xcd, 0x63, 0x73, 0xb8, 0xa0, 0x78, 0x8e, 0xd8,
	0xe9, 0xec, 0xff, 0xbb, 0x6e, 0x6b, 0x8f, 0x4d, 0x82, 0x17, 0x55,
	0x5f, 0x77, 0xb9, 0xca, 0xef, 0xeb, 0x8a, 0x89, 0x3d, 0x37,
};

struct follow_fixup {
	u16 at, owner;
	u32 offset, tag;
};

static const struct follow_fixup fields[] = {
	{ 4, 29, 752, 1 },	     { 8, 29, 1368, 2 },
	{ 48 + 56, 23, 0, 1 },	     { 48 + 72, 23, 873472, 1 },
	{ 48 + 88, 23, 3493888, 1 }, { 48 + 228, 26, 0, 1 },
	{ 48 + 252, 28, 48, 1 },     { 48 + 256, 28, 48, 1 },
	{ 48 + 284, 24, 0, 1 },	     { 48 + 312, 24, 0, 1 },
	{ 48 + 472, 12, 0, 1 },	     { 48 + 504, 28, 256, 1 },
};

static const u16 tokens[] = { 44, 596, 612 };
static bool overlap(const void *a, size_t na, const void *b, size_t nb)
{
	unsigned long x = (unsigned long)a, y = (unsigned long)b, ex, ey;

	return check_add_overflow(x, na, &ex) ||
	       check_add_overflow(y, nb, &ey) || (x < ey && y < ex);
}

bool m9x_faa_public_vs_follow_template_valid(const u8 *blob, size_t bytes)
{
	u8 digest[32];

	if (!blob || bytes != M9_FAA_VS_FOLLOW_TEMPLATE_BYTES)
		return false;
	sha256(blob, bytes, digest);
	return !memcmp(digest, template_sha, 32);
}

int m9x_faa_public_vs_follow_encode(const u8 *blob, size_t bytes,
				    const struct m9x_faa_graph_draw_owner
					    owner[M9_FAA_DRAW_SOURCE_OWNERS],
				    u32 live_primary_bytes, u64 prefix_point,
				    const struct m9x_faa_public_vs_follow_image
					    *out,
				    struct m9x_faa_public_vs_follow_endpoints
					    *ends)
{
	u32 words[ARRAY_SIZE(fields)], address, final, i, j;
	void *dest[3];
	size_t spans[3];

	if (!owner || !out || !ends || !out->primary || !out->secondary ||
	    out->primary_bytes < 48 || out->secondary_bytes < 616 ||
	    (live_primary_bytes != 4992 && live_primary_bytes != 4996) ||
	    (prefix_point & 15) || (u32)prefix_point < 0x100 ||
	    (u32)prefix_point > U32_MAX - 0x40 ||
	    !m9x_faa_public_vs_follow_template_valid(blob, bytes))
		return -EINVAL;
	if (m9x_faa_graph_draw_owners_validate(owner) ||
	    check_add_overflow(live_primary_bytes, 48u, &final) ||
	    final > owner[27].bytes || owner[29].bytes < 1372)
		return -ERANGE;
	dest[0] = out->primary;
	spans[0] = 48;
	dest[1] = out->secondary;
	spans[1] = 616;
	dest[2] = ends;
	spans[2] = sizeof(*ends);
	for (i = 0; i < 3; i++) {
		if (overlap(dest[i], spans[i], blob, bytes) ||
		    overlap(dest[i], spans[i], owner, 31 * sizeof(*owner)) ||
		    overlap(dest[i], spans[i], out, sizeof(*out)))
			return -EINVAL;
		for (j = i + 1; j < 3; j++)
			if (overlap(dest[i], spans[i], dest[j], spans[j]))
				return -EINVAL;
	}
	for (i = 0; i < ARRAY_SIZE(fields); i++) {
		const struct follow_fixup *f = &fields[i];

		if (f->offset > owner[f->owner].bytes ||
		    owner[f->owner].bytes - f->offset < 4 ||
		    check_add_overflow(owner[f->owner].base, f->offset,
				       &address) ||
		    (address & 3))
			return -ERANGE;
		words[i] = address | f->tag;
	}
	memcpy(out->primary, blob, 48);
	memcpy(out->secondary, blob + 48, 616);
	for (i = 0; i < ARRAY_SIZE(fields); i++) {
		u32 at = fields[i].at;

		m9_cmd_put(at < 48 ? out->primary + at :
				     out->secondary + at - 48,
			   words[i]);
	}
	for (i = 0; i < ARRAY_SIZE(tokens); i++)
		m9_cmd_put(out->secondary + tokens[i],
			   ((u32)prefix_point + (i + 1) * 0x10) | 1);
	*ends = (struct m9x_faa_public_vs_follow_endpoints){
		.final_end = final,
		/* Same-owner source 248f2 relative slot: high opcode preserved. */
		.patch_word = M9_FAA_VS_FOLLOW_PATCH_EXPECTED |
			      ((1284u - 704u) / 4),
		.original_wait = prefix_point + 0x20,
		.independent_finish = prefix_point + 0x30,
	};
	return 0;
}
