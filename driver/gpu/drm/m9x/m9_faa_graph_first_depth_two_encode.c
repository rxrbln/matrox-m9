/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/errno.h>
#include <linux/string.h>
#include "m9_faa_graph_first_depth_two_encode.h"
static u32 word(const u8*p){return (u32)p[0]|(u32)p[1]<<8|(u32)p[2]<<16|(u32)p[3]<<24;}
static void put(u8*p,u32 v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
int m9x_faa_first_depth_two_encode(const u8 *sample,size_t sample_bytes,
 const u8 *first,size_t first_bytes,
 const struct m9x_faa_graph_draw_owner *owner,const struct m9x_faa_depth_views *views,
 u32 init_bytes,u64 init_point,const struct m9x_faa_color_input *input,
 u8 *primary,size_t primary_capacity,u8 *secondary,size_t secondary_capacity,
 u32 *primary_bytes,u32 *secondary_bytes,u32 *ends,
 const struct m9x_faa_color_image *first_scratch,
 struct m9x_faa_first_depth_scratch *prefix_scratch)
{
 u32 length,sec,extra,i,expected;
 int ret;
 if(!owner||!views||!input||!primary||!secondary||!primary_bytes||!secondary_bytes||
  !ends||!first_scratch||!first_scratch->secondary_length||!prefix_scratch||
  input->layout.program_count!=2||
  (input->declaration_count!=3&&input->declaration_count!=5)||
  init_point>~(u64)0-0x110||(u32)init_point>0xfffffeefu||
  init_bytes>owner[27].bytes||owner[27].bytes-init_bytes<M9_FAA_FIRST_DEPTH_PREFIX_CAPACITY+3588||
  primary_capacity<M9_FAA_FIRST_DEPTH_PREFIX_CAPACITY+3588||secondary_capacity<1352+(input->declaration_count==5?24:0))
  return -EINVAL;
 if(input->depth_flags&&!m9x_faa_depth_flags_valid(input->depth_flags))return -EINVAL;
 ret=m9x_faa_depth_stages_encode(views,0x3f800000,&(struct m9x_faa_depth_stage_image){0});
 if(ret)return ret;
 ret=m9x_faa_color_encode(first,first_bytes,owner,init_bytes,init_point+0x70,input,first_scratch);
 if(ret)return ret;
 extra=input->declaration_count==5?24:0;expected=1352+extra;
 sec=*first_scratch->secondary_length;
 if(sec!=expected)return -EINVAL;
 ret=m9x_faa_first_depth_prefix_encode(sample,sample_bytes,owner,views,init_point,
  primary,primary_capacity,&length,ends,prefix_scratch);
 if(ret)return ret;
 memcpy(primary+length,first_scratch->primary+64,3588);
 memcpy(secondary,first_scratch->secondary,sec);
 /* Original count2 FIRST 181fa0: EF enable bit, A9 bound depth,
  * default null BA view from12e75a, and A8 live comparison/write state. */
 put(secondary+660+extra,word(secondary+660+extra)&~0x20000000u);
 put(secondary+816+extra,views->owner[M9_FAA_BOUND_DEPTH].base|1);
 put(secondary+832+extra,0);put(secondary+836+extra,0xf0010100);
 put(secondary+940+extra,m9x_faa_depth_a8(input->depth_flags));
 for(i=0;i<M9_FAA_FIRST_DEPTH_PREFIX_ENDS;i++)ends[i]+=init_bytes;
 ends[10]=init_bytes+length+24;
 ends[11]=ends[12]=init_bytes+length+3588-12;
 ends[13]=init_bytes+length+3588;
 *primary_bytes=length+3588;*secondary_bytes=sec;
 return 0;
}
