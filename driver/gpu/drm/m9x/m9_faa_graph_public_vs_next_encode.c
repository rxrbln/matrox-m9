// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* Same retained bounded328B public MOV program; source-known closed DRAW.
 * All owners, cursors, tokens, lengths, dimensions and FS ranges are live.
 */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <crypto/sha2.h>
#include "m9_faa_graph_public_vs_next_encode.h"

static const u8 next_sha[32] = {
	0x88, 0xb6, 0x3c, 0x37, 0xed, 0x40, 0xbe, 0xcb, 0xbd, 0x57, 0x9b,
	0x07, 0x90, 0x49, 0xff, 0x62, 0xb6, 0x2f, 0xde, 0x60, 0xb2, 0x59,
	0x63, 0xa2, 0x28, 0xe0, 0xbe, 0xde, 0x0c, 0x87, 0xc5, 0x58
};

static const struct {
	u16 at, owner;
	u32 offset, tag, pc;
} fields[] = {
	{ 4, 29, 2688, 1, 0x24cb2 },	   { 8, 29, 4388, 2, 0x24cd5 },
	{ 52, 29, 4388, 1, 0x24cb2 },	   { 56, 29, 5236, 2, 0x24cd5 },
	{ 180, 24, 0, 1, 0x24932 },	   { 196, 24, 0, 1, 0x24932 },
	{ 212, 24, 0, 1, 0x24932 },	   { 392, 28, 512, 1, 0x24932 },
	{ 412, 26, 0, 1, 0x248a0 },	   { 436, 28, 32, 1, 0x248d1 },
	{ 440, 28, 48, 1, 0x248d1 },	   { 748, 12, 0, 1, 0x248a0 },
	{ 780, 28, 256, 1, 0x248d1 },	   { 968, 23, 0, 1, 0x24932 },
	{ 984, 23, 873472, 1, 0x24932 },   { 1000, 23, 3493888, 1, 0x24932 },
	{ 1040, 26, 0, 1, 0x248a0 },	   { 1064, 28, 48, 1, 0x248d1 },
	{ 1068, 28, 48, 1, 0x248d1 },	   { 1096, 24, 0, 1, 0x24932 },
	{ 1124, 24, 0, 1, 0x24932 },	   { 1292, 28, 256, 1, 0x248d1 },
	{ 1456, 23, 1746944, 1, 0x24932 }, { 1472, 23, 2620416, 1, 0x24932 },
	{ 1532, 26, 0, 1, 0x248a0 },	   { 1556, 28, 48, 1, 0x248d1 },
	{ 1560, 28, 152, 1, 0x248d1 },	   { 1588, 24, 0, 1, 0x24932 },
	{ 1732, 28, 256, 1, 0x248d1 },	   { 1948, 8, 0, 1, 0x24932 },
	{ 1952, 8, 20, 0, 0x2490e },	   { 1980, 1, 0, 1, 0x24932 },
	{ 1996, 4, 0, 1, 0x24932 },	   { 2012, 5, 0, 1, 0x24932 },
	{ 2028, 25, 0, 1, 0x24932 },	   { 2040, 22, 0, 1, 0x24932 },
	{ 2072, 2, 0, 1, 0x24932 },	   { 2076, 3, 0, 1, 0x24932 },
	{ 2220, 23, 2620416, 1, 0x24932 }, { 2224, 23, 3493888, 1, 0x24932 },
	{ 2240, 23, 1746944, 1, 0x24932 }, { 2260, 26, 0, 1, 0x248a0 },
	{ 2284, 28, 0, 1, 0x248d1 },	   { 2288, 28, 32, 1, 0x248d1 },
	{ 2560, 12, 0, 1, 0x248a0 },	   { 2592, 28, 256, 1, 0x248d1 }
};

static const struct {
	u16 at, delta, tag;
} tokens[] = { { 60, 16, 1 },	 { 108, 32, 1 },   { 836, 48, 1 },
	       { 896, 64, 1 },	 { 1324, 80, 1 },  { 1384, 96, 1 },
	       { 1712, 112, 1 }, { 1740, 128, 1 }, { 1812, 144, 1 },
	       { 1908, 160, 1 }, { 848, 48, 0 },   { 1336, 80, 0 },
	       { 1752, 128, 0 } };
static const struct {
	u16 at, target;
} lengths[] = { { 28, 764 }, { 808, 1260 }, { 1296, 1688 } };
static const u16 fixed_systemva[] = { 772, 776, 780, 1268, 1272, 1276 };
static void next_put32(u8 *p, u32 w)
{
	p[0] = w;
	p[1] = w >> 8;
	p[2] = w >> 16;
	p[3] = w >> 24;
}

static bool next_overlap(const void *a, size_t n, const void *b, size_t m)
{
	unsigned long x = (unsigned long)a, y = (unsigned long)b, ex, ey;

	return check_add_overflow(x, n, &ex) || check_add_overflow(y, m, &ey) ||
	       (x < ey && y < ex);
}

bool m9x_faa_public_vs_next_blob_valid(const u8 *blob, size_t bytes)
{
	u8 hash[32];

	if (!blob || bytes != 2608)
		return false;
	sha256(blob, bytes, hash);
	return !memcmp(hash, next_sha, 32);
}

int m9x_faa_public_vs_next_vertices_valid(const u32 v[24])
{
	u32 i, j;

	if (!v)
		return -EINVAL;
	for (i = 0; i < 3; i++) {
		for (j = 0; j < 2; j++)
			if ((v[i * 8 + j] & 0x7fffffffu) > 0x3f800000u)
				return -ERANGE;
		if (v[i * 8 + 2] > 0x3f800000u || v[i * 8 + 3] != 0x3f800000u)
			return -ERANGE;
		for (j = 4; j < 8; j++)
			if (v[i * 8 + j] > 0x3f800000u)
				return -ERANGE;
	}
	return 0;
}

int m9x_faa_public_vs_next_encode(const u8 *blob, size_t bytes,
				  const struct m9x_faa_graph_draw_owner
					  owner[M9_FAA_DRAW_SOURCE_OWNERS],
				  u32 primary_cursor, u32 secondary_cursor,
				  u32 vertex_cursor, u64 unlock_point,
				  u32 fs_begin, u32 fs_end,
				  const struct m9x_faa_public_vs_next_image *out)
{
	u32 values[ARRAY_SIZE(fields)], i, j, offset, address;
	void *dest[2];
	size_t spans[2];

	if (!owner || !out || !out->primary || !out->secondary ||
	    out->primary_bytes < 60 || out->secondary_bytes < 2548 ||
	    (primary_cursor & 3) || primary_cursor < 8672 ||
	    (secondary_cursor & 3) || secondary_cursor < 2688 ||
	    (vertex_cursor & 127) || vertex_cursor < 512 ||
	    (unlock_point & 15) || (u32)unlock_point < 0x1b0 ||
	    (u32)unlock_point > U32_MAX - 0xe0 || (fs_begin & 7) ||
	    (fs_end & 7) || fs_begin > fs_end || fs_end - fs_begin < 16 ||
	    fs_end > 152 || !m9x_faa_public_vs_next_blob_valid(blob, bytes))
		return -EINVAL;
	if (m9x_faa_graph_draw_owners_validate(owner) ||
	    primary_cursor > owner[27].bytes ||
	    owner[27].bytes - primary_cursor < 60 ||
	    owner[29].bytes < 2548 + 512 + 48 ||
	    secondary_cursor > owner[29].bytes - 2548 - 512 - 48 ||
	    owner[28].bytes < 128 || vertex_cursor > owner[28].bytes - 128)
		return -ERANGE;
	dest[0] = out->primary;
	spans[0] = 60;
	dest[1] = out->secondary;
	spans[1] = 2548;
	for (i = 0; i < 2; i++) {
		if (next_overlap(dest[i], spans[i], blob, bytes) ||
		    next_overlap(dest[i], spans[i], owner,
				 31 * sizeof(*owner)) ||
		    next_overlap(dest[i], spans[i], out, sizeof(*out)))
			return -EINVAL;
		for (j = i + 1; j < 2; j++)
			if (next_overlap(dest[i], spans[i], dest[j], spans[j]))
				return -EINVAL;
	}
	for (i = 0; i < ARRAY_SIZE(fields); i++) {
		offset = fields[i].offset;
		if (fields[i].owner == 29)
			offset = secondary_cursor + offset - 2688;
		if (fields[i].owner == 28 && fields[i].offset == 512)
			offset = vertex_cursor;
		if (fields[i].at == 60 + 4912 - 2688)
			offset = fs_begin;
		if (fields[i].at == 60 + 4916 - 2688)
			offset = fs_end;
		if (offset > owner[fields[i].owner].bytes ||
		    owner[fields[i].owner].bytes - offset < 4 ||
		    check_add_overflow(owner[fields[i].owner].base, offset,
				       &address) ||
		    (address & 3))
			return -ERANGE;
		if (fields[i].pc == 0x248a0)
			values[i] = (address & 0xfffff000) | fields[i].tag |
				    (!address ? 4 : 0);
		else if (fields[i].pc == 0x2490e)
			values[i] = (address | fields[i].tag) & 0xfffffffcu;
		else
			values[i] = address | fields[i].tag;
	}
	memcpy(out->primary, blob, 60);
	memcpy(out->secondary, blob + 60, 2548);
	for (i = 0; i < ARRAY_SIZE(fields); i++)
		next_put32(fields[i].at < 60 ?
				   out->primary + fields[i].at :
				   out->secondary + fields[i].at - 60,
			   values[i]);
	for (i = 0; i < ARRAY_SIZE(tokens); i++)
		next_put32(out->secondary + tokens[i].at,
			   ((u32)unlock_point + tokens[i].delta) |
				   tokens[i].tag);
	for (i = 0; i < ARRAY_SIZE(lengths); i++)
		next_put32(out->secondary + lengths[i].at,
			   0x49d00000 |
				   ((lengths[i].target - lengths[i].at) / 4));
	for (i = 0; i < ARRAY_SIZE(fixed_systemva); i++)
		next_put32(out->secondary + fixed_systemva[i], 0x40004000);
	next_put32(out->secondary + 4792 - 2688, (240u << 16) | 320u);
	next_put32(out->secondary + 4800 - 2688, (240u << 16) | 320u);
	next_put32(out->secondary + 4908 - 2688,
		   0x00500000u | ((fs_end - fs_begin) / 8u) << 8);
	return 0;
}
