/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_DRAW_PARAMS_H
#define M9_DRAW_PARAMS_H

#include <stdbool.h>
#include <stdint.h>

/* Fixed two-pass draw resources, sized as in the verified userspace replay. */
#define M9_SYSTEM_BYTES           0x21000u
#define M9_SYSTEM_PREFIX_DWORDS   48u
#define M9_LINKED_OFFSET          0x300u
#define M9_LINKED_PREFIX_DWORDS   8u
#define M9_PROGRAM_BYTES          0x2000u
#define M9_PROGRAM_DWORDS         80u
#define M9_FRAGMENT_DB_MIN_BYTES  0x1c000u
#define M9_TRANSIENT_VIEWS        6u
/* Views 0..3 belong to the pass-through program, 4.. carry CPU varyings. */
#define M9_TRANSIENT_CPU_VIEW     4u
#define M9_TRANSIENT_VIEW_BYTES   0x16800u
#define M9_TRANSIENT_VERTEX_BYTES 16u
#define M9_BE_BYTES               0x100000u

#define M9_COLOR_FORMAT_A8R8G8B8  0x66u
#define M9_DEPTH_FORMAT_D24S8     0x7du
#define M9_HZ_FORMAT              0x78u
#define M9_HZ_CLEAR               0x007ff000u

/* Raster topology selector shared by indices 22 and d7. */
enum m9_topology {
   M9_TOPOLOGY_LINES = 0x1,
   M9_TOPOLOGY_LINE_STRIP = 0x2,
   M9_TOPOLOGY_TRIANGLES = 0x3,
   M9_TOPOLOGY_TRIANGLE_STRIP = 0x4,
   M9_TOPOLOGY_TRIANGLE_FAN = 0x5,
   M9_TOPOLOGY_QUADS = 0x6,
   M9_TOPOLOGY_QUAD_STRIP = 0x7,
   M9_TOPOLOGY_POLYGON = 0xd,
};

/* Kernel-accepted raster streams 00..04 with stride words 10..12. */
#define M9_RASTER_MAX_STREAMS     5u
#define M9_RASTER_MAX_DECL        8u
/* Native fragment constants; the span at +0x1000 is hardware-measured. */
#define M9_RASTER_CONST_OFFSET    0x1000u

/* Raster pass of a native fragment program job. */
struct m9_raster_fs {
   unsigned vertex_entry;                       /* preprocessing c3 */
   uint32_t r88;
   unsigned const_count;
   unsigned stream_count;
   uint32_t stream_view[M9_RASTER_MAX_STREAMS]; /* transient view per stream */
   uint32_t stride[3];                          /* 10..12 */
   unsigned decl_count;
   uint32_t decl[M9_RASTER_MAX_DECL];
};

/* Render-target size dependent words of one job. */
struct m9_draw_params {
   uint32_t width, height;
   uint32_t color_desc;     /* raster b1 */
   uint32_t depth_desc;     /* raster b9 */
   uint32_t aux_desc;       /* raster bb */
   uint32_t clip_min;       /* raster f5 and c4 without scissor */
   uint32_t clip_max;       /* raster f6 and c5 without scissor */
   uint32_t system_prefix[M9_SYSTEM_PREFIX_DWORDS];
   uint32_t linked_prefix[M9_LINKED_PREFIX_DWORDS];
};

/* Preprocessing source record declaration (indices 10 and 23). */
struct m9_vertex_decl {
   uint8_t position_components;  /* x,y[,z[,w]] */
   uint8_t color_components;     /* r,g,b[,a] */
   uint32_t record_bytes;
   uint32_t words[4];
};

int m9_draw_params_init(struct m9_draw_params *params, uint32_t width,
                        uint32_t height, uint8_t color_format);
uint64_t m9_hz_bytes(uint32_t width, uint32_t height);
int m9_vertex_decl_init(struct m9_vertex_decl *decl,
                        unsigned position_components,
                        unsigned color_components);
uint32_t m9_depth_control(bool test, bool write, unsigned pipe_func);
uint32_t m9_depth_ef(bool test, bool write);
uint32_t m9_cull_ef(unsigned pipe_cull_face, bool front_ccw);
uint32_t m9_color_mask(unsigned pipe_colormask);
void m9_scissor_words(uint32_t minx, uint32_t miny, uint32_t maxx,
                      uint32_t maxy, uint32_t *min, uint32_t *max);
uint32_t m9_raster_control(bool flat, enum m9_topology topology);
uint32_t m9_raster_control_decl(bool flat, enum m9_topology topology,
                                unsigned decl_count);
int m9_raster_fs_init(struct m9_raster_fs *r, unsigned vertex_entry,
                      uint32_t r88, unsigned const_count,
                      unsigned varying_count, const bool *flat_link,
                      const uint32_t *varying_view);
/* Texture k links coordinate stream texture_coord[k] as attribute 5+k. */
int m9_raster_fs_init_textured(struct m9_raster_fs *r, unsigned vertex_entry,
                               uint32_t r88, unsigned const_count,
                               unsigned varying_count, const bool *flat_link,
                               const uint32_t *varying_view,
                               unsigned coord_count,
                               const uint32_t *coord_view,
                               unsigned texture_count,
                               const uint8_t *texture_coord);
uint32_t m9_raster_d7(enum m9_topology topology);
uint32_t m9_raster_class(bool lines);
bool m9_topology_valid(unsigned topology);
uint32_t m9_draw_preprocess_vertices(uint32_t raster_vertices);
uint32_t m9_draw_max_source_vertices(void);

#endif
