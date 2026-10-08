/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_LIVE_H
#define M9_FAA_LIVE_H
#include "m9_faa_program.h"
#include "m9_faa_fragment_state.h"
#include "pipe/p_state.h"
#include <stddef.h>
struct nir_shader;
struct m9_vs_outputs;
struct m9_faa_live_program_cache;
/* Owned by one retained immutable VS state. Constants, raw records, viewport,
 * declaration binding and the final upload are still rebuilt each draw. */
struct m9_faa_live_program_cache *m9_faa_live_program_cache_create(void);
void m9_faa_live_program_cache_destroy(struct m9_faa_live_program_cache *cache);

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
   struct m9_faa_live_program_cache *compile_cache; /* Optional VS-state owner. */
   const struct nir_shader *source_identity; /* Immutable unspecialized VS. */
   const uint32_t *inline_values;
   unsigned inline_count;
   /* Zero for the original VS; normalized uniform COLOR0 uses its appended
    * CB byte offset plus one. Set only after the private NIR rewrite succeeds. */
   unsigned uniform_color_offset_plus_one;
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
   uint8_t varying_location[3];
   uint64_t carry_output_registers;
   uint16_t fused_source_mask; /* Ordinary head/tail availability, count2 only. */
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
/* Explicit two-vector COLOR candidate. The fused VS head and its ordinary
 * tested viewport tail use their own sparse constants. This is not vendor
 * tail byte equality or GPU qualification; four-vector uploads remain
 * available through color_build_state. CPU construction is not a receipt. */
bool m9_faa_live_gl_color_fused_build_state(const struct m9_faa_live_gl *state,
                                          const struct nir_shader *fs,
                                          bool legacy_flat,
                                          struct m9_faa_gl_upload *output,
                                          const char **reason);
bool m9_faa_live_gl_color_fused_build(const struct m9_faa_live_gl *state,
                                    const struct nir_shader *fs,
                                    struct m9_faa_gl_upload *output,
                                    const char **reason);
/* Assemble the source-qualified fixed-function stage-0 combine fragment with
 * live GPU POSITION/COLOR0/TEXCOORD0 preprocessing. This is an owned CPU
 * upload only: native texture ownership, sampler descriptors and raster
 * routing have no application admission yet. */
bool m9_faa_live_gl_texture_combine_build(const struct m9_faa_live_gl *state,
                                          enum m9_faa_texture_combine combine,
                                          bool swapped_arguments,
                                          struct m9_faa_gl_upload *output,
                                          const char **reason);
/* Exactly COLOR0, TEXCOORD0, TEXCOORD1 and two MODULATE texture units.
 * Builds an owned four-vector CPU upload with live VS/CB/vertex inputs; the
 * native sampler and full dual command image remain separately gated. */
bool m9_faa_live_gl_dual_texture_build(const struct m9_faa_live_gl *state,
                                       struct m9_faa_gl_upload *output,
                                       const char **reason);
/* Move a bounded retained FS uniform COLOR to an owned GPU VS uniform output.
 * NULL clones mean the original varying path; success never mutates NIR. */
bool m9_faa_live_gl_color_uniform_normalize(const struct m9_faa_live_gl *live,
                                           const struct nir_shader *fs,
                                           const void *cb,size_t bytes,
                                           struct nir_shader **vertex,
                                           struct nir_shader **fragment,
                                           void **constants,size_t *constant_bytes);
bool m9_faa_fragment_gallium_state(const struct pipe_blend_state *blend,
                                  const struct pipe_depth_stencil_alpha_state *dsa,
                                  struct m9_faa_fragment_state *out);
bool m9_faa_live_gl_alpha_decode(const struct nir_shader *fs,const void *cb,size_t bytes,
                                  enum m9_faa_alpha_mode *mode,bool *present);
bool m9_faa_live_gl_color_fused_effects_build_cb(const struct m9_faa_live_gl *state,
                                                const struct nir_shader *fs,
                                                const struct m9_faa_fragment_state *effects,
                                                const void *fs_cb0,size_t fs_cb0_bytes,
                                                struct m9_faa_gl_upload *output,
                                                const char **reason);
/* Explicit bounded smooth COLOR0 effects. NIR must still be a pass-through
 * COLOR fragment; the supplied source-proven test/blend state is lowered
 * separately. Successful owned construction grants no native admission. */
bool m9_faa_live_gl_color_fused_effects_build(const struct m9_faa_live_gl *state,
                                             const struct nir_shader *fs,
                                             const struct m9_faa_fragment_state *effects,
                                             struct m9_faa_gl_upload *output,
                                             const char **reason);
void m9_faa_gl_upload_fini(struct m9_faa_gl_upload *upload);
#endif
