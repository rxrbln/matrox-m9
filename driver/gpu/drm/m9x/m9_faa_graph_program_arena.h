/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_PROGRAM_ARENA_H
#define M9_FAA_GRAPH_PROGRAM_ARENA_H

#include "m9_faa_graph_color_encode.h"

#define M9_FAA_PROGRAM_ARENA_JOBS 32u
#define M9_FAA_PROGRAM_PAGE_BYTES 4096u

/* Every secondary is a freshly encoded live COLOR4 stage. Its four C2 page
 * addresses must already name owner26; only its entry selectors are changed.
 * The caller owns code and secondary storage through the final GPU receipt. */
struct m9x_faa_program_arena_job {
	const u8 *program;
	u32 program_bytes;
	u32 entry_bytes[4];
	u8 *secondary;
	u32 secondary_bytes;
	u32 declaration_count;
};

/* Pure CPU preparation. A failure leaves every destination unchanged. Code
 * outside the copied live program spans is not initialized or interpreted. */
int m9x_faa_program_arena_encode(
	const struct m9x_faa_graph_draw_owner *program_owner,
	const struct m9x_faa_program_arena_job *job, u32 count,
	u8 *code_page, u32 code_capacity, u32 *slot_offset);

/* Two-vector fused POSITION/COLOR profile. The fresh source secondary has
 * one VS declaration group for decl3 and two for decl5, plus two C2 pairs.
 * Unused entry_bytes[2..3] must be zero. This does not submit a frame. */
int m9x_faa_program_arena_two_encode(
 const struct m9x_faa_graph_draw_owner *program_owner,
 const struct m9x_faa_program_arena_job *job, u32 count,
 u8 *code_page, u32 code_capacity, u32 *slot_offset);

#endif
