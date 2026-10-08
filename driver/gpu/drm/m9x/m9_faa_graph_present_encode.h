/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_GRAPH_PRESENT_ENCODE_H
#define M9_FAA_GRAPH_PRESENT_ENCODE_H
#include <linux/types.h>

#define M9_FAA_PRESENT_PRIMARY_BYTES 100u
#define M9_FAA_PRESENT_COPY_END_BYTES 88u

/* An ordinary CC21 destination has the same backing address and shape in
 * selector8's F021 view. These are returned live view facts, not addresses
 * inferred from a descriptor's numeric value. The source must have retired
 * the genuine Unlock F021->F022 copy before this separate-owner operation. */
struct m9x_faa_present_view {
 u32 gpu_base;
 u64 bytes;
 u32 width, height, span;
 u32 descriptor0, descriptor1;
};
struct m9x_faa_present_image {
 u8 primary[M9_FAA_PRESENT_PRIMARY_BYTES];
 u32 end_bytes[3];
 u64 final_point;
};

/* Pure prepare only; neither fences nor posts nor validates GEM ownership.
 * The runtime must retain source, destination and command owners, reserve
 * READ/WRITE fences before publication, and independently retire final_point.
 * Immutable Unlock history is not replaced by this presentation frontier. */
int m9x_faa_present_encode(const struct m9x_faa_present_view *source,
 const struct m9x_faa_present_view *destination, u64 unlock_point,
 struct m9x_faa_present_image *out);
#endif
