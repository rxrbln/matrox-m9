// SPDX-License-Identifier: GPL-2.0-only
/* Locate typed four-stage source packets; never copy a captured command. */
#ifdef M9_FAA_TEXTURE_HOST_TEST
#include <errno.h>
#else
#include <linux/errno.h>
#endif
#include "m9_faa_graph_texture_place.h"

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

int m9x_faa_texture_bind_place(
	const struct m9x_faa_texture_bind_input *in,
	const struct m9x_faa_texture_program_interface *program,
	u8 *secondary, u32 bytes)
{
	struct m9x_faa_texture_bind_words image;
	u32 stream = 0, raster = 0, texture = 0, words, i;
	int ret;

	if (!secondary || !program || !bytes || (bytes & 3) ||
	    bytes < 48 || bytes > 0x40000 ||
	    program->stage_count != 4 || program->sampler_count != 1 ||
	    program->color_stream != 0 || program->texcoord_stream != 1 ||
	    (program->color_op != 4 && program->color_op != 7) ||
	    program->entry_bytes[0] != 48 || program->entry_bytes[1] != 104 ||
	    program->entry_bytes[2] != 144 || program->entry_bytes[3] != 0)
		return -EINVAL;
	ret = m9x_faa_texture_bind_encode(in, &image);
	if (ret)
		return ret;
	words = bytes / 4;
	for (i = 1; i + 11 < words; i++) {
		if (rd(secondary, i) == 4 &&
		    rd(secondary, i + 1) == 0x23232323 &&
		    (rd(secondary, i + 2) & ~(1u << 25)) == 0x05300d88 &&
		    (rd(secondary, i + 3) & ~(1u << 25)) == 0x05300b36 &&
		    rd(secondary, i + 4) == 0x4b08 &&
		    rd(secondary, i + 5) == 0) {
			if (stream)
				return -EINVAL;
			stream = i;
		}
		if (rd(secondary, i) == 3 &&
		    rd(secondary, i + 1) == 0x45aeacab &&
		    (rd(secondary, i + 3) == in->raster_ae_base ||
		     rd(secondary, i + 3) == in->raster_ae_base + 0x40) &&
		    rd(secondary, i + 4) == 0x22444442) {
			if (raster)
				return -EINVAL;
			raster = i;
		}
		if (rd(secondary, i) == 0x45454570 &&
		    rd(secondary, i - 1) == 1 &&
		    rd(secondary, i + 2) == 3 &&
		    rd(secondary, i + 3) == 0x457a7978 &&
		    rd(secondary, i + 7) == 3 &&
		    rd(secondary, i + 8) == 0x45877d7c) {
			if (texture)
				return -EINVAL;
			texture = i;
		}
	}
	if (!stream || !raster || !texture ||
	    !(stream < raster && raster < texture))
		return -EINVAL;

	/* Every packet site was checked before the first write. Texture address
	 * is always replaced with the retained owner's live VRAM address. */
	wr(secondary, stream + 2, image.stream0);
	wr(secondary, stream + 3, image.stream1);
	wr(secondary, raster + 3, image.raster_ae);
	wr(secondary, texture + 1, image.unit70);
	wr(secondary, texture + 4, image.address78);
	wr(secondary, texture + 5, image.descriptor79);
	wr(secondary, texture + 6, image.descriptor7a);
	wr(secondary, texture + 9, image.sampler7c);
	wr(secondary, texture + 10, image.sampler7d);
	wr(secondary, texture + 11, image.sampler87);
	return 0;
}
