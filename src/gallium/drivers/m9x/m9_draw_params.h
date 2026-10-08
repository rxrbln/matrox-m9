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
/* A compiled c2 image is sized from the program it holds, not from the page
 * the captured pass-through happens to fit in.  The cap is the driver's own
 * allocation bound; no capture names a hardware program length. */
#define M9_PROGRAM_MAX_BYTES      0x40000u
#define M9_FRAGMENT_DB_MIN_BYTES  0x1c000u
#define M9_TRANSIENT_VIEWS        20u
/* Views 0..3 belong to the vertex program and the rest carry CPU varyings,
 * one per interpolated input or distinct texture coordinate.  One raster
 * pass reads at most seven of them, because it keeps three of its ten
 * stream slots for position, c and d; a shader split across passes needs
 * one view per coordinate of every pass, so the arena holds more. */
#define M9_TRANSIENT_CPU_VIEW     4u
/* Views the second preprocessing draw renders varyings 1..4 into.  They sit
 * on the CPU range on purpose: a native vertex job binds no CPU stream
 * object, so m9_stream_is_cpu leaves every view of it local, and the arena
 * has twenty views for both uses. */
#define M9_TRANSIENT_VS_EXTRA_VIEW 4u
/* The packed intermediate record the head writes sits above those views and
 * spans one view a varying, since a view holds sixteen bytes a vertex and a
 * record field is four dwords. */
#define M9_TRANSIENT_VS_RECORD_VIEW 8u
#define M9_TRANSIENT_VIEW_BYTES   0x16800u
#define M9_TRANSIENT_VERTEX_BYTES 16u
#define M9_BE_BYTES               0x100000u

#define M9_COLOR_FORMAT_A8R8G8B8  0x66u
/* Native 10-bit GigaColor, one code in both the sampler and the target role
 * (docs/parhelia-m9-gigacolor-10bit.md). */
#define M9_COLOR_FORMAT_A2B10G10R10 0x68u
#define M9_COLOR_FORMAT_A2R10G10B10 0x6au
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

/* Kernel-accepted raster streams 00..09 with stride words 10..14; the
 * engine has ten slots and five packed stride words, CAPTURED in the
 * vendor's own multi-varying programs. */
#define M9_RASTER_MAX_STREAMS     10u
/* Index 22 counts declaration words in four bits: at most sixteen. */
#define M9_RASTER_MAX_DECL        16u
/* Interpolated inputs one raster pass can carry.  The declaration budget no
 * longer sets it: a general interpolant declares in the single xyzw word
 * attributes 0, 5, 6 and 13 use, which MEASURED exact for one to six inputs
 * against the CPU reference on the M9128, so an input costs one word rather
 * than an xy/zw pair.  Six is the interpolation register file: a seventh
 * declares and rasterizes without stalling but reads garbage, whatever
 * attribute number it is given (MEASURED, tools/m9-user-raster-streams.c,
 * docs/parhelia-m9-raster-stream-limit-re.md). */
#define M9_RASTER_MAX_INTERP      6u
/* Native fragment constants; the span at +0x1000 is hardware-measured. */
#define M9_RASTER_CONST_OFFSET    0x1000u
/* Constant pairs one raster pass may carry.  MEASURED on the M9128: 56 pairs
 * (fsconst:24) renders every pixel exactly and 68 (fsconst:30) fails all
 * 76800 and stops the engine, so the ceiling lies between the two and the
 * driver stays at the highest measured-good count.  Index 8c carries the
 * count in eight bits, which is why the field alone admitted 127.
 * docs/parhelia-m9-fragment-pass-splitting.md */
#define M9_RASTER_MAX_CONSTS      56u
/* The declaration terminator both passes end their index-23 run with. */
#define M9_DECL_END               0x00004b08u
/* The preprocessing constant span starts one pair into the system prefix
 * (CAPTURED 8d = system + 0x10) and may run up to the linked record. */
#define M9_VS_CONST_OFFSET        0x10u
#define M9_VS_CONST_MAX_PAIRS     ((M9_LINKED_OFFSET - M9_VS_CONST_OFFSET) / 8u)
/* Additional preprocessing spans share the submission's system object, above
 * every raster span (at most M9_FS_MAX_PASSES * 56 pairs from +0x1000).
 * Their fixed slots include the viewport tail, not just shader uniforms. */
#define M9_VS_RUN_CONST_OFFSET    0x10000u
#define M9_VS_RUN_CONST_BYTES     ((8u * M9_VS_CONST_MAX_PAIRS + 255u) & ~255u)
#define M9_VS_MAX_CONST_RUNS      (1u + (M9_SYSTEM_BYTES - M9_VS_RUN_CONST_OFFSET) / M9_VS_RUN_CONST_BYTES)
/* Pairs the captured pass-through program's span holds. */
#define M9_VS_CONST_PAIRS         22u
/* Prefix dwords the tail reads as its viewport scale, depth scale, viewport
 * bias and depth bias: constant pairs 9..12, which is prefix dword 4 + 2p. */
#define M9_VS_VIEWPORT_DWORD      22u
#define M9_VS_DEPTH_SCALE_DWORD   24u
#define M9_VS_BIAS_DWORD          26u
#define M9_VS_DEPTH_BIAS_DWORD    28u
#define M9_VS_GUARD_X_DWORD       41u
#define M9_VS_GUARD_Y_DWORD       43u

/* Padding strips in m9_split_pad order: top, bottom, left, right.  A strip
 * intersecting the retained pixel clip would overwrite real fragments with
 * the padding's zero inputs.  Omit it: the original geometry already reaches
 * that target edge, so it needs no padding there. */
static inline unsigned
m9_split_pad_mask(unsigned width, unsigned height, unsigned x0, unsigned y0,
                  unsigned x1, unsigned y1)
{
   if (!width || !height || x0 >= x1 || y0 >= y1 ||
       x1 > width || y1 > height)
      return 0;
   return (y0 > 0 ? 1u : 0u) | (y1 < height ? 2u : 0u) |
          (x0 > 0 ? 4u : 0u) | (x1 < width ? 8u : 0u);
}

/* A per-fragment record carrying values from one raster pass to the next.
 * The descriptor names the record's stride in dwords and the dword the
 * operand starts at; the route word emitted at 0x74 + k names where the
 * remaining components come from, as record dword indices.
 * DERIVED in docs/parhelia-m9-fragment-pass-routes.md from the seven
 * captured two-pass GLSL cases and the five-sampler schedule. */
#define M9_RECORD_ABSENT       0xffu
#define M9_RECORD_MAX_DWORDS   0xffu
/* The four values descriptor bits 20:19 take across those captures. */
enum m9_record_form {
   M9_RECORD_PAIR = 0x1,      /* ee09: a two-dword operand */
   M9_RECORD_SLOT = 0x3,      /* ee19: a four-dword slot, or a sampler modifier */
   M9_RECORD_RESOURCE = 0x2,  /* ee11: a third component that addresses the resource */
};

uint32_t m9_record_desc(enum m9_record_form form, unsigned stride_dwords,
                        unsigned offset_dwords);
/* A coordinate route feeds the sampler slot it precedes; a value route
 * arrives in the register pair a texel would.  c1..c3 are record dword
 * indices, M9_RECORD_ABSENT drops a component. */
uint32_t m9_record_route(bool coordinate, unsigned index, unsigned c1,
                         unsigned c2, unsigned c3);
/* Route indices a coordinate route is captured at.  Index 3 never appears,
 * so the driver does not extrapolate the low byte there. */
#define M9_RECORD_MAX_COORD_ROUTE 3u
/* An upper bound on the record backing a target of this size needs.  The
 * descriptor carries a stride and a start offset and no pitch or extent at
 * all, so the record is a flat array the engine indexes by fragment, as the
 * preprocessing streams are indexed by vertex.  Its granularity -- one
 * record per fragment, per quad or per sample -- is not decoded, so this
 * rounds the target out to whole 64-pixel blocks. */
uint64_t m9_record_bytes(uint32_t width, uint32_t height,
                         unsigned stride_dwords);

/* Raster pass of a native fragment program job. */
struct m9_raster_fs {
   unsigned vertex_entry;                       /* preprocessing c3 */
   uint32_t r88;
   uint32_t r89;                                /* 0: the colour pass default */
   unsigned const_count;
   unsigned stream_count;
   uint32_t stream_view[M9_RASTER_MAX_STREAMS]; /* transient view per stream */
   uint32_t stride[(M9_RASTER_MAX_STREAMS + 1) / 2];  /* 10..14 */
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
/* The b1 descriptor of one colour attachment of this extent and format. */
int m9_color_surface_desc(uint32_t width, uint32_t height,
                          uint8_t color_format, uint32_t *desc);
/* Row-major 4x4; object coordinates become clip coordinates. */
void m9_draw_params_matrix(struct m9_draw_params *params, const float m[16]);

uint64_t m9_hz_bytes(uint32_t width, uint32_t height);
uint32_t m9_depth_row_span(uint32_t width, uint32_t height);
int m9_vertex_decl_init(struct m9_vertex_decl *decl,
                        unsigned position_components,
                        unsigned color_components);
uint32_t m9_depth_control(bool test, bool write, unsigned pipe_func);
uint32_t m9_depth_ef(bool test, bool write);

/* a9/aa with the stencil test off (CAPTURED, vendor GL baseline). */
#define M9_STENCIL_A9_OFF 0x08ffff00u
#define M9_STENCIL_AA_OFF 0x00ffff00u

/* One stencil face: PIPE_FUNC_* and PIPE_STENCIL_OP_* values. */
struct m9_stencil_face {
   unsigned func, fail_op, zfail_op, zpass_op;
   uint8_t ref, valuemask, writemask;
};

/* Adds the stencil test to a depth a8; back NULL is one-sided. */
void m9_stencil_words(uint32_t depth_a8, const struct m9_stencil_face *front,
                      const struct m9_stencil_face *back, bool front_ccw,
                      uint32_t *a8, uint32_t *a9, uint32_t *aa);
uint32_t m9_cull_ef(unsigned pipe_cull_face, bool front_ccw);
uint32_t m9_color_mask(unsigned pipe_colormask);
void m9_scissor_words(uint32_t minx, uint32_t miny, uint32_t maxx,
                      uint32_t maxy, uint32_t *min, uint32_t *max);
uint32_t m9_raster_control(bool flat, enum m9_topology topology);
/* Index 22 of the preprocessing pass; bits 11:8 are its declaration word
 * count minus one, as they are in the raster control. */
uint32_t m9_preprocess_control(unsigned decl_count);
/* Rewrites the viewport, depth range and guard band the appended tail reads,
 * for a pass whose vertex program emits clip coordinates of its own. */
void m9_draw_params_linked_viewport(const float scale[3], const float bias[3],
                                    uint32_t *linked);
void m9_draw_params_viewport(struct m9_draw_params *params,
                             const float scale[3], const float bias[3],
                             uint32_t *prefix);
uint32_t m9_raster_control_decl(bool flat, enum m9_topology topology,
                                unsigned decl_count);
/* The raster pass keeps three of its ten stream slots for position, c and d,
 * and its declaration holds sixteen words; a layout past either cannot be
 * emitted.  Stated once, so the compiler's gate and the emitter agree. */
static inline bool
m9_raster_fs_layout_ok(unsigned varying_count, unsigned coord_count,
                       unsigned texture_count)
{
   return varying_count <= M9_RASTER_MAX_INTERP &&
          varying_count + coord_count + 3 <= M9_RASTER_MAX_STREAMS &&
          varying_count + texture_count + 3 <= M9_RASTER_MAX_DECL &&
          texture_count <= 4;
}
int m9_raster_fs_init(struct m9_raster_fs *r, unsigned vertex_entry,
                      uint32_t r88, unsigned const_count,
                      unsigned varying_count, const bool *flat_link,
                      const uint32_t *varying_view);
/* A texel slot whose value comes from a record, not from a sampler: it
 * links no coordinate stream and declares no attribute. */
#define M9_RASTER_COORD_NONE 0xffu
/* The declaration word's low twelve bits are four three-bit component
 * selectors, 6 leaving a slot unwritten; 0x0688 is the plain 0,1,2,3 every
 * captured coordinate attribute uses.  This reads the pair at component c
 * of a stream and nothing else, which is how a coordinate shares the
 * varying's own stream instead of taking an output target of its own. */
static inline uint32_t
m9_coord_select(unsigned component)
{
   return component + 2 > 4 ? 0x0688u :
          component | (component + 1) << 3 | 6u << 6 | 6u << 9;
}

/* Texture k links coordinate stream texture_coord[k] as attribute 5+k.
 * coord_select, when given, carries one selector word a coordinate stream;
 * zero there is the plain 0x0688. */
int m9_raster_fs_init_textured(struct m9_raster_fs *r, unsigned vertex_entry,
                               uint32_t r88, unsigned const_count,
                               unsigned varying_count, const bool *flat_link,
                               const uint32_t *varying_view,
                               unsigned coord_count,
                               const uint32_t *coord_view,
                               const uint32_t *coord_select,
                               unsigned texture_count,
                               const uint8_t *texture_coord,
                               bool clip_position);
uint32_t m9_raster_d7(enum m9_topology topology);
uint32_t m9_raster_class(bool lines, bool blend_dest);
bool m9_topology_valid(unsigned topology);
uint32_t m9_draw_preprocess_vertices(uint32_t raster_vertices);
/* Source records one job preprocesses, and the stride of the transient
 * stream views that hold them.  The vendor's 0x16800 is its own partition
 * of a 0x5a000 context resource, and its five-stream captures repartition
 * the same resource at 0x12400 (docs/parhelia-m9-transient-arena-re.md):
 * the view stride is a driver allocation, not a hardware extent.  Because
 * one job's records are its raster index space, the ceiling is bounded by
 * the 16-bit index.  M9X_JOB_VERTICES moves it, so one binary weighs the
 * per-job cost against the records a job carries. */
#define M9_JOB_VERTICES_MIN 256u
#define M9_JOB_VERTICES_MAX 65532u
/* Four times the vendor's partition.  MEASURED on refract at 1920x1200, one
 * binary with this the only variable and no draw skipped in any cell: 130
 * jobs and 210.8 ms a frame at 11520, 70 jobs and 166.5 ms here, 67 and 165.9
 * at 32768, 55 and 165.5 at 46080.  The gain is the back-end clear a split
 * job takes, which is why it stops paying past this.  65532 -- the most the
 * 16-bit raster index allows -- exhausts VRAM instead: the transient views
 * are 20 * ceiling * 16 bytes, 21 MiB there, and the run reports "cannot
 * allocate target depth", draws nothing and times the stream out (MEASURED).
 * M9X_JOB_VERTICES=5760 restores the vendor's own partition. */
#define M9_JOB_VERTICES_DEFAULT 23040u
/* How the second preprocessing draw hands its varyings on, and the words
 * that shape follows (m9_draw_params.c). */
void m9_draw_set_vs_record(unsigned mode);
unsigned m9_vs_record_mode(void);
uint32_t m9_vs_extra_desc(unsigned target, unsigned targets);
uint32_t m9_vs_gather_route(unsigned target);
void m9_draw_set_job_vertices(uint32_t vertices);
uint32_t m9_draw_max_source_vertices(void);
uint32_t m9_transient_view_bytes(void);
/* CPU-filled linear stream views need only this job's copied records.
 * GPU-rendered transient targets retain their configured surface extent. */
uint32_t m9_cpu_stream_view_bytes(uint32_t records, uint32_t copies);

#endif
