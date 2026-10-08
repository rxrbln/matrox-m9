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
#endif
