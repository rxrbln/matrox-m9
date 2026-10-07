/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include "m9_faa_graph_frame_append_encode.h"

struct span { u16 from, to, bytes; };
/* Original M9128 source maps the qualified bound first-stage packet to its
 * second same-frame DRAW. These are producer slice relationships, not source
 * addresses. Every omitted destination word is rebuilt below. */
static const struct span copies[] = {
	{0,144,28}, {32,176,12}, {44,200,4}, {60,208,44},
	{108,256,196}, {308,456,20}, {332,480,108}, {440,600,4},
	{444,692,176}, {636,868,96}, {736,968,8}, {748,980,44},
	{796,1028,424}, {1224,1456,8}, {1236,1468,44},
	{1284,1516,324}, {1612,1844,24}, {1640,1872,8},
	{1652,1884,56}, {1712,1944,92}, {1808,2040,660},
};
struct token_copy { u16 from, to; };
static const struct token_copy token[] = {
	{56,204}, {104,252}, {732,964}, {744,976}, {792,1024},
	{1220,1452}, {1232,1464}, {1280,1512}, {1608,1840},
	{1636,1868}, {1648,1880}, {1708,1940}, {1804,2036},
};
/* fce50/fc8f0 sentinel and pool save: all literal controls are source
 * instructions for this fixed profile. Six typed owner bindings are zeroed
 * here and derived below from the live retained native allocations. */
static const u32 save_prefix[36] = {
	1,0x45454524,0,3,0x453ce6e6,0x6dc00000,0x42000000,0xa1,
	9,0x44444444,0x30081898,0x30081898,0x30081898,0x30081898,
	0x44444444,0x30081898,0x30081898,0x30081898,0x30081898,
	2,0x45452f2e,0,0,2,0x4545421a,0,0xa1000009,
	2,0x4545421a,0,0xa3000009,3,0x45e6421a,0,0xa4000009,
	0x40008000,
};

static u32 get(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 |
	       (u32)p[3] << 24;
}

static void put(u8 *p, u32 v)
{
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static bool overlaps(const void *a, u32 asize, const void *b, u32 bsize)
{
	unsigned long x = (unsigned long)a, y = (unsigned long)b;

	return x <= y ? y - x < asize : x - y < bsize;
}

int m9x_faa_frame_append_encode(
	const struct m9x_faa_graph_draw_owner owner[31],
	u32 primary_cursor, u32 secondary_cursor, u64 live_next_epoch,
	u32 cb_owner_offset, u32 vertex_owner_offset,
	const struct m9x_faa_color_input *in, bool reused_program,
	const u8 *first_color_secondary, u32 first_color_bytes,
	const u8 *bound_first_primary, u32 bound_primary_bytes,
	const u8 *bound_first_secondary, u32 bound_secondary_bytes,
	const struct m9x_faa_frame_append_image *out)
{
	static const u16 bind_at[] = {8,84,88,100,116,132};
	static const u8 bind_owner[] = {22,22,22,8,8,8};
	static const u16 bind_offset[] = {0,640,704,8,8,16};
	static const u8 bind_tag[] = {1,5,5,1,5,1};
	static const struct { u16 at, delta; u8 tag; } lock_token[] = {
		{128, 0x170, 1}, {184, 0x180, 1},
		{320, 0x180, 0}, {3084, 0x180, 0},
		{3548, 0x170, 0}, {3568, 0x190, 1},
	};
	u32 i, value, low = (u32)live_next_epoch, prior_cb_base;
	u32 gather_bytes, vertex_bytes;
	u32 insert, old_secondary_start;
	u8 *secondary;

	if (!in)
		return -EINVAL;
	if (in->layout.entry_bytes[3] < in->layout.entry_bytes[2] ||
	    in->record_count > 12288 || in->record_count > U32_MAX / 32)
		return -ERANGE;
	gather_bytes = in->layout.entry_bytes[3] - in->layout.entry_bytes[2];
	vertex_bytes = in->record_count * 32;
	if (!owner || !in || !out || !out->primary || !out->secondary ||
	    !first_color_secondary || !bound_first_primary ||
	    !bound_first_secondary || !reused_program ||
	    first_color_bytes != 2712 || bound_secondary_bytes != 2468 ||
	    bound_primary_bytes < M9_FAA_FRAME_APPEND_LOCK_TAIL_BYTES + 60 ||
	    bound_primary_bytes > U32_MAX - M9_FAA_FRAME_APPEND_PRIMARY_INSERT_BYTES ||
	    out->primary_capacity < bound_primary_bytes +
	        M9_FAA_FRAME_APPEND_PRIMARY_INSERT_BYTES ||
	    out->secondary_capacity < 2700 ||
	    overlaps(out->primary, bound_primary_bytes + 60,
	             out->secondary, 2700) ||
	    overlaps(out->primary, bound_primary_bytes + 60,
	             first_color_secondary, 2712) ||
	    overlaps(out->primary, bound_primary_bytes + 60,
	             bound_first_primary, bound_primary_bytes) ||
	    overlaps(out->primary, bound_primary_bytes + 60,
	             bound_first_secondary, 2468) ||
	    overlaps(out->secondary, 2700, first_color_secondary, 2712) ||
	    overlaps(out->secondary, 2700, bound_first_primary, bound_primary_bytes) ||
	    overlaps(out->secondary, 2700, bound_first_secondary, 2468) ||
	    (primary_cursor & 3) || (secondary_cursor & 3) ||
	    (live_next_epoch & 15) || low < 0xb0 || low > U32_MAX - 0x230 ||
	    in->layout.program_count != 4 ||
	    !in->layout.program_bytes || in->layout.program_bytes > 4096 ||
	    (in->layout.program_bytes & 7) ||
	    in->layout.entry_bytes[0] != 0 ||
	    in->layout.entry_bytes[1] < 8 ||
	    in->layout.entry_bytes[2] <= in->layout.entry_bytes[1] ||
	    in->layout.entry_bytes[3] <= in->layout.entry_bytes[2] ||
	    in->layout.entry_bytes[3] > in->layout.program_bytes ||
	    in->layout.program_bytes - in->layout.entry_bytes[3] != 200 ||
	    (in->layout.entry_bytes[1] & 7) ||
	    (in->layout.entry_bytes[2] & 7) ||
	    (in->layout.entry_bytes[3] & 7) ||
	    (gather_bytes != 32 && gather_bytes != 40) ||
	    !in->layout.constants_bytes ||
	    in->layout.constants_bytes > 16384 ||
	    (in->layout.constants_bytes & 7) ||
	    in->record_stride != 32 || !in->record_count ||
	    in->record_count % 3 ||
	    in->declaration_count != 5 ||
	    (cb_owner_offset & 7) || (vertex_owner_offset & 7) ||
	    cb_owner_offset > owner[28].bytes ||
	    owner[28].bytes - cb_owner_offset < in->layout.constants_bytes ||
	    vertex_owner_offset > owner[28].bytes ||
	    owner[28].bytes - vertex_owner_offset < vertex_bytes ||
	    (u64)owner[28].base + cb_owner_offset +
	        in->layout.constants_bytes > U32_MAX ||
	    (u64)owner[28].base + vertex_owner_offset + vertex_bytes > U32_MAX ||
	    primary_cursor < bound_primary_bytes ||
	    primary_cursor > owner[27].bytes ||
	    owner[27].bytes - primary_cursor < 60 ||
	    secondary_cursor > owner[29].bytes ||
	    owner[29].bytes - secondary_cursor < 2700 ||
	    owner[8].bytes < 20 || owner[22].bytes < 708 ||
	    get(bound_first_secondary + 56) != (low + 0xd0 | 1) ||
	    get(bound_first_secondary + 104) != (low + 0xe0 | 1) ||
	    get(bound_first_primary + bound_primary_bytes - 4) !=
	        (low + 0x190 | 1) ||
	    (u64)owner[27].base + primary_cursor + 60 > U32_MAX ||
	    (u64)owner[29].base + secondary_cursor + 2700 > U32_MAX)
		return -EINVAL;
	insert = bound_primary_bytes - M9_FAA_FRAME_APPEND_LOCK_TAIL_BYTES;
	old_secondary_start = secondary_cursor - bound_secondary_bytes;
	if (secondary_cursor < bound_secondary_bytes ||
	    get(bound_first_primary + insert - 60) != 0x45452d2c ||
	    get(bound_first_primary + insert - 60 + 48) != 0x45452d2c ||
	    get(bound_first_primary + insert) != 0x45454524 ||
	    get(bound_first_primary + insert - 60 + 4) !=
	        owner[29].base + old_secondary_start + 1 ||
	    get(bound_first_primary + insert - 60 + 8) !=
	        owner[29].base + old_secondary_start + 1598 ||
	    get(bound_first_primary + insert - 60 + 52) !=
	        owner[29].base + old_secondary_start + 1597 ||
	    get(bound_first_primary + insert - 60 + 56) !=
	        owner[29].base + secondary_cursor + 2)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(lock_token); i++) {
		value = low + lock_token[i].delta;
		if (get(bound_first_primary + insert + lock_token[i].at) !=
		    (value | lock_token[i].tag))
			return -EINVAL;
	}
	for (i = 0; i < ARRAY_SIZE(bind_at); i++) {
		if ((owner[bind_owner[i]].base & 7) ||
		    (u64)owner[bind_owner[i]].base + bind_offset[i] >
		    U32_MAX - bind_tag[i])
			return -ERANGE;
	}
	/* The regenerated first-stage CB bind at 2120 identifies the old
	 * owner-relative CB origin. A later caller can retain it or append a new
	 * CB; all eight consumers preserve their original live span offsets. */
	prior_cb_base = get(bound_first_secondary + 2120) & ~7u;
	if (prior_cb_base < owner[28].base ||
	    prior_cb_base - owner[28].base > owner[28].bytes ||
	    owner[28].bytes - (prior_cb_base - owner[28].base) <
	        in->layout.constants_bytes)
		return -ERANGE;
	{ static const u16 cb_from[] = {372,376,900,904,1392,1396,2120,2124};
	for (i = 0; i < ARRAY_SIZE(cb_from); i++) {
		value = get(bound_first_secondary + cb_from[i]) & ~7u;
		if (value < prior_cb_base ||
		    value - prior_cb_base > in->layout.constants_bytes)
			return -ERANGE;
	}
	}
	/* The first DRAW ends at insert. Its 60-byte primary indirect packet is
	 * source-repeated with four pointers to the freshly appended secondary
	 * range. Move the still-unposted Lock tail after it, then advance every
	 * source GPU dependency in that tail by the second DRAW's 0xa0 epoch. */
	memcpy(out->primary, bound_first_primary, insert);
	memcpy(out->primary + insert,
	       bound_first_primary + insert - 60, 60);
	put(out->primary + insert + 4,
	    owner[29].base + secondary_cursor + 1);
	put(out->primary + insert + 8,
	    owner[29].base + secondary_cursor + 1830);
	put(out->primary + insert + 52,
	    owner[29].base + secondary_cursor + 1829);
	put(out->primary + insert + 56,
	    owner[29].base + secondary_cursor + 2702);
	memcpy(out->primary + insert + 60,
	       bound_first_primary + insert,
	       M9_FAA_FRAME_APPEND_LOCK_TAIL_BYTES);
	for (i = 0; i < ARRAY_SIZE(lock_token); i++)
		put(out->primary + insert + 60 + lock_token[i].at,
		    (low + lock_token[i].delta + 0xa0) |
		    lock_token[i].tag);
	secondary = out->secondary;
	memset(secondary, 0, 2700);
	for (i = 0; i < ARRAY_SIZE(save_prefix); i++)
		put(secondary + i * 4, save_prefix[i]);
	for (i = 0; i < ARRAY_SIZE(bind_at); i++) {
		value = owner[bind_owner[i]].base + bind_offset[i];
		put(secondary + bind_at[i], value | bind_tag[i]);
	}
	for (i = 0; i < ARRAY_SIZE(copies); i++)
		memcpy(secondary + copies[i].to,
		       bound_first_secondary + copies[i].from, copies[i].bytes);
	/* The source's first-only raster-save state is repeated from the
	 * already live-encoded initial COLOR4 state, preserving live metadata. */
	memcpy(secondary + 588, first_color_secondary + 2432, 104);
	put(secondary + 172, 0x49d00000 | ((892 - 172) / 4));
	put(secondary + 188, 3);
	put(secondary + 192, 0x45e3e6e6);
	put(secondary + 196, 1);
	for (i = 0; i < ARRAY_SIZE(token); i++)
		put(secondary + token[i].to,
		    get(bound_first_secondary + token[i].from) + 0xa0);
	put(secondary + 452,
	    ((get(bound_first_secondary + 304) & 0xff1fffff) |
	     0x00100000) & 0xfffffffc);
	put(secondary + 476, (owner[28].base + vertex_owner_offset) | 1);
	/* Eight source kind13/14 bindings use the current CB allocation. Its
	 * base may be retained from draw1 or point to a newly appended CB0. */
	{ static const struct { u16 at, from; } cb[] = {
		{520,372}, {524,376}, {1132,900}, {1136,904},
		{1624,1392}, {1628,1396}, {2352,2120}, {2356,2124},
	};
	for (i = 0; i < ARRAY_SIZE(cb); i++) {
		value = get(bound_first_secondary + cb[i].from);
		put(secondary + cb[i].at,
		    (owner[28].base + cb_owner_offset +
		    ((value & ~7u) - prior_cb_base)) | (value & 7));
	}
	}
	put(secondary + 936, 0x49d00000 | ((1388 - 936) / 4));
	put(secondary + 1424, 0x49d00000 | ((1816 - 1424) / 4));
	return 0;
}
