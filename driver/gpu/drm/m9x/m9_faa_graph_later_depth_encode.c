/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/errno.h>
#include <linux/string.h>
#include "m9_faa_graph_later_depth_encode.h"
static u32 get(const u8 *p)
{return (u32)p[0]|(u32)p[1]<<8|(u32)p[2]<<16|(u32)p[3]<<24;}
static void put(u8 *p,u32 v)
{p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
static u32 later(u32 old,int extra,bool clear)
{
 u32 wait[3]={840,1316,1720},i,at=old;
 for(i=0;i<3;i++)if(old>=wait[i]+extra)at+=12;
 if(clear&&old>=64)at-=4;
 return at;
}
/* This source-clear path contains the existing graph sentinel's 92-byte
 * b4fe0/b5030 clear, unlike FIRST or initial attachment setup. Its address
 * comes from retained owner8, not from the source's historical GPU base. */
static int prefix(const u8 *sample,size_t sample_bytes,
 const struct m9x_faa_graph_draw_owner *owner,const struct m9x_faa_depth_views *v,
 u64 epoch,struct m9x_faa_later_depth_scratch *s,u8 *out,u32 *bytes,u32 ends[9])
{
 struct m9x_faa_depth_stage_image hz;
 u32 i,at,front,n;
 int ret;
 ret=m9x_faa_depth_stages_encode(v,0x3f800000,&hz);if(ret)return ret;
 ret=m9x_faa_color_attachment_clear_encode(v,(u32)epoch,0x3f800000,&s->attachment);if(ret)return ret;
 ret=m9x_faa_depth_clear_encode(v,(u32)epoch+0x10,0x3f800000,&s->depth);if(ret)return ret;
 ret=m9x_faa_sample_clear_encode(sample,sample_bytes,owner,v,epoch+0x10,s->sample,sizeof(s->sample),&n);if(ret)return ret;
 /* Initialized b5e70/fc29a..fc2a3 installs the retained graph
  * sentinel view in this sample-clear draw, not the old HZ view.
  * f52a5 produces the absent mode-state mask literally zero. */
 put(s->sample+44,1);put(s->sample+48,0x00010004);
 put(s->sample+76,owner[8].base|1);put(s->sample+80,0xf0000008);
 put(s->sample+3020,0);put(s->sample+3104,owner[8].base|1);
 front=s->attachment.end_bytes[1];
 /* Owner profiles are validated before any final-image change. Scratch
  * is not submitted and may be overwritten on a refusal. */
 if(owner[8].bytes<128)return -ERANGE;
 memcpy(out,s->attachment.word,front);ends[0]=s->attachment.end_bytes[0];ends[1]=ends[2]=front;
 at=front;put(out+at,0x4545e3e6);put(out+at+4,1);put(out+at+8,((u32)epoch+0x30)|1);
 for(i=0;i<20;i++)put(out+at+12+i*4,hz.hz[i]);
 put(out+at+20,owner[8].base|1);put(out+at+44,owner[8].base|1);
 put(out+at+48,0xf0000008);put(out+at+56,1);put(out+at+72,0);put(out+at+88,0x00010004);
 at+=92;ends[3]=ends[4]=at;
 /* faba3..fabee emits this source state group after the sentinel clear.
  * It is not an address/program blob and has no allocation-dependent word. */
 {static const u32 state[7]={0x4545e6e6,3,3,0x45a4a3a1,0,0,0};memcpy(out+at,state,sizeof(state));at+=sizeof(state);}
 ends[5]=at;memcpy(out+at,s->depth.word,M9_FAA_DEPTH_CLEAR_BYTES);
 ends[6]=at+M9_FAA_DEPTH_CLEAR_SETUP_BYTES;at+=M9_FAA_DEPTH_CLEAR_BYTES;ends[7]=at;
 memcpy(out+at,s->sample,n);at+=n;ends[8]=at;*bytes=at;return 0;
}

int m9x_faa_later_depth_encode(const u8 *sample,size_t sample_bytes,
 const u8 *first,size_t first_bytes,
 const struct m9x_faa_graph_draw_owner owner[M9_FAA_LATER_DEPTH_OWNERS],
 const struct m9x_faa_depth_views *views,u32 init_bytes,u32 primary_cursor,
 u32 secondary_cursor,u64 previous,u64 next,u32 clear_flags,
 const struct m9x_faa_color_input *input,struct m9x_faa_later_depth_image *out,
 struct m9x_faa_later_depth_scratch *scratch)
{
 u32 size,front=0,clear_ends[9],ends[13],count=0,sec=0,unused=0,at,i,start,last;
 u32 old_bytes,expected,new_bytes,wait[3];int extra,ret;bool clear=clear_flags==3;
 struct m9x_faa_color_next_image tmp;
 u64 epoch=next-0x10,delta=clear?0x150:0xd0;
 if(!owner||!views||!input||!out||!scratch||!out->image.program||!out->image.constants||
  !out->image.primary||!out->image.secondary||!out->image.secondary_length||
  (clear_flags&&clear_flags!=3)||(previous&15)||
  previous>~(u64)0-delta||next!=previous+0x10||(u32)epoch>0xfffffea0u||
  (primary_cursor&3)||(secondary_cursor&3)||
  input->declaration_count<3||input->declaration_count>17||!(input->declaration_count&1))return -EINVAL;
 ret=m9x_faa_graph_draw_owners_validate(owner);if(ret)return ret;
 ret=m9x_faa_depth_stages_encode(views,0x3f800000,&(struct m9x_faa_depth_stage_image){0});if(ret)return ret;
 for(i=0;i<M9_FAA_DEPTH_OWNERS;i++)if(owner[31+i].base!=views->owner[i].base||owner[31+i].bytes<views->owner[i].capacity_bytes)return -EINVAL;
 extra=((int)((input->declaration_count+3)/4)-2)*24;
 expected=2572+extra-(clear?4:0);
 if(secondary_cursor>0x3fe00||expected>0x3fe00-secondary_cursor||
  secondary_cursor>owner[29].bytes||expected>owner[29].bytes-secondary_cursor||
  (u64)owner[29].base+secondary_cursor+expected>0xffffffffu||
  out->image.secondary_bytes<expected)return -ERANGE;
 /* Build the prefix into caller scratch first. No prefix is copied to the
  * final image until every live input and delegated encoder has succeeded. */
 if(clear){
  ret=prefix(sample,sample_bytes,owner,views,epoch,scratch,
   scratch->prefix,&front,clear_ends);if(ret)return ret;
 }
 size=front+3632;
 if(primary_cursor>owner[27].bytes||size>owner[27].bytes-primary_cursor||
  out->image.primary_bytes<size)return -ERANGE;
 tmp=(struct m9x_faa_color_next_image){
  .image={out->image.program,out->image.constants,scratch->tail,scratch->next_secondary,
   out->image.program_bytes,out->image.constants_bytes,sizeof(scratch->tail),sizeof(scratch->next_secondary),&sec},
  .clear_length=&unused,.secondary_scratch=scratch->first_secondary,
  .secondary_scratch_capacity=sizeof(scratch->first_secondary)};
 ret=m9x_faa_color_next_encode(first,first_bytes,owner,init_bytes,secondary_cursor,
  epoch+(clear?0x80:0),input,0,&tmp);if(ret)return ret;
 old_bytes=sec;wait[0]=840+extra;wait[1]=1316+extra;wait[2]=1720+extra;
 /* The initialized depth source retains three actual GPU43 dependencies
  * omitted by the old ordinary COLOR repeat. Produce those groups from
  * each preceding live completion token, never a fake CPU completion. */
 start=0;at=0;
 for(i=0;i<3;i++){
  last=wait[i];memcpy(out->image.secondary+at,scratch->next_secondary+start,last-start);at+=last-start;
  put(out->image.secondary+at,1);put(out->image.secondary+at+4,0x45454543);
  put(out->image.secondary+at+8,get(scratch->next_secondary+last-4)&~15u);at+=12;start=last;
 }
 memcpy(out->image.secondary+at,scratch->next_secondary+start,old_bytes-start);new_bytes=old_bytes+36;
 if(clear){
  /* With the complete preceding clear, E6 has already been entered.
   * Source uses a 16-byte E3 group in place of the no-clear 20-byte E3/E6. */
  memmove(out->image.secondary+60,out->image.secondary+64,new_bytes-64);
  put(out->image.secondary+44,2);put(out->image.secondary+48,0x4545e3e6);put(out->image.secondary+52,1);
  put(out->image.secondary+56,get(scratch->next_secondary+60));new_bytes-=4;
 }
 {u32 slot[3]={28,808+extra,1284+extra},target[3]={764+extra,1248+extra,1664+extra};
  for(i=0;i<3;i++){u32 a=later(slot[i],extra,clear),b=later(target[i],extra,clear);put(out->image.secondary+a,0x49d00000|((b-a)/4));}}
 /* Retain the source FIRST-style null BA view. HZ is an independently
  * retained clear owner; a later frame does not silently attach it here. */
 at=later(1744+extra,extra,clear);put(out->image.secondary+at,get(out->image.secondary+at)&~0x20000000u);
 put(out->image.secondary+later(1900+extra,extra,clear),views->owner[M9_FAA_BOUND_DEPTH].base|1);
 put(out->image.secondary+later(1916+extra,extra,clear),0);
 put(out->image.secondary+later(1920+extra,extra,clear),0xf0010100);
 put(out->image.secondary+later(2024+extra,extra,clear),0x0038038f);
 /* Restore the original final GPU43 preceding the natural Lock footer.
  * The ordinary repeat intentionally omitted it; initialized depth does not. */
 memmove(scratch->tail+3612,scratch->tail+3604,20);
 put(scratch->tail+3604,0x45454543);put(scratch->tail+3608,(u32)(epoch+delta-0x20));
 {u32 range_end=later(1676+extra,extra,clear),pointer_at[4]={4,8,52,56},pointer_end[4]={0,range_end,range_end,new_bytes};
  for(i=0;i<4;i++)put(scratch->tail+pointer_at[i],(owner[29].base+secondary_cursor+pointer_end[i])|(i&1?2:1));}
 if(clear){
  memcpy(out->image.primary,scratch->prefix,front);
  for(i=0;i<9;i++)ends[count++]=primary_cursor+clear_ends[i];
 }
 memcpy(out->image.primary+front,scratch->tail,3632);
 ends[count++]=primary_cursor+front+60;ends[count++]=primary_cursor+size-12;
 ends[count++]=primary_cursor+size-12;ends[count++]=primary_cursor+size;
 out->primary_length=size;out->secondary_length=new_bytes;out->end_count=count;
 memcpy(out->end_bytes,ends,count*4);out->clear_bytes=front;
 out->draw_end_bytes=primary_cursor+front+60;out->final_point=epoch+delta;
 *out->image.secondary_length=new_bytes;return 0;
}
