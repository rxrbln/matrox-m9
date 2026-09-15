/* SPDX-License-Identifier: MIT */
#ifndef M9_STATE_H
#define M9_STATE_H

#include "m9_internal.h"

enum m9_dirty {
   M9_DIRTY_FRAMEBUFFER = 1ull << 0,
   M9_DIRTY_DEPTH       = 1ull << 1,
   M9_DIRTY_TEXTURES    = 1ull << 2,
   M9_DIRTY_PROGRAM     = 1ull << 3,
   M9_DIRTY_VERTEX      = 1ull << 4,
   M9_DIRTY_PRIMITIVE   = 1ull << 5,
   M9_DIRTY_ALL         = (1ull << 6) - 1,
};

void m9_context_state_init(struct m9_context *context,
                           struct m9_screen *screen);
void m9_context_state_cleanup(struct m9_context *context);
int m9_context_set_framebuffer(struct m9_context *context,
                               struct m9_resource *color,
                               struct m9_resource *depth,
                               unsigned width, unsigned height);
void m9_context_set_depth(struct m9_context *context, bool test, bool write);
int m9_resource_clear_linear(struct m9_resource *resource, uint32_t value,
                             unsigned x, unsigned y,
                             unsigned width, unsigned height);

#endif
