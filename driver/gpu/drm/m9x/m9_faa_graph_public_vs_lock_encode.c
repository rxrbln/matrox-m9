// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* Natural public-VS normal Lock after same-graph actual160. No source pixel
 * input. Source3572 bytes,8 live owners/6 tokens/2 extents,3 END posts.
 */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <crypto/sha2.h>
#include "m9_faa_graph_public_vs_lock_encode.h"

#define SOURCE_START 5004u
static const u8 lock_sha256[SHA256_DIGEST_SIZE] = {
	0xbb, 0x5f, 0xb2, 0xde, 0x14, 0xdc, 0xc2, 0x89, 0x36, 0x46, 0x08,
	0x11, 0x35, 0x5b, 0xa8, 0xfa, 0x04, 0x98, 0x47, 0x60, 0xbc, 0xc8,
	0xa7, 0xe4, 0x94, 0x21, 0x05, 0x9a, 0x02, 0xe9, 0xde, 0x27
};

static const struct {
	u16 offset, owner, addend;
	u32 encoder;
} address_fixups[] = { { 5008, 22, 0, 0x24930 },   { 5072, 22, 644, 0x24930 },
		       { 5076, 22, 708, 0x24919 }, { 5084, 8, 8, 0x256f1 },
		       { 5096, 8, 12, 0x256f1 },   { 5108, 8, 16, 0x256f1 },
		       { 5196, 1, 0, 0x24930 },	   { 8224, 1, 0, 0x24930 } };
static const struct {
	u16 offset, delta, tag;
} timeline_fixups[] = { { 5132, 16, 1 }, { 5188, 32, 1 }, { 5324, 32, 0 },
			{ 8088, 32, 0 }, { 8552, 16, 0 }, { 8572, 48, 1 } };
static void lock_put32(u8 *p, u32 v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

bool m9x_faa_public_vs_lock_blob_valid(const u8 *blob, size_t bytes)
{
	u8 digest[SHA256_DIGEST_SIZE];

	if (!blob || bytes != M9_FAA_VS_LOCK_PRIMARY_BYTES)
		return false;
	sha256(blob, bytes, digest);
	return !memcmp(digest, lock_sha256, sizeof(digest));
}

int m9x_faa_public_vs_lock_encode(const u8 *blob, size_t bytes,
				  const struct m9x_faa_graph_draw_owner
					  owner[M9_FAA_DRAW_SOURCE_OWNERS],
				  u32 live_primary_bytes, u32 width, u32 height,
				  u64 draw_point, u8 *primary,
				  size_t primary_bytes)
{
	u32 values[ARRAY_SIZE(address_fixups)], address, i;

	if (!owner || !primary ||
	    primary_bytes < M9_FAA_VS_LOCK_PRIMARY_BYTES || width != 320 ||
	    height != 240 || (draw_point & 15) || (u32)draw_point < 0x160 ||
	    (u32)draw_point > U32_MAX - 0x40 ||
	    live_primary_bytes < SOURCE_START || (live_primary_bytes & 3) ||
	    owner[27].bytes < M9_FAA_VS_LOCK_PRIMARY_BYTES ||
	    live_primary_bytes >
		    owner[27].bytes - M9_FAA_VS_LOCK_PRIMARY_BYTES ||
	    !m9x_faa_public_vs_lock_blob_valid(blob, bytes))
		return -EINVAL;
	if (m9x_faa_graph_draw_owners_validate(owner))
		return -ERANGE;
	/* Refuse every invalid input before touching the output. */
	for (i = 0; i < ARRAY_SIZE(address_fixups); i++) {
		const struct m9x_faa_graph_draw_owner *o =
			&owner[address_fixups[i].owner];
		u32 addend = address_fixups[i].addend;

		if (addend > o->bytes || o->bytes - addend < 4 ||
		    check_add_overflow(o->base, addend, &address) ||
		    (address & 3))
			return -ERANGE;
		values[i] = (address_fixups[i].encoder == 0x24919 ?
				     address & 0xfffffffdu :
				     address) |
			    1;
	}
	memcpy(primary, blob, M9_FAA_VS_LOCK_PRIMARY_BYTES);
	for (i = 0; i < ARRAY_SIZE(address_fixups); i++)
		lock_put32(primary + address_fixups[i].offset - SOURCE_START,
			   values[i]);
	for (i = 0; i < ARRAY_SIZE(timeline_fixups); i++)
		lock_put32(primary + timeline_fixups[i].offset - SOURCE_START,
			   ((u32)draw_point + timeline_fixups[i].delta) |
				   timeline_fixups[i].tag);
	lock_put32(primary + 5168 - SOURCE_START, height << 16 | width);
	lock_put32(primary + 8560 - SOURCE_START, height << 16 | width);
	return 0;
}
