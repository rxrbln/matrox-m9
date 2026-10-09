/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Engine fill of a video-memory range: the vendor's colour-target rectangle
 * copy (MxMD.dll resource unlock download, 280c040c) from a zeroed tiled
 * source into the range bound as a linear raw32 colour target.
 */
#ifndef M9_ENGINE_FILL_ENCODE_H
#define M9_ENGINE_FILL_ENCODE_H

#include <linux/types.h>

/* Elements a row; 1024 makes a row one 4 KiB page. */
#define M9X_ENGINE_FILL_SPAN		1024u
#define M9X_ENGINE_FILL_MAX_SPAN	2048u
#define M9X_ENGINE_FILL_MAX_ROWS	2048u
/* Dwords a launch after the first adds: b0/b1/f4, fc and its e6/e3. */
#define M9X_ENGINE_FILL_LAUNCH_WORDS	9u

struct m9x_engine_fill_request {
	/* Untagged local GPU addresses of the range and the zeroed source. */
	u32 dest;
	u32 bytes;
	u32 source;
	/* Rows of @span elements the tiled source holds. */
	u32 source_rows;
	u32 span;
	u32 fence_bus;
	/* First free timeline target; the stream uses a run above it. */
	u32 token;
};

struct m9x_engine_fill_plan {
	u32 dwords;
	u32 launches;
	u32 rows;
	/* Writeback target the stream completes with. */
	u32 final;
};

u32 m9x_engine_fill_capacity(u32 launches);
int m9x_engine_fill_encode(const struct m9x_engine_fill_request *r,
			   u32 *words, u32 capacity,
			   struct m9x_engine_fill_plan *plan);

#endif
