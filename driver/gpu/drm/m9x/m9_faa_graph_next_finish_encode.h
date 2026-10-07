/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_GRAPH_NEXT_FINISH_ENCODE_H
#define M9_FAA_GRAPH_NEXT_FINISH_ENCODE_H

#include "m9_faa_graph_draw_encode.h"

#define M9_FAA_NEXT_FINISH_PRIMARY_BYTES 24u
#define M9_FAA_NEXT_FINISH_SECONDARY_BYTES 708u
#define M9_FAA_NEXT_FINISH_SECONDARY_OFFSET 2040u
#define M9_FAA_NEXT_FINISH_BLOB_BYTES 732u

struct m9x_faa_graph_next_finish_image {
	u8 *primary, *secondary;
	u32 primary_bytes, secondary_bytes;
};

bool m9x_faa_graph_next_finish_blob_valid(const u8 *blob, size_t bytes);

/* Two ordered END posts after the retained second-DRAW prefix. The caller
 * authenticates the graph, reserves its six BOs, supplies the live cursor,
 * posts +12 without a wait, then posts +24 and waits for prefix+0x30. */
int m9x_faa_graph_next_finish_encode(const u8 *blob, size_t bytes,
	const struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS],
	u32 live_primary_bytes, u64 prefix_point,
	const struct m9x_faa_graph_next_finish_image *image);

#endif
