/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include "m9_faa_application.h"
#include "m9_internal.h"
#include "nir.h"
#include "util/u_debug.h"
#include "util/u_inlines.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The fragment word carries the generator's factor and function values. */
static_assert(M9_FAA_FACTOR_ZERO==PARHELIA_M9_FAA_FACTOR_ZERO&&
              M9_FAA_FACTOR_ONE==PARHELIA_M9_FAA_FACTOR_ONE&&
              M9_FAA_FACTOR_SRC_COLOR==PARHELIA_M9_FAA_FACTOR_SRC_COLOR&&
              M9_FAA_FACTOR_SRC_ALPHA==PARHELIA_M9_FAA_FACTOR_SRC_ALPHA&&
              M9_FAA_FACTOR_INV_SRC_ALPHA==PARHELIA_M9_FAA_FACTOR_INV_SRC_ALPHA&&
              M9_FAA_FACTOR_DST_COLOR==PARHELIA_M9_FAA_FACTOR_DST_COLOR&&
              M9_FAA_FACTOR_INV_DST_COLOR==PARHELIA_M9_FAA_FACTOR_INV_DST_COLOR&&
              M9_FAA_FACTOR_INV_SRC_COLOR==PARHELIA_M9_FAA_FACTOR_INV_SRC_COLOR,"uAPI factor");
static_assert(M9_FAA_ALPHA_FUNC_GEQUAL==PARHELIA_M9_FAA_ALPHA_GEQUAL&&
              M9_FAA_ALPHA_FUNC_LESS==PARHELIA_M9_FAA_ALPHA_LESS&&
              M9_FAA_ALPHA_FUNC_GREATER==PARHELIA_M9_FAA_ALPHA_GREATER,"uAPI alpha function");

static bool
m9_faa_dual_input(nir_scalar value,unsigned location,unsigned component)
{
   value=nir_scalar_chase_movs(value);
   if(!nir_scalar_is_intrinsic(value)||value.def->bit_size!=32)return false;
   nir_intrinsic_instr *in=nir_def_as_intrinsic(value.def);
   if(in->intrinsic!=nir_intrinsic_load_interpolated_input||
      nir_intrinsic_dest_type(in)!=nir_type_float32||
      nir_intrinsic_io_semantics(in).location!=location||
      nir_intrinsic_io_semantics(in).num_slots!=1||
      nir_intrinsic_component(in)+value.comp!=component||
      !nir_src_is_const(in->src[1])||nir_src_as_uint(in->src[1])||
      !in->src[0].ssa)return false;
   nir_instr *bary=nir_def_instr(in->src[0].ssa);
   return bary->type==nir_instr_type_intrinsic&&
          nir_instr_as_intrinsic(bary)->intrinsic==nir_intrinsic_load_barycentric_pixel&&
          /* NONE follows the shade model; flat shading is refused apart. */
          (nir_intrinsic_interp_mode(nir_instr_as_intrinsic(bary))==INTERP_MODE_SMOOTH||
           nir_intrinsic_interp_mode(nir_instr_as_intrinsic(bary))==INTERP_MODE_NONE);
}

static unsigned
m9_faa_dual_product(nir_scalar value,unsigned component,unsigned depth,
                     nir_tex_instr *texture[2],unsigned sampler[2])
{
   value=nir_scalar_chase_movs(value);
   if(!value.def||value.def->bit_size!=32||depth>3)return 0;
   if(nir_scalar_is_alu(value)&&nir_scalar_alu_op(value)==nir_op_fmul) {
      unsigned a=m9_faa_dual_product(nir_scalar_chase_alu_src(value,0),component,depth+1,texture,sampler);
      unsigned b=m9_faa_dual_product(nir_scalar_chase_alu_src(value,1),component,depth+1,texture,sampler);
      return a&&b&&!(a&b)?a|b:0;
   }
   if(m9_faa_dual_input(value,VARYING_SLOT_COL0,component))return 1;
   nir_instr *instr=nir_def_instr(value.def);
   if(instr->type!=nir_instr_type_tex||value.comp!=component||
      value.def->num_components!=4)return 0;
   nir_tex_instr *tex=nir_instr_as_tex(instr);
   int coordinate=nir_tex_instr_src_index(tex,nir_tex_src_coord);
   int projector=nir_tex_instr_src_index(tex,nir_tex_src_projector);
   if(tex->op!=nir_texop_tex||tex->sampler_dim!=GLSL_SAMPLER_DIM_2D||
      tex->is_array||tex->is_shadow||tex->dest_type!=nir_type_float32||
      tex->coord_components!=2||tex->num_srcs!=(projector<0?1u:2u)||coordinate<0||
      tex->texture_index!=tex->sampler_index||tex->texture_index>=PIPE_MAX_SAMPLERS)
      return 0;
   for(unsigned unit=0;unit<2;unit++) {
      nir_def *coord=tex->src[coordinate].src.ssa;
      if(!m9_faa_dual_input(nir_get_scalar(coord,0),VARYING_SLOT_TEX0+unit,0)||
         !m9_faa_dual_input(nir_get_scalar(coord,1),VARYING_SLOT_TEX0+unit,1))continue;
      /* A projector must be the coordinate's own w; the build proves w = 1. */
      if(projector>=0&&
         !m9_faa_dual_input(nir_get_scalar(tex->src[projector].src.ssa,0),VARYING_SLOT_TEX0+unit,3))
         return 0;
      if(texture[unit]&&texture[unit]!=tex)return 0;
      texture[unit]=tex;sampler[unit]=tex->sampler_index;
      return 2u<<unit;
   }
   return 0;
}

/* The units whose lookups divide by the coordinate's w, and whether the
 * alpha is COLOR0.a alone. */
static void
m9_faa_application_texture_form(const nir_shader *fs,unsigned *projected,bool *color_alpha)
{
   *projected=0;*color_alpha=false;
   nir_function_impl *impl=nir_shader_get_entrypoint((nir_shader*)fs);
   nir_intrinsic_instr *store=NULL;
   nir_foreach_block(block,impl)nir_foreach_instr(instr,block) {
      if(instr->type==nir_instr_type_tex) {
         nir_tex_instr *tex=nir_instr_as_tex(instr);
         int c=nir_tex_instr_src_index(tex,nir_tex_src_coord);
         if(nir_tex_instr_src_index(tex,nir_tex_src_projector)<0||c<0)continue;
         for(unsigned unit=0;unit<2;unit++)
            if(m9_faa_dual_input(nir_get_scalar(tex->src[c].src.ssa,0),VARYING_SLOT_TEX0+unit,0))
               *projected|=1u<<unit;
      }
      if(instr->type==nir_instr_type_intrinsic&&
         nir_instr_as_intrinsic(instr)->intrinsic==nir_intrinsic_store_output)
         store=nir_instr_as_intrinsic(instr);
   }
   *color_alpha=store&&m9_faa_dual_input(nir_get_scalar(store->src[0].ssa,3),VARYING_SLOT_COL0,3);
}

/* Whether the VS writes w = 1 to a texture coordinate: a constant one, or
 * an input whose element format has no fourth channel. */
static bool
m9_faa_application_vs_w_one(const nir_shader *vs,unsigned location,
                            const struct pipe_vertex_element *elements,unsigned element_count)
{
   nir_function_impl *impl=nir_shader_get_entrypoint((nir_shader*)vs);
   nir_intrinsic_instr *store=NULL;
   nir_foreach_block(block,impl)nir_foreach_instr(instr,block) {
      if(instr->type!=nir_instr_type_intrinsic)continue;
      nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
      if(in->intrinsic==nir_intrinsic_store_output&&nir_intrinsic_io_semantics(in).location==location) {
         if(store)return false;
         store=in;
      }
   }
   if(!store||!(nir_intrinsic_write_mask(store)&(1u<<(3-nir_intrinsic_component(store))))||
      nir_intrinsic_component(store)>3)return false;
   nir_scalar w=nir_scalar_chase_movs(nir_get_scalar(store->src[0].ssa,3-nir_intrinsic_component(store)));
   if(nir_scalar_is_const(w))return nir_scalar_as_float(w)==1.0f;
   if(!nir_scalar_is_intrinsic(w))return false;
   nir_intrinsic_instr *input=nir_def_as_intrinsic(w.def);
   if(input->intrinsic!=nir_intrinsic_load_input||!nir_src_is_const(input->src[0])||
      nir_src_as_uint(input->src[0])||nir_intrinsic_component(input)+w.comp!=3)return false;
   unsigned at=nir_intrinsic_io_semantics(input).location;
   unsigned rank=util_bitcount64(vs->info.inputs_read&BITFIELD64_MASK(at));
   if(rank>=element_count)return false;
   return util_format_get_nr_components(elements[rank].src_format)<4;
}

/* TEX0 or TEX0 * TEX1 in every component, without COLOR0. */
static bool
m9_faa_application_texture_unlit(const nir_shader *fs)
{
   nir_tex_instr *texture[2]={0};unsigned slots[2]={0},textures=0;
   nir_intrinsic_instr *store=NULL;
   if(!fs||fs->info.stage!=MESA_SHADER_FRAGMENT||!fs->info.io_lowered)return false;
   nir_function_impl *impl=nir_shader_get_entrypoint((nir_shader*)fs);
   if(!impl||!exec_list_is_singular(&impl->body))return false;
   nir_foreach_block(block,impl)nir_foreach_instr(instr,block) {
      if(instr->type==nir_instr_type_tex)textures++;
      if(instr->type==nir_instr_type_intrinsic&&
         nir_instr_as_intrinsic(instr)->intrinsic==nir_intrinsic_store_output) {
         if(store)return false;
         store=nir_instr_as_intrinsic(instr);
      }
   }
   if((textures!=1&&textures!=2)||!store||store->src[0].ssa->num_components!=4)return false;
   for(unsigned c=0;c<4;c++)
      if(m9_faa_dual_product(nir_get_scalar(store->src[0].ssa,c),c,0,texture,slots)!=
         (textures==2?6u:2u))
         return false;
   return true;
}

bool
m9_faa_application_texture_linkage(const nir_shader *fs,
                                   struct m9_vs_outputs *linkage,
                                   unsigned sampler[2],unsigned *count,
                                   const char **reason)
{
   const char *why="FAA textures require complete smooth COLOR0 * TEX0 * TEX1 RGBA";
   nir_tex_instr *texture[2]={0};unsigned slots[2]={0},textures=0;
   nir_intrinsic_instr *store=NULL;
   if(!fs||!linkage||!sampler||!count||fs->info.stage!=MESA_SHADER_FRAGMENT||
      !fs->info.io_lowered)goto fail;
   nir_function_impl *impl=nir_shader_get_entrypoint((nir_shader*)fs);
   if(!impl)goto fail;
   nir_foreach_block(block,impl)nir_foreach_instr(instr,block)
      if(instr->type==nir_instr_type_tex)textures++;
   if(!textures){*count=0;if(reason)*reason=NULL;return true;}
   if((textures!=1&&textures!=2)||!exec_list_is_singular(&impl->body))goto fail;
   nir_cf_node *node=exec_node_data(nir_cf_node,exec_list_get_head(&impl->body),node);
   if(node->type!=nir_cf_node_block)goto fail;
   nir_foreach_instr(instr,nir_cf_node_as_block(node)) {
      if(instr->type==nir_instr_type_alu||instr->type==nir_instr_type_load_const||
         instr->type==nir_instr_type_tex)continue;
      if(instr->type!=nir_instr_type_intrinsic)goto fail;
      nir_intrinsic_instr *in=nir_instr_as_intrinsic(instr);
      switch(in->intrinsic) {
      case nir_intrinsic_load_barycentric_pixel:
      case nir_intrinsic_load_interpolated_input:break;
      /* A lowered alpha test; the caller decodes it. */
      case nir_intrinsic_terminate_if:
      case nir_intrinsic_load_ubo:
      case nir_intrinsic_load_ubo_vec4:break;
      case nir_intrinsic_store_output:if(store)goto fail;store=in;break;
      default:goto fail;
      }
   }
   if(!store||nir_intrinsic_component(store)||nir_intrinsic_write_mask(store)!=15||
      nir_intrinsic_src_type(store)!=nir_type_float32||
      store->src[0].ssa->num_components!=4||store->src[0].ssa->bit_size!=32||
      !nir_src_is_const(store->src[1])||nir_src_as_uint(store->src[1]))goto fail;
   nir_io_semantics io=nir_intrinsic_io_semantics(store);
   if((io.location!=FRAG_RESULT_COLOR&&io.location!=FRAG_RESULT_DATA0)||
      io.num_slots!=1||io.dual_source_blend_index||io.fb_fetch_output||
      io.fb_fetch_output_coherent||io.per_view||io.high_16bits||io.high_dvec2||
      io.no_sysval_output)goto fail;
   /* COLOR0 * TEX0 (* TEX1) in every colour component; alpha the same or
    * COLOR0.a alone (GL MODULATE of an RGB texture), which the caller
    * admits only where the alpha is not observed. */
   for(unsigned c=0;c<4;c++) {
      unsigned mask=m9_faa_dual_product(nir_get_scalar(store->src[0].ssa,c),c,0,texture,slots);
      if(mask!=(textures==2?7u:3u)&&!(c==3&&mask==1u))
         goto fail;
   }
   if(textures==1) {
      if(!texture[0]||texture[1])goto fail;
      *linkage=(struct m9_vs_outputs){.count=2};
      linkage->stream[0].location=VARYING_SLOT_COL0;
      linkage->stream[1].location=VARYING_SLOT_TEX0;
      sampler[0]=slots[0];sampler[1]=0;*count=1;
      if(reason)*reason=NULL;
      return true;
   }
   if(!texture[0]||!texture[1]||slots[0]==slots[1])goto fail;
   *linkage=(struct m9_vs_outputs){.count=3};
   linkage->stream[0].location=VARYING_SLOT_COL0;
   linkage->stream[1].location=VARYING_SLOT_TEX0;
   linkage->stream[2].location=VARYING_SLOT_TEX1;
   memcpy(sampler,slots,sizeof(slots));*count=2;
   if(reason)*reason=NULL;
   return true;
fail:if(reason)*reason=why;return false;
}

static bool
m9_faa_application_texture_valid(const struct m9_faa_application_texture *t)
{
   const struct drm_parhelia_m9_faa_texture_view *v=&t->view;
   struct m9_render_layout layout;
   return t->resource&&t->resource->target==PIPE_TEXTURE_2D&&
      t->resource->width0==v->width&&t->resource->height0==v->height&&
      t->resource->last_level+1u>=v->levels&&t->resource->depth0==1&&
      t->resource->array_size==1&&
      v->handle&&v->profile==12&&m9_faa_texture_code(v->format)&&v->levels>=1&&v->levels<=11&&
      v->width&&v->height&&v->width<=1024&&v->height<=1024&&
      !(v->width&(v->width-1))&&!(v->height&(v->height-1))&&
      v->storage_kind==((v->width>64||v->height>64)?0x21u:1u)&&
      (v->min_filter==1||v->min_filter==2)&&(v->mag_filter==1||v->mag_filter==2)&&
      v->mip_filter>=1&&v->mip_filter<=3&&(v->levels>1||v->mip_filter==1)&&
      m9_faa_texture_address(v->address_u)&&m9_faa_texture_address(v->address_v)&&!t->offset&&
      m9_render_surface_layout(v->width,v->height,4,&layout)&&
      t->owner_bytes>=layout.size;
}

void
m9_faa_application_fini(struct m9_faa_application_upload *upload)
{
   if (upload) {
      for (unsigned i=0;i<2;i++)
         pipe_resource_reference(&upload->texture[i].resource,NULL);
      m9_faa_gl_upload_fini(&upload->shader);
      memset(upload, 0, sizeof(*upload));
   }
}

void
m9_faa_application_frame_reset(struct m9_faa_application_frame *frame)
{
   if (!frame) return;
   for (uint32_t i = 0; i < frame->count; i++)
      m9_faa_application_fini(&frame->draws[i]);
   frame->count = 0;
   frame->storage_bytes = 0;
   frame->clear_flags = frame->clear_color = 0;
}

void
m9_faa_application_frame_fini(struct m9_faa_application_frame *frame)
{
   if (!frame) return;
   m9_faa_application_frame_reset(frame);
   free(frame->draws);
   memset(frame, 0, sizeof(*frame));
}

/* Only packed, builder-owned triangle lists can be joined. Every source
 * position/normal remains raw; the GPU runs the unchanged producer once per
 * record. No indexed topology or per-draw system value survives this ABI. */
static bool
m9_faa_application_packed(const struct m9_faa_gl_upload *s)
{
   uintptr_t base = (uintptr_t)s->storage;
   uint64_t total = (uint64_t)s->layout.bytes + s->constants_bytes +
                    s->geometry_bytes + (uint64_t)s->declaration_words * 4;
   return base && s->layout.bytes &&
          (s->layout.count == 2 || s->layout.count == 4) &&
          s->record_bytes && !(s->record_bytes % 16) &&
          s->vertex_count && !(s->vertex_count % 3) &&
          s->vertex_count <= M9_FAA_GL_UPLOAD_MAX_VERTICES &&
          (uint64_t)s->vertex_count * s->record_bytes == s->vertex_bytes &&
          s->vertex_offset == 128 &&
          (uint64_t)s->vertex_offset + s->vertex_bytes == s->geometry_bytes &&
          total == s->storage_bytes && total <= UINTPTR_MAX - base &&
          (uintptr_t)s->program == base &&
          (uintptr_t)s->constants == base + s->layout.bytes &&
          (uintptr_t)s->geometry == base + s->layout.bytes + s->constants_bytes &&
          (uintptr_t)s->declaration == base + s->layout.bytes +
                                             s->constants_bytes + s->geometry_bytes;
}

static bool
m9_faa_application_state_equal(const struct m9_faa_application_state *a,
                                const struct m9_faa_application_state *b)
{
   return a->width == b->width && a->height == b->height &&
          !memcmp(a->viewport.scale, b->viewport.scale, sizeof(a->viewport.scale)) &&
          !memcmp(a->viewport.translate, b->viewport.translate, sizeof(a->viewport.translate)) &&
          a->scissor_enabled == b->scissor_enabled &&
          (!a->scissor_enabled ||
           (a->scissor.minx == b->scissor.minx && a->scissor.miny == b->scissor.miny &&
            a->scissor.maxx == b->scissor.maxx && a->scissor.maxy == b->scissor.maxy)) &&
          a->depth_enabled == b->depth_enabled && a->depth_write == b->depth_write &&
          a->depth_func == b->depth_func && a->cull_face == b->cull_face &&
          a->front_ccw == b->front_ccw && a->flatshade == b->flatshade &&
          a->flatshade_first == b->flatshade_first &&
          a->fragment.blend == b->fragment.blend && a->fragment.alpha == b->fragment.alpha &&
          a->fragment.source_stream == b->fragment.source_stream;
}

static bool
m9_faa_application_merge(struct m9_faa_application_upload *last,
                          const struct m9_faa_application_upload *next)
{
   const struct m9_faa_gl_upload *a = &last->shader, *b = &next->shader;
   if (!m9_faa_application_packed(a) || !m9_faa_application_packed(b) ||
       !m9_faa_application_state_equal(&last->state, &next->state) ||
       a->vertex_count > M9_FAA_GL_UPLOAD_MAX_VERTICES - b->vertex_count ||
       a->record_bytes != b->record_bytes ||
       a->constants_bytes != b->constants_bytes ||
       a->declaration_words != b->declaration_words ||
       a->carry_output_registers != b->carry_output_registers ||
       a->fused_source_mask != b->fused_source_mask ||
       a->color_flat != b->color_flat ||
       a->color_legacy_interpolation != b->color_legacy_interpolation ||
       a->native_admission != b->native_admission ||
       last->texture_count != next->texture_count ||
       memcmp(&a->layout, &b->layout, sizeof(a->layout)) ||
       memcmp(a->constant_begin, b->constant_begin, sizeof(a->constant_begin)) ||
       memcmp(a->constant_end, b->constant_end, sizeof(a->constant_end)) ||
       memcmp(a->varying_location, b->varying_location, sizeof(a->varying_location)) ||
       memcmp(a->program, b->program, a->layout.bytes) ||
       memcmp(a->constants, b->constants, a->constants_bytes) ||
       memcmp(a->geometry, b->geometry, a->vertex_offset) ||
       memcmp(a->declaration, b->declaration, (size_t)a->declaration_words * 4) ||
       b->vertex_bytes > UINT32_MAX - a->geometry_bytes ||
       b->vertex_bytes > SIZE_MAX - a->storage_bytes)
      return false;
   for (unsigned i=0;i<last->texture_count;i++)
      if (last->texture[i].resource != next->texture[i].resource ||
          last->texture[i].generation != next->texture[i].generation ||
          last->texture[i].offset != next->texture[i].offset ||
          last->texture[i].owner_bytes != next->texture[i].owner_bytes ||
          memcmp(&last->texture[i].view,&next->texture[i].view,sizeof(last->texture[i].view)))
         return false;
   struct m9_faa_gl_upload joined = *a;
   joined.storage_bytes += b->vertex_bytes;
   joined.storage = malloc(joined.storage_bytes);
   if (!joined.storage) return false; /* Retain the two independent draws. */
   joined.vertex_count += b->vertex_count;
   joined.vertex_bytes += b->vertex_bytes;
   joined.geometry_bytes += b->vertex_bytes;
   joined.program = joined.storage;
   joined.constants = (uint32_t *)((uint8_t *)joined.storage + joined.layout.bytes);
   joined.geometry = (uint32_t *)((uint8_t *)joined.constants + joined.constants_bytes);
   joined.declaration = (uint32_t *)((uint8_t *)joined.geometry + joined.geometry_bytes);
   size_t prefix = a->layout.bytes + (size_t)a->constants_bytes + a->geometry_bytes;
   memcpy(joined.storage, a->storage, prefix);
   memcpy((uint8_t *)joined.storage + prefix,
          (const uint8_t *)b->geometry + b->vertex_offset, b->vertex_bytes);
   memcpy(joined.declaration, a->declaration, (size_t)a->declaration_words * 4);
   m9_faa_gl_upload_fini(&last->shader);
   last->shader = joined;
   return true;
}

bool
m9_faa_application_frame_append(struct m9_faa_application_frame *frame,
                                struct m9_faa_application_upload *upload,
                                uint32_t clear_flags, uint32_t clear_color,
                                const char **reason)
{
   const char *why = "FAA frame input has no owned upload or INIT identity";
   if (!frame || !upload || !upload->shader.storage ||
       !upload->shader.storage_bytes || !upload->graph_id ||
       !upload->init_point || (upload->init_point & 15) ||
       !upload->state.width || !upload->state.height)
      goto fail;
   if (frame->count) {
      const struct m9_faa_application_upload *first = &frame->draws[0];
      if (upload->graph_id != first->graph_id ||
          upload->init_point != first->init_point ||
          upload->generation != first->generation ||
          upload->state.width != first->state.width ||
          upload->state.height != first->state.height ||
          !upload->texture_count != !first->texture_count ||
          clear_flags != frame->clear_flags || clear_color != frame->clear_color) {
         why = "FAA draw changed graph, dimensions, texture route or pending frame clear";
         goto fail;
      }
   }
   for (uint32_t i = 0; i < frame->count; i++) {
      if (upload == &frame->draws[i] ||
          upload->shader.storage == frame->draws[i].shader.storage) {
         why = "FAA draw aliases an earlier owned frame upload";
         goto fail;
      }
   }
   /* Bound CPU snapshot memory independently of the kernel's actual packet
    * arenas. Hitting this bound must never split a frame or clear its depth. */
   const size_t budget = 64u * 1024u * 1024u;
   if (upload->shader.storage_bytes > budget ||
       frame->storage_bytes > budget - upload->shader.storage_bytes ||
       frame->count == UINT32_MAX) {
      why = "FAA frame exceeds owned input budget";
      goto fail;
   }
   if (frame->count) {
      struct m9_faa_application_upload *last = &frame->draws[frame->count - 1];
      size_t old_bytes = last->shader.storage_bytes;
      if (m9_faa_application_merge(last, upload)) {
         frame->storage_bytes = frame->storage_bytes - old_bytes + last->shader.storage_bytes;
         m9_faa_application_fini(upload);
         if (reason) *reason = NULL;
         return true;
      }
   }
   if (frame->count == frame->capacity) {
      uint32_t capacity = frame->capacity ? frame->capacity : 16;
      if (frame->capacity) {
         if (capacity > UINT32_MAX / 2) goto fail;
         capacity *= 2;
      }
      if ((uint64_t)capacity * sizeof(*frame->draws) > SIZE_MAX) goto fail;
      void *draws = realloc(frame->draws, (size_t)capacity * sizeof(*frame->draws));
      if (!draws) { why = "FAA frame descriptor allocation failed"; goto fail; }
      frame->draws = draws;
      frame->capacity = capacity;
   }
   frame->draws[frame->count++] = *upload;
   frame->storage_bytes += upload->shader.storage_bytes;
   frame->clear_flags = clear_flags;
   frame->clear_color = clear_color;
   memset(upload, 0, sizeof(*upload));
   if (reason) *reason = NULL;
   return true;
fail:
   if (reason) *reason = why;
   return false;
}

static bool
source_index(const struct m9_faa_application_draw *d, unsigned at,
             uint32_t *value)
{
   uint64_t index = (uint64_t)d->draw->start + at;
   int64_t biased;
   if (d->info->index_size) {
      unsigned bytes = d->info->index_size;
      if ((bytes != 1 && bytes != 2 && bytes != 4) || !d->index_map ||
          index > SIZE_MAX / bytes)
         return false;
      size_t offset = (size_t)index * bytes;
      if (offset > d->index_bytes || bytes > d->index_bytes - offset)
         return false;
      uint32_t word = 0;
      memcpy(&word, (const uint8_t *)d->index_map + offset, bytes);
      biased = (int64_t)word + d->draw->index_bias;
   } else {
      biased = index;
   }
   if (biased < 0 || (uint64_t)biased > UINT32_MAX)
      return false;
   *value = biased;
   return true;
}

bool
m9_faa_application_build(const struct m9_faa_application_draw *d,
                         struct m9_faa_application_upload *output,
                         const char **reason)
{
   const char *why = "invalid FAA application draw";
   uint32_t *indices = NULL;
   struct nir_shader *color_vs=NULL,*color_fs=NULL;
   void *color_constants=NULL;size_t color_constant_bytes=0;
   struct m9_faa_application_upload fresh = {0};
   struct m9_vs_outputs texture_linkage={0};unsigned sampler[2],texture_count=0;
   if (!d || !output || output->shader.storage || output->texture_count ||
       output->texture[0].resource || output->texture[1].resource || !d->info || !d->draw ||
       !d->fs || !d->draw->count || d->info->instance_count != 1 ||
       d->info->start_instance || d->info->primitive_restart ||
       d->draw->count > M9_FAA_GL_UPLOAD_MAX_VERTICES ||
       !d->state.width || !d->state.height ||
       d->state.depth_func > PIPE_FUNC_ALWAYS ||
       d->state.cull_face > PIPE_FACE_FRONT_AND_BACK)
      goto fail;
   for (unsigned c = 0; c < 3; c++)
      if (!isfinite(d->state.viewport.scale[c]) ||
          !isfinite(d->state.viewport.translate[c]))
         goto fail;
   if (d->state.scissor_enabled &&
       (d->state.scissor.minx > d->state.scissor.maxx ||
        d->state.scissor.miny > d->state.scissor.maxy ||
        d->state.scissor.maxx > d->state.width ||
        d->state.scissor.maxy > d->state.height))
      goto fail;
   unsigned triangles;
   switch (d->info->mode) {
   case MESA_PRIM_TRIANGLES:
      if (d->draw->count % 3) goto fail;
      triangles = d->draw->count / 3;
      break;
   case MESA_PRIM_QUADS:
      if (d->draw->count % 4) goto fail;
      triangles = d->draw->count / 4 * 2;
      break;
   case MESA_PRIM_QUAD_STRIP:
      if (d->draw->count < 4 || d->draw->count % 2) goto fail;
      triangles = (d->draw->count / 2 - 1) * 2;
      break;
   case MESA_PRIM_TRIANGLE_STRIP:
   case MESA_PRIM_TRIANGLE_FAN:
      if (d->draw->count < 3) goto fail;
      triangles = d->draw->count - 2;
      break;
   default:
      why = "FAA application topology has no qualified triangle expansion";
      goto fail;
   }
   if (triangles > M9_FAA_GL_UPLOAD_MAX_VERTICES / 3) {
      why = "FAA application expanded draw exceeds owned snapshot budget";
      goto fail;
   }
   unsigned count = triangles * 3;
   indices = malloc((size_t)count * sizeof(*indices));
   if (!indices) { why = "FAA application index snapshot allocation failed"; goto fail; }
   for (unsigned triangle = 0; triangle < triangles; triangle++) {
      unsigned source[3];
      if (d->info->mode == MESA_PRIM_TRIANGLES) {
         source[0] = 3 * triangle; source[1] = source[0] + 1; source[2] = source[0] + 2;
      } else if (d->info->mode == MESA_PRIM_QUADS) {
         unsigned first = (triangle / 2) * 4;
         if (d->state.flatshade) {
            /* Match the incoming Gallium provoking convention; CPU work
             * changes only triangle indices, never shader COLOR values. */
            if (d->state.flatshade_first) {
               source[0] = first;
               source[1] = first + (triangle & 1u ? 2 : 1);
               source[2] = first + (triangle & 1u ? 3 : 2);
            } else {
               source[0] = first + (triangle & 1u ? 1 : 0);
               source[1] = first + (triangle & 1u ? 2 : 1);
               source[2] = first + 3;
            }
         } else {
            source[0] = first; source[1] = first + (triangle & 1u ? 2 : 1);
            source[2] = first + (triangle & 1u ? 3 : 2);
         }
      } else if (d->info->mode == MESA_PRIM_QUAD_STRIP) {
         /* GL quad-strip quad order is (2i,2i+1,2i+3,2i+2).
          * Preserve orientation using its diagonal and requested COLOR source. */
         unsigned first = (triangle / 2) * 2;
         if (d->state.flatshade) {
            if (d->state.flatshade_first) {
               source[0] = first;
               source[1] = first + (triangle & 1u ? 3 : 1);
               source[2] = first + (triangle & 1u ? 2 : 3);
            } else {
               source[0] = first + (triangle & 1u ? 0 : 2);
               source[1] = first + (triangle & 1u ? 1 : 0);
               source[2] = first + 3;
            }
         } else {
            source[0] = first; source[1] = first + (triangle & 1u ? 3 : 1);
            source[2] = first + (triangle & 1u ? 2 : 3);
         }
      } else if (d->info->mode == MESA_PRIM_TRIANGLE_FAN) {
         source[0] = 0; source[1] = triangle + 1; source[2] = triangle + 2;
      } else {
         /* Smooth values are per vertex. Alternating the first two indices
          * preserves strip face orientation without transforming a vertex. */
         source[0] = triangle + (triangle & 1u);
         source[1] = triangle + !(triangle & 1u); source[2] = triangle + 2;
      }
      if (d->state.flatshade && d->state.flatshade_first) {
         /* Match Gallium's PV_FIRST→PV_LAST translation. Fan's first
          * provoking vertex is the rim vertex, not the shared center. */
         if (d->info->mode == MESA_PRIM_TRIANGLE_STRIP) {
            source[0] = triangle;
            source[1] = triangle + 1 + (triangle & 1u);
            source[2] = triangle + 2 - (triangle & 1u);
         } else if (d->info->mode == MESA_PRIM_TRIANGLE_FAN) {
            source[0] = triangle + 1; source[1] = triangle + 2; source[2] = 0;
         }
         unsigned first = source[0]; source[0] = source[1]; source[1] = source[2]; source[2] = first;
      }
      for (unsigned c = 0; c < 3; c++)
         if (!source_index(d, source[c], &indices[3 * triangle + c])) {
            why = "FAA application index snapshot leaves its retained owner";
            goto fail;
         }
   }
   struct m9_faa_live_gl live = d->live;
   live.first = 0;
   live.count = count;
   live.indices = indices;
   /* The source-qualified position tail carries an integer top-down
    * rectangle and default GL depth range. Do not silently substitute the
    * framebuffer size for an unrepresentable application viewport. */
   float w = 2 * d->state.viewport.scale[0];
   float h = -2 * d->state.viewport.scale[1];
   float x = d->state.viewport.translate[0] - d->state.viewport.scale[0];
   float y = d->state.viewport.translate[1] + d->state.viewport.scale[1];
   float z_near = d->state.viewport.translate[2] - d->state.viewport.scale[2];
   float z_far = d->state.viewport.translate[2] + d->state.viewport.scale[2];
   if (!(w > 0 && h > 0 && x >= 0 && y >= 0) ||
       w > 15359 || h > 15359 || x > 15359 - w || y > 15359 - h ||
       floorf(w) != w || floorf(h) != h || floorf(x) != x || floorf(y) != y ||
       !(z_near >= 0.0f && z_near <= 1.0f && z_far >= 0.0f && z_far <= 1.0f)) {
      why = "FAA application viewport/depth range has no qualified position tail";
      goto fail;
   }
   /* Diagnostic M9X_FAA_DIAG_FAR=1 moves a glDepthRange(1,1) DRAW (Quake3's
    * sky) just below the far plane, to tell z == 1 apart from its texels. */
   if (z_near == 1.0f && z_far == 1.0f && debug_get_num_option("M9X_FAA_DIAG_FAR", 0) == 1)
      z_near = z_far = 1.0f - 1.0f / 65536.0f;
   /* The FAA raster samples at integer pixel positions (MEASURED, edge
    * coverage and texels); GL samples at half-integer ones. */
   live.viewport = (struct m9_faa_viewport){x, y, w, h, .centre = 0.5f,
      .depth_range = true, .depth_near = z_near, .depth_far = z_far};
   const nir_shader *texture_fs=d->fs;
   /* White COLOR0: a stride-0 element at COLOR0's rank among the inputs. */
   static const float white[4]={1.0f,1.0f,1.0f,1.0f};
   struct pipe_vertex_element white_elements[PIPE_MAX_ATTRIBS];
   struct pipe_vertex_buffer white_buffers[PIPE_MAX_ATTRIBS];
   const uint8_t *white_maps[PIPE_MAX_ATTRIBS];unsigned white_bytes[PIPE_MAX_ATTRIBS];
   if(m9_faa_application_texture_unlit(d->fs)) {
      if(live.element_count>=PIPE_MAX_ATTRIBS||live.buffer_count>=PIPE_MAX_ATTRIBS||
         !m9_faa_live_gl_texture_white_normalize(live.vs,d->fs,&color_vs,&color_fs)) {
         why="FAA white COLOR0 clone failed";goto fail;
      }
      unsigned rank=util_bitcount64(live.vs->info.inputs_read&BITFIELD64_MASK(VERT_ATTRIB_COLOR0));
      if(rank>live.element_count){why="FAA white COLOR0 input rank";goto fail;}
      for(unsigned i=0,j=0;i<=live.element_count;i++)
         white_elements[i]=i==rank?(struct pipe_vertex_element){
            .src_format=PIPE_FORMAT_R32G32B32A32_FLOAT,.vertex_buffer_index=live.buffer_count}:
            live.elements[j++];
      memcpy(white_buffers,live.buffers,live.buffer_count*sizeof(*white_buffers));
      memcpy(white_maps,live.maps,live.buffer_count*sizeof(*white_maps));
      memcpy(white_bytes,live.map_bytes,live.buffer_count*sizeof(*white_bytes));
      white_buffers[live.buffer_count]=(struct pipe_vertex_buffer){.is_user_buffer=true,.buffer.user=white};
      white_maps[live.buffer_count]=(const uint8_t*)white;white_bytes[live.buffer_count]=sizeof(white);
      live.elements=white_elements;live.element_count++;
      live.buffers=white_buffers;live.maps=white_maps;live.map_bytes=white_bytes;live.buffer_count++;
      live.vs=color_vs;live.compile_cache=NULL;
      texture_fs=color_fs;
   }
   if(!m9_faa_application_texture_linkage(texture_fs,&texture_linkage,sampler,&texture_count,&why))goto fail;
   if(texture_count) {
      const bool dual=texture_count==2;
      if(d->state.flatshade||d->state.fragment.source_stream||
         !m9_faa_application_texture_valid(&d->texture[0])||
         (dual&&(!m9_faa_application_texture_valid(&d->texture[1])||
                 d->texture[0].resource==d->texture[1].resource||
                 d->texture[0].view.handle==d->texture[1].view.handle))) {
         why="FAA texture flat shading or retained texture owners are not qualified";goto fail;
      }
      unsigned projected;bool color_alpha;
      m9_faa_application_texture_form(texture_fs,&projected,&color_alpha);
      for(unsigned unit=0;unit<2;unit++)
         if((projected&(1u<<unit))&&
            !m9_faa_application_vs_w_one(live.vs,VARYING_SLOT_TEX0+unit,live.elements,live.element_count)) {
            why="FAA projected texture coordinate has no unit w";goto fail;
         }
      /* Blend and the alpha test the state tracker lowered into the FS. */
      struct m9_faa_texture_fragment_state family=d->state.texture_fragment;
      enum m9_faa_alpha_mode alpha;bool alpha_present;
      if(!m9_faa_live_gl_alpha_decode(texture_fs,d->fs_cb0,d->fs_cb0_bytes,&alpha,&alpha_present)) {
         why="FAA textured alpha test has no source-qualified interface";goto fail;
      }
      if(d->state.fragment.alpha!=M9_FAA_ALPHA_OFF&&
         (!alpha_present||alpha!=d->state.fragment.alpha)) {
         why="FAA DSA and lowered fragment alpha predicates disagree";goto fail;
      }
      if(alpha_present) {
         static const struct {enum m9_faa_alpha_func func;float ref;} test[]={
            [M9_FAA_ALPHA_GT_ZERO]={M9_FAA_ALPHA_FUNC_GREATER,0.0f},
            [M9_FAA_ALPHA_LT_HALF]={M9_FAA_ALPHA_FUNC_LESS,0.5f},
            [M9_FAA_ALPHA_GE_HALF]={M9_FAA_ALPHA_FUNC_GEQUAL,0.5f}};
         if(alpha==M9_FAA_ALPHA_OFF||(unsigned)alpha>=ARRAY_SIZE(test)||
            !m9_faa_alpha_ref_from_gl(test[alpha].func,test[alpha].ref,&family.alpha_ref)) {
            why="FAA textured alpha test has no source program";goto fail;
         }
         family.alpha_test=true;family.alpha_func=test[alpha].func;
      }
      /* M9X_FAA_FAMILIES=1 admits them until the card has run them. */
      if((family.blend||family.alpha_test)&&!debug_get_bool_option("M9X_FAA_FAMILIES",false)) {
         why="FAA textured blend/alpha families are disabled";goto fail;
      }
      if((family.blend||family.alpha_test)&&!d->state.texture_fragment_valid) {
         why="FAA textured blend factors have no source program";goto fail;
      }
      /* An RGBX render surface's X byte stands for its alpha. */
      for(unsigned i=0;i<texture_count;i++)
         if(d->texture[i].alpha_undefined&&
            (family.blend||family.alpha_test||d->state.destination_alpha)) {
            why="FAA RGBX surface texture alpha is observed";goto fail;
         }
      /* COLOR0.a alone is exact only where alpha is never observed. */
      if(color_alpha&&(family.blend||family.alpha_test||d->state.destination_alpha)) {
         why="FAA texture alpha from COLOR0 alone is observed";goto fail;
      }
      live.linkage=&texture_linkage;
      if(family.blend||family.alpha_test) {
         /* GL MODULATE: alpha is COLOR0.a * TEX0.a (* TEX1.a). */
         family.program=dual?M9_FAA_TEXTURE_DUAL:M9_FAA_TEXTURE_SINGLE;
         family.alpha_op=M9_FAA_TEXTURE_ALPHA_MODULATE;
         if(!m9_faa_live_gl_texture_family_build(&live,&family,&fresh.shader,&why))goto fail;
         fresh.fragment=PARHELIA_M9_FAA_FRAGMENT_MODULATE|
            (family.blend?PARHELIA_M9_FAA_FRAGMENT_BLEND|PARHELIA_M9_FAA_FRAGMENT_SRC(family.src)|
                          PARHELIA_M9_FAA_FRAGMENT_DST(family.dst):0)|
            (family.alpha_test?PARHELIA_M9_FAA_FRAGMENT_ALPHA_TEST|
                               PARHELIA_M9_FAA_FRAGMENT_FUNC(family.alpha_func)|
                               PARHELIA_M9_FAA_FRAGMENT_REF(family.alpha_ref):0);
      } else if(!(dual?m9_faa_live_gl_dual_texture_build(&live,&fresh.shader,&why):
                       m9_faa_live_gl_single_texture_build(&live,&fresh.shader,&why))) {
         goto fail;
      }
      /* A colour write mask changes only the stage's ab and f0 words. */
      fresh.fragment|=PARHELIA_M9_FAA_FRAGMENT_NO_COLOR(
         d->state.texture_fragment_valid?d->state.texture_fragment.color_off:0);
      for(unsigned i=0;i<texture_count;i++) {
         fresh.texture[i]=d->texture[i];fresh.texture[i].resource=NULL;
         pipe_resource_reference(&fresh.texture[i].resource,d->texture[i].resource);
      }
      fresh.texture_count=texture_count;fresh.state=d->state;
      free(indices);ralloc_free(color_vs);ralloc_free(color_fs);free(color_constants);
      *output=fresh;if(reason)*reason=NULL;return true;
   }
   if(d->state.texture_fragment_valid&&!d->state.color_fragment_valid) {
      why="FAA blend/alpha factors have no source-qualified RGBA interface";goto fail;
   }
   /* The source two-stage route has continuous M9128 rendering and pool
    * reuse proof. Keep M9X_FAA_FUSED=0 for four-stage comparisons. Both
    * builders consume owned GL state and raw records. */
   const char *fused_option = getenv("M9X_FAA_FUSED");
   bool fused = !fused_option || !strcmp(fused_option, "1");
   if(!m9_faa_live_gl_color_uniform_normalize(&live,d->fs,d->fs_cb0,d->fs_cb0_bytes,&color_vs,&color_fs,&color_constants,&color_constant_bytes)) {
      why="FAA current fragment color leaves its bounded retained uniform interface";goto fail;
   }
   const struct nir_shader *fs=color_fs?color_fs:d->fs;
   if(color_vs) {
      live.vs=color_vs;live.cb0=color_constants;live.cb0_bytes=color_constant_bytes;
      /* The clone replaces COLOR0 with one CB load at this exact offset.
       * Reuse code by semantic variant, never by the ephemeral clone pointer
       * or current color values. Current constants/raw inputs remain owned. */
      live.uniform_color_offset_plus_one=color_constant_bytes-16+1;
   }
   enum m9_faa_alpha_mode alpha;bool alpha_present;
   if (!m9_faa_live_gl_alpha_decode(fs,d->fs_cb0,d->fs_cb0_bytes,&alpha,&alpha_present)) {
      why="FAA fragment discard has no source-qualified alpha interface";goto fail;
   }
   struct m9_faa_fragment_state fragment=d->state.fragment;
   if(fragment.source_stream) {why="FAA effects require the bound COLOR0 stream";goto fail;}
   if (alpha_present) {
      if(fragment.alpha!=M9_FAA_ALPHA_OFF&&fragment.alpha!=alpha) {
         why="FAA DSA and lowered fragment alpha predicates disagree";goto fail;
      }
      fragment.alpha=alpha;
   }
   bool effects=fragment.blend!=M9_FAA_BLEND_REPLACE||fragment.alpha!=M9_FAA_ALPHA_OFF;
   if (effects && (!fused||d->state.flatshade)) {
      why="FAA blend/alpha requires its source-qualified smooth fused interface";goto fail;
   }
   if (!(effects ? m9_faa_live_gl_color_fused_effects_build_cb(&live,fs,&fragment,
                       d->fs_cb0,d->fs_cb0_bytes,&fresh.shader,&why) :
         fused ? m9_faa_live_gl_color_fused_build_state(&live, fs,
                    d->state.flatshade, &fresh.shader, &why) :
                 m9_faa_live_gl_color_build_state(&live, fs,
                    d->state.flatshade, &fresh.shader, &why)))
      goto fail;
   if (fresh.shader.color_flat != d->state.flatshade) {
      why = "FAA COLOR interpolation and application raster state disagree";
      goto fail;
   }
   fresh.state = d->state;
   fresh.state.fragment = fragment;
   free(indices);ralloc_free(color_vs);ralloc_free(color_fs);free(color_constants);
   *output = fresh;
   if (reason) *reason = NULL;
   return true;
fail:
   free(indices);ralloc_free(color_vs);ralloc_free(color_fs);free(color_constants);
   m9_faa_application_fini(&fresh);
   if (reason) *reason = why;
   return false;
}
