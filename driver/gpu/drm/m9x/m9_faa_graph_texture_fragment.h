/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_GRAPH_TEXTURE_FRAGMENT_H
#define M9_FAA_GRAPH_TEXTURE_FRAGMENT_H

#include "m9_faa_graph_texture_bind.h"

enum m9x_faa_program_kind {
	M9_FAA_PROGRAM_DUAL,	/* COLOR0 * texture0 * texture1 */
	M9_FAA_PROGRAM_SINGLE,	/* COLOR0 * texture0 */
	M9_FAA_PROGRAM_KINDS
};

/* Longest FS (dual SELECT blend + test) and its C3 utility pairs. */
#define M9_FAA_TEXTURE_FRAGMENT_MAX_PROGRAM 120u
#define M9_FAA_TEXTURE_FRAGMENT_MAX_CONSTANTS 56u

struct m9x_faa_texture_fragment_iface {
	u32 program_bytes, constant_bytes;
	/* Stage words emitted with this program: f0 fragment mode, 88 usage,
	 * ab colour write mask.
	 */
	u32 raster_f0, usage, color_mask;
};

/* The original M9128 textured FS of program kind (M9_FAA_PROGRAM_DUAL or
 * _SINGLE) for a PARHELIA_M9_FAA_FRAGMENT_* state word: the FIRST FS for
 * word 0, else the later-switch form of the source captures (SELECT or
 * MODULATE stage alpha, ten ADD factor pairs, GEQUAL/LESS/GREATER alpha
 * tests). Emits the FS vector and its C3 utility pairs; states without
 * source refuse with -EINVAL and write nothing.
 */
int m9x_faa_texture_fragment_build(u32 kind, u32 word, u8 *program,
				   u32 program_capacity, u8 *constants,
				   u32 constant_capacity,
				   struct m9x_faa_texture_fragment_iface *iface);

/* Compare an upload's FS vector and FS C3 span with the generated ones. */
int m9x_faa_texture_fragment_check(u32 kind, u32 word, const u8 *program,
				   u32 program_bytes, const u8 *constants,
				   u32 constant_bytes,
				   struct m9x_faa_texture_fragment_iface *iface);

#endif
