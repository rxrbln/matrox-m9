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
  (input->declaration_count!=3&&input->declaration_count!=5))return -EINVAL;
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

/* Original initialized N2/N3 edef2ae2 shares these stage slices with the
 * source FIRST extra-DRAW producer. All pointer-bearing words come from a
 * independently live-encoded job, never a captured allocation. */
struct later_copy_span { u16 from, to, bytes; };
/* Original source copies of source-known packet words from a newly encoded
 * live FIRST stage. Address-bearing words thus retain that job's allocation
 * and live C2/CB/geometry metadata, not captured source GPU addresses. */
static const struct later_copy_span later_copies[] = {
	{0,144,28},{32,176,12},{44,200,4},{60,208,44},
	{108,256,196},{308,456,20},{332,480,108},{440,600,4},
	{444,692,176},{636,868,68},{708,940,24},{736,968,8},
	{748,980,44},{796,1028,48},{944,1076,268},
	{1236,1344,80},{1320,1428,24},{1348,1456,8},
	{1360,1468,44},{1408,1516,32},{1516,1548,20},
	{1556,1568,216},{1796,1784,56},{1856,1844,24},
	{1884,1872,8},{1896,1884,56},{1956,1944,92},
	{2052,2040,660},
};
static const u32 later_save_prefix[36] = {
	1,0x45454524,0,3,0x453ce6e6,0x6dc00000,0x42000000,0xa1,
	9,0x44444444,0x30081898,0x30081898,0x30081898,0x30081898,
	0x44444444,0x30081898,0x30081898,0x30081898,0x30081898,
	2,0x45452f2e,0,0,2,0x4545421a,0,0xa1000009,
	2,0x4545421a,0,0xa3000009,3,0x45e6421a,0,0xa4000009,
	0x40008000,
};

static bool overlap(const void *a,u32 as,const void *b,u32 bs)
{unsigned long x=(unsigned long)a,y=(unsigned long)b;return x<=y?y-x<as:x-y<bs;}
int m9x_faa_later_depth_append_encode(
 const struct m9x_faa_graph_draw_owner owner[M9_FAA_LATER_DEPTH_OWNERS],
 const struct m9x_faa_depth_views *views,u32 pstart,u32 sstart,
 u64 previous,u32 clear_flags,u32 raster_flags,u32 count,u32 old_declaration_count,
 const u8 *old,u32 old_bytes,u32 old_secondary_bytes,
 const u32 *old_ends,u32 end_count,u64 old_point,
 const u8 *fresh,u32 fresh_bytes,const struct m9x_faa_color_input *input,
 struct m9x_faa_later_depth_append_image *out)
{
 static const u16 lock_at[]={128,184,320,3084,3548,3568};
 static const u8 lock_back[]={0x20,0x10,0x10,0x10,0x20,0};
 static const u8 lock_tag[]={1,1,0,0,0,1};
 static const u16 epoch_at[]={204,252,964,976,1024,1452,1464,1512,1840,1868,1880,1940,2036};
 static const int epoch_delta[]={-0x20,-0x10,0,0,0x10,0x20,0x20,0x30,0x40,0x50,0x50,0x60,0x70};
 static const u8 epoch_tag[]={1,1,1,0,1,1,0,1,1,1,0,1,1};
 static const u16 save_at[]={8,84,88,100,116,132},save_offset[]={0,640,704,8,8,16};
 static const u8 save_owner[]={22,22,22,8,8,8},save_tag[]={1,5,5,1,5,1};
 u32 i,insert,old_begin,old_mid,cursor,new_bytes,shift,last_shift,append_bytes,min_bytes,max_bytes;u32 cull_bytes=(raster_flags&6)?24:0;u64 delta=clear_flags?0x150:0xd0;
 if(!owner||!views||!old||!old_ends||!fresh||!input||!out||!out->primary||!out->secondary||
  (clear_flags&&clear_flags!=3)||(raster_flags&~7u)||(raster_flags&6)==6||!count||count>31||
  (input->declaration_count!=3&&input->declaration_count!=5)||
  (old_declaration_count!=3&&old_declaration_count!=5)||
  fresh_bytes!=2712-(input->declaration_count==3?24:0)||
  input->layout.program_count!=4||input->record_stride!=32||
  !input->record_count||input->record_count%3||input->record_count>12288||
  (previous&15)||(pstart&3)||(sstart&3)||
  previous>~(u64)0-delta-count*0xa0||
  old_point!=previous+delta+(count-1)*0xa0||(u32)old_point>U32_MAX-0xa0||
  old_bytes<3632||old_bytes>U32_MAX-60||end_count!=(clear_flags?13u:4u))
  return -EINVAL;
 shift=input->declaration_count==3?24:0;last_shift=old_declaration_count==3?24:0;append_bytes=2700-shift;
 max_bytes=(clear_flags?2568:2572)+cull_bytes+(count-1)*2700;
 min_bytes=max_bytes-count*24;
 if(old_secondary_bytes<min_bytes||old_secondary_bytes>max_bytes||
  (max_bytes-old_secondary_bytes)%24||
  (count==1&&old_secondary_bytes!=max_bytes-last_shift)||
  (count>1&&(old_secondary_bytes<min_bytes+24-last_shift||old_secondary_bytes>max_bytes-last_shift)))return -EINVAL;
 if(m9x_faa_graph_draw_owners_validate(owner)||
  m9x_faa_depth_stages_encode(views,0x3f800000,&(struct m9x_faa_depth_stage_image){0}))return -EINVAL;
 for(i=0;i<3;i++)if(owner[31+i].base!=views->owner[i].base||owner[31+i].bytes<views->owner[i].capacity_bytes)return -EINVAL;
 new_bytes=old_bytes+60;cursor=sstart+old_secondary_bytes;
 if(pstart>owner[27].bytes||new_bytes>owner[27].bytes-pstart||
  sstart>0x3fe00||old_secondary_bytes>0x3fe00-sstart||append_bytes>0x3fe00-cursor||
  cursor>owner[29].bytes||append_bytes>owner[29].bytes-cursor||
  out->primary_capacity<new_bytes||out->secondary_capacity<append_bytes||
  (u64)owner[27].base+pstart+new_bytes>U32_MAX||
  (u64)owner[29].base+cursor+append_bytes>U32_MAX)return -ERANGE;
 if(overlap(out->primary,new_bytes,out->secondary,append_bytes)||
  overlap(out->primary,new_bytes,old,old_bytes)||overlap(out->primary,new_bytes,fresh,fresh_bytes)||
  overlap(out->secondary,append_bytes,old,old_bytes)||overlap(out->secondary,append_bytes,fresh,fresh_bytes))return -EINVAL;
 insert=old_bytes-3572;
 old_begin=count==1?sstart:cursor-(2700-last_shift);
 old_mid=(count==1?(clear_flags?1696:1700)+cull_bytes:1828)-last_shift;
 if(get(old+insert)!=0x45454524||
  get(old+insert-60)!=0x45452d2c||get(old+insert-12)!=0x45452d2c||
  get(old+insert-56)!=(owner[29].base+old_begin+1)||
  get(old+insert-52)!=(owner[29].base+old_begin+old_mid+2)||
  get(old+insert-8)!=(owner[29].base+old_begin+old_mid+1)||
  get(old+insert-4)!=(owner[29].base+cursor+2)||
  old_ends[end_count-4]!=pstart+insert||old_ends[end_count-1]!=pstart+old_bytes)
  return -EINVAL;
 for(i=0;i<ARRAY_SIZE(lock_at);i++)if(get(old+insert+lock_at[i])!=(((u32)old_point-lock_back[i])|lock_tag[i]))return -EINVAL;
 /* Source new stages use the same source-proven depth/null-BA contract.
  * Fresh program, masks, interpolation and typed raw owner metadata remain
  * those of this independently encoded job. */
 if(get(fresh+2076-shift)!=(views->owner[2].base|1)||get(fresh+2092-shift)||
  get(fresh+2096-shift)!=0xf0010100||get(fresh+2200-shift)!=0x0038038f||
  (get(fresh+1920-shift)&0x03000000)!=(raster_flags&2?0x03000000:raster_flags&4?0x02000000:0)||
  !!(get(fresh+1936-shift)&0x4000)!=!!(raster_flags&1)||
  input->fragment_flags!=!!(raster_flags&1))return -EINVAL;
 for(i=0;i<ARRAY_SIZE(save_at);i++)if((u64)owner[save_owner[i]].base+save_offset[i]+save_tag[i]>U32_MAX)return -ERANGE;
 memcpy(out->primary,old,insert);memcpy(out->primary+insert,old+insert-60,60);
 put(out->primary+insert+4,owner[29].base+cursor+1);put(out->primary+insert+8,owner[29].base+cursor+1830-shift);
 put(out->primary+insert+52,owner[29].base+cursor+1829-shift);put(out->primary+insert+56,owner[29].base+cursor+append_bytes+2);
 memcpy(out->primary+insert+60,old+insert,3572);
 for(i=0;i<ARRAY_SIZE(lock_at);i++)put(out->primary+insert+60+lock_at[i],((u32)old_point+0xa0-lock_back[i])|lock_tag[i]);
 memset(out->secondary,0,append_bytes);
 for(i=0;i<ARRAY_SIZE(later_save_prefix);i++)put(out->secondary+4*i,later_save_prefix[i]);
 for(i=0;i<ARRAY_SIZE(save_at);i++)put(out->secondary+save_at[i],(owner[save_owner[i]].base+save_offset[i])|save_tag[i]);
 for(i=0;i<ARRAY_SIZE(later_copies);i++){
  u32 from=later_copies[i].from,to=later_copies[i].to,bytes=later_copies[i].bytes;
  if(shift&&from==444&&to==692)bytes-=shift;
  else if(shift&&from>=576)from-=shift;
  if(shift&&to>=868)to-=shift;
  memcpy(out->secondary+to,fresh+from,bytes);
 }
 memcpy(out->secondary+588,fresh+2432-shift,104);
 put(out->secondary+172,0x49d00000|((892-shift-172)/4));put(out->secondary+188,3);
 put(out->secondary+192,0x45e3e6e6);put(out->secondary+196,1);
 for(i=0;i<ARRAY_SIZE(epoch_at);i++)put(out->secondary+epoch_at[i]-(epoch_at[i]>=868?shift:0),((u32)old_point+epoch_delta[i])|epoch_tag[i]);
 put(out->secondary+452,((get(fresh+304)&0xff1fffff)|0x00100000)&0xfffffffc);
 put(out->secondary+476,get(fresh+328));put(out->secondary+936-shift,0x49d00000|((1388-936)/4));
 put(out->secondary+1424-shift,0x49d00000|((1816-1424)/4));
 memcpy(out->end_bytes,old_ends,end_count*4);
 for(i=end_count-4;i<end_count;i++)out->end_bytes[i]+=60;
 out->primary_bytes=new_bytes;out->secondary_bytes=append_bytes;out->end_count=end_count;
 out->draw_end_bytes=pstart+insert+60;out->final_point=old_point+0xa0;return 0;
}
