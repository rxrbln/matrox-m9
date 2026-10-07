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
#endif
