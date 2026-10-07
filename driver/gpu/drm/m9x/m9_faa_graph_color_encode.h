/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_COLOR_ENCODE_H
#define M9_FAA_GRAPH_COLOR_ENCODE_H
#include "m9_faa_graph_draw_encode.h"
#define M9_FAA_COLOR_PROGRAM_BYTES 296u
#define M9_FAA_COLOR_CONSTANT_BYTES 136u
#define M9_FAA_COLOR_GEOMETRY_BYTES 224u
#define M9_FAA_COLOR_PRIMARY_BYTES 3696u
#define M9_FAA_COLOR_SECONDARY_BYTES 1352u
#define M9_FAA_COLOR_SECONDARY_CAPACITY 2880u
#define M9_FAA_COLOR_PROGRAM_CAPACITY 4096u
#define M9_FAA_COLOR_GEOMETRY_CAPACITY (0x40000u-256u)
#define M9_FAA_COLOR_TEMPLATE_BYTES 5004u
struct m9x_faa_color_layout {
 u32 program_bytes,program_count,entry_bytes[4];
 u32 constants_bytes,constant_begin[4],constant_end[4];
 u32 geometry_bytes,vertex_offset,vertex_bytes;
};
struct m9x_faa_color_input {
 struct m9x_faa_color_layout layout;
 const u8 *program,*constants,*geometry,*declaration;
 u32 record_stride,record_count,declaration_count;
 /* Native-owned arena offset, never an input GPU address or UAPI field.
  * Each pending draw keeps disjoint CB/header/raw records in owner28. */
 u32 data_offset;
};
struct m9x_faa_color_image {
 u8 *program,*constants,*primary,*secondary;
 u32 program_bytes,constants_bytes,primary_bytes,secondary_bytes;
 u32 *secondary_length;
};
bool m9x_faa_color_template_valid(const u8 *blob,size_t bytes);
int m9x_faa_color_encode(const u8 *blob,size_t bytes,
 const struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS],
 u32 init_bytes,u64 init_point,const struct m9x_faa_color_input *input,
 const struct m9x_faa_color_image *image);
bool m9x_faa_color_four_template_valid(const u8 *blob,size_t bytes);
int m9x_faa_color_four_encode(const u8 *blob,size_t bytes,const struct m9x_faa_graph_draw_owner owner[31],
 u32 init_bytes,u64 init_point,const struct m9x_faa_color_input *input,const struct m9x_faa_color_image *image);
#endif
