/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_DEPTH_MODE_BATCH_H
#define M9_FAA_DEPTH_MODE_BATCH_H
#include "m9_faa_depth_mode_append.h"
struct m9x_faa_depth_mode_batch_job {u32 depth_flags,declaration_count;};
struct m9x_faa_depth_mode_batch_plan {
 struct m9x_faa_depth_mode_state first,after;
 u32 delta,end_count,secondary_bytes,additional_primary_bytes,initial_reset_bytes;
 u32 mode_changes;
};
/* Owned copied jobs only. Compute the exact source transaction before
 * allocating, rotating pools or changing a retained graph. No GPU state is
 * published by this plan. Public native integration independently validates
 * profile2, viewport, initialized history and authenticated origin. */
static inline int m9x_faa_depth_mode_batch_count(bool first,u32 clear_flags,
 const struct m9x_faa_depth_mode_state *before,
 const struct m9x_faa_depth_mode_batch_job *jobs,u32 count,
 struct m9x_faa_depth_mode_batch_plan *out)
{
 struct m9x_faa_depth_mode_batch_plan p={0};
 struct m9x_faa_depth_mode_state state;
 struct m9x_faa_depth_mode_image route;
 u32 i,extra,flags;
 if(!before||!jobs||!out||!count||count>M9_FAA_FRAME_BATCH_MAX_DRAWS||
  (clear_flags&&clear_flags!=3)||(first&&clear_flags!=3)||
  (first?before->valid:!before->valid))return -EINVAL;
 state=*before;
 for(i=0;i<count;i++){
  if(jobs[i].declaration_count!=3&&jobs[i].declaration_count!=5)return -EINVAL;
  flags=jobs[i].depth_flags?jobs[i].depth_flags:15;
  if(!m9x_faa_depth_mode_flags_valid(flags)||
   (state.valid&&!state.enabled&&!state.embedded_hz)||
   (!state.valid&&(state.enabled||state.embedded_hz)))return -EINVAL;
  route.after=(struct m9x_faa_depth_mode_state){true,!!(flags&1),state.embedded_hz||!(flags&1)};
  route.reset_delta=state.valid&&state.enabled!=route.after.enabled?0x50:0;
  extra=jobs[i].declaration_count==5?24:0;
  if(!i){
   /* The explicit later CLEAR3 control stops at an unread finite query
    * if its initial draw disables depth. Prior-disabled CLEAR3 state is
    * also not closed by the current source receipt. Preserve that gate. */
   if(!first&&clear_flags&&(!state.enabled||state.embedded_hz||!route.after.enabled))return -EOPNOTSUPP;
   p.delta=first||clear_flags?0x110:0x90;
   p.end_count=first?14:clear_flags?13:4;
   p.secondary_bytes=1352+extra+(!first&&!clear_flags&&!route.reset_delta?4:0);
   if(route.reset_delta){
    p.delta+=0x50;p.end_count+=3;p.mode_changes++;
    p.initial_reset_bytes=4156+(state.embedded_hz?16:0);
   }
   p.first=route.after;
  }else{
   p.delta+=0x60;p.additional_primary_bytes+=24;
   if(route.reset_delta){
    p.delta+=0x50;p.end_count+=3;p.mode_changes++;
    p.additional_primary_bytes+=116+4156+(state.embedded_hz?16:0);
    p.secondary_bytes+=1352+extra;
   }else p.secondary_bytes+=1484+extra;
  }
  state=route.after;
 }
 p.after=state;*out=p;return 0;
}
/* Authenticate live address routes independently after allocations. */
static inline int m9x_faa_depth_mode_batch_plan(bool first,u32 clear_flags,
 const struct m9x_faa_depth_mode_state *before,
 const struct m9x_faa_graph_draw_owner *owner,const struct m9x_faa_depth_views *views,
 const struct m9x_faa_depth_mode_batch_job *jobs,u32 count,
 struct m9x_faa_depth_mode_batch_plan *out)
{
 struct m9x_faa_depth_mode_batch_plan p;
 struct m9x_faa_depth_mode_state state;
 struct m9x_faa_depth_mode_image route;
 int ret=m9x_faa_depth_mode_batch_count(first,clear_flags,before,jobs,count,&p);
 if(ret)return ret;
 if(!owner||!views||!out)return -EINVAL;
 state=*before;
 for(u32 i=0;i<count;i++){
  ret=m9x_faa_depth_mode_two_plan(jobs[i].depth_flags?jobs[i].depth_flags:15,
    0,&state,owner,views,&route);if(ret)return ret;
  state=route.after;
 }
 *out=p;return 0;
}
#endif
