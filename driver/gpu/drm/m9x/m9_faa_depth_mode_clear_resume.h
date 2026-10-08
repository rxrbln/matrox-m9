/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_DEPTH_MODE_CLEAR_RESUME_H
#define M9_FAA_DEPTH_MODE_CLEAR_RESUME_H
#include "m9_faa_depth_mode_later.h"
#define M9_FAA_CLEAR_RESUME_PRIMARY_BYTES 7740u
struct m9x_faa_clear_resume_scratch {
 struct m9x_faa_depth_mode_later_scratch later;
 u8 primary[7744],secondary[1376],program[4096],data[0x40000];
};
/* Continue ONLY a source-closed CLEAR3 prefix whose actual origin+80 read
 * has retired, with immutable enabled/nonembedded before state. Native
 * authenticates that prefix journal/owner/ranges and no pending frame.
 * previous is the original Unlock, not the prefix receipt. Seven new ENDs,
 * final previous+160. This emits no CLEAR prefix and cannot repost it.
 * Current original source profile is smooth count2,320x240,decl3/5.
 * Values/vertices/programs/addresses are rebuilt from live typed inputs. */
int m9x_faa_depth_mode_clear_resume_encode(
 const u8*sample,size_t sample_bytes,const u8*color,size_t color_bytes,
 const u8*reset,size_t reset_bytes,const struct m9x_faa_graph_draw_owner*owner,
 const struct m9x_faa_depth_views*views,u32 init_bytes,u32 primary_cursor,
 u32 secondary_cursor,u64 previous,u64 retired_prefix,
 const struct m9x_faa_depth_mode_state*before,const struct m9x_faa_color_input*input,
 struct m9x_faa_later_depth_image*out,struct m9x_faa_clear_resume_scratch*scratch,
 struct m9x_faa_depth_mode_state*after);
#endif
