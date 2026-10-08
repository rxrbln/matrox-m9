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

#endif
