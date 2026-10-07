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
