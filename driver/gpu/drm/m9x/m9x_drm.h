/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef __M9X_DRM_H__
#define __M9X_DRM_H__

#include <uapi/drm/drm.h>

/* Preserve the provisional command numbers used by the existing Mesa code. */
#define DRM_PARHELIA_M9_CREATE_BO	0x09
#define DRM_PARHELIA_M9_SUBMIT		0x0a
#define DRM_PARHELIA_M9_GET_BO_INFO	0x0b
#define DRM_PARHELIA_M9_SURFACE_ACCESS	0x0c

#define DRM_IOCTL_PARHELIA_M9_CREATE_BO \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_CREATE_BO, \
		 struct drm_parhelia_m9_create_bo)
#define DRM_IOCTL_PARHELIA_M9_SUBMIT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_SUBMIT, \
		 struct drm_parhelia_m9_submit)
#define DRM_IOCTL_PARHELIA_M9_GET_BO_INFO \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_GET_BO_INFO, \
		 struct drm_parhelia_m9_get_bo_info)
#define DRM_IOCTL_PARHELIA_M9_SURFACE_ACCESS \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_SURFACE_ACCESS, \
		 struct drm_parhelia_m9_surface_access)

/*
 * M9 command and staging objects are immutable DMA32 allocations unless
 * PARHELIA_M9_BO_VRAM requests pinned local video memory.  COMMAND and VRAM
 * are mutually exclusive.  The mmap offset is a DRM cookie and no engine
 * address is exposed.
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
#define PARHELIA_M9_BO_VRAM	0x00000008u
/* GPU render/scanout color storage, stored tiled; implies BO_VRAM. */
#define PARHELIA_M9_BO_TILED	0x00000010u

/* Query storage and the render-node-safe mmap cookie after handle import. */
struct drm_parhelia_m9_get_bo_info {
	__u32 handle;
	__u32 memory_class; /* out */
	__u64 size; /* out */
	__u64 mmap_offset; /* out: DRM mmap cookie */
	__u32 layout; /* out: PARHELIA_M9_LAYOUT_* */
	__u32 pad;
};

#define PARHELIA_M9_MEMORY_CLASS_DMA32	1u
#define PARHELIA_M9_MEMORY_CLASS_VRAM	2u

#define PARHELIA_M9_LAYOUT_LINEAR	0u
#define PARHELIA_M9_LAYOUT_TILED	1u

/*
 * De-tiled CPU access to a rectangle of a TILED VRAM BO.  descriptor0/1 are
 * the surface's cc21xxxx descriptor and layers << 16 | height; the user
 * buffer holds top-down rows of BGRX dwords, data_stride bytes per row.
 */
#define PARHELIA_M9_ACCESS_READ		1u
#define PARHELIA_M9_ACCESS_WRITE	2u

struct drm_parhelia_m9_surface_access {
	__u32 handle;
	__u32 op;
	__u32 descriptor0;
	__u32 descriptor1;
	__u32 surface_width;
	__u32 x;
	__u32 y;
	__u32 width;
	__u32 height;
	__u32 data_stride;
	__u64 data_ptr;
	__u64 data_size;
	__u32 flags;
	__u32 pad;
};

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
	PARHELIA_M9_RELOC_SYSTEM,
	PARHELIA_M9_RELOC_PROGRAM,
	PARHELIA_M9_RELOC_STREAM,
};

/*
 * SYSTEM names a non-command DMA32 BO by its class-1 engine address (vertex
 * sources, 8d/8e constant spans); PROGRAM the VRAM c2 program backing; STREAM
 * a VRAM stream written by a preprocessing pass or read by its raster pass.
 * e3/43 values are 0x10-aligned dependency offsets below
 * PARHELIA_M9_DEPENDENCY_LIMIT, rebased onto the kernel-owned job target.
 * A PARHELIA_M9_RANGE_BREAK packet with value zero ends one execution range.
 */
#define PARHELIA_M9_DEPENDENCY_LIMIT	0x400u
#define PARHELIA_M9_RANGE_BREAK		0x4545452cu

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

enum drm_parhelia_m9_program_type {
	PARHELIA_M9_PROGRAM_NONE,
	PARHELIA_M9_PROGRAM_FIXED_P00_S28_INLINE,
};

/*
 * NOOP_FENCE requires all command and BO fields to be zero.  FIRST_TRIANGLE accepts one
 * read/write VRAM BO reference as its kernel-validated linear color target;
 * all command bytes, resource addresses and executable state remain selected
 * by the kernel.
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
	__u32 program_type;
	__u64 out_point;
};

#endif
