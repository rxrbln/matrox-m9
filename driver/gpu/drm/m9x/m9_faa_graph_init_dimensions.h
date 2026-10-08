/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_INIT_DIMENSIONS_H
#define M9_FAA_GRAPH_INIT_DIMENSIONS_H
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include "m9_faa_graph_surface_layout.h"

/* Whole original INIT320 and INIT1024 closure: immutable code/database,
 * five HZ geometry producers and one b5e70 companion page-run. Addresses
 * and timeline words must already be bound at their original offsets. */
#define M9_FAA_INIT_DIMENSION_SOURCE_BYTES 4880u
#define M9_FAA_INIT_DIMENSION_CLEAR_START 0x11d8u
#define M9_FAA_INIT_DIMENSION_CLEAR_END 0x1224u
#define M9_FAA_INIT_DIMENSION_CLEAR_WORDS 194u
static inline int m9_faa_init_page_words(u32 base,u32 bytes,u32 *words,
 u32 capacity,u32 *count)
{
 u32 page,remaining,leading,value,n=0;
 if(!words||!count||!bytes||(base|bytes)&4095u||
    (u64)base+bytes>((u64)1<<32))return -ERANGE;
 page=base>>12;remaining=bytes>>12;leading=remaining<((-page)&7)?remaining:((-page)&7);
 if(!!leading+(remaining-leading+7)/8>capacity)return -ENOSPC;
 value=((page&0xffff8u)|0xd00000u)<<5;
 if(leading){
  u32 mask=remaining==leading?(0xffu>>(8-leading))<<(page&7):0xffu<<(8-leading);
  words[n++]=value|(mask&0xffu);
  value=(value&~0x1ffff00u)|((value+0x100)&0x1ffff00u);remaining-=leading;
 }
 while(remaining>=8){words[n++]=value|0xffu;value=(value&~0x1ffff00u)|((value+0x100)&0x1ffff00u);remaining-=8;}
 if(remaining)words[n++]=value|(0xffu>>(8-remaining));
 *count=n;return 0;
}
static inline int m9x_faa_graph_init_dimensions_encode(u8 *primary,u32 capacity,
 const struct m9x_faa_surface_layout *layout,u32 source_companion,u32 companion,
 u32 *scratch,u32 scratch_words,u32 *primary_bytes)
{
 static const u32 sites[]={0x454,0x474,0x1170,0x1188,0x11b0};
 static const u32 source[]={0x00480028,0xf0000028,0x00480028,0xf0000028,0x00480028};
 static const u32 headers[]={0x4545ace6,2,0,0x45e62b2a,0x454545e6,0xffff,0x20000012};
 struct m9x_faa_surface_layout expected;
 u32 original[19],count,old_count,bytes,i;int ret;
 if(!primary||!layout||!primary_bytes||!scratch||
    capacity<M9_FAA_INIT_DIMENSION_SOURCE_BYTES||
    !((layout->width==320&&layout->height==240)||
      (layout->width==1024&&layout->height==768))||
    !m9x_faa_surface_layout(layout->width,layout->height,&expected)||
    memcmp(layout,&expected,sizeof(expected)))return -EINVAL;
 for(i=0;i<5;i++)if(get_unaligned_le32(primary+sites[i])!=source[i])return -EINVAL;
 for(i=0;i<7;i++)if(get_unaligned_le32(primary+0x11bc+i*4)!=headers[i])return -EINVAL;
 if(get_unaligned_le32(primary+0x1224)!=0x4545e3e6||
    get_unaligned_le32(primary+0x1228)!=1)return -EINVAL;
 ret=m9_faa_init_page_words(source_companion,0x97000,original,19,&old_count);
 if(ret)return ret;
 if(old_count!=19)return -EINVAL;
 for(i=0;i<19;i++)if(get_unaligned_le32(primary+0x11d8+i*4)!=original[i])return -EINVAL;
 ret=m9_faa_init_page_words(companion,layout->owner[M9_FAA_SURFACE_COMPANION].clear,
  scratch,scratch_words,&count);if(ret)return ret;
 bytes=M9_FAA_INIT_DIMENSION_SOURCE_BYTES-19*4+count*4;
 if(bytes>capacity)return -ENOSPC;
 /* Every read and capacity check precedes the first output modification. */
 for(i=0;i<5;i++)put_unaligned_le32((i==1||i==3)?
   0xf0000000u|layout->hz.storage_width:
   (layout->hz.allocation_height<<16)|layout->hz.storage_width,primary+sites[i]);
 memmove(primary+0x11d8+count*4,primary+0x1224,M9_FAA_INIT_DIMENSION_SOURCE_BYTES-0x1224);
 put_unaligned_le32(0x20000000u|(count-1),primary+0x11d4);
 for(i=0;i<count;i++)put_unaligned_le32(scratch[i],primary+0x11d8+i*4);
 *primary_bytes=bytes;return 0;
}
#endif
