/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_GRAPH_MIXED_DRAW_H
#define M9_FAA_GRAPH_MIXED_DRAW_H

#include "m9_faa_graph_single_first_encode.h"
#include "m9_faa_graph_later_stage.h"

/* Admit one DRAW of program kind (M9_FAA_PROGRAM_DUAL/SINGLE)
 * and size its arena slot: constants, then header and padded records.
 */
int m9x_faa_mixed_draw_check(const struct m9x_faa_color_input *job, u32 kind,
			     u32 *span);

/* Firmware templates of both programs' FIRST encodes. */
struct m9x_faa_mixed_templates {
	const u8 *sample, *color, *dual, *single;
	size_t sample_bytes, color_bytes, dual_bytes, single_bytes;
};

/* The DRAW program's live FIRST encode at epoch, dual or single by kind.
 * stage receives the FIRST stage and stage_bytes its length.
 */
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
			       u32 *stage_bytes);

#endif
