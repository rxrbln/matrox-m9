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
#include <math.h>
#include <string.h>

#include "util/u_math.h"
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
      .height = height, .layers = 1, .layout = 1,
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
   /* Measured: a surface of 64x64 or less is stored by the aperture
    * calculator, which is what its allocation size and the sampler both
    * assume.  Rendering it with the ordinary tiled bit set wrote rows
    * through the other permutation, and sampling the result returned a
    * shuffled image (rtt64, rtt32). */
   desc.address_offset = !layout.small;
   params->color_desc = m9_surface_desc0(&desc);
   desc.format = M9_DEPTH_FORMAT_D24S8;
   params->depth_desc = m9_surface_desc0(&desc);
   desc.format = M9_HZ_FORMAT;
   desc.address_offset = false;
   params->aux_desc = m9_surface_desc0(&desc);
   params->clip_min = 0;
   params->clip_max = width | height << 16;

   /* The vertex program transforms by a matrix in its constant span; the
    * driver has always supplied glOrtho(0,W,0,H,-1,1) there, because the
    * vertices reaching it are already in screen space.  The remaining
    * words of the captured prefix are size invariant. */
   sys = params->system_prefix;
   memcpy(sys, m9_passthrough_system_320x240, sizeof(params->system_prefix));
   {
      const float ortho[16] = {
         2.0f / w, 0.0f,     0.0f, -1.0f,
         0.0f,     2.0f / h, 0.0f, -1.0f,
         0.0f,     0.0f,    -1.0f,  0.0f,
         0.0f,     0.0f,     0.0f,  1.0f,
      };

      m9_draw_params_matrix(params, ortho);
   }
   m9_put_float(sys, 22, w / 2);
   m9_put_float(sys, 23, -h / 2);
   m9_put_float(sys, 24, 0.5f);
   m9_put_float(sys, 25, 1.0f);
   m9_put_float(sys, 26, w / 2);
   m9_put_float(sys, 27, h / 2);
   m9_put_float(sys, 28, 0.5f);
   m9_put_float(sys, 29, h);
   m9_put_float(sys, M9_VS_GUARD_X_DWORD, M9_GUARD_BAND / (w / 2) - 1);
   m9_put_float(sys, M9_VS_GUARD_Y_DWORD, -(M9_GUARD_BAND / (h / 2) - 1));

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

/* The four rows of the transform the vertex program applies, as pairs of
 * its constant span.  Each row is split: its first two components and its
 * last two live in different pairs, and the four DP4 slots of the captured
 * program name them in the order below (MEASURED; see
 * docs/parhelia-m9-native-vertex-isa.md).  Pair 3 is named by no slot. */
void
m9_draw_params_matrix(struct m9_draw_params *params, const float m[16])
{
   static const uint8_t dword[8] = { 14, 12, 8, 4, 20, 18, 16, 6 };
   uint32_t *sys = params->system_prefix;

   for (unsigned half = 0; half < 8; half++) {
      m9_put_float(sys, dword[half], m[2 * half]);
      m9_put_float(sys, dword[half] + 1, m[2 * half + 1]);
   }
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
   return m9_raster_control_decl(flat, topology, 5);
}

uint32_t
m9_preprocess_control(unsigned decl_count)
{
   /* The captured pass-through value is this with five words. */
   return 0x0008000eu | (decl_count - 1) << 8;
}

/* The tail applies scale then bias to the divided position; the shipping
 * words are the framebuffer's own viewport, and a pass whose program emits
 * clip coordinates needs the viewport actually in force.  The guard band is
 * the same ratio against the new half extent (the band does not cull:
 * docs/parhelia-m9-native-vertex-isa.md). */
void
m9_draw_params_viewport(struct m9_draw_params *params, const float scale[3],
                        const float bias[3], uint32_t *prefix)
{
   float ax = fabsf(scale[0]), ay = fabsf(scale[1]);

   if (!params || !scale || !bias || !prefix)
      return;
   m9_put_float(prefix, M9_VS_VIEWPORT_DWORD, scale[0]);
   m9_put_float(prefix, M9_VS_VIEWPORT_DWORD + 1, scale[1]);
   m9_put_float(prefix, M9_VS_DEPTH_SCALE_DWORD, scale[2]);
   m9_put_float(prefix, M9_VS_BIAS_DWORD, bias[0]);
   m9_put_float(prefix, M9_VS_BIAS_DWORD + 1, bias[1]);
   m9_put_float(prefix, M9_VS_DEPTH_BIAS_DWORD, bias[2]);
   m9_put_float(prefix, M9_VS_GUARD_X_DWORD,
                ax > 0.0f ? M9_GUARD_BAND / ax - 1.0f : 0.0f);
   m9_put_float(prefix, M9_VS_GUARD_Y_DWORD,
                ay > 0.0f ? -(M9_GUARD_BAND / ay - 1.0f) : 0.0f);
}

uint32_t
m9_raster_control_decl(bool flat, enum m9_topology topology,
                       unsigned decl_count)
{
   /* Bits 11:8 carry the raster declaration word count minus one. */
   return (flat ? 0x4000u : 0) | 0x2000u | (decl_count - 1) << 8 | topology;
}

int
m9_raster_fs_init(struct m9_raster_fs *r, unsigned vertex_entry,
                  uint32_t r88, unsigned const_count, unsigned varying_count,
                  const bool *flat_link, const uint32_t *varying_view)
{
   return m9_raster_fs_init_textured(r, vertex_entry, r88, const_count,
                                     varying_count, flat_link, varying_view,
                                     0, NULL, 0, NULL);
}

int
m9_raster_fs_init_textured(struct m9_raster_fs *r, unsigned vertex_entry,
                           uint32_t r88, unsigned const_count,
                           unsigned varying_count, const bool *flat_link,
                           const uint32_t *varying_view, unsigned coord_count,
                           const uint32_t *coord_view, unsigned texture_count,
                           const uint8_t *texture_coord)
{
   uint32_t stride[M9_RASTER_MAX_STREAMS] = { 0 };
   unsigned streams = varying_count + coord_count;

   if (!r || !m9_raster_fs_layout_ok(varying_count, coord_count,
                                     texture_count) ||
       const_count > 0x7f ||
       (coord_count && !coord_view) || (texture_count && !texture_coord))
      return -EINVAL;
   for (unsigned k = 0; k < texture_count; k++)
      if (texture_coord[k] != M9_RASTER_COORD_NONE &&
          texture_coord[k] >= coord_count)
         return -EINVAL;
   memset(r, 0, sizeof(*r));
   r->vertex_entry = vertex_entry;
   r->r88 = r88;
   r->const_count = const_count;
   /* Varying slot k is an xy/zw pair on stream k.  Attributes 5..8 are the
    * four texture coordinates, so interpolated inputs take 1..4 and then
    * 9..10: numbering the fifth k+1 would have claimed texture unit 0's
    * coordinate (MEASURED). */
   for (unsigned k = 0; k < varying_count; k++) {
      uint32_t link = flat_link[k] ? 0x80000000u : 0;
      uint32_t attr = k < 4 ? k + 1 : k + 5;

      r->decl[r->decl_count++] = link | attr << 20 | k << 16 | 0x0d88;
      r->decl[r->decl_count++] = link | attr << 20 | k << 16 | 0x06b6;
      r->stream_view[k] = varying_view[k];
      stride[k] = 0x10;
   }
   for (unsigned c = 0; c < coord_count; c++) {
      r->stream_view[varying_count + c] = coord_view[c];
      stride[varying_count + c] = 0x10;
   }
   /* Captured fs-tex2d/-two, pr-tex-mul-color: texture k reads its
    * coordinate stream as attribute 5+k, xyzw. */
   for (unsigned k = 0; k < texture_count; k++)
      if (texture_coord[k] != M9_RASTER_COORD_NONE)
         r->decl[r->decl_count++] = (5 + k) << 20 |
                                    (varying_count + texture_coord[k]) << 16 |
                                    0x0688;
   r->decl[r->decl_count++] = streams << 16 | 0x0688;
   r->decl[r->decl_count++] = 0x00c00000u | (streams + 1) << 16 | 0x31b6;
   r->decl[r->decl_count++] = 0x00d00000u | (streams + 2) << 16 | 0x0688;
   r->stream_view[streams] = 1;
   r->stream_view[streams + 1] = 2;
   r->stream_view[streams + 2] = 3;
   stride[streams] = 0x10;
   stride[streams + 1] = 0x06;
   stride[streams + 2] = 0x10;
   r->stream_count = streams + 3;
   for (unsigned i = 0; i < (M9_RASTER_MAX_STREAMS + 1) / 2; i++)
      r->stride[i] = stride[2 * i] |
                     (2 * i + 1 < M9_RASTER_MAX_STREAMS ? stride[2 * i + 1] : 0) << 16;
   return 0;
}

/* Descriptor format field 0x77 in [31:25] with layout 1, which every
 * captured record descriptor carries (CAPTURED, all seven two-pass cases and
 * the five-sampler schedule). */
#define M9_RECORD_FORMAT 0xee000000u

uint32_t
m9_record_desc(enum m9_record_form form, unsigned stride_dwords,
               unsigned offset_dwords)
{
   if (!stride_dwords || stride_dwords > M9_RECORD_MAX_DWORDS ||
       offset_dwords + 2 > stride_dwords)
      return 0;
   return M9_RECORD_FORMAT | (uint32_t)form << 19 | 0x00010000u |
          offset_dwords << 8 | stride_dwords;
}

uint32_t
m9_record_route(bool coordinate, unsigned c1, unsigned c2, unsigned c3)
{
   return (coordinate ? 0x10u : 0x0cu) | (c1 & 0xff) << 8 |
          (c2 & 0xff) << 16 | (c3 & 0xff) << 24;
}

/* One record a shaded fragment, indexed by the engine's own fragment counter
 * inside a raster range (MEASURED, docs/parhelia-m9-fragment-pass-hardware.md
 * section 3b: a 320x240 draw reached index 77431 and a 160x120 one 19495 on
 * the same target, so the count follows the fragments a pass shades and not
 * the target's area).  Fragments are shaded in whole 2x2 quads and a shared
 * primitive edge shades its quads twice, so round out to quads and carry two
 * quad rows and columns of edge overdraw. */
uint64_t
m9_record_bytes(uint32_t width, uint32_t height, unsigned stride_dwords)
{
   uint64_t w = align64(width, 2), h = align64(height, 2);

   if (!width || !height || !stride_dwords ||
       stride_dwords > M9_RECORD_MAX_DWORDS)
      return 0;
   return (w * h + 4 * (w + h) + 64) * stride_dwords * sizeof(uint32_t);
}

uint32_t
m9_raster_d7(enum m9_topology topology)
{
   return 0x200058a0u | topology;
}

uint32_t
m9_raster_class(bool lines, bool blend_dest)
{
   /* Blended lines are not captured; they keep the line class. */
   if (lines)
      return 0xc8008400u;
   return blend_dest ? 0xc5008200u : 0xc4008400u;
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
