// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/errno.h>
#include <linux/string.h>
#include "m9_cmd.h"
#include "m9_faa_graph_later_depth_two_encode.h"

/* Original initialized count2 short constructors. The corresponding count4
 * (dual) producer keeps the HZ and sentinel attachment views (28 bytes
 * each), its own 2800040c policy words, the full depth constructor with its
 * C0 pair and the graph-sentinel sample slot. A depth-only (D3D clear flags
 * 2) count4 prefix stops after the depth constructor.
 */
#define PREFIX_FOUR 1u
#define PREFIX_DEPTH_ONLY 2u

/* Returns the END count (9, or 8 for a depth-only prefix) or an error. */
static int prefix(const u8 *sample, size_t sample_bytes,
		  const struct m9x_faa_graph_draw_owner *owner,
		  const struct m9x_faa_depth_views *v, u64 epoch,
		  struct m9x_faa_later_depth_scratch *s, u32 *bytes,
		  u32 ends[9], u32 form)
{
	struct m9x_faa_depth_stage_image hz;
	bool four = form & PREFIX_FOUR;
	u32 color, at, n, i, depth = M9_FAA_DEPTH_CLEAR_BYTES;
	int ret;

	ret = m9x_faa_depth_stages_encode(v, 0x3f800000, &hz);
	if (ret)
		return ret;
	ret = m9x_faa_color_attachment_clear_encode(v, (u32)epoch, 0x3f800000,
						    &s->attachment);
	if (ret)
		return ret;
	ret = m9x_faa_depth_clear_encode(v, (u32)epoch + 0x10, 0x3f800000,
					 &s->depth);
	if (ret)
		return ret;
	ret = m9x_faa_sample_clear_encode(sample, sample_bytes, owner, v,
					  epoch + 0x10, s->sample,
					  sizeof(s->sample), &n);
	if (ret)
		return ret;
	if (owner[8].bytes < 128 || n < 3180)
		return -ERANGE;
	/* Same qualified short depth/sample constructors as FIRST2. The sentinel
	 * is the retained graph owner, not the separately bound HZ allocation.
	 */
	if (!four) {
		s->depth.word[44] = 0x00080300;
		s->depth.word[127] = 0;
		memmove(s->depth.word + 146, s->depth.word + 148,
			M9_FAA_DEPTH_CLEAR_BYTES - 148 * 4);
		depth -= 8;
		m9_cmd_put(s->sample + 20, 0x2e00040c);
	}
	m9_cmd_put(s->sample + 44, 1);
	m9_cmd_put(s->sample + 48, 0x00010004);
	m9_cmd_put(s->sample + 76, owner[8].base | 1);
	m9_cmd_put(s->sample + 80, 0xf0000008);
	m9_cmd_put(s->sample + 3020, 0);
	if (four) {
		/* count4 keeps the graph-sentinel HZ view in sample slot0. */
		m9_cmd_put(s->sample + 3104, owner[8].base | 1);
		m9_cmd_put(s->sample + 3108, 0xf0000001);
		m9_cmd_put(s->sample + 3112, 0x00010001);
	} else {
		/* Original initialized count2 keeps sampled primary in target
		 * slot0; neither FIRST's disabled slot nor count4's view.
		 */
		m9_cmd_put(s->sample + 3104, owner[1].base | 1);
		m9_cmd_put(s->sample + 3108, 0xf0210000 | v->owner[0].width);
		m9_cmd_put(s->sample + 3112, 0x10000 | v->owner[0].height);
		m9_cmd_put(s->sample + 856 * 4, 0x2e00040c);
		memmove(s->sample + 793 * 4, s->sample + 795 * 4, n - 795 * 4);
		n -= 8;
	}
	color = s->attachment.end_bytes[0];
	memcpy(s->prefix, s->attachment.word, color);
	ends[0] = color;
	at = color;
	m9_cmd_put(s->prefix + at, 0x45e3e6e6);
	m9_cmd_put(s->prefix + at + 4, 1);
	m9_cmd_put(s->prefix + at + 8, 2);
	m9_cmd_put(s->prefix + at + 12, ((u32)epoch + 0x20) | 1);
	at += 16;
	if (four) {
		memcpy(s->prefix + at, hz.hz, 28);
		at += 28;
	}
	memcpy(s->prefix + at, hz.hz + 7, 52);
	if (!four)
		m9_cmd_put(s->prefix + at + 12, 0x2e00040c);
	at += 52;
	ends[2] = at;
	ends[1] = at;
	m9_cmd_put(s->prefix + at, M9_CMD_FENCE);
	m9_cmd_put(s->prefix + at + 4, 1);
	m9_cmd_put(s->prefix + at + 8, ((u32)epoch + 0x30) | 1);
	at += 12;
	if (four) {
		memcpy(s->prefix + at, hz.hz, 28);
		m9_cmd_put(s->prefix + at + 8, owner[8].base | 1);
		at += 28;
	}
	memcpy(s->prefix + at, hz.hz + 7, 52);
	m9_cmd_put(s->prefix + at + 4, owner[8].base | 1);
	m9_cmd_put(s->prefix + at + 8, 0xf0000008);
	m9_cmd_put(s->prefix + at + 12, four ? 0x2800040cu : 0x2e00040cu);
	m9_cmd_put(s->prefix + at + 16, 1);
	m9_cmd_put(s->prefix + at + 32, 0);
	m9_cmd_put(s->prefix + at + 48, 0x00010004);
	at += 52;
	ends[4] = at;
	ends[3] = at;
	{
		static const u32 state[7] = { 0x4545e6e6, 3, 3, 0x45a4a3a1,
					      0,	  0, 0 };
		memcpy(s->prefix + at, state, sizeof(state));
		at += sizeof(state);
	}
	ends[5] = at;
	memcpy(s->prefix + at, s->depth.word, depth);
	ends[6] = at + M9_FAA_DEPTH_CLEAR_SETUP_BYTES - (four ? 0 : 8);
	at += depth;
	ends[7] = at;
	if (form & PREFIX_DEPTH_ONLY) {
		if (at > sizeof(s->prefix))
			return -ERANGE;
		*bytes = at;
		return 8;
	}
	memcpy(s->prefix + at, s->sample, n);
	at += n;
	ends[8] = at;
	if (at > sizeof(s->prefix))
		return -ERANGE;
	for (i = 0; i < 9; i++)
		if (ends[i] > at)
			return -EINVAL;
	*bytes = at;
	return 9;
}

int m9x_faa_later_depth_two_clear_encode(const u8 *sample, size_t sample_bytes,
					 const struct m9x_faa_graph_draw_owner
						 owner[34],
					 const struct m9x_faa_depth_views *views,
					 u32 cursor, u64 origin,
					 struct m9x_faa_later_depth_clear_image *out,
					 struct m9x_faa_later_depth_scratch *scratch)
{
	u32 bytes, ends[9], i;
	int ret;

	if (!owner || !views || !out || !out->primary || !scratch ||
	    (cursor & 3) || (origin & 15) || origin > ~(u64)0 - 0x80 ||
	    (u32)origin > 0xffffff7fu)
		return -EINVAL;
	if (m9x_faa_graph_draw_owners_validate(owner))
		return -EINVAL;
	for (i = 0; i < 3; i++)
		if (owner[31 + i].base != views->owner[i].base ||
		    owner[31 + i].bytes < views->owner[i].capacity_bytes)
			return -EINVAL;
	ret = prefix(sample, sample_bytes, owner, views, origin, scratch,
		     &bytes, ends, 0);
	if (ret < 0)
		return ret;
	if (cursor > owner[27].bytes || bytes > owner[27].bytes - cursor ||
	    out->capacity < bytes)
		return -ERANGE;
	memmove(out->primary, scratch->prefix, bytes);
	for (i = 0; i < 9; i++)
		out->end_bytes[i] = cursor + ends[i];
	out->bytes = bytes;
	out->end_count = 9;
	out->final_point = origin + 0x80;
	return 0;
}

int m9x_faa_later_depth_two_encode(const u8 *sample, size_t sample_bytes,
				   const u8 *first, size_t first_bytes,
				   const struct m9x_faa_graph_draw_owner
					   owner[34],
				   const struct m9x_faa_depth_views *views,
				   u32 init_bytes, u32 pcursor, u32 scursor,
				   u64 previous, u64 next, u32 clear_flags,
				   const struct m9x_faa_color_input *input,
				   struct m9x_faa_later_depth_image *out,
				   struct m9x_faa_later_depth_scratch *scratch)
{
	u32 front = 0, clear_ends[9], sec = 0, extra, size, i, count = 0,
	    ends[13];
	int ret;
	bool clear = clear_flags == 3;
	u64 delta = clear ? 0x110 : 0x90;
	struct m9x_faa_color_image tmp;

	if (!owner || !views || !input || !out || !scratch ||
	    !out->image.program || !out->image.constants ||
	    !out->image.primary || !out->image.secondary ||
	    !out->image.secondary_length || (clear_flags && clear_flags != 3) ||
	    (previous & 15) || previous > ~(u64)0 - 0x110 ||
	    next != previous + 0x10 || (u32)previous > 0xfffffeefu ||
	    (pcursor & 3) || (scursor & 3) ||
	    input->layout.program_count != 2 ||
	    (input->declaration_count != 3 && input->declaration_count != 5))
		return -EINVAL;
	if (input->depth_flags &&
	    !m9x_faa_depth_flags_valid(input->depth_flags))
		return -EINVAL;
	if (m9x_faa_graph_draw_owners_validate(owner) ||
	    m9x_faa_depth_stages_encode(views, 0x3f800000,
					&(struct m9x_faa_depth_stage_image){
						0 }))
		return -EINVAL;
	for (i = 0; i < 3; i++)
		if (owner[31 + i].base != views->owner[i].base ||
		    owner[31 + i].bytes < views->owner[i].capacity_bytes)
			return -EINVAL;
	extra = input->declaration_count == 5 ? 24 : 0;
	if (scursor > owner[29].bytes ||
	    1352 + extra + (clear ? 0 : 4) > owner[29].bytes - scursor ||
	    (u64)owner[29].base + scursor + 1352 + extra + 4 > 0xffffffffu)
		return -ERANGE;
	if (clear) {
		ret = prefix(sample, sample_bytes, owner, views, previous,
			     scratch, &front, clear_ends, 0);
		if (ret < 0)
			return ret;
	}
	size = front + 3588;
	if (pcursor > owner[27].bytes || size > owner[27].bytes - pcursor ||
	    out->image.primary_bytes < size ||
	    out->image.secondary_bytes < 1352 + extra + (clear ? 0 : 4))
		return -ERANGE;
	tmp = (struct m9x_faa_color_image){ out->image.program,
					    out->image.constants,
					    scratch->tail,
					    scratch->first_secondary,
					    out->image.program_bytes,
					    out->image.constants_bytes,
					    sizeof(scratch->tail),
					    sizeof(scratch->first_secondary),
					    &sec };
	ret = m9x_faa_color_encode(first, first_bytes, owner, init_bytes,
				   previous + 0x70, input, &tmp);
	if (ret)
		return ret;
	if (sec != 1352 + extra)
		return -EINVAL;
	/* Compose only after every typed producer/extent refusal has passed. */
	memcpy(scratch->next_secondary, scratch->first_secondary, sec);
	m9_cmd_put(scratch->next_secondary + 660 + extra,
		   m9_cmd_get(scratch->next_secondary + 660 + extra) &
			   ~0x20000000u);
	m9_cmd_put(scratch->next_secondary + 816 + extra,
		   views->owner[2].base | 1);
	m9_cmd_put(scratch->next_secondary + 832 + extra, 0);
	m9_cmd_put(scratch->next_secondary + 836 + extra, 0xf0010100);
	m9_cmd_put(scratch->next_secondary + 940 + extra,
		   m9x_faa_depth_a8(input->depth_flags));
	if (!clear) {
		static const u32 sec_at[7] = { 12, 60, 592, 620, 632, 692, 788 };
		static const u32 prim_at[6] = {
			216, 272, 408, 3172, 3628, 3648
		};
		for (i = 0; i < 7; i++) {
			u32 at = sec_at[i] + (sec_at[i] >= 508 ? extra : 0);

			m9_cmd_put(scratch->next_secondary + at,
				   m9_cmd_get(scratch->next_secondary + at) -
					   0x80);
		}
		for (i = 0; i < 6; i++)
			m9_cmd_put(scratch->tail + prim_at[i],
				   m9_cmd_get(scratch->tail + prim_at[i]) -
					   0x80);
		memmove(scratch->next_secondary + 20,
			scratch->next_secondary + 16, sec - 16);
		m9_cmd_put(scratch->next_secondary, 3);
		m9_cmd_put(scratch->next_secondary + 4, 0x45e3e6e6);
		m9_cmd_put(scratch->next_secondary + 8, 1);
		m9_cmd_put(scratch->next_secondary + 12, 2);
		m9_cmd_put(scratch->next_secondary + 16,
			   ((u32)previous + 0x10) | 1);
		sec += 4;
	}
	m9_cmd_put(scratch->tail + 68, (owner[29].base + scursor) | 1);
	m9_cmd_put(scratch->tail + 72,
		   (owner[29].base + scursor + 580 + extra + (clear ? 0 : 4)) |
			   2);
	m9_cmd_put(scratch->tail + 80,
		   (owner[29].base + scursor + 580 + extra + (clear ? 0 : 4)) |
			   1);
	m9_cmd_put(scratch->tail + 84, (owner[29].base + scursor + sec) | 2);
	if (clear) {
		memcpy(out->image.primary, scratch->prefix, front);
		for (i = 0; i < 9; i++)
			ends[count++] = pcursor + clear_ends[i];
	}
	memcpy(out->image.primary + front, scratch->tail + 64, 3588);
	memcpy(out->image.secondary, scratch->next_secondary, sec);
	ends[count++] = pcursor + front + 24;
	ends[count++] = pcursor + size - 12;
	ends[count++] = pcursor + size - 12;
	ends[count++] = pcursor + size;
	out->primary_length = size;
	out->secondary_length = sec;
	out->end_count = count;
	out->clear_bytes = front;
	memcpy(out->end_bytes, ends, count * 4);
	out->draw_end_bytes = pcursor + front + 24;
	out->final_point = previous + delta;
	*out->image.secondary_length = sec;
	return 0;
}

/* Original dual (count4) later clear prefix: CLEAR3 (clear_flags 3, nine
 * ENDs) or depth-only (clear_flags 2, eight ENDs, no sampled colour clear).
 */
int m9x_faa_later_depth_four_clear_encode(const u8 *sample,
					  size_t sample_bytes,
					  const struct m9x_faa_graph_draw_owner
						  owner[34],
					  const struct m9x_faa_depth_views *views,
					  u32 cursor, u64 origin, u32 clear_flags,
					  struct m9x_faa_later_depth_clear_image *out,
					  struct m9x_faa_later_depth_scratch *scratch)
{
	u32 bytes, ends[9], i;
	int count;

	if (!owner || !views || !out || !out->primary || !scratch ||
	    (clear_flags != 3 && clear_flags != 2) || (cursor & 3) ||
	    (origin & 15) || origin > ~(u64)0 - 0x80 ||
	    (u32)origin > 0xffffff7fu)
		return -EINVAL;
	if (m9x_faa_graph_draw_owners_validate(owner))
		return -EINVAL;
	for (i = 0; i < 3; i++)
		if (owner[31 + i].base != views->owner[i].base ||
		    owner[31 + i].bytes < views->owner[i].capacity_bytes)
			return -EINVAL;
	count = prefix(sample, sample_bytes, owner, views, origin, scratch,
		       &bytes, ends,
		       PREFIX_FOUR | (clear_flags == 2 ? PREFIX_DEPTH_ONLY : 0));
	if (count < 0)
		return count;
	if (cursor > owner[27].bytes || bytes > owner[27].bytes - cursor ||
	    out->capacity < bytes)
		return -ERANGE;
	memmove(out->primary, scratch->prefix, bytes);
	for (i = 0; i < (u32)count; i++)
		out->end_bytes[i] = cursor + ends[i];
	out->end_count = count;
	out->bytes = bytes;
	out->final_point = origin + (clear_flags == 3 ? 0x80 : 0x50);
	return 0;
}
