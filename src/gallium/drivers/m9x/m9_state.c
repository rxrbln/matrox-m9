/* SPDX-License-Identifier: MIT */
#include "m9_state.h"
#include "m9_batch.h"
#include "m9_format.h"

#include <errno.h>
#include <string.h>

#include "util/u_inlines.h"

void
m9_context_state_init(struct m9_context *context, struct m9_screen *screen)
{
   memset(context, 0, sizeof(*context));
   context->screen = screen;
   context->base.screen = screen ? &screen->base : NULL;
   context->dirty = M9_DIRTY_ALL;
   m9_batch_init(&context->batch);
}

void
m9_context_state_cleanup(struct m9_context *context)
{
   if (!context)
      return;
   pipe_resource_reference(&context->color, NULL);
   pipe_resource_reference(&context->depth, NULL);
   for (unsigned i = 0; i < ARRAY_SIZE(context->textures); i++)
      pipe_resource_reference(&context->textures[i], NULL);
}

static bool
m9_framebuffer_resource_valid(const struct m9_resource *resource,
                              enum m9_resource_kind kind)
{
   struct m9_format_info format;
   struct m9_render_layout layout;
   unsigned usage = kind == M9_RESOURCE_LINEAR_COLOR ?
                    M9_FORMAT_COLOR : M9_FORMAT_DEPTH;
   uint64_t bytes;

   if (!resource || !resource->handle || resource->kind != kind ||
       resource->domain != M9_BO_VRAM || resource->layout != 1 ||
       resource->row_span > UINT16_MAX ||
       !resource->width || !resource->height || !resource->cpp ||
       !m9_format_info(resource->base.format, &format) ||
       !(format.usage & usage) || format.cpp != resource->cpp ||
       format.code != resource->format ||
       !m9_render_surface_layout(resource->width, resource->height,
                                 resource->cpp, &layout) ||
       resource->row_span != layout.row_span)
      return false;
   bytes = layout.size;
   return bytes <= resource->size;
}

int
m9_context_set_framebuffer(struct m9_context *context,
                           struct m9_resource *color,
                           struct m9_resource *depth,
                           unsigned width, unsigned height)
{
   if (!context || !m9_framebuffer_resource_valid(
                          color, M9_RESOURCE_LINEAR_COLOR) ||
       !width || !height || width > color->width || height > color->height ||
       (depth && (!m9_framebuffer_resource_valid(
                             depth, M9_RESOURCE_LINEAR_DEPTH) ||
                  width > depth->width || height > depth->height)))
      return -EINVAL;
   pipe_resource_reference(&context->color, &color->base);
   pipe_resource_reference(&context->depth, depth ? &depth->base : NULL);
   context->framebuffer_width = width;
   context->framebuffer_height = height;
   context->dirty |= M9_DIRTY_FRAMEBUFFER;
   return 0;
}

void
m9_context_set_depth(struct m9_context *context, bool test, bool write)
{
   if (!context)
      return;
   if (context->depth_test == test && context->depth_write == write)
      return;
   context->depth_test = test;
   context->depth_write = write;
   context->dirty |= M9_DIRTY_DEPTH;
}

int
m9_resource_clear_linear(struct m9_resource *resource, uint32_t value,
                         unsigned x, unsigned y,
                         unsigned width, unsigned height)
{
   uint64_t last;

   if (!resource || !resource->map || !resource->map_writable ||
       (resource->kind != M9_RESOURCE_LINEAR_COLOR &&
        resource->kind != M9_RESOURCE_LINEAR_DEPTH) ||
       (resource->cpp != 2 && resource->cpp != 4) || !width || !height ||
       x > resource->width || width > resource->width - x ||
       y > resource->height || height > resource->height - y)
      return -EINVAL;
   last = ((uint64_t)(y + height - 1) * resource->row_span + x + width) *
          resource->cpp;
   if (last > resource->size)
      return -EINVAL;
   for (unsigned row = y; row < y + height; row++) {
      uint8_t *dst = (uint8_t *)resource->map +
                     ((uint64_t)row * resource->row_span + x) * resource->cpp;
      if (resource->cpp == 4) {
         uint32_t *dwords = (uint32_t *)dst;
         for (unsigned col = 0; col < width; col++)
            dwords[col] = value;
      } else {
         uint16_t *words = (uint16_t *)dst;
         for (unsigned col = 0; col < width; col++)
            words[col] = value;
      }
   }
   return 0;
}
