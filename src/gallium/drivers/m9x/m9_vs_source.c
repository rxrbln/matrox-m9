/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Builds the preprocessing source records a native vertex job reads.  The
 * draw module hands the raster pass transformed vertices; a job that runs its
 * vertex program on the GPU needs the raw attributes instead, packed in the
 * order the index-23 declaration names them.
 */
#include "m9_vs_source.h"

#include <limits.h>
#include <string.h>

#include "nir.h"
#include "util/format/u_format.h"
#include "util/u_math.h"

/* Every component the record carries is a float, so a format qualifies only
 * when it unpacks to float without losing what the shader asked for. */
static bool
m9_vs_source_format_ok(enum pipe_format format)
{
   const struct util_format_description *d = util_format_description(format);

   return d && !util_format_is_compressed(format) &&
          d->colorspace == UTIL_FORMAT_COLORSPACE_RGB &&
          util_format_get_blocksize(format) <= 16;
}

bool
m9_vs_output_is_one(struct nir_shader *nir,
                    const struct m9_vs_program *program,
                    const struct pipe_vertex_element *elements,
                    unsigned element_count, unsigned location,
                    unsigned component)
{
   nir_intrinsic_instr *store = NULL;

   if (!nir || !program || !elements || component >= 4)
      return false;
   nir_function_impl *entry = nir_shader_get_entrypoint(nir);

   if (!entry || exec_list_length(&entry->body) != 1)
      return false;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

            if (intr->intrinsic != nir_intrinsic_store_output ||
                nir_intrinsic_io_semantics(intr).location != location)
               continue;
            unsigned first = nir_intrinsic_component(intr);

            if (component < first ||
                component - first >= intr->src[0].ssa->num_components ||
                !(nir_intrinsic_write_mask(intr) & BITFIELD_BIT(component - first)))
               continue;
            if (store || !nir_src_is_const(intr->src[1]) ||
                nir_src_as_uint(intr->src[1]))
               return false;
            store = intr;
         }
      }
   }
   if (!store)
      return false;
   nir_scalar s = nir_scalar_chase_movs(nir_get_scalar(store->src[0].ssa,
                                  component - nir_intrinsic_component(store)));

   if (s.def->bit_size != 32)
      return false;
   if (nir_scalar_is_const(s))
      return nir_scalar_as_float(s) == 1.0f;
   if (!nir_scalar_is_intrinsic(s))
      return false;
   nir_intrinsic_instr *input = nir_def_as_intrinsic(s.def);

   if (input->intrinsic != nir_intrinsic_load_input ||
       !nir_src_is_const(input->src[0]) || nir_src_as_uint(input->src[0]))
      return false;
   unsigned input_location = nir_intrinsic_io_semantics(input).location;
   unsigned input_component = nir_intrinsic_component(input) + s.comp;

   if (input_component >= 4)
      return false;
   for (unsigned i = 0; i < program->attr_count; i++) {
      if (program->attrs[i].constant ||
          program->attrs[i].location != input_location)
         continue;
      unsigned element = m9_vs_attr_element(program, i);

      if (element >= element_count ||
          !m9_vs_source_format_ok(elements[element].src_format))
         return false;
      const struct util_format_description *format =
         util_format_description(elements[element].src_format);

      return format->swizzle[input_component] == PIPE_SWIZZLE_1;
   }
   return false;
}

bool
m9_vs_source_layout_init(const struct m9_vs_program *program,
                         const struct pipe_vertex_element *elements,
                         unsigned element_count,
                         const struct pipe_vertex_buffer *buffers,
                         unsigned buffer_count,
                         struct m9_vs_source_layout *out,
                         const char **reason)
{
   unsigned dword = 0;

#define REFUSE(why) do { if (reason) *reason = (why); return false; } while (0)
   if (!program || !out || (element_count && !elements))
      REFUSE("no vertex elements");
   memset(out, 0, sizeof(*out));
   for (unsigned i = 0; i < program->attr_count; i++) {
      const struct m9_vs_attr *a = &program->attrs[i];
      const struct pipe_vertex_element *e;
      struct m9_vs_source_attr *s = &out->attr[i];
      unsigned components = 0;
      unsigned element;

      /* A coefficient slot has no element: the caller writes its four
       * floats into every record of the draw. */
      if (a->constant) {
         s->constant = true;
         s->coeff_stream = a->coeff_stream;
         s->components = 4;
         s->dword = dword;
         if (s->dword != a->offset)
            REFUSE("the record layout disagrees with the declaration");
         dword += 4;
         continue;
      }
      element = m9_vs_attr_element(program, i);

      if (element >= element_count)
         REFUSE("an attribute has no vertex element");
      e = &elements[element];
      if (e->vertex_buffer_index >= buffer_count)
         REFUSE("an attribute has no vertex buffer");
      if (e->instance_divisor)
         REFUSE("an instanced attribute");
      if (e->dual_slot)
         REFUSE("a dual-slot attribute");
      if (!m9_vs_source_format_ok(e->src_format))
         REFUSE("a vertex format the record cannot carry");
      /* Components beyond what the element carries unpack to the GL default
       * already, so a short element serves a wider read. */
      components = MIN2(a->components, 4u);
      uint64_t offset = (uint64_t)buffers[e->vertex_buffer_index].buffer_offset +
                        e->src_offset;
      if (offset > UINT_MAX)
         REFUSE("an attribute offset leaves the vertex buffer");
      s->buffer = e->vertex_buffer_index;
      s->src_offset = offset;
      s->stride = e->src_stride;
      s->format = e->src_format;
      s->components = components;
      s->dword = dword;
      if (s->dword != a->offset)
         REFUSE("the record layout disagrees with the declaration");
      dword += 4;
   }
   out->attr_count = program->attr_count;
   out->record_dwords = dword;
   if (!dword || dword != program->record_bytes / 4)
      REFUSE("the record size disagrees with the declaration");
   return true;
#undef REFUSE
}

/* Compare against the remaining mapping instead of multiplying an
 * arbitrary vertex index by the input stride. Zero stride consumes only
 * the texel itself, even for a high draw/index base. */
static bool
m9_vs_source_range(const struct m9_vs_source_attr *s, unsigned size,
                   uint64_t last)
{
   uint64_t end = (uint64_t)s->src_offset + util_format_get_blocksize(s->format);

   return end <= size && (!s->stride || last <= (size - end) / s->stride);
}

bool
m9_vs_source_in_range(const struct m9_vs_source_layout *layout,
                      const unsigned *buffer_size, unsigned buffer_count,
                      unsigned first, unsigned count)
{
   if (!layout || !buffer_size || !count)
      return false;
   for (unsigned a = 0; a < layout->attr_count; a++) {
      const struct m9_vs_source_attr *s = &layout->attr[a];

      if (s->constant)
         continue;
      if (s->buffer >= buffer_count)
         return false;
      if (!m9_vs_source_range(s, buffer_size[s->buffer],
                              (uint64_t)first + count - 1))
         return false;
   }
   return true;
}

/* A zero-stride element addresses one bounded texel for the whole draw.
 * Expand it into ordinary source records; the GPU declaration and its record
 * stride stay the same as for per-vertex attributes. */
static bool
m9_vs_source_repeat(const struct m9_vs_source_layout *layout,
                    const struct m9_vs_source_attr *s,
                    const struct util_format_unpack_description *unpack,
                    const uint8_t *map, unsigned size, unsigned count,
                    float *out)
{
   float rgba[4];

   if (!m9_vs_source_range(s, size, 0))
      return false;
   unpack->unpack_rgba(rgba, map + s->src_offset, 1);
   for (unsigned c = s->components; c < 4; c++)
      rgba[c] = c == 3 ? 1.0f : 0.0f;
   for (unsigned v = 0; v < count; v++)
      memcpy(out + (uint64_t)v * layout->record_dwords + s->dword,
             rgba, sizeof(rgba));
   return true;
}

bool
m9_vs_source_records(const struct m9_vs_source_layout *layout,
                     const uint8_t *const *buffer_map,
                     const unsigned *buffer_size, unsigned buffer_count,
                     unsigned first, unsigned count, float *out)
{
   if (!layout || !buffer_map || !buffer_size || !out || !count)
      return false;
   for (unsigned a = 0; a < layout->attr_count; a++) {
      const struct m9_vs_source_attr *s = &layout->attr[a];
      const struct util_format_unpack_description *unpack;

      if (s->constant) {
         for (unsigned v = 0; v < count; v++)
            memcpy(out + (uint64_t)v * layout->record_dwords + s->dword,
                   s->value, sizeof(s->value));
         continue;
      }
      unpack = util_format_unpack_description(s->format);
      if (s->buffer >= buffer_count || !buffer_map[s->buffer] || !unpack ||
          !unpack->unpack_rgba)
         return false;
      if (!s->stride) {
         if (!m9_vs_source_repeat(layout, s, unpack, buffer_map[s->buffer],
                                  buffer_size[s->buffer], count, out))
            return false;
         continue;
      }
      /* The last vertex this draw reads must lie inside the mapping. */
      if (!m9_vs_source_range(s, buffer_size[s->buffer],
                              (uint64_t)first + count - 1))
         return false;

      for (unsigned v = 0; v < count; v++) {
         const uint8_t *src = buffer_map[s->buffer] +
                              ((uint64_t)first + v) * s->stride + s->src_offset;
         float *dst = out + (uint64_t)v * layout->record_dwords + s->dword;
         float rgba[4];

         unpack->unpack_rgba(rgba, src, 1);
         for (unsigned c = 0; c < 4; c++)
            dst[c] = c < s->components ? rgba[c] :
                     (c == 3 ? 1.0f : 0.0f);
      }
   }
   return true;
}

bool
m9_vs_source_records_gather(const struct m9_vs_source_layout *layout,
                            const uint8_t *const *buffer_map,
                            const unsigned *buffer_size,
                            unsigned buffer_count, unsigned first,
                            const uint32_t *vertex, unsigned count, float *out)
{
   if (!layout || !buffer_map || !buffer_size || !vertex || !out || !count)
      return false;
   for (unsigned a = 0; a < layout->attr_count; a++) {
      const struct m9_vs_source_attr *s = &layout->attr[a];
      const struct util_format_unpack_description *unpack;

      if (s->constant) {
         for (unsigned v = 0; v < count; v++)
            memcpy(out + (uint64_t)v * layout->record_dwords + s->dword,
                   s->value, sizeof(s->value));
         continue;
      }
      unpack = util_format_unpack_description(s->format);
      if (s->buffer >= buffer_count || !buffer_map[s->buffer] || !unpack ||
          !unpack->unpack_rgba)
         return false;
      if (!s->stride) {
         if (!m9_vs_source_repeat(layout, s, unpack, buffer_map[s->buffer],
                                  buffer_size[s->buffer], count, out))
            return false;
         continue;
      }
      for (unsigned v = 0; v < count; v++) {
         uint64_t at = (uint64_t)first + vertex[v];
         const uint8_t *src;
         float *dst = out + (uint64_t)v * layout->record_dwords + s->dword;
         float rgba[4];

         if (!m9_vs_source_range(s, buffer_size[s->buffer], at))
            return false;
         src = buffer_map[s->buffer] + at * s->stride + s->src_offset;
         unpack->unpack_rgba(rgba, src, 1);
         for (unsigned c = 0; c < 4; c++)
            dst[c] = c < s->components ? rgba[c] : (c == 3 ? 1.0f : 0.0f);
      }
   }
   return true;
}
