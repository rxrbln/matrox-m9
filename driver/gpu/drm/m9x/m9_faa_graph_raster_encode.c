// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include "m9_cmd.h"
#include "m9_faa_graph_raster_encode.h"

static u32 moved(u32 at, const u32 insert[4], u32 count)
{
	u32 i, delta = 0;

	for (i = 0; i < count; i++)
		if (at >= insert[i])
			delta += 12;
	return at + delta;
}

static int raster_encode(const struct m9x_faa_raster_input *in,
			 const struct m9x_faa_raster_output *out, bool two)
{
	u32 stages = two ? 2 : 4;
	u32 launch[4], binding[4], insert[4], branches[3], targets[3];
	u32 pointer[4], raster = 0, launches = 0, nb = 0, nr = 0, ni = 0;
	u32 at, i, n, end, source, destination, continuation;
	bool cull;

	if (!in || !out || !in->primary || !in->secondary || !out->primary ||
	    !out->secondary || !out->secondary_bytes || !in->primary_bytes ||
	    (in->primary_bytes & 3) || !in->secondary_bytes ||
	    (in->secondary_bytes & 3) || in->primary_bytes > 0x40000 ||
	    in->secondary_bytes > 0x40000 || (in->secondary_base & 3) ||
	    (in->flags & ~7u) || (in->flags & 6) == 6 ||
	    in->database_bytes < 8 || (in->database_bytes & 7) ||
	    (in->no_cull_start_bytes & 7) || (in->cull_start_bytes & 7) ||
	    in->no_cull_start_bytes > in->database_bytes - 8 ||
	    in->cull_start_bytes > in->database_bytes - 8 ||
	    in->no_cull_start_bytes == in->cull_start_bytes ||
	    in->no_cull_start_bytes / 8 > 0xffff ||
	    in->cull_start_bytes / 8 > 0xffff)
		return -EINVAL;
	cull = in->flags & 6;
	n = in->secondary_bytes;
	if (check_add_overflow(in->secondary_base, n, &end))
		return -ERANGE;
	/* These exact source packets mark four record consumers: declaration,
	 * gather, position and raster. Missing 6A binds are emitted by f760c
	 * immediately before 68/6B/6C, as in the original retained cull control.
	 */
	for (at = 0; at < n; at += 4) {
		u32 value = m9_cmd_get(in->secondary + at);

		if ((value & 0xffff0000) == 0x49d00000) {
			u32 distance = (value & 0xffff) * 4, target;

			if (nb == (two ? 0 : 3) || !distance ||
			    check_add_overflow(at, distance, &target) ||
			    target >= n)
				return -EINVAL;
			branches[nb] = at;
			targets[nb++] = target;
		}
		if (value == 3 && n - at >= 24 &&
		    m9_cmd_get(in->secondary + at + 4) == 0x456c6b68) {
			if (launches == stages ||
			    m9_cmd_get(in->secondary + at + 8) ||
			    m9_cmd_get(in->secondary + at + 12) != 0x1030)
				return -EINVAL;
			launch[launches] = at;
			binding[launches] = ~0u;
			if (at >= 8 &&
			    m9_cmd_get(in->secondary + at - 8) == 0x4545456a &&
			    (at < 12 ||
			     m9_cmd_get(in->secondary + at - 12) != 1))
				return -EINVAL;
			if (at >= 12 &&
			    m9_cmd_get(in->secondary + at - 12) == 1 &&
			    m9_cmd_get(in->secondary + at - 8) == 0x4545456a) {
				u32 bind = m9_cmd_get(in->secondary + at - 4);

				if ((bind & 0xffff) ||
				    bind >> 16 != in->no_cull_start_bytes / 8)
					return -EINVAL;
				binding[launches] = at - 4;
			} else if (cull) {
				insert[ni++] = at;
			}
			launches++;
		}
		/* Final raster F0/EF/D7 followed by C1. Preprocessing descriptors
		 * f0000400 deliberately do not satisfy this raster descriptor.
		 */
		if (value == 3 && n - at >= 32 &&
		    m9_cmd_get(in->secondary + at + 4) == 0x45d7eff0 &&
		    (m9_cmd_get(in->secondary + at + 8) & 0xff000000) ==
			    0xc4000000) {
			if (++nr != 1 ||
			    (m9_cmd_get(in->secondary + at + 12) &
			     0x03000000) ||
			    m9_cmd_get(in->secondary + at + 20) != 1 ||
			    m9_cmd_get(in->secondary + at + 24) != 0x45454522 ||
			    (m9_cmd_get(in->secondary + at + 28) & 0x4000))
				return -EINVAL;
			raster = at;
		}
	}
	if (launches != stages || nb != (two ? 0 : 3) || nr != 1 ||
	    binding[0] == ~0u || binding[stages - 1] == ~0u ||
	    raster <= launch[stages - 2] || raster >= launch[stages - 1] ||
	    (two && ni))
		return -EINVAL;
	for (at = 0; at < in->secondary_bytes; at += 4)
		if (m9_cmd_get(in->secondary + at) == 0x4545456a) {
			for (i = 0; i < stages; i++)
				if (binding[i] == at + 4)
					break;
			if (i == stages)
				return -EINVAL;
		}
	/* Strict endpoints establish two complete consecutive indirect ranges.
	 * Do not scan arbitrary address-looking words or rewrite other owners.
	 */
	for (i = 0; i < 4; i++) {
		at = in->range_at[i];
		if ((at & 3) || at > in->primary_bytes ||
		    in->primary_bytes - at < 4 ||
		    (i && at <= in->range_at[i - 1]))
			return -EINVAL;
		pointer[i] = m9_cmd_get(in->primary + at);
		if ((pointer[i] & 3) != (i & 1 ? 2u : 1u) ||
		    (pointer[i] & ~3u) < in->secondary_base ||
		    (pointer[i] & ~3u) > end)
			return -EINVAL;
		pointer[i] = (pointer[i] & ~3u) - in->secondary_base;
	}
	if (pointer[0] || !pointer[1] || pointer[1] != pointer[2] ||
	    pointer[1] >= n || pointer[3] != n)
		return -EINVAL;
	for (i = 0; i < ni; i++) {
		u32 j;

		for (j = 0; j < nb; j++)
			if (targets[j] == insert[i])
				return -EINVAL;
		if (pointer[1] == insert[i])
			return -EINVAL;
	}
	for (i = 0; i < nb; i++)
		if ((moved(targets[i], insert, ni) -
		     moved(branches[i], insert, ni)) /
			    4 >
		    0xffff)
			return -ERANGE;
	if (check_add_overflow(n, ni * 12, &n) ||
	    check_add_overflow(in->secondary_base, n, &end) ||
	    out->primary_capacity < in->primary_bytes ||
	    out->secondary_capacity < n)
		return -ERANGE;
	if (m9_cmd_ranges_overlap(out->primary, in->primary_bytes,
				  out->secondary, n) ||
	    m9_cmd_ranges_overlap(out->primary, in->primary_bytes, in->primary,
				  in->primary_bytes) ||
	    m9_cmd_ranges_overlap(out->primary, in->primary_bytes,
				  in->secondary, in->secondary_bytes) ||
	    m9_cmd_ranges_overlap(out->secondary, n, in->primary,
				  in->primary_bytes) ||
	    m9_cmd_ranges_overlap(out->secondary, n, in->secondary,
				  in->secondary_bytes) ||
	    m9_cmd_ranges_overlap(out->secondary_bytes,
				  sizeof(*out->secondary_bytes), out->primary,
				  in->primary_bytes) ||
	    m9_cmd_ranges_overlap(out->secondary_bytes,
				  sizeof(*out->secondary_bytes), out->secondary,
				  n) ||
	    m9_cmd_ranges_overlap(out->secondary_bytes,
				  sizeof(*out->secondary_bytes), in->primary,
				  in->primary_bytes) ||
	    m9_cmd_ranges_overlap(out->secondary_bytes,
				  sizeof(*out->secondary_bytes), in->secondary,
				  in->secondary_bytes))
		return -EINVAL;
	/* All validation precedes this first publication. */
	memcpy(out->primary, in->primary, in->primary_bytes);
	continuation =
		(cull ? in->cull_start_bytes : in->no_cull_start_bytes) / 8;
	destination = 0;
	source = destination;
	for (i = 0; i < ni; i++) {
		memcpy(out->secondary + destination, in->secondary + source,
		       insert[i] - source);
		destination += insert[i] - source;
		m9_cmd_put(out->secondary + destination, 1);
		m9_cmd_put(out->secondary + destination + 4, 0x4545456a);
		m9_cmd_put(out->secondary + destination + 8,
			   continuation << 16);
		destination += 12;
		source = insert[i];
	}
	memcpy(out->secondary + destination, in->secondary + source,
	       in->secondary_bytes - source);
	for (i = 0; i < stages; i++)
		if (binding[i] != ~0u)
			m9_cmd_put(out->secondary +
					   moved(binding[i], insert, ni),
				   (m9_cmd_get(in->secondary + binding[i]) &
				    0xffff) |
					   continuation << 16);
	for (i = 0; i < nb; i++)
		m9_cmd_put(out->secondary + moved(branches[i], insert, ni),
			   0x49d00000 | ((moved(targets[i], insert, ni) -
					  moved(branches[i], insert, ni)) /
					 4));
	for (i = 0; i < 4; i++)
		m9_cmd_put(out->primary + in->range_at[i],
			   (in->secondary_base +
			    moved(pointer[i], insert, ni)) |
				   (i & 1 ? 2u : 1u));
	m9_cmd_put(out->secondary + moved(raster + 12, insert, ni),
		   m9_cmd_get(in->secondary + raster + 12) |
			   (in->flags & M9_FAA_RASTER_CULL_CW  ? 0x03000000 :
			    in->flags & M9_FAA_RASTER_CULL_CCW ? 0x02000000 :
								 0));
	m9_cmd_put(out->secondary + moved(raster + 28, insert, ni),
		   m9_cmd_get(in->secondary + raster + 28) |
			   (in->flags & M9_FAA_RASTER_FLAT ? 0x4000 : 0));
	*out->secondary_bytes = n;
	return 0;
}

int m9x_faa_raster_encode(const struct m9x_faa_raster_input *in,
			  const struct m9x_faa_raster_output *out)
{
	return raster_encode(in, out, false);
}

/* Source FIRST count2 already has both resident6A bindings. No branch or
 * insertion is borrowed from the four-stage retained state family.
 */
int m9x_faa_raster_two_encode(const struct m9x_faa_raster_input *in,
			      const struct m9x_faa_raster_output *out)
{
	return raster_encode(in, out, true);
}
