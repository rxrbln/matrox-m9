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

#endif
