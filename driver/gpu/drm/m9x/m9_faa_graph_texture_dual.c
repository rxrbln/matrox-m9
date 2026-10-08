// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* Original 102b:0540 two-texture packet motif; no captured GPU address is
 * copied. Neither a texture upload nor a whole-draw admission lives here.
 */
#ifdef M9_FAA_TEXTURE_HOST_TEST
#include <errno.h>
#include <stdint.h>
#define U32_MAX UINT32_MAX
#else
#include <linux/errno.h>
#include <linux/limits.h>
#endif
#include "m9_faa_graph_texture_dual.h"
/* After the host-test typedefs of the texture headers. */
#include "m9_cmd.h"

int m9x_faa_texture_dual_bind_place(const struct m9x_faa_texture_bind_input
					    unit[2],
				    const struct m9x_faa_texture_dual_program *program,
				    u8 *secondary, u32 bytes)
{
	struct m9x_faa_texture_bind_input input;
	struct m9x_faa_texture_bind_words image[2];
	u32 stream = 0, raster = 0, site[2] = { 0, 0 }, words, i, j;
	int ret;

	if (!unit || !program || !secondary || (bytes & 3) || bytes < 64 ||
	    bytes > 0x40000 || program->stage_count != 4 ||
	    program->sampler_count != 2 || program->coordinate_sets != 2 ||
	    program->color_op0 != 4 || program->color_op1 != 4 ||
	    program->entry_bytes[0] != 0 || program->entry_bytes[1] != 72 ||
	    (program->entry_bytes[2] & 7) || (program->entry_bytes[3] & 7) ||
	    program->entry_bytes[2] <= program->entry_bytes[1] ||
	    program->entry_bytes[3] - program->entry_bytes[2] != 56 ||
	    !program->program_bytes || program->program_bytes > 4096 ||
	    (program->program_bytes & 7) ||
	    program->program_bytes <= program->entry_bytes[3] ||
	    (program->program_bytes - program->entry_bytes[3] != 192 &&
	     program->program_bytes - program->entry_bytes[3] != 200) ||
	    unit[0].stage != 0 || unit[1].stage != 1 ||
	    unit[0].coordinate_sets != 2 || unit[1].coordinate_sets != 2 ||
	    unit[0].coordinate_index != 0 || unit[1].coordinate_index != 1 ||
	    unit[0].owner_base == unit[1].owner_base ||
	    (unsigned long long)unit[0].owner_base + unit[0].owner_bytes >
		    U32_MAX ||
	    (unsigned long long)unit[1].owner_base + unit[1].owner_bytes >
		    U32_MAX ||
	    ((unsigned long long)unit[0].owner_base <
		     (unsigned long long)unit[1].owner_base +
			     unit[1].owner_bytes &&
	     (unsigned long long)unit[1].owner_base <
		     (unsigned long long)unit[0].owner_base +
			     unit[0].owner_bytes))
		return -EINVAL;

	for (j = 0; j < 2; j++) {
		/* 467e0 and f44xx use the same descriptor/sampler producer for
		 * both units. Bind helper coordinate words are intentionally unused:
		 * the dual stream/raster program is checked independently below.
		 */
		input = unit[j];
		input.stage = 0;
		input.raster_ae_base = 0x28207006;
		ret = m9x_faa_texture_bind_encode(&input, &image[j]);
		if (ret)
			return ret;
	}

	words = bytes / 4;
	for (i = 1; i + 11 < words; i++) {
		if (m9_cmd_get(secondary + 4 * i) == 4 &&
		    m9_cmd_get(secondary + 4 * (i + 1)) == 0x23232323 &&
		    (m9_cmd_get(secondary + 4 * (i + 2)) & 0x00ffffff) ==
			    0x300d88 &&
		    ((m9_cmd_get(secondary + 4 * (i + 3)) & 0x00ffffff) ==
			     0x300b36 ||
		     (m9_cmd_get(secondary + 4 * (i + 3)) & 0x00ffffff) ==
			     0x3006b6) &&
		    (m9_cmd_get(secondary + 4 * (i + 4)) & 0x00ffffff) ==
			    0x400d88 &&
		    ((m9_cmd_get(secondary + 4 * (i + 5)) & 0x00ffffff) ==
			     0x400b36 ||
		     (m9_cmd_get(secondary + 4 * (i + 5)) & 0x00ffffff) ==
			     0x4006b6)) {
			if (stream)
				return -EINVAL;
			stream = i;
		}
		if (m9_cmd_get(secondary + 4 * i) == 3 &&
		    m9_cmd_get(secondary + 4 * (i + 1)) == 0x45aeacab &&
		    ((m9_cmd_get(secondary + 4 * (i + 2)) == 0xffff &&
		      m9_cmd_get(secondary + 4 * (i + 3)) == 0) ||
		     m9_cmd_get(secondary + 4 * (i + 3)) == 0x2820c8c6) &&
		    m9_cmd_get(secondary + 4 * (i + 4)) == 0x22444442) {
			if (raster)
				return -EINVAL;
			raster = i;
		}
		for (j = 0; j < 2; j++) {
			if (m9_cmd_get(secondary + 4 * (i - 1)) == 1 &&
			    m9_cmd_get(secondary + 4 * i) == 0x45454570 + j &&
			    m9_cmd_get(secondary + 4 * (i + 2)) == 3 &&
			    m9_cmd_get(secondary + 4 * (i + 3)) == 0x457a7978 &&
			    m9_cmd_get(secondary + 4 * (i + 7)) == 3 &&
			    m9_cmd_get(secondary + 4 * (i + 8)) == 0x45877d7c) {
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
	 * packet-order validation. Stable opcode words came from source f44xx.
	 */
	for (j = 0; j < 2; j++) {
		i = site[j];
		m9_cmd_put(secondary + 4 * (i + 1), image[j].unit70);
		m9_cmd_put(secondary + 4 * (i + 4), image[j].address78);
		m9_cmd_put(secondary + 4 * (i + 5), image[j].descriptor79);
		m9_cmd_put(secondary + 4 * (i + 6), image[j].descriptor7a);
		m9_cmd_put(secondary + 4 * (i + 9), image[j].sampler7c);
		m9_cmd_put(secondary + 4 * (i + 10), image[j].sampler7d);
		m9_cmd_put(secondary + 4 * (i + 11), image[j].sampler87);
	}
	return 0;
}

int m9x_faa_texture_dual_bind_append_place(const struct m9x_faa_texture_bind_input unit[2],
					   const struct m9x_faa_texture_dual_program *program,
					   u8 *secondary, u32 bytes)
{
	static const u32 site[2] = { 2508, 2608 };
	struct m9x_faa_texture_bind_input input;
	struct m9x_faa_texture_bind_words image[2];
	u32 j, at;
	int ret;

	if (!unit || !program || !secondary || bytes != 2868 ||
	    program->stage_count != 4 || program->sampler_count != 2 ||
	    program->coordinate_sets != 2 || program->color_op0 != 4 ||
	    program->color_op1 != 4 || program->entry_bytes[0] ||
	    program->entry_bytes[1] != 72 || (program->entry_bytes[2] & 7) ||
	    (program->entry_bytes[3] & 7) ||
	    program->entry_bytes[2] <= program->entry_bytes[1] ||
	    program->entry_bytes[3] - program->entry_bytes[2] != 56 ||
	    !program->program_bytes || program->program_bytes > 4096 ||
	    (program->program_bytes & 7) ||
	    program->program_bytes <= program->entry_bytes[3] ||
	    (program->program_bytes - program->entry_bytes[3] != 192 &&
	     program->program_bytes - program->entry_bytes[3] != 200) ||
	    unit[0].stage != 0 || unit[1].stage != 1 ||
	    unit[0].coordinate_sets != 2 || unit[1].coordinate_sets != 2 ||
	    unit[0].coordinate_index != 0 || unit[1].coordinate_index != 1 ||
	    unit[0].owner_base == unit[1].owner_base ||
	    (unsigned long long)unit[0].owner_base + unit[0].owner_bytes >
		    U32_MAX ||
	    (unsigned long long)unit[1].owner_base + unit[1].owner_bytes >
		    U32_MAX ||
	    ((unsigned long long)unit[0].owner_base <
		     (unsigned long long)unit[1].owner_base +
			     unit[1].owner_bytes &&
	     (unsigned long long)unit[1].owner_base <
		     (unsigned long long)unit[0].owner_base +
			     unit[0].owner_bytes))
		return -EINVAL;
	for (j = 0; j < 2; j++) {
		input = unit[j];
		input.stage = 0;
		input.raster_ae_base = 0x28207006;
		ret = m9x_faa_texture_bind_encode(&input, &image[j]);
		if (ret)
			return ret;
		at = site[j] / 4;
		/* Check both real source packets before changing either. The
		 * earlier null-target 70 packet at offset 600 does not qualify.
		 */
		if (m9_cmd_get(secondary + 4 * (at - 1)) != 1 ||
		    m9_cmd_get(secondary + 4 * at) != 0x45454570 + j ||
		    m9_cmd_get(secondary + 4 * (at + 2)) != 3 ||
		    m9_cmd_get(secondary + 4 * (at + 3)) != 0x457a7978 ||
		    m9_cmd_get(secondary + 4 * (at + 7)) != 3 ||
		    m9_cmd_get(secondary + 4 * (at + 8)) != 0x45877d7c)
			return -EINVAL;
	}
	for (j = 0; j < 2; j++) {
		at = site[j] / 4;
		m9_cmd_put(secondary + 4 * (at + 1), image[j].unit70);
		m9_cmd_put(secondary + 4 * (at + 4), image[j].address78);
		m9_cmd_put(secondary + 4 * (at + 5), image[j].descriptor79);
		m9_cmd_put(secondary + 4 * (at + 6), image[j].descriptor7a);
		m9_cmd_put(secondary + 4 * (at + 9), image[j].sampler7c);
		m9_cmd_put(secondary + 4 * (at + 10), image[j].sampler7d);
		m9_cmd_put(secondary + 4 * (at + 11), image[j].sampler87);
	}
	return 0;
}

int m9x_faa_texture_dual_bind_later_place(const struct m9x_faa_texture_bind_input
						  unit[2],
					  const struct m9x_faa_texture_dual_program *program,
					  u8 *secondary, u32 bytes)
{
	struct m9x_faa_texture_bind_input input;
	struct m9x_faa_texture_bind_words image[2];
	u32 j, at, shift = bytes == 2740 ? 4 : 0;
	int ret;

	if (!unit || !program || !secondary ||
	    (bytes != 2736 && bytes != 2740) || program->stage_count != 4 ||
	    program->sampler_count != 2 || program->coordinate_sets != 2 ||
	    program->color_op0 != 4 || program->color_op1 != 4 ||
	    program->entry_bytes[0] || program->entry_bytes[1] != 72 ||
	    (program->entry_bytes[2] & 7) || (program->entry_bytes[3] & 7) ||
	    program->entry_bytes[2] <= program->entry_bytes[1] ||
	    program->entry_bytes[3] - program->entry_bytes[2] != 56 ||
	    !program->program_bytes || program->program_bytes > 4096 ||
	    (program->program_bytes & 7) ||
	    program->program_bytes <= program->entry_bytes[3] ||
	    (program->program_bytes - program->entry_bytes[3] != 192 &&
	     program->program_bytes - program->entry_bytes[3] != 200) ||
	    unit[0].stage || unit[1].stage != 1 ||
	    unit[0].coordinate_sets != 2 || unit[1].coordinate_sets != 2 ||
	    unit[0].coordinate_index || unit[1].coordinate_index != 1 ||
	    unit[0].owner_base == unit[1].owner_base ||
	    (unsigned long long)unit[0].owner_base + unit[0].owner_bytes >
		    U32_MAX ||
	    (unsigned long long)unit[1].owner_base + unit[1].owner_bytes >
		    U32_MAX ||
	    ((unsigned long long)unit[0].owner_base <
		     (unsigned long long)unit[1].owner_base +
			     unit[1].owner_bytes &&
	     (unsigned long long)unit[1].owner_base <
		     (unsigned long long)unit[0].owner_base +
			     unit[0].owner_bytes))
		return -EINVAL;
	for (j = 0; j < 2; j++) {
		input = unit[j];
		input.stage = 0;
		input.raster_ae_base = 0x28207006;
		ret = m9x_faa_texture_bind_encode(&input, &image[j]);
		if (ret)
			return ret;
		at = (2376 + 100 * j + shift) / 4;
		if (m9_cmd_get(secondary + 4 * (at - 1)) != 1 ||
		    m9_cmd_get(secondary + 4 * at) != 0x45454570 + j ||
		    m9_cmd_get(secondary + 4 * (at + 2)) != 3 ||
		    m9_cmd_get(secondary + 4 * (at + 3)) != 0x457a7978 ||
		    m9_cmd_get(secondary + 4 * (at + 7)) != 3 ||
		    m9_cmd_get(secondary + 4 * (at + 8)) != 0x45877d7c)
			return -EINVAL;
	}
	for (j = 0; j < 2; j++) {
		at = (2376 + 100 * j + shift) / 4;
		m9_cmd_put(secondary + 4 * (at + 1), image[j].unit70);
		m9_cmd_put(secondary + 4 * (at + 4), image[j].address78);
		m9_cmd_put(secondary + 4 * (at + 5), image[j].descriptor79);
		m9_cmd_put(secondary + 4 * (at + 6), image[j].descriptor7a);
		m9_cmd_put(secondary + 4 * (at + 9), image[j].sampler7c);
		m9_cmd_put(secondary + 4 * (at + 10), image[j].sampler7d);
		m9_cmd_put(secondary + 4 * (at + 11), image[j].sampler87);
	}
	return 0;
}

int m9x_faa_texture_dual_colorpass_place(const struct m9x_faa_texture_bind_input
						 unit[2],
					 const struct m9x_faa_texture_dual_program *program,
					 u8 *secondary, u32 bytes)
{
	struct m9x_faa_texture_bind_input live;
	struct m9x_faa_texture_bind_words image;
	int ret;

	if (!unit || !program || !secondary || bytes != 2792 ||
	    program->stage_count != 4 || program->sampler_count != 2 ||
	    program->coordinate_sets != 2 || program->color_op0 != 3 ||
	    program->color_op1 != 3 || program->entry_bytes[0] ||
	    program->entry_bytes[1] != 32 || !program->program_bytes ||
	    program->program_bytes > 4096 || (program->program_bytes & 7) ||
	    program->entry_bytes[2] <= program->entry_bytes[1] ||
	    program->entry_bytes[3] >= program->program_bytes ||
	    program->entry_bytes[3] - program->entry_bytes[2] != 56 ||
	    (program->entry_bytes[2] & 7) || (program->entry_bytes[3] & 7) ||
	    (program->program_bytes - program->entry_bytes[3] != 192 &&
	     program->program_bytes - program->entry_bytes[3] != 200) ||
	    unit[0].stage || unit[1].stage != 1 ||
	    unit[0].coordinate_sets != 2 || unit[1].coordinate_sets != 2 ||
	    unit[0].coordinate_index || unit[1].coordinate_index != 1 ||
	    unit[0].owner_base == unit[1].owner_base ||
	    (unsigned long long)unit[0].owner_base + unit[0].owner_bytes >
		    U32_MAX ||
	    (unsigned long long)unit[1].owner_base + unit[1].owner_bytes >
		    U32_MAX ||
	    ((unsigned long long)unit[0].owner_base <
		     (unsigned long long)unit[1].owner_base +
			     unit[1].owner_bytes &&
	     (unsigned long long)unit[1].owner_base <
		     (unsigned long long)unit[0].owner_base +
			     unit[0].owner_bytes) ||
	    m9_cmd_get(secondary + 4 * (2520 / 4)) != 0x45454570 ||
	    m9_cmd_get(secondary + 4 * (2528 / 4)) != 3 ||
	    m9_cmd_get(secondary + 4 * (2532 / 4)) != 0x457a7978 ||
	    m9_cmd_get(secondary + 4 * (2552 / 4)) != 0x45877d7c ||
	    m9_cmd_get(secondary + 4 * (2620 / 4)) != 0x45454571 ||
	    m9_cmd_get(secondary + 4 * (2624 / 4)) != 0 ||
	    m9_cmd_get(secondary + 4 * (2632 / 4)) != 0x45454572 ||
	    m9_cmd_get(secondary + 4 * (2644 / 4)) != 0x45454573)
		return -EINVAL;
	live = unit[0];
	live.raster_ae_base = 0x28207006;
	ret = m9x_faa_texture_bind_encode(&live, &image);
	if (ret)
		return ret;
	/* All packet and owner validation precedes every write. Unit 1 remains
	 * a retained owner, but SELECTARG2 does not sample it in this profile.
	 */
	m9_cmd_put(secondary + 4 * (2524 / 4), image.unit70);
	m9_cmd_put(secondary + 4 * (2536 / 4), image.address78);
	m9_cmd_put(secondary + 4 * (2540 / 4), image.descriptor79);
	m9_cmd_put(secondary + 4 * (2544 / 4), image.descriptor7a);
	m9_cmd_put(secondary + 4 * (2556 / 4), image.sampler7c);
	m9_cmd_put(secondary + 4 * (2560 / 4), image.sampler7d);
	m9_cmd_put(secondary + 4 * (2564 / 4), image.sampler87);
	return 0;
}
