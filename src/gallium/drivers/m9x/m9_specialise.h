/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Specialising a shader on the uniform values a draw is submitted with.
 *
 * Neither native backend emits hardware branches.  Uniform specialization
 * removes selected control flow; bounded varying arithmetic becomes GPU
 * selects.  Both stages share these passes: the screen declares no integer
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

/* Mark the constant-buffer dwords of uniform conditions, including boolean
 * operands of varying guards, so set_inlinable_constants sends their values. */
void m9_nir_find_inlinable_uniforms(struct nir_shader *nir);

/* Substitute those values and fold, which unrolls a uniform-bounded loop
 * and removes a uniform-decided branch or select. */
void m9_nir_inline_uniforms(struct nir_shader *nir, const uint32_t *values);

/* Run the same NIR folding after another exact, live specialization. */
void m9_nir_fold_specialization(struct nir_shader *nir);

/* Execute both pure arithmetic arms and select their SSA results on the GPU.
 * Bounded branches only; loops, texture fetches, indirect loads and side
 * effects keep their control flow and the backend's ordinary refusal. */
void m9_nir_lower_alu_if(struct nir_shader *nir);

#endif
