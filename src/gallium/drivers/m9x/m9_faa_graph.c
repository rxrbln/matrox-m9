/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <errno.h>
#include <string.h>

#include "m9_internal.h"
#include "m9_screen.h"
#include "util/u_inlines.h"

void
m9_faa_graph_destroy(struct m9_screen *screen, struct m9_faa_graph *graph)
{
   if (!screen || !graph)
      return;
   m9_bo_destroy(screen, &graph->sentinel);
   m9_bo_destroy(screen, &graph->hz);
   m9_bo_destroy(screen, &graph->companion);
   m9_bo_destroy(screen, &graph->auxiliary);
   m9_bo_destroy(screen, &graph->main);
   memset(graph, 0, sizeof(*graph));
}

int
m9_faa_graph_create(struct m9_screen *screen, uint32_t width,
                     uint32_t height, struct m9_faa_graph *graph)
{
   struct m9_faa_graph fresh = {0};
   int ret;

   if (!screen || !graph || graph->main.handle || graph->auxiliary.handle ||
       graph->companion.handle || graph->hz.handle || graph->sentinel.handle ||
       graph->initialized || !m9_faa_graph_layout(width, height, &fresh.layout))
      return -EINVAL;
   ret = m9_bo_create_engine_only(screen, fresh.layout.main_bytes,
                                   M9_RESOURCE_LINEAR_BUFFER, &fresh.main);
   if (!ret)
      ret = m9_bo_create_engine_only(screen, fresh.layout.auxiliary_bytes,
                                      M9_RESOURCE_LINEAR_BUFFER, &fresh.auxiliary);
   if (!ret)
      ret = m9_bo_create_engine_only(screen, fresh.layout.companion_bytes,
                                      M9_RESOURCE_LINEAR_DEPTH, &fresh.companion);
   if (!ret)
      ret = m9_bo_create_engine_only(screen, fresh.layout.hz_bytes,
                                      M9_RESOURCE_LINEAR_DEPTH, &fresh.hz);
   if (!ret)
      ret = m9_bo_create(screen, M9_FAA_SENTINEL_BYTES, PARHELIA_M9_BO_VRAM,
                          M9_RESOURCE_LINEAR_BUFFER, &fresh.sentinel);
   if (!ret && (!fresh.sentinel.map || fresh.sentinel.size <
                sizeof(m9_faa_sentinel_init)))
      ret = -EINVAL;
   if (ret) {
      m9_faa_graph_destroy(screen, &fresh);
      return ret;
   }
   /* No CPU zero-fill of opaque stores or the remaining sentinel bytes.
    * The genuine clear/pool command producers initialize GPU-owned state. */
   memcpy(fresh.sentinel.map, m9_faa_sentinel_init,
          sizeof(m9_faa_sentinel_init));
   *graph = fresh;
   return 0;
}

void
m9_faa_surface_destroy(struct m9_screen *screen, struct m9_faa_surface *surface)
{
   if (!screen || !surface)
      return;
   m9_faa_graph_destroy(screen, &surface->graph);
   m9_bo_destroy(screen, &surface->primary);
   pipe_resource_reference(&surface->destination, NULL);
   pipe_resource_reference(&surface->depth, NULL);
   memset(surface, 0, sizeof(*surface));
}

int
m9_faa_surface_prepare(struct m9_screen *screen,
                        struct m9_resource *destination,
                        struct m9_resource *depth, uint64_t offset,
                        uint32_t width, uint32_t height,
                        struct m9_faa_surface *surface)
{
   struct m9_faa_surface fresh = {0};
   uint64_t minimum = (uint64_t)width * height * 4;
   int ret;

   if (!screen || !surface || !destination || !destination->handle ||
       destination->domain != M9_BO_VRAM || destination->cpp != 4 ||
       width > destination->width || height > destination->height ||
       !destination->width || !destination->height || (offset & 255) ||
       offset > destination->size || minimum > destination->size - offset ||
       (depth && (!depth->handle || depth->domain != M9_BO_VRAM)) ||
       !m9_faa_primary_layout(width, height, destination->format,
                              &fresh.primary_layout))
      return -EINVAL;
   if (surface->destination == &destination->base &&
       surface->depth == (depth ? &depth->base : NULL) &&
       surface->destination_offset == offset && surface->width == width &&
       surface->height == height && surface->format == destination->format)
      return 0;
   /* Preparation cannot retire or replace a group later made executable. */
   if (surface->graph.initialized)
      return -EBUSY;
   ret = m9_bo_create_engine_only(screen, fresh.primary_layout.size,
                                   M9_RESOURCE_LINEAR_COLOR, &fresh.primary);
   if (!ret)
      ret = m9_faa_graph_create(screen, width, height, &fresh.graph);
   if (ret) {
      m9_faa_surface_destroy(screen, &fresh);
      return ret;
   }
   fresh.primary.width = width;
   fresh.primary.height = height;
   fresh.primary.row_span = fresh.primary_layout.row_span;
   fresh.primary.cpp = 4;
   fresh.primary.format = destination->format;
   fresh.primary.layout = 2;
   fresh.destination_offset = offset;
   fresh.width = width;
   fresh.height = height;
   fresh.format = destination->format;
   pipe_resource_reference(&fresh.destination, &destination->base);
   pipe_resource_reference(&fresh.depth, depth ? &depth->base : NULL);
   m9_faa_surface_destroy(screen, surface);
   *surface = fresh;
   return 0;
}
