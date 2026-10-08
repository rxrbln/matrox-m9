// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* Normal Lock prefix, source plan RE 9834d2a5. Eight owner fields, five
 * timeline values and two extents are live. Remaining bytes are source-known
 * fixed diagnostic state/program, explicitly bounded to the red 320x240
 * profile. In particular the two shader-loader E3 payloads come from the
 * genuine 25860 timeline getter, not from immutable shader constants.
 */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <crypto/sha2.h>
#include "m9_cmd.h"
#include "m9_faa_graph_lock_encode.h"

#define SOURCE_START 4992u
static const u8 lock_sha256[SHA256_DIGEST_SIZE] = {
	0x39, 0x44, 0x0c, 0x4d, 0x8c, 0xfe, 0x84, 0xb2, 0x8f, 0x97, 0x79,
	0x2e, 0xd3, 0x60, 0x6b, 0x11, 0x37, 0xee, 0x55, 0xb3, 0x81, 0xdf,
	0x1f, 0x2f, 0x32, 0xa2, 0xef, 0x62, 0x04, 0x81, 0x31, 0xde,
};

static const struct {
	u16 offset, owner, addend;
	u32 encoder;
} address_fixups[] = {
	{ 4996, 22, 0, 0x24930 },   { 5060, 22, 644, 0x24930 },
	{ 5064, 22, 708, 0x24919 }, { 5072, 8, 8, 0x256f1 },
	{ 5084, 8, 12, 0x256f1 },   { 5096, 8, 16, 0x256f1 },
	{ 5184, 1, 0, 0x24930 },    { 8212, 1, 0, 0x24930 },
};

static const struct {
	u16 offset, delta, tag;
} timeline_fixups[] = {
	{ 5120, 0x10, 1 }, { 5176, 0x20, 1 }, { 5312, 0x20, 0 },
	{ 8076, 0x20, 0 }, { 8544, 0x30, 1 },
};

bool m9x_faa_graph_lock_blob_valid(const u8 *blob, size_t bytes)
{
	u8 digest[SHA256_DIGEST_SIZE];

	if (!blob || bytes != M9_FAA_LOCK_PRIMARY_BYTES)
		return false;
	sha256(blob, bytes, digest);
	return !memcmp(digest, lock_sha256, sizeof(digest));
}

int m9x_faa_graph_lock_encode(const u8 *blob, size_t bytes,
			      const struct m9x_faa_graph_draw_owner
				      owner[M9_FAA_DRAW_SOURCE_OWNERS],
			      u32 live_primary_bytes, u32 width, u32 height,
			      u64 draw_point, u8 *primary, size_t primary_bytes)
{
	u32 values[ARRAY_SIZE(address_fixups)], address, i;

	if (!owner || !primary || primary_bytes < M9_FAA_LOCK_PRIMARY_BYTES ||
	    width != 320 || height != 240 || (draw_point & 15) ||
	    (u32)draw_point < 0x140 || (u32)draw_point > U32_MAX - 0x40 ||
	    live_primary_bytes < SOURCE_START || (live_primary_bytes & 3) ||
	    owner[27].bytes < M9_FAA_LOCK_PRIMARY_BYTES ||
	    live_primary_bytes > owner[27].bytes - M9_FAA_LOCK_PRIMARY_BYTES ||
	    !m9x_faa_graph_lock_blob_valid(blob, bytes))
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
	memcpy(primary, blob, M9_FAA_LOCK_PRIMARY_BYTES);
	for (i = 0; i < ARRAY_SIZE(address_fixups); i++)
		m9_cmd_put(primary + address_fixups[i].offset - SOURCE_START,
			   values[i]);
	for (i = 0; i < ARRAY_SIZE(timeline_fixups); i++)
		m9_cmd_put(primary + timeline_fixups[i].offset - SOURCE_START,
			   ((u32)draw_point + timeline_fixups[i].delta) |
				   timeline_fixups[i].tag);
	m9_cmd_put(primary + 5156 - SOURCE_START, height << 16 | width);
	m9_cmd_put(primary + 8532 - SOURCE_START, height << 16 | width);
	return 0;
}
