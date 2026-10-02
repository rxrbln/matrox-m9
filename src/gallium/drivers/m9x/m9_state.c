/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
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

/*
 * A mipmapped colour image is stored as a texture chain whose every level
 * outside the packed tail is laid out at its own extent -- a large level by
 * m9_render_surface_layout, a small one by the aperture calculator's pages
 * (m9_texture_layout_init) -- and the BGRA family is the one the
 * render-target address calculator also stores, so the engine renders into
 * such a level in place.  The tail levels share one 32x16 atlas at an origin
 * inside the parent allocation and have no surface shape of their own.  The
 * 10-bit family shares that placement, so a level renders in place wherever
 * its texture code is the render format's own.
 */
bool
m9_texture_renders_level(const struct m9_resource *resource, unsigned level)
{
   struct m9_format_info format;
   const struct m9_texture_level *lvl;
   struct m9_render_layout layout;

   if (!resource || level >= resource->texture.levels ||
       resource->texture.kind != M9_TEXTURE_KIND_2D ||
       resource->texture.faces != 1 ||
       resource->kind != M9_RESOURCE_LINEAR_TEXTURE ||
       resource->texture_code != (uint8_t)(resource->format << 1) ||
       !m9_format_info(resource->base.format, &format) ||
       !(format.usage & M9_FORMAT_COLOR))
      return false;
   lvl = &resource->texture.level[level];
   /* The colour attachment is relocated with a 256-aligned byte offset. */
   if (lvl->in_tail || (lvl->offset & 255) || lvl->depth != 1 ||
       !m9_render_surface_layout(lvl->width, lvl->height, resource->cpp,
                                 &layout))
      return false;
   /* A small level is stored by the aperture calculator, whose rows end
    * with the level's own 1 KiB pages rather than the contract height. */
   return (layout.small ||
           (uint64_t)layout.row_span * layout.allocation_height *
           resource->cpp <= lvl->face_bytes) &&
          lvl->offset + lvl->face_bytes <= resource->size;
}

static bool
m9_framebuffer_resource_valid(const struct m9_resource *resource,
                              enum m9_resource_kind kind, unsigned level)
{
   struct m9_format_info format;
   struct m9_render_layout layout;
   unsigned usage = kind == M9_RESOURCE_LINEAR_COLOR ?
                    M9_FORMAT_COLOR : M9_FORMAT_DEPTH;
   uint32_t width, height;
   bool texture;

   if (!resource || !resource->handle ||
       resource->domain != M9_BO_VRAM || resource->layout != 1 ||
       !resource->width || !resource->height || !resource->cpp ||
       !m9_format_info(resource->base.format, &format) ||
       !(format.usage & usage) || format.cpp != resource->cpp ||
       format.code != resource->format)
      return false;
   texture = kind == M9_RESOURCE_LINEAR_COLOR &&
             m9_texture_renders_level(resource, level);
   width = texture ? resource->texture.level[level].width : resource->width;
   height = texture ? resource->texture.level[level].height : resource->height;
   if (!m9_render_surface_layout(width, height, resource->cpp, &layout))
      return false;
   if (texture)
      return true;
   /* A plain render surface holds one level, so only level 0 is a target. */
   return !level && resource->kind == kind &&
          resource->row_span <= UINT16_MAX &&
          resource->row_span == layout.row_span &&
          layout.size <= resource->size;
}

/* Whether the engine can bind this level as colour target 0, which is what a
 * copy needs of its destination. */
bool
m9_resource_is_color_target(const struct m9_resource *resource,
                            unsigned level)
{
   return m9_framebuffer_resource_valid(resource, M9_RESOURCE_LINEAR_COLOR,
                                        level);
}

int
m9_context_set_framebuffer(struct m9_context *context,
                           struct m9_resource *color,
                           struct m9_resource *depth, unsigned level,
                           unsigned width, unsigned height)
{
   uint32_t level_width, level_height;

   if (!context || !m9_framebuffer_resource_valid(
                          color, M9_RESOURCE_LINEAR_COLOR, level))
      return -EINVAL;
   level_width = level ? color->texture.level[level].width : color->width;
   level_height = level ? color->texture.level[level].height : color->height;
   if (!width || !height || width > level_width || height > level_height ||
       (depth && (!m9_framebuffer_resource_valid(
                             depth, M9_RESOURCE_LINEAR_DEPTH, 0) ||
                  width > depth->width || height > depth->height)))
      return -EINVAL;
   context->color_offset = level ? color->texture.level[level].offset : 0;
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
