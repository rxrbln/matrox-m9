/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * M9 hardware word encoders shared by the kernel FAA producers and the Mesa
 * m9x driver. The kernel owns this file; Mesa carries a byte-identical copy.
 */
#ifndef M9_HW_ENCODE_H
#define M9_HW_ENCODE_H

#ifdef __KERNEL__
#include <linux/types.h>
#define M9_HW_U32 u32
#else
#include <stdint.h>
#define M9_HW_U32 uint32_t
#endif

/*
 * A packet header holds four register indices, (MMIO - 0x1000) >> 2,
 * consumed low byte first. Index 0x45 ends the packet.
 */
#define M9_HW_IDX_END		0x45u
#define M9_HW_HDR(a, b, c, d) \
	((M9_HW_U32)(a) | (M9_HW_U32)(b) << 8 | (M9_HW_U32)(c) << 16 | \
	 (M9_HW_U32)(d) << 24)
#define M9_HW_HDR1(a)		M9_HW_HDR(a, M9_HW_IDX_END, M9_HW_IDX_END, \
					  M9_HW_IDX_END)
#define M9_HW_HDR2(a, b)	M9_HW_HDR(a, b, M9_HW_IDX_END, M9_HW_IDX_END)
#define M9_HW_HDR3(a, b, c)	M9_HW_HDR(a, b, c, M9_HW_IDX_END)

/*
 * Pass framing through index e6. Each step goes with an e3 tag and an
 * index-43 wait. A range opens with two steps and the 0x42000000 control,
 * which is also the vendor's switch to another context.
 */
#define M9_HW_E6_STEP			0x00000003u
#define M9_HW_E6_RANGE_OPEN		0x42000000u
/* The block prologue and epilogue around a draw (CAPTURED, 343 prologues). */
#define M9_HW_TRANSITION_END		0x38000000u
#define M9_HW_TRANSITION_PUBLISH	0x40004000u
#define M9_HW_TRANSITION_RECORD		0x0c000000u
#define M9_HW_TRANSITION_OPEN		0x60600060u
#define M9_HW_TRANSITION_RESUME		0x604fff9fu
#define M9_HW_TRANSITION_FIRST		0x2c000002u
#define M9_HW_TRANSITION_NEXT		0x2c000000u
/* | the block length in dwords, bits 15:0. */
#define M9_HW_TRANSITION_LENGTH		0x49d00000u
#define M9_HW_TRANSITION_LENGTH_MASK	0xffff0000u
#define M9_HW_TRANSITION_START		0x08000000u

/* Surface format codes, descriptor word 0 bits 31:25. */
#define M9_HW_FORMAT_A8R8G8B8		0x66u
#define M9_HW_FORMAT_A2B10G10R10	0x68u
#define M9_HW_FORMAT_A2R10G10B10	0x6au
/* HZ, and the vendor's FAA primary and PRESENT views. */
#define M9_HW_FORMAT_HZ			0x78u
#define M9_HW_FORMAT_D24S8		0x7du

/*
 * Surface descriptor word 0: row span 15:0, layout 18:16, address offset
 * (tiled placement) 21, resource layout 23, format 31:25.
 */
static inline M9_HW_U32 m9_hw_surface_desc0(M9_HW_U32 format,
					    M9_HW_U32 resource_layout,
					    M9_HW_U32 address_offset,
					    M9_HW_U32 layout,
					    M9_HW_U32 row_span)
{
	return (row_span & 0xffffu) | ((layout & 7u) << 16) |
	       ((address_offset & 1u) << 21) |
	       ((resource_layout & 1u) << 23) | ((format & 0x7fu) << 25);
}

/* Surface descriptor word 1: height 15:0, layers 27:16. */
static inline M9_HW_U32 m9_hw_surface_desc1(M9_HW_U32 height,
					    M9_HW_U32 layers)
{
	return (height & 0xffffu) | ((layers & 0xfffu) << 16);
}

/*
 * Depth comparison codes of a8 bits 4:2 (the vendor's 139670 table), and
 * the conversion from GL/Gallium order NEVER, LESS, EQUAL, LEQUAL,
 * GREATER, NOTEQUAL, GEQUAL, ALWAYS.
 */
#define M9_HW_CMP_NEVER		0u
#define M9_HW_CMP_EQUAL		1u
#define M9_HW_CMP_LESS		2u
#define M9_HW_CMP_LEQUAL	3u
#define M9_HW_CMP_GREATER	4u
#define M9_HW_CMP_GEQUAL	5u
#define M9_HW_CMP_NOTEQUAL	6u
#define M9_HW_CMP_ALWAYS	7u

static inline M9_HW_U32 m9_hw_compare(M9_HW_U32 gl_order)
{
	static const unsigned char code[8] = {
		M9_HW_CMP_NEVER, M9_HW_CMP_LESS, M9_HW_CMP_EQUAL,
		M9_HW_CMP_LEQUAL, M9_HW_CMP_GREATER, M9_HW_CMP_NOTEQUAL,
		M9_HW_CMP_GEQUAL, M9_HW_CMP_ALWAYS,
	};

	return code[gl_order & 7u];
}

/* a8: depth test enable 0, write 1, comparison 4:2 (no stencil). */
static inline M9_HW_U32 m9_hw_depth_a8(M9_HW_U32 enable, M9_HW_U32 write,
				       M9_HW_U32 compare)
{
	return 0x00380380u | (enable & 1u) | ((write & 1u) << 1) |
	       ((compare & 7u) << 2);
}

/* The depth enable bits of ef; a8 alone enables nothing. */
static inline M9_HW_U32 m9_hw_depth_ef(M9_HW_U32 enable, M9_HW_U32 write)
{
	return enable ? (write ? 0x0000f000u : 0x00007000u) : 0;
}

/* ab: one nibble per pixel column x % 4, bits B 0, G 1, R 2, A 3. */
static inline M9_HW_U32 m9_hw_color_mask(M9_HW_U32 r, M9_HW_U32 g,
					 M9_HW_U32 b, M9_HW_U32 a)
{
	M9_HW_U32 nibble = (r ? 4u : 0) | (g ? 2u : 0) | (b ? 1u : 0) |
			   (a ? 8u : 0);

	return 0xffff0000u | nibble * 0x1111u;
}

/*
 * Texture unit words: 70 sampling form, 79 format/kind/width, 7a levels,
 * layers and height, 7c filter and wrap, 7d LOD bias and range.
 */
#define M9_HW_WRAP_REPEAT		0u
#define M9_HW_WRAP_CLAMP_TO_EDGE	1u
#define M9_HW_WRAP_MIRROR_REPEAT	4u
#define M9_HW_WRAP_CLAMP		6u
#define M9_HW_WRAP_CLAMP_TO_BORDER	7u

#define M9_HW_7C_MIN_LINEAR		0x002u
#define M9_HW_7C_MAG_LINEAR		0x010u
#define M9_HW_7C_MIP_NEAREST		0x080u
#define M9_HW_7C_MIP_LINEAR		0x100u
/* Captured only for NEAREST_MIPMAP_NEAREST with LINEAR magnification. */
#define M9_HW_7C_MIP_NEAREST_MAG_LINEAR	0x400u

static inline M9_HW_U32 m9_hw_texture_70(M9_HW_U32 form, M9_HW_U32 lod_range,
					 M9_HW_U32 compare_enable,
					 M9_HW_U32 compare)
{
	return 0x00040018u | ((form & 3u) << 8) | ((lod_range & 1u) << 19) |
	       (compare_enable ? 1u << 26 | (compare & 7u) << 27 : 0);
}

static inline M9_HW_U32 m9_hw_texture_79(M9_HW_U32 code, M9_HW_U32 kind,
					 M9_HW_U32 width)
{
	return (code & 0xffu) << 24 | (kind & 0xffu) << 16 | (width & 0xffffu);
}

static inline M9_HW_U32 m9_hw_texture_7a(M9_HW_U32 last_level,
					 M9_HW_U32 layers, M9_HW_U32 height)
{
	return (last_level & 0xfu) << 28 | (layers & 0xfffu) << 16 |
	       (height & 0xffffu);
}

static inline M9_HW_U32 m9_hw_sampler_7c(M9_HW_U32 filter, M9_HW_U32 wrap_s,
					 M9_HW_U32 wrap_t, M9_HW_U32 wrap_r)
{
	return 0x00009200u | filter | (wrap_s & 7u) << 22 |
	       (wrap_t & 7u) << 25 | (wrap_r & 7u) << 28;
}

/* LOD bias in 1/32 (10-bit two's complement), range lo..hi in 1/32. */
static inline M9_HW_U32 m9_hw_sampler_7d(M9_HW_U32 bias, M9_HW_U32 lo,
					 M9_HW_U32 hi)
{
	return (bias & 0x3ffu) | (lo & 0x1ffu) << 10 | (hi & 0x1ffu) << 19;
}

#endif
