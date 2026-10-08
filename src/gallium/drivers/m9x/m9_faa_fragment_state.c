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
