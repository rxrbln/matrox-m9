/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_VS_TEXTURE_H
#define M9_VS_TEXTURE_H

struct pipe_context;

/* Units the draw module samples for the vertex stage. */
#define M9_VS_MAX_SAMPLERS 8

void m9_vs_texture_install(struct pipe_context *pctx);

#endif
