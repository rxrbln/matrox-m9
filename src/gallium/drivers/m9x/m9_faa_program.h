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

#endif
