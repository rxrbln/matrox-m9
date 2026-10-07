/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/errno.h>
#include <linux/string.h>
#include "m9_faa_graph_depth_encode.h"

/* Original FLOATOBJ_SetLong/GetFloat round trips at b591b..b599d. For the
 * positive admitted dimensions this integer encoding is exact, without FPU. */
static u32 m9_faa_dimension_float(u32 n)
{
 u32 exponent=31-__builtin_clz(n);
 return ((exponent+127)<<23)|((n-(1u<<exponent))<<(23-exponent));
}

int m9x_faa_depth_stages_encode(const struct m9x_faa_depth_views *views,
 u32 depth_bits,struct m9x_faa_depth_stage_image *out)
{
 struct m9x_faa_depth_stage_image image;
 const struct m9x_faa_depth_view *hz,*depth;
 u32 i;
 if(!views||!out||depth_bits!=0x3f800000u||
  views->color_policy!=M9_FAA_DEPTH_COLOR_SYSTEM_B||
  views->depth_aux_role!=M9_FAA_DEPTH_HZ||
  views->aux_depth_role!=M9_FAA_BOUND_DEPTH)return -EINVAL;
 for(i=0;i<M9_FAA_DEPTH_OWNERS;i++){
  struct m9x_faa_depth_view expected=m9x_faa_bound_depth_profile(i);
  const struct m9x_faa_depth_view *v=&views->owner[i];
  expected.base=v->base;
  if(!v->base||(v->base&4095)||v->base>0xffffffffu-v->capacity_bytes||
   memcmp(v,&expected,sizeof(expected)))return -EINVAL;
  if(i&&(u64)v->base+v->capacity_bytes>0x40000000u)return -EINVAL;
  if(!i&&v->base<0x40000000u)return -EINVAL;
 }
 hz=&views->owner[M9_FAA_DEPTH_HZ];depth=&views->owner[M9_FAA_BOUND_DEPTH];
 if((u64)hz->base+hz->capacity_bytes>depth->base&&
  (u64)depth->base+depth->capacity_bytes>hz->base)return -EINVAL;
 /* Original HZ writer f8bd0..f8dce and tagged address encoder24932.
  * Policy words are guarded by the exact class5/format78 clear profile. */
 image=(struct m9x_faa_depth_stage_image){
  .hz={0x7a7978f0,0x2800040c,hz->base|1,0xf0000001,0x00010001,
   0x454545f2,0,0xf3f0b1b0,hz->base|1,0xf0000000|hz->storage_width,
   0x2800040c,0,0x92908fe6,0x04000001,0xc0000600,0xffffffff,0xffffffff,
   0x4545fcef,0x000000aa,(hz->allocation_height<<16)|hz->storage_width},
  /* b58fa..b599d: depth clear value and integer rectangle, with X aligned
   * to eight pixels by b578e..b5796. The admitted full width is aligned. */
  .rectangle={0x504e4c48,depth_bits,0,0,0,0x5e4f4c48,depth_bits,0,
   m9_faa_dimension_float(depth->height),m9_faa_dimension_float(depth->width)}
 };
 *out=image;
 return 0;
}
