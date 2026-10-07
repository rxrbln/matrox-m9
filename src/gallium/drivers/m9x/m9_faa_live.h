/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_LIVE_H
#define M9_FAA_LIVE_H
#include "m9_faa_program.h"
#include "pipe/p_state.h"
#include <stddef.h>
struct nir_shader;
struct m9_vs_outputs;

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

/* CPU snapshot budget, not a hardware vertex/program capability. */
#define M9_FAA_GL_UPLOAD_MAX_VERTICES 2048u
#define M9_FAA_GL_UPLOAD_SLOT_BUDGET 188u

/* Generic GL carry compiler interface. Fragment is a final LIVE compiler
 * vector with its dense keys, supplied by its separately qualified frontend.
 * One or two direct vec4 linkage streams must be explicit and complete; absent
 * diffuse/texcoord outputs are not silently manufactured. */
struct m9_faa_live_gl {
   const struct nir_shader *vs;
   const struct m9_vs_outputs *linkage;
   const void *cb0;
   size_t cb0_bytes;
   struct m9_faa_program_vector fragment;
   struct m9_faa_dense_constants fragment_constants;
   const struct pipe_vertex_element *elements;
   unsigned element_count;
   const struct pipe_vertex_buffer *buffers;
   unsigned buffer_count;
   const uint8_t *const *maps;
   const unsigned *map_bytes;
   const uint32_t *indices; /* Optional count owned indices, relative to first. */
   unsigned first, count;
   struct m9_faa_viewport viewport;
};

/* All buffers point into one owned allocation. Constants and geometry stay
 * separate: no unsupported combined-owner offset or native placement is
 * inferred from the old 100h source geometry offset. No resource/NIR pointers
 * survive. Call fini before retargeting. Zero-initialize before first build. */
struct m9_faa_gl_upload {
   void *storage;
   size_t storage_bytes;
   uint32_t *program, *constants, *geometry, *declaration;
   struct m9_faa_program_layout layout;
   uint32_t constant_begin[4], constant_end[4], constants_bytes;
   uint32_t geometry_bytes, vertex_offset, vertex_bytes;
   uint32_t declaration_words, record_bytes, vertex_count;
   uint8_t varying_location[2];
   uint64_t carry_output_registers;
   bool color_flat; /* Live FS load_input vs smooth interpolation; no raster receipt. */
   bool color_legacy_interpolation; /* COLOR0 INTERP_MODE_NONE follows live raster flatshade. */
   /* Explicitly false until a broader native layout/fragment/launcher
    * contract and hardware receipt are qualified. */
   bool native_admission;
};

bool m9_faa_live_gl_build(const struct m9_faa_live_gl *state,
                           struct m9_faa_gl_upload *output,
                           const char **reason);
/* Compile a real pass-through COLOR0 FS into the live four-vector upload.
 * Handles scalarized stores and smooth pixel interpolation; unsupported
 * fragment arithmetic, side effects or additional inputs refuse. */
bool m9_faa_live_gl_color_build(const struct m9_faa_live_gl *state,
                              const struct nir_shader *fs,
                              struct m9_faa_gl_upload *output,
                              const char **reason);
/* Legacy FF COLOR interpolation follows the live Gallium raster state. */
bool m9_faa_live_gl_color_build_state(const struct m9_faa_live_gl *state,
                                    const struct nir_shader *fs, bool legacy_flat,
                                    struct m9_faa_gl_upload *output,
                                    const char **reason);
void m9_faa_gl_upload_fini(struct m9_faa_gl_upload *upload);
#endif
