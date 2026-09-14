/* SPDX-License-Identifier: MIT */
#ifndef M9_BATCH_H
#define M9_BATCH_H

#include "m9_internal.h"

enum m9_primitive {
   M9_PRIM_POINTS,
   M9_PRIM_LINES,
   M9_PRIM_LINE_STRIP,
   M9_PRIM_TRIANGLES,
   M9_PRIM_TRIANGLE_STRIP,
   M9_PRIM_TRIANGLE_FAN,
};

struct m9_primitive_info {
   uint8_t frontend_selector;
   uint16_t raster_control;
   uint32_t vertex_count;
   bool raster_control_proved;
};

struct m9_surface_desc {
   uint32_t row_span;
   uint32_t height;
   uint32_t layers;
   uint8_t layout;
   uint8_t format;
   bool address_offset;
   bool resource_layout;
};

struct m9_sampler_desc {
   unsigned min_filter, mag_filter, mip_filter;
   unsigned wrap_s, wrap_t, wrap_r;
   unsigned anisotropy;
   unsigned compiler_input;
   int lod_bias_1_256;
   unsigned first_level, last_level;
};

struct m9_linked_program {
   uint32_t resource_69_handle;
   uint64_t resource_69_offset;
   uint32_t resource_6c_handle;
   uint64_t resource_6c_offset;
   uint32_t control_6a;
   uint32_t entry_6b;
   bool captured_gltri;
};

enum m9_vertex_input_format {
   M9_VERTEX_FLOAT32,
   M9_VERTEX_UNORM8,
};

struct m9_vertex_input {
   const void *data;
   uint32_t stride;
   uint32_t offset;
   uint8_t components;
   enum m9_vertex_input_format format;
};

void m9_batch_init(struct m9_batch *batch);
int m9_batch_emit_regs(struct m9_batch *batch, const uint8_t *indices,
                       const uint32_t *values, unsigned count);
int m9_batch_add_reloc(struct m9_batch *batch, uint32_t command_dword,
                       uint32_t handle, uint64_t offset,
                       enum drm_parhelia_m9_reloc_kind kind,
                       unsigned access);
int m9_batch_bind_surface(struct m9_batch *batch, uint8_t address_index,
                          uint32_t handle, uint64_t offset,
                          enum drm_parhelia_m9_reloc_kind kind,
                          const struct m9_surface_desc *desc);
int m9_batch_bind_texture(struct m9_batch *batch, uint32_t handle,
                          uint64_t offset, const struct m9_surface_desc *desc,
                          const struct m9_sampler_desc *sampler);
int m9_batch_bind_program(struct m9_batch *batch,
                          const struct m9_linked_program *program);
int m9_batch_emit_triangle_raster(struct m9_batch *batch, unsigned width,
                                  unsigned height, bool depth_test,
                                  bool depth_write);
int m9_batch_emit_draw32(struct m9_batch *batch, uint32_t handle,
                         uint64_t offset, uint32_t count, uint32_t flags);
int m9_batch_emit_draw16(struct m9_batch *batch, uint32_t handle,
                         uint64_t offset, uint32_t count, uint32_t flags);
int m9_batch_emit_draw_numeric(struct m9_batch *batch, uint32_t start,
                               uint32_t count, uint32_t flags);
int m9_primitive_map(enum m9_primitive primitive, uint32_t primitive_count,
                     struct m9_primitive_info *out);
uint32_t m9_surface_desc0(const struct m9_surface_desc *desc);
uint32_t m9_surface_desc1(const struct m9_surface_desc *desc);
uint32_t m9_sampler_control(const struct m9_sampler_desc *desc);
uint32_t m9_sampler_lod(const struct m9_sampler_desc *desc);
int m9_mip_layout_init(struct m9_mip_layout *layout, uint32_t width,
                       uint32_t height, uint32_t depth, unsigned levels,
                       unsigned cpp);
int m9_repack_vertices(struct m9_vertex *dst, unsigned dst_capacity,
                       const struct m9_vertex_input *position,
                       const struct m9_vertex_input *color,
                       const struct m9_vertex_input *texcoord,
                       unsigned first, unsigned count);
int m9_batch_prepare_submit(const struct m9_batch *batch,
                            const struct m9_resource *command,
                            uint32_t context_id,
                            struct drm_parhelia_m9_submit *submit);
int m9_batch_prepare_execute(const struct m9_batch *batch,
                             const struct m9_resource *command,
                             uint32_t context_id,
                             struct drm_parhelia_m9_submit *submit);
int m9_batch_validate(struct m9_screen *screen, struct m9_resource *command,
                      const struct m9_batch *batch, uint32_t context_id);
int m9_batch_submit(struct m9_screen *screen, struct m9_resource *command,
                    const struct m9_batch *batch, uint32_t context_id,
                    uint64_t *out_point);
int m9_submit_noop_fence(struct m9_screen *screen, uint64_t *out_point);
int m9_submit_first_triangle(struct m9_screen *screen, uint32_t color_handle,
                             uint64_t *out_point);

#endif
