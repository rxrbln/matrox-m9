/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_PROGRAM_H
#define M9_FAA_PROGRAM_H

#include <stdbool.h>
#include <stdint.h>

/* Final native vectors, after allocation/scheduling. Vector zero is the
 * fragment program; the remaining vectors are preprocessing entries, each
 * including its STOP. Short fragment vectors include the compiler's STOP
 * padding to four slots. These are live compiler outputs, never capture blobs.
 * This interface does not qualify ordinary VS programs as FAA preprocessors. */
struct m9_faa_program_vector {
   const uint32_t *words;
   uint32_t slots;
};

struct m9_faa_program_layout {
   uint32_t bytes;
   uint32_t count;
   uint32_t entry_bytes[4];
};

/* Two and four vectors are the source-qualified diffuse/depth and textured
 * shapes. All validation precedes writes; output must not overlap any input.
 * Inputs must remain owned through this synchronous call. No marker, opcode,
 * register or scheduling rewrite is performed here.
 * Successful linking is not a graph INIT receipt or a shader admission. */
bool m9_faa_program_link(const struct m9_faa_program_vector *vectors,
                         uint32_t count, uint32_t *output,
                         uint32_t capacity_bytes,
                         struct m9_faa_program_layout *layout);

/* Original M9128 ps_2_0 `dcl v0; mov oC0,v0` frontend. The one-color and
 * unused-TEXCOORD controls, plus a changed position-ALU control, produce the
 * same eight fragment slots. The live COLOR0 linkage must occupy stream 0;
 * other fragment inputs, texture/depth state and output routes are not
 * admitted here. Utility constants are two live-owned pairs at FS C3 [0,16).
 * This encodes ISA fields, not a captured program allocation or address. */
struct m9_faa_color_fs_interface {
   uint32_t slots;
   uint32_t constant_pairs;
   uint32_t source_stream;
   uint32_t result_register;
};
bool m9_faa_color_fs_build(uint32_t source_stream,
                           uint32_t *program, uint32_t program_capacity_bytes,
                           uint32_t *constants, uint32_t constant_capacity_bytes,
                           struct m9_faa_color_fs_interface *interface);

/* Source-qualified flat COLOR0: four slots with independent utility order.
 * Requires matching native flat raster/fragment metadata; successful shader
 * construction alone grants no native state admission. */
bool m9_faa_color_flat_fs_build(uint32_t source_stream,
                              uint32_t *program, uint32_t program_capacity_bytes,
                              uint32_t *constants, uint32_t constant_capacity_bytes,
                              struct m9_faa_color_fs_interface *interface);

/* Original M9128 stage-0 texture + smooth COLOR0 fixed-function combine.
 * The sampled texel occupies the source's first texel pair; COLOR0 occupies
 * interpolation stream zero. This builds only the six-slot fragment vector.
 * A caller still needs a live UV stream, sampler/resource state and the
 * texture-specific raster command grammar before any native draw. */
enum m9_faa_texture_combine {
   M9_FAA_TEXTURE_MODULATE = 4,
   M9_FAA_TEXTURE_ADD = 7,
};
struct m9_faa_texture_fs_interface {
   uint32_t slots, source_stream, texel_unit, result_register;
};
bool m9_faa_texture_fs_build(enum m9_faa_texture_combine combine,
                             bool swapped_arguments,
                             uint32_t source_stream, uint32_t texel_unit,
                             uint32_t *program, uint32_t program_capacity_bytes,
                             struct m9_faa_texture_fs_interface *interface);

/* Original M9128 COLOR0 * texture0 * texture1 frontend. This is the
 * nine-slot fragment vector for exactly two MODULATE stages, with smooth
 * COLOR0 in raster stream 0 and sampler units 0/1. Texture ownership,
 * coordinates, sampling state, and command packet binding are separate. */
struct m9_faa_dual_texture_fs_interface {
   uint32_t slots, color_stream, texel0_unit, texel1_unit, result_register;
};
bool m9_faa_dual_texture_fs_build(uint32_t color_stream,
                                  uint32_t texel0_unit,
                                  uint32_t texel1_unit,
                                  uint32_t *program,
                                  uint32_t program_capacity_bytes,
                                  struct m9_faa_dual_texture_fs_interface *interface);

enum m9_faa_move_input {
   M9_FAA_MOVE_ATTRIBUTES,
   M9_FAA_MOVE_RECORDS,
};

struct m9_faa_move_interface {
   uint32_t slots;
   uint32_t constant_pairs;
   uint32_t output_targets;
   /* Compiler liveness seed for the two native registers of every target.
    * This is compiler metadata, not a hardware register write. */
   uint64_t output_registers;
};

/* Emit the source-qualified pair moves used by carry and gather stages.
 * inputs[i] is a live attribute/record index, in the required output order;
 * caller declarations and retained record bindings must provide those inputs.
 * The attribute slice covers three pairs, the record slice covers two.
 * saturate_targets explicitly selects pair clamp modifiers from live state;
 * no diffuse clamp is assumed for arbitrary Gallium varyings.
 * This does not compile position/clip/depth or admit an app FAA draw. */
bool m9_faa_preprocess_moves(enum m9_faa_move_input kind,
                            const uint8_t *inputs, uint32_t count,
                            uint32_t saturate_targets, uint32_t *output,
                            uint32_t capacity_bytes,
                            struct m9_faa_move_interface *interface);

struct m9_faa_viewport {
   uint32_t x, y, width, height;
};

struct m9_faa_position_interface {
   uint32_t slots, constant_pairs;
   uint64_t output_registers;
};

/* Original public clip-space position stage: retained route0 arrives in
 * r00/r02; two complete outputs carry clip and screen position, and r21
 * carries the source clip code. Inputs use D3D clip z in [0,w]. The separate
 * live FAA GL compiler adapter performs (z+w)/2 on the GPU first.
 * Default depth range only; no point/fog/user-clip/AA policy is implied.
 * Emit 25 instruction slots and 13 dense constant pairs into disjoint owned
 * outputs. All validation precedes writes. This does not bind a graph/job. */
bool m9_faa_preprocess_position(const struct m9_faa_viewport *viewport,
                                uint32_t *output, uint32_t capacity_bytes,
                                float *constants, uint32_t constant_bytes,
                                struct m9_faa_position_interface *interface);

/* Address-free upload preparation for the public VS four-vector shape.
 * Each dense constant span is supplied by its live compiler/runtime, in
 * vector order. Utility pairs precede FS/carry/gather/position constants.
 * Two FLOAT4 input attributes use one 32-byte interleaved record. This
 * synchronous builder does not issue packets or admit a generic FAA draw. */
struct m9_faa_dense_constants {
   const uint32_t *words;
   uint32_t pairs;
};
struct m9_faa_preprocess_upload {
   struct m9_faa_program_vector vectors[4];
   struct m9_faa_dense_constants constants[4];
   struct m9_faa_viewport viewport;
   const uint32_t *vertices; /* Three raw POSITION0 + TEXCOORD0 records. */
   const uint32_t *declaration; /* m9_vs_decl_words: two FLOAT4 attributes. */
};
struct m9_faa_preprocess_upload_layout {
   struct m9_faa_program_layout program;
   uint32_t constants_bytes;
   uint32_t constant_begin[4], constant_end[4];
   uint32_t geometry_bytes, vertex_offset, vertex_bytes;
   uint32_t declaration[5];
};
/* Owned snapshot for the source-qualified public COPY profile. It contains
 * no pointers or graph tokens; producers must derive its words from live
 * shader/state/vertices. Wider programs use their own qualified contracts. */
struct m9_faa_public_upload {
   struct m9_faa_preprocess_upload_layout layout;
   uint32_t program[82], constants[38], geometry[56];
};
bool m9_faa_preprocess_upload_build(
   const struct m9_faa_preprocess_upload *input,
   uint32_t *program, uint32_t program_capacity,
   uint32_t *constants, uint32_t constant_capacity,
   uint32_t *geometry, uint32_t geometry_capacity,
   struct m9_faa_preprocess_upload_layout *layout);

/* Source 248f2 deferred field, restricted to forward fields in the same
 * retained owner. No CPU/GPU base participates; high12 opcode bits survive.
 * Caller performs the actual write only after its owner/lifetime checks. */
bool m9_faa_relative_slot_word(uint32_t prior, uint32_t slot_offset,
                              uint32_t target_offset, uint32_t owner_bytes,
                              uint32_t *word);

#endif
