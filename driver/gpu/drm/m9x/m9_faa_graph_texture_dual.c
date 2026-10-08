// SPDX-License-Identifier: GPL-2.0-only
/* Original 102b:0540 two-texture packet motif; no captured GPU address is
 * copied. Neither a texture upload nor a whole-draw admission lives here. */
#ifdef M9_FAA_TEXTURE_HOST_TEST
#include <errno.h>
#include <stdint.h>
#define U32_MAX UINT32_MAX
#else
#include <linux/errno.h>
#include <linux/limits.h>
#endif
#include "m9_faa_graph_texture_dual.h"

static u32 rd(const u8 *p, u32 word)
{
	const u8 *b = p + 4 * word;
	return (u32)b[0] | (u32)b[1] << 8 | (u32)b[2] << 16 |
	       (u32)b[3] << 24;
}

static void wr(u8 *p, u32 word, u32 value)
{
	u8 *b = p + 4 * word;
	b[0] = value;
	b[1] = value >> 8;
	b[2] = value >> 16;
	b[3] = value >> 24;
}

int m9x_faa_texture_dual_bind_place(
	const struct m9x_faa_texture_bind_input unit[2],
	const struct m9x_faa_texture_dual_program *program,
	u8 *secondary, u32 bytes)
{
	struct m9x_faa_texture_bind_input input;
	struct m9x_faa_texture_bind_words image[2];
	u32 stream = 0, raster = 0, site[2] = {0, 0}, words, i, j;
	int ret;

	if (!unit || !program || !secondary || (bytes & 3) ||
	    bytes < 64 || bytes > 0x40000 ||
	    program->stage_count != 4 || program->sampler_count != 2 ||
	    program->coordinate_sets != 2 || program->color_op0 != 4 ||
	    program->color_op1 != 4 || program->entry_bytes[0] != 0 ||
	    program->entry_bytes[1] != 72 || program->entry_bytes[2] != 144 ||
	    program->entry_bytes[3] != 200 ||
	    unit[0].stage != 0 || unit[1].stage != 1 ||
	    unit[0].coordinate_sets != 2 || unit[1].coordinate_sets != 2 ||
	    unit[0].coordinate_index != 0 || unit[1].coordinate_index != 1 ||
	    unit[0].owner_base == unit[1].owner_base ||
	    (unsigned long long)unit[0].owner_base + unit[0].owner_bytes > U32_MAX ||
	    (unsigned long long)unit[1].owner_base + unit[1].owner_bytes > U32_MAX ||
	    ((unsigned long long)unit[0].owner_base <
	     (unsigned long long)unit[1].owner_base + unit[1].owner_bytes &&
	     (unsigned long long)unit[1].owner_base <
	     (unsigned long long)unit[0].owner_base + unit[0].owner_bytes))
		return -EINVAL;

	for (j = 0; j < 2; j++) {
		/* 467e0 and f44xx use the same descriptor/sampler producer for
		 * both units. Bind helper coordinate words are intentionally unused:
		 * the dual stream/raster program is checked independently below. */
		input = unit[j];
		input.stage = 0;
		input.raster_ae_base = 0x28207006;
		ret = m9x_faa_texture_bind_encode(&input, &image[j]);
		if (ret)
			return ret;
	}

	words = bytes / 4;
	for (i = 1; i + 11 < words; i++) {
		if (rd(secondary, i) == 4 &&
		    rd(secondary, i + 1) == 0x23232323 &&
		    rd(secondary, i + 2) == 0x05300d88 &&
		    rd(secondary, i + 3) == 0x05300b36 &&
		    rd(secondary, i + 4) == 0x07400d88 &&
		    rd(secondary, i + 5) == 0x07400b36) {
			if (stream)
				return -EINVAL;
			stream = i;
		}
		if (rd(secondary, i) == 3 &&
		    rd(secondary, i + 1) == 0x45aeacab &&
		    rd(secondary, i + 3) == 0x2820c8c6 &&
		    rd(secondary, i + 4) == 0x22444442) {
			if (raster)
				return -EINVAL;
			raster = i;
		}
		for (j = 0; j < 2; j++) {
			if (rd(secondary, i - 1) == 1 &&
			    rd(secondary, i) == 0x45454570 + j &&
			    rd(secondary, i + 2) == 3 &&
			    rd(secondary, i + 3) == 0x457a7978 &&
			    rd(secondary, i + 7) == 3 &&
			    rd(secondary, i + 8) == 0x45877d7c) {
				if (site[j])
					return -EINVAL;
				site[j] = i;
			}
		}
	}
	if (!stream || !raster || !site[0] || !site[1] ||
	    !(stream < raster && raster < site[0] && site[0] < site[1]))
		return -EINVAL;

	/* Full preflight precedes all writes, including distinct live-owner and
	 * packet-order validation. Stable opcode words came from source f44xx. */
	for (j = 0; j < 2; j++) {
		i = site[j];
		wr(secondary, i + 1, image[j].unit70);
		wr(secondary, i + 4, image[j].address78);
		wr(secondary, i + 5, image[j].descriptor79);
		wr(secondary, i + 6, image[j].descriptor7a);
		wr(secondary, i + 9, image[j].sampler7c);
		wr(secondary, i + 10, image[j].sampler7d);
		wr(secondary, i + 11, image[j].sampler87);
	}
	return 0;
}
