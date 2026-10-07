/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_GRAPH_NEXT_ENCODE_H
#define M9_FAA_GRAPH_NEXT_ENCODE_H

#include "m9_faa_graph_draw_encode.h"

#define M9_FAA_NEXT_PRIMARY_BYTES 24u
#define M9_FAA_NEXT_SECONDARY_BYTES 668u
#define M9_FAA_NEXT_VERTEX_BYTES 60u
#define M9_FAA_NEXT_SECONDARY_OFFSET 1372u
#define M9_FAA_NEXT_VERTEX_OFFSET 464u
#define M9_FAA_NEXT_BLOB_BYTES 692u

struct m9x_faa_graph_next_image {
	u8 *primary, *secondary, *vertices;
	u32 primary_bytes, secondary_bytes, vertex_bytes;
};

bool m9x_faa_graph_next_blob_valid(const u8 *blob, size_t bytes);

/* Source-only bounded second DRAW after the same graph's retired Unlock.
 * Every address and timeline field is generated from retained live owners.
 * The caller owns the graph, live cursor, fence, publication and wait. */
int m9x_faa_graph_next_encode(const u8 *blob, size_t bytes,
	const struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS],
	u32 live_primary_bytes, u64 unlock_point, const u32 vertices[15],
	const struct m9x_faa_graph_next_image *image);

#endif
