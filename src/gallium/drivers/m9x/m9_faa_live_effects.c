/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include "m9_faa_live.h"
#include "m9_faa_fragment_state.h"
#include "nir.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool overlap(const void *a,size_t ab,const void *b,size_t bb)
{
   uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
   return x<=y ? y-x<ab : x-y<bb;
}

/* Gallium GL1 factors are admitted as exact RGBA operations. Separate alpha
 * factors, logic operations and partial color writes need their own compiler
 * allocation interface and cannot borrow the source COLOR0 forms. */
bool m9_faa_fragment_gallium_state(const struct pipe_blend_state *blend,
                                  const struct pipe_depth_stencil_alpha_state *dsa,
                                  struct m9_faa_fragment_state *out)
{
   struct m9_faa_fragment_state result={0};
   if(!dsa||!out)return false;
   if(blend) {
      const struct pipe_rt_blend_state *rt=&blend->rt[0];
      if(blend->logicop_enable||blend->alpha_to_coverage||blend->alpha_to_one||
         rt->colormask!=PIPE_MASK_RGBA)return false;
      if(rt->blend_enable) {
         if(rt->rgb_func!=PIPE_BLEND_ADD||rt->alpha_func!=PIPE_BLEND_ADD||
            rt->rgb_src_factor!=rt->alpha_src_factor||rt->rgb_dst_factor!=rt->alpha_dst_factor)return false;
         unsigned src=rt->rgb_src_factor,dst=rt->rgb_dst_factor;
         if(src==PIPE_BLENDFACTOR_ONE&&dst==PIPE_BLENDFACTOR_ZERO)result.blend=M9_FAA_BLEND_REPLACE;
         else if(src==PIPE_BLENDFACTOR_SRC_ALPHA&&dst==PIPE_BLENDFACTOR_INV_SRC_ALPHA)result.blend=M9_FAA_BLEND_ALPHA;
         else if(src==PIPE_BLENDFACTOR_ONE&&dst==PIPE_BLENDFACTOR_ONE)result.blend=M9_FAA_BLEND_ADD;
         else if(src==PIPE_BLENDFACTOR_SRC_ALPHA&&dst==PIPE_BLENDFACTOR_ONE)result.blend=M9_FAA_BLEND_ALPHA_ADD;
         else if(src==PIPE_BLENDFACTOR_DST_COLOR&&dst==PIPE_BLENDFACTOR_ZERO)result.blend=M9_FAA_BLEND_FILTER;
         else return false;
      }
   }
   if(dsa->alpha_enabled) {
      if(dsa->alpha_func==PIPE_FUNC_GREATER&&dsa->alpha_ref_value==0)result.alpha=M9_FAA_ALPHA_GT_ZERO;
      else if(dsa->alpha_func==PIPE_FUNC_LESS&&dsa->alpha_ref_value==.5f)result.alpha=M9_FAA_ALPHA_LT_HALF;
      else if(dsa->alpha_func==PIPE_FUNC_GEQUAL&&dsa->alpha_ref_value==.5f)result.alpha=M9_FAA_ALPHA_GE_HALF;
      else return false;
   }
   *out=result;return true;
}

/* Match the state tracker's bounded discard predicate. Source instructions
 * implement the surviving comparison, while NIR discard_if states its inverse.
 * References are resolved only from actual owned FS CB0 or literal words. */
bool m9_faa_live_gl_alpha_decode(const nir_shader *fs,const void *cb,size_t bytes,
                                  enum m9_faa_alpha_mode *mode,bool *present)
{
   if(!fs||!mode||!present||fs->info.stage!=MESA_SHADER_FRAGMENT||
      !fs->info.io_lowered||(bytes&&!cb))return false;
   nir_function_impl *impl=nir_shader_get_entrypoint((nir_shader*)fs);
   if(!impl||!exec_list_is_singular(&impl->body))return false;
   nir_cf_node *node=exec_node_data(nir_cf_node,exec_list_get_head(&impl->body),node);
   if(node->type!=nir_cf_node_block)return false;
   nir_intrinsic_instr *kill=NULL;
   nir_scalar written_alpha={0};
   nir_foreach_instr(instr,nir_cf_node_as_block(node)) {
      if(instr->type!=nir_instr_type_intrinsic)continue;
      nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
      if(in->intrinsic==nir_intrinsic_terminate||in->intrinsic==nir_intrinsic_demote||
         in->intrinsic==nir_intrinsic_demote_if)return false;
      if(in->intrinsic==nir_intrinsic_terminate_if){if(kill)return false;kill=in;}
      if(in->intrinsic==nir_intrinsic_store_output) {
         nir_io_semantics sem=nir_intrinsic_io_semantics(in);
         unsigned component=nir_intrinsic_component(in);
         if((sem.location==FRAG_RESULT_COLOR||sem.location==FRAG_RESULT_DATA0)&&
            component<=3&&(nir_intrinsic_write_mask(in)&(1u<<(3-component)))) {
            if(written_alpha.def)return false;
            written_alpha=nir_scalar_chase_movs(nir_get_scalar(in->src[0].ssa,3-component));
         }
      }
   }
   if(!kill){*mode=M9_FAA_ALPHA_OFF;*present=false;return true;}
   if(!written_alpha.def)return false;
   nir_scalar cond=nir_scalar_chase_movs(nir_get_scalar(kill->src[0].ssa,0));
   bool inverse=nir_scalar_is_alu(cond)&&nir_scalar_alu_op(cond)==nir_op_inot;
   nir_scalar cmp=inverse?nir_scalar_chase_movs(nir_scalar_chase_alu_src(cond,0)):cond;
   if(!nir_scalar_is_alu(cmp))return false;
   nir_op op=nir_scalar_alu_op(cmp);
   if(op!=nir_op_flt&&op!=nir_op_fge)return false;
   for(unsigned swap=0;swap<2;swap++) {
      nir_scalar a=nir_scalar_chase_movs(nir_scalar_chase_alu_src(cmp,swap));
      nir_scalar ref=nir_scalar_chase_movs(nir_scalar_chase_alu_src(cmp,!swap));
      if(!nir_scalar_equal(a,written_alpha)||!nir_scalar_is_intrinsic(a))continue;
      nir_intrinsic_instr *load=nir_def_as_intrinsic(a.def);
      if((load->intrinsic!=nir_intrinsic_load_input&&load->intrinsic!=nir_intrinsic_load_interpolated_input)||
         nir_intrinsic_io_semantics(load).location!=VARYING_SLOT_COL0||
         nir_intrinsic_component(load)+a.comp!=3||a.def->bit_size!=32||
         nir_intrinsic_dest_type(load)!=nir_type_float32||
         !nir_src_is_const(load->src[load->intrinsic==nir_intrinsic_load_input?0:1])||
         nir_src_as_uint(load->src[load->intrinsic==nir_intrinsic_load_input?0:1]))continue;
      float value;
      if(nir_scalar_is_const(ref))value=nir_scalar_as_float(ref);
      else {
         if(!nir_scalar_is_intrinsic(ref)||ref.def->bit_size!=32)continue;
         nir_intrinsic_instr *u=nir_def_as_intrinsic(ref.def);
         if((u->intrinsic!=nir_intrinsic_load_ubo&&u->intrinsic!=nir_intrinsic_load_ubo_vec4)||
            !nir_src_is_const(u->src[0])||nir_src_as_uint(u->src[0])||!nir_src_is_const(u->src[1]))continue;
         uint64_t offset=nir_src_as_uint(u->src[1]);
         if(u->intrinsic==nir_intrinsic_load_ubo_vec4)
            offset=(offset+nir_intrinsic_base(u))*16+nir_intrinsic_component(u)*4;
         offset+=ref.comp*4;
         if(!cb||(offset&3)||offset>bytes||bytes-offset<4)return false;
         memcpy(&value,(const uint8_t*)cb+offset,4);
      }
      if(!isfinite(value))return false;
      unsigned func=(op==nir_op_flt?(swap?2:1):(swap?3:0))^(inverse?0:1);
      enum m9_faa_alpha_mode result;
      if(func==2&&value==0.0f)result=M9_FAA_ALPHA_GT_ZERO;
      else if(func==1&&value==.5f)result=M9_FAA_ALPHA_LT_HALF;
      else if(func==0&&value==.5f)result=M9_FAA_ALPHA_GE_HALF;
      else return false;
      *mode=result;*present=true;return true;
   }
   return false;
}

/* Relink a fresh live fused upload, preserving its GPU VS instructions and
 * dense keys. No NIR mutation or CPU vertex transform occurs. The source
 * FS utility prefix and program-vector boundary are owned independently. */
bool
m9_faa_live_gl_color_fused_effects_build_cb(const struct m9_faa_live_gl *live,
                                         const struct nir_shader *fs,
                                         const struct m9_faa_fragment_state *state,
                                         const void *fs_cb0,size_t fs_cb0_bytes,
                                         struct m9_faa_gl_upload *output,
                                         const char **reason)
{
   struct m9_faa_gl_upload first={0},result={0};
   struct m9_faa_fragment_interface interface;
   uint32_t fragment[28],utility[12];
   const char *why="unsupported FAA fragment-state interface";
   nir_shader *clean=NULL;
   if (!live || !fs || !state || !output || output->storage ||
       overlap(output,sizeof(*output),live,sizeof(*live)) ||
       overlap(output,sizeof(*output),state,sizeof(*state)) ||
       overlap(output,sizeof(*output),fs,sizeof(*fs)) ||
       overlap(output,sizeof(*output),live->cb0,live->cb0_bytes) ||
       overlap(output,sizeof(*output),fs_cb0,fs_cb0_bytes) ||
       !m9_faa_fragment_state_build(state,fragment,sizeof(fragment),utility,
                                     sizeof(utility),&interface)) goto fail;
   enum m9_faa_alpha_mode alpha;bool present;
   if(!m9_faa_live_gl_alpha_decode(fs,fs_cb0,fs_cb0_bytes,&alpha,&present)||
      (present&&alpha!=state->alpha)){why="FAA discard predicate/reference has no paired alpha-test profile";goto fail;}
   if(present){
      clean=nir_shader_clone(NULL,fs);
      if(!clean){why="FAA fragment clone allocation failed";goto fail;}
      nir_foreach_function_impl(impl,clean) {
         nir_foreach_block(block,impl) {
            nir_foreach_instr_safe(instr,block) {
               if(instr->type==nir_instr_type_intrinsic&&
                  nir_instr_as_intrinsic(instr)->intrinsic==nir_intrinsic_terminate_if)nir_instr_remove(instr);
            }
         }
         nir_progress(true,impl,nir_metadata_none);
      }
      nir_opt_dce(clean);
   }
   if (!m9_faa_live_gl_color_fused_build_state(live,clean?clean:fs,false,&first,&why)) goto fail;
   if (first.color_flat || first.color_legacy_interpolation ||
       first.layout.count!=2 || first.layout.entry_bytes[0] ||
       first.layout.entry_bytes[1]!=64 || first.constant_begin[0] ||
       first.constant_end[0]!=16 || first.constant_begin[1]!=16 ||
       first.constant_end[1]!=first.constants_bytes) {
      why="FAA effects require the bounded smooth COLOR0 fused interface";
      goto fail;
   }
   for (unsigned i=0;i<live->buffer_count;i++)
      if (overlap(output,sizeof(*output),live->maps[i],live->map_bytes[i])) {
         why="FAA effects output aliases a retained vertex input";goto fail;
      }
   result=first;
   result.storage=NULL;
   uint32_t fsbytes=interface.slots*8,cbbytes=interface.constant_pairs*8;
   uint32_t vsbytes=first.layout.bytes-first.layout.entry_bytes[1];
   uint32_t vscb=first.constant_end[1]-first.constant_begin[1];
   size_t total=(size_t)fsbytes+vsbytes+cbbytes+vscb+first.geometry_bytes+
                first.declaration_words*4;
   if (total<first.storage_bytes ||
       total>64u*1024u*1024u) goto fail;
   result.storage=malloc(total);
   if (!result.storage) {why="out of memory";goto fail;}
   result.storage_bytes=total;
   result.program=result.storage;
   result.constants=(uint32_t*)((uint8_t*)result.storage+fsbytes+vsbytes);
   result.geometry=(uint32_t*)((uint8_t*)result.constants+cbbytes+vscb);
   result.declaration=(uint32_t*)((uint8_t*)result.geometry+result.geometry_bytes);
   struct m9_faa_program_vector vectors[2]={
      {fragment,interface.slots},
      {(const uint32_t*)((const uint8_t*)first.program+64),vsbytes/8}
   };
   if (!m9_faa_program_link(vectors,2,result.program,fsbytes+vsbytes,&result.layout)) {
      why="FAA effect vectors failed live linking";goto fail;
   }
   result.constant_begin[0]=0;result.constant_end[0]=cbbytes;
   result.constant_begin[1]=cbbytes;result.constant_end[1]=cbbytes+vscb;
   result.constants_bytes=cbbytes+vscb;
   memcpy(result.constants,utility,cbbytes);
   memcpy((uint8_t*)result.constants+cbbytes,(uint8_t*)first.constants+16,vscb);
   memcpy(result.geometry,first.geometry,first.geometry_bytes);
   memcpy(result.declaration,first.declaration,first.declaration_words*4);
   /* Native raster metadata and the corresponding actual GL state must be
    * authenticated before enabling this new application interface. */
   result.native_admission=false;
   m9_faa_gl_upload_fini(&first);ralloc_free(clean);
   *output=result;
   if (reason)*reason=NULL;
   return true;
fail:
   m9_faa_gl_upload_fini(&first);m9_faa_gl_upload_fini(&result);ralloc_free(clean);
   if (reason)*reason=why;
   return false;
}

bool m9_faa_live_gl_color_fused_effects_build(const struct m9_faa_live_gl *live,
                                             const struct nir_shader *fs,
                                             const struct m9_faa_fragment_state *state,
                                             struct m9_faa_gl_upload *output,
                                             const char **reason)
{
   return m9_faa_live_gl_color_fused_effects_build_cb(live,fs,state,NULL,0,output,reason);
}
