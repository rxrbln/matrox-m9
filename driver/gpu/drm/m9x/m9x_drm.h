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
#define DRM_PARHELIA_M9_FAA_GRAPH_PREPARE	0x0e
#define DRM_PARHELIA_M9_FAA_GRAPH_RELEASE	0x0f
#define DRM_PARHELIA_M9_FAA_GRAPH_INIT	0x10
#define DRM_PARHELIA_M9_FAA_GRAPH_DRAW_RED	0x11
#define DRM_PARHELIA_M9_FAA_GRAPH_UNLOCK	0x12
#define DRM_PARHELIA_M9_FAA_GRAPH_NEXT_DRAW	0x13
#define DRM_PARHELIA_M9_FAA_GRAPH_NEXT_FINISH	0x14
#define DRM_PARHELIA_M9_FAA_GRAPH_NEXT_LOCK	0x15
#define DRM_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS	0x16
#define DRM_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_FOLLOW	0x17

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

/* The six roles match Mesa's m9_faa_init_request. PREPARE only retains and
 * validates the live owners. It posts no GPU work and is not an init receipt. */
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
#define PARHELIA_M9_FAA_GRAPH_INITIALIZED 2u
#define PARHELIA_M9_FAA_DRAW_PREFIX_RETIRED 3u
#define PARHELIA_M9_FAA_DRAW_FINISH_RETIRED 4u
#define PARHELIA_M9_FAA_DRAW_WITH_FINISH 1u
#define PARHELIA_M9_FAA_DRAW_WITH_LOCK 2u
#define PARHELIA_M9_FAA_LOCK_PREFIX_RETIRED 5u
#define PARHELIA_M9_FAA_UNLOCK_RETIRED 6u
#define PARHELIA_M9_FAA_NEXT_PREFIX_RETIRED 7u
#define PARHELIA_M9_FAA_NEXT_FINISH_RETIRED 8u
#define PARHELIA_M9_FAA_NEXT_LOCK_RETIRED 9u
#define PARHELIA_M9_FAA_PUBLIC_VS_PREFIX_RETIRED 10u
#define PARHELIA_M9_FAA_PUBLIC_VS_FOLLOW_PREFIX_RETIRED 11u
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

/* INIT is a single fresh first-context operation. State 2 and point become
 * visible only after the producer's actual hardware wait has retired. */
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

/* Private source-bounded first red-draw preprocessing prefix on a retained
 * graph. The caller supplies the exact INIT receipt; outputs are zero on
 * input. State 3 and draw_point require the later actual GPU wait at the
 * independent finish boundary. The original public DRAW has not returned,
 * and this is neither a pixel/readback nor a Gallium draw receipt.
 * WITH_FINISH additionally submits returned DRAW + explicit queue finish;
 * state 4 proves its retirement only, with no normal Lock/resolve claim. */
struct drm_parhelia_m9_faa_graph_draw_red {
	__u64 graph_id;
	__u64 init_point;
	__u64 draw_point;
	__u32 generation;
	__u32 flags;
	__u32 state;
	__u32 pad;
};
#define DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_DRAW_RED \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_FAA_GRAPH_DRAW_RED, \
		struct drm_parhelia_m9_faa_graph_draw_red)

/* Private initial public-VS COPY preprocessing only, mutually exclusive with
 * DRAW_RED on the same INIT receipt. Payload bytes are little-endian shader/
 * FLOAT4 input words; there are no user, CPU or GPU addresses. Exact code,
 * declaration and 320x240 shape stay guarded until wider qualification.
 * State 10 proves independent preprocessing retirement, not returned DRAW,
 * gather/position/raster, Lock or pixels. All outputs/flags/pad start zero. */
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
	__u8 program[328];
	__u8 constants[152];
	__u8 geometry[224];
};
#define DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS, \
		 struct drm_parhelia_m9_faa_graph_public_vs)

/* Separate continuation of the retained public-VS prefix. State11 proves
 * the new independent prefix wait, not public DRAW return or pixels. */
struct drm_parhelia_m9_faa_graph_public_vs_follow {
 __u64 graph_id, prefix_point, follow_point;
 __u32 generation, flags, state, pad;
};
#define DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_FOLLOW \
 DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_FOLLOW, \
          struct drm_parhelia_m9_faa_graph_public_vs_follow)

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
/* Only SUBMIT_AGGREGATE admits this zero-valued complete-job boundary. */
#define PARHELIA_M9_JOB_BREAK		0x4545452du
#define PARHELIA_M9_AGGREGATE_MAX_JOBS	4u

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
/* Default-off kernel admission; PROGRAM_NONE, 2..4 complete jobs. An empty
 * VALIDATE_ONLY|AGGREGATE request probes admission without posting. */
#define PARHELIA_M9_SUBMIT_AGGREGATE		(1u << 6)

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


/* Private same-graph normal Unlock after a successfully copied Lock view.
 * No caller-provided command data or addresses. Exact Lock receipt required;
 * state6/unlock_point only follow actual retirement of the separate finish. */
struct drm_parhelia_m9_faa_graph_unlock {
	__u64 graph_id;
	__u64 lock_point;
	__u64 unlock_point;
	__u32 generation;
	__u32 flags;
	__u32 state;
	__u32 pad;
};
#define DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_UNLOCK \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_FAA_GRAPH_UNLOCK, \
		struct drm_parhelia_m9_faa_graph_unlock)

/* Private repeated same-graph triangle preprocessing after normal Unlock.
 * Vertices are three live XYZRHW + packed diffuse records (five u32 each);
 * coordinates are float32 bits. The retained red fragment shader is unchanged.
 * State7 proves preprocessing/independent finish only, not returned draw or
 * subsequent-frame pixels. Cursors are kernel-retained and never wrap;
 * flags, outputs and padding must be zero on input. */
struct drm_parhelia_m9_faa_graph_next_draw {
	__u64 graph_id;
	__u64 unlock_point;
	__u64 draw_point;
	__u32 generation;
	__u32 flags;
	__u32 state;
	__u32 pad;
	__u32 vertices[15];
	__u32 reserved;
};
#define DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_NEXT_DRAW \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_FAA_GRAPH_NEXT_DRAW, \
		struct drm_parhelia_m9_faa_graph_next_draw)

/* Private returned second-DRAW rendering/finish on the same retained graph.
 * Requires the actual preprocessing receipt; state8 proves retirement only.
 * No second Lock or image is implied. All outputs/flags/padding start zero. */
struct drm_parhelia_m9_faa_graph_next_finish {
	__u64 graph_id;
	__u64 prefix_point;
	__u64 finish_point;
	__u32 generation;
	__u32 flags;
	__u32 state;
	__u32 pad;
};
#define DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_NEXT_FINISH \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_FAA_GRAPH_NEXT_FINISH, \
		struct drm_parhelia_m9_faa_graph_next_finish)

/* Private second normal Lock after real second rendering retirement.
 * State9 proves posted Lock-prefix retirement; F021 pixels are separate. */
struct drm_parhelia_m9_faa_graph_next_lock {
	__u64 graph_id;
	__u64 finish_point;
	__u64 lock_point;
	__u32 generation;
	__u32 flags;
	__u32 state;
	__u32 pad;
};
#define DRM_IOCTL_PARHELIA_M9_FAA_GRAPH_NEXT_LOCK \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_FAA_GRAPH_NEXT_LOCK, \
		struct drm_parhelia_m9_faa_graph_next_lock)

#endif
