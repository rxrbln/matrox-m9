/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Engine readback of a tiled render target into system memory, after the
 * vendor DdBlt video->system copy (MxMD.dll 0xb1290).
 */
#ifndef M9_ENGINE_READ_ENCODE_H
#define M9_ENGINE_READ_ENCODE_H

#include <linux/types.h>

/* The vendor alternates its copy between two coherent rings. */
#define M9X_ENGINE_READ_RINGS		2u
/* Dwords one chunk adds: b6/f2/fc and its e6/e3 token. */
#define M9X_ENGINE_READ_CHUNK_WORDS	7u

struct m9x_engine_read_ring {
	u32 bus;
	u32 bytes;
};

/* Where a chunk landed: ring, byte offset, first row relative to y. */
struct m9x_engine_read_chunk {
	u32 ring, offset, row, rows;
};

struct m9x_engine_read_request {
	/* Untagged local GPU address and the live descriptor pair. */
	u32 source;
	u32 descriptor0, descriptor1;
	/* Rows y..y+height-1 remain; one batch takes as many as fit. */
	u32 x, y, width, height;
	u32 fence_bus;
	/* First free timeline target; the batch uses a run above it. */
	u32 token;
	/* Replay the vendor 2-D context state before the copy. */
	bool state;
	struct m9x_engine_read_ring ring[M9X_ENGINE_READ_RINGS];
};

struct m9x_engine_read_plan {
	u32 dwords;
	u32 chunks;
	u32 rows;
	/* Ring row stride in bytes. */
	u32 pitch;
	/* Writeback target the batch completes with. */
	u32 final;
};

int m9x_engine_read_encode(const struct m9x_engine_read_request *r,
			   u32 *words, u32 capacity,
			   struct m9x_engine_read_chunk *chunks, u32 max_chunks,
			   struct m9x_engine_read_plan *plan);

#endif
