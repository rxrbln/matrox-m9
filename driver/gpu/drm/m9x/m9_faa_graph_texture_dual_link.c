// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* Original M9128 102b:0540 C2/C3 producer: four live shader entries. */
#ifdef M9_FAA_TEXTURE_HOST_TEST
#include <errno.h>
#include <stdint.h>
#define U32_MAX UINT32_MAX
#else
#include <linux/errno.h>
#include <linux/limits.h>
#endif
#include "m9_faa_graph_texture_dual_link.h"

static u32 rd(const u8 *p, u32 at)
{
	return (u32)p[at] | (u32)p[at + 1] << 8 | (u32)p[at + 2] << 16 |
	       (u32)p[at + 3] << 24;
}

static void wr(u8 *p, u32 at, u32 value)
{
	p[at] = value;
	p[at + 1] = value >> 8;
	p[at + 2] = value >> 16;
	p[at + 3] = value >> 24;
}

int m9x_faa_texture_dual_link_place(const struct m9x_faa_texture_dual_link_input
					    *in,
				    u8 *secondary, u32 bytes)
{
	/* Source packet order: carry, gather, position, fragment. Stage index
	 * remains program-vector order FS/carry/gather/position.
	 */
	static const u32 c2[4] = { 2432, 364, 1032, 1660 };
	static const u32 c3[4] = { 2452, 384, 1052, 1680 };
	u32 program_word[4], start_word[4], end_word[4], count_word[4];
	u32 i, start, end;

	if (!in || !secondary || bytes != 2880 || (in->program_base & 4095) ||
	    (in->constants_base & 7) ||
	    (in->constants_tag != 1 && in->constants_tag != 2) ||
	    (in->program_offset & 7) || (in->data_offset & 7) ||
	    !in->program_bytes || (in->program_bytes & 7) ||
	    in->program_bytes > 4096 ||
	    in->program_offset > 4096 - in->program_bytes ||
	    in->program_base > U32_MAX - in->program_owner_bytes ||
	    in->constants_base > U32_MAX - in->constants_owner_bytes ||
	    in->program_offset > in->program_owner_bytes ||
	    in->program_bytes > in->program_owner_bytes - in->program_offset ||
	    in->data_offset > in->constants_owner_bytes ||
	    in->constants_bytes > in->constants_owner_bytes - in->data_offset ||
	    in->entry_bytes[0] || in->entry_bytes[1] != 72 ||
	    in->entry_bytes[2] <= in->entry_bytes[1] ||
	    in->entry_bytes[3] <= in->entry_bytes[2] ||
	    in->entry_bytes[3] - in->entry_bytes[2] != 56 ||
	    (in->program_bytes - in->entry_bytes[3] != 192 &&
	     in->program_bytes - in->entry_bytes[3] != 200))
		return -EINVAL;

	for (i = 0; i < 4; i++) {
		if ((in->entry_bytes[i] & 7) ||
		    in->entry_bytes[i] >= in->program_bytes ||
		    (in->constant_begin[i] & 7) || (in->constant_end[i] & 7) ||
		    in->constant_begin[i] > in->constant_end[i] ||
		    in->constant_end[i] > in->constants_bytes ||
		    in->constant_end[i] - in->constant_begin[i] > 504 ||
		    rd(secondary, c2[i] - 4) != 0x458fc3c2 ||
		    rd(secondary, c3[i] - 4) != 0x8f8e8d8c ||
		    rd(secondary, c2[i] + 8) != 0 ||
		    rd(secondary, c2[i] + 12) != 4)
			return -EINVAL;
		start = in->data_offset + in->constant_begin[i];
		end = in->data_offset + in->constant_end[i];
		/* C3 tags the authenticated constant-owner domain; FIRST-depth
		 * owner28 is SYSTEM/tag1, while the standalone shader control used
		 * LOCAL/tag2. C2 retains one page-rooted program identity.
		 */
		start_word[i] = (in->constants_base + start) |
				in->constants_tag;
		end_word[i] = (in->constants_base + end) | in->constants_tag;
		program_word[i] = (in->program_offset + in->entry_bytes[i]) / 8;
		count_word[i] =
			0x00500000 |
			((in->constant_end[i] - in->constant_begin[i]) << 5);
	}
	for (i = 0; i < 4; i++) {
		wr(secondary, c2[i], in->program_base | 1);
		wr(secondary, c2[i] + 4, program_word[i]);
		wr(secondary, c3[i], count_word[i]);
		wr(secondary, c3[i] + 4, start_word[i]);
		wr(secondary, c3[i] + 8, end_word[i]);
	}
	return 0;
}
