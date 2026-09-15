/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_INTERNAL_H
#define M9_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "drm-uapi/m9x_drm.h"
#include "m9_draw_params.h"
#include "util/simple_mtx.h"
#include "util/u_dynarray.h"

#define M9_PCI_ID 0x0540u
#define M9_PACKET_STOP 0x45u
#define M9_MAX_COMMAND_DWORDS 8192u
#define M9_MAX_RELOCS 128u
#define M9_MAX_BOS 64u
#define M9_MAX_MIP_LEVELS 13u

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
   void *map;
   /* Hierarchical-Z metadata owned by a D24S8 depth image. */
   struct m9_resource *hz;
};

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
   struct drm_parhelia_m9_reloc relocs[M9_MAX_RELOCS];
   struct drm_parhelia_m9_bo_ref bos[M9_MAX_BOS];
   uint32_t command_count;
   uint32_t reloc_count;
   uint32_t bo_count;
   enum m9_program_mode program_mode;
   bool sealed;
};

struct draw_context;
struct m9_fs_state;

/* Pass-level raster state shared by every draw of one two-pass job. */
struct m9_job_key {
   struct m9_resource *depth, *aux;
   uint32_t depth_control, depth_ef, color_mask, clip_min, clip_max;
};

/* Per-context state is complete rather than inherited from the classic
 * Parhelia shadow.  Dirty bits describe only M9 packet groups. */
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
   struct pipe_resource *textures[4];
   uint32_t framebuffer_width, framebuffer_height;
   bool depth_test;
   bool depth_write;
   bool lost;

   /* Vertex processing runs on the CPU in the Gallium draw module. */
   struct draw_context *draw;
   struct pipe_framebuffer_state framebuffer;
   const struct pipe_rasterizer_state *rasterizer;
   const struct pipe_depth_stencil_alpha_state *dsa;
   const struct pipe_blend_state *blend;
   struct pipe_scissor_state scissor;
   struct m9_fs_state *fs;
   struct pipe_vertex_buffer vertex_buffers[PIPE_MAX_ATTRIBS];
   unsigned num_vertex_buffers;
   struct pipe_constant_buffer vs_constants[PIPE_MAX_CONSTANT_BUFFERS];

   /* Fixed two-pass resources and the pending job. */
   struct m9_resource system, program, fragment_db, transient, be, source;
   struct m9_resource target_depth, target_aux;
   struct m9_draw_params params;
   struct m9_vertex_decl decl;
   bool params_valid;
   struct m9_job_key job_key;
   struct util_dynarray records;  /* float source records */
   struct util_dynarray draws;    /* struct m9_draw_record */
   uint32_t warned;
   bool debug;
};

#endif
