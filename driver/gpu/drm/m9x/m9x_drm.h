/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef __M9X_DRM_H__
#define __M9X_DRM_H__

#include <uapi/drm/drm.h>

/* Preserve the provisional command numbers used by the existing Mesa code. */
#define DRM_PARHELIA_M9_CREATE_BO	0x09
#define DRM_PARHELIA_M9_SUBMIT		0x0a

#define DRM_IOCTL_PARHELIA_M9_CREATE_BO \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_CREATE_BO, \
		 struct drm_parhelia_m9_create_bo)
#define DRM_IOCTL_PARHELIA_M9_SUBMIT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_SUBMIT, \
		 struct drm_parhelia_m9_submit)

/*
 * M9 command and staging objects are immutable DMA32 allocations. The mmap
 * offset is a DRM cookie and no engine address is exposed.
 */
struct drm_parhelia_m9_create_bo {
	__u64 size;
	__u64 mmap_offset; /* out */
	__u32 flags;
	__u32 handle; /* out */
};

#define PARHELIA_M9_BO_COMMAND	(1u << 0)
#define PARHELIA_M9_BO_READ	(1u << 1)
#define PARHELIA_M9_BO_WRITE	(1u << 2)

/*
 * DRAW16_END only: the exclusive byte end names the upper halfword of the
 * final dword.  Access bits remain independently derived from reloc kind.
 */
#define PARHELIA_M9_RELOC_PACKED16_ODD	(1u << 3)

struct drm_parhelia_m9_bo_ref {
	__u32 handle;
	__u32 flags;
};

enum drm_parhelia_m9_reloc_kind {
	PARHELIA_M9_RELOC_REQUESTER,
	PARHELIA_M9_RELOC_COLOR,
	PARHELIA_M9_RELOC_DEPTH,
	PARHELIA_M9_RELOC_TEXTURE,
	PARHELIA_M9_RELOC_RESOURCE_69,
	PARHELIA_M9_RELOC_DRAW32_START,
	PARHELIA_M9_RELOC_DRAW32_END,
	PARHELIA_M9_RELOC_DRAW16_START,
	PARHELIA_M9_RELOC_DRAW16_END,
	PARHELIA_M9_RELOC_FENCE,
	PARHELIA_M9_RELOC_QUERY,
	PARHELIA_M9_RELOC_SPECIAL,
	PARHELIA_M9_RELOC_RESOURCE_6C,
};

struct drm_parhelia_m9_reloc {
	__u32 command_dword;
	__u32 handle;
	__u64 target_offset;
	__u32 kind;
	__u32 flags;
	__u32 pad[2];
};

#define PARHELIA_M9_SUBMIT_VALIDATE_ONLY	(1u << 0)
#define PARHELIA_M9_SUBMIT_NOOP_FENCE		(1u << 1)
#define PARHELIA_M9_SUBMIT_FIRST_TRIANGLE	(1u << 2)

/*
 * NOOP_FENCE and FIRST_TRIANGLE ignore command fields.  FIRST_TRIANGLE is a
 * literal kernel-selected diagnostic; it accepts no userspace command bytes
 * or buffer handles.
 */
struct drm_parhelia_m9_submit {
	__u32 context_id;
	__u32 command_handle;
	__u64 command_offset;
	__u32 command_length;
	__u32 relocation_count;
	__u64 relocations_ptr;
	__u32 bo_count;
	__u32 flags;
	__u64 bos_ptr;
	__u32 out_syncobj;
	__u32 pad;
	__u64 out_point;
};

#endif
