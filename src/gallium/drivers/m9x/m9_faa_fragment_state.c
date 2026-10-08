/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include "m9_faa_fragment_state.h"
#include <stddef.h>
#include <string.h>

struct form { uint8_t op, cls, dst, a, b, c; uint32_t control; };
/* Original 16fa20 COLOR0 interpolation forms, allocated by 16ea00.
 * Blend reserves destination r18/r1a and redirects the result to r0b/r0a.
 * These are register/operator fields, without source program/owner bytes. */
static const struct form interpolate[6] = {
   {0x1c,0,0x1d,0x7f,0x7f,0x7a,0xa28fcf10},
   {0x1c,0,1,0x79,0x7f,0x7f,0x8c020d10},
   {0x1c,0,2,0x1d,0x7f,0x7f,0xac8001f0},
   {0x1c,0,0x1c,0x79,0x7f,0x7f,0x8c024d10},
   {0x1c,0,3,1,2,0x79,0xac4fcd10},
   {0x1c,4,0x1b,0x1c,2,3,0xac4fcdd0},
};
static bool overlap(const void *a, size_t ab, const void *b, size_t bb)
{
   uintptr_t x=(uintptr_t)a, y=(uintptr_t)b;
   return x <= y ? y-x < ab : x-y < bb;
}
static void emit(uint32_t *words, uint32_t *n, struct form f)
{
   words[2 * *n] = ((uint32_t)(f.a & 7) << 29) | ((uint32_t)f.b << 22) |
      ((uint32_t)f.c << 15) | ((uint32_t)f.dst << 8) | ((uint32_t)f.cls << 5) | f.op;
   words[2 * *n + 1] = f.control | (f.a >> 3);
   ++*n;
}

bool m9_faa_fragment_state_build(const struct m9_faa_fragment_state *state,
                                 uint32_t *program, uint32_t program_capacity,
                                 uint32_t *constants, uint32_t constant_capacity,
                                 struct m9_faa_fragment_interface *interface)
{
   if (!state || !program || !constants || !interface ||
       (uintptr_t)state % _Alignof(struct m9_faa_fragment_state) ||
       (uintptr_t)interface % _Alignof(struct m9_faa_fragment_interface) || state->source_stream ||
       (unsigned)state->blend > M9_FAA_BLEND_FILTER || (unsigned)state->alpha > M9_FAA_ALPHA_GE_HALF)
      return false;
   bool blend = state->blend != M9_FAA_BLEND_REPLACE;
   bool alpha = state->alpha != M9_FAA_ALPHA_OFF;
   const uint32_t slots = 8 + (blend ? 2 : 0) + (alpha ? 4 : 0);
   const uint32_t pairs = 2 + (blend ? 1 : 0) + (alpha ? 3 : 0);
   const uint32_t pb = slots * 8, cb = pairs * 8;
   if ((uintptr_t)program % _Alignof(uint32_t) ||
       (uintptr_t)constants % _Alignof(uint32_t) || program_capacity < pb ||
       constant_capacity < cb || pb > UINTPTR_MAX - (uintptr_t)program ||
       cb > UINTPTR_MAX - (uintptr_t)constants ||
       overlap(program,pb,constants,cb) ||
       overlap(program,pb,interface,sizeof(*interface)) ||
       overlap(constants,cb,interface,sizeof(*interface)) ||
       overlap(program,pb,state,sizeof(*state)) ||
       overlap(constants,cb,state,sizeof(*state)) ||
       overlap(interface,sizeof(*interface),state,sizeof(*state)))
      return false;

   uint32_t words[28], utility[12] = {0x3f800000,0x3f000000}, n=0, k=blend ? 4 : 2;
   for (uint32_t i=0;i<6;++i) {
      struct form f=interpolate[i];
      if (blend) {
         if (i==1 || i==2 || i==4) f.dst+=2;
         if (i==4) { f.a+=2; f.b+=2; }
         if (i==5) { f.dst=0; f.b+=2; f.c+=2; }
      }
      emit(words,&n,f);
   }
   uint8_t scale=blend ? 0 : 0x1b;
   uint8_t ba=blend ? (alpha ? 8 : 0x0a) : (alpha ? 6 : 0x2d);
   uint8_t rg=blend ? 0x0b : 0x2d;
   /* Original combined controls require BA before the alpha conversion.
    * The alpha-disabled order still follows the source-alpha factor. */
   bool alpha_first=alpha || (blend && (state->blend==M9_FAA_BLEND_ALPHA ||
                                      state->blend==M9_FAA_BLEND_ALPHA_ADD));
   struct form rg_move={blend ? 0x1c : 0x1d,4,rg,0x40,scale,0x44,0xae0ffc10};
   struct form ba_move={blend || alpha ? 0x1c : 0x1e,4,ba,0x42,scale,0x45,0xae0ffc10};
   if (alpha_first || !blend) { emit(words,&n,ba_move); emit(words,&n,rg_move); }
   else { emit(words,&n,rg_move); emit(words,&n,ba_move); }
   if (alpha) {
      uint32_t ref=state->alpha==M9_FAA_ALPHA_GT_ZERO ? 0 : 0x43000000; /* float128 */
      utility[k++]=ref; utility[k++]=ref;
      utility[k++]=0; utility[k++]=0x437f0000; /* alpha conversion: 0,255 */
      /* Original selector18104c reads the immutable 1f6414 pair for key7cf4,
       * modifier1. The epsilon is part of that source conversion, not an
       * invented threshold or a native address. */
      utility[k++]=0x40000001; utility[k++]=0x3f800000;
      uint8_t temp=blend ? 0 : 0x1b;
      uint32_t convert=blend ? 0xa040c3c0 : 0xa04083c0;
      emit(words,&n,(struct form){0x1d,0,temp,0x7f,ba,0x7f,convert});
      emit(words,&n,(struct form){0x1d,0,temp,0x7f,0x7f,temp,0xa29fccc0});
      emit(words,&n,(struct form){blend ? 0x1c : 0x1e,blend ? 4 : 0,
                                blend ? 0x0a : 0x2d,0x7f,0x7f,ba,0xa29fff10});
      uint8_t cmp=state->alpha==M9_FAA_ALPHA_GT_ZERO ? 2 :
                  state->alpha==M9_FAA_ALPHA_LT_HALF ? 1 : 0;
      emit(words,&n,(struct form){0x1c,cmp,0x40,0x7f,temp,0x7f,
                                  blend ? 0xa3c08000 : 0xa3c04000});
   }
   if (blend) {
      utility[k++]=0; utility[k++]=0x3f800000;
      switch (state->blend) {
      case M9_FAA_BLEND_ALPHA:
         if (alpha) {
            emit(words,&n,(struct form){0x1d,4,0x2d,0x0b,0x0a,0x18,0xa4bfcdc0});
            emit(words,&n,(struct form){0x1e,4,0x2d,0x0a,0x0a,0x1a,0xa4bfcdc0});
         } else {
            emit(words,&n,(struct form){0x1e,4,0x2d,0x0a,0x0a,0x1a,0xa4bfcdc0});
            emit(words,&n,(struct form){0x1d,4,0x2d,0x0b,0x0a,0x18,0xa4bfcdc0});
         }
         break;
      case M9_FAA_BLEND_ADD:
         emit(words,&n,(struct form){0x1d,4,0x2d,0x7f,0x0b,0x18,0xa0400c50});
         emit(words,&n,(struct form){0x1e,4,0x2d,0x7f,0x0a,0x1a,0xa0400c50});
         break;
      case M9_FAA_BLEND_ALPHA_ADD:
         /* Alpha-tested SRC_ALPHA/ONE publishes RGB before BA; original
          * alpha-disabled allocation publishes BA before RGB. */
         if (alpha) {
         emit(words,&n,(struct form){0x1d,4,0x2d,0x0a,0x0b,0x18,0xac5fcf50});
         emit(words,&n,(struct form){0x1e,4,0x2d,0x0a,0x0a,0x1a,0xac5fcf50});
         } else {
         emit(words,&n,(struct form){0x1e,4,0x2d,0x0a,0x0a,0x1a,0xac5fcf50});
         emit(words,&n,(struct form){0x1d,4,0x2d,0x0a,0x0b,0x18,0xac5fcf50});
         }
         break;
      case M9_FAA_BLEND_FILTER:
         emit(words,&n,(struct form){0x1d,4,0x2d,0x18,0x0b,0x7f,0xac3fc140});
         emit(words,&n,(struct form){0x1e,4,0x2d,0x1a,0x0a,0x7f,0xac3fc140});
         break;
      default: return false;
      }
   } else if (!alpha) {
      utility[k++]=0; utility[k++]=0x3f800000;
   } else { utility[k++]=0; utility[k++]=0x3f800000; }
   if (n != slots || k != pairs * 2) return false;
   /* Original f40e3 finish: fourth slot from the final vector end. */
   words[2 * (n-4) + 1] |= 0x40000000;
   struct m9_faa_fragment_interface result={slots,pairs,0,0x2d,
                                           blend ? 0xbf80 : 0x9ff0,blend,alpha};
   memcpy(program,words,pb); memcpy(constants,utility,cb); *interface=result;
   return true;
}

bool m9_faa_fragment_state_decode(const uint32_t *program, uint32_t bytes,
                                  const uint32_t *constants, uint32_t constant_bytes,
                                  struct m9_faa_fragment_state *state,
                                  struct m9_faa_fragment_interface *interface)
{
   if (!program || !constants || !state || !interface ||
       (uintptr_t)program % _Alignof(uint32_t) ||
       (uintptr_t)constants % _Alignof(uint32_t) ||
       (uintptr_t)state % _Alignof(struct m9_faa_fragment_state) ||
       (uintptr_t)interface % _Alignof(struct m9_faa_fragment_interface) ||
       bytes > 112 || bytes < 64 || (bytes & 7) ||
       constant_bytes > 48 || constant_bytes < 16 || (constant_bytes & 7) ||
       bytes > UINTPTR_MAX - (uintptr_t)program ||
       constant_bytes > UINTPTR_MAX - (uintptr_t)constants ||
       overlap(state,sizeof(*state),program,bytes) ||
       overlap(interface,sizeof(*interface),program,bytes) ||
       overlap(state,sizeof(*state),constants,constant_bytes) ||
       overlap(interface,sizeof(*interface),constants,constant_bytes) ||
       overlap(state,sizeof(*state),interface,sizeof(*interface))) return false;
   for (unsigned blend=0;blend<=M9_FAA_BLEND_FILTER;blend++)
      for (unsigned alpha=0;alpha<=M9_FAA_ALPHA_GE_HALF;alpha++) {
         struct m9_faa_fragment_state candidate={0,blend,alpha};
         struct m9_faa_fragment_interface found;
         uint32_t p[28],k[12];
         if (!m9_faa_fragment_state_build(&candidate,p,sizeof(p),k,sizeof(k),&found) ||
             found.slots*8 != bytes || found.constant_pairs*8 != constant_bytes ||
             memcmp(p,program,bytes) || memcmp(k,constants,constant_bytes)) continue;
         *state=candidate; *interface=found; return true;
      }
   return false;
}

/* Textured later-switch programs, decoded from the original 16ea00
 * allocation of each program/alpha-op/blend/test combination. The
 * compare slot (destination 40) takes its class from the state, blended
 * forms get the factor tail below; finish marks are added on emission. */
static const struct form dual_select[9] = {
   {0x1d,4,0x0c,0x42,0x79,0x45,0xae0ffc00},
   {0x1c,4,0x0b,0xc0,0x79,0x44,0xae0ffc00},
   {0x1d,4,0x08,0x02,0x0c,0x7f,0xac2fc000},
   {0x1c,0,0x09,0xff,0x7f,0x02,0xa28fff00},
   {0x1d,4,0x0a,0x06,0x08,0x7f,0xac2fc000},
   {0x1e,4,0x09,0xff,0x7f,0x02,0xa28fff20},
   {0x1c,4,0x0e,0x00,0x0b,0x7f,0xac2fc140},
   {0x1e,4,0x2d,0x7f,0x09,0x0a,0xa3400ec0},
   {0x1d,4,0x2d,0x04,0x0e,0x7f,0xac2fc140},
};
static const struct form dual_select_blend[10] = {
   {0x1d,4,0x0d,0x42,0x79,0x45,0xae0ffc00},
   {0x1c,4,0x0c,0xc0,0x79,0x44,0xae0ffc00},
   {0x1d,4,0x08,0x02,0x0d,0x7f,0xac2fc000},
   {0x1c,0,0x09,0xff,0x7f,0x02,0xa28fff00},
   {0x1d,4,0x0a,0x06,0x08,0x7f,0xac2fc000},
   {0x1e,4,0x09,0xff,0x7f,0x02,0xa28fff20},
   {0x1c,4,0x0e,0x00,0x0c,0x7f,0xac2fc140},
   {0x1c,4,0x0f,0x7f,0x09,0x0a,0xa3400ec0},
   {0x1c,4,0x16,0x04,0x0e,0x7f,0xac2fc140},
   {0x1c,4,0x15,0xff,0x7f,0x0f,0xa29fff00},
};
static const struct form dual_select_test[12] = {
   {0x1d,4,0x0c,0x42,0x79,0x45,0xae0ffc00},
   {0x1c,4,0x0b,0xc0,0x79,0x44,0xae0ffc00},
   {0x1d,4,0x08,0x02,0x0c,0x7f,0xac2fc000},
   {0x1e,4,0x15,0xff,0x7f,0x02,0xa28fff20},
   {0x1d,4,0x0a,0x06,0x08,0x7f,0xac2fc000},
   {0x1c,4,0x0e,0x00,0x0b,0x7f,0xac2fc140},
   {0x1c,4,0x0f,0x7f,0x15,0x0a,0xa3400ec0},
   {0x1d,4,0x2d,0x04,0x0e,0x7f,0xac2fc140},
   {0x1d,0,0x15,0x7f,0x0f,0x7f,0xa04083c0},
   {0x1d,0,0x15,0x7f,0x7f,0x15,0xa29fccc0},
   {0x1e,0,0x2d,0xff,0x7f,0x0f,0xa29fff00},
   {0x1c,0,0x40,0x7f,0x15,0x7f,0xa3c04000},
};
static const struct form dual_select_blend_test[13] = {
   {0x1d,4,0x0d,0x42,0x79,0x45,0xae0ffc00},
   {0x1c,4,0x0c,0xc0,0x79,0x44,0xae0ffc00},
   {0x1d,4,0x08,0x02,0x0d,0x7f,0xac2fc000},
   {0x1c,0,0x09,0xff,0x7f,0x02,0xa28fff00},
   {0x1d,4,0x0a,0x06,0x08,0x7f,0xac2fc000},
   {0x1e,4,0x09,0xff,0x7f,0x02,0xa28fff20},
   {0x1c,4,0x0e,0x00,0x0c,0x7f,0xac2fc140},
   {0x1c,4,0x0f,0x7f,0x09,0x0a,0xa3400ec0},
   {0x1c,4,0x16,0x04,0x0e,0x7f,0xac2fc140},
   {0x1d,0,0x0e,0x7f,0x0f,0x7f,0xa040c3c0},
   {0x1d,0,0x0e,0x7f,0x7f,0x0e,0xa29fccc0},
   {0x1c,4,0x15,0xff,0x7f,0x0f,0xa29fff00},
   {0x1c,0,0x40,0x7f,0x0e,0x7f,0xa3c08000},
};
static const struct form dual_modulate[6] = {
   {0x1c,4,0x0b,0xc2,0x79,0x45,0xae003c00},
   {0x1c,4,0x0a,0xc0,0x79,0x44,0xae003c00},
   {0x1c,4,0x08,0x02,0x0b,0x7f,0xac200140},
   {0x1c,4,0x09,0x00,0x0a,0x7f,0xac200140},
   {0x1e,4,0x2d,0x06,0x08,0x7f,0xac200140},
   {0x1d,4,0x2d,0x04,0x09,0x7f,0xac200140},
};
static const struct form dual_modulate_blend[6] = {
   {0x1c,4,0x0d,0xc2,0x79,0x45,0xae003c00},
   {0x1c,4,0x0c,0xc0,0x79,0x44,0xae003c00},
   {0x1c,4,0x08,0x02,0x0d,0x7f,0xac200140},
   {0x1c,4,0x09,0x00,0x0c,0x7f,0xac200140},
   {0x1c,4,0x14,0x06,0x08,0x7f,0xac200140},
   {0x1c,4,0x15,0x04,0x09,0x7f,0xac200140},
};
static const struct form dual_modulate_test[10] = {
   {0x1c,4,0x0b,0xc2,0x79,0x45,0xae003c00},
   {0x1c,4,0x0a,0xc0,0x79,0x44,0xae003c00},
   {0x1c,4,0x08,0x02,0x0b,0x7f,0xac200140},
   {0x1c,4,0x09,0x00,0x0a,0x7f,0xac200140},
   {0x1c,4,0x0e,0x06,0x08,0x7f,0xac200140},
   {0x1d,4,0x2d,0x04,0x09,0x7f,0xac200140},
   {0x1d,0,0x09,0x7f,0x0e,0x7f,0xa04043c0},
   {0x1d,0,0x09,0x7f,0x7f,0x09,0xa29fccc0},
   {0x1e,0,0x2d,0xff,0x7f,0x0e,0xa29fff00},
   {0x1c,0,0x40,0x7f,0x09,0x7f,0xa3c00000},
};
static const struct form dual_modulate_blend_test[10] = {
   {0x1c,4,0x0d,0xc2,0x79,0x45,0xae003c00},
   {0x1c,4,0x0c,0xc0,0x79,0x44,0xae003c00},
   {0x1c,4,0x08,0x02,0x0d,0x7f,0xac200140},
   {0x1c,4,0x09,0x00,0x0c,0x7f,0xac200140},
   {0x1c,4,0x0e,0x06,0x08,0x7f,0xac200140},
   {0x1c,4,0x15,0x04,0x09,0x7f,0xac200140},
   {0x1d,0,0x08,0x7f,0x0e,0x7f,0xa04083c0},
   {0x1d,0,0x08,0x7f,0x7f,0x08,0xa29fccc0},
   {0x1c,4,0x14,0xff,0x7f,0x0e,0xa29fff00},
   {0x1c,0,0x40,0x7f,0x08,0x7f,0xa3c04000},
};
static const struct form single_select[6] = {
   {0x1d,4,0x06,0x42,0x79,0x45,0xae003c00},
   {0x1c,4,0x1a,0xc0,0x79,0x44,0xae003c00},
   {0x1d,4,0x08,0x02,0x06,0x7f,0xac200000},
   {0x1e,4,0x08,0xff,0x7f,0x02,0xa2803f20},
   {0x1d,4,0x2d,0x00,0x1a,0x7f,0xac200140},
   {0x1e,0,0x2d,0xff,0x7f,0x08,0xa29fff00},
};
static const struct form single_select_blend[8] = {
   {0x1d,4,0x04,0x42,0x79,0x45,0xae003c00},
   {0x1c,4,0x09,0xc0,0x79,0x44,0xae003c00},
   {0x1d,4,0x13,0x02,0x04,0x7f,0xac200000},
   {0x1c,0,0x11,0xff,0x7f,0x02,0xa2803f00},
   {0x1d,0,0x05,0x7f,0x7f,0x13,0xa2803f00},
   {0x1e,4,0x05,0xff,0x7f,0x02,0xa2803f20},
   {0x1c,4,0x0c,0x00,0x09,0x7f,0xac200140},
   {0x1c,4,0x0b,0xff,0x7f,0x05,0xa29fff00},
};
static const struct form single_select_test[9] = {
   {0x1d,4,0x06,0x42,0x79,0x45,0xae003c00},
   {0x1c,4,0x1a,0xc0,0x79,0x44,0xae003c00},
   {0x1d,4,0x08,0x02,0x06,0x7f,0xac200000},
   {0x1e,4,0x08,0xff,0x7f,0x02,0xa2803f20},
   {0x1d,4,0x2d,0x00,0x1a,0x7f,0xac200140},
   {0x1d,0,0x05,0x7f,0x08,0x7f,0xa04043c0},
   {0x1d,0,0x05,0x7f,0x7f,0x05,0xa29fccc0},
   {0x1e,0,0x2d,0xff,0x7f,0x08,0xa29fff00},
   {0x1c,0,0x40,0x7f,0x05,0x7f,0xa3c00000},
};
static const struct form single_select_blend_test[11] = {
   {0x1d,4,0x04,0x42,0x79,0x45,0xae003c00},
   {0x1c,4,0x09,0xc0,0x79,0x44,0xae003c00},
   {0x1d,4,0x13,0x02,0x04,0x7f,0xac200000},
   {0x1c,0,0x11,0xff,0x7f,0x02,0xa2803f00},
   {0x1d,0,0x05,0x7f,0x7f,0x13,0xa2803f00},
   {0x1e,4,0x05,0xff,0x7f,0x02,0xa2803f20},
   {0x1c,4,0x0c,0x00,0x09,0x7f,0xac200140},
   {0x1d,0,0x08,0x7f,0x05,0x7f,0xa04083c0},
   {0x1d,0,0x08,0x7f,0x7f,0x08,0xa29fccc0},
   {0x1c,4,0x0b,0xff,0x7f,0x05,0xa29fff00},
   {0x1c,0,0x40,0x7f,0x08,0x7f,0xa3c04000},
};
static const struct form single_modulate[4] = {
   {0x1c,4,0x04,0xc2,0x79,0x45,0xae003c00},
   {0x1c,4,0x1a,0xc0,0x79,0x44,0xae003c00},
   {0x1e,4,0x2d,0x02,0x04,0x7f,0xac200140},
   {0x1d,4,0x2d,0x00,0x1a,0x7f,0xac200140},
};
static const struct form single_modulate_blend[4] = {
   {0x1c,4,0x04,0xc2,0x79,0x45,0xae003c00},
   {0x1c,4,0x09,0xc0,0x79,0x44,0xae003c00},
   {0x1c,4,0x0b,0x02,0x04,0x7f,0xac200140},
   {0x1c,4,0x0c,0x00,0x09,0x7f,0xac200140},
};
static const struct form single_modulate_test[8] = {
   {0x1c,4,0x04,0xc2,0x79,0x45,0xae003c00},
   {0x1c,4,0x1a,0xc0,0x79,0x44,0xae003c00},
   {0x1c,4,0x08,0x02,0x04,0x7f,0xac200140},
   {0x1d,4,0x2d,0x00,0x1a,0x7f,0xac200140},
   {0x1d,0,0x05,0x7f,0x08,0x7f,0xa04043c0},
   {0x1d,0,0x05,0x7f,0x7f,0x05,0xa29fccc0},
   {0x1e,0,0x2d,0xff,0x7f,0x08,0xa29fff00},
   {0x1c,0,0x40,0x7f,0x05,0x7f,0xa3c00000},
};
static const struct form single_modulate_blend_test[8] = {
   {0x1c,4,0x04,0xc2,0x79,0x45,0xae003c00},
   {0x1c,4,0x09,0xc0,0x79,0x44,0xae003c00},
   {0x1c,4,0x05,0x02,0x04,0x7f,0xac200140},
   {0x1c,4,0x0c,0x00,0x09,0x7f,0xac200140},
   {0x1d,0,0x08,0x7f,0x05,0x7f,0xa04083c0},
   {0x1d,0,0x08,0x7f,0x7f,0x08,0xa29fccc0},
   {0x1c,4,0x0b,0xff,0x7f,0x05,0xa29fff00},
   {0x1c,0,0x40,0x7f,0x08,0x7f,0xa3c04000},
};

enum { RG = 0x100, BA, XRG, XBA, K_LAST = 0xff };
/* Blend tail of 170ff0: RGB writes r18 half, BA the r1a half. Operands name
 * the source RG/BA results (X: 0x80 modifier); K is the C3 pair index in
 * control bits 19:14, K_LAST the trailing (1, 0.5) utility pair. */
static const struct factor_tail {
   uint8_t src, dst;
   uint16_t rgb[3], ba[3];
   uint32_t control;
   uint8_t k;
   bool rg_first;
} factor_tail[] = {
   {M9_FAA_FACTOR_ONE,M9_FAA_FACTOR_ONE,{0xff,RG,0x18},{0xff,BA,0x1a},0xa0400c40,K_LAST,false},
   {M9_FAA_FACTOR_DST_COLOR,M9_FAA_FACTOR_ZERO,{0x18,RG,0x7f},{0x1a,BA,0x7f},0xac300140,0x3f,false},
   {M9_FAA_FACTOR_ZERO,M9_FAA_FACTOR_SRC_COLOR,{RG,0x18,0x7f},{BA,0x1a,0x7f},0xac300140,0x3f,false},
   {M9_FAA_FACTOR_SRC_ALPHA,M9_FAA_FACTOR_INV_SRC_ALPHA,{RG,BA,0x18},{BA,BA,0x1a},0xa4b00dc0,0x3f,false},
   /* scheduled RGB first even after a BA-first base */
   {M9_FAA_FACTOR_ONE,M9_FAA_FACTOR_INV_SRC_ALPHA,{RG,BA,0x18},{BA,BA,0x1a},0xa4e00c40,K_LAST,true},
   {M9_FAA_FACTOR_SRC_ALPHA,M9_FAA_FACTOR_ONE,{XBA,RG,0x18},{XBA,BA,0x1a},0xac500f40,0x3f,false},
   {M9_FAA_FACTOR_DST_COLOR,M9_FAA_FACTOR_ONE,{0x98,RG,0x18},{0x9a,BA,0x1a},0xac500d40,0x3f,false},
   {M9_FAA_FACTOR_DST_COLOR,M9_FAA_FACTOR_SRC_COLOR,{0x18,RG,0x18},{0x1a,BA,0x1a},0xa5500540,0x3f,false},
   {M9_FAA_FACTOR_INV_DST_COLOR,M9_FAA_FACTOR_ZERO,{XRG,0x18,0x7f},{XBA,0x1a,0x7f},0x9c403d40,2,false},
   {M9_FAA_FACTOR_INV_DST_COLOR,M9_FAA_FACTOR_ONE,{0x18,0x18,RG},{0x1a,0x1a,BA},0xa4e00440,K_LAST,false},
};

struct texture_variant {
   const struct form *forms;
   uint8_t count, rg, ba;
   bool rg_first;
   uint32_t usage;
};
#define V(f, ...) {f, sizeof(f) / sizeof(f[0]), __VA_ARGS__}
/* [program][alpha op][blend][alpha test] */
static const struct texture_variant texture_variant[2][2][2][2] = {
   {{{V(single_select,0,0,false,0x0110dfe3),V(single_select_test,0,0,false,0x0110dfe3)},
     {V(single_select_blend,0x0c,0x0b,true,0x0110ff03),V(single_select_blend_test,0x0c,0x0b,true,0x0110ff03)}},
    {{V(single_modulate,0,0,false,0x0110dfe3),V(single_modulate_test,0,0,false,0x0110dfe3)},
     {V(single_modulate_blend,0x0c,0x0b,false,0x0110ff03),V(single_modulate_blend_test,0x0c,0x0b,true,0x0110ff03)}}},
   {{{V(dual_select,0,0,false,0x0130fb0f),V(dual_select_test,0,0,false,0x0130fb0f)},
     {V(dual_select_blend,0x16,0x15,true,0x0130b30f),V(dual_select_blend_test,0x16,0x15,true,0x0130b30f)}},
    {{V(dual_modulate,0,0,false,0x0130ff0f),V(dual_modulate_test,0,0,false,0x0130ff0f)},
     {V(dual_modulate_blend,0x15,0x14,false,0x0130f30f),V(dual_modulate_blend_test,0x15,0x14,true,0x0130f30f)}}},
};
#undef V

static uint8_t tail_operand(uint16_t v, const struct texture_variant *t)
{
   switch (v) {
   case RG: return t->rg;
   case BA: return t->ba;
   case XRG: return 0x80 | t->rg;
   case XBA: return 0x80 | t->ba;
   default: return v;
   }
}

bool m9_faa_texture_fragment_build(const struct m9_faa_texture_fragment_state *state,
                                   uint32_t *program, uint32_t program_capacity,
                                   uint32_t *constants, uint32_t constant_capacity,
                                   struct m9_faa_texture_fragment_interface *interface)
{
   if (!state || !program || !constants || !interface ||
       (uintptr_t)state % _Alignof(struct m9_faa_texture_fragment_state) ||
       (uintptr_t)interface % _Alignof(struct m9_faa_texture_fragment_interface) ||
       (uintptr_t)program % _Alignof(uint32_t) || (uintptr_t)constants % _Alignof(uint32_t) ||
       (unsigned)state->program > M9_FAA_TEXTURE_DUAL ||
       (unsigned)state->alpha_op > M9_FAA_TEXTURE_ALPHA_MODULATE ||
       (state->alpha_test && (unsigned)state->alpha_func > M9_FAA_ALPHA_FUNC_GREATER))
      return false;
   const struct factor_tail *tail = NULL;
   if (state->blend) {
      for (unsigned i = 0; i < sizeof(factor_tail) / sizeof(factor_tail[0]); i++)
         if (factor_tail[i].src == state->src && factor_tail[i].dst == state->dst)
            tail = &factor_tail[i];
      if (!tail)
         return false;
   }
   const struct texture_variant *t =
      &texture_variant[state->program][state->alpha_op][state->blend][state->alpha_test];

   /* C3 order: stage-1 select (0,1), blend (0,0), reference, alpha
    * conversion (0,255) and (2+e,1), then the combine's (0,1),(1,0.5). */
   uint32_t utility[14], k = 0;
   if (state->program == M9_FAA_TEXTURE_DUAL && state->alpha_op == M9_FAA_TEXTURE_ALPHA_SELECT) {
      utility[k++] = 0; utility[k++] = 0x3f800000;
   }
   if (state->blend) { utility[k++] = 0; utility[k++] = 0; }
   if (state->alpha_test) {
      union { float f; uint32_t u; } ref = { (float)state->alpha_ref };
      utility[k++] = ref.u; utility[k++] = ref.u;
      utility[k++] = 0; utility[k++] = 0x437f0000;
      utility[k++] = 0x40000001; utility[k++] = 0x3f800000;
   }
   utility[k++] = 0; utility[k++] = 0x3f800000;
   utility[k++] = 0x3f800000; utility[k++] = 0x3f000000;

   const uint32_t slots = t->count + (tail ? 2 : 0), pairs = k / 2;
   const uint32_t pb = slots * 8, cb = pairs * 8;
   if (program_capacity < pb || constant_capacity < cb ||
       pb > UINTPTR_MAX - (uintptr_t)program || cb > UINTPTR_MAX - (uintptr_t)constants ||
       overlap(program, pb, constants, cb) ||
       overlap(program, pb, interface, sizeof(*interface)) ||
       overlap(constants, cb, interface, sizeof(*interface)) ||
       overlap(program, pb, state, sizeof(*state)) ||
       overlap(constants, cb, state, sizeof(*state)) ||
       overlap(interface, sizeof(*interface), state, sizeof(*state)))
      return false;

   uint32_t words[30], n = 0;
   for (uint32_t i = 0; i < t->count; i++) {
      struct form f = t->forms[i];
      if (f.dst == 0x40)
         f.cls = state->alpha_func;
      emit(words, &n, f);
   }
   if (tail) {
      uint32_t control = tail->control | (uint32_t)(tail->k == K_LAST ? pairs - 1 : tail->k) << 14;
      struct form rgb = {0x1d, 4, 0x2d, tail_operand(tail->rgb[0], t), tail_operand(tail->rgb[1], t),
                         tail_operand(tail->rgb[2], t), control};
      struct form ba = {0x1e, 4, 0x2d, tail_operand(tail->ba[0], t), tail_operand(tail->ba[1], t),
                        tail_operand(tail->ba[2], t), control};
      bool rg_first = t->rg_first || tail->rg_first;
      emit(words, &n, rg_first ? rgb : ba);
      emit(words, &n, rg_first ? ba : rgb);
   }
   words[2 * (n - 4) + 1] |= 0x40000000;
   memcpy(program, words, pb);
   memcpy(constants, utility, cb);
   *interface = (struct m9_faa_texture_fragment_interface){
      slots, pairs,
      state->blend ? 0xc5008200 : state->alpha_test ? 0xc4008200 : 0xc4008600,
      t->usage, state->blend, state->alpha_test,
   };
   return true;
}

/* For 8-bit alpha k: k > 255r iff k > floor(255r); k < 255r and
 * k >= 255r iff against ceil(255r). Q3's 0.5 becomes 128. */
bool m9_faa_alpha_ref_from_gl(enum m9_faa_alpha_func func, float ref, uint8_t *out)
{
   if (!out || (unsigned)func > M9_FAA_ALPHA_FUNC_GREATER || !(ref >= 0.0f && ref <= 1.0f))
      return false;
   float x = ref * 255.0f;
   uint32_t i = (uint32_t)x;
   if (func != M9_FAA_ALPHA_FUNC_GREATER && (float)i < x)
      i++;
   *out = i;
   return true;
}
