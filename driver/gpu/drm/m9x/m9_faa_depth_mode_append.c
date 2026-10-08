// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/string.h>
#include <linux/overflow.h>
#include "m9_faa_depth_mode_append.h"
#include "m9_faa_graph_depth_encode.h"

static u32 word(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static void put(u8 *p, u32 v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

int m9x_faa_depth_mode_append_encode(const struct m9x_faa_depth_mode_append_input
					     *in,
				     struct m9x_faa_depth_mode_append_image *out)
{
	static const u32 sec_at[7] = { 12, 60, 592, 620, 632, 692, 788 };
	static const u32 sec_delta[7] = { 0x90, 0xa0, 0xb0, 0xc0,
					  0xc0, 0xd0, 0xe0 };
	static const u32 sec_tag[7] = { 1, 1, 1, 1, 0, 1, 1 };
	static const u32 tail_at[6] = { 128, 184, 320, 3084, 3540, 3560 };
	static const u32 tail_back[6] = { 0x20, 0x10, 0x10, 0x10, 0x20, 0 };
	static const u32 tail_tag[6] = { 1, 1, 0, 0, 0, 1 };
	struct m9x_faa_depth_mode_image route;
	struct m9x_faa_depth_mode_reset_image reset;
	static const u32 indexed_save[36] = {
		1,	    0x45454524, 0,	    3,		0x453ce6e6,
		0x6dc00000, 0x42000000, 0xa1,	    9,		0x44444444,
		0x30081898, 0x30081898, 0x30081898, 0x30081898, 0x44444444,
		0x30081898, 0x30081898, 0x30081898, 0x30081898, 2,
		0x45452f2e, 0,		0,	    2,		0x4545421a,
		0,	    0xa1000009, 2,	    0x4545421a, 0,
		0xa3000009, 3,		0x45e6421a, 0,		0xa4000009,
		0x40008000
	};
	u8 save[116];
	u32 extra, last_extra, n, split, insert, newpb, newpoint_low, i, at,
		first, last, newsec, addends;
	bool changed;
	u64 point, fresh_epoch;
	int ret;

	if (!in || !out || !in->owner || !in->views || !in->old_primary ||
	    !in->fresh_secondary || !in->old_ends || !out->primary ||
	    !out->secondary || !out->reset_scratch || !out->ends ||
	    !in->old_draw_count ||
	    in->old_draw_count >= M9_FAA_FRAME_BATCH_MAX_DRAWS ||
	    (in->declaration_count != 3 && in->declaration_count != 5) ||
	    (in->old_last_declaration_count != 3 &&
	     in->old_last_declaration_count != 5) ||
	    !in->before.valid || in->old_final_point < 0xd0 ||
	    (in->old_final_point & 15) ||
	    in->old_final_point > ~(u64)0 - 0xb0 ||
	    (u32)in->old_final_point > 0xffffff4fu ||
	    (in->initial_end_count != 14 && in->initial_end_count != 13 &&
	     in->initial_end_count != 4 && in->initial_end_count != 7) ||
	    in->old_end_count < in->initial_end_count ||
	    in->old_end_count > M9_FAA_DEPTH_MODE_MAX_ENDS ||
	    (in->old_end_count - in->initial_end_count) % 3)
		return -EINVAL;
	extra = in->declaration_count == 5 ? 24 : 0;
	last_extra = in->old_last_declaration_count == 5 ? 24 : 0;
	if (in->fresh_secondary_bytes != 1352 + extra)
		return -EINVAL;
	if (m9x_faa_depth_stages_encode(in->views, 0x3f800000,
					&(struct m9x_faa_depth_stage_image){
						0 }))
		return -EINVAL;
	ret = m9x_faa_depth_mode_two_plan(in->depth_flags,
					  word(in->fresh_secondary + 660 +
					       extra),
					  &in->before, in->owner, in->views,
					  &route);
	if (ret)
		return ret;
	changed = !!route.reset_delta;
	addends = changed ? 3 : 0;
	n = changed ? 4156 + (in->before.embedded_hz ? 16 : 0) : 0;
	insert = changed ? 116 + n + 24 : 24;
	newsec = (changed ? 1352 : 1484) + extra;
	if (in->old_end_count > M9_FAA_DEPTH_MODE_MAX_ENDS - addends ||
	    out->end_capacity < in->old_end_count + addends)
		return -ERANGE;
	if (in->fresh_secondary_bytes != 1352 + extra ||
	    in->old_primary_bytes < 3564 + 24 ||
	    (in->old_last_secondary_bytes != 1352 + last_extra &&
	     in->old_last_secondary_bytes != 1484 + last_extra &&
	     !(in->old_draw_count == 1 && in->initial_end_count == 4 &&
	       in->old_last_secondary_bytes == 1356 + last_extra)) ||
	    in->old_secondary_bytes < in->old_last_secondary_bytes ||
	    check_add_overflow(in->old_primary_bytes, insert, &newpb) ||
	    out->primary_capacity < newpb || out->secondary_capacity < newsec ||
	    out->reset_capacity < n ||
	    in->primary_start > in->owner[27].bytes ||
	    newpb > in->owner[27].bytes - in->primary_start ||
	    in->secondary_start > in->owner[29].bytes ||
	    in->old_secondary_bytes >
		    in->owner[29].bytes - in->secondary_start ||
	    newsec > in->owner[29].bytes - in->secondary_start -
			     in->old_secondary_bytes)
		return -ERANGE;
	if (m9x_faa_graph_draw_owners_validate(in->owner))
		return -EINVAL;
	split = in->old_primary_bytes - 3564;
	point = in->old_final_point + (changed ? 0xb0 : 0x60);
	newpoint_low = (u32)point;
	fresh_epoch = in->old_final_point - (changed ? 0x60 : 0xb0);
	for (i = 0; i < in->old_end_count; i++)
		if ((in->old_ends[i] & 3) ||
		    in->old_ends[i] < in->primary_start ||
		    in->old_ends[i] >
			    in->primary_start + in->old_primary_bytes ||
		    (i && in->old_ends[i] < in->old_ends[i - 1]))
			return -EINVAL;
	if (in->old_ends[in->old_end_count - 4] != in->primary_start + split ||
	    in->old_ends[in->old_end_count - 3] !=
		    in->primary_start + in->old_primary_bytes - 12 ||
	    in->old_ends[in->old_end_count - 2] !=
		    in->old_ends[in->old_end_count - 3] ||
	    in->old_ends[in->old_end_count - 1] !=
		    in->primary_start + in->old_primary_bytes)
		return -ESTALE;
	first = in->secondary_start + in->old_secondary_bytes -
		in->old_last_secondary_bytes;
	last = in->secondary_start + in->old_secondary_bytes - 772;
	{
		u32 expected[6] = { 0x45452d2c,
				    (in->owner[29].base + first) | 1,
				    (in->owner[29].base + last) | 2,
				    0x45452d2c,
				    (in->owner[29].base + last) | 1,
				    (in->owner[29].base + in->secondary_start +
				     in->old_secondary_bytes) |
					    2 };
		for (i = 0; i < 6; i++)
			if (word(in->old_primary + split - 24 + 4 * i) !=
			    expected[i])
				return -ESTALE;
	}
	if (word(in->fresh_secondary) != 2 ||
	    word(in->fresh_secondary + 4) != 0x4545e3e6 ||
	    word(in->fresh_secondary + 8) != 1 ||
	    word(in->fresh_secondary + 48) != 2 ||
	    word(in->fresh_secondary + 52) != 0x4545e3e6 ||
	    word(in->fresh_secondary + 56) != 1)
		return -EINVAL;
	for (i = 0; i < 7; i++) {
		at = sec_at[i] + (sec_at[i] >= 508 ? extra : 0);
		if (word(in->fresh_secondary + at) !=
		    (((u32)fresh_epoch + sec_delta[i]) | sec_tag[i]))
			return -ESTALE;
	}
	for (i = 0; i < 6; i++)
		if (word(in->old_primary + split + tail_at[i]) !=
		    (((u32)in->old_final_point - tail_back[i]) | tail_tag[i]))
			return -ESTALE;
	ret = m9x_faa_depth_mode_save_encode(in->owner, save, sizeof(save));
	if (ret)
		return ret;
	if (changed) {
		reset = (struct m9x_faa_depth_mode_reset_image){
			out->reset_scratch, out->reset_capacity, 0, { 0 }, 0
		};
		ret = m9x_faa_depth_mode_reset_encode(in->reset_template,
						      in->reset_template_bytes,
						      in->owner, in->views,
						      &in->before, false,
						      in->old_final_point -
							      0x20,
						      &reset);
		if (ret)
			return ret;
	} else {
		memset(&reset, 0, sizeof(reset));
	}
	/* All refusal paths precede mutation of assembled primary/secondary/ends.
	 * Caller scratch is separate from those retained images.
	 */
	memmove(out->primary, in->old_primary, split);
	memmove(out->primary + split + insert, in->old_primary + split, 3564);
	if (changed) {
		memcpy(out->primary + split, save, sizeof(save));
		memcpy(out->primary + split + 116, reset.primary, reset.bytes);
	}
	at = split + (changed ? 116 + reset.bytes : 0);
	put(out->primary + at, 0x45452d2c);
	put(out->primary + at + 4, (in->owner[29].base + in->secondary_start +
				    in->old_secondary_bytes) |
					   1);
	put(out->primary + at + 8, (in->owner[29].base + in->secondary_start +
				    in->old_secondary_bytes + newsec - 772) |
					   2);
	put(out->primary + at + 12, 0x45452d2c);
	put(out->primary + at + 16, (in->owner[29].base + in->secondary_start +
				     in->old_secondary_bytes + newsec - 772) |
					    1);
	put(out->primary + at + 20, (in->owner[29].base + in->secondary_start +
				     in->old_secondary_bytes + newsec) |
					    2);
	for (i = 0; i < 6; i++)
		put(out->primary + split + insert + tail_at[i],
		    (newpoint_low - tail_back[i]) | tail_tag[i]);
	if (changed) {
		memcpy(out->secondary, in->fresh_secondary, 1352 + extra);
	} else {
		const u8 *s = in->fresh_secondary;

		put(out->secondary, 3);
		put(out->secondary + 4, 0x45e3e6e6);
		put(out->secondary + 8, 1);
		put(out->secondary + 12, 2);
		put(out->secondary + 16, word(s + 12));
		memcpy(out->secondary + 20, s + 16, 32);
		memcpy(out->secondary + 52, indexed_save, sizeof(indexed_save));
		put(out->secondary + 60, in->owner[22].base | 1);
		put(out->secondary + 136,
		    ((in->owner[22].base + 644) & 0xffffff84u) | 5);
		put(out->secondary + 140,
		    ((in->owner[22].base + 708) & 0xfffffffdu) | 1);
		put(out->secondary + 152, (in->owner[8].base + 8) | 1);
		put(out->secondary + 168, (in->owner[8].base + 12) | 1);
		put(out->secondary + 184, (in->owner[8].base + 16) | 1);
		memcpy(out->secondary + 196, s + 48, 504 + extra);
		memcpy(out->secondary + 700 + extra, s + 568 + extra,
		       1352 + extra - 568 - extra);
	}
	extra += changed ? 0 : 132;
	put(out->secondary + 660 + extra, route.ef);
	put(out->secondary + 816 + extra, route.a9);
	put(out->secondary + 832 + extra, route.ba);
	put(out->secondary + 836 + extra, route.ba_descriptor);
	put(out->secondary + 940 + extra, route.a8);
	memmove(out->ends, in->old_ends, (in->old_end_count - 4) * 4);
	if (changed)
		for (i = 0; i < 3; i++)
			out->ends[in->old_end_count - 4 + i] =
				in->primary_start + split + 116 +
				reset.end_bytes[i];
	for (i = 0; i < 4; i++)
		out->ends[in->old_end_count - 4 + addends + i] =
			in->old_ends[in->old_end_count - 4 + i] + insert;
	out->primary_bytes = newpb;
	out->secondary_bytes = newsec;
	out->end_count = in->old_end_count + addends;
	out->final_point = point;
	out->after = route.after;
	return 0;
}
