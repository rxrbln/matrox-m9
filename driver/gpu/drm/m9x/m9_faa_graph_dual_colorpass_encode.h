/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_DUAL_COLORPASS_ENCODE_H
#define M9_FAA_GRAPH_DUAL_COLORPASS_ENCODE_H
#include "m9_faa_graph_dual_first_encode.h"

#define M9_FAA_DUAL_COLORPASS_SECONDARY_BYTES 2792u

/* Build the source-known MODULATE program/CB CPU shadow used solely by the
 * existing dual FIRST packet constructor. The diagnostic publishes only the
 * caller's SELECTARG2 code/CB and the transformed COLOR-pass packet image. */
int m9x_faa_dual_colorpass_shadow(
 const struct m9x_faa_color_input *pass,
 u8 *program,size_t program_capacity,
 u8 *constants,size_t constants_capacity,
 struct m9x_faa_color_input *dual);

/* Transform an already typed, unpublished dual FIRST image. Both uploads
 * must describe the same live three-varying geometry and VS stages. The
 * COLOR-pass upload carries its own source-generated FS32 and dense CB.
 * This helper never changes a captured address into a native address. */
int m9x_faa_dual_colorpass_encode(
 const struct m9x_faa_color_input *dual,
 const struct m9x_faa_color_input *pass,
 const struct m9x_faa_graph_draw_owner owner[31],
 u32 init_bytes,const u32 dual_ends[M9_FAA_DUAL_FIRST_ENDS],
 const u8 *dual_primary,u32 primary_bytes,
 const u8 *dual_secondary,u32 dual_secondary_bytes,
 u8 *primary,size_t primary_capacity,
 u8 *secondary,size_t secondary_capacity);
#endif
