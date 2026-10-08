// SPDX-License-Identifier: GPL-2.0-only
/* Original M9128 fresh FIRST COLOR4 extra-DRAW composition, CPU only. */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include "m9_faa_graph_frame_first_append_encode.h"

struct copy_span { u16 from, to, bytes; };
/* Original source copies of source-known packet words from a newly encoded
 * live FIRST stage. Address-bearing words thus retain that job's allocation
 * and live C2/CB/geometry metadata, not captured source GPU addresses. */
static const struct copy_span copies[] = {
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
static const u32 save_prefix[36] = {
	1,0x45454524,0,3,0x453ce6e6,0x6dc00000,0x42000000,0xa1,
	9,0x44444444,0x30081898,0x30081898,0x30081898,0x30081898,
	0x44444444,0x30081898,0x30081898,0x30081898,0x30081898,
	2,0x45452f2e,0,0,2,0x4545421a,0,0xa1000009,
	2,0x4545421a,0,0xa3000009,3,0x45e6421a,0,0xa4000009,
	0x40008000,
};
static const struct { u16 at, delta; u8 tag; } secondary_epoch[] = {
	{204,0x130,1},{252,0x140,1},{964,0x150,1},
	{976,0x150,0},{1024,0x160,1},{1452,0x170,1},
	{1464,0x170,0},{1512,0x180,1},{1840,0x190,1},
	{1868,0x1a0,1},{1880,0x1a0,0},{1940,0x1b0,1},
	{2036,0x1c0,1},
};
static const u16 lock_at_new[] = {128,184,320,3084,3540,3560};
static const u16 lock_at_first[] = {128,184,320,3084,3548,3568};
static const u16 lock_delta[] = {0x130,0x140,0x140,0x140,0x130,0x150};
static const u8 lock_tag[] = {1,1,0,0,0,1};
static const u16 save_at[] = {8,84,88,100,116,132};
static const u8 save_owner[] = {22,22,22,8,8,8};
static const u16 save_offset[] = {0,640,704,8,8,16};
static const u8 save_tag[] = {1,5,5,1,5,1};

static u32 get(const u8 *p)
{
	return (u32)p[0] | (u32)p[1]<<8 | (u32)p[2]<<16 | (u32)p[3]<<24;
}
static void put(u8 *p, u32 v)
{
	p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24;
}
/* Removing the unused second four-word declaration group shortens the first
 * COLOR secondary after its declaration block. The appended source grammar
 * copies the one remaining group and shifts every later field with it. */
static u32 moved(u32 at, u32 shift)
{
	return at >= 576 ? at-shift : at;
}
static bool overlaps(const void *a, u32 as, const void *b, u32 bs)
{
	unsigned long x=(unsigned long)a, y=(unsigned long)b;
	return x<=y ? y-x<as : x-y<bs;
}

int m9x_faa_frame_first_append_encode(
	const struct m9x_faa_graph_draw_owner owner[31],
	u32 init_bytes, u64 init_point, u32 old_draw_count,
	u32 primary_cursor, u32 secondary_cursor,
	const u32 old_end_bytes[M9_FAA_FIRST_APPEND_ENDS],
	u64 old_final_point,
	const u8 *old_primary, u32 old_primary_bytes,
	const u8 *first_secondary, u32 first_secondary_bytes,
	const u8 *extra_first_secondary, u32 extra_first_bytes,
	const struct m9x_faa_color_input *extra_input, bool programs_retained,
	struct m9x_faa_frame_first_append_image *out)
{
	u32 i, insert, tail_bytes, growth, old_second_start;
	u32 low=(u32)init_point, old_epoch, next_epoch;
	u32 first_end_mid, new_bytes, source_raw, old_last_bytes;
	u32 shift, first_bytes, append_bytes, old_range;
	u8 *dst;

	if (!owner || !old_end_bytes || !old_primary || !first_secondary ||
	    !extra_first_secondary || !extra_input || !out ||
	    !out->primary || !out->secondary || !programs_retained ||
	    old_draw_count<1 || old_draw_count>31 ||
	    (init_point&15) || low<0xb0 || low>U32_MAX-0x1550 ||
	    (init_bytes&3) || (primary_cursor&3) ||
	    (secondary_cursor&3) ||
	    (extra_input->declaration_count!=3 &&
	     extra_input->declaration_count!=5) ||
	    extra_input->layout.program_count!=4 ||
	    !extra_input->layout.program_bytes ||
	    extra_input->layout.program_bytes>4096 ||
	    (extra_input->layout.program_bytes&7) ||
	    extra_input->layout.entry_bytes[0]!=0 ||
	    extra_input->layout.entry_bytes[3]>
	       extra_input->layout.program_bytes ||
	    extra_input->layout.program_bytes-
	       extra_input->layout.entry_bytes[3]!=200 ||
	    (extra_input->record_stride!=32 &&
	     !(extra_input->record_stride==16 &&
	       extra_input->declaration_count==3)) ||
	    !extra_input->record_count ||
	    extra_input->record_count>12288 ||
	    extra_input->record_count%3 ||
	    (u32)old_final_point!=low+0x150+(old_draw_count-1)*0xa0 ||
	    old_final_point<init_point ||
	    primary_cursor<old_primary_bytes ||
	    primary_cursor-old_primary_bytes!=init_bytes)
		return -EINVAL;
	shift=extra_input->declaration_count==3?24:0;
	first_bytes=first_secondary_bytes;
	append_bytes=2700-shift;
	if ((first_bytes!=2688&&first_bytes!=2712) ||
	    extra_first_bytes!=2712-shift ||
	    secondary_cursor<first_bytes)
		return -EINVAL;
	tail_bytes=old_draw_count==1 ? 3572 : 3564;
	growth=old_draw_count==1 ? 52 : 60;
	if (old_primary_bytes<tail_bytes+60 ||
	    old_primary_bytes>U32_MAX-growth ||
	    primary_cursor>owner[27].bytes ||
	    owner[27].bytes-primary_cursor<growth ||
	    secondary_cursor>owner[29].bytes ||
	    owner[29].bytes-secondary_cursor<append_bytes ||
	    (u64)owner[27].base+primary_cursor+growth>U32_MAX ||
	    (u64)owner[29].base+secondary_cursor+append_bytes>U32_MAX ||
	    owner[8].bytes<20 || owner[22].bytes<708 ||
	    out->primary_capacity<old_primary_bytes+growth ||
	    out->secondary_capacity<append_bytes)
		return -ERANGE;
	new_bytes=old_primary_bytes+growth;
	if (overlaps(out->primary,new_bytes,out->secondary,append_bytes) ||
	    overlaps(out->primary,new_bytes,old_primary,old_primary_bytes) ||
	    overlaps(out->primary,new_bytes,first_secondary,first_bytes) ||
	    overlaps(out->primary,new_bytes,extra_first_secondary,extra_first_bytes) ||
	    overlaps(out->secondary,append_bytes,old_primary,old_primary_bytes) ||
	    overlaps(out->secondary,append_bytes,first_secondary,first_bytes) ||
	    overlaps(out->secondary,append_bytes,extra_first_secondary,extra_first_bytes))
		return -EINVAL;
	insert=old_primary_bytes-tail_bytes;
	/* The previous job may have a different declaration shape. Its source
	 * indirect range gives the actual retained start and length; verify that
	 * every earlier job accounts for one 2676/2700-byte append interval. */
	old_range=get(old_primary+insert-60+4);
	if (old_range<owner[29].base+1 ||
	    old_range-owner[29].base-1>secondary_cursor)
		return -EINVAL;
	old_second_start=old_range-owner[29].base-1;
	old_last_bytes=secondary_cursor-old_second_start;
	if (old_last_bytes!=(old_draw_count==1 ? first_bytes : 2676) &&
	    !(old_draw_count>1&&old_last_bytes==2700))
		return -EINVAL;
	if (old_draw_count==1) {
		if (old_second_start) return -EINVAL;
	} else {
		u32 prior=old_draw_count-2, spent;
		if (old_second_start<first_bytes) return -EINVAL;
		spent=old_second_start-first_bytes;
		if (spent<prior*2676 || spent>prior*2700 ||
		    (spent-prior*2676)%24) return -EINVAL;
	}
	first_end_mid=old_last_bytes-870;
	if (get(old_primary+insert)!=0x45454524 ||
	    get(old_primary+insert-60)!=0x45452d2c ||
	    get(old_primary+insert-12)!=0x45452d2c ||
	    get(old_primary+insert-60+4)!=
	       owner[29].base+old_second_start+1 ||
	    get(old_primary+insert-60+8)!=
	       owner[29].base+old_second_start+first_end_mid ||
	    get(old_primary+insert-60+52)!=
	       owner[29].base+old_second_start+first_end_mid-1 ||
	    get(old_primary+insert-60+56)!=
	       owner[29].base+secondary_cursor+2 ||
	    old_end_bytes[10]!=init_bytes+insert ||
	    old_end_bytes[13]!=primary_cursor ||
	    (old_draw_count==1 &&
	     (get(old_primary+insert+3288)!=0x454545c0 ||
	      get(old_primary+insert+3292)!=4)))
		return -EINVAL;
	old_epoch=low+(old_draw_count-1)*0xa0;
	next_epoch=old_epoch+0xa0;
	for (i=0;i<ARRAY_SIZE(lock_at_new);i++) {
		u32 at=old_draw_count==1 ? lock_at_first[i] : lock_at_new[i];
		if (get(old_primary+insert+at)!=
		    ((old_epoch+lock_delta[i])|lock_tag[i]))
			return -EINVAL;
	}
	for (i=0;i<ARRAY_SIZE(save_at);i++)
		if ((owner[save_owner[i]].base&7) ||
		    (u64)owner[save_owner[i]].base+save_offset[i]+save_tag[i]>
	    U32_MAX)
			return -ERANGE;
	source_raw=get(extra_first_secondary+328)&~7u;
	if (source_raw<owner[28].base ||
	    source_raw-owner[28].base>=owner[28].bytes ||
	    get(extra_first_secondary+328)!=source_raw+1)
		return -ERANGE;
	/* Every branch after this point only writes into caller-owned output. */
	memcpy(out->primary,old_primary,insert);
	memcpy(out->primary+insert,old_primary+insert-60,60);
	put(out->primary+insert+4,owner[29].base+secondary_cursor+1);
	put(out->primary+insert+8,owner[29].base+secondary_cursor+1830-shift);
	put(out->primary+insert+52,owner[29].base+secondary_cursor+1829-shift);
	put(out->primary+insert+56,owner[29].base+secondary_cursor+append_bytes+2);
	if (old_draw_count==1) {
		memcpy(out->primary+insert+60,old_primary+insert,3288);
		memcpy(out->primary+insert+60+3288,
		       old_primary+insert+3296,tail_bytes-3296);
	} else {
		memcpy(out->primary+insert+60,old_primary+insert,tail_bytes);
	}
	for (i=0;i<ARRAY_SIZE(lock_at_new);i++)
		put(out->primary+insert+60+lock_at_new[i],
		    (next_epoch+lock_delta[i])|lock_tag[i]);
	dst=out->secondary;
	memset(dst,0,append_bytes);
	for (i=0;i<ARRAY_SIZE(save_prefix);i++)
		put(dst+i*4,save_prefix[i]);
	for (i=0;i<ARRAY_SIZE(save_at);i++)
		put(dst+save_at[i],
		    (owner[save_owner[i]].base+save_offset[i])|save_tag[i]);
	for (i=0;i<ARRAY_SIZE(copies);i++) {
		u32 from=copies[i].from,to=copies[i].to,bytes=copies[i].bytes;
		if (shift && from==444 && to==692)
			bytes-=shift; /* one four-word declaration group */
		else if (shift && from>=576)
			from-=shift;
		if (shift && to>=868)
			to-=shift;
		memcpy(dst+to,extra_first_secondary+from,bytes);
	}
	memcpy(dst+588,extra_first_secondary+2432-shift,104);
	/* 248f2 encodes the distance to the post-declaration target. The
	 * source slot at172 precedes the removed group; only this branch loses
	 * six DWORDs when the new job declares POSITION alone. */
	put(dst+172,0x49d00000u|((892-shift-172)/4));
	put(dst+188,3); put(dst+192,0x45e3e6e6); put(dst+196,1);
	for (i=0;i<ARRAY_SIZE(secondary_epoch);i++)
		put(dst+moved(secondary_epoch[i].at,shift),
		    (old_epoch+secondary_epoch[i].delta)|
		    secondary_epoch[i].tag);
	put(dst+452,((get(extra_first_secondary+304)&0xff1fffffu)|
	    0x00100000u)&0xfffffffcu);
	put(dst+476,get(extra_first_secondary+328));
	put(dst+936-shift,0x49d00000u|((1388-936)/4));
	put(dst+1424-shift,0x49d00000u|((1816-1424)/4));
	for (i=0;i<M9_FAA_FIRST_APPEND_ENDS;i++)
		out->end_bytes[i]=old_end_bytes[i]+
		    (i<10 ? 0 : i==10 ? 60 : growth);
	out->primary_bytes=new_bytes;
	out->secondary_bytes=append_bytes;
	out->final_point=old_final_point+0xa0;
	return 0;
}
