// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* Source original HEAD -> returned public DRAW. No independent finish. */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <crypto/sha2.h>
#include "m9_faa_graph_public_vs_draw_encode.h"

static const u8 template_sha[32] = { 0x9f, 0xa2, 0x08, 0xc5, 0xb5, 0x8c, 0x1b,
				     0x78, 0x2d, 0x0c, 0x53, 0x53, 0x05, 0xd6,
				     0xdf, 0x08, 0xb1, 0x22, 0xc2, 0x40, 0x66,
				     0xef, 0x5c, 0xc5, 0x51, 0x8e, 0x13, 0x13,
				     0x63, 0x06, 0xd5, 0x14 };
struct draw_field {
	u16 at, owner;
	u32 offset, tag, pc;
};

static const struct draw_field fields[] = {
	{ 4, 29, 0, 1, 0x256f1 },	   { 8, 29, 1840, 2, 0x256f1 },
	{ 52, 29, 1840, 1, 0x256f1 },	   { 56, 29, 2688, 2, 0x256f1 },
	{ 176, 24, 0, 1, 0x24930 },	   { 192, 24, 0, 1, 0x24930 },
	{ 208, 24, 0, 1, 0x24930 },	   { 388, 28, 384, 1, 0x24930 },
	{ 408, 26, 32, 1, 0x24886 },	   { 412, 26, 32, 1, 0x248ab },
	{ 432, 28, 32, 1, 0x248c8 },	   { 436, 28, 48, 1, 0x248c8 },
	{ 644, 12, 0, 1, 0x24886 },	   { 676, 28, 256, 1, 0x248c8 },
	{ 864, 23, 0, 1, 0x24930 },	   { 880, 23, 873472, 1, 0x24930 },
	{ 896, 23, 3493888, 1, 0x24930 },  { 1036, 26, 88, 1, 0x24886 },
	{ 1040, 26, 88, 1, 0x248ab },	   { 1060, 28, 48, 1, 0x248c8 },
	{ 1064, 28, 48, 1, 0x248c8 },	   { 1092, 24, 0, 1, 0x24930 },
	{ 1120, 24, 0, 1, 0x24930 },	   { 1280, 12, 0, 1, 0x24886 },
	{ 1312, 28, 256, 1, 0x248c8 },	   { 1476, 23, 1746944, 1, 0x24930 },
	{ 1492, 23, 2620416, 1, 0x24930 }, { 1508, 23, 3493888, 1, 0x24930 },
	{ 1648, 26, 128, 1, 0x24886 },	   { 1652, 26, 128, 1, 0x248ab },
	{ 1672, 28, 48, 1, 0x248c8 },	   { 1676, 28, 152, 1, 0x248c8 },
	{ 1704, 24, 0, 1, 0x24930 },	   { 1840, 12, 0, 1, 0x24886 },
	{ 1872, 28, 256, 1, 0x248c8 },	   { 2088, 8, 0, 1, 0x24930 },
	{ 2092, 8, 20, 1, 0x24909 },	   { 2120, 1, 0, 1, 0x24930 },
	{ 2136, 4, 0, 1, 0x24930 },	   { 2152, 5, 0, 1, 0x24930 },
	{ 2168, 25, 0, 1, 0x24930 },	   { 2180, 22, 0, 1, 0x24930 },
	{ 2212, 2, 0, 1, 0x24930 },	   { 2216, 3, 0, 1, 0x24930 },
	{ 2360, 23, 2620416, 1, 0x24930 }, { 2364, 23, 3493888, 1, 0x24930 },
	{ 2380, 23, 1746944, 1, 0x24930 }, { 2400, 26, 0, 1, 0x24886 },
	{ 2404, 26, 0, 1, 0x248ab },	   { 2424, 28, 0, 1, 0x248c8 },
	{ 2428, 28, 32, 1, 0x248c8 },	   { 2700, 12, 0, 1, 0x24886 },
	{ 2732, 28, 256, 1, 0x248c8 },
};

struct draw_token {
	u16 at;
	u32 delta, flag;
};

static const struct draw_token tokens[] = {
	{ 56, 16, 1 },	  { 104, 32, 1 },   { 732, 48, 1 },   { 792, 64, 1 },
	{ 1344, 80, 1 },  { 1404, 96, 1 },  { 1852, 112, 1 }, { 1880, 128, 1 },
	{ 1952, 144, 1 }, { 2048, 160, 1 }, { 744, 48, 0 },   { 1356, 80, 0 },
	{ 1892, 128, 0 },
};

struct draw_relative {
	u16 at, target;
};

static const struct draw_relative lengths[] = { { 28, 660 },
						{ 704, 1280 },
						{ 1316, 1828 } };
static void put32(u8 *p, u32 w)
{
	p[0] = w;
	p[1] = w >> 8;
	p[2] = w >> 16;
	p[3] = w >> 24;
}

static bool overlap(const void *a, size_t na, const void *b, size_t nb)
{
	unsigned long x = (unsigned long)a, y = (unsigned long)b, ex, ey;

	return check_add_overflow(x, na, &ex) ||
	       check_add_overflow(y, nb, &ey) || (x < ey && y < ex);
}

bool m9x_faa_public_vs_draw_template_valid(const u8 *blob, size_t bytes)
{
	u8 digest[32];

	if (!blob || bytes != M9_FAA_VS_DRAW_TEMPLATE_BYTES)
		return false;
	sha256(blob, bytes, digest);
	return !memcmp(digest, template_sha, 32);
}

int m9x_faa_public_vs_draw_encode(const u8 *blob, size_t bytes,
				  const struct m9x_faa_graph_draw_owner
					  owner[M9_FAA_DRAW_SOURCE_OWNERS],
				  u32 live_primary_bytes, u64 head_point,
				  u32 fs_begin, u32 fs_end,
				  const struct m9x_faa_public_vs_draw_image *out,
				  struct m9x_faa_public_vs_draw_endpoints *ends)
{
	u32 words[ARRAY_SIZE(fields)], address, final, i, k;
	void *dest[3];
	size_t spans[3];

	if (!owner || !out || !ends || !out->primary || !out->secondary ||
	    out->primary_bytes < 60 || out->secondary_bytes < 2688 ||
	    (live_primary_bytes != 4944 && live_primary_bytes != 4948) ||
	    (head_point & 15) || (fs_begin & 7) || (fs_end & 7) ||
	    fs_begin > fs_end || fs_end - fs_begin < 16 || fs_end > 152 ||
	    (u32)head_point < 0xc0 || (u32)head_point > U32_MAX - 0xb0 ||
	    !m9x_faa_public_vs_draw_template_valid(blob, bytes))
		return -EINVAL;
	if (m9x_faa_graph_draw_owners_validate(owner) ||
	    check_add_overflow(live_primary_bytes, 60u, &final) ||
	    final > owner[27].bytes || owner[29].bytes < 2688)
		return -ERANGE;
	dest[0] = out->primary;
	spans[0] = 60;
	dest[1] = out->secondary;
	spans[1] = 2688;
	dest[2] = ends;
	spans[2] = sizeof(*ends);
	for (i = 0; i < 3; i++) {
		if (overlap(dest[i], spans[i], blob, bytes) ||
		    overlap(dest[i], spans[i], owner, 31 * sizeof(*owner)) ||
		    overlap(dest[i], spans[i], out, sizeof(*out)))
			return -EINVAL;
		for (k = i + 1; k < 3; k++)
			if (overlap(dest[i], spans[i], dest[k], spans[k]))
				return -EINVAL;
	}
	for (i = 0; i < ARRAY_SIZE(fields); i++) {
		const struct draw_field *f = &fields[i];
		u32 offset = f->offset;
		/* The live FS e0/e1 operands address pairs relative to this bound span,
		 * independently of the immutable utility pairs preceding the caller's FS.
		 */
		if (f->at == 2424)
			offset = fs_begin;
		if (f->at == 2428)
			offset = fs_end;
		if (offset > owner[f->owner].bytes ||
		    owner[f->owner].bytes - offset < 4 ||
		    check_add_overflow(owner[f->owner].base, offset,
				       &address) ||
		    (address & 3))
			return -ERANGE;
		switch (f->pc) {
		case 0x24886:
			words[i] = (address & 0xfffff000) | f->tag |
				   (!address ? 4 : 0);
			break;
		case 0x248ab:
			if (address & 7)
				return -ERANGE;
			words[i] = (address & 0xfff) >> 3;
			break;
		case 0x24909:
			words[i] = (address | f->tag) & 0xfffffffc;
			break;
		case 0x24930:
		case 0x248c8:
		case 0x256f1:
			words[i] = address | f->tag;
			break;
		default:
			return -EINVAL;
		}
	}
	memcpy(out->primary, blob, 60);
	memcpy(out->secondary, blob + 60, 2688);
	for (i = 0; i < ARRAY_SIZE(fields); i++)
		put32(fields[i].at < 60 ? out->primary + fields[i].at :
					  out->secondary + fields[i].at - 60,
		      words[i]);
	for (i = 0; i < ARRAY_SIZE(tokens); i++)
		put32(out->secondary + tokens[i].at,
		      ((u32)head_point + tokens[i].delta) | tokens[i].flag);
	for (i = 0; i < ARRAY_SIZE(lengths); i++)
		put32(out->secondary + lengths[i].at,
		      0x49d00000 | ((lengths[i].target - lengths[i].at) / 4));
	put32(out->secondary + 2360, 0x00500000u | ((fs_end - fs_begin) / 8u)
							   << 8);
	/* Current authenticated program/geometry interface admits only320x240. */
	put32(out->secondary + 2244, (240u << 16) | 320u);
	put32(out->secondary + 2252, (240u << 16) | 320u);
	*ends = (struct m9x_faa_public_vs_draw_endpoints){ final,
							   head_point + 0xa0 };
	return 0;
}
