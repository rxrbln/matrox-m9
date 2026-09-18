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
#include "drm-uapi/m9x_drm.h"
#include "m9_draw_params.h"
#include "m9_vs_compile.h"
#include "m9_texture.h"
#include "util/simple_mtx.h"
#include "util/u_dynarray.h"

#define M9_PCI_ID 0x0540u
#define M9_PACKET_STOP 0x45u
#define M9_MAX_COMMAND_DWORDS 8192u
/* Relocation sites one submission may present.  The kernel takes at most
 * one a command dword, so the command image is the only real bound; the
 * batch grows its array and never refuses a job the stream still fits. */
#define M9_MAX_RELOCS M9_MAX_COMMAND_DWORDS
/* Objects one submission may reference; the kernel's own ceiling. */
#define M9_MAX_BOS 128u
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

struct m9_prime_import {
   struct m9_prime_import *next;
   uint32_t handle;
   uint32_t references;
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
   uint32_t texture_unplaced;  /* levels whose content could not be stored */
   uint32_t levels_written;    /* levels a transfer has actually written */
   bool bind_logged;
   void *map;
   /* Hierarchical-Z metadata owned by a D24S8 depth image. */
   struct m9_resource *hz;
};

struct m9_target_depth {
   uint32_t width, height;
   uint64_t used;
   struct m9_resource depth, aux;
   /* The engine's own back-end object, which belongs to a target rather than
    * to the context: a shape revisited after another finds its own. */
   struct m9_resource be;
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

struct m9_screen {
   struct pipe_screen base;
   int fd;
   uint32_t chip_id;
   uint32_t uapi_version;
   bool validate_submit;
   bool execute_submit;
   simple_mtx_t import_lock;
   struct m9_prime_import *imports;
   /* One inter-pass record serves the whole screen.  The engine runs one job
    * at a time and a record lives only inside one job's raster passes, so a
    * second context needs no second copy -- and at 1920x1200 a copy is tens
    * of MiB, which a third glmark2 scene could not allocate (MEASURED). */
   simple_mtx_t record_lock;
   struct m9_resource record;
   unsigned record_dwords;
   /* Fixed-fragment database firmware bound through index 69. */
   uint8_t *fragment_db;
   size_t fragment_db_size;
};

struct m9_fence {
   struct pipe_reference reference;
   uint64_t point;
   bool signaled;
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
   enum m9_program_mode program_mode;
   bool sealed;
};

struct draw_context;
struct m9_fs_state;
struct m9_fs_variant;
struct m9_velems;
struct m9_vs_textures;

/* Pass-level raster state shared by every draw of one two-pass job. */
struct m9_job_key {
   struct m9_resource *depth, *aux;
   const struct m9_fs_state *fs;   /* native fragment program or NULL */
   const struct m9_fs_variant *variant;  /* its program for the blend state */
   uint32_t depth_control, depth_ef, color_mask, clip_min, clip_max;
   /* Native preprocessing program of this job, the c2 image holding it and
    * the viewport its appended tail applies; NULL runs the draw module's
    * transformed records through the captured program. */
   const struct m9_vs_program *vs;
   uint32_t vs_program;
   float viewport_scale[3], viewport_bias[3];
   /* One entry a texture slot of every raster pass: a shader split across
    * passes binds four in each of them. */
   struct m9_texture_unit units[M9_DRAW_MAX_UNITS];
   struct pipe_resource *unit_res[M9_DRAW_MAX_UNITS];
   /* Content over descriptor size of a sampled render target, s and t. */
   float unit_scale[M9_DRAW_MAX_UNITS][2];
   /* A coordinate the shader computed affinely from a varying, resolved
    * against the constant buffer and applied per vertex.  The third term
    * is the comparison reference of a shadow fetch. */
   float coord_scale[M9_DRAW_MAX_UNITS][3];
   float coord_bias[M9_DRAW_MAX_UNITS][3];
   unsigned unit_count;
};

/* Per-context state is complete rather than inherited from the classic
 * Parhelia shadow.  Dirty bits describe only M9 packet groups. */
/* A vertex shader: the draw module's copy, which every draw still runs, and
 * the native preprocessing program when the backend covered it. */
struct m9_vs_state {
   void *draw;
   struct m9_vs_program program;
   const char *reason;
   bool native;
   /* Never reused, so a cached c2 image can name the program it holds
    * without a pointer that a later allocation could match. */
   uint64_t serial;
};

struct m9_context {
   struct pipe_context base;
   struct m9_screen *screen;
   struct m9_batch batch;
   struct m9_resource command;
   uint32_t context_id;
   uint64_t last_point;
   uint64_t dirty;
   struct pipe_resource *color;
   struct pipe_resource *depth;
   /* Texture units referenced by the pending job. */
   struct pipe_resource *textures[M9_TEXTURE_MAX_UNITS];
   uint32_t framebuffer_width, framebuffer_height;
   bool depth_test;
   bool depth_write;
   bool lost;

   /* Vertex processing runs on the CPU in the Gallium draw module. */
   struct draw_context *draw;
   struct pipe_framebuffer_state framebuffer;
   struct pipe_viewport_state viewport;
   struct m9_velems *velems;
   const struct pipe_rasterizer_state *rasterizer;
   const struct pipe_depth_stencil_alpha_state *dsa;
   const struct pipe_blend_state *blend;
   struct pipe_scissor_state scissor;
   struct m9_fs_state *fs;
   struct m9_vs_state *vs;
   struct pipe_vertex_buffer vertex_buffers[PIPE_MAX_ATTRIBS];
   unsigned num_vertex_buffers;
   struct pipe_constant_buffer vs_constants[PIPE_MAX_CONSTANT_BUFFERS];
   struct pipe_constant_buffer fs_constants;
   uint32_t fs_inline[MAX_INLINABLE_UNIFORMS];  /* fragment inlinable uniforms */
   struct pipe_sampler_view *fs_views[PIPE_MAX_SAMPLERS];
   const struct pipe_sampler_state *fs_samplers[PIPE_MAX_SAMPLERS];
   /* Vertex-stage sampling, done by the draw module (m9_vs_texture.c). */
   struct m9_vs_textures *vs_textures;

   /* Fixed two-pass resources and the pending job. */
   struct m9_resource system, program, fragment_db, transient, be, source;
   /* Clip window the engine state object was last cleared for. */
   uint32_t be_clip;
   /* The four arenas the CPU fills per job are double buffered.  An
    * asynchronous submit posts its job and returns, so the engine is still
    * reading the previous job's records, indices, constants and streams
    * while the next job is built; holding a kernel reference to the object
    * does not stop the CPU overwriting what is inside it.  The kernel
    * drains the posted job at the *next* submission, so by the time a
    * second job has been submitted the first has retired and its set is
    * free: two sets are enough, and alternating them is exact rather than
    * a guess about timing. */
   struct m9_resource system_alt, source_alt, index_alt, cpu_streams_alt;
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
   uint64_t target_clock;
   struct m9_resource placeholder_texture;  /* 1x1 unit-0 binding */
   bool record_written;               /* a split job ran since the last flush */
   /* Pass-through fragment slots with a native vertex program behind them,
    * for a draw whose fragment shader keeps the captured colour image. */
   struct m9_resource vs_c2;
   uint64_t vs_c2_serial;
   struct m9_draw_params params;
   struct m9_vertex_decl decl;
   bool params_valid;
   struct m9_job_key job_key;
   struct util_dynarray records;  /* float source records */
   struct util_dynarray indices;  /* uint16 raster indices into records */
   struct util_dynarray draws;    /* struct m9_draw_record */
   struct util_dynarray extra;    /* float4 CPU varyings per source record */
   struct util_dynarray key_consts, job_consts;  /* raster constant floats */
   /* Preprocessing constant pairs, two floats each, in program order. */
   struct util_dynarray key_vs_consts, job_vs_consts;
   /* Floats per source record of the pending job. */
   unsigned record_floats;
   /* References held for every texture the pending job binds, and the stream
    * budget its restated unit blocks have already claimed. */
   struct util_dynarray job_textures;  /* struct pipe_resource * */
   unsigned job_unit_dwords;
   uint32_t warned;
   uint32_t skipped_draws;      /* dropped for an unimplemented state */
   uint32_t submitted_draws;
   uint32_t native_vs_draws, native_vs_fallbacks;
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
   unsigned dump_from, dump_count;
   unsigned frame_jobs, only_first, only_after;
   unsigned split_state, split_uniforms, split_vertices, split_stream;
   unsigned flush_count;
   unsigned split_target, split_program, split_depth, split_mask, split_clip;
};

#endif
