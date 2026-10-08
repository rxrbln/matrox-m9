/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_LATER_DEPTH_ENCODE_H
#define M9_FAA_GRAPH_LATER_DEPTH_ENCODE_H
#include "m9_faa_graph_color_next_encode.h"
#include "m9_faa_graph_depth_encode.h"
#include "m9_faa_graph_sample_clear_encode.h"
#define M9_FAA_LATER_DEPTH_OWNERS 34u
#define M9_FAA_LATER_DEPTH_PREFIX_CAPACITY 5424u
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
/* Appends one same-frame DRAW before the still-unposted3572-byte Lock tail.
 * old_secondary_bytes is the accumulated later suffix, beginning at the
 * caller's retained secondary_start. Output secondary contains only the new
 * declaration-sized stage (2676/2700 bytes); retained stages are never rewritten.
 * extra_first_secondary is independently regenerated from that job's live
 * input and code/CB ownership, with C2 entries already placed by the caller.
 * raster_flags is the source-qualified common raster state (FLAT/CW/CCW).
 * Cull inserts24 bytes into only the initial stage; cached additional stages
 * retain their2700-byte grammar. The caller applies the raster encoder to
 * both the initial image and each independently generated FIRST stage.
 * Source declaration3 removes one24-byte group; declaration5 retains it.
 * Native caller authenticates the immutable full per-job declaration history;
 * this helper joins the typed last count to its actual ranges and checks the
 * count-derived total extent. No arbitrary packet shapes.
 */
struct m9x_faa_later_depth_append_image {
 u8 *primary,*secondary;
 u32 primary_capacity,secondary_capacity;
 u32 primary_bytes,secondary_bytes,end_bytes[M9_FAA_LATER_DEPTH_ENDS],end_count;
 u32 draw_end_bytes;
 u64 final_point;
};
int m9x_faa_later_depth_append_encode(
 const struct m9x_faa_graph_draw_owner owner[M9_FAA_LATER_DEPTH_OWNERS],
 const struct m9x_faa_depth_views *,u32 primary_start,u32 secondary_start,
 u64 previous_unlock,u32 clear_flags,u32 raster_flags,u32 old_draw_count,u32 old_last_declaration_count,
 const u8 *old_primary,u32 old_primary_bytes,u32 old_secondary_bytes,
 const u32 *old_end_bytes,u32 old_end_count,u64 old_final_point,
 const u8 *extra_first_secondary,u32 extra_first_bytes,
 const struct m9x_faa_color_input *,
 struct m9x_faa_later_depth_append_image *);
#endif
