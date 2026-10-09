/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Engine upload of a system-memory image into a tiled colour target or
 * texture level, after the vendor system->video upload (MxMD.dll resource
 * unlock with the shadow caps bit, b21b0 -> 56a10 -> 1b8d0 -> 1b650).
 */
#ifndef M9_ENGINE_WRITE_ENCODE_H
#define M9_ENGINE_WRITE_ENCODE_H

#include <linux/types.h>

/* Rows one launch and one linear source descriptor may span. */
#define M9X_ENGINE_WRITE_MAX_ROWS	2048u

struct m9x_engine_write_request {
	/* Untagged local GPU address and live descriptor pair of the target. */
	u32 dest;
	u32 descriptor0, descriptor1;
	/* Rows y..y+height-1 remain; one batch takes as many as fit. */
	u32 x, y, width, height;
	/* Untagged class-1 GPU address of the staging image and its bytes. */
	u32 source;
	u32 source_bytes;
	u32 fence_bus;
	/* First free timeline target; the batch uses a run above it. */
	u32 token;
};

struct m9x_engine_write_plan {
	u32 dwords;
	/* Rows this batch copies; the caller stages them at @pitch. */
	u32 rows;
	u32 pitch;
	/* Writeback target the batch completes with. */
	u32 final;
};

u32 m9x_engine_write_words(void);
int m9x_engine_write_encode(const struct m9x_engine_write_request *r,
			    u32 *words, u32 capacity,
			    struct m9x_engine_write_plan *plan);

#endif
