/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_FIRST_DEPTH_TWO_APPEND_H
#define M9_FAA_GRAPH_FIRST_DEPTH_TWO_APPEND_H
#include "m9_faa_graph_first_depth_two_encode.h"
struct m9x_faa_first_depth_two_append_input {
 const struct m9x_faa_graph_draw_owner *owner;
 const u8 *old_primary,*fresh_secondary;
 u32 old_primary_bytes,old_secondary_bytes,old_draw_count;
 u32 old_last_declaration_count,fresh_secondary_bytes,declaration_count;
 u32 init_bytes;
 u64 init_point;
 const u32 *old_ends;
};
struct m9x_faa_first_depth_two_append_output {
 u8 *primary,*secondary;
 size_t primary_capacity,secondary_capacity;
 u32 *primary_bytes,*secondary_bytes,*ends;
 u64 *final_point;
};
/* The secondary output is only the newly appended job. Native retains all
 * preceding images/program/data and authenticates their declaration history.
 * Fresh secondary must be the independently encoded FIRST2 depth image. */
int m9x_faa_first_depth_two_append(const struct m9x_faa_first_depth_two_append_input *,
 const struct m9x_faa_first_depth_two_append_output *);
#endif
