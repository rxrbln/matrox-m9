// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* DRAWs of the dual and single-texture FAA programs in one frame. */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include "m9_faa_graph_mixed_draw.h"

int m9x_faa_mixed_draw_check(const struct m9x_faa_color_input *job, u32 kind,
			     u32 *span)
{
	bool one = kind == M9_FAA_PROGRAM_SINGLE;
	u32 raw, records, geo;

	if (!job || !span || kind > M9_FAA_PROGRAM_SINGLE ||
	    job->data_offset || job->layout.program_count != 4 ||
	    (job->record_stride != 64 && !(one && job->record_stride == 48)) ||
	    job->declaration_count != (one ? 7 : 9) ||
	    job->dual_base_only == one || job->single_base_only != one ||
	    job->fragment_flags || job->dual_colorpass_diagnostic ||
	    job->layout.constants_bytes > 16384 || job->record_count > 12288 ||
	    !job->record_count || job->record_count % 3 ||
	    !m9x_faa_depth_flags_valid(job->depth_flags))
		return -EOPNOTSUPP;
	records = (job->record_count + 3) & ~3u;
	geo = (job->layout.constants_bytes + 127) & ~127u;
	if (check_mul_overflow(records, job->record_stride, &raw) ||
	    check_add_overflow(geo,
			       job->layout.geometry_bytes > 128 + raw ?
			       job->layout.geometry_bytes : 128 + raw, span))
		return -EOPNOTSUPP;
	return 0;
}

int m9x_faa_mixed_first_encode(u32 kind, const struct m9x_faa_mixed_templates *tpl,
			       const struct m9x_faa_graph_draw_owner owner[31],
			       const struct m9x_faa_depth_views *views,
			       u32 init_bytes, u64 epoch,
			       const struct m9x_faa_color_input *job,
			       const struct m9x_faa_texture_bind_input *bind,
			       u32 program_offset, u8 *primary,
			       size_t primary_capacity, u8 *stage,
			       u32 *primary_bytes, u32 *ends,
			       const struct m9x_faa_color_image *live,
			       struct m9x_faa_first_depth_scratch *scratch,
			       u32 *stage_bytes)
{
	if (!stage_bytes || !tpl)
		return -EINVAL;
	if (kind == M9_FAA_PROGRAM_SINGLE) {
		struct m9x_faa_single_first_input one = {
			.color = job, .texture = bind,
			.program_offset = program_offset
		};

		*stage_bytes = M9_FAA_SINGLE_FIRST_SECONDARY_BYTES;
		return m9x_faa_single_first_encode(tpl->sample, tpl->sample_bytes,
						   tpl->color, tpl->color_bytes,
						   tpl->single, tpl->single_bytes,
						   owner, views,
						   init_bytes, epoch, &one,
						   primary, primary_capacity,
						   stage, *stage_bytes,
						   primary_bytes, ends, live,
						   scratch);
	}
	if (kind == M9_FAA_PROGRAM_DUAL) {
		struct m9x_faa_dual_first_input two = {
			.color = job, .texture = bind,
			.program_offset = program_offset
		};

		*stage_bytes = M9_FAA_DUAL_FIRST_SECONDARY_BYTES;
		return m9x_faa_dual_first_encode(tpl->sample, tpl->sample_bytes,
						 tpl->color, tpl->color_bytes,
						 tpl->dual, tpl->dual_bytes,
						 owner, views,
						 init_bytes, epoch, &two,
						 primary, primary_capacity,
						 stage, *stage_bytes,
						 primary_bytes, ends, live,
						 scratch);
	}
	return -EINVAL;
}
