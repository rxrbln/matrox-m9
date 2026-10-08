/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_GRAPH_TEXTURE_BIND_H
#define M9_FAA_GRAPH_TEXTURE_BIND_H

#ifdef M9_FAA_TEXTURE_HOST_TEST
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
typedef uint32_t u32;
typedef uint8_t u8;
#else
#include <linux/types.h>
#endif

/* Source-bounded M9128 stage-0, profile-12 BGRA8 texture input.
 * The caller must retain and fence the VRAM BO through actual retirement.
 * This builds input words only; no FAA packet is submitted by this API. */
enum m9x_faa_texture_domain {
	M9_FAA_TEXTURE_VRAM = 1,
	M9_FAA_TEXTURE_SYSTEM = 2,
};

struct m9x_faa_texture_bind_input {
	u32 owner_base;
	u32 owner_bytes;
	u32 owner_domain;
	u32 profile;
	u32 format;
	u32 width, height;
	/* Original 2-D layout kind 01 through 64x64, kind 21 when either
	 * dimension exceeds 64. The BO's data must use that storage layout. */
	u32 storage_kind;
	u32 levels;
	/* D3D texture-stage state enums, admitted subset of the source tables:
	 * point/linear MIN/MAG and MIP, and U/V WRAP 1, CLAMP 3 or the vendor
	 * wrap table's GL CLAMP entry 6 (the stage's border words are zero).
	 * A mip chain may reach 1x1 through the 32x16 tail atlas.
	 */
	u32 stage;
	u32 min_filter, mag_filter, mip_filter;
	u32 address_u, address_v;
	u32 coordinate_sets;
	u32 coordinate_index;
	/* Actual program/raster producer's pre-selection AE word. The helper
	 * accepts only the two source-observed two-coordinate families. */
	u32 raster_ae_base;
};

struct m9x_faa_texture_bind_words {
	u32 unit70;
	u32 address78, descriptor79, descriptor7a;
	u32 sampler7c, sampler7d, sampler87;
	u32 stream0, stream1, raster_ae;
};

int m9x_faa_texture_bind_encode(
	const struct m9x_faa_texture_bind_input *input,
	struct m9x_faa_texture_bind_words *words);

#endif
