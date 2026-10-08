/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_TEXTURE_DUAL_LINK_H
#define M9_FAA_GRAPH_TEXTURE_DUAL_LINK_H

#include "m9_faa_graph_texture_bind.h"

/* Internal live-owner inputs, never a userspace address ABI. The caller
 * authenticates/pins both owners and retains them through DMA retirement.
 * Program/constant bytes come from the live four-vector compiler layout. */
struct m9x_faa_texture_dual_link_input {
	u32 program_base, program_owner_bytes, program_offset, program_bytes;
	u32 entry_bytes[4];
	u32 constants_base, constants_owner_bytes, data_offset, constants_bytes;
	/* C3 byte-address tag: 2 for standalone LOCAL constants, 1 for
	 * retained FIRST-depth SYSTEM owner28. Caller proves the mapped domain. */
	u32 constants_tag;
	u32 constant_begin[4], constant_end[4];
};

/* Link only the four C2/C3 packet sites in an independently constructed,
 * source-qualified 2880-byte dual secondary. This is not a whole-command
 * constructor or submission admission. No captured owner address is used. */
int m9x_faa_texture_dual_link_place(
	const struct m9x_faa_texture_dual_link_input *input,
	u8 *secondary, u32 secondary_bytes);

#endif
