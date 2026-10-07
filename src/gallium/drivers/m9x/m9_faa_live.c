/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include "m9_faa_live.h"
#include "m9_vs_source.h"
#include "nir.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(struct m9_faa_public_upload) == 796, "upload ABI");

static bool
overlap(const void *a, size_t na, const void *b, size_t nb)
{
   uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
   return na && nb && (x <= y ? y - x < na : x - y < nb);
}

/* Classification never rewrites the caller's NIR or evaluates a vertex ALU.
 * Only scalar MOV/vec chains may feed the complete supported stores. */
static bool
stores(const nir_shader *shader, mesa_shader_stage stage,
       nir_intrinsic_instr **out, unsigned count)
{
   if (!shader || shader->info.stage != stage || !shader->info.io_lowered ||
       shader->xfb_info)
      return false;
   nir_function_impl *impl = nir_shader_get_entrypoint((nir_shader *)shader);
   if (!impl || !exec_list_is_singular(&impl->body))
      return false;
   nir_cf_node *node = exec_node_data(nir_cf_node, exec_list_get_head(&impl->body), node);
   if (node->type != nir_cf_node_block)
      return false;
   unsigned n = 0;
   nir_foreach_instr(instr, nir_cf_node_as_block(node)) {
      if (instr->type == nir_instr_type_alu || instr->type == nir_instr_type_load_const)
         continue;
      if (instr->type != nir_instr_type_intrinsic)
         return false;
      nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
      switch (in->intrinsic) {
      case nir_intrinsic_load_input:
      case nir_intrinsic_load_ubo:
      case nir_intrinsic_load_ubo_vec4:
         break;
      case nir_intrinsic_store_output:
         if (nir_intrinsic_io_semantics(in).dual_source_blend_index ||
             nir_intrinsic_io_semantics(in).fb_fetch_output ||
             nir_intrinsic_io_semantics(in).fb_fetch_output_coherent ||
             nir_intrinsic_io_semantics(in).per_view ||
             nir_intrinsic_io_semantics(in).high_16bits ||
             nir_intrinsic_io_semantics(in).high_dvec2 ||
             nir_intrinsic_io_semantics(in).no_sysval_output)
            return false;
         if (n == count || nir_intrinsic_component(in) ||
             nir_intrinsic_write_mask(in) != 15 ||
             nir_intrinsic_src_type(in) != nir_type_float32 ||
             in->src[0].ssa->bit_size != 32 || in->src[0].ssa->num_components != 4 ||
             !nir_src_is_const(in->src[1]) || nir_src_as_uint(in->src[1]) ||
             nir_intrinsic_io_semantics(in).num_slots != 1)
            return false;
         out[n++] = in;
         break;
      default:
         return false;
      }
   }
   return n == count;
}

static bool
input_vec(nir_def *value, unsigned *location)
{
   for (unsigned c = 0; c < 4; c++) {
      nir_scalar s = nir_scalar_chase_movs(nir_get_scalar(value, c));
      if (s.def->bit_size != 32 || !nir_scalar_is_intrinsic(s))
         return false;
      nir_intrinsic_instr *in = nir_def_as_intrinsic(s.def);
      if (in->intrinsic != nir_intrinsic_load_input ||
          nir_intrinsic_dest_type(in) != nir_type_float32 ||
          !nir_src_is_const(in->src[0]) || nir_src_as_uint(in->src[0]) ||
          nir_intrinsic_component(in) + s.comp != c)
         return false;
      unsigned loc = nir_intrinsic_io_semantics(in).location;
      if (loc >= 64 || (c && loc != *location))
         return false;
      *location = loc;
   }
   return true;
}

void
m9_faa_gl_upload_fini(struct m9_faa_gl_upload *upload)
{
   if (upload) {
      free(upload->storage);
      memset(upload, 0, sizeof(*upload));
   }
}

bool
m9_faa_live_gl_build(const struct m9_faa_live_gl *s,
                       struct m9_faa_gl_upload *output, const char **reason)
{
   struct m9_vs_program p = {0};
   struct m9_faa_gl_upload result = {0};
   const char *why = "invalid live FAA GL upload inputs";
   bool compiled = false;
   uint32_t carry[2 * M9_FAA_GL_UPLOAD_SLOT_BUDGET], gather[10], position[50];
   uint32_t carry_constants[126], position_constants[26], declaration[M9_VS_MAX_DECL];
   struct m9_faa_move_interface gi;
   struct m9_faa_position_interface pi;
   struct m9_vs_source_layout source;
   if (!s || !output || output->storage || !s->vs || !s->linkage ||
       (s->linkage->count < 1 || s->linkage->count > 2) || !s->count || s->count > M9_FAA_GL_UPLOAD_MAX_VERTICES ||
       !s->elements || !s->buffers || !s->maps || !s->map_bytes ||
       s->element_count > PIPE_MAX_ATTRIBS || s->buffer_count > PIPE_MAX_ATTRIBS ||
       (s->cb0_bytes && !s->cb0) || s->cb0_bytes > UINT32_MAX ||
       (uintptr_t)s->cb0 % _Alignof(float) ||
       !s->fragment.words || s->fragment.slots > M9_FAA_GL_UPLOAD_SLOT_BUDGET ||
       s->fragment.slots < 4 || s->fragment_constants.pairs > 63 ||
       (s->fragment_constants.pairs && (!s->fragment_constants.words ||
        (uintptr_t)s->fragment_constants.words % _Alignof(uint32_t))) ||
       overlap(output, sizeof(*output), s, sizeof(*s)) ||
       overlap(output, sizeof(*output), s->cb0, s->cb0_bytes) ||
       overlap(output, sizeof(*output), s->fragment.words, s->fragment.slots * 8) ||
       overlap(output, sizeof(*output), s->fragment_constants.words, s->fragment_constants.pairs * 8))
      goto fail;
   for (unsigned i = 0; i < s->linkage->count; i++) {
      const struct m9_vs_output *o = &s->linkage->stream[i];
      if (o->coordinate || o->paired || o->coeff_record || o->component ||
          o->location == VARYING_SLOT_POS || o->location >= 64 ||
          (i && o->location == s->linkage->stream[0].location))
         goto fail;
   }
   for (unsigned i = 0; i < s->buffer_count; i++)
      if (overlap(output, sizeof(*output), s->maps[i], s->map_bytes[i]))
         goto fail;
   if (!m9_vs_compile_faa_gl(s->vs, s->linkage, &p, &why))
      goto fail;
   compiled = true;
   unsigned slots = m9_vs_producer_slot_count(&p), pairs = m9_vs_const_count(&p);
   if (slots > M9_FAA_GL_UPLOAD_SLOT_BUDGET || pairs > 63 || p.varying_count != s->linkage->count ||
       !p.record_bytes || p.record_bytes % 16 || p.texture_count) {
      why = "FAA GL producer limits or direct stream interface";
      goto fail;
   }
   /* The generic constant resolver deliberately supplies GL defaults for
    * missing words. This owner snapshot instead requires every live key to
    * be within the actual retained CB0, before resolving any dense span. */
   util_dynarray_foreach(&p.consts, struct m9_vs_const, key) {
      if (key->kind == M9_VS_CONST_COORD) {
         why = "FAA coordinate constant binding is not qualified";
         goto fail;
      }
      if (key->kind == M9_VS_CONST_UNIFORM)
         for (unsigned lane = 0; lane < 2; lane++) {
            uint32_t at = key->offset[lane];
            if (at != M9_VS_UNIFORM_ZERO &&
                ((at & 3) || at > s->cb0_bytes || s->cb0_bytes - at < 4)) {
               why = "FAA GL uniform key leaves retained CB0";
               goto fail;
            }
         }
   }
   if (m9_vs_emit_faa(&p, carry, ARRAY_SIZE(carry)) != 2 * slots ||
       m9_vs_constants(&p, s->cb0, s->cb0_bytes / 4, carry_constants,
                          ARRAY_SIZE(carry_constants)) != pairs ||
       !m9_vs_source_layout_init(&p, s->elements, s->element_count, s->buffers,
                                  s->buffer_count, &source, &why))
      goto fail;
   unsigned declarations = m9_vs_decl_words(&p, declaration, ARRAY_SIZE(declaration));
   const uint8_t inputs[2] = {0, 1};
   if (!declarations ||
       !m9_faa_preprocess_moves(M9_FAA_MOVE_RECORDS, inputs, p.varying_count, 0, gather, sizeof(gather), &gi) ||
       !m9_faa_preprocess_position(&s->viewport, position, sizeof(position),
                                    (float *)position_constants, sizeof(position_constants), &pi)) {
      why = "FAA GL declaration or position interface";
      goto fail;
   }
   const struct m9_faa_program_vector vectors[4] = {
      s->fragment, {carry, slots}, {gather, gi.slots}, {position, pi.slots},
   };
   const struct m9_faa_dense_constants constants[4] = {
      s->fragment_constants, {carry_constants, pairs}, {NULL, 0},
      {position_constants, pi.constant_pairs},
   };
   unsigned program_bytes = 0;
   for (unsigned i = 0; i < 4; i++) program_bytes += vectors[i].slots * 8;
   result.constants_bytes = 16;
   for (unsigned i = 0; i < 4; i++) {
      result.constant_begin[i] = result.constants_bytes;
      result.constants_bytes += constants[i].pairs * 8;
      result.constant_end[i] = result.constants_bytes;
   }
   /* All capacities follow the compiled stride and the live vertex count,
    * with a bounded CPU budget. This does not establish native draw lengths. */
   uint64_t vertices = (uint64_t)s->count * p.record_bytes;
   if (vertices > UINT32_MAX - 128u) goto fail;
   result.vertex_bytes = vertices; result.vertex_offset = 128;
   result.geometry_bytes = 128 + result.vertex_bytes;
   result.vertex_count = s->count; result.record_bytes = p.record_bytes;
   result.declaration_words = declarations;
   memcpy(result.varying_location, p.varying_location, sizeof(result.varying_location));
   for (unsigned i = 0; i <= p.varying_count; i++)
      for (unsigned half = 0; half < 2; half++)
         result.carry_output_registers |= UINT64_C(1) << (0x11 + 4 * i + 2 * half);
   size_t bytes = (size_t)program_bytes + result.constants_bytes + result.geometry_bytes + declarations * 4u;
   result.storage = calloc(1, bytes);
   if (!result.storage) {why = "out of memory"; goto fail;}
   result.storage_bytes = bytes;
   result.program = result.storage;
   result.constants = result.program + program_bytes / 4;
   result.geometry = result.constants + result.constants_bytes / 4;
   result.declaration = result.geometry + result.geometry_bytes / 4;
   if (!m9_faa_program_link(vectors, 4, result.program, program_bytes, &result.layout)) {
      why = "FAA live fragment or preprocessing vector finish";
      goto fail;
   }
   const uint32_t utility[4] = {0x3f800000, 0, 0, 0x3f800000};
   memcpy(result.constants, utility, sizeof(utility));
   for (unsigned i = 0; i < 4; i++)
      if (constants[i].pairs)
         memcpy((uint8_t *)result.constants + result.constant_begin[i],
                constants[i].words, constants[i].pairs * 8);
   const float header[8] = {s->viewport.width * .5f, s->viewport.height * -.5f,
      1, 0, s->viewport.x + s->viewport.width * .5f,
      s->viewport.y + s->viewport.height * .5f, 0, 0};
   memcpy(result.geometry, header, sizeof(header));
   bool records = s->indices ?
      m9_vs_source_records_gather(&source, s->maps, s->map_bytes, s->buffer_count,
                                   s->first, s->indices, s->count,
                                   (float *)((uint8_t *)result.geometry + 128)) :
      m9_vs_source_records(&source, s->maps, s->map_bytes, s->buffer_count,
                             s->first, s->count,
                             (float *)((uint8_t *)result.geometry + 128));
   if (!records) {why = "FAA GL raw input mapping bounds"; goto fail;}
   memcpy(result.declaration, declaration, declarations * 4);
   m9_vs_program_fini(&p);
   *output = result;
   if (reason) *reason = NULL;
   return true;
fail:
   if (compiled) m9_vs_program_fini(&p);
   m9_faa_gl_upload_fini(&result);
   if (reason) *reason = why;
   return false;
}

static bool
fragment_color(const nir_shader *fs, bool *flat, bool *legacy)
{
   if (!fs || fs->info.stage != MESA_SHADER_FRAGMENT || !fs->info.io_lowered || fs->xfb_info)
      return false;
   nir_function_impl *impl = nir_shader_get_entrypoint((nir_shader *)fs);
   if (!impl || !exec_list_is_singular(&impl->body)) return false;
   nir_cf_node *node = exec_node_data(nir_cf_node, exec_list_get_head(&impl->body), node);
   if (node->type != nir_cf_node_block) return false;
   unsigned written = 0, output_location = UINT32_MAX, interpolation = 0;
   nir_foreach_instr(instr, nir_cf_node_as_block(node)) {
      if (instr->type == nir_instr_type_alu || instr->type == nir_instr_type_load_const)
         continue;
      if (instr->type != nir_instr_type_intrinsic) return false;
      nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
      if (in->intrinsic == nir_intrinsic_load_barycentric_pixel) {
         if (nir_intrinsic_interp_mode(in) != INTERP_MODE_SMOOTH &&
             nir_intrinsic_interp_mode(in) != INTERP_MODE_NONE) return false;
         continue;
      }
      if (in->intrinsic == nir_intrinsic_load_input) {
         nir_io_semantics io = nir_intrinsic_io_semantics(in);
         if (io.location != VARYING_SLOT_COL0 || io.num_slots != 1 ||
             io.per_view || io.high_16bits || io.high_dvec2 ||
             nir_intrinsic_dest_type(in) != nir_type_float32 ||
             in->def.bit_size != 32 || !nir_src_is_const(in->src[0]) ||
             nir_src_as_uint(in->src[0]) || (interpolation && interpolation != 2))
            return false;
         interpolation = 2;
         continue;
      }
      if (in->intrinsic == nir_intrinsic_load_interpolated_input) {
         if (nir_intrinsic_io_semantics(in).location != VARYING_SLOT_COL0 ||
             nir_intrinsic_dest_type(in) != nir_type_float32 ||
             in->def.bit_size != 32 ||
             !nir_src_is_const(in->src[1]) || nir_src_as_uint(in->src[1]))
            return false;
         nir_def *bary = in->src[0].ssa;
         if (nir_def_instr(bary)->type != nir_instr_type_intrinsic ||
             nir_def_as_intrinsic(bary)->intrinsic != nir_intrinsic_load_barycentric_pixel)
            return false;
         unsigned mode = nir_intrinsic_interp_mode(nir_def_as_intrinsic(bary));
         unsigned kind = mode == INTERP_MODE_SMOOTH ? 1 : mode == INTERP_MODE_NONE ? 3 : 0;
         if (!kind || (interpolation && interpolation != kind)) return false;
         interpolation = kind;
         continue;
      }
      if (in->intrinsic != nir_intrinsic_store_output) return false;
      nir_io_semantics io = nir_intrinsic_io_semantics(in);
      if ((io.location != FRAG_RESULT_COLOR && io.location != FRAG_RESULT_DATA0) ||
          (output_location != UINT32_MAX && output_location != io.location) ||
          io.dual_source_blend_index || io.fb_fetch_output || io.fb_fetch_output_coherent ||
          io.per_view || io.high_16bits || io.high_dvec2 || io.no_sysval_output ||
          io.num_slots != 1 || nir_intrinsic_src_type(in) != nir_type_float32 ||
          in->src[0].ssa->bit_size != 32 || !nir_src_is_const(in->src[1]) ||
          nir_src_as_uint(in->src[1]))
         return false;
      output_location = io.location;
      unsigned mask = nir_intrinsic_write_mask(in), base = nir_intrinsic_component(in);
      if (!mask || base >= 4 || mask >> in->src[0].ssa->num_components || mask >> (4 - base))
         return false;
      for (unsigned c = 0; c < in->src[0].ssa->num_components; c++) {
         if (!(mask & (1u << c))) continue;
         unsigned bit = 1u << (base + c);
         if (written & bit) return false;
         nir_scalar scalar = nir_scalar_chase_movs(nir_get_scalar(in->src[0].ssa, c));
         if (!nir_scalar_is_intrinsic(scalar)) return false;
         nir_intrinsic_instr *load = nir_def_as_intrinsic(scalar.def);
         if ((load->intrinsic != nir_intrinsic_load_interpolated_input &&
              load->intrinsic != nir_intrinsic_load_input) ||
             nir_intrinsic_io_semantics(load).location != VARYING_SLOT_COL0 ||
             nir_intrinsic_component(load) + scalar.comp != base + c)
            return false;
         written |= bit;
      }
   }
   if (written != 15 || !interpolation) return false;
   *flat = interpolation == 2;
   *legacy = interpolation == 3;
   return true;
}

bool
m9_faa_live_gl_color_build_state(const struct m9_faa_live_gl *s, const nir_shader *fs, bool legacy_flat,
                          struct m9_faa_gl_upload *output, const char **reason)
{
   uint32_t program[16], constants[4];
   struct m9_faa_color_fs_interface interface;
   bool flat = false, legacy = false;
   if (!s || !s->linkage || s->linkage->count != 1 ||
       s->linkage->stream[0].location != VARYING_SLOT_COL0 || !fragment_color(fs, &flat, &legacy)) {
      if (reason) *reason = "FAA fragment must pass consistently smooth or flat COLOR0 to the complete color output";
      return false;
   }
   if (legacy) flat = legacy_flat;
   bool built = flat ? m9_faa_color_flat_fs_build(0, program, sizeof(program), constants, sizeof(constants), &interface) :
                       m9_faa_color_fs_build(0, program, sizeof(program), constants, sizeof(constants), &interface);
   if (!built) {
      if (reason) *reason = "FAA COLOR fragment encoding failed";
      return false;
   }
   struct m9_faa_live_gl live = *s;
   live.fragment = (struct m9_faa_program_vector){program, interface.slots};
   live.fragment_constants = (struct m9_faa_dense_constants){constants, interface.constant_pairs};
   if (!m9_faa_live_gl_build(&live, output, reason)) return false;
   /* The actual FIRST frontend emits distinct smooth/flat programs and
    * utility order. Native admission must bind matching fragment metadata. */
   output->color_flat = flat;
   output->color_legacy_interpolation = legacy;
   return true;
}

bool
m9_faa_live_gl_color_build(const struct m9_faa_live_gl *s, const nir_shader *fs,
                          struct m9_faa_gl_upload *output, const char **reason)
{
   return m9_faa_live_gl_color_build_state(s, fs, false, output, reason);
}

static bool
color_vec(nir_def *value, const void *cb, size_t bytes, bool uniforms_only,
          uint32_t words[4])
{
   uint64_t first = 0;
   for (unsigned c = 0; c < 4; c++) {
      nir_scalar s = nir_scalar_chase_movs(nir_get_scalar(value, c));
      if (s.def->bit_size != 32)
         return false;
      if (!uniforms_only && nir_scalar_is_const(s)) {
         words[c] = nir_scalar_as_uint(s);
      } else {
         if (!nir_scalar_is_intrinsic(s))
            return false;
         nir_intrinsic_instr *in = nir_def_as_intrinsic(s.def);
         if ((in->intrinsic != nir_intrinsic_load_ubo &&
              in->intrinsic != nir_intrinsic_load_ubo_vec4) ||
             !nir_src_is_const(in->src[0]) || nir_src_as_uint(in->src[0]) ||
             !nir_src_is_const(in->src[1]))
            return false;
         uint64_t at;
         if (in->intrinsic == nir_intrinsic_load_ubo)
            at = (uint64_t)nir_src_as_uint(in->src[1]) + 4u * s.comp;
         else if (in->intrinsic == nir_intrinsic_load_ubo_vec4)
            at = 16ull * ((uint64_t)nir_intrinsic_base(in) + nir_src_as_uint(in->src[1])) +
                 4u * (nir_intrinsic_component(in) + s.comp);
         else
            return false;
         if ((at & 3) || !cb || at > bytes || bytes - at < 4 ||
             (uniforms_only && ((c && at != first + 4u * c) || (!c && (at & 15)))))
            return false;
         if (!c) first = at;
         memcpy(&words[c], (const uint8_t *)cb + at, 4);
      }
      float f;
      memcpy(&f, &words[c], 4);
      if ((words[c] >> 31) || !isfinite(f) || f > 1.0f)
         return false;
   }
   return true;
}

/* Original 16fa20 MOV fields, selected from live supported shader semantics.
 * Constant selectors refer to each vector's own dense xy/zw pair order.
 * E=3f for public carry (different unused field from generic carry moves).
 * Finish belongs four slots before the end, from actual f40e3. */
static void
mov(uint32_t *out, unsigned source, unsigned target, unsigned cls,
    unsigned op, bool constant, bool clamp)
{
   out[0] = 0xffc00000u | source << 15 | target << 8 | cls << 5 | op |
            (clamp ? 0x80u : 0);
   out[1] = constant ? 0xa280331fu : 0xa28fff1fu;
}

bool
m9_faa_live_public_mov_build(const struct m9_faa_live_public_mov *s,
                              struct m9_faa_public_upload *output)
{
   if (!s || !output || !s->d3d_clip_space ||
       (uintptr_t)output % _Alignof(struct m9_faa_public_upload) ||
       sizeof(*output) > UINTPTR_MAX - (uintptr_t)output ||
       overlap(output, sizeof(*output), s, sizeof(*s)) ||
       overlap(output, sizeof(*output), s->vs_cb0, s->vs_cb0_bytes) ||
       overlap(output, sizeof(*output), s->fs_cb0, s->fs_cb0_bytes) ||
       !s->maps || !s->map_bytes || !s->buffers || !s->elements ||
       s->buffer_count > PIPE_MAX_ATTRIBS || s->element_count > PIPE_MAX_ATTRIBS ||
       s->color_location >= 64 || s->texcoord_location >= 64 ||
       s->color_location == s->texcoord_location ||
       s->color_location == VARYING_SLOT_POS || s->texcoord_location == VARYING_SLOT_POS)
      return false;
   if (overlap(output, sizeof(*output), s->elements, s->element_count * sizeof(*s->elements)) ||
       overlap(output, sizeof(*output), s->buffers, s->buffer_count * sizeof(*s->buffers)) ||
       overlap(output, sizeof(*output), s->maps, s->buffer_count * sizeof(*s->maps)) ||
       overlap(output, sizeof(*output), s->map_bytes, s->buffer_count * sizeof(*s->map_bytes)) ||
       overlap(output, sizeof(*output), s->vs, s->vs ? sizeof(*s->vs) : 0) ||
       overlap(output, sizeof(*output), s->fs, s->fs ? sizeof(*s->fs) : 0))
      return false;
   for (unsigned i = 0; i < s->buffer_count; i++)
      if (overlap(output, sizeof(*output), s->maps[i], s->map_bytes[i]))
         return false;
   nir_intrinsic_instr *vs[3], *fs[1], *pos = NULL, *col = NULL, *tex = NULL;
   if (!stores(s->vs, MESA_SHADER_VERTEX, vs, 3) ||
       !stores(s->fs, MESA_SHADER_FRAGMENT, fs, 1) ||
       (nir_intrinsic_io_semantics(fs[0]).location != FRAG_RESULT_COLOR &&
        nir_intrinsic_io_semantics(fs[0]).location != FRAG_RESULT_DATA0))
      return false;
   for (unsigned i = 0; i < 3; i++) {
      unsigned loc = nir_intrinsic_io_semantics(vs[i]).location;
      nir_intrinsic_instr **which = loc == VARYING_SLOT_POS ? &pos :
                                   loc == s->color_location ? &col :
                                   loc == s->texcoord_location ? &tex : NULL;
      if (!which || *which) return false;
      *which = vs[i];
   }
   if (!pos || !col || !tex) return false;
   unsigned position = 0, coord = 0;
   uint32_t colors[2][4];
   if (!input_vec(pos->src[0].ssa, &position) ||
       !input_vec(tex->src[0].ssa, &coord) || position == coord ||
       !color_vec(col->src[0].ssa, s->vs_cb0, s->vs_cb0_bytes, true, colors[1]) ||
       !color_vec(fs[0]->src[0].ssa, s->fs_cb0, s->fs_cb0_bytes, false, colors[0]))
      return false;
   struct m9_vs_program p = {0};
   p.attr_count = 2; p.record_bytes = 32;
   p.inputs_read = UINT64_C(1) << position | UINT64_C(1) << coord;
   for (unsigned i = 0; i < 2; i++) {
      p.attrs[i].location = i ? coord : position;
      p.attrs[i].offset = 4 * i; p.attrs[i].components = 4;
      for (unsigned c = 0; c < 4; c++) p.attrs[i].select[c] = c;
      unsigned element = m9_vs_attr_element(&p, i);
      if (element >= s->element_count ||
          s->elements[element].src_format != PIPE_FORMAT_R32G32B32A32_FLOAT)
         return false;
   }
   struct m9_vs_source_layout source;
   float vertices[24]; uint32_t decl[5];
   if (m9_vs_decl_words(&p, decl, 5) != 5 ||
       !m9_vs_source_layout_init(&p, s->elements, s->element_count, s->buffers,
                                  s->buffer_count, &source, NULL) ||
       !m9_vs_source_records(&source, s->maps, s->map_bytes, s->buffer_count,
                              s->first, 3, vertices))
      return false;
   uint32_t fragment[8], carry[14], gather[10], position_words[50];
   mov(fragment, 0x7f, 0x2d, 4, 0x1e, true, true);
   mov(fragment + 2, 0x7f, 0x2d, 4, 0x1d, true, true);
   /* Source constant selectors: xy=0, zw=1. */
   fragment[1] |= 1u << 14;
   fragment[1] |= 0x40000000u;
   for (unsigned i = 4; i < 8; i += 2) {
      fragment[i] = 0xffff801cu; fragment[i + 1] = 0xa01fc00fu;
   }
   mov(carry, 0x4f, 0x1b, 0, 0x1c, false, false);
   mov(carry + 2, 0x4e, 0x19, 0, 0x1c, false, false);
   mov(carry + 4, 0x7f, 0x17, 0, 0x1c, true, false);
   carry[5] |= 1u << 14;
   mov(carry + 6, 0x7f, 0x15, 0, 0x1c, true, false);
   carry[7] |= 0x40000000u;
   mov(carry + 8, 0x47, 0x13, 0, 0x1c, false, false);
   mov(carry + 10, 0x46, 0x11, 0, 0x1c, false, false);
   carry[12] = 0xffff801cu; carry[13] = 0xa01fc00fu;
   const uint8_t inputs[2] = {0, 1};
   struct m9_faa_move_interface gi;
   struct m9_faa_position_interface pi;
   float viewport_constants[26];
   if (!m9_faa_preprocess_moves(M9_FAA_MOVE_RECORDS, inputs, 2, 0,
                                 gather, sizeof(gather), &gi) ||
       !m9_faa_preprocess_position(&s->viewport, position_words, sizeof(position_words),
                                    viewport_constants, sizeof(viewport_constants), &pi))
      return false;
   struct m9_faa_preprocess_upload upload = {
      .vectors = {{fragment, 4}, {carry, 7}, {gather, gi.slots}, {position_words, pi.slots}},
      .constants = {{colors[0], 2}, {colors[1], 2}, {NULL, 0},
                    {(const uint32_t *)viewport_constants, pi.constant_pairs}},
      .viewport = s->viewport, .vertices = (const uint32_t *)vertices, .declaration = decl,
   };
   struct m9_faa_public_upload result;
   if (!m9_faa_preprocess_upload_build(&upload, result.program, sizeof(result.program),
                                        result.constants, sizeof(result.constants),
                                        result.geometry, sizeof(result.geometry), &result.layout))
      return false;
   memcpy(output, &result, sizeof(result));
   return true;
}
