/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_GRAPH_LATER_SWITCH_H
#define M9_FAA_GRAPH_LATER_SWITCH_H

#include "m9_faa_graph_dual_first_encode.h"

/* Stage-0 null-target reload plus the C0 pair; replaces one 0x70 word. */
#define M9_FAA_SWITCH_RELOAD_BYTES 100u
/* No-clear and appended stages widen the leading E3 group by one word. */
#define M9_FAA_SWITCH_E3_BYTES 4u
#define M9_FAA_SWITCH_STAGE_MAX (M9_FAA_DUAL_FIRST_SECONDARY_BYTES + \
				 M9_FAA_SWITCH_RELOAD_BYTES + \
				 M9_FAA_SWITCH_E3_BYTES)

/* Fixed 1x1 null-target sampler state; no allocation address. */
extern const u32 m9x_faa_null_target[24];

/* A later DRAW that builds a new fragment program carries that program's
 * FIRST stage, freshly encoded at the DRAW's epoch from live owners, plus
 * the stage-0 reload a plain later stage carries. first is a single (2720)
 * or dual (2880) FIRST stage. split receives the second primary range.
 */
int m9x_faa_switch_stage_encode(const u8 *first, u32 first_bytes, u32 epoch,
				bool e3, u8 *out, u32 capacity, u32 *bytes,
				u32 *split);

#endif
