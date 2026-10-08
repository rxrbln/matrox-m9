/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* Original M9128 CPU allocation geometry. No GPU initialization is implied. */
#ifndef M9_FAA_GRAPH_SURFACE_LAYOUT_H
#define M9_FAA_GRAPH_SURFACE_LAYOUT_H
#include <linux/types.h>

enum m9x_faa_surface_role {
 M9_FAA_SURFACE_PRIMARY, M9_FAA_SURFACE_MAIN, M9_FAA_SURFACE_AUXILIARY,
 M9_FAA_SURFACE_COMPANION, M9_FAA_SURFACE_HZ, M9_FAA_SURFACE_SENTINEL,
 M9_FAA_SURFACE_ROLES
};
struct m9x_faa_surface_extent {
 u32 logical, clear, backing, capacity;
};
struct m9x_faa_surface_geometry {
 u32 width,height,allocation_width,allocation_height,storage_width;
};
struct m9x_faa_surface_layout {
 u32 width,height,primary_descriptor,companion_descriptor,hz_descriptor,ad;
 struct m9x_faa_surface_extent owner[M9_FAA_SURFACE_ROLES];
 struct m9x_faa_surface_geometry primary,companion,hz;
};
static inline u64 m9_faa_layout_align(u64 value,u32 quantum)
{return (value+quantum-1)&~((u64)quantum-1);}

/* 45eb0 resource extent, 464d9 clear rounding, 46197/546ea backing
 * alignment, then native GEM page capacity. Keep all four distinct. */
static inline int m9x_faa_surface_layout(u32 width,u32 height,
 struct m9x_faa_surface_layout *out)
{
 struct m9x_faa_surface_layout v={.width=width,.height=height};
 u32 row,rows,small,quantum;u64 tail,elements,raw,clear,budget;
 if(!out||!width||!height||width>2048||height>2048)return 0;
 small=width<=64&&height<=64;row=m9_faa_layout_align(width,32);
 rows=width<=16&&height<=16?16:m9_faa_layout_align(height,small?4:32);
 tail=(u64)(rows-height)*row+row-width;elements=(u64)row*rows;
 if(tail<8)elements+=8-tail;
 clear=m9_faa_layout_align(elements*4,small?1024:4096);
 v.owner[M9_FAA_SURFACE_PRIMARY]=(struct m9x_faa_surface_extent){
  .logical=clear,.backing=m9_faa_layout_align(clear,0x8000),
  .capacity=m9_faa_layout_align(clear,0x8000)};
 v.primary=(struct m9x_faa_surface_geometry){width,height,row,rows,
  m9_faa_layout_align(width,8)<32?32:m9_faa_layout_align(width,8)};
 v.primary_descriptor=0xcc020000u|(small?0:0x200000u)|row;
 /* Exact binary source constant 0.38499999046325684=1614807/2^22.
  * The bounded integer product is exact before its source >>3. */
 budget=((u64)width*height*344*1614807)>>25;
 if(budget>0x2000000)budget=0x2000000;
 rows=(budget+1023)>>12;
 raw=(u64)(rows?rows:1)*4096+32;
 v.owner[M9_FAA_SURFACE_MAIN]=(struct m9x_faa_surface_extent){
  .logical=m9_faa_layout_align(raw,128),.backing=m9_faa_layout_align(raw,128),
  .capacity=m9_faa_layout_align(raw,4096)};
 rows=((budget>>4)+1023)>>12;raw=(u64)(rows?rows:1)*4096+32;
 v.owner[M9_FAA_SURFACE_AUXILIARY]=(struct m9x_faa_surface_extent){
  .logical=m9_faa_layout_align(raw,128),.backing=m9_faa_layout_align(raw,128),
  .capacity=m9_faa_layout_align(raw,4096)};
 row=m9_faa_layout_align(width,64);rows=m9_faa_layout_align(height,16);
 tail=(u64)(rows-height)*row+row-width;elements=(u64)row*rows;
 if(tail<4)elements+=4-tail;
 raw=elements*8;
 quantum=width<=64&&height<=32?1024:4096;clear=m9_faa_layout_align(raw,quantum);
 v.owner[M9_FAA_SURFACE_COMPANION]=(struct m9x_faa_surface_extent){
  .logical=raw,.clear=clear,.backing=m9_faa_layout_align(clear,0x8000),
  .capacity=m9_faa_layout_align(clear,0x8000)};
 v.companion=(struct m9x_faa_surface_geometry){width,height,row,rows,
  m9_faa_layout_align(width,8)<32?32:m9_faa_layout_align(width,8)};
 v.companion_descriptor=0x7a020000u|(quantum==1024?0:0x200000u)|row;
 v.hz.width=(width+7)>>3;v.hz.height=(height+3)>>2;
 v.hz.allocation_width=m9_faa_layout_align(v.hz.width,8);
 v.hz.allocation_height=m9_faa_layout_align(v.hz.height+8,8);
 v.hz.storage_width=v.hz.allocation_width;
 raw=m9_faa_layout_align((u64)v.hz.allocation_width*v.hz.allocation_height*4,128);
 clear=m9_faa_layout_align(raw,1024);
 v.owner[M9_FAA_SURFACE_HZ]=(struct m9x_faa_surface_extent){
  .logical=clear,.clear=clear,.backing=m9_faa_layout_align(clear,0x8000),
  .capacity=m9_faa_layout_align(clear,0x8000)};
 v.hz_descriptor=0xf0010000u|m9_faa_layout_align(width,64);
 v.ad=v.owner[M9_FAA_SURFACE_MAIN].logical>>5;
 v.owner[M9_FAA_SURFACE_SENTINEL]=(struct m9x_faa_surface_extent){
  .logical=128,.backing=128,.capacity=4096};
 *out=v;return 1;
}
/* Source selector1 reads allocation geometry (467e0+46838/469f1),
 * independently of logical clear bytes and spare backing pages. */
static inline u32 m9x_faa_primary_window_bytes(const struct m9x_faa_surface_layout *layout)
{return layout->primary.allocation_width*layout->primary.allocation_height*4u;}
/* Original 13b900 setup word 84: from the x/y guard band coefficients,
 * (floor(log2(trunc|g|)) - 1) & 7 at bits 14:12 (x) and 7:5 (y). */
#define M9_FAA_GUARD_FIELD_MASK 0x70e0u
static inline u32 m9x_faa_guard_bits(u32 magnitude)
{
 u32 n=0;
 while(magnitude>1){magnitude>>=1;n++;}
 return (n-1)&7;
}
/* Truncated magnitude of an IEEE single (fabs, fistp with RC=chop). */
static inline u32 m9x_faa_float_trunc(u32 bits)
{
 u32 e=(bits>>23)&0xff,m=(bits&0x7fffff)|0x800000;
 if(e<127)return 0;
 if(e>=127+31)return 0xffffffffu;
 return e>=150?m<<(e-150):m>>(150-e);
}
static inline u32 m9x_faa_guard_field(u32 guard_x_bits,u32 guard_y_bits)
{
 return m9x_faa_guard_bits(m9x_faa_float_trunc(guard_x_bits))<<12|
  m9x_faa_guard_bits(m9x_faa_float_trunc(guard_y_bits))<<5;
}
/* Full-target viewport: the coefficient is 15359/(size/2) - 1. */
static inline u32 m9x_faa_guard_field_size(u32 width,u32 height)
{
 return m9x_faa_guard_bits((2*15359-width)/width)<<12|
  m9x_faa_guard_bits((2*15359-height)/height)<<5;
}
static inline void m9x_faa_guard_field_put(u8 *p,u32 field)
{
 u32 v=(u32)p[0]|(u32)p[1]<<8|(u32)p[2]<<16|(u32)p[3]<<24;
 v=(v&~M9_FAA_GUARD_FIELD_MASK)|field;
 p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;
}
static inline int m9x_faa_runtime_shape(u32 width,u32 height)
{return (width==320&&height==240)||(width==1024&&height==768);}
#endif
