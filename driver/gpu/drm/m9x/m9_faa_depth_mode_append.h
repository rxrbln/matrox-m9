/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_DEPTH_MODE_APPEND_H
#define M9_FAA_DEPTH_MODE_APPEND_H
#include "m9_faa_depth_mode_reset.h"
#include "m9_faa_graph_batch_limits.h"
#define M9_FAA_DEPTH_MODE_MAX_ENDS (14+3*(M9_FAA_FRAME_BATCH_MAX_DRAWS-1))
struct m9x_faa_depth_mode_append_input {
 const struct m9x_faa_graph_draw_owner *owner;
 const struct m9x_faa_depth_views *views;
 const u8 *reset_template, *old_primary, *fresh_secondary;
 size_t reset_template_bytes;
 u32 old_primary_bytes, old_secondary_bytes, old_last_secondary_bytes;
 u32 old_draw_count, old_last_declaration_count, declaration_count;
 u32 fresh_secondary_bytes, primary_start, depth_flags;
 u32 secondary_start, initial_end_count;
 const u32 *old_ends;
 u32 old_end_count;
 u64 old_final_point;
 struct m9x_faa_depth_mode_state before;
};
struct m9x_faa_depth_mode_append_image {
 u8 *primary, *secondary, *reset_scratch;
 size_t primary_capacity, secondary_capacity, reset_capacity;
 u32 *ends;
 u32 end_capacity;
 u32 primary_bytes, secondary_bytes, end_count;
 u64 final_point;
 struct m9x_faa_depth_mode_state after;
};
/* Source-qualified count2 append within the current CPU recording.
 * Native authenticates immutable history and supplies the current state and
 * point; this planner never publishes state or claims retirement. Fresh
 * color2 secondary is encoded at old_final_point+10, equivalently the FIRST2
 * helper receives old_final_point-60 (its own +70 bias). Output secondary is
 * only the new job; preceding code/data/secondary remain retained unchanged.
 * Same-mode jobs emit the existing cached secondary-save grammar (+60).
 * primary_start/secondary_start are live cursors on the true retained owners.
 * initial_end_count is14 FIRST,13 initialized CLEAR3,4 initialized no-clear,
 * or7 initialized no-clear mode reset; native authenticates that profile.
 */
int m9x_faa_depth_mode_append_encode(
 const struct m9x_faa_depth_mode_append_input *,
 struct m9x_faa_depth_mode_append_image *);
#endif
