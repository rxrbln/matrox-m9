/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_DEPTH_MODE_H
#define M9_FAA_DEPTH_MODE_H
#include <linux/errno.h>
#include <linux/types.h>
#include "m9_faa_depth_state.h"
#include "m9_faa_graph_draw_encode.h"
#include "m9_faa_graph_depth.h"
/* Explicit application disable, distinct from flags0's historical internal
 * fixture default. Enable/write/compare retain their existing ABI meanings. */
#define M9_FAA_DEPTH_DISABLED 32u
struct m9x_faa_depth_mode_state {
 bool valid, enabled, embedded_hz;
};
struct m9x_faa_depth_mode_image {
 u32 ef, a9, ba, ba_descriptor, a8;
 /* Original count2 implicit association reset, not application CLEAR and
  * not a receipt. Caller must encode/post its body before accepting state. */
 u32 reset_delta;
 struct m9x_faa_depth_mode_state after;
};
static inline bool m9x_faa_depth_mode_flags_valid(u32 flags)
{
 return m9x_faa_depth_flags_valid(flags) ||
  ((flags & M9_FAA_DEPTH_DISABLED) && !(flags & 1u) && !(flags & ~63u));
}
/* Pure count2 state preparation. This does not publish state, post a reset,
 * admit a public request, or claim that its requested point has retired.
 * Primary embedded companion/HZ are retained graph roles4/5; separately
 * bound depth is views role2. Never derive ownership from numeric overlap. */
static inline int m9x_faa_depth_mode_two_plan(u32 flags, u32 ef,
 const struct m9x_faa_depth_mode_state *before,
 const struct m9x_faa_graph_draw_owner *owner,
 const struct m9x_faa_depth_views *views,
 struct m9x_faa_depth_mode_image *out)
{
 struct m9x_faa_depth_mode_image image={0};
 struct m9x_faa_surface_layout layout;
 bool enabled;
 if(!owner||!views||!out||!before||
  !m9x_faa_depth_mode_flags_valid(flags)||
  (before->valid&&!before->enabled&&!before->embedded_hz)||
  (!before->valid&&(before->enabled||before->embedded_hz))||
  !m9x_faa_surface_layout(views->owner[0].width,views->owner[0].height,&layout))
  return -EINVAL;
 enabled=!!(flags&1u);
 image.after=(struct m9x_faa_depth_mode_state){true,enabled,before->embedded_hz||!enabled};
 if(!views->owner[2].base||(views->owner[2].base&4095)||
  views->owner[2].base>=0x40000000u)return -EINVAL;
 if(image.after.embedded_hz){
  const u32 roles[2]={4,5};
  const u32 bytes[2]={layout.owner[M9_FAA_SURFACE_COMPANION].capacity,
   layout.owner[M9_FAA_SURFACE_HZ].capacity};
  for(u32 i=0;i<2;i++){
   const struct m9x_faa_graph_draw_owner *v=&owner[roles[i]];
   if(!v->base||(v->base&4095)||v->base>=0x40000000u||
    v->bytes<bytes[i]||(u64)v->base+v->bytes>0x40000000u)return -ERANGE;
  }
  if((u64)owner[4].base+owner[4].bytes>owner[5].base&&
   (u64)owner[5].base+owner[5].bytes>owner[4].base)return -ERANGE;
 }
 image.ef=(ef&~0x20000000u)|(enabled?0:0x20000000u);
 image.a9=(enabled?views->owner[2].base:owner[4].base)|1u;
 image.ba=image.after.embedded_hz?owner[5].base|1u:0;
 image.ba_descriptor=image.after.embedded_hz?0xf0010000u|layout.width:0xf0010100u;
 image.a8=m9x_faa_depth_a8(flags);
 image.reset_delta=before->valid&&before->enabled!=enabled?0x50:0;
 *out=image;
 return 0;
}
#endif
