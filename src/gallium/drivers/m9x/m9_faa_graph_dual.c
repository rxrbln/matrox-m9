/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include "m9_internal.h"
#include "m9_screen.h"
#include "m9_faa_application.h"
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

static int
dual_pack(const struct m9_faa_gl_upload *u,struct drm_parhelia_m9_faa_color_draw *d)
{
   uintptr_t base=(uintptr_t)u->storage;
   uint64_t total=(uint64_t)u->layout.bytes+u->constants_bytes+u->geometry_bytes+36;
   if(!base||u->layout.count!=4||u->layout.entry_bytes[0]||u->layout.entry_bytes[1]!=72||
      !u->layout.bytes||u->layout.bytes>4096||u->record_bytes!=64||u->declaration_words!=9||
      !u->vertex_count||u->vertex_count%3||u->vertex_count>M9_FAA_GL_UPLOAD_MAX_VERTICES||
      u->vertex_offset!=128||(uint64_t)u->vertex_count*64!=u->vertex_bytes||
      (uint64_t)u->vertex_bytes+128!=u->geometry_bytes||
      !u->constants_bytes||u->constants_bytes>16384||
      total!=u->storage_bytes||total>0x42000||total>UINTPTR_MAX-base||
      (uintptr_t)u->program!=base||
      (uintptr_t)u->constants!=base+u->layout.bytes||
      (uintptr_t)u->geometry!=base+u->layout.bytes+u->constants_bytes||
      (uintptr_t)u->declaration!=base+u->layout.bytes+u->constants_bytes+u->geometry_bytes)
      return -EOPNOTSUPP;
   for(unsigned i=0;i<4;i++)
      if((u->layout.entry_bytes[i]&7)||u->layout.entry_bytes[i]>=u->layout.bytes||
         (i&&u->layout.entry_bytes[i]<=u->layout.entry_bytes[i-1])||
         (u->constant_begin[i]&7)||(u->constant_end[i]&7)||
         u->constant_begin[i]>u->constant_end[i]||u->constant_end[i]>u->constants_bytes)
         return -EINVAL;
   *d=(struct drm_parhelia_m9_faa_color_draw){.upload=base,.upload_bytes=total,
      .record_stride=64,.record_count=u->vertex_count,.declaration_count=9,
      .depth_flags=PARHELIA_M9_FAA_COLOR_DEPTH_LESS_WRITE};
   d->layout.program_bytes=u->layout.bytes;d->layout.program_count=4;
   memcpy(d->layout.entry_bytes,u->layout.entry_bytes,sizeof(d->layout.entry_bytes));
   d->layout.constants_bytes=u->constants_bytes;
   memcpy(d->layout.constant_begin,u->constant_begin,sizeof(d->layout.constant_begin));
   memcpy(d->layout.constant_end,u->constant_end,sizeof(d->layout.constant_end));
   d->layout.geometry_bytes=u->geometry_bytes;d->layout.vertex_offset=128;d->layout.vertex_bytes=u->vertex_bytes;
   return 0;
}

static bool
dual_texture_current(const struct m9_faa_application_texture *t)
{
   if(!t->resource)return false;
   const struct m9_resource *r=(const struct m9_resource *)t->resource;
   const struct drm_parhelia_m9_faa_texture_view *v=&t->view;
   struct m9_render_layout layout;
   return t->resource->target==PIPE_TEXTURE_2D&&t->resource->width0==64&&
      t->resource->height0==64&&t->resource->depth0==1&&t->resource->array_size==1&&
      !t->resource->last_level&&r->domain==M9_BO_VRAM&&!r->tiled&&
      r->width==64&&r->height==64&&r->depth==1&&
      r->handle==v->handle&&r->generation==t->generation&&r->size==t->owner_bytes&&
      r->kind==M9_RESOURCE_LINEAR_TEXTURE&&r->texture.levels==1&&
      r->texture.level[0].width==64&&r->texture.level[0].height==64&&
      r->texture.level[0].depth==1&&
      r->texture.level[0].offset==t->offset&&!t->offset&&
      !(r->texture_unplaced&1)&&(r->levels_written&1)&&r->texture_code==M9_TEXTURE_CODE_BGRA&&
      m9_render_surface_layout(64,64,4,&layout)&&r->size>=layout.size&&
      v->handle&&v->profile==12&&v->format==102&&v->width==64&&v->height==64&&
      v->storage_kind==1&&v->levels==1&&v->min_filter==1&&v->mag_filter==1&&
      v->mip_filter==1&&v->address_u==1&&v->address_v==1;
}

/* Retained dual DRAWs after the authenticated Unlock of the previous
 * textured Lock: the same program and pair (or a source-proved stage-1 owner
 * change) for up to three DRAWs, CLEAR3, depth-only or no clear, direct
 * Unlock epoch. */
static int
m9_faa_surface_dual_later_kernel(struct m9_screen *screen,struct m9_faa_surface *surface,
                                 const struct m9_faa_application_frame *frame)
{
   struct drm_parhelia_m9_faa_graph_dual_later request={0};
   struct drm_parhelia_m9_faa_color_draw draws[3]={{0}},saved[3];
   _Static_assert(sizeof(request)==272,"dual LATER ABI");
   struct m9_faa_graph *g=&surface->graph;
   const struct m9_faa_application_upload *first=&frame->draws[0];
   if(!frame->count||frame->count>3||
      (frame->clear_flags&&frame->clear_flags!=3&&frame->clear_flags!=2)||
      frame->clear_color)
      return -EOPNOTSUPP;
   if(g->color_texture_count!=2||g->color_program_count!=4||!g->color_lock_point||
      !g->color_unlock_attempted||g->color_lock_point>UINT64_MAX-0x20||
      g->color_unlock_point!=g->color_lock_point+0x20||(g->color_unlock_point&15)||
      g->color_unlock_point>UINT64_MAX-0x290||g->color_next_attempted||
      g->color_present_attempted||g->color_present_point||g->color_frame_present_point||
      g->color_frame_maintenance_point||!g->color_depth_initialized)
      return -ESTALE;
   for(unsigned i=0;i<frame->count;i++) {
      const struct m9_faa_application_upload *u=&frame->draws[i];
      const struct m9_faa_application_state *s=&u->state;
      if(u->graph_id!=g->prepared_id||u->init_point!=g->init_point||
         u->generation!=g->prepared_generation)return -ESTALE;
      if(u->texture_count!=2||s->width!=320||s->height!=240||
         !s->depth_enabled||!s->depth_write||s->depth_func!=PIPE_FUNC_LESS||
         s->flatshade||s->cull_face!=PIPE_FACE_NONE||s->scissor_enabled||
         s->fragment.blend||s->fragment.alpha||s->fragment.source_stream||
         u->texture[0].resource==u->texture[1].resource||
         u->texture[0].view.handle==u->texture[1].view.handle)return -EOPNOTSUPP;
      for(unsigned t=0;t<2;t++) {
         if(!dual_texture_current(&u->texture[t]))return -ESTALE;
         if(u->texture[t].resource!=first->texture[t].resource||
            memcmp(&u->texture[t].view,&first->texture[t].view,sizeof(u->texture[t].view)))
            return -EOPNOTSUPP;
      }
      int ret=dual_pack(&u->shader,&draws[i]);if(ret)return ret;
      if(i&&(memcmp(&u->shader.layout,&first->shader.layout,sizeof(u->shader.layout))||
         memcmp(u->shader.program,first->shader.program,u->shader.layout.bytes)||
         memcmp(u->shader.declaration,first->shader.declaration,36)))return -EOPNOTSUPP;
   }
   const uint64_t delta=(frame->clear_flags==3?0x150:frame->clear_flags==2?0x120:0xd0)+
                        (frame->count-1u)*UINT64_C(0xa0);
   request=(struct drm_parhelia_m9_faa_graph_dual_later){.graph_id=g->prepared_id,
      .previous_unlock_point=g->color_unlock_point,.generation=g->prepared_generation,
      .width=320,.height=240,.clear_flags=frame->clear_flags,
      .depth_flags=draws[0].depth_flags};
   if(frame->count==1) {
      request.version=PARHELIA_M9_FAA_DUAL_LATER_VERSION;
      request.upload=draws[0].upload;request.upload_bytes=draws[0].upload_bytes;
      request.record_stride=draws[0].record_stride;request.record_count=draws[0].record_count;
      request.declaration_count=draws[0].declaration_count;request.layout=draws[0].layout;
   } else {
      request.version=PARHELIA_M9_FAA_DUAL_LATER_BATCH_VERSION;
      request.draws=(uintptr_t)draws;request.draw_count=frame->count;
   }
   request.texture[0]=first->texture[0].view;request.texture[1]=first->texture[1].view;
   const struct drm_parhelia_m9_faa_graph_dual_later before=request;
   memcpy(saved,draws,sizeof(saved));
   g->color_next_attempted=true;
   if(ioctl(screen->fd,DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_DUAL_LATER,&request))return -errno;
   struct drm_parhelia_m9_faa_graph_dual_later immutable=request;
   immutable.lock_point=0;immutable.state=0;
   if(request.lock_point!=before.previous_unlock_point+delta||
      request.state!=PARHELIA_M9_FAA_DUAL_LATER_LOCK_RETIRED||
      memcmp(&immutable,&before,sizeof(before))||memcmp(saved,draws,sizeof(saved)))
      return -EPROTO;
   int ret=m9_wait_for_point(screen,request.lock_point);if(ret)return ret;
   g->color_next_previous_unlock_point=before.previous_unlock_point;
   g->color_next_lock_point=g->color_lock_point=request.lock_point;
   g->color_lock_delta=delta;g->color_texture_later=true;
   g->color_frame_draw_count=0;g->color_frame_flags=0;
   g->color_unlock_attempted=false;g->color_unlock_point=0;
   return 0;
}

int
m9_faa_surface_dual_batch_kernel(struct m9_screen *screen,struct m9_faa_surface *surface,
                                 const struct m9_faa_application_frame *frame)
{
   struct drm_parhelia_m9_faa_graph_dual_batch request={0};
   struct drm_parhelia_m9_faa_color_draw draws[3]={{0}},saved[3];
   _Static_assert(sizeof(request)==168,"dual BATCH ABI");
   if(!screen||!surface||!frame||!frame->draws||!frame->count)return -EINVAL;
   struct m9_faa_graph *g=&surface->graph;
   if(g->color_texture_count&&screen->faa.chip==M9_FAA_CHIP_M9&&screen->wait_point&&
      surface->width==320&&surface->height==240&&surface->format==0x66)
      return m9_faa_surface_dual_later_kernel(screen,surface,frame);
   if(frame->count>3||screen->faa.chip!=M9_FAA_CHIP_M9||!screen->wait_point||
      surface->width!=320||surface->height!=240||surface->format!=0x66||
      frame->clear_flags!=3||frame->clear_color)return -EOPNOTSUPP;
   if(!g->prepared||!g->initialized||!g->prepared_id||!g->init_point||(g->init_point&15)||
      g->color_lock_attempted||g->color_lock_point||g->color_depth_initialized||
      g->color_program_count||g->color_texture_count||g->color_frame_draw_count||
      g->color_unlock_attempted||g->color_present_attempted||g->color_next_attempted||
      g->color_unlock_point||g->color_present_point||g->color_next_lock_point||
      g->color_next_previous_unlock_point||g->color_frame_flags||
      g->color_frame_maintenance_point||g->color_frame_present_point||g->color_frame_mode_planned||
      g->public_vs_attempted||g->public_vs_head_attempted||g->public_vs_draw_attempted||
      g->public_vs_lock_attempted||g->public_vs_next_attempted)return -ESTALE;
   const struct m9_faa_application_upload *first=&frame->draws[0];
   for(unsigned i=0;i<frame->count;i++) {
      const struct m9_faa_application_upload *u=&frame->draws[i];
      const struct m9_faa_application_state *s=&u->state;
      if(u->graph_id!=g->prepared_id||u->init_point!=g->init_point||
         u->generation!=g->prepared_generation)return -ESTALE;
      if(u->texture_count!=2||s->width!=320||s->height!=240||
         !s->depth_enabled||!s->depth_write||s->depth_func!=PIPE_FUNC_LESS||
         s->flatshade||s->cull_face!=PIPE_FACE_NONE||s->scissor_enabled||
         s->fragment.blend||s->fragment.alpha||s->fragment.source_stream||
         s->viewport.scale[0]!=160||s->viewport.scale[1]!=-120||s->viewport.scale[2]!=.5f||
         s->viewport.translate[0]!=160||s->viewport.translate[1]!=120||s->viewport.translate[2]!=.5f||
         u->texture[0].resource==u->texture[1].resource||
         u->texture[0].view.handle==u->texture[1].view.handle)return -EOPNOTSUPP;
      for(unsigned t=0;t<2;t++) {
         if(!dual_texture_current(&u->texture[t]))return -ESTALE;
         if(u->texture[t].resource!=first->texture[t].resource||
            memcmp(&u->texture[t].view,&first->texture[t].view,sizeof(u->texture[t].view)))
            return -EOPNOTSUPP;
      }
      int ret=dual_pack(&u->shader,&draws[i]);if(ret)return ret;
      if(i&&(memcmp(&u->shader.layout,&first->shader.layout,sizeof(u->shader.layout))||
         memcmp(u->shader.program,first->shader.program,u->shader.layout.bytes)||
         memcmp(u->shader.declaration,first->shader.declaration,36)))return -EOPNOTSUPP;
   }
   uint64_t delta=0x150+(frame->count-1u)*UINT64_C(0xa0);
   if(g->init_point>UINT64_MAX-delta)return -ERANGE;
   if(frame->count==1) {
      struct drm_parhelia_m9_faa_texture_view texture[2]={first->texture[0].view,first->texture[1].view};
      int ret=m9_faa_surface_dual_first_kernel(screen,surface,&first->shader,texture);
      if(ret)return ret;
   } else {
      request=(struct drm_parhelia_m9_faa_graph_dual_batch){.graph_id=g->prepared_id,
         .init_point=g->init_point,.draws=(uintptr_t)draws,.draw_count=frame->count,
         .generation=g->prepared_generation,.version=PARHELIA_M9_FAA_DUAL_BATCH_VERSION,
         .width=320,.height=240,.clear_flags=3};
      request.texture[0]=first->texture[0].view;request.texture[1]=first->texture[1].view;
      const struct drm_parhelia_m9_faa_graph_dual_batch before=request;
      memcpy(saved,draws,sizeof(saved));g->color_lock_attempted=true;g->color_program_count=4;
      if(ioctl(screen->fd,DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_DUAL_BATCH,&request))return -errno;
      struct drm_parhelia_m9_faa_graph_dual_batch immutable=request;
      immutable.lock_point=0;immutable.state=0;
      if(request.lock_point!=g->init_point+delta||request.state!=PARHELIA_M9_FAA_DUAL_BATCH_LOCK_RETIRED||
         memcmp(&immutable,&before,sizeof(before))||memcmp(saved,draws,sizeof(saved)))return -EPROTO;
      int ret=m9_wait_for_point(screen,request.lock_point);if(ret)return ret;
      g->color_lock_point=request.lock_point;g->color_lock_delta=delta;g->color_depth_initialized=true;
   }
   g->color_texture_count=2;g->color_frame_draw_count=frame->count;
   g->color_frame_flags=PARHELIA_M9_FAA_COLOR_FRAME_FIRST;
   return 0;
}
