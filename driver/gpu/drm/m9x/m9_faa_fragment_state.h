/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9X_FAA_FRAGMENT_STATE_H
#define M9X_FAA_FRAGMENT_STATE_H
#include <linux/types.h>
#include <linux/string.h>

/* These are complete bounded smooth COLOR0 compiler profiles. Texture,
 * flat, separate-alpha, arbitrary blend factors and disabled depth are
 * separate interfaces and cannot reuse this register allocation. */
enum m9_faa_blend_mode {
   M9_FAA_BLEND_REPLACE,
   M9_FAA_BLEND_ALPHA,
   M9_FAA_BLEND_ADD,
   M9_FAA_BLEND_ALPHA_ADD,
   M9_FAA_BLEND_FILTER,
};
enum m9_faa_alpha_mode {
   M9_FAA_ALPHA_OFF,
   M9_FAA_ALPHA_GT_ZERO,
   M9_FAA_ALPHA_LT_HALF,
   M9_FAA_ALPHA_GE_HALF,
};
struct m9_faa_fragment_state {
   u32 source_stream;
   enum m9_faa_blend_mode blend;
   enum m9_faa_alpha_mode alpha;
};
struct m9_faa_fragment_interface {
   u32 slots, constant_pairs, source_stream, result_register;
   /* Source compiler allocation metadata for these emitted forms. Native
    * binding must pair these with the owned program and live resource state;
    * shader construction grants no native submission admission. */
   u32 available_register_mask;
   bool reads_destination, alpha_test;
};

/* Mirror of the source-proven bounded ISA constructor; no owner/address
 * is encoded. Full shader+utility equality is required before raster effects. */
struct m9x_fragment_form { u8 op, cls, dst, a, b, c; u32 control; };
/* Original 16fa20 COLOR0 interpolation forms, allocated by 16ea00.
 * Blend reserves destination r18/r1a and redirects the result to r0b/r0a.
 * These are register/operator fields, without source program/owner bytes. */
static const struct m9x_fragment_form m9x_fragment_interpolate[6] = {
   {0x1c,0,0x1d,0x7f,0x7f,0x7a,0xa28fcf10},
   {0x1c,0,1,0x79,0x7f,0x7f,0x8c020d10},
   {0x1c,0,2,0x1d,0x7f,0x7f,0xac8001f0},
   {0x1c,0,0x1c,0x79,0x7f,0x7f,0x8c024d10},
   {0x1c,0,3,1,2,0x79,0xac4fcd10},
   {0x1c,4,0x1b,0x1c,2,3,0xac4fcdd0},
};
static inline bool m9x_fragment_overlap(const void *a, size_t ab, const void *b, size_t bb)
{
   unsigned long x=(unsigned long)a, y=(unsigned long)b;
   return x <= y ? y-x < ab : x-y < bb;
}
static inline void m9x_fragment_emit(u32 *words, u32 *n, struct m9x_fragment_form f)
{
   words[2 * *n] = ((u32)(f.a & 7) << 29) | ((u32)f.b << 22) |
      ((u32)f.c << 15) | ((u32)f.dst << 8) | ((u32)f.cls << 5) | f.op;
   words[2 * *n + 1] = f.control | (f.a >> 3);
   ++*n;
}

static inline bool m9x_faa_fragment_state_build(const struct m9_faa_fragment_state *state,
                                 u32 *program, u32 program_capacity,
                                 u32 *constants, u32 constant_capacity,
                                 struct m9_faa_fragment_interface *interface)
{
   if (!state || !program || !constants || !interface ||
       (unsigned long)state % _Alignof(struct m9_faa_fragment_state) ||
       (unsigned long)interface % _Alignof(struct m9_faa_fragment_interface) || state->source_stream ||
       (unsigned)state->blend > M9_FAA_BLEND_FILTER || (unsigned)state->alpha > M9_FAA_ALPHA_GE_HALF)
      return false;
   bool blend = state->blend != M9_FAA_BLEND_REPLACE;
   bool alpha = state->alpha != M9_FAA_ALPHA_OFF;
   /* The only combined compiler allocation proved by the original control. */
   if (blend && alpha && (state->blend != M9_FAA_BLEND_ALPHA ||
                         state->alpha != M9_FAA_ALPHA_GE_HALF))
      return false;
   const u32 slots = 8 + (blend ? 2 : 0) + (alpha ? 4 : 0);
   const u32 pairs = 2 + (blend ? 1 : 0) + (alpha ? 3 : 0);
   const u32 pb = slots * 8, cb = pairs * 8;
   if ((unsigned long)program % _Alignof(u32) ||
       (unsigned long)constants % _Alignof(u32) || program_capacity < pb ||
       constant_capacity < cb || pb > (~0UL) - (unsigned long)program ||
       cb > (~0UL) - (unsigned long)constants ||
       m9x_fragment_overlap(program,pb,constants,cb) ||
       m9x_fragment_overlap(program,pb,interface,sizeof(*interface)) ||
       m9x_fragment_overlap(constants,cb,interface,sizeof(*interface)) ||
       m9x_fragment_overlap(program,pb,state,sizeof(*state)) ||
       m9x_fragment_overlap(constants,cb,state,sizeof(*state)) ||
       m9x_fragment_overlap(interface,sizeof(*interface),state,sizeof(*state)))
      return false;

   u32 words[28], utility[12] = {0x3f800000,0x3f000000}, n=0, k=blend ? 4 : 2;
   for (u32 i=0;i<6;++i) {
      struct m9x_fragment_form f=m9x_fragment_interpolate[i];
      if (blend) {
         if (i==1 || i==2 || i==4) f.dst+=2;
         if (i==4) { f.a+=2; f.b+=2; }
         if (i==5) { f.dst=0; f.b+=2; f.c+=2; }
      }
      m9x_fragment_emit(words,&n,f);
   }
   u8 scale=blend ? 0 : 0x1b;
   u8 ba=blend ? (alpha ? 8 : 0x0a) : (alpha ? 6 : 0x2d);
   u8 rg=blend ? 0x0b : 0x2d;
   bool alpha_first=blend && (state->blend==M9_FAA_BLEND_ALPHA ||
                            state->blend==M9_FAA_BLEND_ALPHA_ADD);
   struct m9x_fragment_form rg_move={blend ? 0x1c : 0x1d,4,rg,0x40,scale,0x44,0xae0ffc10};
   struct m9x_fragment_form ba_move={blend || alpha ? 0x1c : 0x1e,4,ba,0x42,scale,0x45,0xae0ffc10};
   if (alpha_first || !blend) { m9x_fragment_emit(words,&n,ba_move); m9x_fragment_emit(words,&n,rg_move); }
   else { m9x_fragment_emit(words,&n,rg_move); m9x_fragment_emit(words,&n,ba_move); }
   if (alpha) {
      u32 ref=state->alpha==M9_FAA_ALPHA_GT_ZERO ? 0 : 0x43000000; /* float128 */
      utility[k++]=ref; utility[k++]=ref;
      utility[k++]=0; utility[k++]=0x437f0000; /* alpha conversion: 0,255 */
      /* Original selector18104c reads the immutable 1f6414 pair for key7cf4,
       * modifier1. The epsilon is part of that source conversion, not an
       * invented threshold or a native address. */
      utility[k++]=0x40000001; utility[k++]=0x3f800000;
      u8 temp=blend ? 0 : 0x1b;
      u32 convert=blend ? 0xa040c3c0 : 0xa04083c0;
      m9x_fragment_emit(words,&n,(struct m9x_fragment_form){0x1d,0,temp,0x7f,ba,0x7f,convert});
      m9x_fragment_emit(words,&n,(struct m9x_fragment_form){0x1d,0,temp,0x7f,0x7f,temp,0xa29fccc0});
      m9x_fragment_emit(words,&n,(struct m9x_fragment_form){blend ? 0x1c : 0x1e,blend ? 4 : 0,
                                blend ? 0x0a : 0x2d,0x7f,0x7f,ba,0xa29fff10});
      u8 cmp=state->alpha==M9_FAA_ALPHA_GT_ZERO ? 2 :
                  state->alpha==M9_FAA_ALPHA_LT_HALF ? 1 : 0;
      m9x_fragment_emit(words,&n,(struct m9x_fragment_form){0x1c,cmp,0x40,0x7f,temp,0x7f,
                                  blend ? 0xa3c08000 : 0xa3c04000});
   }
   if (blend) {
      utility[k++]=0; utility[k++]=0x3f800000;
      switch (state->blend) {
      case M9_FAA_BLEND_ALPHA:
         if (alpha) {
            m9x_fragment_emit(words,&n,(struct m9x_fragment_form){0x1d,4,0x2d,0x0b,0x0a,0x18,0xa4bfcdc0});
            m9x_fragment_emit(words,&n,(struct m9x_fragment_form){0x1e,4,0x2d,0x0a,0x0a,0x1a,0xa4bfcdc0});
         } else {
            m9x_fragment_emit(words,&n,(struct m9x_fragment_form){0x1e,4,0x2d,0x0a,0x0a,0x1a,0xa4bfcdc0});
            m9x_fragment_emit(words,&n,(struct m9x_fragment_form){0x1d,4,0x2d,0x0b,0x0a,0x18,0xa4bfcdc0});
         }
         break;
      case M9_FAA_BLEND_ADD:
         m9x_fragment_emit(words,&n,(struct m9x_fragment_form){0x1d,4,0x2d,0x7f,0x0b,0x18,0xa0400c50});
         m9x_fragment_emit(words,&n,(struct m9x_fragment_form){0x1e,4,0x2d,0x7f,0x0a,0x1a,0xa0400c50});
         break;
      case M9_FAA_BLEND_ALPHA_ADD:
         m9x_fragment_emit(words,&n,(struct m9x_fragment_form){0x1e,4,0x2d,0x0a,0x0a,0x1a,0xac5fcf50});
         m9x_fragment_emit(words,&n,(struct m9x_fragment_form){0x1d,4,0x2d,0x0a,0x0b,0x18,0xac5fcf50});
         break;
      case M9_FAA_BLEND_FILTER:
         m9x_fragment_emit(words,&n,(struct m9x_fragment_form){0x1d,4,0x2d,0x18,0x0b,0x7f,0xac3fc140});
         m9x_fragment_emit(words,&n,(struct m9x_fragment_form){0x1e,4,0x2d,0x1a,0x0a,0x7f,0xac3fc140});
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

static inline bool m9x_faa_fragment_state_decode(const u32 *program, u32 bytes,
                                  const u32 *constants, u32 constant_bytes,
                                  struct m9_faa_fragment_state *state,
                                  struct m9_faa_fragment_interface *interface)
{
   if (!program || !constants || !state || !interface ||
       (unsigned long)program % _Alignof(u32) ||
       (unsigned long)constants % _Alignof(u32) ||
       (unsigned long)state % _Alignof(struct m9_faa_fragment_state) ||
       (unsigned long)interface % _Alignof(struct m9_faa_fragment_interface) ||
       bytes > 112 || bytes < 64 || (bytes & 7) ||
       constant_bytes > 48 || constant_bytes < 16 || (constant_bytes & 7) ||
       bytes > (~0UL) - (unsigned long)program ||
       constant_bytes > (~0UL) - (unsigned long)constants ||
       m9x_fragment_overlap(state,sizeof(*state),program,bytes) ||
       m9x_fragment_overlap(interface,sizeof(*interface),program,bytes) ||
       m9x_fragment_overlap(state,sizeof(*state),constants,constant_bytes) ||
       m9x_fragment_overlap(interface,sizeof(*interface),constants,constant_bytes) ||
       m9x_fragment_overlap(state,sizeof(*state),interface,sizeof(*interface))) return false;
   for (unsigned blend=0;blend<=M9_FAA_BLEND_FILTER;blend++)
      for (unsigned alpha=0;alpha<=M9_FAA_ALPHA_GE_HALF;alpha++) {
         struct m9_faa_fragment_state candidate={0,blend,alpha};
         struct m9_faa_fragment_interface found;
         u32 p[28],k[12];
         if (!m9x_faa_fragment_state_build(&candidate,p,sizeof(p),k,sizeof(k),&found) ||
             found.slots*8 != bytes || found.constant_pairs*8 != constant_bytes ||
             memcmp(p,program,bytes) || memcmp(k,constants,constant_bytes)) continue;
         *state=candidate; *interface=found; return true;
      }
   return false;
}

#endif
