/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_GRAPH_DEPTH_RASTER_ENCODE_H
#define M9_FAA_GRAPH_DEPTH_RASTER_ENCODE_H
#include "m9_faa_graph_depth_draw_encode.h"
#include "m9_faa_graph_raster_encode.h"

struct m9x_faa_depth_raster_input {
	struct m9x_faa_raster_input streams;
	u32 primary_cursor;
	u32 end_bytes[M9_FAA_DEPTH_DRAW_ENDS];
};
struct m9x_faa_depth_raster_output {
	struct m9x_faa_raster_output streams;
	u32 end_bytes[M9_FAA_DEPTH_DRAW_ENDS];
};

/* Bounded retained depth DRAW/Lock suffix. Primary length and END positions
 * remain unchanged; inserted continuation binds grow only the secondary.
 * Caller supplies the validated resident library's instruction starts. */
int m9x_faa_depth_raster_encode(const struct m9x_faa_depth_raster_input *input,
			      struct m9x_faa_depth_raster_output *output);
#endif
