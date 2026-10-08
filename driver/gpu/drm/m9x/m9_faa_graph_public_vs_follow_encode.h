/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_GRAPH_PUBLIC_VS_FOLLOW_ENCODE_H
#define M9_FAA_GRAPH_PUBLIC_VS_FOLLOW_ENCODE_H
#include "m9_faa_graph_draw_encode.h"
#include "m9_hw_encode.h"
#define M9_FAA_VS_FOLLOW_PRIMARY_BYTES 48u
#define M9_FAA_VS_FOLLOW_SECONDARY_BYTES 616u
#define M9_FAA_VS_FOLLOW_SECONDARY_OFFSET 752u
#define M9_FAA_VS_FOLLOW_TEMPLATE_BYTES 664u
#define M9_FAA_VS_FOLLOW_PATCH_OFFSET 704u
#define M9_FAA_VS_FOLLOW_PATCH_EXPECTED M9_HW_TRANSITION_LENGTH
struct m9x_faa_public_vs_follow_image {
	u8 *primary, *secondary;
	u32 primary_bytes, secondary_bytes;
};
struct m9x_faa_public_vs_follow_endpoints {
	u32 final_end, patch_word;
	u64 original_wait, independent_finish;
};
bool m9x_faa_public_vs_follow_template_valid(const u8 *blob, size_t bytes);
int m9x_faa_public_vs_follow_encode(
    const u8 *blob, size_t bytes,
    const struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS], u32 live_primary_bytes,
    u64 prefix_point, const struct m9x_faa_public_vs_follow_image *image,
    struct m9x_faa_public_vs_follow_endpoints *ends);
#endif
