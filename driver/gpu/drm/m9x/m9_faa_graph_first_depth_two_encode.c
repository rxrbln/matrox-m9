// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/errno.h>
#include <linux/string.h>
#include "m9_cmd.h"
#include "m9_faa_graph_first_depth_two_encode.h"
#include "m9_faa_depth_mode.h"

int m9x_faa_first_depth_two_encode(const u8 *sample, size_t sample_bytes,
				   const u8 *first, size_t first_bytes,
				   const struct m9x_faa_graph_draw_owner *owner,
				   const struct m9x_faa_depth_views *views,
				   u32 init_bytes, u64 init_point,
				   const struct m9x_faa_color_input *input,
				   u8 *primary, size_t primary_capacity,
				   u8 *secondary, size_t secondary_capacity,
				   u32 *primary_bytes, u32 *secondary_bytes,
				   u32 *ends,
				   const struct m9x_faa_color_image *first_scratch,
				   struct m9x_faa_first_depth_scratch *prefix_scratch)
{
	u32 length, sec, extra, i, expected;
	int ret;
	struct m9x_faa_depth_mode_image mode;
	const struct m9x_faa_depth_mode_state before = { 0 };

	if (!owner || !views || !input || !primary || !secondary ||
	    !primary_bytes || !secondary_bytes || !ends || !first_scratch ||
	    !first_scratch->secondary_length || !prefix_scratch ||
	    input->layout.program_count != 2 ||
	    (input->declaration_count != 3 && input->declaration_count != 5) ||
	    init_point > ~(u64)0 - 0x110 || (u32)init_point > 0xfffffeefu ||
	    init_bytes > owner[27].bytes ||
	    owner[27].bytes - init_bytes <
		    m9x_faa_first_depth_prefix_bound(views) + 3588 ||
	    primary_capacity < m9x_faa_first_depth_prefix_bound(views) + 3588 ||
	    secondary_capacity <
		    1352 + (input->declaration_count == 5 ? 24 : 0))
		return -EINVAL;
	if (input->depth_flags &&
	    !m9x_faa_depth_mode_flags_valid(input->depth_flags))
		return -EINVAL;
	ret = m9x_faa_depth_stages_encode(views, 0x3f800000,
					  &(struct m9x_faa_depth_stage_image){
						  0 });
	if (ret)
		return ret;
	ret = m9x_faa_color_encode(first, first_bytes, owner, init_bytes,
				   init_point + 0x70, input, first_scratch);
	if (ret)
		return ret;
	extra = input->declaration_count == 5 ? 24 : 0;
	expected = 1352 + extra;
	sec = *first_scratch->secondary_length;
	if (sec != expected)
		return -EINVAL;
	ret = m9x_faa_depth_mode_two_plan(input->depth_flags ?
						  input->depth_flags :
						  15,
					  m9_cmd_get(first_scratch->secondary +
						     660 + extra),
					  &before, owner, views, &mode);
	if (ret)
		return ret;
	ret = m9x_faa_first_depth_prefix_encode(sample, sample_bytes, owner,
						views, init_point, primary,
						primary_capacity, &length, ends,
						prefix_scratch);
	if (ret)
		return ret;
	memcpy(primary + length, first_scratch->primary + 64, 3588);
	memcpy(secondary, first_scratch->secondary, sec);
	/* Original count2 FIRST 181fa0: EF enable bit, A9 bound depth,
	 * default null BA view from12e75a, and A8 live comparison/write state.
	 */
	m9_cmd_put(secondary + 660 + extra, mode.ef);
	m9_cmd_put(secondary + 816 + extra, mode.a9);
	m9_cmd_put(secondary + 832 + extra, mode.ba);
	m9_cmd_put(secondary + 836 + extra, mode.ba_descriptor);
	m9_cmd_put(secondary + 940 + extra, mode.a8);
	for (i = 0; i < M9_FAA_FIRST_DEPTH_PREFIX_ENDS; i++)
		ends[i] += init_bytes;
	ends[10] = init_bytes + length + 24;
	ends[12] = init_bytes + length + 3588 - 12;
	ends[11] = init_bytes + length + 3588 - 12;
	ends[13] = init_bytes + length + 3588;
	*primary_bytes = length + 3588;
	*secondary_bytes = sec;
	return 0;
}
