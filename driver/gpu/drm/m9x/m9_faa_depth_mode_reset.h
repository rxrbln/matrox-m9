/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_DEPTH_MODE_RESET_H
#define M9_FAA_DEPTH_MODE_RESET_H
#include "m9_faa_depth_mode.h"
#define M9_FAA_DEPTH_MODE_RESET_TEMPLATE_BYTES 4156u
#define M9_FAA_DEPTH_MODE_RESET_CAPACITY 4172u
struct m9x_faa_depth_mode_reset_image {
 u8 *primary;
 size_t capacity;
 u32 bytes, end_bytes[3];
 u64 final_point;
};
/* Original implicit association reset. All 30 address fields/eight timeline
 * sites in the immutable internal program template are placeholders. This
 * does not clear application color/depth, publish state, or invent a fence.
 * epoch is the source reset's first token; final token is epoch+40. */
int m9x_faa_depth_mode_reset_encode(const u8 *blob,size_t bytes,
 const struct m9x_faa_graph_draw_owner *owner,const struct m9x_faa_depth_views *views,
 const struct m9x_faa_depth_mode_state *before,bool resolved_primary,u64 epoch,
 struct m9x_faa_depth_mode_reset_image *out);
/* Already-qualified pool-save control in unindexed primary grammar, used
 * before an in-batch mode reset. Relative owner identity remains retained. */
int m9x_faa_depth_mode_save_encode(const struct m9x_faa_graph_draw_owner *,
 u8 *,size_t);
#endif
