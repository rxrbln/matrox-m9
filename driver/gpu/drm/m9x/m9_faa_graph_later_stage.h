/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_GRAPH_LATER_STAGE_H
#define M9_FAA_GRAPH_LATER_STAGE_H

#include "m9_faa_graph_dual_first_encode.h"
#include "m9_faa_graph_program_arena.h"
#include "m9_faa_graph_texture_fragment.h"

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

/* One DRAW of a composed later frame: its stage within the frame's
 * secondary and the sampler sites there, for placement at submit.
 */
struct m9x_faa_frame_stage {
	u32 offset, bytes, units, site[2];
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

/* Sampler sites of a FIRST stage (single 2720 or dual 2880 bytes). */
int m9x_faa_first_stage_sites(u32 first_bytes, u32 *units, u32 site[2]);

/* Program cache of a retained graph, after the vendor's (13cd00): every
 * built program, where it lives in the owner-26 arena and the raster cull
 * state it last emitted, and the active one. A program is identified by
 * its kind, fragment state and exact bytes (FS and the VS-derived carry,
 * gather and position vectors). page mirrors the arena; bytes from
 * upload to page_bytes are not in owner 26 yet.
 */
/* Owner-26 allocation: the vendor gives each program 0x1080 bytes, the
 * retained graph one arena of whole pages for all of them.
 */
#define M9_FAA_PROGRAM_ARENA_BYTES 0x10000u

struct m9x_faa_program_entry {
	u32 kind, fragment, hash, offset, bytes, cull;
};

struct m9x_faa_program_cache {
	u32 count, active, page_bytes, upload, arena_bytes, capacity;
	struct m9x_faa_program_entry *entry;
	u8 *page;
};

/* Programs start 64-byte aligned and never cross an arena page, so an
 * arena of arena_bytes holds at most this many entries.
 */
static inline u32 m9x_faa_program_cache_capacity(u32 arena_bytes)
{
	return arena_bytes / 64;
}

/* Empty cache over caller storage: capacity entries and an arena_bytes
 * page mirror, arena_bytes a whole number of 4096-byte pages.
 */
int m9x_faa_program_cache_init(struct m9x_faa_program_cache *cache,
			       u32 arena_bytes,
			       struct m9x_faa_program_entry *entry, u8 *page);

/* Copy the state of src into dst of the same arena size. */
int m9x_faa_program_cache_copy(struct m9x_faa_program_cache *dst,
			       const struct m9x_faa_program_cache *src);

/* Forget every program; the next DRAWs build again from arena offset 0.
 * Only valid at a transaction boundary, when no frame uses the arena.
 */
void m9x_faa_program_cache_reset(struct m9x_faa_program_cache *cache);

/* Account one DRAW of program kind and fragment state with raster cull
 * flags. A FIRST frame builds into an empty cache. Returns the DRAW's form,
 * 6A decision and arena offset; -ERANGE when the arena or entry table is
 * full (the caller may reset at the frame boundary and retry).
 */
int m9x_faa_program_cache_draw(struct m9x_faa_program_cache *cache, u32 kind,
			       u32 fragment, const u8 *program, u32 bytes,
			       u32 raster, bool first,
			       enum m9x_faa_later_form *form, bool *cull_state,
			       u32 *offset);

#endif
