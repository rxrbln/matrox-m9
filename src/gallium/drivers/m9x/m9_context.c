/* SPDX-License-Identifier: MIT */
#include "m9_screen.h"

#include <errno.h>
#include <stdio.h>

#include "m9_batch.h"
#include "m9_internal.h"
#include "m9_state.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"

struct m9_transfer {
   struct pipe_transfer base;
};

static uint64_t
m9_transfer_offset(const struct m9_resource *resource, unsigned level,
                   const struct pipe_box *box, unsigned *stride)
{
   if (resource->base.target == PIPE_BUFFER) {
      *stride = 0;
      return box->x;
   }
   if (resource->kind == M9_RESOURCE_LINEAR_TEXTURE) {
      const struct m9_mip_level *mip = &resource->mip.level[level];

      if (mip->in_tail) {
         *stride = 32 * resource->cpp;
         return mip->offset +
                ((uint64_t)(mip->tail_y + box->y) * 32 +
                 mip->tail_x + box->x) * resource->cpp;
      }
      *stride = mip->row_span * resource->cpp;
      return mip->offset +
             ((uint64_t)box->y * mip->row_span + box->x) * resource->cpp;
   }
   *stride = resource->row_span * resource->cpp;
   return ((uint64_t)box->y * resource->row_span + box->x) * resource->cpp;
}

static void *
m9_transfer_map(struct pipe_context *pctx, struct pipe_resource *pres,
                unsigned level, unsigned usage, const struct pipe_box *box,
                struct pipe_transfer **out)
{
   struct m9_resource *resource = (struct m9_resource *)pres;
   struct m9_transfer *transfer;
   uint64_t offset, last;
   unsigned width, height, stride;

   (void)pctx;
   if (!resource || !resource->map ||
       ((usage & PIPE_MAP_WRITE) && !resource->map_writable) ||
       !box || !out || box->x < 0 ||
       box->y < 0 || box->z || box->width < 0 || box->height < 0 ||
       box->depth != 1 || level > pres->last_level)
      return NULL;
   width = pres->target == PIPE_BUFFER ? pres->width0 :
           (pres->width0 >> level ? pres->width0 >> level : 1);
   height = pres->target == PIPE_BUFFER ? 1 :
            (pres->height0 >> level ? pres->height0 >> level : 1);
   if ((unsigned)box->x > width || (unsigned)box->width > width - box->x ||
       (unsigned)box->y > height || (unsigned)box->height > height - box->y)
      return NULL;
   offset = m9_transfer_offset(resource, level, box, &stride);
   last = offset;
   if (pres->target == PIPE_BUFFER)
      last += box->width;
   else if (box->width && box->height)
      last += (uint64_t)(box->height - 1) * stride +
              (uint64_t)box->width * resource->cpp;
   if (last > resource->size)
      return NULL;
   transfer = CALLOC_STRUCT(m9_transfer);
   if (!transfer)
      return NULL;
   pipe_resource_reference(&transfer->base.resource, pres);
   transfer->base.level = level;
   transfer->base.usage = usage;
   transfer->base.box = *box;
   transfer->base.stride = stride;
   transfer->base.layer_stride = pres->target == PIPE_BUFFER ? 0 :
                                 (uintptr_t)stride * height;
   *out = &transfer->base;
   return (uint8_t *)resource->map + offset;
}

static void
m9_transfer_unmap(struct pipe_context *pctx, struct pipe_transfer *transfer)
{
   (void)pctx;
   if (transfer->usage & PIPE_MAP_WRITE)
      __sync_synchronize();
   pipe_resource_reference(&transfer->resource, NULL);
   FREE(transfer);
}

static void
m9_context_flush(struct pipe_context *pctx,
                 struct pipe_fence_handle **out_fence, unsigned flags)
{
   struct m9_context *context = (struct m9_context *)pctx;
   struct pipe_fence_handle *fence = NULL;
   uint64_t point = context->last_point;
   int ret = 0;

   (void)flags;
   if (context->batch.command_count) {
      if (context->lost) {
         ret = -EIO;
      } else {
         context->batch.sealed = true;
         ret = m9_batch_submit(context->screen, &context->command,
                               &context->batch, context->context_id, &point);
         if (ret)
            context->lost = true;
         else
            context->last_point = point;
      }
      m9_batch_init(&context->batch);
      context->dirty = M9_DIRTY_ALL;
   }
   if (out_fence) {
      if (!ret)
         fence = m9_fence_create(point, true);
      pctx->screen->fence_reference(pctx->screen, out_fence, fence);
      if (fence)
         pctx->screen->fence_reference(pctx->screen, &fence, NULL);
   }
}

static void
m9_context_destroy(struct pipe_context *pctx)
{
   struct m9_context *context = (struct m9_context *)pctx;

   m9_context_flush(pctx, NULL, 0);
   m9_context_state_cleanup(context);
   m9_command_bo_destroy(context->screen, &context->command);
   FREE(context->vertex_stream.vertices);
   FREE(context);
}

struct pipe_context *
m9_context_create(struct pipe_screen *pscreen, void *priv, unsigned flags)
{
   struct m9_screen *screen = (struct m9_screen *)pscreen;
   struct m9_context *context;

   if (flags & PIPE_CONTEXT_COMPUTE_ONLY)
      return NULL;
   /* Every lower-level lifetime and submit operation is wired below this
    * gate.  The remaining linked-program record contains process-local host
    * pointers in the only captured fixed-function slot, so accepting a GL
    * context before a clean constructor exists would submit stale addresses. */
   if (!screen->execute_submit || !screen->linked_program_ready) {
      fprintf(stderr,
              "m9x: rendering awaits executable submit and linked-program constructor\n");
      return NULL;
   }
   context = CALLOC_STRUCT(m9_context);
   if (!context)
      return NULL;
   m9_context_state_init(context, screen);
   context->base.priv = priv;
   context->base.destroy = m9_context_destroy;
   context->base.flush = m9_context_flush;
   context->base.buffer_map = m9_transfer_map;
   context->base.buffer_unmap = m9_transfer_unmap;
   context->base.texture_map = m9_transfer_map;
   context->base.texture_unmap = m9_transfer_unmap;
   if (m9_command_bo_create(screen, &context->command)) {
      FREE(context);
      return NULL;
   }
   return &context->base;
}
