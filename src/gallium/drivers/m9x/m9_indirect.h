/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_INDIRECT_H
#define M9_INDIRECT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Gallium's indexed command has five dwords; a non-indexed one has four.
 * All bounds are checked before mapping a live argument BO. */
struct m9_indirect_args {
   uint32_t count;
   uint32_t instance_count;
   uint32_t start;
   int32_t index_bias;
   uint32_t start_instance;
};

static inline bool
m9_indirect_span(uint64_t buffer_bytes, uint32_t offset, uint32_t stride,
                 uint32_t draw_count, bool indexed, uint32_t *span)
{
   uint32_t command_bytes = indexed ? 20 : 16;
   uint64_t needed;

   if (!span || (offset & 3) || (stride & 3))
      return false;
   if (!stride)
      stride = command_bytes;
   if (stride < command_bytes)
      return false;
   if (!draw_count) {
      *span = 0;
      return offset <= buffer_bytes;
   }
   needed = (uint64_t)(draw_count - 1) * stride + command_bytes;
   if (offset > buffer_bytes || needed > buffer_bytes - offset ||
       needed > UINT32_MAX)
      return false;
   *span = (uint32_t)needed;
   return true;
}

static inline void
m9_indirect_decode(const uint8_t *bytes, bool indexed,
                   struct m9_indirect_args *out)
{
   uint32_t bias;

   memcpy(&out->count, bytes, 4);
   memcpy(&out->instance_count, bytes + 4, 4);
   memcpy(&out->start, bytes + 8, 4);
   memcpy(&bias, bytes + 12, 4);
   out->index_bias = indexed ? (int32_t)bias : 0;
   if (indexed)
      memcpy(&out->start_instance, bytes + 16, 4);
   else
      out->start_instance = bias;
}

#endif
