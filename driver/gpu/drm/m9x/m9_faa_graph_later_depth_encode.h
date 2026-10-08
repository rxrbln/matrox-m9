/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_LATER_DEPTH_ENCODE_H
#define M9_FAA_GRAPH_LATER_DEPTH_ENCODE_H
#include "m9_faa_graph_color_next_encode.h"
#include "m9_faa_graph_depth_encode.h"
#include "m9_faa_graph_sample_clear_encode.h"
#define M9_FAA_LATER_DEPTH_OWNERS 34u
#define M9_FAA_LATER_DEPTH_PREFIX_CAPACITY 4736u
#define M9_FAA_LATER_DEPTH_PRIMARY_CAPACITY (M9_FAA_LATER_DEPTH_PREFIX_CAPACITY+3632u)
#define M9_FAA_LATER_DEPTH_ENDS 13u
struct m9x_faa_later_depth_scratch {
 struct m9x_faa_attachment_clear_image attachment;
 struct m9x_faa_depth_clear_image depth;
 u8 sample[M9_FAA_SAMPLE_CLEAR_CAPACITY];
 u8 prefix[M9_FAA_LATER_DEPTH_PREFIX_CAPACITY];
 u8 tail[M9_FAA_COLOR_PRIMARY_BYTES];
 u8 first_secondary[M9_FAA_COLOR_SECONDARY_CAPACITY];
 u8 next_secondary[M9_FAA_COLOR_SECONDARY_CAPACITY];
};
struct m9x_faa_later_depth_image {
 struct m9x_faa_color_image image;
 u32 primary_length,secondary_length;
 u32 end_bytes[M9_FAA_LATER_DEPTH_ENDS],end_count;
 u32 clear_bytes,draw_end_bytes;
 u64 final_point;
};
/* Original initialized depth owner reuse after authenticated native Unlock.
 * owner31..33 are the retained SYSTEM/HZ/depth views. This pure encoder maps,
 * submits, retires and reclaims nothing. previous_unlock authenticates the
 * caller history; live_next_point must describe the source's next epoch.
 * The currently qualified relation is live_next=previous_unlock+10.
 * Output primary starts at live primary_cursor; END offsets are absolute in
 * that owned buffer. Native caller retains all owners until real retirement.
 * Only clear0 or full black TARGET|ZBUFFER clear3/depth1/LESS-write are admitted.
 * No FIRST constructors or HZ initialization are replayed. */
int m9x_faa_later_depth_encode(const u8 *sample,size_t sample_bytes,
 const u8 *first,size_t first_bytes,
 const struct m9x_faa_graph_draw_owner owner[M9_FAA_LATER_DEPTH_OWNERS],
 const struct m9x_faa_depth_views *,u32 init_bytes,u32 primary_cursor,
 u32 secondary_cursor,u64 previous_unlock,u64 live_next_point,u32 clear_flags,
 const struct m9x_faa_color_input *,struct m9x_faa_later_depth_image *,
 struct m9x_faa_later_depth_scratch *);
#endif
