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

/* Only semantic GL defaults derived from the bound unpack format. Values,
 * buffer addresses and strides are not compiler keys. */
struct m9_vs_format_key {
   uint64_t zero[4];
   uint64_t one[4];
};

bool m9_vs_format_key_init(uint64_t inputs_read,
                            const struct pipe_vertex_element *elements,
                            unsigned element_count,
                            struct m9_vs_format_key *key);

/* Retry only a four-attribute refusal using exact absent-component defaults.
 * Adopt the lowered graph with its program; existing accepted images remain
 * unchanged. This is the ordinary vertex interface, not FAA preprocessing. */
bool m9_vs_compile_format_source(struct nir_shader **nir,
                                  const struct m9_vs_outputs *outputs,
                                  struct m9_vs_program *program,
                                  const char **reason,
                                  unsigned max_slots, unsigned max_const_pairs,
                                  const struct m9_vs_format_key *key);

/* Where one declared attribute's components come from. */
struct m9_vs_source_attr {
   unsigned buffer;        /* index into the bound vertex buffers */
   unsigned src_offset;    /* byte offset of the element from the buffer base */
   unsigned stride;        /* zero repeats one bounded source texel */
   enum pipe_format format;
   unsigned components;    /* components the shader reads */
   unsigned dword;         /* dword offset of the element in the record */
   bool scalar;            /* writes only component zero at this dword */
   bool scalar_first;      /* initializes its packed source slot defaults */
   /* A slot no vertex element feeds: value[] goes into every record of the
    * draw.  The caller fills it -- a coordinate stream's scale and bias --
    * between the layout and the records. */
   bool constant;
   uint8_t coeff_stream;
   float value[4];
};

struct m9_vs_source_layout {
   struct m9_vs_source_attr attr[M9_VS_MAX_SOURCE_ATTRIBUTES];
   unsigned attr_count;
   unsigned record_dwords;  /* floats per source record */
};

/* Proves a stored output component is exactly one, either a literal or a
 * direct input component whose bound format unpacks to the GL default one.
 * Other expressions and components present in the source remain unproved. */
bool m9_vs_output_is_one(struct nir_shader *nir,
                         const struct m9_vs_program *program,
                         const struct pipe_vertex_element *elements,
                         unsigned element_count, unsigned location,
                         unsigned component);

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

/* The same, for count vertices named one at a time by vertex[] rather than by
 * a contiguous run: a welded draw's unique records come from scattered
 * corners of the original.  Every named vertex is bounds-checked, because
 * unlike a run its highest member is not known in advance. */
bool m9_vs_source_records_gather(const struct m9_vs_source_layout *layout,
                                 const uint8_t *const *buffer_map,
                                 const unsigned *buffer_size,
                                 unsigned buffer_count, unsigned first,
                                 const uint32_t *vertex, unsigned count,
                                 float *out);

#endif
