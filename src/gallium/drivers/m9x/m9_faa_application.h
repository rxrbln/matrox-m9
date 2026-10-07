/* SPDX-License-Identifier: MIT */
#ifndef M9_FAA_APPLICATION_H
#define M9_FAA_APPLICATION_H
#include "m9_faa_live.h"

/* Gallium semantics, not encoded GPU registers. A future typed draw ioctl
 * must admit these exact values before it can consume the owned upload. */
struct m9_faa_application_state {
   uint32_t width, height;
   struct pipe_viewport_state viewport;
   struct pipe_scissor_state scissor;
   bool scissor_enabled, depth_enabled, depth_write;
   unsigned depth_func, cull_face;
   bool front_ccw, flatshade, flatshade_first;
};

struct m9_faa_application_draw {
   struct m9_faa_live_gl live;
   const struct nir_shader *fs;
   const struct pipe_draw_info *info;
   const struct pipe_draw_start_count_bias *draw;
   const void *index_map;
   size_t index_bytes;
   struct m9_faa_application_state state;
};

struct m9_faa_application_upload {
   struct m9_faa_gl_upload shader;
   struct m9_faa_application_state state;
   /* Set only by the context snapshot from its real INIT receipt. A later
    * submit must revalidate all three against the still-retained graph. */
   uint64_t graph_id, init_point;
   uint32_t generation;
};

/* Expand topology/indices only; positions, lighting and clipping execute in
 * the live GPU program. Failure leaves output untouched. No borrowed pointer
 * survives success, and successful construction grants no native admission. */
bool m9_faa_application_build(const struct m9_faa_application_draw *draw,
                             struct m9_faa_application_upload *output,
                             const char **reason);
void m9_faa_application_fini(struct m9_faa_application_upload *upload);

/* A frame owns every live draw snapshot until its final retirement. Realloc
 * moves descriptors only: pointers inside each shader still name its owned
 * storage. There are no borrowed Gallium resources or shader pointers here.
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
