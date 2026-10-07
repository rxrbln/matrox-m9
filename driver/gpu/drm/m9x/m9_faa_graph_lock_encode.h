/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_GRAPH_LOCK_ENCODE_H
#define M9_FAA_GRAPH_LOCK_ENCODE_H
#include "m9_faa_graph_draw_encode.h"
#define M9_FAA_LOCK_PRIMARY_BYTES 3556u
#define M9_FAA_LOCK_FIRST_POST_BYTES 3544u
bool m9x_faa_graph_lock_blob_valid(const u8 *blob, size_t bytes);
/* Fixed 320x240 red graph. Caller retains the DRAW graph through Lock's
 * actual receipt. Destination is disjoint from the authenticated blob. */
int m9x_faa_graph_lock_encode(const u8 *blob, size_t bytes,
 const struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS],
 u32 live_primary_bytes, u32 width, u32 height, u64 draw_point,
 u8 *primary, size_t primary_bytes);
#endif
