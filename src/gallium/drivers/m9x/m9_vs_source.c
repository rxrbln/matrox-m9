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
                         struct m9_vs_source_layout *out)
{
   unsigned dword = 0;

   if (!program || !out || (element_count && !elements))
      return false;
   memset(out, 0, sizeof(*out));
   for (unsigned i = 0; i < program->attr_count; i++) {
      const struct m9_vs_attr *a = &program->attrs[i];
      const struct pipe_vertex_element *e;
      struct m9_vs_source_attr *s = &out->attr[i];
      unsigned components = 0;

      if (a->driver_location >= element_count)
         return false;
      e = &elements[a->driver_location];
      if (e->vertex_buffer_index >= buffer_count ||
          buffers[e->vertex_buffer_index].is_user_buffer ||
          e->instance_divisor || !m9_vs_source_format_ok(e->src_format))
         return false;
      /* The declaration's selectors 0..3 pick a source component; 4 and 5 are
       * the literals the record never carries. */
      for (unsigned c = 0; c < 4; c++)
         if (a->select[c] < 4)
            components = MAX2(components, (unsigned)a->select[c] + 1u);
      if (components > util_format_get_nr_components(e->src_format))
         return false;
      s->buffer = e->vertex_buffer_index;
      s->src_offset = e->src_offset;
      s->stride = e->src_stride;
      s->format = e->src_format;
      s->components = components;
      s->dword = dword;
      if (s->dword != a->offset)
         return false;
      dword += components;
   }
   out->attr_count = program->attr_count;
   out->record_dwords = dword;
   return dword && dword == program->record_bytes / 4;
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
         float rgba[4];

         unpack->unpack_rgba(rgba, src, 1);
         memcpy(out + (uint64_t)v * layout->record_dwords + s->dword, rgba,
                s->components * sizeof(float));
      }
   }
   return true;
}
