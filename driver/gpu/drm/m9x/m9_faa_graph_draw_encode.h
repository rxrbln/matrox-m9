/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_GRAPH_DRAW_ENCODE_H
#define M9_FAA_GRAPH_DRAW_ENCODE_H

#include <linux/types.h>

/* Source owner indices follow the sealed post-INIT plan. The caller maps
 * retained INIT owners 0..25 unchanged, old primary 26 -> 27, old startup
 * 27 -> 30, and supplies three new live owners at 26/28/29. */
#define M9_FAA_DRAW_SOURCE_OWNERS 31u
#define M9_FAA_DRAW_PROGRAM_BYTES 240u
#define M9_FAA_DRAW_CONSTANTS_BYTES 444u
#define M9_FAA_DRAW_SECONDARY_BYTES 664u
#define M9_FAA_DRAW_PRIMARY_SUFFIX_BYTES 88u
#define M9_FAA_DRAW_BLOB_BYTES 123604u
#define M9_FAA_DRAW_BLOB_FNV 0x47268950u

struct m9x_faa_graph_draw_owner {
	u32 base, bytes;
};

/* The constants gap [136,256) has no source writes and is not initialized.
 * All destinations must be caller-owned and disjoint from the source blob. */
struct m9x_faa_graph_draw_image {
	u8 *program, *constants, *secondary, *primary_suffix;
	u32 program_bytes, constants_bytes, secondary_bytes, primary_suffix_bytes;
};

bool m9x_faa_graph_draw_blob_valid(const u8 *blob, size_t blob_bytes);

int m9x_faa_graph_draw_encode(const u8 *blob, size_t blob_bytes,
		const struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS],
		u32 live_init_primary_bytes, u64 init_point,
		const struct m9x_faa_graph_draw_image *image);

#endif
