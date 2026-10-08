/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_COLOR_ENCODE_H
#define M9_FAA_GRAPH_COLOR_ENCODE_H
#include "m9_faa_graph_draw_encode.h"
#include "m9_faa_graph_surface_layout.h"
#include "m9_faa_depth_state.h"
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
 /* Paired bounded fragment/interpolation mode derived from raster state.
  * Native encoders verify its actual program and utility interface. */
 u32 fragment_flags;
 /* Owned API comparison/write state, not a captured A8 word. */
 u32 depth_flags;
 /* Dual path face culling: PARHELIA_M9_FAA_COLOR_RASTER_CULL_CW/CCW. */
 u32 raster_flags;
 /* Immutable retained graph geometry, populated by native preparation.
  * Zero/zero is reserved for historical 320 source fixtures. */
 u32 width,height;
 /* Internal four-target staging for the dedicated dual-texture constructor.
  * The ordinary COLOR submit path must never publish this incomplete image. */
 bool dual_base_only;
	/* Same for the three-target single-texture constructor. */
	bool single_base_only;
 /* Explicit initial-frame diagnostic: public SELECTARG2 on two bound
  * stages, same three-varying preprocessing and immutable live owners. */
 bool dual_colorpass_diagnostic;
};
static inline int m9x_faa_color_dimensions(const struct m9x_faa_color_input *in,
 u32 init_bytes,struct m9x_faa_surface_layout *layout)
{
 u32 width=in->width,height=in->height;
 if(!width&&!height){width=320;height=240;}
 if(!((width==320&&height==240)||(width==1024&&height==768))||
  !m9x_faa_surface_layout(width,height,layout))return 0;
 /* Original INIT length includes the live companion page run. */
 return width==320?(init_bytes==4880||init_bytes==4884):init_bytes==5576;
}
static inline int m9x_faa_color_dimension_owners(
 const struct m9x_faa_graph_draw_owner *owner,
 const struct m9x_faa_surface_layout *layout)
{
 static const u32 roles[]={1,2,3,4,5,8};
 for(u32 i=0;i<M9_FAA_SURFACE_ROLES;i++){
  u32 need=(i==M9_FAA_SURFACE_MAIN||i==M9_FAA_SURFACE_AUXILIARY)?
   layout->owner[i].logical:layout->owner[i].capacity;
  if(owner[roles[i]].bytes<need)return 0;
 }
 return 1;
}
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
bool m9x_faa_color_flat_fragment_valid(const struct m9x_faa_color_input *input);
int m9x_faa_fused_source_mask(const u8 *program, u32 bytes,
 u32 begin, u16 *mask);
int m9x_faa_carry_source_mask(const u8 *program, u32 program_bytes,
 u32 begin, u32 end, u32 targets, u16 *mask);
bool m9x_faa_color_four_template_valid(const u8 *blob,size_t bytes);
int m9x_faa_color_four_encode(const u8 *blob,size_t bytes,const struct m9x_faa_graph_draw_owner owner[31],
 u32 init_bytes,u64 init_point,const struct m9x_faa_color_input *input,const struct m9x_faa_color_image *image);
#endif
