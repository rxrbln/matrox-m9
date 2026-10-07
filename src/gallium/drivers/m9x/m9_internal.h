/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_INTERNAL_H
#define M9_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "compiler/shader_info.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "m9_aggregate_uapi.h"
#include "m9_draw_params.h"
#include "m9_faa.h"
#include "m9_faa_application.h"
#include "m9_vs_compile.h"
#include "m9_vs_source.h"
#include "m9_texture.h"
#include "util/simple_mtx.h"
#include "util/u_atomic.h"
#include "util/u_dynarray.h"

#define M9_PCI_ID 0x0540u
#define M9_PACKET_STOP 0x45u
#define M9_MAX_COMMAND_DWORDS 8192u
/* The kernel frames every packet with a count word into a 32 KiB stream
 * image that also holds an 8-byte prefix and a 44-byte footer. */
#define M9_MAX_FRAMED_DWORDS ((32768u - 8u - 44u) / 4u)
/* Relocation sites one submission may present.  The kernel takes at most
 * one a command dword, so the command image is the only real bound; the
 * batch grows its array and never refuses a job the stream still fits. */
#define M9_MAX_RELOCS M9_MAX_COMMAND_DWORDS
/* Objects one submission may reference; the kernel's own ceiling. */
#define M9_MAX_BOS 128u
/* Colour targets one draw binds; m9_draw.h states it as M9_DRAW_MAX_COLOR
 * and m9_draw.h cannot be included before this header's own definitions. */
#define M9_MAX_COLOR 4u
#define M9_MAX_MIP_LEVELS 13u
#define M9_MAX_SKIP_REASONS 16u

enum m9_resource_kind {
   M9_RESOURCE_LINEAR_BUFFER,
   M9_RESOURCE_COMMAND,
   M9_RESOURCE_LINEAR_COLOR,
   M9_RESOURCE_LINEAR_DEPTH,
   M9_RESOURCE_LINEAR_TEXTURE,
   M9_RESOURCE_SYSTEM_STATE,
};

enum m9_bo_domain {
   /* CREATE_BO: DMA32 system memory accepted by the submit validator. */
   M9_BO_DMA32,
   /* Driver VRAM GEM class; pinned local-memory objects for render/scanout. */
   M9_BO_VRAM,
};

/* One GEM handle shared by every resource that imported or exported it; the
 * entry closes a handle this screen created once the last resource goes. */
struct m9_prime_import {
   struct m9_prime_import *next;
   uint32_t handle;
   uint32_t references;
   bool owns_handle;
};

struct m9_mip_level {
   uint64_t offset;
   uint32_t width, height, depth;
   uint32_t row_span;
   uint16_t tail_x, tail_y;
   bool in_tail;
};

struct m9_mip_layout {
   struct m9_mip_level level[M9_MAX_MIP_LEVELS];
   uint64_t size;
   uint8_t levels;
   uint8_t cpp;
};

/* This is intentionally independent of struct parhelia_resource. */
struct m9_resource {
   struct pipe_resource base;
   uint32_t handle;
   enum m9_resource_kind kind;
   uint64_t size;
   uint64_t mmap_offset;
   uint32_t width, height, depth;
   uint32_t row_span;
   uint32_t cpp;
   uint8_t format;
   uint8_t layout;
   enum m9_bo_domain domain;
   bool tiled;  /* kernel PARHELIA_M9_LAYOUT_TILED storage */
   bool handle_owned;
   bool map_writable;
   int map_fd;
   struct m9_prime_import *prime_import;
   struct m9_mip_layout mip;
   /* Tiled sampler storage; texture.levels is 0 for other images. */
   struct m9_texture_layout texture;
   enum pipe_format texture_storage;
   uint8_t texture_code;
   /* Bytes a 4x4 block occupies; 0 unless the image is S3TC. */
   uint8_t block_bytes;
   uint32_t texture_unplaced;  /* levels whose content could not be stored */
   bool level_refused;         /* a clamped level was already named */
   uint32_t levels_written;    /* levels a transfer has actually written */
   /* Bumped whenever this driver hands out a writable mapping, so
    * anything derived from the contents can tell they have moved on. */
   uint64_t generation;
   /* Last submitted reader/writer of a private back-end state object.
    * Zero means this owner has never been submitted, not GPU retirement. */
   uint64_t be_point;
   bool bind_logged;
   void *map;
   /* Hierarchical-Z metadata owned by a D24S8 depth image. */
   struct m9_resource *hz;
};

/* One surface-owned FAA graph. The indirect command pool belongs to the
 * kernel's trusted producer and is deliberately absent from this group.
 * initialized becomes true only after the real GPU initialization retires. */
struct m9_faa_graph {
   struct m9_faa_graph_layout layout;
   struct m9_resource main, auxiliary, companion, hz, sentinel;
   /* PREPARE pins CPU-owned GEM resources only. A real GPU init receipt is
    * still required before initialized can become true. Generation0 is valid. */
   uint64_t prepared_id;
   uint64_t init_point;
   uint32_t prepared_generation;
   bool prepared;
   bool initialized;
   bool color_lock_attempted;
   uint64_t color_lock_point;
   /* Exact submitted delta, published only after receipt and wait succeed. */
   uint32_t color_lock_delta;
   uint32_t color_frame_draw_count; /* Authenticated batch receipt, zero for legacy FRAME. */
   uint32_t color_program_count;
   bool color_depth_initialized; /* Set only after an authenticated depth FRAME. */
   uint32_t color_frame_flags; /* Exact FIRST/INITIALIZED protocol of the last FRAME. */
   /* Per-cycle NEXT attempt remains consumed on any reported failure.
    * Historical authentic base/point stay until graph release. */
   bool color_next_attempted;
   uint64_t color_next_previous_unlock_point, color_next_lock_point;
   bool color_unlock_attempted;
   uint64_t color_unlock_point;
   /* A typed preprocessing prefix is distinct from a returned DRAW/image.
    * Any attempted ioctl may consume INIT; retry needs a new graph token. */
   bool public_vs_head_attempted;
   uint64_t public_vs_head_point;
   bool public_vs_draw_attempted;
   uint64_t public_vs_draw_point;
   bool public_vs_lock_attempted;
   uint64_t public_vs_lock_point;
   bool public_vs_unlock_attempted;
   uint64_t public_vs_unlock_point;
   bool public_vs_next_attempted;
   uint64_t public_vs_next_lock_point;
   bool public_vs_next_unlock_attempted;
   uint64_t public_vs_next_unlock_point;
   bool public_vs_attempted;
   uint64_t public_vs_point;
   bool public_vs_follow_attempted;
   uint64_t public_vs_follow_point;
};

/* A context's prepared sampled target and the attachments it stands for.
 * The ordinary destination retains its own layout and level offset. No
 * prepared owner is an executable draw/resolve route until initialization,
 * pool ownership and matching compiled shader variants are established. */
struct m9_faa_surface {
   struct m9_faa_primary_layout primary_layout;
   struct m9_resource primary;
   struct m9_faa_graph graph;
   struct pipe_resource *destination, *depth;
   uint64_t destination_offset;
   uint32_t width, height;
   uint8_t format;
   bool destination_needs_clear;
};

/* What the engine's back-end object last carried.  It keeps per-target state
 * there, and a job that finds another shape's state stops the engine, so the
 * object is cleared when this changes rather than ahead of every job. */
struct m9_be_shape {
   bool valid;
   const struct m9_fs_variant *variant;
   uint32_t color, color_offset, depth, aux;
   uint32_t color_desc, depth_desc, clip_min, clip_max;
   unsigned color_count, passes;
};

struct m9_target_depth {
   uint32_t width, height;
   uint64_t used;
   /* The pair a scratch surface binds is its own: the engine state below
    * belongs to one surface, not to every target of its shape. */
   bool scratch;
   struct m9_resource depth, aux;
   /* The engine's own back-end object, which belongs to a target rather than
    * to the context: a shape revisited after another finds its own.  There
    * are two because a job posted asynchronously retires only at the next
    * submission: a clear swaps to the spare and zeroes that, so the engine
    * is never reading the object the CPU overwrites.  The spare is allocated
    * the first time a target of this shape wants a clear. */
   struct m9_resource be[2];
   unsigned be_slot;
   struct m9_be_shape be_shape;
};

/* Enough for several full-screen pairs, so that a scene alternating a
 * window target with the screen keeps both; beyond that a size is one the
 * context has stopped drawing into. */
#define M9_TARGET_DEPTH_BUDGET (32u << 20)

static inline uint32_t
m9_resource_bo_flags(const struct pipe_resource *templ)
{
   const unsigned gpu_image = PIPE_BIND_RENDER_TARGET |
                              PIPE_BIND_DEPTH_STENCIL |
                              PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_SHARED |
                              PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET |
                              PIPE_BIND_USE_FRONT_RENDERING;

   /* Gallium buffers are the command/vertex/index/constant upload domain.
    * Staging-only images likewise remain CPU-oriented DMA32 objects.  A
    * staging usage hint cannot move a raster-consumed image out of the
    * vendor's local-memory placement. */
   return templ->target == PIPE_BUFFER ||
          (templ->usage == PIPE_USAGE_STAGING && !(templ->bind & gpu_image)) ?
          0 : PARHELIA_M9_BO_VRAM;
}

static inline uint32_t
m9_resource_row_span(uint32_t width)
{
   return (width + 7) & ~7u;
}

struct m9_render_layout {
   uint32_t row_span;
   uint32_t allocation_height;
   uint64_t size;
   /* The vendor's small resource state.  It is not only an allocation
    * quantum: a small surface is stored by the aperture address calculator,
    * so its descriptor must leave the ordinary tiled bit clear. */
   bool small;
};

/* The CPU surface window mis-places the last row of a surface whose height is
 * odd.  MEASURED on the M9128: row 256/row_span -- the first row of the second
 * 1 KiB page -- comes back holding row height-1 instead, in both storage
 * families and at every width, while the raster engine and the sampler place
 * the very same surface correctly (rtt:64x23 is exact where tex:64x23 is not).
 * A window therefore addresses an even number of rows.  The extra row is never
 * written and never sampled; the allocation reserves it so the kernel's own
 * size contract agrees. */
static inline uint32_t
m9_surface_even_height(uint32_t height)
{
   return (height + 1) & ~1u;
}

static inline bool
m9_render_surface_layout(uint32_t width, uint32_t height, uint32_t cpp,
                         struct m9_render_layout *layout)
{
   bool small = width <= 64 && height <= 64;
   uint32_t height_quantum = small ? (cpp == 4 ? 8 : 4) : 32;
   uint32_t byte_quantum;
   uint64_t elements, tail;

   if (!layout || !width || !height || width > 2048 || height > 2048 ||
       (cpp != 2 && cpp != 4))
      return false;
   /* Lay the surface out for the even height its window addresses. */
   height = m9_surface_even_height(height);
   byte_quantum = cpp == 4 ? (small ? 0x400 : 0x1000) :
                             (small ? 0x200 : 0x800);
   layout->row_span = (width + 31) & ~31u;
   layout->allocation_height =
      (height + height_quantum - 1) & ~(height_quantum - 1);
   if (width <= 16 && height <= 16)
      layout->allocation_height = 16;
   elements = (uint64_t)layout->row_span * layout->allocation_height;
   tail = (uint64_t)(layout->allocation_height - height) *
          layout->row_span + layout->row_span - width;
   if (tail < 8)
      elements += 8 - tail;
   layout->size = (elements * cpp + byte_quantum - 1) &
                  ~(uint64_t)(byte_quantum - 1);
   layout->small = small;
   return true;
}

/* A depth surface at its own row span, which may exceed the colour one. */
static inline bool
m9_depth_surface_layout(uint32_t width, uint32_t height,
                        struct m9_render_layout *layout)
{
   uint32_t span = m9_depth_row_span(width, height);

   return m9_render_surface_layout(span > width ? span : width, height, 4,
                                   layout);
}

struct m9_screen {
   struct pipe_screen base;
   int fd;
   uint32_t chip_id;
   uint32_t uapi_version;
   bool validate_submit;
   bool execute_submit;
   bool native_depth_bias;
   bool wait_point;
   bool aggregate_submit;    /* explicit opt-in plus kernel capability */
   bool queued_submit;       /* M9X_QUEUE=1 plus typed kernel admission */
   /* Actual successful WAIT_POINT observations, shared by contexts and
    * presentation fences. Posting a command never advances this value. */
   p_atomic_uint64_t completed_point;
   simple_mtx_t import_lock;
   struct m9_prime_import *imports;
   /* One inter-pass record serves the whole screen.  The engine runs one job
    * at a time and a record lives only inside one job's raster passes, so a
    * second context needs no second copy -- and at 1920x1200 a copy is tens
    * of MiB, which a third glmark2 scene could not allocate (MEASURED). */
   simple_mtx_t record_lock;
   struct m9_resource record;
   unsigned record_dwords;
   /* The multisample fragment store: a buffer group of its own, sized from
    * the target the probe renders into and shared by the screen the way the
    * inter-pass record is.  Allocated only while a bind index is named. */
   simple_mtx_t faa_store_lock;
   struct m9_resource faa_store, faa_store_aux, faa_companion;
   uint32_t faa_store_width, faa_store_height;
   /* Current mapped main owner only, protected by faa_store_lock. A failed
    * post has no trustworthy final point and forbids CPU readback. */
   uint64_t faa_store_point;
   bool faa_store_dirty, faa_store_failed;
   /* Fixed-fragment database firmware bound through index 69. */
   uint8_t *fragment_db;
   size_t fragment_db_size;
   /* Multisample state of the engine; off until a measurement earns it. */
   struct m9_faa faa;
};

struct m9_fence {
   struct pipe_reference reference;
   uint64_t point;
   int signaled;
};

struct m9_vertex {
   float position[4];
   float color[4];
   float texcoord[2];
};

struct m9_vertex_stream {
   struct m9_vertex *vertices;
   uint32_t count;
   uint32_t capacity;
};

enum m9_program_mode {
   M9_PROGRAM_MODE_NONE,
   M9_PROGRAM_MODE_EXTERNAL,
   M9_PROGRAM_MODE_FIXED_INLINE,
};

struct m9_batch {
   uint32_t command[M9_MAX_COMMAND_DWORDS];
   /* Grown on demand; a batch must be zeroed before its first init. */
   struct drm_parhelia_m9_reloc *relocs;
   struct drm_parhelia_m9_bo_ref bos[M9_MAX_BOS];
   uint32_t command_count;
   uint32_t reloc_count, reloc_capacity;
   uint32_t bo_count;
   uint32_t aggregate_jobs, aggregate_framed, aggregate_span;
   enum m9_program_mode program_mode;
   bool sealed;
};

struct draw_context;
struct m9_query;
struct m9_fs_state;
struct m9_fs_variant;
struct m9_velems;
struct m9_vs_textures;
struct m9_faa_application_upload;
struct m9_context;
bool m9_context_faa_build_draw(struct m9_context *ctx,
                              const struct pipe_draw_info *info,
                              const struct pipe_draw_start_count_bias *draw,
                              struct m9_faa_application_upload *upload,
                              const char **reason);
/* Consume only an authenticated returned COLOR Lock. Copies source F021
 * rows into owned memory, performs genuine Unlock, then writes the retained
 * ordinary target without replacing the active Lock's aperture mapper. */
int m9_context_faa_submit_draw(struct m9_context *ctx,
                               const struct pipe_draw_info *info,
                               const struct pipe_draw_start_count_bias *draw,
                               const char **reason);
int m9_context_faa_flush_frame(struct m9_context *ctx, const char **reason);
int m9_context_faa_present_lock(struct m9_context *ctx, uint64_t graph_id,
                               uint32_t generation, uint64_t lock_point);

/* Pass-level raster state shared by every draw of one two-pass job. */
struct m9_job_key {
   struct m9_resource *depth, *aux;
   const struct m9_fs_state *fs;   /* native fragment program or NULL */
   const struct m9_fs_variant *variant;  /* its program for the blend state */
   uint32_t depth_control, depth_ef, color_mask, clip_min, clip_max;
   bool depth_bias;
   uint32_t depth_bias_units, depth_bias_factor;
   bool stencil;
   uint32_t stencil_a9, stencil_aa;
   /* Raster ac: the write masks of colour targets 2 and 3. */
   uint32_t color_mask_hi;
   /* The job binds colour target 0 alone, whatever the framebuffer holds.
    * The clear quad runs the fixed-function program, whose one colour output
    * reaches target 0: binding the rest would leave the engine writing
    * undefined data into attachments it has no program for. */
   bool single_target;
   /* Native preprocessing program of this job, the c2 image holding it and
    * the viewport its appended tail applies; NULL runs the draw module's
    * transformed records through the captured program. */
   const struct m9_vs_program *vs;
   uint32_t vs_program;
   /* Program slot of the tail-free second head, 0 when the program has at
    * most one varying and needs no second preprocessing draw. */
   uint32_t vs_extra_entry;
   /* Program slot of the route-fed draw that de-interleaves the second
    * head's record into the stream views the raster stage reads. */
   uint32_t vs_gather_entry;
   uint32_t vs_producer_entry;
   /* Stream targets that draw binds: the varying indices past the two the
    * fused pass carries that the fragment program reads, so a varying
    * nothing reads costs no target. */
   uint32_t vs_extra_targets;
   /* The fused pass binds its own fourth output target for varying 1. */
   bool vs_fused_pair;
   float viewport_scale[3], viewport_bias[3];
   /* Separate bounded vertex sampler snapshot, part of the job state key.
    * Its resource stays retained through queued submission/retirement. */
   bool vs_texture;
   struct m9_texture_unit vs_unit;
   struct pipe_resource *vs_unit_res;
   bool vs_texture_second;
   struct m9_texture_unit vs_unit_second;
   struct pipe_resource *vs_unit_res_second;
   /* One entry a texture slot of every raster pass: a shader split across
    * passes binds four in each of them. */
   struct m9_texture_unit units[M9_DRAW_MAX_UNITS];
   struct pipe_resource *unit_res[M9_DRAW_MAX_UNITS];
   /* Content over descriptor size of a sampled render target, s and t. */
   float unit_scale[M9_DRAW_MAX_UNITS][2];
   /* A coordinate the shader computed affinely from a varying, resolved
    * against the constant buffer and applied per vertex.  The third term
    * is the comparison reference of a shadow fetch, the fourth the
    * projector. */
   float coord_scale[M9_DRAW_MAX_UNITS][4];
   float coord_bias[M9_DRAW_MAX_UNITS][4];
   unsigned unit_count;
};

/* Everything m9_submit_job consumes of one pending job, so that a job can be
 * held aside while another is built and submitted. */
struct m9_job_state {
   struct m9_job_key key;
   struct util_dynarray records, indices, draws, extra;
   struct util_dynarray job_textures, job_consts, job_vs_consts;
   struct util_dynarray job_vs_runs, job_vs_values;
   unsigned record_floats, job_unit_dwords;
};

/* A multi-pass job whose geometry covers part of its render target stops the
 * engine, so it is held here until the driver is outside the draw module and
 * can render it into a scratch surface of the pixels it does cover -- which
 * it covers whole -- and copy that into place. */
struct m9_deferred_job {
   bool pending;
   struct pipe_resource *dst;       /* reference held */
   uint32_t x, y, width, height;    /* the covered box, rows from the top */
   uint32_t target_height;          /* the height its records were made in */
   struct m9_job_state state;
};

/* A scratch colour surface, kept per shape: a split job renders into one of
 * exactly the size of the box it covers. */
struct m9_scratch_target {
   struct pipe_resource *res;
   uint64_t used;
};

/* Enough for a few window-sized boxes; beyond that a shape is one the context
 * has stopped drawing. */
#define M9_SCRATCH_BUDGET (24u << 20)

/* Per-context state is complete rather than inherited from the classic
 * Parhelia shadow.  Dirty bits describe only M9 packet groups. */
/* A vertex shader: the draw module's copy, which every draw still runs, and
 * the native preprocessing program when the backend covered it. */
/* The program compiled for one output-stream layout.  A textured job asks
 * for the coordinate streams its fragment variant reads, so the layout is a
 * property of the pair rather than of the vertex shader, and a shader drawn
 * with several fragment programs keeps one of these per layout. */
struct m9_vs_variant {
   struct m9_vs_outputs outputs;
   struct m9_vs_program program;
   const char *reason;
   bool ok;
   bool gpu_padding;
   /* Never reused, so a cached c2 image can name the program it holds
    * without a pointer that a later allocation could match. */
   uint64_t serial;
};

/* The shader compiled against one set of inlinable uniform values.  Neither
 * backend has control flow, so a loop whose bound is a uniform only reaches
 * the engine once that value is substituted and the loop unrolls; the values
 * are fixed per draw, so one of these is kept per set the context sends. */
struct m9_vs_spec {
   struct m9_vs_format_key format_key;
   struct m9_vs_gate_key gate_key;
   uint32_t values[MAX_INLINABLE_UNIFORMS];
   /* The lowered, specialised clone the backend consumed, kept so a further
    * layout can be compiled from it. */
   struct nir_shader *nir;
   struct m9_vs_program program;
   const char *reason;
   /* What the compiled program cost, kept after a refusal discards it so the
    * debug line can state the length and the span a bound turned away. */
   unsigned slots, consts, literals, uniforms;
   bool native;
   struct util_dynarray variants;   /* struct m9_vs_variant * */
};

struct m9_vs_state {
   void *draw;
   /* The lowered clone before specialisation, and the dwords the values
    * that specialise it come from. */
   struct nir_shader *src;
   struct m9_vs_uniform_gates uniform_gates;
   unsigned inline_count;
   struct util_dynarray specs;      /* struct m9_vs_spec * */
   /* The specialisation the draws being emitted run; never NULL once a
    * draw has selected one. */
   struct m9_vs_spec *cur;
   uint64_t serial;
};

/* Four posted readers and one CPU builder. The active set remains in the
 * context fields; the four parked sets retain their last real fence point. */
struct m9_job_arena {
   struct m9_resource source, system, index, cpu_streams;
   uint64_t point;
};

/* Queued code is immutable while either a recorded or posted job names it. */
struct m9_native_image {
   uint64_t serial;
   struct m9_resource image;
};

struct m9_context {
   struct pipe_context base;
   struct m9_screen *screen;
   struct m9_batch batch;
   struct m9_batch *aggregate;
   struct util_dynarray aggregate_resources; /* retained pipe_resource refs */
   struct m9_resource *aggregate_be[4];
   struct m9_resource command;
   uint32_t context_id;
   uint64_t last_point;
   uint64_t dirty;
   struct pipe_resource *color;
   struct m9_faa_surface faa_surface;
   struct m9_faa_application_frame faa_frame;
   /* Ordinary clear does not reset this graph's fragment/HZ state. Preserve
    * the actual requested state until a qualified native frame-reset API. */
   struct {
      bool pending, scissored;
      unsigned buffers;
      uint32_t color_mask;
      uint8_t stencil_mask;
      union pipe_color_union color;
      double depth;
      unsigned stencil;
      struct pipe_scissor_state scissor;
   } faa_application_clear;
   /* Byte offset of the bound colour level inside that object; a mip level
    * above zero is a render surface of its own at its chain offset. */
   uint64_t color_offset;
   /* Colour attachments past the first: the resource, its b1 descriptor and
    * its render format class, one a target.  color_count counts them all,
    * target 0 included.  M9_DRAW_MAX_COLOR is m9_draw.h's, which cannot be
    * included here: it includes m9_batch.h, which this header defines.
    * docs/parhelia-m9-mrt-routing.md */
   struct pipe_resource *color_extra[M9_MAX_COLOR - 1];
   uint32_t color_extra_desc[M9_MAX_COLOR - 1];
   uint8_t color_class[M9_MAX_COLOR];
   uint8_t color_format[M9_MAX_COLOR];
   unsigned color_count;
   struct pipe_resource *depth;
   /* Texture units referenced by the pending job. */
   struct pipe_resource *textures[M9_TEXTURE_MAX_UNITS];
   uint32_t framebuffer_width, framebuffer_height;
   bool depth_test;
   bool depth_write;
   bool lost;

   /* Vertex processing runs on the CPU in the Gallium draw module. */
   struct draw_context *draw;
   /* Copies and blits run on the raster back end as a textured quad. */
   struct blitter_context *blitter;
   struct pipe_framebuffer_state framebuffer;
   struct pipe_viewport_state viewport;
   struct m9_velems *velems;
   const struct pipe_rasterizer_state *rasterizer;
   const struct pipe_depth_stencil_alpha_state *dsa;
   struct pipe_stencil_ref stencil_ref;
   const struct pipe_blend_state *blend;
   struct pipe_blend_color blend_color;  /* resolved into fragment constants */
   /* Blending into several colour targets draws one target a pass: 1 + the
    * target being drawn, and the states that pass binds. */
   unsigned mrt_select;
   struct pipe_blend_state mrt_blend;
   struct pipe_depth_stencil_alpha_state mrt_dsa;
   struct pipe_scissor_state scissor;
   struct m9_fs_state *fs;
   /* The handle the state tracker bound, which a draw stage may wrap. */
   void *fs_handle;
   bool lines;          /* the primitive being recorded is a line */
   void (*bind_fs_wrapped)(struct pipe_context *, void *);
   struct m9_vs_state *vs;
   struct pipe_vertex_buffer vertex_buffers[PIPE_MAX_ATTRIBS];
   unsigned num_vertex_buffers;
   struct pipe_constant_buffer vs_constants[PIPE_MAX_CONSTANT_BUFFERS];
   struct pipe_constant_buffer fs_constants;
   uint32_t fs_inline[MAX_INLINABLE_UNIFORMS];  /* fragment inlinable uniforms */
   uint32_t vs_inline[MAX_INLINABLE_UNIFORMS];  /* vertex inlinable uniforms */
   struct pipe_sampler_view *fs_views[PIPE_MAX_SAMPLERS];
   const struct pipe_sampler_state *fs_samplers[PIPE_MAX_SAMPLERS];
   /* Vertex-stage sampling, done by the draw module (m9_vs_texture.c). */
   struct m9_vs_textures *vs_textures;

   /* Fixed two-pass resources and the pending job. */
   struct m9_resource system, program, fragment_db, transient, be, source;
   /* Clip window the engine state object was last cleared for. */
   uint32_t be_clip;
   /* The four arenas the CPU fills per job are double buffered in legacy
    * mode. An
    * asynchronous submit posts its job and returns, so the engine is still
    * reading the previous job's records, indices, constants and streams
    * while the next job is built; holding a kernel reference to the object
    * does not stop the CPU overwriting what is inside it.  The kernel
    * drains the posted job at the *next* submission, so by the time a
    * second job has been submitted the first has retired and its set is
    * free: two sets are enough, and alternating them is exact rather than
    * a guess about timing. Opted-in queue mode adds three parked sets and
    * waits each set's real point before reuse: four readers + one builder. */
   struct m9_resource system_alt, source_alt, index_alt, cpu_streams_alt;
   struct m9_job_arena queue_arena[3];
   uint64_t arena_point, arena_alt_point;
   unsigned arena_cursor;
   /* The raster index run, in its own object: a draw range is a raw bus
    * address, so only this has to be physically contiguous, and keeping it
    * out of the record arena keeps that demand small. */
   struct m9_resource index;
   /* The CPU-written stream views, in system memory: the raster engine
    * fetches a stream from either address class, and the host aperture
    * takes writes at 265 MB/s against 1380 MB/s to system memory. */
   struct m9_resource cpu_streams;
   /* The depth and HZ pair a target without a depth attachment of its own
    * binds, kept per size: a scene that alternates target sizes would
    * otherwise reallocate and refill them on every switch. */
   struct m9_resource *target_depth, *target_aux, *target_be;
   struct util_dynarray target_depths;  /* struct m9_target_depth * */
   struct m9_target_depth *target_entry;  /* the one bound, for its be pair */
   struct m9_be_shape be_shape;       /* of ctx->be, when no entry owns one */
   uint64_t target_clock;
   struct m9_resource placeholder_texture;  /* 1x1 unit-0 binding */
   bool record_written;               /* a split job ran since the last flush */
   /* Pass-through fragment slots with a native vertex program behind them,
    * for a draw whose fragment shader keeps the captured colour image. */
   struct m9_resource vs_c2;
   uint64_t vs_c2_serial;
   struct util_dynarray vs_c2_images;  /* struct m9_native_image */
   /* The query whose range this job falls inside, and the snapshots its
    * stream opens and closes with. */
   struct m9_query *query_active, *query_begin, *query_end;
   /* A query paused around work that passes no samples, and the one whose
    * range that work falls inside: its jobs are bracketed for subtraction. */
   struct m9_query *query_paused, *query_exclude;
   unsigned query_exclude_slot;
   struct m9_draw_params params;
   struct m9_vertex_decl decl;
   bool params_valid;
   /* The colour surface is scratch for a depth-only framebuffer. */
   bool color_discard;
   struct m9_job_key job_key;
   /* The split job held for the scratch path, the surfaces it renders into
    * and a guard against running one inside another. */
   struct m9_deferred_job deferred;
   struct util_dynarray scratch_targets;  /* struct m9_scratch_target * */
   uint64_t scratch_clock;
   bool in_scratch;
   bool scratch_bind;   /* the target being bound is a scratch surface */
   /* A colour level the engine cannot render in place renders into a tiled
    * proxy that is seeded from and written back to that level of `tex`. */
   struct {
      struct pipe_resource *tex, *scratch;
      unsigned level, layer;
      uint64_t generation;  /* of tex when the proxy last matched it */
      bool bound, busy;
   } proxy;
   struct util_dynarray records;  /* float source records */
   struct util_dynarray indices;  /* uint16 raster indices into records */
   struct util_dynarray draws;    /* struct m9_draw_record */
   struct util_dynarray extra;    /* float4 CPU varyings per source record */
   struct util_dynarray key_consts, job_consts;  /* raster constant floats */
   /* Evicted fragment variants a pending job may still name; freed at sync. */
   struct util_dynarray retired_fs_variants;
   /* Preprocessing constant pairs, two floats each, in program order. */
   struct util_dynarray key_vs_consts, job_vs_consts;
   /* One snapshot per changed native constant span, owned by this job. */
   struct util_dynarray job_vs_runs, job_vs_values;
   /* Floats per source record of the pending job. */
   unsigned record_floats;
   /* References held for every texture the pending job binds, and the stream
    * budget its restated unit blocks have already claimed. */
   struct util_dynarray job_textures;  /* struct pipe_resource * */
   /* Welded unindexed draws, kept across frames.  struct m9_weld_entry *. */
   struct util_dynarray weld_cache;
   uint64_t weld_clock;
   unsigned job_unit_dwords;
   uint32_t warned;
   uint32_t skipped_draws;      /* dropped for an unimplemented state */
   uint32_t engine_blits;       /* copies and blits the raster engine drew */
   uint32_t engine_clears;      /* render-target clears queued on the engine */
   uint32_t padded_draws;       /* partial split draws padded to the extent */
   bool queries_enabled;        /* pipe set_active_query_state, recorded */
   uint32_t submitted_draws;
   uint32_t native_vs_draws, native_vs_fallbacks;
   /* Subsets of the aggregate counters, identified only while the actual
    * driver-owned util_blitter operation holds its running flag. */
   uint32_t native_vs_blitter_draws, native_vs_blitter_fallbacks;
   /* Last successfully submitted packed-record producer, for opt-in proof. */
   uint32_t vs_record_vertices, vs_record_fields, vs_record_view;
   uint32_t vs_record_job_seq;
   uint32_t vs_record_source, vs_record_index, vs_record_program;
   uint64_t vs_record_point;
   bool vs_record_inputs_dumped;
   /* Why the native vertex path refused, one entry a reason: the reasons
    * are static strings, so the pointer identifies them. */
   struct util_dynarray native_vs_reasons;
   /* Kernel window traffic: the de-tiling copies cost a host-aperture
    * access per row, so count both directions and their volume. */
   uint32_t window_reads, window_writes;
   uint64_t window_read_bytes, window_write_bytes;
   uint32_t map_usage_seen;     /* OR of every texture transfer usage */
   uint32_t window_read_usage;  /* OR of the usage of every windowed read */
   uint32_t window_reads_explicit;  /* reads a PIPE_MAP_READ asked for */
   /* Draws dropped per distinct reason; the strings are literals, so the
    * pointer identifies the reason and the table needs no allocation. */
   struct {
      const char *reason;
      uint32_t draws;
   } skip_reason[M9_MAX_SKIP_REASONS];
   unsigned skip_reasons;
   bool debug;
   const char *dump_path;   /* M9X_DUMP: append every job's words */
   bool dry_run;            /* M9X_DRYRUN: validate jobs, never execute */
   unsigned dump_seq;
   unsigned dump_native_seq;
   bool dump_native_valid;
   unsigned dump_from, dump_count;
   unsigned frame_jobs, only_first, only_after;
   unsigned split_state, split_uniforms, split_vertices, split_stream;
   unsigned flush_count;
   /* Draw totals at the last frame boundary, so a frame's own skip rate can
    * be reported without a second set of counters. */
   uint32_t frame_draws_base, frame_skipped_base;
   unsigned skipped_frames;   /* consecutive frames over the skip threshold */
   unsigned split_target, split_program, split_depth, split_mask, split_clip;
};

#endif
