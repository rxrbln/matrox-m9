/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_VS_TEXTURE_H
#define M9_VS_TEXTURE_H

#include <stdbool.h>
#include <stdint.h>

struct pipe_context;
struct pipe_sampler_view;
struct pipe_sampler_state;

/* Units the draw module samples for the vertex stage. */
#define M9_VS_MAX_SAMPLERS 8

bool m9_vs_texture_init(struct pipe_context *pctx);
void m9_vs_texture_fini(struct pipe_context *pctx);
void m9_vs_texture_views(struct pipe_context *pctx, unsigned start,
                         unsigned count, unsigned unbind_trailing,
                         struct pipe_sampler_view **views);
void m9_vs_texture_states(struct pipe_context *pctx, unsigned start,
                          unsigned count, void **states);
void m9_vs_texture_refresh(struct pipe_context *pctx, uint32_t used);
/* Borrow currently retained bindings; jobs snapshot words and retain the
 * resource before sampler/view state can change. */
struct pipe_sampler_view *m9_vs_texture_view(struct pipe_context *pctx,
                                           unsigned slot);
const struct pipe_sampler_state *m9_vs_texture_state(struct pipe_context *pctx,
                                                    unsigned slot);

#endif
