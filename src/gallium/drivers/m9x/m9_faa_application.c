/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include "m9_faa_application.h"
#include "nir.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

void
m9_faa_application_fini(struct m9_faa_application_upload *upload)
{
   if (upload) {
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
          clear_flags != frame->clear_flags || clear_color != frame->clear_color) {
         why = "FAA draw changed graph, dimensions or pending frame clear";
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
   if (!d || !output || output->shader.storage || !d->info || !d->draw ||
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
   if (!(w > 0 && h > 0 && x >= 0 && y >= 0) ||
       w > 15359 || h > 15359 || x > 15359 - w || y > 15359 - h ||
       floorf(w) != w || floorf(h) != h || floorf(x) != x || floorf(y) != y ||
       d->state.viewport.scale[2] != .5f ||
       d->state.viewport.translate[2] != .5f) {
      why = "FAA application viewport/depth range has no qualified position tail";
      goto fail;
   }
   live.viewport = (struct m9_faa_viewport){x, y, w, h};
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
