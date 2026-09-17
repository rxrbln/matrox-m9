/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * DisplayPort mode and link training for the external transmitter.  The
 * transmitter carries the training state machine: this code selects a link
 * rate and a lane count, sets one bit and waits for the result.  Register map
 * and sequence from xf86-video-parhelia
 * docs/parhelia-m9148-displayport-transmitter-re.md, recovered from the vendor
 * X driver's LLDP module and cross-checked against the VBIOS.
 *
 * m9_reg.h already names the registers the common bring-up uses; only the
 * link, capability and AUX block is added here.
 */
#ifndef M9_DP_H
#define M9_DP_H

#include <linux/types.h>

struct m9x_device;

/* Receiver capability, a mirror the transmitter fetches over AUX. */
#define M9_DP_CAP_MAX_RATE	0x51	/* DPCD 0x00001 */
#define M9_DP_CAP_MAX_LANES	0x52	/* DPCD 0x00002, b4:0 lanes, b7 EF */
#define M9_DP_CAP_DOWNSPREAD	0x53	/* DPCD 0x00003 */
#define M9_DP_CAP_DFP		0x55	/* DPCD 0x00005 */

/* Link configuration. */
#define M9_DP_DONGLE		0x5c
#define M9_DP_LINK_BW		0x5d	/* LINK_BW_SET */
#define M9_DP_LANE_COUNT	0x5e	/* LANE_COUNT_SET, b7 enhanced framing */
#define M9_DP_TRAIN		0x5f	/* b0 0 -> 1 starts hardware training */
#define M9_DP_TRAIN_START	BIT(0)
#define M9_DP_PATTERN		0x60

/* Lane status, a mirror of DPCD 0x00202..0x00204. */
#define M9_DP_LANE01_STATUS	0x63
#define M9_DP_LANE23_STATUS	0x64
#define M9_DP_ALIGN_STATUS	0x65

/* Interrupt status, write 1 to clear.  Reported only while the matching bit is
 * enabled in M9_DP_TX_EVENT. */
#define M9_DP_IRQ_BRIDGE	BIT(0)
#define M9_DP_IRQ_TRAINED	BIT(1)
#define M9_DP_IRQ_HPD_PULSE	BIT(2)
#define M9_DP_IRQ_AUX_ERROR	BIT(3)
#define M9_DP_IRQ_HPD_OFF	BIT(4)
#define M9_DP_IRQ_AUX_DONE	BIT(5)
#define M9_DP_IRQ_HPD_ON	BIT(6)
#define M9_DP_EVENT_ENABLE	0xb2u	/* what the firmware leaves behind */

/* Native AUX engine. */
#define M9_DP_AUX_ADDR0		0x7d
#define M9_DP_AUX_ADDR1		0x7e
#define M9_DP_AUX_CMD		0x7f	/* b7:4 command, b3:0 address b19:16 */
#define M9_DP_AUX_CMD_WRITE	0x80u
#define M9_DP_AUX_CMD_READ	0x90u
#define M9_DP_AUX_LEN		0x80	/* transfer length minus one */
#define M9_DP_AUX_WDATA		0x81
#define M9_DP_AUX_RDATA		0x82
#define M9_DP_AUX_GO		0x83
#define M9_DP_AUX_REPLY		0x84	/* b7:5 reply code, b4:0 byte count */
#define M9_DP_AUX_REPLY_CODE	0xe0u
#define M9_DP_AUX_REPLY_COUNT	0x1fu
#define M9_DP_AUX_ERRINFO	0x85
#define M9_DP_AUX_MAX		16u

/* Training master status: select with 0x8f, read back through 0x8d. */
#define M9_DP_STATUS_SEL	0x8f
#define M9_DP_STATUS_SEL_TRAIN	0x04u
#define M9_DP_STATUS		0x8d
#define M9_DP_STATUS_MASK	0x70u
#define M9_DP_STATUS_TRAINED	0x40u

/* Arms the capability mirror; both vendor implementations open with it. */
#define M9_DP_ARM_A		0xaa
#define M9_DP_ARM_B		0xac

#define M9_DP_RATE_RBR		0x06u	/* 1.62 Gbps */
#define M9_DP_RATE_HBR		0x0au	/* 2.7 Gbps  */

int m9x_dp_train(struct m9x_device *mdev, unsigned int index, u32 pixel_khz);

#endif
