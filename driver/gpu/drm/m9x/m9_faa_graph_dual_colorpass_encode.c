/* SPDX-License-Identifier: GPL-2.0-only */
/* Public SELECTARG2 diagnostic on the source-qualified dual FIRST graph. */
#include <linux/errno.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include "m9_faa_graph_dual_colorpass_encode.h"

static u32 rd(const u8 *p,u32 at)
{return (u32)p[at]|(u32)p[at+1]<<8|(u32)p[at+2]<<16|(u32)p[at+3]<<24;}
static void wr(u8 *p,u32 at,u32 v)
{p[at]=v;p[at+1]=v>>8;p[at+2]=v>>16;p[at+3]=v>>24;}
static bool overlaps(const void *a,size_t an,const void *b,size_t bn)
{return (uintptr_t)a<(uintptr_t)b+bn&&(uintptr_t)b<(uintptr_t)a+an;}

/* Four register forms decoded from original 16fa20 SELECTARG2 COLOR0
 * compiler output. No program VA, texture VA, C2/C3 range or record is
 * captured here; the ISA fields are generated for the admitted operation. */
static bool colorpass_fs32(const u8 *p)
{
 static const struct {u8 op,cls,dst,a,b,c;u32 control;} forms[4]={
  {0x1e,4,0x04,0xff,0x7f,0x02,0xe2803f20},
  {0x1d,4,0x04,0x42,0x79,0x45,0xae003c00},
  {0x1d,4,0x2d,0xc0,0x79,0x44,0xae003c00},
  {0x1e,4,0x2d,0xff,0x7f,0x04,0xa2803f00}};
 u32 i;
 for(i=0;i<4;i++){
  const typeof(forms[0]) *f=&forms[i];
  u32 w0=((u32)(f->a&7)<<29)|((u32)f->b<<22)|
   ((u32)f->c<<15)|((u32)f->dst<<8)|((u32)f->cls<<5)|f->op;
  if(rd(p,i*8)!=w0||rd(p,i*8+4)!=(f->control|f->a>>3))return false;
 }
 return true;
}

int m9x_faa_dual_colorpass_encode(
 const struct m9x_faa_color_input *dual,
 const struct m9x_faa_color_input *pass,
 const struct m9x_faa_graph_draw_owner owner[31],
 u32 init_bytes,const u32 dual_ends[M9_FAA_DUAL_FIRST_ENDS],
 const u8 *dual_primary,u32 primary_bytes,
 const u8 *dual_secondary,u32 dual_secondary_bytes,
 u8 *primary,size_t primary_capacity,
 u8 *secondary,size_t secondary_capacity)
{
 static const u32 c2[4]={2432,364,1032,1660};
 static const u32 c3[4]={2452,384,1052,1680};
 static const u32 utility[4]={0,0x3f800000,0x3f800000,0x3f000000};
 u32 i,at,end,program_offset=0,cb;
 if(!dual||!pass||!owner||!dual_ends||!dual_primary||!dual_secondary||
    !primary||!secondary||!dual->program||!pass->program||
    !dual->constants||!pass->constants||!dual->geometry||!pass->geometry||
    !dual->declaration||!pass->declaration||
    dual_secondary_bytes!=M9_FAA_DUAL_FIRST_SECONDARY_BYTES||
    primary_bytes<4708||primary_capacity<primary_bytes||
    secondary_capacity<M9_FAA_DUAL_COLORPASS_SECONDARY_BYTES||
    overlaps(primary,primary_bytes,dual_primary,primary_bytes)||
    overlaps(secondary,M9_FAA_DUAL_COLORPASS_SECONDARY_BYTES,
             dual_secondary,dual_secondary_bytes)||
    dual->layout.program_count!=4||pass->layout.program_count!=4||
    dual->layout.entry_bytes[0]||pass->layout.entry_bytes[0]||
    dual->layout.entry_bytes[1]!=72||pass->layout.entry_bytes[1]!=32||
    dual->layout.program_bytes<72||
    dual->layout.program_bytes>M9_FAA_COLOR_PROGRAM_CAPACITY||
    dual->layout.constants_bytes>M9_FAA_COLOR_CONSTANT_BYTES||
    pass->layout.constant_begin[0]>pass->layout.constants_bytes||
    pass->layout.constant_end[0]>pass->layout.constants_bytes||
    dual->layout.constant_begin[0]>dual->layout.constants_bytes||
    dual->layout.constant_end[0]>dual->layout.constants_bytes||
    pass->layout.program_bytes!=dual->layout.program_bytes-40||
    pass->layout.constants_bytes+8!=dual->layout.constants_bytes||
    dual->layout.constant_begin[0]!=pass->layout.constant_begin[0]||
    pass->layout.constant_end[0]!=pass->layout.constant_begin[0]+16||
    dual->layout.constant_end[0]!=dual->layout.constant_begin[0]+24||
    !colorpass_fs32(pass->program)||
    memcmp(pass->constants+pass->layout.constant_begin[0],utility,sizeof(utility))||
    memcmp(pass->program+32,dual->program+72,dual->layout.program_bytes-72)||
    memcmp(pass->constants,dual->constants,pass->layout.constant_begin[0])||
    memcmp(pass->constants+pass->layout.constant_end[0],
           dual->constants+dual->layout.constant_end[0],
           pass->layout.constants_bytes-pass->layout.constant_end[0])||
    pass->layout.geometry_bytes!=dual->layout.geometry_bytes||
    pass->layout.vertex_bytes!=dual->layout.vertex_bytes||
    pass->layout.vertex_offset!=dual->layout.vertex_offset||
    pass->record_stride!=dual->record_stride||
    pass->record_count!=dual->record_count||
    pass->declaration_count!=dual->declaration_count||
    pass->declaration_count!=9||
    pass->data_offset!=dual->data_offset||
    memcmp(pass->geometry,dual->geometry,dual->layout.geometry_bytes)||
    memcmp(pass->declaration,dual->declaration,dual->declaration_count*4)||
    !owner[23].bytes||owner[23].bytes<6*0xb3000u+16||
    owner[23].base>U32_MAX-5*0xb3000u||
    owner[28].base>U32_MAX-owner[28].bytes||
    pass->data_offset>owner[28].bytes||
    pass->layout.constants_bytes>owner[28].bytes-pass->data_offset||
    owner[29].base>U32_MAX-M9_FAA_DUAL_FIRST_SECONDARY_BYTES||
    owner[29].bytes<M9_FAA_DUAL_FIRST_SECONDARY_BYTES)
  return -EINVAL;
 for(i=1;i<4;i++)
  if(pass->layout.entry_bytes[i]+40!=dual->layout.entry_bytes[i]||
     pass->layout.constant_begin[i]+8!=dual->layout.constant_begin[i]||
     pass->layout.constant_end[i]+8!=dual->layout.constant_end[i])return -EINVAL;
 for(i=0;i<4;i++)
  if(pass->layout.constant_begin[i]>pass->layout.constant_end[i]||
     pass->layout.constant_end[i]>pass->layout.constants_bytes||
     (pass->layout.entry_bytes[i]&7))return -EINVAL;
 if(dual_ends[10]<init_bytes+60||dual_ends[13]!=init_bytes+primary_bytes||
    dual_ends[10]-init_bytes>primary_bytes){return -EINVAL;}
 at=dual_ends[10]-init_bytes-4;
 if(at+4>primary_bytes||
    rd(dual_primary,at)!=((owner[29].base+M9_FAA_DUAL_FIRST_SECONDARY_BYTES)|2)||
    rd(dual_secondary,2368)!=0x45100100||
    rd(dual_secondary,2388)!=0x45110302||
    rd(dual_secondary,2408)!=0x45120504||
    rd(dual_secondary,2380)!=0x00100010||
    rd(dual_secondary,2400)!=0x00100010||
    rd(dual_secondary,2420)!=0x00100006||
    rd(dual_secondary,2620)!=0x45454571||
    rd(dual_secondary,2720)!=0x45454572||
    rd(dual_secondary,2868)!=0x23000001||
    rd(dual_secondary,2876)!=dual->record_count)return -EINVAL;
 /* This diagnostic is the initial code slot only. A future mixed-program
  * frame must supply its own authenticated offset instead of inheriting it. */
 for(i=0;i<4;i++)
  if(rd(dual_secondary,c2[i])!=(owner[26].base|1)||
     rd(dual_secondary,c2[i]+4)!=dual->layout.entry_bytes[i]/8||
     rd(dual_secondary,c2[i]+8)!=0)return -EINVAL;
 /* All checks precede any caller-visible write. The source two texture
  * owners stay retained; SELECTARG2 makes unit 1 an idle packet. */
 memcpy(primary,dual_primary,primary_bytes);
 wr(primary,at,(owner[29].base+M9_FAA_DUAL_COLORPASS_SECONDARY_BYTES)|2);
 memcpy(secondary,dual_secondary,2624);
 memcpy(secondary+2624,dual_secondary+2712,168);
 wr(secondary,2008,0x00000503);
 wr(secondary,2348,0x0110ffc3);
 wr(secondary,2392,(owner[23].base+3*0xb3000u)|1);
 wr(secondary,2396,(owner[23].base+5*0xb3000u)|1);
 wr(secondary,2400,0x00060010);
 wr(secondary,2412,(owner[23].base+4*0xb3000u)|1);
 wr(secondary,2416,0);
 wr(secondary,2420,0x00000010);
 wr(secondary,2708,0x00020688);
 wr(secondary,2720,0x00c331b6);
 wr(secondary,2724,0x00d40688);
 wr(secondary,2728,0);
 cb=owner[28].base+pass->data_offset;
 for(i=0;i<4;i++){
  program_offset=pass->layout.entry_bytes[i]/8;
  end=pass->layout.constant_end[i]-pass->layout.constant_begin[i];
  wr(secondary,c2[i]+4,program_offset);
  wr(secondary,c3[i],0x00500000|(end<<5));
  wr(secondary,c3[i]+4,(cb+pass->layout.constant_begin[i])|1);
  wr(secondary,c3[i]+8,(cb+pass->layout.constant_end[i])|1);
 }
 return 0;
}
