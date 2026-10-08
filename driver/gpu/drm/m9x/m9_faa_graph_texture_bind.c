/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
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

/* One 2-D level's surface-contract footprint (row span, allocation rows,
 * small aperture class), as the vendor render-surface allocator sizes it.
 */
static void level_layout(u32 width, u32 height, u32 *span, u32 *rows,
			 int *small)
{
	*small = width <= 64 && height <= 64;
	*span = (width + 31) & ~31u;
	*rows = (height + (*small ? 7 : 31)) & ~(*small ? 7u : 31u);
	if (width <= 16 && height <= 16)
		*rows = 16;
}

/* Packed mip chain extent of the ordinary texture layout (Mesa
 * m9_texture_layout): each level by its own extent, small levels by the
 * aperture calculator's 1 KiB pages. Levels of at most 16x16 share one
 * 32x16 tail atlas (0x800 bytes) placed where the first of them starts.
 */
static u32 chain_extent(u32 width, u32 height, u32 levels)
{
	u32 offset = 0, end = 0, l;
	int tail = 0;

	for (l = 0; l < levels; l++) {
		u32 w = width >> l ? width >> l : 1;
		u32 h = height >> l ? height >> l : 1;
		u32 span, rows, quantum, face, whole;
		int small;

		if (w <= 16 && h <= 16) {
			if (!tail)
				offset += 0x800;
			tail = 1;
			continue;
		}
		level_layout(w, h, &span, &rows, &small);
		quantum = small ? 0x400 : 0x1000;
		whole = (span * rows * 4 + quantum - 1) & ~(quantum - 1);
		face = small ? ((h + (span & 63 ? 8 : 4) - 1) /
				(span & 63 ? 8 : 4)) * 0x400 : whole;
		if (offset + whole > end)
			end = offset + whole;
		offset += face;
	}
	return offset > end ? offset : end;
}

/* D3DTADDRESS WRAP 1, CLAMP 3 and the vendor table's entry 6 (GL CLAMP)
 * to the 7c wrap field (MxMD 0x1f639c: 0,0,4,1,7,5,6).
 */
static int wrap_mode(u32 address, u32 *mode)
{
	switch (address) {
	case 1:
		*mode = 0;
		return 0;
	case 3:
		*mode = 1;
		return 0;
	case 6:
		*mode = 6;
		return 0;
	default:
		return -EINVAL;
	}
}

int m9x_faa_texture_bind_encode(
	const struct m9x_faa_texture_bind_input *in,
	struct m9x_faa_texture_bind_words *out)
{
	struct m9x_faa_texture_bind_words image;
	u32 kind, extent, span, alloc_height, tail, wrap_u, wrap_v;

	if (!in || !out || in->owner_domain != M9_FAA_TEXTURE_VRAM ||
	    (in->owner_base & 255) ||
	    in->owner_base > U32_MAX - in->owner_bytes ||
	    in->profile != 12 ||
	    in->format != 102 || !in->width || !in->height ||
	    in->width > 1024 || in->height > 1024 ||
	    (in->width & (in->width - 1)) ||
	    (in->height & (in->height - 1)) ||
	    !in->levels || in->levels > 11 ||
	    (in->width >> (in->levels - 1) == 0 &&
	     in->height >> (in->levels - 1) == 0) || in->stage != 0 ||
	    (in->min_filter != 1 && in->min_filter != 2) ||
	    (in->mag_filter != 1 && in->mag_filter != 2) ||
	    in->mip_filter < 1 || in->mip_filter > 3 ||
	    wrap_mode(in->address_u, &wrap_u) ||
	    wrap_mode(in->address_v, &wrap_v) ||
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
	if (in->levels > 1)
		extent = chain_extent(in->width, in->height, in->levels);
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
	/* Mipmapped sampling: last level in 7a[31:28] and 7c nearest 0x80 /
	 * linear 0x100, the ordinary sampler's MEASURED encoding.
	 */
	if (in->mip_filter != 1)
		image.descriptor7a |= (in->levels - 1) << 28;
	/* 18afb0/18ba27 tables, checked against independent MIN/MAG and
	 * U/V wrap controls.
	 */
	image.sampler7c = 0x00009200 |
		(in->mip_filter == 3 ? 0x100 : 0x80) |
		(in->mip_filter == 2 && in->mag_filter == 2 &&
		 in->min_filter == 1 ? 0x400 : 0) |
		((in->min_filter == 2) ? 2 : 0) |
		((in->mag_filter == 2) ? (2 << 3) : 0) |
		wrap_u << 22 | wrap_v << 25;
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
