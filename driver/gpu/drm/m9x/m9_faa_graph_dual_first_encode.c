/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* Source-qualified M9128 FIRST-depth COLOR0/TEX0/TEX1 command producer. */
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/overflow.h>
#include <crypto/sha2.h>
#include "m9_faa_graph_dual_first_encode.h"

struct addr_field { u16 at;u8 owner,tag;u32 offset; };
/* owner 31 denotes the new bound depth; 32 denotes the ordinary command
 * SYSTEM owner's published +0x2000 view. All other roles are live graph owners. */
static const struct addr_field addresses[]={
 {116,24,1,0},{132,24,1,0},{148,24,1,0},{164,24,1,0},
 {344,28,1,0xffffffff},{624,12,1,0},{656,28,1,0xfffffffe},
 {708,32,0,0},{712,32,0,0},{716,32,0,0},
 {844,23,1,0},{860,23,1,1},{876,23,1,2},{892,23,1,5},
 {1088,24,1,0},{1116,24,1,0},{1144,24,1,0},
 {1292,12,1,0},{1324,28,1,0xfffffffe},
 {1360,32,0,0},{1364,32,0,0},{1368,32,0,0},
 {1488,23,1,3},{1504,23,1,4},{1520,23,1,5},
 {1716,24,1,0},{1852,12,1,0},{1884,28,1,0xfffffffe},
 {2100,8,1,0},{2104,8,0,20},{2132,1,1,0},{2148,31,1,0},
 {2180,25,1,0},{2192,22,1,0},{2224,2,1,0},{2228,3,1,0},
 {2372,23,1,0},{2376,23,1,1},{2392,23,1,2},{2396,23,1,3},
 {2412,23,1,5},{2416,23,1,4},{2832,12,1,0},
 {2864,28,1,0xfffffffe}
};
struct epoch_field {u16 at;u16 delta;u8 tag;};
static const struct epoch_field epochs[]={
 {56,0x90,1},{104,0xa0,1},{772,0xb0,1},{784,0xb0,0},
 {832,0xc0,1},{1416,0xd0,1},{1428,0xd0,0},{1476,0xe0,1},
 {1924,0xf0,1},{1952,0x100,1},{1964,0x100,0},
 {2024,0x110,1},{2120,0x120,1}
};
static const u8 template_sha[SHA256_DIGEST_SIZE]={
 0x83,0x21,0x2a,0x22,0x11,0x35,0xd9,0x0b,0x63,0xc2,0x9a,0xe4,
 0x33,0x4a,0x7d,0x7e,0xd6,0x56,0xd5,0x80,0xb1,0xb0,0x5d,0xe8,
 0x20,0x70,0x69,0xa4,0xbf,0x8c,0xe6,0xe6};
static const u8 fragment_sha[SHA256_DIGEST_SIZE]={
 0xf6,0x11,0x84,0x38,0x49,0xaa,0x62,0xc5,0xb4,0xcf,0xa5,0x53,
 0x90,0x22,0xc7,0xff,0x7e,0x24,0x39,0xcc,0x3b,0x48,0xef,0xae,
 0x5e,0x7e,0x77,0x40,0x68,0xe2,0x73,0x24};
/* The original 72-byte two-sampler fragment program binds these three
 * operation pairs through C3. They are scalar shader inputs, independent of
 * allocation, framebuffer dimensions and texture placement. */
static const u32 fragment_utility[6]={
 0u,0x3f800000u,0u,0x3f800000u,0x3f800000u,0x3f000000u};
static u32 rd(const u8 *p,u32 at)
{return (u32)p[at]|(u32)p[at+1]<<8|(u32)p[at+2]<<16|(u32)p[at+3]<<24;}
static void wr(u8 *p,u32 at,u32 v)
{p[at]=v;p[at+1]=v>>8;p[at+2]=v>>16;p[at+3]=v>>24;}
/* Truncated magnitude of an IEEE single, as fistp after fabs with RC=chop. */
static u32 float_trunc(u32 bits)
{
 u32 e=(bits>>23)&0xff,m=(bits&0x7fffff)|0x800000;
 if(e<127)return 0;
 if(e>=127+31)return U32_MAX;
 return e>=150?m<<(e-150):m>>(150-e);
}
static u32 floor_log2(u32 v)
{u32 n=0;while(v>1){v>>=1;n++;}return n;}
/* Original 13b900: guard band coefficients of the position constants select
 * two 3-bit fields (log2-1) at bits 14:12 (x) and 7:5 (y) of setup word 84. */
u32 m9x_faa_guard_field(u32 guard_x_bits,u32 guard_y_bits)
{
 return ((floor_log2(float_trunc(guard_x_bits))-1)&7)<<12|
  ((floor_log2(float_trunc(guard_y_bits))-1)&7)<<5;
}
static bool sha_ok(const u8 *data,u32 bytes,const u8 expected[32])
{u8 digest[32];sha256(data,bytes,digest);return !memcmp(digest,expected,32);}

int m9x_faa_dual_first_encode(const u8 *sample,size_t sample_bytes,
 const u8 *color_template,size_t color_template_bytes,
 const u8 *dual_template,size_t dual_template_bytes,
 const struct m9x_faa_graph_draw_owner owner[31],
 const struct m9x_faa_depth_views *views,u32 init_bytes,u64 init_point,
 const struct m9x_faa_dual_first_input *input,
 u8 *primary,size_t primary_capacity,u8 *secondary,size_t secondary_capacity,
 u32 *primary_bytes,u32 *ends,
 const struct m9x_faa_color_image *color_scratch,
 struct m9x_faa_first_depth_scratch *prefix_scratch)
{
 const struct m9x_faa_color_input *c;
 struct m9x_faa_texture_dual_link_input link={0};
 struct m9x_faa_texture_dual_program program={0};
 struct m9x_faa_surface_layout layout;
 u8 *stage;u32 length,raw,geo,padded,base,limit,off,value,i,j;
 int ret;
 if(!sample||!color_template||!dual_template||!owner||!views||!input||
  !input->color||!input->texture||!primary||!secondary||!primary_bytes||
  !ends||!color_scratch||!color_scratch->secondary||
  color_scratch->secondary_bytes<M9_FAA_DUAL_FIRST_SECONDARY_BYTES||
  !prefix_scratch||
  dual_template_bytes!=M9_FAA_DUAL_FIRST_SECONDARY_BYTES||
  secondary_capacity<M9_FAA_DUAL_FIRST_SECONDARY_BYTES||
  primary_capacity<M9_FAA_FIRST_DEPTH_PREFIX_CAPACITY+
   M9_FAA_DUAL_FIRST_PRIMARY_TAIL_BYTES||
  (init_point&15)||init_point<0xb0||init_point>~(u64)0-0x150||
  (u32)init_point>0xfffffeafu||
  !sha_ok(dual_template,dual_template_bytes,template_sha))return -EINVAL;
 c=input->color;
 if(!m9x_faa_surface_layout(views->owner[M9_FAA_BOUND_COLOR].width,
  views->owner[M9_FAA_BOUND_COLOR].height,&layout)||
  c->layout.constant_end[3]-c->layout.constant_begin[3]!=104)return -EINVAL;
 if(!c->program||!c->constants||!c->geometry||!c->declaration||
  !c->dual_base_only||c->layout.program_count!=4||
  c->layout.entry_bytes[0]||c->layout.entry_bytes[1]!=72||
  c->layout.entry_bytes[3]-c->layout.entry_bytes[2]!=56||
  (c->layout.program_bytes-c->layout.entry_bytes[3]!=192&&
   c->layout.program_bytes-c->layout.entry_bytes[3]!=200)||
  c->declaration_count!=9||c->layout.constants_bytes>16384||
  c->layout.constant_begin[0]>c->layout.constant_end[0]||
  c->layout.constant_end[0]-c->layout.constant_begin[0]!=24||
  c->layout.constant_end[0]>c->layout.constants_bytes||
  memcmp(c->constants+c->layout.constant_begin[0],fragment_utility,
         sizeof(fragment_utility))||
  c->layout.vertex_offset!=128||c->fragment_flags||
  !m9x_faa_depth_flags_valid(c->depth_flags)||
  !sha_ok(c->program,72,fragment_sha)||
  check_mul_overflow(c->record_stride,c->record_count,&raw)||
  raw!=c->layout.vertex_bytes||c->record_count>12288||
  c->data_offset>owner[28].bytes)return -EINVAL;
 geo=(c->layout.constants_bytes+127)&~127u;
 padded=(c->record_count+3)&~3u;
 if(geo>owner[28].bytes-c->data_offset||
  owner[28].bytes-c->data_offset-geo<128+padded*c->record_stride||
  owner[23].bytes<6*0xb3000u+16||owner[24].bytes<16||
  owner[27].bytes<0x6004||owner[29].bytes<2880||
  input->program_offset>owner[26].bytes||
  c->layout.program_bytes>owner[26].bytes-input->program_offset||
  (input->program_offset&7))return -ERANGE;
 ret=m9x_faa_graph_draw_owners_validate(owner);if(ret)return ret;
 /* All potentially refusing builders target caller-owned scratch. Published
  * primary/secondary stay untouched until every packet has passed. */
 ret=m9x_faa_color_four_encode(color_template,color_template_bytes,owner,
  init_bytes,init_point+0x70,c,color_scratch);if(ret)return ret;
 if(*color_scratch->secondary_length!=2736)return -EINVAL;
 stage=color_scratch->secondary;
 /* The four-stage live producer already derived the variable carry mask.
  * Save it before replacing the incomplete four-target secondary body. */
 value=rd(stage,304);memcpy(stage,dual_template,2880);
 wr(stage,320,(0xfu<<24)|(value&0xffffu));
 wr(stage,324,0xfe207924);wr(stage,1008,0x0070ffff);
 wr(stage,1012,0xfe207e92);
 wr(stage,88,0x0008000e|((c->declaration_count-1)<<8));
 /* Four packed target lanes, source 13a884/13a89e: each lane occupies
  * four DWORDs. The generated gather/position heads consume that record. */
 for(i=0;i<4;i++)wr(stage,120+i*16,0xee190010|(i<<10));
 wr(stage,816,0x0008000e);wr(stage,1460,0x0008000e);
 wr(stage,1092,0xee190410);wr(stage,1120,0xee190810);
 wr(stage,1132,0x0f0e0d0c);wr(stage,1720,0xee190010);
 wr(stage,352,c->record_stride);
 wr(stage,684,padded);wr(stage,1336,padded);wr(stage,1896,padded);
 wr(stage,2876,c->record_count);
 /* The source's third input group is a genuine packet. Its padding is
  * always zero; no captured CPU scratch words become metadata. */
 for(i=0;i<3;i++)for(j=0;j<4;j++)
  wr(stage,552+i*24+j*4,j+i*4<c->declaration_count?
   rd(c->declaration,(i*4+j)*4):0);
 wr(stage,2292,0xffff);wr(stage,2296,0);
 wr(stage,2316,layout.height<<16|layout.width);
 wr(stage,2324,layout.height<<16|layout.width);
 /* Seven dimension words: primary/companion descriptors, the MAIN extent
  * AD quartet and the active rectangle above. */
 wr(stage,2136,layout.primary_descriptor);
 wr(stage,2152,layout.companion_descriptor);
 for(i=0;i<4;i++)wr(stage,2240+i*4,layout.ad);
 /* Position constants k[19]/k[21] are the live x/y guard coefficients. */
 value=m9x_faa_guard_field(rd(c->constants,c->layout.constant_begin[3]+76),
  rd(c->constants,c->layout.constant_begin[3]+84));
 for(i=0;i<3;i++){
  static const u16 at[]={76,804,1448};
  wr(stage,at[i],(rd(stage,at[i])&~0x70e0u)|value);
 }
 wr(stage,1996,(rd(stage,1996)&~0x70e0u)|value);
 wr(stage,2272,m9x_faa_depth_a8(c->depth_flags));
 /* Source f0/ef/d7 and fragment usage describe this exact authenticated
  * dual-multiply FS72 interface. Other fragment programs refuse above. */
 wr(stage,72,0x00a80200);wr(stage,800,0x00280600);
 wr(stage,1444,0x00088e00);wr(stage,1992,0xd8004055);
 wr(stage,2348,0x0130fb0f);wr(stage,2352,0xfe207ff8);
 wr(stage,1636,0x0010ef03);wr(stage,1640,0xfe201fd2);
 wr(stage,2008,0x603);wr(stage,2844,0x29340000);
 /* The authenticated 72-byte dual-multiply fragment frontend declares
  * COLOR0, UV0 and UV1. These semantic records are generated from the
  * admitted fragment interface; they are not caller-provided commands. */
 wr(stage,2792,0x00510688);wr(stage,2796,0x00620688);
 wr(stage,2808,0x00030688);wr(stage,2812,0x00c431b6);
 wr(stage,2816,0x00d50688);
 for(i=0;i<ARRAY_SIZE(epochs);i++)wr(stage,epochs[i].at,
  ((u32)init_point+epochs[i].delta)|epochs[i].tag);
 wr(stage,28,0x49d00000|((700-28)/4));
 wr(stage,744,0x49d00000|((1352-744)/4));
 wr(stage,1388,0x49d00000|((1900-1388)/4));
 for(i=0;i<ARRAY_SIZE(addresses);i++){
  const struct addr_field *f=&addresses[i];off=f->offset;
  if(f->owner==31){base=views->owner[M9_FAA_BOUND_DEPTH].base;
   limit=views->owner[M9_FAA_BOUND_DEPTH].capacity_bytes;}
  else if(f->owner==32){base=owner[27].base+0x2000;limit=owner[27].bytes-0x2000;}
  else{base=owner[f->owner].base;limit=owner[f->owner].bytes;}
  if(f->owner==23){
   /* Original untransformed XYZ producer (position tail 200 bytes) binds
    * SCREEN before CLIP in the last raster input group. The 192-byte
    * XYZRHW producer uses the opposite order. Both still publish the same
    * three position output banks above; only these two reader fields move.
    * All addresses remain derived from the retained live owner. */
   if(c->layout.program_bytes-c->layout.entry_bytes[3]==200){
    if(f->at==2396)off=4;
    else if(f->at==2416)off=3;
   }
   off*=0xb3000u;
  }
  if(f->owner==28){off=f->offset==0xffffffff?geo+128:geo;
   off+=c->data_offset;}
  if(off>limit||limit-off<4||check_add_overflow(base,off,&value)||
   (value&3))return -ERANGE;
  wr(stage,f->at,value|f->tag);
 }
 link.program_base=owner[26].base;link.program_owner_bytes=owner[26].bytes;
 link.program_offset=input->program_offset;
 link.program_bytes=c->layout.program_bytes;
 link.constants_base=owner[28].base;link.constants_owner_bytes=owner[28].bytes;
 link.data_offset=c->data_offset;link.constants_bytes=c->layout.constants_bytes;
 link.constants_tag=1;
 for(i=0;i<4;i++){
  link.entry_bytes[i]=c->layout.entry_bytes[i];
  link.constant_begin[i]=c->layout.constant_begin[i];
  link.constant_end[i]=c->layout.constant_end[i];
  program.entry_bytes[i]=c->layout.entry_bytes[i];
 }
 program.stage_count=4;program.sampler_count=2;
 program.coordinate_sets=2;program.color_op0=4;program.color_op1=4;
 program.program_bytes=c->layout.program_bytes;
 ret=m9x_faa_texture_dual_link_place(&link,stage,2880);if(ret)return ret;
 ret=m9x_faa_texture_dual_bind_place(input->texture,&program,stage,2880);
 if(ret)return ret;
 ret=m9x_faa_first_depth_prefix_encode(sample,sample_bytes,owner,views,
  init_point,primary,primary_capacity,&length,ends,prefix_scratch);
 if(ret)return ret;
 /* Full source primary differs from the proven live four-stage FIRST by
  * exactly one eight-byte GPU43 dependency pair at tail-relative 3348.
  * The vendor's 14 END boundaries retain their source order. */
 memcpy(primary+length,color_scratch->primary+64,3348);
 memcpy(primary+length+3348,color_scratch->primary+64+3348+8,
  M9_FAA_DUAL_FIRST_PRIMARY_TAIL_BYTES-3348);
 wr(primary,length+8,(owner[29].base+1912)|2);
 wr(primary,length+52,(owner[29].base+1912)|1);
 wr(primary,length+56,(owner[29].base+2880)|2);
 /* The source FIRST prefix has ten ENDs; the complete draw/Lock adds four.
  * Every endpoint is relative to the live INIT cursor, not a captured VA. */
 for(i=0;i<M9_FAA_FIRST_DEPTH_PREFIX_ENDS;i++)ends[i]+=init_bytes;
 ends[10]=init_bytes+length+60;
 ends[11]=ends[12]=init_bytes+length+M9_FAA_DUAL_FIRST_PRIMARY_TAIL_BYTES-12;
 ends[13]=init_bytes+length+M9_FAA_DUAL_FIRST_PRIMARY_TAIL_BYTES;
 memcpy(secondary,stage,2880);
 *primary_bytes=length+M9_FAA_DUAL_FIRST_PRIMARY_TAIL_BYTES;
 return 0;
}
