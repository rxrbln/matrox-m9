/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_REPLAY_H
#define M9_REPLAY_H

#include "pipe/p_context.h"
#include "pipe/p_state.h"

struct m9_context;

/* Record-and-replay of FAA-admitted draws: admitted draws and clears are
 * recorded instead of rendered ordinarily; a frame the FAA path cannot
 * present is replayed through the ordinary path. */
void m9_replay_init(struct m9_context *ctx);
void m9_replay_fini(struct m9_context *ctx);
bool m9_replay_enabled(void);
bool m9_replay_recording(const struct m9_context *ctx);
bool m9_replay_replaying(const struct m9_context *ctx);
int m9_replay_record_draw(struct m9_context *ctx, const struct pipe_draw_info *info,
                          unsigned drawid_offset,
                          const struct pipe_draw_start_count_bias *draws,
                          unsigned num_draws);
int m9_replay_record_clear(struct m9_context *ctx, unsigned buffers,
                           uint32_t color_clear_mask, uint8_t stencil_clear_mask,
                           const struct pipe_scissor_state *scissor,
                           const union pipe_color_union *color,
                           double depth, unsigned stencil);
/* The FAA frame was presented: drop the recording. */
void m9_replay_discard(struct m9_context *ctx);
/* The FAA frame was not presented: render the recording ordinarily.
 * Returns the number of replayed operations. */
unsigned m9_replay_materialize(struct m9_context *ctx);

#endif
