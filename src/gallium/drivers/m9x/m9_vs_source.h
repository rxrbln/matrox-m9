/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Preprocessing source records for a draw whose vertex program runs on the
 * GPU.  The draw module's records hold transformed position and colour; a
 * native vertex job instead wants the raw attributes the shader declared, laid
 * out as the index-23 declaration describes them.
 */
#ifndef M9_VS_SOURCE_H
#define M9_VS_SOURCE_H

#include "m9_vs_compile.h"

#include "pipe/p_state.h"

/* Where one declared attribute's components come from. */
struct m9_vs_source_attr {
   unsigned buffer;        /* index into the bound vertex buffers */
   unsigned src_offset;    /* byte offset of the element from the buffer base */
   unsigned stride;
   enum pipe_format format;
   unsigned components;    /* components the shader reads */
   unsigned dword;         /* dword offset of the element in the record */
};

struct m9_vs_source_layout {
   struct m9_vs_source_attr attr[M9_VS_MAX_ATTRIBUTES];
   unsigned attr_count;
   unsigned record_dwords;  /* floats per source record */
};

/* Resolves each attribute the program declared to the vertex element that
 * feeds it.  Returns false when the draw cannot be served natively: an
 * attribute with no element, a format the record cannot carry, or an
 * instanced or otherwise non-linear source.  A user buffer is accepted: the
 * caller passes its pointer as the mapping, as the draw module does. */
bool m9_vs_source_layout_init(const struct m9_vs_program *program,
                              const struct pipe_vertex_element *elements,
                              unsigned element_count,
                              const struct pipe_vertex_buffer *buffers,
                              unsigned buffer_count,
                              struct m9_vs_source_layout *out,
                              const char **reason);

/* True when vertices first..first+count-1 lie inside every mapping the
 * layout reads, so that the record writer below cannot fail on bounds. */
bool m9_vs_source_in_range(const struct m9_vs_source_layout *layout,
                           const unsigned *buffer_size, unsigned buffer_count,
                           unsigned first, unsigned count);

/* Writes count records starting at vertex first into out, which holds
 * count * record_dwords floats.  The caller maps the vertex buffers and
 * passes their bases and sizes, so this stays a pure function: mapping is the
 * context's business and the record packing can be tested without one. */
bool m9_vs_source_records(const struct m9_vs_source_layout *layout,
                          const uint8_t *const *buffer_map,
                          const unsigned *buffer_size, unsigned buffer_count,
                          unsigned first, unsigned count, float *out);

#endif
