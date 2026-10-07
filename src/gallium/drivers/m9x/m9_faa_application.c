/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include "m9_faa_application.h"
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
   if (!m9_faa_live_gl_color_build(&live, d->fs, &fresh.shader, &why))
      goto fail;
   /* Original GL1 FF COLOR barycentrics deliberately have INTERP_MODE_NONE:
    * the incoming Gallium flatshade state supplies their interpolation mode. */
   if (fresh.shader.color_legacy_interpolation)
      fresh.shader.color_flat = d->state.flatshade;
   if (fresh.shader.color_flat != d->state.flatshade) {
      why = "FAA COLOR interpolation and application raster state disagree";
      goto fail;
   }
   fresh.state = d->state;
   free(indices);
   *output = fresh;
   if (reason) *reason = NULL;
   return true;
fail:
   free(indices);
   m9_faa_application_fini(&fresh);
   if (reason) *reason = why;
   return false;
}
