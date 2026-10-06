/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Specialising a shader on the uniform values a draw is submitted with.
 *
 * Neither native backend has control flow, so a branch or a loop only
 * reaches the hardware when the value that decides it is known at draw
 * time.  Both stages share these passes: the screen declares no integer
 * support, so an integer uniform arrives as a float and the NIR has to be
 * lowered to match before a loop bound reads as the number it is.
 */
#ifndef M9_SPECIALISE_H
#define M9_SPECIALISE_H

#include <stdint.h>

struct nir_shader;

/* Bring a shader whose options declare no_integers to the float-only form
 * the screen advertises. */
void m9_nir_lower_integers(struct nir_shader *nir);

/* Mark the constant-buffer dwords that decide control flow or a select,
 * so the state tracker sends their values through set_inlinable_constants. */
void m9_nir_find_inlinable_uniforms(struct nir_shader *nir);

/* Substitute those values and fold, which unrolls a uniform-bounded loop
 * and removes a uniform-decided branch or select. */
void m9_nir_inline_uniforms(struct nir_shader *nir, const uint32_t *values);

#endif
