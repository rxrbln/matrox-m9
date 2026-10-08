// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* Original M9128 retained dual draw, constructed from live owned stages. */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include "m9_cmd.h"
#include "m9_faa_graph_dual_later_encode.h"

/* The source drops FIRST-only state groups after it has retained the graph
 * targets. These are packet boundaries, not a copied captured command image.
 * All C2/C3, geometry, sampler and shader words come from fresh_secondary.
 */
struct span {
	u16 from, to, bytes;
};

static const struct span stage_spans[] = {
	{ 0, 0, 456 },	      { 460, 560, 440 },  { 1000, 1000, 284 },
	{ 1308, 1284, 204 },  { 1588, 1488, 20 }, { 1628, 1508, 216 },
	{ 1868, 1724, 1012 },
};

static const u32 null_target[] = {
	0x00040118, 3,		0x457a7978, 0,		0xcc010001, 0x00010001,
	3,	    0x45877d7c, 0x200,	    0x0ff80000, 0,	    4,
	0x85848382, 0,		0,	    0,		0,	    4,
	0x7f7e8180, 0xffffffff, 0,	    0,		0,	    1,
};

int m9x_faa_dual_later_encode(const u8 *sample, size_t sample_bytes,
			      const struct m9x_faa_graph_draw_owner owner[34],
			      const struct m9x_faa_depth_views *views,
			      u32 pcursor, u32 scursor, u64 previous, u64 next,
			      u32 clear_flags,
			      const u8 fresh_secondary[M9_FAA_DUAL_FIRST_SECONDARY_BYTES],
			      const u8 fresh_tail[M9_FAA_DUAL_FIRST_PRIMARY_TAIL_BYTES],
			      struct m9x_faa_dual_later_image *out,
			      struct m9x_faa_dual_later_scratch *scratch)
{
	struct m9x_faa_later_depth_clear_image clear_image;
	u32 front = 0, sec_bytes, tail_bytes, total, mid, epoch, i, mask;
	u8 *s, *p;
	int ret;
	bool clear = clear_flags == 3, depth = clear_flags == 2;

	if (!sample || !owner || !views || !fresh_secondary || !fresh_tail ||
	    !out || !out->primary || !out->secondary || !scratch ||
	    (clear_flags && clear_flags != 3 && clear_flags != 2) ||
	    (pcursor & 3) ||
	    (scursor & 3) || (previous & 15) || previous < 0x130 ||
	    previous > ~(u64)0 - 0x150 || next != previous + 0x10 ||
	    (u32)previous > 0xfffffeafu)
		return -EINVAL;
	ret = m9x_faa_graph_draw_owners_validate(owner);
	if (ret)
		return ret;
	/* Source-qualified 320x240 and 1024x768 continuations; the fresh FIRST
	 * stage and tail already carry the live dimension words.
	 */
	if (!m9x_faa_runtime_shape(views->owner[M9_FAA_BOUND_COLOR].width,
				   views->owner[M9_FAA_BOUND_COLOR].height))
		return -EINVAL;
	for (i = 0; i < 3; i++)
		if (owner[31 + i].base != views->owner[i].base ||
		    owner[31 + i].bytes < views->owner[i].capacity_bytes)
			return -EINVAL;
	sec_bytes = clear ? M9_FAA_DUAL_LATER_SECONDARY_CLEAR :
			    M9_FAA_DUAL_LATER_SECONDARY_NOCLEAR;
	tail_bytes = clear ? M9_FAA_DUAL_LATER_TAIL_CLEAR :
			     M9_FAA_DUAL_LATER_TAIL_NOCLEAR;
	if (scursor > owner[29].bytes ||
	    sec_bytes > owner[29].bytes - scursor || scursor > 0x3fe00 ||
	    sec_bytes > 0x3fe00 - scursor ||
	    (u64)owner[29].base + scursor + sec_bytes > U32_MAX)
		return -ERANGE;
	/* Original dual count4 prefixes: CLEAR3, or depth-only (no sampled
	 * colour clear) followed by the no-clear DRAW/Lock form later on.
	 */
	if (clear_flags) {
		clear_image = (struct m9x_faa_later_depth_clear_image){
			.primary = scratch->clear.prefix,
			.capacity = sizeof(scratch->clear.prefix)
		};
		ret = m9x_faa_later_depth_four_clear_encode(sample, sample_bytes,
							    owner, views, 0,
							    previous,
							    clear_flags,
							    &clear_image,
							    &scratch->clear);
		if (ret)
			return ret;
		front = clear_image.bytes;
	}
	/* The source no-clear span opens with the 96-byte F021->F022 copy that
	 * the public Unlock already posted; repeating it corrupts retained pixels.
	 */
	total = front + tail_bytes;
	if (pcursor > owner[27].bytes || total > owner[27].bytes - pcursor ||
	    out->primary_capacity < total ||
	    out->secondary_capacity < sec_bytes ||
	    (u64)owner[27].base + pcursor + total > U32_MAX)
		return -ERANGE;
	if (m9_cmd_overlap(out->primary, total, out->secondary, sec_bytes) ||
	    m9_cmd_overlap(out->primary, total, fresh_secondary, 2880) ||
	    m9_cmd_overlap(out->primary, total, fresh_tail, 3624) ||
	    m9_cmd_overlap(out->secondary, sec_bytes, fresh_secondary, 2880) ||
	    m9_cmd_overlap(out->secondary, sec_bytes, fresh_tail, 3624))
		return -EINVAL;
	epoch = (u32)previous - (clear ? 0 : depth ? 0x30 : 0x80);
	if (m9_cmd_get(fresh_secondary + 4) != 0x454545c7 ||
	    m9_cmd_get(fresh_secondary + 56) != ((epoch + 0x90) | 1) ||
	    m9_cmd_get(fresh_secondary + 104) != ((epoch + 0xa0) | 1) ||
	    m9_cmd_get(fresh_tail) != M9_CMD_LINK ||
	    m9_cmd_get(fresh_tail + 48) != M9_CMD_LINK ||
	    m9_cmd_get(fresh_tail + 60) != 0x45454524 ||
	    m9_cmd_get(fresh_tail + 188) != ((epoch + 0x130) | 1) ||
	    m9_cmd_get(fresh_tail + 3620) != ((epoch + 0x150) | 1))
		return -ESTALE;
	/* Source f25338 changes the retained input mask when re-entering this
	 * stage. Preserve the live compiler's scratch/output bits underneath.
	 */
	mask = m9_cmd_get(fresh_secondary + 320);
	if ((mask & 0xff000000) != 0x0f000000)
		return -EINVAL;
	s = scratch->secondary;
	p = scratch->primary;
	memset(s, 0, sizeof(scratch->secondary));
	for (i = 0; i < ARRAY_SIZE(stage_spans); i++)
		memcpy(s + stage_spans[i].to,
		       fresh_secondary + stage_spans[i].from,
		       stage_spans[i].bytes);
	memcpy(s + 456, null_target, sizeof(null_target));
	m9_cmd_put(s + 552, 0x454545c0);
	m9_cmd_put(s + 556, 4);
	m9_cmd_put(s + 28, 0x49d00000 | ((800 - 28) / 4));
	m9_cmd_put(s + 844, 0x49d00000 | ((1328 - 844) / 4));
	m9_cmd_put(s + 1364, 0x49d00000 | ((1756 - 1364) / 4));
	m9_cmd_put(s + 320, ((mask & 0xff1fffffu) | 0x00100000u) & ~3u);
	if (!clear) {
		memmove(s + 60, s + 56, M9_FAA_DUAL_LATER_SECONDARY_CLEAR - 56);
		m9_cmd_put(s + 44, 3);
		m9_cmd_put(s + 48, 0x45e3e6e6);
		m9_cmd_put(s + 52, 1);
		m9_cmd_put(s + 56, 2);
		m9_cmd_put(s + 28, 0x49d00000 | ((804 - 28) / 4));
	}
	if (clear_flags)
		memcpy(p, scratch->clear.prefix, front);
	memcpy(p + front, fresh_tail, 3348);
	if (clear) {
		m9_cmd_put(p + front + 3348, 0x454545c0);
		m9_cmd_put(p + front + 3352, 4);
	}
	memcpy(p + front + 3348 + (clear ? 8 : 0), fresh_tail + 3348,
	       3624 - 3348);
	mid = clear ? 1768 : 1772;
	m9_cmd_put(p + front + 4, (owner[29].base + scursor) | 1);
	m9_cmd_put(p + front + 8, (owner[29].base + scursor + mid) | 2);
	m9_cmd_put(p + front + 52, (owner[29].base + scursor + mid) | 1);
	m9_cmd_put(p + front + 56, (owner[29].base + scursor + sec_bytes) | 2);
	if (clear_flags) {
		for (i = 0; i < clear_image.end_count; i++)
			out->end_bytes[i] = pcursor + clear_image.end_bytes[i];
	} else {
		i = 0;
	}
	out->end_bytes[i++] = pcursor + front + 60;
	out->end_bytes[i++] = pcursor + total - 12;
	out->end_bytes[i++] = pcursor + total - 12;
	out->end_bytes[i++] = pcursor + total;
	memcpy(out->primary, p, total);
	memcpy(out->secondary, s, sec_bytes);
	out->primary_bytes = total;
	out->secondary_bytes = sec_bytes;
	out->end_count = i;
	out->clear_bytes = front;
	out->draw_end_bytes = pcursor + front + 60;
	out->final_point = previous + (clear ? 0x150 : depth ? 0x120 : 0xd0);
	return 0;
}
