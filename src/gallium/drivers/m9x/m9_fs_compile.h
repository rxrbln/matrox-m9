/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * NIR to native M9148 fragment program backend.  Emits only instruction
 * forms captured from the vendor compiler (parhelia-m9-native-glsl-corpus.md)
 * and refuses everything else.
 */
#ifndef M9_FS_COMPILE_H
#define M9_FS_COMPILE_H

#include <stdbool.h>
#include <stdint.h>

#include "util/u_dynarray.h"

struct nir_shader;
struct nir_shader_compiler_options;

/* The vendor splits beyond six interpolated varying slots per raster pass. */
#define M9_FS_MAX_VARYINGS 6u
/* First texel lands in 00/02, the second in 04/06. */
#define M9_FS_MAX_TEXTURES 2u

enum m9_fs_const_kind {
   M9_FS_CONST_LITERAL,
   M9_FS_CONST_UNIFORM,    /* two constant-buffer components or zero */
   M9_FS_CONST_FRAG_Y,     /* (0, drawable height - 1) */
};

#define M9_FS_UNIFORM_ZERO 0xffffffffu

struct m9_fs_const {
   enum m9_fs_const_kind kind;
   float value[2];
   uint32_t offset[2];     /* constant buffer 0 byte offsets */
};

struct m9_fs_varying {
   uint8_t location;       /* gl_varying_slot */
   uint8_t components;     /* component mask read by the program */
};

struct m9_fs_program {
   struct util_dynarray slots;    /* uint32_t W0, W1 */
   struct util_dynarray consts;   /* struct m9_fs_const, fixed tail included */
   struct m9_fs_varying varyings[M9_FS_MAX_VARYINGS];
   unsigned varying_count;
   uint8_t textures[M9_FS_MAX_TEXTURES];  /* sampler index per texel pair */
   uint8_t texcoords[M9_FS_MAX_TEXTURES]; /* coordinate gl_varying_slot */
   uint8_t texcoord_components[M9_FS_MAX_TEXTURES]; /* first coordinate component */
   unsigned texture_count;
   uint16_t registers_used;       /* bit per register pair group 08..1f */
   bool discard, fragcoord, fragdepth;
};

extern const struct nir_shader_compiler_options m9_fs_nir_options;

/* Marks uniforms used by if conditions as inlinable (screen finalize_nir). */
void m9_fs_find_inlinable_uniforms(struct nir_shader *nir);
/* Inlines constant buffer words for the inlinable uniforms and folds the
 * control flow that depended on them. */
void m9_fs_inline_uniforms(struct nir_shader *nir, const uint32_t *values);

/* Returns false with a static reason when the shader is not covered. */
bool m9_fs_compile(struct nir_shader *nir, struct m9_fs_program *program,
                   const char **reason);
void m9_fs_program_fini(struct m9_fs_program *program);
unsigned m9_fs_slot_count(const struct m9_fs_program *program);
uint32_t m9_fs_raster_88(const struct m9_fs_program *program);

#endif
