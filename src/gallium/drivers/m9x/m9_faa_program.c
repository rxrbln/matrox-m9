/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include "m9_faa_program.h"

#include <stddef.h>
#include <math.h>
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

/* The original 16fa20 record packer joins every field below to the public
 * 24-record position stage. These are instruction forms, not program bytes
 * or captured resource addresses. Source fields b/c/e distinguish registers
 * from constant-pair selectors; unused fields are kept as the packer emits. */
static void
position_slot(uint32_t **out, uint32_t form, unsigned a, unsigned b,
                unsigned c, unsigned d, unsigned cls, unsigned op,
                unsigned e, unsigned sa, unsigned sb, unsigned sc)
{
   *(*out)++ = (a & 7u) << 29 | b << 22 | c << 15 | d << 8 | cls << 5 | op;
   *(*out)++ = form | (a >> 3) | e << 14 | sa << 8 | sb << 6 | sc << 4;
}

bool
m9_faa_preprocess_position(const struct m9_faa_viewport *viewport,
                            uint32_t *output, uint32_t capacity_bytes,
                            float *constants, uint32_t constant_bytes,
                            struct m9_faa_position_interface *interface)
{
   const uint32_t bytes = 25u * 8u, kbytes = 13u * 8u;
   struct m9_faa_position_interface info = {25, 13, UINT64_C(0x200aa0000)};
   if (!viewport || !output || !constants || !interface ||
       !viewport->width || !viewport->height || viewport->x > 15359u ||
       viewport->y > 15359u || viewport->width > 15359u - viewport->x ||
       viewport->height > 15359u - viewport->y ||
       capacity_bytes < bytes || constant_bytes < kbytes ||
       (uintptr_t)output % _Alignof(uint32_t) ||
       (uintptr_t)constants % _Alignof(float) ||
       bytes > UINTPTR_MAX - (uintptr_t)output ||
       kbytes > UINTPTR_MAX - (uintptr_t)constants ||
       overlap(output, bytes, constants, kbytes) ||
       overlap(output, bytes, interface, sizeof(*interface)) ||
       overlap(constants, kbytes, interface, sizeof(*interface)) ||
       overlap(output, bytes, viewport, sizeof(*viewport)) ||
       overlap(constants, kbytes, viewport, sizeof(*viewport)) ||
       overlap(interface, sizeof(*interface), viewport, sizeof(*viewport)))
      return false;

   float x = viewport->x, y = viewport->y;
   float hx = viewport->width * 0.5f, hy = viewport->height * 0.5f;
   /* Actual 134880 field writers and f485b/f4863 dense-span publication.
    * Guard coefficients include the live origin. Fixed pairs are source
    * default-depth/clip coefficients, never inherited heap contents. */
   const float k[26] = {hx, -hy, 1, 1, x + hx, y + hy, 0,
      (float)viewport->height, 0, 2, -16352, 16351, x, y, 1, 0.5f,
      INFINITY, -INFINITY, 1, (15359.0f - x) / hx - 1,
      1, -((15359.0f - y) / hy - 1), 127, 128, 1.25f, 1.5f};
   uint32_t words[50], *p = words;
   const unsigned n = 0x7f;
#define S(form,a,b,c,d,cl,op,e,sa,sb,sc) \
   position_slot(&p, form, a,b,c,d,cl,op,e,sa,sb,sc)
   /* Finiteness, reciprocal seed and two Newton refinements. */
   S(0x90000000, n,2,8,0x18,2,0x1c,5,0,1,1);
   S(0xae203000, 2,n,n,0x0e,0,0x1d,n,2,0,0);
   S(0xa2803f00, n,n,0,0x11,0,0x1c,n,3,0,1);
   S(0x8c202c10, 2,0x0e,n,0x0c,5,0x1c,26,3,0,1);
   S(0xa2803f00, n,n,2,0x13,0,0x1c,n,3,0,1);
   S(0xa2600f00, 0x0e,0x0c,0x0c,0x0e,0,0x1d,4,3,0,0);
   S(0x8c202c10, 2,0x0e,n,5,5,0x1c,26,3,0,1);
   S(0xa2600f00, 0x0e,5,5,5,0,0x1d,4,3,0,0);
   /* Perspective divide, reciprocal and dense viewport/depth constants. */
   S(0xac200000, 2,5,n,4,0,0x1d,n,0,0,0);
   S(0xac200000, 0,5,n,7,0,0x1c,n,1,0,0);
   S(0xa2803f00, n,n,5,0x17,0,0x1e,n,3,0,0);
   S(0xb0400010, n,7,2,8,0,0x1c,0,1,1,1);
   S(0xb0400000, n,4,3,0x17,0,0x1d,1,0,0,0);
   /* Source screen-output conversion and clip-code ALU forms. The packed
    * format/clip arithmetic is not a generic software pixel decoder. */
   S(0xae800fc0, n,n,8,0x15,0,0x1c,n,3,3,1);
   S(0xa2803300, n,n,n,0x21,0,0x1c,7,3,0,0);
   S(0xa0200010, n,2,n,9,0,0x1c,9,1,3,1);
   S(0xa0200010, n,2,n,0x0a,0,0x1c,10,1,3,1);
   S(0x8c000c00, 9,0,0x21,6,0,0x1c,7,1,0,1);
   S(0x8c000c00, 0x0a,0,6,0x0b,1,0x1c,7,1,3,1);
   S(0x90000000, n,0,8,0x0f,2,0x1c,5,0,1,1);
   S(0x8c000c00, 2,2,0x0b,0x0d,3,0x1c,7,3,0,1);
   S(0xada00c00, 0x0f,0x18,0x0f,0x0a,0,0x1d,7,0,0,0);
   S(0xaf400000, 0x0a,0x0d,n,0x19,0,0x1c,12,0,1,3);
   S(0xac800000, 0x19,n,n,0x21,0,0x1c,7,1,3,0);
#undef S
   words[2 * (25 - 4) + 1] |= 0x40000000u;
   words[48] = 0xffff801cu;
   words[49] = 0xa01fc00fu;
   memcpy(output, words, bytes);
   memcpy(constants, k, kbytes);
   *interface = info;
   return true;
}
