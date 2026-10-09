/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_ENGINE_2D_STATE_H
#define M9_ENGINE_2D_STATE_H

#include <linux/types.h>

/*
 * Emits the 2-D blit context; token is the timeline target its two bank
 * switches carry.  @used is the full length even when -ENOSPC.
 */
int m9x_engine_2d_state(u32 *words, u32 capacity, u32 token, u32 *used);
u32 m9x_engine_2d_state_words(void);

#endif
