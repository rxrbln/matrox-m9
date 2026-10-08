/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
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

/* Initialized same-context DRAW retains a 1x1 null-target 70 packet before
 * the two genuine texture packets. The latter have distinct source sites;
 * no generic scan may mistake the null target for stage 0. */
int m9x_faa_texture_dual_bind_later_place(
	const struct m9x_faa_texture_bind_input unit[2],
	const struct m9x_faa_texture_dual_program *program,
	u8 *secondary, u32 secondary_bytes);

/* Public SELECTARG2 diagnostic keeps both retained texture references but
 * only stage 0 has an active sampler packet. The stage-1 packet is idle. */
int m9x_faa_texture_dual_colorpass_place(
	const struct m9x_faa_texture_bind_input unit[2],
	const struct m9x_faa_texture_dual_program *program,
	u8 *secondary, u32 secondary_bytes);

/* Bind units 1 (single-texture) or 2 at their sampler 0x70 header sites,
 * as reported by the later stage composer. unit[j] is stage j.
 */
int m9x_faa_texture_bind_sites(const struct m9x_faa_texture_bind_input *unit,
			       u32 units, u8 *secondary, u32 secondary_bytes,
			       const u32 site[2]);

/* Single-texture FS48 stage: one sampler packet whose 0x70 header is at
 * byte offset site; the following stage-1 unit packet must be idle.
 */
int m9x_faa_texture_single_bind_place(const struct m9x_faa_texture_bind_input *unit,
				      const struct m9x_faa_texture_dual_program *program,
				      u8 *secondary, u32 secondary_bytes, u32 site);

#endif
