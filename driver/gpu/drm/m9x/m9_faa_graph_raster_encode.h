/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_GRAPH_RASTER_ENCODE_H
#define M9_FAA_GRAPH_RASTER_ENCODE_H
#include <linux/types.h>

#define M9_FAA_RASTER_FLAT 1u
#define M9_FAA_RASTER_CULL_CW 2u
#define M9_FAA_RASTER_CULL_CCW 4u

/* The caller supplies its typed live secondary range and four primary
 * indirect range fields. Database offsets are instruction starts produced
 * by the source fixed-function library, never source CPU/GPU addresses. */
struct m9x_faa_raster_input {
	const u8 *primary, *secondary;
	u32 primary_bytes, secondary_bytes, secondary_base;
	u32 range_at[4];
	u32 database_bytes, no_cull_start_bytes, cull_start_bytes;
	u32 flags;
};
struct m9x_faa_raster_output {
	u8 *primary, *secondary;
	u32 primary_capacity, secondary_capacity;
	u32 *secondary_bytes;
};
int m9x_faa_raster_encode(const struct m9x_faa_raster_input *input,
			 const struct m9x_faa_raster_output *output);
int m9x_faa_raster_two_encode(const struct m9x_faa_raster_input *input,
			     const struct m9x_faa_raster_output *output);
#endif
