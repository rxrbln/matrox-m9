// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* Locate typed four-stage source packets; never copy a captured command. */
#ifdef M9_FAA_TEXTURE_HOST_TEST
#include <errno.h>
#else
#include <linux/errno.h>
#endif
#include "m9_faa_graph_texture_place.h"
/* After the host-test typedefs of the texture headers. */
#include "m9_cmd.h"

int m9x_faa_texture_bind_place(const struct m9x_faa_texture_bind_input *in,
			       const struct m9x_faa_texture_program_interface *program,
			       u8 *secondary, u32 bytes)
{
	struct m9x_faa_texture_bind_words image;
	u32 stream = 0, raster = 0, texture = 0, words, i;
	int ret;

	if (!secondary || !program || !bytes || (bytes & 3) || bytes < 48 ||
	    bytes > 0x40000 || program->stage_count != 4 ||
	    program->sampler_count != 1 || program->color_stream != 0 ||
	    program->texcoord_stream != 1 ||
	    (program->color_op != 4 && program->color_op != 7) ||
	    program->entry_bytes[0] != 48 || program->entry_bytes[1] != 104 ||
	    program->entry_bytes[2] != 144 || program->entry_bytes[3] != 0)
		return -EINVAL;
	ret = m9x_faa_texture_bind_encode(in, &image);
	if (ret)
		return ret;
	words = bytes / 4;
	for (i = 1; i + 11 < words; i++) {
		if (m9_cmd_get(secondary + 4 * i) == 4 &&
		    m9_cmd_get(secondary + 4 * (i + 1)) == 0x23232323 &&
		    (m9_cmd_get(secondary + 4 * (i + 2)) & ~(1u << 25)) ==
			    0x05300d88 &&
		    (m9_cmd_get(secondary + 4 * (i + 3)) & ~(1u << 25)) ==
			    0x05300b36 &&
		    m9_cmd_get(secondary + 4 * (i + 4)) == 0x4b08 &&
		    m9_cmd_get(secondary + 4 * (i + 5)) == 0) {
			if (stream)
				return -EINVAL;
			stream = i;
		}
		if (m9_cmd_get(secondary + 4 * i) == 3 &&
		    m9_cmd_get(secondary + 4 * (i + 1)) == 0x45aeacab &&
		    (m9_cmd_get(secondary + 4 * (i + 3)) ==
			     in->raster_ae_base ||
		     m9_cmd_get(secondary + 4 * (i + 3)) ==
			     in->raster_ae_base + 0x40) &&
		    m9_cmd_get(secondary + 4 * (i + 4)) == 0x22444442) {
			if (raster)
				return -EINVAL;
			raster = i;
		}
		if (m9_cmd_get(secondary + 4 * i) == 0x45454570 &&
		    m9_cmd_get(secondary + 4 * (i - 1)) == 1 &&
		    m9_cmd_get(secondary + 4 * (i + 2)) == 3 &&
		    m9_cmd_get(secondary + 4 * (i + 3)) == 0x457a7978 &&
		    m9_cmd_get(secondary + 4 * (i + 7)) == 3 &&
		    m9_cmd_get(secondary + 4 * (i + 8)) == 0x45877d7c) {
			if (texture)
				return -EINVAL;
			texture = i;
		}
	}
	if (!stream || !raster || !texture ||
	    !(stream < raster && raster < texture))
		return -EINVAL;

	/* Every packet site was checked before the first write. Texture address
	 * is always replaced with the retained owner's live VRAM address.
	 */
	m9_cmd_put(secondary + 4 * (stream + 2), image.stream0);
	m9_cmd_put(secondary + 4 * (stream + 3), image.stream1);
	m9_cmd_put(secondary + 4 * (raster + 3), image.raster_ae);
	m9_cmd_put(secondary + 4 * (texture + 1), image.unit70);
	m9_cmd_put(secondary + 4 * (texture + 4), image.address78);
	m9_cmd_put(secondary + 4 * (texture + 5), image.descriptor79);
	m9_cmd_put(secondary + 4 * (texture + 6), image.descriptor7a);
	m9_cmd_put(secondary + 4 * (texture + 9), image.sampler7c);
	m9_cmd_put(secondary + 4 * (texture + 10), image.sampler7d);
	m9_cmd_put(secondary + 4 * (texture + 11), image.sampler87);
	return 0;
}
