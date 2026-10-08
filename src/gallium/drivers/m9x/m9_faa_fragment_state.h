/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_FRAGMENT_STATE_H
#define M9_FAA_FRAGMENT_STATE_H
#include <stdbool.h>
#include <stdint.h>

/* These are complete bounded smooth COLOR0 compiler profiles. Texture,
 * flat, separate-alpha, arbitrary blend factors and disabled depth are
 * separate interfaces and cannot reuse this register allocation. */
enum m9_faa_blend_mode {
   M9_FAA_BLEND_REPLACE,
   M9_FAA_BLEND_ALPHA,
   M9_FAA_BLEND_ADD,
   M9_FAA_BLEND_ALPHA_ADD,
   M9_FAA_BLEND_FILTER,
};
enum m9_faa_alpha_mode {
   M9_FAA_ALPHA_OFF,
   M9_FAA_ALPHA_GT_ZERO,
   M9_FAA_ALPHA_LT_HALF,
   M9_FAA_ALPHA_GE_HALF,
};
struct m9_faa_fragment_state {
   uint32_t source_stream;
   enum m9_faa_blend_mode blend;
   enum m9_faa_alpha_mode alpha;
};
struct m9_faa_fragment_interface {
   uint32_t slots, constant_pairs, source_stream, result_register;
   /* Source compiler allocation metadata for these emitted forms. Native
    * binding must pair these with the owned program and live resource state;
    * shader construction grants no native submission admission. */
   uint32_t available_register_mask;
   bool reads_destination, alpha_test;
};
/* Outputs are disjoint, owned and untouched on refusal. FS C3 starts at
 * pair zero; VS C3 follows the returned utility pairs in a fused profile.
 * Standard alpha blend also admits the source-qualified GE128 test. */
bool m9_faa_fragment_state_build(const struct m9_faa_fragment_state *state,
                                 uint32_t *program, uint32_t program_capacity,
                                 uint32_t *constants, uint32_t constant_capacity,
                                 struct m9_faa_fragment_interface *interface);
/* Recognize an owned emitted profile by its complete ISA and FS utility
 * span. Other same-length shaders are refused; output is immutable on
 * refusal. This is intended for the live fused upload/interface seam. */
bool m9_faa_fragment_state_decode(const uint32_t *program, uint32_t bytes,
                                  const uint32_t *constants, uint32_t constant_bytes,
                                  struct m9_faa_fragment_state *state,
                                  struct m9_faa_fragment_interface *interface);

/* Textured FAA programs of the original later program switch: the dual
 * COLOR0 * texture0 * texture1 (MODULATE lightmap) and the single
 * COLOR0 * texture0 combine, each optionally blended and alpha tested. */
enum m9_faa_texture_program {
   M9_FAA_TEXTURE_SINGLE,
   M9_FAA_TEXTURE_DUAL,
};
/* SELECT: D3D ALPHAOP SELECTARG1 texture (stage 1 SELECTARG1 current).
 * MODULATE: GL MODULATE, texture * diffuse (stage 1 texture * current). */
enum m9_faa_texture_alpha {
   M9_FAA_TEXTURE_ALPHA_SELECT,
   M9_FAA_TEXTURE_ALPHA_MODULATE,
};
enum m9_faa_blend_factor {
   M9_FAA_FACTOR_ZERO,
   M9_FAA_FACTOR_ONE,
   M9_FAA_FACTOR_SRC_COLOR,
   M9_FAA_FACTOR_SRC_ALPHA,
   M9_FAA_FACTOR_INV_SRC_ALPHA,
   M9_FAA_FACTOR_DST_COLOR,
   M9_FAA_FACTOR_INV_DST_COLOR,
};
/* Values are the source compare classes. */
enum m9_faa_alpha_func {
   M9_FAA_ALPHA_FUNC_GEQUAL,
   M9_FAA_ALPHA_FUNC_LESS,
   M9_FAA_ALPHA_FUNC_GREATER,
};
struct m9_faa_texture_fragment_state {
   enum m9_faa_texture_program program;
   enum m9_faa_texture_alpha alpha_op;
   bool blend, alpha_test;
   /* ADD equation, identical RGB and alpha factors */
   enum m9_faa_blend_factor src, dst;
   enum m9_faa_alpha_func alpha_func;
   uint8_t alpha_ref; /* D3D ALPHAREF */
};
struct m9_faa_texture_fragment_interface {
   uint32_t slots, constant_pairs;
   /* Stage words f0 (fragment mode) and 88 (register usage) that the
    * original switch stage emits together with this program. */
   uint32_t raster_f0, usage;
   bool reads_destination, alpha_test;
};
/* Emits the FS vector and its dense C3 utility pairs. Unqualified states
 * are refused and leave all outputs untouched. */
bool m9_faa_texture_fragment_build(const struct m9_faa_texture_fragment_state *state,
                                   uint32_t *program, uint32_t program_capacity,
                                   uint32_t *constants, uint32_t constant_capacity,
                                   struct m9_faa_texture_fragment_interface *interface);
/* GL reference in [0,1] to the D3D integer reference the source compares
 * the converted 8-bit alpha against. */
bool m9_faa_alpha_ref_from_gl(enum m9_faa_alpha_func func, float ref, uint8_t *out);
#endif
