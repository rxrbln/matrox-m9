/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef __PARHELIA_M9_REG_H__
#define __PARHELIA_M9_REG_H__

#define M9_REG_FRONTEND_KICK		0x0314
#define M9_REG_FRONTEND_STATE		0x031c
#define M9_REG_SERVICE_CONTROL		0x030c
#define M9_REG_SERVICE_BASE		0x0330
#define M9_REG_SERVICE_LANE0		0x0334
#define M9_REG_SERVICE_LANE1		0x0338
#define M9_REG_SERVICE_LANE2		0x033c
#define M9_REG_SERVICE_LANE3		0x0340
#define M9_REG_SERVICE_CLEAR0		0x0344
#define M9_REG_SERVICE_CLEAR1		0x0348
#define M9_REG_SERVICE_CLEAR2		0x034c
#define M9_REG_SERVICE_CLEAR3		0x0350
#define M9_REG_SERVICE_MAP		0x0354
#define M9_REG_TRANSLATION_BASE		0x0360
#define M9_REG_TRANSLATION_LIMIT	0x0364
#define M9_REG_SERVICE_MODE		0x03a8
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
#define M9_REG_CONTEXT_STATE		0x1088
#define M9_REG_RESTORE_BASE		0x1094
#define M9_REG_RESTORE_LIMIT		0x1098
#define M9_REG_CONTEXT_BASE		0x109c
#define M9_REG_RESTORE_TAGGED		0x10ec
#define M9_REG_APERTURE_PROPERTY	0x0f04
#define M9_REG_APERTURE_CONTROL		0x0f0c
#define M9_REG_MEMORY_STATUS		0x0f28
#define M9_REG_PTE_BASE			0x0f64
#define M9_REG_PTE_LIMIT		0x0f68
#define M9_REG_LOCAL_BASE0		0x0f74
#define M9_REG_LOCAL_BASE1		0x0f78
#define M9_REG_LOCAL_LIMIT		0x0f7c
#define M9_REG_SURFACE_BASE0		0x1320
#define M9_REG_SURFACE_BASE1		0x1324
#define M9_REG_SURFACE_LIMIT0		0x1328
#define M9_REG_SURFACE_BASE2		0x132c
#define M9_REG_SURFACE_BASE3		0x1330
#define M9_REG_SURFACE_LIMIT1		0x1334

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

/* Hardware cursor, route 0 (parhelia-m9-cursor-gamma-re.md section 1). */
#define M9_CURSOR0			0x18c0
#define M9_CURSOR_PALETTE		0x0000
#define M9_CURSOR_CONTROL		0x0004
#define M9_CURSOR_POSITION		0x0008
#define M9_CURSOR_ADDRESS		0x000c
#define M9_CURSOR_CONFIG		0x0010
#define M9_CURSOR_OFF			0x00u
#define M9_CURSOR_COLOR			0x05u
#define M9_CURSOR_CONFIG_COLOR		0xcc000000u
/* The colour image is 64 rows of 64 pixels stored with a 256-byte row. */
#define M9_CURSOR_SIZE			64u
#define M9_CURSOR_ROW_BYTES		256u
#define M9_CURSOR_BYTES			(M9_CURSOR_ROW_BYTES * M9_CURSOR_SIZE)
/* The vendor upload clears bit 24 of every source dword. */
#define M9_CURSOR_DWORD_MASK		0xfeffffffu
/* The origin is shifted by the cursor size so it can leave the top left. */
#define M9_CURSOR_POS_MASK		0x7fffu

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

#define M9_REQUESTER_DMA_TAG		2u
#define M9_COMMAND_SYSTEM_TAG		M9_REQUESTER_DMA_TAG
#define M9_AUX_DMA32_CLASS		2u
#define M9_AUX_DMA32_TAG		0u
#define M9_AUX_QUIESCENT		BIT(0)
#define M9_AUX_PARK			BIT(1)

#define M9_PACKET_STOP			0x45u
#define M9_PACKET_FENCE_HEADER		0x4545e3e6u


/* Board-controller (HMC) ownership of the shared GPIO registers: the request
 * bit is active low and the grant appears in a separate register. */
#define M9_HMC_REQUEST		0x0318
#define M9_HMC_GRANT		0x03b0
#define M9_HMC_DDC_REQUEST	BIT(29)
#define M9_HMC_DDC_GRANT	BIT(1)
#define M9_HMC_MISCIO_REQUEST	BIT(28)
#define M9_HMC_MISCIO_GRANT	BIT(0)
#define M9_HMC_GRANT_TRIES	2000u

/* Bit-banged DDC: data bit n, output enable at n + 16. */
#define M9_DDC			0x1c48
#define M9_MISCIO		0x1c4c
#define M9_DDC_OE_SHIFT		16


/* Native modeset on display controller 0.  Field encodings are the M9's own:
 * every timing value is (n - 1), horizontal 15 bit and vertical 14 bit
 * (xf86-video-parhelia src/parhelia_reg.h, recovered from the vendor driver). */
#define M9_DC_MISC			0x0004
#define M9_DC_HOLD			0x0088
#define M9_DC_HOLD_SET			0x00000012u
#define M9_DC_HOLD_CLR			0x00000032u
#define M9_DC_HOLD2			0x0090
#define M9_DC_CTRL_INTERLACE		BIT(12)
#define M9_DC_PITCH_FORMAT_SHIFT	24
#define M9_DCT_CTRL			0x0000
#define M9_DCT_RESET			0x00030000u
#define M9_DCT_H_MASK			0x7fffu
#define M9_DCT_V_MASK			0x3fffu

/* Frequency synthesisers.  0x0e30/34/38 are ~500 MHz system clocks in both
 * the firmware's and the vendor driver's state; the pixel clock of controller
 * 0 is 0x0e48 (MEASURED: the vendor's 1920x1200 capture holds 0x0004d6f6
 * there, which decodes to exactly 154000 kHz, and the firmware's 1024x768
 * holds 0x00063a1d, 65089 kHz).  Field widths are b0 6, b1 5, b2 5, b3 4
 * bits, each stored as value - 1. */
#define M9_FS_OSC			0x0e24
#define M9_FS_PIXEL0			0x0e48
#define M9_FS_B0_MAX			64u
#define M9_FS_B1_MAX			32u
#define M9_FS_B2_MAX			32u
#define M9_FS_B3_MAX			16u
#define M9_FS_REF_KHZ			27000u
#define M9_FS_LOAD			BIT(31)
#define M9_FS_INIT			BIT(21)
#define M9_FS_LOCK			BIT(24)
#define M9_VCO_MIN_KHZ			800000u
#define M9_VCO_MAX_KHZ			1600000u
#define M9_FS_OUT_MIN_KHZ		54000u
#define M9_FS_OUT_MAX_KHZ		108000u
#define M9_CLKSEL0			0x1c00
#define M9_CLK_HOLD			BIT(0)
#define M9_CLK_SEL_MASK			0x0000001eu
#define M9_CLK_SEL_SHIFT		1
#define M9_CLK_SEL_FS2			4u
#define M9_REG_SYNCARM			0x1c24
/* Digital link 0: the vendor sets bit 22 for a negative vsync and leaves the
 * rest of the word alone across a mode change. */
#define M9_DVO_HSYNC_NEG		BIT(20)
#define M9_DVO_VSYNC_NEG		BIT(22)
#define M9_VIDRST_STROBE		BIT(31)

#endif
