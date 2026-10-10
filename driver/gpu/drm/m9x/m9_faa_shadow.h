/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_SHADOW_H
#define M9_FAA_SHADOW_H

#ifdef M9_FAA_SHADOW_HOST_TEST
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
#else
#include <linux/types.h>
#endif

/*
 * The FAA context's register values, as its own streams last wrote them,
 * and the registers ordinary jobs have changed since: after an interlude
 * the dirty ones are written back before the FAA part continues.
 */
struct m9x_faa_shadow {
	u32 value[256];
	u32 written[8];
	u32 dirty[8];
	/* Links whose range no known owner maps; their state is not seen. */
	u32 unresolved;
};

/* CPU view of a secondary range a stream links to, or NULL. */
typedef const u8 *(*m9x_faa_shadow_resolve)(void *ctx, u32 gpu, u32 bytes);

/* m9_submit.c: the state an ordinary job may write. */
bool m9x_submit_index_writable(u8 index);
bool m9x_faa_shadow_restorable(u8 index);
void m9x_faa_shadow_record(struct m9x_faa_shadow *s, const u8 *stream,
			   u32 bytes, m9x_faa_shadow_resolve resolve,
			   void *ctx);
void m9x_faa_shadow_mark(struct m9x_faa_shadow *s, const u8 *stream,
			 u32 bytes, m9x_faa_shadow_resolve resolve, void *ctx);
/* Prepare without committing: >=0 words or an error; dirty state is kept
 * until the successful END post records the actual emitted packets. */
int m9x_faa_shadow_restore(const struct m9x_faa_shadow *s, u8 *out,
			  u32 capacity);

#endif
