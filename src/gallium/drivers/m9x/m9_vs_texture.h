/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_VS_TEXTURE_H
#define M9_VS_TEXTURE_H

#include <stdbool.h>

struct pipe_context;
struct pipe_sampler_view;

/* Units the draw module samples for the vertex stage. */
#define M9_VS_MAX_SAMPLERS 8

bool m9_vs_texture_init(struct pipe_context *pctx);
void m9_vs_texture_fini(struct pipe_context *pctx);
void m9_vs_texture_views(struct pipe_context *pctx, unsigned start,
                         unsigned count, unsigned unbind_trailing,
                         struct pipe_sampler_view **views);
void m9_vs_texture_states(struct pipe_context *pctx, unsigned start,
                          unsigned count, void **states);
void m9_vs_texture_refresh(struct pipe_context *pctx);

#endif
