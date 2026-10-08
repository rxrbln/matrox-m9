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

#endif
