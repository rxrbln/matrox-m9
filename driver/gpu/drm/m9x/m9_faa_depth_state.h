/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_DEPTH_STATE_H
#define M9_FAA_DEPTH_STATE_H
#include "m9_hw_encode.h"
/* ABI: bit0 enable, bit1 write, bits4:2 Gallium comparison (NEVER..ALWAYS).
 * Original M9128 139670 jump TABLE, not linear order of return blocks:
 * D3D1..8 -> 0,2,1,3,4,6,5,7. 137f20 places that code in A8[4:2].
 * Disabled depth changes the shader/EF route and is not admitted here. */
static inline bool m9x_faa_depth_flags_valid(u32 flags)
{
 return (flags & 1u) && !(flags & ~31u);
}
static inline u32 m9x_faa_depth_a8(u32 flags)
{
	/* Zero is only the internal legacy source-fixture default LEQUAL/write.
	 * Both public request paths require explicit enabled flags.
	 */
	if (!flags)
		flags = 15;
	return m9_hw_depth_a8(flags & 1u, flags >> 1, m9_hw_compare(flags >> 2));
}
static inline bool m9x_faa_depth_a8_valid(u32 word)
{
 return (word & ~30u) == 0x00380381u;
}
#endif
