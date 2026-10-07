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
#define DRM_PARHELIA_M9_WAIT_POINT	0x0d

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

/* Query/wait never posts a command. timeout_ns=0 polls, U64_MAX waits up
 * to the driver's bounded watchdog. point=0 is a capability/last-observed-progress
 * query. flags and pad must be zero. -ETIME leaves work pending. */
struct drm_parhelia_m9_wait_point {
	__u64 point;
	__u64 timeout_ns;
	__u64 completed_point;
	__u32 flags;
	__u32 pad;
};
#define DRM_IOCTL_PARHELIA_M9_WAIT_POINT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_WAIT_POINT, \
		 struct drm_parhelia_m9_wait_point)

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
/*
 * Video memory the engine alone touches.  It is placed above the host
 * aperture's identity window, so the object has no mmap cookie and no CPU
 * mapping is ever possible; requires BO_VRAM and excludes BO_TILED.
 */
#define PARHELIA_M9_BO_ENGINE_ONLY 0x00000020u

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
 * Format modifiers for the storage m9_render_surface_layout() produces.
 *
 * Matrox has no vendor namespace in drm_fourcc.h, so 0x4d ('M') is this
 * out-of-tree driver's own choice; it is deliberately far from the
 * sequentially allocated upstream ids (0x00..0x0c at the time of writing) and
 * carries no upstream registration.  Treat it as private to this driver until
 * a namespace is allocated.
 *
 * The modifier names the surface's *shape* contract, which is all an importer
 * can act on: the texel order inside a band is the render address
 * calculator's own and is not an affine function of the coordinates
 * (MEASURED), so a modifier cannot describe it and no importer may assume it.
 * A buffer carrying one of these must be read back through the engine (the
 * surface-access ioctl or a sampler bound with the matching descriptor kind),
 * never as linear rows.
 */
#define DRM_FORMAT_MOD_VENDOR_MATROX	0x4d
#define fourcc_mod_matrox_code(val) \
	((((__u64)DRM_FORMAT_MOD_VENDOR_MATROX) << 56) | \
	 ((val) & 0x00ffffffffffffffULL))

/*
 * The ordinary render tiling, used above 64 texels in either dimension:
 * the row span is padded to 32 texels, the allocation height to 32 rows, and
 * the whole surface to 4 KiB at 32 bpp (2 KiB at 16 bpp).  This is the
 * storage a colour target, a scanout surface and an FBO sampled through
 * descriptor kind 0x21 all carry.
 */
#define DRM_FORMAT_MOD_MATROX_M9_TILED		fourcc_mod_matrox_code(1)
/*
 * The distinct family the aperture address calculator stores at 64x64 and
 * below: the row span is still padded to 32 texels, but the allocation height
 * is padded to 8 rows at 32 bpp (4 at 16 bpp, and 16 rows up to 16x16) and
 * the surface to 1 KiB (512 bytes at 16 bpp).  Reading it with the ordinary
 * kind returns only the first 16x16 texels (MEASURED), so it cannot share one
 * modifier with the tiling above.
 */
#define DRM_FORMAT_MOD_MATROX_M9_TILED_SMALL	fourcc_mod_matrox_code(2)

/*
 * De-tiled CPU access to a rectangle of a TILED VRAM BO.  descriptor0/1 are
 * the surface's cc21xxxx descriptor and layers << 16 | height; the user
 * buffer holds top-down rows of BGRX dwords, data_stride bytes per row.
 * With PARHELIA_M9_ACCESS_TEXTURE the BO is a VRAM texture, offset is the
 * byte offset of one level inside it, descriptor0 is format << 24 |
 * kind << 16 | width and rows hold elements of that format's size.
 */
#define PARHELIA_M9_ACCESS_READ		1u
#define PARHELIA_M9_ACCESS_WRITE	2u
#define PARHELIA_M9_ACCESS_TEXTURE	(1u << 0)
/* Private retired FAA output only: descriptor c8210140/100f0, 320x240
 * RGBA rows. The handle identifies the exact original sampled color BO;
 * it does not expose its sampled storage through a different descriptor. */
#define PARHELIA_M9_ACCESS_FAA_OUTPUT	(1u << 1)

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
	__u32 offset;
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
	/* The multisample fragment store; admitted only while the probe mask
	 * names the index that binds it. */
	PARHELIA_M9_RELOC_FRAGMENT,
	/* Vendor kind2b: tagged VRAM pool6 state storage at index24.
	 * Validation requires the complete captured surface-binding record.
	 * Execution awaits a kernel-owned pool initialization contract. */
	PARHELIA_M9_RELOC_FAA_POOL,
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
/* Opt-in retention beyond the next submit. The client must wait out_point
 * before overwriting CPU-visible inputs; legacy flags-zero remains one job. */
#define PARHELIA_M9_SUBMIT_QUEUED		(1u << 3)
/* Private source-qualified continuation after FIRST_TRIANGLE's returned
 * normal Lock. Reuses the same color handle; the kernel owns the suffix. */
#define PARHELIA_M9_SUBMIT_FAA_UNLOCK		(1u << 4)
/* Private reverse conversion after a retired normal Unlock. The caller
 * must use the same retained color handle and read F021 only after return. */
#define PARHELIA_M9_SUBMIT_FAA_RELOCK		(1u << 5)

enum drm_parhelia_m9_program_type {
	PARHELIA_M9_PROGRAM_NONE,
	PARHELIA_M9_PROGRAM_FIXED_P00_S28_INLINE,
};

/*
 * NOOP_FENCE requires all command and BO fields to be zero. FIRST_TRIANGLE
 * accepts one read/write VRAM BO reference as its kernel-validated color
 * target; FAA_UNLOCK accepts that same retained handle only after the
 * source-matched Lock view. Both use kernel-owned executable state.
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
