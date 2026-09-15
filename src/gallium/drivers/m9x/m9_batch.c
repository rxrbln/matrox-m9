/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include "m9_batch.h"
#include "m9_resource69_inline.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/ioctl.h>

#include "util/macros.h"

static uint32_t
m9_align(uint32_t value, uint32_t alignment)
{
   return (value + alignment - 1) & ~(alignment - 1);
}

static bool
m9_header_is_draw(uint32_t header)
{
   uint32_t launch = header & 0x60000000u;
   uint32_t base = header & ~0x60000000u;

   return (launch == 0x20000000u || launch == 0x60000000u) &&
          (base == 0x03000001u || base == 0x06000001u ||
           base == 0x07000001u);
}

void
m9_batch_checkpoint(const struct m9_batch *batch,
                    struct m9_batch_checkpoint *checkpoint)
{
   checkpoint->command_count = batch->command_count;
   checkpoint->reloc_count = batch->reloc_count;
   checkpoint->bo_count = batch->bo_count;
   checkpoint->program_mode = batch->program_mode;
   for (unsigned i = 0; i < batch->bo_count; i++)
      checkpoint->bo_flags[i] = batch->bos[i].flags;
}

void
m9_batch_rollback(struct m9_batch *batch,
                  const struct m9_batch_checkpoint *checkpoint)
{
   batch->command_count = checkpoint->command_count;
   batch->reloc_count = checkpoint->reloc_count;
   batch->bo_count = checkpoint->bo_count;
   batch->program_mode = checkpoint->program_mode;
   for (unsigned i = 0; i < checkpoint->bo_count; i++)
      batch->bos[i].flags = checkpoint->bo_flags[i];
}

void
m9_batch_init(struct m9_batch *batch)
{
   memset(batch, 0, sizeof(*batch));
}

int
m9_batch_emit_regs(struct m9_batch *batch, const uint8_t *indices,
                   const uint32_t *values, unsigned count)
{
   unsigned first = 0;
   unsigned packets = 0;

   if (!batch || !indices || !values || !count || batch->sealed)
      return -EINVAL;
   for (unsigned i = 0; i < count; i++)
      if (indices[i] == M9_PACKET_STOP)
         return -EINVAL;

   while (first < count) {
      unsigned n = count - first > 4 ? 4 : count - first;
      uint32_t header = 0x45454545u;

      for (unsigned i = 0; i < n; i++) {
         header &= ~(0xffu << (8 * i));
         header |= (uint32_t)indices[first + i] << (8 * i);
      }
      if (n == 4 && m9_header_is_draw(header))
         n = 3;
      packets++;
      first += n;
   }
   if (batch->command_count + count + packets > M9_MAX_COMMAND_DWORDS)
      return -ENOSPC;

   first = 0;
   while (first < count) {
      unsigned n = count - first > 4 ? 4 : count - first;
      uint32_t header = 0x45454545u;

      for (unsigned i = 0; i < n; i++) {
         header &= ~(0xffu << (8 * i));
         header |= (uint32_t)indices[first + i] << (8 * i);
      }
      if (n == 4 && m9_header_is_draw(header)) {
         n = 3;
         header = (header & 0x00ffffffu) | 0x45000000u;
      }
      batch->command[batch->command_count++] = header;
      for (unsigned i = 0; i < n; i++)
         batch->command[batch->command_count++] = values[first + i];
      first += n;
   }
   return 0;
}

static int
m9_batch_add_bo(struct m9_batch *batch, uint32_t handle, unsigned access)
{
   if (!handle || access & ~(PARHELIA_M9_BO_READ | PARHELIA_M9_BO_WRITE))
      return -EINVAL;
   for (unsigned i = 0; i < batch->bo_count; i++) {
      if (batch->bos[i].handle == handle) {
         batch->bos[i].flags |= access;
         return 0;
      }
   }
   if (batch->bo_count == M9_MAX_BOS)
      return -ENOSPC;
   batch->bos[batch->bo_count++] = (struct drm_parhelia_m9_bo_ref) {
      .handle = handle,
      .flags = access,
   };
   return 0;
}

int
m9_batch_add_reloc(struct m9_batch *batch, uint32_t command_dword,
                   uint32_t handle, uint64_t offset,
                   enum drm_parhelia_m9_reloc_kind kind, unsigned access)
{
   const unsigned access_mask = PARHELIA_M9_BO_READ | PARHELIA_M9_BO_WRITE;
   unsigned bo_access = access & access_mask;
   int ret;

   if (!batch || command_dword >= batch->command_count ||
       kind > PARHELIA_M9_RELOC_STREAM || batch->sealed || !bo_access ||
       access & ~(access_mask | PARHELIA_M9_RELOC_PACKED16_ODD) ||
       ((access & PARHELIA_M9_RELOC_PACKED16_ODD) &&
        kind != PARHELIA_M9_RELOC_DRAW16_END))
      return -EINVAL;
   if (batch->reloc_count == M9_MAX_RELOCS)
      return -ENOSPC;
   ret = m9_batch_add_bo(batch, handle, bo_access);
   if (ret)
      return ret;
   batch->relocs[batch->reloc_count++] = (struct drm_parhelia_m9_reloc) {
      .command_dword = command_dword,
      .handle = handle,
      .target_offset = offset,
      .kind = kind,
      .flags = access,
   };
   return 0;
}

unsigned
m9_reloc_kind_access(enum drm_parhelia_m9_reloc_kind kind)
{
   /* Mirrors the kernel: BO refs must carry exactly the union of these. */
   switch (kind) {
   case PARHELIA_M9_RELOC_COLOR:
   case PARHELIA_M9_RELOC_DEPTH:
   case PARHELIA_M9_RELOC_PROGRAM:
   case PARHELIA_M9_RELOC_STREAM:
      return PARHELIA_M9_BO_READ | PARHELIA_M9_BO_WRITE;
   case PARHELIA_M9_RELOC_SYSTEM:
   case PARHELIA_M9_RELOC_TEXTURE:
   case PARHELIA_M9_RELOC_RESOURCE_69:
   case PARHELIA_M9_RELOC_RESOURCE_6C:
   case PARHELIA_M9_RELOC_DRAW32_START:
   case PARHELIA_M9_RELOC_DRAW32_END:
   case PARHELIA_M9_RELOC_DRAW16_START:
   case PARHELIA_M9_RELOC_DRAW16_END:
      return PARHELIA_M9_BO_READ;
   default:
      return 0;
   }
}

int
m9_batch_emit_packet(struct m9_batch *batch,
                     const struct m9_packet_value *values, unsigned count)
{
   struct m9_batch_checkpoint checkpoint;
   uint8_t indices[4];
   uint32_t words[4], header = 0x45454545u, base;
   int ret;

   if (!batch || !values || !count || count > 4 || batch->sealed)
      return -EINVAL;
   for (unsigned i = 0; i < count; i++) {
      indices[i] = values[i].index;
      words[i] = values[i].handle ? 0 : values[i].value;
      header &= ~(0xffu << (8 * i));
      header |= (uint32_t)indices[i] << (8 * i);
   }
   /* A split header would move the relocated value dwords. */
   if (count == 4 && m9_header_is_draw(header))
      return -EINVAL;
   m9_batch_checkpoint(batch, &checkpoint);
   base = batch->command_count;
   ret = m9_batch_emit_regs(batch, indices, words, count);
   for (unsigned i = 0; !ret && i < count; i++) {
      if (!values[i].handle)
         continue;
      ret = m9_batch_add_reloc(batch, base + 1 + i, values[i].handle,
                               values[i].offset, values[i].kind,
                               m9_reloc_kind_access(values[i].kind));
   }
   if (ret)
      m9_batch_rollback(batch, &checkpoint);
   return ret;
}

int
m9_batch_emit_range_break(struct m9_batch *batch)
{
   if (!batch || batch->sealed)
      return -EINVAL;
   if (batch->command_count + 2 > M9_MAX_COMMAND_DWORDS)
      return -ENOSPC;
   batch->command[batch->command_count++] = PARHELIA_M9_RANGE_BREAK;
   batch->command[batch->command_count++] = 0;
   return 0;
}

uint32_t
m9_surface_desc0(const struct m9_surface_desc *desc)
{
   return (desc->row_span & 0xffffu) |
          ((uint32_t)(desc->layout & 7u) << 16) |
          ((uint32_t)desc->address_offset << 21) |
          ((uint32_t)desc->resource_layout << 23) |
          ((uint32_t)(desc->format & 0x7fu) << 25);
}

uint32_t
m9_surface_desc1(const struct m9_surface_desc *desc)
{
   return (desc->height & 0xffffu) | ((desc->layers & 0xfffu) << 16);
}

int
m9_batch_bind_surface(struct m9_batch *batch, uint8_t address_index,
                      uint32_t handle, uint64_t offset,
                      enum drm_parhelia_m9_reloc_kind kind,
                      const struct m9_surface_desc *desc)
{
   const uint8_t indices[] = { address_index, (uint8_t)(address_index + 1) };
   const uint32_t values[] = { 0, m9_surface_desc0(desc) };
   uint32_t address_dword = batch ? batch->command_count + 1 : 0;
   struct m9_batch_checkpoint checkpoint;
   int ret;

   if (!batch || !desc || (address_index != 0xb0 && address_index != 0xb2 &&
                 address_index != 0xb4 && address_index != 0xb6 &&
                 address_index != 0xb8 && address_index != 0xba))
      return -EINVAL;
   m9_batch_checkpoint(batch, &checkpoint);
   if (!desc->row_span || !desc->height || !desc->layers ||
       (offset & 255) ||
       (address_index <= 0xb6 && kind != PARHELIA_M9_RELOC_COLOR &&
        kind != PARHELIA_M9_RELOC_STREAM) ||
       (address_index == 0xb8 && kind != PARHELIA_M9_RELOC_DEPTH &&
        kind != PARHELIA_M9_RELOC_STREAM) ||
       (address_index == 0xba && kind != PARHELIA_M9_RELOC_DEPTH))
      return -EINVAL;
   ret = m9_batch_emit_regs(batch, indices, values, 2);
   if (ret)
      return ret;
   /* Destination reads depend on blend/logic-op state.  The current kernel
    * executable boundary conservatively reserves every attachment both ways. */
   ret = m9_batch_add_reloc(batch, address_dword, handle, offset, kind,
                            m9_reloc_kind_access(kind));
   if (ret)
      m9_batch_rollback(batch, &checkpoint);
   return ret;
}

int
m9_batch_emit_texture_unit(struct m9_batch *batch, unsigned index,
                           const struct m9_texture_unit *unit)
{
   struct m9_batch_checkpoint checkpoint;
   int ret;

   if (!batch || batch->sealed || !unit || !unit->handle ||
       index >= M9_TEXTURE_MAX_UNITS)
      return -EINVAL;
   /* Captured GLSL order: 70+unit selects, then 78/79/7a, 7c/7d/87 and
    * the border words 80/81/7e/7f. */
   const struct m9_packet_value select[] = {
      { .index = 0x70 + index, .value = unit->w70 },
   };
   const struct m9_packet_value bind[] = {
      { .index = 0x78, .handle = unit->handle,
        .kind = PARHELIA_M9_RELOC_TEXTURE },
      { .index = 0x79, .value = unit->w79 },
      { .index = 0x7a, .value = unit->w7a },
   };
   const struct m9_packet_value sampler[] = {
      { .index = 0x7c, .value = unit->w7c },
      { .index = 0x7d, .value = unit->w7d },
      { .index = 0x87, .value = 0 },
   };
   const struct m9_packet_value border[] = {
      { .index = 0x80, .value = unit->w80 },
      { .index = 0x81, .value = unit->w81 },
      { .index = 0x7e, .value = unit->w7e },
      { .index = 0x7f, .value = unit->w7f },
   };

   m9_batch_checkpoint(batch, &checkpoint);
   ret = m9_batch_emit_packet(batch, select, ARRAY_SIZE(select));
   if (!ret)
      ret = m9_batch_emit_packet(batch, bind, ARRAY_SIZE(bind));
   if (!ret)
      ret = m9_batch_emit_packet(batch, sampler, ARRAY_SIZE(sampler));
   if (!ret)
      ret = m9_batch_emit_packet(batch, border, ARRAY_SIZE(border));
   if (ret)
      m9_batch_rollback(batch, &checkpoint);
   return ret;
}

int
m9_batch_bind_program(struct m9_batch *batch,
                      const struct m9_linked_program *program)
{
   (void)batch;
   (void)program;
   /* External 69/6c resources need kernel-owned compact GPUVA bindings. */
   return -EOPNOTSUPP;
}

int
m9_batch_bind_fixed_inline_program(struct m9_batch *batch)
{
   if (!batch || batch->sealed || batch->program_mode != M9_PROGRAM_MODE_NONE ||
       batch->command_count || batch->reloc_count || batch->bo_count)
      return -EINVAL;
   if (M9_RESOURCE69_COUNT_FREE_DWORDS > M9_MAX_COMMAND_DWORDS)
      return -ENOSPC;
   memcpy(batch->command, m9_resource69_inline_count_free,
          sizeof(m9_resource69_inline_count_free));
   batch->command_count = M9_RESOURCE69_COUNT_FREE_DWORDS;
   batch->program_mode = M9_PROGRAM_MODE_FIXED_INLINE;
   return 0;
}

int
m9_batch_emit_triangle_raster(struct m9_batch *batch, unsigned width,
                              unsigned height, bool depth_test,
                              bool depth_write)
{
   const uint8_t indices[] = { 0x22, 0xa8, 0xa9, 0xaa,
                              0xf5, 0xf6, 0xc4, 0xc5, 0x10, 0x11 };
   uint32_t depth = 0x00380388u | (depth_test ? 1u : 0u) |
                    (depth_write ? 2u : 0u);
   uint32_t extent;
   uint32_t values[10];

   if (!width || !height || width > 0xffffu || height > 0xffffu)
      return -EINVAL;
   extent = width | (height << 16);
   values[0] = 0x00006403;
   values[1] = depth;
   values[2] = 0x08ffff00;
   values[3] = 0x00ffff00;
   values[4] = values[6] = 0;
   values[5] = values[7] = extent;
   values[8] = 0x00100010;
   values[9] = 0x00100006;
   return m9_batch_emit_regs(batch, indices, values, 10);
}

int
m9_batch_emit_draw32(struct m9_batch *batch, uint32_t handle,
                     uint64_t offset, uint32_t count, uint32_t flags)
{
   uint64_t span = 4ull * count;
   struct m9_batch_checkpoint checkpoint;
   uint32_t base;
   int ret;

   if (!batch || batch->sealed || !count || (offset & 3) ||
       (flags != 0x20000000u && flags != 0x60000000u))
      return -EINVAL;
   if (span > UINT32_MAX || offset > UINT64_MAX - span)
      return -EOVERFLOW;
   if (batch->command_count + 3 > M9_MAX_COMMAND_DWORDS)
      return -ENOSPC;
   m9_batch_checkpoint(batch, &checkpoint);
   base = batch->command_count;
   batch->command[batch->command_count++] = 0x07000001u | flags;
   batch->command[batch->command_count++] = 0;
   batch->command[batch->command_count++] = span;
   ret = m9_batch_add_reloc(batch, base + 1, handle, offset,
                            PARHELIA_M9_RELOC_DRAW32_START,
                            PARHELIA_M9_BO_READ);
   if (!ret)
      ret = m9_batch_add_reloc(batch, base + 2, handle, offset + span,
                               PARHELIA_M9_RELOC_DRAW32_END,
                               PARHELIA_M9_BO_READ);
   if (ret)
      m9_batch_rollback(batch, &checkpoint);
   return ret;
}

int
m9_batch_emit_draw16(struct m9_batch *batch, uint32_t handle,
                     uint64_t offset, uint32_t count, uint32_t flags)
{
   uint64_t end, span;
   struct m9_batch_checkpoint checkpoint;
   uint32_t base;
   int ret;

   if (!batch || batch->sealed || !count || (offset & 3) ||
       (flags != 0x20000000u && flags != 0x60000000u))
      return -EINVAL;
   span = 4ull * ((count + 1ull) / 2ull);
   if (span > UINT32_MAX || offset > UINT64_MAX - span)
      return -EOVERFLOW;
   if (batch->command_count + 3 > M9_MAX_COMMAND_DWORDS)
      return -ENOSPC;
   m9_batch_checkpoint(batch, &checkpoint);
   end = offset + span;
   base = batch->command_count;
   batch->command[batch->command_count++] = 0x06000001u | flags;
   batch->command[batch->command_count++] = 0;
   batch->command[batch->command_count++] = span;
   ret = m9_batch_add_reloc(batch, base + 1, handle, offset,
                            PARHELIA_M9_RELOC_DRAW16_START,
                            PARHELIA_M9_BO_READ);
   if (!ret)
      ret = m9_batch_add_reloc(batch, base + 2, handle, end,
                               PARHELIA_M9_RELOC_DRAW16_END,
                               PARHELIA_M9_BO_READ |
                               (count & 1 ?
                                PARHELIA_M9_RELOC_PACKED16_ODD : 0));
   if (ret)
      m9_batch_rollback(batch, &checkpoint);
   return ret;
}

int
m9_batch_emit_draw_numeric(struct m9_batch *batch, uint32_t start,
                           uint32_t count, uint32_t flags)
{
   if (!batch || batch->sealed || !count ||
       (flags != 0x20000000u && flags != 0x60000000u))
      return -EINVAL;
   if (start > UINT_MAX - count)
      return -EOVERFLOW;
   if (batch->command_count + 3 > M9_MAX_COMMAND_DWORDS)
      return -ENOSPC;
   batch->command[batch->command_count++] = 0x03000001u | flags;
   batch->command[batch->command_count++] = start;
   batch->command[batch->command_count++] = start + count;
   return 0;
}

int
m9_primitive_map(enum m9_primitive primitive, uint32_t primitive_count,
                 struct m9_primitive_info *out)
{
   static const uint8_t selector[] = { 0, 2, 3, 5, 6, 7 };
   uint64_t vertices;

   if (!out || primitive > M9_PRIM_TRIANGLE_FAN)
      return -EINVAL;
   switch (primitive) {
   case M9_PRIM_POINTS: vertices = primitive_count; break;
   case M9_PRIM_LINES: vertices = 2ull * primitive_count; break;
   case M9_PRIM_LINE_STRIP: vertices = primitive_count ? primitive_count + 1 : 0; break;
   case M9_PRIM_TRIANGLES: vertices = 3ull * primitive_count; break;
   case M9_PRIM_TRIANGLE_STRIP:
   case M9_PRIM_TRIANGLE_FAN:
      vertices = primitive_count ? primitive_count + 2 : 0; break;
   default: return -EINVAL;
   }
   if (vertices > UINT_MAX)
      return -EOVERFLOW;
   *out = (struct m9_primitive_info) {
      .frontend_selector = selector[primitive],
      .vertex_count = vertices,
      .raster_control = primitive == M9_PRIM_TRIANGLES ? 0x6403 : 0,
      .raster_control_proved = primitive == M9_PRIM_TRIANGLES,
   };
   return 0;
}

int
m9_mip_layout_init(struct m9_mip_layout *layout, uint32_t width,
                   uint32_t height, uint32_t depth, unsigned levels,
                   unsigned cpp)
{
   static const uint8_t tail_x[5] = { 0, 16, 24, 16, 24 };
   static const uint8_t tail_y[5] = { 0, 0, 0, 8, 8 };
   uint64_t offset = 0;
   int tail_first = -1;
   unsigned max_levels = 1;
   uint32_t max_dim;

   if (!layout || !width || !height || depth != 1 || !cpp || cpp > 16 ||
       !levels || levels > M9_MAX_MIP_LEVELS)
      return -EINVAL;
   max_dim = width > height ? width : height;
   while (max_dim > 1) {
      max_dim >>= 1;
      max_levels++;
   }
   if (levels > max_levels)
      return -EINVAL;
   memset(layout, 0, sizeof(*layout));
   layout->levels = levels;
   layout->cpp = cpp;
   for (unsigned l = 0; l < levels; l++) {
      struct m9_mip_level *level = &layout->level[l];
      level->width = width >> l ? width >> l : 1;
      level->height = height >> l ? height >> l : 1;
      level->depth = 1;
      level->row_span = m9_resource_row_span(level->width);
      if (level->width <= 16 && level->height <= 16) {
         unsigned relative;
         if (tail_first < 0) {
            tail_first = l;
            offset = (offset + 255) & ~255ull;
         }
         relative = l - tail_first;
         if (relative >= 5)
            relative = 4;
         level->offset = offset;
         level->tail_x = tail_x[relative];
         level->tail_y = tail_y[relative];
         level->in_tail = true;
      } else {
         uint64_t bytes = (uint64_t)level->row_span *
                          m9_align(level->height, 16) * cpp;
         level->offset = offset;
         offset += (bytes + 255) & ~255ull;
      }
   }
   if (tail_first >= 0)
      offset += 32u * 16u * cpp;
   layout->size = (offset + 255) & ~255ull;
   return 0;
}

static int
m9_vertex_read(float *dst, unsigned dst_components,
               const struct m9_vertex_input *input, unsigned vertex)
{
   const uint8_t *src;

   if (!input)
      return 0;
   if (!input->data || !input->stride || !input->components ||
       input->components > dst_components)
      return -EINVAL;
   src = (const uint8_t *)input->data + input->offset +
         (uint64_t)vertex * input->stride;
   if (input->format == M9_VERTEX_FLOAT32) {
      memcpy(dst, src, input->components * sizeof(float));
   } else if (input->format == M9_VERTEX_UNORM8) {
      for (unsigned c = 0; c < input->components; c++)
         dst[c] = src[c] * (1.0f / 255.0f);
   } else {
      return -EINVAL;
   }
   return 0;
}

int
m9_repack_vertices(struct m9_vertex *dst, unsigned dst_capacity,
                   const struct m9_vertex_input *position,
                   const struct m9_vertex_input *color,
                   const struct m9_vertex_input *texcoord,
                   unsigned first, unsigned count)
{
   if (!dst || !position || count > dst_capacity || first > UINT_MAX - count)
      return -EINVAL;
   for (unsigned v = 0; v < count; v++) {
      struct m9_vertex canonical = {
         .position = { 0.0f, 0.0f, 0.0f, 1.0f },
         .color = { 1.0f, 1.0f, 1.0f, 1.0f },
         .texcoord = { 0.0f, 0.0f },
      };
      int ret = m9_vertex_read(canonical.position, 4, position, first + v);
      if (!ret)
         ret = m9_vertex_read(canonical.color, 4, color, first + v);
      if (!ret)
         ret = m9_vertex_read(canonical.texcoord, 2, texcoord, first + v);
      if (ret)
         return ret;
      dst[v] = canonical;
   }
   return 0;
}

int
m9_batch_prepare_submit(const struct m9_batch *batch,
                        const struct m9_resource *command,
                        uint32_t context_id,
                        struct drm_parhelia_m9_submit *submit)
{
   uint64_t bytes;

   if (!batch || !command || !submit || !batch->command_count ||
       command->kind != M9_RESOURCE_COMMAND || !command->handle ||
       !command->map)
      return -EINVAL;
   bytes = (uint64_t)batch->command_count * sizeof(uint32_t);
   if (bytes > 32768 || bytes > command->size)
      return -E2BIG;
   *submit = (struct drm_parhelia_m9_submit) {
      .context_id = context_id,
      .command_handle = command->handle,
      .command_length = bytes,
      .relocation_count = batch->reloc_count,
      .relocations_ptr = batch->reloc_count ?
                         (uintptr_t)batch->relocs : 0,
      .bo_count = batch->bo_count,
      .flags = PARHELIA_M9_SUBMIT_VALIDATE_ONLY,
      .bos_ptr = batch->bo_count ? (uintptr_t)batch->bos : 0,
      .program_type = batch->program_mode == M9_PROGRAM_MODE_FIXED_INLINE ?
                      PARHELIA_M9_PROGRAM_FIXED_P00_S28_INLINE :
                      PARHELIA_M9_PROGRAM_NONE,
   };
   return 0;
}

int
m9_batch_prepare_execute(const struct m9_batch *batch,
                         const struct m9_resource *command,
                         uint32_t context_id,
                         struct drm_parhelia_m9_submit *submit)
{
   int ret;

   if (!batch || !batch->sealed)
      return -EINVAL;
   ret = m9_batch_prepare_submit(batch, command, context_id, submit);
   if (!ret)
      submit->flags = 0;
   return ret;
}

int
m9_batch_validate(struct m9_screen *screen, struct m9_resource *command,
                  const struct m9_batch *batch, uint32_t context_id)
{
   struct drm_parhelia_m9_submit submit;
   int ret;

   if (!screen || !screen->validate_submit)
      return -EOPNOTSUPP;
   ret = m9_batch_prepare_submit(batch, command, context_id, &submit);
   if (ret)
      return ret;
   memcpy(command->map, batch->command, submit.command_length);
   __sync_synchronize();
   if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_M9_SUBMIT, &submit))
      return -errno;
   return submit.out_point ? -EPROTO : 0;
}

int
m9_batch_submit(struct m9_screen *screen, struct m9_resource *command,
                const struct m9_batch *batch, uint32_t context_id,
                uint64_t *out_point)
{
   struct drm_parhelia_m9_submit submit;
   int ret;

   if (!screen || !screen->execute_submit || !out_point)
      return -EOPNOTSUPP;
   ret = m9_batch_prepare_execute(batch, command, context_id, &submit);
   if (ret)
      return ret;
   memcpy(command->map, batch->command, submit.command_length);
   __sync_synchronize();
   if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_M9_SUBMIT, &submit))
      return -errno;
   if (!submit.out_point)
      return -EPROTO;
   *out_point = submit.out_point;
   return 0;
}

int
m9_submit_noop_fence(struct m9_screen *screen, uint64_t *out_point)
{
   struct drm_parhelia_m9_submit submit = {
      .flags = PARHELIA_M9_SUBMIT_NOOP_FENCE,
   };

   if (!screen || !out_point)
      return -EINVAL;
   if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_M9_SUBMIT, &submit))
      return -errno;
   if (!submit.out_point)
      return -EPROTO;
   *out_point = submit.out_point;
   return 0;
}

int
m9_submit_first_triangle(struct m9_screen *screen, uint32_t color_handle,
                         uint64_t *out_point)
{
   struct drm_parhelia_m9_bo_ref ref = {
      .handle = color_handle,
      .flags = PARHELIA_M9_BO_READ | PARHELIA_M9_BO_WRITE,
   };
   struct drm_parhelia_m9_submit submit = {
      .bo_count = 1,
      .flags = PARHELIA_M9_SUBMIT_FIRST_TRIANGLE,
      .bos_ptr = (uintptr_t)&ref,
   };

   if (!screen || !color_handle || !out_point)
      return -EINVAL;
   if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_M9_SUBMIT, &submit))
      return -errno;
   if (!submit.out_point)
      return -EPROTO;
   *out_point = submit.out_point;
   return 0;
}
