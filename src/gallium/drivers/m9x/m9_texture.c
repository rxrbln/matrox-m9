/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Texture placement: the 32x16 packed tail (levels up to 16x16) is the
 * captured address permutation.  Cube faces through 64x64 and every volume
 * level use the source-derived calculators.  Larger plane levels report
 * -ENOTSUP and use the kernel texture window; larger cubes remain unplaced.
 */
#include "m9_hw_encode.h"
#include "m9_texture.h"

#include "m9_internal.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "pipe/p_defines.h"
#include "util/format/u_format.h"
#include "util/format_srgb.h"
#include "util/macros.h"

bool
m9_texture_format(enum pipe_format format, struct m9_texture_format *out)
{
   struct m9_texture_format info;

   /* sRGB is stored as its UNORM twin and decoded by the program. */
   format = util_format_linear(format);
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
   /* DEPTH24 is a 32-bit element, LE d << 8, with the ordinary placement
    * (CAPTURED, docs/parhelia-m9-texture-storage.md 3); a 16-bit depth is
    * widened into it like the narrow colour formats. */
   case PIPE_FORMAT_X8Z24_UNORM:
   case PIPE_FORMAT_S8_UINT_Z24_UNORM:
      info = (struct m9_texture_format){ format, M9_TEXTURE_CODE_DEPTH24 };
      break;
   case PIPE_FORMAT_Z16_UNORM:
      info = (struct m9_texture_format){ PIPE_FORMAT_X8Z24_UNORM,
                                         M9_TEXTURE_CODE_DEPTH24 };
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

   format = util_format_linear(format);
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

bool
m9_texture_compressed_small(uint32_t width, uint32_t height,
                            uint8_t block_bytes)
{
   /* The storage kind a level is written and sampled through: the small
    * 2-D kind up to 64 in both the 32-aligned span and the microtile-
    * aligned height, the ordinary tiled kind above it (MEASURED). */
   uint32_t span = (width + 31) & ~31u;
   uint32_t lines = block_bytes == 8 ? ((height + 15) & ~15u) :
                                       ((height + 7) & ~7u);

   return span <= 64 && lines <= 64;
}

uint64_t
m9_texture_compressed_bytes(uint32_t width, uint32_t height,
                            uint8_t block_bytes)
{
   /* Section 4.1's sel = 1 granularity rows, which the measurement of
    * section 12.2 selects: width to 32 texels, height to 64 for the 4-bit
    * family and 32 for the 8-bit one, then a 4 KiB object quantum.  This
    * is an allocation bound -- how far the bank swizzle spreads a level --
    * not the stride the chain advances by. */
   uint64_t pad_w = (width + 31) & ~31u;
   uint64_t pad_h = block_bytes == 8 ? ((height + 63) & ~63u) :
                                       ((height + 31) & ~31u);
   uint64_t bytes = pad_w * pad_h * block_bytes / 16;

   return (bytes + 0xfff) & ~0xfffull;
}

uint64_t
m9_texture_compressed_level_bytes(uint32_t width, uint32_t height,
                                  uint8_t block_bytes)
{
   /*
    * MEASURED (fixture dxtmip*, sweeping M9X_DXT_LEVEL_OFFSETS): the
    * sampler computes every level's address itself from level 0's extent,
    * so a chain can only be placed where the sweep finds it.  Three rules
    * cover every shape measured:
    *
    *   a tiled level is charged a span of 64 texels and a height of
    *   section 4.1's sel = 1 granularity -- 64 rows for the 4-bit family,
    *   32 for the 8-bit one.  132x68 DXT1 advances by 0x3000, which is
    *   192x128 and not the 160 the window descriptor carries.
    *
    *   a small-kind level is charged its 32-aligned span and its own
    *   height: the 32x32 level of a 64x64 DXT1 chain advances by 0x200.
    *
    * The 4 KiB quantum of m9_texture_compressed_bytes() is an allocation
    * bound, not a stride; using it as one put every level past 64x64 where
    * nothing reads it.
    */
   bool small = m9_texture_compressed_small(width, height, block_bytes);
   uint64_t span = small ? ((width + 31) & ~31u) : ((width + 63) & ~63u);
   uint64_t rows = small ? height :
                   block_bytes == 8 ? ((height + 63) & ~63u) :
                                      ((height + 31) & ~31u);

   return span * rows * block_bytes / 16;
}

/*
 * M9X_DXT_LEVEL_OFFSETS="0,1000,3000" places a compressed chain by hand, in
 * hex bytes, one entry a level.  It is how the hardware's own level bases
 * were measured: the sampler addresses every level from one base and picks
 * the level's address itself, so a chain can only be placed where a sweep
 * finds it.  Levels past the list keep their computed offset.
 */
static void
m9_compressed_offsets_override(struct m9_texture_layout *layout)
{
   const char *env = getenv("M9X_DXT_LEVEL_OFFSETS");
   uint64_t end = 0;
   char *next;

   if (!env)
      return;
   for (unsigned l = 0; l < layout->levels && *env; l++) {
      uint64_t off = strtoull(env, &next, 16);

      if (next == env)
         break;
      layout->level[l].offset = off & ~0xffull;
      env = *next ? next + 1 : next;
   }
   for (unsigned l = 0; l < layout->levels; l++)
      end = MAX2(end, layout->level[l].offset + layout->level[l].face_bytes);
   layout->size = end;
}

int
m9_texture_layout_compressed(struct m9_texture_layout *layout, uint32_t width,
                             uint32_t height, unsigned levels,
                             uint8_t block_bytes)
{
   static const uint8_t tail_x[5] = { 0, 16, 24, 16, 24 };
   static const uint8_t tail_y[5] = { 0, 0, 0, 8, 8 };
   uint32_t max_dim = MAX2(width, height);
   unsigned chain = 1;
   uint64_t offset = 0, end = 0;
   int tail = -1;

   if (!layout || !width || !height || !levels ||
       levels > M9_TEXTURE_MAX_LEVELS || width > 2048 || height > 2048 ||
       (block_bytes != 8 && block_bytes != 16))
      return -EINVAL;
   while (max_dim >>= 1)
      chain++;
   if (levels > chain)
      return -EINVAL;
   memset(layout, 0, sizeof(*layout));
   layout->kind = M9_TEXTURE_KIND_2D;
   layout->levels = levels;
   layout->faces = 1;
   layout->block_bytes = block_bytes;
   for (unsigned l = 0; l < levels; l++) {
      struct m9_texture_level *lvl = &layout->level[l];
      uint32_t w = MAX2(width >> l, 1), h = MAX2(height >> l, 1);

      /* A level below one block still occupies a whole block, so that is
       * the extent the window writes and the allocation holds. */
      lvl->width = MAX2(w, 4);
      lvl->height = MAX2(h, 4);
      lvl->depth = 1;
      offset = (offset + 0x1ff) & ~0x1ffull;   /* per-level alignment */
      if (w <= M9_TEXTURE_TAIL_EXTENT && h <= M9_TEXTURE_TAIL_EXTENT) {
         unsigned rel;

         /* MEASURED: the levels of 16 texels or less share one 32x16
          * surface, at the same origins the uncompressed capture proves --
          * level 2 of a 64x32 DXT1 chain is the only base the sampler
          * accepts for all five of them. */
         if (tail < 0) {
            tail = (int)l;
            lvl->offset = offset;
            lvl->face_bytes = m9_texture_compressed_level_bytes(
               M9_COMPRESSED_TAIL_WIDTH, M9_COMPRESSED_TAIL_HEIGHT,
               block_bytes);
            offset += lvl->face_bytes;
         } else {
            lvl->offset = layout->level[tail].offset;
            lvl->face_bytes = layout->level[tail].face_bytes;
         }
         rel = l - (unsigned)tail;
         if (rel >= ARRAY_SIZE(tail_x))
            return -EINVAL;
         lvl->tail_x = tail_x[rel];
         lvl->tail_y = tail_y[rel];
         lvl->in_tail = true;
      } else {
         lvl->offset = offset;
         lvl->face_bytes = m9_texture_compressed_level_bytes(w, h,
                                                             block_bytes);
         offset += lvl->face_bytes;
      }
      /* The stride is what the sampler walks; the object still has to hold
       * the bank swizzle's reach, which below 128x128 is several times a
       * level's own bytes (section 12.2). */
      end = MAX2(end, lvl->offset +
                      MAX2(lvl->face_bytes,
                           m9_texture_compressed_bytes(lvl->width,
                                                       lvl->height,
                                                       block_bytes)));
   }
   layout->size = (end + 0xfff) & ~0xfffull;
   m9_compressed_offsets_override(layout);
   return 0;
}

/*
 * Vendor m9x659, the mode-2 aperture calculator, with the state its
 * initializer m9x611 derives from the surface (STATIC, KernelLibraries.o
 * 0x23150): `depth` is the slice count, the category flag is set when the
 * 32-aligned span is a multiple of 64 (twice the 32-bit horizontal quantum),
 * and the two masks are set for an even and a multiple-of-4 depth.  With
 * depth 1 and span 32 this is the captured tail placement.
 */
static uint64_t
m9_aperture_address(unsigned x, unsigned y, unsigned z, unsigned depth,
                    unsigned span)
{
   unsigned cat = !(span & 63);
   unsigned page = ((y >> 4) & !(depth & 1)) + 2 * ((y >> 2) & !(depth & 3));
   uint64_t a = (x & 7) << 2 | (x & 8) << 4 | (y & 3) << 5;

   a |= (((x >> 4) ^ (y >> 2)) & 1) << 8;
   a |= (((y >> 2) ^ (y >> 4) ^ (x >> 5) ^ z ^ (y >> 3)) & 1) << 9;
   return a + ((uint64_t)(page ^ z) + (uint64_t)(y >> (3 - cat)) * depth) *
              0x400;
}

/* Vendor m9x474, the mode-2 local calculator (STATIC), over the 32-aligned
 * extents: one 4 KiB tile per 32x32 texels of a slice. */
static uint64_t
m9_local_address(unsigned x, unsigned y, unsigned z, unsigned width,
                 unsigned height)
{
   uint64_t tile = ((uint64_t)z * (height >> 5) + (y >> 5)) * (width >> 5) +
                   (x >> 5);
   uint64_t a = (x & 7) << 2 | (x & 8) << 4 | (y & 3) << 5;

   a |= (((x >> 4) ^ (y >> 2)) & 1) << 8;
   a |= ((z ^ (y >> 3) ^ (x >> 5) ^ (y >> 2)) & 1) << 9;
   a |= (((y >> 4) ^ (x >> 7) ^ (x >> 5) ^ (x >> 6) ^ (z >> 1)) & 1) << 10;
   a |= (((x >> 7) ^ (x >> 5) ^ (y >> 5) ^ (y >> 3)) & 1) << 11;
   return (tile << 12) + a;
}

/* Address of a texel of a calculator-placed level, relative to its face. */
static uint64_t
m9_level_address(const struct m9_texture_layout *layout,
                 const struct m9_texture_level *lvl, unsigned x, unsigned y,
                 unsigned z)
{
   if (lvl->in_tail)
      return m9_aperture_address(lvl->tail_x + x, lvl->tail_y + y, z,
                                 layout->tail_depth,
                                 M9_COMPRESSED_TAIL_WIDTH);
   if (lvl->local)
      return m9_local_address(x, y, z, (lvl->width + 31) & ~31u,
                              (lvl->height + 31) & ~31u);
   return m9_aperture_address(x, y, z, lvl->depth, (lvl->width + 31) & ~31u);
}

/* Keep only the calculator-placed levels whose texels are all distinct
 * inside their allocation; the tail levels share the first one's atlas.  A
 * level that would overwrite another is left unplaced, and every later level
 * with it, so that nothing is ever written to a wrong address. */
static int
m9_texture_verify(struct m9_texture_layout *layout, uint32_t candidates)
{
   uint8_t *seen = NULL;
   uint64_t seen_bytes = 0, seen_offset = UINT64_MAX;
   int ret = 0;

   for (unsigned l = 0; l < layout->levels; l++) {
      const struct m9_texture_level *lvl = &layout->level[l];
      uint64_t bytes = lvl->face_bytes;
      bool ok = true;

      if (!(candidates & (1u << l)))
         continue;
      /* A new region unless this tail level shares the previous atlas. */
      if (lvl->offset != seen_offset) {
         free(seen);
         seen = calloc((size_t)(bytes / 4), 1);
         if (!seen) {
            ret = -ENOMEM;
            break;
         }
         seen_bytes = bytes;
         seen_offset = lvl->offset;
      }
      for (unsigned z = 0; ok && z < lvl->depth; z++)
         for (unsigned y = 0; ok && y < lvl->height; y++)
            for (unsigned x = 0; ok && x < lvl->width; x++) {
               uint64_t a = m9_level_address(layout, lvl, x, y, z);

               if (a + 4 > seen_bytes || seen[a / 4])
                  ok = false;
               else
                  seen[a / 4] = 1;
            }
      if (!ok)
         break;
      layout->placed |= 1u << l;
   }
   free(seen);
   return ret;
}

int
m9_texture_layout_init(struct m9_texture_layout *layout,
                       enum m9_texture_kind kind, uint32_t width,
                       uint32_t height, uint32_t depth, unsigned levels)
{
   static const uint8_t tail_x[5] = { 0, 16, 24, 16, 24 };
   static const uint8_t tail_y[5] = { 0, 0, 0, 8, 8 };
   uint32_t max_dim = MAX3(width, height, depth);
   uint32_t candidates = 0;
   unsigned chain = 1;
   uint64_t offset = 0, end = 0;
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
         candidates |= 1u << l;
      } else if (lvl->width <= M9_TEXTURE_TAIL_EXTENT &&
                 lvl->height <= M9_TEXTURE_TAIL_EXTENT &&
                 lvl->depth < M9_TEXTURE_TAIL_EXTENT) {
         unsigned rel;

         if (tail < 0) {
            tail = l;
            /* The 32x16 atlas is one 2 KiB page pair per slice. */
            layout->tail_depth = lvl->depth;
            lvl->face_bytes = (uint64_t)M9_TEXTURE_TAIL_BYTES * lvl->depth;
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
         candidates |= 1u << l;
      } else if (kind == M9_TEXTURE_KIND_3D) {
         uint32_t span = (lvl->width + 31) & ~31u;
         unsigned rows = span & 63 ? 8 : 4;

         /* MEASURED (M9X_TEXTURE_ADDRCODE): the sampler addresses a volume
          * of 16 slices or more, or wider than 64, with the local
          * calculator, and every other one with the aperture calculator. */
         lvl->local = lvl->depth >= 16 || span > 64;
         if (lvl->local)
            lvl->face_bytes = (uint64_t)span * ((lvl->height + 31) & ~31u) *
                              lvl->depth * 4;
         else
            /* One 1 KiB page per slice for every 8 (span 32) or 4 (span 64)
             * rows. */
            lvl->face_bytes = (uint64_t)DIV_ROUND_UP(lvl->height, rows) *
                              lvl->depth * 0x400;
         lvl->offset = offset;
         offset += lvl->face_bytes;
         candidates |= 1u << l;
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
         /* MEASURED (M9X_TEXTURE_ADDRCODE, 100x37): the sampler advances
          * past a small level by the aperture calculator's 1 KiB pages --
          * 8 rows at span 32, 4 at span 64 -- so 50x18 takes 0x1400, not
          * the 0x1800 the surface contract allocates. */
         if (render.small)
            lvl->face_bytes = (uint64_t)DIV_ROUND_UP(lvl->height,
                                                     render.row_span & 63 ?
                                                     8 : 4) * 0x400;
         /* The kernel window still checks the surface contract's extent. */
         end = MAX2(end, offset + layout->faces *
                         ((((uint64_t)render.row_span *
                            render.allocation_height * 4) + quantum - 1) &
                          ~(quantum - 1)));
         /* A standalone plane is the source surface allocation, including
          * its eight spare elements after the last full row. Keep the level
          * descriptor and texel addresses unchanged. A mip chain retains
          * its separately qualified packed extent/placement contract. */
         if(kind==M9_TEXTURE_KIND_2D&&levels==1)
            end=MAX2(end,render.size);
         lvl->offset = offset;
         offset += layout->faces * lvl->face_bytes;
         /* kind 09 selects the aperture calculator.  The original cube
          * constructor advances each mip by six face allocations, and its
          * per-face object uses the same mode-2 address calculation as a
          * plane.  Exhaustive verification below proves that calculation
          * injective and inside each face only through a 64-texel span;
          * at 65, x=64 aliases x=0.  Keep wider faces unplaced. */
         if (kind == M9_TEXTURE_KIND_CUBE && lvl->width <= 64 &&
             lvl->height <= 64)
            candidates |= 1u << l;
      }
   }
   layout->size = MAX2(offset, end);
   if (kind == M9_TEXTURE_KIND_1D)
      layout->placed = (2u << (levels - 1)) - 1;
   else if (m9_texture_verify(layout, candidates))
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

bool
m9_texture_layout_complete(const struct m9_texture_layout *layout)
{
   return layout && layout->levels &&
          layout->placed == (2u << (layout->levels - 1)) - 1;
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
                m9_level_address(layout, lvl, x, y, z);
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

/* Texels x..x+7 of an 8-aligned run are 32 contiguous bytes in every
 * placement (A[4:2] = x[2:0], and no higher bit reads x[2:0]). */
static unsigned
m9_texel_run(unsigned x, int left)
{
   return !(x & 7) && left >= 8 ? 8 : 1;
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
         for (int dx = 0; dx < box->width;) {
            unsigned f, z, n = m9_texel_run(box->x + dx, box->width - dx);
            uint64_t off;

            m9_texture_layer(layout, face, box, dz, &f, &z);
            ret = m9_texel_offset(layout, level, f, box->x + dx,
                                  box->y + dy, z, &off);
            if (ret)
               break;
            memcpy(map + off, row + 4 * dx, 4 * n);
            dx += n;
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
         for (int dx = 0; dx < box->width;) {
            unsigned f, z, n = m9_texel_run(box->x + dx, box->width - dx);
            uint64_t off;

            m9_texture_layer(layout, face, box, dz, &f, &z);
            ret = m9_texel_offset(layout, level, f, box->x + dx,
                                  box->y + dy, z, &off);
            if (ret)
               break;
            memcpy(row + 4 * dx, map + off, 4 * n);
            dx += n;
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
   case PIPE_TEX_WRAP_REPEAT: return M9_HW_WRAP_REPEAT;
   case PIPE_TEX_WRAP_CLAMP_TO_EDGE: return M9_HW_WRAP_CLAMP_TO_EDGE;
   case PIPE_TEX_WRAP_MIRROR_REPEAT: return M9_HW_WRAP_MIRROR_REPEAT;
   case PIPE_TEX_WRAP_CLAMP: return M9_HW_WRAP_CLAMP;
   case PIPE_TEX_WRAP_CLAMP_TO_BORDER: return M9_HW_WRAP_CLAMP_TO_BORDER;
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
   filter = (linear_min ? M9_HW_7C_MIN_LINEAR : 0) |
            (s->mag_img_filter == PIPE_TEX_FILTER_LINEAR ? M9_HW_7C_MAG_LINEAR : 0);
   if (mip) {
      filter |= s->min_mip_filter == PIPE_TEX_MIPFILTER_LINEAR ?
                M9_HW_7C_MIP_LINEAR : M9_HW_7C_MIP_NEAREST;
      if (s->mag_img_filter == PIPE_TEX_FILTER_LINEAR && !linear_min &&
          s->min_mip_filter == PIPE_TEX_MIPFILTER_NEAREST)
         filter |= M9_HW_7C_MIP_NEAREST_MAG_LINEAR;
      /* The sampler's LOD counts from level 0, Gallium's from the view's
       * base level. */
      unsigned base = bind->view_first_level * 32;

      lo = MIN2(base + m9_texture_lod(s->min_lod), 511u);
      if (bind->view_last_level < bind->last_level)
         hi = MIN2(hi, bind->view_last_level * 32);
      if (s->max_lod + bind->view_first_level < (float)bind->last_level)
         hi = MIN2(hi, base + m9_texture_lod(s->max_lod));
   }
   bias = CLAMP(lroundf(s->lod_bias * 32.0f), -512, 511);
   shadow = s->compare_mode == PIPE_TEX_COMPARE_R_TO_TEXTURE;
   form = bind->kind == M9_TEXTURE_KIND_CUBE ? 3 :
          bind->kind == M9_TEXTURE_KIND_3D || shadow ? 2 : 1;

   unit->handle = handle;
   /* The unit words are shared with the kernel FAA producers. */
   unit->w7d = m9_hw_sampler_7d((uint32_t)bias, lo, hi);
   unit->w70 = m9_hw_texture_70(form, unit->w7d != M9_TEXTURE_LOD_DEFAULT,
                                shadow, compare[s->compare_func & 7]);
   unit->w79 = m9_hw_texture_79(bind->code, bind->kind, bind->width);
   unit->w7a = m9_hw_texture_7a(mip ? bind->last_level : 0, bind->depth,
                                bind->height);
   unit->w7c = m9_hw_sampler_7c(filter, m9_texture_wrap(s->wrap_s, &ok),
                                m9_texture_wrap(s->wrap_t, &ok),
                                m9_texture_wrap(s->wrap_r, &ok));
   /* Raw DEPTH16 sampling. */
   if (bind->code == 0xb8 && !shadow)
      unit->w7c |= 1u << 31;
   /* GL does not decode the border, so an sRGB unit stores it encoded and
    * the program's decode returns it. */
   for (unsigned c = 0; c < 4; c++) {
      float v = CLAMP(s->border_color.f[c], 0.0f, 1.0f);

      border[c] = bind->srgb && c < 3 ?
                  util_format_linear_float_to_srgb_8unorm(v) :
                  (unsigned)lroundf(v * 255.0f);
   }
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
