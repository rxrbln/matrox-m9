/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

#include "m9_internal.h"
#include "m9_screen.h"
#include "m9_faa_graph_uapi.h"
#include "m9_faa_program.h"
#include "util/u_inlines.h"

void
m9_faa_graph_destroy(struct m9_screen *screen, struct m9_faa_graph *graph)
{
   if (!screen || !graph)
      return;
   /* A failed release cannot invalidate the kernel's independent GEM refs
    * and pins. Teardown closes Mesa handles; file-close owns any retained
    * kernel group. Retarget calls release explicitly and propagates failure. */
   (void)m9_faa_graph_release_kernel(screen, graph);
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
       graph->prepared_id || graph->prepared || graph->initialized ||
       graph->public_vs_attempted || graph->public_vs_point ||
       graph->public_vs_head_attempted || graph->public_vs_head_point ||
       graph->public_vs_draw_attempted || graph->public_vs_draw_point ||
       graph->public_vs_lock_attempted || graph->public_vs_lock_point ||
       graph->public_vs_unlock_attempted || graph->public_vs_unlock_point ||
       graph->public_vs_follow_attempted || graph->public_vs_follow_point ||
       !m9_faa_graph_layout(width, height, &fresh.layout))
      return -EINVAL;
   ret = m9_bo_create_engine_only(screen, fresh.layout.main_bytes,
                                   M9_RESOURCE_LINEAR_BUFFER, &fresh.main);
   if (!ret)
      ret = m9_bo_create_engine_only(screen, fresh.layout.auxiliary_bytes,
                                      M9_RESOURCE_LINEAR_BUFFER, &fresh.auxiliary);
   if (!ret)
      ret = m9_bo_create_engine_only(screen, fresh.layout.companion_capacity_bytes ?
                                      fresh.layout.companion_capacity_bytes :
                                      fresh.layout.companion_bytes,
                                      M9_RESOURCE_LINEAR_DEPTH, &fresh.companion);
   if (!ret)
      ret = m9_bo_create_engine_only(screen, fresh.layout.hz_capacity_bytes ?
                                      fresh.layout.hz_capacity_bytes :
                                      fresh.layout.hz_bytes,
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
   /* The kernel's native context keeps independent references to a graph
    * after file RELEASE. Retarget still releases the exact old token before
    * publishing fresh owners; the new graph cannot inherit INIT's receipt. */
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
   ret = m9_faa_graph_release_kernel(screen, &surface->graph);
   if (ret) {
      m9_faa_surface_destroy(screen, &fresh);
      return ret;
   }
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
       actual->companion_clear_bytes != graph.companion_clear_bytes ||
       actual->companion_capacity_bytes != graph.companion_capacity_bytes ||
       actual->hz_clear_bytes != graph.hz_clear_bytes ||
       actual->hz_capacity_bytes != graph.hz_capacity_bytes ||
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
   /* Clear ranges are source-qualified only for these two roles. Zero
    * does not authorize a clear of another owner. Backing padding is not
    * CPU-initialized or included in the GPU clear extent. */
   const uint64_t clear[M9_FAA_INIT_OWNER_COUNT] = {
      0, 0, 0, graph.companion_clear_bytes, graph.hz_clear_bytes, 0,
   };
   /* Retained prepared180 page capacities, independently confirmed by
    * the actual constructor/backing allocator. No initialization receipt. */
   const uint64_t source_capacity[M9_FAA_INIT_OWNER_COUNT] = {
      0x50000, 0x137000, 0x14000, graph.companion_capacity_bytes,
      graph.hz_capacity_bytes, 0x1000,
   };
   fresh.source_capacity_compatible = true;
   for (unsigned i = 0; i < M9_FAA_INIT_OWNER_COUNT; i++) {
      const struct m9_resource *owner = owners[i];
      if (!owner->handle || !owner->handle_owned ||
          owner->domain != M9_BO_VRAM || owner->size < logical[i] ||
          owner->size < clear[i] ||
          owner->handle == ((const struct m9_resource *)surface->destination)->handle)
         return false;
      for (unsigned k = 0; k < i; k++)
         if (owner->handle == owners[k]->handle)
            return false;
      fresh.source_capacity_compatible &= owner->size == source_capacity[i];
      fresh.owners[i] = (struct m9_faa_init_owner) {
         .handle = owner->handle, .logical_bytes = logical[i],
         .clear_bytes = clear[i], .capacity_bytes = owner->size,
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

/* PREPARE has no GPU endpoint. Its id/generation is a retained-owner token,
 * not evidence of initialized GPU stores, shaders or a usable FAA job. */
int
m9_faa_surface_prepare_kernel(struct m9_screen *screen,
                                struct m9_faa_surface *surface)
{
   struct m9_faa_init_request snapshot;
   struct drm_parhelia_m9_faa_graph_prepare request = {0};
   struct m9_faa_graph *graph;

   if (!screen || !surface)
      return -EINVAL;
   graph = &surface->graph;
   if (graph->initialized)
      return graph->prepared && graph->prepared_id && graph->init_point ? 0 : -EPROTO;
   if (!m9_faa_surface_init_request(screen, surface, &snapshot) ||
       !snapshot.source_capacity_compatible)
      return -EOPNOTSUPP;
   if (graph->prepared_id)
      return graph->prepared ? 0 : -EPROTO;
   if (graph->prepared)
      return -EPROTO;
   _Static_assert(M9_FAA_INIT_OWNER_COUNT == PARHELIA_M9_FAA_GRAPH_OWNERS,
                  "FAA owner count differs from kernel ABI");
   _Static_assert((unsigned)M9_FAA_INIT_PRIMARY == (unsigned)PARHELIA_M9_FAA_PRIMARY &&
                  (unsigned)M9_FAA_INIT_MAIN == (unsigned)PARHELIA_M9_FAA_MAIN &&
                  (unsigned)M9_FAA_INIT_AUXILIARY == (unsigned)PARHELIA_M9_FAA_AUXILIARY &&
                  (unsigned)M9_FAA_INIT_COMPANION == (unsigned)PARHELIA_M9_FAA_COMPANION &&
                  (unsigned)M9_FAA_INIT_HZ == (unsigned)PARHELIA_M9_FAA_HZ &&
                  (unsigned)M9_FAA_INIT_SENTINEL == (unsigned)PARHELIA_M9_FAA_SENTINEL,
                  "FAA owner ordering differs from kernel ABI");
   request.width = snapshot.width;
   request.height = snapshot.height;
   request.format = snapshot.format;
   request.primary_desc = snapshot.primary_desc;
   request.companion_desc = snapshot.companion_desc;
   request.hz_desc = snapshot.hz_desc;
   request.ad = snapshot.ad;
   for (unsigned i = 0; i < M9_FAA_INIT_OWNER_COUNT; i++) {
      request.owners[i].handle = snapshot.owners[i].handle;
      request.owners[i].logical_bytes = snapshot.owners[i].logical_bytes;
      request.owners[i].capacity_bytes = snapshot.owners[i].capacity_bytes;
      request.owners[i].clear_bytes = snapshot.owners[i].clear_bytes;
   }
   if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_PREPARE, &request))
      return -errno;
   if (!request.graph_id)
      return -EPROTO;
   graph->prepared_id = request.graph_id;
   graph->prepared_generation = request.generation;
   if (request.state != PARHELIA_M9_FAA_GRAPH_PREPARED || request.init_point) {
      (void)m9_faa_graph_release_kernel(screen, graph);
      return -EPROTO;
   }
   graph->prepared = true;
   return 0;
}

int
m9_faa_surface_init_kernel(struct m9_screen *screen,
                            struct m9_faa_surface *surface)
{
   struct drm_parhelia_m9_faa_graph_init request = {0};
   struct m9_faa_graph *graph;

   _Static_assert(sizeof(request) == 32, "FAA INIT ioctl ABI differs from kernel");

   if (!screen || !surface)
      return -EINVAL;
   graph = &surface->graph;
   if (!graph->prepared || !graph->prepared_id)
      return -EINVAL;
   if (graph->initialized)
      return graph->init_point ? 0 : -EPROTO;
   if (graph->init_point)
      return -EPROTO;
   request.graph_id = graph->prepared_id;
   request.generation = graph->prepared_generation;
   if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_INIT, &request))
      return -errno;
   /* The ioctl waits for the native producer's actual retirement. A
    * PREPARED token, an unretired point, or a changed identity proves no
    * usable graph, even if ioctl itself reported success. */
   if (request.graph_id != graph->prepared_id ||
       request.generation != graph->prepared_generation || request.flags ||
       request.pad || request.state != PARHELIA_M9_FAA_GRAPH_INITIALIZED ||
       !request.init_point)
      return -EPROTO;
   graph->init_point = request.init_point;
   graph->initialized = true;
   return 0;
}

static int
m9_faa_surface_public_vs_submit_kernel(struct m9_screen *screen,
                               struct m9_faa_surface *surface,
                               const struct m9_faa_public_upload *upload, bool head)
{
   struct drm_parhelia_m9_faa_graph_public_vs request = {0};
   struct m9_faa_graph *graph;

   _Static_assert(sizeof(request) == 840, "public VS kernel ABI");
   _Static_assert(sizeof(struct m9_faa_public_upload) == 796, "owned upload ABI");
   _Static_assert(sizeof(request.layout) == sizeof(upload->layout), "upload layout ABI");
   if (!screen || !surface || !upload)
      return -EINVAL;
   graph = &surface->graph;
   const uint64_t delta = head ? 0x10 : 0x50;
   bool *attempted = head ? &graph->public_vs_head_attempted : &graph->public_vs_attempted;
   uint64_t *retired_point = head ? &graph->public_vs_head_point : &graph->public_vs_point;
   if (screen->faa.chip != M9_FAA_CHIP_M9 || !screen->wait_point ||
       surface->width != 320 || surface->height != 240 || surface->format != 0x66)
      return -EOPNOTSUPP;
   if (!graph->prepared || !graph->prepared_id || !graph->initialized ||
       !graph->init_point || graph->init_point > UINT64_MAX - delta)
      return -EINVAL;
   if (graph->public_vs_attempted || graph->public_vs_point ||
       graph->public_vs_head_attempted || graph->public_vs_head_point)
      return -ESTALE;
   if (upload->layout.program.bytes != sizeof(request.program) ||
       upload->layout.program.count != 4 ||
       upload->layout.constants_bytes != sizeof(request.constants) ||
       upload->layout.geometry_bytes != sizeof(request.geometry))
      return -EOPNOTSUPP;
   request.graph_id = graph->prepared_id;
   request.generation = graph->prepared_generation;
   request.init_point = graph->init_point;
   memcpy(&request.layout, &upload->layout, sizeof(request.layout));
   memcpy(request.program, upload->program, sizeof(request.program));
   memcpy(request.constants, upload->constants, sizeof(request.constants));
   memcpy(request.geometry, upload->geometry, sizeof(request.geometry));
   const struct drm_parhelia_m9_faa_graph_public_vs before = request;
   /* Failure can occur after publication. Preserve all owners and do not
    * retry a potentially consumed graph. Kernel RELEASE/reset owns cleanup. */
   *attempted = true;
   if (ioctl(screen->fd, head ? DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_HEAD :
             DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS, &request))
      return -errno;
   if (request.graph_id != before.graph_id || request.generation != before.generation ||
       request.init_point != before.init_point || request.flags || request.pad ||
       request.state != (head ? PARHELIA_M9_FAA_PUBLIC_VS_HEAD_RETIRED :
                                  PARHELIA_M9_FAA_PUBLIC_VS_PREFIX_RETIRED) ||
       request.prefix_point != before.init_point + delta ||
       memcmp(&request.layout, &before.layout, sizeof(request.layout)) ||
       memcmp(request.program, before.program, sizeof(request.program)) ||
       memcmp(request.constants, before.constants, sizeof(request.constants)) ||
       memcmp(request.geometry, before.geometry, sizeof(request.geometry)))
      return -EPROTO;
   int ret = m9_wait_for_point(screen, request.prefix_point);
   if (ret)
      return ret;
   *retired_point = request.prefix_point;
   return 0;
}

int
m9_faa_surface_public_vs_kernel(struct m9_screen *screen,
                               struct m9_faa_surface *surface,
                               const struct m9_faa_public_upload *upload)
{
   return m9_faa_surface_public_vs_submit_kernel(screen, surface, upload, false);
}

int
m9_faa_surface_public_vs_head_kernel(struct m9_screen *screen,
                                    struct m9_faa_surface *surface,
                                    const struct m9_faa_public_upload *upload)
{
   return m9_faa_surface_public_vs_submit_kernel(screen, surface, upload, true);
}

int
m9_faa_surface_public_vs_draw_kernel(struct m9_screen *screen,
                                     struct m9_faa_surface *surface)
{
   struct drm_parhelia_m9_faa_graph_public_vs_draw request = {0};
   struct m9_faa_graph *graph;

   _Static_assert(sizeof(request) == 40, "public VS draw kernel ABI");
   if (!screen || !surface)
      return -EINVAL;
   graph = &surface->graph;
   if (screen->faa.chip != M9_FAA_CHIP_M9 || !screen->wait_point ||
       surface->width != 320 || surface->height != 240 || surface->format != 0x66)
      return -EOPNOTSUPP;
   if (!graph->prepared || !graph->prepared_id || !graph->initialized ||
       !graph->public_vs_head_attempted || !graph->init_point ||
       graph->public_vs_attempted || graph->public_vs_point ||
       graph->public_vs_follow_attempted || graph->public_vs_follow_point ||
       graph->init_point > UINT64_MAX - 0xb0 ||
       graph->public_vs_head_point != graph->init_point + 0x10)
      return -EINVAL;
   if (graph->public_vs_draw_attempted || graph->public_vs_draw_point)
      return -ESTALE;
   request.graph_id = graph->prepared_id;
   request.generation = graph->prepared_generation;
   request.head_point = graph->public_vs_head_point;
   const struct drm_parhelia_m9_faa_graph_public_vs_draw before = request;
   graph->public_vs_draw_attempted = true;
   if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_DRAW, &request))
      return -errno;
   if (request.graph_id != before.graph_id || request.generation != before.generation ||
       request.head_point != before.head_point || request.flags || request.pad ||
       request.state != PARHELIA_M9_FAA_PUBLIC_VS_DRAW_OBSERVED ||
       request.draw_point != before.head_point + 0xa0)
      return -EPROTO;
   int ret = m9_wait_for_point(screen, request.draw_point);
   if (ret)
      return ret;
   /* This is an early observation, never permission to recycle any owner. */
   graph->public_vs_draw_point = request.draw_point;
   return 0;
}

int
m9_faa_surface_public_vs_lock_kernel(struct m9_screen *screen,
                                     struct m9_faa_surface *surface)
{
   struct drm_parhelia_m9_faa_graph_public_vs_lock request = {0};
   struct m9_faa_graph *graph;

   _Static_assert(sizeof(request) == 40, "public VS Lock kernel ABI");
   if (!screen || !surface)
      return -EINVAL;
   graph = &surface->graph;
   if (screen->faa.chip != M9_FAA_CHIP_M9 || !screen->wait_point ||
       surface->width != 320 || surface->height != 240 || surface->format != 0x66)
      return -EOPNOTSUPP;
   if (!graph->prepared || !graph->prepared_id || !graph->initialized ||
       !graph->public_vs_head_attempted || !graph->init_point ||
       graph->public_vs_attempted || graph->public_vs_point ||
       graph->public_vs_follow_attempted || graph->public_vs_follow_point ||
       graph->init_point > UINT64_MAX - 0xe0 ||
       graph->public_vs_head_point != graph->init_point + 0x10 ||
       !graph->public_vs_draw_attempted ||
       graph->public_vs_draw_point != graph->init_point + 0xb0)
      return -EINVAL;
   if (graph->public_vs_lock_attempted || graph->public_vs_lock_point)
      return -ESTALE;
   request.graph_id = graph->prepared_id;
   request.generation = graph->prepared_generation;
   request.draw_point = graph->public_vs_draw_point;
   const struct drm_parhelia_m9_faa_graph_public_vs_lock before = request;
   graph->public_vs_lock_attempted = true;
   if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_LOCK, &request))
      return -errno;
   if (request.graph_id != before.graph_id || request.generation != before.generation ||
       request.draw_point != before.draw_point || request.flags || request.pad ||
       request.state != PARHELIA_M9_FAA_PUBLIC_VS_LOCK_RETIRED ||
       request.lock_point != before.draw_point + 0x30)
      return -EPROTO;
   int ret = m9_wait_for_point(screen, request.lock_point);
   if (ret)
      return ret;
   /* Final source write wait completed; the kernel separately guards F021. */
   graph->public_vs_lock_point = request.lock_point;
   return 0;
}

int
m9_faa_surface_public_vs_unlock_kernel(struct m9_screen *screen,
                                     struct m9_faa_surface *surface)
{
   struct drm_parhelia_m9_faa_graph_unlock request = {0};
   struct m9_faa_graph *graph;

   _Static_assert(sizeof(request) == 40, "public VS Unlock kernel ABI");
   if (!screen || !surface)
      return -EINVAL;
   graph = &surface->graph;
   if (screen->faa.chip != M9_FAA_CHIP_M9 || !screen->wait_point ||
       surface->width != 320 || surface->height != 240 || surface->format != 0x66)
      return -EOPNOTSUPP;
   if (!graph->prepared || !graph->prepared_id || !graph->initialized ||
       !graph->public_vs_head_attempted || !graph->init_point ||
       graph->public_vs_attempted || graph->public_vs_point ||
       graph->public_vs_follow_attempted || graph->public_vs_follow_point ||
       graph->init_point > UINT64_MAX - 0x100 ||
       graph->public_vs_head_point != graph->init_point + 0x10 ||
       !graph->public_vs_draw_attempted ||
       graph->public_vs_draw_point != graph->init_point + 0xb0 ||
       !graph->public_vs_lock_attempted ||
       graph->public_vs_lock_point != graph->init_point + 0xe0)
      return -EINVAL;
   if (graph->public_vs_unlock_attempted || graph->public_vs_unlock_point)
      return -ESTALE;
   request.graph_id = graph->prepared_id;
   request.generation = graph->prepared_generation;
   request.lock_point = graph->public_vs_lock_point;
   const struct drm_parhelia_m9_faa_graph_unlock before = request;
   graph->public_vs_unlock_attempted = true;
   if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_UNLOCK, &request))
      return -errno;
   if (request.graph_id != before.graph_id || request.generation != before.generation ||
       request.lock_point != before.lock_point || request.flags || request.pad ||
       request.state != PARHELIA_M9_FAA_UNLOCK_RETIRED ||
       request.unlock_point != before.lock_point + 0x20)
      return -EPROTO;
   int ret = m9_wait_for_point(screen, request.unlock_point);
   if (ret)
      return ret;
   /* Final copyback and independent finish retired on this retained graph. */
   graph->public_vs_unlock_point = request.unlock_point;
   return 0;
}

int
m9_faa_surface_public_vs_follow_kernel(struct m9_screen *screen,
                                      struct m9_faa_surface *surface)
{
   struct drm_parhelia_m9_faa_graph_public_vs_follow request = {0};
   struct m9_faa_graph *graph;

   _Static_assert(sizeof(request) == 40, "public VS follow kernel ABI");
   if (!screen || !surface)
      return -EINVAL;
   graph = &surface->graph;
   if (screen->faa.chip != M9_FAA_CHIP_M9 || !screen->wait_point)
      return -EOPNOTSUPP;
   if (!graph->prepared || !graph->prepared_id || !graph->initialized ||
       !graph->public_vs_attempted || !graph->init_point ||
       graph->public_vs_head_attempted || graph->public_vs_head_point ||
       graph->init_point > UINT64_MAX - 0x80 ||
       graph->public_vs_point != graph->init_point + 0x50)
      return -EINVAL;
   if (graph->public_vs_follow_attempted || graph->public_vs_follow_point)
      return -ESTALE;
   request.graph_id = graph->prepared_id;
   request.generation = graph->prepared_generation;
   request.prefix_point = graph->public_vs_point;
   const struct drm_parhelia_m9_faa_graph_public_vs_follow before = request;
   graph->public_vs_follow_attempted = true;
   if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_FOLLOW, &request))
      return -errno;
   if (request.graph_id != before.graph_id || request.generation != before.generation ||
       request.prefix_point != before.prefix_point || request.flags || request.pad ||
       request.state != PARHELIA_M9_FAA_PUBLIC_VS_FOLLOW_PREFIX_RETIRED ||
       request.follow_point != before.prefix_point + 0x30)
      return -EPROTO;
   int ret = m9_wait_for_point(screen, request.follow_point);
   if (ret)
      return ret;
   graph->public_vs_follow_point = request.follow_point;
   return 0;
}

int
m9_faa_graph_release_kernel(struct m9_screen *screen, struct m9_faa_graph *graph)
{
   struct drm_parhelia_m9_faa_graph_release request = {0};

   if (!screen || !graph)
      return -EINVAL;
   if (!graph->prepared_id)
      return graph->prepared || graph->initialized || graph->init_point ||
             graph->public_vs_attempted || graph->public_vs_point ||
             graph->public_vs_head_attempted || graph->public_vs_head_point ||
             graph->public_vs_draw_attempted || graph->public_vs_draw_point ||
       graph->public_vs_lock_attempted || graph->public_vs_lock_point ||
       graph->public_vs_unlock_attempted || graph->public_vs_unlock_point ||
             graph->public_vs_follow_attempted || graph->public_vs_follow_point ? -EPROTO : 0;
   request.graph_id = graph->prepared_id;
   request.generation = graph->prepared_generation;
   if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_RELEASE, &request))
      return -errno;
   graph->prepared_id = 0;
   graph->prepared_generation = 0;
   graph->prepared = false;
   graph->initialized = false;
   graph->init_point = 0;
   graph->public_vs_head_attempted = false;
   graph->public_vs_head_point = 0;
   graph->public_vs_draw_attempted = false;
   graph->public_vs_draw_point = 0;
   graph->public_vs_lock_attempted = false;
   graph->public_vs_lock_point = 0;
   graph->public_vs_unlock_attempted = false;
   graph->public_vs_unlock_point = 0;
   graph->public_vs_attempted = false;
   graph->public_vs_point = 0;
   graph->public_vs_follow_attempted = false;
   graph->public_vs_follow_point = 0;
   return 0;
}
