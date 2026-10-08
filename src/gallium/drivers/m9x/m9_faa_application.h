/* SPDX-License-Identifier: MIT */
#ifndef M9_FAA_APPLICATION_H
#define M9_FAA_APPLICATION_H
#include "m9_faa_live.h"
#include "m9_vs_source.h"
#include <drm.h>
#include "m9_faa_graph_uapi.h"

/* Context-derived level-zero view. Construction borrows this resource;
 * a successful upload retains it and copies every sampler/extent word. */
struct m9_faa_application_texture {
   struct pipe_resource *resource;
   struct drm_parhelia_m9_faa_texture_view view;
   uint64_t generation, offset, owner_bytes;
};

/* Gallium semantics, not encoded GPU registers. A future typed draw ioctl
 * must admit these exact values before it can consume the owned upload. */
struct m9_faa_application_state {
   uint32_t width, height;
   struct pipe_viewport_state viewport;
   struct pipe_scissor_state scissor;
   bool scissor_enabled, depth_enabled, depth_write;
   unsigned depth_func, cull_face;
   bool front_ccw, flatshade, flatshade_first;
   struct m9_faa_fragment_state fragment;
   /* Blend and alpha test of a textured draw, when its factors qualify. */
   struct m9_faa_texture_fragment_state texture_fragment;
   bool texture_fragment_valid, color_fragment_valid;
   /* The destination stores alpha (blending or readback may observe it). */
   bool destination_alpha;
};

struct m9_faa_application_draw {
   struct m9_faa_live_gl live;
   const struct nir_shader *fs;
   const void *fs_cb0;
   size_t fs_cb0_bytes;
   const struct pipe_draw_info *info;
   const struct pipe_draw_start_count_bias *draw;
   const void *index_map;
   size_t index_bytes;
   struct m9_faa_application_state state;
   struct m9_faa_application_texture texture[2];
};

struct m9_faa_application_upload {
   struct m9_faa_gl_upload shader;
   struct m9_faa_application_state state;
   /* Set only by the context snapshot from its real INIT receipt. A later
    * submit must revalidate all three against the still-retained graph. */
   uint64_t graph_id, init_point;
   uint32_t generation;
   uint32_t texture_count;
   struct m9_faa_application_texture texture[2];
   /* Kernel fragment word of a textured program (0: the FIRST program). */
   uint32_t fragment;
};

/* Recognize the complete live COLOR0 * TEX0 * TEX1 RGBA expression.
 * No shader instruction or vertex value is evaluated on the CPU. */
bool m9_faa_application_texture_linkage(const struct nir_shader *fs,
                                       struct m9_vs_outputs *linkage,
                                       unsigned sampler[2], unsigned *count,
                                       const char **reason);

/* Expand topology/indices only; positions, lighting and clipping execute in
 * the live GPU program. Failure leaves output untouched. No borrowed pointer
 * survives success, and successful construction grants no native admission. */
bool m9_faa_application_build(const struct m9_faa_application_draw *draw,
                             struct m9_faa_application_upload *output,
                             const char **reason);
void m9_faa_application_fini(struct m9_faa_application_upload *upload);

/* A frame owns every live draw snapshot until its final retirement. Realloc
 * moves descriptors only: pointers inside each shader still name its owned
 * storage. Texture resources are retained and sampler parameters copied;
 * borrowed shader pointers never escape construction.
 * This queue grants no native packet admission. */
struct m9_faa_application_frame {
   struct m9_faa_application_upload *draws;
   uint32_t count, capacity;
   size_t storage_bytes;
   uint32_t clear_flags, clear_color;
};

/* Move ownership only on success. A mismatch, allocation failure or aliased
 * owner leaves both inputs untouched. Only the first append records clear. */
bool m9_faa_application_frame_append(struct m9_faa_application_frame *frame,
                                    struct m9_faa_application_upload *upload,
                                    uint32_t clear_flags, uint32_t clear_color,
                                    const char **reason);
void m9_faa_application_frame_reset(struct m9_faa_application_frame *frame);
void m9_faa_application_frame_fini(struct m9_faa_application_frame *frame);
#endif
