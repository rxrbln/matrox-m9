/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Texture placement: the 32x16 packed tail (levels up to 16x16) is the
 * captured address permutation, and it covers a plane, a cube face and a
 * volume slice alike -- the face is an allocation stride and the slice is
 * an input of the address calculator.  Levels outside the tail report
 * -ENOTSUP and need the kernel texture window, which is plane-only
 * (docs/parhelia-m9-texture-storage.md).
 */
#include "m9_texture.h"

#include "m9_internal.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "pipe/p_defines.h"
#include "util/format/u_format.h"
#include "util/macros.h"

bool
m9_texture_format(enum pipe_format format, struct m9_texture_format *out)
{
   struct m9_texture_format info;

   /* Beyond 4x4 only 32-bit placement is captured: narrower formats are
    * expanded into the RGBA8 (c8) or alpha-less RGB8 (c9) layout. */
   switch (format) {
   case PIPE_FORMAT_R8G8B8A8_UNORM:
   case PIPE_FORMAT_B4G4R4A4_UNORM:
   case PIPE_FORMAT_B5G5R5A1_UNORM:
   case PIPE_FORMAT_A8_UNORM:
   case PIPE_FORMAT_L8A8_UNORM:
   case PIPE_FORMAT_I8_UNORM:
      info = (struct m9_texture_format){ PIPE_FORMAT_R8G8B8A8_UNORM, 0xc8 };
      break;
   case PIPE_FORMAT_R8G8B8X8_UNORM:
   case PIPE_FORMAT_B8G8R8X8_UNORM:
   case PIPE_FORMAT_B5G6R5_UNORM:
   case PIPE_FORMAT_L8_UNORM:
      info = (struct m9_texture_format){ PIPE_FORMAT_R8G8B8X8_UNORM, 0xc9 };
      break;
   case PIPE_FORMAT_B8G8R8A8_UNORM:
      info = (struct m9_texture_format){ PIPE_FORMAT_B8G8R8A8_UNORM, 0xcc };
      break;
   /* 10-bit GigaColor is storage class 2 at 32 bits per texel: the same
    * placement as the BGRA8 family, so the element is stored as it comes
    * (docs/parhelia-m9-gigacolor-10bit.md 3.2). */
   case PIPE_FORMAT_R10G10B10A2_UNORM:
      info = (struct m9_texture_format){ PIPE_FORMAT_R10G10B10A2_UNORM,
                                         M9_TEXTURE_CODE_A2B10G10R10 };
      break;
   case PIPE_FORMAT_B10G10R10A2_UNORM:
      info = (struct m9_texture_format){ PIPE_FORMAT_B10G10R10A2_UNORM,
                                         M9_TEXTURE_CODE_A2R10G10B10 };
      break;
   case PIPE_FORMAT_R10G10B10X2_UNORM:
      info = (struct m9_texture_format){ PIPE_FORMAT_R10G10B10X2_UNORM,
                                         M9_TEXTURE_CODE_A2B10G10R10 };
      break;
   case PIPE_FORMAT_B10G10R10X2_UNORM:
      info = (struct m9_texture_format){ PIPE_FORMAT_B10G10R10X2_UNORM,
                                         M9_TEXTURE_CODE_A2R10G10B10 };
      break;
   default:
      return false;
   }
   if (out)
      *out = info;
   return true;
}

bool
m9_texture_compressed(enum pipe_format format, uint8_t *code,
                      uint8_t *block_bytes)
{
   uint8_t c, b;

   switch (format) {
   case PIPE_FORMAT_DXT1_RGB:
   case PIPE_FORMAT_DXT1_RGBA:
      c = M9_TEXTURE_CODE_DXT1;
      b = 8;
      break;
   case PIPE_FORMAT_DXT3_RGBA:
      c = M9_TEXTURE_CODE_DXT3;
      b = 16;
      break;
   case PIPE_FORMAT_DXT5_RGBA:
      c = M9_TEXTURE_CODE_DXT5;
      b = 16;
      break;
   default:
      return false;
   }
   if (code)
      *code = c;
   if (block_bytes)
      *block_bytes = b;
   return true;
}

uint64_t
m9_texture_compressed_bytes(uint32_t width, uint32_t height,
                            uint8_t block_bytes)
{
   /* Section 4.1's sel = 1 granularity rows, which the measurement of
    * section 12.2 selects: width to 32 texels, height to 64 for the 4-bit
    * family and 32 for the 8-bit one, then a 4 KiB object quantum. */
   uint64_t pad_w = (width + 31) & ~31u;
   uint64_t pad_h = block_bytes == 8 ? ((height + 63) & ~63u) :
                                       ((height + 31) & ~31u);
   uint64_t bytes = pad_w * pad_h * block_bytes / 16;

   return (bytes + 0xfff) & ~0xfffull;
}

int
m9_texture_layout_compressed(struct m9_texture_layout *layout, uint32_t width,
                             uint32_t height, unsigned levels,
                             uint8_t block_bytes)
{
   uint64_t offset = 0;

   if (!layout || !width || !height || !levels ||
       levels > M9_TEXTURE_MAX_LEVELS || width > 2048 || height > 2048 ||
       (block_bytes != 8 && block_bytes != 16))
      return -EINVAL;
   memset(layout, 0, sizeof(*layout));
   layout->kind = M9_TEXTURE_KIND_2D;
   layout->levels = levels;
   layout->faces = 1;
   layout->block_bytes = block_bytes;
   for (unsigned l = 0; l < levels; l++) {
      struct m9_texture_level *lvl = &layout->level[l];

      /* A level below one block still occupies a whole block. */
      lvl->width = MAX2(width >> l, 4);
      lvl->height = MAX2(height >> l, 4);
      lvl->depth = 1;
      lvl->face_bytes = m9_texture_compressed_bytes(lvl->width, lvl->height,
                                                    block_bytes);
      lvl->offset = offset;
      offset += lvl->face_bytes;
   }
   layout->size = offset;
   return 0;
}

/* Vendor m9x659 (mode-2 aperture) with the calculator state captured for
 * the tail: depth 1, category 0, bank masks 0. */
static uint64_t
m9_tail_address(unsigned x, unsigned y, unsigned z)
{
   uint64_t a = (x & 7) << 2 | (x & 8) << 4 | (y & 3) << 5;

   a |= (((x >> 4) ^ (y >> 2)) & 1) << 8;
   a |= (((y >> 2) ^ (y >> 4) ^ (x >> 5) ^ z ^ (y >> 3)) & 1) << 9;
   return a + ((uint64_t)z + (y >> 3)) * 0x400;
}

/* The tail levels of every face share one atlas and a volume's slice pages
 * are indexed by the same address bits as rows 8 and up, so whether a chain
 * fits is a property of its extents, not of its target.  Walk the tail and
 * keep only the levels whose texels are all distinct inside the allocation:
 * a level that would overwrite another is left unplaced, and every later
 * level with it, so that nothing is ever written to a wrong address. */
static int
m9_texture_tail_verify(struct m9_texture_layout *layout, int tail)
{
   const struct m9_texture_level *first;
   uint64_t bytes;
   uint8_t *seen;

   if (tail < 0)
      return 0;
   first = &layout->level[tail];
   bytes = first->face_bytes;
   seen = calloc((size_t)(bytes / 4), 1);
   if (!seen)
      return -ENOMEM;
   for (unsigned l = tail; l < layout->levels; l++) {
      const struct m9_texture_level *lvl = &layout->level[l];
      bool ok = true;

      for (unsigned z = 0; ok && z < lvl->depth; z++)
         for (unsigned y = 0; ok && y < lvl->height; y++)
            for (unsigned x = 0; ok && x < lvl->width; x++) {
               uint64_t a = m9_tail_address(lvl->tail_x + x, lvl->tail_y + y,
                                            z);

               if (a + 4 > bytes || seen[a / 4])
                  ok = false;
               else
                  seen[a / 4] = 1;
            }
      if (!ok)
         break;
      layout->placed |= 1u << l;
   }
   free(seen);
   return 0;
}

int
m9_texture_layout_init(struct m9_texture_layout *layout,
                       enum m9_texture_kind kind, uint32_t width,
                       uint32_t height, uint32_t depth, unsigned levels)
{
   static const uint8_t tail_x[5] = { 0, 16, 24, 16, 24 };
   static const uint8_t tail_y[5] = { 0, 0, 0, 8, 8 };
   uint32_t max_dim = MAX3(width, height, depth);
   unsigned chain = 1;
   uint64_t offset = 0;
   int tail = -1;

   if (!layout || !width || !height || !depth || !levels ||
       levels > M9_TEXTURE_MAX_LEVELS || width > 2048 || height > 2048 ||
       depth > 2048 ||
       (kind != M9_TEXTURE_KIND_2D && kind != M9_TEXTURE_KIND_CUBE &&
        kind != M9_TEXTURE_KIND_3D && kind != M9_TEXTURE_KIND_1D) ||
       (kind != M9_TEXTURE_KIND_3D && depth != 1) ||
       (kind == M9_TEXTURE_KIND_CUBE && width != height) ||
       (kind == M9_TEXTURE_KIND_1D && (height != 1 || levels != 1)))
      return -EINVAL;
   while (max_dim >>= 1)
      chain++;
   if (levels > chain)
      return -EINVAL;
   memset(layout, 0, sizeof(*layout));
   layout->kind = kind;
   layout->levels = levels;
   layout->faces = kind == M9_TEXTURE_KIND_CUBE ? 6 : 1;
   for (unsigned l = 0; l < levels; l++) {
      struct m9_texture_level *lvl = &layout->level[l];

      lvl->width = MAX2(width >> l, 1);
      lvl->height = MAX2(height >> l, 1);
      lvl->depth = kind == M9_TEXTURE_KIND_3D ? MAX2(depth >> l, 1) : 1;
      if (kind == M9_TEXTURE_KIND_1D) {
         lvl->face_bytes = ((uint64_t)lvl->width * 4 + 255) & ~255ull;
         lvl->offset = offset;
         offset += lvl->face_bytes;
      } else if (lvl->width <= M9_TEXTURE_TAIL_EXTENT &&
                 lvl->height <= M9_TEXTURE_TAIL_EXTENT) {
         unsigned rel;

         if (tail < 0) {
            tail = l;
            /* Every 3-D slice advances the calculator by one 1 KiB page. */
            lvl->face_bytes = M9_TEXTURE_TAIL_BYTES +
                              ((uint64_t)lvl->depth - 1) * 0x400;
            lvl->offset = offset;
            offset += layout->faces * lvl->face_bytes;
         } else {
            lvl->face_bytes = layout->level[tail].face_bytes;
            lvl->offset = layout->level[tail].offset;
         }
         rel = l - tail;
         lvl->tail_x = tail_x[rel];
         lvl->tail_y = tail_y[rel];
         lvl->in_tail = true;
      } else {
         struct m9_render_layout render;

         /* Vendor allocation contract (STATIC); the placement is not captured. */
         uint64_t quantum;

         if (!m9_render_surface_layout(lvl->width, lvl->height, 4, &render))
            return -EINVAL;
         /* A level inside a chain is packed by its own extent.  The spare
          * elements a standalone surface reserves push the next level past
          * where the sampler looks for it, and from there the chain reads
          * nothing: a 256x64 texture came out black from level 3 down. */
         quantum = render.small ? 0x400 : 0x1000;
         lvl->face_bytes = (((uint64_t)render.row_span *
                             render.allocation_height * 4) + quantum - 1) &
                           ~(quantum - 1);
         lvl->face_bytes *= lvl->depth;
         lvl->offset = offset;
         offset += layout->faces * lvl->face_bytes;
      }
   }
   layout->size = offset;
   if (kind == M9_TEXTURE_KIND_1D)
      layout->placed = (2u << (levels - 1)) - 1;
   else if (m9_texture_tail_verify(layout, tail))
      return -ENOMEM;
   return 0;
}

enum m9_texture_kind
m9_texture_kind_for_target(enum pipe_texture_target target)
{
   switch (target) {
   case PIPE_TEXTURE_1D: return M9_TEXTURE_KIND_1D;
   case PIPE_TEXTURE_2D:
   case PIPE_TEXTURE_RECT: return M9_TEXTURE_KIND_2D;
   case PIPE_TEXTURE_CUBE: return M9_TEXTURE_KIND_CUBE;
   case PIPE_TEXTURE_3D: return M9_TEXTURE_KIND_3D;
   default: return 0;
   }
}

/* MEASURED (tools/m9-mesa-glsl volume:16xHxD on the M9128, 16x16 covering
 * every smaller extent because a texel's address does not depend on the
 * level's extent): a volume slice lands where this model puts it for
 *
 *   height <= 4   at every depth up to 8,
 *   height <= 8   at depth 2, 3, 5, 6 and 7 -- but not at 4 or 8,
 *   height <= 16  only at depth 1, where the volume is a plane.
 *
 * Everything else, and every depth above 8, uses an address calculator
 * state the captures do not cover: 16x8x4 read half its rows from the
 * slice two pages away, and 16x16x2 read none of its texels correctly. */
static bool
m9_volume_agrees(uint32_t height, uint32_t depth)
{
   if (depth > 8)
      return false;
   if (depth == 1)
      return height <= M9_TEXTURE_TAIL_EXTENT;
   if (height <= 4)
      return true;
   return height <= 8 && depth != 4 && depth != 8;
}

bool
m9_texture_layout_complete(const struct m9_texture_layout *layout)
{
   if (!layout || !layout->levels ||
       layout->placed != (2u << (layout->levels - 1)) - 1)
      return false;
   /* Only the base level is sampled -- a tail chain clamps the sampler's
    * last level to the base -- so the hardware has to agree with this model
    * there.  The levels above it are written to addresses this layout
    * proved disjoint from the base, and nothing reads them. */
   return layout->kind != M9_TEXTURE_KIND_3D ||
          m9_volume_agrees(layout->level[0].height, layout->level[0].depth);
}

enum m9_texture_kind
m9_texture_bind_kind(const struct m9_texture_layout *layout, uint32_t width,
                     uint32_t height)
{
   if (layout && (layout->kind == M9_TEXTURE_KIND_CUBE ||
                  layout->kind == M9_TEXTURE_KIND_3D ||
                  layout->kind == M9_TEXTURE_KIND_1D))
      return layout->kind;
   /* Reading 128x128 and larger textures as the small kind returned a single
    * constant texel, so the storage kind must be the one they were written
    * through (MEASURED). */
   return width <= 64 && height <= 64 ? M9_TEXTURE_KIND_2D :
                                        M9_TEXTURE_KIND_RENDER;
}

bool
m9_texture_level_placed(const struct m9_texture_layout *layout, unsigned level)
{
   return layout && level < layout->levels &&
          (layout->placed & (1u << level));
}

int
m9_texel_offset(const struct m9_texture_layout *layout, unsigned level,
                unsigned face, unsigned x, unsigned y, unsigned z,
                uint64_t *offset)
{
   const struct m9_texture_level *lvl;

   if (!layout || !offset || level >= layout->levels)
      return -EINVAL;
   lvl = &layout->level[level];
   if (face >= layout->faces || x >= lvl->width || y >= lvl->height ||
       z >= lvl->depth)
      return -EINVAL;
   if (!m9_texture_level_placed(layout, level))
      return -ENOTSUP;
   if (layout->kind == M9_TEXTURE_KIND_1D)
      *offset = lvl->offset + 4ull * x;
   else
      *offset = lvl->offset + face * lvl->face_bytes +
                m9_tail_address(lvl->tail_x + x, lvl->tail_y + y, z);
   return 0;
}

/* Gallium addresses a cube face as the box's z layer, so on a cube the layer
 * selects the face and every face is one slice deep. */
static void
m9_texture_layer(const struct m9_texture_layout *layout, unsigned face,
                 const struct pipe_box *box, int dz, unsigned *out_face,
                 unsigned *out_z)
{
   bool cube = layout->kind == M9_TEXTURE_KIND_CUBE;

   *out_face = cube ? face + box->z + dz : face;
   *out_z = cube ? 0 : box->z + dz;
}

static int
m9_texture_box_check(const struct m9_texture_layout *layout, unsigned level,
                     unsigned face, const struct pipe_box *box,
                     uint64_t map_size)
{
   unsigned f, z;
   uint64_t first, last;
   int ret;

   if (!box || box->x < 0 || box->y < 0 || box->z < 0 || box->width <= 0 ||
       box->height <= 0 || box->depth <= 0)
      return -EINVAL;
   m9_texture_layer(layout, face, box, 0, &f, &z);
   ret = m9_texel_offset(layout, level, f, box->x, box->y, z, &first);
   if (!ret) {
      m9_texture_layer(layout, face, box, box->depth - 1, &f, &z);
      ret = m9_texel_offset(layout, level, f, box->x + box->width - 1,
                            box->y + box->height - 1, z, &last);
   }
   if (ret)
      return ret;
   /* The tail and 3-D pages end with the level allocation. */
   return layout->level[level].offset +
          (uint64_t)layout->faces * layout->level[level].face_bytes >
          map_size ? -EINVAL : 0;
}

int
m9_texture_store(const struct m9_texture_layout *layout, uint8_t *map,
                 uint64_t map_size, enum pipe_format storage,
                 unsigned level, unsigned face, const struct pipe_box *box,
                 enum pipe_format src_format, const uint8_t *src,
                 unsigned src_stride)
{
   uint8_t *row;
   int ret = map && src ? m9_texture_box_check(layout, level, face, box,
                                                map_size) : -EINVAL;

   if (ret)
      return ret;
   row = malloc((size_t)box->width * 4);
   if (!row)
      return -ENOMEM;
   for (int dz = 0; !ret && dz < box->depth; dz++) {
      for (int dy = 0; !ret && dy < box->height; dy++) {
         const uint8_t *line = src + ((size_t)dz * box->height + dy) *
                                     src_stride;

         if (!util_format_translate(storage, row, box->width * 4, 0, 0,
                                    src_format, line, src_stride, 0, 0,
                                    box->width, 1)) {
            ret = -EINVAL;
            break;
         }
         for (int dx = 0; dx < box->width; dx++) {
            unsigned f, z;
            uint64_t off;

            m9_texture_layer(layout, face, box, dz, &f, &z);
            ret = m9_texel_offset(layout, level, f, box->x + dx,
                                  box->y + dy, z, &off);
            if (ret)
               break;
            memcpy(map + off, row + 4 * dx, 4);
         }
      }
   }
   free(row);
   return ret;
}

int
m9_texture_fetch(const struct m9_texture_layout *layout,
                 const uint8_t *map, uint64_t map_size,
                 enum pipe_format storage, unsigned level, unsigned face,
                 const struct pipe_box *box, enum pipe_format dst_format,
                 uint8_t *dst, unsigned dst_stride)
{
   uint8_t *row;
   int ret = map && dst ? m9_texture_box_check(layout, level, face, box,
                                                map_size) : -EINVAL;

   if (ret)
      return ret;
   row = malloc((size_t)box->width * 4);
   if (!row)
      return -ENOMEM;
   for (int dz = 0; !ret && dz < box->depth; dz++) {
      for (int dy = 0; !ret && dy < box->height; dy++) {
         for (int dx = 0; dx < box->width; dx++) {
            unsigned f, z;
            uint64_t off;

            m9_texture_layer(layout, face, box, dz, &f, &z);
            ret = m9_texel_offset(layout, level, f, box->x + dx,
                                  box->y + dy, z, &off);
            if (ret)
               break;
            memcpy(row + 4 * dx, map + off, 4);
         }
         if (!ret && !util_format_translate(dst_format,
                                            dst + ((size_t)dz * box->height +
                                                   dy) * dst_stride,
                                            dst_stride, 0, 0, storage, row,
                                            box->width * 4, 0, 0,
                                            box->width, 1))
            ret = -EINVAL;
      }
   }
   free(row);
   return ret;
}

static unsigned
m9_texture_wrap(unsigned wrap, bool *ok)
{
   switch (wrap) {
   case PIPE_TEX_WRAP_REPEAT: return 0;
   case PIPE_TEX_WRAP_CLAMP_TO_EDGE: return 1;
   case PIPE_TEX_WRAP_MIRROR_REPEAT: return 4;
   case PIPE_TEX_WRAP_CLAMP: return 6;
   case PIPE_TEX_WRAP_CLAMP_TO_BORDER: return 7;
   default:
      *ok = false;
      return 0;
   }
}

/* The four border fields carry the channels in the descriptor format's own
 * order, because the vendor converts the border through the same resource
 * format join as a texel (docs/parhelia-m9-texture-state.md, 0x187b50 and
 * 0x18be15).  CAPTURED: the c8 (RGBA8) border case of
 * captures/m9x-linux-texture-resources-20260914 requested (0.125, 0.25, 0.75,
 * 1) and emitted 7e=40002000 7f=ff00bf00, whose 7e[15:8] is the requested
 * red, and its preserved frame renders (32, 64, 191) -- so for an
 * RGBA-ordered code red belongs in 7e[15:8].  MEASURED on the M9128: a cc
 * (BGRA8) surface with the same encoding returns red and blue exchanged,
 * which is that same field read as blue.
 *
 * out[k] is the pipe channel the hardware reads from field k: 7e[15:8],
 * 7e[31:24], 7f[15:8], 7f[31:24]. */
static void
m9_texture_border_order(uint8_t code, uint8_t out[4])
{
   static const uint8_t rgba[4] = { 0, 1, 2, 3 };
   static const uint8_t bgra[4] = { 2, 1, 0, 3 };

   /* A2R10G10B10 carries blue at bit 0, the same channel order as the
    * BGRA8 code; A2B10G10R10 carries red there, like the RGBA8 one. */
   memcpy(out, code == M9_TEXTURE_CODE_BGRA ||
               code == M9_TEXTURE_CODE_A2R10G10B10 ? bgra : rgba, 4);
}

static unsigned
m9_texture_lod(float lod)
{
   /* 7d LOD bounds: 9 bits with 5 fraction bits. */
   return (unsigned)lroundf(CLAMP(lod, 0.0f, 511.0f / 32.0f) * 32.0f);
}

int
m9_texture_unit_init(struct m9_texture_unit *unit, uint32_t handle,
                     const struct m9_texture_bind *bind,
                     const struct pipe_sampler_state *s)
{
   static const uint8_t compare[8] = {
      [PIPE_FUNC_NEVER] = 0, [PIPE_FUNC_LESS] = 1, [PIPE_FUNC_LEQUAL] = 2,
      [PIPE_FUNC_EQUAL] = 3, [PIPE_FUNC_NOTEQUAL] = 4, [PIPE_FUNC_GEQUAL] = 5,
      [PIPE_FUNC_GREATER] = 6, [PIPE_FUNC_ALWAYS] = 7,
   };
   bool ok = true, mip, linear_min, shadow;
   unsigned form, lo = 0, hi = 511, border[4];
   uint8_t order[4];
   uint32_t filter;
   long bias;

   if (!unit || !handle || !bind || !s || !bind->width ||
       bind->width > 0xffff || !bind->height || bind->height > 0xffff ||
       !bind->depth || bind->depth > 0xfff || bind->last_level > 15 ||
       bind->view_last_level > bind->last_level ||
       bind->view_first_level > bind->view_last_level)
      return -EINVAL;
   /* A view based above level 0 keeps the chain resident and pins the LOD
    * range to it: the descriptor addresses every level from one base, so a
    * re-based view is a clamp rather than a second allocation. */
   mip = s->min_mip_filter != PIPE_TEX_MIPFILTER_NONE || bind->view_first_level;
   linear_min = s->min_img_filter == PIPE_TEX_FILTER_LINEAR;
   filter = (linear_min ? 0x2 : 0) | 0x200 |
            (s->mag_img_filter == PIPE_TEX_FILTER_LINEAR ? 0x10 : 0);
   if (mip) {
      filter |= s->min_mip_filter == PIPE_TEX_MIPFILTER_LINEAR ? 0x100 : 0x80;
      /* 7c[10] is captured only for NEAREST_MIPMAP_NEAREST, LINEAR mag. */
      if (s->mag_img_filter == PIPE_TEX_FILTER_LINEAR && !linear_min &&
          s->min_mip_filter == PIPE_TEX_MIPFILTER_NEAREST)
         filter |= 0x400;
      lo = MAX2(m9_texture_lod(s->min_lod), bind->view_first_level * 32);
      if (bind->view_last_level < bind->last_level)
         hi = MIN2(hi, bind->view_last_level * 32);
      if (s->max_lod < (float)bind->last_level)
         hi = MIN2(hi, m9_texture_lod(s->max_lod));
   }
   bias = CLAMP(lroundf(s->lod_bias * 32.0f), -512, 511);
   shadow = s->compare_mode == PIPE_TEX_COMPARE_R_TO_TEXTURE;
   form = bind->kind == M9_TEXTURE_KIND_CUBE ? 3 :
          bind->kind == M9_TEXTURE_KIND_3D || shadow ? 2 : 1;

   unit->handle = handle;
   unit->w7d = ((uint32_t)bias & 0x3ff) | lo << 10 | hi << 19;
   unit->w70 = 0x00040018u | form << 8 |
               (unit->w7d != M9_TEXTURE_LOD_DEFAULT ? 1u << 19 : 0) |
               (shadow ? 1u << 26 | (uint32_t)compare[s->compare_func & 7] << 27 : 0);
   unit->w79 = (uint32_t)bind->code << 24 | (uint32_t)bind->kind << 16 |
               bind->width;
   unit->w7a = (mip ? bind->last_level : 0) << 28 | bind->depth << 16 |
               bind->height;
   unit->w7c = filter | 0x9000u |
               m9_texture_wrap(s->wrap_s, &ok) << 22 |
               m9_texture_wrap(s->wrap_t, &ok) << 25 |
               m9_texture_wrap(s->wrap_r, &ok) << 28;
   /* Raw DEPTH16 sampling. */
   if (bind->code == 0xb8 && !shadow)
      unit->w7c |= 1u << 31;
   for (unsigned c = 0; c < 4; c++)
      border[c] = (unsigned)lroundf(CLAMP(s->border_color.f[c], 0.0f, 1.0f) *
                                    255.0f);
   m9_texture_border_order(bind->code, order);
   unit->w7e = border[order[1]] << 24 | border[order[0]] << 8;
   unit->w7f = border[order[3]] << 24 | border[order[2]] << 8;
   unit->w80 = 0xffffffffu;
   unit->w81 = 0;
   return ok ? 0 : -EINVAL;
}

/* Captured fs-uniform/fs-color raster unit 0 (m9x-vendor-glsl-programs). */
void
m9_texture_unit_placeholder(struct m9_texture_unit *unit, uint32_t handle)
{
   *unit = (struct m9_texture_unit) {
      .handle = handle,
      .w70 = 0x00040118u,
      .w79 = 0xcc000000u | (uint32_t)M9_TEXTURE_KIND_2D << 16 | 1,
      .w7a = 0x00010001u,
      .w7c = 0x00000200u,
      .w7d = M9_TEXTURE_LOD_DEFAULT,
      .w80 = 0xffffffffu,
      .placeholder = true,
   };
}
