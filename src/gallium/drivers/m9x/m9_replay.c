/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include "m9_replay.h"
#include "m9_internal.h"

#include "util/format/u_format.h"
#include "util/u_dynarray.h"
#include "util/u_framebuffer.h"
#include "util/u_helpers.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_debug.h"

/* Stages the driver implements: vertex and fragment. */
#define M9_REPLAY_STAGES 2
#define M9_REPLAY_VIEWS PIPE_MAX_SAMPLERS

enum m9_replay_seen {
   SEEN_VIEWPORT = 1u << 0,
   SEEN_SCISSOR = 1u << 1,
   SEEN_CLIP = 1u << 2,
   SEEN_BLEND_COLOR = 1u << 3,
   SEEN_STENCIL_REF = 1u << 4,
   SEEN_SAMPLE_MASK = 1u << 5,
   SEEN_STIPPLE = 1u << 6,
   SEEN_FRAMEBUFFER = 1u << 7,
};

/* Bound state the driver reads at draw time, apart from the vertex and
 * constant buffers, which every recorded draw snapshots itself. */
struct m9_replay_state {
   void *vs, *fs, *velems, *blend, *dsa, *rast;
   void *samplers[M9_REPLAY_STAGES][PIPE_MAX_SAMPLERS];
   struct pipe_sampler_view *views[M9_REPLAY_STAGES][M9_REPLAY_VIEWS];
   struct pipe_framebuffer_state fb;
   struct pipe_viewport_state viewport[PIPE_MAX_VIEWPORTS];
   struct pipe_scissor_state scissor[PIPE_MAX_VIEWPORTS];
   struct pipe_clip_state clip;
   struct pipe_blend_color blend_color;
   struct pipe_stencil_ref stencil_ref;
   unsigned sample_mask;
   struct pipe_poly_stipple stipple;
   uint32_t inlined[M9_REPLAY_STAGES][MAX_INLINABLE_UNIFORMS];
   unsigned inlined_count[M9_REPLAY_STAGES];
   unsigned seen;
};

enum m9_replay_kind { REPLAY_DRAW, REPLAY_CLEAR };

struct m9_replay_op {
   enum m9_replay_kind kind;
   unsigned state;            /* index into states */
   /* REPLAY_DRAW */
   struct pipe_draw_info info;
   unsigned drawid_offset, num_draws;
   struct pipe_draw_start_count_bias *draws;
   struct pipe_vertex_buffer vb[PIPE_MAX_ATTRIBS];
   unsigned num_vb;
   struct pipe_constant_buffer cb[M9_REPLAY_STAGES][PIPE_MAX_CONSTANT_BUFFERS];
   /* REPLAY_CLEAR */
   unsigned buffers;
   uint32_t color_clear_mask;
   uint8_t stencil_clear_mask;
   bool scissored;
   struct pipe_scissor_state scissor;
   union pipe_color_union color;
   double depth;
   unsigned stencil;
};

struct m9_replay_hooks {
   void (*bind_vs)(struct pipe_context *, void *);
   void (*bind_fs)(struct pipe_context *, void *);
   void (*bind_velems)(struct pipe_context *, void *);
   void (*bind_blend)(struct pipe_context *, void *);
   void (*bind_dsa)(struct pipe_context *, void *);
   void (*bind_rast)(struct pipe_context *, void *);
   void (*bind_samplers)(struct pipe_context *, mesa_shader_stage, unsigned, unsigned, void **);
   void (*set_views)(struct pipe_context *, mesa_shader_stage, unsigned, unsigned, unsigned,
                     struct pipe_sampler_view **);
   void (*set_fb)(struct pipe_context *, const struct pipe_framebuffer_state *);
   void (*set_viewports)(struct pipe_context *, unsigned, unsigned, const struct pipe_viewport_state *);
   void (*set_scissors)(struct pipe_context *, unsigned, unsigned, const struct pipe_scissor_state *);
   void (*set_clip)(struct pipe_context *, const struct pipe_clip_state *);
   void (*set_blend_color)(struct pipe_context *, const struct pipe_blend_color *);
   void (*set_stencil_ref)(struct pipe_context *, const struct pipe_stencil_ref);
   void (*set_sample_mask)(struct pipe_context *, unsigned);
   void (*set_stipple)(struct pipe_context *, const struct pipe_poly_stipple *);
   void (*set_inlinable)(struct pipe_context *, mesa_shader_stage, uint, uint32_t *);
   void (*set_vbs)(struct pipe_context *, unsigned, const struct pipe_vertex_buffer *);
   void (*set_cb)(struct pipe_context *, mesa_shader_stage, uint, const struct pipe_constant_buffer *);
   void (*delete_vs)(struct pipe_context *, void *);
   void (*delete_fs)(struct pipe_context *, void *);
   void (*delete_velems)(struct pipe_context *, void *);
   void (*delete_blend)(struct pipe_context *, void *);
   void (*delete_dsa)(struct pipe_context *, void *);
   void (*delete_rast)(struct pipe_context *, void *);
   void (*delete_sampler)(struct pipe_context *, void *);
   void (*blit)(struct pipe_context *, const struct pipe_blit_info *);
   void (*copy_region)(struct pipe_context *, struct pipe_resource *, unsigned, unsigned,
                       unsigned, unsigned, struct pipe_resource *, unsigned,
                       const struct pipe_box *);
   void (*clear_rt)(struct pipe_context *, struct pipe_surface *, const union pipe_color_union *,
                    unsigned, unsigned, unsigned, unsigned, bool);
   void (*clear_ds)(struct pipe_context *, struct pipe_surface *, unsigned, double, unsigned,
                    unsigned, unsigned, unsigned, unsigned, bool);
   void (*flush_resource)(struct pipe_context *, struct pipe_resource *);
};

struct m9_replay_deferred {
   void (*destroy)(struct pipe_context *, void *);
   void *cso;
};

struct m9_replay {
   struct m9_replay_hooks hooks;
   struct m9_replay_state live;
   /* Live vertex and constant buffers, restored after a replay. */
   struct pipe_vertex_buffer vb[PIPE_MAX_ATTRIBS];
   unsigned num_vb;
   struct pipe_constant_buffer cb[M9_REPLAY_STAGES][PIPE_MAX_CONSTANT_BUFFERS];
   uint8_t *cb_copy[M9_REPLAY_STAGES][PIPE_MAX_CONSTANT_BUFFERS];
   bool recording, replaying, dirty;
   struct util_dynarray states;     /* struct m9_replay_state */
   struct util_dynarray ops;        /* struct m9_replay_op */
   struct util_dynarray snapshots;  /* malloc'd blocks */
   struct util_dynarray deferred;   /* struct m9_replay_deferred */
};

static inline struct m9_replay *
replay_of(struct pipe_context *pctx)
{
   return ((struct m9_context *)pctx)->replay;
}

static int
stage_index(mesa_shader_stage stage)
{
   return stage == MESA_SHADER_VERTEX ? 0 : stage == MESA_SHADER_FRAGMENT ? 1 : -1;
}

static mesa_shader_stage
stage_of(unsigned i)
{
   return i ? MESA_SHADER_FRAGMENT : MESA_SHADER_VERTEX;
}

bool
m9_replay_enabled(void)
{
   return !m9_faa_shadow_enabled();
}

bool
m9_replay_recording(const struct m9_context *ctx)
{
   return ctx->replay && ctx->replay->recording;
}

bool
m9_replay_replaying(const struct m9_context *ctx)
{
   return ctx->replay && ctx->replay->replaying;
}

/* State copies hold their own references to views and surfaces. */
static void
state_copy(struct m9_replay_state *dst, const struct m9_replay_state *src)
{
   struct pipe_framebuffer_state fb = dst->fb;
   struct pipe_sampler_view *views[M9_REPLAY_STAGES][M9_REPLAY_VIEWS];

   memcpy(views, dst->views, sizeof(views));
   *dst = *src;
   dst->fb = fb;
   memcpy(dst->views, views, sizeof(views));
   util_copy_framebuffer_state(&dst->fb, &src->fb);
   for (unsigned s = 0; s < M9_REPLAY_STAGES; s++)
      for (unsigned i = 0; i < M9_REPLAY_VIEWS; i++)
         pipe_sampler_view_reference(&dst->views[s][i], src->views[s][i]);
}

static void
state_release(struct m9_replay_state *state)
{
   util_unreference_framebuffer_state(&state->fb);
   for (unsigned s = 0; s < M9_REPLAY_STAGES; s++)
      for (unsigned i = 0; i < M9_REPLAY_VIEWS; i++)
         pipe_sampler_view_reference(&state->views[s][i], NULL);
}

static void
state_apply(struct pipe_context *pctx, const struct m9_replay_hooks *h,
            const struct m9_replay_state *s)
{
   if (s->seen & SEEN_FRAMEBUFFER)
      h->set_fb(pctx, &s->fb);
   h->bind_rast(pctx, s->rast);
   h->bind_dsa(pctx, s->dsa);
   h->bind_blend(pctx, s->blend);
   h->bind_velems(pctx, s->velems);
   h->bind_vs(pctx, s->vs);
   h->bind_fs(pctx, s->fs);
   for (unsigned i = 0; i < M9_REPLAY_STAGES; i++) {
      void *samplers[PIPE_MAX_SAMPLERS];
      struct pipe_sampler_view *views[M9_REPLAY_VIEWS];

      memcpy(samplers, s->samplers[i], sizeof(samplers));
      memcpy(views, s->views[i], sizeof(views));
      h->bind_samplers(pctx, stage_of(i), 0, PIPE_MAX_SAMPLERS, samplers);
      h->set_views(pctx, stage_of(i), 0, M9_REPLAY_VIEWS, 0, views);
      if (s->inlined_count[i]) {
         uint32_t values[MAX_INLINABLE_UNIFORMS];

         memcpy(values, s->inlined[i], sizeof(values));
         h->set_inlinable(pctx, stage_of(i), s->inlined_count[i], values);
      }
   }
   if (s->seen & SEEN_VIEWPORT)
      h->set_viewports(pctx, 0, PIPE_MAX_VIEWPORTS, s->viewport);
   if (s->seen & SEEN_SCISSOR)
      h->set_scissors(pctx, 0, PIPE_MAX_VIEWPORTS, s->scissor);
   if (s->seen & SEEN_CLIP)
      h->set_clip(pctx, &s->clip);
   if (s->seen & SEEN_BLEND_COLOR)
      h->set_blend_color(pctx, &s->blend_color);
   if (s->seen & SEEN_STENCIL_REF)
      h->set_stencil_ref(pctx, s->stencil_ref);
   if (s->seen & SEEN_SAMPLE_MASK)
      h->set_sample_mask(pctx, s->sample_mask);
   if (s->seen & SEEN_STIPPLE)
      h->set_stipple(pctx, &s->stipple);
}

static void *
snapshot(struct m9_replay *r, const void *data, size_t bytes)
{
   void *copy = malloc(MAX2(bytes, 1));

   if (!copy)
      return NULL;
   if (bytes)
      memcpy(copy, data, bytes);
   util_dynarray_append_typed(&r->snapshots, void *, copy);
   return copy;
}

static void
release_recording(struct pipe_context *pctx, struct m9_replay *r)
{
   /* Recorded vertex buffers and indices are owned snapshots. */
   util_dynarray_foreach(&r->states, struct m9_replay_state, s)
      state_release(s);
   util_dynarray_foreach(&r->snapshots, void *, p)
      free(*p);
   util_dynarray_clear(&r->ops);
   util_dynarray_clear(&r->states);
   util_dynarray_clear(&r->snapshots);
   r->recording = false;
   /* CSOs the state tracker deleted while recorded draws named them. */
   util_dynarray_foreach(&r->deferred, struct m9_replay_deferred, d)
      d->destroy(pctx, d->cso);
   util_dynarray_clear(&r->deferred);
}

/* Settle a recorded FAA frame before an operation that reads or writes
 * what its draws reference. */
static void
settle(struct pipe_context *pctx)
{
   struct m9_replay *r = replay_of(pctx);

   if (r->recording && !r->replaying)
      m9_context_faa_settle(((struct m9_context *)pctx));
}

/* ---- wrapped state hooks: track the live state, mark recordings dirty ---- */

#define BIND_WRAPPER(name, field)                                         \
static void                                                               \
wrap_##name(struct pipe_context *pctx, void *cso)                         \
{                                                                         \
   struct m9_replay *r = replay_of(pctx);                                 \
   r->live.field = cso;                                                   \
   r->dirty = true;                                                       \
   r->hooks.name(pctx, cso);                                              \
}
BIND_WRAPPER(bind_vs, vs)
BIND_WRAPPER(bind_fs, fs)
BIND_WRAPPER(bind_velems, velems)
BIND_WRAPPER(bind_blend, blend)
BIND_WRAPPER(bind_dsa, dsa)
BIND_WRAPPER(bind_rast, rast)

#define DELETE_WRAPPER(name)                                              \
static void                                                               \
wrap_##name(struct pipe_context *pctx, void *cso)                         \
{                                                                         \
   struct m9_replay *r = replay_of(pctx);                                 \
   if (r->recording) {                                                    \
      struct m9_replay_deferred d = { r->hooks.name, cso };               \
      util_dynarray_append_typed(&r->deferred, struct m9_replay_deferred, d);   \
      return;                                                             \
   }                                                                      \
   r->hooks.name(pctx, cso);                                              \
}
DELETE_WRAPPER(delete_vs)
DELETE_WRAPPER(delete_fs)
DELETE_WRAPPER(delete_velems)
DELETE_WRAPPER(delete_blend)
DELETE_WRAPPER(delete_dsa)
DELETE_WRAPPER(delete_rast)
DELETE_WRAPPER(delete_sampler)

static void
wrap_bind_samplers(struct pipe_context *pctx, mesa_shader_stage stage,
                   unsigned start, unsigned num, void **samplers)
{
   struct m9_replay *r = replay_of(pctx);
   int s = stage_index(stage);

   for (unsigned i = 0; s >= 0 && i < num && start + i < PIPE_MAX_SAMPLERS; i++)
      r->live.samplers[s][start + i] = samplers ? samplers[i] : NULL;
   r->dirty = true;
   r->hooks.bind_samplers(pctx, stage, start, num, samplers);
}

static void
wrap_set_views(struct pipe_context *pctx, mesa_shader_stage stage,
               unsigned start, unsigned num, unsigned unbind_trailing,
               struct pipe_sampler_view **views)
{
   struct m9_replay *r = replay_of(pctx);
   int s = stage_index(stage);

   for (unsigned i = 0; s >= 0 && i < num + unbind_trailing &&
                        start + i < M9_REPLAY_VIEWS; i++)
      pipe_sampler_view_reference(&r->live.views[s][start + i],
                                  views && i < num ? views[i] : NULL);
   r->dirty = true;
   r->hooks.set_views(pctx, stage, start, num, unbind_trailing, views);
}

static void
wrap_set_fb(struct pipe_context *pctx, const struct pipe_framebuffer_state *fb)
{
   struct m9_replay *r = replay_of(pctx);

   util_copy_framebuffer_state(&r->live.fb, fb);
   r->live.seen |= SEEN_FRAMEBUFFER;
   r->dirty = true;
   r->hooks.set_fb(pctx, fb);
}

static void
wrap_set_viewports(struct pipe_context *pctx, unsigned start, unsigned num,
                   const struct pipe_viewport_state *v)
{
   struct m9_replay *r = replay_of(pctx);

   for (unsigned i = 0; i < num && start + i < PIPE_MAX_VIEWPORTS; i++)
      r->live.viewport[start + i] = v[i];
   r->live.seen |= SEEN_VIEWPORT;
   r->dirty = true;
   r->hooks.set_viewports(pctx, start, num, v);
}

static void
wrap_set_scissors(struct pipe_context *pctx, unsigned start, unsigned num,
                  const struct pipe_scissor_state *v)
{
   struct m9_replay *r = replay_of(pctx);

   for (unsigned i = 0; i < num && start + i < PIPE_MAX_VIEWPORTS; i++)
      r->live.scissor[start + i] = v[i];
   r->live.seen |= SEEN_SCISSOR;
   r->dirty = true;
   r->hooks.set_scissors(pctx, start, num, v);
}

#define SET_WRAPPER(name, type, field, bit)                               \
static void                                                               \
wrap_##name(struct pipe_context *pctx, const type *v)                     \
{                                                                         \
   struct m9_replay *r = replay_of(pctx);                                 \
   r->live.field = *v;                                                    \
   r->live.seen |= bit;                                                   \
   r->dirty = true;                                                       \
   r->hooks.name(pctx, v);                                                \
}
SET_WRAPPER(set_clip, struct pipe_clip_state, clip, SEEN_CLIP)
SET_WRAPPER(set_blend_color, struct pipe_blend_color, blend_color, SEEN_BLEND_COLOR)
SET_WRAPPER(set_stipple, struct pipe_poly_stipple, stipple, SEEN_STIPPLE)

static void
wrap_set_stencil_ref(struct pipe_context *pctx, const struct pipe_stencil_ref ref)
{
   struct m9_replay *r = replay_of(pctx);

   r->live.stencil_ref = ref;
   r->live.seen |= SEEN_STENCIL_REF;
   r->dirty = true;
   r->hooks.set_stencil_ref(pctx, ref);
}

static void
wrap_set_sample_mask(struct pipe_context *pctx, unsigned mask)
{
   struct m9_replay *r = replay_of(pctx);

   r->live.sample_mask = mask;
   r->live.seen |= SEEN_SAMPLE_MASK;
   r->dirty = true;
   r->hooks.set_sample_mask(pctx, mask);
}

static void
wrap_set_inlinable(struct pipe_context *pctx, mesa_shader_stage stage,
                   uint num, uint32_t *values)
{
   struct m9_replay *r = replay_of(pctx);
   int s = stage_index(stage);

   if (s >= 0) {
      r->live.inlined_count[s] = MIN2(num, MAX_INLINABLE_UNIFORMS);
      memcpy(r->live.inlined[s], values, r->live.inlined_count[s] * sizeof(*values));
   }
   r->dirty = true;
   r->hooks.set_inlinable(pctx, stage, num, values);
}

static void
wrap_set_vbs(struct pipe_context *pctx, unsigned count,
             const struct pipe_vertex_buffer *buffers)
{
   struct m9_replay *r = replay_of(pctx);

   util_set_vertex_buffers_count(r->vb, &r->num_vb, buffers, count);
   r->num_vb = count;
   r->hooks.set_vbs(pctx, count, buffers);
}

static void
wrap_set_cb(struct pipe_context *pctx, mesa_shader_stage stage, uint index,
            const struct pipe_constant_buffer *cb)
{
   struct m9_replay *r = replay_of(pctx);
   int s = stage_index(stage);

   if (s >= 0 && index < PIPE_MAX_CONSTANT_BUFFERS) {
      struct pipe_constant_buffer *dst = &r->cb[s][index];

      /* A user buffer is only valid during this call: keep a copy. */
      free(r->cb_copy[s][index]);
      r->cb_copy[s][index] = NULL;
      util_copy_constant_buffer(dst, cb);
      if (cb && cb->user_buffer) {
         r->cb_copy[s][index] = malloc(MAX2(cb->buffer_size, 1));
         if (r->cb_copy[s][index])
            memcpy(r->cb_copy[s][index], cb->user_buffer, cb->buffer_size);
         dst->user_buffer = r->cb_copy[s][index];
      }
   }
   r->hooks.set_cb(pctx, stage, index, cb);
}

static void
wrap_blit(struct pipe_context *pctx, const struct pipe_blit_info *info)
{
   settle(pctx);
   replay_of(pctx)->hooks.blit(pctx, info);
}

static void
wrap_copy_region(struct pipe_context *pctx, struct pipe_resource *dst,
                 unsigned dst_level, unsigned dstx, unsigned dsty, unsigned dstz,
                 struct pipe_resource *src, unsigned src_level,
                 const struct pipe_box *box)
{
   settle(pctx);
   replay_of(pctx)->hooks.copy_region(pctx, dst, dst_level, dstx, dsty, dstz,
                                      src, src_level, box);
}

static void
wrap_clear_rt(struct pipe_context *pctx, struct pipe_surface *dst,
              const union pipe_color_union *color, unsigned x, unsigned y,
              unsigned w, unsigned h, bool render_condition)
{
   settle(pctx);
   replay_of(pctx)->hooks.clear_rt(pctx, dst, color, x, y, w, h, render_condition);
}

static void
wrap_clear_ds(struct pipe_context *pctx, struct pipe_surface *dst,
              unsigned flags, double depth, unsigned stencil, unsigned x,
              unsigned y, unsigned w, unsigned h, bool render_condition)
{
   settle(pctx);
   replay_of(pctx)->hooks.clear_ds(pctx, dst, flags, depth, stencil, x, y, w, h,
                                   render_condition);
}

static void
wrap_flush_resource(struct pipe_context *pctx, struct pipe_resource *res)
{
   settle(pctx);
   replay_of(pctx)->hooks.flush_resource(pctx, res);
}

/* ---- recording ---- */

static bool
begin_op(struct m9_replay *r, struct m9_replay_op *op)
{
   memset(op, 0, sizeof(*op));
   if (!r->recording || r->dirty || !util_dynarray_num_elements(&r->states, struct m9_replay_state)) {
      struct m9_replay_state *s = util_dynarray_grow(&r->states, struct m9_replay_state, 1);

      if (!s)
         return false;
      memset(s, 0, sizeof(*s));
      state_copy(s, &r->live);
      r->dirty = false;
      r->recording = true;
   }
   op->state = util_dynarray_num_elements(&r->states, struct m9_replay_state) - 1;
   return true;
}

static const uint8_t *
buffer_data(const struct pipe_vertex_buffer *vb)
{
   if (vb->is_user_buffer)
      return vb->buffer.user;
   return vb->buffer.resource ? ((struct m9_resource *)vb->buffer.resource)->map : NULL;
}

static void
index_bounds(const void *indices, unsigned size, unsigned count, bool restart,
             unsigned restart_index, uint32_t *lo, uint32_t *hi)
{
   *lo = UINT32_MAX;
   *hi = 0;
   for (unsigned i = 0; i < count; i++) {
      uint32_t v = size == 1 ? ((const uint8_t *)indices)[i] :
                   size == 2 ? ((const uint16_t *)indices)[i] :
                               ((const uint32_t *)indices)[i];

      if (restart && v == restart_index)
         continue;
      *lo = MIN2(*lo, v);
      *hi = MAX2(*hi, v);
   }
}

int
m9_replay_record_draw(struct m9_context *ctx, const struct pipe_draw_info *info,
                      unsigned drawid_offset,
                      const struct pipe_draw_start_count_bias *draws,
                      unsigned num_draws)
{
   struct m9_replay *r = ctx->replay;
   struct m9_replay_op op;
   unsigned element_count = 0;
   const struct pipe_vertex_element *elements =
      m9_context_vertex_elements(ctx, &element_count);
   int64_t vlo = INT64_MAX, vhi = INT64_MIN;

   if (!r || r->replaying || !num_draws || !begin_op(r, &op))
      return -ENOMEM;
   op.kind = REPLAY_DRAW;
   op.info = *info;
   op.drawid_offset = drawid_offset;
   op.num_draws = num_draws;
   op.draws = snapshot(r, draws, num_draws * sizeof(*draws));
   if (!op.draws)
      return -ENOMEM;
   /* Indices and the vertex range they reach. */
   for (unsigned d = 0; d < num_draws; d++) {
      int64_t lo = draws[d].start, hi = (int64_t)draws[d].start + draws[d].count - 1;

      if (!draws[d].count)
         continue;
      if (info->index_size) {
         const uint8_t *base = info->has_user_indices ? info->index.user :
                               info->index.resource ? ((struct m9_resource *)info->index.resource)->map : NULL;
         uint32_t ilo, ihi;

         if (!base)
            return -EINVAL;
         index_bounds(base + (size_t)draws[d].start * info->index_size,
                      info->index_size, draws[d].count, info->primitive_restart,
                      info->restart_index, &ilo, &ihi);
         if (ilo > ihi)
            continue;
         lo = (int64_t)ilo + draws[d].index_bias;
         hi = (int64_t)ihi + draws[d].index_bias;
      }
      vlo = MIN2(vlo, lo);
      vhi = MAX2(vhi, hi);
   }
   if (info->index_size) {
      /* One copy of the whole index run each draw names. */
      uint32_t first = UINT32_MAX, last = 0;
      const uint8_t *base = info->has_user_indices ? info->index.user :
                            ((struct m9_resource *)info->index.resource)->map;

      for (unsigned d = 0; d < num_draws; d++) {
         first = MIN2(first, draws[d].start);
         last = MAX2(last, draws[d].start + draws[d].count);
      }
      if (first > last)
         first = last = 0;
      uint8_t *copy = snapshot(r, base + (size_t)first * info->index_size,
                               (size_t)(last - first) * info->index_size);

      if (!copy)
         return -ENOMEM;
      op.info.has_user_indices = true;
      op.info.index.user = copy - (size_t)first * info->index_size;
   }
   /* The bytes of every vertex buffer the elements read in that range. */
   op.num_vb = r->num_vb;
   for (unsigned b = 0; b < r->num_vb; b++) {
      const struct pipe_vertex_buffer *vb = &r->vb[b];
      const uint8_t *data = buffer_data(vb);
      int64_t lo = INT64_MAX, hi = INT64_MIN;

      if (!data)
         continue;
      for (unsigned e = 0; e < element_count; e++) {
         const struct pipe_vertex_element *el = &elements[e];
         int64_t first = vlo, last = vhi;

         if (el->vertex_buffer_index != b)
            continue;
         if (el->instance_divisor) {
            first = info->start_instance;
            last = info->start_instance +
                   DIV_ROUND_UP(MAX2(info->instance_count, 1), el->instance_divisor) - 1;
         }
         if (first > last)
            continue;
         lo = MIN2(lo, (int64_t)vb->buffer_offset + el->src_offset + first * el->src_stride);
         hi = MAX2(hi, (int64_t)vb->buffer_offset + el->src_offset + last * el->src_stride +
                       util_format_get_blocksize(el->src_format));
      }
      if (lo >= hi || lo < 0)
         continue;
      uint8_t *copy = snapshot(r, data + lo, hi - lo);

      if (!copy)
         return -ENOMEM;
      /* buffer.user addresses byte 0 of the original buffer. */
      op.vb[b].is_user_buffer = true;
      op.vb[b].buffer_offset = vb->buffer_offset;
      op.vb[b].buffer.user = copy - lo;
   }
   /* Constant buffers as bound for this draw. */
   for (unsigned s = 0; s < M9_REPLAY_STAGES; s++)
      for (unsigned i = 0; i < PIPE_MAX_CONSTANT_BUFFERS; i++) {
         const struct pipe_constant_buffer *cb = &r->cb[s][i];
         const uint8_t *data = cb->user_buffer ? cb->user_buffer :
                               cb->buffer ? (const uint8_t *)((struct m9_resource *)cb->buffer)->map +
                                            cb->buffer_offset : NULL;

         if (!data || !cb->buffer_size)
            continue;
         op.cb[s][i].buffer_size = cb->buffer_size;
         op.cb[s][i].user_buffer = snapshot(r, data, cb->buffer_size);
         if (!op.cb[s][i].user_buffer)
            return -ENOMEM;
      }
   util_dynarray_append_typed(&r->ops, struct m9_replay_op, op);
   return 0;
}

int
m9_replay_record_clear(struct m9_context *ctx, unsigned buffers,
                       uint32_t color_clear_mask, uint8_t stencil_clear_mask,
                       const struct pipe_scissor_state *scissor,
                       const union pipe_color_union *color,
                       double depth, unsigned stencil)
{
   struct m9_replay *r = ctx->replay;
   struct m9_replay_op op;

   if (!r || r->replaying || !begin_op(r, &op))
      return -ENOMEM;
   op.kind = REPLAY_CLEAR;
   op.buffers = buffers;
   op.color_clear_mask = color_clear_mask;
   op.stencil_clear_mask = stencil_clear_mask;
   op.scissored = scissor != NULL;
   if (scissor)
      op.scissor = *scissor;
   if (color)
      op.color = *color;
   op.depth = depth;
   op.stencil = stencil;
   util_dynarray_append_typed(&r->ops, struct m9_replay_op, op);
   return 0;
}

void
m9_replay_discard(struct m9_context *ctx)
{
   if (ctx->replay && ctx->replay->recording)
      release_recording(&ctx->base, ctx->replay);
}

unsigned
m9_replay_materialize(struct m9_context *ctx)
{
   struct m9_replay *r = ctx->replay;
   struct pipe_context *pctx = &ctx->base;
   const struct m9_replay_hooks *h;
   unsigned count, applied = UINT_MAX;

   if (!r || !r->recording || r->replaying)
      return 0;
   h = &r->hooks;
   count = util_dynarray_num_elements(&r->ops, struct m9_replay_op);
   r->replaying = true;
   util_dynarray_foreach(&r->ops, struct m9_replay_op, op) {
      if (op->state != applied) {
         state_apply(pctx, h, util_dynarray_element(&r->states, struct m9_replay_state,
                                                    op->state));
         applied = op->state;
      }
      if (op->kind == REPLAY_CLEAR) {
         pctx->clear(pctx, op->buffers, op->color_clear_mask, op->stencil_clear_mask,
                     op->scissored ? &op->scissor : NULL, &op->color, op->depth,
                     op->stencil);
         continue;
      }
      h->set_vbs(pctx, op->num_vb, op->vb);
      for (unsigned s = 0; s < M9_REPLAY_STAGES; s++)
         for (unsigned i = 0; i < PIPE_MAX_CONSTANT_BUFFERS; i++)
            if (op->cb[s][i].user_buffer || r->cb[s][i].buffer || r->cb[s][i].user_buffer)
               h->set_cb(pctx, stage_of(s), i,
                         op->cb[s][i].user_buffer ? &op->cb[s][i] : NULL);
      pctx->draw_vbo(pctx, &op->info, op->drawid_offset, NULL, op->draws, op->num_draws);
   }
   /* Back to the live state the application set. */
   state_apply(pctx, h, &r->live);
   h->set_vbs(pctx, r->num_vb, r->vb);
   for (unsigned s = 0; s < M9_REPLAY_STAGES; s++)
      for (unsigned i = 0; i < PIPE_MAX_CONSTANT_BUFFERS; i++)
         h->set_cb(pctx, stage_of(s), i,
                   r->cb[s][i].buffer || r->cb[s][i].user_buffer ? &r->cb[s][i] : NULL);
   r->replaying = false;
   release_recording(pctx, r);
   return count;
}

void
m9_replay_init(struct m9_context *ctx)
{
   struct pipe_context *p = &ctx->base;
   struct m9_replay *r = CALLOC_STRUCT(m9_replay);

   if (!r)
      return;
   util_dynarray_init(&r->states, NULL);
   util_dynarray_init(&r->ops, NULL);
   util_dynarray_init(&r->snapshots, NULL);
   util_dynarray_init(&r->deferred, NULL);
   r->hooks = (struct m9_replay_hooks){
      p->bind_vs_state, p->bind_fs_state, p->bind_vertex_elements_state,
      p->bind_blend_state, p->bind_depth_stencil_alpha_state, p->bind_rasterizer_state,
      p->bind_sampler_states, p->set_sampler_views, p->set_framebuffer_state,
      p->set_viewport_states, p->set_scissor_states, p->set_clip_state,
      p->set_blend_color, p->set_stencil_ref, p->set_sample_mask,
      p->set_polygon_stipple, p->set_inlinable_constants, p->set_vertex_buffers,
      p->set_constant_buffer, p->delete_vs_state, p->delete_fs_state,
      p->delete_vertex_elements_state, p->delete_blend_state,
      p->delete_depth_stencil_alpha_state, p->delete_rasterizer_state,
      p->delete_sampler_state, p->blit, p->resource_copy_region,
      p->clear_render_target, p->clear_depth_stencil, p->flush_resource,
   };
   ctx->replay = r;
   p->bind_vs_state = wrap_bind_vs;
   p->bind_fs_state = wrap_bind_fs;
   p->bind_vertex_elements_state = wrap_bind_velems;
   p->bind_blend_state = wrap_bind_blend;
   p->bind_depth_stencil_alpha_state = wrap_bind_dsa;
   p->bind_rasterizer_state = wrap_bind_rast;
   p->bind_sampler_states = wrap_bind_samplers;
   p->set_sampler_views = wrap_set_views;
   p->set_framebuffer_state = wrap_set_fb;
   p->set_viewport_states = wrap_set_viewports;
   p->set_scissor_states = wrap_set_scissors;
   p->set_clip_state = wrap_set_clip;
   p->set_blend_color = wrap_set_blend_color;
   p->set_stencil_ref = wrap_set_stencil_ref;
   p->set_sample_mask = wrap_set_sample_mask;
   p->set_polygon_stipple = wrap_set_stipple;
   p->set_inlinable_constants = wrap_set_inlinable;
   p->set_vertex_buffers = wrap_set_vbs;
   p->set_constant_buffer = wrap_set_cb;
   p->delete_vs_state = wrap_delete_vs;
   p->delete_fs_state = wrap_delete_fs;
   p->delete_vertex_elements_state = wrap_delete_velems;
   p->delete_blend_state = wrap_delete_blend;
   p->delete_depth_stencil_alpha_state = wrap_delete_dsa;
   p->delete_rasterizer_state = wrap_delete_rast;
   p->delete_sampler_state = wrap_delete_sampler;
   p->blit = wrap_blit;
   p->resource_copy_region = wrap_copy_region;
   p->clear_render_target = wrap_clear_rt;
   p->clear_depth_stencil = wrap_clear_ds;
   p->flush_resource = wrap_flush_resource;
}

void
m9_replay_fini(struct m9_context *ctx)
{
   struct m9_replay *r = ctx->replay;

   if (!r)
      return;
   release_recording(&ctx->base, r);
   state_release(&r->live);
   util_set_vertex_buffers_count(r->vb, &r->num_vb, NULL, 0);
   for (unsigned s = 0; s < M9_REPLAY_STAGES; s++)
      for (unsigned i = 0; i < PIPE_MAX_CONSTANT_BUFFERS; i++) {
         pipe_resource_reference(&r->cb[s][i].buffer, NULL);
         free(r->cb_copy[s][i]);
      }
   util_dynarray_fini(&r->states);
   util_dynarray_fini(&r->ops);
   util_dynarray_fini(&r->snapshots);
   util_dynarray_fini(&r->deferred);
   FREE(r);
   ctx->replay = NULL;
}
