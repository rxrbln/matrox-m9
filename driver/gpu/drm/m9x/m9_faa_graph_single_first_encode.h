/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_GRAPH_SINGLE_FIRST_ENCODE_H
#define M9_FAA_GRAPH_SINGLE_FIRST_ENCODE_H

#include "m9_faa_graph_dual_first_encode.h"

#define M9_FAA_SINGLE_FIRST_FIRMWARE "m9x/faa-graph-single-first-template.bin"
#define M9_FAA_SINGLE_FIRST_SECONDARY_BYTES M9_FAA_SINGLE_STAGE_BYTES
/* Secondary offset of the second primary range (dual: 1912). */
#define M9_FAA_SINGLE_FIRST_SPLIT 1840u
/* Byte offset of the stage-0 0x70 unit packet header. */
#define M9_FAA_SINGLE_FIRST_SAMPLER_SITE 2448u
#define M9_FAA_SINGLE_FIRST_VERTEX_BANKS 5u

/* COLOR0 * TEX0 MODULATE with stage 1 disabled (FS48). The template is
 * address-free; its SHA authenticates the immutable three-target opcodes.
 * Vertex records are XYZ, D3DCOLOR, UV0 at +16: stride 24 (FVF 0x142) or
 * 32 (FVF 0x242, the dual record with UV1 ignored), or the live 64-byte
 * vec4 record of the dual path with its UV1 ignored.
 */
struct m9x_faa_single_first_input {
	const struct m9x_faa_color_input *color;
	const struct m9x_faa_texture_bind_input *texture; /* exactly one unit */
	u32 program_offset;
};

int m9x_faa_single_first_encode(const u8 *sample, size_t sample_bytes,
				const u8 *color_template,
				size_t color_template_bytes,
				const u8 *single_template,
				size_t single_template_bytes,
				const struct m9x_faa_graph_draw_owner owner[31],
				const struct m9x_faa_depth_views *views,
				u32 init_bytes, u64 init_point,
				const struct m9x_faa_single_first_input *input,
				u8 *primary, size_t primary_capacity,
				u8 *secondary, size_t secondary_capacity,
				u32 *primary_bytes, u32 *ends,
				const struct m9x_faa_color_image *color_scratch,
				struct m9x_faa_first_depth_scratch *prefix_scratch);

#endif
