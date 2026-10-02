/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Vertex-stage texture fetch.  Vertex programs run on the CPU in the draw
 * module, so its own sampler does the fetching and the driver only has to
 * hand it a linear copy of every bound image.  The pipe_context entries are
 * wrapped from m9_screen.c, which installs this: the vtable itself is built
 * in m9_context.c.
 */
#include "m9_vs_texture.h"

#include "m9_internal.h"

#include "draw/draw_context.h"
#include "util/format/u_format.h"
#include "util/macros.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_memory.h"

#include <string.h>

/* An image that cannot be read is published as this texel instead. */
static const uint8_t m9_vs_zero_texel[16];

struct m9_vs_textures {
   void (*bind_sampler_states)(struct pipe_context *, mesa_shader_stage,
                               unsigned, unsigned, void **);
   void (*set_sampler_views)(struct pipe_context *, mesa_shader_stage,
                             unsigned, unsigned, unsigned,
                             struct pipe_sampler_view **);
   void (*draw_vbo)(struct pipe_context *, const struct pipe_draw_info *,
                    unsigned, const struct pipe_draw_indirect_info *,
                    const struct pipe_draw_start_count_bias *, unsigned);
   void (*destroy)(struct pipe_context *);
   struct pipe_sampler_view *views[M9_VS_MAX_SAMPLERS];
   struct pipe_sampler_state *states[M9_VS_MAX_SAMPLERS];
   /* De-tiled mip chain handed to the draw module, grown as images change. */
   uint8_t *linear[M9_VS_MAX_SAMPLERS];
   size_t linear_size[M9_VS_MAX_SAMPLERS];
   unsigned num_views, num_states;
};

static inline struct m9_context *
m9_vs_context(struct pipe_context *pctx)
{
   return (struct m9_context *)pctx;
}

/* Publish one bound image to the draw module as a linear mip chain.  Storage
 * is tiled, so every level goes through a transfer; an image that cannot be
 * read is published as a single zero texel rather than left unmapped, which
 * the sampler would follow into a null pointer. */
static void
m9_vs_map_view(struct pipe_context *pctx, struct m9_vs_textures *vs,
               unsigned slot)
{
   uint32_t row_stride[PIPE_MAX_TEXTURE_LEVELS] = {0};
   uint32_t img_stride[PIPE_MAX_TEXTURE_LEVELS] = {0};
   uint32_t mip_offsets[PIPE_MAX_TEXTURE_LEVELS] = {0};
   struct pipe_sampler_view *view = vs->views[slot];
   struct draw_context *draw = m9_vs_context(pctx)->draw;
   struct pipe_resource *res = view ? view->texture : NULL;
   unsigned first = 0, last = 0;
   size_t total = 0;
   bool mapped = res && res->target != PIPE_BUFFER;

   if (mapped) {
      unsigned blocksize = util_format_get_blocksize(view->format);

      first = view->u.tex.first_level;
      last = MIN2(view->u.tex.last_level, res->last_level);
      mapped = first <= last && last < PIPE_MAX_TEXTURE_LEVELS;
      for (unsigned level = first; mapped && level <= last; level++) {
         row_stride[level] = u_minify(res->width0, level) * blocksize;
         img_stride[level] = row_stride[level] * u_minify(res->height0, level);
         mip_offsets[level] = total;
         total += img_stride[level];
      }
   }
   if (mapped && total > vs->linear_size[slot]) {
      uint8_t *grown = REALLOC(vs->linear[slot], vs->linear_size[slot], total);

      mapped = grown;
      if (grown) {
         vs->linear[slot] = grown;
         vs->linear_size[slot] = total;
      }
   }
   for (unsigned level = first; mapped && level <= last; level++) {
      struct pipe_box box = { .width = u_minify(res->width0, level),
                              .height = u_minify(res->height0, level),
                              .depth = 1 };
      struct pipe_transfer *transfer = NULL;
      const uint8_t *src = pipe_texture_map(pctx, res, level, 0, PIPE_MAP_READ,
                                            0, 0, box.width, box.height,
                                            &transfer);

      if (!src) {
         mapped = false;
         break;
      }
      /* Bytes only: the draw module decodes an sRGB view itself. */
      util_format_translate(util_format_linear(view->format),
                            vs->linear[slot] + mip_offsets[level],
                            row_stride[level], 0, 0,
                            util_format_linear(res->format), src,
                            transfer->stride, 0, 0, box.width, box.height);
      pipe_texture_unmap(pctx, transfer);
   }
   if (!mapped) {
      memset(row_stride, 0, sizeof(row_stride));
      memset(img_stride, 0, sizeof(img_stride));
      memset(mip_offsets, 0, sizeof(mip_offsets));
      draw_set_mapped_texture(draw, MESA_SHADER_VERTEX, slot, 1, 1, 1, 0, 0,
                              0, 0, m9_vs_zero_texel, row_stride, img_stride,
                              mip_offsets);
      return;
   }
   draw_set_mapped_texture(draw, MESA_SHADER_VERTEX, slot,
                           res->width0, res->height0, 1, first, last, 0, 0,
                           vs->linear[slot], row_stride, img_stride,
                           mip_offsets);
}

void
m9_vs_texture_views(struct pipe_context *pctx, unsigned start, unsigned count,
                    unsigned unbind_trailing, struct pipe_sampler_view **views)
{
   struct m9_vs_textures *vs = m9_vs_context(pctx)->vs_textures;

   if (!vs)
      return;
   for (unsigned i = 0; i < count + unbind_trailing &&
                        start + i < ARRAY_SIZE(vs->views); i++)
      pipe_sampler_view_reference(&vs->views[start + i],
                                  views && i < count ? views[i] : NULL);
   vs->num_views = 0;
   for (unsigned i = 0; i < ARRAY_SIZE(vs->views); i++)
      if (vs->views[i])
         vs->num_views = i + 1;
   draw_set_sampler_views(m9_vs_context(pctx)->draw, MESA_SHADER_VERTEX,
                          vs->views, vs->num_views);
}

void
m9_vs_texture_states(struct pipe_context *pctx, unsigned start,
                     unsigned count, void **states)
{
   struct m9_vs_textures *vs = m9_vs_context(pctx)->vs_textures;

   if (!vs)
      return;
   for (unsigned i = 0; i < count && start + i < ARRAY_SIZE(vs->states); i++)
      vs->states[start + i] = states ? states[i] : NULL;
   vs->num_states = 0;
   for (unsigned i = 0; i < ARRAY_SIZE(vs->states); i++)
      if (vs->states[i])
         vs->num_states = i + 1;
   draw_set_samplers(m9_vs_context(pctx)->draw, MESA_SHADER_VERTEX,
                     vs->states, vs->num_states);
}

/* Render targets sampled by a vertex program change between draws, so the
 * linear copy is refreshed rather than cached. */
void
m9_vs_texture_refresh(struct pipe_context *pctx)
{
   struct m9_vs_textures *vs = m9_vs_context(pctx)->vs_textures;

   for (unsigned i = 0; vs && i < vs->num_views; i++)
      m9_vs_map_view(pctx, vs, i);
}

void
m9_vs_texture_fini(struct pipe_context *pctx)
{
   struct m9_context *ctx = m9_vs_context(pctx);
   struct m9_vs_textures *vs = ctx->vs_textures;

   if (!vs)
      return;
   for (unsigned i = 0; i < ARRAY_SIZE(vs->views); i++) {
      pipe_sampler_view_reference(&vs->views[i], NULL);
      FREE(vs->linear[i]);
   }
   ctx->vs_textures = NULL;
   FREE(vs);
}

bool
m9_vs_texture_init(struct pipe_context *pctx)
{
   struct m9_vs_textures *vs = CALLOC_STRUCT(m9_vs_textures);

   if (!vs)
      return false;
   m9_vs_context(pctx)->vs_textures = vs;
   return true;
}
