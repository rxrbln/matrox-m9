/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include "m9_faa_graph_batch_limits.h"
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

#include "m9_internal.h"
#include "m9_screen.h"
#include "m9_faa_graph_uapi.h"
#include "m9_faa_program.h"
#include "m9_faa_live.h"
#include "m9_faa_application.h"
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
       graph->color_lock_attempted || graph->color_lock_point ||
       graph->color_program_count || graph->color_lock_delta || graph->color_frame_draw_count ||
       graph->color_depth_initialized || graph->color_frame_flags || graph->color_frame_maintenance_point ||
       graph->color_next_attempted || graph->color_next_previous_unlock_point ||
       graph->color_next_lock_point ||
       graph->color_unlock_attempted || graph->color_unlock_point ||
       graph->public_vs_attempted || graph->public_vs_point ||
       graph->public_vs_head_attempted || graph->public_vs_head_point ||
       graph->public_vs_draw_attempted || graph->public_vs_draw_point ||
       graph->public_vs_lock_attempted || graph->public_vs_lock_point ||
       graph->public_vs_unlock_attempted || graph->public_vs_unlock_point ||
       graph->public_vs_next_attempted || graph->public_vs_next_lock_point ||
       graph->public_vs_next_unlock_attempted || graph->public_vs_next_unlock_point ||
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
   if (surface->graph.prepared || surface->graph.initialized) {
      struct m9_faa_graph *graph = &surface->graph;
      const struct m9_resource *owners[] = {
         &surface->primary, &graph->main, &graph->auxiliary,
         &graph->companion, &graph->hz, &graph->sentinel,
      };
      /* The ordinary destination is a presentation copy, not one of the
       * retained GPU graph owners. A same-layout back-buffer change must
       * not RELEASE that graph: the native context still owns it afterward
       * and cannot PREPARE a replacement on the running engine. */
      if (!graph->prepared || !graph->prepared_id ||
          width != surface->width || height != surface->height ||
          destination->format != surface->format ||
          surface->depth != (depth ? &depth->base : NULL))
         return -EOPNOTSUPP;
      if (graph->color_next_attempted || graph->public_vs_attempted ||
          graph->public_vs_head_attempted || graph->public_vs_draw_attempted ||
          graph->public_vs_lock_attempted || graph->public_vs_unlock_attempted ||
          graph->public_vs_next_attempted || graph->public_vs_next_unlock_attempted ||
          graph->public_vs_follow_attempted ||
          (graph->color_lock_attempted &&
           (!graph->initialized || !graph->color_lock_point ||
            !graph->color_unlock_attempted ||
            graph->color_lock_point > UINT64_MAX - 0x20 ||
            graph->color_unlock_point != graph->color_lock_point + 0x20)))
         return -EBUSY;
      for (unsigned i = 0; i < ARRAY_SIZE(owners); i++)
         if (destination->handle == owners[i]->handle)
            return -EINVAL;
      pipe_resource_reference(&surface->destination, &destination->base);
      surface->destination_offset = offset;
      /* Swapping presentation buffers and binding a different same-size FBO
       * are indistinguishable here. Require a real full-frame reset before
       * this new destination can receive retained sampled contents. */
      surface->destination_needs_clear = true;
      return 0;
   }
   /* The kernel's native context keeps independent references to a graph
    * after file RELEASE. Retarget still releases the exact old token before
    * publishing fresh owners; the new graph cannot inherit INIT's receipt. */
   /* The returned normal-Lock view addresses a tiled GEM primary. A
    * userspace layout field cannot turn an ENGINE_ONLY GEM into that owner.
    * Current CREATE_BO rejects TILED|ENGINE_ONLY, so use real BO_TILED. */
   ret = m9_bo_create(screen, fresh.primary_layout.capacity_bytes, PARHELIA_M9_BO_TILED,
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
       surface->graph.initialized || !m9_faa_runtime_dimensions(surface->width,surface->height) || surface->format != 0x66 ||
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
   /* Source constructor/backing capacities for each admitted shape.
    * These CPU contracts do not constitute an initialization receipt. */
   const uint64_t source_capacity[M9_FAA_INIT_OWNER_COUNT] = {
      primary.capacity_bytes, (graph.main_bytes+4095)&~UINT64_C(4095),
      (graph.auxiliary_bytes+4095)&~UINT64_C(4095), graph.companion_capacity_bytes,
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
m9_faa_color_upload_request(const struct m9_faa_gl_upload *upload,
                            struct drm_parhelia_m9_faa_graph_color_lock *request)
{
   if (!upload || !upload->storage || !request) return -EINVAL;
   uint64_t total = (uint64_t)upload->layout.bytes + upload->constants_bytes +
                    upload->geometry_bytes + (uint64_t)upload->declaration_words * 4;
   uintptr_t base = (uintptr_t)upload->storage;
   if ((upload->layout.count != 2 && upload->layout.count != 4) ||
       !upload->layout.bytes || upload->layout.bytes % 8 ||
       upload->layout.entry_bytes[0] ||
       !upload->record_bytes || upload->record_bytes % 16 ||
       !upload->vertex_count || upload->vertex_count > M9_FAA_GL_UPLOAD_MAX_VERTICES ||
       (uint64_t)upload->record_bytes * upload->vertex_count != upload->vertex_bytes ||
       upload->vertex_offset > upload->geometry_bytes ||
       upload->vertex_bytes > upload->geometry_bytes - upload->vertex_offset ||
       !upload->declaration_words || upload->declaration_words > M9_VS_MAX_DECL ||
       !upload->constants_bytes || upload->constants_bytes % 8 ||
       total > UINT32_MAX || total != upload->storage_bytes || total > UINTPTR_MAX - base ||
       (uintptr_t)upload->program != base ||
       (uintptr_t)upload->constants != base + upload->layout.bytes ||
       (uintptr_t)upload->geometry != base + upload->layout.bytes + upload->constants_bytes ||
       (uintptr_t)upload->declaration != base + upload->layout.bytes +
                                           upload->constants_bytes + upload->geometry_bytes)
      return -EINVAL;
   for (unsigned i = 0; i < 4; i++) {
      uint32_t entry = upload->layout.entry_bytes[i];
      if (i >= upload->layout.count) {
         if (entry) return -EINVAL;
      } else {
         uint32_t next = i + 1 == upload->layout.count ? upload->layout.bytes :
                                                         upload->layout.entry_bytes[i + 1];
         if (entry % 8 || next < entry || next > upload->layout.bytes || next - entry < 32)
            return -EINVAL;
      }
   }
   uint32_t end = upload->layout.count == 2 ? 0 : 16;
   for (unsigned i = 0; i < 4; i++) {
      uint32_t begin = upload->constant_begin[i], next = upload->constant_end[i];
      if (i >= upload->layout.count) {
         if (begin || next) return -EINVAL;
      } else {
         if (begin != end || begin % 8 || next < begin || next % 8 ||
             next > upload->constants_bytes || (next - begin) / 8 > M9_VS_MAX_CONST_PAIRS)
            return -EINVAL;
         end = next;
      }
   }
   if (end != upload->constants_bytes)
      return -EINVAL;
   request->upload = base;
   request->upload_bytes = total;
   request->record_stride = upload->record_bytes;
   request->record_count = upload->vertex_count;
   request->declaration_count = upload->declaration_words;
   request->layout.program_bytes = upload->layout.bytes;
   request->layout.program_count = upload->layout.count;
   memcpy(request->layout.entry_bytes, upload->layout.entry_bytes, sizeof(request->layout.entry_bytes));
   request->layout.constants_bytes = upload->constants_bytes;
   memcpy(request->layout.constant_begin, upload->constant_begin, sizeof(request->layout.constant_begin));
   memcpy(request->layout.constant_end, upload->constant_end, sizeof(request->layout.constant_end));
   request->layout.geometry_bytes = upload->geometry_bytes;
   request->layout.vertex_offset = upload->vertex_offset;
   request->layout.vertex_bytes = upload->vertex_bytes;
   return 0;
}

/* Explicit two-texture FIRST diagnostic. The upload is produced from live
 * GL inputs; each texture handle names an independently retained VRAM BO.
 * Subsequent frames and application shader classification need separate
 * source-qualified admission. */
int
m9_faa_surface_dual_first_kernel(struct m9_screen *screen,
                                 struct m9_faa_surface *surface,
                                 const struct m9_faa_gl_upload *upload,
                                 const struct drm_parhelia_m9_faa_texture_view *texture)
{
   struct drm_parhelia_m9_faa_graph_dual_first request = {0};
   struct drm_parhelia_m9_faa_graph_color_lock packed = {0};
   if (!screen || !surface || !upload || !texture)
      return -EINVAL;
   struct m9_faa_graph *graph = &surface->graph;
   if (screen->faa.chip != M9_FAA_CHIP_M9 || !screen->wait_point ||
       surface->width != 320 || surface->height != 240 ||
       surface->format != 0x66 || !graph->prepared || !graph->initialized ||
       !graph->prepared_id || !graph->init_point || (graph->init_point & 15) ||
       graph->init_point > UINT64_MAX - 0x150 ||
       graph->color_lock_attempted || graph->color_lock_point ||
       graph->color_depth_initialized || graph->color_next_attempted ||
       graph->color_unlock_attempted || graph->public_vs_attempted ||
       upload->layout.count != 4 || upload->declaration_words != 9 ||
       upload->record_bytes != 64 || !upload->vertex_count ||
       upload->vertex_count % 3 || !texture[0].handle ||
       !texture[1].handle || texture[0].handle == texture[1].handle)
      return -EOPNOTSUPP;
   int ret = m9_faa_color_upload_request(upload, &packed);
   if (ret) return ret;
   request.graph_id = graph->prepared_id;
   request.init_point = graph->init_point;
   request.upload = packed.upload;
   request.upload_bytes = packed.upload_bytes;
   request.generation = graph->prepared_generation;
   request.version = PARHELIA_M9_FAA_DUAL_FIRST_VERSION;
   request.record_stride = packed.record_stride;
   request.record_count = packed.record_count;
   request.declaration_count = packed.declaration_count;
   memcpy(&request.layout, &packed.layout, sizeof(request.layout));
   request.width = surface->width;request.height = surface->height;
   request.clear_flags = PARHELIA_M9_FAA_COLOR_CLEAR_TARGET |
                         PARHELIA_M9_FAA_COLOR_CLEAR_DEPTH;
   request.depth_flags = 7; /* original bound LESS/write profile */
   memcpy(request.texture, texture, sizeof(request.texture));
   const struct drm_parhelia_m9_faa_graph_dual_first before = request;
   graph->color_lock_attempted = true;
   graph->color_program_count = 4;
   if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_DUAL_FIRST, &request))
      return -errno;
   if (request.lock_point != before.init_point + 0x150 ||
       request.state != PARHELIA_M9_FAA_DUAL_FIRST_LOCK_RETIRED)
      return -EPROTO;
   struct drm_parhelia_m9_faa_graph_dual_first returned = request;
   returned.lock_point = 0;returned.state = 0;
   if (memcmp(&returned, &before, sizeof(before)))
      return -EPROTO;
   ret = m9_wait_for_point(screen, request.lock_point);
   if (ret) return ret;
   graph->color_lock_point = request.lock_point;
   graph->color_lock_delta = 0x150;
   graph->color_depth_initialized = true;
   graph->color_unlock_attempted = false;
   graph->color_unlock_point = 0;
   return 0;
}


/* Keep the exact authenticated transaction shape. Accepting either of two
 * plausible deltas loses the distinction between clear and no-clear Lock. */
static bool
m9_faa_color_lock_history_valid(const struct m9_faa_graph *graph)
{
   const uint64_t delta = graph->color_lock_delta;
   uint64_t previous;
   const bool initialized_frame = graph->color_frame_flags == PARHELIA_M9_FAA_COLOR_FRAME_INITIALIZED;
   if ((graph->color_frame_flags & ~3u) || graph->color_frame_flags == 3 ||
       (initialized_frame && (!graph->color_depth_initialized || !graph->color_next_lock_point)) ||
       ((graph->color_frame_maintenance_point || graph->color_frame_present_point) && !graph->color_frame_draw_count))
      return false;
   if (graph->color_frame_draw_count) {
      uint32_t count = graph->color_frame_draw_count;
      if (count > M9_FAA_FRAME_BATCH_MAX_DRAWS || (graph->color_program_count != 2 && graph->color_program_count != 4))
         return false;
      bool first = !graph->color_next_lock_point;
      const bool fused = graph->color_program_count == 2;
      if (fused && !first && !initialized_frame) return false;
      uint64_t exact = fused ? 0x110 + (count - 1u) * UINT64_C(0x60) :
         ((first || initialized_frame) ? 0x150 : 0x1a0) + (count - 1u) * UINT64_C(0xa0);
      if (graph->color_frame_mode_planned) {
         if (!fused || !graph->color_depth_mode_valid ||
             graph->color_frame_mode_changes > count ||
             (graph->color_frame_clear_flags && graph->color_frame_clear_flags != 3) ||
             (first && graph->color_frame_clear_flags != 3)) return false;
         exact = (first || graph->color_frame_clear_flags ? 0x110 : 0x90) +
                 (count - 1u) * UINT64_C(0x60) + graph->color_frame_mode_changes * UINT64_C(0x50);
      }
      const uint64_t prior_present = graph->color_frame_present_point;
      uint64_t origin = graph->color_next_previous_unlock_point;
      if (prior_present) {
         if (!initialized_frame || origin > UINT64_MAX - 0x20 || prior_present != origin + 0x20)
            return false;
         exact += 0x20;
         origin = prior_present;
      }
      const uint64_t maintenance = graph->color_frame_maintenance_point;
      if (maintenance) {
         if (!initialized_frame || origin > UINT64_MAX - 0x10 ||
             maintenance != origin + 0x10)
            return false;
         exact += 0x10;
      }
      if (delta != exact ||
          (!first && graph->color_lock_point != graph->color_next_lock_point) ||
          (first && graph->color_next_previous_unlock_point))
         return false;
      previous = first ? graph->init_point : graph->color_next_previous_unlock_point;
      return previous && !(previous & 15) && previous <= UINT64_MAX - exact &&
             graph->color_lock_point == previous + exact;
   }
   if (graph->color_next_lock_point) {
      if (graph->color_program_count != 4 ||
          graph->color_lock_point != graph->color_next_lock_point ||
          (initialized_frame ? delta != 0x150 :
           (delta != 0xd0 && delta != 0xe0 && delta != 0x1a0)))
         return false;
      previous = graph->color_next_previous_unlock_point;
   } else {
      if (graph->color_next_previous_unlock_point ||
          (graph->color_program_count != 2 && graph->color_program_count != 4) ||
          (graph->color_program_count == 2 ?
           (delta != 0xa0 && delta != 0x110) :
           (delta != 0xe0 && delta != 0x150)))
         return false;
      previous = graph->init_point;
   }
   return previous && !(previous & 15) && previous <= UINT64_MAX - delta &&
          graph->color_lock_point == previous + delta;
}

int
m9_faa_surface_color_lock_kernel(struct m9_screen *screen,
                                struct m9_faa_surface *surface,
                                const struct m9_faa_gl_upload *upload)
{
   struct drm_parhelia_m9_faa_graph_color_lock request = {0};
   _Static_assert(sizeof(request) == 136, "FAA COLOR Lock kernel ABI");
   if (!screen || !surface || !upload || !upload->storage)
      return -EINVAL;
   struct m9_faa_graph *graph = &surface->graph;
   if (screen->faa.chip != M9_FAA_CHIP_M9 || !screen->wait_point ||
       surface->width != 320 || surface->height != 240 || surface->format != 0x66 ||
       (upload->layout.count != 2 && upload->layout.count != 4))
      return -EOPNOTSUPP;
   const uint64_t delta = upload->layout.count == 2 ? 0xa0 : 0xe0;
   if (!graph->prepared || !graph->prepared_id || !graph->initialized ||
       !graph->init_point || graph->init_point > UINT64_MAX - delta)
      return -EINVAL;
   if (graph->color_lock_attempted || graph->color_lock_point ||
       graph->color_program_count || graph->color_lock_delta || graph->color_frame_draw_count ||
       graph->color_depth_initialized || graph->color_frame_flags || graph->color_frame_maintenance_point ||
       graph->color_next_attempted || graph->color_next_previous_unlock_point ||
       graph->color_next_lock_point ||
       graph->color_unlock_attempted || graph->color_unlock_point ||
       graph->public_vs_attempted || graph->public_vs_point ||
       graph->public_vs_head_attempted || graph->public_vs_head_point ||
       graph->public_vs_draw_attempted || graph->public_vs_draw_point ||
       graph->public_vs_lock_attempted || graph->public_vs_lock_point ||
       graph->public_vs_unlock_attempted || graph->public_vs_unlock_point ||
       graph->public_vs_next_attempted || graph->public_vs_next_lock_point ||
       graph->public_vs_next_unlock_attempted || graph->public_vs_next_unlock_point ||
       graph->public_vs_follow_attempted || graph->public_vs_follow_point)
      return -ESTALE;
   int ret = m9_faa_color_upload_request(upload, &request);
   if (ret) return ret;
   request.graph_id = graph->prepared_id;
   request.init_point = graph->init_point;
   request.generation = graph->prepared_generation;
   const struct drm_parhelia_m9_faa_graph_color_lock before = request;
   /* The kernel may publish before reporting an error. Keep the consumed
    * graph and its owners until authenticated RELEASE or reset. */
   graph->color_lock_attempted = true;
   graph->color_program_count = upload->layout.count;
   if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_COLOR_LOCK, &request))
      return -errno;
   if (request.lock_point != before.init_point + delta ||
       request.state != PARHELIA_M9_FAA_COLOR_LOCK_RETIRED)
      return -EPROTO;
   struct drm_parhelia_m9_faa_graph_color_lock returned = request;
   returned.lock_point = 0;
   returned.state = 0;
   if (memcmp(&returned, &before, sizeof(before)))
      return -EPROTO;
   ret = m9_wait_for_point(screen, request.lock_point);
   if (ret) return ret;
   graph->color_lock_point = request.lock_point;
   graph->color_lock_delta = delta;
   graph->color_frame_draw_count = 0;
   graph->color_frame_flags = 0;
   graph->color_frame_maintenance_point = 0;
   return 0;
}

int
m9_faa_surface_color_next_kernel(struct m9_screen *screen,
                                 struct m9_faa_surface *surface,
                                 const struct m9_faa_gl_upload *upload,
                                 const struct m9_faa_application_state *state,
                                 uint32_t clear_flags, uint32_t clear_color)
{
   struct drm_parhelia_m9_faa_graph_color_next request = {0};
   struct drm_parhelia_m9_faa_graph_color_lock input = {0};
   _Static_assert(sizeof(request) == 160, "FAA COLOR_NEXT kernel ABI");
   _Static_assert(offsetof(struct drm_parhelia_m9_faa_graph_color_next, width) ==
                  sizeof(input), "FAA COLOR_NEXT owned upload prefix");
   if (!screen || !surface || !upload || !state)
      return -EINVAL;
   struct m9_faa_graph *graph = &surface->graph;
   if (screen->faa.chip != M9_FAA_CHIP_M9 || !screen->wait_point ||
       surface->width != 320 || surface->height != 240 || surface->format != 0x66 ||
       state->width != surface->width || state->height != surface->height ||
       state->depth_enabled || state->cull_face != PIPE_FACE_NONE || state->flatshade ||
       state->scissor_enabled || upload->layout.count != 4 || (upload->record_bytes != 32 &&
        !(upload->record_bytes == 16 && upload->declaration_words == 3)) ||
       (!upload->vertex_count || upload->vertex_count % 3) ||
       (clear_flags & ~PARHELIA_M9_FAA_COLOR_CLEAR_TARGET) || clear_color)
      return -EOPNOTSUPP;
   const uint64_t delta = clear_flags ? 0xe0 : 0xd0;
   if (!graph->prepared || !graph->prepared_id || !graph->initialized ||
       !graph->init_point || !graph->color_lock_attempted || !graph->color_lock_point ||
       graph->color_program_count != 4 || !graph->color_unlock_attempted ||
       graph->color_lock_point > UINT64_MAX - 0x20 ||
       graph->color_unlock_point != graph->color_lock_point + 0x20 ||
       (graph->color_unlock_point & 15) || graph->color_unlock_point > UINT64_MAX - delta ||
       graph->color_next_attempted ||
       !m9_faa_color_lock_history_valid(graph) ||
       graph->public_vs_attempted || graph->public_vs_point ||
       graph->public_vs_head_attempted || graph->public_vs_head_point ||
       graph->public_vs_draw_attempted || graph->public_vs_draw_point ||
       graph->public_vs_lock_attempted || graph->public_vs_lock_point ||
       graph->public_vs_unlock_attempted || graph->public_vs_unlock_point ||
       graph->public_vs_next_attempted || graph->public_vs_next_lock_point ||
       graph->public_vs_next_unlock_attempted || graph->public_vs_next_unlock_point ||
       graph->public_vs_follow_attempted || graph->public_vs_follow_point)
      return -ESTALE;
   int ret = m9_faa_color_upload_request(upload, &input);
   if (ret) return ret;
   /* The source ABI adds six state words after the same owned blob layout.
    * No token or address is inherited from the upload itself. */
   memcpy(&request, &input, sizeof(input));
   request.graph_id = graph->prepared_id;
   if (graph->color_present_point || graph->color_present_attempted) return -EOPNOTSUPP;
   request.previous_unlock_point = graph->color_unlock_point;
   request.generation = graph->prepared_generation;
   request.width = state->width;
   request.height = state->height;
   request.clear_flags = clear_flags;
   request.clear_color = clear_color;
   const struct drm_parhelia_m9_faa_graph_color_next before = request;
   graph->color_next_attempted = true;
   if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_COLOR_NEXT, &request))
      return -errno;
   if (request.lock_point != before.previous_unlock_point + delta ||
       request.state != PARHELIA_M9_FAA_COLOR_NEXT_LOCK_RETIRED)
      return -EPROTO;
   struct drm_parhelia_m9_faa_graph_color_next returned = request;
   returned.lock_point = 0;
   returned.state = 0;
   if (memcmp(&returned, &before, sizeof(before)))
      return -EPROTO;
   ret = m9_wait_for_point(screen, request.lock_point);
   if (ret) return ret;
   /* Publish a new current Lock only after its authentic receipt and real
    * wait. Any failed NEXT preserves the old spent Lock/Unlock markers and
    * the attempted flag, so it cannot be retried or presented as success. */
   graph->color_next_previous_unlock_point = before.previous_unlock_point;
   graph->color_next_lock_point = request.lock_point;
   graph->color_lock_point = request.lock_point;
   graph->color_lock_delta = delta;
   graph->color_frame_draw_count = 0;
   graph->color_frame_flags = 0;
   graph->color_frame_maintenance_point = 0;
   graph->color_unlock_attempted = false;
   graph->color_unlock_point = 0;
   return 0;
}

int
m9_faa_surface_color_frame_kernel(struct m9_screen *screen,
                                 struct m9_faa_surface *surface,
                                 const struct m9_faa_gl_upload *upload,
                                 const struct m9_faa_application_state *state,
                                 uint32_t clear_flags, uint32_t clear_color)
{
   struct drm_parhelia_m9_faa_graph_color_frame request = {0};
   struct drm_parhelia_m9_faa_graph_color_lock input = {0};
   _Static_assert(sizeof(request) == 160, "FAA COLOR_FRAME kernel ABI");
   _Static_assert(offsetof(struct drm_parhelia_m9_faa_graph_color_frame, width) ==
                  sizeof(input), "FAA COLOR_FRAME owned upload prefix");
   if (!screen || !surface || !upload || !state)
      return -EINVAL;
   struct m9_faa_graph *graph = &surface->graph;
   const bool sampled_reset = clear_flags ==
      (PARHELIA_M9_FAA_COLOR_CLEAR_TARGET | PARHELIA_M9_FAA_COLOR_CLEAR_DEPTH);
   if (screen->faa.chip != M9_FAA_CHIP_M9 || !screen->wait_point ||
       !m9_faa_runtime_dimensions(surface->width,surface->height) || surface->format != 0x66 ||
       state->width != surface->width || state->height != surface->height ||
       state->scissor_enabled || upload->color_flat != state->flatshade ||
       (upload->layout.count != 2 && upload->layout.count != 4) || (upload->record_bytes != 32 &&
        !(upload->record_bytes == 16 && upload->declaration_words == 3)) ||
       (!upload->vertex_count || upload->vertex_count % 3) ||
       (clear_flags & ~(PARHELIA_M9_FAA_COLOR_CLEAR_TARGET | PARHELIA_M9_FAA_COLOR_CLEAR_DEPTH)) || clear_color)
      return -EOPNOTSUPP;
   uint32_t depth_flags = 0, raster_flags = 0;
   if (sampled_reset) {
      if (!state->depth_enabled || state->depth_func > PIPE_FUNC_ALWAYS ||
          (state->cull_face != PIPE_FACE_NONE && state->cull_face != PIPE_FACE_FRONT &&
           state->cull_face != PIPE_FACE_BACK))
         return -EOPNOTSUPP;
      depth_flags = 1u | (state->depth_write ? 2u : 0u) | (state->depth_func << 2);
      if (state->flatshade) raster_flags |= PARHELIA_M9_FAA_COLOR_RASTER_FLAT;
      if (state->cull_face != PIPE_FACE_NONE)
         raster_flags |= ((state->cull_face == PIPE_FACE_BACK) == state->front_ccw) ?
            PARHELIA_M9_FAA_COLOR_RASTER_CULL_CW : PARHELIA_M9_FAA_COLOR_RASTER_CULL_CCW;
   } else if (state->depth_enabled || state->cull_face != PIPE_FACE_NONE || state->flatshade ||
              (clear_flags & ~PARHELIA_M9_FAA_COLOR_CLEAR_TARGET)) {
      return -EOPNOTSUPP;
   }
   const bool first = !graph->color_lock_attempted;
   /* Original M9128 count2 FIRST terminates normal Lock at INIT+0x110.
    * Repeated two-stage frames require their own authenticated profile;
    * never reuse the four-stage timeline merely because the ABI matches. */
   if (upload->layout.count == 2 && (!first || !sampled_reset))
      return -EOPNOTSUPP;
   if (first && clear_flags && !sampled_reset)
      return -EOPNOTSUPP; /* FIRST requires the complete sampled reset producer. */
   const bool initialized_frame = !first && sampled_reset && graph->color_depth_initialized;
   const uint64_t delta = upload->layout.count == 2 ? 0x110 :
                          sampled_reset ? ((first || initialized_frame) ? 0x150 : 0x1a0) :
                          first ? 0xe0 : clear_flags ? 0xe0 : 0xd0;
   if (first) {
      if (!graph->prepared || !graph->prepared_id || !graph->initialized ||
          !graph->init_point || (graph->init_point & 15) || graph->init_point > UINT64_MAX - delta)
         return -ESTALE;
      if (graph->color_lock_attempted || graph->color_lock_point ||
          graph->color_program_count || graph->color_lock_delta || graph->color_frame_draw_count ||
       graph->color_depth_initialized || graph->color_frame_flags || graph->color_frame_maintenance_point ||
          graph->color_next_attempted || graph->color_next_previous_unlock_point ||
          graph->color_next_lock_point ||
          graph->color_unlock_attempted || graph->color_unlock_point ||
          graph->public_vs_attempted || graph->public_vs_point ||
          graph->public_vs_head_attempted || graph->public_vs_head_point ||
          graph->public_vs_draw_attempted || graph->public_vs_draw_point ||
          graph->public_vs_lock_attempted || graph->public_vs_lock_point ||
          graph->public_vs_unlock_attempted || graph->public_vs_unlock_point ||
          graph->public_vs_next_attempted || graph->public_vs_next_lock_point ||
          graph->public_vs_next_unlock_attempted || graph->public_vs_next_unlock_point ||
          graph->public_vs_follow_attempted || graph->public_vs_follow_point)
         return -ESTALE;
   } else {
      if (!graph->prepared || !graph->prepared_id || !graph->initialized ||
          !graph->init_point || !graph->color_lock_attempted || !graph->color_lock_point ||
          graph->color_program_count != 4 || !graph->color_unlock_attempted ||
          graph->color_lock_point > UINT64_MAX - 0x20 ||
          graph->color_unlock_point != graph->color_lock_point + 0x20 ||
          (graph->color_unlock_point & 15) || graph->color_unlock_point > UINT64_MAX - delta ||
          graph->color_next_attempted ||
          !m9_faa_color_lock_history_valid(graph) ||
       graph->public_vs_attempted || graph->public_vs_point ||
          graph->public_vs_head_attempted || graph->public_vs_head_point ||
          graph->public_vs_draw_attempted || graph->public_vs_draw_point ||
          graph->public_vs_lock_attempted || graph->public_vs_lock_point ||
          graph->public_vs_unlock_attempted || graph->public_vs_unlock_point ||
          graph->public_vs_next_attempted || graph->public_vs_next_lock_point ||
          graph->public_vs_next_unlock_attempted || graph->public_vs_next_unlock_point ||
          graph->public_vs_follow_attempted || graph->public_vs_follow_point)
         return -ESTALE;
   }
   int ret = m9_faa_color_upload_request(upload, &input);
   if (ret) return ret;
   /* The source ABI adds six state words after the same owned blob layout.
    * No token or address is inherited from the upload itself. */
   memcpy(&request, &input, sizeof(input));
   request.graph_id = graph->prepared_id;
   if (graph->color_present_point || graph->color_present_attempted) return -EOPNOTSUPP;
   request.previous_point = first ? graph->init_point : graph->color_unlock_point;
   request.flags = first ? PARHELIA_M9_FAA_COLOR_FRAME_FIRST :
                   initialized_frame ? PARHELIA_M9_FAA_COLOR_FRAME_INITIALIZED : 0;
   request.generation = graph->prepared_generation;
   request.width = state->width;
   request.height = state->height;
   request.clear_flags = clear_flags;
   request.clear_color = clear_color;
   request.depth_flags = depth_flags;
   request.raster_flags = raster_flags;
   const struct drm_parhelia_m9_faa_graph_color_frame before = request;
   if (first) {
      graph->color_lock_attempted = true;
      graph->color_program_count = upload->layout.count;
   } else {
      graph->color_next_attempted = true;
   }
   if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_COLOR_FRAME, &request))
      return -errno;
   if (request.lock_point != before.previous_point + delta ||
       request.state != (first ? PARHELIA_M9_FAA_COLOR_FRAME_FIRST_LOCK_RETIRED :
                                 PARHELIA_M9_FAA_COLOR_FRAME_NEXT_LOCK_RETIRED))
      return -EPROTO;
   struct drm_parhelia_m9_faa_graph_color_frame returned = request;
   returned.lock_point = 0;
   returned.state = 0;
   if (memcmp(&returned, &before, sizeof(before)))
      return -EPROTO;
   ret = m9_wait_for_point(screen, request.lock_point);
   if (ret) return ret;
   /* Publish a new current Lock only after its authentic receipt and real
    * wait. Any failed NEXT preserves the old spent Lock/Unlock markers and
    * the attempted flag, so it cannot be retried or presented as success. */
   if (!first) {
      graph->color_next_previous_unlock_point = before.previous_point;
      graph->color_next_lock_point = request.lock_point;
   }
   graph->color_lock_point = request.lock_point;
   graph->color_lock_delta = delta;
   graph->color_frame_flags = before.flags;
   graph->color_frame_maintenance_point = 0;
   graph->color_frame_draw_count = 0;
   if (sampled_reset) {
      graph->color_depth_initialized = true;
      graph->color_depth_mode_valid = true;
      graph->color_depth_enabled = true;
      graph->color_depth_embedded_hz = false;
   }
   graph->color_unlock_attempted = false;
   graph->color_unlock_point = 0;
   return 0;
}


static int
m9_faa_color_batch_with_storage(struct m9_screen *screen,
                                 struct m9_faa_surface *surface,
                                 const struct m9_faa_application_frame *frame,
                                 struct drm_parhelia_m9_faa_color_draw *draws,
                                 struct drm_parhelia_m9_faa_color_draw *saved)
{
   struct drm_parhelia_m9_faa_graph_color_frame_batch request = {0};
   _Static_assert(sizeof(draws[0]) == 112, "FAA batch draw kernel ABI");
   _Static_assert(sizeof(request) == 80, "FAA frame batch kernel ABI");
   if (!screen || !surface || !frame || !frame->draws || !frame->count)
      return -EINVAL;
   if (frame->count > M9_FAA_FRAME_BATCH_MAX_DRAWS) return -E2BIG;
   const struct m9_faa_application_upload *head = &frame->draws[0];
   struct m9_faa_graph *graph = &surface->graph;
   if (head->graph_id != graph->prepared_id || head->init_point != graph->init_point ||
       head->generation != graph->prepared_generation)
      return -ESTALE;
   if (frame->count == 1 && !graph->color_depth_initialized && head->state.depth_enabled)
      return m9_faa_surface_color_frame_kernel(screen, surface, &head->shader,
                                               &head->state, frame->clear_flags,
                                               frame->clear_color);
   const bool first = !graph->color_lock_attempted;
   const bool initialized_frame = !first && graph->color_depth_initialized;
   const unsigned programs = head->shader.layout.count;
   if ((programs != 2 && programs != 4) ||
       (programs == 2 && !first && !initialized_frame))
      return -EOPNOTSUPP;
   /* Original count2 FIRST and initialized clear3 both use +0x110,
    * with +0x60 per extra DRAW. Their flags and previous receipt remain
    * distinct. A returned maintenance epoch is authenticated separately and
    * never replaces the application's previous Unlock entitlement. */
   uint64_t delta = programs == 2 ? 0x110 + (frame->count - 1u) * UINT64_C(0x60) :
      ((first || initialized_frame) ? 0x150 : 0x1a0) + (frame->count - 1u) * UINT64_C(0xa0);
   if (screen->faa.chip != M9_FAA_CHIP_M9 || !screen->wait_point ||
       !m9_faa_runtime_dimensions(surface->width,surface->height) || surface->format != 0x66 ||
       (first ? frame->clear_flags != 3 : (frame->clear_flags && frame->clear_flags != 3)) ||
       (programs != 2 && frame->clear_flags != 3) ||
       frame->clear_color)
      return -EOPNOTSUPP;
   bool mode_valid = !first && graph->color_depth_mode_valid;
   bool enabled = !first && graph->color_depth_enabled;
   bool embedded_hz = !first && graph->color_depth_embedded_hz;
   uint32_t mode_changes = 0;
   if (programs == 2) {
      if (!first && (!initialized_frame || !mode_valid)) return -ESTALE;
      /* The original later CLEAR3 disabled/reset frontier is unread.
       * Never replace that transaction with an enabled/ALWAYS draw. */
      if (!first && frame->clear_flags && (!enabled || embedded_hz || !head->state.depth_enabled))
         return -EOPNOTSUPP;
      delta = (first || frame->clear_flags ? 0x110 : 0x90) + (frame->count - 1u) * UINT64_C(0x60);
   }
   /* Validate the entire frame before publishing an attempt or entering the
    * ioctl. Each descriptor points at its own immutable CPU-owned snapshot. */
   for (uint32_t i = 0; i < frame->count; i++) {
      const struct m9_faa_application_upload *u = &frame->draws[i];
      const struct m9_faa_application_state *state = &u->state;
      struct drm_parhelia_m9_faa_graph_color_lock packed = {0};
      if (u->graph_id != graph->prepared_id || u->init_point != graph->init_point ||
          u->generation != graph->prepared_generation)
         return -ESTALE;
      if (state->width != surface->width || state->height != surface->height ||
          (programs != 2 && !state->depth_enabled) || state->depth_func > PIPE_FUNC_ALWAYS ||
          state->scissor_enabled || u->shader.color_flat != state->flatshade ||
          (state->cull_face != PIPE_FACE_NONE && state->cull_face != PIPE_FACE_FRONT && state->cull_face != PIPE_FACE_BACK) ||
          u->shader.layout.count != programs || (u->shader.record_bytes != 32 &&
           !(u->shader.record_bytes == 16 && u->shader.declaration_words == 3)) ||
          (u->shader.declaration_words != 3 && u->shader.declaration_words != 5) ||
          !u->shader.vertex_count || u->shader.vertex_count % 3)
         return -EOPNOTSUPP;
      int ret = m9_faa_color_upload_request(&u->shader, &packed);
      if (ret) return ret;
      draws[i].upload = packed.upload;
      draws[i].upload_bytes = packed.upload_bytes;
      draws[i].record_stride = packed.record_stride;
      draws[i].record_count = packed.record_count;
      draws[i].declaration_count = packed.declaration_count;
      draws[i].layout = packed.layout;
      draws[i].depth_flags = (state->depth_enabled ? 1u : PARHELIA_M9_FAA_COLOR_DEPTH_DISABLED) | (state->depth_write ? 2u : 0u) | (state->depth_func << 2);
      if (programs == 2) {
         if (mode_valid && enabled != state->depth_enabled) { mode_changes++; delta += 0x50; }
         mode_valid = true; enabled = state->depth_enabled; embedded_hz |= !enabled;
      }
      if (state->flatshade) draws[i].raster_flags |= PARHELIA_M9_FAA_COLOR_RASTER_FLAT;
      if (state->cull_face != PIPE_FACE_NONE)
         draws[i].raster_flags |= ((state->cull_face == PIPE_FACE_BACK) == state->front_ccw) ?
            PARHELIA_M9_FAA_COLOR_RASTER_CULL_CW : PARHELIA_M9_FAA_COLOR_RASTER_CULL_CCW;
      /* Each job owns its shader interface and full flat/smooth metadata.
       * Cull launch state remains common until its transition is proved. */
      if (i && draws[i].layout.program_count != 2 &&
          (draws[i].raster_flags & 6u) != (draws[0].raster_flags & 6u))
         return -EOPNOTSUPP;
      /* Each stage uploads its own validated declaration. Constant-normal
       * and varying-normal geometry can share a frame with different
       * input attributes; the raw record stride remains common. */
   }
   if (!graph->prepared || !graph->prepared_id || !graph->initialized ||
       !graph->init_point || (graph->init_point & 15) ||
       graph->public_vs_attempted || graph->public_vs_point ||
       graph->public_vs_head_attempted || graph->public_vs_head_point ||
       graph->public_vs_draw_attempted || graph->public_vs_draw_point ||
       graph->public_vs_lock_attempted || graph->public_vs_lock_point ||
       graph->public_vs_unlock_attempted || graph->public_vs_unlock_point ||
       graph->public_vs_next_attempted || graph->public_vs_next_lock_point ||
       graph->public_vs_next_unlock_attempted || graph->public_vs_next_unlock_point ||
       graph->public_vs_follow_attempted || graph->public_vs_follow_point)
      return -ESTALE;
   if (first) {
      if (graph->color_lock_point || graph->color_program_count || graph->color_lock_delta ||
          graph->color_frame_draw_count || graph->color_depth_initialized || graph->color_frame_flags || graph->color_frame_maintenance_point || graph->color_next_attempted ||
          graph->color_next_previous_unlock_point || graph->color_next_lock_point ||
          graph->color_unlock_attempted || graph->color_unlock_point)
         return -ESTALE;
   } else if (graph->color_program_count != programs || !graph->color_unlock_attempted || graph->color_next_attempted ||
              graph->color_lock_point > UINT64_MAX - 0x20 ||
              graph->color_unlock_point != graph->color_lock_point + 0x20 ||
              !m9_faa_color_lock_history_valid(graph)) {
      return -ESTALE;
   }
   const uint64_t prior_present = graph->color_present_point;
   if (graph->color_present_attempted && !prior_present) return -ESTALE;
   if (prior_present && (!initialized_frame || !graph->color_present_attempted ||
       graph->color_unlock_point > UINT64_MAX - 0x20 ||
       prior_present != graph->color_unlock_point + 0x20)) return -ESTALE;
   request.previous_point = first ? graph->init_point : graph->color_unlock_point;
   if (request.previous_point > UINT64_MAX - delta ||
       (prior_present && prior_present > UINT64_MAX - delta)) return -ERANGE;
   request.graph_id = graph->prepared_id;
   request.draws = (uintptr_t)draws;
   request.draw_count = frame->count;
   request.generation = graph->prepared_generation;
   request.flags = first ? PARHELIA_M9_FAA_COLOR_FRAME_FIRST :
                   initialized_frame ? PARHELIA_M9_FAA_COLOR_FRAME_INITIALIZED : 0;
   request.width = surface->width; request.height = surface->height;
   request.clear_flags = frame->clear_flags; request.clear_color = frame->clear_color;
   const struct drm_parhelia_m9_faa_graph_color_frame_batch before = request;
   memcpy(saved, draws, frame->count * sizeof(draws[0]));
   if (first) { graph->color_lock_attempted = true; graph->color_program_count = programs; }
   else graph->color_next_attempted = true;
   if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_COLOR_FRAME_BATCH, &request))
      return -errno;
   uint64_t origin = prior_present ? prior_present : before.previous_point;
   if (request.maintenance_point) {
      if (!initialized_frame || origin > UINT64_MAX - 0x10 ||
          request.maintenance_point != origin + 0x10)
         return -EPROTO;
      origin = request.maintenance_point;
   }
   if (origin > UINT64_MAX - delta || request.lock_point != origin + delta ||
       request.state != (first ? PARHELIA_M9_FAA_COLOR_BATCH_FIRST_LOCK_RETIRED :
                                 PARHELIA_M9_FAA_COLOR_BATCH_NEXT_LOCK_RETIRED))
      return -EPROTO;
   struct drm_parhelia_m9_faa_graph_color_frame_batch returned = request;
   returned.lock_point = 0; returned.state = 0;
   returned.maintenance_point = 0;
   if (memcmp(&returned, &before, sizeof(before)) ||
       memcmp(saved, draws, frame->count * sizeof(draws[0])))
      return -EPROTO;
   int ret = m9_wait_for_point(screen, request.lock_point);
   if (ret) return ret;
   if (!first) {
      graph->color_next_previous_unlock_point = before.previous_point;
      graph->color_next_lock_point = request.lock_point;
   }
   graph->color_lock_point = request.lock_point;
   graph->color_lock_delta = request.lock_point - before.previous_point;
   graph->color_frame_flags = before.flags;
   graph->color_frame_maintenance_point = request.maintenance_point;
   graph->color_frame_present_point = prior_present;
   graph->color_present_point = 0; graph->color_present_attempted = false;
   graph->color_frame_draw_count = frame->count;
   graph->color_depth_initialized = true;
   graph->color_frame_mode_planned = programs == 2;
   graph->color_frame_mode_changes = mode_changes;
   graph->color_frame_clear_flags = frame->clear_flags;
   graph->color_depth_mode_valid = true;
   graph->color_depth_enabled = programs == 2 ? enabled : true;
   graph->color_depth_embedded_hz = programs == 2 && embedded_hz;
   graph->color_unlock_attempted = false; graph->color_unlock_point = 0;
   return 0;
}


int
m9_faa_surface_color_batch_kernel(struct m9_screen *screen,
                                  struct m9_faa_surface *surface,
                                  const struct m9_faa_application_frame *frame)
{
   if (!screen || !surface || !frame || !frame->draws || !frame->count)
      return -EINVAL;
   if (frame->count > M9_FAA_FRAME_BATCH_MAX_DRAWS) return -E2BIG;
   /* Keep the request and immutable receipt comparison off the application
    * stack, sized to this frame. No attempt is published on allocation failure. */
   struct drm_parhelia_m9_faa_color_draw *draws =
      calloc(2u * frame->count, sizeof(*draws));
   if (!draws) return -ENOMEM;
   int ret = m9_faa_color_batch_with_storage(screen, surface, frame, draws,
                                             draws + frame->count);
   free(draws);
   return ret;
}


int
m9_faa_surface_color_present_kernel(struct m9_screen *screen,
                                  struct m9_faa_surface *surface,
                                  uint32_t destination_handle, uint32_t offset,
                                  uint32_t descriptor0, uint32_t descriptor1)
{
   struct drm_parhelia_m9_faa_graph_present request = {0};
   _Static_assert(sizeof(request) == 80, "FAA GPU presentation ABI");
   if (!screen || !surface || !destination_handle) return -EINVAL;
   struct m9_faa_graph *g = &surface->graph;
   if (screen->faa.chip != M9_FAA_CHIP_M9 || !screen->wait_point ||
       !m9_faa_runtime_dimensions(surface->width,surface->height) || surface->format != 0x66 ||
       descriptor0 != (0xcc210000u|surface->width) || descriptor1 != (0x10000u|surface->height) || (offset & 4095))
      return -EOPNOTSUPP;
   if (!g->prepared || !g->initialized || !g->prepared_id ||
       !g->color_lock_attempted || !g->color_depth_initialized ||
       !g->color_lock_point || g->color_lock_point > UINT64_MAX - 0x40 ||
       !m9_faa_color_lock_history_valid(g) || g->color_unlock_attempted ||
       g->color_unlock_point || g->color_present_attempted || g->color_present_point)
      return -ESTALE;
   request.graph_id = g->prepared_id; request.generation = g->prepared_generation;
   request.lock_point = g->color_lock_point; request.destination_handle = destination_handle;
   request.destination_offset = offset; request.width = surface->width; request.height = surface->height;
   request.descriptor0 = descriptor0; request.descriptor1 = descriptor1;
   const struct drm_parhelia_m9_faa_graph_present before = request;
   /* An exposed failure consumes this Lock. The fallback may only be chosen
    * before an attempt, never after the kernel has begun genuine Unlock. */
   g->color_present_attempted = true; g->color_unlock_attempted = true;
   if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_PRESENT, &request)) return -errno;
   if (request.unlock_point != before.lock_point + 0x20 ||
       request.present_point != request.unlock_point + 0x20 ||
       request.state != PARHELIA_M9_FAA_PRESENT_RETIRED) return -EPROTO;
   struct drm_parhelia_m9_faa_graph_present returned = request;
   returned.unlock_point = 0; returned.present_point = 0; returned.state = 0;
   if (memcmp(&returned, &before, sizeof(before))) return -EPROTO;
   int ret = m9_wait_for_point(screen, request.unlock_point);
   if (!ret) ret = m9_wait_for_point(screen, request.present_point);
   if (ret) return ret;
   g->color_unlock_point = request.unlock_point;
   g->color_present_point = request.present_point;
   g->color_next_attempted = false;
   return 0;
}

int
m9_faa_surface_color_unlock_kernel(struct m9_screen *screen,
                                  struct m9_faa_surface *surface)
{
   struct drm_parhelia_m9_faa_graph_unlock request = {0};
   _Static_assert(sizeof(request) == 40, "FAA COLOR Unlock kernel ABI");
   if (!screen || !surface) return -EINVAL;
   struct m9_faa_graph *graph = &surface->graph;
   if (screen->faa.chip != M9_FAA_CHIP_M9 || !screen->wait_point ||
       !m9_faa_runtime_dimensions(surface->width,surface->height) || surface->format != 0x66)
      return -EOPNOTSUPP;
   if (graph->color_program_count != 2 && graph->color_program_count != 4)
      return -EINVAL;
   if (!graph->prepared || !graph->prepared_id || !graph->initialized ||
       !graph->init_point ||
       (graph->color_next_lock_point ? !graph->color_next_attempted : graph->color_next_attempted) ||
       !graph->color_lock_attempted || !graph->color_lock_point ||
       graph->color_lock_point > UINT64_MAX - 0x20 ||
       !m9_faa_color_lock_history_valid(graph) ||
       graph->public_vs_attempted || graph->public_vs_point ||
       graph->public_vs_head_attempted || graph->public_vs_head_point ||
       graph->public_vs_draw_attempted || graph->public_vs_draw_point ||
       graph->public_vs_lock_attempted || graph->public_vs_lock_point ||
       graph->public_vs_unlock_attempted || graph->public_vs_unlock_point ||
       graph->public_vs_next_attempted || graph->public_vs_next_lock_point ||
       graph->public_vs_next_unlock_attempted || graph->public_vs_next_unlock_point ||
       graph->public_vs_follow_attempted || graph->public_vs_follow_point)
      return -EINVAL;
   if (graph->color_unlock_attempted || graph->color_unlock_point)
      return -ESTALE;
   request.graph_id = graph->prepared_id;
   request.generation = graph->prepared_generation;
   request.lock_point = graph->color_lock_point;
   const struct drm_parhelia_m9_faa_graph_unlock before = request;
   graph->color_unlock_attempted = true;
   if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_UNLOCK, &request))
      return -errno;
   if (request.graph_id != before.graph_id || request.generation != before.generation ||
       request.lock_point != before.lock_point || request.flags || request.pad ||
       request.state != PARHELIA_M9_FAA_UNLOCK_RETIRED ||
       request.unlock_point != before.lock_point + 0x20)
      return -EPROTO;
   int ret = m9_wait_for_point(screen, request.unlock_point);
   if (ret) return ret;
   graph->color_unlock_point = request.unlock_point;
   graph->color_next_attempted = false;
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
   if (graph->color_lock_attempted || graph->color_lock_point ||
       graph->color_program_count || graph->color_lock_delta || graph->color_frame_draw_count ||
       graph->color_depth_initialized || graph->color_frame_flags || graph->color_frame_maintenance_point ||
       graph->color_next_attempted || graph->color_next_previous_unlock_point ||
       graph->color_next_lock_point ||
       graph->color_unlock_attempted || graph->color_unlock_point ||
       graph->public_vs_attempted || graph->public_vs_point ||
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
   const bool next = graph->public_vs_next_attempted;
   if (next && (graph->init_point > UINT64_MAX - 0x1f0 ||
                !graph->public_vs_unlock_attempted ||
                graph->public_vs_unlock_point != graph->init_point + 0x100 ||
                graph->public_vs_next_lock_point != graph->init_point + 0x1d0))
      return -EINVAL;
   if (!next && (graph->public_vs_next_lock_point ||
                 graph->public_vs_next_unlock_attempted ||
                 graph->public_vs_next_unlock_point))
      return -EINVAL;
   bool *attempted = next ? &graph->public_vs_next_unlock_attempted :
                            &graph->public_vs_unlock_attempted;
   uint64_t *point = next ? &graph->public_vs_next_unlock_point :
                           &graph->public_vs_unlock_point;
   if (*attempted || *point)
      return -ESTALE;
   request.graph_id = graph->prepared_id;
   request.generation = graph->prepared_generation;
   request.lock_point = next ? graph->public_vs_next_lock_point : graph->public_vs_lock_point;
   const struct drm_parhelia_m9_faa_graph_unlock before = request;
   *attempted = true;
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
   *point = request.unlock_point;
   return 0;
}

int
m9_faa_surface_public_vs_next_kernel(struct m9_screen *screen,
                                    struct m9_faa_surface *surface,
                                    const uint32_t vertices[24])
{
   struct drm_parhelia_m9_faa_graph_public_vs_next request = {0};
   struct m9_faa_graph *graph;

   _Static_assert(sizeof(request) == 136, "public VS NEXT kernel ABI");
   if (!screen || !surface || !vertices)
      return -EINVAL;
   graph = &surface->graph;
   if (screen->faa.chip != M9_FAA_CHIP_M9 || !screen->wait_point ||
       surface->width != 320 || surface->height != 240 || surface->format != 0x66)
      return -EOPNOTSUPP;
   if (!graph->prepared || !graph->prepared_id || !graph->initialized ||
       !graph->init_point || graph->init_point > UINT64_MAX - 0x1d0 ||
       !graph->public_vs_head_attempted ||
       graph->public_vs_head_point != graph->init_point + 0x10 ||
       !graph->public_vs_draw_attempted ||
       graph->public_vs_draw_point != graph->init_point + 0xb0 ||
       !graph->public_vs_lock_attempted ||
       graph->public_vs_lock_point != graph->init_point + 0xe0 ||
       !graph->public_vs_unlock_attempted ||
       graph->public_vs_unlock_point != graph->init_point + 0x100 ||
       graph->public_vs_attempted || graph->public_vs_point ||
       graph->public_vs_follow_attempted || graph->public_vs_follow_point)
      return -EINVAL;
   if (graph->public_vs_next_attempted || graph->public_vs_next_lock_point ||
       graph->public_vs_next_unlock_attempted || graph->public_vs_next_unlock_point)
      return -ESTALE;
   request.graph_id = graph->prepared_id;
   request.generation = graph->prepared_generation;
   request.unlock_point = graph->public_vs_unlock_point;
   memcpy(request.vertices, vertices, sizeof(request.vertices));
   const struct drm_parhelia_m9_faa_graph_public_vs_next before = request;
   graph->public_vs_next_attempted = true;
   if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_NEXT, &request))
      return -errno;
   if (request.graph_id != before.graph_id || request.generation != before.generation ||
       request.unlock_point != before.unlock_point || request.flags || request.pad ||
       request.state != PARHELIA_M9_FAA_PUBLIC_VS_NEXT_LOCK_RETIRED ||
       request.lock_point != before.unlock_point + 0xd0 ||
       memcmp(request.vertices, before.vertices, sizeof(request.vertices)))
      return -EPROTO;
   int ret = m9_wait_for_point(screen, request.lock_point);
   if (ret)
      return ret;
   graph->public_vs_next_lock_point = request.lock_point;
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
             graph->color_lock_attempted || graph->color_lock_point ||
             graph->color_program_count || graph->color_lock_delta || graph->color_frame_draw_count ||
       graph->color_depth_initialized || graph->color_frame_flags || graph->color_frame_maintenance_point ||
       graph->color_next_attempted || graph->color_next_previous_unlock_point ||
       graph->color_next_lock_point ||
             graph->color_unlock_attempted || graph->color_unlock_point ||
             graph->public_vs_attempted || graph->public_vs_point ||
             graph->public_vs_head_attempted || graph->public_vs_head_point ||
             graph->public_vs_draw_attempted || graph->public_vs_draw_point ||
       graph->public_vs_lock_attempted || graph->public_vs_lock_point ||
       graph->public_vs_unlock_attempted || graph->public_vs_unlock_point ||
       graph->public_vs_next_attempted || graph->public_vs_next_lock_point ||
       graph->public_vs_next_unlock_attempted || graph->public_vs_next_unlock_point ||
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
   graph->color_lock_attempted = false;
   graph->color_lock_point = 0;
   graph->color_program_count = 0;
   graph->color_lock_delta = 0;
   graph->color_frame_draw_count = 0;
   graph->color_depth_initialized = false;
   graph->color_depth_mode_valid = graph->color_depth_enabled = graph->color_depth_embedded_hz = false;
   graph->color_frame_mode_planned = false;
   graph->color_frame_mode_changes = graph->color_frame_clear_flags = 0;
   graph->color_frame_flags = 0;
   graph->color_frame_maintenance_point = 0;
   graph->color_next_attempted = false;
   graph->color_next_previous_unlock_point = 0;
   graph->color_next_lock_point = 0;
   graph->color_unlock_attempted = false;
   graph->color_unlock_point = 0;
   graph->public_vs_head_attempted = false;
   graph->public_vs_head_point = 0;
   graph->public_vs_draw_attempted = false;
   graph->public_vs_draw_point = 0;
   graph->public_vs_lock_attempted = false;
   graph->public_vs_lock_point = 0;
   graph->public_vs_unlock_attempted = false;
   graph->public_vs_unlock_point = 0;
   graph->public_vs_next_attempted = false;
   graph->public_vs_next_lock_point = 0;
   graph->public_vs_next_unlock_attempted = false;
   graph->public_vs_next_unlock_point = 0;
   graph->public_vs_attempted = false;
   graph->public_vs_point = 0;
   graph->public_vs_follow_attempted = false;
   graph->public_vs_follow_point = 0;
   return 0;
}
