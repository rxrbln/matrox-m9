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
  struct m9x_faa_depth_view expected=m9x_faa_bound_depth_profile_dimensions(i,
   views->owner[M9_FAA_BOUND_COLOR].width,views->owner[M9_FAA_BOUND_COLOR].height);
  const struct m9x_faa_depth_view *v=&views->owner[i];
  expected.base=v->base;
  if(!expected.logical_bytes||!v->base||(v->base&4095)||v->base>0xffffffffu-v->capacity_bytes||
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

/* The bounded class4/format3d clear constructor b5630 installs this inline
 * program through dc9e0/a1eb0 -> a2e90/a28d0. These are CPU-known ISA words,
 * not an application shader or allocation-dependent program address. */
int m9x_faa_depth_clear_encode(const struct m9x_faa_depth_views *views,
 u32 prior,u32 depth_bits,struct m9x_faa_depth_clear_image *out)
{
 static const u32 setup[54]={
  0x4545e3e6,1,0,0x45d7eff0,0xc0000600,0x08000055,0,0xf4f3f2ec,
  0,0,0,0,0x45454522,0,0x4545e3e6,1,0,0x4545b1b0,0,0,
  0x4545b9b8,0,0,0xadadadad,32,32,32,32,0x454545af,0,
  0x45aaa9a8,0,0,0,0x45aeacab,0xffffffff,0xffffffff,0x22444442,
  0xc5c4f6f5,0,0x7fff7fff,0,0,0xc18a8988,0x00180300,0xfe007ffa,
  0,0,0x4545e3e6,0x04000001,0,0x4545c3c2,4,0
 };
 static const u32 instruction[7][2]={
  {0x5fff841d,0xac80000f},{0xe13f851d,0xa2c0730f},
  {0xe17f83bc,0xa3c0430f},{0xffff9100,0xee80c31f},
  {0xffff9300,0xae80c31f},{0xffff9110,0xae81031f},
  {0xffff9310,0xae81031f}
 };
 /* a1eb0 clear constructor constant pairs: numerical ISA inputs. Last
  * pair uses the admitted live depth width (FLOATOBJ conversion). */
 static const u32 constant[5][2]={
  {0x45fffc00,0x46000000},{0x40000000,0x46001000},
  {0x467fe000,0x46000c00},{0x3f7fff00,0x3f7fff00},{0,0}
 };
 struct m9x_faa_depth_stage_image stages;
 struct m9x_faa_depth_clear_image image={0};
 u32 *w=image.word,i,at;
 if(!out||(prior&15)||prior>0xffffffffu-0x40u||
  m9x_faa_depth_stages_encode(views,depth_bits,&stages))return -EINVAL;
 memcpy(w,setup,sizeof(setup));
 w[2]=(prior+0x30)|1;w[16]=(prior+0x40)|1;
 w[18]=views->owner[M9_FAA_BOUND_DEPTH].base|1;
 /* Original target descriptor b5880/fbb19; bounded profile format and
  * visible width select the source 7022 descriptor family. */
 w[19]=0x70220000|views->owner[M9_FAA_BOUND_DEPTH].width;
 w[41]=w[42]=0x40004000;w[50]=prior+0x40;
 at=54;
 for(i=0;i<7;i++){
  w[at++]=0x4591908f+i*0x00020200;w[at++]=0x80000000;
  w[at++]=instruction[i][0];w[at++]=instruction[i][1];
 }
 w[82]=0x8f8e8d8c;
 w[87]=0x4545e3e6;w[88]=0x04000001;w[89]=prior+0x40;
 w[90]=0x459f9e8f;w[91]=0xc00007c0;w[92]=0;w[93]=depth_bits;
 for(i=0;i<6;i++){
  at=94+4*i;w[at]=0x4591908f+i*0x00020200;w[at+1]=0xc0000600;
  if(i<5){w[at+2]=constant[i][0];w[at+3]=constant[i][1];}
  else w[at+3]=m9_faa_dimension_float(views->owner[M9_FAA_BOUND_DEPTH].width);
 }
 for(i=0;i<4;i++)w[118+2*i]=0x45454574+i;
 w[126]=0x45454570;w[127]=0x00040118;
 /* Reset the four target state slots, exactly the b5630 inline clear
  * constructor. All absent words are source-known zero state fields. */
 for(i=0;i<4;i++){
  at=i?148+(i-1)*20:128;
  if(i)w[at]=0x45454570+i;
  if(i)at+=2;
  w[at]=0x457a7978;w[at+4]=0x45877d7c;
  w[at+8]=0x85848382;w[at+13]=0x7f7e8180;
 }
 w[146]=0x454545c0;w[147]=4;
 memcpy(w+M9_FAA_DEPTH_CLEAR_SETUP_BYTES/4,stages.rectangle,sizeof(stages.rectangle));
 *out=image;
 return 0;
}

/* Source resource creation/setup plus bound color/HZ/depth clear. The
 * source color resource is SYSTEM flags30/PTE-b; the b5e70 page indices
 * encode its GPU virtual range rather than a physical MDL/PFN list. */
int m9x_faa_attachment_clear_encode(const struct m9x_faa_depth_views *views,
 u32 prior,u32 depth_bits,struct m9x_faa_attachment_clear_image *out)
{
 struct m9x_faa_depth_stage_image stages;
 struct m9x_faa_attachment_clear_image image={0};
 const struct m9x_faa_depth_view *color;
 u32 *w=image.word,at,page,last,n=0,i,color_end,hz_end;
 if(!out||(prior&15)||prior>0xffffffffu-0x40u||
  m9x_faa_depth_stages_encode(views,depth_bits,&stages))return -EINVAL;
 color=&views->owner[M9_FAA_BOUND_COLOR];
 if((color->logical_bytes&4095)||!color->logical_bytes||
  (((color->base>>12)+(color->logical_bytes>>12)+7)/8-(color->base>>15))>97)
  return -EINVAL;
 /* a3870-created HZ child's first initialization precedes all clear ENDs.
  * Source output 8672..8768; early E3 observes prior+10. */
 w[0]=0x45e3e6e6;w[1]=1;w[2]=2;w[3]=(prior+0x10)|1;
 memcpy(w+4,stages.hz,sizeof(stages.hz));
 w[4+15]=0xffff0000; /* source class5/format78 initial fill */
 at=24;
 w[at++]=0x45454570;w[at++]=0;w[at++]=0x457a7978;
 w[at++]=views->owner[M9_FAA_DEPTH_HZ].base|1;
 w[at++]=0xf0000001;w[at++]=0x00010001;
 w[at++]=0x4545ace6;w[at++]=2;w[at++]=0xffffffff;
 w[at++]=0x45e62b2a;w[at++]=0x454545e6;w[at++]=0xffff;
 /* b60ab stores word_count-1. Generate each selected live eight-page
  * group, including first/last partial masks; never retain captured pages. */
 at++;page=color->base>>12;last=page+color->logical_bytes/4096;
 while(page<last){
  u32 group=page&~7u,end=group+8<last?group+8:last;
  u32 mask=((1u<<(end-page))-1)<<(page&7);
  w[at++]=((group|0xd00000u)<<5)|mask;n++;page=end;
 }
 w[36]=0x20000000|(n-1);
 w[at++]=0x4545e3e6;w[at++]=1;w[at++]=(prior+0x20)|1;
 w[at++]=0x454545e6;w[at++]=1;
 color_end=at*4;
 for(i=0;i<20;i++)w[at++]=stages.hz[i];
 hz_end=at*4;
 if(m9x_faa_depth_clear_encode(views,prior,depth_bits,
  (struct m9x_faa_depth_clear_image *)(w+at)))return -EINVAL;
 at+=M9_FAA_DEPTH_CLEAR_BYTES/4;
 image.bytes=at*4;
 image.end_bytes[0]=color_end;
 image.end_bytes[1]=image.end_bytes[2]=hz_end;
 image.end_bytes[3]=hz_end+M9_FAA_DEPTH_CLEAR_SETUP_BYTES;
 image.end_bytes[4]=image.bytes;
 *out=image;
 return 0;
}

/* Matching retained COLOR4 clear: the separate SYSTEM color's page clear
 * precedes HZ initialization. This is not the public-VS creation prologue:
 * f5227..f56ae emits nine controls, then b5fc3's page-run family. The source
 * initializes HZ with its E3/E6 header only after that first color END. */
int m9x_faa_color_attachment_clear_encode(const struct m9x_faa_depth_views *views,
 u32 queue_start,u32 depth_bits,struct m9x_faa_attachment_clear_image *out)
{
 static const u32 prefix[15]={
  0x45458fe6,0x04000001,0x80000600,0x45929190,0,0,0xffffffff,
  0x45454570,0,0x4545ace6,2,0xffffffff,0x45e62b2a,0x454545e6,0xffff
 };
 struct m9x_faa_depth_stage_image stages;
 struct m9x_faa_attachment_clear_image image={0};
 const struct m9x_faa_depth_view *color;
 u32 *w=image.word,at=16,page,last,n=0,i,color_end,hz_end;
 if(!out||(queue_start&15)||queue_start>0xffffffffu-0x40u||
  m9x_faa_depth_stages_encode(views,depth_bits,&stages))return -EINVAL;
 color=&views->owner[M9_FAA_BOUND_COLOR];
 if((color->logical_bytes&4095)||!color->logical_bytes||
  (((color->base>>12)+(color->logical_bytes>>12)+7)/8-(color->base>>15))>97)
  return -EINVAL;
 memcpy(w,prefix,sizeof(prefix));
 page=color->base>>12;last=page+color->logical_bytes/4096;
 while(page<last){
  u32 group=page&~7u,end=group+8<last?group+8:last;
  u32 mask=((1u<<(end-page))-1)<<(page&7);
  w[at++]=((group|0xd00000u)<<5)|mask;n++;page=end;
 }
 w[15]=0x20000000|(n-1);
 w[at++]=0x4545e3e6;w[at++]=1;w[at++]=(queue_start+0x10)|1;
 w[at++]=0x454545e6;w[at++]=1;color_end=at*4;
 w[at++]=0x45e3e6e6;w[at++]=1;w[at++]=2;w[at++]=(queue_start+0x20)|1;
 for(i=0;i<20;i++)w[at++]=stages.hz[i];
 hz_end=at*4;
 if(m9x_faa_depth_clear_encode(views,queue_start,depth_bits,
  (struct m9x_faa_depth_clear_image *)(w+at)))return -EINVAL;
 at+=M9_FAA_DEPTH_CLEAR_BYTES/4;
 image.bytes=at*4;image.end_bytes[0]=color_end;
 image.end_bytes[1]=image.end_bytes[2]=hz_end;
 image.end_bytes[3]=hz_end+M9_FAA_DEPTH_CLEAR_SETUP_BYTES;
 image.end_bytes[4]=image.bytes;
 *out=image;return 0;
}
