// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include "m9_cmd.h"
#include "m9_faa_graph_batch_limits.h"
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/overflow.h>
#include "m9_faa_graph_first_depth_two_append.h"

static u32 moved(u32 at, u32 extra)
{
	return at >= 508 ? at + extra : at;
}

int m9x_faa_first_depth_two_append(const struct m9x_faa_first_depth_two_append_input *in,
				   const struct m9x_faa_first_depth_two_append_output *out)
{
	static const u32 insert[36] = {
		1,	    0x45454524, 0,	    3,		0x453ce6e6,
		0x6dc00000, 0x42000000, 0xa1,	    9,		0x44444444,
		0x30081898, 0x30081898, 0x30081898, 0x30081898, 0x44444444,
		0x30081898, 0x30081898, 0x30081898, 0x30081898, 2,
		0x45452f2e, 0,		0,	    2,		0x4545421a,
		0,	    0xa1000009, 2,	    0x4545421a, 0,
		0xa3000009, 3,		0x45e6421a, 0,		0xa4000009,
		0x40008000
	};
	static const u32 sec_at[7] = { 12, 60, 592, 620, 632, 692, 788 };
	static const u32 sec_delta[7] = { 0x90, 0xa0, 0xb0, 0xc0,
					  0xc0, 0xd0, 0xe0 };
	static const u32 sec_tag[7] = { 1, 1, 1, 1, 0, 1, 1 };
	static const u32 tail_at[6] = { 128, 184, 320, 3084, 3540, 3560 };
	static const u32 tail_back[6] = { 0x20, 0x10, 0x10, 0x10, 0x20, 0 };
	static const u32 tail_tag[6] = { 1, 1, 0, 0, 0, 1 };
	u32 extra, old_extra, newsec, newprimary, split, minsec, maxsec, first,
		last, delta, at, i, e[14], pages, groups_min, groups_max, link;
	u64 oldpoint, point;
	const u8 *s;

	if (!in || !out || !in->owner || !in->old_primary ||
	    !in->fresh_secondary || !in->old_ends || !out->primary ||
	    !out->secondary || !out->primary_bytes || !out->secondary_bytes ||
	    !out->ends || !out->final_point || !in->old_draw_count ||
	    in->old_draw_count >= M9_FAA_FRAME_BATCH_MAX_DRAWS ||
	    (in->declaration_count != 3 && in->declaration_count != 5) ||
	    (in->old_last_declaration_count != 3 &&
	     in->old_last_declaration_count != 5) ||
	    (in->init_point & 15) ||
	    in->init_point >
		    ~(u64)0 - 0x110 - M9_FAA_FRAME_BATCH_MAX_DRAWS * 0x60 ||
	    (u32)in->init_point >
		    U32_MAX - 0x110 - M9_FAA_FRAME_BATCH_MAX_DRAWS * 0x60)
		return -EINVAL;
	if (m9x_faa_graph_draw_owners_validate(in->owner))
		return -EINVAL;
	extra = in->declaration_count == 5 ? 24 : 0;
	old_extra = in->old_last_declaration_count == 5 ? 24 : 0;
	newsec = 1484 + extra;
	s = in->fresh_secondary;
	minsec = 1352 + (in->old_draw_count - 1) * 1484;
	maxsec = minsec + 24 * in->old_draw_count;
	if (in->fresh_secondary_bytes != 1352 + extra ||
	    in->old_secondary_bytes < minsec ||
	    in->old_secondary_bytes > maxsec ||
	    (in->old_secondary_bytes - minsec) % 24 ||
	    in->old_secondary_bytes <
		    (in->old_draw_count == 1 ? 1352 : 1484) + old_extra ||
	    check_add_overflow(in->old_primary_bytes, 24u, &newprimary) ||
	    in->old_primary_bytes < 3564 ||
	    out->primary_capacity < newprimary ||
	    out->secondary_capacity < newsec ||
	    in->init_bytes > in->owner[27].bytes ||
	    newprimary > in->owner[27].bytes - in->init_bytes ||
	    in->old_secondary_bytes > in->owner[29].bytes ||
	    newsec > in->owner[29].bytes - in->old_secondary_bytes ||
	    in->owner[22].bytes < 712 || in->owner[8].bytes < 20)
		return -ERANGE;
	oldpoint = in->init_point + 0x110 + (in->old_draw_count - 1) * 0x60;
	point = oldpoint + 0x60;
	split = in->old_primary_bytes - 3564;
	for (i = 0; i < 14; i++) {
		e[i] = in->old_ends[i];
		if (e[i] < in->init_bytes ||
		    e[i] > in->init_bytes + in->old_primary_bytes ||
		    (e[i] & 3) || (i && e[i] < e[i - 1]))
			return -EINVAL;
	}
	/* INIT authenticates the admitted source shape; each full logical page
	 * interval has one word per eight-page group, including both edge masks.
	 */
	if (in->init_bytes != 4880 && in->init_bytes != 4884 &&
	    in->init_bytes != 5576)
		return -EINVAL;
	pages = in->init_bytes == 5576 ? 769 : 80;
	groups_min = (pages + 7) / 8;
	groups_max = (pages + 14) / 8;
	if (e[0] != in->init_bytes + 64 || e[1] != e[0] ||
	    (e[2] - e[1] < 56 + groups_min * 4 ||
	     e[2] - e[1] > 56 + groups_max * 4) ||
	    e[3] - e[2] != 52 || e[4] != e[3] || e[5] - e[4] != 824 ||
	    e[6] - e[5] != 40 ||
	    (e[7] - e[6] < 3468 + groups_min * 4 ||
	     e[7] - e[6] > 3468 + groups_max * 4) ||
	    e[8] - e[7] != 64 || e[9] != e[8] ||
	    e[9] - in->init_bytes > M9_FAA_FIRST_DEPTH_PREFIX_CAPACITY)
		return -EINVAL;
	if (e[9] - in->init_bytes + 24 * in->old_draw_count != split ||
	    e[10] != in->init_bytes + split ||
	    e[11] != in->init_bytes + in->old_primary_bytes - 12 ||
	    e[12] != e[11] || e[13] != in->init_bytes + in->old_primary_bytes)
		return -EINVAL;
	first = in->old_secondary_bytes -
		(in->old_draw_count == 1 ? 1352 : 1484) - old_extra;
	last = in->old_secondary_bytes - 772;
	{
		u32 expected[6] = {
			M9_CMD_LINK,
			(in->owner[29].base + first) | 1,
			(in->owner[29].base + last) | 2,
			M9_CMD_LINK,
			(in->owner[29].base + last) | 1,
			(in->owner[29].base + in->old_secondary_bytes) | 2
		};
		for (i = 0; i < 6; i++)
			if (m9_cmd_get(in->old_primary + split - 24 + 4 * i) !=
			    expected[i])
				return -ESTALE;
	}
	if (m9_cmd_get(s) != 2 || m9_cmd_get(s + 4) != M9_CMD_FENCE ||
	    m9_cmd_get(s + 8) != 1 || m9_cmd_get(s + 48) != 2 ||
	    m9_cmd_get(s + 52) != M9_CMD_FENCE || m9_cmd_get(s + 56) != 1 ||
	    m9_cmd_get(s + moved(552, extra)) != 2 ||
	    m9_cmd_get(s + moved(556, extra)) != 0x45451819 ||
	    m9_cmd_get(s + moved(560, extra)) ||
	    m9_cmd_get(s + moved(564, extra)) != 0x7fffffff ||
	    !m9x_faa_depth_a8_valid(m9_cmd_get(s + 940 + extra)))
		return -EINVAL;
	for (i = 0; i < 7; i++)
		if (m9_cmd_get(s + moved(sec_at[i], extra)) !=
		    (((u32)in->init_point + sec_delta[i]) | sec_tag[i]))
			return -ESTALE;
	for (i = 0; i < 6; i++)
		if (m9_cmd_get(in->old_primary + split + tail_at[i]) !=
		    (((u32)oldpoint - tail_back[i]) | tail_tag[i]))
			return -ESTALE;
	/* Every refusal above precedes output writes. Existing secondary/code/data
	 * remain immutable. The new source prologue preserves pool-save and graph
	 * sentinel state; these control literals are not GPU addresses.
	 */
	memmove(out->primary, in->old_primary, split);
	memmove(out->primary + split + 24, in->old_primary + split, 3564);
	link = in->owner[29].base + in->old_secondary_bytes;
	m9_cmd_put_link(out->primary + split, link | 1,
			(link + 712 + extra) | 2);
	m9_cmd_put_link(out->primary + split + 12, (link + 712 + extra) | 1,
			(link + newsec) | 2);
	for (i = 0; i < 6; i++)
		m9_cmd_put(out->primary + split + 24 + tail_at[i],
			   ((u32)point - tail_back[i]) | tail_tag[i]);
	m9_cmd_put(out->secondary, 3);
	m9_cmd_put(out->secondary + 4, 0x45e3e6e6);
	m9_cmd_put(out->secondary + 8, 1);
	m9_cmd_put(out->secondary + 12, 2);
	m9_cmd_put(out->secondary + 16, m9_cmd_get(s + 12));
	memcpy(out->secondary + 20, s + 16, 32);
	memcpy(out->secondary + 52, insert, sizeof(insert));
	m9_cmd_put(out->secondary + 60, in->owner[22].base | 1);
	m9_cmd_put(out->secondary + 136,
		   ((in->owner[22].base + 644) & 0xffffff84u) | 5);
	m9_cmd_put(out->secondary + 140,
		   ((in->owner[22].base + 708) & 0xfffffffdu) | 1);
	m9_cmd_put(out->secondary + 152, (in->owner[8].base + 8) | 1);
	m9_cmd_put(out->secondary + 168, (in->owner[8].base + 12) | 1);
	m9_cmd_put(out->secondary + 184, (in->owner[8].base + 16) | 1);
	memcpy(out->secondary + 196, s + 48, 504 + extra);
	memcpy(out->secondary + 700 + extra, s + 568 + extra,
	       in->fresh_secondary_bytes - 568 - extra);
	delta = in->old_draw_count * 0x60;
	for (i = 0; i < 7; i++) {
		at = moved(sec_at[i], extra);
		at = at == 12	      ? 16 :
		     at < 48	      ? at + 4 :
		     at < 552 + extra ? at + 148 :
					at + 132;
		m9_cmd_put(out->secondary + at,
			   ((u32)in->init_point + sec_delta[i] + delta) |
				   sec_tag[i]);
	}
	for (i = 10; i < 14; i++)
		e[i] += 24;
	memcpy(out->ends, e, sizeof(e));
	*out->primary_bytes = newprimary;
	*out->secondary_bytes = newsec;
	*out->final_point = point;
	return 0;
}
