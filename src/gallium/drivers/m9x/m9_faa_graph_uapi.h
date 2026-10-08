/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_GRAPH_UAPI_H
#define M9_FAA_GRAPH_UAPI_H

/* Transitional copy of the typed kernel owner-preparation API. The canonical
 * drm-uapi declaration takes priority when the installed headers contain it. */
#ifndef DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_PREPARE
#define DRM_PARHELIA_M9_FAA_GRAPH_PREPARE 0x0e
#define DRM_PARHELIA_M9_FAA_GRAPH_RELEASE 0x0f
#define PARHELIA_M9_FAA_GRAPH_OWNERS 6u
enum drm_parhelia_m9_faa_owner_role {
	PARHELIA_M9_FAA_PRIMARY,
	PARHELIA_M9_FAA_MAIN,
	PARHELIA_M9_FAA_AUXILIARY,
	PARHELIA_M9_FAA_COMPANION,
	PARHELIA_M9_FAA_HZ,
	PARHELIA_M9_FAA_SENTINEL,
};
#define PARHELIA_M9_FAA_GRAPH_PREPARED 1u
struct drm_parhelia_m9_faa_owner {
	__u32 handle;
	__u32 pad;
	__u64 logical_bytes;
	__u64 capacity_bytes;
	__u64 clear_bytes;
};
struct drm_parhelia_m9_faa_graph_prepare {
	__u32 width, height, format;
	__u32 primary_desc, companion_desc, hz_desc, ad;
	__u32 flags;
	struct drm_parhelia_m9_faa_owner owners[PARHELIA_M9_FAA_GRAPH_OWNERS];
	/* Outputs are zero on input. A future executable path must recheck both
	 * identity and transport generation, and obtain a real init point. */
	__u64 graph_id;
	__u64 init_point;
	__u32 generation;
	__u32 state;
};
#define DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_PREPARE \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_FAA_GRAPH_PREPARE, \
		 struct drm_parhelia_m9_faa_graph_prepare)
struct drm_parhelia_m9_faa_graph_release {
	__u64 graph_id;
	__u32 generation;
	__u32 flags;
};
#define DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_RELEASE \
	DRM_IOW(DRM_COMMAND_BASE + DRM_PARHELIA_M9_FAA_GRAPH_RELEASE, \
		struct drm_parhelia_m9_faa_graph_release)

#endif

#ifndef DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_COLOR_LOCK
#define DRM_PARHELIA_M9_FAA_GRAPH_COLOR_LOCK 0x1c
#define PARHELIA_M9_FAA_COLOR_LOCK_RETIRED 16u
struct drm_parhelia_m9_faa_upload_layout {
   __u32 program_bytes, program_count, entry_bytes[4];
   __u32 constants_bytes, constant_begin[4], constant_end[4];
   __u32 geometry_bytes, vertex_offset, vertex_bytes;
};
struct drm_parhelia_m9_faa_graph_color_lock {
   __u64 graph_id, init_point, lock_point, upload;
   __u32 upload_bytes, generation, flags, state, pad;
   __u32 record_stride, record_count, declaration_count;
   struct drm_parhelia_m9_faa_upload_layout layout;
};
#define DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_COLOR_LOCK \
   DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_FAA_GRAPH_COLOR_LOCK, \
            struct drm_parhelia_m9_faa_graph_color_lock)
#endif

#ifndef DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_COLOR_NEXT
#define DRM_PARHELIA_M9_FAA_GRAPH_COLOR_NEXT 0x1d
#define PARHELIA_M9_FAA_COLOR_NEXT_LOCK_RETIRED 17u
#define PARHELIA_M9_FAA_COLOR_CLEAR_TARGET 1u
#define PARHELIA_M9_FAA_COLOR_CLEAR_DEPTH 2u
/* bit0 enabled; bit1 write; bits4:2 PIPE_FUNC_NEVER..ALWAYS. */
#define PARHELIA_M9_FAA_COLOR_DEPTH_LESS_WRITE 7u
#define PARHELIA_M9_FAA_COLOR_DEPTH_DISABLED 32u
struct drm_parhelia_m9_faa_graph_color_next {
 __u64 graph_id, previous_unlock_point, lock_point, upload;
 __u32 upload_bytes, generation, flags, state, pad;
 __u32 record_stride, record_count, declaration_count;
 struct drm_parhelia_m9_faa_upload_layout layout;
 __u32 width, height, clear_flags, clear_color, depth_flags, raster_flags;
};
#define DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_COLOR_NEXT DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_FAA_GRAPH_COLOR_NEXT, struct drm_parhelia_m9_faa_graph_color_next)
#endif

#ifndef DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_COLOR_FRAME
#define DRM_PARHELIA_M9_FAA_GRAPH_COLOR_FRAME 0x1e
#define PARHELIA_M9_FAA_COLOR_FRAME_FIRST 1u
#define PARHELIA_M9_FAA_COLOR_FRAME_INITIALIZED 2u
#define PARHELIA_M9_FAA_COLOR_FRAME_FIRST_LOCK_RETIRED 18u
#define PARHELIA_M9_FAA_COLOR_FRAME_NEXT_LOCK_RETIRED 19u
#define PARHELIA_M9_FAA_COLOR_RASTER_FLAT 1u
#define PARHELIA_M9_FAA_COLOR_RASTER_CULL_CW 2u
#define PARHELIA_M9_FAA_COLOR_RASTER_CULL_CCW 4u
struct drm_parhelia_m9_faa_graph_color_frame {
 __u64 graph_id, previous_point, lock_point, upload;
 __u32 upload_bytes, generation, flags, state, pad;
 __u32 record_stride, record_count, declaration_count;
 struct drm_parhelia_m9_faa_upload_layout layout;
 __u32 width, height, clear_flags, clear_color, depth_flags, raster_flags;
};
#define DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_COLOR_FRAME DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_FAA_GRAPH_COLOR_FRAME, struct drm_parhelia_m9_faa_graph_color_frame)
#endif

#ifndef DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_COLOR_FRAME_BATCH
/* One clear, immutable per-draw inputs, and one final Lock. All descriptors
 * and owned upload bytes must be copied and validated before the first post.
 * This schema reserves the command; unsupported profiles are not dispatched.
 * FIRST authenticates INIT, otherwise previous_point authenticates Unlock.
 * Reserved fields and output lock_point/state must be zero on input. */
#define DRM_PARHELIA_M9_FAA_GRAPH_COLOR_FRAME_BATCH 0x1f
#define PARHELIA_M9_FAA_COLOR_BATCH_FIRST_LOCK_RETIRED 20u
#define PARHELIA_M9_FAA_COLOR_BATCH_NEXT_LOCK_RETIRED 21u
struct drm_parhelia_m9_faa_color_draw {
 __u64 upload;
 __u32 upload_bytes, record_stride, record_count, declaration_count;
 struct drm_parhelia_m9_faa_upload_layout layout;
 __u32 depth_flags, raster_flags, pad[2];
};
struct drm_parhelia_m9_faa_graph_color_frame_batch {
 __u64 graph_id, previous_point, lock_point, draws;
 __u32 draw_count, generation, flags, state;
 __u32 width, height, clear_flags, clear_color;
 /* INITIALIZED output only: zero, or a genuine secondary-close global
  * point exactly previous_point+0x10. Pool completion is a separate timeline.
  * ordinary_point (output only): zero, or the retired point of the last
  * ordinary job after that idle origin; lock_point then counts from it.
  * All four words remain zero on input; the request stays80bytes. */
 union {
  __u32 pad[4];
  struct { __u64 maintenance_point; __u64 ordinary_point; };
 };
};
#define DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_COLOR_FRAME_BATCH DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_FAA_GRAPH_COLOR_FRAME_BATCH, struct drm_parhelia_m9_faa_graph_color_frame_batch)

#endif

/* A newer kernel exposes synchronous first-context initialization. Keep this
 * declaration separate so installed headers with PREPARE but without INIT
 * still compile; the old kernel will return ENOTTY at the ioctl boundary. */
#ifndef DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_INIT
#define DRM_PARHELIA_M9_FAA_GRAPH_INIT 0x10
#define PARHELIA_M9_FAA_GRAPH_INITIALIZED 2u
struct drm_parhelia_m9_faa_graph_init {
   __u64 graph_id;
   __u32 generation;
   __u32 flags;
   __u64 init_point;
   __u32 state;
   __u32 pad;
};
#define DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_INIT \
   DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_FAA_GRAPH_INIT, \
            struct drm_parhelia_m9_faa_graph_init)
#endif

#ifndef DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS
#define DRM_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS 0x16
#define PARHELIA_M9_FAA_PUBLIC_VS_PREFIX_RETIRED 10u
struct drm_parhelia_m9_faa_public_vs_layout {
   __u32 program_bytes, program_count, entry_bytes[4];
   __u32 constants_bytes, constant_begin[4], constant_end[4];
   __u32 geometry_bytes, vertex_offset, vertex_bytes;
   __u32 declaration[5];
};
struct drm_parhelia_m9_faa_graph_public_vs {
   __u64 graph_id, init_point, prefix_point;
   __u32 generation, flags, state, pad;
   struct drm_parhelia_m9_faa_public_vs_layout layout;
   __u8 program[328], constants[152], geometry[224];
};
#define DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS \
   DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS, \
            struct drm_parhelia_m9_faa_graph_public_vs)
#endif
#ifndef DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_FOLLOW
#define DRM_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_FOLLOW 0x17
#define PARHELIA_M9_FAA_PUBLIC_VS_FOLLOW_PREFIX_RETIRED 11u
struct drm_parhelia_m9_faa_graph_public_vs_follow {
   __u64 graph_id, prefix_point, follow_point;
   __u32 generation, flags, state, pad;
};
#define DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_FOLLOW \
   DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_FOLLOW, \
            struct drm_parhelia_m9_faa_graph_public_vs_follow)
#endif

#ifndef DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_HEAD
#define DRM_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_HEAD 0x18
#define PARHELIA_M9_FAA_PUBLIC_VS_HEAD_RETIRED 12u
#define DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_HEAD \
   DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_HEAD, \
            struct drm_parhelia_m9_faa_graph_public_vs)
#endif

#ifndef DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_DRAW
#define DRM_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_DRAW 0x19
#define PARHELIA_M9_FAA_PUBLIC_VS_DRAW_OBSERVED 13u
struct drm_parhelia_m9_faa_graph_public_vs_draw {
   __u64 graph_id, head_point, draw_point;
   __u32 generation, flags, state, pad;
};
#define DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_DRAW \
   DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_DRAW, \
            struct drm_parhelia_m9_faa_graph_public_vs_draw)
#endif

#ifndef DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_LOCK
#define DRM_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_LOCK 0x1a
#define PARHELIA_M9_FAA_PUBLIC_VS_LOCK_RETIRED 14u
struct drm_parhelia_m9_faa_graph_public_vs_lock {
   __u64 graph_id, draw_point, lock_point;
   __u32 generation, flags, state, pad;
};
#define DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_LOCK \
   DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_LOCK, \
            struct drm_parhelia_m9_faa_graph_public_vs_lock)
#endif

#ifndef DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_UNLOCK
#define DRM_PARHELIA_M9_FAA_GRAPH_UNLOCK 0x12
#define PARHELIA_M9_FAA_UNLOCK_RETIRED 6u
struct drm_parhelia_m9_faa_graph_unlock {
   __u64 graph_id, lock_point, unlock_point;
   __u32 generation, flags, state, pad;
};
#define DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_UNLOCK \
   DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_FAA_GRAPH_UNLOCK, \
            struct drm_parhelia_m9_faa_graph_unlock)
#endif

#ifndef DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_NEXT
#define DRM_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_NEXT 0x1b
#define PARHELIA_M9_FAA_PUBLIC_VS_NEXT_LOCK_RETIRED 15u
struct drm_parhelia_m9_faa_graph_public_vs_next {
   __u64 graph_id, unlock_point, lock_point;
   __u32 generation, flags, state, pad;
   __u32 vertices[24];
};
#define DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_NEXT \
   DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_NEXT, \
            struct drm_parhelia_m9_faa_graph_public_vs_next)
#endif
#ifndef DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_PRESENT
/* Retired Lock -> authentic Unlock -> separate-owner GPU presentation.
 * Input outputs/padding are zero. U stays immutable; P is a distinct frontier.
 * Destination descriptor is its ordinary CC21 view, never a caller GPUVA. */
#define DRM_PARHELIA_M9_FAA_GRAPH_PRESENT 0x20
#define PARHELIA_M9_FAA_PRESENT_RETIRED 22u
struct drm_parhelia_m9_faa_graph_present {
 __u64 graph_id, lock_point, unlock_point, present_point;
 __u32 destination_handle, generation, width, height;
 __u32 descriptor0, descriptor1, destination_offset, flags;
 __u32 state, pad[3];
};
#define DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_PRESENT DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_FAA_GRAPH_PRESENT, struct drm_parhelia_m9_faa_graph_present)

#endif

#ifndef DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_DUAL_FIRST
#define DRM_PARHELIA_M9_FAA_GRAPH_DUAL_FIRST 0x21
#define PARHELIA_M9_FAA_DUAL_FIRST_VERSION 1u
#define PARHELIA_M9_FAA_DUAL_FIRST_COLORPASS_DIAGNOSTIC 1u
#define PARHELIA_M9_FAA_DUAL_FIRST_LOCK_RETIRED 23u
struct drm_parhelia_m9_faa_texture_view {
 __u32 handle,profile,format,width,height,storage_kind,levels;
 __u32 min_filter,mag_filter,mip_filter,address_u,address_v;
};
struct drm_parhelia_m9_faa_graph_dual_first {
 __u64 graph_id,init_point,lock_point,upload;
 __u32 upload_bytes,generation,version,flags,state,pad;
 __u32 record_stride,record_count,declaration_count;
 struct drm_parhelia_m9_faa_upload_layout layout;
 __u32 width,height,clear_flags,clear_color,depth_flags;
 struct drm_parhelia_m9_faa_texture_view texture[2];
 /* Output only: zero, or the retired point of the last ordinary job
  * after INIT; FIRST then continued from it. Zero on input. */
 __u64 ordinary_point;
};
#define DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_DUAL_FIRST \
 DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_FAA_GRAPH_DUAL_FIRST, \
          struct drm_parhelia_m9_faa_graph_dual_first)
#endif

#ifndef DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_DUAL_BATCH
#define DRM_PARHELIA_M9_FAA_GRAPH_DUAL_BATCH 0x22
#define PARHELIA_M9_FAA_DUAL_BATCH_VERSION 1u
/* Version 2: draws point to drm_parhelia_m9_faa_dual_draw records. */
#define PARHELIA_M9_FAA_DUAL_BATCH_PROGRAMS_VERSION 2u
#define PARHELIA_M9_FAA_DUAL_BATCH_LOCK_RETIRED 24u
struct drm_parhelia_m9_faa_graph_dual_batch {
 __u64 graph_id,init_point,lock_point,draws;
 __u32 draw_count,generation,version,flags,state,pad;
 __u32 width,height,clear_flags,clear_color;
 struct drm_parhelia_m9_faa_texture_view texture[2];
 /* Output only, as in DUAL_FIRST. */
 __u64 ordinary_point;
};
#define DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_DUAL_BATCH \
 DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_FAA_GRAPH_DUAL_BATCH, \
          struct drm_parhelia_m9_faa_graph_dual_batch)
#endif

#ifndef DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_DUAL_LATER
#define DRM_PARHELIA_M9_FAA_GRAPH_DUAL_LATER 0x25
#define PARHELIA_M9_FAA_DUAL_LATER_VERSION 1u
#define PARHELIA_M9_FAA_DUAL_LATER_LOCK_RETIRED 27u
/* Version 2: draw_count descriptors at draws, upload fields zero. */
#define PARHELIA_M9_FAA_DUAL_LATER_BATCH_VERSION 2u
/* Version 3: draws point to drm_parhelia_m9_faa_dual_draw records, each with
 * its own texture pair; texture[] equals the first record's. */
#define PARHELIA_M9_FAA_DUAL_LATER_TEXTURES_VERSION 3u
#define PARHELIA_M9_FAA_DUAL_LATER_MAX_DRAWS 64u
/* Program of a version 3 DRAW; a SINGLE DRAW has no stage-1 texture. */
#define PARHELIA_M9_FAA_PROGRAM_DUAL 0u
#define PARHELIA_M9_FAA_PROGRAM_SINGLE 1u
struct drm_parhelia_m9_faa_dual_draw {
   struct drm_parhelia_m9_faa_color_draw draw;
   struct drm_parhelia_m9_faa_texture_view texture[2];
   uint32_t program, pad;
};
struct drm_parhelia_m9_faa_graph_dual_later {
 __u64 graph_id,previous_unlock_point,lock_point,upload;
 __u32 upload_bytes,generation,version,flags,state,pad;
 __u32 record_stride,record_count,declaration_count;
 struct drm_parhelia_m9_faa_upload_layout layout;
 __u32 width,height,clear_flags,clear_color,depth_flags;
 struct drm_parhelia_m9_faa_texture_view texture[2];
 __u64 draws;
 __u32 draw_count,pad2;
};
#define DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_DUAL_LATER \
 DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_FAA_GRAPH_DUAL_LATER, \
          struct drm_parhelia_m9_faa_graph_dual_later)
#endif

#endif
