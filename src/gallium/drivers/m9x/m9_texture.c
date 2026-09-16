/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Texture placement: only the 32x16 packed tail (levels up to 16x16) has a
 * captured address permutation; other levels report -ENOTSUP and need the
 * kernel texture window (docs/parhelia-m9-texture-storage.md).
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
   default:
      return false;
   }
   if (out)
      *out = info;
   return true;
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
                 lvl->height <= M9_TEXTURE_TAIL_EXTENT && lvl->depth <= 15) {
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
         quantum = lvl->width <= 64 && lvl->height <= 64 ? 0x400 : 0x1000;
         lvl->face_bytes = (((uint64_t)render.row_span *
                             render.allocation_height * 4) + quantum - 1) &
                           ~(quantum - 1);
         lvl->face_bytes *= lvl->depth;
         lvl->offset = offset;
         offset += layout->faces * lvl->face_bytes;
      }
   }
   layout->size = offset;
   return 0;
}

bool
m9_texture_level_placed(const struct m9_texture_layout *layout, unsigned level)
{
   const struct m9_texture_level *lvl;

   if (!layout || level >= layout->levels)
      return false;
   lvl = &layout->level[level];
   switch (layout->kind) {
   case M9_TEXTURE_KIND_1D:
      return true;
   case M9_TEXTURE_KIND_3D:
      /* Slice pages collide with rows 8.. under this calculator state. */
      return lvl->in_tail && !level && lvl->height <= 8;
   default:
      return lvl->in_tail;
   }
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

static int
m9_texture_box_check(const struct m9_texture_layout *layout, unsigned level,
                     unsigned face, const struct pipe_box *box,
                     uint64_t map_size)
{
   uint64_t first, last;
   int ret;

   if (!box || box->x < 0 || box->y < 0 || box->z < 0 || box->width <= 0 ||
       box->height <= 0 || box->depth <= 0)
      return -EINVAL;
   ret = m9_texel_offset(layout, level, face, box->x, box->y, box->z, &first);
   if (!ret)
      ret = m9_texel_offset(layout, level, face, box->x + box->width - 1,
                            box->y + box->height - 1,
                            box->z + box->depth - 1, &last);
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
            uint64_t off;

            ret = m9_texel_offset(layout, level, face, box->x + dx,
                                  box->y + dy, box->z + dz, &off);
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
            uint64_t off;

            ret = m9_texel_offset(layout, level, face, box->x + dx,
                                  box->y + dy, box->z + dz, &off);
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
   uint32_t filter;
   long bias;

   if (!unit || !handle || !bind || !s || !bind->width ||
       bind->width > 0xffff || !bind->height || bind->height > 0xffff ||
       !bind->depth || bind->depth > 0xfff || bind->last_level > 15 ||
       bind->view_last_level > bind->last_level)
      return -EINVAL;
   mip = s->min_mip_filter != PIPE_TEX_MIPFILTER_NONE;
   linear_min = s->min_img_filter == PIPE_TEX_FILTER_LINEAR;
   filter = (linear_min ? 0x2 : 0) | 0x200 |
            (s->mag_img_filter == PIPE_TEX_FILTER_LINEAR ? 0x10 : 0);
   if (mip) {
      filter |= s->min_mip_filter == PIPE_TEX_MIPFILTER_LINEAR ? 0x100 : 0x80;
      /* 7c[10] is captured only for NEAREST_MIPMAP_NEAREST, LINEAR mag. */
      if (s->mag_img_filter == PIPE_TEX_FILTER_LINEAR && !linear_min &&
          s->min_mip_filter == PIPE_TEX_MIPFILTER_NEAREST)
         filter |= 0x400;
      lo = m9_texture_lod(s->min_lod);
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
   unit->w7e = border[1] << 24 | border[0] << 8;
   unit->w7f = border[3] << 24 | border[2] << 8;
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
