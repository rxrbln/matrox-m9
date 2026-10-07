/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_GRAPH_COLOR_NEXT_ENCODE_H
#define M9_FAA_GRAPH_COLOR_NEXT_ENCODE_H
#include "m9_faa_graph_color_encode.h"

#define M9_FAA_COLOR_NEXT_PRIMARY_BYTES 3624u
#define M9_FAA_COLOR_NEXT_CLEAR_CAPACITY 128u

struct m9x_faa_color_next_image {
	/* Build live program state with the first encoder, then emit the genuine
	 * retained grammar. Primary needs its 3696-byte scratch capacity. */
	struct m9x_faa_color_image image;
	u8 *clear;
	u32 clear_capacity;
	u32 *clear_length;
	/* Separate caller-owned storage; never submitted to the GPU. */
	u8 *secondary_scratch;
	u32 secondary_scratch_capacity;
};

int m9x_faa_color_next_encode(const u8 *blob, size_t bytes,
	const struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS],
	u32 init_bytes, u32 secondary_cursor, u64 previous_unlock_point,
	const struct m9x_faa_color_input *input, u32 clear_flags,
	const struct m9x_faa_color_next_image *output);
#endif
