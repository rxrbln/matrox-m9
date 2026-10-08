/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/string.h>
#include "m9_faa_depth_mode_clear_resume.h"
static u32 word(const u8*p){return (u32)p[0]|(u32)p[1]<<8|(u32)p[2]<<16|(u32)p[3]<<24;}
static void put(u8*p,u32 v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
int m9x_faa_depth_mode_clear_resume_encode(const u8*sample,size_t sample_bytes,
 const u8*color,size_t color_bytes,const u8*reset,size_t reset_bytes,
 const struct m9x_faa_graph_draw_owner*o,const struct m9x_faa_depth_views*v,
 u32 init_bytes,u32 pcursor,u32 scursor,u64 previous,u64 retired,
 const struct m9x_faa_depth_mode_state*before,const struct m9x_faa_color_input*in,
 struct m9x_faa_later_depth_image*out,struct m9x_faa_clear_resume_scratch*s,
 struct m9x_faa_depth_mode_state*after)
{
 struct m9x_faa_later_depth_image tmp;
 struct m9x_faa_depth_mode_state planned={0};
 u32 sec=0,extra,at,i,geometry,constants;int ret;
 if(!o||!v||!before||!before->valid||!before->enabled||before->embedded_hz||
  !in||!out||!s||!after||!out->image.program||!out->image.constants||
  !out->image.primary||!out->image.secondary||!out->image.secondary_length||
  in->layout.program_count!=2||in->fragment_flags||!(in->depth_flags&32)||
  (in->declaration_count!=3&&in->declaration_count!=5)||
  v->owner[0].width!=320||v->owner[0].height!=240||
  (previous&15)||previous>~(u64)0-0x160||(u32)previous>0xfffffe9fu||retired!=previous+0x80||
  (pcursor&3)||(scursor&3))return -EINVAL;
 extra=in->declaration_count==5?24:0;
 if(in->layout.constants_bytes>16384||!in->record_count||in->record_count>12288||
  in->record_count%3||(in->record_stride!=32&&(in->record_stride!=16||in->declaration_count!=3)))return -ERANGE;
 geometry=(in->layout.constants_bytes+127)&~127u;
 constants=geometry+128+((in->record_count+3)&~3u)*in->record_stride;
 if(in->layout.constants_bytes>16384||in->record_count>12288||
  constants>sizeof(s->data)||in->layout.program_bytes>sizeof(s->program)||
  out->image.program_bytes<in->layout.program_bytes||out->image.constants_bytes<constants||
  out->image.primary_bytes<7740||out->image.secondary_bytes<1340+extra||
  pcursor>o[27].bytes||7744>o[27].bytes-pcursor||
  scursor>o[29].bytes||1352+extra>o[29].bytes-scursor)return -ERANGE;
 tmp=(struct m9x_faa_later_depth_image){.image={s->program,s->data,s->primary,s->secondary,
  sizeof(s->program),sizeof(s->data),sizeof(s->primary),sizeof(s->secondary),&sec}};
 ret=m9x_faa_depth_mode_later_encode(sample,sample_bytes,color,color_bytes,reset,reset_bytes,
  o,v,init_bytes,pcursor,scursor,retired,retired+0x10,before,in,&tmp,&s->later,&planned);
 if(ret)return ret;
 if(tmp.primary_length!=7744||tmp.clear_bytes!=4156||sec!=1352+extra||tmp.end_count!=7||
  tmp.final_point!=previous+0x160||word(s->primary+80)!=0x45e3e6e6||
  word(s->primary+88)!=2||word(s->primary+4156)!=0x45452d2c||word(s->primary+4168)!=0x45452d2c||
  word(s->secondary+624+extra)!=1||word(s->secondary+628+extra)!=0x45454543||
  word(s->secondary+632+extra)!=(u32)previous+0x110)return -EINVAL;
 /* Original return from the authentic finite query restores uncounted
  * mode around the reset's first12-byte E3, followed by count3 re-entry.
  * These are source queue-mode controls, not fabricated terminators. */
 memmove(s->primary+100,s->primary+96,4156-96);
 put(s->primary+80,0x4545e3e6);put(s->primary+88,((u32)previous+0xa0)|1);
 put(s->primary+92,0x454545e6);put(s->primary+96,3);
 /* Same live unindexed indirect ranges. The reached finite-query branch
  * omits the secondary three-word dependency and later primary two-word
  * GPU43 control; it does NOT claim the future Lock point has retired. */
 memcpy(s->primary+4160,s->later.later.tail+64,3560);
 memcpy(s->primary+7720,s->later.later.tail+64+3568,20);
 at=624+extra;memmove(s->secondary+at,s->secondary+at+12,sec-at-12);sec-=12;
 put(s->primary+4180,(o[29].base+scursor+sec)|2);
 /* No caller output is changed until every typed producer and shape check
  * above succeeded. Program/constant/record ownership follows live inputs. */
 memcpy(out->image.program,s->program,in->layout.program_bytes);
 memcpy(out->image.constants,s->data,constants);
 memcpy(out->image.primary,s->primary,7740);memcpy(out->image.secondary,s->secondary,sec);
 out->primary_length=7740;out->secondary_length=sec;out->clear_bytes=4160;
 out->end_count=7;out->draw_end_bytes=pcursor+4184;out->final_point=previous+0x160;
 {static const u32 ends[7]={4132,4132,4160,4184,7728,7728,7740};
  for(i=0;i<7;i++)out->end_bytes[i]=pcursor+ends[i];}
 *out->image.secondary_length=sec;*after=planned;return 0;
}
