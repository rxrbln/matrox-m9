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
struct m9_faa_graph;
struct m9_faa_surface;

struct pipe_screen *m9_screen_create(int fd,
                                     const struct pipe_screen_config *config);
int m9_bo_create(struct m9_screen *screen, uint64_t size, uint32_t flags,
                 unsigned kind, struct m9_resource *resource);
int m9_bo_create_engine_only(struct m9_screen *screen, uint64_t size,
                             unsigned kind, struct m9_resource *resource);
void m9_bo_destroy(struct m9_screen *screen, struct m9_resource *resource);
/* Caller owns a zeroed graph and retires its GPU users before destruction.
 * Creation publishes all five buffers together, or leaves the graph intact. */
int m9_faa_graph_create(struct m9_screen *screen, uint32_t width,
                        uint32_t height, struct m9_faa_graph *graph);
void m9_faa_graph_destroy(struct m9_screen *screen, struct m9_faa_graph *graph);
/* Preparation retains destination/depth references and publishes all six
 * owners atomically. It emits nothing and leaves graph.initialized false.
 * A matching target reuses its owners. Replacing an initialized target
 * returns EBUSY; destroy requires all GPU users to have retired. */
int m9_faa_surface_prepare(struct m9_screen *screen,
                           struct m9_resource *destination,
                           struct m9_resource *depth, uint64_t offset,
                           uint32_t width, uint32_t height,
                           struct m9_faa_surface *surface);
void m9_faa_surface_destroy(struct m9_screen *screen,
                            struct m9_faa_surface *surface);
int m9_command_bo_create(struct m9_screen *screen,
                         struct m9_resource *resource);
void m9_command_bo_destroy(struct m9_screen *screen,
                           struct m9_resource *resource);
struct pipe_context *m9_context_create(struct pipe_screen *screen, void *priv,
                                       unsigned flags);
struct pipe_fence_handle *m9_fence_create(uint64_t point, bool signaled);
int m9_wait_for_point(struct m9_screen *screen, uint64_t point);

void m9_faa_store_report(struct m9_screen *screen);

#endif
