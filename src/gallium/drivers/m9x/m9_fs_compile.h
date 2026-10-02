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

/* The interpolation register file stops at six; M9_RASTER_MAX_INTERP in
 * m9_draw_params.h carries the measurement. */
#define M9_FS_MAX_VARYINGS 6u
/* First texel lands in 00/02, the second in 04/06.  A unit is one fetch: the
 * engine samples at the coordinate it interpolates and the program has no
 * fetch instruction (CAPTURED, parhelia-m9-native-glsl-corpus.md section 6),
 * so repeated taps of one sampler at different coordinates take a unit each
 * (DERIVED from that, and matching the vendor's own indep_2..indep_8 pass
 * counts).  Only fetches that agree in sampler and coordinate share.  Past
 * four, the shader has to be split across raster passes; the contract is in
 * docs/parhelia-m9-fragment-pass-splitting.md. */
#define M9_FS_MAX_TEXTURES 4u
/* Raster passes one shader may be split across.  A pass binds four units,
 * the first of which a continuation spends on the record it reads, so eight
 * cover every corpus shader the backend otherwise supports -- but a shader
 * whose coordinates the engine cannot interpolate costs one producing pass
 * a fetch, and glmark2's nine-tap tilt-shift blur needs fourteen.  MEASURED
 * on the card: a fourteen-pass job builds, submits and draws with no stream
 * timeout.  Its *pixels* are another matter -- see the coordconv:4 fixture
 * and docs/parhelia-m9-fragment-pass-splitting.md section 8. */
#define M9_FS_MAX_PASSES 16u
/* Colour targets one fragment program may write.  Target p renders into the
 * surface bound at raster b0 + 2p and owns one native destination of its
 * own; docs/parhelia-m9-mrt-routing.md carries the measurement. */
#define M9_FS_MAX_TARGETS 4u
/* A texel slot fed by a sampler rather than by an inter-pass record. */
#define M9_FS_ROUTE_NONE 0xffu
/* Compiled variants one shader may keep.  A shader specialised on a uniform
 * -- a loop bound, an if condition -- can be asked for one per value, so the
 * cache is capped and a further one is refused rather than compiled. */
#define M9_FS_MAX_VARIANTS 64u

enum m9_fs_const_kind {
   M9_FS_CONST_LITERAL,
   M9_FS_CONST_UNIFORM,    /* two constant-buffer components or zero */
   M9_FS_CONST_FRAG_Y,     /* (0, drawable height - 1) */
   /* Alpha-test reference, both lanes scaled by 256 (MEASURED with the
    * vendor kill program: the reference is constant pair 0). */
   M9_FS_CONST_ALPHA_REF,
   /* Blend colour channel offset[lane], or one minus it; resolved at
    * draw time so a colour change needs no recompile. */
   M9_FS_CONST_BLEND_COLOR,
   M9_FS_CONST_BLEND_COLOR_INV,
};

#define M9_FS_UNIFORM_ZERO 0xffffffffu

/* One coefficient of a coordinate transform: a constant, optionally plus a
 * component of the fragment constant buffer, or its reciprocal, scaled by
 * another constant.  That covers a literal, a uniform, a uniform scaled by a
 * literal, and one over a uniform -- the step a shader that walks its
 * neighbours writes as 1.0 / resolution.x. */
struct m9_fs_coord_term {
   float literal;
   float factor;
   uint32_t offset;        /* M9_FS_UNIFORM_ZERO when there is no uniform */
   bool reciprocal;        /* the uniform enters as 1 / value */
};

/* A texture coordinate that is an affine function of an interpolated pair:
 * coordinate = varying * scale + bias.  Interpolation is linear, so folding
 * this into the varying the raster engine interpolates samples at exactly
 * the coordinate the shader asked for, which is what lets a unit sample at
 * a computed coordinate even though the engine cannot.
 */
/* The third component is the comparison reference of a shadow fetch. */
struct m9_fs_coord_affine {
   bool present;
   struct m9_fs_coord_term scale[3], bias[3];
};

struct m9_fs_const {
   enum m9_fs_const_kind kind;
   float value[2];
   uint32_t offset[2];     /* constant buffer 0 byte offsets */
};

struct m9_fs_varying {
   uint8_t location;       /* gl_varying_slot */
   uint8_t components;     /* component mask read by the program */
};

/* Blend factors lowered into the program.  The classic forms are the
 * vendor's (captures/m9x-vendor-glamor-blend-20260915); everything else is
 * built from the multiply, multiply-add, minimum and move forms. */
enum m9_fs_factor {
   M9_FS_ZERO, M9_FS_ONE, M9_FS_SRC_ALPHA, M9_FS_INV_SRC_ALPHA,
   M9_FS_DST_ALPHA, M9_FS_INV_DST_ALPHA, M9_FS_SRC_COLOR, M9_FS_INV_SRC_COLOR,
   M9_FS_DST_COLOR, M9_FS_INV_DST_COLOR,
   M9_FS_CONST_COLOR, M9_FS_INV_CONST_COLOR,
   M9_FS_CONST_ALPHA, M9_FS_INV_CONST_ALPHA,
   M9_FS_SRC_ALPHA_SATURATE,
};

enum m9_fs_blend_func {
   M9_FS_FUNC_ADD, M9_FS_FUNC_SUBTRACT, M9_FS_FUNC_REVERSE_SUBTRACT,
   M9_FS_FUNC_MIN, M9_FS_FUNC_MAX,
};

struct m9_fs_blend {
   uint8_t src, dst;              /* enum m9_fs_factor, R,G,B (and A) */
   /* The colour target stores sRGB, so the program encodes what it writes.
    * Sampler-side decode is not implemented, so a blended sRGB draw -- which
    * would have to decode the destination -- is refused. */
   bool srgb;
   uint8_t func;                  /* enum m9_fs_blend_func */
   /* A differs from R,G,B: alpha_src/alpha_dst/alpha_func apply to it. */
   bool separate;
   uint8_t alpha_src, alpha_dst, alpha_func;
};

bool m9_fs_blend_same(const struct m9_fs_blend *a, const struct m9_fs_blend *b);

struct m9_fs_program {
   struct util_dynarray slots;    /* uint32_t W0, W1 */
   struct util_dynarray consts;   /* struct m9_fs_const, fixed tail included */
   struct m9_fs_varying varyings[M9_FS_MAX_VARYINGS];
   unsigned varying_count;
   uint8_t textures[M9_FS_MAX_TEXTURES];  /* sampler index per texel pair */
   uint8_t texcoords[M9_FS_MAX_TEXTURES]; /* coordinate gl_varying_slot */
   uint8_t texcoord_components[M9_FS_MAX_TEXTURES]; /* first coordinate component */
   /* xy divided by w of the same varying; the driver divides per vertex */
   bool texcoord_projective[M9_FS_MAX_TEXTURES];
   /* The unit compares against a third interpolated component, which the
    * sampler word selects (docs/parhelia-m9-openbits-gl-capture.md). */
   bool texcoord_shadow[M9_FS_MAX_TEXTURES];
   /* A cube or volume fetch reads a third interpolated component as the
    * r coordinate; the unit's form word selects the storage family. */
   bool texcoord_volume[M9_FS_MAX_TEXTURES];
   struct m9_fs_coord_affine texcoord_affine[M9_FS_MAX_TEXTURES];
   unsigned texture_count;
   uint16_t registers_used;       /* bit per register pair group 08..1f */
   bool discard, fragcoord, fragdepth;
   bool blend_dest;               /* raster f0 c5008200: destination blend */
   /* A texel slot fed by the record a previous raster pass wrote: the index
    * of the pass that produced it, M9_FS_ROUTE_NONE for an ordinary fetch.
    * A routed value arrives in the register pair a texel does, so the rest
    * of the program treats it as one.
    * docs/parhelia-m9-fragment-pass-routes.md */
   uint8_t texture_source[M9_FS_MAX_TEXTURES];
   /* A texel slot whose *coordinate* comes from the record rather than from
    * an interpolated varying: the index of the pass that produced it, or
    * M9_FS_ROUTE_NONE.  The unit keeps its own texture and sampler; only
    * where it samples changes, which is how a coordinate the fragment stage
    * computes reaches a fetch the engine issues before the program runs
    * (CAPTURED, docs/parhelia-m9-fragment-pass-routes.md). */
   uint8_t texcoord_source[M9_FS_MAX_TEXTURES];
   /* The program hands its result to a later pass through registers
    * 0x11/0x13 instead of writing the colour output 0x2d. */
   bool writes_record;
   /* Scalars it hands on, 1..4; the record slot rounds up to a pair. */
   unsigned record_components;
   /* Colour targets the program writes: one bit a target and the highest
    * written one plus one.  A bound target the shader leaves alone keeps its
    * bit clear and the driver masks its writes off. */
   uint8_t target_mask;
   unsigned target_count;
};

/* One fragment shader as the raster passes the engine runs it in.  Pass i
 * hands its result to every later pass through record slot i. */
struct m9_fs_passes {
   struct m9_fs_program pass[M9_FS_MAX_PASSES];
   unsigned count;
};

extern const struct nir_shader_compiler_options m9_fs_nir_options;

/* Two coordinate transforms that fold to the same per-vertex stream. */
bool m9_fs_coord_affine_same(const struct m9_fs_coord_affine *a,
                             const struct m9_fs_coord_affine *b);

/* Rewrites the colour output with the vendor's sRGB transfer curve.  Exposed
 * so the offline test can check the lowering against a reference encode. */
void m9_fs_lower_srgb(struct nir_shader *nir);

/* Writes gl_FragColor into every one of this many colour targets, which is
 * what GL does with a shader that names only the one output. */
void m9_fs_lower_fragcolor(struct nir_shader *nir, unsigned targets);

/* Returns false with a static reason when the shader is not covered.  A NULL
 * blend, or ONE/ZERO, writes the colour unblended. */
bool m9_fs_compile(struct nir_shader *nir, const struct m9_fs_blend *blend,
                   struct m9_fs_program *program, const char **reason);
/* The same, splitting a shader the engine cannot run in one raster pass
 * across several of them.  A one-pass result is byte for byte what
 * m9_fs_compile returns. */
bool m9_fs_compile_passes(struct nir_shader *nir,
                          const struct m9_fs_blend *blend,
                          struct m9_fs_passes *passes, const char **reason);
/* Texel slots the pass samples at a coordinate an earlier pass produced. */
unsigned m9_fs_coord_route_count(const struct m9_fs_program *program);
void m9_fs_passes_fini(struct m9_fs_passes *passes);
/* Texel slots the pass fills from a sampler rather than from a record. */
unsigned m9_fs_sampler_count(const struct m9_fs_program *program);
/* Coordinate streams the pass interpolates; units that agree in location,
 * component, projector and transform share one. */
unsigned m9_fs_coord_stream_count(const struct m9_fs_program *program);
void m9_fs_program_fini(struct m9_fs_program *program);
unsigned m9_fs_slot_count(const struct m9_fs_program *program);
uint32_t m9_fs_raster_88(const struct m9_fs_program *program);

#endif
