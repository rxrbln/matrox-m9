/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_GRAPH_VERTEX_BANKS_H
#define M9_FAA_GRAPH_VERTEX_BANKS_H

/* Logical extent of vertex owner 23 (m9_faa_owner_specs[23]). */
#define M9_FAA_VERTEX_BANK_EXTENT 0x460080u

/* Original 182d0c..182d43: per-bank stride of the vertex owner. */
static inline u32 m9x_faa_vertex_bank_stride(u32 extent, u32 banks)
{
	return ((extent / (16 * banks + 4)) >> 6) << 10;
}

/* The live owner must be the page-aligned allocation of that extent. */
static inline bool m9x_faa_vertex_banks_fit(u32 base, u32 bytes, u32 banks)
{
	u32 stride = m9x_faa_vertex_bank_stride(M9_FAA_VERTEX_BANK_EXTENT,
						banks);

	return bytes >= M9_FAA_VERTEX_BANK_EXTENT &&
	       bytes - M9_FAA_VERTEX_BANK_EXTENT < 4096 &&
	       banks * stride + 16 <= bytes && base <= ~0u - bytes;
}

#endif
