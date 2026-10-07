/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Uniform specialization and bounded arithmetic if-conversion:
 * the passes both native backends share.
 */
#include "m9_specialise.h"

#include <string.h>

#include "nir.h"
#include "nir_builder.h"
#include "compiler/shader_info.h"

/* st lowers the driver's declared default uniforms to load_ubo_vec4 without
 * retaining the speculation property of load_uniform.  The native backends
 * turn these loads into constant-register references, not GPU buffer reads.
 * Restore that property only for a complete, fixed, in-bounds load of the
 * declared default uniform storage.  Indirects and other buffers retain their
 * original access flags and all of peephole_select's remaining guards apply. */
static bool
default_uniform_can_speculate(nir_builder *b, nir_intrinsic_instr *intr,
                              void *data)
{
   if (!b->shader->info.first_ubo_is_default_ubo ||
       intr->intrinsic != nir_intrinsic_load_ubo_vec4 ||
       !nir_src_is_const(intr->src[0]) || nir_src_as_uint(intr->src[0]) ||
       !nir_src_is_const(intr->src[1]) || intr->def.bit_size != 32 ||
       nir_intrinsic_component(intr) + intr->def.num_components > 4)
      return false;
   uint64_t slot = (uint64_t)nir_intrinsic_base(intr) +
                   nir_src_as_uint(intr->src[1]);
   enum gl_access_qualifier access = nir_intrinsic_access(intr);

   if (slot >= b->shader->num_uniforms ||
       (access & (ACCESS_VOLATILE | ACCESS_CAN_SPECULATE)))
      return false;
   nir_intrinsic_set_access(intr, access | ACCESS_CAN_SPECULATE);
   return true;
}

void
m9_nir_lower_alu_if(nir_shader *nir)
{
   bool progress;

   NIR_PASS(_, nir, nir_shader_intrinsics_pass, default_uniform_can_speculate,
            nir_metadata_all, NULL);
   /* Native CMP/SEL already implement varying-dependent ternaries.  Use
    * the same forms for larger arithmetic if/else arms rather than require
    * a uniform condition or a hardware branch.  A finite limit preserves
    * NIR's speculation checks: unlike limit=~0 it rejects texture fetches,
    * discards, stores and guarded indirect loads. Reciprocal and other
    * special-function arithmetic have native implementations too; Mesa's
    * expensive-ALU preference must not force those branches onto the CPU.
    * The finite bound and native slot/register limits still apply. */
   do {
      progress = false;
      NIR_PASS(progress, nir, nir_opt_peephole_select,
               &(nir_opt_peephole_select_options){ .limit = 64,
                                                  .expensive_alu_ok = true });
      if (!progress)
         break;
      NIR_PASS(progress, nir, nir_opt_remove_phis);
      NIR_PASS(progress, nir, nir_opt_copy_prop);
      NIR_PASS(progress, nir, nir_opt_cse);
      NIR_PASS(progress, nir, nir_opt_dce);
      /* If-conversion can make an inactive arm's uninitialised value
       * visible as an undef input of a select.  Pick the legal value zero
       * so both backends can represent that input in a constant register. */
      NIR_PASS(progress, nir, nir_lower_undef_to_zero, NULL);
   } while (progress);
}

/* Uniforms are lowered to load_ubo_vec4, which nir_find_inlinable_uniforms
 * does not track: collect the constant buffer dwords an if condition reads. */
static bool
cond_dwords(nir_scalar s, uint16_t *dw, unsigned *count, unsigned depth)
{
   if (nir_scalar_is_const(s))
      return true;
   if (depth > 16)
      return false;
   if (nir_scalar_is_alu(s)) {
      nir_alu_instr *alu = nir_def_as_alu(s.def);

      if (nir_op_infos[alu->op].output_size && !nir_op_is_vec(alu->op))
         return false;
      for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++)
         if (!cond_dwords(nir_scalar_chase_alu_src(s, i), dw, count, depth + 1))
            return false;
      return true;
   }
   if (nir_scalar_is_intrinsic(s)) {
      nir_intrinsic_instr *intr = nir_def_as_intrinsic(s.def);
      unsigned word;

      if (intr->intrinsic != nir_intrinsic_load_ubo_vec4 ||
          !nir_src_is_const(intr->src[0]) || nir_src_as_uint(intr->src[0]) ||
          !nir_src_is_const(intr->src[1]))
         return false;
      word = (nir_intrinsic_base(intr) + nir_src_as_uint(intr->src[1])) * 4 +
             nir_intrinsic_component(intr) + s.comp;
      for (unsigned i = 0; i < *count; i++)
         if (dw[i] == word)
            return true;
      if (*count == MAX_INLINABLE_UNIFORMS)
         return false;
      dw[(*count)++] = word;
      return true;
   }
   return false;
}

/* A bounded loop may already have become a ladder of varying-dependent
 * exits before st hands NIR to the driver.  Its uniform count then appears
 * in a boolean subcondition, for example (x < stops[i]) && (n_stop > i).
 * Specialise the wholly uniform boolean operand, without specialising the
 * uniform threshold of a varying comparison or any arbitrary data leaf.
 * Keep the original whole-condition collection transactional. */
static void
condition_uniforms(nir_scalar s, uint16_t *dw, unsigned *count, unsigned depth)
{
   uint16_t trial[MAX_INLINABLE_UNIFORMS];
   unsigned n = *count;

   if (depth > 16)
      return;
   memcpy(trial, dw, sizeof(trial));
   if (cond_dwords(s, trial, &n, 0)) {
      memcpy(dw, trial, sizeof(trial));
      *count = n;
      return;
   }
   if (!nir_scalar_is_alu(s) || s.def->bit_size != 1)
      return;
   nir_alu_instr *alu = nir_def_as_alu(s.def);

   if (alu->op != nir_op_iand && alu->op != nir_op_ior &&
       alu->op != nir_op_ixor && alu->op != nir_op_inot)
      return;
   for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++)
      condition_uniforms(nir_scalar_chase_alu_src(s, i), dw, count, depth + 1);
}

/* Records one constant buffer dword, keeping the set free of repeats. */
static bool
dword_add(uint16_t *dw, unsigned *count, unsigned word)
{
   for (unsigned i = 0; i < *count; i++)
      if (dw[i] == word)
         return true;
   if (*count == MAX_INLINABLE_UNIFORMS)
      return false;
   dw[(*count)++] = word;
   return true;
}

/* The uniform dwords an expression reads, ignoring everything else.  A loop
 * bound is compared against an induction variable, so unlike an if condition
 * the expression never folds whole and only its uniform leaves matter. */
static void
bound_dwords(nir_scalar s, uint16_t *dw, unsigned *count, unsigned depth)
{
   if (depth > 16 || nir_scalar_is_const(s))
      return;
   if (nir_scalar_is_alu(s)) {
      nir_alu_instr *alu = nir_def_as_alu(s.def);

      if (nir_op_infos[alu->op].output_size && !nir_op_is_vec(alu->op))
         return;
      for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++)
         bound_dwords(nir_scalar_chase_alu_src(s, i), dw, count, depth + 1);
      return;
   }
   if (nir_scalar_is_intrinsic(s)) {
      nir_intrinsic_instr *intr = nir_def_as_intrinsic(s.def);

      if (intr->intrinsic != nir_intrinsic_load_ubo_vec4 ||
          !nir_src_is_const(intr->src[0]) || nir_src_as_uint(intr->src[0]) ||
          !nir_src_is_const(intr->src[1]))
         return;
      dword_add(dw, count,
                (nir_intrinsic_base(intr) + nir_src_as_uint(intr->src[1])) * 4 +
                nir_intrinsic_component(intr) + s.comp);
   }
}

static bool
branch_breaks(struct exec_list *list)
{
   nir_block *last = list->head_sentinel.next == &list->tail_sentinel ? NULL :
                     nir_cf_node_as_block(exec_node_data(nir_cf_node,
                                                         list->tail_sentinel.prev,
                                                         node));

   return last && nir_block_ends_in_break(last);
}

/* Uniforms a loop exits on.  The trip count is fixed once the draw is
 * submitted, so specialising the variant on the value lets nir_opt_loop_unroll
 * replace the loop with its body; a loop the backend cannot reach this way is
 * still refused as control flow. */
static void
loop_bound_uniforms(struct exec_list *body, bool in_loop, uint16_t *dw,
                    unsigned *count)
{
   foreach_list_typed(nir_cf_node, node, node, body) {
      switch (node->type) {
      case nir_cf_node_if: {
         nir_if *nif = nir_cf_node_as_if(node);

         if (in_loop && (branch_breaks(&nif->then_list) ||
                         branch_breaks(&nif->else_list)))
            bound_dwords(nir_get_scalar(nif->condition.ssa, 0), dw, count, 0);
         loop_bound_uniforms(&nif->then_list, in_loop, dw, count);
         loop_bound_uniforms(&nif->else_list, in_loop, dw, count);
         break;
      }
      case nir_cf_node_loop:
         loop_bound_uniforms(&nir_cf_node_as_loop(node)->body, true, dw, count);
         break;
      default:
         break;
      }
   }
}

/* NIR types a UBO load's element index as an integer, so nir_lower_int_to_float
 * converts the constant ones with everything else; they address the buffer and
 * have to stay integers.  The buffer index in src[0] is untyped and is left
 * alone, the way gallivm's own fixup leaves it. */
static bool
ubo_offset_int(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_ubo_vec4)
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   nir_src_rewrite(&intr->src[1], nir_f2i32(b, intr->src[1].ssa));
   return true;
}

/* The screen advertises no integer support, so Mesa stores an integer uniform
 * in the constant buffer as a float (uniform_query.cpp, uniform_int_float).
 * A driver that declares no_integers is the one that lowers its own NIR to
 * match; gallivm does the same for the copy the draw module runs, so this
 * one only ever sees the clone a native backend compiles. */
void
m9_nir_lower_integers(nir_shader *nir)
{
   if (!nir->options->no_integers)
      return;
   NIR_PASS(_, nir, nir_lower_int_to_float);
   NIR_PASS(_, nir, nir_shader_intrinsics_pass, ubo_offset_int,
            nir_metadata_control_flow, NULL);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_opt_dce);
}

void
m9_nir_find_inlinable_uniforms(nir_shader *nir)
{
   uint16_t dw[MAX_INLINABLE_UNIFORMS] = { 0 };
   unsigned count = 0;

   NIR_PASS(_, nir, nir_opt_constant_folding);
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         /* Mesa turns short uniform-decided branches into selects before
          * finalising NIR.  They need the same constant-buffer values as
          * an if: inlining them removes the inactive expression and its
          * inputs without adding hardware control flow. */
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_alu)
               continue;
            nir_alu_instr *alu = nir_instr_as_alu(instr);

            if (alu->op != nir_op_bcsel)
               continue;
            for (unsigned c = 0; c < alu->def.num_components; c++) {
               nir_scalar condition = nir_scalar_chase_alu_src(
                  nir_get_scalar(&alu->def, c), 0);

               condition_uniforms(condition, dw, &count, 0);
            }
         }
         nir_if *nif = nir_block_get_following_if(block);

         if (!nif)
            continue;
         condition_uniforms(nir_get_scalar(nif->condition.ssa, 0), dw, &count, 0);
      }
   }
   nir_foreach_function_impl(impl, nir)
      loop_bound_uniforms(&impl->body, false, dw, &count);
   memcpy(nir->info.inlinable_uniform_dw_offsets, dw, sizeof(dw));
   nir->info.num_inlinable_uniforms = count;
}

static bool
inline_ubo_vec4(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   const uint32_t *values = data;
   const struct shader_info *info = &b->shader->info;
   nir_scalar comps[NIR_MAX_VEC_COMPONENTS];
   bool hit = false;
   nir_def *vec;
   unsigned base;

   if (intr->intrinsic != nir_intrinsic_load_ubo_vec4 ||
       !nir_src_is_const(intr->src[0]) || nir_src_as_uint(intr->src[0]) ||
       !nir_src_is_const(intr->src[1]) || intr->def.bit_size != 32)
      return false;
   base = (nir_intrinsic_base(intr) + nir_src_as_uint(intr->src[1])) * 4 +
          nir_intrinsic_component(intr);
   b->cursor = nir_after_instr(&intr->instr);
   for (unsigned c = 0; c < intr->def.num_components; c++) {
      comps[c] = nir_get_scalar(&intr->def, c);
      for (unsigned i = 0; i < info->num_inlinable_uniforms; i++) {
         if (info->inlinable_uniform_dw_offsets[i] != base + c)
            continue;
         comps[c] = nir_get_scalar(nir_build_imm(b, 1, 32,
                                   &(nir_const_value){ .u32 = values[i] }), 0);
         hit = true;
      }
   }
   if (!hit)
      return false;
   vec = nir_vec_scalars(b, comps, intr->def.num_components);
   nir_def_rewrite_uses_after(&intr->def, vec);
   return true;
}

void
m9_nir_inline_uniforms(nir_shader *nir, const uint32_t *values)
{
   if (!nir->info.num_inlinable_uniforms ||
       !nir_shader_intrinsics_pass(nir, inline_ubo_vec4,
                                   nir_metadata_control_flow, (void *)values))
      return;
   m9_nir_fold_specialization(nir);
}

void
m9_nir_fold_specialization(nir_shader *nir)
{
   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, nir, nir_opt_constant_folding);
      /* With the bound inlined the trip count is a constant, so the loop
       * unrolls; one longer than max_unroll_iterations stays a loop and is
       * refused as control flow rather than mis-compiled. */
      NIR_PASS(progress, nir, nir_opt_loop);
      NIR_PASS(progress, nir, nir_opt_loop_unroll);
      NIR_PASS(progress, nir, nir_opt_dead_cf);
      NIR_PASS(progress, nir, nir_opt_remove_phis);
      NIR_PASS(progress, nir, nir_opt_peephole_select,
               &(nir_opt_peephole_select_options){ .limit = 8 });
      NIR_PASS(progress, nir, nir_opt_copy_prop);
      /* A variable index st lowered to a ladder of compares folds to one
       * constant index, so the temporary array becomes plain values. */
      NIR_PASS(progress, nir, nir_lower_vars_to_ssa);
      NIR_PASS(progress, nir, nir_opt_algebraic);
      NIR_PASS(progress, nir, nir_opt_cse);
      NIR_PASS(progress, nir, nir_opt_dce);
   } while (progress);
}
