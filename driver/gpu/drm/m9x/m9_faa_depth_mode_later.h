/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_DEPTH_MODE_LATER_H
#define M9_FAA_DEPTH_MODE_LATER_H
#include "m9_faa_depth_mode_reset.h"
#include "m9_faa_graph_later_depth_two_encode.h"
struct m9x_faa_depth_mode_later_scratch {
 struct m9x_faa_later_depth_scratch later;
 u8 reset[M9_FAA_DEPTH_MODE_RESET_CAPACITY];
};
/* Source-qualified no-explicit-CLEAR count2 caller after authentic Unlock.
 * A mode change emits the complete implicit reset (+50) before live DRAW.
 * This is not application color/depth clearing. Native authenticates the
 * retained initialized graph, origin/next and the committed before state.
 * Planned after state publishes only on actual successful Lock retirement.
 * Explicit CLEAR3+disabled remains gated at a new finite original source
 * input boundary; no completion is substituted here. */
int m9x_faa_depth_mode_later_encode(
 const u8 *sample,size_t sample_bytes,const u8 *color,size_t color_bytes,
 const u8 *reset_template,size_t reset_template_bytes,
 const struct m9x_faa_graph_draw_owner *owner,
 const struct m9x_faa_depth_views *views,u32 init_bytes,u32 primary_cursor,
 u32 secondary_cursor,u64 origin,u64 live_next,
 const struct m9x_faa_depth_mode_state *before,
 const struct m9x_faa_color_input *input,
 struct m9x_faa_later_depth_image *out,
 struct m9x_faa_depth_mode_later_scratch *scratch,
 struct m9x_faa_depth_mode_state *after);
#endif
