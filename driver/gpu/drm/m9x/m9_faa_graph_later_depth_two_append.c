/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/overflow.h>
#include "m9_faa_graph_later_depth_two_encode.h"
static u32 word(const u8*p){return (u32)p[0]|(u32)p[1]<<8|(u32)p[2]<<16|(u32)p[3]<<24;}
static void put(u8*p,u32 v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
static u32 moved(u32 at,u32 extra){return at>=508?at+extra:at;}
int m9x_faa_later_depth_two_append_encode(
 const struct m9x_faa_graph_draw_owner owner[34],const struct m9x_faa_depth_views*views,
 u32 pstart,u32 sstart,u64 previous,u32 clear_flags,u32 raster_flags,u32 count,u32 old_decl,
 const u8*old,u32 old_bytes,u32 old_sec,const u32*old_ends,u32 end_count,u64 old_point,
 const u8*s,u32 fresh_bytes,const struct m9x_faa_color_input*input,
 struct m9x_faa_later_depth_append_image*out)
{
 static const u32 insert[36]={1,0x45454524,0,3,0x453ce6e6,0x6dc00000,0x42000000,0xa1,
  9,0x44444444,0x30081898,0x30081898,0x30081898,0x30081898,0x44444444,
  0x30081898,0x30081898,0x30081898,0x30081898,2,0x45452f2e,0,0,
  2,0x4545421a,0,0xa1000009,2,0x4545421a,0,0xa3000009,
  3,0x45e6421a,0,0xa4000009,0x40008000};
 static const u32 sec_at[7]={12,60,592,620,632,692,788};
 static const u32 sec_delta[7]={0x90,0xa0,0xb0,0xc0,0xc0,0xd0,0xe0};
 static const u32 sec_tag[7]={1,1,1,1,0,1,1};
 static const u32 tail_at[6]={128,184,320,3084,3540,3560};
 static const u32 tail_back[6]={0x20,0x10,0x10,0x10,0x20,0};
 static const u32 tail_tag[6]={1,1,0,0,0,1};

 u32 extra,old_extra,newsec,newprimary,split,minsec,maxsec,first,last,delta,at,i,ends[13],front;
 u64 point,base;bool clear=clear_flags==3;
 if(!owner||!views||!old||!s||!old_ends||!input||!out||!out->primary||!out->secondary||
  !count||count>=32||(clear_flags&&clear_flags!=3)||(raster_flags&~7u)||(raster_flags&6)==6||
  (input->declaration_count!=3&&input->declaration_count!=5)||(old_decl!=3&&old_decl!=5)||
  input->layout.program_count!=2||(input->record_stride!=32&&!(input->record_stride==16&&input->declaration_count==3))||
  !input->record_count||input->record_count%3||input->record_count>12288||
  input->fragment_flags!=!!(raster_flags&1)||(previous&15)||(pstart&3)||(sstart&3)||
  previous>~(u64)0-0x110-32*0x60||(u32)previous>U32_MAX-0x110-32*0x60||
  old_point!=previous+(clear?0x110:0x90)+(count-1)*0x60||
  end_count!=(clear?13u:4u))return -EINVAL;
 if(m9x_faa_graph_draw_owners_validate(owner)||m9x_faa_depth_stages_encode(views,0x3f800000,&(struct m9x_faa_depth_stage_image){0}))return -EINVAL;
 for(i=0;i<3;i++)if(owner[31+i].base!=views->owner[i].base||owner[31+i].bytes<views->owner[i].capacity_bytes)return -EINVAL;
 extra=input->declaration_count==5?24:0;old_extra=old_decl==5?24:0;
 newsec=1484+extra;base=previous-(clear?0:0x80);
 minsec=(clear?1352:1356)+(count-1)*1484;maxsec=minsec+24*count;
 if(fresh_bytes!=1352+extra||old_sec<minsec||old_sec>maxsec||(old_sec-minsec)%24||
  (count==1&&old_sec!=minsec+old_extra)||
  (count>1&&(old_sec<minsec+old_extra||old_sec>maxsec-24+old_extra))||
  old_bytes<3588+24*(count-1)||check_add_overflow(old_bytes,24u,&newprimary)||
  out->primary_capacity<newprimary||out->secondary_capacity<newsec||
  pstart>owner[27].bytes||newprimary>owner[27].bytes-pstart||
  sstart>owner[29].bytes||old_sec>owner[29].bytes-sstart||newsec>owner[29].bytes-sstart-old_sec||
  (u64)owner[29].base+sstart+old_sec+newsec>U32_MAX||owner[22].bytes<712||owner[8].bytes<20)return -ERANGE;
 front=old_bytes-3588-24*(count-1);
 if(clear?(front<4656||front>4664||(front&3)):front!=0)return -EINVAL;
 split=old_bytes-3564;point=old_point+0x60;
 for(i=0;i<end_count;i++){
  ends[i]=old_ends[i];if(ends[i]<pstart||ends[i]>pstart+old_bytes||(ends[i]&3)||(i&&ends[i]<ends[i-1]))return -EINVAL;
 }
 if((clear&&ends[8]!=pstart+front)||ends[end_count-4]!=pstart+split||
  ends[end_count-3]!=pstart+old_bytes-12||ends[end_count-2]!=ends[end_count-3]||ends[end_count-1]!=pstart+old_bytes)return -EINVAL;
 first=sstart+old_sec-(count==1?(clear?1352:1356):1484)-old_extra;
 last=sstart+old_sec-772;
 {u32 expected[6]={0x45452d2c,(owner[29].base+first)|1,(owner[29].base+last)|2,
  0x45452d2c,(owner[29].base+last)|1,(owner[29].base+sstart+old_sec)|2};
  for(i=0;i<6;i++)if(word(old+split-24+4*i)!=expected[i])return -ESTALE;}
 if(word(s)!=2||word(s+4)!=0x4545e3e6||word(s+8)!=1||word(s+48)!=2||word(s+52)!=0x4545e3e6||word(s+56)!=1||
  word(s+moved(552,extra))!=2||word(s+moved(556,extra))!=0x45451819||word(s+moved(560,extra))||word(s+moved(564,extra))!=0x7fffffff||
  word(s+816+extra)!=(views->owner[2].base|1)||word(s+832+extra)||word(s+836+extra)!=0xf0010100||word(s+940+extra)!=0x38038f||
  (word(s+660+extra)&0x03000000)!=(raster_flags&2?0x03000000:raster_flags&4?0x02000000:0)||
  !!(word(s+676+extra)&0x4000)!=!!(raster_flags&1))return -EINVAL;
 for(i=0;i<7;i++)if(word(s+moved(sec_at[i],extra))!=((u32)base+sec_delta[i]|sec_tag[i]))return -ESTALE;
 for(i=0;i<6;i++)if(word(old+split+tail_at[i])!=((u32)old_point-tail_back[i]|tail_tag[i]))return -ESTALE;
 memmove(out->primary,old,split);memmove(out->primary+split+24,old+split,3564);
 put(out->primary+split,0x45452d2c);put(out->primary+split+4,(owner[29].base+sstart+old_sec)|1);
 put(out->primary+split+8,(owner[29].base+sstart+old_sec+712+extra)|2);
 put(out->primary+split+12,0x45452d2c);put(out->primary+split+16,(owner[29].base+sstart+old_sec+712+extra)|1);
 put(out->primary+split+20,(owner[29].base+sstart+old_sec+newsec)|2);
 for(i=0;i<6;i++)put(out->primary+split+24+tail_at[i],((u32)point-tail_back[i])|tail_tag[i]);
 put(out->secondary,3);put(out->secondary+4,0x45e3e6e6);put(out->secondary+8,1);put(out->secondary+12,2);put(out->secondary+16,word(s+12));
 memcpy(out->secondary+20,s+16,32);memcpy(out->secondary+52,insert,sizeof(insert));
 put(out->secondary+60,owner[22].base|1);put(out->secondary+136,((owner[22].base+644)&0xffffff84u)|5);
 put(out->secondary+140,((owner[22].base+708)&0xfffffffdu)|1);put(out->secondary+152,(owner[8].base+8)|1);
 put(out->secondary+168,(owner[8].base+12)|1);put(out->secondary+184,(owner[8].base+16)|1);
 memcpy(out->secondary+196,s+48,504+extra);memcpy(out->secondary+700+extra,s+568+extra,fresh_bytes-568-extra);
 delta=count*0x60;
 for(i=0;i<7;i++){
  at=moved(sec_at[i],extra);at=at==12?16:at<48?at+4:at<552+extra?at+148:at+132;
  put(out->secondary+at,((u32)base+sec_delta[i]+delta)|sec_tag[i]);
 }
 for(i=end_count-4;i<end_count;i++)ends[i]+=24;
 memcpy(out->end_bytes,ends,end_count*4);out->end_count=end_count;out->primary_bytes=newprimary;out->secondary_bytes=newsec;
 out->draw_end_bytes=pstart+split+24;out->final_point=point;return 0;
}
