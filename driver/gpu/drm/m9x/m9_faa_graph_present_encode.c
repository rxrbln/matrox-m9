// SPDX-License-Identifier: GPL-2.0-only
// Copyright 2026 René Rebe <rene@exactco.de>
#include <linux/errno.h>
#include <linux/string.h>
#include "m9_faa_graph_present_encode.h"
#include "m9_faa_graph_surface_layout.h"

static bool view_valid(const struct m9x_faa_present_view *v, bool source)
{
	u64 end;
	struct m9x_faa_surface_layout layout;

	if (!v || !v->gpu_base || (v->gpu_base & 0xfff) ||
	    !m9x_faa_runtime_shape(v->width, v->height) ||
	    !m9x_faa_surface_layout(v->width, v->height, &layout) ||
	    v->span != layout.primary.allocation_width ||
	    v->bytes != (source ? layout.owner[0].capacity :
				  layout.owner[0].logical) ||
	    v->descriptor1 != (0x10000u | v->height) ||
	    v->descriptor0 != ((source ? 0xf0220000u : 0xcc210000u) | v->span))
		return false;
	end = (u64)v->gpu_base + v->bytes;
	return end <= 0x100000000ULL;
}

int m9x_faa_present_encode(const struct m9x_faa_present_view *source,
			   const struct m9x_faa_present_view *destination,
			   u64 unlock_point, struct m9x_faa_present_image *out)
{
	struct m9x_faa_present_image image;
	u32 low = (u32)unlock_point, i;
	u32 words[25] = {
		0x45e3e6e6, 1,		2,	    0,		0x90928fe6,
		0x04000001, 0xc0000600, 0xffffffff, 0,		0xf0b1b091,
		0,	    0,		0,	    0x080c040c, 0xf2f37978,
		0,	    0,		0,	    0,		0x4545fcef,
		0xaa,	    0,		0x4545e3e6, 1,		0,
	};
	if (!out || !view_valid(source, true) ||
	    !view_valid(destination, false) ||
	    source->width != destination->width ||
	    source->height != destination->height || !unlock_point ||
	    (low & 15) || low > 0xffffffcfu ||
	    unlock_point > (~(u64)0) - 0x20 ||
	    ((u64)source->gpu_base <
		     (u64)destination->gpu_base + destination->bytes &&
	     (u64)destination->gpu_base <
		     (u64)source->gpu_base + source->bytes))
		return -EINVAL;
	words[3] = (u32)(unlock_point + 0x10) | 1;
	words[11] = destination->gpu_base | 1;
	/* Original selector8 adjusts format66 to78; selector9 keeps ordinary66.
	 * Descriptor layout/address-offset bits and live span are unchanged.
	 */
	words[12] = (destination->descriptor0 & 0x01ffffffu) | (0x78u << 25);
	words[15] = source->gpu_base | 1;
	words[16] = source->descriptor0;
	/* The original nonalias branch 4a1cd..4a26b negates BOTH extents. */
	words[21] = ((0u - destination->height) & 0xffffu) << 16 |
		    ((0u - destination->width) & 0xffffu);
	words[24] = (u32)(unlock_point + 0x20) | 1;
	for (i = 0; i < 25; i++) {
		image.primary[4 * i] = words[i];
		image.primary[4 * i + 1] = words[i] >> 8;
		image.primary[4 * i + 2] = words[i] >> 16;
		image.primary[4 * i + 3] = words[i] >> 24;
	}
	image.end_bytes[1] = M9_FAA_PRESENT_COPY_END_BYTES;
	image.end_bytes[0] = M9_FAA_PRESENT_COPY_END_BYTES;
	image.end_bytes[2] = M9_FAA_PRESENT_PRIMARY_BYTES;
	image.final_point = unlock_point + 0x20;
	memcpy(out, &image, sizeof(image));
	return 0;
}
