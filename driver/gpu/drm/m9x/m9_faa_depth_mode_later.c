// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/string.h>
#include "m9_faa_depth_mode_later.h"

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

int m9x_faa_depth_mode_later_encode(const u8 *sample, size_t sample_bytes,
				    const u8 *first, size_t first_bytes,
				    const u8 *reset_blob, size_t reset_bytes,
				    const struct m9x_faa_graph_draw_owner *o,
				    const struct m9x_faa_depth_views *v,
				    u32 init_bytes, u32 pcursor, u32 scursor,
				    u64 origin, u64 next,
				    const struct m9x_faa_depth_mode_state
					    *before,
				    const struct m9x_faa_color_input *input,
				    struct m9x_faa_later_depth_image *out,
				    struct m9x_faa_depth_mode_later_scratch
					    *scratch,
				    struct m9x_faa_depth_mode_state *after)
{
	struct m9x_faa_depth_mode_image mode;
	struct m9x_faa_depth_mode_reset_image reset;
	struct m9x_faa_color_image tmp;
	struct m9x_faa_color_input enabled_input;
	u32 i, extra, sec, front, bytes;
	int ret;

	if (!o || !v || !before || !before->valid || !input || !out ||
	    !scratch || !after || !out->image.program ||
	    !out->image.constants || !out->image.primary ||
	    !out->image.secondary || !out->image.secondary_length ||
	    input->layout.program_count != 2 ||
	    (input->declaration_count != 3 && input->declaration_count != 5) ||
	    (pcursor & 3) || (scursor & 3) || (origin & 15) ||
	    origin > ~(u64)0 - 0xe0 || (u32)origin > 0xffffff1fu ||
	    next != origin + 0x10)
		return -EINVAL;
	ret = m9x_faa_depth_mode_two_plan(input->depth_flags ?
						  input->depth_flags :
						  15,
					  0, before, o, v, &mode);
	if (ret)
		return ret;
	if (m9x_faa_graph_draw_owners_validate(o) ||
	    m9x_faa_depth_stages_encode(v, 0x3f800000,
					&(struct m9x_faa_depth_stage_image){
						0 }))
		return -EINVAL;
	for (i = 0; i < 3; i++)
		if (o[31 + i].base != v->owner[i].base ||
		    o[31 + i].bytes < v->owner[i].capacity_bytes)
			return -EINVAL;
	extra = input->declaration_count == 5 ? 24 : 0;
	front = mode.reset_delta ? 4156 + (before->embedded_hz ? 16 : 0) : 0;
	bytes = front + 3588;
	sec = 1352 + extra + (front ? 0 : 4);
	if (pcursor > o[27].bytes || bytes > o[27].bytes - pcursor ||
	    scursor > o[29].bytes || sec > o[29].bytes - scursor ||
	    out->image.primary_bytes < bytes ||
	    out->image.secondary_bytes < sec)
		return -ERANGE;
	if (!front) {
		/* The existing no-clear counted constructor is shared. Generate its
		 * enable-compatible scratch without changing caller semantics, then
		 * apply the authenticated current association route below.
		 */
		enabled_input = *input;
		enabled_input.depth_flags =
			input->depth_flags ?
				((input->depth_flags & ~32u) | 1u) :
				15;
		ret = m9x_faa_later_depth_two_encode(sample, sample_bytes,
						     first, first_bytes, o, v,
						     init_bytes, pcursor,
						     scursor, origin, next, 0,
						     &enabled_input, out,
						     &scratch->later);
		if (ret)
			return ret;
	} else {
		reset = (struct m9x_faa_depth_mode_reset_image){
			scratch->reset, sizeof(scratch->reset), 0, { 0 }, 0
		};
		ret = m9x_faa_depth_mode_reset_encode(reset_blob, reset_bytes,
						      o, v, before, true,
						      origin + 0x10, &reset);
		if (ret)
			return ret;
		tmp = (struct m9x_faa_color_image){
			out->image.program,
			out->image.constants,
			scratch->later.tail,
			scratch->later.first_secondary,
			out->image.program_bytes,
			out->image.constants_bytes,
			sizeof(scratch->later.tail),
			sizeof(scratch->later.first_secondary),
			&sec
		};
		ret = m9x_faa_color_encode(first, first_bytes, o, init_bytes,
					   origin + 0x40, input, &tmp);
		if (ret)
			return ret;
		if (sec != 1352 + extra)
			return -EINVAL;
		put(scratch->later.tail + 68, (o[29].base + scursor) | 1);
		put(scratch->later.tail + 72,
		    (o[29].base + scursor + 580 + extra) | 2);
		put(scratch->later.tail + 80,
		    (o[29].base + scursor + 580 + extra) | 1);
		put(scratch->later.tail + 84, (o[29].base + scursor + sec) | 2);
		memcpy(out->image.primary, reset.primary, front);
		memcpy(out->image.primary + front, scratch->later.tail + 64,
		       3588);
		memcpy(out->image.secondary, scratch->later.first_secondary,
		       sec);
		for (i = 0; i < 3; i++)
			out->end_bytes[i] = pcursor + reset.end_bytes[i];
		out->end_bytes[3] = pcursor + front + 24;
		out->end_bytes[5] = pcursor + bytes - 12;
		out->end_bytes[4] = pcursor + bytes - 12;
		out->end_bytes[6] = pcursor + bytes;
		out->end_count = 7;
		out->primary_length = bytes;
		out->secondary_length = sec;
		out->clear_bytes = front;
		out->draw_end_bytes = pcursor + front + 24;
		out->final_point = origin + 0xe0;
		*out->image.secondary_length = sec;
	}
	extra += (front ? 0 : 4);
	/* Preserve shader-dependent EF fields. Only source-derived depth routes
	 * are replaced; fragment blend/alpha allocator state remains intact.
	 */
	mode.ef = (word(out->image.secondary + 660 + extra) & ~0x20000000u) |
		  (mode.after.enabled ? 0 : 0x20000000u);
	put(out->image.secondary + 660 + extra, mode.ef);
	put(out->image.secondary + 816 + extra, mode.a9);
	put(out->image.secondary + 832 + extra, mode.ba);
	put(out->image.secondary + 836 + extra, mode.ba_descriptor);
	put(out->image.secondary + 940 + extra, mode.a8);
	*after = mode.after;
	return 0;
}
