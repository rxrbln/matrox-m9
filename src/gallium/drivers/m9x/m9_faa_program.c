/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include "m9_faa_program.h"

#include <stddef.h>
#include <string.h>

static bool
overlap(const void *a, uint32_t a_bytes, const void *b, uint32_t b_bytes)
{
   uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
   return x <= y ? y - x < a_bytes : x - y < b_bytes;
}

bool
m9_faa_program_link(const struct m9_faa_program_vector *vectors,
                    uint32_t count, uint32_t *output,
                    uint32_t capacity_bytes,
                    struct m9_faa_program_layout *layout)
{
   struct m9_faa_program_layout result = {0};
   if (!vectors || !output || !layout || (count != 2 && count != 4) ||
       (uintptr_t)output % _Alignof(uint32_t))
      return false;

   for (uint32_t i = 0; i < count; i++) {
      const struct m9_faa_program_vector *v = &vectors[i];
      if (!v->words || (uintptr_t)v->words % _Alignof(uint32_t) ||
          v->slots < 4u || v->slots > UINT32_MAX / 8u)
         return false;
      uint32_t bytes = v->slots * 8u;
      if (bytes > UINT32_MAX - result.bytes)
         return false;
      result.entry_bytes[i] = result.bytes;
      result.bytes += bytes;
   }
   if (result.bytes > capacity_bytes ||
       result.bytes > UINTPTR_MAX - (uintptr_t)output ||
       overlap(output, result.bytes, layout, sizeof(*layout)) ||
       overlap(output, result.bytes, vectors, count * sizeof(*vectors)))
      return false;

   for (uint32_t i = 0; i < count; i++) {
      const struct m9_faa_program_vector *v = &vectors[i];
      uint32_t bytes = v->slots * 8u;
      if (bytes > UINTPTR_MAX - (uintptr_t)v->words ||
          overlap(output, result.bytes, v->words, bytes) ||
          overlap(layout, sizeof(*layout), v->words, bytes) ||
          overlap(layout, sizeof(*layout), vectors, count * sizeof(*vectors)))
         return false;
      /* Actual f40e3 finish mark: fourth slot from the vector's end.
       * Actual 14eb79/14eb7c preprocessing STOP appended before linking. */
      if (!(v->words[2u * (v->slots - 4u) + 1] & 0x40000000u))
         return false;
      uint32_t last = 2u * (v->slots - 1u);
      bool stop = v->words[last] == 0xffff801cu &&
                  v->words[last + 1] == 0xa01fc00fu;
      if (i && !stop)
         return false;
   }

   for (uint32_t i = 0; i < count; i++)
      memcpy((unsigned char *)output + result.entry_bytes[i], vectors[i].words,
             vectors[i].slots * 8u);
   result.count = count;
   *layout = result;
   return true;
}

bool
m9_faa_preprocess_moves(enum m9_faa_move_input kind, const uint8_t *inputs,
                        uint32_t count, uint32_t saturate_targets, uint32_t *output,
                        uint32_t capacity_bytes,
                        struct m9_faa_move_interface *interface)
{
   struct m9_faa_move_interface result = {0};
   uint32_t words[14];
   bool attributes = kind == M9_FAA_MOVE_ATTRIBUTES;
   if ((!attributes && kind != M9_FAA_MOVE_RECORDS) || !inputs || !count ||
       count > (attributes ? 3u : 2u) ||
       (saturate_targets & ~((1u << count) - 1u)) || !output || !interface ||
       (uintptr_t)output % _Alignof(uint32_t))
      return false;
   result.slots = 2u * count + 1u;
   if (result.slots < 4)
      result.slots = 4;
   uint32_t bytes = result.slots * 8u;
   if (bytes > capacity_bytes || bytes > UINTPTR_MAX - (uintptr_t)output ||
       overlap(output, bytes, interface, sizeof(*interface)))
      return false;

   for (uint32_t i = 0; i < count; i++) {
      if (inputs[i] >= (attributes ? 3u : 2u))
         return false;
      uint32_t source = attributes ? 0x46u + 8u * inputs[i] : 4u * inputs[i];
      uint32_t target = 0x11u + 4u * i;
      for (uint32_t half = 0; half < 2; half++) {
         uint32_t src = source + (attributes ? half : 2u * half);
         uint32_t dst = target + 2u * half;
         uint32_t slot = attributes ? 2u * count - 1u - (2u * i + half) :
                                      2u * i + half;
         /* Source final MOV pair: no ALU/K reads; pair-preserving C swizzle1.
          * Carry has unused E=0, gather has unused E=7f, as decoded by the
          * original expanded-record frontend. Both constant counts are zero. */
         words[2u * slot] = 0xffc00000u | src << 15 | dst << 8 | 0x1cu |
                                  ((saturate_targets >> i & 1u) << 7);
         words[2u * slot + 1] = attributes ? 0xa2803f1fu : 0xa29fff1fu;
         result.output_registers |= UINT64_C(1) << dst;
      }
      result.output_targets |= 1u << i;
   }
   for (uint32_t i = 2u * count; i < result.slots; i++) {
      words[2u * i] = 0xffff801cu;
      words[2u * i + 1] = 0xa01fc00fu;
   }
   words[2u * (result.slots - 4u) + 1] |= 0x40000000u;
   memcpy(output, words, bytes);
   *interface = result;
   return true;
}
