/* SPDX-License-Identifier: MIT */
#ifndef M9_SCREEN_H
#define M9_SCREEN_H

struct pipe_screen;
struct pipe_screen_config;
struct m9_resource;
struct m9_screen;

struct pipe_screen *m9_screen_create(int fd,
                                     const struct pipe_screen_config *config);
int m9_command_bo_create(struct m9_screen *screen,
                         struct m9_resource *resource);
void m9_command_bo_destroy(struct m9_screen *screen,
                           struct m9_resource *resource);

#endif
