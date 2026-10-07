/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_GRAPH_DRAW_TAIL_ENCODE_H
#define M9_FAA_GRAPH_DRAW_TAIL_ENCODE_H

#include "m9_faa_graph_draw_encode.h"

#define M9_FAA_DRAW_TAIL_SECONDARY_OFFSET 664u
#define M9_FAA_DRAW_TAIL_SECONDARY_BYTES 708u
#define M9_FAA_DRAW_TAIL_PRIMARY_BYTES 24u
#define M9_FAA_DRAW_TAIL_BLOB_BYTES 732u

struct m9x_faa_graph_draw_tail_image {
	u8 *secondary, *primary;
	u32 secondary_bytes, primary_bytes;
};

bool m9x_faa_graph_draw_tail_blob_valid(const u8 *blob, size_t bytes);

/* Bounded source-red diagnostic only: callers retain the same graph and
 * observe its prefix point before publishing either tail post. Destinations
 * are caller-owned, disjoint from each other and the authenticated blob. */
int m9x_faa_graph_draw_tail_encode(const u8 *blob, size_t bytes,
	const struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS],
	u32 live_primary_bytes, u32 width, u32 height, u64 prefix_point,
	const struct m9x_faa_graph_draw_tail_image *image);

#endif
