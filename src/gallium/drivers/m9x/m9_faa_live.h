/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_LIVE_H
#define M9_FAA_LIVE_H
#include "m9_faa_program.h"
#include "pipe/p_state.h"
#include <stddef.h>
struct nir_shader;

/* Bounded public MOV shader interface, before graph/job binding. The caller
 * retains lowered NIR, CB0 and mapped vertex buffers throughout this call.
 * POSITION is already D3D clip space; GL z adaptation needs a larger carry
 * program and is deliberately not admitted by this 328-byte interface.
 * COLOR and TEXCOORD locations come from live shader linkage. */
struct m9_faa_live_public_mov {
   const struct nir_shader *vs, *fs;
   const void *vs_cb0, *fs_cb0;
   size_t vs_cb0_bytes, fs_cb0_bytes;
   unsigned color_location, texcoord_location;
   const struct pipe_vertex_element *elements;
   unsigned element_count;
   const struct pipe_vertex_buffer *buffers;
   unsigned buffer_count;
   const uint8_t *const *maps;
   const unsigned *map_bytes;
   unsigned first;
   bool d3d_clip_space;
   struct m9_faa_viewport viewport;
};

/* Address-free ABI payload: layout92 + program328 + constants152 + records224.
 * No handles, GPU addresses, captured inputs, ioctl or graph INIT receipt.
 * Initialized application state must not be inferred from successful build. */

/* Exactly three nonindexed vertices, two complete FLOAT4 input semantics;
 * VS = position input, CB0 color, texcoord input. FS = literal/CB0 vec4.
 * Colors must be finite +0..1 (negative zero refused): source clamp then
 * preserves these live MOV values. ALU, textures, indirect loads, partial
 * outputs, side effects and control flow refuse. All failures leave output
 * unchanged. Output storage must be disjoint from retained NIR storage. */
bool m9_faa_live_public_mov_build(const struct m9_faa_live_public_mov *state,
                                  struct m9_faa_public_upload *output);
#endif
