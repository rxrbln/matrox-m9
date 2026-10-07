/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_DEPTH_H
#define M9_FAA_GRAPH_DEPTH_H
#include <linux/types.h>
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

/* Exact admitted 320x240 attachment profiles. The HZ child is 40x60;
 * allocated heights are observed profile fields, not guessed alignment rules
 * for other sizes. Native live addresses are installed after allocation. */
static inline struct m9x_faa_depth_view
m9x_faa_bound_depth_profile(enum m9x_faa_depth_role role)
{
 static const struct m9x_faa_depth_view shape[M9_FAA_DEPTH_OWNERS]={
  { .logical_bytes=0x50000,.capacity_bytes=0x50000,.width=320,.height=240,
    .profile_class=4,.format=0x7c,.allocation_kind=1,.logical_format=5,
    .element_bits=32,.allocation_width=320,.allocation_height=256,.storage_width=320 },
  { .logical_bytes=0x3000,.capacity_bytes=0x8000,.width=40,.height=60,
    .profile_class=5,.format=0x78,.allocation_kind=0,.logical_format=11,
    .element_bits=32,.allocation_width=40,.allocation_height=72,.storage_width=40 },
  { .logical_bytes=0x97000,.capacity_bytes=0x98000,.width=320,.height=240,
    .profile_class=4,.format=0x3d,.allocation_kind=0,.logical_format=0,
    .element_bits=64,.allocation_width=320,.allocation_height=240,.storage_width=320 },
 };
 if((unsigned int)role>=M9_FAA_DEPTH_OWNERS)return (struct m9x_faa_depth_view){0};
 return shape[role];
}
struct m9x_faa_depth_views {
 struct m9x_faa_depth_view owner[M9_FAA_DEPTH_OWNERS];
 u32 color_policy;
 /* Logical link indices replace original depth+1d8/HZ+1dc CPU pointers. */
 u32 depth_aux_role, aux_depth_role;
};
#endif
