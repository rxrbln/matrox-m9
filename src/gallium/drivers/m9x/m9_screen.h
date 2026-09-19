/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_SCREEN_H
#define M9_SCREEN_H

#include <stdbool.h>
#include <stdint.h>

struct pipe_screen;
struct pipe_screen_config;
struct m9_resource;
struct m9_screen;

struct pipe_screen *m9_screen_create(int fd,
                                     const struct pipe_screen_config *config);
int m9_bo_create(struct m9_screen *screen, uint64_t size, uint32_t flags,
                 unsigned kind, struct m9_resource *resource);
int m9_bo_create_engine_only(struct m9_screen *screen, uint64_t size,
                             unsigned kind, struct m9_resource *resource);
void m9_bo_destroy(struct m9_screen *screen, struct m9_resource *resource);
int m9_command_bo_create(struct m9_screen *screen,
                         struct m9_resource *resource);
void m9_command_bo_destroy(struct m9_screen *screen,
                           struct m9_resource *resource);
struct pipe_context *m9_context_create(struct pipe_screen *screen, void *priv,
                                       unsigned flags);
struct pipe_fence_handle *m9_fence_create(uint64_t point, bool signaled);

void m9_faa_store_report(struct m9_screen *screen);

#endif
