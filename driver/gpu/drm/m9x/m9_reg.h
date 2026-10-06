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
#define M9_REG_FENCE2_ADDRESS		0x1070
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
#define M9_DCT_HDISPLAY2		0x000c
#define M9_DCT_HBLANK			0x0008
#define M9_DCT_HTOTAL			0x0010
#define M9_DCT_HSYNC			0x0014
#define M9_DCT_VBLANK			0x0020
/* Free-running line counter, confirmed to advance on all four heads. */
#define M9_DCT_LINE			0x0024
#define M9_DCT_VTOTAL			0x002c
#define M9_DCT_VSYNC			0x0030
#define M9_DCT_VSYNCEND			0x005c

/*
 * Output path select, and the global video-reset strobe.  Bit 0 hands the
 * serialisers the legacy VGA CRTC instead of the display controllers: while it
 * is set the head sends the VGA raster whatever the controller is programmed
 * with, and a card the BIOS never POSTed into a VBE mode comes up that way.
 * MEASURED on a boot without vga=: head 0 programmed 1280x720 while the
 * transmitter reported 640x480 htotal 800 vtotal 525, and putting bit 0 back
 * after the handoff stopped the transmitter sending anything at all.
 * Bit 1 is the native-path flag a POSTed card is found with.
 */
#define M9_REG_VGAFLAG			0x1c10
#define M9_VGAFLAG_VGA			BIT(0)
#define M9_VGAFLAG_NATIVE		BIT(1)
#define M9_REG_VGAMISC			0x1fdc
#define M9_VGAMISC_NATIVE		BIT(16)
/*
 * The legacy VGA miscellaneous output register, byte wide, written at 0x1fc2
 * and read back at 0x1fcc.  Bits 3:2 are the CRTC clock select and 2 is the
 * one that takes it off the fixed 25.175 MHz VGA oscillator and onto the
 * frequency synthesiser the clock select names.  MEASURED: with the firmware's
 * 0x63 left in place, a head programmed 1280x720 at 74250 kHz rastered at
 * 15.0 kHz / 20 Hz - 25.175 MHz over that raster - whatever was written to the
 * synthesiser, and 0x68 took it to the programmed 45.0 kHz / 60.00 Hz.
 */
#define M9_REG_VGA_MISC_WRITE		0x1fc2
#define M9_REG_VGA_MISC_READ		0x1fcc
#define M9_VGA_MISC_CLOCK_MASK		0x0cu
#define M9_VGA_MISC_CLOCK_SYNTH		0x08u
#define M9_VGA_MISC_NATIVE		0x68u
#define M9_REG_DVO0			0x1c9c
/*
 * Per-pad output gate.  MEASURED: the high nibble gates, active high for off,
 * one bit per pad.  POST leaves 0xe1 - pad 0 ungated and pads 1..3 held off -
 * which is why a freshly posted board lights exactly one connector however
 * healthy the other three controllers are, and clearing a head's bit is what
 * makes its transmitter start receiving video.  The gate only takes effect
 * while that pad's transmitter is initialised with its output enabled:
 * written to a dark, uninitialised pad it does nothing, which is what made
 * the register look inert.  Clearing a bit starts the pad; setting it again
 * does not stop one that is already running.
 *
 * The low nibble is read-only and changes as pads come up, but its meaning is
 * NOT established: it has been observed set for a connector with no sink and
 * clear for one that was demonstrably transmitting, so it is neither a
 * hot-plug input nor a reliable "this pad is driving" flag.  Nothing gates on
 * it.
 */
#define M9_REG_PAD_GATE			0x1cac
#define M9_PAD_GATE_OFF(pad)		BIT(4 + (pad))
#define M9_PAD_GATE_UP(pad)		BIT(pad)

/*
 * Every head raises three interrupts per frame.  These are the lowest of each
 * head's three, the group the documented head 0 source belongs to.  MEASURED
 * with tools/m9-irq-scan: the status word latches a source whether or not it
 * is unmasked, so acknowledging what is set and timing its return gives each
 * source's frequency, and running every head at a different refresh rate
 * attributes it.  The other two groups are (2,3,12,13) for heads 0,1 and
 * (10,11,14,15) for heads 2,3; which of a head's three is the vertical blank
 * edge proper is not established, and this is the group that reproduces the
 * documented head 0 bit.
 */
#define M9_IRQ_LOW_VBLANK0		BIT(0)
#define M9_IRQ_LOW_VBLANK1		BIT(1)
#define M9_IRQ_LOW_VBLANK2		BIT(8)
#define M9_IRQ_LOW_VBLANK3		BIT(9)
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

/* Bit-banged DDC: data bit n, output enable at n + 16.  Four buses, the even
 * bit of each pair the clock.  0x1c4c is MISCIO - transmitter resets and the
 * board identification EEPROM - and carries no monitor EDID. */
#define M9_DDC			0x1c48
#define M9_MISCIO		0x1c4c
#define M9_DDC_OE_SHIFT		16
#define M9_MISCIO_OE_SHIFT	10
/* MISCIO function ID 0x20 + connector is that connector's transmitter reset
 * line; [WIN] 0x390e0 drives it low for 10 ms and releases it. */
#define M9_MISCIO_TX_RESET(conn)	(0x20 + (conn))
#define M9_MISCIO_RESET_US		10000u

/* External DisplayPort transmitter, one per connector on its own DDC pins.
 * Its I2C bridge has to be off while the host reads the sink's EDID. */
#define M9_DP_TX_MAIN		0x00
#define M9_DP_TX_BRIDGE		BIT(1)
/* Hotplug lives on the transmitter's second seven-bit address. */
#define M9_DP_TX_HPD		0x7c
#define M9_DP_TX_BRIDGE_SETTLE_MS	20u
/*
 * Output gate.  MEASURED on the connector the firmware trained: 0x00 lets the
 * stream out and both 0x10 and 0x30 blank it, which is the opposite of what
 * the saved binaries' "0x30 when enabled" reads like.  The vendor's captured
 * sequence passes through 0x30 and closes on 0x00; the closing write is the
 * one that matters.
 *
 * MEASURED, and the reason nothing may leave this register blanked: 0x30 also
 * kills the AUX engine and clears the lane-status mirror at 0x63..0x65, and
 * the link does not come back when the gate reopens - it has to be trained
 * again.  0x10 does not; it blanks the stream while AUX keeps answering.  So
 * a probe that blanks before reading the capability mirror reads zeros and
 * concludes the sink has no lanes.
 */
#define M9_DP_TX_GATE		0x0a
#define M9_DP_TX_GATE_ON	0x00u
#define M9_DP_TX_GATE_OFF	0x10u
#define M9_DP_TX_GATE_ARM	0x30u
#define M9_DP_TX_POWER		0x74
#define M9_DP_TX_FAULT		0x75
#define M9_DP_TX_STATUS		0x76
#define M9_DP_TX_EVENT		0x77
#define M9_DP_TX_FAULT_INFO	0x79
#define M9_DP_TX_PAGE3_MARK	0x26
/*
 * Main-stream attributes, 0x10..0x1f, eight little-endian 16-bit fields:
 * htotal, hstart, hactive, vtotal, vstart, vactive, hsync, vsync, the two sync
 * widths carrying the polarity in bit 15.  Nothing in the whole vendor X run
 * writes them - CAPTURED, by decoding every bit-banged transaction on 0x1c48 -
 * so they report the video this transmitter is actually sending, and are the
 * one reading available downstream of the CRTC without a person looking at a
 * panel.  They read zero while the pad is dark.
 */
#define M9_DP_TX_MSA		0x10
#define M9_DP_TX_MSA_BYTES	16u
#define M9_DP_TX_MSA_SYNC_NEG	BIT(15)
/* Hotplug byte bit 2 sets while the transmitter is not driving. */
#define M9_DP_TX_HPD_IDLE	BIT(2)


/* Native modeset on display controller 0.  Field encodings are the M9's own:
 * every timing value is (n - 1), horizontal 15 bit and vertical 14 bit
 * (xf86-video-parhelia src/parhelia_reg.h, recovered from the vendor driver). */
#define M9_DC_MISC			0x0004
#define M9_DC_HOLD			0x0088
#define M9_DC_HOLD_SET			0x00000012u
#define M9_DC_HOLD_CLR			0x00000032u
#define M9_DC_HOLD2			0x0090
/* Controllers 2 and 3 hold only half a page: no hold pair of their own, and
 * two extra words the vendor writes for them alone.  Copied verbatim from the
 * four-head run, meaning not established. */
#define M9_DC_XEND2			0x0010
#define M9_DC_OPAQUE			0x0064
#define M9_DC_OPAQUE_VALUE		0x80010000u
#define M9_DC_HOLD_COMPANION		0x00000300u
/* One watermark word served all four heads and all four modes. */
#define M9_WATERMARK_VALUE		0x000203f7u
#define M9_DC_CTRL_INTERLACE		BIT(12)
#define M9_DC_PITCH_FORMAT_SHIFT	24
#define M9_DCT_CTRL			0x0000
#define M9_DCT_RESET			0x00030000u
#define M9_DCT_H_MASK			0x7fffu
#define M9_DCT_V_MASK			0x3fffu

/*
 * Frequency synthesisers.  A controller owns no pixel-clock register: its
 * clock-select word picks one synthesiser, register = 0x0e38 + 4 * select
 * (MEASURED: one four-head vendor run decodes selects 4/1/2/3 at 0x0e48/3c/
 * 40/44 to exactly the four heads' mode clocks).  Selects 0, 5 and 6 address
 * 0x0e38 and 0x0e4c/0x0e50, which carry the system and board clocks and must
 * never take a pixel word.  Field widths are b0 6, b1 5, b2 5, b3 4 bits,
 * each stored as value - 1.
 */
#define M9_FS_OSC			0x0e24
#define M9_FS_BASE			0x0e38
#define M9_FS_REG(sel)			(M9_FS_BASE + 4 * (sel))
#define M9_FS_PIXEL_FIRST		1u
#define M9_FS_PIXEL_LAST		4u
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
/* The four clock selects are consecutive; which head owns which is in the
 * bank table, but the VGA handoff parks all of them. */
#define M9_CLKSEL_COUNT			4u
#define M9_CLKSEL(i)			(M9_CLKSEL0 + 4 * (i))
#define M9_CLK_HOLD			BIT(0)
#define M9_CLK_SEL_MASK			0x0000001eu
#define M9_CLK_SEL_SHIFT		1
/* Set on clock select 0 alone across the VGA handoff; meaning not established,
 * copied from the vendor sequence. */
#define M9_CLK_HANDOFF			BIT(25)
#define M9_REG_SYNCARM			0x1c24
#define M9_SYNCARM_RUN			BIT(0)
#define M9_SYNCARM_HANDOFF		BIT(8)
/* Digital link 0: the vendor sets bit 22 for a negative vsync and leaves the
 * rest of the word alone across a mode change. */
#define M9_DVO_HSYNC_NEG		BIT(20)
#define M9_DVO_VSYNC_NEG		BIT(22)
#define M9_DVO_SRC_MASK			0x1fu
#define M9_DVO_PATH_OFF			BIT(18)
/* Clock-keyed link format registers the board patches alongside the link.
 * 0x1c38 carries links 0 and 1, 0x1c3c links 2 and 3, one 16-bit half each. */
#define M9_REG_LINK_FORMAT0		0x1c38
#define M9_REG_LINK_FORMAT1		0x1c3c
#define M9_LINK_FORMAT_REG(link)	(M9_REG_LINK_FORMAT0 + 4 * ((link) / 2))
#define M9_LINK_FORMAT_MASK(link)	((link) & 1 ? 0xffff0000u : 0x0000ffffu)
#define M9_VIDRST_STROBE		BIT(31)

#endif
