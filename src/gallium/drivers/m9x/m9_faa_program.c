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

/* Source `16fa20` emits these eight M9128 forms for a single COLOR0 input.
 * The last pair reads raster stream 0's 0x40/42 and 0x44/45 halves and writes
 * r2d, the source oC0 route. Encoding the register fields keeps the source
 * instruction contract visible and leaves all program/constant addresses to
 * the retained owner binder. There is no shader or vertex byte captured here. */
struct m9_faa_color_fs_form {
   uint8_t op, cls, dst, a, b, c;
   uint32_t control;
};

static const struct m9_faa_color_fs_form m9_faa_color_fs_forms[8] = {
   {0x1c, 0, 0x1d, 0x7f, 0x7f, 0x7a, 0xa28fcf10},
   {0x1c, 0, 0x01, 0x79, 0x7f, 0x7f, 0x8c020d10},
   {0x1c, 0, 0x02, 0x1d, 0x7f, 0x7f, 0xac8001f0},
   {0x1c, 0, 0x1c, 0x79, 0x7f, 0x7f, 0x8c024d10},
   {0x1c, 0, 0x03, 0x01, 0x02, 0x79, 0xec4fcd10},
   {0x1c, 4, 0x1b, 0x1c, 0x02, 0x03, 0xac4fcdd0},
   {0x1e, 4, 0x2d, 0x42, 0x1b, 0x45, 0xae0ffc10},
   {0x1d, 4, 0x2d, 0x40, 0x1b, 0x44, 0xae0ffc10},
};

/* Original FIRST flat COLOR0 frontend bypasses interpolation and writes
 * raster stream0's two flat halves directly to oC0. Finish belongs four
 * slots from the end, so the two output instructions include two STOP pads.
 * These are ISA forms, independent of program/constant owner addresses. */
static const struct m9_faa_color_fs_form m9_faa_color_flat_forms[4] = {
   {0x1e, 4, 0x2d, 0x7f, 0x7f, 0x45, 0xe29fff10},
   {0x1d, 4, 0x2d, 0x7f, 0x7f, 0x44, 0xa29fff10},
   {0x1c, 0, 0x00, 0x7f, 0x7f, 0x7f, 0xa01fc000},
   {0x1c, 0, 0x00, 0x7f, 0x7f, 0x7f, 0xa01fc000},
};

static bool
m9_faa_color_fs_build_mode(uint32_t source_stream, bool flat,
                       uint32_t *program, uint32_t program_capacity_bytes,
                       uint32_t *constants, uint32_t constant_capacity_bytes,
                       struct m9_faa_color_fs_interface *interface)
{
   static const uint32_t utility[4] = {
      0x3f800000u, 0x3f000000u, 0u, 0x3f800000u,
   };
   static const uint32_t flat_utility[4] = {0u, 0x3f800000u, 0x3f800000u, 0x3f000000u};
   const uint32_t slots = flat ? 4 : 8;
   const uint32_t bytes = slots * 8;
   const struct m9_faa_color_fs_form *forms = flat ? m9_faa_color_flat_forms : m9_faa_color_fs_forms;
   struct m9_faa_color_fs_interface result = {slots, 2, 0, 0x2d};
   if (source_stream != 0 || !program || !constants || !interface ||
       (uintptr_t)program % _Alignof(uint32_t) ||
       (uintptr_t)constants % _Alignof(uint32_t) ||
       program_capacity_bytes < bytes || constant_capacity_bytes < 16 ||
       bytes > UINTPTR_MAX - (uintptr_t)program ||
       16 > UINTPTR_MAX - (uintptr_t)constants ||
       overlap(program, bytes, constants, 16) ||
       overlap(program, bytes, interface, sizeof(*interface)) ||
       overlap(constants, 16, interface, sizeof(*interface)))
      return false;
   for (uint32_t i = 0; i < slots; i++) {
      const struct m9_faa_color_fs_form *s = &forms[i];
      program[2 * i] = ((uint32_t)(s->a & 7u) << 29) |
         ((uint32_t)s->b << 22) | ((uint32_t)s->c << 15) |
         ((uint32_t)s->dst << 8) | ((uint32_t)s->cls << 5) | s->op;
      program[2 * i + 1] = s->control | s->a >> 3;
   }
   memcpy(constants, flat ? flat_utility : utility, sizeof(utility));
   *interface = result;
   return true;
}

bool
m9_faa_color_fs_build(uint32_t source_stream,
                       uint32_t *program, uint32_t program_capacity_bytes,
                       uint32_t *constants, uint32_t constant_capacity_bytes,
                       struct m9_faa_color_fs_interface *interface)
{
   return m9_faa_color_fs_build_mode(source_stream, false, program, program_capacity_bytes,
                                    constants, constant_capacity_bytes, interface);
}

bool
m9_faa_color_flat_fs_build(uint32_t source_stream,
                            uint32_t *program, uint32_t program_capacity_bytes,
                            uint32_t *constants, uint32_t constant_capacity_bytes,
                            struct m9_faa_color_fs_interface *interface)
{
   return m9_faa_color_fs_build_mode(source_stream, true, program, program_capacity_bytes,
                                    constants, constant_capacity_bytes, interface);
}

/* f40e3 finishes the six-slot fragment vector at slot two. 16fa20 emits
 * the two COLOR0 interpolants, combines each half with the stage-0 texel,
 * and routes the result to oC0. 181fa0's texture/depth state is separate;
 * these are ISA register fields, with no captured resource address. */
bool
m9_faa_texture_fs_build(enum m9_faa_texture_combine combine,
                        bool swapped_arguments,
                        uint32_t source_stream, uint32_t texel_unit,
                        uint32_t *program, uint32_t program_capacity_bytes,
                        struct m9_faa_texture_fs_interface *interface)
{
   struct m9_faa_color_fs_form forms[6] = {
      {0x1d, 4, 0x06, 0x42, 0x79, 0x45, 0xae003c00},
      {0x1c, 4, 0x1a, 0x40, 0x79, 0x44, 0xae003c10},
      {0x1d, 4, 0x08, 0x02, 0x06, 0x7f, 0xec200000},
      {0x1e, 4, 0x08, 0x3f, 0x7f, 0x02, 0xa2803f38},
      {0x1d, 4, 0x2d, 0x00, 0x1a, 0x7f, 0xac200140},
      {0x1e, 0, 0x2d, 0x3f, 0x7f, 0x08, 0xa29fff18},
   };
   struct m9_faa_texture_fs_interface result = {6, 0, 0, 0x2d};
   const uint32_t bytes = 6 * 8;
   if ((combine != M9_FAA_TEXTURE_MODULATE && combine != M9_FAA_TEXTURE_ADD) ||
       (swapped_arguments && combine != M9_FAA_TEXTURE_ADD) ||
       source_stream || texel_unit || !program || !interface ||
       (uintptr_t)program % _Alignof(uint32_t) ||
       program_capacity_bytes < bytes ||
       bytes > UINTPTR_MAX - (uintptr_t)program ||
       overlap(program, bytes, interface, sizeof(*interface)))
      return false;
   if (combine == M9_FAA_TEXTURE_ADD) {
      forms[2].b = forms[4].b = 0x7f;
      forms[2].c = swapped_arguments ? 0x02 : 0x06;
      forms[4].c = swapped_arguments ? 0x00 : 0x1a;
      forms[2].a = swapped_arguments ? 0x06 : 0x02;
      forms[4].a = swapped_arguments ? 0x1a : 0x00;
      forms[2].control = 0xec800c00;
      forms[4].control = 0xac800d10;
   }
   for (uint32_t i = 0; i < result.slots; i++) {
      const struct m9_faa_color_fs_form *s = &forms[i];
      program[2 * i] = ((uint32_t)(s->a & 7u) << 29) |
         ((uint32_t)s->b << 22) | ((uint32_t)s->c << 15) |
         ((uint32_t)s->dst << 8) | ((uint32_t)s->cls << 5) | s->op;
      program[2 * i + 1] = s->control | s->a >> 3;
   }
   *interface = result;
   return true;
}

/* The original M9128 two-stage MODULATE shader composes the first sampled
 * texel with smooth COLOR0, then combines the second sampled texel. The ISA
 * register form is stable across the source's 64x64 and 32x32 second-stage
 * texture controls; dimensions reside in the separate sampler descriptor.
 * These are decoded instruction forms, not a captured program allocation. */
bool
m9_faa_dual_texture_fs_build(uint32_t color_stream, uint32_t texel0_unit,
                             uint32_t texel1_unit, uint32_t *program,
                             uint32_t program_capacity_bytes,
                             struct m9_faa_dual_texture_fs_interface *interface)
{
   static const struct m9_faa_color_fs_form forms[9] = {
      {0x1d, 4, 0x0c, 0x42, 0x79, 0x45, 0xae0ffc00},
      {0x1c, 4, 0x0b, 0xc0, 0x79, 0x44, 0xae0ffc00},
      {0x1d, 4, 0x08, 0x02, 0x0c, 0x7f, 0xac2fc000},
      {0x1c, 0, 0x09, 0xff, 0x7f, 0x02, 0xa28fff00},
      {0x1d, 4, 0x0a, 0x06, 0x08, 0x7f, 0xac2fc000},
      {0x1e, 4, 0x09, 0xff, 0x7f, 0x02, 0xe28fff20},
      {0x1c, 4, 0x0e, 0x00, 0x0b, 0x7f, 0xac2fc140},
      {0x1e, 4, 0x2d, 0x7f, 0x09, 0x0a, 0xa3400ec0},
      {0x1d, 4, 0x2d, 0x04, 0x0e, 0x7f, 0xac2fc140},
   };
   struct m9_faa_dual_texture_fs_interface result = {9, 0, 0, 1, 0x2d};
   const uint32_t bytes = 9u * 8u;
   if (color_stream != 0 || texel0_unit != 0 || texel1_unit != 1 ||
       !program || !interface ||
       (uintptr_t)program % _Alignof(uint32_t) ||
       program_capacity_bytes < bytes ||
       bytes > UINTPTR_MAX - (uintptr_t)program ||
       overlap(program, bytes, interface, sizeof(*interface)))
      return false;
   for (uint32_t i = 0; i < result.slots; i++) {
      const struct m9_faa_color_fs_form *s = &forms[i];
      program[2 * i] = ((uint32_t)(s->a & 7u) << 29) |
         ((uint32_t)s->b << 22) | ((uint32_t)s->c << 15) |
         ((uint32_t)s->dst << 8) | ((uint32_t)s->cls << 5) | s->op;
      program[2 * i + 1] = s->control | s->a >> 3;
   }
   *interface = result;
   return true;
}

/* Original public SELECTARG2 on both bound stages keeps the dual draw's
 * COLOR/UV0/UV1 preprocessing but selects the smooth COLOR0 input in the
 * fragment frontend. 16fa20 emits these four decoded register operations:
 * two stream-0 interpolants followed by the two halves of oC0. The caller
 * derives live code and C3 addresses; no captured allocation is encoded. */
bool
m9_faa_dual_colorpass_fs_build(uint32_t color_stream,
                              uint32_t *program, uint32_t program_capacity_bytes,
                              uint32_t *constants, uint32_t constant_capacity_bytes,
                              struct m9_faa_color_fs_interface *interface)
{
   static const struct m9_faa_color_fs_form forms[4] = {
      {0x1e, 4, 0x04, 0xff, 0x7f, 0x02, 0xe2803f20},
      {0x1d, 4, 0x04, 0x42, 0x79, 0x45, 0xae003c00},
      {0x1d, 4, 0x2d, 0xc0, 0x79, 0x44, 0xae003c00},
      {0x1e, 4, 0x2d, 0xff, 0x7f, 0x04, 0xa2803f00},
   };
   static const uint32_t utility[4] = {
      0u, 0x3f800000u, 0x3f800000u, 0x3f000000u,
   };
   const uint32_t bytes = 4 * 8;
   struct m9_faa_color_fs_interface result = {4, 2, 0, 0x2d};

   if (color_stream || !program || !constants || !interface ||
       (uintptr_t)program % _Alignof(uint32_t) ||
       (uintptr_t)constants % _Alignof(uint32_t) ||
       program_capacity_bytes < bytes ||
       constant_capacity_bytes < sizeof(utility) ||
       bytes > UINTPTR_MAX - (uintptr_t)program ||
       sizeof(utility) > UINTPTR_MAX - (uintptr_t)constants ||
       overlap(program, bytes, constants, sizeof(utility)) ||
       overlap(program, bytes, interface, sizeof(*interface)) ||
       overlap(constants, sizeof(utility), interface, sizeof(*interface)))
      return false;
   for (uint32_t i = 0; i < 4; i++) {
      const struct m9_faa_color_fs_form *s = &forms[i];
      program[2 * i] = ((uint32_t)(s->a & 7u) << 29) |
         ((uint32_t)s->b << 22) | ((uint32_t)s->c << 15) |
         ((uint32_t)s->dst << 8) | ((uint32_t)s->cls << 5) | s->op;
      program[2 * i + 1] = s->control | s->a >> 3;
   }
   memcpy(constants, utility, sizeof(utility));
   *interface = result;
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
   /* Window z = MinZ + (MaxZ - MinZ) * z, the source ZRANGE pair. */
   float z_near = viewport->depth_range ? viewport->depth_near : 0.0f;
   float z_span = viewport->depth_range ? viewport->depth_far - viewport->depth_near : 1.0f;
   /* Actual 134880 field writers and f485b/f4863 dense-span publication.
    * Guard coefficients include the live origin. Fixed pairs are source
    * default-depth/clip coefficients, never inherited heap contents. */
   const float k[26] = {hx, -hy, z_span, 1, x + hx - viewport->centre,
      y + hy - viewport->centre, z_near,
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


bool
m9_faa_relative_slot_word(uint32_t prior, uint32_t slot_offset,
                          uint32_t target_offset, uint32_t owner_bytes,
                          uint32_t *word)
{
   if (!word || (uintptr_t)word % _Alignof(uint32_t) ||
       4u > UINTPTR_MAX - (uintptr_t)word || owner_bytes < 4 || (slot_offset & 3) ||
       (target_offset & 3) || slot_offset > owner_bytes - 4 ||
       target_offset > owner_bytes - 4 || target_offset < slot_offset ||
       target_offset - slot_offset > 0x3ffffcu)
      return false;
   *word = (prior & ~0xfffffu) | ((target_offset - slot_offset) >> 2);
   return true;
}

bool
m9_faa_preprocess_upload_build(const struct m9_faa_preprocess_upload *input,
   uint32_t *program, uint32_t program_capacity,
   uint32_t *constants, uint32_t constant_capacity,
   uint32_t *geometry, uint32_t geometry_capacity,
   struct m9_faa_preprocess_upload_layout *layout)
{
   struct m9_faa_preprocess_upload_layout result = {0};
   /* Source-created raw program extent1080; constant area ends at geometry
    * offset100. Larger live images need a separately proved owner layout. */
   uint32_t linked[0x1080 / 4], dense[256 / 4], records[224 / 4] = {0};
   const uint32_t declaration[5] = {
      0x00100d88u, 0x001006b6u, 0x04200d88u, 0x042006b6u, 0x00004b08u,
   };
   if (!input || !program || !constants || !geometry || !layout ||
       !input->vertices || !input->declaration ||
       !input->viewport.width || !input->viewport.height ||
       input->viewport.x > 15359u || input->viewport.y > 15359u ||
       input->viewport.width > 15359u - input->viewport.x ||
       input->viewport.height > 15359u - input->viewport.y ||
       (uintptr_t)program % _Alignof(uint32_t) ||
       (uintptr_t)constants % _Alignof(uint32_t) ||
       (uintptr_t)geometry % _Alignof(uint32_t) ||
       (uintptr_t)input->vertices % _Alignof(uint32_t) ||
       (uintptr_t)input->declaration % _Alignof(uint32_t) ||
       (uintptr_t)layout % _Alignof(struct m9_faa_preprocess_upload_layout) ||
       96u > UINTPTR_MAX - (uintptr_t)input->vertices ||
       sizeof(declaration) > UINTPTR_MAX - (uintptr_t)input->declaration ||
       memcmp(input->declaration, declaration, sizeof(declaration)))
      return false;
   /* Source constant allocator starts with two immutable utility pairs. */
   dense[0] = 0x3f800000u; dense[1] = 0;
   dense[2] = 0; dense[3] = 0x3f800000u;
   result.constants_bytes = 16;
   for (uint32_t i = 0; i < 4; i++) {
      const struct m9_faa_dense_constants *k = &input->constants[i];
      if (k->pairs > 63u || (k->pairs && (!k->words ||
          (uintptr_t)k->words % _Alignof(uint32_t))) ||
          k->pairs > (sizeof(dense) - result.constants_bytes) / 8u ||
          (k->pairs && k->pairs * 8u > UINTPTR_MAX - (uintptr_t)k->words))
         return false;
      result.constant_begin[i] = result.constants_bytes;
      if (k->pairs)
         memcpy((unsigned char *)dense + result.constants_bytes,
                k->words, k->pairs * 8u);
      result.constants_bytes += k->pairs * 8u;
      result.constant_end[i] = result.constants_bytes;
   }
   /* The existing linker validates every vector's STOP/finish and all
    * lengths, before copying into this private scratch image. */
   if (!m9_faa_program_link(input->vectors, 4, linked, sizeof(linked),
                            &result.program))
      return false;
   result.geometry_bytes = sizeof(records);
   result.vertex_offset = 128;
   result.vertex_bytes = 96;
   memcpy(result.declaration, declaration, sizeof(declaration));
   float header[8] = {
      input->viewport.width * 0.5f, input->viewport.height * -0.5f,
      1.0f, 0.0f,
      input->viewport.x + input->viewport.width * 0.5f - input->viewport.centre,
      input->viewport.y + input->viewport.height * 0.5f - input->viewport.centre,
      0.0f, 0.0f,
   };
   memcpy(records, header, sizeof(header));
   memcpy((unsigned char *)records + 128, input->vertices, 96);
   void *outputs[4] = {program, constants, geometry, layout};
   uint32_t sizes[4] = {result.program.bytes, result.constants_bytes,
                        sizeof(records), sizeof(*layout)};
   if (program_capacity < sizes[0] || constant_capacity < sizes[1] ||
       geometry_capacity < sizes[2])
      return false;
   for (uint32_t i = 0; i < 4; i++) {
      if (sizes[i] > UINTPTR_MAX - (uintptr_t)outputs[i] ||
          overlap(outputs[i], sizes[i], input, sizeof(*input)) ||
          overlap(outputs[i], sizes[i], input->vertices, 96) ||
          overlap(outputs[i], sizes[i], input->declaration, sizeof(declaration)))
         return false;
      for (uint32_t j = i + 1; j < 4; j++)
         if (overlap(outputs[i], sizes[i], outputs[j], sizes[j]))
            return false;
      for (uint32_t j = 0; j < 4; j++) {
         uint32_t bytes = input->vectors[j].slots * 8u;
         uint32_t kbytes = input->constants[j].pairs * 8u;
         if (overlap(outputs[i], sizes[i], input->vectors[j].words, bytes) ||
             (kbytes && overlap(outputs[i], sizes[i],
                                input->constants[j].words, kbytes)))
            return false;
      }
   }
   memcpy(program, linked, result.program.bytes);
   memcpy(constants, dense, result.constants_bytes);
   memcpy(geometry, records, sizeof(records));
   *layout = result;
   return true;
}
