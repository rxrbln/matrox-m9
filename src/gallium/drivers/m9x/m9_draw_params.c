/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Every render-size, vertex-layout and raster-state dependent word of the
 * two-pass draw.  The encodings are hardware-measured on the M9148
 * (parhelia-m9-cpu-tnl-raster-state.md, parhelia-m9-cpu-tnl-draw-parameters.md).
 */
#include "m9_draw_params.h"
#include "m9_vs_record.h"
#include "m9_batch.h"
#include "m9_passthrough.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#include "util/u_debug.h"
#include "util/u_math.h"
#include "pipe/p_defines.h"

#define M9_GUARD_BAND 15359.0f

/* M9X_GUARD=N computes both guard bands from a half-extent of N instead of
 * the target's own, which separates the guard band from the rest of the
 * per-shape viewport state.  The geometry stays inside the viewport, so the
 * image a draw produces is unchanged. */
static float
m9_guard(float half_extent)
{
   long fixed = debug_get_num_option("M9X_GUARD", 0);

   if (fixed > 0)
      half_extent = (float)fixed;
   return half_extent > 0.0f ? M9_GUARD_BAND / half_extent - 1.0f : 0.0f;
}

static void
m9_put_float(uint32_t *words, unsigned index, float value)
{
   memcpy(&words[index], &value, sizeof(value));
}

/* A 10-bit colour target is the same 32-bit class 2 surface as the 8-bit
 * one, and takes render class 0 at index 89 like any packed colour target
 * (docs/parhelia-m9-gigacolor-10bit.md 3.2, 4.1).  Every colour attachment
 * of a multiple-target framebuffer takes its b1 descriptor from here. */
int
m9_color_surface_desc(uint32_t width, uint32_t height, uint8_t color_format,
                      uint32_t *desc_out)
{
   struct m9_surface_desc desc = {
      .height = height, .layers = 1, .layout = 1,
      .format = color_format,
   };
   struct m9_render_layout layout;

   if (!desc_out || (color_format != M9_COLOR_FORMAT_A8R8G8B8 &&
                     color_format != M9_COLOR_FORMAT_A2B10G10R10 &&
                     color_format != M9_COLOR_FORMAT_A2R10G10B10) ||
       !m9_render_surface_layout(width, height, 4, &layout))
      return -EINVAL;
   desc.row_span = layout.row_span;
   /* Measured: a surface of 64x64 or less is stored by the aperture
    * calculator, which is what its allocation size and the sampler both
    * assume.  Rendering it with the ordinary tiled bit set wrote rows
    * through the other permutation, and sampling the result returned a
    * shuffled image (rtt64, rtt32). */
   desc.address_offset = !layout.small;
   *desc_out = m9_surface_desc0(&desc);
   return 0;
}

/*
 * The depth unit tests the last partial 64-column group of a surface against
 * wrong values when the row span is only 32-aligned: in piglit's hiz scene at
 * 400, 416 and 352 wide a farther triangle won there, and a 64-aligned span
 * removes every such pixel (MEASURED).  Small surfaces need it as well:
 * stencil written into 12..30-wide surfaces taller than 32 rows landed in
 * other cells at a 32 span and is exact at 64 (MEASURED, zprobe).  The vendor
 * aligns its depth layouts the same way
 * (parhelia-m9-hierarchical-z-static-closure.md, +0x16c).
 */
uint32_t
m9_depth_row_span(uint32_t width, uint32_t height)
{
   (void)height;
   return (width + 63) & ~63u;
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
   uint32_t color_desc;

   if (!params || m9_color_surface_desc(width, height, color_format,
                                        &color_desc) ||
       !m9_render_surface_layout(width, height, 4, &layout))
      return -EINVAL;
   memset(params, 0, sizeof(*params));
   params->width = width;
   params->height = height;
   desc.row_span = layout.row_span;
   desc.address_offset = !layout.small;
   params->color_desc = color_desc;
   desc.format = M9_DEPTH_FORMAT_D24S8;
   desc.row_span = m9_depth_row_span(width, height);
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
   m9_put_float(sys, M9_VS_GUARD_X_DWORD, m9_guard(w / 2));
   m9_put_float(sys, M9_VS_GUARD_Y_DWORD, -m9_guard(h / 2));

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
   /* One word per 8x4 cell of the depth surface's tiled grid. */
   uint64_t cells = (uint64_t)m9_depth_row_span(width, height) / 8 *
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

/* Function code, then sfail, zfail and zpass codes, three bits each. */
static uint32_t
m9_stencil_quartet(const struct m9_stencil_face *f)
{
   static const uint8_t op[8] = {
      [PIPE_STENCIL_OP_KEEP] = 0, [PIPE_STENCIL_OP_REPLACE] = 1,
      [PIPE_STENCIL_OP_INVERT] = 2, [PIPE_STENCIL_OP_ZERO] = 3,
      [PIPE_STENCIL_OP_INCR_WRAP] = 4, [PIPE_STENCIL_OP_DECR_WRAP] = 5,
      [PIPE_STENCIL_OP_INCR] = 6, [PIPE_STENCIL_OP_DECR] = 7,
   };

   return (m9_depth_control(true, false, f->func) >> 2 & 7) |
          (uint32_t)op[f->fail_op & 7] << 3 |
          (uint32_t)op[f->zfail_op & 7] << 6 |
          (uint32_t)op[f->zpass_op & 7] << 9;
}

static uint32_t
m9_stencil_triplet(const struct m9_stencil_face *f)
{
   return (uint32_t)f->writemask << 16 | (uint32_t)f->valuemask << 8 | f->ref;
}

/* a8 bit 5 enables the test, bit 6 the second face; the quartet at bit 7
 * and a9 serve clockwise faces, the one at bit 19 and aa counter-clockwise
 * faces, in the winding m9_cull_ef culls by (CAPTURED, vendor GL
 * fixed-state cases 29..31, 45..59, 85/86 and the gl15 two-sided cases). */
void
m9_stencil_words(uint32_t depth_a8, const struct m9_stencil_face *front,
                 const struct m9_stencil_face *back, bool front_ccw,
                 uint32_t *a8, uint32_t *a9, uint32_t *aa)
{
   const struct m9_stencil_face *cw = front, *ccw = NULL;

   *a8 = depth_a8;
   *a9 = M9_STENCIL_A9_OFF;
   *aa = M9_STENCIL_AA_OFF;
   if (!front)
      return;
   if (back) {
      cw = front_ccw ? back : front;
      ccw = front_ccw ? front : back;
   }
   /* One-sided keeps the second quartet at ALWAYS/KEEP, as the vendor. */
   *a8 = (depth_a8 & 0x1fu) | 0x20u | (ccw ? 0x40u : 0) |
         m9_stencil_quartet(cw) << 7 |
         (ccw ? m9_stencil_quartet(ccw) : 7u) << 19;
   *a9 = 0x08000000u | m9_stencil_triplet(cw);
   if (ccw)
      *aa = m9_stencil_triplet(ccw);
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
   m9_put_float(prefix, M9_VS_GUARD_X_DWORD, m9_guard(ax));
   m9_put_float(prefix, M9_VS_GUARD_Y_DWORD, -m9_guard(ay));
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
                                     0, NULL, NULL, 0, NULL, true);
}

int
m9_raster_fs_init_textured(struct m9_raster_fs *r, unsigned vertex_entry,
                           uint32_t r88, unsigned const_count,
                           unsigned varying_count, const bool *flat_link,
                           const uint32_t *varying_view, unsigned coord_count,
                           const uint32_t *coord_view,
                           const uint32_t *coord_select,
                           unsigned texture_count,
                           const uint8_t *texture_coord, bool clip_position)
{
   uint32_t stride[M9_RASTER_MAX_STREAMS] = { 0 };
   unsigned streams = varying_count + coord_count;

   if (!r || !m9_raster_fs_layout_ok(varying_count, coord_count,
                                     texture_count) ||
       const_count > M9_RASTER_MAX_CONSTS ||
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
   /* Varying slot k reads stream k as one xyzw word, the form attributes 0,
    * 5, 6 and 13 declare with; the captured xy/zw pair renders the same
    * pixels and costs a second word (MEASURED).  Attributes 5..8 are the
    * four texture coordinates, so interpolated inputs take 1..4 and then
    * 9..10: numbering the fifth k+1 would have claimed texture unit 0's
    * coordinate (MEASURED). */
   for (unsigned k = 0; k < varying_count; k++) {
      uint32_t link = flat_link[k] ? 0x80000000u : 0;
      uint32_t attr = k < 4 ? k + 1 : k + 5;

      r->decl[r->decl_count++] = link | attr << 20 | k << 16 | 0x0688;
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
      if (texture_coord[k] != M9_RASTER_COORD_NONE) {
         uint32_t select = coord_select ? coord_select[texture_coord[k]] : 0;

         r->decl[r->decl_count++] = (5 + k) << 20 |
                                    (varying_count + texture_coord[k]) << 16 |
                                    (select ? select : 0x0688u);
      }
   /* Attribute 0 is the rasterizer position, 12 the clip code and 13 the
    * clip position; 556 of 556 captured raster passes declare all three.
    * The clip position is the one the pass can do without -- the same
    * pixels come out with its declaration dropped (MEASURED) -- which is
    * what frees the preprocessing pass's output target 1 for a varying. */
   r->decl[r->decl_count++] = streams << 16 | 0x0688;
   r->stream_view[streams] = 1;
   stride[streams] = 0x10;
   r->decl[r->decl_count++] = 0x00c00000u | (streams + 1) << 16 | 0x31b6;
   r->stream_view[streams + 1] = 2;
   stride[streams + 1] = 0x06;
   streams++;
   if (clip_position) {
      r->decl[r->decl_count++] = 0x00d00000u | (streams + 1) << 16 | 0x0688;
      r->stream_view[streams + 1] = 3;
      stride[streams + 1] = 0x10;
      streams++;
   }
   r->stream_count = streams + 1;
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
m9_record_route(bool coordinate, unsigned index, unsigned c1, unsigned c2,
                unsigned c3)
{
   /* A coordinate route names the slot it feeds in its own low byte:
    * 0x10 at 0x74, 0x14 at 0x75, 0x18 at 0x76 (CAPTURED, 84/12/2 occurrences
    * with no counter-example).  A value route is 0x0c at every index. */
   return (coordinate ? 0x10u + 4 * index : 0x0cu) | (c1 & 0xff) << 8 |
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

/* How the second preprocessing draw hands its varyings on.  0 is the shape
 * the first implementation bound, one separate stream view a varying, which
 * hardware refused; 1 keeps those views and only makes the descriptor a
 * record, which it also refused; 2 is the captured chain -- one packed
 * record the head writes and a route-fed draw de-interleaves into the views
 * the raster stage reads (docs/parhelia-m9-varying-transport-re.md). */
static unsigned m9_vs_record = 2;

void
m9_draw_set_vs_record(unsigned mode)
{
   m9_vs_record = mode;
}

unsigned
m9_vs_record_mode(void)
{
   return m9_vs_record;
}

uint32_t
m9_vs_extra_desc(unsigned target, unsigned targets)
{
   if (!m9_vs_record)
      return 0x68002000u;
   if (m9_vs_record == 1)
      return m9_record_desc(M9_RECORD_SLOT, 4, 0);
   return m9_record_desc(M9_RECORD_SLOT, 4 * targets, 4 * target);
}

uint32_t
m9_vs_gather_route(unsigned target)
{
   /* Component 0 comes from the descriptor's own offset; 1..3 are the
    * dwords behind it. */
   return m9_record_route(false, target, 4 * target + 1, 4 * target + 2,
                          4 * target + 3);
}

static uint32_t m9_job_vertices = M9_JOB_VERTICES_DEFAULT;

void
m9_draw_set_job_vertices(uint32_t vertices)
{
   m9_job_vertices = CLAMP(vertices, M9_JOB_VERTICES_MIN,
                           M9_JOB_VERTICES_MAX) & ~3u;
}

uint32_t
m9_draw_max_source_vertices(void)
{
   return m9_job_vertices;
}

uint32_t
m9_transient_view_bytes(void)
{
   /* Each view starts and ends on a whole packed-record bank quantum.
    * The ordinary default (23040 vertices) already meets this alignment. */
   uint32_t bytes = m9_job_vertices * M9_TRANSIENT_VERTEX_BYTES;
   return (bytes + M9_VS_RECORD_BANK_BYTES - 1u) &
          ~(M9_VS_RECORD_BANK_BYTES - 1u);
}

uint32_t
m9_cpu_stream_view_bytes(uint32_t records, uint32_t copies)
{
   if (!records || records % 4 || !copies ||
       copies > m9_draw_max_source_vertices() / records)
      return 0;
   uint32_t bytes = records * copies * M9_TRANSIENT_VERTEX_BYTES;
   return (bytes + M9_VS_RECORD_BANK_BYTES - 1u) &
          ~(M9_VS_RECORD_BANK_BYTES - 1u);
}
