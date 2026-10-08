/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_TEXTURE_DUAL_H
#define M9_FAA_GRAPH_TEXTURE_DUAL_H

#include "m9_faa_graph_texture_bind.h"

/* The original M9128 COLOR0, TEXCOORD0, TEXCOORD1 four-stage interface.
 * Program bytes and the rest of the command image need separate validation.
 * This helper authenticates and fills only the two texture-unit packets. */
struct m9x_faa_texture_dual_program {
	u32 stage_count, sampler_count, coordinate_sets;
	u32 color_op0, color_op1;
	u32 entry_bytes[4];
	u32 program_bytes;
};

int m9x_faa_texture_dual_bind_place(
	const struct m9x_faa_texture_bind_input unit[2],
	const struct m9x_faa_texture_dual_program *program,
	u8 *secondary, u32 secondary_bytes);

/* A repeated FIRST DRAW has a distinct 2868-byte source grammar. Its
 * preliminary 1x1 null-target packet is not a texture owner; the two real
 * texture packets follow at source-qualified sites 2508 and 2608. */
int m9x_faa_texture_dual_bind_append_place(
	const struct m9x_faa_texture_bind_input unit[2],
	const struct m9x_faa_texture_dual_program *program,
	u8 *secondary, u32 secondary_bytes);

#endif
