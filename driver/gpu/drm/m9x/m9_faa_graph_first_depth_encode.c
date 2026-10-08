/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/errno.h>
#include <linux/string.h>
#include "m9_faa_graph_first_depth_encode.h"

static void put(u8 *p,u32 v)
{p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}

int m9x_faa_first_depth_prefix_encode(const u8 *blob,size_t bytes,
 const struct m9x_faa_graph_draw_owner *owner,const struct m9x_faa_depth_views *views,
 u64 init_point,u8 *primary,size_t capacity,u32 *length,u32 *ends,
 struct m9x_faa_first_depth_scratch *scratch)
{
 struct m9x_faa_depth_stage_image hz;
 const struct m9x_faa_depth_view *color;
 u32 e[M9_FAA_FIRST_DEPTH_PREFIX_ENDS],page,last,count,at,i,sample_bytes;
 int ret;
 if(!scratch||!primary||!length||!ends||!owner||!views||
  (init_point&15)||init_point<0xb0||init_point>~(u64)0-0x150||
  (u32)init_point>0xfffffeafu)return -EINVAL;
 ret=m9x_faa_graph_draw_owners_validate(owner);if(ret)return ret;
 ret=m9x_faa_depth_stages_encode(views,0x3f800000,&hz);if(ret)return ret;
 ret=m9x_faa_depth_clear_encode(views,(u32)init_point,0x3f800000,&scratch->depth);if(ret)return ret;
 ret=m9x_faa_sample_clear_encode(blob,bytes,owner,views,init_point,
  scratch->sample,sizeof(scratch->sample),&sample_bytes);if(ret)return ret;
 if(owner[8].bytes<128)return -ERANGE;
 color=&views->owner[M9_FAA_BOUND_COLOR];page=color->base>>12;last=page+color->logical_bytes/4096;
 count=((last+7)>>3)-(page>>3);
 /* Source FIRST removes one unused C0 pair from each constructor. The
  * fresh target slot starts disabled and HZ's default null descriptor is
  * independent of the attached depth allocation. These are initialized
  * state policy, not copied address-like source data. */
 scratch->depth.word[44]=0x00080300;
 scratch->depth.word[127]=0;
 memmove(scratch->depth.word+146,scratch->depth.word+148,
  M9_FAA_DEPTH_CLEAR_BYTES-148*4);
 put(scratch->sample+5*4,0x2e00040c);
 memset(scratch->sample+776*4,0,12);
 put(scratch->sample+856*4,0x2e00040c);
 memmove(scratch->sample+793*4,scratch->sample+795*4,sample_bytes-795*4);
 sample_bytes-=8;
 at=64+(56+count*4)+52+(M9_FAA_DEPTH_CLEAR_BYTES-8)+sample_bytes+64;
 if(at>capacity)return -ERANGE;
 /* First HZ constructor: source 13-word class5/format78 body with the
  * live parent allocation and visible/storage dimensions, preceded by c1. */
 put(primary,0x4545e3e6);put(primary+4,1);put(primary+8,((u32)init_point+0x10)|1);
 memcpy(primary+12,hz.hz+7,13*4);
 put(primary+24,0x2e00040c);put(primary+44,0xffff0000);
 e[0]=e[1]=at=64;
 /* Fresh SYSTEM bound color has the short page-clear state. Generate
  * every virtual page group from its admitted flags30/B owner. */
 {static const u32 control[9]={0x45454570,0,0x4545ace6,2,0xffffffff,
  0x45e62b2a,0x454545e6,0xffff,0};
  memcpy(primary+at,control,sizeof(control));put(primary+at+32,0x20000000|(count-1));at+=36;}
 while(page<last){u32 group=page&~7u,end=group+8<last?group+8:last;
  put(primary+at,((group|0xd00000u)<<5)|(((1u<<(end-page))-1)<<(page&7)));
  at+=4;page=end;}
 put(primary+at,0x4545e3e6);put(primary+at+4,1);put(primary+at+8,((u32)init_point+0x20)|1);
 put(primary+at+12,0x454545e6);put(primary+at+16,1);at+=20;e[2]=at;
 memcpy(primary+at,hz.hz+7,13*4);put(primary+at+12,0x2e00040c);at+=52;e[3]=e[4]=at;
 memcpy(primary+at,scratch->depth.word,M9_FAA_DEPTH_CLEAR_BYTES-8);
 e[5]=at+M9_FAA_DEPTH_CLEAR_SETUP_BYTES-8;at+=M9_FAA_DEPTH_CLEAR_BYTES-8;e[6]=at;
 memcpy(primary+at,scratch->sample,sample_bytes);at+=sample_bytes;e[7]=at;
 /* Fresh sampled clear closes with the graph sentinel's own 4x1/128B
  * HZ-style constructor. It is not the new bound depth/HZ allocation. */
 put(primary+at,0x4545e3e6);put(primary+at+4,1);put(primary+at+8,((u32)init_point+0x80)|1);
 memcpy(primary+at+12,hz.hz+7,13*4);put(primary+at+16,owner[8].base|1);
 put(primary+at+20,0xf0000008);put(primary+at+24,0x2e00040c);
 put(primary+at+28,1);put(primary+at+44,0);put(primary+at+60,0x00010004);
 at+=64;e[8]=e[9]=at;
 for(i=0;i<M9_FAA_FIRST_DEPTH_PREFIX_ENDS;i++)ends[i]=e[i];
 *length=at;return 0;
}

int m9x_faa_first_depth_encode(const u8 *sample,size_t sample_bytes,
 const u8 *first,size_t first_bytes,
 const struct m9x_faa_graph_draw_owner *owner,const struct m9x_faa_depth_views *views,
 u32 init_bytes,u64 init_point,const struct m9x_faa_color_input *input,
 u8 *primary,size_t primary_capacity,u8 *secondary,size_t secondary_capacity,
 u32 *primary_bytes,u32 *secondary_bytes,u32 *ends,
 const struct m9x_faa_color_image *first_scratch,
 struct m9x_faa_first_depth_scratch *prefix_scratch)
{
 u32 length,sec,i,ef,shift;
 int ret;
 if(!owner||!primary||!secondary||!primary_bytes||!secondary_bytes||!ends||!first_scratch||
  !first_scratch->secondary_length||!prefix_scratch||!input||
  input->layout.program_count!=4||
  (input->declaration_count!=3&&input->declaration_count!=5)||
  init_point>~(u64)0-0x150||(u32)init_point>0xfffffeafu||
  init_bytes>owner[27].bytes||owner[27].bytes-init_bytes<
   m9x_faa_first_depth_prefix_bound(views)+M9_FAA_COLOR_PRIMARY_BYTES-64||
  primary_capacity<m9x_faa_first_depth_prefix_bound(views)+M9_FAA_COLOR_PRIMARY_BYTES-64||
  secondary_capacity<M9_FAA_COLOR_SECONDARY_CAPACITY)return -EINVAL;
 if(input->depth_flags&&!m9x_faa_depth_flags_valid(input->depth_flags))return -EINVAL;
 ret=m9x_faa_depth_stages_encode(views,0x3f800000,
  &(struct m9x_faa_depth_stage_image){0});if(ret)return ret;
 /* The normal live encoder generates GPU transforms, shader entries,
  * ranges and declarations. Its first-only head is replaced by the genuine
  * fresh bound-clear prefix. Remaining epochs begin after source point130. */
 ret=m9x_faa_color_four_encode(first,first_bytes,owner,init_bytes,
  init_point+0x70,input,first_scratch);if(ret)return ret;
 sec=*first_scratch->secondary_length;
 shift=input->declaration_count==3?24:0;
 if(sec!=2712-shift||sec>secondary_capacity)return -EINVAL;
 ret=m9x_faa_first_depth_prefix_encode(sample,sample_bytes,owner,views,init_point,
  primary,primary_capacity,&length,ends,prefix_scratch);if(ret)return ret;
 memcpy(primary+length,first_scratch->primary+64,M9_FAA_COLOR_PRIMARY_BYTES-64);
 memcpy(secondary,first_scratch->secondary,sec);
 /* Original 181fa0 ZENABLE mode lowers EF's depth mode bit. A8 separately
  * lowers the live enable/write/comparison state, with format3d validated above. */
 ef=(u32)secondary[1920-shift]|(u32)secondary[1921-shift]<<8|
  (u32)secondary[1922-shift]<<16|(u32)secondary[1923-shift]<<24;
 put(secondary+1920-shift,ef&~0x20000000u);
 put(secondary+2076-shift,views->owner[M9_FAA_BOUND_DEPTH].base|1);
 /* FIRST's 12e75a constructor has no BA/HZ attachment in this slot. It
  * initializes the default null G16R16 view; the bound HZ owner remains
  * separately retained by the preceding clear producers. */
 put(secondary+2092-shift,0);put(secondary+2096-shift,0xf0010100);
 put(secondary+2200-shift,m9x_faa_depth_a8(input->depth_flags));
 for(i=0;i<M9_FAA_FIRST_DEPTH_PREFIX_ENDS;i++)ends[i]+=init_bytes;
 ends[10]=init_bytes+length+60;
 ends[11]=ends[12]=init_bytes+length+M9_FAA_COLOR_PRIMARY_BYTES-64-12;
 ends[13]=init_bytes+length+M9_FAA_COLOR_PRIMARY_BYTES-64;
 *primary_bytes=length+M9_FAA_COLOR_PRIMARY_BYTES-64;*secondary_bytes=sec;
 return 0;
}

int m9x_faa_first_depth_color_stage_encode(const u8 *first,size_t first_bytes,
 const struct m9x_faa_graph_draw_owner *owner,const struct m9x_faa_depth_views *views,
 u32 init_bytes,u64 init_point,const struct m9x_faa_color_input *input,
 u8 *primary,size_t primary_capacity,u8 *secondary,size_t secondary_capacity,
 u32 *primary_bytes,u32 *secondary_bytes,
 const struct m9x_faa_color_image *first_scratch)
{
 struct m9x_faa_depth_stage_image validated;
 u32 sec,tail,ef_at,depth_at,aux_at,a8_at;bool two;int ret;
 if(!owner||!views||!input||!primary||!secondary||!primary_bytes||!secondary_bytes||
  !first_scratch||!first_scratch->secondary_length||
  (input->layout.program_count!=2&&input->layout.program_count!=4)||
  (input->declaration_count!=3&&input->declaration_count!=5)||
  init_point>~(u64)0-0x150||(u32)init_point>0xfffffeafu||
  (input->depth_flags&&!m9x_faa_depth_flags_valid(input->depth_flags)))return -EINVAL;
 two=input->layout.program_count==2;tail=two?3588u:3632u;
 sec=two?(input->declaration_count==3?1352u:1376u):
         (input->declaration_count==3?2688u:2712u);
 if(primary_capacity<tail||secondary_capacity<sec)return -ERANGE;
 ret=m9x_faa_depth_stages_encode(views,0x3f800000,&validated);if(ret)return ret;
 ret=(two?m9x_faa_color_encode:m9x_faa_color_four_encode)(first,first_bytes,
  owner,init_bytes,init_point+0x70,input,first_scratch);if(ret)return ret;
 if(*first_scratch->secondary_length!=sec)return -EINVAL;
 /* These bytes are exactly the live full-FIRST stage after its clear
  * prefix. Range operands and all shader/data metadata still come from
  * the live color encoder; the unused FIRST-only64-byte head is omitted. */
 memcpy(primary,first_scratch->primary+64,tail);
 memcpy(secondary,first_scratch->secondary,sec);
 if(two){u32 extra=input->declaration_count==5?24:0;
  ef_at=660+extra;depth_at=816+extra;aux_at=832+extra;a8_at=940+extra;
 }else{u32 shift=input->declaration_count==3?24:0;
  ef_at=1920-shift;depth_at=2076-shift;aux_at=2092-shift;a8_at=2200-shift;}
 put(secondary+ef_at,((u32)secondary[ef_at]|(u32)secondary[ef_at+1]<<8|
  (u32)secondary[ef_at+2]<<16|(u32)secondary[ef_at+3]<<24)&~0x20000000u);
 put(secondary+depth_at,views->owner[M9_FAA_BOUND_DEPTH].base|1);
 put(secondary+aux_at,0);put(secondary+aux_at+4,0xf0010100);
 put(secondary+a8_at,m9x_faa_depth_a8(input->depth_flags));
 *primary_bytes=tail;*secondary_bytes=sec;return 0;
}
