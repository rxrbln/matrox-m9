/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_FIRST_DEPTH_ENCODE_H
#define M9_FAA_GRAPH_FIRST_DEPTH_ENCODE_H
#include "m9_faa_graph_depth_encode.h"
#include "m9_faa_graph_sample_clear_encode.h"
#include "m9_faa_graph_color_encode.h"
#define M9_FAA_FIRST_DEPTH_PREFIX_CAPACITY 5344u
#define M9_FAA_FIRST_DEPTH_PREFIX_ENDS 10u
static inline u32 m9x_faa_first_depth_prefix_bound(const struct m9x_faa_depth_views *views)
{
 struct m9x_faa_surface_layout layout;
 u32 color_groups,sample_groups;
 if(!views||!m9x_faa_surface_layout(views->owner[M9_FAA_BOUND_COLOR].width,
  views->owner[M9_FAA_BOUND_COLOR].height,&layout))return 0;
 color_groups=(views->owner[M9_FAA_BOUND_COLOR].logical_bytes/4096+14)/8;
 sample_groups=(layout.owner[M9_FAA_SURFACE_PRIMARY].logical/4096+14)/8;
 return 64+56+color_groups*4+52+(M9_FAA_DEPTH_CLEAR_BYTES-8)+
  M9_FAA_SAMPLE_CLEAR_TEMPLATE_BYTES+sample_groups*4+20-8+64;
}
struct m9x_faa_first_depth_scratch {
 struct m9x_faa_depth_clear_image depth;
 u8 sample[M9_FAA_SAMPLE_CLEAR_CAPACITY];
};
#define M9_FAA_FIRST_DEPTH_ENDS 14u
/* Genuine fresh INIT -> bound clear; no retained HZ/raster state is assumed.
 * The returned ten END offsets cover only primary commands. Application
 * secondary remains unposted until its complete, closed draw is appended. */
int m9x_faa_first_depth_prefix_encode(const u8 *,size_t,
 const struct m9x_faa_graph_draw_owner *,const struct m9x_faa_depth_views *,
 u64 init_point,u8 *primary,size_t capacity,u32 *length,u32 *ends,
 struct m9x_faa_first_depth_scratch *scratch);
/* Appends the closed live four-stage draw and natural normal Lock. The
 * returned point is INIT+150; source finite wait branches are GPU43 packets,
 * never synthetic CPU receipts. FIRST has a null/default auxiliary BA view. */
int m9x_faa_first_depth_encode(const u8 *sample,size_t sample_bytes,
 const u8 *first,size_t first_bytes,
 const struct m9x_faa_graph_draw_owner *owner,const struct m9x_faa_depth_views *,
 u32 init_bytes,u64 init_point,const struct m9x_faa_color_input *,
 u8 *primary,size_t primary_capacity,u8 *secondary,size_t secondary_capacity,
 u32 *primary_bytes,u32 *secondary_bytes,u32 *ends,
 const struct m9x_faa_color_image *first_scratch,
 struct m9x_faa_first_depth_scratch *prefix_scratch);
#endif
