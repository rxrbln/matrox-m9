/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_DUAL_FIRST_ENCODE_H
#define M9_FAA_GRAPH_DUAL_FIRST_ENCODE_H

#include "m9_faa_graph_first_depth_encode.h"
#include "m9_faa_graph_texture_dual.h"
#include "m9_faa_graph_texture_dual_link.h"

#define M9_FAA_DUAL_FIRST_SECONDARY_BYTES 2880u
#define M9_FAA_DUAL_FIRST_PRIMARY_TAIL_BYTES 3624u
#define M9_FAA_DUAL_FIRST_ENDS 14u

/* The packet template is address-free. Its SHA authenticates only immutable
 * M9128 opcodes for the admitted 320x240, four-target, two-unit profile.
 * Program, input, owner and sampler fields are supplied by live retained
 * objects. No source GPU address is an API input. */
struct m9x_faa_dual_first_input {
 const struct m9x_faa_color_input *color;
 const struct m9x_faa_texture_bind_input *texture; /* exactly two units */
 u32 program_offset;
};

int m9x_faa_dual_first_encode(const u8 *sample,size_t sample_bytes,
 const u8 *color_template,size_t color_template_bytes,
 const u8 *dual_template,size_t dual_template_bytes,
 const struct m9x_faa_graph_draw_owner owner[31],
 const struct m9x_faa_depth_views *views,u32 init_bytes,u64 init_point,
 const struct m9x_faa_dual_first_input *input,
 u8 *primary,size_t primary_capacity,u8 *secondary,size_t secondary_capacity,
 u32 *primary_bytes,u32 *ends,
 const struct m9x_faa_color_image *color_scratch,
 struct m9x_faa_first_depth_scratch *prefix_scratch);

#endif
