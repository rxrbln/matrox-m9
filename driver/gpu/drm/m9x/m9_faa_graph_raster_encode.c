// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include "m9_faa_graph_raster_encode.h"

static u32 get(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}
static void put(u8 *p, u32 word)
{
	p[0] = word; p[1] = word >> 8; p[2] = word >> 16; p[3] = word >> 24;
}
static bool overlap(const void *a, u32 na, const void *b, u32 nb)
{
	unsigned long x = (unsigned long)a, y = (unsigned long)b, xe, ye;
	return check_add_overflow(x, (unsigned long)na, &xe) ||
	       check_add_overflow(y, (unsigned long)nb, &ye) || (x < ye && y < xe);
}
static u32 moved(u32 at, const u32 insert[4], u32 count)
{
	u32 i, delta = 0;
	for (i = 0; i < count; i++)
		if (at >= insert[i]) delta += 12;
	return at + delta;
}

int m9x_faa_raster_encode(const struct m9x_faa_raster_input *in,
			 const struct m9x_faa_raster_output *out)
{
	u32 launch[4], binding[4], insert[4], branches[3], targets[3];
	u32 pointer[4], raster = 0, launches = 0, nb = 0, nr = 0, ni = 0;
	u32 at, i, n, end, source, destination, continuation;
	bool cull;

	if (!in || !out || !in->primary || !in->secondary || !out->primary ||
	    !out->secondary || !out->secondary_bytes ||
	    !in->primary_bytes || (in->primary_bytes & 3) ||
	    !in->secondary_bytes || (in->secondary_bytes & 3) ||
	    in->primary_bytes > 0x40000 || in->secondary_bytes > 0x40000 ||
	    (in->secondary_base & 3) || (in->flags & ~7u) ||
	    (in->flags & 6) == 6 || in->database_bytes < 8 || (in->database_bytes & 7) ||
	    (in->no_cull_start_bytes & 7) || (in->cull_start_bytes & 7) ||
	    in->no_cull_start_bytes > in->database_bytes - 8 ||
	    in->cull_start_bytes > in->database_bytes - 8 ||
	    in->no_cull_start_bytes == in->cull_start_bytes ||
	    in->no_cull_start_bytes / 8 > 0xffff ||
	    in->cull_start_bytes / 8 > 0xffff)
		return -EINVAL;
	cull = in->flags & 6;
	n = in->secondary_bytes;
	if (check_add_overflow(in->secondary_base, n, &end)) return -ERANGE;
	/* These exact source packets mark four record consumers: declaration,
	 * gather, position and raster. Missing 6A binds are emitted by f760c
	 * immediately before 68/6B/6C, as in the original retained cull control. */
	for (at = 0; at < n; at += 4) {
		u32 value = get(in->secondary + at);
		if ((value & 0xffff0000) == 0x49d00000) {
			u32 distance = (value & 0xffff) * 4, target;
			if (nb == 3 || !distance || check_add_overflow(at, distance, &target) || target >= n)
				return -EINVAL;
			branches[nb] = at; targets[nb++] = target;
		}
		if (value == 3 && n - at >= 24 &&
		    get(in->secondary + at + 4) == 0x456c6b68) {
			if (launches == 4 || get(in->secondary + at + 8) ||
			    get(in->secondary + at + 12) != 0x1030)
				return -EINVAL;
			launch[launches] = at;
			binding[launches] = ~0u;
			if (at >= 8 && get(in->secondary + at - 8) == 0x4545456a &&
			    (at < 12 || get(in->secondary + at - 12) != 1)) return -EINVAL;
			if (at >= 12 && get(in->secondary + at - 12) == 1 &&
			    get(in->secondary + at - 8) == 0x4545456a) {
				u32 bind = get(in->secondary + at - 4);
				if ((bind & 0xffff) || bind >> 16 != in->no_cull_start_bytes / 8) return -EINVAL;
				binding[launches] = at - 4;
			} else if (cull) insert[ni++] = at;
			launches++;
		}
		/* Final raster F0/EF/D7 followed by C1. Preprocessing descriptors
		 * f0000400 deliberately do not satisfy this raster descriptor. */
		if (value == 3 && n - at >= 32 &&
		    get(in->secondary + at + 4) == 0x45d7eff0 &&
		    (get(in->secondary + at + 8) & 0xff000000) == 0xc4000000) {
			if (++nr != 1 || (get(in->secondary + at + 12) & 0x03000000) ||
			    get(in->secondary + at + 20) != 1 ||
			    get(in->secondary + at + 24) != 0x45454522 ||
			    (get(in->secondary + at + 28) & 0x4000)) return -EINVAL;
			raster = at;
		}
	}
	if (launches != 4 || nb != 3 || nr != 1 || binding[0] == ~0u || binding[3] == ~0u ||
	    raster <= launch[2] || raster >= launch[3]) return -EINVAL;
	for (at = 0; at < in->secondary_bytes; at += 4)
		if (get(in->secondary + at) == 0x4545456a) {
			for (i = 0; i < 4; i++) if (binding[i] == at + 4) break;
			if (i == 4) return -EINVAL;
		}
	/* Strict endpoints establish two complete consecutive indirect ranges.
	 * Do not scan arbitrary address-looking words or rewrite other owners. */
	for (i = 0; i < 4; i++) {
		at = in->range_at[i];
		if ((at & 3) || at > in->primary_bytes || in->primary_bytes - at < 4 ||
		    (i && at <= in->range_at[i - 1])) return -EINVAL;
		pointer[i] = get(in->primary + at);
		if ((pointer[i] & 3) != (i & 1 ? 2u : 1u) ||
		    (pointer[i] & ~3u) < in->secondary_base ||
		    (pointer[i] & ~3u) > end) return -EINVAL;
		pointer[i] = (pointer[i] & ~3u) - in->secondary_base;
	}
	if (pointer[0] || !pointer[1] || pointer[1] != pointer[2] ||
	    pointer[1] >= n || pointer[3] != n) return -EINVAL;
	for (i = 0; i < ni; i++) {
		u32 j;
		for (j = 0; j < nb; j++) if (targets[j] == insert[i]) return -EINVAL;
		if (pointer[1] == insert[i]) return -EINVAL;
	}
	for (i = 0; i < nb; i++)
		if ((moved(targets[i], insert, ni) - moved(branches[i], insert, ni)) / 4 > 0xffff)
			return -ERANGE;
	if (check_add_overflow(n, ni * 12, &n) ||
	    check_add_overflow(in->secondary_base, n, &end) ||
	    out->primary_capacity < in->primary_bytes || out->secondary_capacity < n)
		return -ERANGE;
	if (overlap(out->primary, in->primary_bytes, out->secondary, n) ||
	    overlap(out->primary, in->primary_bytes, in->primary, in->primary_bytes) ||
	    overlap(out->primary, in->primary_bytes, in->secondary, in->secondary_bytes) ||
	    overlap(out->secondary, n, in->primary, in->primary_bytes) ||
	    overlap(out->secondary, n, in->secondary, in->secondary_bytes) ||
	    overlap(out->secondary_bytes, sizeof(*out->secondary_bytes), out->primary, in->primary_bytes) ||
	    overlap(out->secondary_bytes, sizeof(*out->secondary_bytes), out->secondary, n) ||
	    overlap(out->secondary_bytes, sizeof(*out->secondary_bytes), in->primary, in->primary_bytes) ||
	    overlap(out->secondary_bytes, sizeof(*out->secondary_bytes), in->secondary, in->secondary_bytes)) return -EINVAL;
	/* All validation precedes this first publication. */
	memcpy(out->primary, in->primary, in->primary_bytes);
	continuation = (cull ? in->cull_start_bytes : in->no_cull_start_bytes) / 8;
	source = destination = 0;
	for (i = 0; i < ni; i++) {
		memcpy(out->secondary + destination, in->secondary + source, insert[i] - source);
		destination += insert[i] - source;
		put(out->secondary + destination, 1);
		put(out->secondary + destination + 4, 0x4545456a);
		put(out->secondary + destination + 8, continuation << 16);
		destination += 12; source = insert[i];
	}
	memcpy(out->secondary + destination, in->secondary + source, in->secondary_bytes - source);
	for (i = 0; i < 4; i++) if (binding[i] != ~0u)
		put(out->secondary + moved(binding[i], insert, ni),
		    (get(in->secondary + binding[i]) & 0xffff) | continuation << 16);
	for (i = 0; i < nb; i++)
		put(out->secondary + moved(branches[i], insert, ni),
		    0x49d00000 | ((moved(targets[i], insert, ni) - moved(branches[i], insert, ni)) / 4));
	for (i = 0; i < 4; i++) put(out->primary + in->range_at[i],
		(in->secondary_base + moved(pointer[i], insert, ni)) | (i & 1 ? 2u : 1u));
	put(out->secondary + moved(raster + 12, insert, ni),
	    get(in->secondary + raster + 12) |
	    (in->flags & M9_FAA_RASTER_CULL_CW ? 0x03000000 :
	     in->flags & M9_FAA_RASTER_CULL_CCW ? 0x02000000 : 0));
	put(out->secondary + moved(raster + 28, insert, ni),
	    get(in->secondary + raster + 28) | (in->flags & M9_FAA_RASTER_FLAT ? 0x4000 : 0));
	*out->secondary_bytes = n;
	return 0;
}
