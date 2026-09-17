/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * M9148 texture storage placement and texture-unit words, reproduced from the
 * vendor GL 1.5 capture (xf86-video-parhelia docs/parhelia-m9-texture-storage.md
 * and tools/m9-texture-layout-analyze.py).
 */
#ifndef M9_TEXTURE_H
#define M9_TEXTURE_H

#include <stdbool.h>
#include <stdint.h>

#include "pipe/p_state.h"
#include "util/format/u_formats.h"

#define M9_TEXTURE_MAX_LEVELS 13u
#define M9_TEXTURE_MAX_UNITS 4u
/* Application texture slots one job binds across all its raster passes: a
 * split fragment shader binds up to four in each of its passes. */
#define M9_DRAW_MAX_UNITS 32u
/* Levels up to 16x16 share one 32x16 packed tail. */
#define M9_TEXTURE_TAIL_EXTENT 16u
#define M9_TEXTURE_TAIL_BYTES 0x800u
#define M9_TEXTURE_LOD_DEFAULT 0x0ff80000u
/* Descriptor format code of the BGRA render-target family. */
#define M9_TEXTURE_CODE_BGRA 0xccu

/* Descriptor 79[23:16]. */
enum m9_texture_kind {
   M9_TEXTURE_KIND_2D = 0x01,
   M9_TEXTURE_KIND_CUBE = 0x09,
   M9_TEXTURE_KIND_3D = 0x11,
   M9_TEXTURE_KIND_1D = 0x18,
   M9_TEXTURE_KIND_RENDER = 0x21,
};

struct m9_texture_format {
   enum pipe_format storage;  /* 32-bit element bytes as stored */
   uint8_t code;              /* descriptor 79[31:24] */
};

struct m9_texture_level {
   uint64_t offset;           /* first face of the level */
   uint64_t face_bytes;
   uint32_t width, height, depth;
   uint16_t tail_x, tail_y;
   bool in_tail;
};

struct m9_texture_layout {
   enum m9_texture_kind kind;
   unsigned levels, faces;
   uint64_t size;
   /* Bit per level whose texels were verified to land on distinct addresses
    * inside their allocation; other levels have no usable placement. */
   uint32_t placed;
   struct m9_texture_level level[M9_TEXTURE_MAX_LEVELS];
};

/* Resource and view facts that select the unit words. */
struct m9_texture_bind {
   uint8_t code;
   enum m9_texture_kind kind;
   uint32_t width, height, depth;
   unsigned last_level;       /* resident levels above the base */
   unsigned view_last_level;  /* relative to the base */
};

struct m9_texture_unit {
   uint32_t handle;
   uint32_t w70, w79, w7a, w7c, w7d, w7e, w7f, w80, w81;
   bool placeholder;          /* unsampled unit-0 binding, 82..85 cleared */
};

bool m9_texture_format(enum pipe_format format, struct m9_texture_format *out);
/* Descriptor kind a Gallium target is stored and sampled as, 0 when the
 * target has no captured texture storage. */
enum m9_texture_kind m9_texture_kind_for_target(enum pipe_texture_target target);
/* Every level of every face has a verified placement.  A cube face and a
 * volume slice have no other way to reach their storage, so a layout that
 * is not complete has to be refused rather than placed wrongly. */
bool m9_texture_layout_complete(const struct m9_texture_layout *layout);
/* Descriptor kind the sampler binds this storage with: a cube and a volume
 * name themselves, a plane is the small 2-D kind up to 64 in both
 * dimensions and the ordinary tiled kind above it. */
enum m9_texture_kind m9_texture_bind_kind(const struct m9_texture_layout *layout,
                                          uint32_t width, uint32_t height);
int m9_texture_layout_init(struct m9_texture_layout *layout,
                           enum m9_texture_kind kind, uint32_t width,
                           uint32_t height, uint32_t depth, unsigned levels);
bool m9_texture_level_placed(const struct m9_texture_layout *layout,
                             unsigned level);
int m9_texel_offset(const struct m9_texture_layout *layout, unsigned level,
                    unsigned face, unsigned x, unsigned y, unsigned z,
                    uint64_t *offset);
/* box->z is the first slice of a volume and the first face of a cube, the
 * way Gallium addresses a layer; `face` is the face the box starts at. */
int m9_texture_store(const struct m9_texture_layout *layout, uint8_t *map,
                     uint64_t map_size, enum pipe_format storage,
                     unsigned level, unsigned face, const struct pipe_box *box,
                     enum pipe_format src_format, const uint8_t *src,
                     unsigned src_stride);
int m9_texture_fetch(const struct m9_texture_layout *layout,
                     const uint8_t *map, uint64_t map_size,
                     enum pipe_format storage, unsigned level, unsigned face,
                     const struct pipe_box *box, enum pipe_format dst_format,
                     uint8_t *dst, unsigned dst_stride);
int m9_texture_unit_init(struct m9_texture_unit *unit, uint32_t handle,
                         const struct m9_texture_bind *bind,
                         const struct pipe_sampler_state *sampler);
/* Native fragment programs that sample nothing still bind unit 0: the vendor
 * raster pass always carries a 1x1 BGRA texture there. */
void m9_texture_unit_placeholder(struct m9_texture_unit *unit, uint32_t handle);

#endif
