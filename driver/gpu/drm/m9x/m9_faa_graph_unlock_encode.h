/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_GRAPH_UNLOCK_ENCODE_H
#define M9_FAA_GRAPH_UNLOCK_ENCODE_H
#include <linux/types.h>
#include <linux/errno.h>
#include "m9_faa_graph_surface_layout.h"
#define M9_FAA_UNLOCK_PRIMARY_BYTES 96u
#define M9_FAA_UNLOCK_FIRST_POST_BYTES 84u
/* RE4f005ade: public Unlock's F021 -> F022 copy and separate finish.
 * The descriptor modes and copy state below are source-proven only for the
 * admitted source geometry. Both typed operands alias the retained primary;
 * addresses, dimensions and timeline words are always live inputs.
 * Admission is atomic: no output writes occur on an invalid request. */
static inline int m9x_faa_graph_unlock_encode(u32 base, u64 bytes,
		u32 width, u32 height, u64 lock_point, u8 *out, size_t out_bytes)
{
	u32 words[24] = {
		0x4545e3e6, 1, 0, 0x90928fe6, 0x04000001, 0xc0000600,
		0xffffffff, 0, 0xf0b1b091, 0, 0, 0, 0x0a0c060c,
		0xf2f37978, 0, 0, 0, 0, 0x4545fcef, 0xaa, 0,
		0x4545e3e6, 1, 0,
	};
	struct m9x_faa_surface_layout layout;
	u32 i;

	if (!out || out_bytes < M9_FAA_UNLOCK_PRIMARY_BYTES ||
	    !m9x_faa_runtime_shape(width,height) ||
	    !m9x_faa_surface_layout(width,height,&layout) ||
	    bytes != layout.owner[M9_FAA_SURFACE_PRIMARY].capacity ||
	    (base & 4095) || (u64)base + bytes > 0x100000000ull ||
	    (lock_point & 15) || (u32)lock_point < 0x170 ||
	    (u32)lock_point > 0xffffffcfu)
		return -EINVAL;
	words[2] = ((u32)lock_point + 0x10) | 1;
	words[10] = words[14] = base | 1;
	words[11] = 0xf0220000 | width;
	words[15] = 0xf0210000 | width;
	words[20] = height << 16 | width;
	words[23] = ((u32)lock_point + 0x20) | 1;
	for (i = 0; i < 24; i++) {
		out[4 * i] = words[i];
		out[4 * i + 1] = words[i] >> 8;
		out[4 * i + 2] = words[i] >> 16;
		out[4 * i + 3] = words[i] >> 24;
	}
	return 0;
}
#endif
