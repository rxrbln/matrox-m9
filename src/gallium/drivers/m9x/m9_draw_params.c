/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Every render-size, vertex-layout and raster-state dependent word of the
 * two-pass draw.  The encodings are hardware-measured on the M9148
 * (parhelia-m9-cpu-tnl-raster-state.md, parhelia-m9-cpu-tnl-draw-parameters.md).
 */
#include "m9_draw_params.h"
#include "m9_batch.h"
#include "m9_passthrough.h"

#include <errno.h>
#include <string.h>

#include "pipe/p_defines.h"

#define M9_GUARD_BAND 15359.0f

static void
m9_put_float(uint32_t *words, unsigned index, float value)
{
   memcpy(&words[index], &value, sizeof(value));
}

int
m9_draw_params_init(struct m9_draw_params *params, uint32_t width,
                    uint32_t height, uint8_t color_format)
{
   struct m9_surface_desc desc = {
      .height = height, .layers = 1, .layout = 1, .address_offset = true,
      .format = color_format,
   };
   struct m9_render_layout layout;
   float w = width, h = height;
   uint32_t *sys, *linked;

   if (!params || color_format != M9_COLOR_FORMAT_A8R8G8B8 ||
       !m9_render_surface_layout(width, height, 4, &layout))
      return -EINVAL;
   memset(params, 0, sizeof(*params));
   params->width = width;
   params->height = height;
   desc.row_span = layout.row_span;
   params->color_desc = m9_surface_desc0(&desc);
   desc.format = M9_DEPTH_FORMAT_D24S8;
   params->depth_desc = m9_surface_desc0(&desc);
   desc.format = M9_HZ_FORMAT;
   desc.address_offset = false;
   params->aux_desc = m9_surface_desc0(&desc);
   params->clip_min = 0;
   params->clip_max = width | height << 16;

   /* glOrtho(0,W,0,H,-1,1) with a full viewport and depth range [0,1]; the
    * remaining words of the captured prefix are size invariant. */
   sys = params->system_prefix;
   memcpy(sys, m9_passthrough_system_320x240, sizeof(params->system_prefix));
   m9_put_float(sys, 9, 2.0f / h);
   m9_put_float(sys, 14, 2.0f / w);
   m9_put_float(sys, 22, w / 2);
   m9_put_float(sys, 23, -h / 2);
   m9_put_float(sys, 24, 0.5f);
   m9_put_float(sys, 25, 1.0f);
   m9_put_float(sys, 26, w / 2);
   m9_put_float(sys, 27, h / 2);
   m9_put_float(sys, 28, 0.5f);
   m9_put_float(sys, 29, h);
   m9_put_float(sys, 41, M9_GUARD_BAND / (w / 2) - 1);
   m9_put_float(sys, 43, -(M9_GUARD_BAND / (h / 2) - 1));

   linked = params->linked_prefix;
   m9_put_float(linked, 0, w / 2);
   m9_put_float(linked, 1, -h / 2);
   m9_put_float(linked, 2, 0.5f);
   m9_put_float(linked, 3, 0.0f);
   m9_put_float(linked, 4, w / 2);
   m9_put_float(linked, 5, h / 2);
   m9_put_float(linked, 6, 0.5f);
   linked[7] = m9_passthrough_linked_320x240[7];
   return 0;
}

uint64_t
m9_hz_bytes(uint32_t width, uint32_t height)
{
   /* One word per 8x4 cell of the 32-aligned tiled grid. */
   uint64_t cells = ((width + 31ull) & ~31ull) / 8 *
                    (((height + 31ull) & ~31ull) / 4);

   return (cells * 4 + 0xfff) & ~0xfffull;
}

static uint32_t
m9_decl_word(unsigned dword, unsigned attr, unsigned s0, unsigned s1,
             unsigned s2, unsigned s3)
{
   return dword << 24 | attr << 20 | s0 | s1 << 3 | s2 << 6 | s3 << 9;
}

int
m9_vertex_decl_init(struct m9_vertex_decl *decl, unsigned position_components,
                    unsigned color_components)
{
   if (!decl || position_components < 2 || position_components > 4 ||
       color_components < 3 || color_components > 4)
      return -EINVAL;
   decl->position_components = position_components;
   decl->color_components = color_components;
   decl->record_bytes = 4 * (position_components + color_components);
   /* Selectors 0-3 pick a source component, 4 is 0.0, 5 is 1.0 and 6 leaves
    * the slot to the paired word; position slots are z,w | x,y. */
   decl->words[0] = m9_decl_word(0, 1, position_components > 2 ? 2 : 4,
                                 position_components > 3 ? 3 : 5, 6, 6);
   decl->words[1] = m9_decl_word(0, 1, 6, 6, 0, 1);
   decl->words[2] = m9_decl_word(position_components, 2, 0, 1, 6, 6);
   decl->words[3] = m9_decl_word(position_components, 2, 6, 6, 2,
                                 color_components > 3 ? 3 : 5);
   return 0;
}

uint32_t
m9_depth_control(bool test, bool write, unsigned pipe_func)
{
   static const uint8_t code[8] = {
      [PIPE_FUNC_NEVER] = 0, [PIPE_FUNC_EQUAL] = 1, [PIPE_FUNC_LESS] = 2,
      [PIPE_FUNC_LEQUAL] = 3, [PIPE_FUNC_GREATER] = 4, [PIPE_FUNC_GEQUAL] = 5,
      [PIPE_FUNC_NOTEQUAL] = 6, [PIPE_FUNC_ALWAYS] = 7,
   };

   if (!test)
      return 0x0038038a;
   return 0x00380381 | (uint32_t)code[pipe_func & 7] << 2 | (write ? 2 : 0);
}

uint32_t
m9_depth_ef(bool test, bool write)
{
   /* a8 alone enables nothing; ef carries the depth enable. */
   return test ? (write ? 0x0000f000 : 0x00007000) : 0;
}

uint32_t
m9_cull_ef(unsigned pipe_cull_face, bool front_ccw)
{
   if (pipe_cull_face != PIPE_FACE_FRONT && pipe_cull_face != PIPE_FACE_BACK)
      return 0;
   /* Measured through Mesa: cull back with CCW front faces is 0x03000000. */
   return (pipe_cull_face == PIPE_FACE_BACK) == front_ccw ?
          0x03000000 : 0x02000000;
}

uint32_t
m9_color_mask(unsigned pipe_colormask)
{
   uint32_t nibble = (pipe_colormask & PIPE_MASK_R ? 4 : 0) |
                     (pipe_colormask & PIPE_MASK_G ? 2 : 0) |
                     (pipe_colormask & PIPE_MASK_B ? 1 : 0) |
                     (pipe_colormask & PIPE_MASK_A ? 8 : 0);

   /* Each nibble gates the pixel columns x % 4 == n. */
   return 0xffff0000u | nibble * 0x1111u;
}

void
m9_scissor_words(uint32_t minx, uint32_t miny, uint32_t maxx, uint32_t maxy,
                 uint32_t *min, uint32_t *max)
{
   /* Exclusive bounds in top-down surface rows. */
   *min = minx | miny << 16;
   *max = maxx | maxy << 16;
}

uint32_t
m9_raster_control(bool flat, enum m9_topology topology)
{
   return (flat ? 0x6400u : 0x2400u) | topology;
}

uint32_t
m9_raster_d7(enum m9_topology topology)
{
   return 0x200058a0u | topology;
}

uint32_t
m9_raster_class(bool lines)
{
   return lines ? 0xc8008400u : 0xc4008400u;
}

bool
m9_topology_valid(unsigned topology)
{
   return (topology >= M9_TOPOLOGY_LINES &&
           topology <= M9_TOPOLOGY_QUAD_STRIP) ||
          topology == M9_TOPOLOGY_POLYGON;
}

uint32_t
m9_draw_preprocess_vertices(uint32_t raster_vertices)
{
   /* Preprocessing ranges end on a four-record boundary. */
   return (raster_vertices + 3) & ~3u;
}

uint32_t
m9_draw_max_source_vertices(void)
{
   return M9_TRANSIENT_VIEW_BYTES / M9_TRANSIENT_VERTEX_BYTES & ~3u;
}
