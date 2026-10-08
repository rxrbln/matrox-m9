// SPDX-License-Identifier: GPL-2.0-only
/* Original M9128 dual-texture FIRST: append one DRAW before normal Lock. */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include "m9_faa_graph_dual_first_append.h"

struct copy_span { u16 from, to, bytes; };
/* The source copies these packet intervals from the newly encoded live
 * FIRST image. They include all stage C2/C3, owner, sampler and declaration
 * fields; the new save/null-state block and timeline are built below. */
static const struct copy_span copies[] = {
	{0,144,28},{32,176,12},{44,200,4},{60,208,44},
	{108,256,212},{324,472,20},{348,496,108},{456,616,4},
	{460,708,200},{676,908,68},{748,980,24},{776,1008,8},
	{788,1020,44},{836,1068,64},{1000,1132,284},
	{1308,1416,80},{1392,1500,24},{1420,1528,8},
	{1432,1540,44},{1480,1588,32},{1588,1620,20},
	{1628,1640,216},{1868,1856,56},{1928,1916,24},
	{1956,1944,8},{1968,1956,56},{2028,2016,92},
	{2124,2112,756},
};
/* The fixed 1x1 null-target state uses no GPU allocation or captured
 * address. These words are the same in the source's second and third DRAWs.
 * The surrounding source-owner and source-timeline words are not literals. */
static const u32 null_target[24] = {
	0x00040118,3,0x457a7978,0,0xcc010001,0x00010001,3,
	0x45877d7c,0x200,0x0ff80000,0,4,0x85848382,0,0,0,0,
	4,0x7f7e8180,0xffffffff,0,0,0,1,
};
static const u32 save_prefix[36] = {
	1,0x45454524,0,3,0x453ce6e6,0x6dc00000,0x42000000,0xa1,
	9,0x44444444,0x30081898,0x30081898,0x30081898,0x30081898,
	0x44444444,0x30081898,0x30081898,0x30081898,0x30081898,
	2,0x45452f2e,0,0,2,0x4545421a,0,0xa1000009,
	2,0x4545421a,0,0xa3000009,3,0x45e6421a,0,0xa4000009,
	0x40008000,
};
static const struct { u16 at, delta; u8 tag; } epoch[] = {
	{204,0x130,1},{252,0x140,1},{1004,0x150,1},
	{1016,0x150,0},{1064,0x160,1},{1524,0x170,1},
	{1536,0x170,0},{1584,0x180,1},{1912,0x190,1},
	{1940,0x1a0,1},{1952,0x1a0,0},{2012,0x1b0,1},
	{2108,0x1c0,1},
};
static const u16 lock_at[] = {128,184,320,3084,3540,3560};
static const u16 lock_delta[] = {0x130,0x140,0x140,0x140,0x130,0x150};
static const u8 lock_tag[] = {1,1,0,0,0,1};
static const u16 save_at[] = {8,84,88,100,116,132};
static const u8 save_owner[] = {22,22,22,8,8,8};
static const u16 save_offset[] = {0,640,704,8,12,16};
static const u8 save_tag[] = {1,5,5,1,1,1};

static u32 rd(const u8 *p, u32 at)
{ return (u32)p[at] | (u32)p[at+1]<<8 | (u32)p[at+2]<<16 | (u32)p[at+3]<<24; }
static void wr(u8 *p, u32 at, u32 v)
{ p[at]=v; p[at+1]=v>>8; p[at+2]=v>>16; p[at+3]=v>>24; }
static bool overlaps(const void *a, u32 as, const void *b, u32 bs)
{
	unsigned long x=(unsigned long)a, y=(unsigned long)b;
	return x<=y ? y-x<as : x-y<bs;
}

int m9x_faa_dual_first_append_encode(
	const struct m9x_faa_graph_draw_owner owner[31],
	u32 init_bytes, u64 init_point, u32 old_draw_count,
	u32 primary_cursor, u32 secondary_cursor,
	const u32 old_end_bytes[M9_FAA_DUAL_FIRST_ENDS], u64 old_final_point,
	const u8 *old_primary, u32 old_primary_bytes,
	const u8 *first_secondary, const u8 *extra_first_secondary,
	bool programs_and_textures_retained,
	struct m9x_faa_dual_append_image *out)
{
	u32 low=(u32)init_point, split, prefix, old_start, old_length, old_epoch;
	u32 new_bytes, raw, i; u8 *dst;
	if (!owner || !old_end_bytes || !old_primary || !first_secondary ||
	    !extra_first_secondary || !out || !out->primary || !out->secondary ||
	    !programs_and_textures_retained || old_draw_count<1 ||
	    old_draw_count>2 ||
	    (init_point&15) || low<0xb0 ||
	    init_point>~(u64)0-0x150-2*0xa0 ||
	    low>U32_MAX-0x150-2*0xa0 ||
	    (init_bytes!=4880 && init_bytes!=4884) ||
	    (u64)primary_cursor!=(u64)init_bytes+old_primary_bytes ||
	    old_primary_bytes<3624+(old_draw_count-1)*60 ||
	    secondary_cursor!=2880+(old_draw_count-1)*2868 ||
	    old_final_point!=init_point+0x150+(old_draw_count-1)*0xa0)
		return -EINVAL;
	if (m9x_faa_graph_draw_owners_validate(owner)) return -EINVAL;
	if (old_primary_bytes>owner[27].bytes || old_primary_bytes>U32_MAX-60)
		return -ERANGE;
	new_bytes=old_primary_bytes+60;
	if (primary_cursor>owner[27].bytes ||
	    owner[27].bytes-primary_cursor<60 ||
	    secondary_cursor>owner[29].bytes ||
	    owner[29].bytes-secondary_cursor<M9_FAA_DUAL_APPEND_SECONDARY_BYTES ||
	    (u64)owner[27].base+primary_cursor+60>U32_MAX ||
	    (u64)owner[29].base+secondary_cursor+
	      M9_FAA_DUAL_APPEND_SECONDARY_BYTES>U32_MAX ||
	    owner[8].bytes<20 || owner[22].bytes<712 ||
	    out->primary_capacity<new_bytes ||
	    out->secondary_capacity<M9_FAA_DUAL_APPEND_SECONDARY_BYTES)
		return -ERANGE;
	if (overlaps(out->primary,new_bytes,out->secondary,
	             M9_FAA_DUAL_APPEND_SECONDARY_BYTES) ||
	    overlaps(out->primary,new_bytes,old_primary,old_primary_bytes) ||
	    overlaps(out->primary,new_bytes,first_secondary,2880) ||
	    overlaps(out->primary,new_bytes,extra_first_secondary,2880) ||
	    overlaps(out->secondary,M9_FAA_DUAL_APPEND_SECONDARY_BYTES,
	             old_primary,old_primary_bytes) ||
	    overlaps(out->secondary,M9_FAA_DUAL_APPEND_SECONDARY_BYTES,
	             first_secondary,2880) ||
	    overlaps(out->secondary,M9_FAA_DUAL_APPEND_SECONDARY_BYTES,
	             extra_first_secondary,2880)) return -EINVAL;
	prefix=old_primary_bytes-3624-(old_draw_count-1)*60;
	/* The source prefix is 4652 bytes; regenerated sampled page runs may
	 * widen it. The authenticated END[9] pins the actual live prefix. */
	if (prefix<4652 || prefix>M9_FAA_FIRST_DEPTH_PREFIX_CAPACITY ||
	    (prefix&3) || old_end_bytes[9]!=init_bytes+prefix)
		return -ESTALE;
	split=old_primary_bytes-3564;
	if (split<60 || rd(old_primary,split)!=0x45454524 ||
	    rd(old_primary,split-60)!=0x45452d2c ||
	    rd(old_primary,split-12)!=0x45452d2c ||
	    rd(old_primary,split-60+4)<owner[29].base+1)
		return -ESTALE;
	old_start=rd(old_primary,split-60+4)-owner[29].base-1;
	if (old_start>secondary_cursor) return -ESTALE;
	old_length=secondary_cursor-old_start;
	if (old_start!=(old_draw_count==1?0:2880+(old_draw_count-2)*2868) ||
	    old_length!=(old_draw_count==1?2880:2868) ||
	    rd(old_primary,split-60+8)!=
	      ((owner[29].base+old_start+old_length-968)|2) ||
	    rd(old_primary,split-60+52)!=
	      ((owner[29].base+old_start+old_length-968)|1) ||
	    rd(old_primary,split-60+56)!=
	      ((owner[29].base+secondary_cursor)|2) ||
	    old_end_bytes[10]!=init_bytes+split ||
	    old_end_bytes[13]!=primary_cursor)
		return -ESTALE;
	for (i=0;i<M9_FAA_DUAL_FIRST_ENDS;i++)
		if (old_end_bytes[i]<init_bytes ||
		    old_end_bytes[i]>primary_cursor ||
		    (i && old_end_bytes[i]<old_end_bytes[i-1]))
			return -ESTALE;
	old_epoch=low+(old_draw_count-1)*0xa0;
	for (i=0;i<ARRAY_SIZE(lock_at);i++)
		if (rd(old_primary,split+lock_at[i])!=
		    ((old_epoch+lock_delta[i])|lock_tag[i]))
			return -ESTALE;
	/* No program/sampler switch is sourced by this bounded repeated profile. */
	for (i=0;i<4;i++) {
		static const u16 c2[]={364,1032,1660,2432};
		if (rd(first_secondary,c2[i])!=rd(extra_first_secondary,c2[i]) ||
		    rd(first_secondary,c2[i]+4)!=rd(extra_first_secondary,c2[i]+4))
			return -EINVAL;
	}
	for (i=0;i<2;i++) {
		static const u16 tex[]={2536,2636};
		if (rd(first_secondary,tex[i])!=rd(extra_first_secondary,tex[i]))
			return -EINVAL;
	}
	/* Source no-cull append remains 2868 bytes. CW/CCW inserts another
	 * 24 bytes and changes the 6A selector, so it requires its own producer. */
	if (rd(extra_first_secondary,636)!=0x29340000 ||
	    rd(extra_first_secondary,2844)!=0x29340000)
		return -EINVAL;
	raw=rd(extra_first_secondary,344);
	if ((raw&7)!=1 || raw-1<owner[28].base ||
	    raw-1-owner[28].base>=owner[28].bytes) return -ERANGE;
	for (i=0;i<ARRAY_SIZE(save_at);i++)
		if ((owner[save_owner[i]].base&7) ||
	    (u64)owner[save_owner[i]].base+save_offset[i]+save_tag[i]>U32_MAX)
			return -ERANGE;
	/* All refusals precede output writes. Published GPU memory is untouched. */
	memcpy(out->primary,old_primary,split);
	memcpy(out->primary+split,old_primary+split-60,60);
	wr(out->primary,split+4,(owner[29].base+secondary_cursor)|1);
	wr(out->primary,split+8,
	   (owner[29].base+secondary_cursor+1900)|2);
	wr(out->primary,split+52,
	   (owner[29].base+secondary_cursor+1900)|1);
	wr(out->primary,split+56,
	   (owner[29].base+secondary_cursor+2868)|2);
	memcpy(out->primary+split+60,old_primary+split,3564);
	for (i=0;i<ARRAY_SIZE(lock_at);i++)
		wr(out->primary,split+60+lock_at[i],
		   (old_epoch+0xa0+lock_delta[i])|lock_tag[i]);
	dst=out->secondary;
	memset(dst,0,M9_FAA_DUAL_APPEND_SECONDARY_BYTES);
	for (i=0;i<ARRAY_SIZE(save_prefix);i++)wr(dst,i*4,save_prefix[i]);
	for (i=0;i<ARRAY_SIZE(save_at);i++)
		wr(dst,save_at[i],
		   (owner[save_owner[i]].base+save_offset[i])|save_tag[i]);
	for (i=0;i<ARRAY_SIZE(null_target);i++)
		wr(dst,604+i*4,null_target[i]);
	wr(dst,700,0x454545c0);wr(dst,704,4);
	for (i=0;i<ARRAY_SIZE(copies);i++)
		memcpy(dst+copies[i].to,extra_first_secondary+copies[i].from,
		       copies[i].bytes);
	wr(dst,172,0x49d00000u|((932-172)/4));
	wr(dst,188,3);wr(dst,192,0x45e3e6e6);wr(dst,196,1);
	wr(dst,468,((rd(extra_first_secondary,320)&0xff1fffffu)|
	              0x00100000u)&0xfffffffcu);
	wr(dst,492,raw);
	wr(dst,976,0x49d00000u|((1460-976)/4));
	wr(dst,1496,0x49d00000u|((1888-1496)/4));
	for (i=0;i<ARRAY_SIZE(epoch);i++)
		wr(dst,epoch[i].at,(old_epoch+epoch[i].delta)|epoch[i].tag);
	for (i=0;i<M9_FAA_DUAL_FIRST_ENDS;i++)
		out->end_bytes[i]=old_end_bytes[i]+(i<10?0:60);
	out->primary_bytes=new_bytes;
	out->secondary_bytes=M9_FAA_DUAL_APPEND_SECONDARY_BYTES;
	out->final_point=old_final_point+0xa0;
	return 0;
}
