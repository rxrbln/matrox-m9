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
};
struct m9x_faa_depth_views {
 struct m9x_faa_depth_view owner[M9_FAA_DEPTH_OWNERS];
 u32 color_policy;
 /* Logical link indices replace original depth+1d8/HZ+1dc CPU pointers. */
 u32 depth_aux_role, aux_depth_role;
};
#endif
