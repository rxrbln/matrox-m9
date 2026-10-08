/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_SCREEN_H
#define M9_SCREEN_H

#include <stdbool.h>
#include <stdint.h>

struct pipe_screen;
struct pipe_screen_config;
struct m9_resource;
struct m9_screen;
struct m9_faa_graph;
struct m9_faa_surface;
struct m9_faa_public_upload;

struct pipe_screen *m9_screen_create(int fd,
                                     const struct pipe_screen_config *config);
int m9_bo_create(struct m9_screen *screen, uint64_t size, uint32_t flags,
                 unsigned kind, struct m9_resource *resource);
int m9_bo_create_engine_only(struct m9_screen *screen, uint64_t size,
                             unsigned kind, struct m9_resource *resource);
void m9_bo_destroy(struct m9_screen *screen, struct m9_resource *resource);
/* Caller owns a zeroed graph and retires its GPU users before destruction.
 * Creation publishes all five buffers together, or leaves the graph intact. */
int m9_faa_graph_create(struct m9_screen *screen, uint32_t width,
                        uint32_t height, struct m9_faa_graph *graph);
void m9_faa_graph_destroy(struct m9_screen *screen, struct m9_faa_graph *graph);
/* Preparation retains destination/depth references and publishes all six
 * owners atomically. It emits nothing and leaves graph.initialized false.
 * A matching target reuses its owners. The kernel retains its native owner
 * references after an initialized graph's file token is released. */
int m9_faa_surface_prepare(struct m9_screen *screen,
                           struct m9_resource *destination,
                           struct m9_resource *depth, uint64_t offset,
                           uint32_t width, uint32_t height,
                           struct m9_faa_surface *surface);
void m9_faa_surface_destroy(struct m9_screen *screen,
                            struct m9_faa_surface *surface);

/* Internal request snapshot only: no public ioctl or initialization receipt.
 * The caller retains the surface until a future kernel transaction acquires
 * every GEM owner. Capacities remain separate from the logical footprint;
 * no captured GPU address is encoded. */
enum m9_faa_init_owner_role {
   M9_FAA_INIT_PRIMARY, M9_FAA_INIT_MAIN, M9_FAA_INIT_AUXILIARY,
   M9_FAA_INIT_COMPANION, M9_FAA_INIT_HZ, M9_FAA_INIT_SENTINEL,
   M9_FAA_INIT_OWNER_COUNT,
};
struct m9_faa_init_owner {
   uint32_t handle;
   /* Mesa helper footprint; not the vendor resource+1bc clear extent. */
   uint64_t logical_bytes, capacity_bytes;
   /* Source-qualified companion/HZ clear range; zero means unmodeled. */
   uint64_t clear_bytes;
};
struct m9_faa_init_request {
   uint32_t width, height, format;
   uint32_t primary_desc, companion_desc, hz_desc, ad;
   /* Equality with the bounded source owner's page capacities, not a
    * kernel initialization result or an arbitrary-size allocation rule. */
   bool source_capacity_compatible;
   struct m9_faa_init_owner owners[M9_FAA_INIT_OWNER_COUNT];
};
/* First request class matches the qualified320x240/format66 graph only.
 * Rejection preserves the output and all retained surface owners. */
bool m9_faa_surface_init_request(const struct m9_screen *screen,
                                  const struct m9_faa_surface *surface,
                                  struct m9_faa_init_request *request);
/* Typed owner preparation only. ENOTTY on an older kernel leaves the graph
 * allocated and uninitialized. One graph may be retained per DRM file. */
int m9_faa_surface_prepare_kernel(struct m9_screen *screen,
                                   struct m9_faa_surface *surface);
/* Opt-in synchronous first-context INIT. Only the kernel's state-2/nonzero
 * point receipt marks the graph initialized; no application draw is emitted. */
int m9_faa_surface_init_kernel(struct m9_screen *screen,
                               struct m9_faa_surface *surface);
/* Submit owned, live public COPY uploads on the exact initialized surface.
 * A returned point proves preprocessing only, not an app DRAW or image. */
int m9_faa_surface_public_vs_kernel(struct m9_screen *screen,
                                  struct m9_faa_surface *surface,
                                  const struct m9_faa_public_upload *upload);
int m9_faa_surface_public_vs_head_kernel(struct m9_screen *screen,
                                  struct m9_faa_surface *surface,
                                  const struct m9_faa_public_upload *upload);
/* Submit the complete original DRAW following the observed HEAD. The returned
 * counter precedes the final raster: owners remain held for the Lock tail. */
int m9_faa_surface_public_vs_draw_kernel(struct m9_screen *screen,
                                       struct m9_faa_surface *surface);
/* Complete the original normal Lock after DRAW. Successful completion permits
 * the separately guarded F021 pixel read; failure keeps all owners retained. */
int m9_faa_surface_public_vs_lock_kernel(struct m9_screen *screen,
                                       struct m9_faa_surface *surface);
/* Finish normal Unlock after a successfully copied F021 view. Kernel admission
 * authenticates the view read; an ioctl failure must not permit a retry. */
int m9_faa_surface_public_vs_unlock_kernel(struct m9_screen *screen,
                                         struct m9_faa_surface *surface);
/* Second retained draw and complete normal Lock, with unchanged shader and
 * constants. The vertices are three raw FLOAT4 clip + FLOAT4 texcoord records.
 * F021 read and normal Unlock remain separately authenticated steps. */
int m9_faa_surface_public_vs_next_kernel(struct m9_screen *screen,
                                       struct m9_faa_surface *surface,
                                       const uint32_t vertices[24]);
/* Submit the complete two-vector COLOR draw and normal Lock with a live,
 * owned upload. Retirement and a correct F021 image are separate proofs. */
struct m9_faa_gl_upload;
int m9_faa_surface_color_lock_kernel(struct m9_screen *screen,
                                    struct m9_faa_surface *surface,
                                    const struct m9_faa_gl_upload *upload);
struct m9_faa_application_state;
int m9_faa_surface_color_next_kernel(struct m9_screen *screen,
                                    struct m9_faa_surface *surface,
                                    const struct m9_faa_gl_upload *upload,
                                    const struct m9_faa_application_state *state,
                                    uint32_t clear_flags, uint32_t clear_color);
int m9_faa_surface_color_frame_kernel(struct m9_screen *screen,
                                    struct m9_faa_surface *surface,
                                    const struct m9_faa_gl_upload *upload,
                                    const struct m9_faa_application_state *state,
                                    uint32_t clear_flags, uint32_t clear_color);
struct drm_parhelia_m9_faa_texture_view;
int m9_faa_surface_dual_first_kernel(struct m9_screen *screen,
                                    struct m9_faa_surface *surface,
                                    const struct m9_faa_gl_upload *upload,
                                    const struct drm_parhelia_m9_faa_texture_view *texture);
struct m9_faa_application_frame;
int m9_faa_surface_dual_batch_kernel(struct m9_screen *screen,
                                    struct m9_faa_surface *surface,
                                    const struct m9_faa_application_frame *frame);
int m9_faa_surface_color_batch_kernel(struct m9_screen *screen,
                                     struct m9_faa_surface *surface,
                                     const struct m9_faa_application_frame *frame);
int m9_faa_surface_color_present_kernel(struct m9_screen *screen,
                                       struct m9_faa_surface *surface,
                                       uint32_t handle, uint32_t offset,
                                       uint32_t descriptor0, uint32_t descriptor1);
int m9_faa_surface_color_unlock_kernel(struct m9_screen *screen,
                                      struct m9_faa_surface *surface);
/* Continue only the actually retired public prefix on this retained graph.
 * Success records the next preprocessing endpoint; pixels remain separate. */
int m9_faa_surface_public_vs_follow_kernel(struct m9_screen *screen,
                                         struct m9_faa_surface *surface);
/* Exact token release works after reset. Failure preserves token and owners;
 * kernel file-close still owns final cleanup if context teardown cannot release. */
int m9_faa_graph_release_kernel(struct m9_screen *screen,
                                 struct m9_faa_graph *graph);
int m9_command_bo_create(struct m9_screen *screen,
                         struct m9_resource *resource);
void m9_command_bo_destroy(struct m9_screen *screen,
                           struct m9_resource *resource);
struct pipe_context *m9_context_create(struct pipe_screen *screen, void *priv,
                                       unsigned flags);
struct pipe_fence_handle *m9_fence_create(uint64_t point, bool signaled);
int m9_wait_for_point(struct m9_screen *screen, uint64_t point);

void m9_faa_store_report(struct m9_screen *screen);

#endif
