/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_GRAPH_LATER_STAGE_H
#define M9_FAA_GRAPH_LATER_STAGE_H

#include "m9_faa_graph_dual_first_encode.h"

/* Stage-0 null-target reload plus the C0 pair; replaces one 0x70 word. */
#define M9_FAA_SWITCH_RELOAD_BYTES 100u
/* No-clear and appended stages widen the leading E3 group by one word. */
#define M9_FAA_SWITCH_E3_BYTES 4u
#define M9_FAA_SWITCH_STAGE_MAX (M9_FAA_DUAL_FIRST_SECONDARY_BYTES + \
				 M9_FAA_SWITCH_RELOAD_BYTES + \
				 M9_FAA_SWITCH_E3_BYTES)
/* An appended DRAW's stage opens with the 144-byte save block. */
#define M9_FAA_APPEND_SAVE_BYTES 144u
#define M9_FAA_APPEND_STAGE_MAX (M9_FAA_APPEND_SAVE_BYTES + \
				 M9_FAA_SWITCH_STAGE_MAX)

/* Fixed 1x1 null-target sampler state; no allocation address. */
extern const u32 m9x_faa_null_target[24];

enum m9x_faa_program_kind {
	M9_FAA_PROGRAM_DUAL,	/* COLOR*T0*T1 MODULATE, FS72 */
	M9_FAA_PROGRAM_SINGLE,	/* COLOR*T0 MODULATE, FS48 */
	M9_FAA_PROGRAM_KINDS
};

/* Vendor program cache (13cd00) outcome for a later DRAW. */
enum m9x_faa_later_form {
	M9_FAA_LATER_PLAIN,	/* current program kept */
	M9_FAA_LATER_CACHED,	/* cached program selected */
	M9_FAA_LATER_BUILD,	/* program built and uploaded */
};

struct m9x_faa_later_stage {
	u32 bytes, split;	/* stage length, second primary range */
	u32 units, site[2];	/* sampler 0x70 header offsets */
};

/* Later stage of a DRAW from its program's live FIRST stage (single 2720
 * or dual 2880 bytes, any epoch). epoch is the DRAW's. e3 widens the E3
 * group (no-clear first DRAW or any append). cull_state keeps the 6A cull
 * groups: the program last emitted another cull state (BUILD keeps all).
 * A NULL out validates and sizes the stage without writing.
 */
int m9x_faa_later_stage_encode(const u8 *first, u32 first_bytes, u32 epoch,
			       enum m9x_faa_later_form form, bool append,
			       bool e3, bool cull_state, u8 *out, u32 capacity,
			       struct m9x_faa_later_stage *stage);

#endif
