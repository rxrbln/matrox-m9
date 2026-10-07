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
   /* The returned normal-Lock view addresses a tiled GEM primary. A
    * userspace layout field cannot turn an ENGINE_ONLY GEM into that owner.
    * Current CREATE_BO rejects TILED|ENGINE_ONLY, so use real BO_TILED. */
   ret = m9_bo_create(screen, fresh.primary_layout.size, PARHELIA_M9_BO_TILED,
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

/* Allocation prepares owners; it does not initialize their GPU state. This
 * snapshot is intentionally not submitted through FIRST_TRIANGLE, which owns
 * a different one-shot private graph and cannot return our init receipt. */
bool
m9_faa_surface_init_request(const struct m9_screen *screen,
                             const struct m9_faa_surface *surface,
                             struct m9_faa_init_request *request)
{
   struct m9_faa_init_request fresh = {0};
   struct m9_faa_graph_layout graph;
   struct m9_faa_primary_layout primary;
   const struct m9_resource *owners[M9_FAA_INIT_OWNER_COUNT];

   if (!screen || !surface || !request || screen->faa.chip != M9_FAA_CHIP_M9 ||
       surface->graph.initialized || surface->width != 320 ||
       surface->height != 240 || surface->format != 0x66 ||
       !surface->destination || !surface->primary.tiled ||
       !m9_faa_graph_layout(surface->width, surface->height, &graph) ||
       !m9_faa_primary_layout(surface->width, surface->height,
                              surface->format, &primary))
      return false;
   /* Compare derived fields explicitly: C padding is not graph metadata. */
   const struct m9_faa_graph_layout *actual = &surface->graph.layout;
   if (actual->width != graph.width || actual->height != graph.height ||
       actual->main_bytes != graph.main_bytes ||
       actual->auxiliary_bytes != graph.auxiliary_bytes ||
       actual->companion_bytes != graph.companion_bytes ||
       actual->hz_bytes != graph.hz_bytes ||
       actual->companion_row_span != graph.companion_row_span ||
       actual->companion_desc != graph.companion_desc ||
       actual->hz_desc != graph.hz_desc ||
       surface->primary_layout.size != primary.size ||
       surface->primary_layout.row_span != primary.row_span ||
       surface->primary_layout.allocation_height != primary.allocation_height ||
       surface->primary_layout.descriptor != primary.descriptor)
      return false;
   owners[M9_FAA_INIT_PRIMARY] = &surface->primary;
   owners[M9_FAA_INIT_MAIN] = &surface->graph.main;
   owners[M9_FAA_INIT_AUXILIARY] = &surface->graph.auxiliary;
   owners[M9_FAA_INIT_COMPANION] = &surface->graph.companion;
   owners[M9_FAA_INIT_HZ] = &surface->graph.hz;
   owners[M9_FAA_INIT_SENTINEL] = &surface->graph.sentinel;
   const uint64_t logical[M9_FAA_INIT_OWNER_COUNT] = {
      primary.size, graph.main_bytes, graph.auxiliary_bytes,
      graph.companion_bytes, graph.hz_bytes, M9_FAA_SENTINEL_BYTES,
   };
   /* Retained prepared180 source graph: compare capacities only. The
    * companion/HZ padding contract is still missing from Mesa allocation;
    * this snapshot neither grows them nor admits a GPU transaction. */
   const uint64_t source_capacity[M9_FAA_INIT_OWNER_COUNT] = {
      0x50000, 0x137000, 0x14000, 0x98000, 0x8000, 0x1000,
   };
   fresh.source_capacity_compatible = true;
   for (unsigned i = 0; i < M9_FAA_INIT_OWNER_COUNT; i++) {
      const struct m9_resource *owner = owners[i];
      if (!owner->handle || !owner->handle_owned ||
          owner->domain != M9_BO_VRAM || owner->size < logical[i] ||
          owner->handle == ((const struct m9_resource *)surface->destination)->handle)
         return false;
      for (unsigned k = 0; k < i; k++)
         if (owner->handle == owners[k]->handle)
            return false;
      fresh.source_capacity_compatible &= owner->size == source_capacity[i];
      fresh.owners[i] = (struct m9_faa_init_owner) {
         .handle = owner->handle, .logical_bytes = logical[i],
         .capacity_bytes = owner->size,
      };
   }
   fresh.width = surface->width;
   fresh.height = surface->height;
   fresh.format = surface->format;
   fresh.primary_desc = primary.descriptor;
   fresh.companion_desc = graph.companion_desc;
   fresh.hz_desc = graph.hz_desc;
   if (!m9_faa_store_ad(0, graph.main_bytes, &fresh.ad))
      return false;
   *request = fresh;
   return true;
}
