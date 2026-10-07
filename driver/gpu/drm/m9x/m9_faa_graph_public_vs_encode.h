/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_GRAPH_PUBLIC_VS_ENCODE_H
#define M9_FAA_GRAPH_PUBLIC_VS_ENCODE_H
#include "m9_faa_graph_draw_encode.h"
#define M9_FAA_PUBLIC_VS_PROGRAM_BYTES 328u
#define M9_FAA_PUBLIC_VS_CONSTANT_BYTES 152u
#define M9_FAA_PUBLIC_VS_GEOMETRY_BYTES 224u
#define M9_FAA_PUBLIC_VS_CONSTANT_EXTENT 480u
#define M9_FAA_PUBLIC_VS_SECONDARY_BYTES 752u
#define M9_FAA_PUBLIC_VS_PRIMARY_BYTES 112u
#define M9_FAA_PUBLIC_VS_TEMPLATE_BYTES 864u
/* Matches Mesa's address-free m9_faa_preprocess_upload_layout. All lengths
 * are bytes. This initial COPY code/declaration shape is guarded; successful
 * encoding is not generic shader admission or a GPU completion receipt. */
struct m9x_faa_public_vs_upload_layout {
	u32 program_bytes, program_count, entry_bytes[4];
	u32 constants_bytes, constant_begin[4], constant_end[4];
	u32 geometry_bytes, vertex_offset, vertex_bytes;
	u32 declaration[5];
};
struct m9x_faa_public_vs_input {
	const u8 *program, *constants, *geometry;
	u32 program_bytes, constants_bytes, geometry_bytes;
	struct m9x_faa_public_vs_upload_layout layout;
};
struct m9x_faa_public_vs_image {
	u8 *program, *constants, *secondary, *primary;
	u32 program_bytes, constants_bytes, secondary_bytes, primary_bytes;
};
struct m9x_faa_public_vs_endpoints {
	u32 first_end, final_end;
	u64 original_wait, independent_finish;
};
bool m9x_faa_public_vs_template_valid(const u8 *blob, size_t bytes);
/* Input words are caller-owned, copied from live compiled/typed uploads.
 * The authenticated template has no addresses, timeline, declaration or
 * relative-field payloads. This emits ONLY new images; the old INIT prefix,
 * graph/pool/sentinel contents and unknown constants gap are untouched.
 * Caller must hold the same retained binding/generation and program map
 * sequence: actual INIT →314 →CPU-root f/0/c →program upload→31c0, then
 * write primary first64→END twice without wait→secondary/full tail→final
 * END+112 and actual independent wait. This function does no MMIO or post. */
int m9x_faa_public_vs_encode(
	const u8 *blob, size_t bytes,
	const struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS],
	u32 live_init_primary_bytes, u64 init_point,
	const struct m9x_faa_public_vs_input *input,
	const struct m9x_faa_public_vs_image *image,
	struct m9x_faa_public_vs_endpoints *ends);
#endif
