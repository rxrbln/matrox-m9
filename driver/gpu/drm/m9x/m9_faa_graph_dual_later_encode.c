// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* Original M9128 retained dual draw, constructed from live owned stages. */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include "m9_cmd.h"
#include "m9_faa_graph_dual_later_encode.h"
#include "m9_faa_graph_later_stage.h"

static int later_encode(const u8 *sample, size_t sample_bytes,
			const struct m9x_faa_graph_draw_owner owner[34],
			const struct m9x_faa_depth_views *views, u32 pcursor,
			u32 scursor, u64 previous, u64 next, u32 clear_flags,
			const u8 *fresh_secondary, u32 fresh_bytes,
			const u8 fresh_tail[M9_FAA_DUAL_FIRST_PRIMARY_TAIL_BYTES],
			enum m9x_faa_later_form form, bool cull_state,
			struct m9x_faa_dual_later_image *out,
			struct m9x_faa_dual_later_scratch *scratch)
{
	struct m9x_faa_later_depth_clear_image clear_image;
	struct m9x_faa_later_stage stage;
	u32 front = 0, sec_bytes, tail_bytes, total, mid, epoch, i;
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
	/* The first DRAW of the frame: the program's FIRST stage less the
	 * groups its cache form drops, epoch-checked below.
	 */
	epoch = (u32)previous - (clear ? 0 : depth ? 0x30 : 0x80);
	s = scratch->secondary;
	p = scratch->primary;
	ret = m9x_faa_later_stage_encode(fresh_secondary, fresh_bytes, epoch,
					 form, false, !clear, cull_state, s,
					 sizeof(scratch->secondary), &stage);
	if (ret)
		return ret;
	sec_bytes = stage.bytes;
	mid = stage.split;
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
	    m9_cmd_overlap(out->primary, total, fresh_secondary, fresh_bytes) ||
	    m9_cmd_overlap(out->primary, total, fresh_tail, 3624) ||
	    m9_cmd_overlap(out->secondary, sec_bytes, fresh_secondary,
			   fresh_bytes) ||
	    m9_cmd_overlap(out->secondary, sec_bytes, fresh_tail, 3624))
		return -EINVAL;
	if (m9_cmd_get(fresh_secondary + 56) != ((epoch + 0x90) | 1) ||
	    m9_cmd_get(fresh_tail) != M9_CMD_LINK ||
	    m9_cmd_get(fresh_tail + 48) != M9_CMD_LINK ||
	    m9_cmd_get(fresh_tail + 60) != 0x45454524 ||
	    m9_cmd_get(fresh_tail + 188) != ((epoch + 0x130) | 1) ||
	    m9_cmd_get(fresh_tail + 3620) != ((epoch + 0x150) | 1))
		return -ESTALE;
	if (clear_flags)
		memcpy(p, scratch->clear.prefix, front);
	memcpy(p + front, fresh_tail, 3348);
	if (clear) {
		m9_cmd_put(p + front + 3348, 0x454545c0);
		m9_cmd_put(p + front + 3352, 4);
	}
	memcpy(p + front + 3348 + (clear ? 8 : 0), fresh_tail + 3348,
	       3624 - 3348);
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
	out->units = stage.units;
	out->site[0] = stage.site[0];
	out->site[1] = stage.site[1];
	out->final_point = previous + (clear ? 0x150 : depth ? 0x120 : 0xd0);
	return 0;
}

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
	/* Single-program frames: a culled frame re-emits the 6A groups. */
	if (!fresh_secondary)
		return -EINVAL;
	return later_encode(sample, sample_bytes, owner, views, pcursor,
			    scursor, previous, next, clear_flags,
			    fresh_secondary, M9_FAA_DUAL_FIRST_SECONDARY_BYTES,
			    fresh_tail, M9_FAA_LATER_PLAIN,
			    m9_cmd_get(fresh_secondary + 1992) >> 24 & 3, out,
			    scratch);
}

int m9x_faa_later_encode(const u8 *sample, size_t sample_bytes,
			 const struct m9x_faa_graph_draw_owner owner[34],
			 const struct m9x_faa_depth_views *views, u32 pcursor,
			 u32 scursor, u64 previous, u64 next, u32 clear_flags,
			 const u8 *fresh_first, u32 fresh_first_bytes,
			 const u8 fresh_tail[M9_FAA_DUAL_FIRST_PRIMARY_TAIL_BYTES],
			 enum m9x_faa_later_form form, bool cull_state,
			 struct m9x_faa_dual_later_image *out,
			 struct m9x_faa_dual_later_scratch *scratch)
{
	return later_encode(sample, sample_bytes, owner, views, pcursor,
			    scursor, previous, next, clear_flags, fresh_first,
			    fresh_first_bytes, fresh_tail, form, cull_state, out,
			    scratch);
}
