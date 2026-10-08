// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/errno.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include "m9_faa_graph_dual_batch_input.h"

/* Copy draw descriptors and their uploads; draw 0 defines the program. */
static int dual_draws_copy(u64 draws, u32 draw_count, u32 width, u32 height,
			   u32 flags, u32 clear_flags,
			   struct m9x_faa_owned_color_batch **result)
{
	struct drm_parhelia_m9_faa_color_draw *desc = NULL;
	struct m9x_faa_owned_color_batch *batch = NULL;
	u32 i, j, bytes, sum, raw;
	int ret = -EINVAL;

	if (check_mul_overflow(draw_count, (u32)sizeof(*desc), &bytes))
		return -ERANGE;
	desc = memdup_user(u64_to_user_ptr(draws), bytes);
	if (IS_ERR(desc))
		return PTR_ERR(desc);
	batch = kzalloc(sizeof(*batch), GFP_KERNEL);
	if (!batch) {
		ret = -ENOMEM;
		goto out;
	}
	batch->flags = flags;
	batch->clear_flags = clear_flags;
	for (i = 0; i < draw_count; i++) {
		const struct drm_parhelia_m9_faa_color_draw *d = &desc[i];
		const struct drm_parhelia_m9_faa_upload_layout *l = &d->layout;
		struct m9x_faa_owned_color_draw *job = &batch->draw[i];
		struct m9x_faa_color_input *in = &job->input;

		if (!d->upload || d->pad[0] || d->pad[1] || d->raster_flags ||
		    !m9x_faa_depth_flags_valid(d->depth_flags) ||
		    d->record_stride != 64 || !d->record_count ||
		    d->record_count % 3 || d->record_count > 12288 ||
		    d->declaration_count != 9 || l->program_count != 4 ||
		    !l->program_bytes || l->program_bytes > 4096 ||
		    (l->program_bytes & 7) || !l->constants_bytes ||
		    l->constants_bytes > 16384 || (l->constants_bytes & 7) ||
		    !l->geometry_bytes || l->geometry_bytes > 0x40000 ||
		    l->vertex_offset != 128 ||
		    check_mul_overflow(d->record_count, 64u, &raw) ||
		    raw != l->vertex_bytes || raw > l->geometry_bytes ||
		    l->vertex_offset > l->geometry_bytes - raw ||
		    check_add_overflow(l->program_bytes, l->constants_bytes,
				       &sum) ||
		    check_add_overflow(sum, l->geometry_bytes, &sum) ||
		    check_add_overflow(sum, 36u, &sum) ||
		    sum != d->upload_bytes || sum > 0x42000)
			goto out;
		for (j = 0; j < 4; j++)
			if ((l->entry_bytes[j] & 7) ||
			    l->entry_bytes[j] >= l->program_bytes ||
			    (j && l->entry_bytes[j] <= l->entry_bytes[j - 1]) ||
			    (l->constant_begin[j] & 7) ||
			    (l->constant_end[j] & 7) ||
			    l->constant_begin[j] > l->constant_end[j] ||
			    l->constant_end[j] > l->constants_bytes)
				goto out;
		job->upload = memdup_user(u64_to_user_ptr(d->upload), sum);
		if (IS_ERR(job->upload)) {
			ret = PTR_ERR(job->upload);
			job->upload = NULL;
			goto out;
		}
		memcpy(&in->layout, l, sizeof(*l));
		in->program = job->upload;
		in->constants = in->program + l->program_bytes;
		in->geometry = in->constants + l->constants_bytes;
		in->declaration = in->geometry + l->geometry_bytes;
		in->width = width;
		in->height = height;
		in->record_stride = d->record_stride;
		in->record_count = d->record_count;
		in->declaration_count = d->declaration_count;
		in->depth_flags = d->depth_flags;
		in->dual_base_only = true;
		job->bytes = sum;
		if (i) {
			const struct m9x_faa_color_input *f =
				&batch->draw[0].input;
			if (l->program_bytes != f->layout.program_bytes ||
			    memcmp(in->program, f->program, l->program_bytes) ||
			    memcmp(l->entry_bytes, f->layout.entry_bytes,
				   sizeof(l->entry_bytes)) ||
			    memcmp(in->declaration, f->declaration, 36) ||
			    d->depth_flags != f->depth_flags) {
				ret = -EOPNOTSUPP;
				goto out;
			}
		}
		batch->count++;
	}
	*result = batch;
	batch = NULL;
	ret = 0;
out:
	kfree(desc);
	m9x_faa_color_batch_free(batch);
	return ret;
}

int m9x_faa_dual_batch_copy(const struct drm_parhelia_m9_faa_graph_dual_batch
				    *args,
			    struct m9x_faa_owned_color_batch **result)
{
	if (!result)
		return -EINVAL;
	*result = NULL;
	if (!args || !args->graph_id || !args->init_point ||
	    (args->init_point & 15) || args->lock_point || !args->draws ||
	    args->draw_count < 2 ||
	    args->draw_count > M9_FAA_DUAL_BATCH_MAX_DRAWS ||
	    args->version != PARHELIA_M9_FAA_DUAL_BATCH_VERSION ||
	    args->flags || args->state || args->pad || args->width != 320 ||
	    args->height != 240 || args->clear_flags != 3 || args->clear_color)
		return -EOPNOTSUPP;
	return dual_draws_copy(args->draws, args->draw_count, args->width,
			       args->height, PARHELIA_M9_FAA_COLOR_FRAME_FIRST,
			       3, result);
}

int m9x_faa_dual_later_batch_copy(const struct drm_parhelia_m9_faa_graph_dual_later
					  *args,
				  struct m9x_faa_owned_color_batch **result)
{
	if (!result)
		return -EINVAL;
	*result = NULL;
	if (!args ||
	    args->version != PARHELIA_M9_FAA_DUAL_LATER_BATCH_VERSION ||
	    !args->draws || !args->draw_count ||
	    args->draw_count > M9_FAA_DUAL_BATCH_MAX_DRAWS || args->pad2 ||
	    args->upload || args->upload_bytes || args->record_count ||
	    args->record_stride || args->declaration_count ||
	    (args->clear_flags && args->clear_flags != 3 &&
	     args->clear_flags != 2))
		return -EOPNOTSUPP;
	return dual_draws_copy(args->draws, args->draw_count, args->width,
			       args->height, 0, args->clear_flags, result);
}
