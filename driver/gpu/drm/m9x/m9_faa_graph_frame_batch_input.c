/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/errno.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "m9_faa_graph_frame_batch_input.h"

void m9x_faa_color_batch_free(struct m9x_faa_owned_color_batch *batch)
{
 u32 i;
 if(!batch)return;
 for(i=0;i<M9_FAA_FRAME_BATCH_MAX_DRAWS;i++)kfree(batch->draw[i].upload);
 kfree(batch);
}

int m9x_faa_color_batch_copy(const struct drm_parhelia_m9_faa_graph_color_frame_batch *args,
 struct m9x_faa_owned_color_batch **result)
{
 struct drm_parhelia_m9_faa_color_draw *desc=NULL;
 struct m9x_faa_owned_color_batch *batch=NULL;
 u32 i,j,bytes,sum,raw;int ret=-EINVAL;
 if(!result)return -EINVAL;
 *result=NULL;
 if(!args||!args->graph_id||!args->previous_point||(args->previous_point&15)||
  args->lock_point||!args->draws||!args->draw_count||
  args->draw_count>M9_FAA_FRAME_BATCH_MAX_DRAWS||
  args->flags&~(PARHELIA_M9_FAA_COLOR_FRAME_FIRST|PARHELIA_M9_FAA_COLOR_FRAME_INITIALIZED)||
  args->flags==(PARHELIA_M9_FAA_COLOR_FRAME_FIRST|PARHELIA_M9_FAA_COLOR_FRAME_INITIALIZED)||args->state||
  args->width!=320||args->height!=240||
  ((args->flags&PARHELIA_M9_FAA_COLOR_FRAME_INITIALIZED)?(args->clear_flags&&args->clear_flags!=3):args->clear_flags!=3)||args->clear_color||
  args->pad[0]||args->pad[1]||args->pad[2]||args->pad[3])return -EINVAL;
 static_assert(sizeof(*args)==80);
 static_assert(sizeof(*desc)==112);
 if(check_mul_overflow(args->draw_count,(u32)sizeof(*desc),&bytes))return -ERANGE;
 desc=memdup_user(u64_to_user_ptr(args->draws),bytes);
 if(IS_ERR(desc))return PTR_ERR(desc);
 batch=kzalloc(sizeof(*batch),GFP_KERNEL);if(!batch){ret=-ENOMEM;goto out;}
 batch->flags=args->flags;batch->clear_flags=args->clear_flags;
 for(i=0;i<args->draw_count;i++){
  const struct drm_parhelia_m9_faa_color_draw *d=&desc[i];
  const struct drm_parhelia_m9_faa_upload_layout *l=&d->layout;
  struct m9x_faa_owned_color_draw *job=&batch->draw[i];
  struct m9x_faa_color_input *input=&job->input;
  if(!d->upload||d->pad[0]||d->pad[1]||d->depth_flags!=7||(d->raster_flags&~7u)||(d->raster_flags&6)==6||
   d->record_stride!=32||!d->record_count||d->record_count%3||
   (d->declaration_count!=3&&d->declaration_count!=5)||l->program_count!=4||
   !l->program_bytes||l->program_bytes>4096||(l->program_bytes&7)||
   !l->constants_bytes||l->constants_bytes>16384||(l->constants_bytes&7)||
   !l->geometry_bytes||l->geometry_bytes>0x40000||l->vertex_offset!=128||
   check_mul_overflow(d->record_count,d->record_stride,&raw)||raw!=l->vertex_bytes||
   raw>l->geometry_bytes||l->vertex_offset>l->geometry_bytes-raw||
   check_add_overflow(l->program_bytes,l->constants_bytes,&sum)||
   check_add_overflow(sum,l->geometry_bytes,&sum)||
   check_add_overflow(sum,d->declaration_count*4u,&sum)||sum!=d->upload_bytes||sum>0x42000)goto out;
  for(j=0;j<4;j++)if((l->entry_bytes[j]&7)||l->entry_bytes[j]>=l->program_bytes||
   (j&&l->entry_bytes[j]<=l->entry_bytes[j-1])||
   (l->constant_begin[j]&7)||(l->constant_end[j]&7)||
   l->constant_begin[j]>l->constant_end[j]||l->constant_end[j]>l->constants_bytes)goto out;
  job->upload=memdup_user(u64_to_user_ptr(d->upload),sum);
  if(IS_ERR(job->upload)){ret=PTR_ERR(job->upload);job->upload=NULL;goto out;}
  memcpy(&input->layout,l,sizeof(*l));
  input->program=job->upload;input->constants=input->program+l->program_bytes;
  input->geometry=input->constants+l->constants_bytes;
  input->declaration=input->geometry+l->geometry_bytes;
  input->record_stride=d->record_stride;input->record_count=d->record_count;
  input->declaration_count=d->declaration_count;job->bytes=sum;job->raster_flags=d->raster_flags;
  /* Each job is independently shader/constant validated by the live
   * first-stage encoder. Preserve a common viewport and cull orientation;
   * raw declarations, flat/smooth and immutable code belong to each job. */
  if(i){const struct m9x_faa_color_input *first=&batch->draw[0].input;
   if((job->raster_flags&6)!=(batch->draw[0].raster_flags&6)||
    memcmp(input->geometry,first->geometry,128)){ret=-EOPNOTSUPP;goto out;}}
  batch->count++;
 }
 *result=batch;batch=NULL;ret=0;
out:kfree(desc);m9x_faa_color_batch_free(batch);return ret;
}
