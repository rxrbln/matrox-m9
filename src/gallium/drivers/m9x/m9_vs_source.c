/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Builds the preprocessing source records a native vertex job reads.  The
 * draw module hands the raster pass transformed vertices; a job that runs its
 * vertex program on the GPU needs the raw attributes instead, packed in the
 * order the index-23 declaration names them.
 */
#include "m9_vs_source.h"

#include <string.h>

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

      unsigned element = m9_vs_attr_element(program, i);

      if (element >= element_count)
         REFUSE("an attribute has no vertex element");
      e = &elements[element];
      if (e->vertex_buffer_index >= buffer_count)
         REFUSE("an attribute has no vertex buffer");
      if (e->instance_divisor)
         REFUSE("an instanced attribute");
      if (e->dual_slot)
         REFUSE("a dual-slot attribute");
      if (!e->src_stride)
         REFUSE("a zero-stride attribute");
      if (!m9_vs_source_format_ok(e->src_format))
         REFUSE("a vertex format the record cannot carry");
      /* Components beyond what the element carries unpack to the GL default
       * already, so a short element serves a wider read. */
      components = MIN2(a->components, 4u);
      s->buffer = e->vertex_buffer_index;
      s->src_offset = buffers[e->vertex_buffer_index].buffer_offset +
                      e->src_offset;
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

bool
m9_vs_source_in_range(const struct m9_vs_source_layout *layout,
                      const unsigned *buffer_size, unsigned buffer_count,
                      unsigned first, unsigned count)
{
   if (!layout || !buffer_size || !count)
      return false;
   for (unsigned a = 0; a < layout->attr_count; a++) {
      const struct m9_vs_source_attr *s = &layout->attr[a];
      unsigned texel = util_format_get_blocksize(s->format);

      if (s->buffer >= buffer_count || !s->stride)
         return false;
      if ((uint64_t)(first + count - 1) * s->stride + s->src_offset + texel >
          buffer_size[s->buffer])
         return false;
   }
   return true;
}

bool
m9_vs_source_records(const struct m9_vs_source_layout *layout,
                     const uint8_t *const *buffer_map,
                     const unsigned *buffer_size, unsigned buffer_count,
                     unsigned first, unsigned count, float *out)
{
   if (!layout || !buffer_map || !buffer_size || !out)
      return false;
   for (unsigned a = 0; a < layout->attr_count; a++) {
      const struct m9_vs_source_attr *s = &layout->attr[a];
      const struct util_format_unpack_description *unpack =
         util_format_unpack_description(s->format);
      unsigned texel = util_format_get_blocksize(s->format);

      if (s->buffer >= buffer_count || !buffer_map[s->buffer] || !unpack ||
          !unpack->unpack_rgba || !s->stride)
         return false;
      /* The last vertex this draw reads must lie inside the mapping. */
      if ((uint64_t)(first + count - 1) * s->stride + s->src_offset + texel >
          buffer_size[s->buffer])
         return false;

      for (unsigned v = 0; v < count; v++) {
         const uint8_t *src = buffer_map[s->buffer] +
                              (uint64_t)(first + v) * s->stride + s->src_offset;
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
   if (!layout || !buffer_map || !buffer_size || !vertex || !out)
      return false;
   for (unsigned a = 0; a < layout->attr_count; a++) {
      const struct m9_vs_source_attr *s = &layout->attr[a];
      const struct util_format_unpack_description *unpack =
         util_format_unpack_description(s->format);
      unsigned texel = util_format_get_blocksize(s->format);

      if (s->buffer >= buffer_count || !buffer_map[s->buffer] || !unpack ||
          !unpack->unpack_rgba || !s->stride)
         return false;
      for (unsigned v = 0; v < count; v++) {
         uint64_t at = (uint64_t)first + vertex[v];
         const uint8_t *src;
         float *dst = out + (uint64_t)v * layout->record_dwords + s->dword;
         float rgba[4];

         if (at * s->stride + s->src_offset + texel > buffer_size[s->buffer])
            return false;
         src = buffer_map[s->buffer] + at * s->stride + s->src_offset;
         unpack->unpack_rgba(rgba, src, 1);
         for (unsigned c = 0; c < 4; c++)
            dst[c] = c < s->components ? rgba[c] : (c == 3 ? 1.0f : 0.0f);
      }
   }
   return true;
}
