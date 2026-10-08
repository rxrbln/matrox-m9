/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Builds the preprocessing source records a native vertex job reads.  The
 * draw module hands the raster pass transformed vertices; a job that runs its
 * vertex program on the GPU needs the raw attributes instead, packed in the
 * order the index-23 declaration names them.
 */
#include "m9_vs_source.h"
#include "m9_specialise.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"
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
m9_vs_format_key_init(uint64_t inputs_read,
                       const struct pipe_vertex_element *elements,
                       unsigned element_count, struct m9_vs_format_key *key)
{
   struct m9_vs_format_key trial = {0};
   unsigned element = 0;

   if (!key || (inputs_read && !elements))
      return false;
   for (unsigned location = 0; location < 64; location++) {
      if (!(inputs_read & BITFIELD64_BIT(location)))
         continue;
      if (element >= element_count || elements[element].instance_divisor ||
          elements[element].dual_slot ||
          !m9_vs_source_format_ok(elements[element].src_format))
         return false;
      const struct util_format_description *d =
         util_format_description(elements[element++].src_format);
      for (unsigned c = 0; c < 4; c++) {
         if (d->swizzle[c] == PIPE_SWIZZLE_0)
            trial.zero[c] |= BITFIELD64_BIT(location);
         else if (d->swizzle[c] == PIPE_SWIZZLE_1)
            trial.one[c] |= BITFIELD64_BIT(location);
      }
   }
   *key = trial;
   return true;
}

static bool
format_input(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   const struct m9_vs_format_key *key = data;
   if (intr->intrinsic != nir_intrinsic_load_input ||
       intr->def.bit_size != 32 ||
       nir_intrinsic_dest_type(intr) != nir_type_float32 ||
       !nir_src_is_const(intr->src[0]) || nir_src_as_uint(intr->src[0]))
      return false;
   unsigned location = nir_intrinsic_io_semantics(intr).location;
   unsigned first = nir_intrinsic_component(intr);
   unsigned count = intr->def.num_components;
   if (location >= 64 || first + count > 4)
      return false;
   uint64_t bit = BITFIELD64_BIT(location);
   unsigned real = 0;
   bool changed = false;
   nir_scalar components[4];
   b->cursor = nir_after_instr(&intr->instr);
   for (unsigned c = 0; c < count; c++) {
      if ((key->zero[first + c] | key->one[first + c]) & bit) {
         components[c] = nir_get_scalar(nir_imm_float(b,
                            (key->one[first + c] & bit) ? 1.0f : 0.0f), 0);
         changed = true;
      } else {
         components[c] = nir_get_scalar(&intr->def, c);
         real = c + 1;
      }
   }
   if (!changed)
      return false;
   nir_def *value = nir_vec_scalars(b, components, count);
   nir_def_rewrite_uses_after(&intr->def, value);
   if (real) {
      intr->num_components = real;
      intr->def.num_components = real;
   } else {
      nir_instr_remove(&intr->instr);
   }
   return true;
}

bool
m9_vs_compile_format_source(struct nir_shader **nir,
                             const struct m9_vs_outputs *outputs,
                             struct m9_vs_program *program, const char **reason,
                             unsigned max_slots, unsigned max_const_pairs,
                             const struct m9_vs_format_key *key)
{
   const char *original_reason = NULL;
   bool defaults = false;
   if (key)
      for (unsigned c = 0; c < 4; c++)
         defaults |= (key->zero[c] | key->one[c]) != 0;
   /* The first compile mutates its source. Save only the bounded retry's
    * pre-geometry graph, with the same parent as the caller's owner. */
   nir_shader *saved = defaults ? nir_shader_clone(ralloc_parent(*nir), *nir) : NULL;
   if (m9_vs_compile_bounded_source(nir, outputs, program, &original_reason,
                                     max_slots, max_const_pairs)) {
      ralloc_free(saved);
      if (reason) *reason = original_reason;
      return true;
   }
   struct m9_vs_program trial;
   if (saved && original_reason &&
       !strcmp(original_reason, "more than four vertex attributes") &&
       nir_shader_intrinsics_pass(saved, format_input,
                                   nir_metadata_control_flow, (void *)key)) {
      NIR_PASS(_, saved, nir_opt_constant_folding);
      NIR_PASS(_, saved, nir_opt_dce);
      NIR_PASS(_, saved, nir_opt_shrink_vectors, true);
      NIR_PASS(_, saved, nir_opt_dce);
      if (m9_vs_compile_bounded_source(&saved, outputs, &trial, NULL,
                                         max_slots, max_const_pairs)) {
         bool fits = m9_vs_slot_count(&trial) <= max_slots &&
                     m9_vs_extra_slot_count(&trial) <= max_slots &&
                     (!m9_vs_producer_slot_count(&trial) ||
                      m9_vs_producer_slot_count(&trial) + M9_VS_TAIL_SLOTS <= max_slots) &&
                     m9_vs_const_count(&trial) <= max_const_pairs;
         if (fits) {
            *program = trial;
            ralloc_free(*nir);
            *nir = saved;
            if (reason) *reason = NULL;
            return true;
         }
         m9_vs_program_fini(&trial);
      }
   }
   ralloc_free(saved);
   if (reason) *reason = original_reason;
   return false;
}

static bool
uniform_gate_word(nir_scalar s, unsigned uniforms, unsigned *word)
{
   s = nir_scalar_chase_movs(s);
   if (!nir_scalar_is_intrinsic(s) || s.def->bit_size != 32)
      return false;
   nir_intrinsic_instr *i = nir_def_as_intrinsic(s.def);
   if (i->intrinsic != nir_intrinsic_load_ubo_vec4 ||
       !nir_src_is_const(i->src[0]) || nir_src_as_uint(i->src[0]) ||
       !nir_src_is_const(i->src[1]) ||
       (nir_intrinsic_access(i) & ACCESS_VOLATILE))
      return false;
   uint64_t w = ((uint64_t)nir_intrinsic_base(i) + nir_src_as_uint(i->src[1])) * 4 +
                nir_intrinsic_component(i) + s.comp;
   if (w > UINT16_MAX || w / 4 >= uniforms)
      return false;
   *word = w;
   return true;
}

static bool
uniform_gate_costly(nir_scalar s)
{
   s = nir_scalar_chase_movs(s);
   if (!nir_scalar_is_alu(s))
      return false;
   switch (nir_def_as_alu(s.def)->op) {
   case nir_op_fsin: case nir_op_fcos: case nir_op_frsq:
   case nir_op_frcp: case nir_op_fsqrt: case nir_op_fexp2: case nir_op_flog2:
      return true;
   default:
      return false;
   }
}

void
m9_vs_find_uniform_gates(nir_shader *nir, struct m9_vs_uniform_gates *gates)
{
   memset(gates, 0, sizeof(*gates));
   if (!nir || nir->info.stage != MESA_SHADER_VERTEX ||
       !nir->info.first_ubo_is_default_ubo)
      return;
   /* Prioritize a uniform multiplying an SFU result; then other direct
    * multiplicative gates. The bounded word list contains no live values. */
   for (unsigned priority = 0; priority < 2; priority++) {
      nir_foreach_function_impl(impl, nir) {
         nir_foreach_block(block, impl) {
            nir_foreach_instr(instr, block) {
               if (instr->type != nir_instr_type_alu)
                  continue;
               nir_alu_instr *alu = nir_instr_as_alu(instr);
               if (alu->op != nir_op_fmul || alu->def.bit_size != 32)
                  continue;
               for (unsigned c = 0; c < alu->def.num_components; c++) {
                  for (unsigned src = 0; src < 2; src++) {
                     nir_scalar result = nir_get_scalar(&alu->def, c);
                     unsigned word;
                     if (uniform_gate_costly(nir_scalar_chase_alu_src(result, 1-src)) !=
                         (priority == 0) ||
                         !uniform_gate_word(nir_scalar_chase_alu_src(result, src),
                                              nir->num_uniforms, &word))
                        continue;
                     bool have = false;
                     for (unsigned j = 0; j < gates->count; j++)
                        have |= gates->word[j] == word;
                     if (have)
                        continue;
                     if (gates->count == M9_VS_MAX_GATE_UNIFORMS)
                        return;
                     gates->word[gates->count++] = word;
                  }
               }
            }
         }
      }
   }
}

bool
m9_vs_gate_key_init(const struct m9_vs_uniform_gates *gates,
                     const void *data, size_t bytes, struct m9_vs_gate_key *key)
{
   struct m9_vs_gate_key trial = {0};
   if (!gates || !key || gates->count > M9_VS_MAX_GATE_UNIFORMS ||
       (gates->count && !data))
      return false;
   for (unsigned i = 0; i < gates->count; i++) {
      size_t offset = (size_t)gates->word[i] * 4;
      if (offset > bytes || bytes - offset < 4)
         return false;
      uint32_t value;
      memcpy(&value, (const uint8_t *)data + offset, sizeof(value));
      if (value == 0)
         trial.zero |= 1u << i;
      else if (value == 0x3f800000u)
         trial.one |= 1u << i;
   }
   *key = trial;
   return true;
}

struct uniform_gate_input {
   const struct m9_vs_uniform_gates *gates;
   const struct m9_vs_gate_key *key;
};

static bool
uniform_gate_input(nir_builder *b, nir_intrinsic_instr *i, void *data)
{
   const struct uniform_gate_input *input = data;
   if (i->intrinsic != nir_intrinsic_load_ubo_vec4 || i->def.bit_size != 32 ||
       !nir_src_is_const(i->src[0]) || nir_src_as_uint(i->src[0]) ||
       !nir_src_is_const(i->src[1]) ||
       (nir_intrinsic_access(i) & ACCESS_VOLATILE))
      return false;
   uint64_t first = ((uint64_t)nir_intrinsic_base(i) + nir_src_as_uint(i->src[1])) * 4 +
                    nir_intrinsic_component(i);
   nir_scalar components[NIR_MAX_VEC_COMPONENTS];
   bool progress = false;
   b->cursor = nir_after_instr(&i->instr);
   for (unsigned c = 0; c < i->def.num_components; c++) {
      components[c] = nir_get_scalar(&i->def, c);
      for (unsigned gate = 0; gate < input->gates->count; gate++) {
         unsigned bit = 1u << gate;
         if (input->gates->word[gate] != first+c ||
             !((input->key->zero | input->key->one) & bit))
            continue;
         components[c] = nir_get_scalar(nir_imm_float(b,
                                  (input->key->one & bit) ? 1.0f : 0.0f), 0);
         progress = true;
      }
   }
   if (progress)
      nir_def_rewrite_uses_after(&i->def,
                         nir_vec_scalars(b, components, i->def.num_components));
   return progress;
}

static bool
source_program_fits(const struct m9_vs_program *p, unsigned slots, unsigned consts)
{
   return m9_vs_slot_count(p) <= slots && m9_vs_extra_slot_count(p) <= slots &&
          (!m9_vs_producer_slot_count(p) ||
           m9_vs_producer_slot_count(p) + M9_VS_TAIL_SLOTS <= slots) &&
          m9_vs_const_count(p) <= consts;
}

bool
m9_vs_compile_gate_source(nir_shader **nir, const struct m9_vs_outputs *outputs,
                            struct m9_vs_program *program, const char **reason,
                            unsigned max_slots, unsigned max_const_pairs,
                            const struct m9_vs_format_key *format_key,
                            const struct m9_vs_uniform_gates *gates,
                            const struct m9_vs_gate_key *gate_key)
{
   bool known = gates && gate_key && gates->count <= M9_VS_MAX_GATE_UNIFORMS &&
                !(gate_key->zero & gate_key->one) &&
                !((gate_key->zero | gate_key->one) >> gates->count) &&
                (gate_key->zero | gate_key->one);
   nir_shader *saved = known ? nir_shader_clone(ralloc_parent(*nir), *nir) : NULL;
   const char *original_reason = NULL;
   bool ok = m9_vs_compile_format_source(nir, outputs, program, &original_reason,
                                            max_slots, max_const_pairs, format_key);
   if (saved && (!ok || !source_program_fits(program,max_slots,max_const_pairs))) {
      struct uniform_gate_input input = {gates,gate_key};
      if (nir_shader_intrinsics_pass(saved, uniform_gate_input,
                                     nir_metadata_control_flow, &input)) {
         m9_nir_fold_specialization(saved);
         struct m9_vs_program trial;
         if (m9_vs_compile_format_source(&saved,outputs,&trial,NULL,
                                           max_slots,max_const_pairs,format_key)) {
            if (source_program_fits(&trial,max_slots,max_const_pairs)) {
               if (ok) m9_vs_program_fini(program);
               *program = trial;
               ralloc_free(*nir);
               *nir = saved;
               if (reason) *reason = NULL;
               return true;
            }
            m9_vs_program_fini(&trial);
         }
      }
   }
   ralloc_free(saved);
   if (reason) *reason = original_reason;
   return ok;
}

/* Retain a compilable source for a draw-selected bounded layout. A shader
 * declaring more outputs than FIRST/CONTINUE can publish must still request
 * at most five live streams when its fragment variant is known. This seed
 * is never used as the draw's output image. */
bool
m9_vs_compile_native_source(nir_shader **nir, struct m9_vs_program *program,
                             const char **reason, unsigned max_slots,
                             unsigned max_const_pairs,
                             const struct m9_vs_format_key *format_key,
                             const struct m9_vs_uniform_gates *gates,
                             const struct m9_vs_gate_key *gate_key)
{
   const char *why = NULL;
   bool ok = m9_vs_compile_gate_source(nir, NULL, program, &why, max_slots,
                                        max_const_pairs, format_key, gates,
                                        gate_key);
   if (!ok && why && !strcmp(why, "more than five varyings")) {
      const struct m9_vs_outputs position_only = {0};
      ok = m9_vs_compile_gate_source(nir, &position_only, program, &why,
                                      max_slots, max_const_pairs, format_key,
                                      gates, gate_key);
   }
   if (reason)
      *reason = why;
   return ok;
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
   unsigned dword = 0, sources = 0;

#define REFUSE(why) do { if (reason) *reason = (why); return false; } while (0)
   if (!program || !out || (element_count && !elements) ||
       program->attr_count > M9_VS_MAX_ATTRIBUTES)
      REFUSE("no vertex elements");
   memset(out, 0, sizeof(*out));
   for (unsigned i = 0; i < program->attr_count; i++) {
      const struct m9_vs_attr *a = &program->attrs[i];
      unsigned lanes = a->scalar_count ? a->scalar_count : 1;

      if (a->offset != dword || lanes > 4 ||
          sources + lanes > M9_VS_MAX_SOURCE_ATTRIBUTES ||
          (a->scalar_count && (a->constant || a->components != 4)))
         REFUSE("the record layout disagrees with the declaration");
      for (unsigned lane = 0; lane < lanes; lane++) {
         struct m9_vs_source_attr *s = &out->attr[sources++];
         const struct pipe_vertex_element *e;
         unsigned element;

         /* A coefficient slot has no element: the caller writes its four
          * floats into every record of the draw. */
         if (a->constant) {
            s->constant = true;
            s->coeff_stream = a->coeff_stream;
            s->components = 4;
            s->dword = dword;
            continue;
         }
         if (a->scalar_count) {
            unsigned location = a->scalar_location[lane];
            if (location >= 64 || !(program->inputs_read & BITFIELD64_BIT(location)))
               REFUSE("a packed scalar has no vertex input");
            element = util_bitcount64(program->inputs_read & BITFIELD64_MASK(location));
         } else {
            element = m9_vs_attr_element(program, i);
         }
         if (element >= element_count)
            REFUSE("an attribute has no vertex element");
         e = &elements[element];
         if (!buffers || e->vertex_buffer_index >= buffer_count)
            REFUSE("an attribute has no vertex buffer");
         if (e->instance_divisor)
            REFUSE("an instanced attribute");
         if (e->dual_slot)
            REFUSE("a dual-slot attribute");
         if (!m9_vs_source_format_ok(e->src_format))
            REFUSE("a vertex format the record cannot carry");
         uint64_t offset = (uint64_t)buffers[e->vertex_buffer_index].buffer_offset +
                           e->src_offset;
         if (offset > UINT_MAX)
            REFUSE("an attribute offset leaves the vertex buffer");
         s->buffer = e->vertex_buffer_index;
         s->src_offset = offset;
         s->stride = e->src_stride;
         s->format = e->src_format;
         /* Each scalar semantic contributes its own component zero, from
          * its own live source. Other element components remain irrelevant. */
         s->scalar = a->scalar_count != 0;
         s->scalar_first = s->scalar && lane == 0;
         s->components = s->scalar ? 1 : MIN2(a->components, 4u);
         s->dword = dword + (s->scalar ? lane : 0);
      }
      dword += 4;
   }
   out->attr_count = sources;
   out->record_dwords = dword;
   if (!dword || dword != program->record_bytes / 4)
      REFUSE("the record size disagrees with the declaration");
   return true;
#undef REFUSE
}

/* A nonzero stride cannot address a vertex above UINT_MAX in this bounded
 * mapping. After that guard, unsigned stride times vertex fits uint64_t;
 * compare it against the remaining bytes without a division per gather.
 * Zero stride consumes only the texel, even for a high draw/index base. */
static bool
m9_vs_source_range(const struct m9_vs_source_attr *s, unsigned size,
                   uint64_t last)
{
   uint64_t end = (uint64_t)s->src_offset + util_format_get_blocksize(s->format);

   return end <= size && (!s->stride ||
                          (last <= UINT_MAX && last * s->stride <= size - end));
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

/* A packed group is initialized by its first logical source, then each
 * source writes only its own lane. The unoccupied lanes stay GL defaults. */
static void
m9_vs_source_store(const struct m9_vs_source_attr *s, float *dst,
                    const float rgba[4])
{
   if (s->scalar) {
      if (s->scalar_first) {
         dst[0] = dst[1] = dst[2] = 0.0f;
         dst[3] = 1.0f;
      }
      dst[0] = rgba[0];
   } else {
      for (unsigned c = 0; c < 4; c++)
         dst[c] = c < s->components ? rgba[c] : (c == 3 ? 1.0f : 0.0f);
   }
}

/* Channels of a plain 32-bit float format, which are copied without the
 * generic unpack; zero for every other format. */
static unsigned
m9_vs_source_float_channels(enum pipe_format format)
{
   switch (format) {
   case PIPE_FORMAT_R32_FLOAT:
      return 1;
   case PIPE_FORMAT_R32G32_FLOAT:
      return 2;
   case PIPE_FORMAT_R32G32B32_FLOAT:
      return 3;
   case PIPE_FORMAT_R32G32B32A32_FLOAT:
      return 4;
   default:
      return 0;
   }
}

static inline void
m9_vs_source_fetch(const struct util_format_unpack_description *unpack,
                   unsigned float_channels, const uint8_t *src, float rgba[4])
{
   if (float_channels) {
      rgba[0] = rgba[1] = rgba[2] = 0.0f;
      rgba[3] = 1.0f;
      memcpy(rgba, src, float_channels * sizeof(float));
   } else {
      unpack->unpack_rgba(rgba, src, 1);
   }
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
   for (unsigned v = 0; v < count; v++)
      m9_vs_source_store(s, out + (uint64_t)v * layout->record_dwords + s->dword,
                         rgba);
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
      const unsigned nf = m9_vs_source_float_channels(s->format);
      /* The last vertex this draw reads must lie inside the mapping. */
      if (!m9_vs_source_range(s, buffer_size[s->buffer],
                              (uint64_t)first + count - 1))
         return false;

      for (unsigned v = 0; v < count; v++) {
         const uint8_t *src = buffer_map[s->buffer] +
                              ((uint64_t)first + v) * s->stride + s->src_offset;
         float *dst = out + (uint64_t)v * layout->record_dwords + s->dword;
         float rgba[4];

         m9_vs_source_fetch(unpack, nf, src, rgba);
         m9_vs_source_store(s, dst, rgba);
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
      const unsigned nf = m9_vs_source_float_channels(s->format);
      for (unsigned v = 0; v < count; v++) {
         uint64_t at = (uint64_t)first + vertex[v];
         const uint8_t *src;
         float *dst = out + (uint64_t)v * layout->record_dwords + s->dword;
         float rgba[4];

         if (!m9_vs_source_range(s, buffer_size[s->buffer], at))
            return false;
         src = buffer_map[s->buffer] + at * s->stride + s->src_offset;
         m9_vs_source_fetch(unpack, nf, src, rgba);
         m9_vs_source_store(s, dst, rgba);
      }
   }
   return true;
}

struct clip_input {
   const uint8_t *base;
   unsigned stride;
   unsigned floats;        /* direct 32-bit float components, 0 to unpack */
   const struct util_format_unpack_description *unpack;
   float rgba[4];
};

static void
clip_fetch(struct clip_input *in, unsigned vertex)
{
   const uint8_t *src = in->base + (uint64_t)vertex * in->stride;

   if (in->floats) {
      in->rgba[0] = in->rgba[1] = in->rgba[2] = 0.0f;
      in->rgba[3] = 1.0f;
      memcpy(in->rgba, src, in->floats * sizeof(float));
   } else {
      in->unpack->unpack_rgba(in->rgba, src, 1);
   }
}

static bool
clip_constant(const float *v, unsigned terms)
{
   for (unsigned t = 1; v && t < terms; t++)
      if (v[t] != 0.0f)
         return false;
   return true;
}

/* Folds the node list into z, w = affine functions of the fetched inputs
 * (coefficient 0 the constant term, then four a used element).  False when
 * a product of two vertex-dependent terms makes it non-affine. */
static bool
clip_affine(const struct m9_vs_program *program, const uint8_t *constants,
            const int *slot, unsigned terms, float *aff)
{
   const struct m9_vs_clip_node *node = program->clip_nodes.data;
   unsigned nodes = util_dynarray_num_elements(&program->clip_nodes,
                                               struct m9_vs_clip_node);

   for (unsigned i = 0; i < nodes; i++) {
      const struct m9_vs_clip_node *n = &node[i];
      float *r = aff + (size_t)i * terms;
      bool operands = n->op >= M9_VS_CLIP_ADD;
      const float *a = operands ? aff + (size_t)n->a * terms : NULL;
      const float *b = n->op == M9_VS_CLIP_ADD || n->op == M9_VS_CLIP_MUL ||
                       n->op == M9_VS_CLIP_FMA ? aff + (size_t)n->b * terms : NULL;
      const float *c = n->op == M9_VS_CLIP_FMA ? aff + (size_t)n->c * terms : NULL;
      bool a_const = clip_constant(a, terms), b_const = clip_constant(b, terms);

      memset(r, 0, terms * sizeof(float));
      switch (n->op) {
      case M9_VS_CLIP_LIT:
         r[0] = n->lit;
         break;
      case M9_VS_CLIP_INPUT:
         r[1 + 4 * slot[util_bitcount64(program->inputs_read &
                                        BITFIELD64_MASK(n->a))] + n->b] = 1.0f;
         break;
      case M9_VS_CLIP_UNIFORM:
         memcpy(&r[0], constants + n->a, sizeof(float));
         break;
      case M9_VS_CLIP_ADD:
         for (unsigned t = 0; t < terms; t++)
            r[t] = a[t] + b[t];
         break;
      case M9_VS_CLIP_MUL:
      case M9_VS_CLIP_FMA:
         if (!a_const && !b_const)
            return false;
         for (unsigned t = 0; t < terms; t++)
            r[t] = a_const ? a[0] * b[t] : a[t] * b[0];
         if (n->op == M9_VS_CLIP_FMA)
            for (unsigned t = 0; t < terms; t++)
               r[t] += c[t];
         break;
      case M9_VS_CLIP_NEG:
         for (unsigned t = 0; t < terms; t++)
            r[t] = -a[t];
         break;
      case M9_VS_CLIP_ABS:
         if (!a_const)
            return false;
         r[0] = fabsf(a[0]);
         break;
      }
   }
   return true;
}

static void
clip_node_eval(const struct m9_vs_program *program, const uint8_t *constants,
               const struct clip_input *input, float *value)
{
   const struct m9_vs_clip_node *node = program->clip_nodes.data;
   unsigned nodes = util_dynarray_num_elements(&program->clip_nodes,
                                               struct m9_vs_clip_node);

   for (unsigned i = 0; i < nodes; i++) {
      const struct m9_vs_clip_node *n = &node[i];

      switch (n->op) {
      case M9_VS_CLIP_LIT:
         value[i] = n->lit;
         break;
      case M9_VS_CLIP_INPUT:
         value[i] = input[util_bitcount64(program->inputs_read &
                                          BITFIELD64_MASK(n->a))].rgba[n->b];
         break;
      case M9_VS_CLIP_UNIFORM:
         memcpy(&value[i], constants + n->a, sizeof(float));
         break;
      case M9_VS_CLIP_ADD:
         value[i] = value[n->a] + value[n->b];
         break;
      case M9_VS_CLIP_MUL:
         value[i] = value[n->a] * value[n->b];
         break;
      case M9_VS_CLIP_FMA:
         value[i] = value[n->a] * value[n->b] + value[n->c];
         break;
      case M9_VS_CLIP_NEG:
         value[i] = -value[n->a];
         break;
      case M9_VS_CLIP_ABS:
         value[i] = fabsf(value[n->a]);
         break;
      }
   }
}

int
m9_vs_clip_outside_near(const struct m9_vs_program *program,
                        const struct pipe_vertex_element *elements,
                        unsigned element_count,
                        const struct pipe_vertex_buffer *buffers,
                        const uint8_t *const *buffer_map,
                        const unsigned *buffer_size, unsigned buffer_count,
                        const uint8_t *constants, size_t constant_size,
                        unsigned first, unsigned count, uint8_t *flags)
{
   const struct m9_vs_clip_node *node = program->clip_nodes.data;
   unsigned nodes = util_dynarray_num_elements(&program->clip_nodes,
                                               struct m9_vs_clip_node);
   struct clip_input input[PIPE_MAX_ATTRIBS];
   int slot[PIPE_MAX_ATTRIBS];
   unsigned used_count = 0, terms;
   uint64_t used = 0;
   float *scratch;
   int outside = 0;

   if (!nodes || !count || program->clip_z >= nodes || program->clip_w >= nodes)
      return -1;
   for (unsigned i = 0; i < nodes; i++) {
      const struct pipe_vertex_element *e;
      const struct util_format_description *desc;
      unsigned element, size;
      uint64_t end;

      if (node[i].op == M9_VS_CLIP_UNIFORM &&
          (!constants || (size_t)node[i].a + sizeof(float) > constant_size))
         return -1;
      if (node[i].op != M9_VS_CLIP_INPUT)
         continue;
      if (node[i].a >= 64 || node[i].b >= 4 ||
          !(program->inputs_read & BITFIELD64_BIT(node[i].a)))
         return -1;
      element = util_bitcount64(program->inputs_read &
                                BITFIELD64_MASK(node[i].a));
      if (element >= element_count || element >= PIPE_MAX_ATTRIBS)
         return -1;
      if (used & BITFIELD64_BIT(element))
         continue;
      e = &elements[element];
      if (e->vertex_buffer_index >= buffer_count || e->instance_divisor ||
          !buffer_map[e->vertex_buffer_index])
         return -1;
      input[element].unpack = util_format_unpack_description(e->src_format);
      if (!input[element].unpack || !input[element].unpack->unpack_rgba)
         return -1;
      desc = util_format_description(e->src_format);
      input[element].floats = desc && util_format_is_float(e->src_format) &&
                              desc->is_array && desc->channel[0].size == 32 &&
                              desc->swizzle[0] == PIPE_SWIZZLE_X ?
                              desc->nr_channels : 0;
      /* Same bounds as the record writer: the last vertex read must fit. */
      size = buffer_size[e->vertex_buffer_index];
      end = (uint64_t)buffers[e->vertex_buffer_index].buffer_offset +
            e->src_offset + util_format_get_blocksize(e->src_format);
      if (end > size || (e->src_stride &&
                         ((uint64_t)first + count - 1) * e->src_stride > size - end))
         return -1;
      input[element].base = buffer_map[e->vertex_buffer_index] +
                            buffers[e->vertex_buffer_index].buffer_offset +
                            e->src_offset;
      input[element].stride = e->src_stride;
      slot[element] = used_count++;
      used |= BITFIELD64_BIT(element);
   }
   terms = 1 + 4 * used_count;
   scratch = malloc(sizeof(*scratch) * MAX2((size_t)nodes * terms, nodes));
   if (!scratch)
      return -1;
   if (clip_affine(program, constants, slot, terms, scratch)) {
      const float *z = scratch + (size_t)program->clip_z * terms;
      const float *w = scratch + (size_t)program->clip_w * terms;

      unsigned e0 = used_count == 1 ? (unsigned)ffsll(used) - 1 : 0;

      /* One float position element: read it in place. */
      if (used_count == 1 && input[e0].floats >= 3) {
         const struct clip_input *in = &input[e0];
         const float *zc = z + 1, *wc = w + 1;
         bool four = in->floats >= 4;

         for (unsigned v = 0; v < count && (flags || !outside); v++) {
            float p[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

            if (four)
               memcpy(p, in->base + (uint64_t)(first + v) * in->stride, 16);
            else
               memcpy(p, in->base + (uint64_t)(first + v) * in->stride, 12);
            float cz = z[0] + zc[0] * p[0] + zc[1] * p[1] + zc[2] * p[2] +
                       zc[3] * p[3];
            float cw = w[0] + wc[0] * p[0] + wc[1] * p[1] + wc[2] * p[2] +
                       wc[3] * p[3];

            bool o = !(cw > 0.0f && cz >= -cw);

            if (flags)
               flags[v] = o;
            outside |= o;
         }
      } else {
         for (unsigned v = 0; v < count && (flags || !outside); v++) {
            float cz = z[0], cw = w[0];

            u_foreach_bit64(element, used) {
               struct clip_input *in = &input[element];
               unsigned t = 1 + 4 * slot[element];

               clip_fetch(in, first + v);
               for (unsigned k = 0; k < 4; k++) {
                  cz += z[t + k] * in->rgba[k];
                  cw += w[t + k] * in->rgba[k];
               }
            }
            bool o = !(cw > 0.0f && cz >= -cw);

            if (flags)
               flags[v] = o;
            outside |= o;
         }
      }
   } else {
      for (unsigned v = 0; v < count && (flags || !outside); v++) {
         u_foreach_bit64(element, used)
            clip_fetch(&input[element], first + v);
         clip_node_eval(program, constants, input, scratch);
         bool o = !(scratch[program->clip_w] > 0.0f &&
                    scratch[program->clip_z] >= -scratch[program->clip_w]);

         if (flags)
            flags[v] = o;
         outside |= o;
      }
   }
   free(scratch);
   return outside;
}
