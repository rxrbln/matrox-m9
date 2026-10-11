/* SPDX-License-Identifier: MIT */
#ifndef M9X_PUBLIC_H
#define M9X_PUBLIC_H

#include "pipe/p_defines.h"

struct pipe_screen;

struct pipe_screen *m9_screen_create(int fd,
                                     const struct pipe_screen_config *config);

#endif
