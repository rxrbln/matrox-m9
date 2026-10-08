/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef __M9X_DRM_FRAGMENT_H__
#define __M9X_DRM_FRAGMENT_H__

/* drm_parhelia_m9_faa_dual_draw.fragment: the textured FAA fragment
 * program of a DRAW beyond its kind. Zero is the FIRST program (D3D
 * SELECTARG1 texture alpha, no blend, no alpha test).
 *
 * MODULATE: GL MODULATE stage alpha (texture * diffuse, stage 1 texture *
 * current). BLEND: ADD with identical RGB/alpha factors SRC (7:4) and
 * DST (11:8). ALPHA_TEST: FUNC (13:12) against the 8-bit REF (23:16).
 * Other bits are zero; unsourced combinations are refused.
 */
#define PARHELIA_M9_FAA_FRAGMENT_MODULATE 0x1u
#define PARHELIA_M9_FAA_FRAGMENT_BLEND 0x2u
#define PARHELIA_M9_FAA_FRAGMENT_ALPHA_TEST 0x4u
#define PARHELIA_M9_FAA_FRAGMENT_SRC(f) ((f) << 4)
#define PARHELIA_M9_FAA_FRAGMENT_DST(f) ((f) << 8)
#define PARHELIA_M9_FAA_FRAGMENT_FUNC(f) ((f) << 12)
#define PARHELIA_M9_FAA_FRAGMENT_REF(r) ((r) << 16)
#define PARHELIA_M9_FAA_FRAGMENT_MASK 0x00ff3ff7u

/* Blend factors. Sourced (src, dst) pairs: ONE/ONE, DST_COLOR/ZERO,
 * ZERO/SRC_COLOR, SRC_ALPHA/INV_SRC_ALPHA, ONE/INV_SRC_ALPHA,
 * SRC_ALPHA/ONE, DST_COLOR/ONE, DST_COLOR/SRC_COLOR, INV_DST_COLOR/ZERO,
 * INV_DST_COLOR/ONE.
 */
#define PARHELIA_M9_FAA_FACTOR_ZERO 0u
#define PARHELIA_M9_FAA_FACTOR_ONE 1u
#define PARHELIA_M9_FAA_FACTOR_SRC_COLOR 2u
#define PARHELIA_M9_FAA_FACTOR_SRC_ALPHA 3u
#define PARHELIA_M9_FAA_FACTOR_INV_SRC_ALPHA 4u
#define PARHELIA_M9_FAA_FACTOR_DST_COLOR 5u
#define PARHELIA_M9_FAA_FACTOR_INV_DST_COLOR 6u

/* Alpha test: pass when alpha FUNC REF (D3D ALPHAREF integer). */
#define PARHELIA_M9_FAA_ALPHA_GEQUAL 0u
#define PARHELIA_M9_FAA_ALPHA_LESS 1u
#define PARHELIA_M9_FAA_ALPHA_GREATER 2u

#endif
