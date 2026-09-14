/* SPDX-License-Identifier: MIT */
#ifndef M9_INTERNAL_H
#define M9_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "m9x_drm.h"

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
   struct m9_mip_layout mip;
   void *map;
};

struct m9_screen {
   struct pipe_screen base;
   int fd;
   uint32_t chip_id;
   uint32_t uapi_version;
   bool validate_submit;
   bool execute_submit;
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

struct m9_batch {
   uint32_t command[M9_MAX_COMMAND_DWORDS];
   struct drm_parhelia_m9_reloc relocs[M9_MAX_RELOCS];
   struct drm_parhelia_m9_bo_ref bos[M9_MAX_BOS];
   uint32_t command_count;
   uint32_t reloc_count;
   uint32_t bo_count;
   bool sealed;
};

/* Per-context state is complete rather than inherited from the classic
 * Parhelia shadow.  Dirty bits describe only M9 packet groups. */
struct m9_context {
   struct pipe_context base;
   struct m9_screen *screen;
   struct m9_batch batch;
   struct m9_vertex_stream vertex_stream;
   uint32_t context_id;
   uint64_t last_point;
   uint64_t dirty;
   struct m9_resource *color;
   struct m9_resource *depth;
   struct m9_resource *textures[4];
   uint32_t framebuffer_width, framebuffer_height;
   bool depth_test;
   bool depth_write;
   bool lost;
};

#endif
