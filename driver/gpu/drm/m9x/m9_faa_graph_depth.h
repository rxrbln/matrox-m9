/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_DEPTH_H
#define M9_FAA_GRAPH_DEPTH_H
#include <linux/types.h>
#include "m9_faa_graph_surface_layout.h"
/* Source roles31/32/33 are distinct from the six borrowed graph owners.
 * This is an internal ownership contract, not a userspace address payload. */
enum m9x_faa_depth_role {
 M9_FAA_BOUND_COLOR, M9_FAA_DEPTH_HZ, M9_FAA_BOUND_DEPTH, M9_FAA_DEPTH_OWNERS
};
enum m9x_faa_depth_color_policy {
 M9_FAA_DEPTH_COLOR_SYSTEM_A, M9_FAA_DEPTH_COLOR_SYSTEM_B
};
struct m9x_faa_depth_view {
 u32 base, logical_bytes, capacity_bytes, width, height, profile_class, format;
 /* Original CPU resource fields used by b5e70/b5030, independent of base.
  * +164/+184/+188/+18c describe storage, not visible rectangle dimensions. */
 u32 allocation_kind, logical_format, element_bits;
 u32 allocation_width, allocation_height, storage_width;
};

/* Allocation/profile producers are independent of live placement. Source
 * class/format/kind stays fixed; geometry/clear/backing follows the original
 * typed allocator. Command consumers remain separately dimension-gated. */
static inline struct m9x_faa_depth_view
m9x_faa_bound_depth_profile_dimensions(enum m9x_faa_depth_role role,
 u32 width,u32 height)
{
 struct m9x_faa_surface_layout layout;
 struct m9x_faa_depth_view v={0};
 const struct m9x_faa_surface_geometry *g;
 const struct m9x_faa_surface_extent *e;
 if((unsigned int)role>=M9_FAA_DEPTH_OWNERS||
  !((width==320&&height==240)||(width==1024&&height==768))||
  !m9x_faa_surface_layout(width,height,&layout))return v;
 if(role==M9_FAA_BOUND_COLOR){
  g=&layout.primary;e=&layout.owner[M9_FAA_SURFACE_PRIMARY];
  v.profile_class=4;v.format=0x7c;v.allocation_kind=1;
  v.logical_format=5;v.element_bits=32;v.logical_bytes=e->logical;
 }else if(role==M9_FAA_DEPTH_HZ){
  g=&layout.hz;e=&layout.owner[M9_FAA_SURFACE_HZ];
  v.profile_class=5;v.format=0x78;v.logical_format=11;
  v.element_bits=32;v.logical_bytes=e->clear;
 }else{
  g=&layout.companion;e=&layout.owner[M9_FAA_SURFACE_COMPANION];
  v.profile_class=4;v.format=0x3d;v.element_bits=64;
  v.logical_bytes=e->clear;
 }
 v.capacity_bytes=e->capacity;v.width=g->width;v.height=g->height;
 v.allocation_width=g->allocation_width;v.allocation_height=g->allocation_height;
 v.storage_width=g->storage_width;return v;
}
static inline struct m9x_faa_depth_view
m9x_faa_bound_depth_profile(enum m9x_faa_depth_role role)
{return m9x_faa_bound_depth_profile_dimensions(role,320,240);}
struct m9x_faa_depth_views {
 struct m9x_faa_depth_view owner[M9_FAA_DEPTH_OWNERS];
 u32 color_policy;
 /* Logical link indices replace original depth+1d8/HZ+1dc CPU pointers. */
 u32 depth_aux_role, aux_depth_role;
};
#endif
