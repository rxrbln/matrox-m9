/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include "m9_internal.h"
#include "m9_screen.h"
#include "m9_faa_application.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

static int
dual_pack(const struct m9_faa_gl_upload *u,const struct m9_faa_application_state *s,
          bool single,struct drm_parhelia_m9_faa_color_draw *d)
{
   uintptr_t base=(uintptr_t)u->storage;
   /* Dual programs with four vec4 inputs, single ones with three; the FS
    * length follows the fragment family. */
   const uint32_t record=single?48:64,words=single?7:9;
   uint64_t total=(uint64_t)u->layout.bytes+u->constants_bytes+u->geometry_bytes+4*words;
   if(!base||u->layout.count!=4||u->layout.entry_bytes[0]||
      !u->layout.entry_bytes[1]||
      !u->layout.bytes||u->layout.bytes>4096||u->record_bytes!=record||u->declaration_words!=words||
      !u->vertex_count||u->vertex_count%3||u->vertex_count>M9_FAA_GL_UPLOAD_MAX_VERTICES||
      u->vertex_offset!=128||(uint64_t)u->vertex_count*record!=u->vertex_bytes||
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
      .record_stride=record,.record_count=u->vertex_count,.declaration_count=words,
      .depth_flags=1u|(s->depth_write?2u:0u)|((uint32_t)s->depth_func<<2)};
   /* A disabled depth test neither tests nor writes: ALWAYS, no write. */
   if(!s->depth_enabled)d->depth_flags=1u|((uint32_t)PIPE_FUNC_ALWAYS<<2);
   if(s->cull_face==PIPE_FACE_FRONT||s->cull_face==PIPE_FACE_BACK)
      d->raster_flags=((s->cull_face==PIPE_FACE_BACK)==s->front_ccw)?
         PARHELIA_M9_FAA_COLOR_RASTER_CULL_CW:PARHELIA_M9_FAA_COLOR_RASTER_CULL_CCW;
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
   /* Any power-of-two single-level BGRA 2-D owner: the kernel derives the
    * storage kind (0x01, or 0x21 above 64) and extent from these sizes. */
   const uint32_t w=v->width,h=v->height;
   return w&&h&&w<=1024&&h<=1024&&!(w&(w-1))&&!(h&(h-1))&&
      t->resource->target==PIPE_TEXTURE_2D&&t->resource->width0==w&&
      t->resource->height0==h&&t->resource->depth0==1&&t->resource->array_size==1&&
      t->resource->last_level+1u==v->levels&&r->domain==M9_BO_VRAM&&!r->tiled&&
      r->width==w&&r->height==h&&r->depth==1&&
      r->handle==v->handle&&r->generation==t->generation&&r->size==t->owner_bytes&&
      r->kind==M9_RESOURCE_LINEAR_TEXTURE&&r->texture.levels==v->levels&&
      r->texture.level[0].width==w&&r->texture.level[0].height==h&&
      r->texture.level[0].depth==1&&
      r->texture.level[0].offset==t->offset&&!t->offset&&
      !(r->texture_unplaced&((2u<<(v->levels-1))-1))&&
      (r->levels_written&((2u<<(v->levels-1))-1))==((2u<<(v->levels-1))-1)&&
      m9_faa_texture_code(r->texture_code)&&r->texture_code==v->format&&
      m9_render_surface_layout(w,h,4,&layout)&&r->size>=layout.size&&
      v->handle&&v->profile==12&&
      v->storage_kind==(w>64||h>64?0x21u:0x01u)&&v->levels>=1&&v->levels<=11&&
      (v->min_filter==1||v->min_filter==2)&&(v->mag_filter==1||v->mag_filter==2)&&
      v->mip_filter>=1&&v->mip_filter<=3&&(v->levels>1||v->mip_filter==1)&&
      m9_faa_texture_address(v->address_u)&&m9_faa_texture_address(v->address_v);
}

/* Per-DRAW checks shared by later transactions and the frame pre-check.
 * prior[] holds the DRAWs before this one: every DRAW of one kind carries
 * the same record declaration. */
static int
dual_draw_admissible(const struct m9_faa_surface *surface,
                     const struct m9_faa_application_upload *u,
                     const struct m9_faa_application_upload *prior,unsigned prior_count,
                     struct drm_parhelia_m9_faa_color_draw *packed)
{
   const struct m9_faa_graph *g=&surface->graph;
   const struct m9_faa_application_state *s=&u->state;
   if(u->graph_id!=g->prepared_id||u->init_point!=g->init_point||
      u->generation!=g->prepared_generation)return -ESTALE;
   if((u->texture_count!=1&&u->texture_count!=2)||
      s->width!=surface->width||s->height!=surface->height||
      (s->depth_enabled&&s->depth_func>PIPE_FUNC_ALWAYS)||
      s->flatshade||s->cull_face==PIPE_FACE_FRONT_AND_BACK||s->scissor_enabled||
      s->fragment.source_stream||
      (u->texture_count==2&&
       (u->texture[0].resource==u->texture[1].resource||
        u->texture[0].view.handle==u->texture[1].view.handle)))return -EOPNOTSUPP;
   for(unsigned t=0;t<u->texture_count;t++)
      if(!dual_texture_current(&u->texture[t]))return -ESTALE;
   int ret=dual_pack(&u->shader,s,u->texture_count==1,packed);if(ret)return ret;
   /* Programs may differ per DRAW (the kernel caches each by its bytes);
    * one kind keeps one record declaration. */
   const struct m9_faa_application_upload *f=NULL;
   for(unsigned i=0;i<prior_count&&!f;i++)
      if(prior[i].texture_count==u->texture_count)f=&prior[i];
   if(f&&(u->shader.declaration_words!=f->shader.declaration_words||
          memcmp(u->shader.declaration,f->shader.declaration,u->shader.declaration_words*4)))
      return -EOPNOTSUPP;
   return 0;
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
   _Static_assert(sizeof(request)==280,"dual LATER ABI");
   struct m9_faa_graph *g=&surface->graph;
   const struct m9_faa_application_upload *first=&frame->draws[0];
   if(!frame->count||frame->count>PARHELIA_M9_FAA_DUAL_LATER_MAX_DRAWS||
      (frame->clear_flags&&frame->clear_flags!=3&&frame->clear_flags!=2)||
      frame->clear_color)
      return -EOPNOTSUPP;
   if(g->color_texture_count!=2||g->color_program_count!=4||!g->color_lock_point||
      !g->color_unlock_attempted||g->color_lock_point>UINT64_MAX-0x20||
      g->color_unlock_point!=g->color_lock_point+0x20||(g->color_unlock_point&15)||
      g->color_unlock_point>UINT64_MAX-0x1a0-PARHELIA_M9_FAA_DUAL_LATER_MAX_DRAWS*0xa0u||
      g->color_next_attempted||
      g->color_present_attempted||g->color_present_point||g->color_frame_present_point||
      g->color_frame_maintenance_point||!g->color_depth_initialized)
      return -ESTALE;
   /* Each DRAW carries its own texture pair (version 3). */
   struct drm_parhelia_m9_faa_dual_draw *draws=calloc(frame->count,sizeof(*draws));
   struct drm_parhelia_m9_faa_dual_draw *saved=calloc(frame->count,sizeof(*saved));
   int ret=-ENOMEM;
   if(!draws||!saved)goto out;
   for(unsigned i=0;i<frame->count;i++) {
      const struct m9_faa_application_upload *u=&frame->draws[i];
      ret=dual_draw_admissible(surface,u,frame->draws,i,&draws[i].draw);if(ret)goto out;
      /* A single DRAW names no stage-1 texture. */
      draws[i].program=u->texture_count==1?PARHELIA_M9_FAA_PROGRAM_SINGLE:
                                          PARHELIA_M9_FAA_PROGRAM_DUAL;
      draws[i].fragment=u->fragment;
      for(unsigned t=0;t<u->texture_count;t++)draws[i].texture[t]=u->texture[t].view;
   }
   const uint64_t delta=(frame->clear_flags==3?0x150:frame->clear_flags==2?0x120:0xd0)+
                        (frame->count-1u)*UINT64_C(0xa0);
   request=(struct drm_parhelia_m9_faa_graph_dual_later){.graph_id=g->prepared_id,
      .previous_unlock_point=g->color_unlock_point,.generation=g->prepared_generation,
      .width=surface->width,.height=surface->height,.clear_flags=frame->clear_flags,
      .depth_flags=draws[0].draw.depth_flags};
   if(frame->count==1&&!draws[0].draw.raster_flags&&!draws[0].fragment&&
      draws[0].program==PARHELIA_M9_FAA_PROGRAM_DUAL) {
      const struct drm_parhelia_m9_faa_color_draw *d=&draws[0].draw;
      request.version=PARHELIA_M9_FAA_DUAL_LATER_VERSION;
      request.upload=d->upload;request.upload_bytes=d->upload_bytes;
      request.record_stride=d->record_stride;request.record_count=d->record_count;
      request.declaration_count=d->declaration_count;request.layout=d->layout;
   } else {
      request.version=PARHELIA_M9_FAA_DUAL_LATER_TEXTURES_VERSION;
      request.draws=(uintptr_t)draws;request.draw_count=frame->count;
   }
   request.texture[0]=first->texture[0].view;request.texture[1]=first->texture[1].view;
   const struct drm_parhelia_m9_faa_graph_dual_later before=request;
   memcpy(saved,draws,frame->count*sizeof(*draws));
   g->color_next_attempted=true;
   if(ioctl(screen->fd,DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_DUAL_LATER,&request)){ret=-errno;goto out;}
   struct drm_parhelia_m9_faa_graph_dual_later immutable=request;
   immutable.lock_point=0;immutable.state=0;immutable.ordinary_point=0;
   ret=-EPROTO;
   /* Ordinary jobs after the Unlock move the frame's origin. */
   const uint64_t origin=request.ordinary_point?request.ordinary_point:before.previous_unlock_point;
   if((request.ordinary_point&15)||
      (request.ordinary_point&&request.ordinary_point<=before.previous_unlock_point)||
      origin>UINT64_MAX-delta||request.lock_point!=origin+delta||
      request.state!=PARHELIA_M9_FAA_DUAL_LATER_LOCK_RETIRED||
      memcmp(&immutable,&before,sizeof(before))||
      memcmp(saved,draws,frame->count*sizeof(*draws)))
      goto out;
   ret=m9_wait_for_point(screen,request.lock_point);if(ret)goto out;
   g->color_next_previous_unlock_point=before.previous_unlock_point;
   g->color_next_lock_point=g->color_lock_point=request.lock_point;
   g->color_lock_delta=request.lock_point-before.previous_unlock_point;g->color_texture_later=true;
   g->color_frame_draw_count=0;g->color_frame_flags=0;g->color_frame_ordinary_point=request.ordinary_point;
   g->color_unlock_attempted=false;g->color_unlock_point=0;
   ret=0;
out:
   free(draws);free(saved);
   return ret;
}

static int
m9_faa_surface_dual_frame_kernel(struct m9_screen *screen,struct m9_faa_surface *surface,
                                 const struct m9_faa_application_frame *frame)
{
   struct drm_parhelia_m9_faa_graph_dual_batch request={0};
   /* Version 1 packs plain records; version 2 adds programs and textures. */
   struct drm_parhelia_m9_faa_dual_draw records[3]={{{0}}},saved[3];
   struct drm_parhelia_m9_faa_color_draw plain[3]={{0}};
   _Static_assert(sizeof(request)==176,"dual BATCH ABI");
   if(!screen||!surface||!frame||!frame->draws||!frame->count)return -EINVAL;
   struct m9_faa_graph *g=&surface->graph;
   if(g->color_texture_count&&screen->faa.chip==M9_FAA_CHIP_M9&&screen->wait_point&&
      m9_faa_runtime_dimensions(surface->width,surface->height)&&surface->format==0x66)
      return m9_faa_surface_dual_later_kernel(screen,surface,frame);
   if(frame->count>3||screen->faa.chip!=M9_FAA_CHIP_M9||!screen->wait_point||
      !m9_faa_runtime_dimensions(surface->width,surface->height)||surface->format!=0x66||
      frame->clear_flags!=3||frame->clear_color)return -EOPNOTSUPP;
   if(!g->prepared||!g->initialized||!g->prepared_id||!g->init_point||(g->init_point&15)||
      g->color_lock_attempted||g->color_lock_point||g->color_depth_initialized||
      g->color_program_count||g->color_texture_count||g->color_frame_draw_count||
      g->color_unlock_attempted||g->color_present_attempted||g->color_next_attempted||
      g->color_unlock_point||g->color_present_point||g->color_next_lock_point||
      g->color_next_previous_unlock_point||g->color_frame_flags||
      g->color_frame_maintenance_point||g->color_frame_ordinary_point||g->color_frame_present_point||g->color_frame_mode_planned||
      g->public_vs_attempted||g->public_vs_head_attempted||g->public_vs_draw_attempted||
      g->public_vs_lock_attempted||g->public_vs_next_attempted)return -ESTALE;
   const struct m9_faa_application_upload *first=&frame->draws[0];
   bool programs=false;
   for(unsigned i=0;i<frame->count;i++) {
      const struct m9_faa_application_upload *u=&frame->draws[i];
      const struct m9_faa_application_state *s=&u->state;
      if(s->viewport.scale[0]!=surface->width*.5f||s->viewport.scale[1]!=surface->height*-.5f||
         s->viewport.translate[0]!=surface->width*.5f||
         s->viewport.translate[1]!=surface->height*.5f)return -EOPNOTSUPP;
      int ret=dual_draw_admissible(surface,u,frame->draws,i,&records[i].draw);if(ret)return ret;
      /* The FIRST binds draw 0's pair; dual DRAWs keep it. */
      if(u->texture_count==2&&first->texture_count==2)
         for(unsigned t=0;t<2;t++)
            if(u->texture[t].resource!=first->texture[t].resource||
               memcmp(&u->texture[t].view,&first->texture[t].view,sizeof(u->texture[t].view)))
               return -EOPNOTSUPP;
      if(u->texture_count==2&&first->texture_count==1)return -EOPNOTSUPP;
      records[i].program=u->texture_count==1?PARHELIA_M9_FAA_PROGRAM_SINGLE:
                                            PARHELIA_M9_FAA_PROGRAM_DUAL;
      for(unsigned t=0;t<u->texture_count;t++)records[i].texture[t]=u->texture[t].view;
      records[i].fragment=u->fragment;
      programs|=u->texture_count==1||u->fragment;
      plain[i]=records[i].draw;
   }
   uint64_t delta=0x150+(frame->count-1u)*UINT64_C(0xa0);
   if(g->init_point>UINT64_MAX-delta)return -ERANGE;
   /* Descriptor batches of one or more DRAWs carry live depth and cull. */
   {
      request=(struct drm_parhelia_m9_faa_graph_dual_batch){.graph_id=g->prepared_id,
         .init_point=g->init_point,
         .draws=programs?(uintptr_t)records:(uintptr_t)plain,.draw_count=frame->count,
         .generation=g->prepared_generation,
         .version=programs?PARHELIA_M9_FAA_DUAL_BATCH_PROGRAMS_VERSION:PARHELIA_M9_FAA_DUAL_BATCH_VERSION,
         .width=surface->width,.height=surface->height,.clear_flags=3};
      request.texture[0]=records[0].texture[0];request.texture[1]=records[0].texture[1];
      const struct drm_parhelia_m9_faa_graph_dual_batch before=request;
      memcpy(saved,records,sizeof(saved));g->color_lock_attempted=true;g->color_program_count=4;
      struct drm_parhelia_m9_faa_color_draw plain_saved[3];memcpy(plain_saved,plain,sizeof(plain));
      if(ioctl(screen->fd,DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_DUAL_BATCH,&request))return -errno;
      struct drm_parhelia_m9_faa_graph_dual_batch immutable=request;
      immutable.lock_point=0;immutable.state=0;immutable.ordinary_point=0;
      /* Ordinary jobs after INIT move the FIRST's origin. */
      const uint64_t origin=request.ordinary_point?request.ordinary_point:g->init_point;
      if((request.ordinary_point&15)||(request.ordinary_point&&request.ordinary_point<=g->init_point)||
         origin>UINT64_MAX-delta||
         request.lock_point!=origin+delta||request.state!=PARHELIA_M9_FAA_DUAL_BATCH_LOCK_RETIRED||
         memcmp(&immutable,&before,sizeof(before))||memcmp(saved,records,sizeof(saved))||
         memcmp(plain_saved,plain,sizeof(plain)))return -EPROTO;
      int ret=m9_wait_for_point(screen,request.lock_point);if(ret)return ret;
      g->color_lock_point=request.lock_point;g->color_lock_delta=request.lock_point-g->init_point;
      g->color_frame_ordinary_point=request.ordinary_point;g->color_depth_initialized=true;
   }
   g->color_texture_count=2;g->color_frame_draw_count=frame->count;
   g->color_frame_flags=PARHELIA_M9_FAA_COLOR_FRAME_FIRST;
   return 0;
}

/* DRAWs that may share one FIRST transaction: the same depth and cull
 * state (the winding matters only when culling) and one pair. Later DRAWs
 * carry their own A8 depth word and 6A raster groups (vendor later/). */
static bool
dual_same_state(const struct m9_faa_application_upload *a,
                const struct m9_faa_application_upload *b, bool first)
{
   if(!first)return true;
   if(a->state.depth_enabled!=b->state.depth_enabled||
      a->state.depth_write!=b->state.depth_write||
      a->state.depth_func!=b->state.depth_func||
      a->state.cull_face!=b->state.cull_face||
      (a->state.cull_face!=PIPE_FACE_NONE&&
       a->state.front_ccw!=b->state.front_ccw))return false;
   for(unsigned t=0;t<2;t++)
      if(a->texture[t].resource!=b->texture[t].resource||
         memcmp(&a->texture[t].view,&b->texture[t].view,sizeof(a->texture[t].view)))
         return false;
   return true;
}

/* Whether next may join a transaction of n DRAWs: one declaration per
 * kind; a FIRST also one program per kind, later DRAWs switch programs
 * through the kernel's program cache. */
static bool
dual_same_program(const struct m9_faa_application_upload *group,uint32_t n,
                  const struct m9_faa_application_upload *next,bool first)
{
   for(uint32_t i=0;i<n;i++) {
      const struct m9_faa_application_upload *u=&group[i];
      if(u->texture_count!=next->texture_count)continue;
      if(u->shader.declaration_words!=next->shader.declaration_words||
         memcmp(u->shader.declaration,next->shader.declaration,u->shader.declaration_words*4))
         return false;
      if(first&&
         (memcmp(&u->shader.layout,&next->shader.layout,sizeof(u->shader.layout))||
          memcmp(u->shader.program,next->shader.program,u->shader.layout.bytes)))
         return false;
   }
   return true;
}

/* Bytes one DRAW takes in the kernel's 256 KiB later data arena. */
static uint64_t
dual_arena_bytes(const struct m9_faa_application_upload *u)
{
   const uint64_t raw=128+(uint64_t)align(u->shader.vertex_count,4)*64;
   return align64(u->shader.constants_bytes,128)+
      MAX2((uint64_t)u->shader.geometry_bytes,raw);
}

/* Every DRAW of a frame is checked before its first transaction posts, so
 * a refusal never leaves a partially rendered frame behind. */
static int
dual_frame_admissible(const struct m9_faa_surface *surface,
                      const struct m9_faa_application_frame *frame)
{
   struct drm_parhelia_m9_faa_color_draw packed;
   for(uint32_t i=0;i<frame->count;i++) {
      int ret=dual_draw_admissible(surface,&frame->draws[i],frame->draws,i,&packed);
      if(ret)return ret;
   }
   return 0;
}

/* A frame is submitted as consecutive retained transactions of at most
 * three DRAWs sharing one texture pair. Only the first carries the frame's
 * clear; each later one follows an Unlock of its predecessor's Lock. The
 * caller presents after the last Lock. */
int
m9_faa_surface_dual_batch_kernel(struct m9_screen *screen,struct m9_faa_surface *surface,
                                 const struct m9_faa_application_frame *frame)
{
   if(!screen||!surface||!frame||!frame->draws||!frame->count)return -EINVAL;
   int checked=dual_frame_admissible(surface,frame);
   if(checked)return checked;
   for(uint32_t start=0;start<frame->count;){
      /* A FIRST holds three DRAWs of one pair; later transactions take up to
       * the kernel's limit, each DRAW with its own pair. */
      const bool later=surface->graph.color_texture_count!=0;
      const uint32_t limit=later?PARHELIA_M9_FAA_DUAL_LATER_MAX_DRAWS:3;
      uint64_t arena=align64(dual_arena_bytes(&frame->draws[start]),128);
      uint32_t n=1;
      while(start+n<frame->count&&n<limit&&
            dual_same_state(&frame->draws[start],&frame->draws[start+n],!later)&&
            dual_same_program(frame->draws+start,n,&frame->draws[start+n],!later)&&
            arena+dual_arena_bytes(&frame->draws[start+n])<=0x40000) {
         arena=align64(arena+dual_arena_bytes(&frame->draws[start+n]),128);
         n++;
      }
      struct m9_faa_application_frame part={.draws=frame->draws+start,.count=n,
         .clear_flags=start?0:frame->clear_flags,
         .clear_color=start?0:frame->clear_color};
      if(start){
         int ret=m9_faa_surface_color_unlock_kernel(screen,surface);
         if(ret)return ret;
      }
      int ret=m9_faa_surface_dual_frame_kernel(screen,surface,&part);
      if(ret)return ret;
      start+=n;
   }
   return 0;
}
