/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_TEXTURE_PLACE_H
#define M9_FAA_GRAPH_TEXTURE_PLACE_H

#include "m9_faa_graph_texture_bind.h"

/* Original M9128 four-stage COLOR0/TEXCOORD0 fixed-function profile.
 * Program bytes and all non-texture secondary words are owned and validated
 * by the caller; this helper only finds and rewrites its typed texture sites. */
struct m9x_faa_texture_program_interface {
	u32 stage_count, sampler_count;
	u32 color_stream, texcoord_stream;
	u32 color_op; /* Original public COLOROP 4 MODULATE or 7 ADD. */
	u32 entry_bytes[4];
};

int m9x_faa_texture_bind_place(
	const struct m9x_faa_texture_bind_input *input,
	const struct m9x_faa_texture_program_interface *program,
	u8 *secondary, u32 secondary_bytes);

#endif
