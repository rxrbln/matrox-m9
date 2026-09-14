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
#define M9_REQUESTER_CTL0_FALLBACK	0x1f280800
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
#define M9_REG_IRQ0_STATUS		0x08d8
#define M9_REG_IRQ0_MASK		0x08dc
#define M9_REG_IRQ1_STATUS		0x08e4
#define M9_REG_IRQ1_MASK		0x08e8
#define M9_REG_IRQ2_STATUS		0x08f8
#define M9_REG_IRQ2_MASK		0x08fc
#define M9_REG_FENCE0_ADDRESS		0x1068
#define M9_REG_FENCE1_ADDRESS		0x106c
#define M9_REG_CONTEXT_BASE		0x109c

#define M9_COMMAND_SYSTEM_TAG		2u
#define M9_AUX_DMA32_CLASS		2u
#define M9_AUX_DMA32_TAG		0u
#define M9_AUX_QUIESCENT		BIT(0)
#define M9_AUX_PARK			BIT(1)

#define M9_PACKET_STOP			0x45u
#define M9_PACKET_FENCE_HEADER		0x4545e3e6u

#endif
