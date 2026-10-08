/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef __M9_CMD_H__
#define __M9_CMD_H__

#include <linux/types.h>
#include <linux/unaligned.h>

#include "m9_hw_encode.h"

/*
 * A packet header holds four register indices, (MMIO - 0x1000) >> 2,
 * consumed low byte first; index 0x45 ends the packet.
 */
#define M9_CMD_IDX_END		M9_HW_IDX_END
#define M9_CMD_HDR(a, b, c, d)	M9_HW_HDR(a, b, c, d)
#define M9_CMD_HDR2(a, b)	M9_HW_HDR2(a, b)

/* Secondary range: start | domain, end | 2 (counted pools: untagged end). */
#define M9_CMD_LINK		M9_CMD_HDR2(0x2c, 0x2d)
/* Tracker record: tag address, token | 1. */
#define M9_CMD_TRACK		M9_CMD_HDR2(0x1a, 0x42)
/* Fence record: mode, target | 1. */
#define M9_CMD_FENCE		M9_CMD_HDR2(0xe6, 0xe3)

#define M9_CMD_LINK_BYTES	12u

static inline u32 m9_cmd_get(const u8 *p)
{
	return get_unaligned_le32(p);
}

static inline void m9_cmd_put(u8 *p, u32 v)
{
	put_unaligned_le32(v, p);
}

static inline void m9_cmd_link(u32 w[3], u32 start, u32 end)
{
	w[0] = M9_CMD_LINK;
	w[1] = start;
	w[2] = end;
}

static inline void m9_cmd_put_link(u8 *p, u32 start, u32 end)
{
	m9_cmd_put(p, M9_CMD_LINK);
	m9_cmd_put(p + 4, start);
	m9_cmd_put(p + 8, end);
}

/*
 * Wrap-free: the higher-starting range begins inside the lower one (a is
 * lower on a tie). For non-empty ranges this is byte intersection.
 */
static inline bool m9_cmd_overlap(const void *a, size_t an,
				  const void *b, size_t bn)
{
	unsigned long x = (unsigned long)a, y = (unsigned long)b;

	return x <= y ? y - x < an : x - y < bn;
}

/*
 * Half-open byte ranges: an empty range never overlaps, and a range whose
 * end wraps the address space is refused as overlapping.
 */
static inline bool m9_cmd_ranges_overlap(const void *a, size_t an,
					 const void *b, size_t bn)
{
	unsigned long x = (unsigned long)a, y = (unsigned long)b;
	unsigned long ex = x + an, ey = y + bn;

	return ex < x || ey < y || (x < ey && y < ex);
}

#endif
