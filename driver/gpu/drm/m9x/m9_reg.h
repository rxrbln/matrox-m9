/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef __PARHELIA_M9_REG_H__
#define __PARHELIA_M9_REG_H__

#define M9_REG_FRONTEND_KICK		0x0314
#define M9_REG_FRONTEND_STATE		0x031c
#define M9_REG_COMMAND_BASE		0x0400
#define M9_REG_COMMAND_END		0x0404
#define M9_REG_REQUESTER_STATE		0x0448
#define M9_REG_REQUESTER_CTL0		0x044c
#define M9_REG_REQUESTER_CTL1		0x0450
#define M9_REG_REQUESTER_CTL2		0x0454
/* Vendor fallback for the packed {8,32,24,32}/{4,8,8} partition tuples.
 * Preserve warm firmware values until the target property tuple is known. */
#define M9_REQUESTER_CTL0_FALLBACK	0x00280800
#define M9_REQUESTER_CTL1_FALLBACK	0x1f3f2707
#define M9_REQUESTER_CTL2_FALLBACK	0x00070703
#define M9_REG_AUX_BASE			0x0500
#define M9_REG_AUX_PRODUCER		0x0504
#define M9_REG_AUX_STATUS		0x0508
#define M9_REG_AUX_PROGRESS		0x050c
#define M9_REG_AUX_ADDRESS		0x0510
#define M9_REG_AUX_COMPANION		0x0514
#define M9_REG_AUX_OPAQUE0		0x0520
#define M9_REG_AUX_OPAQUE1		0x0524
#define M9_REG_ENGINE_STATUS		0x08f0
#define M9_REG_IRQ0_ACK			0x08d8
#define M9_REG_IRQ0_MASK		0x08dc
#define M9_REG_IRQ_LOW_RAW		0x08e0
#define M9_REG_IRQ_LOW_ACK		0x08e4
#define M9_REG_IRQ_LOW_MASK		0x08e8
#define M9_REG_IRQ_HIGH_RAW		0x08f4
#define M9_REG_IRQ_HIGH_ACK		0x08f8
#define M9_REG_IRQ_HIGH_MASK		0x08fc
#define M9_REG_FENCE0_ADDRESS		0x1068
#define M9_REG_FENCE1_ADDRESS		0x106c
#define M9_REG_CONTEXT_BASE		0x109c

#define M9_DC0				0x1800
#define M9_DC_CTRL			0x0000
#define M9_DC_XSTART			0x0008
#define M9_DC_XEND			0x000c
#define M9_DC_YSTART			0x0014
#define M9_DC_YEND			0x0018
#define M9_DC_LUT_INDEX			0x0038
#define M9_DC_LUT_RED			0x003c
#define M9_DC_LUT_GREEN			0x0040
#define M9_DC_LUT_BLUE			0x0044
#define M9_DC_START			0x0048
#define M9_DC_PITCH			0x0058
#define M9_DC_ENABLE			BIT(0)
#define M9_DC_LUT_BYPASS		BIT(14)
#define M9_DC_LUT_LATCH			BIT(31)
#define M9_DC_START_MASK		0xffffff80u
#define M9_DC_PITCH_MASK		0x00003fffu
#define M9_DC_FORMAT_SHIFT		22
#define M9_DC_FORMAT_MASK		(0xffu << M9_DC_FORMAT_SHIFT)
#define M9_DC_FORMAT_XRGB8888		0xccu

#define M9_DCT0				0x1900
#define M9_DCT_HBLANK			0x0008
#define M9_DCT_HTOTAL			0x0010
#define M9_DCT_HSYNC			0x0014
#define M9_DCT_VBLANK			0x0020
#define M9_DCT_VTOTAL			0x002c
#define M9_DCT_VSYNC			0x0030
#define M9_DCT_VSYNCEND			0x005c

#define M9_REG_VGAFLAG			0x1c10
#define M9_VGAFLAG_VGA			BIT(0)
#define M9_VGAFLAG_NATIVE		BIT(1)
#define M9_REG_VGAMISC			0x1fdc
#define M9_VGAMISC_NATIVE		BIT(16)
#define M9_REG_DVO0			0x1c9c

#define M9_IRQ_LOW_VBLANK0		BIT(0)
#define M9_IRQ_HIGH_LOW_CASCADE	BIT(31)

#define M9_COMMAND_SYSTEM_TAG		2u
#define M9_AUX_DMA32_CLASS		2u
#define M9_AUX_DMA32_TAG		0u
#define M9_AUX_QUIESCENT		BIT(0)
#define M9_AUX_PARK			BIT(1)

#define M9_PACKET_STOP			0x45u
#define M9_PACKET_FENCE_HEADER		0x4545e3e6u

#endif
