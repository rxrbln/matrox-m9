/* SPDX-License-Identifier: GPL-2.0-only */
/* The original 102b:0540 stage-0 descriptor/coordinate producer. The GPU
 * address is taken solely from a live retained VRAM owner. This bounded
 * source control does not lower an arbitrary sampler or fragment shader. */
#ifdef M9_FAA_TEXTURE_HOST_TEST
#include <errno.h>
#include <stdint.h>
#define U32_MAX UINT32_MAX
#else
#include <linux/errno.h>
#include <linux/limits.h>
#endif
#include "m9_faa_graph_texture_bind.h"

int m9x_faa_texture_bind_encode(
	const struct m9x_faa_texture_bind_input *in,
	struct m9x_faa_texture_bind_words *out)
{
	struct m9x_faa_texture_bind_words image;
	u32 kind, extent, span, alloc_height, tail;

	if (!in || !out || in->owner_domain != M9_FAA_TEXTURE_VRAM ||
	    (in->owner_base & 255) ||
	    in->owner_base > U32_MAX - in->owner_bytes ||
	    in->profile != 12 ||
	    in->format != 102 || !in->width || !in->height ||
	    in->width > 1024 || in->height > 1024 ||
	    (in->width & (in->width - 1)) ||
	    (in->height & (in->height - 1)) ||
	    in->levels != 1 || in->stage != 0 ||
	    (in->min_filter != 1 && in->min_filter != 2) ||
	    (in->mag_filter != 1 && in->mag_filter != 2) ||
	    in->mip_filter != 1 ||
	    (in->address_u != 1 && in->address_u != 3) ||
	    (in->address_v != 1 && in->address_v != 3) ||
	    (in->coordinate_sets != 1 && in->coordinate_sets != 2) ||
	    in->coordinate_index >= in->coordinate_sets)
		return -EINVAL;

	kind = in->width > 64 || in->height > 64 ? 0x21 : 0x01;
	/* The texture view has the same allocation footprint as the vendor's
	 * render-surface allocator.  A width-filling level needs eight spare
	 * texels after its last row, then the storage kind's byte alignment.
	 * The 64x64x4 source level therefore occupies 0x4400, not 0x4000.
	 * This is an allocation bound; the descriptor still uses the logical
	 * width and height below. */
	span = (in->width + 31) & ~31u;
	alloc_height = (in->height + (kind == 1 ? 7 : 31)) &
			 ~(kind == 1 ? 7u : 31u);
	if (in->width <= 16 && in->height <= 16)
		alloc_height = 16;
	tail = (alloc_height - in->height) * span + span - in->width;
	extent = span * alloc_height + (tail < 8 ? 8 - tail : 0);
	extent = (extent * 4 + (kind == 1 ? 0x3ff : 0xfff)) &
		 ~(kind == 1 ? 0x3ffu : 0xfffu);
	if (in->storage_kind != kind || in->owner_bytes < extent)
		return -EINVAL;

	if (in->coordinate_sets == 1) {
		if (in->raster_ae_base != 0x28206ff6)
			return -EINVAL;
	} else if (in->raster_ae_base != 0x28207006 &&
		   in->raster_ae_base != 0x28207776) {
		return -EINVAL;
	}

	/* 467e0 selector 2: idx78 low 16 is the requested horizontal extent,
	 * idx79 low 16 is height and bits 27:16 are the one-layer extent.
	 * Kind 01 becomes 21 above 64 in either dimension. Format code 0x66
	 * occupies idx78[31:25]. Kind-11 relocation adds
	 * texture tag 1 to a 256-byte-aligned live VRAM base. */
	image.unit70 = 0x40040118;
	image.address78 = in->owner_base | 1;
	image.descriptor79 = (0x66u << 25) | (kind << 16) | in->width;
	image.descriptor7a = (1u << 16) | in->height;
	/* 18afb0/18ba27 tables, checked against independent MIN/MAG and
	 * U/V clamp controls. Mip-linear needs a resident chain, so it stays
	 * outside this single-level owner contract. */
	image.sampler7c = 0x00009280 |
		((in->min_filter == 2) ? 2 : 0) |
		((in->mag_filter == 2) ? (2 << 3) : 0) |
		((in->address_u == 3) ? (1 << 22) : 0) |
		((in->address_v == 3) ? (1 << 25) : 0);
	image.sampler7d = 0x0ff80000;
	image.sampler87 = 0;
	/* 105648/17f91d coordinate selection leaves the program intact.
	 * f70fa/f7102 select a stream record; fbee8 selects its raster half. */
	image.stream0 = 0x05300d88 | (in->coordinate_index << 25);
	image.stream1 = 0x05300b36 | (in->coordinate_index << 25);
	image.raster_ae = in->raster_ae_base +
		(in->coordinate_index << 6);
	*out = image;
	return 0;
}
