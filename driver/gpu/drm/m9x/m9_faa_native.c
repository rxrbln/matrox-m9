// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Private replay of the coherent surface-owned M9128 vendor FAA frame.
 * Command grammar and source validity come from the normal CPU capture.
 * This diagnostic exposes neither arbitrary indirect execution nor app MSAA.
 * Opaque stores, HZ, pool and allocation tails receive no fabricated memset.
 */
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/iosys-map.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>

#ifdef CONFIG_X86_64
#include <asm/memtype.h>
#include <asm/set_memory.h>
#endif
#include <linux/uaccess.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>

#include "m9_drv.h"
#include "m9_faa_graph_present_encode.h"
#include "m9_faa_graph_init_dimensions.h"
#include "m9_reg.h"
#include "m9_faa_output.h"
#include "m9_faa_graph_draw_encode.h"
#include "m9_faa_graph_color_encode.h"
#include "m9_faa_graph_color_next_encode.h"
#include "m9_faa_graph_depth_encode.h"
#include "m9_faa_graph_sample_clear_encode.h"
#include "m9_faa_graph_first_depth_encode.h"
#include "m9_faa_graph_later_depth_encode.h"
#include "m9_faa_graph_later_depth_two_encode.h"
#include "m9_faa_graph_frame_batch_input.h"
#include "m9_faa_graph_dual_batch_input.h"
#include "m9_faa_graph_frame_first_append_encode.h"
#include "m9_faa_graph_first_depth_two_encode.h"
#include "m9_faa_graph_first_depth_two_append.h"
#include "m9_faa_graph_program_arena.h"
#include "m9_faa_primary_pool.h"
#include "m9_faa_graph_depth_draw_encode.h"
#include "m9_faa_graph_depth_raster_encode.h"
#include "m9_faa_graph_depth.h"
#include "m9_faa_graph_public_vs_encode.h"
#include "m9_faa_graph_public_vs_follow_encode.h"
#include "m9_faa_graph_public_vs_draw_encode.h"
#include "m9_faa_graph_public_vs_lock_encode.h"
#include "m9_faa_graph_public_vs_next_encode.h"
#include "m9_faa_graph_unlock_encode.h"
#include "m9_faa_graph_next_encode.h"
#include "m9_faa_graph_next_finish_encode.h"
#include "m9_faa_graph_dual_first_encode.h"
#include "m9_faa_graph_single_first_encode.h"
#include "m9_faa_graph_mixed_draw.h"
#include "m9_faa_graph_dual_colorpass_encode.h"
#include "m9_faa_graph_dual_first_append.h"
#include "m9_faa_graph_dual_later_encode.h"
#include "m9_faa_graph_texture_owner.h"
#include "m9_faa_graph_draw_tail_encode.h"
#include "m9_faa_graph_lock_encode.h"
#include "m9_faa_depth_mode_batch.h"
#include "m9_faa_depth_mode_later.h"
#include "m9_faa_depth_mode_clear_resume.h"

struct m9_faa_owner_spec {
	u32 captured, bytes;
	bool local;
	/* Per-owner domain/policy; historical DMA-physical owners stay false.
	 * Only a source-closed generated output template may select these. */
	bool system, flags30;
	u32 mapped_bytes;
};
struct m9_faa_span {
	u32 owner, offset, blob_offset, bytes;
};
struct m9_faa_fixup {
	u32 dst, offset, src, addend, encoder, tag, captured;
};
struct m9_faa_tracker {
	u32 owner, offset, captured;
};
struct m9_faa_clear_segment {
	u32 owner, bytes, start, end;
};
#if (defined(M9_FAA_CONSTANT_SHADER_CONTROL) + \
     defined(M9_FAA_CONSTANT_RED_CONTROL) + \
     defined(M9_FAA_ORDINARY_SYSTEM_CONTROL) + \
     defined(M9_FAA_INIT_ONLY_CONTROL) + \
     defined(M9_FAA_GRAPH_INIT_CONTROL) + \
     defined(M9_FAA_PRECLEAR_CONTROL) + \
     defined(M9_FAA_PREPARED_PRECLEAR_CONTROL) + \
     defined(M9_FAA_PREPARED_FLOW_CONTROL) + \
     defined(M9_FAA_PRECLEAR_DRAW_CONTROL) + \
     defined(M9_FAA_PRECLEAR_OUTPUT_CONTROL) + \
     defined(M9_FAA_PRECLEAR_OUTPUT_WRITE_CONTROL) + \
     defined(M9_FAA_PRECLEAR_INPLACE_OUTPUT_CONTROL) + \
     defined(M9_FAA_PRECLEAR_INPLACE_COPY_CONTROL) + \
     defined(M9_FAA_PUBLIC_SHARED_DEPTH_FLUSH_CONTROL) + \
     defined(M9_FAA_PUBLIC_SHARED_DEPTH_RESOLVE_CONTROL) + \
     defined(M9_FAA_PUBLIC_SHARED_DEPTH_RETURNED_CONTROL) + \
     defined(M9_FAA_PUBLIC_SHARED_DEPTH_REPLACEMENT_CONTROL) + \
     defined(M9_FAA_PUBLIC_SHARED_DEPTH_REPLACEMENT_SUBMIT_CONTROL) + \
     defined(M9_FAA_PUBLIC_SHARED_DEPTH_REPLACEMENT_RETURNED_CONTROL) + \
     defined(M9_FAA_POST_TRIANGLE_CONTROL)) > 1
#error "Select only one complete private FAA source profile"
#endif

#if defined(M9_FAA_ORDINARY_SYSTEM_CONTROL)
/* Whole ordinary public draw into its own SYSTEM owner. No sampled caller
 * color, FAA activation, companion clear or resolve is present in this flow. */
#include "m9_ordinary_system_red_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/ordinary-system-constant-red-known-inputs.bin"
#elif defined(M9_FAA_INIT_ONLY_CONTROL)
/* Independent vendor queue finish at the returned context constructor.
 * No triangle or output request, and no invented output owner. */
#include "m9_faa_init_only_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-init-only-known-inputs.bin"
#elif defined(M9_FAA_GRAPH_INIT_CONTROL)
/* Six source owners are borrowed from a PREPARED graph; the remaining
 * private owners and the fresh queue primary follow the sealed source plan. */
#include "m9_faa_graph_init_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-graph-init-known-inputs.bin"
#elif defined(M9_FAA_PREPARED_PRECLEAR_CONTROL)
/* Fresh source constructor/preparation precedes queue creation. Retain and
 * publish its otherwise unconsumed local conversion backing before c0. */
#include "m9_faa_prepared_preclear_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-prepared-preclear-known-inputs.bin"
#elif defined(M9_FAA_PREPARED_FLOW_CONTROL)
/* Whole continuation of the fresh prepared source. Its generated phases
 * and program access inventory are part of this profile's source proof. */
#include "m9_faa_prepared_flow_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-prepared-flow-known-inputs.bin"
#elif defined(M9_FAA_PRECLEAR_CONTROL)
/* Completed public black CLEAR42, then the original draw stopped at its
 * first GPU query. Independent finish; no completed triangle or output. */
#include "m9_faa_preclear_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-preclear-known-inputs.bin"
#elif defined(M9_FAA_PRECLEAR_DRAW_CONTROL)
/* Completed public black clear, then the original triangle preprocessing
 * launch stopped at its next GPU wait. Earlier inputs remain byte-exact. */
#include "m9_faa_preclear_draw_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-preclear-draw-known-inputs.bin"
#elif defined(M9_FAA_PRECLEAR_OUTPUT_CONTROL)
/* Returned public triangle, then the genuine output format transition
 * stopped at its next GPU wait. The output exists but is not readable. */
#include "m9_faa_preclear_output_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-preclear-output-known-inputs.bin"
#elif defined(M9_FAA_PRECLEAR_OUTPUT_WRITE_CONTROL)
/* The original output path posted its complete resolve before waiting
 * for CPU access. Preserve that posted prefix and its genuine footer. */
#include "m9_faa_preclear_output_write_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-preclear-output-write-known-inputs.bin"
#elif defined(M9_FAA_POST_TRIANGLE_CONTROL)
/* Independent vendor queue finish after the returned public triangle,
 * before output/resolve. Its final counter is in its counted secondary. */
#include "m9_faa_post_triangle_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-post-triangle-known-inputs.bin"
#elif defined(M9_FAA_PRECLEAR_INPLACE_OUTPUT_CONTROL)
/* Genuine OS allocation failure selected the sampled destination. Its
 * original format-transition wait and rectangle launch remain pending. */
#include "m9_faa_preclear_inplace_output_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-preclear-inplace-output-known-inputs.bin"
#elif defined(M9_FAA_PRECLEAR_INPLACE_COPY_CONTROL)
/* Original sampled resolve has emitted its rectangle and CPU access
 * preparation, then stopped at a new wait. No CPU pixels are supplied. */
#include "m9_faa_preclear_inplace_copy_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-preclear-inplace-copy-known-inputs.bin"
#elif defined(M9_FAA_PUBLIC_SHARED_DEPTH_FLUSH_CONTROL)
/* Public depth state and a shared-primary target change select the genuine
 * dirty-graph flush. Its original CPU wait and readback remain pending. */
#include "m9_faa_public_shared_depth_flush_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-public-shared-depth-flush-known-inputs.bin"
#elif defined(M9_FAA_PUBLIC_SHARED_DEPTH_RESOLVE_CONTROL)
/* The genuine public dirty-graph flush has dispatched its mode9 resolve,
 * then stops at its original cleanup wait. CPU readback remains pending. */
#include "m9_faa_public_shared_depth_resolve_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-public-shared-depth-resolve-known-inputs.bin"
#elif defined(M9_FAA_PUBLIC_SHARED_DEPTH_RETURNED_CONTROL)
/* Genuine public aa750 has returned after its source-emitted cleanup and
 * fragment-pointer reset. Replacement draw and CPU readback remain stopped. */
#include "m9_faa_public_shared_depth_returned_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-public-shared-depth-returned-known-inputs.bin"
#elif defined(M9_FAA_PUBLIC_SHARED_DEPTH_REPLACEMENT_CONTROL)
/* The original outer caller continues after its returned dirty flush and
 * prepares the replacement draw, then stops before a new GPU wait. */
#include "m9_faa_public_shared_depth_replacement_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-public-shared-depth-replacement-known-inputs.bin"
#elif defined(M9_FAA_PUBLIC_SHARED_DEPTH_REPLACEMENT_SUBMIT_CONTROL)
/* The original replacement renderer reaches its second finite transition
 * wait. Its query and independent finish retain their source timeline. */
#include "m9_faa_public_shared_depth_replacement_submit_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-public-shared-depth-replacement-submit-known-inputs.bin"
#elif defined(M9_FAA_PUBLIC_SHARED_DEPTH_REPLACEMENT_RETURNED_CONTROL)
/* Original public replacement DRAW has genuinely returned. Its newly
 * emitted rendering commands still require this independent retirement. */
#include "m9_faa_public_shared_depth_replacement_returned_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-public-shared-depth-replacement-returned-known-inputs.bin"
#elif defined(M9_FAA_CONSTANT_RED_CONTROL)
/* A distinct whole public source capture changes c0 through SETCONST.
 * Never patch captured green command or program bytes in place. */
#include "m9_faa_constant_red_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-constant-red-known-inputs.bin"
#elif defined(M9_FAA_CONSTANT_SHADER_CONTROL)
/* Whole independently captured public CREATEPS/SETCONST/SETPS flow. The
 * input blob and all owner/fixup tables are one source profile; never splice
 * shader bytes or captured secondary lengths into the normal profile. */
#include "m9_faa_constant_green_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-constant-green-known-inputs.bin"
#else
#include "m9_faa_native_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-normal-known-inputs.bin"
#endif
#ifndef M9_FAA_OUTPUT_PENDING
#define M9_FAA_OUTPUT_PENDING 0
#endif
/* The older sampled-destination template has no ordinary output owner.
 * It must never be advertised as a completed ac1f0 output contract. */
#ifndef M9_FAA_OUTPUT_OWNER
#define M9_FAA_OUTPUT_OWNER U32_MAX
#endif
#ifndef M9_FAA_COUNTER_VIRTUAL
#define M9_FAA_COUNTER_VIRTUAL 0
#endif
#if defined(M9_FAA_PREPARED_FLOW_CONTROL) || \
    defined(M9_FAA_PRECLEAR_OUTPUT_WRITE_CONTROL) || \
    defined(M9_FAA_PRECLEAR_INPLACE_OUTPUT_CONTROL) || \
    defined(M9_FAA_PRECLEAR_INPLACE_COPY_CONTROL) || \
    defined(M9_FAA_PUBLIC_SHARED_DEPTH_FLUSH_CONTROL) || \
    defined(M9_FAA_PUBLIC_SHARED_DEPTH_RESOLVE_CONTROL) || \
    defined(M9_FAA_PUBLIC_SHARED_DEPTH_RETURNED_CONTROL) || \
    defined(M9_FAA_PUBLIC_SHARED_DEPTH_REPLACEMENT_CONTROL) || \
    defined(M9_FAA_PUBLIC_SHARED_DEPTH_REPLACEMENT_SUBMIT_CONTROL) || \
    defined(M9_FAA_PUBLIC_SHARED_DEPTH_REPLACEMENT_RETURNED_CONTROL)
/* These profiles retain the same original public draw and C2 access
 * before their later output or target-change branches diverge. */
#define M9_FAA_PROGRAM_ACCESS_CONTROL 1
#endif

struct m9_faa_owner {
	struct drm_gem_vram_object *vram;
	struct iosys_map map;
	void *cpu;
	dma_addr_t dma;
	u32 base, bytes;
	/* True from the first attempted attribute change until verified WB
	 * restoration. A failed/partial CPA change still needs restoration. */
	bool cpu_wc;
	struct drm_mm_node system_node;
};
#ifndef M9_FAA_GRAPH_INIT_CONTROL
/* Without the graph profile there are no graph phases to trace. */
void m9x_faa_trace(struct m9x_device *mdev, const char *phase, u32 index,
		   u32 offset, u64 point, int result)
{
}
#endif
#ifdef M9_FAA_GRAPH_INIT_CONTROL
#define M9_FAA_DEPTH_PREFIX_CAPACITY (M9_FAA_ATTACHMENT_CLEAR_CAPACITY + M9_FAA_SAMPLE_CLEAR_CAPACITY)
#define M9_FAA_DEPTH_FRAME_PRIMARY_CAPACITY 0x40000u
#define M9_FAA_DEPTH_FRAME_ENDS 13
#define M9_FAA_DEPTH_FRAME_MAX_ENDS M9_FAA_DEPTH_MODE_MAX_ENDS
#define M9_FAA_DEPTH_MODE_RESET_FIRMWARE "m9x/faa-depth-mode-reset-template.bin"
#define M9_FAA_SAMPLE_CLEAR_FIRMWARE "m9x/faa-graph-sample-clear-template.bin"
#define M9_FAA_DEPTH_DRAW_FIRMWARE "m9x/faa-graph-depth-draw-template.bin"
struct m9_faa_command_pools {
	/* independent primary/secondary SYSTEM MDLs */
	struct m9_faa_owner spare[2];
	struct m9x_faa_pool_timeline timeline;
	m9x_faa_primary_pool primary;
	struct m9x_faa_secondary_pool secondary;
	u32 generation;
	u64 unlock_point, maintenance_point;
	u32 unlock_end, unlock_posts, parent_end, secondary_cursor, posts;
	bool ready, rotated;
};

struct m9_faa_depth_frame_storage {
 u8 primary[M9_FAA_DEPTH_FRAME_PRIMARY_CAPACITY];
 u8 secondary[M9_FAA_COLOR_SECONDARY_CAPACITY];
 u8 observed[4096];
 u32 end_bytes[M9_FAA_DEPTH_FRAME_MAX_ENDS];
 u32 end_count;
 u32 primary_bytes, primary_start, secondary_cursor, secondary_bytes, generation;
 u64 previous_point, queue_start, final_point;
 u8 *batch_data,*batch_secondary,*batch_program;
 u32 batch_data_bytes,draw_count;
 bool first, initialized_reuse, attempted, retired, clear_prefix,clear_resume;
 struct drm_parhelia_m9_faa_graph_clear_prefix resume_prefix;
 struct m9x_faa_color_input resume_input;
 u8 *resume_upload;
 u32 clear_flags;
 bool dual_first;
 bool dual_later,dual_next_pair;
 bool dual_colorpass;
 bool mode_planned;
 struct m9x_faa_depth_mode_state mode_before,mode_after;
 u32 planned_delta,planned_ends,prefix_posts;
 /* Dual later frames: each DRAW's stage and the program cache it leaves. */
 struct m9x_faa_frame_stage *stages;
 struct m9x_faa_program_cache *programs;
};
struct m9_faa_depth_storage {
 struct m9_faa_owner owner[M9_FAA_DEPTH_OWNERS];
 struct m9x_faa_depth_views views;
 u32 generation;
 u64 previous_point;
 bool first, ready, initialized;
 struct m9x_faa_depth_mode_state mode; /* Only actual Lock retirement commits this. */
 struct m9_faa_depth_frame_storage *frame;
};
/* CPU snapshots are not live GPU allocations. Drop them only after the
 * exact frame Lock has been read and its authentic Unlock retired. The
 * initialized attachment owners and their mappings survive this boundary;
 * a later caller still needs a separately qualified state grammar. */
static void m9_faa_depth_frame_free(struct m9_faa_depth_frame_storage *frame)
{
 if(!frame)return;
 kvfree(frame->resume_upload);kvfree(frame->batch_data);kvfree(frame->batch_secondary);kvfree(frame->batch_program);
 kvfree(frame->stages);kfree(frame->programs);kvfree(frame);
}

/* Opt-in progress evidence, with no extra GPU reads or command changes.
 * The optional pre-action delay lets host dmesg streaming persist a marker
 * before a failing PCI transaction. Disabled by default in both settings. */
static bool m9_faa_frame_trace_enabled;
static unsigned int m9_faa_frame_trace_delay_ms;
module_param_named(faa_frame_trace, m9_faa_frame_trace_enabled, bool, 0600);
module_param_named(faa_frame_trace_delay_ms, m9_faa_frame_trace_delay_ms, uint,
		   0600);
MODULE_PARM_DESC(faa_frame_trace,
		 "Log bounded FAA frame prepare/upload/END phases");
MODULE_PARM_DESC(faa_frame_trace_delay_ms,
		 "Opt-in frame trace pre-action pause, capped100ms");

static void m9_faa_frame_trace(struct m9x_device *mdev, const char *phase,
			       u32 index, u32 offset, u64 point, int result,
			       bool pause)
{
	unsigned int delay;

	if (!READ_ONCE(m9_faa_frame_trace_enabled))
		return;
	dev_info(&mdev->pdev->dev,
		 "M9 FAA frame phase: %s index=%u offset=%u point=%#llx result=%d\n",
		 phase, index, offset, point, result);
	delay = READ_ONCE(m9_faa_frame_trace_delay_ms);
	if (pause && delay)
		msleep(min(delay, 100u));
}

/* faa_frame_trace for the graph ioctls outside this file. */
void m9x_faa_trace(struct m9x_device *mdev, const char *phase, u32 index,
		   u32 offset, u64 point, int result)
{
	m9_faa_frame_trace(mdev, phase, index, offset, point, result, false);
}

static bool m9_faa_color_four_experimental;
module_param_named(faa_color_four_experimental, m9_faa_color_four_experimental,
		   bool, 0600);
MODULE_PARM_DESC(faa_color_four_experimental,
		 "Enable unqualified four-stage COLOR launcher (diagnostic only)");
static bool m9_faa_color_observe;
module_param_named(faa_color_observe, m9_faa_color_observe, bool, 0600);
MODULE_PARM_DESC(faa_color_observe,
		 "Read bounded retained COLOR records and block boundaries after final retirement");

struct m9_faa_color_storage {
	u8 program[4096], primary[3696], secondary[2880];
	u8 clear[128];
	u32 clear_bytes, secondary_cursor, lock_posts;
	u64 previous_unlock_point;
	u8 *constants;
	u32 constants_bytes, geometry_bytes, geometry_offset, program_bytes,
		secondary_bytes, primary_length, draw_length, lock_delta;
	struct m9x_faa_color_layout layout;
};

struct m9_faa_public_vs_storage {
	struct m9x_faa_public_vs_upload_layout layout;
	u8 program[M9_FAA_PUBLIC_VS_PROGRAM_BYTES];
	u8 constants[M9_FAA_PUBLIC_VS_CONSTANT_EXTENT];
	u8 secondary[M9_FAA_PUBLIC_VS_SECONDARY_BYTES];
	u8 primary[M9_FAA_PUBLIC_VS_PRIMARY_BYTES];
	struct m9x_faa_public_vs_endpoints ends;
};
struct m9_faa_public_vs_next_storage {
	u8 primary[60], secondary[2548],
		lock_primary[M9_FAA_VS_LOCK_PRIMARY_BYTES];
	u32 vertices[24];
	u64 unlock_point;
	u32 primary_start, draw_end, lock_end;
};

struct m9_faa_public_vs_lock_storage {
	u8 primary[M9_FAA_VS_LOCK_PRIMARY_BYTES];
};
struct m9_faa_public_vs_draw_storage {
	u8 primary[M9_FAA_VS_DRAW_PRIMARY_BYTES],
		secondary[M9_FAA_VS_DRAW_SECONDARY_BYTES];
	struct m9x_faa_public_vs_draw_endpoints ends;
};

struct m9_faa_public_vs_follow_storage {
	u8 primary[48], secondary[616];
	struct m9x_faa_public_vs_follow_endpoints ends;
};

struct m9_faa_next_finish_storage {
	u8 primary[M9_FAA_NEXT_FINISH_PRIMARY_BYTES];
	u8 secondary[M9_FAA_NEXT_FINISH_SECONDARY_BYTES];
};
struct m9_faa_next_storage {
	u8 primary[M9_FAA_NEXT_PRIMARY_BYTES];
	u8 secondary[M9_FAA_NEXT_SECONDARY_BYTES];
	u8 vertices[M9_FAA_NEXT_VERTEX_BYTES];
};
#endif
struct m9x_faa_replay {
	struct m9_faa_owner owner[ARRAY_SIZE(m9_faa_owner_specs)];
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	/* Program26 is allocated after INIT; SYSTEM constants28/secondary29
	 * are mapped before INIT and remain retained. Original command26
	 * and startup27 stay in owner[]. */
	struct m9_faa_owner draw_owner[3];
	struct m9x_faa_texture_owners dual_textures;
	/* At most one source-qualified stage-1 rebinding is retained in v1.
	 * Both pinned sets survive until DMA-stop teardown. */
	struct m9x_faa_texture_owners dual_next_textures;
	bool dual_active_second;
	/* Programs of the dual/single later frames, committed at retirement. */
	struct m9x_faa_program_cache *programs;
	/* Per-DRAW sets whose release met an unsignaled posted fence. */
	struct m9x_faa_texture_owners *deferred_textures;
	u32 deferred_count;
	struct m9_faa_public_vs_storage *public_vs_image;
	struct m9_faa_color_storage *color_image;
	struct m9_faa_depth_storage *depth_storage;
	struct m9_faa_command_pools *command_pools;
	/* Immutable loader refs keep the authenticated templates resident for
	 * this graph generation; every caller still receives its own loader ref. */
	const struct firmware *template_firmware[3];
	bool color_payload_ready, color_attempted, color_lock_retired;
	u64 color_lock_point;
	u32 color_primary_start, color_posts_start;
	u32 color_secondary_cursor;
	u64 color_next_attempt_point;
	bool public_vs_payload_ready, public_vs_prefix_retired;
	bool public_vs_head_attempted, public_vs_head_retired;
	struct m9x_fence *public_vs_head_fence;
	struct m9_faa_public_vs_draw_storage *public_vs_draw_image;
	struct m9_faa_public_vs_lock_storage *public_vs_lock_image;
	bool public_vs_lock_attempted, public_vs_lock_retired;
	struct m9_faa_public_vs_next_storage *public_vs_next_image;
	bool public_vs_next_attempted, public_vs_next_lock_retired,
		public_vs_next_view_seen;
	struct m9x_fence *public_vs_draw_fence;
	bool public_vs_draw_attempted, public_vs_draw_observed;
	struct m9_faa_public_vs_follow_storage *public_vs_follow_image;
	bool public_vs_follow_retired;
	struct {
		u8 program[M9_FAA_DRAW_PROGRAM_BYTES];
		u8 constants[M9_FAA_DRAW_CONSTANTS_BYTES];
		u8 secondary[M9_FAA_DRAW_SECONDARY_BYTES];
		u8 primary_suffix[M9_FAA_DRAW_PRIMARY_SUFFIX_BYTES];
		u8 tail_secondary[M9_FAA_DRAW_TAIL_SECONDARY_BYTES];
		u8 tail_primary[M9_FAA_DRAW_TAIL_PRIMARY_BYTES];
		u8 lock_primary[M9_FAA_LOCK_PRIMARY_BYTES];
	} *draw_image;
	struct m9_faa_next_storage *next_image;
	struct m9_faa_next_finish_storage *next_finish_image;
	bool graph_next_finish_claimed;
	u8 *next_lock_image;
	bool graph_next_lock_claimed, graph_second_lock_retired,
		second_lock_view_seen;
	bool graph_next_claimed;
	/* Append-only producer cursors and the last actual Unlock frontier. */
	u32 graph_secondary_cursor, graph_vertex_cursor, graph_repeat_draws;
	u32 graph_unlock_end_bytes, graph_unlock_posts;
	struct m9x_faa_graph_next_cursor next_cursor;
	u32 next_posts_start;
	u64 next_unlock_point;
	bool graph_draw_payload_ready;
	bool graph_draw_with_finish;
	bool graph_draw_with_lock;
#endif
	/* Holds all six caller GEM pins through retirement or quarantine. */
	struct m9_faa_graph_binding *graph_binding;
	struct m9x_faa_surface_layout graph_layout;
	u32 graph_generation;
	bool graph_preparing;
	/* The first graph draw is a separate caller on the retained INIT
	 * context. A claim closes INIT-token reuse before any source upload;
	 * every posted END advances this cursor even if its later wait fails. */
	bool graph_draw_claimed;
	u32 graph_draw_posts, graph_draw_end_bytes;
	/* Ordinary jobs appended to the retained primary (stage 2). The last one
	 * retires at ordinary_point after the idle origin ordinary_base; the
	 * next FAA frame continues from there (stage 2b).
	 */
	u32 ordinary_posts, ordinary_end;
	u64 ordinary_base, ordinary_point;
	u64 graph_draw_point;
	/* Prepared program bytes remain CPU inputs until the original c0
	 * access boundary. Keep their source buffer through that upload. */
	const struct firmware *program_inputs;
	bool prepared, attempted, baseline_valid;
	/* A later graph INIT retired after this replay was detached: it
	 * reprogrammed the context, so no engine fetch reaches these owners.
	 */
	bool superseded;
	u32 primary_bytes;
	/* Checked extent of the caller's pinned/reserved color BO. This is
	 * borrowed storage, never a locally mapped owner to release. */
	u64 borrowed_color_bytes;
	bool borrowed_color_observed;
	/* Successful submit transfers the caller's pin and lookup reference.
	 * Keep its native address owned through later diagnostic readback. */
	struct drm_gem_vram_object *retained_color;
	struct drm_gem_object *output_color;
	u64 output_point;
	u32 output_generation;
	bool output_retired;
	/* Raw retained-owner evidence is independent of ordinary pixel output.
	 * Set only after this one-shot profile's final actual wait succeeds. */
	u64 observation_point;
	u32 observation_generation;
	bool observation_retired;
	/* Only a returned normal-Lock CPU view authorizes its Unlock copyback.
	 * The first view closes at Unlock; a new view needs the relock receipt. */
	bool normal_lock_view_seen, unlock_started, unlock_retired;
	u64 unlock_point;
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	/* U remains the actual Unlock. P is published only after its own wait.
	 * A failed exposed copy retains destination pin/ref/fence until DMA stop. */
	u64 present_point;
	u32 present_generation, present_end, present_posts;
	bool present_attempted, present_retired;
	struct drm_gem_object *present_destination;
	struct m9x_fence *present_fence;
#endif
	/* A later genuine public Lock first copies primary back to staging and
	 * must retire its own 0x1c0 wait before F021 can be exposed again. */
	bool relock_started, relock_retired, relock_view_seen;
	u64 relock_point;
	u32 relock_generation;
	/* An alias of the transport's existing completion page, never a new
	 * completion allocation or an independently seeded success counter. */
	struct drm_mm_node counter_node;
	u32 before_hash[ARRAY_SIZE(m9_faa_owner_specs)];
	u32 before_nonzero[ARRAY_SIZE(m9_faa_owner_specs)];
	u32 before_nonzero_words[ARRAY_SIZE(m9_faa_owner_specs)];
};

/* Ordinary jobs followed this idle origin and moved the shared timeline. */
static inline bool m9_faa_ordinary_after(const struct m9x_faa_replay *r,
					 u64 base)
{
	return r->ordinary_point && r->ordinary_base == base;
}

/* FIRST continues after ordinary jobs that followed the retired INIT. */
static inline u64 m9_faa_first_origin(const struct m9x_faa_replay *r,
				      u64 init_point)
{
	return m9_faa_ordinary_after(r, init_point) ? r->ordinary_point :
						      init_point;
}

static inline u32 m9_faa_first_bytes(const struct m9x_faa_replay *r,
				     u64 init_point)
{
	return m9_faa_ordinary_after(r, init_point) ? r->ordinary_end :
						      r->primary_bytes;
}

#ifdef M9_FAA_GRAPH_INIT_CONTROL
static struct m9_faa_owner *m9_faa_command_owner(struct m9x_faa_replay *r,
						 bool secondary)
{
	struct m9_faa_owner *original =
		secondary ? &r->draw_owner[2] : &r->owner[M9_FAA_PRIMARY_OWNER];
	struct m9_faa_command_pools *p = r->command_pools;
	struct m9x_faa_secondary_pool *manager;
	u64 identity;

	if (!p || !p->ready)
		return original;
	manager = secondary ? &p->secondary : &p->primary;
	if (!manager->initialized || manager->quarantined ||
	    manager->active > 1)
		return NULL;
	identity = manager->slot[manager->active].owner.identity;
	if (identity == (unsigned long)original)
		return original;
	if (identity == (unsigned long)&p->spare[secondary ? 1 : 0])
		return &p->spare[secondary ? 1 : 0];
	return NULL;
}

static int m9_faa_command_owner_admit(struct m9x_device *,struct m9_faa_owner *);
static u64 m9_faa_present_origin(const struct m9x_faa_replay *r, u64 previous)
{
	return r->present_retired && r->unlock_point == previous ?
		       r->present_point :
		       previous;
}

/* Before the first Unlock, the retired INIT is the idle origin. */
static u64 m9_faa_ordinary_base(const struct m9x_faa_replay *r)
{
	return r->unlock_point ? m9_faa_present_origin(r, r->unlock_point) :
				 r->observation_point;
}

static u64 m9_faa_idle_origin(struct m9x_faa_replay *r, u64 previous)
{
	u64 base = m9_faa_present_origin(r, previous);

	return m9_faa_ordinary_after(r, base) ? r->ordinary_point : base;
}

static u32 m9_faa_idle_end(struct m9x_faa_replay *r)
{
	if (r->ordinary_point &&
	    r->ordinary_base == m9_faa_present_origin(r, r->unlock_point))
		return r->ordinary_end;
	return r->present_retired ? r->present_end : r->graph_unlock_end_bytes;
}

static u32 m9_faa_idle_posts(struct m9x_faa_replay *r)
{
	return r->present_retired ? r->present_posts : r->graph_unlock_posts;
}

static u64 m9_faa_frame_origin(struct m9x_faa_replay *r, u64 previous)
{
	struct m9_faa_command_pools *p = r->command_pools;
	u64 base = m9_faa_present_origin(r, previous);

	if (p && p->ready && p->unlock_point == previous &&
	    p->maintenance_point)
		return p->maintenance_point;
	return r->ordinary_point && r->ordinary_base == base ?
		       r->ordinary_point :
		       base;
}
#endif

static bool m9_faa_diag;
module_param(m9_faa_diag, bool, 0400);
MODULE_PARM_DESC(m9_faa_diag,
	"Use the kernel-owned normal FAA frame for one FIRST_TRIANGLE diagnostic (M9128 only)");
static bool m9_faa_source_init;
module_param(m9_faa_source_init, bool, 0400);
MODULE_PARM_DESC(m9_faa_source_init,
	"Let the private M9128 Windows FAA stream initialize a fresh context without Linux sequence0");
static bool m9_faa_defer_output_map;
module_param(m9_faa_defer_output_map, bool, 0400);
MODULE_PARM_DESC(m9_faa_defer_output_map,
	"Publish only the private output SYSTEM mapping after source120 retirement");
#ifdef M9_FAA_PROGRAM_UPLOAD_OWNER
static bool m9_faa_program_access_kick = true;
#else
static bool m9_faa_program_access_kick;
#endif
module_param(m9_faa_program_access_kick, bool, 0400);
MODULE_PARM_DESC(m9_faa_program_access_kick,
	"Preserve the private C2 CPU-access frontend kick after actual c0 retirement");
#ifdef M9_FAA_PROGRAM_ACCESS_CONTROL
/* Same-module hardware control: collapsing these waits leaves main/AA
 * stores untouched; preserving them produces GPU state. Resolve is still
 * unqualified. Keep the opt-out for this bounded ordering control. */
static bool m9_faa_phased_submit = true;
#else
static bool m9_faa_phased_submit;
#endif
module_param(m9_faa_phased_submit, bool, 0400);
MODULE_PARM_DESC(m9_faa_phased_submit,
	"Wait at source retirement boundaries in the private phased FAA diagnostic");
MODULE_FIRMWARE(M9_FAA_INPUT_FIRMWARE);
#ifdef M9_FAA_GRAPH_INIT_CONTROL
#define M9_FAA_GRAPH_PUBLIC_VS_FIRMWARE \
	"m9x/faa-graph-public-vs-first-template.bin"
MODULE_FIRMWARE(M9_FAA_GRAPH_PUBLIC_VS_FIRMWARE);
#define M9_FAA_GRAPH_PUBLIC_VS_DRAW_FIRMWARE "m9x/faa-graph-public-vs-draw-template.bin"
MODULE_FIRMWARE(M9_FAA_GRAPH_PUBLIC_VS_DRAW_FIRMWARE);
#define M9_FAA_GRAPH_COLOR_FIRMWARE "m9x/faa-graph-color-lock-template.bin"
#define M9_FAA_GRAPH_COLOR_FOUR_FIRMWARE "m9x/faa-graph-color-four-template.bin"
MODULE_FIRMWARE(M9_FAA_GRAPH_COLOR_FIRMWARE);
MODULE_FIRMWARE(M9_FAA_GRAPH_COLOR_FOUR_FIRMWARE);
#define M9_FAA_GRAPH_PUBLIC_VS_NEXT_FIRMWARE "m9x/faa-graph-public-vs-next-template.bin"
MODULE_FIRMWARE(M9_FAA_GRAPH_PUBLIC_VS_NEXT_FIRMWARE);
#define M9_FAA_GRAPH_PUBLIC_VS_LOCK_FIRMWARE "m9x/faa-graph-public-vs-lock-template.bin"
MODULE_FIRMWARE(M9_FAA_GRAPH_PUBLIC_VS_LOCK_FIRMWARE);
#define M9_FAA_GRAPH_PUBLIC_VS_FOLLOW_FIRMWARE \
 "m9x/faa-graph-public-vs-follow-template.bin"
MODULE_FIRMWARE(M9_FAA_GRAPH_PUBLIC_VS_FOLLOW_FIRMWARE);
#define M9_FAA_GRAPH_DRAW_INPUT_FIRMWARE \
	"m9x/faa-graph-post-init-known-inputs.bin"
MODULE_FIRMWARE(M9_FAA_GRAPH_DRAW_INPUT_FIRMWARE);
#define M9_FAA_GRAPH_DRAW_TAIL_FIRMWARE \
	"m9x/faa-graph-returned-draw-inputs.bin"
MODULE_FIRMWARE(M9_FAA_GRAPH_DRAW_TAIL_FIRMWARE);
#define M9_FAA_GRAPH_LOCK_FIRMWARE "m9x/faa-graph-lock-inputs.bin"
MODULE_FIRMWARE(M9_FAA_GRAPH_LOCK_FIRMWARE);
#define M9_FAA_GRAPH_NEXT_FIRMWARE "m9x/faa-graph-next-inputs.bin"
MODULE_FIRMWARE(M9_FAA_GRAPH_NEXT_FIRMWARE);
#define M9_FAA_GRAPH_NEXT_FINISH_FIRMWARE "m9x/faa-graph-next-finish-inputs.bin"
MODULE_FIRMWARE(M9_FAA_GRAPH_NEXT_FINISH_FIRMWARE);
#endif

#ifdef M9_FAA_GRAPH_INIT_CONTROL
static int m9_faa_template_request(struct m9x_device *mdev,
				   struct m9_faa_graph_binding *binding,
				   u32 generation, u32 slot,
				   const struct firmware **out)
{
	static const char *const name[] = { M9_FAA_SAMPLE_CLEAR_FIRMWARE,
					    M9_FAA_GRAPH_COLOR_FIRMWARE,
					    M9_FAA_GRAPH_COLOR_FOUR_FIRMWARE };
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	const struct firmware *loaded = NULL, *pin = NULL;
	bool missing = false;
	u32 bytes = 0;
	int ret;

	if (!out || !binding || slot >= ARRAY_SIZE(name))
		return -EINVAL;
	*out = NULL;
	/* Linux's alloc_lookup_fw_priv shares the loaded immutable buffer while
	 * any ref is retained. Use a normal request for the caller's independent
	 * lifetime, rather than borrowing a replay pointer across an unlock.

	 */
	m9_faa_frame_trace(mdev, "template-request-begin", slot, 0, generation,
			   0, false);
	ret = request_firmware(&loaded, name[slot], &mdev->pdev->dev);
	if (ret)
		goto release;
	bytes = loaded->size;
	mutex_lock(&t->lock);
	r = t->faa;
	if (t->wedged)
		ret = -EIO;
	else if (!r || r->graph_binding != binding ||
		 r->graph_generation != generation ||
		 t->generation != generation)
		ret = -ESTALE;
	else
		missing = !r->template_firmware[slot];
	mutex_unlock(&t->lock);
	if (ret)
		goto release;
	if (missing) {
		ret = request_firmware(&pin, name[slot], &mdev->pdev->dev);
		if (ret)
			goto release;
		mutex_lock(&t->lock);
		r = t->faa;
		if (t->wedged) {
			ret = -EIO;
		} else if (!r || r->graph_binding != binding ||
			   r->graph_generation != generation ||
			   t->generation != generation) {
			ret = -ESTALE;
		} else if (!r->template_firmware[slot]) {
			r->template_firmware[slot] = pin;
			pin = NULL;
		}
		mutex_unlock(&t->lock);
		if (ret)
			goto release;
	}
	*out = loaded;
	loaded = NULL;
release:
	if (pin)
		release_firmware(pin);
	if (loaded)
		release_firmware(loaded);
	m9_faa_frame_trace(mdev, "template-request-end", slot, bytes,
			   generation, ret, false);
	return ret;
}

#endif

bool m9x_faa_diag_enabled(void)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	return false;
#else
	return m9_faa_diag;
#endif
}

bool m9x_faa_graph_active(struct m9x_device *mdev)
{
	lockdep_assert_held(&mdev->transport.lock);
	return mdev->transport.faa &&
	       (mdev->transport.faa->graph_binding ||
		mdev->transport.faa->graph_preparing);
}

/* The source1024 INIT flags30 primary SYSTEM view maps its full backing.
 * Only this retained immutable graph layout extends the historic80-page
 * diagnostic allowance; ordinary map policy and DMA bounds stay separate. */
u32 m9x_faa_graph_system_extent_locked(struct m9x_device *mdev)
{
	struct m9x_faa_replay *r = mdev->transport.faa;

	lockdep_assert_held(&mdev->transport.lock);
	if (!r || (!r->graph_binding && !r->graph_preparing) ||
	    !m9x_faa_runtime_shape(r->graph_layout.width,
				   r->graph_layout.height))
		return 0;
	return r->graph_layout.owner[M9_FAA_SURFACE_PRIMARY].capacity;
}

bool m9x_faa_graph_profile_enabled(void)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	return true;
#else
	return false;
#endif
}

bool m9x_faa_graph_preparing(struct m9x_device *mdev)
{
	lockdep_assert_held(&mdev->transport.lock);
	return mdev->transport.faa &&
	       mdev->transport.faa->graph_preparing;
}

/* First post-INIT caller only. Its source timeline starts from the same
 * retired b0-era context; an already continued primary or an outstanding
 * phase must not be admitted using INIT's receipt again. No MMIO is read. */
static int m9_faa_graph_continuation_state(
		const struct m9x_transport *t, u32 generation, u64 init_point)
{
	const struct m9x_faa_replay *r = t->faa;
	u64 origin;

	if (t->wedged)
		return -EIO;
	if (!r || !r->graph_binding || !r->prepared || !r->attempted ||
	    !r->observation_retired || !r->observation_point ||
	    r->graph_preparing || r->graph_draw_claimed ||
	    r->graph_generation != generation ||
	    r->observation_generation != generation ||
	    t->generation != generation ||
	    r->observation_point != init_point)
		return -ESTALE;
	origin = m9_faa_first_origin(r, init_point);
	if (t->submitted_point != origin || t->completed_point != origin ||
	    lower_32_bits(origin) > U32_MAX - 0x10 ||
	    t->next_fence != lower_32_bits(origin) + 0x10)
		return -ESTALE;
	if (!t->faa_source_context || !t->faa_virtual_primary ||
	    !t->transport_armed || !t->bus_master_enabled ||
	    !t->engine_transitioned || !t->restore_ready || !t->fence_cpu ||
	    t->async_stop || t->async_pending || t->queue_count)
		return -EBUSY;
	/* Separate typed DRAW/clear/Lock producers admit these closed shapes. */
	if (!m9x_faa_runtime_shape(r->graph_layout.width,r->graph_layout.height))
		return -EOPNOTSUPP;
	return 0;
}

int m9x_faa_graph_retained_get_locked(struct m9x_device *mdev,
		u32 generation, u64 init_point,
		struct m9_faa_graph_binding **out)
{
	int ret;

	lockdep_assert_held(&mdev->transport.lock);
	if (!out)
		return -EINVAL;
	*out = NULL;
	/* Ordinary jobs after the retired INIT retire before FIRST. */
	if (mdev->transport.faa &&
	    m9_faa_ordinary_after(mdev->transport.faa, init_point) &&
	    (mdev->transport.queue_count || mdev->transport.async_pending)) {
		ret = m9x_transport_drain_locked(mdev);
		if (ret)
			return ret;
	}
	ret = m9_faa_graph_continuation_state(&mdev->transport,
					    generation, init_point);
	if (!ret)
		*out = m9x_faa_graph_binding_get(mdev->transport.faa->graph_binding);
	return ret;
}

#ifdef M9_FAA_GRAPH_INIT_CONTROL
/* The signed first-draw source appends 88 bytes to the existing 4880-byte
 * primary: two posts at +64, then an independent finish at +88.  The live
 * INIT primary may be longer because its companion clear was regenerated;
 * adding to r->primary_bytes applies that delta without touching the
 * already retired prefix or resetting COMMAND_BASE.  0x60 is the source
 * timeline distance from the actual INIT receipt to the final writeback. */
static int m9_faa_graph_stage_shape(const struct m9x_faa_replay *r,
				    u32 stage, u64 init_point, u32 *end_bytes,
				    bool *repeat, u32 *target)
{
	u32 end;

	if (!r || !end_bytes || !repeat || !target ||
	    M9_FAA_PRIMARY_BYTES != 4880 || stage != r->graph_draw_posts ||
	    stage >= 3 || !r->primary_bytes ||
	    lower_32_bits(init_point) > U32_MAX - 0x60)
		return -EINVAL;
	if (check_add_overflow(r->primary_bytes,
			       stage == 2 ? 88u : 64u, &end) ||
	    end > r->owner[M9_FAA_PRIMARY_OWNER].bytes ||
	    (stage == 0 && r->graph_draw_end_bytes != r->primary_bytes) ||
	    (stage && r->graph_draw_end_bytes != r->primary_bytes + 64))
		return -ERANGE;
	*end_bytes = end;
	*repeat = stage == 1;
	*target = stage == 2 ? lower_32_bits(init_point) + 0x60 : 0;
	return 0;
}
#endif

/* Claim the separate public caller before its CPU mapping and program
 * upload.  No command is posted here.  A repeated caller cannot use the
 * INIT receipt to begin another draw, even while its first END is pending. */
int m9x_faa_graph_draw_claim_locked(struct m9x_device *mdev,
		struct m9_faa_graph_binding *binding, u32 generation,
		u64 init_point)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	int ret;

	lockdep_assert_held(&t->lock);
	ret = m9_faa_graph_continuation_state(t, generation, init_point);
	if (ret)
		return ret;
	r = t->faa;
	if (!binding || binding != r->graph_binding ||
	    r->primary_bytes < M9_FAA_PRIMARY_BYTES ||
	    r->owner[M9_FAA_PRIMARY_OWNER].vram ||
	    !r->owner[M9_FAA_PRIMARY_OWNER].cpu ||
	    !drm_mm_node_allocated(&r->owner[M9_FAA_PRIMARY_OWNER].system_node) ||
	    r->owner[M9_FAA_PRIMARY_OWNER].system_node.start !=
			r->owner[M9_FAA_PRIMARY_OWNER].base)
		return -ESTALE;
	r->graph_draw_claimed = true;
	r->graph_draw_end_bytes = m9_faa_first_bytes(r, init_point);
	return 0;
#else
	return -EOPNOTSUPP;
#endif
}

/* The future private draw producer calls this only after source-known
 * suffix/secondary uploads, under six graph reservations and TRANSPORT.
 * Even a failed wait after END publication consumes its stage; owner refs
 * stay with the retained replay until teardown/quarantine. */
int m9x_faa_graph_draw_end_locked(struct m9x_device *mdev,
		struct m9_faa_graph_binding *binding, u32 generation,
		u64 init_point, u32 stage, u64 *point, bool *posted)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r = t->faa;
	struct m9_faa_owner *primary;
	u32 end, target;
	bool repeat;
	int ret;

	lockdep_assert_held(&t->lock);
	if (posted)
		*posted = false;
	if (point)
		*point = 0;
	if (!point || !posted || !binding || !r || !r->graph_draw_claimed ||
	    !r->graph_draw_payload_ready ||
	    r->graph_binding != binding || r->graph_generation != generation ||
	    r->observation_generation != generation ||
	    r->observation_point != init_point ||
	    t->generation != generation || t->wedged ||
	    t->submitted_point != init_point || t->completed_point != init_point ||
	    t->next_fence != lower_32_bits(init_point) + 0x10)
		return -ESTALE;
	ret = m9_faa_graph_stage_shape(r, stage, init_point,
				       &end, &repeat, &target);
	if (ret)
		return ret;
	primary = &r->owner[M9_FAA_PRIMARY_OWNER];
	ret = m9x_post_faa_graph_end_locked(mdev, &primary->system_node,
					primary->dma, r->graph_draw_end_bytes,
					end, repeat, target, point, posted);
	if (*posted) {
		r->graph_draw_end_bytes = end;
		r->graph_draw_posts++;
	}
	if (!ret && target)
		r->graph_draw_point = *point;
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

int m9x_faa_graph_continuation_selftest(void)
{
	/* The replay contains the full private owner table; keep it off the
	 * module-init stack even though this is a CPU-only predicate test. */
	static struct m9x_transport t;
	static struct m9x_faa_replay r;
	static u32 counter = 0xb1;

	m9x_faa_surface_layout(320, 240, &r.graph_layout);
	t.faa = &r;
	t.fence_cpu = &counter;
	t.faa_source_context = true;
	t.faa_virtual_primary = true;
	t.transport_armed = true;
	t.bus_master_enabled = true;
	t.engine_transitioned = true;
	t.restore_ready = true;
	t.submitted_point = t.completed_point = 0xb0;
	t.next_fence = 0xc0;
	r.graph_binding = (void *)&t; /* predicate checks presence, not identity */
	r.prepared = r.attempted = r.observation_retired = true;
	r.observation_point = 0xb0;
	if (m9_faa_graph_continuation_state(&t, 0, 0xb0) ||
	    m9_faa_graph_continuation_state(&t, 1, 0xb0) != -ESTALE ||
	    m9_faa_graph_continuation_state(&t, 0, 0xc0) != -ESTALE)
		return -EINVAL;
	t.queue_count = 1;
	if (m9_faa_graph_continuation_state(&t, 0, 0xb0) != -EBUSY)
		return -EINVAL;
	t.queue_count = 0;
	t.async_pending = true;
	if (m9_faa_graph_continuation_state(&t, 0, 0xb0) != -EBUSY)
		return -EINVAL;
	t.async_pending = false;
	t.completed_point = 0xc0; /* another submission cannot reuse INIT */
	if (m9_faa_graph_continuation_state(&t, 0, 0xb0) != -ESTALE)
		return -EINVAL;
	t.completed_point = 0xb0;
	t.wedged = true;
	if (m9_faa_graph_continuation_state(&t, 0, 0xb0) != -EIO)
		return -EINVAL;
	t.wedged = false;
	r.graph_binding = NULL;
	return m9_faa_graph_continuation_state(&t, 0, 0xb0) == -ESTALE ?
		0 : -EINVAL;
}

/* Where FIRST starts on the timeline: INIT, or the ordinary jobs after it. */
u64 m9x_faa_graph_first_origin_locked(struct m9x_device *mdev, u64 init_point)
{
	const struct m9x_faa_replay *r = mdev->transport.faa;

	lockdep_assert_held(&mdev->transport.lock);
	return r ? m9_faa_first_origin(r, init_point) : init_point;
}

/* The source INIT, the vendor's one context activation, has retired. */
bool m9x_faa_source_init_retired_locked(struct m9x_device *mdev)
{
	const struct m9x_faa_replay *r = mdev->transport.faa;

	lockdep_assert_held(&mdev->transport.lock);
	return r && r->graph_binding && !r->graph_preparing && r->attempted &&
	       r->observation_retired && r->observation_point;
}

bool m9x_faa_source_init_enabled(struct m9x_device *mdev)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	return mdev->transport.faa &&
	       mdev->transport.faa->graph_binding &&
	       mdev->pdev->device == 0x0540;
#else
	return m9_faa_diag && m9_faa_source_init && mdev->pdev->device == 0x0540;
#endif
}

/* No command was ever posted: the requester still holds its seeded origin. */
static bool m9_faa_transport_fresh(const struct m9x_transport *t)
{
	return !t->faa_virtual_primary && !t->submitted_point &&
	       !t->completed_point && !t->queue_count && !t->async_pending &&
	       !t->command_slot && !t->command_offset && !t->fence_epoch &&
	       !t->last_fence && t->next_fence == 0x20 && t->fence_cpu &&
	       READ_ONCE(((u32 *)t->fence_cpu)[0]) == 0x10 &&
	       READ_ONCE(((u32 *)t->fence_cpu)[1]) == 0x10;
}

/* A used requester whose Linux first context retired: with ordinary jobs
 * continuing the FAA primary, the graph INIT continues its live timeline.
 * Its queue-initialize part re-establishes the vendor queue state that the
 * M9148 sequence0 sets differently (c0, 62, 66).
 */
static bool m9_faa_transport_continuable(struct m9x_device *mdev)
{
	const struct m9x_transport *t = &mdev->transport;

	return m9x_faa_ordinary_continue_enabled() && !t->faa_virtual_primary &&
	       !t->wedged && !t->async_stop &&
	       t->transport_armed && t->bus_master_enabled &&
	       t->engine_transitioned && t->restore_ready && t->fence_cpu &&
	       m9x_triangle_context_ready_locked(mdev);
}

/* The unchanged native origin10 is an initial CPU counter seed. Fresh
 * source admission must never call it a previous GPU retirement. */
int m9x_faa_source_init_admit_locked(struct m9x_device *mdev,
				    bool context_initialized)
{
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r = t->faa;

	lockdep_assert_held(&t->lock);
	if (!m9x_faa_source_init_enabled(mdev) || !r || !r->prepared ||
	    !t->fence_cpu || t->wedged)
		return -EINVAL;
	if (context_initialized)
		return !r->attempted && m9_faa_transport_continuable(mdev) ?
		       0 : -EINVAL;
	/* Repeated CPU-view preparation may reuse the admitted manager. It
	 * cannot start a second frame: the caller's attempted gate remains. */
	if (t->faa_source_context && r->attempted)
		return t->engine_transitioned && t->restore_ready &&
		       t->transport_armed && t->bus_master_enabled &&
		       !t->queue_count && !t->async_pending ? 0 : -EINVAL;
	if (r->attempted || !m9_faa_transport_fresh(t))
		return -EBUSY;
	return 0;
}

static struct m9_faa_owner *m9_faa_output_owner(struct m9x_faa_replay *r)
{
	if (M9_FAA_OUTPUT_OWNER >= ARRAY_SIZE(r->owner))
		return NULL;
	return &r->owner[M9_FAA_OUTPUT_OWNER];
}

/* The captured flags30 raw-MDL owners all select MmWriteCombined through
 * real 23306c input word1=0. Linux coherent DMA may return a WB direct alias
 * even with DMA_ATTR_WRITE_COMBINE, so establish and verify that CPU contract
 * before publishing any of these private owners. Ordinary DMA and the shared
 * flags110 completion-page alias retain their existing cache attributes.
 */
static int m9_faa_dma_cache_check(struct m9_faa_owner *o, bool wc)
{
#ifdef CONFIG_X86_64
	unsigned long start = (unsigned long)o->cpu;
	unsigned long expected = cachemode2protval(wc ? _PAGE_CACHE_MODE_WC :
						     _PAGE_CACHE_MODE_WB);
	u32 offset;

	if (!pat_enabled() || !o->cpu || is_vmalloc_addr(o->cpu) ||
	    !virt_addr_valid(o->cpu) || !IS_ALIGNED(start, PAGE_SIZE) ||
	    !o->bytes || !IS_ALIGNED(o->bytes, PAGE_SIZE) ||
	    o->bytes > INT_MAX || start > ULONG_MAX - o->bytes)
		return -EOPNOTSUPP;
	for (offset = 0; offset < o->bytes; offset += PAGE_SIZE) {
		unsigned long address = start + offset;
		unsigned int level;
		pte_t *entry = lookup_address(address, &level);
		unsigned long prot;

		if (!virt_addr_valid((void *)address) || !entry ||
		    !pte_present(*entry) || level < PG_LEVEL_4K ||
		    level > PG_LEVEL_1G)
			return -EFAULT;
		prot = pte_val(*entry);
		if (level != PG_LEVEL_4K)
			prot = protval_large_2_4k(prot);
		if ((prot & _PAGE_CACHE_MASK) != expected)
			return -EUCLEAN;
	}
	return 0;
#else
	return -EOPNOTSUPP;
#endif
}

static int m9_faa_dma_prepare_wc(struct m9x_device *mdev,
				 struct m9_faa_owner *o)
{
#ifdef CONFIG_X86_64
	int ret = m9_faa_dma_cache_check(o, false);

	if (ret)
		return ret;
	/* CPA can fail after changing a subset of pages. Retain restoration
	 * ownership before entering it, including on its error paths. */
	o->cpu_wc = true;
	ret = set_memory_wc((unsigned long)o->cpu, o->bytes >> PAGE_SHIFT);
	if (!ret)
		ret = m9_faa_dma_cache_check(o, true);
	if (ret) {
		dev_err(&mdev->pdev->dev,
			"M9 FAA CPU WC preparation refused bytes=%x error=%d\n",
			o->bytes, ret);
		return ret;
	}
	dev_info(&mdev->pdev->dev,
		 "M9 FAA CPU cache verified WB->WC bytes=%x pages=%u before publication\n",
		 o->bytes, o->bytes >> PAGE_SHIFT);
	return 0;
#else
	return -EOPNOTSUPP;
#endif
}

static bool m9_faa_dma_restore_wb(struct m9_faa_owner *o)
{
	if (!o->cpu_wc)
		return true;
#ifdef CONFIG_X86_64
	if (set_memory_wb((unsigned long)o->cpu, o->bytes >> PAGE_SHIFT) ||
	    m9_faa_dma_cache_check(o, false))
		return false;
	o->cpu_wc = false;
	return true;
#else
	return false;
#endif
}

static bool m9_faa_release(struct m9x_device *mdev, struct m9x_faa_replay *r)
{
	u32 i;
	bool owner_reserved[ARRAY_SIZE(r->owner)] = { };
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	bool draw_reserved[ARRAY_SIZE(r->draw_owner)] = { };
 bool depth_reserved[M9_FAA_DEPTH_OWNERS] = { };
#endif

	/* Failed preparation has no submitted FAA work. Attempted owners may
	 * change attributes only once requester teardown stopped bus mastering.
	 * Restore every changed allocation before freeing any owner, so a CPA
	 * failure cannot return WC or partly converted pages to the allocator.
	 */
	if (r->attempted && !r->superseded && mdev->transport.bus_master_enabled)
		goto quarantine;
	for (i = 0; i < ARRAY_SIZE(r->owner); i++)
		if (!m9_faa_dma_restore_wb(&r->owner[i])) {
			dev_err(&mdev->pdev->dev,
				"M9 FAA CPU WB restore failed owner=%u bytes=%x; allocations retained\n",
				i, r->owner[i].bytes);
			goto quarantine;
		}
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	for (i = 0; i < ARRAY_SIZE(r->draw_owner); i++)
		if (!m9_faa_dma_restore_wb(&r->draw_owner[i]))
			goto quarantine;
#endif
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 if (r->depth_storage)
  for (i=0;i<M9_FAA_DEPTH_OWNERS;i++)
   if (!m9_faa_dma_restore_wb(&r->depth_storage->owner[i]))
    goto quarantine;
 if (r->command_pools)
  for (i=0;i<2;i++)
   if (!m9_faa_dma_restore_wb(&r->command_pools->spare[i]))goto quarantine;
#endif
	/* This can fail to reserve a texture BO. Do it before freeing any graph
	 * owner so the whole replay remains retryable during DMA-stop teardown. */
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	if (!m9x_faa_texture_owners_put(&r->dual_textures, true))
		goto quarantine;
	if (!m9x_faa_texture_owners_put(&r->dual_next_textures, true))
		goto quarantine;
	while (r->deferred_count) {
		if (!m9x_faa_texture_owners_put(
			&r->deferred_textures[r->deferred_count - 1], true))
			goto quarantine;
		r->deferred_count--;
	}
	kvfree(r->deferred_textures);
	r->deferred_textures = NULL;
#endif
	/* Reserve every private TTM view before freeing anything. A failed
	 * reservation must not be followed by vunmap or a partial owner release. */
	for (i = 0; i < ARRAY_SIZE(r->owner); i++) {
		if (!r->owner[i].vram)
			continue;
		if (ttm_bo_reserve(&r->owner[i].vram->bo, false, false, NULL))
			goto unreserve_quarantine;
		owner_reserved[i] = true;
	}
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	for (i = 0; i < ARRAY_SIZE(r->draw_owner); i++) {
		if (!r->draw_owner[i].vram)
			continue;
		if (ttm_bo_reserve(&r->draw_owner[i].vram->bo, false, false, NULL))
			goto unreserve_quarantine;
		draw_reserved[i] = true;
	}
#endif

#ifdef M9_FAA_GRAPH_INIT_CONTROL
 if (r->depth_storage)
  for (i=0;i<M9_FAA_DEPTH_OWNERS;i++) {
   struct m9_faa_owner *o=&r->depth_storage->owner[i];
   if (!o->vram) continue;
   if (ttm_bo_reserve(&o->vram->bo,false,false,NULL))
    goto unreserve_quarantine;
   depth_reserved[i]=true;
  }
#endif

	if (drm_mm_node_allocated(&r->counter_node))
		m9x_address_unmap_system(mdev, &r->counter_node);
	for (i = 0; i < ARRAY_SIZE(r->owner); i++) {
		struct m9_faa_owner *o = &r->owner[i];

		if (drm_mm_node_allocated(&o->system_node))
			m9x_address_unmap_system(mdev, &o->system_node);
		if (o->vram) {
			drm_gem_vram_vunmap(o->vram, &o->map);
			ttm_bo_unreserve(&o->vram->bo);
			m9x_vram_unpin(o->vram);
			drm_gem_vram_put(o->vram);
		} else if (o->cpu) {
			dma_free_coherent(&mdev->pdev->dev, o->bytes, o->cpu, o->dma);
		}
	}
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	for (i = 0; i < ARRAY_SIZE(r->draw_owner); i++) {
		struct m9_faa_owner *o = &r->draw_owner[i];

		if (drm_mm_node_allocated(&o->system_node))
			m9x_address_unmap_system(mdev, &o->system_node);
		if (o->vram) {
			drm_gem_vram_vunmap(o->vram, &o->map);
			ttm_bo_unreserve(&o->vram->bo);
			m9x_vram_unpin(o->vram);
			drm_gem_vram_put(o->vram);
		} else if (o->cpu) {
			dma_free_coherent(&mdev->pdev->dev, o->bytes,
					  o->cpu, o->dma);
		}
	}
 if (r->command_pools) {
  for(i=0;i<2;i++){
   struct m9_faa_owner *o=&r->command_pools->spare[i];
   if(drm_mm_node_allocated(&o->system_node))m9x_address_unmap_system(mdev,&o->system_node);
   if(o->cpu)dma_free_coherent(&mdev->pdev->dev,o->bytes,o->cpu,o->dma);
  }
  kfree(r->command_pools);
 }
 if (r->depth_storage) {
  for (i=0;i<M9_FAA_DEPTH_OWNERS;i++) {
   struct m9_faa_owner *o=&r->depth_storage->owner[i];
   if (drm_mm_node_allocated(&o->system_node))
    m9x_address_unmap_system(mdev,&o->system_node);
   if (o->vram) {
    drm_gem_vram_vunmap(o->vram,&o->map);
    ttm_bo_unreserve(&o->vram->bo);
    m9x_vram_unpin(o->vram);
    drm_gem_vram_put(o->vram);
   } else if (o->cpu)
    dma_free_coherent(&mdev->pdev->dev,o->bytes,o->cpu,o->dma);
  }
  m9_faa_depth_frame_free(r->depth_storage->frame);
  kfree(r->depth_storage);
 }
	kfree(r->draw_image);
	kfree(r->public_vs_image);
 if (r->color_image) kfree(r->color_image->constants);
 kfree(r->color_image);
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 kfree(r->programs);
#endif
	kfree(r->public_vs_follow_image);
 kfree(r->public_vs_draw_image);
 kfree(r->public_vs_lock_image);
 kfree(r->public_vs_next_image);
	kfree(r->next_image);
	kfree(r->next_finish_image);
	kfree(r->next_lock_image);
#endif
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 if(r->present_destination) {
  /* The attempted stream was stopped above before releasing these refs. */
  m9x_gem_vram_unpin(r->present_destination);
  drm_gem_object_put(r->present_destination);
 }
 if(r->present_fence) {
  dma_fence_signal(&r->present_fence->base);
  dma_fence_put(&r->present_fence->base);
 }
#endif
	if (r->retained_color) {
		m9x_vram_unpin(r->retained_color);
		drm_gem_vram_put(r->retained_color);
	}
	if (r->output_color)
		drm_gem_object_put(r->output_color);
	if (r->program_inputs)
		release_firmware(r->program_inputs);
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 for(i=0;i<ARRAY_SIZE(r->template_firmware);i++)
  if(r->template_firmware[i])release_firmware(r->template_firmware[i]);
#endif
	m9x_faa_graph_binding_put(r->graph_binding);
	kfree(r);
	return true;
unreserve_quarantine:
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 if (r->depth_storage)
  for(i=0;i<M9_FAA_DEPTH_OWNERS;i++)
   if(depth_reserved[i])ttm_bo_unreserve(&r->depth_storage->owner[i].vram->bo);
	for (i = 0; i < ARRAY_SIZE(r->draw_owner); i++)
		if (draw_reserved[i])
			ttm_bo_unreserve(&r->draw_owner[i].vram->bo);
#endif
	for (i = 0; i < ARRAY_SIZE(r->owner); i++)
		if (owner_reserved[i])
			ttm_bo_unreserve(&r->owner[i].vram->bo);
quarantine:
	mdev->transport.wedged = true;
	mdev->transport.faa = r;
	dev_err(&mdev->pdev->dev,
		"M9 FAA CPU cache owners quarantined; no DMA allocation freed\n");
	return false;
}

static void m9_faa_release_failed_prepare(struct m9x_device *mdev,
					struct m9x_faa_replay *r)
{
	lockdep_assert_held(&mdev->transport.lock);
	if (mdev->transport.wedged) {
		/* Publication may already have exposed these DMA/local owners.
		 * Keep them through the freeze; requester teardown first disables
		 * PCI bus mastering, then m9x_faa_fini drops their ownership. */
		mdev->transport.faa = r;
		dev_err(&mdev->pdev->dev,
			"M9 FAA failed preparation retained until DMA shutdown\n");
		return;
	}
	m9_faa_release(mdev, r);
}

/* Remove every system address node of the replay; owners stay allocated. */
static void m9_faa_unmap_system_locked(struct m9x_device *mdev,
				       struct m9x_faa_replay *r)
{
	u32 i;

	lockdep_assert_held(&mdev->transport.lock);
	if (drm_mm_node_allocated(&r->counter_node))
		m9x_address_unmap_system(mdev, &r->counter_node);
	for (i = 0; i < ARRAY_SIZE(r->owner); i++)
		if (drm_mm_node_allocated(&r->owner[i].system_node))
			m9x_address_unmap_system(mdev, &r->owner[i].system_node);
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	for (i = 0; i < ARRAY_SIZE(r->draw_owner); i++)
		if (drm_mm_node_allocated(&r->draw_owner[i].system_node))
			m9x_address_unmap_system(mdev,
						 &r->draw_owner[i].system_node);
	if (r->depth_storage)
		for (i = 0; i < M9_FAA_DEPTH_OWNERS; i++)
			if (drm_mm_node_allocated(&r->depth_storage->owner[i].system_node))
				m9x_address_unmap_system(mdev,
							 &r->depth_storage->owner[i].system_node);
	if (r->command_pools)
		for (i = 0; i < 2; i++)
			if (drm_mm_node_allocated(&r->command_pools->spare[i].system_node))
				m9x_address_unmap_system(mdev,
							 &r->command_pools->spare[i].system_node);
#endif
}

/* Called by requester teardown only after PCI bus mastering has stopped. */
void m9x_faa_fini(struct m9x_device *mdev)
{
	if (mdev->transport.faa) {
		/* Remove address nodes under their lock, but drop the caller's
		 * GEM reference outside it: its last release takes this lock. */
		struct m9x_faa_replay *r = mdev->transport.faa;

#ifdef M9_FAA_GRAPH_INIT_CONTROL
		if (r->public_vs_head_fence) {
			/* HEAD's c0 is an early observation, not a tail completion.
			 * Requester fini has already disabled bus mastering. */
			dma_fence_set_error(&r->public_vs_head_fence->base, -EIO);
			dma_fence_signal(&r->public_vs_head_fence->base);
			dma_fence_put(&r->public_vs_head_fence->base);
			r->public_vs_head_fence = NULL;
		}
		if (r->public_vs_draw_fence) {
			dma_fence_set_error(&r->public_vs_draw_fence->base, -EIO);
			dma_fence_signal(&r->public_vs_draw_fence->base);
			dma_fence_put(&r->public_vs_draw_fence->base);
			r->public_vs_draw_fence = NULL;
		}

#endif

		mutex_lock(&mdev->transport.lock);
		m9_faa_unmap_system_locked(mdev, r);
		mutex_unlock(&mdev->transport.lock);
		if (m9_faa_release(mdev, r))
			mdev->transport.faa = NULL;
	}
	if (mdev->transport.faa_retiring) {
		struct m9x_faa_replay *r = mdev->transport.faa_retiring;

		mutex_lock(&mdev->transport.lock);
		m9_faa_unmap_system_locked(mdev, r);
		mutex_unlock(&mdev->transport.lock);
		if (m9_faa_release(mdev, r))
			mdev->transport.faa_retiring = NULL;
	}
}

/* An INIT that never wrote COMMAND_BASE or an END for this replay leaves
 * nothing the engine can fetch: release it, so ordinary jobs bring up their
 * own context and a later graph can INIT. Returns false when kept.
 */
#ifdef M9_FAA_GRAPH_INIT_CONTROL
/* A graph INIT retired after the previous graph was detached: its context
 * initializer replaced the fragment pool and sentinel bindings, and the
 * requester fetches the new primary, so the detached replay is released.
 */
static void m9_faa_release_superseded(struct m9x_device *mdev)
{
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *old;

	mutex_lock(&t->lock);
	old = t->faa_retiring;
	if (old && t->faa && t->faa->observation_retired && !t->wedged) {
		old->superseded = true;
		m9_faa_unmap_system_locked(mdev, old);
		t->faa_retiring = NULL;
	} else {
		old = NULL;
	}
	mutex_unlock(&t->lock);
	if (old && !m9_faa_release(mdev, old)) {
		mutex_lock(&t->lock);
		t->faa_retiring = old;
		mutex_unlock(&t->lock);
	}
}

/* Drop the tag1 that INIT put on the restore records' DWORD9/14/19. */
static int m9_faa_untag_virtual_restore(struct m9x_device *mdev)
{
	static const u32 word[] = { 9, 14, 19 };
	struct iosys_map map;
	u32 base, i, j;
	int ret;

	lockdep_assert_held(&mdev->transport.lock);
	ret = m9x_address_restore_region(mdev, &map, &base);
	if (ret)
		return ret;
	if (base != mdev->transport.restore_base)
		return -ERANGE;
	for (i = 0; i < 0x400; i++) {
		u32 work = base + 0x200 + i * 0x600;
		u32 expected[] = { work, base, work + 0x440 };

		for (j = 0; j < ARRAY_SIZE(word); j++)
			if (iosys_map_rd(&map, 0x700 + i * 0x600 + word[j] * 4,
					 u32) != (expected[j] | 1u))
				return -EINVAL;
	}
	for (i = 0; i < 0x400; i++) {
		u32 work = base + 0x200 + i * 0x600;
		u32 expected[] = { work, base, work + 0x440 };

		for (j = 0; j < ARRAY_SIZE(word); j++)
			iosys_map_wr(&map, 0x700 + i * 0x600 + word[j] * 4, u32,
				     expected[j]);
	}
	/* The records are WC; publish them before the requester is rearmed. */
	wmb();
	return 0;
}

/* A graph stops between frames: its INIT retired, no frame, pool transition,
 * public-VS step or PRESENT is in flight.
 */
static bool m9_faa_graph_idle(const struct m9x_faa_replay *r)
{
	if (!r->attempted || !r->observation_retired || r->graph_preparing ||
	    (r->depth_storage && r->depth_storage->frame) ||
	    r->public_vs_head_fence || r->public_vs_draw_fence ||
	    r->present_destination || r->present_fence ||
	    (r->command_pools && r->command_pools->primary.planned))
		return false;
	if (!r->unlock_point)
		return !r->color_attempted && !r->graph_draw_posts;
	return r->unlock_retired && r->present_attempted == r->present_retired;
}
#endif

/* Once its file released the graph and the graph is idle, return the
 * requester to the staging slots on the context the INIT activated, so a
 * later graph INIT continues it like any used requester. The replay's owners
 * stay engine state (fragment pool, sentinel) until that INIT retires.
 */
void m9x_faa_graph_detach_released(struct m9x_device *mdev)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	int ret;

	mutex_lock(&t->lock);
	r = t->faa;
	if (!r || t->wedged || t->faa_retiring || !t->faa_virtual_primary ||
	    !m9x_faa_graph_binding_released(r->graph_binding) ||
	    !m9_faa_graph_idle(r))
		goto unlock;
	ret = m9x_transport_drain_locked(mdev);
	if (ret || t->queue_count || t->async_pending)
		goto unlock;
	if (m9_faa_untag_virtual_restore(mdev))
		goto unlock;
	t->faa = NULL;
	t->faa_retiring = r;
	t->faa_virtual_primary = false;
	t->faa_source_context = false;
	m9x_triangle_adopt_context_locked(mdev);
	ret = m9x_transport_rearm_locked(mdev);
	if (ret) {
		t->wedged = true;
		t->generation++;
	}
	dev_info(&mdev->pdev->dev,
		 "M9 FAA released graph detached at %#llx: requester back on its staging slots (%d)\n",
		 t->completed_point, ret);
unlock:
	mutex_unlock(&t->lock);
#endif
}

bool m9x_faa_graph_release_unposted(struct m9x_device *mdev,
				    struct m9_faa_graph_binding *binding)
{
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;

	mutex_lock(&t->lock);
	r = t->faa;
	if (!r || !binding || r->graph_binding != binding ||
	    r->graph_preparing || r->attempted || t->faa_virtual_primary ||
	    t->wedged || t->queue_count || t->async_pending) {
		mutex_unlock(&t->lock);
		return false;
	}
	m9_faa_unmap_system_locked(mdev, r);
	t->faa = NULL;
	/* Only this replay's INIT could have claimed the context. */
	t->faa_source_context = false;
	mutex_unlock(&t->lock);
	if (m9_faa_release(mdev, r)) {
		dev_info(&mdev->pdev->dev,
			 "M9 FAA unposted graph released; requester unchanged\n");
		return true;
	}
	mutex_lock(&t->lock);
	t->faa = r;
	mutex_unlock(&t->lock);
	return false;
}

static int m9_faa_allocate(struct m9x_device *mdev, struct m9_faa_owner *o,
			   const struct m9_faa_owner_spec *spec, bool defer_output)
{
	struct drm_gem_vram_object *vram;
	u64 end, base;
	s64 offset;
	int ret;

	o->bytes = PAGE_ALIGN(spec->bytes);
	if (!spec->local) {
		if ((spec->flags30 && !spec->system) ||
		    (spec->system && (!spec->mapped_bytes ||
		     !IS_ALIGNED(spec->mapped_bytes, PAGE_SIZE) ||
		     spec->mapped_bytes > o->bytes)))
			return -EINVAL;
		o->cpu = dma_alloc_coherent(&mdev->pdev->dev, o->bytes,
					    &o->dma, GFP_KERNEL);
		if (!o->cpu)
			return -ENOMEM;
		if (upper_32_bits(o->dma) || !IS_ALIGNED(o->dma, PAGE_SIZE) ||
		    (u64)o->dma + o->bytes > U32_MAX)
			return -ERANGE;
		o->base = lower_32_bits(o->dma);
		if (spec->system && spec->flags30) {
			ret = m9_faa_dma_prepare_wc(mdev, o);
			if (ret)
				return ret;
		}
		if (spec->system) {
			if (defer_output) {
				if (!spec->flags30 || spec->mapped_bytes != 0x50000)
					return -EINVAL;
				ret = m9x_address_reserve_faa_output(mdev,
						&o->system_node, spec->mapped_bytes);
			} else {
				ret = spec->flags30 ?
					m9x_address_map_faa_system(mdev, &o->system_node,
							  o->dma, spec->mapped_bytes) :
					m9x_address_map_system(mdev, &o->system_node,
							      o->dma, spec->mapped_bytes);
			}
			if (ret)
				return ret;
			o->base = o->system_node.start;
		}
		return 0;
	}
	vram = drm_gem_vram_create(&mdev->drm, o->bytes, 1);
	if (IS_ERR(vram))
		return PTR_ERR(vram);
	ret = m9x_vram_pin(vram);
	if (ret)
		goto put;
	offset = drm_gem_vram_offset(vram);
	if (offset < 0 || check_add_overflow((u64)offset, (u64)o->bytes, &end) ||
	    end > m9x_vram_gpu_limit(mdev) ||
	    check_add_overflow(M9_VRAM_GPU_BASE, (u64)offset, &base) ||
	    base + o->bytes > U32_MAX || !IS_ALIGNED(base, PAGE_SIZE)) {
		ret = offset < 0 ? offset : -ERANGE;
		goto unpin;
	}
	ret = ttm_bo_reserve(&vram->bo, true, false, NULL);
	if (ret)
		goto unpin;
	ret = drm_gem_vram_vmap(vram, &o->map);
	ttm_bo_unreserve(&vram->bo);
	if (ret)
		goto unpin;
	o->vram = vram;
	o->base = base;
	return 0;
unpin:
	m9x_vram_unpin(vram);
put:
	drm_gem_vram_put(vram);
	return ret;
}

/* Source owner28/29 are mapped during INIT preparation and remain retained.
 * Only program26 is allocated after INIT. Its TTM reservation must finish
 * outside TRANSPORT and before the six borrowed BO reservations. */
/* New local code is allocated outside TRANSPORT and all six graph BO
 * reservations. Invalid typed inputs leave the original INIT claim reusable. */
int m9x_faa_graph_color_prepare(struct m9x_device *mdev,
		struct m9_faa_graph_binding *binding, u32 generation,
		u64 init_point, const struct m9x_faa_color_input *input)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	static const struct m9_faa_owner_spec program_spec =
		{ 0, 0x1080, true, false, false, 0 };
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	struct m9_faa_owner program = { };
	struct m9_faa_color_storage *storage = NULL;
	struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS];
	struct m9x_faa_color_image image;
	const struct firmware *fw = NULL;
	struct m9x_faa_color_input typed_input;
	struct m9x_faa_surface_layout layout;
	u32 i;
	int ret;

	if (!binding || !input)
		return -EINVAL;
 if(!m9x_faa_graph_binding_layout(binding,&layout))return -ESTALE;
 typed_input=*input;typed_input.width=layout.width;typed_input.height=layout.height;input=&typed_input;
 if(input->layout.program_count==4&&!m9_faa_color_four_experimental)return -EOPNOTSUPP;
	ret = request_firmware(&fw, input->layout.program_count==4?M9_FAA_GRAPH_COLOR_FOUR_FIRMWARE:M9_FAA_GRAPH_COLOR_FIRMWARE,
			       &mdev->pdev->dev);
	if (ret)
		return ret;
	if (!(input->layout.program_count==4?m9x_faa_color_four_template_valid(fw->data,fw->size):m9x_faa_color_template_valid(fw->data, fw->size))) {
		ret = -EINVAL;
		goto release;
	}
	storage = kzalloc(sizeof(*storage), GFP_KERNEL);
	if (!storage) {
		ret = -ENOMEM;
		goto release;
	}
	if (input->layout.program_bytes > sizeof(storage->program) ||
     input->layout.constants_bytes > 16384 || input->layout.geometry_bytes > M9_FAA_COLOR_GEOMETRY_CAPACITY ||
     input->record_stride<16 || input->record_stride>256 || (input->record_stride&15) ||
     !input->record_count || input->record_count>12288) {
  ret=-ERANGE;goto release;
 }
 storage->geometry_offset=ALIGN(input->layout.constants_bytes,128);
 if(storage->geometry_offset+input->layout.geometry_bytes+768>0x40000){ret=-ERANGE;goto release;}
 storage->constants=kzalloc(storage->geometry_offset+input->layout.geometry_bytes+768,GFP_KERNEL);
 if (!storage->constants) {ret=-ENOMEM;goto release;}
 storage->constants_bytes=input->layout.constants_bytes;
 storage->geometry_bytes=128+ALIGN(input->record_count,4)*input->record_stride;
 storage->program_bytes=input->layout.program_bytes;
 ret = m9_faa_allocate(mdev, &program, &program_spec, false);
	if (ret)
		goto release;
	mutex_lock(&t->lock);
	ret = m9_faa_graph_continuation_state(t, generation, init_point);
	if (ret)
		goto unlock_uninstalled;
	r = t->faa;
	if (r->graph_binding != binding || r->draw_owner[0].vram ||
	    r->public_vs_image || r->color_image || r->draw_image) {
		ret = -ESTALE;
		goto unlock_uninstalled;
	}
	for (i = 1; i < ARRAY_SIZE(r->draw_owner); i++) {
		struct m9_faa_owner *o = &r->draw_owner[i];

		if (!o->cpu || !drm_mm_node_allocated(&o->system_node) ||
		    o->system_node.start != o->base) {
			ret = -ESTALE;
			goto unlock_uninstalled;
		}
	}
	for (i = 0; i < M9_FAA_DRAW_SOURCE_OWNERS; i++) {
		struct m9_faa_owner *o;

		switch (i) {
		case 26: o = &program; break;
		case 27: o = &r->owner[26]; break;
		case 28: o = &r->draw_owner[1]; break;
		case 29: o = &r->draw_owner[2]; break;
		case 30: o = &r->owner[27]; break;
		default: o = &r->owner[i]; break;
		}
		owner[i].base = o->base;
		owner[i].bytes = o->bytes;
	}
	image = (struct m9x_faa_color_image) {
		.program = storage->program, .constants = storage->constants,
		.secondary = storage->secondary, .primary = storage->primary,
		.program_bytes = sizeof(storage->program),
		.constants_bytes = storage->geometry_offset + input->layout.geometry_bytes + 768,
  .secondary_length=&storage->secondary_bytes,
		.secondary_bytes = sizeof(storage->secondary),
		.primary_bytes = sizeof(storage->primary),
	};
	/* Validate/encode before claiming INIT or publishing this local mapping. */
	ret = m9x_faa_color_encode(fw->data, fw->size, owner,
				   m9_faa_first_bytes(r, init_point),
				   m9_faa_first_origin(r, init_point), input,
				   &image);
	if (ret)
		goto unlock_uninstalled;
	ret = m9x_faa_graph_draw_claim_locked(mdev, binding, generation, init_point);
	if (ret)
		goto unlock_uninstalled;
	storage->layout = input->layout;
 storage->primary_length=input->layout.program_count==4?3696:3652;
 storage->draw_length=input->layout.program_count==4?124:88;
 storage->lock_delta=input->layout.program_count==4?0xe0:0xa0;
	r->draw_owner[0] = program;
	memset(&program, 0, sizeof(program));
	r->color_image = storage;
	storage = NULL;
	/* This class-1 identity publication is distinct from the CPU-root map
	 * after314. Once attempted, all private owners remain with the replay. */
	ret = m9x_address_local_prepare(mdev, r->draw_owner[0].base,
					 r->draw_owner[0].bytes);
	if (ret) {
		if (!t->wedged) {
			t->wedged = true;
			t->generation++;
		}
	} else {
		r->color_payload_ready = true;
	}
	mutex_unlock(&t->lock);
	goto release;
unlock_uninstalled:
	mutex_unlock(&t->lock);
	/* This allocation was never installed or made GPU reachable. TTM
	 * cleanup must stay outside TRANSPORT and caller BO reservations. */
	if (WARN_ON(ttm_bo_reserve(&program.vram->bo, false, false, NULL))) {
		ret = -EIO;
		goto release;
	}
	drm_gem_vram_vunmap(program.vram, &program.map);
	ttm_bo_unreserve(&program.vram->bo);
	m9x_vram_unpin(program.vram);
	drm_gem_vram_put(program.vram);
release:
	if (storage) kfree(storage->constants);
 kfree(storage);
	release_firmware(fw);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

int m9x_faa_graph_public_vs_prepare(struct m9x_device *mdev,
		struct m9_faa_graph_binding *binding, u32 generation,
		u64 init_point, const struct m9x_faa_public_vs_input *input)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	static const struct m9_faa_owner_spec program_spec =
		{ 0, 0x1080, true, false, false, 0 };
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	struct m9_faa_owner program = { };
	struct m9_faa_public_vs_storage *storage = NULL;
	struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS];
	struct m9x_faa_public_vs_image image;
	const struct firmware *fw = NULL;
	u32 i;
	int ret;

	if (!binding || !input)
		return -EINVAL;
	ret = request_firmware(&fw, M9_FAA_GRAPH_PUBLIC_VS_FIRMWARE,
			       &mdev->pdev->dev);
	if (ret)
		return ret;
	if (!m9x_faa_public_vs_template_valid(fw->data, fw->size)) {
		ret = -EINVAL;
		goto release;
	}
	storage = kzalloc(sizeof(*storage), GFP_KERNEL);
	if (!storage) {
		ret = -ENOMEM;
		goto release;
	}
	ret = m9_faa_allocate(mdev, &program, &program_spec, false);
	if (ret)
		goto release;
	mutex_lock(&t->lock);
	ret = m9_faa_graph_continuation_state(t, generation, init_point);
	if (ret)
		goto unlock_uninstalled;
	r = t->faa;
	/* The public VS templates continue INIT itself, not an interlude. */
	if (m9_faa_ordinary_after(r, init_point)) {
		ret = -EOPNOTSUPP;
		goto unlock_uninstalled;
	}
	if (r->graph_binding != binding || r->draw_owner[0].vram ||
	    r->public_vs_image || r->draw_image) {
		ret = -ESTALE;
		goto unlock_uninstalled;
	}
	for (i = 1; i < ARRAY_SIZE(r->draw_owner); i++) {
		struct m9_faa_owner *o = &r->draw_owner[i];

		if (!o->cpu || !drm_mm_node_allocated(&o->system_node) ||
		    o->system_node.start != o->base) {
			ret = -ESTALE;
			goto unlock_uninstalled;
		}
	}
	for (i = 0; i < M9_FAA_DRAW_SOURCE_OWNERS; i++) {
		struct m9_faa_owner *o;

		switch (i) {
		case 26: o = &program; break;
		case 27: o = &r->owner[26]; break;
		case 28: o = &r->draw_owner[1]; break;
		case 29: o = &r->draw_owner[2]; break;
		case 30: o = &r->owner[27]; break;
		default: o = &r->owner[i]; break;
		}
		owner[i].base = o->base;
		owner[i].bytes = o->bytes;
	}
	image = (struct m9x_faa_public_vs_image) {
		.program = storage->program, .constants = storage->constants,
		.secondary = storage->secondary, .primary = storage->primary,
		.program_bytes = sizeof(storage->program),
		.constants_bytes = sizeof(storage->constants),
		.secondary_bytes = sizeof(storage->secondary),
		.primary_bytes = sizeof(storage->primary),
	};
	/* Validate/encode before claiming INIT or publishing this local mapping. */
	ret = m9x_faa_public_vs_encode(fw->data, fw->size, owner,
			r->primary_bytes, init_point, input, &image, &storage->ends);
	if (ret)
		goto unlock_uninstalled;
	ret = m9x_faa_graph_draw_claim_locked(mdev, binding, generation, init_point);
	if (ret)
		goto unlock_uninstalled;
	storage->layout = input->layout;
	r->draw_owner[0] = program;
	memset(&program, 0, sizeof(program));
	r->public_vs_image = storage;
	storage = NULL;
	/* This class-1 identity publication is distinct from the CPU-root map
	 * after314. Once attempted, all private owners remain with the replay. */
	ret = m9x_address_local_prepare(mdev, r->draw_owner[0].base,
					 r->draw_owner[0].bytes);
	if (ret) {
		if (!t->wedged) {
			t->wedged = true;
			t->generation++;
		}
	} else {
		r->public_vs_payload_ready = true;
	}
	mutex_unlock(&t->lock);
	goto release;
unlock_uninstalled:
	mutex_unlock(&t->lock);
	/* This allocation was never installed or made GPU reachable. TTM
	 * cleanup must stay outside TRANSPORT and caller BO reservations. */
	if (WARN_ON(ttm_bo_reserve(&program.vram->bo, false, false, NULL))) {
		ret = -EIO;
		goto release;
	}
	drm_gem_vram_vunmap(program.vram, &program.map);
	ttm_bo_unreserve(&program.vram->bo);
	m9x_vram_unpin(program.vram);
	drm_gem_vram_put(program.vram);
release:
	kfree(storage);
	release_firmware(fw);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

int m9x_faa_graph_draw_prepare(struct m9x_device *mdev,
		struct m9_faa_graph_binding *binding, u32 generation,
		u64 init_point, bool with_finish, bool with_lock)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	static const struct m9_faa_owner_spec program_spec =
		{ 0, 0x1080, true, false, false, 0 };
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	struct m9_faa_owner program = { };
	struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS];
	struct m9x_faa_graph_draw_image image;
	const struct firmware *fw = NULL;
	const struct firmware *tail_fw = NULL;
	const struct firmware *lock_fw = NULL;
	bool published = false;
	u32 i;
	int ret;

	if (with_lock && !with_finish)
		return -EINVAL;
	ret = request_firmware(&fw, M9_FAA_GRAPH_DRAW_INPUT_FIRMWARE,
			       &mdev->pdev->dev);
	if (ret)
		return ret;
	if (!m9x_faa_graph_draw_blob_valid(fw->data, fw->size)) {
		release_firmware(fw);
		return -EINVAL;
	}
	if (with_finish) {
		ret = request_firmware(&tail_fw, M9_FAA_GRAPH_DRAW_TAIL_FIRMWARE,
				       &mdev->pdev->dev);
		if (ret)
			goto preclaim_fail;
		if (!m9x_faa_graph_draw_tail_blob_valid(tail_fw->data, tail_fw->size)) {
			ret = -EINVAL;
			goto preclaim_fail;
		}
	}
	if (with_lock) {
		ret = request_firmware(&lock_fw, M9_FAA_GRAPH_LOCK_FIRMWARE,
				       &mdev->pdev->dev);
		if (ret)
			goto preclaim_fail;
		if (!m9x_faa_graph_lock_blob_valid(lock_fw->data, lock_fw->size)) {
			ret = -EINVAL;
			goto preclaim_fail;
		}
	}
	/* The only TTM reservation in this path is complete before taking
	 * TRANSPORT and before touching the retained replay.  A concurrent
	 * teardown cannot free r across this unlocked preparation interval. */
	ret = m9_faa_allocate(mdev, &program, &program_spec, false);
	if (ret)
		goto preclaim_fail;
	mutex_lock(&t->lock);
	/* The draw templates continue INIT itself, not an interlude. */
	ret = t->faa && m9_faa_ordinary_after(t->faa, init_point) ?
	      -EOPNOTSUPP :
	      m9x_faa_graph_draw_claim_locked(mdev, binding, generation,
					      init_point);
	if (ret)
		goto no_claim;
	r = t->faa;
	r->draw_owner[0] = program;
	memset(&program, 0, sizeof(program));
	/* Source constants and preprocessing secondary are pre-existing
	 * flags30 MDLs. The graph INIT retained their two private mappings;
	 * only this local program owner is newly published after INIT. */
	for (i = 1; i < ARRAY_SIZE(r->draw_owner); i++)
		if (!r->draw_owner[i].cpu ||
		    !drm_mm_node_allocated(&r->draw_owner[i].system_node)) {
			ret = -ESTALE;
			goto unlock_fail;
		}
	/* Local identity publication is separate from the later CPU-root
	 * program access and precedes the source frontend314 kick. */
	published = true;
	ret = m9x_address_local_prepare(mdev, r->draw_owner[0].base,
					 r->draw_owner[0].bytes);
	if (ret)
		goto unlock_fail;
	for (i = 0; i < M9_FAA_DRAW_SOURCE_OWNERS; i++) {
		struct m9_faa_owner *o;

		switch (i) {
		case 26: o = &r->draw_owner[0]; break;
		case 27: o = &r->owner[26]; break;
		case 28: o = &r->draw_owner[1]; break;
		case 29: o = &r->draw_owner[2]; break;
		case 30: o = &r->owner[27]; break;
		default: o = &r->owner[i]; break;
		}
		owner[i].base = o->base;
		owner[i].bytes = o->bytes;
	}
	r->draw_image = kzalloc(sizeof(*r->draw_image), GFP_KERNEL);
	if (!r->draw_image) {
		ret = -ENOMEM;
		goto unlock_fail;
	}
	image = (struct m9x_faa_graph_draw_image) {
		.program = r->draw_image->program,
		.constants = r->draw_image->constants,
		.secondary = r->draw_image->secondary,
		.primary_suffix = r->draw_image->primary_suffix,
		.program_bytes = sizeof(r->draw_image->program),
		.constants_bytes = sizeof(r->draw_image->constants),
		.secondary_bytes = sizeof(r->draw_image->secondary),
		.primary_suffix_bytes = sizeof(r->draw_image->primary_suffix),
	};
	ret = m9x_faa_graph_draw_encode(fw->data, fw->size, owner,
					 r->primary_bytes, init_point, &image);
	if (ret)
		goto unlock_fail;
	if (with_finish) {
		struct m9x_faa_graph_draw_tail_image tail_image = {
			.secondary = r->draw_image->tail_secondary,
			.primary = r->draw_image->tail_primary,
			.secondary_bytes = sizeof(r->draw_image->tail_secondary),
			.primary_bytes = sizeof(r->draw_image->tail_primary),
		};

		/* PREPARE's live graph admission already enforces 320x240 and
		 * the fixed diagnostic companion layout, before binding/pinning. */
		ret = m9x_faa_graph_draw_tail_encode(tail_fw->data, tail_fw->size,
			owner, r->primary_bytes + M9_FAA_DRAW_PRIMARY_SUFFIX_BYTES,
			320, 240, init_point + 0x60, &tail_image);
		if (ret)
			goto unlock_fail;
	}
	if (with_lock) {
		ret = m9x_faa_graph_lock_encode(lock_fw->data, lock_fw->size,
			owner, r->primary_bytes + M9_FAA_DRAW_PRIMARY_SUFFIX_BYTES +
			M9_FAA_DRAW_TAIL_PRIMARY_BYTES, 320, 240, init_point + 0x90,
			r->draw_image->lock_primary, sizeof(r->draw_image->lock_primary));
		if (ret)
			goto unlock_fail;
	}
	r->graph_draw_with_lock = with_lock;
	r->graph_draw_with_finish = with_finish;
	r->graph_draw_payload_ready = true;
	mutex_unlock(&t->lock);
	release_firmware(lock_fw);
	release_firmware(tail_fw);
	release_firmware(fw);
	return 0;
unlock_fail:
	/* A successfully committed class-1 map or local identity may now be
	 * visible. Preserve all owners until bus-master-off; pre-publication
	 * allocation failures leave the transport healthy but the one-shot
	 * graph claim closed. */
	if ((published || t->wedged) && !t->wedged) {
		t->wedged = true;
		t->generation++;
	}
	mutex_unlock(&t->lock);
	release_firmware(lock_fw);
	release_firmware(tail_fw);
	release_firmware(fw);
	return ret;
no_claim:
	mutex_unlock(&t->lock);
	/* The uninstalled local owner never became GPU-reachable. */
	if (WARN_ON(ttm_bo_reserve(&program.vram->bo, false, false,
				    NULL))) {
		/* A failed reserve cannot safely vunmap this private TTM view.
		 * Preserve its allocation rather than returning a dangling map. */
		release_firmware(lock_fw);
		release_firmware(tail_fw);
		release_firmware(fw);
		return -EIO;
	}
	drm_gem_vram_vunmap(program.vram, &program.map);
	ttm_bo_unreserve(&program.vram->bo);
	m9x_vram_unpin(program.vram);
	drm_gem_vram_put(program.vram);
	program.vram = NULL;
preclaim_fail:
	release_firmware(lock_fw);
	release_firmware(tail_fw);
	release_firmware(fw);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

static void m9_faa_write(struct m9_faa_owner *o, u32 offset,
			 const void *source, u32 bytes)
{
	if (o->vram)
		iosys_map_memcpy_to(&o->map, offset, source, bytes);
	else
		memcpy((u8 *)o->cpu + offset, source, bytes);
}

/* FNV is a compact observational label; admission uses byte-exact equality. */
static u32 m9_faa_hash(u32 hash, const u8 *bytes, u32 count)
{
	u32 i;

	for (i = 0; i < count; i++)
		hash = (hash ^ bytes[i]) * 0x01000193;
	return hash;
}

/* Read only firmware-proven local spans. Unknown allocation tails remain
 * untouched and are not admitted as source bytes by this check. This checks
 * the upload through its BAR mapping; GPU identity leaves are checked later.
 */
static int m9_faa_verify_upload(struct m9x_device *mdev,
			      struct m9x_faa_replay *r,
			      const struct firmware *fw)
{
	u32 expected[ARRAY_SIZE(r->owner)] = { 0 };
	u32 observed[ARRAY_SIZE(r->owner)] = { 0 };
	u32 known[ARRAY_SIZE(r->owner)] = { 0 };
	u8 data[256];
	u32 i, offset, count, j;

	/* TTM mappings and admitted SYSTEM owners may be WC. dma_wmb()
	 * alone is only a compiler
	 * barrier on x86 and does not publish outstanding WC VRAM writes.
	 */
	wmb();
	for (i = 0; i < ARRAY_SIZE(m9_faa_spans); i++) {
		const struct m9_faa_span *s = &m9_faa_spans[i];
		struct m9_faa_owner *o = &r->owner[s->owner];

#ifdef M9_FAA_PROGRAM_UPLOAD_OWNER
		if (s->owner == M9_FAA_PROGRAM_UPLOAD_OWNER)
			continue;
#endif
		if (!o->vram)
			continue;
		if (!known[s->owner]) {
			expected[s->owner] = 0x811c9dc5;
			observed[s->owner] = 0x811c9dc5;
		}
		for (offset = 0; offset < s->bytes; offset += count) {
			const u8 *source = fw->data + s->blob_offset + offset;

			count = min_t(u32, sizeof(data), s->bytes - offset);
			iosys_map_memcpy_from(data, &o->map, s->offset + offset, count);
			if (memcmp(data, source, count)) {
				for (j = 0; j < count; j++)
					if (data[j] != source[j])
						break;
				dev_err(&mdev->pdev->dev,
					"M9 FAA upload mismatch: owner=%u base=%#x offset=%#x expected=%#x observed=%#x\n",
					s->owner, o->base, s->offset + offset + j,
					source[j], data[j]);
				return -EIO;
			}
			expected[s->owner] = m9_faa_hash(expected[s->owner], source, count);
			observed[s->owner] = m9_faa_hash(observed[s->owner], data, count);
		}
		known[s->owner] += s->bytes;
	}
	for (i = 0; i < ARRAY_SIZE(r->owner); i++)
		if (known[i])
			dev_info(&mdev->pdev->dev,
				 "M9 FAA known upload verified: owner=%u base=%#x bytes=%u expected_fnv=%#x observed_fnv=%#x\n",
				 i, r->owner[i].base, known[i], expected[i], observed[i]);
	return 0;
}

/* Bounded observations before submit and after actual retirement. Includes
 * local stores, retained SYSTEM mapped prefixes and exact coherent-DMA
 * allocation spans, but never claims unknown
 * initial contents were zero or that a nonzero byte proves a GPU store.
 * Unmapped allocation tails are excluded. No allocation is written.
 */
static void m9_faa_observe(struct m9x_device *mdev,
			 struct m9x_faa_replay *r, bool before)
{
	const u32 budget = 16 * 1024 * 1024;
	u8 data[256];
	u32 i, offset, count, j, local_total = 0, system_total = 0;

	for (i = 0; i < ARRAY_SIZE(r->owner); i++) {
		const struct m9_faa_owner_spec *spec = &m9_faa_owner_specs[i];
		u32 bytes, *total;

		if (r->owner[i].vram) {
			bytes = spec->bytes;
			total = &local_total;
		} else if (r->owner[i].cpu) {
			bytes = spec->system ? spec->mapped_bytes : spec->bytes;
			total = &system_total;
		} else {
			continue;
		}
		if (bytes > r->owner[i].bytes || bytes > budget - *total) {
			dev_err(&mdev->pdev->dev, "M9 FAA observation exceeds %u-byte budget\n",
				budget);
			return;
		}
		*total += bytes;
	}
	/* Order publication/completion observations before BAR and DMA reads. */
	rmb();
	dma_rmb();
	if (mdev->transport.fence_cpu) {
		u32 *counter = mdev->transport.fence_cpu;

		/* Observe the same retained completion page selected by 106c
		 * and 1068/1070. Source query/map waits use +20/+00;
		 * observing +40/+60 does not assign them another wait role. */
		dev_info(&mdev->pdev->dev,
			 "M9 FAA %s counters: +00=%#x/%#x +20=%#x/%#x +40=%#x/%#x +60=%#x/%#x\n",
			 before ? "before submit" : "retired",
			 READ_ONCE(counter[0]), READ_ONCE(counter[1]),
			 READ_ONCE(counter[0x20 / 4]), READ_ONCE(counter[0x24 / 4]),
			 READ_ONCE(counter[0x40 / 4]), READ_ONCE(counter[0x44 / 4]),
			 READ_ONCE(counter[0x60 / 4]), READ_ONCE(counter[0x64 / 4]));
	}
	for (i = 0; i < ARRAY_SIZE(r->owner); i++) {
		struct m9_faa_owner *o = &r->owner[i];
		const struct m9_faa_owner_spec *spec = &m9_faa_owner_specs[i];
		u32 bytes = spec->system ? spec->mapped_bytes : spec->bytes;
		u32 hash = 0x811c9dc5, nonzero = 0, nonzero_words = 0;
		const char *domain = o->vram ? "BAR" : "DMA";

		if (!o->vram && !o->cpu)
			continue;
		for (offset = 0; offset < bytes; offset += count) {
			count = min_t(u32, sizeof(data), bytes - offset);
			if (o->vram)
				iosys_map_memcpy_from(data, &o->map, offset, count);
			else
				memcpy(data, (u8 *)o->cpu + offset, count);
			if (!offset) {
				u32 prefix = min_t(u32, 20, count);

				/* Reuse the already bounded observation read. This
				 * is raw evidence, including unknown source bytes;
				 * no owner role or GPU record meaning is assigned. */
				dev_info(&mdev->pdev->dev,
					 "M9 FAA %s %s raw prefix: owner=%u native=%#x bytes=%u hex=%*phN\n",
					 before ? "before submit" : "retired",
					 domain, i, o->base, prefix, (int)prefix, data);
			}
			hash = m9_faa_hash(hash, data, count);
			for (j = 0; j < count; j++)
				nonzero += !!data[j];
			for (j = 0; j + sizeof(u32) <= count; j += sizeof(u32))
				nonzero_words += !!get_unaligned_le32(data + j);
		}
		if (before) {
			r->before_hash[i] = hash;
			r->before_nonzero[i] = nonzero;
			r->before_nonzero_words[i] = nonzero_words;
		}
		dev_info(&mdev->pdev->dev,
			 "M9 FAA %s %s observation: owner=%u captured=%#x native=%#x bytes=%u fnv=%#x nonzero_bytes=%u nonzero_words=%u\n",
			 before ? "before submit" : "retired",
			 domain, i, spec->captured, o->base, bytes,
			 hash, nonzero, nonzero_words);
		if (!before && r->baseline_valid)
			dev_info(&mdev->pdev->dev,
				 "M9 FAA %s delta: owner=%u before_fnv=%#x after_fnv=%#x hash_changed=%u before_nonzero_bytes=%u after_nonzero_bytes=%u before_nonzero_words=%u after_nonzero_words=%u\n",
				 domain, i, r->before_hash[i], hash, r->before_hash[i] != hash,
				 r->before_nonzero[i], nonzero,
				 r->before_nonzero_words[i], nonzero_words);
	}
	if (before)
		r->baseline_valid = true;
}

#ifdef M9_FAA_GRAPH_INIT_CONTROL
/* Read-only evidence after the separately qualified public-VS prefix. These
 * opaque GPU-owned bytes are never decoded or used as source CPU inputs. */
static void m9_faa_public_vs_prefix_observe(struct m9x_device *mdev,
					 struct m9x_faa_replay *r)
{
	struct m9_faa_owner *o = &r->owner[23];
	struct iosys_map restore;
	u8 data[256];
	u32 base, offset, count, j, hash, nonzero, words[20];
	static const u32 source_offsets[] = { 0, 873472, 3493888 };
	const u32 bytes = m9_faa_owner_specs[23].bytes;

	lockdep_assert_held(&mdev->transport.lock);
	if (!r->public_vs_prefix_retired || !o->vram || o->cpu ||
	    bytes > o->bytes || bytes > 8 * 1024 * 1024 ||
	    m9x_address_restore_region(mdev, &restore, &base) ||
	    base != mdev->transport.restore_base || PARHELIA_M9_RESTORE_BYTES < 8192) {
		dev_info(&mdev->pdev->dev, "M9 FAA public VS prefix observation refused\n");
		return;
	}
	for (j = 0; j < ARRAY_SIZE(source_offsets); j++)
		if (source_offsets[j] > bytes || bytes - source_offsets[j] < 64)
			return;
	rmb();
	dma_rmb();
	hash = 0x811c9dc5;
	nonzero = 0;
	for (offset = 0; offset < 8192; offset += sizeof(data)) {
		iosys_map_memcpy_from(data, &restore, offset, sizeof(data));
		hash = m9_faa_hash(hash, data, sizeof(data));
		for (j = 0; j < sizeof(data); j += 4)
			nonzero += !!get_unaligned_le32(data + j);
	}
	iosys_map_memcpy_from(words, &restore, 0x700 + 2 * 0x600, sizeof(words));
	dev_info(&mdev->pdev->dev,
		 "M9 FAA public VS prefix restore: base=%#x bytes=8192 fnv=%#x nonzero_words=%u "
		 "record2[3/9/10/11/12/14/15/19]=%#x/%#x/%#x/%#x/%#x/%#x/%#x/%#x\n",
		 base, hash, nonzero, words[3], words[9], words[10], words[11],
		 words[12], words[14], words[15], words[19]);
	hash = 0x811c9dc5;
	nonzero = 0;
	for (offset = 0; offset < bytes; offset += count) {
		count = min_t(u32, sizeof(data), bytes - offset);
		iosys_map_memcpy_from(data, &o->map, offset, count);
		hash = m9_faa_hash(hash, data, count);
		for (j = 0; j + 4 <= count; j += 4)
			nonzero += !!get_unaligned_le32(data + j);
	}
	dev_info(&mdev->pdev->dev,
		 "M9 FAA public VS prefix preprocessing: owner=23 native=%#x bytes=%u "
		 "fnv=%#x nonzero_words=%u engine=%#x\n", o->base, bytes, hash,
		 nonzero, m9x_read(mdev, M9_REG_ENGINE_STATUS));
	for (j = 0; j < ARRAY_SIZE(source_offsets); j++) {
		iosys_map_memcpy_from(data, &o->map, source_offsets[j], 64);
		dev_info(&mdev->pdev->dev,
			 "M9 FAA public VS prefix preprocessing raw: owner=23 offset=%#x bytes=64 hex=%*phN\n",
			 source_offsets[j], 64, data);
	}
}
#endif

/* The sampled primary is borrowed, so the ordinary owner loop intentionally
 * skips it. Observe the actual pinned/reserved caller BO without turning it
 * into an owned allocation or extending the GPU command stream. Failure to
 * map observation data never invents a zero baseline or changes submission. */
static void m9_faa_observe_borrowed_color(struct m9x_device *mdev,
					struct m9x_faa_replay *r,
					struct drm_gem_vram_object *color_bo,
					bool before)
{
#ifdef M9_FAA_MULTI_CLEAR
	const u32 i = M9_FAA_COLOR_OWNER;
	struct iosys_map map = IOSYS_MAP_INIT_VADDR(NULL);
	u8 data[256];
	u32 bytes, offset, count, j, hash = 0x811c9dc5;
	u32 nonzero = 0, nonzero_words = 0;
	s64 local_offset;
	u64 base;
	int ret;

	if (!color_bo || i >= ARRAY_SIZE(r->owner) || r->owner[i].vram ||
	    r->owner[i].cpu || !r->borrowed_color_bytes)
		return;
	bytes = m9_faa_owner_specs[i].bytes;
	local_offset = drm_gem_vram_offset(color_bo);
	if (!bytes || bytes > 1024 * 1024 || (bytes & 3) || local_offset < 0 ||
	    bytes > r->borrowed_color_bytes || bytes > color_bo->bo.base.size ||
	    check_add_overflow(M9_VRAM_GPU_BASE, (u64)local_offset, &base) ||
	    base != r->owner[i].base)
		return;
	/* m9_submit_first_triangle owns this reservation across our call. */
	ret = drm_gem_vram_vmap(color_bo, &map);
	if (ret) {
		dev_warn(&mdev->pdev->dev, "M9 FAA borrowed observation unavailable: %d\n", ret);
		return;
	}
	rmb();
	for (offset = 0; offset < bytes; offset += count) {
		count = min_t(u32, sizeof(data), bytes - offset);
		iosys_map_memcpy_from(data, &map, offset, count);
		hash = m9_faa_hash(hash, data, count);
		for (j = 0; j < count; j++)
			nonzero += !!data[j];
		for (j = 0; j < count; j += sizeof(u32))
			nonzero_words += !!get_unaligned_le32(data + j);
	}
	drm_gem_vram_vunmap(color_bo, &map);
	if (before) {
		r->before_hash[i] = hash;
		r->before_nonzero[i] = nonzero;
		r->before_nonzero_words[i] = nonzero_words;
		r->borrowed_color_observed = true;
	}
	dev_info(&mdev->pdev->dev,
		 "M9 FAA %s BAR observation: owner=%u captured=%#x native=%#x bytes=%u fnv=%#x nonzero_bytes=%u nonzero_words=%u\n",
		 before ? "before submit" : "retired", i,
		 m9_faa_owner_specs[i].captured, r->owner[i].base, bytes,
		 hash, nonzero, nonzero_words);
	if (!before && r->borrowed_color_observed)
		dev_info(&mdev->pdev->dev,
			 "M9 FAA BAR delta: owner=%u before_fnv=%#x after_fnv=%#x hash_changed=%u before_nonzero_bytes=%u after_nonzero_bytes=%u before_nonzero_words=%u after_nonzero_words=%u\n",
			 i, r->before_hash[i], hash, r->before_hash[i] != hash,
			 r->before_nonzero[i], nonzero,
			 r->before_nonzero_words[i], nonzero_words);
#else
	(void)mdev;
	(void)r;
	(void)color_bo;
	(void)before;
#endif
}

/* Allocations precede the caller's color BO reservation, as in ordinary
 * FIRST_TRIANGLE. Every private owner survives both publication and timeout. */
static int m9_faa_prepare(struct m9x_device *mdev,
			  struct m9_faa_graph_binding *binding,
			  u32 generation)
{
	const struct firmware *fw = NULL;
	struct m9x_faa_replay *r = NULL;
	u32 i, hash = 0x811c9dc5;
	bool locked = true;
	int ret;

	if ((!m9_faa_diag && !binding) || mdev->pdev->device != 0x0540)
		return -EACCES;
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	if (!binding)
		return -EOPNOTSUPP;
#else
	if (binding)
		return -EOPNOTSUPP;
#endif
	if (m9_faa_defer_output_map) {
#ifndef M9_FAA_PRECLEAR_OUTPUT_WRITE_CONTROL
		return -EOPNOTSUPP;
#else
		if (!m9_faa_phased_submit || M9_FAA_OUTPUT_OWNER == U32_MAX)
			return -EINVAL;
#endif
	}
	if (m9_faa_program_access_kick) {
#ifndef M9_FAA_PROGRAM_ACCESS_CONTROL
		return -EOPNOTSUPP;
#else
		if (!m9_faa_phased_submit)
			return -EINVAL;
#endif
	}
#ifdef M9_FAA_PROGRAM_UPLOAD_OWNER
	/* A deferred upload cannot be bypassed by selecting a single post or
	 * disabling the callback: that would submit unknown program bytes. */
	if (!m9_faa_phased_submit || !m9_faa_program_access_kick ||
	    M9_FAA_PROGRAM_UPLOAD_OWNER >= ARRAY_SIZE(m9_faa_owner_specs) ||
	    M9_FAA_PROGRAM_UPLOAD_OWNER == M9_FAA_COLOR_OWNER)
		return -EINVAL;
#endif
	/* This replay joins tagged virtual fetches to the driver's identity
 * leaves. A diagnostic local-base override has no such proven contract. */
	if (m9x_local_gpu_base)
		return -EOPNOTSUPP;
	mutex_lock(&mdev->transport.lock);
	if (mdev->transport.wedged) {
		ret = -EIO;
		goto unlock;
	}
	if (mdev->transport.faa) {
		ret = binding ? -EBUSY :
			(mdev->transport.faa->attempted ? -EALREADY : 0);
		goto unlock;
	}
	/* Engine prerequisite allocations must also finish outside COLOR RESV. */
	mutex_unlock(&mdev->transport.lock);
	ret = m9x_triangle_prepare_user(mdev);
	if (ret)
		return ret;
	mutex_lock(&mdev->transport.lock);
	if (mdev->transport.faa || mdev->transport.wedged) {
		ret = mdev->transport.wedged ? -EIO :
			(binding ? -EBUSY :
			 (mdev->transport.faa->attempted ? -EALREADY : 0));
		goto unlock;
	}
	/* The source INIT is the requester's first command; refuse it before
	 * claiming the producer slot, so a refusal leaves no replay behind.
	 */
	if (binding && !m9_faa_transport_fresh(&mdev->transport) &&
	    !m9_faa_transport_continuable(mdev)) {
		ret = -ESTALE;
		goto unlock;
	}
	if (binding) {
		/* Claim the native producer slot first, then drop TRANSPORT
		 * before any private TTM reservation. Other engine users refuse
		 * this unprepared first-context producer. */
		r = kzalloc(sizeof(*r), GFP_KERNEL);
		if (!r) {
			ret = -ENOMEM;
			goto unlock;
		}
		r->graph_preparing = true;
		mdev->transport.faa = r;
		mutex_unlock(&mdev->transport.lock);
		locked = false;
	}
	ret = request_firmware(&fw, M9_FAA_INPUT_FIRMWARE, &mdev->pdev->dev);
	if (ret)
		goto release_fw;
	if (fw->size != M9_FAA_BLOB_BYTES) {
		ret = -EINVAL;
		goto release_fw;
	}
	for (i = 0; i < fw->size; i++)
		hash = (hash ^ fw->data[i]) * 0x01000193;
	if (hash != M9_FAA_BLOB_FNV) {
		ret = -EINVAL;
		goto release_fw;
	}
	if (!r) {
		r = kzalloc(sizeof(*r), GFP_KERNEL);
		if (!r) {
			ret = -ENOMEM;
			goto release_fw;
		}
	}
	if (binding && !m9x_faa_graph_binding_layout(binding, &r->graph_layout)) {
		ret = -EINVAL;
		goto free;
	}
	for (i = 0; i < ARRAY_SIZE(r->owner); i++) {
		u32 role, address, source;
		struct m9_faa_owner_spec spec = m9_faa_owner_specs[i];

		if (binding) {
			for (role = 0; role < PARHELIA_M9_FAA_GRAPH_OWNERS; role++) {
				if (!m9x_faa_graph_binding_address(binding, role,
							  &address, &source)) {
					ret = -EINVAL;
					goto free;
				}
				if (source != i)
					continue;
				r->owner[i].base = address;
				r->owner[i].bytes = r->graph_layout.owner[role].capacity;
				break;
			}
			if (role != PARHELIA_M9_FAA_GRAPH_OWNERS)
				continue;
		}
#ifdef M9_FAA_GRAPH_INIT_CONTROL
		/* Original INIT1024 keeps all other private owner extents unchanged.
		 * Source roles6/7 are the primary SYSTEM view and companion HZ. */
		if (binding && i == 6) {
			spec.bytes = r->graph_layout.owner[M9_FAA_SURFACE_PRIMARY].capacity;
			spec.mapped_bytes = spec.bytes;
		} else if (binding && i == 7) {
			spec.bytes = r->graph_layout.owner[M9_FAA_SURFACE_HZ].capacity;
		}
#endif
		/* Only a profile with a sampled caller color substitutes that owner.
		 * An absent sentinel allocates every genuine source owner. */
		if (i == M9_FAA_COLOR_OWNER)
			continue;
		if (binding && !m9_faa_owner_specs[i].local)
			mutex_lock(&mdev->transport.lock);
		ret = m9_faa_allocate(mdev, &r->owner[i], &spec,
				      m9_faa_defer_output_map && i == M9_FAA_OUTPUT_OWNER);
		if (binding && !m9_faa_owner_specs[i].local)
			mutex_unlock(&mdev->transport.lock);
		if (ret)
			goto free;
	}
	if (M9_FAA_COUNTER_VIRTUAL) {
		if (binding)
			mutex_lock(&mdev->transport.lock);
		ret = m9x_address_map_system(mdev, &r->counter_node,
					    mdev->transport.fence_bus, PAGE_SIZE);
		if (binding)
			mutex_unlock(&mdev->transport.lock);
		if (ret)
			goto free;
	}
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	if (binding) {
		static const struct m9_faa_owner_spec preexisting[2] = {
			{ 0, 0x40000, false, true, true, 0x40000 },
			{ 0, 0x40000, false, true, true, 0x40000 },
		};

		/* Source owner28/29 are pre-existing 64-page flags30 MDLs. Keep
		 * primary and counter placement established by the INIT profile,
		 * then retain these two maps before its first command post. Their
		 * contents remain unknown until the separate DRAW caller. */
		for (i = 0; i < ARRAY_SIZE(preexisting); i++) {
			mutex_lock(&mdev->transport.lock);
			ret = m9_faa_allocate(mdev, &r->draw_owner[i + 1],
					      &preexisting[i], false);
			mutex_unlock(&mdev->transport.lock);
			if (ret)
				goto free;
		}
	}
#endif
	for (i = 0; i < ARRAY_SIZE(m9_faa_spans); i++) {
		const struct m9_faa_span *s = &m9_faa_spans[i];

		if (s->owner >= ARRAY_SIZE(r->owner) ||
		    s->offset > m9_faa_owner_specs[s->owner].bytes ||
		    s->bytes > m9_faa_owner_specs[s->owner].bytes - s->offset ||
		    s->blob_offset > fw->size || s->bytes > fw->size - s->blob_offset ||
		    (!binding && s->owner == M9_FAA_COLOR_OWNER)) {
			ret = -EINVAL;
			goto free;
		}
#ifdef M9_FAA_PROGRAM_UPLOAD_OWNER
		if (s->owner == M9_FAA_PROGRAM_UPLOAD_OWNER)
			continue;
#endif
		if (binding && s->owner == 8) {
			/* The caller BO is written only after the ioctl has acquired
			 * all six reservations. Never take its RESV under transport. */
			static const u32 init[5] = {
				0xa4a3a1a1, 0x28041968, 0, 0, 0,
			};

			if (s->offset || s->bytes != sizeof(init) ||
			    memcmp(fw->data + s->blob_offset, init, sizeof(init))) {
				ret = -EINVAL;
				goto free;
			}
			continue;
		}
		m9_faa_write(&r->owner[s->owner], s->offset,
			     fw->data + s->blob_offset, s->bytes);
	}
	ret = m9_faa_verify_upload(mdev, r, fw);
	if (ret)
		goto free;
#ifdef M9_FAA_PROGRAM_UPLOAD_OWNER
	r->program_inputs = fw;
	fw = NULL;
#endif
	if (binding) {
		mutex_lock(&mdev->transport.lock);
		locked = true;
		if (mdev->transport.faa != r || mdev->transport.wedged ||
		    mdev->transport.generation != generation ||
		    (!m9_faa_transport_fresh(&mdev->transport) &&
		     !m9_faa_transport_continuable(mdev))) {
			ret = -ESTALE;
			goto free;
		}
	}
	r->prepared = true;
	r->graph_binding = binding;
	r->graph_generation = generation;
	r->graph_preparing = false;
	mdev->transport.faa = r;
	dev_info(&mdev->pdev->dev, "M9 FAA private inputs: %zu owners, %u known bytes, %zu address/%zu tracker fixups\n",
		 ARRAY_SIZE(r->owner), M9_FAA_BLOB_BYTES,
		 ARRAY_SIZE(m9_faa_fixups), ARRAY_SIZE(m9_faa_trackers));
	ret = 0;
	goto release_fw;
free:
	if (!binding)
		m9_faa_release_failed_prepare(mdev, r);
release_fw:
	if (fw)
		release_firmware(fw);
unlock:
	if (locked)
		mutex_unlock(&mdev->transport.lock);
	if (binding && ret && r) {
		/* Graph preparation never posts. Unmap address nodes under the
		 * transport lock; release local TTM reservations outside it. A
		 * failed address publication can have exposed DMA or left the
		 * frontend frozen: retain every private owner until bus-master-off. */
		mutex_lock(&mdev->transport.lock);
		if (mdev->transport.wedged ||
		    mdev->transport.bus_master_enabled) {
			/* Address publication may already be visible. Preserve the
			 * in-progress slot and mappings until requester teardown. */
			mutex_unlock(&mdev->transport.lock);
			return ret;
		}
		for (i = 0; i < ARRAY_SIZE(r->owner); i++)
			if (!m9_faa_dma_restore_wb(&r->owner[i])) {
				mdev->transport.wedged = true;
				mutex_unlock(&mdev->transport.lock);
				return ret;
			}
#ifdef M9_FAA_GRAPH_INIT_CONTROL
		for (i = 0; i < ARRAY_SIZE(r->draw_owner); i++)
			if (!m9_faa_dma_restore_wb(&r->draw_owner[i])) {
				mdev->transport.wedged = true;
				mutex_unlock(&mdev->transport.lock);
				return ret;
			}
#endif
		if (drm_mm_node_allocated(&r->counter_node))
			m9x_address_unmap_system(mdev, &r->counter_node);
		for (i = 0; i < ARRAY_SIZE(r->owner); i++)
			if (drm_mm_node_allocated(&r->owner[i].system_node))
				m9x_address_unmap_system(mdev, &r->owner[i].system_node);
#ifdef M9_FAA_GRAPH_INIT_CONTROL
		for (i = 0; i < ARRAY_SIZE(r->draw_owner); i++)
			if (drm_mm_node_allocated(&r->draw_owner[i].system_node))
				m9x_address_unmap_system(mdev,
						 &r->draw_owner[i].system_node);
#endif
		if (mdev->transport.faa == r)
			mdev->transport.faa = NULL;
		mutex_unlock(&mdev->transport.lock);
		m9_faa_release(mdev, r);
	}
	return ret;
}

int m9x_faa_prepare_diag(struct m9x_device *mdev)
{
	return m9_faa_prepare(mdev, NULL, 0);
}

int m9x_faa_prepare_graph(struct m9x_device *mdev,
			  struct m9_faa_graph_binding *binding, u32 generation)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	/* The producer takes the binding only after complete preparation.
	 * A failed call leaves its reference with the ioctl. */
	if (!binding)
		return -EINVAL;
	return m9_faa_prepare(mdev, binding, generation);
#else
	return -EOPNOTSUPP;
#endif
}

/* Actual normal 656c0 records preserve descriptor tag1 in DWORD9, 14 and
 * 19 (65836/6584b/65892). Ordinary Linux mode3 records use tag0 instead.
 * Convert only those three static pointers after prior jobs retire; leave
 * work shadows and every other source byte intact. Verify all originals
 * before the first write, and retain the existing identity-leaf admission.
 */
static int m9_faa_prepare_virtual_restore(struct m9x_device *mdev)
{
	struct iosys_map map;
	static const u32 word[] = { 9, 14, 19 };
	u32 base, i, j;
	int ret;

	lockdep_assert_held(&mdev->transport.lock);
	ret = m9x_address_restore_region(mdev, &map, &base);
	if (ret)
		return ret;
	if (base != mdev->transport.restore_base ||
	    !IS_ALIGNED(base, PAGE_SIZE) ||
	    PARHELIA_M9_RESTORE_BYTES < 0x700 + 0x3ff * 0x600 + 0x50 ||
	    base > U32_MAX - PARHELIA_M9_RESTORE_BYTES)
		return -ERANGE;
	for (i = 0; i < 0x400; i++) {
		u32 work = base + 0x200 + i * 0x600;
		u32 expected[] = { work, base, work + 0x440 };

		for (j = 0; j < ARRAY_SIZE(word); j++) {
			u32 offset = 0x700 + i * 0x600 + word[j] * sizeof(u32);
			u32 value = iosys_map_rd(&map, offset, u32);

			if (value != expected[j]) {
				dev_err(&mdev->pdev->dev,
					"M9 FAA virtual restore refused: record=%u word=%u observed=%#x expected=%#x\n",
					i, word[j], value, expected[j]);
				return -EINVAL;
			}
		}
	}
	for (i = 0; i < 0x400; i++) {
		u32 work = base + 0x200 + i * 0x600;
		u32 expected[] = { work, base, work + 0x440 };

		for (j = 0; j < ARRAY_SIZE(word); j++)
			iosys_map_wr(&map, 0x700 + i * 0x600 + word[j] * sizeof(u32),
				     u32, expected[j] | 1u);
	}
	wmb();
	for (i = 0; i < 0x400; i++) {
		u32 work = base + 0x200 + i * 0x600;
		u32 expected[] = { work, base, work + 0x440 };

		for (j = 0; j < ARRAY_SIZE(word); j++)
			if (iosys_map_rd(&map,
				0x700 + i * 0x600 + word[j] * sizeof(u32), u32) !=
			    (expected[j] | 1u))
				return -EIO;
	}
	dev_info(&mdev->pdev->dev,
		 "M9 FAA virtual restore: base=%#x 1024 records, DWORD9/14/19 tag1 verified\n", base);
	return 0;
}

static u32 m9_faa_encode(u32 pc, u32 address, u32 tag)
{
	switch (pc) {
	case 0x24886: return (address & 0xfffff000) | tag | (!address ? 4 : 0);
	case 0x248ab: return ((address & 0xfff) >> 3) & 0xfffff;
	case 0x248c8: return (address & ~3u) | tag;
	case 0x248dc: return (address & 0xffffff84) | tag | 4;
	case 0x24909: return (address | tag) & ~3u;
	case 0x24919: return (address & 0xfffffffd) | 1;
	case 0x24930:
	case 0x256f1: return address | tag;
	default: return 0;
	}
}

/* Actual b5e70 clear uses resource+198 and ceil(resource+1bc/4096).
 * These are 151 logical companion pages, within its 152-page allocation.
 * The retained device+1c bit0 is zero: b6028/b6172 select no tail padding.
 * Page masks must follow the new allocation alignment, not captured masks.
 */
#ifndef M9_FAA_CLEAR_OWNER
#define M9_FAA_CLEAR_OWNER 4u
#define M9_FAA_CLEAR_BYTES 0x97000u
#define M9_FAA_CLEAR_START 0x11d8u
#define M9_FAA_CLEAR_END 0x1224u
#endif
#define M9_FAA_CLEAR_MAX_WORDS 20u

#if !defined(M9_FAA_GRAPH_INIT_CONTROL) && (!defined(M9_FAA_HAS_COMPANION_CLEAR) || M9_FAA_HAS_COMPANION_CLEAR)
static int m9_faa_clear_page_words(u32 base, u32 bytes, u32 *words,
				  u32 capacity, u32 *count)
{
	u32 page, remaining, leading, value, n = 0;

	if (!bytes || !IS_ALIGNED(base, PAGE_SIZE) ||
	    !IS_ALIGNED(bytes, PAGE_SIZE) || (u64)base + bytes > (u64)U32_MAX + 1)
		return -ERANGE;
	page = base >> PAGE_SHIFT;
	remaining = bytes >> PAGE_SHIFT;
	leading = min_t(u32, remaining, (-page) & 7);
	if (!!leading + DIV_ROUND_UP(remaining - leading, 8) > capacity)
		return -ENOSPC;
	value = (((page & 0xffff8u) | 0xd00000u) << 5);
	if (leading) {
		u32 mask = remaining == leading ?
			(0xffu >> (8 - leading)) << (page & 7) :
			0xffu << (8 - leading);

		words[n++] = value | (mask & 0xff);
		value = (value & ~0x1ffff00u) | ((value + 0x100) & 0x1ffff00u);
		remaining -= leading;
	}
	while (remaining >= 8) {
		words[n++] = value | 0xff;
		value = (value & ~0x1ffff00u) | ((value + 0x100) & 0x1ffff00u);
		remaining -= 8;
	}
	if (remaining)
		words[n++] = value | (0xffu >> (8 - remaining));
	*count = n;
	return 0;
}
#endif

/* Patch ordinary addresses/trackers at their captured offsets first, then
 * move the already patched suffix once. No record outside this inline clear
 * is regenerated or reinterpreted. The command buffer has an owned capacity
 * independent of its exact known/submitted prefix length.
 */
#ifdef M9_FAA_MULTI_CLEAR
static int m9_faa_bind_borrowed_color(struct m9x_faa_replay *r, u32 color,
				    struct drm_gem_vram_object *color_bo)
{
	struct m9_faa_owner *owner = &r->owner[M9_FAA_COLOR_OWNER];
	u64 base, bytes;
	s64 offset;

	/* The caller holds this BO's pin and reservation throughout submit;
	 * its existing fence/quarantine path retains it through retirement. */
	if (!color_bo || owner->vram || owner->cpu || owner->bytes ||
	    !m9_faa_owner_specs[M9_FAA_COLOR_OWNER].local)
		return -EINVAL;
	offset = drm_gem_vram_offset(color_bo);
	bytes = color_bo->bo.base.size;
	if (offset < 0 || check_add_overflow(M9_VRAM_GPU_BASE, (u64)offset, &base) ||
	    base != color || !IS_ALIGNED(base, PAGE_SIZE) ||
	    bytes < m9_faa_owner_specs[M9_FAA_COLOR_OWNER].bytes ||
	    bytes > (u64)U32_MAX + 1 - base)
		return -ERANGE;
	owner->base = color;
	r->borrowed_color_bytes = bytes;
	return 0;
}
#endif

/* The public one-target raster producer leaves inactive AB/AC halfwords
 * as stack residue. Preserve its active target mask, but derive neutral
 * masks for the three absent targets instead of replaying that residue.
 * This is separate from address relocation and never alters the capture. */
static int m9_faa_normalize_raster_masks(struct m9x_faa_replay *r)
{
#if defined(M9_FAA_RASTER_MASK_BLOCKS) || defined(M9_FAA_RASTER_MASK_OWNER)
	static const struct {
		u32 owner, ab, route;
	} blocks[] =
#ifdef M9_FAA_RASTER_MASK_BLOCKS
		M9_FAA_RASTER_MASK_BLOCKS;
#else
		{ { M9_FAA_RASTER_MASK_OWNER, M9_FAA_RASTER_MASK_AB_OFFSET,
		    M9_FAA_RASTER_MASK_89_OFFSET } };
#endif
	u32 i;

	/* Refuse a changed later target before rewriting any earlier block. */
	for (i = 0; i < ARRAY_SIZE(blocks); i++) {
		struct m9_faa_owner *owner;
		u32 ab = blocks[i].ab, route = blocks[i].route;
		u8 *data;

		if (blocks[i].owner >= ARRAY_SIZE(r->owner))
			return -EINVAL;
		owner = &r->owner[blocks[i].owner];
		data = owner->cpu;
		if (!data || owner->vram || ab < 8 || route < 12 ||
		    !IS_ALIGNED(ab | route, sizeof(u32)) ||
		    (u64)ab + 8 > owner->bytes || (u64)route + 4 > owner->bytes ||
		    get_unaligned_le32(data + ab - 8) != 3 ||
		    get_unaligned_le32(data + ab - 4) != 0x45aeacab ||
		    get_unaligned_le32(data + route - 12) != 4 ||
		    get_unaligned_le32(data + route - 8) != 0xc18a8988 ||
		    get_unaligned_le32(data + route) != 0xfe207ff8 ||
		    (get_unaligned_le32(data + ab) & 0xffff) != 0xffff)
			return -EINVAL;
	}
	for (i = 0; i < ARRAY_SIZE(blocks); i++) {
		u8 *data = r->owner[blocks[i].owner].cpu;
		u32 ab = blocks[i].ab;

		put_unaligned_le32(get_unaligned_le32(data + ab) | 0xffff0000,
				   data + ab);
		put_unaligned_le32(0xffffffff, data + ab + 4);
	}
#else
	(void)r;
#endif
	return 0;
}

static int m9_faa_rebase_page_clear(struct m9x_faa_replay *r)
{
	struct m9_faa_owner *primary = &r->owner[M9_FAA_PRIMARY_OWNER];
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	u32 *scratch;
	int ret;

	if (!r->graph_binding || !primary->cpu || primary->vram ||
	    r->owner[4].bytes < r->graph_layout.owner[M9_FAA_SURFACE_COMPANION].clear)
		return -EINVAL;
	scratch = kmalloc_array(M9_FAA_INIT_DIMENSION_CLEAR_WORDS, sizeof(*scratch), GFP_KERNEL);
	if (!scratch)
		return -ENOMEM;
	ret = m9x_faa_graph_init_dimensions_encode(primary->cpu, primary->bytes,
		&r->graph_layout, m9_faa_owner_specs[4].captured,
		r->owner[4].base, scratch, M9_FAA_INIT_DIMENSION_CLEAR_WORDS,
		&r->primary_bytes);
	kfree(scratch);
	return ret;
#elif defined(M9_FAA_HAS_COMPANION_CLEAR) && !M9_FAA_HAS_COMPANION_CLEAR
	/* This complete source profile contains no inline companion clear.
	 * Preserve its exact primary and independent secondary finish. */
	if (!primary->cpu || primary->vram ||
	    primary->bytes < M9_FAA_PRIMARY_BYTES ||
	    M9_FAA_CLEAR_OWNER != U32_MAX || M9_FAA_CLEAR_BYTES ||
	    M9_FAA_CLEAR_START || M9_FAA_CLEAR_END)
		return -EINVAL;
	r->primary_bytes = M9_FAA_PRIMARY_BYTES;
	return 0;
#elif defined(M9_FAA_MULTI_CLEAR)
	u32 replacement[ARRAY_SIZE(m9_faa_clear_segments)][M9_FAA_CLEAR_MAX_WORDS];
	u32 counts[ARRAY_SIZE(m9_faa_clear_segments)];
	u32 original[M9_FAA_CLEAR_MAX_WORDS];
	u64 bytes = M9_FAA_PRIMARY_BYTES;
	u8 *data = primary->cpu;
	u32 i, j, original_count;
	int ret;

	/* This profile contains two independently observed b5e70 calls. Check
	 * every owner, packet and final extent before modifying any bytes. */
	if (ARRAY_SIZE(m9_faa_clear_segments) != 2 || !data || primary->vram ||
	    primary->bytes < M9_FAA_PRIMARY_BYTES)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(m9_faa_clear_segments); i++) {
		const struct m9_faa_clear_segment *s = &m9_faa_clear_segments[i];
		struct m9_faa_owner *owner;

		if (s->owner >= ARRAY_SIZE(r->owner) || !s->bytes ||
		    !IS_ALIGNED(s->start, 4) || !IS_ALIGNED(s->end, 4) ||
		    s->start < 28 || s->start >= s->end ||
		    (u64)s->end + 8 > M9_FAA_PRIMARY_BYTES ||
		    (i && s->start - 28 < m9_faa_clear_segments[i - 1].end + 8))
			return -EINVAL;
		owner = &r->owner[s->owner];
		if (s->owner == M9_FAA_COLOR_OWNER) {
			if (owner->vram || owner->cpu || owner->bytes ||
			    r->borrowed_color_bytes < s->bytes)
				return -EINVAL;
		} else if (!owner->vram || owner->bytes < s->bytes) {
			return -EINVAL;
		}
		if (m9_faa_owner_specs[s->owner].bytes < s->bytes ||
		    get_unaligned_le32(data + s->start - 28) != 0x4545ace6 ||
		    get_unaligned_le32(data + s->start - 24) != 2 ||
		    get_unaligned_le32(data + s->start - 20) != 0 ||
		    get_unaligned_le32(data + s->start - 16) != 0x45e62b2a ||
		    get_unaligned_le32(data + s->start - 12) != 0x454545e6 ||
		    get_unaligned_le32(data + s->start - 8) != 0xffff ||
		    get_unaligned_le32(data + s->end) != 0x4545e3e6 ||
		    get_unaligned_le32(data + s->end + 4) != 1)
			return -EINVAL;
		ret = m9_faa_clear_page_words(m9_faa_owner_specs[s->owner].captured,
			s->bytes, original, ARRAY_SIZE(original), &original_count);
		if (ret)
			return ret;
		if (original_count * 4 != s->end - s->start ||
		    get_unaligned_le32(data + s->start - 4) !=
			(0x20000000 | (original_count - 1)))
			return -EINVAL;
		for (j = 0; j < original_count; j++)
			if (get_unaligned_le32(data + s->start + j * 4) != original[j])
				return -EINVAL;
		ret = m9_faa_clear_page_words(owner->base, s->bytes,
			replacement[i], ARRAY_SIZE(replacement[i]), &counts[i]);
		if (ret)
			return ret;
	}
	/* Intermediate suffix growth must fit too, even when the earlier
	 * segment later shrinks and the final length would fit the allocation. */
	for (i = ARRAY_SIZE(m9_faa_clear_segments); i-- > 0;) {
		const struct m9_faa_clear_segment *s = &m9_faa_clear_segments[i];

		bytes = bytes - (s->end - s->start) + counts[i] * 4;
		if (bytes > primary->bytes)
			return -ENOSPC;
	}
	/* Address and timeline fixups have already been applied at their
	 * original offsets. Rebuild from the last segment backwards: shifting
	 * an earlier suffix then preserves the already rebuilt later clear. */
	bytes = M9_FAA_PRIMARY_BYTES;
	for (i = ARRAY_SIZE(m9_faa_clear_segments); i-- > 0;) {
		const struct m9_faa_clear_segment *s = &m9_faa_clear_segments[i];

		memmove(data + s->start + counts[i] * 4, data + s->end,
			bytes - s->end);
		put_unaligned_le32(0x20000000 | (counts[i] - 1),
			data + s->start - 4);
		for (j = 0; j < counts[i]; j++)
			put_unaligned_le32(replacement[i][j], data + s->start + j * 4);
		bytes = bytes - (s->end - s->start) + counts[i] * 4;
	}
	r->primary_bytes = bytes;
	return 0;
#else
	struct m9_faa_owner *companion = &r->owner[M9_FAA_CLEAR_OWNER];
	u32 original[M9_FAA_CLEAR_MAX_WORDS], replacement[M9_FAA_CLEAR_MAX_WORDS];
	u32 original_count, count, bytes, i;
	u8 *data = primary->cpu;
	int ret;

	if (!data || primary->vram || primary->bytes < M9_FAA_PRIMARY_BYTES ||
	    companion->bytes < M9_FAA_CLEAR_BYTES ||
	    (!companion->vram && !r->graph_binding) ||
	    M9_FAA_CLEAR_END > M9_FAA_PRIMARY_BYTES ||
	    get_unaligned_le32(data + 0x11bc) != 0x4545ace6 ||
	    get_unaligned_le32(data + 0x11c0) != 2 ||
	    get_unaligned_le32(data + 0x11c4) != 0 ||
	    get_unaligned_le32(data + 0x11c8) != 0x45e62b2a ||
	    get_unaligned_le32(data + 0x11cc) != 0x454545e6 ||
	    get_unaligned_le32(data + 0x11d0) != 0xffff ||
	    get_unaligned_le32(data + M9_FAA_CLEAR_END) != 0x4545e3e6 ||
	    get_unaligned_le32(data + M9_FAA_CLEAR_END + 4) != 1)
		return -EINVAL;
	ret = m9_faa_clear_page_words(m9_faa_owner_specs[M9_FAA_CLEAR_OWNER].captured,
		M9_FAA_CLEAR_BYTES, original, ARRAY_SIZE(original), &original_count);
	if (ret)
		return ret;
	if (original_count * sizeof(u32) != M9_FAA_CLEAR_END - M9_FAA_CLEAR_START ||
	    get_unaligned_le32(data + M9_FAA_CLEAR_START - 4) !=
		(0x20000000 | (original_count - 1)))
		return -EINVAL;
	for (i = 0; i < original_count; i++)
		if (get_unaligned_le32(data + M9_FAA_CLEAR_START + i * 4) != original[i])
			return -EINVAL;
	ret = m9_faa_clear_page_words(companion->base, M9_FAA_CLEAR_BYTES,
		replacement, ARRAY_SIZE(replacement), &count);
	if (ret)
		return ret;
	bytes = M9_FAA_PRIMARY_BYTES - original_count * 4 + count * 4;
	if (bytes > primary->bytes)
		return -ENOSPC;
	memmove(data + M9_FAA_CLEAR_START + count * 4, data + M9_FAA_CLEAR_END,
		M9_FAA_PRIMARY_BYTES - M9_FAA_CLEAR_END);
	put_unaligned_le32(0x20000000 | (count - 1), data + M9_FAA_CLEAR_START - 4);
	for (i = 0; i < count; i++)
		put_unaligned_le32(replacement[i], data + M9_FAA_CLEAR_START + i * 4);
	r->primary_bytes = bytes;
	return 0;
#endif
}

#ifdef M9_FAA_PROGRAM_UPLOAD_OWNER
/* The prepared source uploads its program only after the original access
 * wait. Its CPU alias is allocated independently of its GPU address. Here
 * the retained local range also owns its checked CPU-root identity alias;
 * no captured aperture index or page-table destination is reused. */
static int m9_faa_upload_program(struct m9x_device *mdev,
				 struct m9x_faa_replay *r, u32 program)
{
	const struct firmware *fw = r->program_inputs;
	struct m9_faa_owner *o;
	u8 observed[256];
	u32 i, offset, count, known = 0;
	int ret;

	if (program != M9_FAA_PROGRAM_UPLOAD_OWNER ||
	    program >= ARRAY_SIZE(r->owner) || !fw)
		return -EINVAL;
	o = &r->owner[program];
	if (!o->vram || o->cpu || !m9_faa_owner_specs[program].local ||
	    m9_faa_owner_specs[program].system)
		return -EINVAL;
	/* Preflight all spans before publishing the CPU alias. Unknown tails
	 * remain unknown; neither mapping nor upload supplies invented bytes. */
	for (i = 0; i < ARRAY_SIZE(m9_faa_spans); i++) {
		const struct m9_faa_span *s = &m9_faa_spans[i];

		if (s->owner != program)
			continue;
		if (!s->bytes || s->offset > m9_faa_owner_specs[program].bytes ||
		    s->bytes > m9_faa_owner_specs[program].bytes - s->offset ||
		    s->offset > o->bytes || s->bytes > o->bytes - s->offset ||
		    s->blob_offset > fw->size || s->bytes > fw->size - s->blob_offset ||
		    check_add_overflow(known, s->bytes, &known))
			return -EINVAL;
	}
	if (!known)
		return -EINVAL;
	/* The caller already checked local identity, so local_prepare inside
	 * this helper makes no GPU mapping writes; only CPU leaves publish. */
	ret = m9x_address_cpu_local_prepare(mdev, o->base, o->bytes);
	if (ret)
		return ret;
	/* The actual program caller uses a raw linear CPU alias, without a
	 * formatted selector1 view. Our pinned TTM map covers BAR offset base;
	 * the CPU-root identity mirror above joins that alias to this owner. */
	for (i = 0; i < ARRAY_SIZE(m9_faa_spans); i++) {
		const struct m9_faa_span *s = &m9_faa_spans[i];

		if (s->owner == program)
			iosys_map_memcpy_to(&o->map, s->offset,
					   fw->data + s->blob_offset, s->bytes);
	}
	wmb();
	/* Actual 2c840/2c878 releases its cached raw CPU view, then writes0.
	 * The retained native TTM view needs no transient formatted slot. */
	m9x_write(mdev, M9_REG_FRONTEND_STATE, 0);
	m9x_read(mdev, M9_REG_FRONTEND_STATE);
	/* Verify only source-known bytes through the retained raw BAR view;
	 * the CPU-root helper establishes its separate live-owner mapping. */
	for (i = 0; i < ARRAY_SIZE(m9_faa_spans); i++) {
		const struct m9_faa_span *s = &m9_faa_spans[i];

		if (s->owner != program)
			continue;
		for (offset = 0; offset < s->bytes; offset += count) {
			count = min_t(u32, sizeof(observed), s->bytes - offset);
			iosys_map_memcpy_from(observed, &o->map, s->offset + offset, count);
			if (memcmp(observed, fw->data + s->blob_offset + offset, count))
				return -EIO;
		}
	}
	dev_info(&mdev->pdev->dev,
		 "M9 FAA program CPU upload verified: owner=%u gpu=%#x bytes=%u known=%u after c0\n",
		 program, o->base, o->bytes, known);
	return 0;
}
#endif

#ifdef M9_FAA_PROGRAM_ACCESS_CONTROL
/* Actual 2cb40 waits for the program resource before its unconditional
 * 2cba8 frontend kick. Prepared profiles then perform the proven raw upload;
 * older profiles retain their independently tested kick-only control.
 * Paired entry-offset encoders identify the retained C2 owner. Later draws
 * may rebind either entry of the same program; validate every pair without
 * limiting the whole stream to two bindings. No captured resource address
 * or captured CPU alias is installed by this bounded control. */
static int m9_faa_program_access(struct m9x_device *mdev,
				 struct m9x_faa_replay *r, u64 point)
{
	u32 i, program = U32_MAX, entries = 0, offsets = 0;
#ifdef M9_FAA_PROGRAM_ENTRY_MASK
	const u32 expected_offsets = M9_FAA_PROGRAM_ENTRY_MASK;
#else
	const u32 expected_offsets = 3;
#endif
	const struct m9_faa_owner_spec *spec;
	struct m9_faa_owner *owner;
	int ret;

	for (i = 0; i < ARRAY_SIZE(m9_faa_fixups); i++) {
		const struct m9_faa_fixup *f = &m9_faa_fixups[i];

		if (f->encoder != 0x248ab)
			continue;
		if (!i || f->offset < 4 ||
		    m9_faa_fixups[i - 1].encoder != 0x24886 ||
		    m9_faa_fixups[i - 1].dst != f->dst ||
		    m9_faa_fixups[i - 1].offset != f->offset - 4 ||
		    m9_faa_fixups[i - 1].src != f->src ||
		    m9_faa_fixups[i - 1].addend != f->addend ||
		    m9_faa_fixups[i - 1].tag != f->tag ||
		    (f->addend != 0 && f->addend != 32) ||
		    f->src >= ARRAY_SIZE(r->owner) ||
		    (program != U32_MAX && program != f->src))
			return -EINVAL;
		program = f->src;
		offsets |= f->addend ? 2 : 1;
		entries++;
	}
	/* A source prefix may contain only preprocessing entry32. Require
	 * its explicit generated inventory; older complete profiles retain
	 * both entries. Pair and unique-owner validation remain identical. */
	if (!entries || !expected_offsets || (expected_offsets & ~3u) ||
	    offsets != expected_offsets)
		return -EINVAL;
	/* Kind19 resident-database bindings use the same page-base encoder
	 * without a C3 half. First identify the unique program from actual
	 * entry-offset pairs, then reject orphan bases on that proven owner.
	 * A valid later rebind must not conceal a missing earlier C3 half. */
	for (i = 0; i < ARRAY_SIZE(m9_faa_fixups); i++) {
		const struct m9_faa_fixup *f = &m9_faa_fixups[i];

		if (f->encoder == 0x24886 && f->src == program &&
		    (i + 1 >= ARRAY_SIZE(m9_faa_fixups) ||
		     m9_faa_fixups[i + 1].encoder != 0x248ab))
			return -EINVAL;
	}
	spec = &m9_faa_owner_specs[program];
	owner = &r->owner[program];
	if (!spec->local || spec->system || spec->bytes != 0x1080 ||
	    !owner->vram || owner->cpu || owner->bytes != PAGE_ALIGN(spec->bytes))
		return -EINVAL;
	ret = m9x_address_check_local_identity(mdev, owner->base, owner->bytes);
	if (ret)
		return ret;
	wmb();
	m9x_write(mdev, M9_REG_FRONTEND_KICK, 1);
	dev_info(&mdev->pdev->dev,
		 "M9 FAA program access kick: point=%#llx owner=%u gpu=%#x bytes=%u\n",
		 point, program, owner->base, owner->bytes);
#ifdef M9_FAA_PROGRAM_UPLOAD_OWNER
	return m9_faa_upload_program(mdev, r, program);
#endif
	return 0;
}

static int m9_faa_publish_output_phase(struct m9x_device *mdev, u32 phase,
                                      u64 point, void *data)
{
	struct m9x_faa_replay *r = data;
#ifdef M9_FAA_PRECLEAR_OUTPUT_WRITE_CONTROL
	struct m9_faa_owner *output;
	int ret;
#endif

	if (phase == 0 && m9_faa_program_access_kick)
		return m9_faa_program_access(mdev, r, point);
#ifdef M9_FAA_PRECLEAR_OUTPUT_WRITE_CONTROL
	if (phase != 1 || !m9_faa_defer_output_map)
		return 0;
	output = m9_faa_output_owner(r);
	if (!output || !output->cpu || output->vram ||
	    !m9_faa_owner_specs[M9_FAA_OUTPUT_OWNER].flags30 ||
	    m9_faa_owner_specs[M9_FAA_OUTPUT_OWNER].mapped_bytes != 0x50000)
		return -EINVAL;
	/* Called after the actual120 wait and before posting the160 suffix.
	 * Existing publisher derives every PTE and C24 word from this owner. */
	ret = m9x_address_publish_faa_output(mdev, &output->system_node, output->dma);

	if (!ret)
		dev_info(&mdev->pdev->dev,
			 "M9 FAA late output mapping: point=%#llx gpu=%#x pages=80\n",
			 point, output->base);
	return ret;
#else
	return 0;
#endif
}
#endif

#ifdef M9_FAA_STARTUP_LOCAL_OWNER
static int m9_faa_prepare_startup_local(struct m9x_device *mdev,
                                      struct m9x_faa_replay *r)
{
	int ret;

	/* Actual 2f899 preparation publishes d0 before queue creation. The
	 * source allocator's page number is not portable: retain an independent
	 * unknown-byte owner and derive both page tables from its live base.
	 * No GPU job has been posted; publish it before the consumed owners. */
	if (M9_FAA_STARTUP_LOCAL_OWNER >= ARRAY_SIZE(r->owner) ||
	    M9_FAA_STARTUP_LOCAL_OWNER == M9_FAA_COLOR_OWNER ||
	    !m9_faa_owner_specs[M9_FAA_STARTUP_LOCAL_OWNER].local ||
	    m9_faa_owner_specs[M9_FAA_STARTUP_LOCAL_OWNER].system ||
	    m9_faa_owner_specs[M9_FAA_STARTUP_LOCAL_OWNER].flags30 ||
	    m9_faa_owner_specs[M9_FAA_STARTUP_LOCAL_OWNER].mapped_bytes ||
	    m9_faa_owner_specs[M9_FAA_STARTUP_LOCAL_OWNER].bytes !=
			M9_FAA_STARTUP_LOCAL_BYTES ||
	    !IS_ALIGNED(M9_FAA_STARTUP_LOCAL_BYTES, PAGE_SIZE) ||
	    !M9_FAA_STARTUP_LOCAL_BYTES ||
	    !r->owner[M9_FAA_STARTUP_LOCAL_OWNER].vram ||
	    r->owner[M9_FAA_STARTUP_LOCAL_OWNER].cpu ||
	    r->owner[M9_FAA_STARTUP_LOCAL_OWNER].bytes != M9_FAA_STARTUP_LOCAL_BYTES)
		return -EINVAL;
	ret = m9x_address_local_prepare(mdev,
			r->owner[M9_FAA_STARTUP_LOCAL_OWNER].base,
			M9_FAA_STARTUP_LOCAL_BYTES);
	if (ret)
		return ret;
	dev_info(&mdev->pdev->dev,
		 "M9 FAA startup local: owner=%u gpu=%#x bytes=%u before primary submit\n",
		 M9_FAA_STARTUP_LOCAL_OWNER,
		 r->owner[M9_FAA_STARTUP_LOCAL_OWNER].base, M9_FAA_STARTUP_LOCAL_BYTES);
	return 0;
}
#endif

static int m9_faa_submit(struct m9x_device *mdev, u32 color,
			  struct drm_gem_vram_object *color_bo, u64 *point,
			  bool *color_active, bool *color_retained,
			  struct m9_faa_graph_binding *expected_binding)
{
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	struct m9x_fence *fence = NULL;
	u32 i, base, target;
	bool posted = false;
	int ret;

	*point = 0;
	*color_active = false;
	*color_retained = false;
	mutex_lock(&t->lock);
	r = t->faa;
	if ((!m9_faa_diag && !(r && r->graph_binding)) ||
	    !r || !r->prepared || r->attempted ||
	    r->retained_color || t->wedged ||
	    !IS_ALIGNED(color, PAGE_SIZE)) {
		ret = t->wedged ? -EIO : -EINVAL;
		goto unlock;
	}
	if (r->graph_binding && t->generation != r->graph_generation) {
		ret = -ESTALE;
		goto unlock;
	}
	if (r->graph_binding != expected_binding) {
		ret = -ESTALE;
		goto unlock;
	}
#ifdef M9_FAA_UNLOCK_PUBLICATION_CONTROL
	/* Split public callers cannot use the monolithic submit fallback: it
	 * would post later command bytes while publishing the first Lock fence. */
	if (!m9_faa_phased_submit) {
		ret = -EOPNOTSUPP;
		goto unlock;
	}
#endif
	/* No live job may observe this probe's counter destinations or state.
 * The earlier ioctl-side drain does not cover the intervening lock gap. */
	ret = m9x_transport_drain_locked(mdev);
	if (ret)
		goto unlock;
	ret = m9x_engine_ready_locked(mdev);
	if (ret)
		goto unlock;
	/* The ordinary transport owns 0x400, the common counters and the
 * default restore area. Do not copy captured MMIO physical addresses. */
	if (!t->restore_ready || !t->transport_armed || t->next_fence < 0x10 ||
	    t->next_fence > U32_MAX - M9_FAA_TRACKER_LAST) {
		ret = -ERANGE;
		goto unlock;
	}
	if (M9_FAA_COUNTER_VIRTUAL &&
	    (!drm_mm_node_allocated(&r->counter_node) ||
	     r->counter_node.size != PAGE_SIZE || r->counter_node.start > U32_MAX)) {
		ret = -EINVAL;
		goto unlock;
	}
	base = t->next_fence - 0x10;
#ifdef M9_FAA_UNLOCK_PUBLICATION_CONTROL
	/* The later source endpoints belong to separate public callers. A fence
	 * for either here would make the first Lock wait on unposted work. */
	if (M9_FAA_NORMAL_LOCK_SPLIT_TARGET != 0x180 ||
#ifdef M9_FAA_RELOCK_PUBLICATION_CONTROL
	    M9_FAA_NORMAL_UNLOCK_SPLIT_TARGET != 0x1a0 ||
	    M9_FAA_TRACKER_LAST != 0x1c0 ||
	    M9_FAA_NORMAL_LOCK_SPLIT_BYTES >= M9_FAA_NORMAL_UNLOCK_SPLIT_BYTES ||
	    M9_FAA_NORMAL_UNLOCK_SPLIT_BYTES >= M9_FAA_PRIMARY_BYTES ||
#else
	    M9_FAA_TRACKER_LAST != 0x1a0 ||
#endif
	    M9_FAA_NORMAL_LOCK_SPLIT_BYTES >= M9_FAA_PRIMARY_BYTES) {
		ret = -EINVAL;
		goto unlock;
	}
	target = base + M9_FAA_NORMAL_LOCK_SPLIT_TARGET - 0x10;
#else
	target = base + M9_FAA_TRACKER_LAST - 0x10;
#endif
	/* Reuse an already observed retirement, or the unchanged CPU seed10
	 * on explicitly admitted fresh source initialization. No CPU advance. */
	if ((READ_ONCE(((u32 *)t->fence_cpu)[0]) & ~15u) != base) {
		ret = -EAGAIN;
		goto unlock;
	}
	ret = m9x_address_check_local_identity(mdev, t->restore_base,
					 PARHELIA_M9_RESTORE_BYTES);
	if (ret)
		goto unlock;
#if !defined(M9_FAA_HAS_SAMPLED_COLOR) || M9_FAA_HAS_SAMPLED_COLOR
	if (r->graph_binding) {
		for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
			u32 address, source;

			if (!m9x_faa_graph_binding_address(r->graph_binding, i,
							  &address, &source) ||
			    source >= ARRAY_SIZE(r->owner) ||
			    r->owner[source].base != address ||
			    r->owner[source].bytes !=
				r->graph_layout.owner[i].capacity) {
				ret = -ESTALE;
				goto unlock;
			}
		}
	} else {
#ifdef M9_FAA_MULTI_CLEAR
	ret = m9_faa_bind_borrowed_color(r, color, color_bo);
	if (ret)
		goto unlock;
#else
	r->owner[M9_FAA_COLOR_OWNER].base = color;
#endif
	}
#else
	if (M9_FAA_COLOR_OWNER != U32_MAX) {
		ret = -EINVAL;
		goto unlock;
	}
#endif
#ifdef M9_FAA_STARTUP_LOCAL_OWNER
	ret = m9_faa_prepare_startup_local(mdev, r);
	if (ret)
		goto unlock;
#endif
	for (i = 0; i < ARRAY_SIZE(r->owner); i++) {
		if (!m9_faa_owner_specs[i].local)
			continue;
#ifdef M9_FAA_STARTUP_LOCAL_OWNER
		if (i == M9_FAA_STARTUP_LOCAL_OWNER)
			continue;
#endif
		ret = m9x_address_local_prepare(mdev, r->owner[i].base,
				r->graph_binding ? r->owner[i].bytes :
				PAGE_ALIGN(m9_faa_owner_specs[i].bytes));
		if (ret) {
			dev_err(&mdev->pdev->dev, "M9 FAA identity leaf refused: owner=%u base=%#x ret=%d\n",
				i, r->owner[i].base, ret);
			goto unlock;
		}
	}
	/* Once patching begins this immutable capture is consumed, even if a
 * later admission fails. A retry must not reinterpret already rebased
 * words as their original captured addresses. */
	r->attempted = true;
	ret = m9_faa_prepare_virtual_restore(mdev);
	if (ret)
		goto unlock;
	for (i = 0; i < ARRAY_SIZE(m9_faa_fixups); i++) {
		const struct m9_faa_fixup *f = &m9_faa_fixups[i];
		struct m9_faa_owner *dst;
		u32 address, value;

		if (f->dst >= ARRAY_SIZE(r->owner) || f->src >= ARRAY_SIZE(r->owner) ||
		    (M9_FAA_COUNTER_VIRTUAL && f->src == M9_FAA_PRIMARY_OWNER)) {
			ret = -EINVAL;
			goto unlock;
		}
		dst = &r->owner[f->dst];
		if (dst->vram || !dst->cpu || !IS_ALIGNED(f->offset, 4) ||
		    f->offset > m9_faa_owner_specs[f->dst].bytes - 4 ||
		    f->addend > (m9_faa_owner_specs[f->src].system ?
			m9_faa_owner_specs[f->src].mapped_bytes :
			m9_faa_owner_specs[f->src].bytes) ||
		    check_add_overflow(r->owner[f->src].base, f->addend, &address) ||
		    get_unaligned_le32((u8 *)dst->cpu + f->offset) != f->captured) {
			ret = -EINVAL;
			goto unlock;
		}
		value = m9_faa_encode(f->encoder, address, f->tag);
		if (m9_faa_encode(f->encoder,
			m9_faa_owner_specs[f->src].captured + f->addend,
			f->tag) != f->captured) {
			ret = -EINVAL;
			goto unlock;
		}
		put_unaligned_le32(value, (u8 *)dst->cpu + f->offset);
	}
	for (i = 0; i < ARRAY_SIZE(m9_faa_trackers); i++) {
		const struct m9_faa_tracker *f = &m9_faa_trackers[i];
		struct m9_faa_owner *dst;
		u32 value = base + (f->captured & ~15u) - 0x10;

		if (f->owner >= ARRAY_SIZE(r->owner)) {
			ret = -EINVAL;
			goto unlock;
		}
		dst = &r->owner[f->owner];
		if (dst->vram || !dst->cpu || !IS_ALIGNED(f->offset, 4) ||
		    f->offset > m9_faa_owner_specs[f->owner].bytes - 4 ||
		    get_unaligned_le32((u8 *)dst->cpu + f->offset) != f->captured) {
			ret = -EINVAL;
			goto unlock;
		}
		put_unaligned_le32(value | (f->captured & 15), (u8 *)dst->cpu + f->offset);
	}
	ret = m9_faa_normalize_raster_masks(r);
	if (ret)
		goto unlock;
	ret = m9_faa_rebase_page_clear(r);
	if (ret)
		goto unlock;
#if defined(M9_FAA_GRAPH_INIT_CONTROL)
	dev_info(&mdev->pdev->dev,
		 "M9 FAA graph INIT geometry: width=%u height=%u companion_bytes=%#x primary_bytes=%u\n",
		 r->graph_layout.width, r->graph_layout.height,
		 r->graph_layout.owner[M9_FAA_SURFACE_COMPANION].clear, r->primary_bytes);
#elif defined(M9_FAA_MULTI_CLEAR)
	for (i = 0; i < ARRAY_SIZE(m9_faa_clear_segments); i++)
		dev_info(&mdev->pdev->dev,
			 "M9 FAA page clear: segment=%u owner=%u base=%#x bytes=%#x primary_bytes=%u\n",
			 i, m9_faa_clear_segments[i].owner,
			 r->owner[m9_faa_clear_segments[i].owner].base,
			 m9_faa_clear_segments[i].bytes, r->primary_bytes);
#elif !defined(M9_FAA_HAS_COMPANION_CLEAR) || M9_FAA_HAS_COMPANION_CLEAR
	dev_info(&mdev->pdev->dev,
		 "M9 FAA companion clear: base=%#x bytes=%#x inline_words=%u primary_bytes=%u\n",
		 r->owner[M9_FAA_CLEAR_OWNER].base, M9_FAA_CLEAR_BYTES,
		 (r->primary_bytes - M9_FAA_PRIMARY_BYTES) / 4 + 19, r->primary_bytes);
#endif
	/* Only a generated source-closed ordinary-output template owns this
	 * object. A successful older sampled resolve is not an output read. */
	if (m9_faa_output_owner(r)) {
		struct m9_faa_owner *output = m9_faa_output_owner(r);

		if (!m9_faa_owner_specs[M9_FAA_OUTPUT_OWNER].system ||
		    !m9_faa_owner_specs[M9_FAA_OUTPUT_OWNER].flags30 ||
		    output->bytes != 0x50000 || !output->cpu ||
		    !drm_mm_node_allocated(&output->system_node)) {
			ret = -EINVAL;
			goto unlock;
		}
		drm_gem_object_get(&color_bo->bo.base);
		r->output_color = &color_bo->bo.base;
	}
	fence = m9x_fence_create_target_locked(mdev, target);
	if (!fence) {
		ret = -ENOMEM;
		goto unlock;
	}
	if (r->graph_binding) {
		for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++)
			dma_resv_add_fence(
				m9x_faa_graph_binding_owner(r->graph_binding, i)->resv,
				&fence->base, DMA_RESV_USAGE_WRITE);
	} else {
		dma_resv_add_fence(color_bo->bo.base.resv, &fence->base,
					   DMA_RESV_USAGE_WRITE);
	}
	/* Actual 23a3b/23aaf normal activation contracts. The ordinary context
 * retired, or fresh source admission retained the initial CPU seed;
 * the two additional tracker words belong to this same
 * retained coherent common buffer, not captured host physical memory.
 * Only the actual known128-byte initializer is reproduced. The low seed
 * retains observed retirement, or the fresh CPU origin10; it never advances
 * a completion counter or publishes GPU success. */
	memset(t->fence_cpu, 0, 128);
	WRITE_ONCE(((u32 *)t->fence_cpu)[0], base);
	WRITE_ONCE(((u32 *)t->fence_cpu)[0x20 / 4], base);
	WRITE_ONCE(((u32 *)t->fence_cpu)[0x40 / 4], base);
	WRITE_ONCE(((u32 *)t->fence_cpu)[0x60 / 4], base);
	if (t->faa_source_context)
		dev_info(&mdev->pdev->dev,
			 "M9 FAA source context: CPU seed=%#x target=%#x no prior GPU submission\n",
			 base, target);
	/* Publish coherent command/counter words and any prior WC writes. */
	wmb();
	m9x_write(mdev, M9_REG_FENCE0_ADDRESS, lower_32_bits(t->fence_bus + 0x20) | 2);
	/* Actual23a47..23a94: the source110/PTEa proxy publishes the same
	 * transport completion page through class1. Other selectors retain
	 * their separately proven physical-tag2 common-buffer fields. */
	if (M9_FAA_COUNTER_VIRTUAL)
		m9x_write(mdev, M9_REG_FENCE1_ADDRESS, (u32)r->counter_node.start | 1);
	m9x_write(mdev, M9_REG_FENCE2_ADDRESS, lower_32_bits(t->fence_bus + 0x40) | 2);
	/* Genuine normal activation selects the virtual restore descriptor.
 * Its native identity leaves were checked separately from FAA pool6. */
	m9x_write(mdev, M9_REG_RESTORE_TAGGED, ((t->restore_base + 0x200) & ~31u) | 1);
	m9x_read(mdev, M9_REG_FENCE2_ADDRESS);
	/* Baseline follows real initialization and every pointer/counter patch,
	 * but precedes this immutable stream's first GPU publication. */
	m9_faa_observe(mdev, r, true);
	m9_faa_observe_borrowed_color(mdev, r, color_bo, true);
	/* Source23a09/2461e consume this live class1/B owner directly. Its
	 * final size includes the regenerated inline clear and shifted suffix.
	 * No transport clone substitutes a physical domain for this primary. */
	if (!m9_faa_owner_specs[M9_FAA_PRIMARY_OWNER].system ||
	    !m9_faa_owner_specs[M9_FAA_PRIMARY_OWNER].flags30 ||
	    r->owner[M9_FAA_PRIMARY_OWNER].vram ||
	    !r->owner[M9_FAA_PRIMARY_OWNER].cpu ||
	    r->owner[M9_FAA_PRIMARY_OWNER].base !=
			r->owner[M9_FAA_PRIMARY_OWNER].system_node.start ||
	    r->owner[M9_FAA_PRIMARY_OWNER].system_node.size !=
			m9_faa_owner_specs[M9_FAA_PRIMARY_OWNER].mapped_bytes) {
		ret = -EINVAL;
		goto unlock;
	}
	if (m9_faa_phased_submit) {
#ifdef M9_FAA_PROGRAM_ACCESS_CONTROL
		/* These source-exported endpoints are after both variable
		 * clear segments, so they all move by the same signed suffix
		 * delta. No captured address or desired completion is replayed. */
#ifdef M9_FAA_SOURCE_PHASES
		/* Public shared-depth metadata joins actual post endpoints and
		 * counted finish tokens. Its140 pause is diagnostic, not an
		 * invented original CPU wait or the in-place profile's170. */
		static const struct m9x_faa_submit_phase source[] = M9_FAA_SOURCE_PHASES;
#else
		static const struct m9x_faa_submit_phase source[] = {
			{ 4980, 0xc0 }, { 5068, 0x120 },
#if defined(M9_FAA_PRECLEAR_INPLACE_OUTPUT_CONTROL) || \
    defined(M9_FAA_PRECLEAR_INPLACE_COPY_CONTROL)
			{ 5304, 0x170 },
#ifdef M9_FAA_PRECLEAR_INPLACE_COPY_CONTROL
			{ 5936, 0x1a0 },
#endif
#else
			{ 5224, 0x160 }, { 5724, 0x180 },
#endif
		};
#endif
		struct m9x_faa_submit_phase phases[ARRAY_SIZE(source)];
		s64 delta = (s64)r->primary_bytes - M9_FAA_PRIMARY_BYTES;

		if (M9_FAA_PRIMARY_BYTES != source[ARRAY_SIZE(source) - 1].bytes ||
		    M9_FAA_TRACKER_LAST != source[ARRAY_SIZE(source) - 1].target) {
			ret = -EINVAL;
			goto unlock;
		}
#ifdef M9_FAA_UNLOCK_PUBLICATION_CONTROL
		if (ARRAY_SIZE(source) <
#ifdef M9_FAA_RELOCK_PUBLICATION_CONTROL
			4 || source[ARRAY_SIZE(source) - 3].bytes !=
				M9_FAA_NORMAL_LOCK_SPLIT_BYTES ||
		    source[ARRAY_SIZE(source) - 3].target !=
				M9_FAA_NORMAL_LOCK_SPLIT_TARGET ||
		    source[ARRAY_SIZE(source) - 2].bytes !=
				M9_FAA_NORMAL_UNLOCK_SPLIT_BYTES ||
		    source[ARRAY_SIZE(source) - 2].target !=
				M9_FAA_NORMAL_UNLOCK_SPLIT_TARGET) {
#else
			3 || source[ARRAY_SIZE(source) - 2].bytes !=
				M9_FAA_NORMAL_LOCK_SPLIT_BYTES ||
		    source[ARRAY_SIZE(source) - 2].target !=
				M9_FAA_NORMAL_LOCK_SPLIT_TARGET) {
#endif
			ret = -EINVAL;
			goto unlock;
		}
#endif
		for (i = 0; i < ARRAY_SIZE(m9_faa_clear_segments); i++)
			if (m9_faa_clear_segments[i].end > source[0].bytes) {
				ret = -EINVAL;
				goto unlock;
			}
		for (i = 0; i < ARRAY_SIZE(source); i++) {
			if ((s64)source[i].bytes + delta <= 0 ||
			    (s64)source[i].bytes + delta > U32_MAX) {
				ret = -ERANGE;
				goto unlock;
			}
			phases[i].bytes = source[i].bytes + delta;
			phases[i].target = base + source[i].target - 0x10;
		}
		ret = m9x_submit_faa_virtual_phases_locked(mdev,
				&r->owner[M9_FAA_PRIMARY_OWNER].system_node,
				r->owner[M9_FAA_PRIMARY_OWNER].dma, phases,
#ifdef M9_FAA_UNLOCK_PUBLICATION_CONTROL
				ARRAY_SIZE(phases) -
#ifdef M9_FAA_RELOCK_PUBLICATION_CONTROL
				2, point, &posted,
#else
				1, point, &posted,
#endif
#else
				ARRAY_SIZE(phases), point, &posted,
#endif
#ifdef M9_FAA_PROGRAM_ACCESS_CONTROL
				(m9_faa_defer_output_map || m9_faa_program_access_kick) ?
					m9_faa_publish_output_phase : NULL,
#else
				NULL,
#endif
				r);
#else
		ret = -EOPNOTSUPP;
#endif
	} else {
		ret = m9x_submit_faa_virtual_primary_locked(mdev,
				&r->owner[M9_FAA_PRIMARY_OWNER].system_node,
				r->owner[M9_FAA_PRIMARY_OWNER].dma, r->primary_bytes,
				target, point, &posted);
	}
	*color_active = posted && ret;
	if (!ret) {
		t->next_fence = target + 0x10;
		r->observation_point = *point;
		r->observation_generation = t->generation;
		r->observation_retired = true;
		/* An output allocation may already exist at an earlier vendor
		 * format-transition wait. Retirement of that prefix does not
		 * complete the later resolve or make its pixels readable. */
		if (!M9_FAA_OUTPUT_PENDING && m9_faa_output_owner(r)) {
			r->output_generation = t->generation;
			r->output_point = *point;
			r->output_retired = true;
		}
		m9_faa_observe(mdev, r, false);
		m9_faa_observe_borrowed_color(mdev, r, color_bo, false);
		/* The one-shot stream retains this address even without a SYSTEM
		 * output. Transfer the caller's existing pin/reference; taking a
		 * new pin here would nest its reservation under TRANSPORT. */
		if (!r->graph_binding) {
			r->retained_color = color_bo;
			*color_retained = true;
		}
		dev_info(&mdev->pdev->dev,
			 "M9 FAA private frame retired: point=%#llx sampled_primary=%#x ordinary_output=%#x 320x240\n",
			 *point, color, m9_faa_output_owner(r) ? m9_faa_output_owner(r)->base : 0);
	}
unlock:
	if (fence && ret)
		dma_fence_set_error(&fence->base, ret);
	if (fence && *color_active) {
		t->wedged = true;
		t->generation++;
		WARN_ON(t->quarantined_fence || t->quarantined_color);
		t->quarantined_fence = fence;
		t->quarantined_color = color_bo;
		fence = NULL;
	} else if (fence) {
		dma_fence_signal(&fence->base);
	}
	if (fence)
		dma_fence_put(&fence->base);
	mutex_unlock(&t->lock);
	return ret;
}

int m9x_faa_submit_diag(struct m9x_device *mdev, u32 color,
			struct drm_gem_vram_object *color_bo, u64 *point,
			bool *color_active, bool *color_retained)
{
	return m9_faa_submit(mdev, color, color_bo, point,
			      color_active, color_retained, NULL);
}

int m9x_faa_submit_graph(struct m9x_device *mdev,
			 struct m9_faa_graph_binding *binding,
			 u64 *point, bool *posted)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	static const u32 init[5] = {
		0xa4a3a1a1, 0x28041968, 0, 0, 0,
	};
	struct drm_gem_object *gem;
	struct drm_gem_vram_object *bo;
	struct iosys_map map = IOSYS_MAP_INIT_VADDR(NULL);
	u32 check[ARRAY_SIZE(init)];
	bool retained;
	int ret;

	if (!binding || !point || !posted)
		return -EINVAL;
	mutex_lock(&mdev->transport.lock);
	ret = mdev->transport.faa &&
	      mdev->transport.faa->graph_binding == binding &&
	      mdev->transport.faa->prepared &&
	      mdev->transport.generation ==
		mdev->transport.faa->graph_generation ? 0 : -ESTALE;
	mutex_unlock(&mdev->transport.lock);
	if (ret)
		return ret;
	gem = m9x_faa_graph_binding_owner(binding, PARHELIA_M9_FAA_SENTINEL);
	if (!gem || !m9x_gem_is_vram(gem) || gem->size < sizeof(init))
		return -EINVAL;
	/* The caller holds all six drm_exec reservations through the actual
	 * native wait. The two nonzero words are source instruction immediates,
	 * not captured address relocations; only the known 20 bytes are written. */
	bo = drm_gem_vram_of_gem(gem);
	ret = drm_gem_vram_vmap(bo, &map);
	if (ret)
		return ret;
	iosys_map_memcpy_to(&map, 0, init, sizeof(init));
	wmb();
	iosys_map_memcpy_from(check, &map, 0, sizeof(check));
	drm_gem_vram_vunmap(bo, &map);
	if (memcmp(check, init, sizeof(init)))
		return -EIO;

	ret = m9_faa_submit(mdev, 0, NULL, point, posted, &retained, binding);
	if (!ret)
		m9_faa_release_superseded(mdev);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

/* A separate, source-bounded public red DRAW caller continues the retained
 * INIT command namespace. The caller holds all six graph reservations; no
 * captured owner address, old prefix, or completion value is replayed. */
int m9x_faa_graph_draw_submit(struct m9x_device *mdev,
		struct m9_faa_graph_binding *binding, u32 generation,
		u64 init_point, u64 *point, bool *posted)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	struct m9_faa_owner *program, *constants, *secondary, *primary;
	struct m9x_fence *fence = NULL;
	u8 observed[M9_FAA_DRAW_PROGRAM_BYTES];
	u32 target, i;
	u64 stage_point;
	bool stage_posted;
	int ret;

	if (!binding || !point || !posted)
		return -EINVAL;
	*point = 0;
	*posted = false;
	mutex_lock(&t->lock);
	r = t->faa;
	if (!r || r->graph_binding != binding ||
	    r->graph_generation != generation || t->generation != generation ||
	    !r->graph_draw_claimed || !r->graph_draw_payload_ready ||
	    r->graph_draw_posts || r->graph_draw_point || !r->draw_image ||
	    !r->observation_retired || r->observation_point != init_point ||
	    t->submitted_point != init_point || t->completed_point != init_point ||
	    t->next_fence != lower_32_bits(init_point) + 0x10 ||
	    lower_32_bits(init_point) > U32_MAX -
		(r->graph_draw_with_lock ? 0xd0 :
		 r->graph_draw_with_finish ? 0xa0 : 0x70) ||
	    !t->faa_virtual_primary || !t->faa_source_context ||
	    !t->transport_armed || !t->bus_master_enabled ||
	    !t->engine_transitioned || !t->restore_ready || !t->fence_cpu ||
	    t->wedged || t->async_stop || t->async_pending || t->queue_count) {
		ret = -ESTALE;
		goto out;
	}
	primary = &r->owner[M9_FAA_PRIMARY_OWNER];
	program = &r->draw_owner[0];
	constants = &r->draw_owner[1];
	secondary = &r->draw_owner[2];
	if (!primary->cpu || !drm_mm_node_allocated(&primary->system_node) ||
	    primary->system_node.start != primary->base ||
	    primary->bytes < M9_FAA_DRAW_PRIMARY_SUFFIX_BYTES ||
	    r->primary_bytes > primary->bytes - M9_FAA_DRAW_PRIMARY_SUFFIX_BYTES ||
	    !program->vram ||
	    program->bytes < M9_FAA_DRAW_PROGRAM_BYTES ||
	    !constants->cpu || !secondary->cpu ||
	    constants->bytes < M9_FAA_DRAW_CONSTANTS_BYTES ||
	    secondary->bytes < M9_FAA_DRAW_SECONDARY_BYTES ||
	    !drm_mm_node_allocated(&constants->system_node) ||
	    !drm_mm_node_allocated(&secondary->system_node) ||
	    constants->system_node.start != constants->base ||
	    secondary->system_node.start != secondary->base) {
		ret = -ESTALE;
		goto out;
	}
	/* Both new SYSTEM owners must still resolve to their retained coherent
	 * pages before publishing the first command that can consume them. */
	for (i = 1; i < ARRAY_SIZE(r->draw_owner); i++) {
		struct m9_faa_owner *o = &r->draw_owner[i];

		ret = m9x_system_pages_mapped(mdev, o->base, o->dma,
					      o->bytes / PAGE_SIZE, 1u, NULL, NULL);
		if (ret)
			goto out;
	}
	if (m9x_address_check_local_identity(mdev, program->base,
					     program->bytes)) {
		ret = -ESTALE;
		goto out;
	}
	/* Genuine frontend314 kick precedes CPU-root f/0/c and the only
	 * source-known 240-byte program upload. Unknown local tail is untouched. */
	m9x_write(mdev, M9_REG_FRONTEND_KICK, 1);
	ret = m9x_address_cpu_local_prepare(mdev, program->base, program->bytes);
	if (ret)
		goto out;
	iosys_map_memcpy_to(&program->map, 0, r->draw_image->program,
				M9_FAA_DRAW_PROGRAM_BYTES);
	wmb();
	m9x_write(mdev, M9_REG_FRONTEND_STATE, 0);
	m9x_read(mdev, M9_REG_FRONTEND_STATE);
	iosys_map_memcpy_from(observed, &program->map, 0, sizeof(observed));
	if (memcmp(observed, r->draw_image->program, sizeof(observed))) {
		ret = -EIO;
		goto out;
	}
	/* Source writes two disjoint constant spans. Do not initialize the
	 * unknown gap [136,256) or any allocation tail. */
	memcpy(constants->cpu, r->draw_image->constants, 136);
	memcpy((u8 *)constants->cpu + 256,
	       r->draw_image->constants + 256, 188);
	memcpy((u8 *)primary->cpu + r->primary_bytes,
	       r->draw_image->primary_suffix, 64);
	wmb();
	/* The single reservation fence covers every posted part, including
	 * the raster tail when requested. Prefix retirement does not release
	 * any owner while the final GPU commands may still access it. */
	target = lower_32_bits(init_point) +
		(r->graph_draw_with_lock ? 0xc0 :
		 r->graph_draw_with_finish ? 0x90 : 0x60);
	fence = m9x_fence_create_target_locked(mdev, target);
	if (!fence) {
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++)
		dma_resv_add_fence(m9x_faa_graph_binding_owner(binding, i)->resv,
				   &fence->base, DMA_RESV_USAGE_WRITE);
	/* The two identical +64 END writes are source events, not receipts.
	 * The indirect preprocessing pointer begins at the second boundary. */
	for (i = 0; i < 2; i++) {
		ret = m9x_faa_graph_draw_end_locked(mdev, binding, generation,
					   init_point, i, &stage_point,
					   &stage_posted);
		*posted |= stage_posted;
		if (ret)
			goto out;
	}
	memcpy(secondary->cpu, r->draw_image->secondary,
	       M9_FAA_DRAW_SECONDARY_BYTES);
	memcpy((u8 *)primary->cpu + r->primary_bytes + 64,
	       r->draw_image->primary_suffix + 64, 24);
	wmb();
	ret = m9x_faa_graph_draw_end_locked(mdev, binding, generation,
				    init_point, 2, point, &stage_posted);
	*posted |= stage_posted;
	if (ret)
		goto out;
	if (*point != init_point + 0x60) {
		ret = -EIO;
		goto out;
	}
	dev_info(&mdev->pdev->dev,
		 "M9 FAA draw prefix retired: init=%#llx prefix=%#llx bytes=%u submitted=%#llx completed=%#llx\n",
		 init_point, *point, r->graph_draw_end_bytes,
		 t->submitted_point, t->completed_point);
	if (r->graph_draw_with_finish) {
		u32 prefix_bytes = r->graph_draw_end_bytes;

		if (r->graph_draw_posts != 3 ||
		    r->graph_draw_point != init_point + 0x60 ||
		    t->completed_point != init_point + 0x60 ||
		    t->submitted_point != init_point + 0x60 ||
		    prefix_bytes != r->primary_bytes + M9_FAA_DRAW_PRIMARY_SUFFIX_BYTES ||
		    primary->bytes - prefix_bytes < M9_FAA_DRAW_TAIL_PRIMARY_BYTES ||
		    secondary->bytes < M9_FAA_DRAW_TAIL_SECONDARY_OFFSET +
			M9_FAA_DRAW_TAIL_SECONDARY_BYTES) {
			ret = -ESTALE;
			goto out;
		}
		/* Preserve original DRAW post then explicit source finish post.
		 * The first post has no completion claim; only the second waits. */
		for (i = 0; i < 2; i++) {
			u32 offset = i ? 692 : 0;
			u32 bytes = i ? 16 : 692;
			u32 end = prefix_bytes + (i + 1) * 12;

			memcpy((u8 *)secondary->cpu + M9_FAA_DRAW_TAIL_SECONDARY_OFFSET +
			       offset, r->draw_image->tail_secondary + offset, bytes);
			memcpy((u8 *)primary->cpu + prefix_bytes + i * 12,
			       r->draw_image->tail_primary + i * 12, 12);
			wmb();
			ret = m9x_post_faa_graph_end_locked(mdev, &primary->system_node,
				primary->dma, r->graph_draw_end_bytes, end, false,
				i ? lower_32_bits(init_point) + 0x90 : 0,
				&stage_point, &stage_posted);
			*posted |= stage_posted;
			if (stage_posted) {
				r->graph_draw_end_bytes = end;
				r->graph_draw_posts++;
			}
			if (ret)
				goto out;
		}
		if (stage_point != init_point + 0x90) {
			ret = -EIO;
			goto out;
		}
		*point = stage_point;
		r->graph_draw_point = *point;
		r->graph_secondary_cursor = M9_FAA_DRAW_TAIL_SECONDARY_OFFSET +
			M9_FAA_DRAW_TAIL_SECONDARY_BYTES;
		r->graph_vertex_cursor = M9_FAA_NEXT_VERTEX_OFFSET;
		dev_info(&mdev->pdev->dev,
			 "M9 FAA returned DRAW finish retired: point=%#llx bytes=%u posts=%u; no Lock or pixels claimed\n",
			 *point, r->graph_draw_end_bytes, r->graph_draw_posts);
	}
	if (r->graph_draw_with_lock) {
		u32 draw_bytes = r->graph_draw_end_bytes;

		if (!r->graph_draw_with_finish || r->graph_draw_posts != 5 ||
		    r->graph_draw_point != init_point + 0x90 ||
		    t->completed_point != init_point + 0x90 ||
		    t->submitted_point != init_point + 0x90 ||
		    draw_bytes != r->primary_bytes + M9_FAA_DRAW_PRIMARY_SUFFIX_BYTES +
			M9_FAA_DRAW_TAIL_PRIMARY_BYTES ||
		    draw_bytes > primary->bytes ||
		    primary->bytes - draw_bytes < M9_FAA_LOCK_PRIMARY_BYTES) {
			ret = -ESTALE;
			goto out;
		}
		/* Source normal Lock posts its populated prefix twice, then
		 * appends the completion packet. Only the last post waits. */
		for (i = 0; i < 3; i++) {
			u32 end = draw_bytes + (i == 2 ? M9_FAA_LOCK_PRIMARY_BYTES :
				M9_FAA_LOCK_FIRST_POST_BYTES);

			if (i == 0)
				memcpy((u8 *)primary->cpu + draw_bytes,
				       r->draw_image->lock_primary, M9_FAA_LOCK_FIRST_POST_BYTES);
			else if (i == 2)
				memcpy((u8 *)primary->cpu + draw_bytes + M9_FAA_LOCK_FIRST_POST_BYTES,
				       r->draw_image->lock_primary + M9_FAA_LOCK_FIRST_POST_BYTES,
				       M9_FAA_LOCK_PRIMARY_BYTES - M9_FAA_LOCK_FIRST_POST_BYTES);
			wmb();
			ret = m9x_post_faa_graph_end_locked(mdev, &primary->system_node,
				primary->dma, r->graph_draw_end_bytes, end, i == 1,
				i == 2 ? target : 0, &stage_point, &stage_posted);
			*posted |= stage_posted;
			if (stage_posted) {
				r->graph_draw_end_bytes = end;
				r->graph_draw_posts++;
			}
			if (ret)
				goto out;
		}
		if (stage_point != init_point + 0xc0) {
			ret = -EIO;
			goto out;
		}
		*point = stage_point;
		r->graph_draw_point = *point;
		dev_info(&mdev->pdev->dev,
			 "M9 FAA normal Lock prefix retired: point=%#llx bytes=%u posts=%u; returned Lock and pixels unproved\n",
			 *point, r->graph_draw_end_bytes, r->graph_draw_posts);
	}
	dma_rmb();
	dev_info(&mdev->pdev->dev,
		 "M9 FAA retired counters: +00=%#x/%#x +20=%#x/%#x +40=%#x/%#x +60=%#x/%#x\n",
		 READ_ONCE(((u32 *)t->fence_cpu)[0]),
		 READ_ONCE(((u32 *)t->fence_cpu)[1]),
		 READ_ONCE(((u32 *)t->fence_cpu)[0x20 / 4]),
		 READ_ONCE(((u32 *)t->fence_cpu)[0x24 / 4]),
		 READ_ONCE(((u32 *)t->fence_cpu)[0x40 / 4]),
		 READ_ONCE(((u32 *)t->fence_cpu)[0x44 / 4]),
		 READ_ONCE(((u32 *)t->fence_cpu)[0x60 / 4]),
		 READ_ONCE(((u32 *)t->fence_cpu)[0x64 / 4]));
out:
	m9x_transport_fence_finish_locked(mdev, fence, ret, *posted);
	mutex_unlock(&t->lock);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

#ifdef M9_FAA_GRAPH_INIT_CONTROL
/* The separately typed first public-VS prefix cannot authorize the fixed-red
 * raster/Lock path. The original source f0 wait remains unread. */
static int m9_faa_public_vs_state(const struct m9x_transport *t,
		struct m9_faa_graph_binding *binding, u32 generation, u64 init_point)
{
	const struct m9x_faa_replay *r = t->faa;

	if (!r || !binding || r->graph_binding != binding ||
	    r->graph_generation != generation || t->generation != generation ||
	    r->observation_generation != generation ||
	    !r->prepared || !r->attempted || !r->graph_draw_claimed ||
	    !r->public_vs_payload_ready || r->public_vs_prefix_retired ||
	    r->public_vs_head_attempted || r->public_vs_head_retired ||
	    !r->public_vs_image || r->draw_image || r->graph_draw_payload_ready ||
	    r->graph_draw_posts || r->graph_draw_point || r->graph_preparing ||
	    r->graph_draw_end_bytes != r->primary_bytes ||
	    !r->observation_retired || r->observation_point != init_point ||
	    t->submitted_point != init_point || t->completed_point != init_point ||
	    t->next_fence != lower_32_bits(init_point) + 0x10 ||
	    lower_32_bits(init_point) > U32_MAX - 0x60 ||
	    !t->faa_virtual_primary || !t->faa_source_context ||
	    !t->transport_armed || !t->bus_master_enabled ||
	    !t->engine_transitioned || !t->restore_ready || !t->fence_cpu ||
	    t->wedged || t->async_stop || t->async_pending || t->queue_count)
		return -ESTALE;
	if (r->public_vs_image->ends.first_end != r->primary_bytes + 64 ||
	    r->public_vs_image->ends.final_end != r->primary_bytes + 112 ||
	    r->public_vs_image->ends.original_wait != init_point + 0x40 ||
	    r->public_vs_image->ends.independent_finish != init_point + 0x50)
		return -ESTALE;
	return 0;
}
#endif

static int m9_faa_graph_public_vs_submit_mode(struct m9x_device *mdev,
		struct m9_faa_graph_binding *binding, u32 generation,
		u64 init_point, u64 *point, bool *posted, bool head)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	struct m9_faa_public_vs_storage *image;
	struct m9_faa_owner *primary, *program, *constants, *secondary;
	struct m9x_fence *fence = NULL;
	u8 observed[M9_FAA_PUBLIC_VS_PROGRAM_BYTES];
	u32 i, target;
	u64 stage_point;
	bool stage_posted;
	int ret;

	if (!point || !posted)
		return -EINVAL;
	*point = 0;
	*posted = false;
	mutex_lock(&t->lock);
	ret = m9_faa_public_vs_state(t, binding, generation, init_point);
	if (ret)
		goto out;
	r = t->faa;
	image = r->public_vs_image;
	primary = &r->owner[M9_FAA_PRIMARY_OWNER];
	program = &r->draw_owner[0];
	constants = &r->draw_owner[1];
	secondary = &r->draw_owner[2];
	if (!primary->cpu || !drm_mm_node_allocated(&primary->system_node) ||
	    primary->system_node.start != primary->base ||
	    image->ends.final_end > primary->bytes || !program->vram ||
	    program->bytes < M9_FAA_PUBLIC_VS_PROGRAM_BYTES ||
	    !constants->cpu || !secondary->cpu ||
	    constants->bytes < M9_FAA_PUBLIC_VS_CONSTANT_EXTENT ||
	    secondary->bytes < M9_FAA_PUBLIC_VS_SECONDARY_BYTES ||
	    !drm_mm_node_allocated(&constants->system_node) ||
	    !drm_mm_node_allocated(&secondary->system_node) ||
	    constants->system_node.start != constants->base ||
	    secondary->system_node.start != secondary->base) {
		ret = -ESTALE;
		goto out;
	}
	/* Check both retained flags30 SYSTEM maps before CPU uploads or posts. */
	for (i = 1; i < ARRAY_SIZE(r->draw_owner); i++) {
		struct m9_faa_owner *o = &r->draw_owner[i];

		ret = m9x_system_pages_mapped(mdev, o->base, o->dma,
					      o->bytes / PAGE_SIZE, 1u, NULL, NULL);
		if (ret)
			goto out;
	}
	ret = m9x_address_check_local_identity(mdev, program->base, program->bytes);
	if (ret)
		goto out;
	target = lower_32_bits(head ? init_point + 0x10 :
			      image->ends.independent_finish);
	fence = m9x_fence_create_target_locked(mdev, target);
	if (!fence) {
		ret = -ENOMEM;
		goto out;
	}
	if (head)
		r->public_vs_head_attempted = true;
	/* Caller reserved all six BOs with one fence slot each. Every posted
	 * part has this final WRITE fence; posted errors quarantine it. */
	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++)
		dma_resv_add_fence(m9x_faa_graph_binding_owner(binding, i)->resv,
				   &fence->base, DMA_RESV_USAGE_WRITE);
	m9x_write(mdev, M9_REG_FRONTEND_KICK, 1);
	ret = m9x_address_cpu_local_prepare(mdev, program->base, program->bytes);
	if (ret)
		goto out;
	iosys_map_memcpy_to(&program->map, 0, image->program, sizeof(image->program));
	wmb();
	m9x_write(mdev, M9_REG_FRONTEND_STATE, 0);
	m9x_read(mdev, M9_REG_FRONTEND_STATE);
	iosys_map_memcpy_from(observed, &program->map, 0, sizeof(observed));
	if (memcmp(observed, image->program, sizeof(observed))) {
		ret = -EIO;
		goto out;
	}
	/* Source-known disjoint spans only; never initialize [152,256). */
	memcpy(constants->cpu, image->constants, M9_FAA_PUBLIC_VS_CONSTANT_BYTES);
	memcpy((u8 *)constants->cpu + 256, image->constants + 256,
	       M9_FAA_PUBLIC_VS_GEOMETRY_BYTES);
	memcpy((u8 *)primary->cpu + r->primary_bytes, image->primary, 64);
	wmb();
	for (i = 0; i < (head ? 2 : 3); i++) {
		u32 end = i == 2 ? image->ends.final_end : image->ends.first_end;

		if (i == 2) {
			memcpy(secondary->cpu, image->secondary, sizeof(image->secondary));
			memcpy((u8 *)primary->cpu + r->primary_bytes + 64,
			       image->primary + 64, sizeof(image->primary) - 64);
			wmb();
		}
		ret = m9x_post_faa_graph_end_locked(mdev, &primary->system_node,
			primary->dma, r->graph_draw_end_bytes, end, i == 1,
			i == (head ? 1 : 2) ? target : 0, &stage_point, &stage_posted);
		*posted |= stage_posted;
		if (stage_posted) {
			r->graph_draw_end_bytes = end;
			r->graph_draw_posts++;
		}
		if (ret)
			goto out;
	}
	if (stage_point != (head ? init_point + 0x10 :
			   image->ends.independent_finish)) {
		ret = -EIO;
		goto out;
	}
	*point = stage_point;
	r->graph_draw_point = stage_point;
	if (head)
		r->public_vs_head_retired = true;
	else {
		r->public_vs_prefix_retired = true;
		m9_faa_public_vs_prefix_observe(mdev, r);
	}
	dev_info(&mdev->pdev->dev,
		 "M9 FAA public VS %s retired: init=%#llx point=%#llx bytes=%u posts=%u program=%u; original DRAW unreturned, no pixels claimed\n",
		 head ? "head" : "prefix", init_point, *point, r->graph_draw_end_bytes, r->graph_draw_posts,
		 M9_FAA_PUBLIC_VS_PROGRAM_BYTES);
	dma_rmb();
	dev_info(&mdev->pdev->dev,
		 "M9 FAA retired counters: +00=%#x/%#x +20=%#x/%#x +40=%#x/%#x +60=%#x/%#x\n",
		 READ_ONCE(((u32 *)t->fence_cpu)[0]),
		 READ_ONCE(((u32 *)t->fence_cpu)[1]),
		 READ_ONCE(((u32 *)t->fence_cpu)[0x20 / 4]),
		 READ_ONCE(((u32 *)t->fence_cpu)[0x24 / 4]),
		 READ_ONCE(((u32 *)t->fence_cpu)[0x40 / 4]),
		 READ_ONCE(((u32 *)t->fence_cpu)[0x44 / 4]),
		 READ_ONCE(((u32 *)t->fence_cpu)[0x60 / 4]),
		 READ_ONCE(((u32 *)t->fence_cpu)[0x64 / 4]));
out:
	if (fence && ret)
		dma_fence_set_error(&fence->base, ret);
	if (ret && *posted) {
		t->wedged = true;
		t->generation++;
		WARN_ON(t->quarantined_fence);
		t->quarantined_fence = fence;
		fence = NULL;
	} else if (fence && head && !ret) {
		/* c0 precedes the remaining original state packets. Keep all six
		 * BOs unavailable until a source-qualified successor or DMA stop. */
		r->public_vs_head_fence = fence;
		fence = NULL;
	} else if (fence) {
		dma_fence_signal(&fence->base);
	}
	if (fence)
		dma_fence_put(&fence->base);
	mutex_unlock(&t->lock);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

int m9x_faa_graph_public_vs_submit(struct m9x_device *mdev,
		struct m9_faa_graph_binding *binding, u32 generation,
		u64 init_point, u64 *point, bool *posted)
{
	return m9_faa_graph_public_vs_submit_mode(mdev, binding, generation,
						init_point, point, posted, false);
}

int m9x_faa_graph_public_vs_head_submit(struct m9x_device *mdev,
		struct m9_faa_graph_binding *binding, u32 generation,
		u64 init_point, u64 *point, bool *posted)
{
	return m9_faa_graph_public_vs_submit_mode(mdev, binding, generation,
						init_point, point, posted, true);
}

/* Version-one little-endian raw snapshot. No caller supplies an owner,
 * physical address, offset or decoder. The compiled trusted source profile
 * selects the exact coherent DMA owners and their logical allocation spans. */
struct m9_faa_dma_header {
	__le32 magic, version, header_bytes, owners;
	__le64 point;
	__le32 generation, bytes;
};
struct m9_faa_dma_entry {
	__le32 owner, base, bytes, allocated_bytes, data_offset, reserved;
};
#define M9_FAA_DMA_SNAPSHOT_BUDGET (16u * 1024u * 1024u)

static int m9_faa_dma_span(const struct m9_faa_owner_spec *spec,
			   const struct m9_faa_owner *owner, u32 *bytes)
{
	if (spec->local || spec->system)
		return 0;
	if (spec->flags30 || spec->mapped_bytes || !spec->bytes || !owner->cpu ||
	    owner->vram || owner->cpu_wc || drm_mm_node_allocated(&owner->system_node) ||
	    upper_32_bits(owner->dma) || !IS_ALIGNED(owner->dma, PAGE_SIZE) ||
	    owner->base != lower_32_bits(owner->dma) ||
	    (u64)owner->dma + owner->bytes > U32_MAX ||
	    (u64)PAGE_ALIGN((u64)spec->bytes) != owner->bytes)
		return -EINVAL;
	*bytes = spec->bytes;
	return 1;
}

int m9x_faa_dma_snapshot(struct m9x_device *mdev, void **data, size_t *bytes)
{
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	struct m9_faa_dma_header *header;
	struct m9_faa_dma_entry *entry;
	u32 i, count = 0, payload = 0, span, total, offset;
	int idx, ret;

	BUILD_BUG_ON(sizeof(*header) != 32);
	BUILD_BUG_ON(sizeof(*entry) != 24);
	*data = NULL;
	*bytes = 0;
	if (!m9_faa_diag)
		return -EACCES;
	if (!drm_dev_enter(&mdev->drm, &idx))
		return -ENODEV;
	if (!mutex_trylock(&t->lock)) {
		ret = -EBUSY;
		goto exit;
	}
	r = t->faa;
	if (t->async_stop) {
		ret = -ENODEV;
		goto unlock;
	}
	if (t->wedged) {
		ret = -EIO;
		goto unlock;
	}
	if (!r || !r->attempted || !r->observation_retired || !r->observation_point ||
	    r->observation_generation != t->generation ||
	    r->observation_point > t->completed_point || t->async_pending ||
	    t->queue_count) {
		ret = -ENODATA;
		goto unlock;
	}
	/* Validate the complete inventory and budget before allocating/copying.
	 * Failed final submission never reaches this raw read contract. */
	for (i = 0; i < ARRAY_SIZE(r->owner); i++) {
		ret = m9_faa_dma_span(&m9_faa_owner_specs[i], &r->owner[i], &span);
		if (ret < 0)
			goto unlock;
		if (!ret)
			continue;
		if (span > M9_FAA_DMA_SNAPSHOT_BUDGET - payload) {
			ret = -E2BIG;
			goto unlock;
		}
		payload += span;
		count++;
	}
	if (!count) {
		ret = -ENODATA;
		goto unlock;
	}
	if (check_mul_overflow(count, (u32)sizeof(*entry), &offset) ||
	    check_add_overflow(offset, (u32)sizeof(*header), &offset) ||
	    check_add_overflow(offset, payload, &total) ||
	    total > M9_FAA_DMA_SNAPSHOT_BUDGET) {
		ret = -E2BIG;
		goto unlock;
	}
	header = kvmalloc(total, GFP_KERNEL);
	if (!header) {
		ret = -ENOMEM;
		goto unlock;
	}
	*header = (struct m9_faa_dma_header) {
		.magic = cpu_to_le32(0x4d44394d), /* M9DM */
		.version = cpu_to_le32(1), .header_bytes = cpu_to_le32(offset),
		.owners = cpu_to_le32(count), .point = cpu_to_le64(r->observation_point),
		.generation = cpu_to_le32(t->generation), .bytes = cpu_to_le32(total),
	};
	entry = (void *)(header + 1);
	rmb();
	dma_rmb();
	for (i = 0; i < ARRAY_SIZE(r->owner); i++) {
		const struct m9_faa_owner_spec *spec = &m9_faa_owner_specs[i];
		struct m9_faa_owner *owner = &r->owner[i];

		if (spec->local || spec->system)
			continue;
		*entry++ = (struct m9_faa_dma_entry) {
			.owner = cpu_to_le32(i), .base = cpu_to_le32(owner->base),
			.bytes = cpu_to_le32(spec->bytes),
			.allocated_bytes = cpu_to_le32(owner->bytes),
			.data_offset = cpu_to_le32(offset), .reserved = 0,
		};
		memcpy((u8 *)header + offset, owner->cpu, spec->bytes);
		offset += spec->bytes;
	}
	*data = header;
	*bytes = total;
	ret = 0;
unlock:
	mutex_unlock(&t->lock);
exit:
	drm_dev_exit(idx);
	return ret;
}

/* A direct F022 diagnostic aperture access is not a passive snapshot.
 * The public Lock sequence retains its cached F021 mapper across Unlock;
 * replacing global aperture state between those calls changes the next
 * returned image. Keep the pre-submit probe, then require the source-owned
 * Lock/relock path until this replay is torn down. */
int m9x_faa_direct_view_admit_locked(struct m9x_device *mdev)
{
	struct m9x_transport *t = &mdev->transport;

	lockdep_assert_held(&t->lock);
#if defined(M9_FAA_PREPARED_FLOW_CONTROL) && defined(M9_FAA_NORMAL_LOCK_CPU_VIEW)
	if (m9_faa_diag && t->faa && t->faa->attempted)
		return -EBUSY;
#endif
	return 0;
}

/* The ioctl already owns the caller's pin and reservation before entering
 * transport.lock. Admit only this private profile's immutable borrowed
 * primary after its final actual write wait, never another caller's BO. */
int m9x_faa_lock_view_admit_locked(struct m9x_device *mdev,
				   struct drm_gem_vram_object *color, u32 base,
				   u64 bytes)
{
	struct m9x_transport *t = &mdev->transport;

#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_faa_replay *r = t->faa;
	u32 bound, source;
	s64 offset;

	lockdep_assert_held(&t->lock);
	/* The source normal Lock staging and primary share owner1. The typed
	 * descriptor window is distinct from backing capacity, and this exact
	 * same-graph retirement point authenticates access.

	 */
	if (!color || m9x_local_gpu_base)
		return -EOPNOTSUPP;
	if (t->async_stop)
		return -ENODEV;
	if (t->wedged)
		return -EIO;
	if (!r || !r->graph_binding || !r->observation_retired ||
	    (!r->color_lock_retired && !r->public_vs_lock_retired &&
	     (!r->graph_draw_with_lock || !r->graph_draw_with_finish)) ||
	    r->graph_generation != t->generation ||
	    r->observation_generation != t->generation ||
	    !r->observation_point ||
	    t->completed_point != r->graph_draw_point ||
	    t->submitted_point != r->graph_draw_point ||
	    t->next_fence != lower_32_bits(r->graph_draw_point) + 0x10 ||
	    t->async_pending || t->queue_count || !t->transport_armed ||
	    !t->bus_master_enabled || !t->faa_source_context ||
	    !t->faa_virtual_primary)
		return -ENODATA;
	if (bytes != m9x_faa_primary_window_bytes(&r->graph_layout))
		return -EOPNOTSUPP;
	/* The second normal Lock uses the exact same source mapper/format but
	 * has its own actual retirement marker and full retained cursor join.
	 * Unlock/preprocessing/rendering receipts alone never admit F021.

	 */
	if (r->color_lock_retired) {
		if (!r->color_image || !r->color_attempted ||
		    r->unlock_started ||
		    r->graph_draw_point != r->color_lock_point ||
		    !r->color_lock_point ||
		    r->graph_draw_posts !=
			    r->color_posts_start +
				    (r->color_image->lock_posts ?
					     r->color_image->lock_posts :
					     4) ||
		    r->graph_draw_end_bytes !=
			    r->color_primary_start +
				    r->color_image->primary_length)
			return -ENODATA;
	} else if (r->public_vs_next_lock_retired) {
		const struct m9_faa_public_vs_next_storage *n =
			r->public_vs_next_image;

		if (!n || !r->public_vs_next_attempted ||
		    r->public_vs_head_fence || r->public_vs_draw_fence ||
		    r->graph_draw_point != n->unlock_point + 0xd0 ||
		    r->unlock_point != n->unlock_point ||
		    r->graph_draw_posts != 13 ||
		    r->graph_draw_end_bytes != n->lock_end ||
		    n->lock_end != n->primary_start + 60 +
					   M9_FAA_VS_LOCK_PRIMARY_BYTES)
			return -ENODATA;
	} else if (r->public_vs_lock_retired) {
		if (!r->public_vs_head_retired || !r->public_vs_draw_observed ||
		    !r->public_vs_lock_attempted || r->unlock_started ||
		    r->public_vs_head_fence || r->public_vs_draw_fence ||
		    r->graph_draw_point != r->observation_point + 0xe0 ||
		    r->graph_draw_posts != 6 ||
		    r->graph_draw_end_bytes !=
			    r->primary_bytes + 124 +
				    M9_FAA_VS_LOCK_PRIMARY_BYTES)
			return -ENODATA;
	} else if (r->unlock_started) {
		if (!r->unlock_retired || !r->normal_lock_view_seen ||
		    !r->graph_second_lock_retired ||
		    r->graph_draw_point != r->observation_point + 0x190 ||
		    r->graph_draw_posts != 17 ||
		    r->graph_draw_end_bytes !=
			    r->primary_bytes +
				    M9_FAA_DRAW_PRIMARY_SUFFIX_BYTES +
				    M9_FAA_DRAW_TAIL_PRIMARY_BYTES +
				    2 * M9_FAA_LOCK_PRIMARY_BYTES +
				    M9_FAA_UNLOCK_PRIMARY_BYTES +
				    M9_FAA_NEXT_PRIMARY_BYTES +
				    M9_FAA_NEXT_FINISH_PRIMARY_BYTES)
			return -ENODATA;
	} else if (r->graph_second_lock_retired ||
		   r->graph_draw_point != r->observation_point + 0xc0 ||
		   r->graph_draw_posts != 8 ||
		   r->graph_draw_end_bytes !=
			   r->primary_bytes + M9_FAA_DRAW_PRIMARY_SUFFIX_BYTES +
				   M9_FAA_DRAW_TAIL_PRIMARY_BYTES +
				   M9_FAA_LOCK_PRIMARY_BYTES) {
		return -ENODATA;
	}

	if (m9x_faa_graph_binding_owner(r->graph_binding,
					PARHELIA_M9_FAA_PRIMARY) !=
		    &color->bo.base ||
	    !m9x_faa_graph_binding_address(r->graph_binding,
					   PARHELIA_M9_FAA_PRIMARY, &bound,
					   &source) ||
	    source != 1 || bound != base || r->owner[source].base != base ||
	    r->owner[source].bytes !=
		    r->graph_layout.owner[M9_FAA_SURFACE_PRIMARY].capacity ||
	    color->bo.base.size != r->owner[source].bytes)
		return -ESTALE;
	offset = drm_gem_vram_offset(color);
	if (offset < 0 || (u64)offset != base || !IS_ALIGNED(base, PAGE_SIZE) ||
	    r->owner[source].bytes > (u64)U32_MAX + 1 - base)
		return -ESTALE;
	return 0;
#elif defined(M9_FAA_PREPARED_FLOW_CONTROL) && \
	defined(M9_FAA_NORMAL_LOCK_CPU_VIEW)
	struct m9x_faa_replay *r = t->faa;
	const struct m9_faa_owner_spec *spec =
		&m9_faa_owner_specs[M9_FAA_COLOR_OWNER];
	s64 offset;
	u64 physical;

	lockdep_assert_held(&t->lock);
	/* This marker follows a complete same-run normal-Lock mapper/owner
	 * join. Earlier prepared prefixes are not that contract.

	 */
	if (
#ifdef M9_FAA_UNLOCK_PUBLICATION_CONTROL
		M9_FAA_NORMAL_LOCK_SPLIT_TARGET != 0x180 ||
#else
		M9_FAA_TRACKER_LAST != 0x180 ||
#endif
		M9_FAA_NORMAL_LOCK_CPU_VIEW != 1 || !m9_faa_diag ||
		m9x_local_gpu_base || !color || bytes != 0x50000 ||
		!spec->local || spec->system || spec->bytes != bytes)
		return -EOPNOTSUPP;
	if (t->async_stop)
		return -ENODEV;
	if (t->wedged)
		return -EIO;
	if (!r || !r->attempted || !r->observation_retired ||
	    !r->observation_point ||
	    r->observation_generation != t->generation ||
	    r->observation_point > t->completed_point || t->async_pending ||
	    t->queue_count || r->retained_color != color ||
	    (r->unlock_started &&
	     (!r->relock_retired || r->relock_generation != t->generation ||
	      r->relock_point != t->completed_point)))
		return -ENODATA;
	offset = drm_gem_vram_offset(color);
	if (offset < 0 ||
	    check_add_overflow(M9_VRAM_GPU_BASE, (u64)offset, &physical) ||
	    physical != base || !IS_ALIGNED(base, PAGE_SIZE) ||
	    bytes > (u64)U32_MAX + 1 - base || color->bo.base.size != bytes ||
	    r->borrowed_color_bytes != bytes ||
	    r->owner[M9_FAA_COLOR_OWNER].base != base)
		return -ESTALE;
	return 0;
#else
	lockdep_assert_held(&t->lock);
	(void)color;
	(void)base;
	(void)bytes;
	return -EOPNOTSUPP;
#endif
}

int m9x_faa_lock_view_observed_locked(struct m9x_device *mdev,
				struct drm_gem_vram_object *color)
{
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r = t->faa;

	lockdep_assert_held(&t->lock);
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	int ret;

	if (!r)
		return -ENODATA;
	ret = m9x_faa_lock_view_admit_locked(mdev, color,
			r->owner[1].base, m9x_faa_primary_window_bytes(&r->graph_layout));
	if (!ret) {
		if (r->public_vs_next_lock_retired)
			r->public_vs_next_view_seen = true;
		else if (r->graph_second_lock_retired)
			r->second_lock_view_seen = true;
		else
			r->normal_lock_view_seen = true;
	}
	return ret;
#elif defined(M9_FAA_UNLOCK_PUBLICATION_CONTROL)
	if (!r || !r->observation_retired ||
	    r->retained_color != color ||
	    r->observation_generation != t->generation ||
	    (r->unlock_started ?
	     (!r->relock_retired || r->relock_generation != t->generation ||
	      r->relock_point != t->completed_point) :
	     r->observation_point != t->completed_point))
		return -ENODATA;
	if (r->unlock_started)
		r->relock_view_seen = true;
	else
		r->normal_lock_view_seen = true;
	return 0;
#else
	(void)r;
	(void)color;
	return 0;
#endif
}

/* A successful CPU view and its same-graph Lock receipt are both required.
 * Caller holds the six graph BO reservations before taking transport.lock. */
int m9x_faa_graph_unlock_get_locked(struct m9x_device *mdev,
		u32 generation, u64 lock_point, struct m9_faa_graph_binding **out)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r = t->faa;
	struct drm_gem_object *gem;
	int ret;

	lockdep_assert_held(&t->lock);
	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!r || !r->graph_binding || !r->normal_lock_view_seen ||
	    (r->unlock_started &&
	     (!r->graph_second_lock_retired || !r->second_lock_view_seen) &&
	     (!r->public_vs_next_lock_retired || !r->public_vs_next_view_seen)) ||
	    r->graph_generation != generation ||
	    t->generation != generation || !lock_point ||
	    r->graph_draw_point != lock_point || !t->engine_transitioned ||
	    !t->restore_ready || !t->fence_cpu)
		return -ESTALE;
	gem = m9x_faa_graph_binding_owner(r->graph_binding, PARHELIA_M9_FAA_PRIMARY);
	if (!gem || !m9x_gem_is_vram(gem))
		return -ESTALE;
	ret = m9x_faa_lock_view_admit_locked(mdev, drm_gem_vram_of_gem(gem),
			r->owner[1].base, m9x_faa_primary_window_bytes(&r->graph_layout));
	if (!ret)
		*out = m9x_faa_graph_binding_get(r->graph_binding);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

#ifdef M9_FAA_GRAPH_INIT_CONTROL
static struct m9_faa_depth_frame_storage *m9_faa_depth_frame_take_retired(
 struct m9x_faa_replay *r,u32 generation,u64 lock_point,u64 unlock_point)
{
 struct m9_faa_depth_storage *depth;
 struct m9_faa_depth_frame_storage *frame;
 if(!r||!r->color_lock_retired||!r->normal_lock_view_seen||
  !r->unlock_started||!r->unlock_retired||r->graph_generation!=generation||
  r->color_lock_point!=lock_point||r->unlock_point!=unlock_point||
  r->graph_draw_point!=unlock_point||lock_point>U64_MAX-0x20||
  unlock_point!=lock_point+0x20)return NULL;
 depth=r->depth_storage;
 if(!depth||!depth->ready||!depth->initialized||depth->generation!=generation)return NULL;
 frame=depth->frame;
 if(!frame||!frame->attempted||!frame->retired||frame->generation!=generation||
  frame->final_point!=lock_point)return NULL;
 depth->frame=NULL;
 return frame;
}
#endif

#ifdef M9_FAA_GRAPH_INIT_CONTROL
/* The caller frees *retired_frame after dropping TRANSPORT. */
static int m9_faa_graph_unlock_post_locked(struct m9x_device *mdev,
					   struct m9_faa_graph_binding *binding,
					   u32 generation, u64 lock_point,
					   u64 *point, bool *posted,
					   struct m9_faa_depth_frame_storage **retired_frame)
{
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	struct m9_faa_graph_binding *checked = NULL;
	struct m9_faa_owner *primary;
	struct m9x_fence *fence = NULL;
	u8 image[M9_FAA_UNLOCK_PRIMARY_BYTES];
	u32 old_bytes, target, i;
	u64 stage_point = 0;
	bool stage_posted = false;
	int ret;

	if (!binding || !point || !posted)
		return -EINVAL;
	*point = 0;
	*posted = false;
	*retired_frame = NULL;
	lockdep_assert_held(&t->lock);
	ret = m9x_faa_graph_unlock_get_locked(mdev, generation, lock_point,
					      &checked);
	if (ret)
		goto out;
	if (checked != binding) {
		ret = -ESTALE;
		goto out;
	}
	r = t->faa;
	primary = m9_faa_command_owner(r, false);
	old_bytes = r->graph_draw_end_bytes;
	if (!primary || !primary->cpu ||
	    !drm_mm_node_allocated(&primary->system_node) ||
	    primary->system_node.start != primary->base ||
	    old_bytes > primary->bytes ||
	    primary->bytes - old_bytes < sizeof(image)) {
		ret = -ESTALE;
		goto out;
	}
	ret = m9x_faa_graph_unlock_encode(r->owner[1].base, r->owner[1].bytes,
					  r->graph_layout.width,
					  r->graph_layout.height, lock_point,
					  image, sizeof(image));
	if (ret)
		goto out;
	target = lower_32_bits(lock_point) + 0x20;
	/* Cover all borrowed surfaces before any release MMIO or END. A late
	 * failure keeps both the fence and retained binding quarantined.

	 */
	fence = m9x_fence_create_target_locked(mdev, target);
	if (!fence) {
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++)
		dma_resv_add_fence(m9x_faa_graph_binding_owner(binding, i)->resv,
				   &fence->base, DMA_RESV_USAGE_WRITE);
	r->present_point = 0;
	r->present_retired = false;
	r->present_attempted = false;
	r->present_end = 0;
	r->present_posts = 0;
	r->unlock_started = true;
	r->unlock_retired = false;
	r->graph_second_lock_retired = false;
	r->second_lock_view_seen = false;
	/* Source2c878 releases the CPU frontend; retained PTEs stay intact. */
	m9x_write(mdev, M9_REG_FRONTEND_STATE, 0);
	for (i = 0; i < 3; i++) {
		u32 end = old_bytes + (i == 2 ? M9_FAA_UNLOCK_PRIMARY_BYTES :
						M9_FAA_UNLOCK_FIRST_POST_BYTES);

		if (i == 0)
			memcpy((u8 *)primary->cpu + old_bytes, image,
			       M9_FAA_UNLOCK_FIRST_POST_BYTES);
		else if (i == 2)
			memcpy((u8 *)primary->cpu + old_bytes +
				       M9_FAA_UNLOCK_FIRST_POST_BYTES,
			       image + M9_FAA_UNLOCK_FIRST_POST_BYTES,
			       M9_FAA_UNLOCK_PRIMARY_BYTES -
				       M9_FAA_UNLOCK_FIRST_POST_BYTES);
		wmb();
		ret = m9x_post_faa_graph_end_locked(mdev, &primary->system_node,
						    primary->dma,
						    r->graph_draw_end_bytes, end,
						    i == 1, i == 2 ? target : 0,
						    &stage_point, &stage_posted);
		*posted |= stage_posted;
		if (stage_posted) {
			r->graph_draw_end_bytes = end;
			r->graph_draw_posts++;
		}
		if (ret)
			goto out;
	}
	if (stage_point != lock_point + 0x20) {
		ret = -EIO;
		goto out;
	}
	*point = stage_point;
	r->graph_draw_point = stage_point;
	r->unlock_retired = true;
	r->unlock_point = stage_point;
	r->ordinary_point = 0;
	r->graph_unlock_end_bytes = r->graph_draw_end_bytes;
	r->graph_unlock_posts = r->graph_draw_posts;
	if (r->command_pools) {
		r->command_pools->unlock_point = 0;
		r->command_pools->maintenance_point = 0;
		r->command_pools->posts = 0;
	}
	*retired_frame = m9_faa_depth_frame_take_retired(r, generation,
							 lock_point, stage_point);
	dev_info(&mdev->pdev->dev,
		 "M9 FAA graph Unlock retired: lock=%#llx unlock=%#llx bytes=%u posts=%u copyback=F021->F022\n",
		 lock_point, *point, r->graph_draw_end_bytes,
		 r->graph_draw_posts);
out:
	m9x_transport_fence_finish_locked(mdev, fence, ret, *posted);
	m9x_faa_graph_binding_put(checked);
	return ret;
}

#endif

int m9x_faa_graph_unlock_submit(struct m9x_device *mdev,
				struct m9_faa_graph_binding *binding,
				u32 generation, u64 lock_point, u64 *point,
				bool *posted)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9_faa_depth_frame_storage *retired_frame = NULL;
	int ret;

	mutex_lock(&mdev->transport.lock);
	ret = m9_faa_graph_unlock_post_locked(mdev, binding, generation,
					      lock_point, point, posted,
					      &retired_frame);
	mutex_unlock(&mdev->transport.lock);
	m9_faa_depth_frame_free(retired_frame);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

int m9x_faa_graph_present_get_locked(struct m9x_device *mdev, u32 generation,
				     u64 lock_point,
				     struct m9_faa_graph_binding **out)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_faa_replay *r = mdev->transport.faa;
	struct drm_gem_object *gem;
	int ret;

	lockdep_assert_held(&mdev->transport.lock);
	if (!out)
		return -EINVAL;
	*out = NULL;
	if (!r || !r->graph_binding || r->graph_generation != generation ||
	    mdev->transport.generation != generation ||
	    !r->color_lock_retired || r->color_lock_point != lock_point ||
	    r->unlock_started)
		return -ESTALE;
	gem = m9x_faa_graph_binding_owner(r->graph_binding,
					  PARHELIA_M9_FAA_PRIMARY);
	if (!gem || !m9x_gem_is_vram(gem))
		return -ESTALE;
	ret = m9x_faa_lock_view_admit_locked(mdev, drm_gem_vram_of_gem(gem),
					     r->owner[1].base,
					     m9x_faa_primary_window_bytes(&r->graph_layout));
	if (!ret)
		*out = m9x_faa_graph_binding_get(r->graph_binding);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

/* A map-only normal Lock follows the same original mapper as SURFACE_ACCESS,
 * but publishes no user pixels. The source Lock itself has already retired.
 * The six owner reservations and destination reservation stay held throughout. */
int m9x_faa_graph_present_submit(struct m9x_device *mdev,
				 struct m9_faa_graph_binding *binding,
				 struct drm_gem_object *destination,
				 const struct m9x_faa_present_view *view,
				 u32 generation, u64 lock_point,
				 u64 *unlock_point, u64 *present_point,
				 bool *posted)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r = NULL;
	struct m9_faa_owner *primary;
	struct drm_gem_object *source;
	struct m9x_faa_present_view src;
	struct m9x_faa_present_image image;
	struct m9x_fence *fence = NULL;
	u64 stage = 0;
	u32 start, i;
	bool exposed = false, stage_posted = false;
	struct m9_faa_depth_frame_storage *retired_frame = NULL;
	int ret;

	if (!binding || !destination || !view || !unlock_point ||
	    !present_point || !posted)
		return -EINVAL;
	*unlock_point = 0;
	*present_point = 0;
	*posted = false;
	mutex_lock(&t->lock);
	r = t->faa;
	source = m9x_faa_graph_binding_owner(binding, PARHELIA_M9_FAA_PRIMARY);
	if (!r || r->graph_binding != binding || !source ||
	    !m9x_gem_is_vram(source) || !r->color_lock_retired ||
	    r->color_lock_point != lock_point || r->unlock_started ||
	    r->present_destination || r->present_fence ||
	    r->graph_generation != generation || t->generation != generation ||
	    lock_point > U64_MAX - 0x40) {
		ret = -ESTALE;
		goto out;
	}
	ret = m9x_faa_lock_view_admit_locked(mdev, drm_gem_vram_of_gem(source),
					     r->owner[1].base,
					     m9x_faa_primary_window_bytes(&r->graph_layout));
	if (ret)
		goto out;
	src = (struct m9x_faa_present_view){
		.gpu_base = r->owner[1].base,
		.bytes = r->owner[1].bytes,
		.width = r->graph_layout.width,
		.height = r->graph_layout.height,
		.span = r->graph_layout.primary.allocation_width,
		.descriptor0 = 0xf0220000u |
			       r->graph_layout.primary.allocation_width,
		.descriptor1 = 0x10000u | r->graph_layout.height
	};
	ret = m9x_faa_present_encode(&src, view, lock_point + 0x20, &image);
	if (ret)
		goto out;
	primary = m9_faa_command_owner(r, false);
	ret = m9_faa_command_owner_admit(mdev, primary);
	if (ret)
		goto out;
	start = r->graph_draw_end_bytes;
	if (start > primary->bytes ||
	    primary->bytes - start < 96 + sizeof(image.primary)) {
		ret = -ENOSPC;
		goto out;
	}
	ret = m9x_address_check_local_mapping(mdev, src.gpu_base, src.bytes);
	if (ret)
		goto out;
	fence = m9x_fence_create_target_locked(mdev,
					       lower_32_bits(image.final_point));
	if (!fence) {
		ret = -ENOMEM;
		goto out;
	}
	if (fence->base.seqno != image.final_point) {
		ret = -ESTALE;
		goto out;
	}
	/* Destination capacity for this fence and the graph's U/P fences was
	 * reserved before entry. Allocate every needed fence before mapper MMIO.
	 */
	dma_resv_add_fence(destination->resv, &fence->base,
			   DMA_RESV_USAGE_WRITE);
	/* Existing published metadata is preserved. A fresh ordinary allocation
	 * uses the same compact/secondary publication as its surface-access path.
	 */
	ret = m9x_address_check_local_mapping(mdev, view->gpu_base,
					      view->bytes);
	if (ret)
		ret = m9x_address_local_prepare(mdev, view->gpu_base,
						view->bytes);
	if (ret)
		goto out;
	m9x_write(mdev, M9_REG_FRONTEND_KICK, 1);
	ret = m9x_address_cpu_local_prepare(mdev, src.gpu_base, src.bytes);
	if (ret)
		goto out;
	ret = m9x_address_surface_window_bounded(mdev, src.gpu_base | 1,
						 0xf0210000u | src.span,
						 0x10000u |
						 r->graph_layout.primary.allocation_height,
						 m9x_faa_primary_window_bytes(&r->graph_layout));
	if (ret)
		goto out;
	m9x_address_surface_release(mdev);
	ret = m9x_faa_lock_view_observed_locked(mdev,
						drm_gem_vram_of_gem(source));
	if (ret)
		goto out;
	/* TRANSPORT stays held from Unlock to PRESENT: no ordinary job lands
	 * between them, so PRESENT keeps its Unlock+0x20 origin.
	 */
	ret = m9_faa_graph_unlock_post_locked(mdev, binding, generation,
					      lock_point, unlock_point,
					      &exposed, &retired_frame);
	*posted = exposed;
	if (ret)
		goto out;
	if (t->faa != r || t->generation != generation || t->wedged ||
	    !r->unlock_retired || *unlock_point != lock_point + 0x20 ||
	    r->unlock_point != *unlock_point ||
	    t->submitted_point != *unlock_point ||
	    t->completed_point != *unlock_point ||
	    t->next_fence != lower_32_bits(*unlock_point) + 0x10 ||
	    t->queue_count || t->async_pending) {
		ret = -ESTALE;
		goto out;
	}
	r->present_attempted = true;
	primary = m9_faa_command_owner(r, false);
	start = r->graph_draw_end_bytes;
	ret = m9_faa_command_owner_admit(mdev, primary);
	if (ret)
		goto out;
	ret = m9x_address_check_local_mapping(mdev, view->gpu_base,
					      view->bytes);
	if (ret)
		goto out;
	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++)
		dma_resv_add_fence(m9x_faa_graph_binding_owner(binding, i)->resv,
				   &fence->base,
				   i == PARHELIA_M9_FAA_PRIMARY ?
				   DMA_RESV_USAGE_READ : DMA_RESV_USAGE_WRITE);
	for (i = 0; i < 3; i++) {
		u32 end = start + image.end_bytes[i];

		if (i == 0)
			memcpy((u8 *)primary->cpu + start, image.primary, 88);
		if (i == 2)
			memcpy((u8 *)primary->cpu + start + 88,
			       image.primary + 88, 12);
		/* The WC primary words precede their END post. */
		wmb();
		ret = m9x_post_faa_graph_end_locked(mdev, &primary->system_node,
						    primary->dma,
						    r->graph_draw_end_bytes, end,
						    i == 1,
						    i == 2 ?
						    lower_32_bits(image.final_point) : 0,
						    &stage, &stage_posted);
		*posted |= stage_posted;
		if (stage_posted) {
			r->graph_draw_end_bytes = end;
			r->graph_draw_posts++;
		}
		if (ret)
			goto out;
	}
	if (stage != image.final_point ||
	    t->completed_point != image.final_point) {
		ret = -EIO;
		goto out;
	}
	r->present_point = stage;
	r->present_generation = generation;
	r->present_retired = true;
	r->present_end = r->graph_draw_end_bytes;
	r->present_posts = r->graph_draw_posts;
	*present_point = stage;
	dev_info(&mdev->pdev->dev,
		 "M9 FAA GPU present retired: Lock=%#llx Unlock=%#llx present=%#llx source=%#x destination=%#x\n",
		 lock_point, *unlock_point, stage, src.gpu_base, view->gpu_base);
	ret = 0;
out:
	if (fence && ret)
		dma_fence_set_error(&fence->base, ret);
	if (ret && *posted) {
		/* The ioctl transfers its destination pin; retain a separate lookup ref.
		 * Even a failed final wait cannot release an exposed address.
		 */
		drm_gem_object_get(destination);
		r->present_destination = destination;
		r->present_fence = fence;
		fence = NULL;
		if (!t->wedged) {
			t->wedged = true;
			t->generation++;
		}
	} else if (fence) {
		dma_fence_signal(&fence->base);
	}
	if (fence)
		dma_fence_put(&fence->base);
	mutex_unlock(&t->lock);
	m9_faa_depth_frame_free(retired_frame);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

/* Authenticate the first returned public-VS Unlock as a successor boundary.
 * This only retains the existing binding. It never resets first-draw flags,
 * ignores a reservation hazard, rewrites a cursor or posts GPU commands. */
int m9x_faa_graph_public_vs_unlocked_get_locked(struct m9x_device *mdev,
		u32 generation, u64 unlock_point, struct m9_faa_graph_binding **out)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r = t->faa;

	lockdep_assert_held(&t->lock);
	if (!out)
		return -EINVAL;
	*out = NULL;
	if (t->wedged)
		return -EIO;
	if (!r || !r->graph_binding || !r->public_vs_payload_ready ||
	    !r->public_vs_image || !r->public_vs_draw_image ||
	    !r->public_vs_lock_image || !r->public_vs_head_retired ||
	    !r->public_vs_draw_observed || !r->public_vs_lock_retired ||
	    !r->public_vs_lock_attempted || r->public_vs_prefix_retired ||
	    r->public_vs_follow_retired || r->draw_image ||
	    r->public_vs_head_fence || r->public_vs_draw_fence ||
	    !r->normal_lock_view_seen || !r->unlock_started || !r->unlock_retired ||
	    r->graph_generation != generation || t->generation != generation ||
	    r->observation_generation != generation || !r->observation_retired ||
	    !r->observation_point || r->observation_point > U64_MAX - 0x100 ||
	    !unlock_point || (unlock_point & 15) ||
	    unlock_point != r->observation_point + 0x100 ||
	    r->unlock_point != unlock_point || r->graph_draw_point != unlock_point ||
	    r->graph_draw_posts != 9 || r->graph_unlock_posts != 9 ||
	    r->graph_draw_end_bytes != r->graph_unlock_end_bytes ||
	    (u64)r->graph_draw_end_bytes != (u64)r->primary_bytes + 124 +
		M9_FAA_VS_LOCK_PRIMARY_BYTES + M9_FAA_UNLOCK_PRIMARY_BYTES ||
	    t->submitted_point != unlock_point || t->completed_point != unlock_point ||
	    lower_32_bits(unlock_point) > U32_MAX - 0x10 ||
	    t->next_fence != lower_32_bits(unlock_point) + 0x10)
		return -ESTALE;
	if (!t->faa_source_context || !t->faa_virtual_primary ||
	    !t->engine_transitioned || !t->restore_ready || !t->fence_cpu ||
	    !t->transport_armed || !t->bus_master_enabled || t->async_stop ||
	    t->async_pending || t->queue_count)
		return -EBUSY;
	*out = m9x_faa_graph_binding_get(r->graph_binding);
	return 0;
#else
	return -EOPNOTSUPP;
#endif
}

#ifdef M9_FAA_GRAPH_INIT_CONTROL
/* Passive diagnostic after the independent dual Lock receipt.  The bound
 * color allocation is a retained WC SYSTEM owner, distinct from the sampled
 * F021 owner.  Its raw dwords are not assumed to be linear RGBA pixels. */
static void m9_faa_dual_bound_color_observe(struct m9x_device *mdev,
					  struct m9x_faa_replay *r)
{
	static const u32 probe_xy[][2] = {
		{0, 0}, {80, 180}, {160, 120}, {240, 180},
	};
	struct m9_faa_depth_storage *storage = r->depth_storage;
	struct m9_faa_owner *owner;
	const struct m9x_faa_depth_view *view;
	const u8 *cpu;
	u32 i, nonzero = 0, hash = 2166136261u, sample[4];

	if (!storage || !storage->ready ||
	    r->graph_layout.width != 320 || r->graph_layout.height != 240)
		return;
	owner = &storage->owner[M9_FAA_BOUND_COLOR];
	view = &storage->views.owner[M9_FAA_BOUND_COLOR];
	if (!owner->cpu || !owner->cpu_wc || owner->vram ||
	    !drm_mm_node_allocated(&owner->system_node) ||
	    owner->system_node.start != owner->base ||
	    view->base != owner->base || view->logical_bytes != 0x50000 ||
	    view->capacity_bytes > owner->bytes ||
	    owner->bytes < view->logical_bytes ||
	    view->storage_width != 320)
		return;

	/* The GPU retired before this observer.  Report the complete raw view
	 * plus a few word positions under the source storage-width hypothesis;
	 * neither statistic by itself proves framebuffer pixel correctness. */
	dma_rmb();
	cpu = owner->cpu;
	for (i = 0; i < view->logical_bytes; i += 4) {
		u32 word = get_unaligned_le32(cpu + i);

		nonzero += word != 0;
		hash = (hash ^ word) * 16777619u;
	}
	for (i = 0; i < ARRAY_SIZE(probe_xy); i++) {
		u32 offset = (probe_xy[i][1] * view->storage_width +
			      probe_xy[i][0]) * 4;

		sample[i] = get_unaligned_le32(cpu + offset);
	}
	dev_info(&mdev->pdev->dev,
		 "M9 FAA dual bound color raw: point=%#llx base=%#x bytes=%u nonzero_dwords=%u wordhash32=%08x\n",
		 (unsigned long long)r->color_lock_point, owner->base, view->logical_bytes,
		 nonzero, hash);
	dev_info(&mdev->pdev->dev,
		 "M9 FAA dual bound color raw words: origin=%08x edge0=%08x center=%08x edge1=%08x; storage-width offsets only\n",
		 sample[0], sample[1], sample[2], sample[3]);
}

/* Source graph roles 1/2/3 are sampled primary, MAIN and AUX. They are
 * borrowed GEM objects: r->owner[] records only their GPU address/capacity,
 * never a local CPU map. The graph binding retains the objects across this
 * actual Lock, and the ioctl holds their reservations here. MAIN/AUX may
 * contain initialized metadata; nonzero raw words are not render coverage. */
static void m9_faa_dual_graph_raw_observe(struct m9x_device *mdev,
					  struct m9x_faa_replay *r,
					  u32 role, const char *name, u32 logical_bytes)
{
	struct m9_faa_owner *owner;
	struct drm_gem_object *gem;
	struct drm_gem_vram_object *bo;
	struct iosys_map map = IOSYS_MAP_INIT_VADDR(NULL);
	const char *refusal = "admission";
	bool engine;
	u8 data[256];
	u32 offset, i, chunk, address, source, nonzero = 0;
	u32 hash = 2166136261u;
	s64 local_offset;
	u64 base;
	int ret;

	lockdep_assert_held(&mdev->transport.lock);
	/* The pre-DRAW probe runs after INIT retirement, before color_lock_point
	 * is assigned. Log the transport's actual retired point for both probes. */
	if (!mdev->transport.completed_point ||
	    mdev->transport.completed_point !=
		mdev->transport.submitted_point) {
		refusal = "retirement";
		goto refused;
	}
	if (role < 1 || role > 3 || !logical_bytes ||
	    logical_bytes != m9_faa_owner_specs[role].bytes ||
	    logical_bytes % 4 || !r->graph_binding ||
	    r->graph_layout.width != 320 || r->graph_layout.height != 240)
		goto refused;
	owner = &r->owner[role];
	gem = m9x_faa_graph_binding_owner(r->graph_binding, role - 1);
	if (!gem || !m9x_faa_graph_binding_address(r->graph_binding,
							 role - 1, &address, &source) ||
	    source != role || address != owner->base ||
	    owner->vram || owner->cpu ||
	    r->graph_layout.owner[role - 1].logical != logical_bytes ||
	    r->graph_layout.owner[role - 1].capacity != owner->bytes) {
		refusal = "binding";
		goto refused;
	}
	dma_resv_assert_held(gem->resv);
	engine = role != 1;
	if ((engine && !m9x_gem_is_engine_vram(gem)) ||
	    (!engine && (!m9x_gem_is_vram(gem) ||
			 !m9x_gem_vram_tiled(gem)))) {
		refusal = "class";
		goto refused;
	}
	local_offset = m9x_gem_vram_offset(gem);
	if (local_offset < 0 ||
	    r->graph_layout.owner[role - 1].capacity > gem->size ||
	    (u64)local_offset > m9x_vram_gpu_limit(mdev) ||
	    r->graph_layout.owner[role - 1].capacity >
		m9x_vram_gpu_limit(mdev) - (u64)local_offset ||
	    check_add_overflow(M9_VRAM_GPU_BASE, (u64)local_offset, &base) ||
	    base != owner->base) {
		refusal = "placement";
		goto refused;
	}
	if (engine) {
		/* MAIN/AUX are engine-only BOs with no GEM vmap. The existing
		 * slot-1 identity aperture is the CPU's read-only route to them.
		 * This observer holds transport.lock, as required by the slot API,
		 * and restores the empty slot before any later command can use it. */
		if (!mdev->bar0_window || logical_bytes > M9_ADDRESS_SURFACE_BYTES ||
		    (u64)local_offset > U32_MAX) {
			refusal = "aperture";
			goto refused;
		}
		m9x_address_raw_window(mdev, (u32)local_offset);
	} else {
		bo = drm_gem_vram_of_gem(gem);
		ret = drm_gem_vram_vmap(bo, &map);
		if (ret) {
			refusal = "vmap";
			goto refused;
		}
	}
	dma_rmb();
	for (offset = 0; offset < logical_bytes; offset += sizeof(data)) {
		chunk = min_t(u32, sizeof(data), logical_bytes - offset);
		if (engine)
			memcpy_fromio(data, (u8 __iomem *)mdev->bar0_window + offset,
				      chunk);
		else
			iosys_map_memcpy_from(data, &map, offset, chunk);
		for (i = 0; i < chunk; i += 4) {
			u32 word = get_unaligned_le32(data + i);

			nonzero += word != 0;
			hash = (hash ^ word) * 16777619u;
		}
	}
	if (engine) {
		memcpy_fromio(data, mdev->bar0_window, 64);
		m9x_address_surface_release(mdev);
	} else {
		iosys_map_memcpy_from(data, &map, 0, 64);
		drm_gem_vram_vunmap(bo, &map);
	}
	dev_info(&mdev->pdev->dev,
		 "M9 FAA dual graph raw %s owner%u: point=%#llx base=%#x bytes=%u nonzero_dwords=%u wordhash32=%08x first64=%*phN; retired physical VRAM only, preexisting metadata possible, no coverage or pixel interpretation\n",
		 name, role,
		 (unsigned long long)mdev->transport.completed_point,
		 owner->base, logical_bytes, nonzero, hash, 64, data);
	return;
refused:
	dev_info(&mdev->pdev->dev,
		 "M9 FAA dual graph raw %s owner%u refused: reason=%s; no raw data observed\n",
		 name, role, refusal);
}

/* These are the actual emitted B0/B2/B8 target pairs. Resolve their tagged
 * local addresses back to the retained owner; never publish a new CPU view.
 * The declaration splice shifts every stage after the carry declaration. */
static void m9_faa_color_intermediates_observe(struct m9x_device *mdev,
					    struct m9x_faa_replay *r)
{
	struct observed_target { u32 at, next_at, owner, bytes; };
	static const struct observed_target four_target[] = {
		{ 116, 120, 24, 0 }, { 132, 136, 24, 0 },
		{ 804, 896, 23, 64 }, { 820, 912, 23, 64 },
		{ 1416, 1372, 23, 64 }, { 1432, 1388, 23, 64 },
		{ 1448, 0, 23, 16 }, /* first-only third position target */
	};
	static const struct observed_target two_target[] = {
		{ 72, 0, 23, 64 }, { 88, 0, 23, 64 },
		{ 104, 0, 23, 64 }, { 120, 0, 23, 16 },
	};
	const struct observed_target *target;
	struct m9_faa_color_storage *n = r->color_image;
	u8 data[256];
	static const u32 boundary[] = { 31, 32, 59, 95, 96, 119, 179, 359 };
	const u8 *completed_secondary;
	u32 completed_bytes, records = 0, target_count, stages;
	bool two;
	u32 i, at, address, descriptor, offset, bytes, stride, chunk, count, j, launches = 0;
	int extra;

	lockdep_assert_held(&mdev->transport.lock);
	if (!m9_faa_color_observe || !r->color_lock_retired || !n ||
	    (n->layout.program_count != 2 && n->layout.program_count != 4) ||
	    (n->layout.program_count == 2 ?
	     n->secondary_bytes < 1352 :
	     n->secondary_bytes < (n->previous_unlock_point ? 2512 : 2688)) ||
	    n->secondary_bytes > sizeof(n->secondary) ||
	    mdev->transport.wedged || mdev->transport.queue_count ||
	    mdev->transport.completed_point != r->color_lock_point ||
	    mdev->transport.generation != r->graph_generation)
		return;
	two = n->layout.program_count == 2;
	stages = two ? 2 : 4;
	target = two ? two_target : four_target;
	target_count = two ? ARRAY_SIZE(two_target) : ARRAY_SIZE(four_target);
	/* The retained color image describes the first job's target namespace.
	 * The last composed launch supplies the actual output record count;
	 * different jobs can carry different counts into the same planes. */
	completed_secondary = n->secondary;
	completed_bytes = n->secondary_bytes;
	if (r->depth_storage && r->depth_storage->frame) {
		struct m9_faa_depth_frame_storage *frame = r->depth_storage->frame;

		if (!frame->retired || frame->final_point != r->color_lock_point)
			return;
		completed_secondary = frame->batch_secondary ?
			frame->batch_secondary : frame->secondary;
		completed_bytes = frame->secondary_bytes;
		if (!completed_bytes || completed_bytes > (frame->batch_secondary ?
			0x40000 : sizeof(frame->secondary)))
			return;
		if (frame->dual_first) {
			/* The ordinary COLOR4 scratch image is not the published dual
			 * packet. Decode only the retained dual frame after its exact Lock
			 * receipt, using its own B0/B2/B4/B6, gather and position binds. */
			static const struct observed_target dual_target[] = {
				{116, 120, 24, 256}, {132, 136, 24, 0},
				{148, 152, 24, 0}, {164, 168, 24, 0},
				{844, 848, 23, 64}, {860, 864, 23, 64},
				{876, 880, 23, 64},
				{1488, 1492, 23, 64}, {1504, 1508, 23, 64},
				{1520, 1524, 23, 64},
			};

			rmb();
			dma_rmb();

			for (i = 0; i < ARRAY_SIZE(dual_target); i++) {
				const struct observed_target *t = &dual_target[i];
				struct m9_faa_owner *o = &r->owner[t->owner];
				u32 available, nbytes;

				if (t->at + 8 > completed_bytes || !o->vram || o->cpu ||
				    iosys_map_is_null(&o->map))
					continue;
				address = get_unaligned_le32(completed_secondary + t->at);
				descriptor = get_unaligned_le32(completed_secondary + t->at + 4);
				if ((address & 3) != 1 || (address & ~3u) < o->base)
					continue;
				offset = (address & ~3u) - o->base;
				if (offset > o->bytes)
					continue;
				available = o->bytes - offset;
				nbytes = min_t(u32, t->bytes, min_t(u32, available, sizeof(data)));
				if (nbytes)
					iosys_map_memcpy_from(data, &o->map, offset, nbytes);
				dev_info(&mdev->pdev->dev,
					 "M9 FAA dual intermediate: point=%#llx owner=%u secondary=%u offset=%#x descriptor=%#x bytes=%u\n",
					 r->color_lock_point, t->owner, t->at, offset,
					 descriptor, nbytes);
				for (chunk = 0; chunk < nbytes; chunk += count) {
					count = min_t(u32, 64, nbytes - chunk);
					dev_info(&mdev->pdev->dev,
						 "M9 FAA dual data: secondary=%u offset=%#x hex=%*phN\n",
						 t->at, offset + chunk, count, data + chunk);
				}
			}
			m9_faa_dual_bound_color_observe(mdev, r);
			m9_faa_dual_graph_raw_observe(mdev, r, 1, "sampled",
						 m9_faa_owner_specs[1].bytes);
			m9_faa_dual_graph_raw_observe(mdev, r, 2, "MAIN",
						 m9_faa_owner_specs[2].bytes);
			m9_faa_dual_graph_raw_observe(mdev, r, 3, "AUX",
						 m9_faa_owner_specs[3].bytes);
			return;
		}
	}
	if (completed_bytes >= 12 && completed_bytes <= 0x40000 &&
	    get_unaligned_le32(completed_secondary + completed_bytes - 12) ==
		0x23000001) {
		records = get_unaligned_le32(completed_secondary + completed_bytes - 4);
		if (!records || records > 12288 || records % 3)
			records = 0;
	}
	/* Decode the already composed launch controls and owner28 raw binding.
	 * These are observations of live packets, never packet initializers. */
	for (at = 0; at + 12 <= completed_bytes; at += 4) {
		u32 opcode = get_unaligned_le32(completed_secondary + at);

		if (opcode == 0x23000001 && launches < 32 * stages &&
		    !get_unaligned_le32(completed_secondary + at + 4)) {
			count = get_unaligned_le32(completed_secondary + at + 8);
			if (count && count <= 12288) {
				dev_info(&mdev->pdev->dev,
					 "M9 FAA COLOR launch: point=%#llx job=%u stage=%u secondary=%u count=%u\n",
					 r->color_lock_point, launches / stages + 1,
					 launches % stages, at, count);
				launches++;
			}
		}
		if (opcode == 0x45100100 && at + 24 <= completed_bytes &&
		    get_unaligned_le32(completed_secondary + at + 16) == 3 &&
		    get_unaligned_le32(completed_secondary + at + 20) == 0x458fc3c2) {
			struct m9_faa_owner *data_owner = &r->draw_owner[1];

			address = get_unaligned_le32(completed_secondary + at + 4);
			stride = get_unaligned_le32(completed_secondary + at + 12);
			if ((address & 3) != 1 || !data_owner->cpu ||
			    (address & ~3u) < data_owner->base || (stride != 16 && stride != 32))
				continue;
			offset = (address & ~3u) - data_owner->base;
			if (offset > data_owner->bytes || stride > data_owner->bytes - offset)
				continue;
			dev_info(&mdev->pdev->dev,
				 "M9 FAA COLOR raw binding: point=%#llx secondary=%u owner=28 address=%#x offset=%#x descriptor=%#x stride=%u\n",
				 r->color_lock_point, at, address, offset,
				 get_unaligned_le32(completed_secondary + at + 8), stride);
		}
	}
	extra = (int)n->secondary_bytes - (n->previous_unlock_point ? 2536 : 2712);
	rmb();
	dma_rmb();
	if (two)
		dev_info(&mdev->pdev->dev,
			 "M9 FAA COLOR observation: vectors=2 linear planes from live B0/B2/B4/B8; no packed owner24 read\n");
	for (i = 0; i < target_count; i++) {
		struct m9_faa_owner *o = &r->owner[target[i].owner];

		if (two) {
			at = target[i].at;
		} else if (n->previous_unlock_point) {
			at = target[i].next_at;
			if (!at) continue;
			if (at >= 680) at += extra;
		} else {
			at = target[i].at < 576 ? target[i].at : target[i].at + extra;
		}
		if (at > n->secondary_bytes - 8)
			continue;
		address = get_unaligned_le32(n->secondary + at);
		descriptor = get_unaligned_le32(n->secondary + at + 4);
		if (two && descriptor != (target[i].bytes == 16 ?
			0xb8002000u : 0x68002000u))
			continue;
		if ((address & 3) != 1 || !o->vram || o->cpu ||
		    iosys_map_is_null(&o->map) || (address & ~3u) < o->base)
			continue;
		offset = (address & ~3u) - o->base;
		bytes = target[i].bytes;
		if (!bytes) {
			/* Packed SLOT records: sample four complete records, including
			 * all active fields, rather than misreading them as linear vec4. */
			if ((descriptor & 0xffff0000) != 0xee190000)
				continue;
			stride = descriptor & 255;
			if (!stride || stride > 16)
				continue;
			bytes = 4 * stride * sizeof(u32);
		}
		if (offset > o->bytes || bytes > o->bytes - offset ||
		    bytes > sizeof(data))
			continue;
		iosys_map_memcpy_from(data, &o->map, offset, bytes);
		/* Kernel %ph logging caps each byte string at64 bytes. */
		for (chunk = 0; chunk < bytes; chunk += count) {
			count = min_t(u32, 64, bytes - chunk);
			dev_info(&mdev->pdev->dev,
				 "M9 FAA COLOR intermediate: point=%#llx owner=%u base=%#x offset=%#x secondary=%u descriptor=%#x total=%u bytes=%u hex=%*phN\n",
				 r->color_lock_point, target[i].owner, o->base,
				 offset + chunk, at, descriptor, bytes, count,
				 count, data + chunk);
		}
		/* Linear owner23 planes have one vec4 or clip-code DWORD per
		 * record. Packed owner24 SLOT data has a different organization;
		 * its existing first-four sample must not be indexed as vec4. */
		if (target[i].owner != 23 || !records)
			continue;
		stride = target[i].bytes / 4;
		for (j = 0; j < ARRAY_SIZE(boundary); j++) {
			u64 sample_offset = (u64)offset + (u64)boundary[j] * stride;

			if (boundary[j] >= records || sample_offset > o->bytes ||
			    stride > o->bytes - sample_offset)
				continue;
			iosys_map_memcpy_from(data, &o->map, sample_offset, stride);
			dev_info(&mdev->pdev->dev,
				 "M9 FAA COLOR boundary: point=%#llx owner=23 base=%#x offset=%#x secondary=%u descriptor=%#x record=%u records=%u launch_bytes=%u bytes=%u hex=%*phN\n",
				 r->color_lock_point, o->base, (u32)sample_offset,
				 at, descriptor, boundary[j], records,
				 completed_bytes, stride, stride, data);
		}
	}
}
#endif

int m9x_faa_graph_color_submit(struct m9x_device *mdev,
			       struct m9_faa_graph_binding *binding,
			       u32 generation, u64 init_point, u64 *point,
			       bool *posted)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	struct m9_faa_color_storage *n;
	struct m9_faa_owner *primary, *program;
	struct m9x_fence *fence = NULL;
	u8 observed[4096];
	bool stage_posted = false;
	u64 stage_point = 0;
	u32 i, end;
	int ret = -ESTALE;

	if (!point || !posted || !binding)
		return -EINVAL;
	*point = 0;
	*posted = false;
	mutex_lock(&t->lock);
	r = t->faa;
	if (!r || r->graph_binding != binding || !r->color_payload_ready ||
	    !r->color_image || r->color_attempted ||
	    r->graph_generation != generation || t->generation != generation ||
	    r->observation_generation != generation ||
	    !r->observation_retired || r->observation_point != init_point ||
	    !r->graph_draw_claimed || r->graph_draw_posts ||
	    r->graph_draw_point ||
	    r->graph_draw_end_bytes != r->primary_bytes ||
	    !t->faa_source_context || !t->faa_virtual_primary ||
	    !t->engine_transitioned || !t->restore_ready ||
	    !t->transport_armed || !t->bus_master_enabled || !t->fence_cpu ||
	    t->async_stop || t->async_pending || t->queue_count || t->wedged ||
	    t->submitted_point != init_point ||
	    t->completed_point != init_point ||
	    t->next_fence != lower_32_bits(init_point) + 0x10)
		goto out;
	n = r->color_image;
	primary = &r->owner[M9_FAA_PRIMARY_OWNER];
	program = &r->draw_owner[0];
	if (!primary->cpu || n->program_bytes > sizeof(observed) ||
	    !program->vram || r->primary_bytes > primary->bytes ||
	    primary->bytes - r->primary_bytes < n->primary_length) {
		ret = -ERANGE;
		goto out;
	}
	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
		struct drm_gem_object *bo =
			m9x_faa_graph_binding_owner(binding, i);

		dma_resv_assert_held(bo->resv);
		if (!dma_resv_test_signaled(bo->resv, DMA_RESV_USAGE_READ)) {
			ret = -EBUSY;
			goto out;
		}
	}
	for (i = 0; i < 3; i++) {
		struct m9_faa_owner *o = i ? &r->draw_owner[i] : primary;

		if (!o->cpu || !drm_mm_node_allocated(&o->system_node) ||
		    o->system_node.start != o->base) {
			ret = -ESTALE;
			goto out;
		}
		ret = m9x_system_pages_mapped(mdev, o->base, o->dma,
					      o->bytes / PAGE_SIZE, 1u, NULL,
					      NULL);
		if (ret)
			goto out;
	}
	ret = m9x_address_check_local_identity(mdev, program->base,
					       program->bytes);
	if (ret)
		goto out;
	ret = m9x_triangle_system_page_admit_locked(mdev, 0x40004000);
	if (ret)
		goto out;
	ret = m9x_triangle_system_page_admit_locked(mdev, 0x40008000);
	if (ret)
		goto out;
	fence = m9x_fence_create_target_locked(mdev, lower_32_bits(init_point) +
							     n->lock_delta);
	if (!fence) {
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++)
		dma_resv_add_fence(m9x_faa_graph_binding_owner(binding, i)->resv,
				   &fence->base, DMA_RESV_USAGE_WRITE);
	r->color_primary_start = r->graph_draw_end_bytes;
	r->color_posts_start = r->graph_draw_posts;
	r->color_attempted = true;
	m9x_write(mdev, M9_REG_FRONTEND_KICK, 1);
	ret = m9x_address_cpu_local_prepare(mdev, program->base,
					    program->bytes);
	if (ret)
		goto out;
	iosys_map_memcpy_to(&program->map, 0, n->program, n->program_bytes);
	/* The CPU stores precede their END post. */
	wmb();
	m9x_write(mdev, M9_REG_FRONTEND_STATE, 0);
	m9x_read(mdev, M9_REG_FRONTEND_STATE);
	iosys_map_memcpy_from(observed, &program->map, 0, n->program_bytes);
	if (memcmp(observed, n->program, n->program_bytes)) {
		ret = -EIO;
		goto out;
	}
	memcpy(r->draw_owner[1].cpu, n->constants, n->constants_bytes);
	memcpy((u8 *)r->draw_owner[1].cpu + n->geometry_offset,
	       n->constants + n->geometry_offset, n->geometry_bytes);
	memcpy(r->draw_owner[2].cpu, n->secondary, n->secondary_bytes);
	for (i = 0; i < 4; i++) {
		u32 old = r->graph_draw_end_bytes;

		if (!i) {
			end = r->primary_bytes + n->draw_length;
			memcpy((u8 *)primary->cpu + r->primary_bytes,
			       n->primary, n->draw_length);
		} else if (i < 3) {
			end = r->primary_bytes + n->primary_length - 12;
			if (i == 1)
				memcpy((u8 *)primary->cpu + r->primary_bytes +
					       n->draw_length,
				       n->primary + n->draw_length,
				       n->primary_length - 12 - n->draw_length);
		} else {
			end = r->primary_bytes + n->primary_length;
			memcpy((u8 *)primary->cpu + r->primary_bytes +
				       n->primary_length - 12,
			       n->primary + n->primary_length - 12, 12);
		}
		/* The CPU stores precede their END post. */
		dma_wmb();
		ret = m9x_post_faa_graph_end_locked(mdev, &primary->system_node,
						    primary->dma, old, end,
						    i == 2,
						    i == 3 ?
						    lower_32_bits(init_point) +
						    n->lock_delta : 0,
						    &stage_point, &stage_posted);
		*posted |= stage_posted;
		if (stage_posted) {
			r->graph_draw_end_bytes = end;
			r->graph_draw_posts++;
		}
		if (ret)
			goto out;
	}
	if (stage_point != init_point + n->lock_delta) {
		ret = -EIO;
		goto out;
	}
	*point = stage_point;
	r->graph_draw_point = stage_point;
	r->color_lock_point = stage_point;
	r->color_lock_retired = true;
	r->color_secondary_cursor = n->secondary_bytes;
	m9_faa_color_intermediates_observe(mdev, r);
	dev_info(&mdev->pdev->dev,
		 "M9 FAA color Lock retired: init=%#llx lock=%#llx bytes=%u posts=%u program=%u secondary=%u; %u live vectors\n",
		 init_point, *point, r->graph_draw_end_bytes,
		 r->graph_draw_posts, n->program_bytes, n->secondary_bytes,
		 n->layout.program_count);
out:
	if (ret)
		*point = 0;
	m9x_transport_fence_finish_locked(mdev, fence, ret, *posted);
	mutex_unlock(&t->lock);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

int m9x_faa_graph_public_vs_next_prepare(struct m9x_device *mdev,
 struct m9_faa_graph_binding *binding,u32 generation,u64 unlock_point,const u32 vertices[24])
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 struct m9x_transport *t=&mdev->transport;struct m9x_faa_replay *r;
 struct m9_faa_public_vs_next_storage *n;
 struct m9_faa_graph_binding *checked=NULL;
 struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS];
 struct m9x_faa_public_vs_next_image out;
 const struct firmware *fw=NULL,*lock_fw=NULL;
 u32 i;int ret=m9x_faa_public_vs_next_vertices_valid(vertices);
 if(ret)return ret;
 ret=request_firmware(&fw,M9_FAA_GRAPH_PUBLIC_VS_NEXT_FIRMWARE,&mdev->pdev->dev);
 if(ret)return ret;
 ret=request_firmware(&lock_fw,M9_FAA_GRAPH_PUBLIC_VS_LOCK_FIRMWARE,&mdev->pdev->dev);
 if(ret)goto release;
 n=kzalloc(sizeof(*n),GFP_KERNEL);if(!n){ret=-ENOMEM;goto release;}
 mutex_lock(&t->lock);
 ret=m9x_faa_graph_public_vs_unlocked_get_locked(mdev,generation,unlock_point,&checked);
 if(ret)goto out;
 if(checked!=binding){ret=-ESTALE;goto out;}
 r=t->faa;
 if(r->public_vs_next_image||r->public_vs_next_attempted){ret=-ESTALE;goto out;}
 for(i=0;i<M9_FAA_DRAW_SOURCE_OWNERS;i++){
  struct m9_faa_owner *o;
  switch(i){case 26:o=&r->draw_owner[0];break;case 27:o=&r->owner[26];break;
   case 28:o=&r->draw_owner[1];break;case 29:o=&r->draw_owner[2];break;
   case 30:o=&r->owner[27];break;default:o=&r->owner[i];break;}
  owner[i]=(struct m9x_faa_graph_draw_owner){o->base,o->bytes};
 }
 n->primary_start=r->graph_draw_end_bytes;n->draw_end=n->primary_start+60;
 n->lock_end=n->draw_end+M9_FAA_VS_LOCK_PRIMARY_BYTES;n->unlock_point=unlock_point;
 out=(struct m9x_faa_public_vs_next_image){n->primary,n->secondary,sizeof(n->primary),sizeof(n->secondary)};
 ret=m9x_faa_public_vs_next_encode(fw->data,fw->size,owner,n->primary_start,2688,512,unlock_point,
  r->public_vs_image->layout.constant_begin[0],r->public_vs_image->layout.constant_end[0],&out);
 if(ret)goto out;
 ret=m9x_faa_public_vs_lock_encode(lock_fw->data,lock_fw->size,owner,n->draw_end,
  320,240,unlock_point+0xa0,n->lock_primary,sizeof(n->lock_primary));
 if(ret)goto out;
 memcpy(n->vertices,vertices,sizeof(n->vertices));r->public_vs_next_image=n;n=NULL;
out:
 mutex_unlock(&t->lock);m9x_faa_graph_binding_put(checked);kfree(n);
release:
 if(lock_fw)release_firmware(lock_fw);if(fw)release_firmware(fw);return ret;
#else
 return -EOPNOTSUPP;
#endif
}

int m9x_faa_graph_public_vs_next_submit(struct m9x_device *mdev,
 struct m9_faa_graph_binding *binding,u32 generation,u64 unlock_point,u64 *point,bool *posted)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 struct m9x_transport *t=&mdev->transport;struct m9x_faa_replay *r;
 struct m9_faa_graph_binding *checked=NULL;struct m9_faa_public_vs_next_storage *n;
 struct m9_faa_owner *primary;struct m9x_fence *fence=NULL;
	bool stage_posted = false;
	u64 stage_point = 0;
	u32 i, end;
	int ret;
 if(!point||!posted||!binding)return -EINVAL;*point=0;*posted=false;
 mutex_lock(&t->lock);
 ret=m9x_faa_graph_public_vs_unlocked_get_locked(mdev,generation,unlock_point,&checked);
 if(ret)goto out;
 if(checked!=binding){ret=-ESTALE;goto out;}
 r=t->faa;n=r->public_vs_next_image;primary=&r->owner[M9_FAA_PRIMARY_OWNER];
 if(!n||r->public_vs_next_attempted||n->unlock_point!=unlock_point||n->primary_start!=r->graph_draw_end_bytes||
  !primary->cpu||!drm_mm_node_allocated(&primary->system_node)||primary->system_node.start!=primary->base||
  n->lock_end>primary->bytes||r->draw_owner[1].bytes<640||r->draw_owner[2].bytes<5236){ret=-ESTALE;goto out;}
 for(i=0;i<PARHELIA_M9_FAA_GRAPH_OWNERS;i++){
  struct drm_gem_object *bo=m9x_faa_graph_binding_owner(binding,i);
  dma_resv_assert_held(bo->resv);
  if(!dma_resv_test_signaled(bo->resv,DMA_RESV_USAGE_READ)){ret=-EBUSY;goto out;}
 }
 for(i=0;i<3;i++){
  struct m9_faa_owner *o=i==0?primary:&r->draw_owner[i];
  if(!o->cpu||!drm_mm_node_allocated(&o->system_node)||o->system_node.start!=o->base){ret=-ESTALE;goto out;}
		ret = m9x_system_pages_mapped(mdev, o->base, o->dma,
					      o->bytes / PAGE_SIZE, 1u, NULL, NULL);
		if (ret)
			goto out;
 }
 ret=m9x_address_check_local_identity(mdev,r->draw_owner[0].base,r->draw_owner[0].bytes);if(ret)goto out;
 ret=m9x_triangle_system_page_admit_locked(mdev,0x40004000);if(ret)goto out;
 ret=m9x_triangle_system_page_admit_locked(mdev,0x40008000);if(ret)goto out;
 fence=m9x_fence_create_target_locked(mdev,lower_32_bits(unlock_point)+0xd0);
 if(!fence){ret=-ENOMEM;goto out;}
 for(i=0;i<PARHELIA_M9_FAA_GRAPH_OWNERS;i++)
  dma_resv_add_fence(m9x_faa_graph_binding_owner(binding,i)->resv,&fence->base,DMA_RESV_USAGE_WRITE);
 r->public_vs_next_attempted=true;r->public_vs_next_view_seen=false;
 memcpy((u8*)r->draw_owner[1].cpu+512,n->vertices,sizeof(n->vertices));
 memcpy((u8*)r->draw_owner[2].cpu+2688,n->secondary,sizeof(n->secondary));
 for(i=0;i<4;i++){
  if(!i){end=n->draw_end;memcpy((u8*)primary->cpu+n->primary_start,n->primary,60);}
  else if(i<3){end=n->draw_end+M9_FAA_VS_LOCK_FIRST_POST_BYTES;
   if(i==1)memcpy((u8*)primary->cpu+n->draw_end,n->lock_primary,M9_FAA_VS_LOCK_FIRST_POST_BYTES);}
  else{end=n->lock_end;memcpy((u8*)primary->cpu+n->draw_end+M9_FAA_VS_LOCK_FIRST_POST_BYTES,
   n->lock_primary+M9_FAA_VS_LOCK_FIRST_POST_BYTES,M9_FAA_VS_LOCK_PRIMARY_BYTES-M9_FAA_VS_LOCK_FIRST_POST_BYTES);}
  dma_wmb();
  ret=m9x_post_faa_graph_end_locked(mdev,&primary->system_node,primary->dma,r->graph_draw_end_bytes,
   end,i==2,i==3?lower_32_bits(unlock_point)+0xd0:0,&stage_point,&stage_posted);
  *posted|=stage_posted;
  if(stage_posted){r->graph_draw_end_bytes=end;r->graph_draw_posts++;}
  if(ret)goto out;
 }
 if(stage_point!=unlock_point+0xd0){ret=-EIO;goto out;}
 *point=stage_point;r->graph_draw_point=stage_point;r->public_vs_next_lock_retired=true;
 dev_info(&mdev->pdev->dev,"M9 FAA public VS next Lock retired: unlock=%#llx lock=%#llx bytes=%u posts=%u secondary=2688..5236 vertices=512..608; same retained program\n",unlock_point,*point,r->graph_draw_end_bytes,r->graph_draw_posts);
 dma_rmb();
 dev_info(&mdev->pdev->dev,"M9 FAA retired counters: +00=%#x/%#x +20=%#x/%#x +40=%#x/%#x +60=%#x/%#x\n",
 READ_ONCE(((u32*)t->fence_cpu)[0]),READ_ONCE(((u32*)t->fence_cpu)[1]),
 READ_ONCE(((u32*)t->fence_cpu)[8]),READ_ONCE(((u32*)t->fence_cpu)[9]),
 READ_ONCE(((u32*)t->fence_cpu)[16]),READ_ONCE(((u32*)t->fence_cpu)[17]),
 READ_ONCE(((u32*)t->fence_cpu)[24]),READ_ONCE(((u32*)t->fence_cpu)[25]));
out:
	if (ret)
		*point = 0;
	m9x_transport_fence_finish_locked(mdev, fence, ret, *posted);
 mutex_unlock(&t->lock);m9x_faa_graph_binding_put(checked);return ret;
#else
 return -EOPNOTSUPP;
#endif
}

#ifdef M9_FAA_GRAPH_INIT_CONTROL
static int m9_faa_public_vs_head_state(struct m9x_transport *t,
		struct m9_faa_graph_binding *binding, u32 generation, u64 head_point)
{
	struct m9x_faa_replay *r = t->faa;

	lockdep_assert_held(&t->lock);
	if (t->wedged)
		return -EIO;
	if (!r || !binding || r->graph_binding != binding ||
	    !r->prepared || !r->attempted || !r->observation_retired ||
	    !r->graph_draw_claimed || !r->public_vs_payload_ready ||
	    !r->public_vs_head_attempted || !r->public_vs_head_retired ||
	    r->public_vs_prefix_retired || r->public_vs_follow_retired ||
	    !r->public_vs_image || !r->public_vs_head_fence ||
	    r->draw_image || r->graph_draw_payload_ready || r->graph_preparing ||
	    r->graph_generation != generation || t->generation != generation ||
	    r->observation_generation != generation || !head_point ||
	    (head_point & 15) || head_point != r->observation_point + 0x10 ||
	    r->graph_draw_point != head_point || r->graph_draw_posts != 2 ||
	    r->graph_draw_end_bytes != r->primary_bytes + 64 ||
	    r->public_vs_image->ends.first_end != r->graph_draw_end_bytes ||
	    t->submitted_point != head_point || t->completed_point != head_point ||
	    t->next_fence != lower_32_bits(head_point) + 0x10 ||
	    r->public_vs_head_fence->base.context != t->fence_context ||
	    r->public_vs_head_fence->base.seqno != head_point ||
	    dma_fence_get_status(&r->public_vs_head_fence->base))
		return -ESTALE;
	if (!t->faa_source_context || !t->faa_virtual_primary ||
	    !t->engine_transitioned || !t->restore_ready || !t->fence_cpu ||
	    !t->transport_armed || !t->bus_master_enabled || t->async_stop ||
	    t->async_pending || t->queue_count)
		return -EBUSY;
	return 0;
}
#endif

/* Caller holds all six reservations. Ignore only this exact authenticated
 * HEAD fence: context/sequence equality alone never exempts another hazard.
 * No allocations, uploads, MMIO, fence signalling or posting occur here. */
int m9x_faa_graph_public_vs_head_hazards(struct m9x_device *mdev,
		struct m9_faa_graph_binding *binding, u32 generation, u64 head_point)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	u32 i;
	int ret;

	mutex_lock(&t->lock);
	ret = m9_faa_public_vs_head_state(t, binding, generation, head_point);
	if (ret)
		goto out;
	r = t->faa;
	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
		struct drm_gem_object *bo = m9x_faa_graph_binding_owner(binding, i);
		struct dma_resv_iter cursor;
		struct dma_fence *fence;
		bool found = false;

		dma_resv_assert_held(bo->resv);
		dma_resv_for_each_fence(&cursor, bo->resv, DMA_RESV_USAGE_READ, fence) {
			int status;

			if (fence == &r->public_vs_head_fence->base) {
				found = true;
				continue;
			}
			status = dma_fence_get_status(fence);
			if (status <= 0) {
				ret = status ? status : -EBUSY;
				goto out;
			}
		}
		if (!found) {
			ret = -ESTALE;
			goto out;
		}
	}
out:
	mutex_unlock(&t->lock);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

#ifdef M9_FAA_GRAPH_INIT_CONTROL
static int m9_faa_public_vs_draw_state(struct m9x_transport *t,
		struct m9_faa_graph_binding *binding, u32 generation, u64 draw_point)
{
	struct m9x_faa_replay *r = t->faa;

	lockdep_assert_held(&t->lock);
	if (t->wedged)
		return -EIO;
	if (!r || !binding || r->graph_binding != binding ||
	    !r->prepared || !r->attempted || !r->observation_retired ||
	    !r->graph_draw_claimed || !r->public_vs_payload_ready ||
	    !r->public_vs_head_attempted || !r->public_vs_head_retired ||
	    !r->public_vs_draw_attempted || !r->public_vs_draw_observed ||
	    !r->public_vs_draw_image || !r->public_vs_head_fence ||
	    r->public_vs_prefix_retired || r->public_vs_follow_retired ||
	    !r->public_vs_image || !r->public_vs_draw_fence ||
	    r->draw_image || r->graph_draw_payload_ready || r->graph_preparing ||
	    r->graph_generation != generation || t->generation != generation ||
	    r->observation_generation != generation || !draw_point ||
	    (draw_point & 15) || draw_point != r->observation_point + 0xb0 ||
	    r->graph_draw_point != draw_point || r->graph_draw_posts != 3 ||
	    r->graph_draw_end_bytes != r->primary_bytes + 124 ||
	    r->public_vs_draw_image->ends.final_end != r->graph_draw_end_bytes ||
	    r->public_vs_draw_image->ends.observed_point != draw_point ||
	    t->submitted_point != draw_point || t->completed_point != draw_point ||
	    t->next_fence != lower_32_bits(draw_point) + 0x10 ||
	    r->public_vs_draw_fence->base.context != t->fence_context ||
	    r->public_vs_draw_fence->base.seqno != draw_point ||
	    dma_fence_get_status(&r->public_vs_draw_fence->base))
		return -ESTALE;
	if (!t->faa_source_context || !t->faa_virtual_primary ||
	    !t->engine_transitioned || !t->restore_ready || !t->fence_cpu ||
	    !t->transport_armed || !t->bus_master_enabled || t->async_stop ||
	    t->async_pending || t->queue_count)
		return -EBUSY;
	return 0;
}
#endif

/* Caller holds all six reservations. Ignore only this exact authenticated
 * DRAW fence: context/sequence equality alone never exempts another hazard.
 * No allocations, uploads, MMIO, fence signalling or posting occur here. */
int m9x_faa_graph_public_vs_draw_hazards(struct m9x_device *mdev,
		struct m9_faa_graph_binding *binding, u32 generation, u64 draw_point)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	u32 i;
	int ret;

	mutex_lock(&t->lock);
	ret = m9_faa_public_vs_draw_state(t, binding, generation, draw_point);
	if (ret)
		goto out;
	r = t->faa;
	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++) {
		struct drm_gem_object *bo = m9x_faa_graph_binding_owner(binding, i);
		struct dma_resv_iter cursor;
		struct dma_fence *fence;
		bool found = false;

		dma_resv_assert_held(bo->resv);
		dma_resv_for_each_fence(&cursor, bo->resv, DMA_RESV_USAGE_READ, fence) {
			int status;

			if (fence == &r->public_vs_draw_fence->base) {
				found = true;
				continue;
			}
			status = dma_fence_get_status(fence);
			if (status <= 0) {
				ret = status ? status : -EBUSY;
				goto out;
			}
		}
		if (!found) {
			ret = -ESTALE;
			goto out;
		}
	}
out:
	mutex_unlock(&t->lock);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

int m9x_faa_graph_public_vs_draw_get_locked(struct m9x_device *mdev,
 u32 generation,u64 draw_point,struct m9_faa_graph_binding **out)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 int ret;
 if(!out)return -EINVAL;
 *out=NULL;
 ret=m9_faa_public_vs_draw_state(&mdev->transport,
  mdev->transport.faa ? mdev->transport.faa->graph_binding : NULL,generation,draw_point);
 if(ret)return ret;
 *out=m9x_faa_graph_binding_get(mdev->transport.faa->graph_binding);
 return 0;
#else
 return -EOPNOTSUPP;
#endif
}

int m9x_faa_graph_public_vs_head_get_locked(struct m9x_device *mdev,
 u32 generation,u64 head_point,struct m9_faa_graph_binding **out)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 int ret;
 if(!out)return -EINVAL;
 *out=NULL;
 ret=m9_faa_public_vs_head_state(&mdev->transport,
  mdev->transport.faa ? mdev->transport.faa->graph_binding : NULL,generation,head_point);
 if(ret)return ret;
 if(mdev->transport.faa->public_vs_draw_attempted ||
    mdev->transport.faa->public_vs_draw_observed)return -ESTALE;
 *out=m9x_faa_graph_binding_get(mdev->transport.faa->graph_binding);
 return 0;
#else
 return -EOPNOTSUPP;
#endif
}

int m9x_faa_graph_public_vs_draw_prepare(struct m9x_device *mdev,
 struct m9_faa_graph_binding *binding,u32 generation,u64 head_point)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 struct m9x_transport *t=&mdev->transport;
 struct m9x_faa_replay *r;
 struct m9_faa_public_vs_draw_storage *storage;
 struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS];
 struct m9x_faa_public_vs_draw_image out;
 const struct firmware *fw;
 u32 i;int ret;
 ret=request_firmware(&fw,M9_FAA_GRAPH_PUBLIC_VS_DRAW_FIRMWARE,&mdev->pdev->dev);
 if(ret)return ret;
 if(!m9x_faa_public_vs_draw_template_valid(fw->data,fw->size)){ret=-EINVAL;goto out_fw;}
 storage=kzalloc(sizeof(*storage),GFP_KERNEL);
 if(!storage){ret=-ENOMEM;goto out_fw;}
 mutex_lock(&t->lock);
 ret=m9_faa_public_vs_head_state(t,binding,generation,head_point);
 if(ret)goto out_lock;
 r=t->faa;
 if(r->public_vs_draw_image || r->public_vs_draw_attempted || r->public_vs_draw_observed){ret=-ESTALE;goto out_lock;}
 for(i=0;i<M9_FAA_DRAW_SOURCE_OWNERS;i++){
  struct m9_faa_owner *o;
  switch(i){case 26:o=&r->draw_owner[0];break;case 27:o=&r->owner[26];break;
   case 28:o=&r->draw_owner[1];break;case 29:o=&r->draw_owner[2];break;
   case 30:o=&r->owner[27];break;default:o=&r->owner[i];break;}
  owner[i]=(struct m9x_faa_graph_draw_owner){o->base,o->bytes};
 }
 out=(struct m9x_faa_public_vs_draw_image){storage->primary,storage->secondary,
  sizeof(storage->primary),sizeof(storage->secondary)};
 ret=m9x_faa_public_vs_draw_encode(fw->data,fw->size,owner,r->graph_draw_end_bytes,
  head_point,r->public_vs_image->layout.constant_begin[0],
  r->public_vs_image->layout.constant_end[0],&out,&storage->ends);
 if(ret)goto out_lock;
 r->public_vs_draw_image=storage;storage=NULL;
out_lock:
 mutex_unlock(&t->lock);kfree(storage);
out_fw:
 release_firmware(fw);return ret;
#else
 return -EOPNOTSUPP;
#endif
}

int m9x_faa_graph_public_vs_draw_submit(struct m9x_device *mdev,
 struct m9_faa_graph_binding *binding,u32 generation,u64 head_point,u64 *point,bool *posted)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 struct m9x_transport *t=&mdev->transport;
 struct m9x_faa_replay *r;
 struct m9_faa_owner *primary,*secondary;
 struct m9_faa_public_vs_draw_storage *image;
 struct m9x_fence *fence=NULL;
	u32 i, old_bytes;
	int ret;
 bool fenced=false;
 if(!point||!posted)return -EINVAL;
 *point=0;*posted=false;
 /* No unlocked interval after exact owned-fence exclusion: caller holds all
  * six reservations, and none of this path removes its HEAD reference. */
 ret=m9x_faa_graph_public_vs_head_hazards(mdev,binding,generation,head_point);
 if(ret)return ret;
 mutex_lock(&t->lock);
 ret=m9_faa_public_vs_head_state(t,binding,generation,head_point);
 if(ret)goto out;
 r=t->faa;primary=&r->owner[M9_FAA_PRIMARY_OWNER];secondary=&r->draw_owner[2];
 image=r->public_vs_draw_image;old_bytes=r->graph_draw_end_bytes;
 if(!image||r->public_vs_draw_attempted||r->public_vs_draw_observed||
  !primary->cpu||!secondary->cpu||!drm_mm_node_allocated(&primary->system_node)||
  primary->system_node.start!=primary->base||old_bytes>primary->bytes||primary->bytes-old_bytes<60||
  secondary->bytes<2688||image->ends.final_end!=old_bytes+60||image->ends.observed_point!=head_point+0xa0){ret=-ESTALE;goto out;}
 for(i=1;i<ARRAY_SIZE(r->draw_owner);i++){
  struct m9_faa_owner *o=&r->draw_owner[i];
  if(!o->cpu||!drm_mm_node_allocated(&o->system_node)||o->system_node.start!=o->base){ret=-ESTALE;goto out;}
		ret = m9x_system_pages_mapped(mdev, o->base, o->dma,
					      o->bytes / PAGE_SIZE, 1u, NULL, NULL);
		if (ret)
			goto out;
 }
 ret=m9x_address_check_local_identity(mdev,r->draw_owner[0].base,r->draw_owner[0].bytes);
 if(ret)goto out;
 /* Preserve the source immediate only while its fixed SYSTEM page belongs
  * to the retained ordinary allocation; do not infer a command-primary role. */
 ret=m9x_triangle_system_page_admit_locked(mdev,0x40004000);
 if(ret)goto out;
 fence=m9x_fence_create_target_locked(mdev,lower_32_bits(image->ends.observed_point));
 if(!fence){ret=-ENOMEM;goto out;}
 r->public_vs_draw_attempted=true;
 for(i=0;i<PARHELIA_M9_FAA_GRAPH_OWNERS;i++)
  dma_resv_add_fence(m9x_faa_graph_binding_owner(binding,i)->resv,&fence->base,DMA_RESV_USAGE_WRITE);
 fenced=true;
 /* HEAD never exposed any secondary. Materialize the entire CPU-known,
  * closed block (including the source43 dependencies) before its first fetch. */
 memcpy(secondary->cpu,image->secondary,sizeof(image->secondary));
 memcpy((u8 *)primary->cpu+old_bytes,image->primary,sizeof(image->primary));
 dma_wmb();
 ret=m9x_post_faa_graph_end_locked(mdev,&primary->system_node,primary->dma,
  old_bytes,image->ends.final_end,false,lower_32_bits(image->ends.observed_point),point,posted);
 if(*posted){r->graph_draw_end_bytes=image->ends.final_end;r->graph_draw_posts++;}
 if(!ret&&*point!=image->ends.observed_point)ret=-EIO;
 if(!ret){
  r->public_vs_draw_observed=true;r->graph_draw_point=*point;
  r->public_vs_draw_fence=fence;fence=NULL;
  dev_info(&mdev->pdev->dev,"M9 FAA public VS draw observed: head=%#llx point=%#llx bytes=%u posts=%u; E3 before final raster, owners held\n",head_point,*point,r->graph_draw_end_bytes,r->graph_draw_posts);
  dma_rmb();
  dev_info(&mdev->pdev->dev,"M9 FAA retired counters: +00=%#x/%#x +20=%#x/%#x +40=%#x/%#x +60=%#x/%#x\n",
   READ_ONCE(((u32 *)t->fence_cpu)[0]),READ_ONCE(((u32 *)t->fence_cpu)[1]),
   READ_ONCE(((u32 *)t->fence_cpu)[0x20/4]),READ_ONCE(((u32 *)t->fence_cpu)[0x24/4]),
   READ_ONCE(((u32 *)t->fence_cpu)[0x40/4]),READ_ONCE(((u32 *)t->fence_cpu)[0x44/4]),
   READ_ONCE(((u32 *)t->fence_cpu)[0x60/4]),READ_ONCE(((u32 *)t->fence_cpu)[0x64/4]));
 }
out:
 if(ret)*point=0;
 if(fence&&ret)dma_fence_set_error(&fence->base,ret);
 if(ret&&fenced){
  /* Same-context insertion can replace HEAD in reservations. Even a failed
   * pre-post cannot signal this replacement while HEAD's tail is reachable. */
  t->wedged=true;t->generation++;WARN_ON(t->quarantined_fence);
  t->quarantined_fence=fence;fence=NULL;
 }
 if(fence)dma_fence_put(&fence->base);
 mutex_unlock(&t->lock);return ret;
#else
 return -EOPNOTSUPP;
#endif
}

int m9x_faa_graph_public_vs_lock_prepare(struct m9x_device *mdev,
 struct m9_faa_graph_binding *binding,u32 generation,u64 draw_point)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 struct m9x_transport *t=&mdev->transport;
 struct m9x_faa_replay *r;
 struct m9_faa_public_vs_lock_storage *storage;
 struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS];
 const struct firmware *fw;
 u32 i;int ret;
 ret=request_firmware(&fw,M9_FAA_GRAPH_PUBLIC_VS_LOCK_FIRMWARE,&mdev->pdev->dev);
 if(ret)return ret;
 if(!m9x_faa_public_vs_lock_blob_valid(fw->data,fw->size)){ret=-EINVAL;goto out_fw;}
 storage=kzalloc(sizeof(*storage),GFP_KERNEL);
 if(!storage){ret=-ENOMEM;goto out_fw;}
 mutex_lock(&t->lock);
 ret=m9_faa_public_vs_draw_state(t,binding,generation,draw_point);
 if(ret)goto out_lock;
 r=t->faa;
 if(r->public_vs_lock_image||r->public_vs_lock_attempted||r->public_vs_lock_retired){ret=-ESTALE;goto out_lock;}
 for(i=0;i<M9_FAA_DRAW_SOURCE_OWNERS;i++){
  struct m9_faa_owner *o;
  switch(i){case 26:o=&r->draw_owner[0];break;case 27:o=&r->owner[26];break;
   case 28:o=&r->draw_owner[1];break;case 29:o=&r->draw_owner[2];break;
   case 30:o=&r->owner[27];break;default:o=&r->owner[i];break;}
  owner[i]=(struct m9x_faa_graph_draw_owner){o->base,o->bytes};
 }
 ret=m9x_faa_public_vs_lock_encode(fw->data,fw->size,owner,r->graph_draw_end_bytes,
  320,240,draw_point,storage->primary,sizeof(storage->primary));
 if(ret)goto out_lock;
 r->public_vs_lock_image=storage;storage=NULL;
out_lock:
 mutex_unlock(&t->lock);kfree(storage);
out_fw:
 release_firmware(fw);return ret;
#else
 return -EOPNOTSUPP;
#endif
}

int m9x_faa_graph_public_vs_lock_submit(struct m9x_device *mdev,
 struct m9_faa_graph_binding *binding,u32 generation,u64 draw_point,u64 *point,bool *posted)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 struct m9x_transport *t=&mdev->transport;
 struct m9x_faa_replay *r;
 struct m9_faa_owner *primary;
 struct m9_faa_public_vs_lock_storage *image;
 struct m9x_fence *fence=NULL;
	u32 i, old_bytes, first_end, final_end;
	int ret;
 bool fenced=false,one_posted=false;
 u64 observed=0;
 if(!point||!posted)return -EINVAL;
 *point=0;*posted=false;
 ret=m9x_faa_graph_public_vs_draw_hazards(mdev,binding,generation,draw_point);
 if(ret)return ret;
 mutex_lock(&t->lock);
 ret=m9_faa_public_vs_draw_state(t,binding,generation,draw_point);
 if(ret)goto out;
 r=t->faa;primary=&r->owner[M9_FAA_PRIMARY_OWNER];image=r->public_vs_lock_image;
 old_bytes=r->graph_draw_end_bytes;
 if(!image||r->public_vs_lock_attempted||r->public_vs_lock_retired||
  !primary->cpu||!drm_mm_node_allocated(&primary->system_node)||
  primary->system_node.start!=primary->base||old_bytes>primary->bytes||
  primary->bytes-old_bytes<M9_FAA_VS_LOCK_PRIMARY_BYTES||
  lower_32_bits(draw_point)>U32_MAX-0x40){ret=-ESTALE;goto out;}
 first_end=old_bytes+M9_FAA_VS_LOCK_FIRST_POST_BYTES;
 final_end=old_bytes+M9_FAA_VS_LOCK_PRIMARY_BYTES;
	ret = m9x_system_pages_mapped(mdev, primary->base, primary->dma,
				      primary->bytes / PAGE_SIZE, 1u, NULL, NULL);
	if (ret)
		goto out;
 ret=m9x_triangle_system_page_admit_locked(mdev,0x40004000);
 if(ret)goto out;
 ret=m9x_triangle_system_page_admit_locked(mdev,0x40008000);
 if(ret)goto out;
 fence=m9x_fence_create_target_locked(mdev,lower_32_bits(draw_point)+0x30);
 if(!fence){ret=-ENOMEM;goto out;}
 r->public_vs_lock_attempted=true;
 for(i=0;i<PARHELIA_M9_FAA_GRAPH_OWNERS;i++)
  dma_resv_add_fence(m9x_faa_graph_binding_owner(binding,i)->resv,&fence->base,DMA_RESV_USAGE_WRITE);
 fenced=true;
 memcpy((u8 *)primary->cpu+old_bytes,image->primary,M9_FAA_VS_LOCK_FIRST_POST_BYTES);
 dma_wmb();
 ret=m9x_post_faa_graph_end_locked(mdev,&primary->system_node,primary->dma,
  old_bytes,first_end,false,0,&observed,&one_posted);
 if(one_posted){*posted=true;r->graph_draw_end_bytes=first_end;r->graph_draw_posts++;}
 if(ret)goto out;
 one_posted=false;
 ret=m9x_post_faa_graph_end_locked(mdev,&primary->system_node,primary->dma,
  first_end,first_end,true,0,&observed,&one_posted);
 if(one_posted){*posted=true;r->graph_draw_posts++;}
 if(ret)goto out;
 /* Source lane+20 probes have desired0 and only maintain a CPU cache;
  * they require no fabricated new device value or MMIO publication. */
 memcpy((u8 *)primary->cpu+first_end,image->primary+M9_FAA_VS_LOCK_FIRST_POST_BYTES,
  M9_FAA_VS_LOCK_PRIMARY_BYTES-M9_FAA_VS_LOCK_FIRST_POST_BYTES);
 dma_wmb();one_posted=false;
 ret=m9x_post_faa_graph_end_locked(mdev,&primary->system_node,primary->dma,
  first_end,final_end,false,lower_32_bits(draw_point)+0x30,point,&one_posted);
 if(one_posted){*posted=true;r->graph_draw_end_bytes=final_end;r->graph_draw_posts++;}
 if(!ret&&*point!=draw_point+0x30)ret=-EIO;
 if(!ret){
  r->public_vs_lock_retired=true;r->graph_draw_point=*point;
  /* Final write-wait footer follows all prior transform/raster/Lock work. */
  dma_fence_signal(&r->public_vs_head_fence->base);
  dma_fence_put(&r->public_vs_head_fence->base);r->public_vs_head_fence=NULL;
  dma_fence_signal(&r->public_vs_draw_fence->base);
  dma_fence_put(&r->public_vs_draw_fence->base);r->public_vs_draw_fence=NULL;
  dma_fence_signal(&fence->base);
  dev_info(&mdev->pdev->dev,"M9 FAA public VS Lock retired: draw=%#llx point=%#llx bytes=%u posts=%u; F021 owner1 eligible\n",draw_point,*point,r->graph_draw_end_bytes,r->graph_draw_posts);
  dma_rmb();
  dev_info(&mdev->pdev->dev,"M9 FAA retired counters: +00=%#x/%#x +20=%#x/%#x +40=%#x/%#x +60=%#x/%#x\n",
   READ_ONCE(((u32 *)t->fence_cpu)[0]),READ_ONCE(((u32 *)t->fence_cpu)[1]),
   READ_ONCE(((u32 *)t->fence_cpu)[0x20/4]),READ_ONCE(((u32 *)t->fence_cpu)[0x24/4]),
   READ_ONCE(((u32 *)t->fence_cpu)[0x40/4]),READ_ONCE(((u32 *)t->fence_cpu)[0x44/4]),
   READ_ONCE(((u32 *)t->fence_cpu)[0x60/4]),READ_ONCE(((u32 *)t->fence_cpu)[0x64/4]));
 }
out:
 if(ret)*point=0;
 if(fence&&ret)dma_fence_set_error(&fence->base,ret);
 if(ret&&fenced){
  t->wedged=true;t->generation++;WARN_ON(t->quarantined_fence);
  t->quarantined_fence=fence;fence=NULL;
 }
 if(fence)dma_fence_put(&fence->base);
 mutex_unlock(&t->lock);return ret;
#else
 return -EOPNOTSUPP;
#endif
}

#ifdef M9_FAA_GRAPH_INIT_CONTROL
static int m9_faa_public_vs_follow_state(struct m9x_transport *t, u32 generation, u64 prefix_point,
					 bool claimed)
{
	struct m9x_faa_replay *r = t->faa;
	lockdep_assert_held(&t->lock);
	if (t->wedged)
		return -EIO;
	if (!r || !r->graph_binding || !r->prepared || !r->attempted || !r->observation_retired ||
	    !r->graph_draw_claimed || !r->public_vs_payload_ready || !r->public_vs_prefix_retired ||
	    r->public_vs_head_attempted || r->public_vs_head_retired ||
	    !r->public_vs_image || r->draw_image || r->graph_draw_payload_ready ||
	    r->public_vs_follow_retired || (!!r->public_vs_follow_image != claimed) ||
	    r->graph_preparing || r->graph_generation != generation ||
	    t->generation != generation || r->observation_generation != generation ||
	    !prefix_point || prefix_point != r->observation_point + 0x50 ||
	    r->graph_draw_point != prefix_point || r->graph_draw_posts != 3 ||
	    r->graph_draw_end_bytes != r->primary_bytes + 112 ||
	    r->public_vs_image->ends.final_end != r->graph_draw_end_bytes ||
	    r->public_vs_image->ends.independent_finish != prefix_point || (prefix_point & 15) ||
	    lower_32_bits(prefix_point) > U32_MAX - 0x40 || t->submitted_point != prefix_point ||
	    t->completed_point != prefix_point ||
	    t->next_fence != lower_32_bits(prefix_point) + 0x10)
		return -ESTALE;
	if (!t->faa_source_context || !t->faa_virtual_primary || !t->engine_transitioned ||
	    !t->restore_ready || !t->fence_cpu || !t->transport_armed || !t->bus_master_enabled ||
	    t->async_stop || t->async_pending || t->queue_count)
		return -EBUSY;
	return 0;
}
#endif
int m9x_faa_graph_public_vs_follow_get_locked(struct m9x_device *mdev, u32 generation,
					      u64 prefix_point, struct m9_faa_graph_binding **out)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	int ret;
	lockdep_assert_held(&mdev->transport.lock);
	if (!out)
		return -EINVAL;
	*out = NULL;
	ret = m9_faa_public_vs_follow_state(&mdev->transport, generation, prefix_point, false);
	if (!ret)
		*out = m9x_faa_graph_binding_get(mdev->transport.faa->graph_binding);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}
int m9x_faa_graph_public_vs_follow_prepare(struct m9x_device *mdev,
					   struct m9_faa_graph_binding *binding, u32 generation,
					   u64 prefix_point)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	struct m9_faa_public_vs_follow_storage *storage;
	struct m9x_faa_public_vs_follow_image image;
	struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS];
	const struct firmware *fw;
	u32 i;
	int ret;
	if (!binding)
		return -EINVAL;
	ret = request_firmware(&fw, M9_FAA_GRAPH_PUBLIC_VS_FOLLOW_FIRMWARE, &mdev->pdev->dev);
	if (ret)
		return ret;
	if (!m9x_faa_public_vs_follow_template_valid(fw->data, fw->size)) {
		ret = -EINVAL;
		goto out_fw;
	}
	storage = kmalloc(sizeof(*storage), GFP_KERNEL);
	if (!storage) {
		ret = -ENOMEM;
		goto out_fw;
	}
	mutex_lock(&t->lock);
	ret = m9_faa_public_vs_follow_state(t, generation, prefix_point, false);
	if (ret)
		goto out_lock;
	r = t->faa;
	if (r->graph_binding != binding) {
		ret = -ESTALE;
		goto out_lock;
	}
	if (!r->draw_owner[2].cpu || r->draw_owner[2].bytes < 1368 ||
	    READ_ONCE(((u32 *)r->draw_owner[2].cpu)[704 / 4]) != M9_FAA_VS_FOLLOW_PATCH_EXPECTED) {
		ret = -ESTALE;
		goto out_lock;
	}
	for (i = 0; i < M9_FAA_DRAW_SOURCE_OWNERS; i++) {
		struct m9_faa_owner *o;
		switch (i) {
		case 26:
			o = &r->draw_owner[0];
			break;
		case 27:
			o = &r->owner[26];
			break;
		case 28:
			o = &r->draw_owner[1];
			break;
		case 29:
			o = &r->draw_owner[2];
			break;
		case 30:
			o = &r->owner[27];
			break;
		default:
			o = &r->owner[i];
			break;
		}
		owner[i].base = o->base;
		owner[i].bytes = o->bytes;
	}
	image = (struct m9x_faa_public_vs_follow_image){
	    storage->primary,
	    storage->secondary,
	    sizeof(storage->primary),
	    sizeof(storage->secondary),
	};
	ret = m9x_faa_public_vs_follow_encode(fw->data, fw->size, owner, r->graph_draw_end_bytes,
					      prefix_point, &image, &storage->ends);
	if (ret)
		goto out_lock;
	r->public_vs_follow_image = storage;
	storage = NULL;
out_lock:
	mutex_unlock(&t->lock);
	kfree(storage);
out_fw:
	release_firmware(fw);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}
int m9x_faa_graph_public_vs_follow_submit(struct m9x_device *mdev,
					  struct m9_faa_graph_binding *binding, u32 generation,
					  u64 prefix_point, u64 *point, bool *posted)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	struct m9_faa_owner *primary, *secondary;
	struct m9_faa_public_vs_follow_storage *image;
	struct m9x_fence *fence = NULL;
	u32 i, page, pte, secondary_pte = 0, old_bytes, target;
	int ret;
	if (!binding || !point || !posted)
		return -EINVAL;
	*point = 0;
	*posted = false;
	mutex_lock(&t->lock);
	ret = m9_faa_public_vs_follow_state(t, generation, prefix_point, true);
	if (ret)
		goto out;
	r = t->faa;
	image = r->public_vs_follow_image;
	primary = &r->owner[M9_FAA_PRIMARY_OWNER];
	secondary = &r->draw_owner[2];
	old_bytes = r->graph_draw_end_bytes;
	if (r->graph_binding != binding || !primary->cpu || !secondary->cpu ||
	    !drm_mm_node_allocated(&primary->system_node) ||
	    primary->system_node.start != primary->base || old_bytes > primary->bytes ||
	    primary->bytes - old_bytes < 48 || secondary->bytes < 1372 ||
	    image->ends.final_end != old_bytes + 48 ||
	    image->ends.original_wait != prefix_point + 0x20 ||
	    image->ends.independent_finish != prefix_point + 0x30 ||
	    image->ends.patch_word != (M9_FAA_VS_FOLLOW_PATCH_EXPECTED | ((1284u - 704u) / 4)) ||
	    READ_ONCE(((u32 *)secondary->cpu)[704 / 4]) != M9_FAA_VS_FOLLOW_PATCH_EXPECTED) {
		ret = -ESTALE;
		goto out;
	}
	for (i = 1; i < ARRAY_SIZE(r->draw_owner); i++) {
		struct m9_faa_owner *o = &r->draw_owner[i];
		if (!o->cpu || !drm_mm_node_allocated(&o->system_node) ||
		    o->system_node.start != o->base) {
			ret = -ESTALE;
			goto out;
		}
		for (page = 0; page < o->bytes / PAGE_SIZE; page++) {
			ret = m9x_address_read_pte(mdev, o->base + page * PAGE_SIZE, &pte);
			if (ret)
				goto out;
			if (pte != (m9x_address_pte_word(o->dma + page * PAGE_SIZE) | 1u)) {
				ret = -ESTALE;
				goto out;
			}
			if (i == 2 && !page)
				secondary_pte = pte;
		}
	}
	ret = m9x_address_check_local_identity(mdev, r->draw_owner[0].base, r->draw_owner[0].bytes);
	if (ret)
		goto out;
	target = lower_32_bits(prefix_point) + 0x30;
	fence = m9x_fence_create_target_locked(mdev, target);
	if (!fence) {
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++)
		dma_resv_add_fence(m9x_faa_graph_binding_owner(binding, i)->resv, &fence->base,
				   DMA_RESV_USAGE_WRITE);
	/* Source writes [752,1284), then248fe patches old704, then writes
	 * [1284,1368) and the primary suffix before its only END. Preserve that
	 * CPU chronology even though no intervening source MMIO is observed. */
	memcpy((u8 *)secondary->cpu + 752, image->secondary, 1284 - 752);
	WRITE_ONCE(((u32 *)secondary->cpu)[704 / 4], image->ends.patch_word);
	memcpy((u8 *)secondary->cpu + 1284, image->secondary + (1284 - 752),
	       1368 - 1284);
	memcpy((u8 *)primary->cpu + old_bytes, image->primary, sizeof(image->primary));
	wmb();
	dev_info(&mdev->pdev->dev,
		 "M9 FAA public VS follow before post: secondary=%#x dma=%pad pte0=%#x "
		 "program=%#x relative704=%#x primary=%#x old=%u new=%u indirect=%#x/%#x/%#x\n",
		 secondary->base, &secondary->dma, secondary_pte, r->draw_owner[0].base,
		 READ_ONCE(((u32 *)secondary->cpu)[704 / 4]), primary->base,
		 old_bytes, image->ends.final_end,
		 READ_ONCE(((u32 *)primary->cpu)[old_bytes / 4]),
		 READ_ONCE(((u32 *)primary->cpu)[old_bytes / 4 + 1]),
		 READ_ONCE(((u32 *)primary->cpu)[old_bytes / 4 + 2]));
	ret = m9x_post_faa_graph_end_locked(mdev, &primary->system_node, primary->dma, old_bytes,
					    image->ends.final_end, false, target, point, posted);
	if (*posted) {
		r->graph_draw_end_bytes = image->ends.final_end;
		r->graph_draw_posts++;
	}
	if (!ret && *point != prefix_point + 0x30)
		ret = -EIO;
	if (!ret) {
		r->graph_draw_point = *point;
		r->public_vs_follow_retired = true;
		dev_info(&mdev->pdev->dev,
			 "M9 FAA public VS follow prefix retired: prefix=%#llx point=%#llx "
			 "bytes=%u posts=%u relative704=%#x; original DRAW pending\n",
			 prefix_point, *point, r->graph_draw_end_bytes, r->graph_draw_posts,
			 image->ends.patch_word);
		dma_rmb();
		dev_info(&mdev->pdev->dev,
			 "M9 FAA retired counters: +00=%#x/%#x +20=%#x/%#x +40=%#x/%#x +60=%#x/%#x\n",
			 READ_ONCE(((u32 *)t->fence_cpu)[0]),
			 READ_ONCE(((u32 *)t->fence_cpu)[1]),
			 READ_ONCE(((u32 *)t->fence_cpu)[0x20 / 4]),
			 READ_ONCE(((u32 *)t->fence_cpu)[0x24 / 4]),
			 READ_ONCE(((u32 *)t->fence_cpu)[0x40 / 4]),
			 READ_ONCE(((u32 *)t->fence_cpu)[0x44 / 4]),
			 READ_ONCE(((u32 *)t->fence_cpu)[0x60 / 4]),
			 READ_ONCE(((u32 *)t->fence_cpu)[0x64 / 4]));
	}
out:
	m9x_transport_fence_finish_locked(mdev, fence, ret, *posted);
	mutex_unlock(&t->lock);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

#ifdef M9_FAA_GRAPH_INIT_CONTROL
static int m9_faa_graph_next_state(struct m9x_transport *t,
		u32 generation, u64 unlock_point, bool claimed)
{
	struct m9x_faa_replay *r = t->faa;

	lockdep_assert_held(&t->lock);
	if (t->wedged)
		return -EIO;
	if (!r || !r->graph_binding || !r->normal_lock_view_seen ||
	    !r->unlock_started || !r->unlock_retired ||
	    (r->graph_next_claimed && r->next_unlock_point == unlock_point) != claimed ||
	    (claimed && !r->next_image) ||
	    r->graph_generation != generation || t->generation != generation ||
	    r->observation_generation != generation || !r->observation_retired ||
	    !unlock_point || unlock_point != r->unlock_point ||
	    !r->graph_draw_payload_ready || r->public_vs_payload_ready ||
	    !r->graph_draw_with_lock || !r->graph_draw_with_finish ||
	    r->graph_draw_point != unlock_point ||
	    r->graph_draw_posts !=	r->graph_unlock_posts ||
	    r->graph_unlock_posts < 11 ||
	    r->graph_draw_end_bytes !=	r->graph_unlock_end_bytes ||
	    r->graph_secondary_cursor < M9_FAA_NEXT_SECONDARY_OFFSET ||
	    r->graph_vertex_cursor < M9_FAA_NEXT_VERTEX_OFFSET ||
	    (claimed && (r->next_unlock_point != unlock_point ||
	      r->next_posts_start !=	r->graph_unlock_posts ||
	      r->next_cursor.primary !=	r->graph_unlock_end_bytes ||
	      r->next_cursor.secondary != r->graph_secondary_cursor ||
	      r->next_cursor.vertices != r->graph_vertex_cursor)) ||
	    (unlock_point & 15) || lower_32_bits(unlock_point) > U32_MAX - 0x60 ||
	    t->submitted_point != unlock_point || t->completed_point != unlock_point ||
	    t->next_fence != lower_32_bits(unlock_point) + 0x10)
		return -ESTALE;
	if (!t->faa_source_context || !t->faa_virtual_primary ||
	    !t->engine_transitioned || !t->restore_ready || !t->fence_cpu ||
	    !t->transport_armed || !t->bus_master_enabled ||
	    t->async_stop || t->async_pending || t->queue_count)
		return -EBUSY;
	return 0;
}
#endif

int m9x_faa_graph_next_get_locked(struct m9x_device *mdev,
		u32 generation, u64 unlock_point, struct m9_faa_graph_binding **out)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	int ret;

	lockdep_assert_held(&mdev->transport.lock);
	if (!out)
		return -EINVAL;
	*out = NULL;
	ret = m9_faa_graph_next_state(&mdev->transport, generation, unlock_point, false);
	if (!ret)
		*out = m9x_faa_graph_binding_get(mdev->transport.faa->graph_binding);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

/* Authenticate and build the live command image before reserving caller BOs.
 * No new GPU owners or mappings are needed by this source continuation. */
int m9x_faa_graph_next_prepare(struct m9x_device *mdev,
		struct m9_faa_graph_binding *binding, u32 generation,
		u64 unlock_point, const u32 vertices[15])
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	struct m9_faa_next_storage *storage;
	struct m9x_faa_graph_next_image image;
	struct m9x_faa_graph_next_cursor cursor;
	struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS];
	const struct firmware *fw;
	u32 i;
	int ret;

	if (!binding || !vertices)
		return -EINVAL;
	ret = request_firmware(&fw, M9_FAA_GRAPH_NEXT_FIRMWARE, &mdev->pdev->dev);
	if (ret)
		return ret;
	if (!m9x_faa_graph_next_blob_valid(fw->data, fw->size)) {
		ret = -EINVAL;
		goto out_fw;
	}
	storage = kmalloc(sizeof(*storage), GFP_KERNEL);
	if (!storage) {
		ret = -ENOMEM;
		goto out_fw;
	}
	mutex_lock(&t->lock);
	ret = m9_faa_graph_next_state(t, generation, unlock_point, false);
	if (ret)
		goto out_lock;
	r = t->faa;
	if (r->graph_binding != binding) {
		ret = -ESTALE;
		goto out_lock;
	}
	for (i = 0; i < M9_FAA_DRAW_SOURCE_OWNERS; i++) {
		struct m9_faa_owner *o;

		switch (i) {
		case 26: o = &r->draw_owner[0]; break;
		case 27: o = &r->owner[26]; break;
		case 28: o = &r->draw_owner[1]; break;
		case 29: o = &r->draw_owner[2]; break;
		case 30: o = &r->owner[27]; break;
		default: o = &r->owner[i]; break;
		}
		owner[i].base = o->base;
		owner[i].bytes = o->bytes;
	}
	image = (struct m9x_faa_graph_next_image) {
		.primary = storage->primary, .primary_bytes = sizeof(storage->primary),
		.secondary = storage->secondary, .secondary_bytes = sizeof(storage->secondary),
		.vertices = storage->vertices, .vertex_bytes = sizeof(storage->vertices),
	};
	cursor = (struct m9x_faa_graph_next_cursor) {
		r->graph_draw_end_bytes, r->graph_secondary_cursor, r->graph_vertex_cursor,
	};
	ret = m9x_faa_graph_next_encode_at(fw->data, fw->size, owner,
			&cursor, unlock_point, vertices, &image);
	if (ret)
		goto out_lock;
	/* Claim only a fully encoded image; invalid inputs leave no half-state. */
	/* The preceding cycle has actually Unlocked. Replace only CPU staging
	 * snapshots after the new image has validated; no GPU owner or GEM lifetime
	 * changes occur under transport.lock. */
	kfree(r->next_image);
	kfree(r->next_finish_image);
	kfree(r->next_lock_image);
	r->next_finish_image = NULL;
	r->next_lock_image = NULL;
	r->graph_next_finish_claimed = false;
	r->graph_next_lock_claimed = false;
	r->next_cursor = cursor;
	r->next_posts_start = r->graph_draw_posts;
	r->next_unlock_point = unlock_point;
	r->graph_next_claimed = true;
	r->next_image = storage;
	storage = NULL;
out_lock:
	mutex_unlock(&t->lock);
	kfree(storage);
out_fw:
	release_firmware(fw);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

int m9x_faa_graph_next_submit(struct m9x_device *mdev,
		struct m9_faa_graph_binding *binding, u32 generation,
		u64 unlock_point, u64 *point, bool *posted)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	struct m9_faa_owner *primary, *secondary, *constants;
	struct m9x_fence *fence = NULL;
	u32 i, target, old_bytes, end;
	int ret;

	if (!binding || !point || !posted)
		return -EINVAL;
	*point = 0;
	*posted = false;
	mutex_lock(&t->lock);
	ret = m9_faa_graph_next_state(t, generation, unlock_point, true);
	if (ret)
		goto out;
	r = t->faa;
	if (r->graph_binding != binding) {
		ret = -ESTALE;
		goto out;
	}
	primary = &r->owner[M9_FAA_PRIMARY_OWNER];
	constants = &r->draw_owner[1];
	secondary = &r->draw_owner[2];
	old_bytes = r->graph_draw_end_bytes;
	if (!primary->cpu || !drm_mm_node_allocated(&primary->system_node) ||
	    primary->system_node.start != primary->base || old_bytes > primary->bytes ||
	    primary->bytes - old_bytes < M9_FAA_NEXT_PRIMARY_BYTES ||
	    !constants->cpu || !secondary->cpu ||
	    r->next_cursor.vertices > constants->bytes ||
	    constants->bytes - r->next_cursor.vertices < M9_FAA_NEXT_VERTEX_STRIDE ||
	    r->next_cursor.secondary > M9_FAA_NEXT_SECONDARY_GUARD - M9_FAA_NEXT_SECONDARY_BYTES ||
	    r->next_cursor.secondary > secondary->bytes ||
	    secondary->bytes - r->next_cursor.secondary < M9_FAA_NEXT_SECONDARY_BYTES + 4) {
		ret = -ESTALE;
		goto out;
	}
	/* Recheck retained SYSTEM mappings, without remapping or reading opaque
	 * GPU output. The requester independently checks the primary PTEs. */
	for (i = 1; i < ARRAY_SIZE(r->draw_owner); i++) {
		struct m9_faa_owner *o = &r->draw_owner[i];

		if (!drm_mm_node_allocated(&o->system_node) ||
		    o->system_node.start != o->base) {
			ret = -ESTALE;
			goto out;
		}
		ret = m9x_system_pages_mapped(mdev, o->base, o->dma,
					      o->bytes / PAGE_SIZE, 1u, NULL, NULL);
		if (ret)
			goto out;
	}
	target = lower_32_bits(unlock_point) + 0x50;
	end = old_bytes + M9_FAA_NEXT_PRIMARY_BYTES;
	fence = m9x_fence_create_target_locked(mdev, target);
	if (!fence) {
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++)
		dma_resv_add_fence(m9x_faa_graph_binding_owner(binding, i)->resv,
				   &fence->base, DMA_RESV_USAGE_WRITE);
	/* Source writes only the new vertex records and command tails. Neither
	 * the earlier constants gap, old command prefix nor private pools change.
	 * The original DRAW wait is unposted; the independent finish posts once. */
	memcpy((u8 *)constants->cpu + r->next_cursor.vertices,
	       r->next_image->vertices, M9_FAA_NEXT_VERTEX_BYTES);
	memcpy((u8 *)secondary->cpu + r->next_cursor.secondary,
	       r->next_image->secondary, M9_FAA_NEXT_SECONDARY_BYTES);
	memcpy((u8 *)primary->cpu + old_bytes,
	       r->next_image->primary, M9_FAA_NEXT_PRIMARY_BYTES);
	wmb();
	ret = m9x_post_faa_graph_end_locked(mdev, &primary->system_node,
			primary->dma, old_bytes, end, false, target, point, posted);
	if (*posted) {
		r->graph_draw_end_bytes = end;
		r->graph_draw_posts++;
	}
	if (!ret && *point != unlock_point + 0x50)
		ret = -EIO;
	if (!ret) {
		r->graph_draw_point = *point;
		r->graph_secondary_cursor += M9_FAA_NEXT_SECONDARY_BYTES;
		r->graph_vertex_cursor += M9_FAA_NEXT_VERTEX_STRIDE;
		r->graph_repeat_draws++;
		dev_info(&mdev->pdev->dev,
			 "M9 FAA repeat draw prefix retired: unlock=%#llx prefix=%#llx bytes=%u posts=%u secondary=%u geometry=%u draw=%u; original DRAW pending\n",
			 unlock_point, *point, end, r->graph_draw_posts,
			 r->graph_secondary_cursor, r->graph_vertex_cursor,
			 r->graph_repeat_draws);
	}
out:
	m9x_transport_fence_finish_locked(mdev, fence, ret, *posted);
	mutex_unlock(&t->lock);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

#ifdef M9_FAA_GRAPH_INIT_CONTROL
static int m9_faa_graph_next_finish_state(struct m9x_transport *t,
		u32 generation, u64 prefix_point, bool claimed)
{
	struct m9x_faa_replay *r = t->faa;

	lockdep_assert_held(&t->lock);
	if (t->wedged)
		return -EIO;
	if (!r || !r->graph_binding || !r->normal_lock_view_seen ||
	    !r->unlock_started || !r->unlock_retired ||
	    !r->graph_next_claimed || !r->next_image ||
	    r->graph_repeat_draws != 1 ||
	    r->graph_next_finish_claimed != claimed || (!!r->next_finish_image != claimed) ||
	    r->graph_generation != generation || t->generation != generation ||
	    r->observation_generation != generation || !r->observation_retired ||
	    !prefix_point || prefix_point != r->unlock_point + 0x50 ||
	    prefix_point != r->observation_point + 0x130 ||
	    r->graph_draw_point != prefix_point || r->graph_draw_posts != 12 ||
	    r->graph_draw_end_bytes != r->primary_bytes +
		M9_FAA_DRAW_PRIMARY_SUFFIX_BYTES + M9_FAA_DRAW_TAIL_PRIMARY_BYTES +
		M9_FAA_LOCK_PRIMARY_BYTES + M9_FAA_UNLOCK_PRIMARY_BYTES +
		M9_FAA_NEXT_PRIMARY_BYTES ||
	    (prefix_point & 15) || lower_32_bits(prefix_point) > U32_MAX - 0x40 ||
	    t->submitted_point != prefix_point || t->completed_point != prefix_point ||
	    t->next_fence != lower_32_bits(prefix_point) + 0x10)
		return -ESTALE;
	if (!t->faa_source_context || !t->faa_virtual_primary ||
	    !t->engine_transitioned || !t->restore_ready || !t->fence_cpu ||
	    !t->transport_armed || !t->bus_master_enabled ||
	    t->async_stop || t->async_pending || t->queue_count)
		return -EBUSY;
	return 0;
}
#endif

int m9x_faa_graph_next_finish_get_locked(struct m9x_device *mdev,
		u32 generation, u64 prefix_point, struct m9_faa_graph_binding **out)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	int ret;

	lockdep_assert_held(&mdev->transport.lock);
	if (!out)
		return -EINVAL;
	*out = NULL;
	ret = m9_faa_graph_next_finish_state(&mdev->transport, generation, prefix_point, false);
	if (!ret)
		*out = m9x_faa_graph_binding_get(mdev->transport.faa->graph_binding);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

/* Authenticate and build the live command image before reserving caller BOs.
 * No new GPU owners or mappings are needed by this source continuation. */
int m9x_faa_graph_next_finish_prepare(struct m9x_device *mdev,
		struct m9_faa_graph_binding *binding, u32 generation,
		u64 prefix_point)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	struct m9_faa_next_finish_storage *storage;
	struct m9x_faa_graph_next_finish_image image;
	struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS];
	const struct firmware *fw;
	u32 i;
	int ret;

	if (!binding)
		return -EINVAL;
	ret = request_firmware(&fw, M9_FAA_GRAPH_NEXT_FINISH_FIRMWARE, &mdev->pdev->dev);
	if (ret)
		return ret;
	if (!m9x_faa_graph_next_finish_blob_valid(fw->data, fw->size)) {
		ret = -EINVAL;
		goto out_fw;
	}
	storage = kmalloc(sizeof(*storage), GFP_KERNEL);
	if (!storage) {
		ret = -ENOMEM;
		goto out_fw;
	}
	mutex_lock(&t->lock);
	ret = m9_faa_graph_next_finish_state(t, generation, prefix_point, false);
	if (ret)
		goto out_lock;
	r = t->faa;
	if (r->graph_binding != binding) {
		ret = -ESTALE;
		goto out_lock;
	}
	for (i = 0; i < M9_FAA_DRAW_SOURCE_OWNERS; i++) {
		struct m9_faa_owner *o;

		switch (i) {
		case 26: o = &r->draw_owner[0]; break;
		case 27: o = &r->owner[26]; break;
		case 28: o = &r->draw_owner[1]; break;
		case 29: o = &r->draw_owner[2]; break;
		case 30: o = &r->owner[27]; break;
		default: o = &r->owner[i]; break;
		}
		owner[i].base = o->base;
		owner[i].bytes = o->bytes;
	}
	image = (struct m9x_faa_graph_next_finish_image) {
		.primary = storage->primary, .primary_bytes = sizeof(storage->primary),
		.secondary = storage->secondary, .secondary_bytes = sizeof(storage->secondary),
	};
	ret = m9x_faa_graph_next_finish_encode(fw->data, fw->size, owner,
			r->graph_draw_end_bytes, prefix_point, &image);
	if (ret)
		goto out_lock;
	/* Claim only a fully encoded image; invalid inputs leave no half-state. */
	r->graph_next_finish_claimed = true;
	r->next_finish_image = storage;
	storage = NULL;
out_lock:
	mutex_unlock(&t->lock);
	kfree(storage);
out_fw:
	release_firmware(fw);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

int m9x_faa_graph_next_finish_submit(struct m9x_device *mdev,
		struct m9_faa_graph_binding *binding, u32 generation,
		u64 prefix_point, u64 *point, bool *posted)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	struct m9_faa_owner *primary, *secondary;
	struct m9x_fence *fence = NULL;
	u32 i, target, old_bytes;
	u64 stage_point;
	bool stage_posted;
	int ret;

	if (!binding || !point || !posted)
		return -EINVAL;
	*point = 0;
	*posted = false;
	mutex_lock(&t->lock);
	ret = m9_faa_graph_next_finish_state(t, generation, prefix_point, true);
	if (ret)
		goto out;
	r = t->faa;
	if (r->graph_binding != binding) {
		ret = -ESTALE;
		goto out;
	}
	primary = &r->owner[M9_FAA_PRIMARY_OWNER];
	secondary = &r->draw_owner[2];
	old_bytes = r->graph_draw_end_bytes;
	if (!primary->cpu || !drm_mm_node_allocated(&primary->system_node) ||
	    primary->system_node.start != primary->base || old_bytes > primary->bytes ||
	    primary->bytes - old_bytes < M9_FAA_NEXT_FINISH_PRIMARY_BYTES ||
	    !secondary->cpu ||
	    secondary->bytes < M9_FAA_NEXT_FINISH_SECONDARY_OFFSET + M9_FAA_NEXT_FINISH_SECONDARY_BYTES) {
		ret = -ESTALE;
		goto out;
	}
	/* Recheck retained SYSTEM mappings, without remapping or reading opaque
	 * GPU output. The requester independently checks the primary PTEs. */
	for (i = 1; i < ARRAY_SIZE(r->draw_owner); i++) {
		struct m9_faa_owner *o = &r->draw_owner[i];

		if (!drm_mm_node_allocated(&o->system_node) ||
		    o->system_node.start != o->base) {
			ret = -ESTALE;
			goto out;
		}
		ret = m9x_system_pages_mapped(mdev, o->base, o->dma,
					      o->bytes / PAGE_SIZE, 1u, NULL, NULL);
		if (ret)
			goto out;
	}
	target = lower_32_bits(prefix_point) + 0x30;
	fence = m9x_fence_create_target_locked(mdev, target);
	if (!fence) {
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++)
		dma_resv_add_fence(m9x_faa_graph_binding_owner(binding, i)->resv,
				   &fence->base, DMA_RESV_USAGE_WRITE);
	/* Preserve the original returned DRAW post followed by the independent
	 * finish. Six reservations retain every owner until the final wait.
	 * Only newly emitted command spans are uploaded; vertex inputs stay. */
	for (i = 0; i < 2; i++) {
		u32 offset = i ? 692 : 0;
		u32 bytes = i ? 16 : 692;
		u32 end = old_bytes + (i + 1) * 12;

		memcpy((u8 *)secondary->cpu + M9_FAA_NEXT_FINISH_SECONDARY_OFFSET + offset,
		       r->next_finish_image->secondary + offset, bytes);
		memcpy((u8 *)primary->cpu + old_bytes + i * 12,
		       r->next_finish_image->primary + i * 12, 12);
		wmb();
		ret = m9x_post_faa_graph_end_locked(mdev, &primary->system_node,
			primary->dma, r->graph_draw_end_bytes, end, false,
			i ? target : 0, &stage_point, &stage_posted);
		*posted |= stage_posted;
		if (stage_posted) {
			r->graph_draw_end_bytes = end;
			r->graph_draw_posts++;
		}
		if (ret)
			goto out;
	}
	*point = stage_point;
	if (!ret && *point != prefix_point + 0x30)
		ret = -EIO;
	if (!ret) {
		r->graph_draw_point = *point;
		r->graph_secondary_cursor += M9_FAA_NEXT_FINISH_SECONDARY_BYTES;
		dev_info(&mdev->pdev->dev,
			 "M9 FAA second returned DRAW finish retired: prefix=%#llx finish=%#llx bytes=%u posts=%u; second Lock pending\n",
			 prefix_point, *point, r->graph_draw_end_bytes, r->graph_draw_posts);
	}
out:
	m9x_transport_fence_finish_locked(mdev, fence, ret, *posted);
	mutex_unlock(&t->lock);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

#ifdef M9_FAA_GRAPH_INIT_CONTROL
static int m9_faa_graph_next_lock_state(struct m9x_transport *t,
		u32 generation, u64 finish_point, bool claimed)
{
	struct m9x_faa_replay *r = t->faa;

	lockdep_assert_held(&t->lock);
	if (t->wedged)
		return -EIO;
	if (!r || !r->graph_binding || !r->normal_lock_view_seen ||
	    !r->unlock_started || !r->unlock_retired ||
	    !r->graph_next_claimed || !r->next_image ||
	    !r->graph_next_finish_claimed || !r->next_finish_image || r->graph_second_lock_retired ||
	    r->graph_next_lock_claimed != claimed || (!!r->next_lock_image != claimed) ||
	    r->graph_generation != generation || t->generation != generation ||
	    r->observation_generation != generation || !r->observation_retired ||
	    !finish_point || finish_point != r->unlock_point + 0x80 ||
	    finish_point != r->observation_point + 0x160 ||
	    r->graph_draw_point != finish_point || r->graph_draw_posts != 14 ||
	    r->graph_draw_end_bytes != r->primary_bytes +
		M9_FAA_DRAW_PRIMARY_SUFFIX_BYTES + M9_FAA_DRAW_TAIL_PRIMARY_BYTES +
		M9_FAA_LOCK_PRIMARY_BYTES + M9_FAA_UNLOCK_PRIMARY_BYTES +
		M9_FAA_NEXT_PRIMARY_BYTES + M9_FAA_NEXT_FINISH_PRIMARY_BYTES ||
	    (finish_point & 15) || lower_32_bits(finish_point) > U32_MAX - 0x40 ||
	    t->submitted_point != finish_point || t->completed_point != finish_point ||
	    t->next_fence != lower_32_bits(finish_point) + 0x10)
		return -ESTALE;
	if (!t->faa_source_context || !t->faa_virtual_primary ||
	    !t->engine_transitioned || !t->restore_ready || !t->fence_cpu ||
	    !t->transport_armed || !t->bus_master_enabled ||
	    t->async_stop || t->async_pending || t->queue_count)
		return -EBUSY;
	return 0;
}
#endif

int m9x_faa_graph_next_lock_get_locked(struct m9x_device *mdev,
		u32 generation, u64 finish_point, struct m9_faa_graph_binding **out)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	int ret;

	lockdep_assert_held(&mdev->transport.lock);
	if (!out)
		return -EINVAL;
	*out = NULL;
	ret = m9_faa_graph_next_lock_state(&mdev->transport, generation, finish_point, false);
	if (!ret)
		*out = m9x_faa_graph_binding_get(mdev->transport.faa->graph_binding);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

/* Authenticate and build the live command image before reserving caller BOs.
 * No new GPU owners or mappings are needed by this source continuation. */
int m9x_faa_graph_next_lock_prepare(struct m9x_device *mdev,
		struct m9_faa_graph_binding *binding, u32 generation,
		u64 finish_point)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	u8 *storage;
	struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS];
	const struct firmware *fw;
	u32 i;
	int ret;

	if (!binding)
		return -EINVAL;
	ret = request_firmware(&fw, M9_FAA_GRAPH_LOCK_FIRMWARE, &mdev->pdev->dev);
	if (ret)
		return ret;
	if (!m9x_faa_graph_lock_blob_valid(fw->data, fw->size)) {
		ret = -EINVAL;
		goto out_fw;
	}
	storage = kmalloc(M9_FAA_LOCK_PRIMARY_BYTES, GFP_KERNEL);
	if (!storage) {
		ret = -ENOMEM;
		goto out_fw;
	}
	mutex_lock(&t->lock);
	ret = m9_faa_graph_next_lock_state(t, generation, finish_point, false);
	if (ret)
		goto out_lock;
	r = t->faa;
	if (r->graph_binding != binding) {
		ret = -ESTALE;
		goto out_lock;
	}
	for (i = 0; i < M9_FAA_DRAW_SOURCE_OWNERS; i++) {
		struct m9_faa_owner *o;

		switch (i) {
		case 26: o = &r->draw_owner[0]; break;
		case 27: o = &r->owner[26]; break;
		case 28: o = &r->draw_owner[1]; break;
		case 29: o = &r->draw_owner[2]; break;
		case 30: o = &r->owner[27]; break;
		default: o = &r->owner[i]; break;
		}
		owner[i].base = o->base;
		owner[i].bytes = o->bytes;
	}
	ret = m9x_faa_graph_lock_encode(fw->data, fw->size, owner,
			r->graph_draw_end_bytes, 320, 240, finish_point,
			storage, M9_FAA_LOCK_PRIMARY_BYTES);
	if (ret)
		goto out_lock;
	/* Claim only a fully encoded image; invalid inputs leave no half-state. */
	r->graph_next_lock_claimed = true;
	r->next_lock_image = storage;
	storage = NULL;
out_lock:
	mutex_unlock(&t->lock);
	kfree(storage);
out_fw:
	release_firmware(fw);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

int m9x_faa_graph_next_lock_submit(struct m9x_device *mdev,
		struct m9_faa_graph_binding *binding, u32 generation,
		u64 finish_point, u64 *point, bool *posted)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	struct m9_faa_owner *primary, *secondary;
	struct m9x_fence *fence = NULL;
	u32 i, target, old_bytes;
	u64 stage_point;
	bool stage_posted;
	int ret;

	if (!binding || !point || !posted)
		return -EINVAL;
	*point = 0;
	*posted = false;
	mutex_lock(&t->lock);
	ret = m9_faa_graph_next_lock_state(t, generation, finish_point, true);
	if (ret)
		goto out;
	r = t->faa;
	if (r->graph_binding != binding) {
		ret = -ESTALE;
		goto out;
	}
	primary = &r->owner[M9_FAA_PRIMARY_OWNER];
	secondary = &r->draw_owner[2];
	old_bytes = r->graph_draw_end_bytes;
	if (!primary->cpu || !drm_mm_node_allocated(&primary->system_node) ||
	    primary->system_node.start != primary->base || old_bytes > primary->bytes ||
	    primary->bytes - old_bytes < M9_FAA_LOCK_PRIMARY_BYTES ||
	    !secondary->cpu ||
	    secondary->bytes < M9_FAA_NEXT_FINISH_SECONDARY_OFFSET + M9_FAA_NEXT_FINISH_SECONDARY_BYTES) {
		ret = -ESTALE;
		goto out;
	}
	/* Recheck retained SYSTEM mappings, without remapping or reading opaque
	 * GPU output. The requester independently checks the primary PTEs. */
	for (i = 1; i < ARRAY_SIZE(r->draw_owner); i++) {
		struct m9_faa_owner *o = &r->draw_owner[i];

		if (!drm_mm_node_allocated(&o->system_node) ||
		    o->system_node.start != o->base) {
			ret = -ESTALE;
			goto out;
		}
		ret = m9x_system_pages_mapped(mdev, o->base, o->dma,
					      o->bytes / PAGE_SIZE, 1u, NULL, NULL);
		if (ret)
			goto out;
	}
	target = lower_32_bits(finish_point) + 0x30;
	fence = m9x_fence_create_target_locked(mdev, target);
	if (!fence) {
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++)
		dma_resv_add_fence(m9x_faa_graph_binding_owner(binding, i)->resv,
				   &fence->base, DMA_RESV_USAGE_WRITE);
	/* The source normal Lock emits its first populated prefix twice,
	 * then appends its completion packet. No earlier prefix is overwritten.
	 * Six write fences cover all three posts and the final wait. */
	for (i = 0; i < 3; i++) {
		u32 end = old_bytes + (i == 2 ? M9_FAA_LOCK_PRIMARY_BYTES :
				M9_FAA_LOCK_FIRST_POST_BYTES);

		if (i == 0)
			memcpy((u8 *)primary->cpu + old_bytes, r->next_lock_image,
			       M9_FAA_LOCK_FIRST_POST_BYTES);
		else if (i == 2)
			memcpy((u8 *)primary->cpu + old_bytes + M9_FAA_LOCK_FIRST_POST_BYTES,
			       r->next_lock_image + M9_FAA_LOCK_FIRST_POST_BYTES,
			       M9_FAA_LOCK_PRIMARY_BYTES - M9_FAA_LOCK_FIRST_POST_BYTES);
		wmb();
		ret = m9x_post_faa_graph_end_locked(mdev, &primary->system_node,
			primary->dma, r->graph_draw_end_bytes, end, i == 1,
			i == 2 ? target : 0, &stage_point, &stage_posted);
		*posted |= stage_posted;
		if (stage_posted) {
			r->graph_draw_end_bytes = end;
			r->graph_draw_posts++;
		}
		if (ret)
			goto out;
	}
	*point = stage_point;
	if (!ret && *point != finish_point + 0x30)
		ret = -EIO;
	if (!ret) {
		r->graph_draw_point = *point;
		r->graph_second_lock_retired = true;
		dev_info(&mdev->pdev->dev,
			 "M9 FAA second normal Lock prefix retired: finish=%#llx lock=%#llx bytes=%u posts=%u; returned Lock and pixels unproved\n",
			 finish_point, *point, r->graph_draw_end_bytes, r->graph_draw_posts);
	}
out:
	m9x_transport_fence_finish_locked(mdev, fence, ret, *posted);
	mutex_unlock(&t->lock);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

/* Continue only the original normal Unlock after the returned 0x180 Lock
 * view has been copied. The full source-known primary was retained at first
 * submit; only its final END and actual 0x1a0 fence are new here. */
int m9x_faa_unlock_diag(struct m9x_device *mdev, u32 color,
			struct drm_gem_vram_object *color_bo, u64 *point,
			bool *posted)
{
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	struct m9x_fence *fence = NULL;
	u32 old_bytes, new_bytes, target;
#ifdef M9_FAA_RELOCK_PUBLICATION_CONTROL
	const u32 unlock_source_bytes = M9_FAA_NORMAL_UNLOCK_SPLIT_BYTES;
	const u32 unlock_source_target = M9_FAA_NORMAL_UNLOCK_SPLIT_TARGET;
#else
	const u32 unlock_source_bytes = M9_FAA_PRIMARY_BYTES;
	const u32 unlock_source_target = M9_FAA_TRACKER_LAST;
#endif
	s64 delta;
	int ret;

	if (!point || !posted)
		return -EINVAL;
	*point = 0;
	*posted = false;
	mutex_lock(&t->lock);
	r = t->faa;
#ifdef M9_FAA_UNLOCK_PUBLICATION_CONTROL
	if (!m9_faa_diag || !r || !r->prepared || !r->attempted ||
	    !r->observation_retired || !r->normal_lock_view_seen ||
	    r->unlock_started || !t->faa_virtual_primary || t->async_stop ||
	    t->async_pending || t->queue_count || !color_bo ||
	    r->retained_color != color_bo || r->borrowed_color_bytes != 0x50000 ||
	    color_bo->bo.base.size != 0x50000 ||
	    r->owner[M9_FAA_COLOR_OWNER].base != color ||
	    r->observation_generation != t->generation ||
	    r->observation_point != t->completed_point ||
	    t->next_fence < 0x10 ||
	    lower_32_bits(r->observation_point) != t->next_fence - 0x10 ||
	    M9_FAA_NORMAL_LOCK_SPLIT_TARGET != 0x180 ||
	    unlock_source_target != 0x1a0 ||
#ifdef M9_FAA_RELOCK_PUBLICATION_CONTROL
	    unlock_source_bytes >= M9_FAA_PRIMARY_BYTES ||
	    M9_FAA_TRACKER_LAST != 0x1c0 ||
#else
	    unlock_source_bytes != M9_FAA_PRIMARY_BYTES ||
	    M9_FAA_TRACKER_LAST != 0x1a0 ||
#endif
	    M9_FAA_NORMAL_LOCK_SPLIT_BYTES >= unlock_source_bytes) {
		ret = -ENODATA;
		goto unlock;
	}
	if (t->wedged) {
		ret = -EIO;
		goto unlock;
	}
	delta = (s64)r->primary_bytes - M9_FAA_PRIMARY_BYTES;
	if ((s64)M9_FAA_NORMAL_LOCK_SPLIT_BYTES + delta <= 0 ||
	    (s64)M9_FAA_NORMAL_LOCK_SPLIT_BYTES + delta > U32_MAX ||
	    (s64)unlock_source_bytes + delta <= 0 ||
	    (s64)unlock_source_bytes + delta > U32_MAX ||
	    r->primary_bytes > r->owner[M9_FAA_PRIMARY_OWNER].bytes ||
	    !drm_mm_node_allocated(&r->owner[M9_FAA_PRIMARY_OWNER].system_node)) {
		ret = -ERANGE;
		goto unlock;
	}
	old_bytes = M9_FAA_NORMAL_LOCK_SPLIT_BYTES + delta;
	new_bytes = unlock_source_bytes + delta;
	if (old_bytes >= new_bytes || new_bytes > r->primary_bytes ||
	    new_bytes - old_bytes !=
			unlock_source_bytes - M9_FAA_NORMAL_LOCK_SPLIT_BYTES ||
	    t->next_fence > U32_MAX -
			(unlock_source_target - M9_FAA_NORMAL_LOCK_SPLIT_TARGET)) {
		ret = -ERANGE;
		goto unlock;
	}
	target = t->next_fence - 0x10 +
		(unlock_source_target - M9_FAA_NORMAL_LOCK_SPLIT_TARGET);
	ret = m9x_transport_drain_locked(mdev);
	if (ret)
		goto unlock;
	/* This is the original live requester, not another engine activation.
	 * A second readiness path could emit unrelated setup before Unlock. */
	if (!t->transport_armed || !t->restore_ready ||
	    !t->bus_master_enabled || !t->engine_transitioned) {
		ret = -ESTALE;
		goto unlock;
	}
	ret = m9x_validate_faa_virtual_resume_locked(mdev,
			&r->owner[M9_FAA_PRIMARY_OWNER].system_node,
			r->owner[M9_FAA_PRIMARY_OWNER].dma, old_bytes,
			new_bytes, target);
	if (ret)
		goto unlock;
	fence = m9x_fence_create_target_locked(mdev, target);
	if (!fence) {
		ret = -ENOMEM;
		goto unlock;
	}
	dma_resv_add_fence(color_bo->bo.base.resv, &fence->base,
			   DMA_RESV_USAGE_WRITE);
	/* Actual a66f0/55a40 release: the CPU-root leaves and descriptors
	 * remain cached, while 2c878 writes 31c=0 before the copyback post. */
	r->unlock_started = true;
	m9x_write(mdev, M9_REG_FRONTEND_STATE, 0);
	ret = m9x_extend_faa_virtual_primary_locked(mdev,
			&r->owner[M9_FAA_PRIMARY_OWNER].system_node,
			r->owner[M9_FAA_PRIMARY_OWNER].dma, old_bytes,
			new_bytes, target, point, posted);
	if (!ret) {
		t->next_fence = target + 0x10;
		r->unlock_retired = true;
		r->unlock_point = *point;
		r->ordinary_point = 0;
		m9_faa_observe(mdev, r, false);
		m9_faa_observe_borrowed_color(mdev, r, color_bo, false);
		dev_info(&mdev->pdev->dev,
			 "M9 FAA normal Unlock retired: lock=%#llx unlock=%#llx primary_bytes=%u copyback=F021->F022\n",
			 r->observation_point, *point, r->primary_bytes);
	}
#else
	(void)r;
	(void)color;
	(void)color_bo;
	ret = -EOPNOTSUPP;
#endif
unlock:
	if (fence && ret)
		dma_fence_set_error(&fence->base, ret);
	if (fence && ret && *posted) {
		t->wedged = true;
		t->generation++;
		WARN_ON(t->quarantined_fence);
		t->quarantined_fence = fence;
		fence = NULL;
	} else if (fence) {
		dma_fence_signal(&fence->base);
	}
	if (fence)
		dma_fence_put(&fence->base);
	mutex_unlock(&t->lock);
	return ret;
}

/* The next original public Lock is a separate primary F022 -> staging F021
 * conversion. It consumes the genuine Unlock receipt before submitting only
 * the source-derived final suffix on the same retained command owner. */
int m9x_faa_relock_diag(struct m9x_device *mdev, u32 color,
			struct drm_gem_vram_object *color_bo, u64 *point,
			bool *posted)
{
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	struct m9x_fence *fence = NULL;
	u32 old_bytes, target;
	s64 delta;
	int ret;

	if (!point || !posted)
		return -EINVAL;
	*point = 0;
	*posted = false;
	mutex_lock(&t->lock);
	r = t->faa;
#ifdef M9_FAA_RELOCK_PUBLICATION_CONTROL
	if (!m9_faa_diag || !r || !r->prepared || !r->attempted ||
	    !r->observation_retired || !r->normal_lock_view_seen ||
	    !r->unlock_started || !r->unlock_retired || !r->unlock_point ||
	    r->relock_started || !t->faa_virtual_primary || t->async_stop ||
	    t->async_pending || t->queue_count || !color_bo ||
	    r->retained_color != color_bo || r->borrowed_color_bytes != 0x50000 ||
	    color_bo->bo.base.size != 0x50000 ||
	    r->owner[M9_FAA_COLOR_OWNER].base != color ||
	    r->observation_generation != t->generation ||
	    r->unlock_point != t->completed_point || t->next_fence < 0x10 ||
	    lower_32_bits(r->unlock_point) != t->next_fence - 0x10 ||
	    M9_FAA_NORMAL_UNLOCK_SPLIT_TARGET != 0x1a0 ||
	    M9_FAA_TRACKER_LAST != 0x1c0 ||
	    M9_FAA_NORMAL_UNLOCK_SPLIT_BYTES >= M9_FAA_PRIMARY_BYTES) {
		ret = -ENODATA;
		goto unlock;
	}
	if (t->wedged) {
		ret = -EIO;
		goto unlock;
	}
	delta = (s64)r->primary_bytes - M9_FAA_PRIMARY_BYTES;
	if ((s64)M9_FAA_NORMAL_UNLOCK_SPLIT_BYTES + delta <= 0 ||
	    (s64)M9_FAA_NORMAL_UNLOCK_SPLIT_BYTES + delta > U32_MAX ||
	    r->primary_bytes > r->owner[M9_FAA_PRIMARY_OWNER].bytes ||
	    !drm_mm_node_allocated(&r->owner[M9_FAA_PRIMARY_OWNER].system_node)) {
		ret = -ERANGE;
		goto unlock;
	}
	old_bytes = M9_FAA_NORMAL_UNLOCK_SPLIT_BYTES + delta;
	if (old_bytes >= r->primary_bytes ||
	    r->primary_bytes - old_bytes !=
			M9_FAA_PRIMARY_BYTES - M9_FAA_NORMAL_UNLOCK_SPLIT_BYTES ||
	    t->next_fence > U32_MAX -
			(M9_FAA_TRACKER_LAST - M9_FAA_NORMAL_UNLOCK_SPLIT_TARGET)) {
		ret = -ERANGE;
		goto unlock;
	}
	target = t->next_fence - 0x10 +
		(M9_FAA_TRACKER_LAST - M9_FAA_NORMAL_UNLOCK_SPLIT_TARGET);
	ret = m9x_transport_drain_locked(mdev);
	if (ret)
		goto unlock;
	if (!t->transport_armed || !t->restore_ready ||
	    !t->bus_master_enabled || !t->engine_transitioned) {
		ret = -ESTALE;
		goto unlock;
	}
	ret = m9x_validate_faa_virtual_resume_locked(mdev,
			&r->owner[M9_FAA_PRIMARY_OWNER].system_node,
			r->owner[M9_FAA_PRIMARY_OWNER].dma, old_bytes,
			r->primary_bytes, target);
	if (ret)
		goto unlock;
	fence = m9x_fence_create_target_locked(mdev, target);
	if (!fence) {
		ret = -ENOMEM;
		goto unlock;
	}
	dma_resv_add_fence(color_bo->bo.base.resv, &fence->base,
			   DMA_RESV_USAGE_WRITE);
	r->relock_started = true;
	ret = m9x_extend_faa_virtual_primary_locked(mdev,
			&r->owner[M9_FAA_PRIMARY_OWNER].system_node,
			r->owner[M9_FAA_PRIMARY_OWNER].dma, old_bytes,
			r->primary_bytes, target, point, posted);
	if (!ret) {
		t->next_fence = target + 0x10;
		r->relock_retired = true;
		r->relock_point = *point;
		r->relock_generation = t->generation;
		m9_faa_observe(mdev, r, false);
		m9_faa_observe_borrowed_color(mdev, r, color_bo, false);
		dev_info(&mdev->pdev->dev,
			 "M9 FAA second normal Lock conversion retired: unlock=%#llx relock=%#llx primary_bytes=%u copyback=F022->F021\n",
			 r->unlock_point, *point, r->primary_bytes);
	}
#else
	(void)r;
	(void)color;
	(void)color_bo;
	ret = -EOPNOTSUPP;
#endif
unlock:
	if (fence && ret)
		dma_fence_set_error(&fence->base, ret);
	if (fence && ret && *posted) {
		t->wedged = true;
		t->generation++;
		WARN_ON(t->quarantined_fence);
		t->quarantined_fence = fence;
		fence = NULL;
	} else if (fence) {
		dma_fence_signal(&fence->base);
	}
	if (fence)
		dma_fence_put(&fence->base);
	mutex_unlock(&t->lock);
	return ret;
}

/* Explicit internal ac1f0 output contract, below unjoined public Lock
 * admission. This never reinterprets the sampled F022 host-aperture view.
 * The actual kind6 flags30 system owner is read through its retained DMA
 * alias after real retirement; 8b020/8c4a0 supplies each source address. */
int m9x_faa_output_read(struct m9x_device *mdev, struct drm_file *file,
		       const struct drm_parhelia_m9_surface_access *args)
{
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	struct m9_faa_owner *output;
	struct drm_gem_object *gem;
	u8 *pixels;
	u64 end;
	u32 x, y, offset;
	int ret = 0;

	if (!m9_faa_diag || args->flags != PARHELIA_M9_ACCESS_FAA_OUTPUT ||
	    args->op != PARHELIA_M9_ACCESS_READ || args->offset ||
	    args->descriptor0 != 0xc8210140 || args->descriptor1 != 0x100f0 ||
	    args->surface_width != 320 || args->width != 320 || args->height != 240 ||
	    args->x || args->y || args->data_stride != 1280 ||
	    args->data_size < 320 * 240 * 4 ||
	    check_add_overflow(args->data_ptr, (u64)(320 * 240 * 4), &end))
		return -EINVAL;
	gem = drm_gem_object_lookup(file, args->handle);
	if (!gem)
		return -ENOENT;
	pixels = kvmalloc(320 * 240 * 4, GFP_KERNEL);
	if (!pixels) {
		ret = -ENOMEM;
		goto put;
	}
	mutex_lock(&t->lock);
	r = t->faa;
	output = r ? m9_faa_output_owner(r) : NULL;
	if (t->wedged || !r || !output || !r->output_retired ||
	    r->output_color != gem || r->output_generation != t->generation ||
	    !r->output_point || !output->cpu || output->bytes != 0x50000 ||
	    !drm_mm_node_allocated(&output->system_node) ||
	    output->system_node.size != output->bytes ||
	    output->system_node.start != output->base) {
		ret = t->wedged ? -EIO : -ENODATA;
		goto unlock;
	}
	/* Actual aff90 calls 2cb40(resource, 0, 1). After its query/write
	 * waits succeed, 2cba8 kicks the frontend before selecting the kind1
	 * CPU alias and copying pixels. Keep that transition after the live
	 * retirement/owner gates; a pre-submit kick is a different ordering.
	 */
	m9x_write(mdev, M9_REG_FRONTEND_KICK, 1);
	/* WC loads need a CPU read barrier after the retained retirement
	 * gate; dma_rmb alone is a compiler barrier on x86. */
	rmb();
	dma_rmb();
	for (y = 0; y < 240; y++)
		for (x = 0; x < 320; x++) {
			ret = m9_faa_output_offset(x, y, &offset);
			if (ret || offset > output->bytes - 4) {
				ret = -ERANGE;
				goto unlock;
			}
			/* Hardware format64 is A8B8G8R8: little-endian RGBA.
			 * There is no CPU color synthesis or sampled-data decoder. */
			memcpy(pixels + (y * 320 + x) * 4,
			       (u8 *)output->cpu + offset, 4);
		}
	dev_info(&mdev->pdev->dev,
		 "M9 FAA output read: point=%#llx system=%#x descriptor=c8210140/100f0 RGBA 320x240 post_wait_kick=1\n",
		 r->output_point, output->base);
unlock:
	mutex_unlock(&t->lock);
	if (!ret && copy_to_user(u64_to_user_ptr(args->data_ptr), pixels, 320 * 240 * 4))
		ret = -EFAULT;
	kvfree(pixels);
put:
	drm_gem_object_put(gem);
	return ret;
}

/* A repeated COLOR frame owns the same graph/private context. Previous Unlock
 * must have retired completely before any retained code/data owner is rewritten.
 * This admission does not rebase a timeline or clear a reservation hazard. */
static int
m9_faa_color_unlocked_origin_get_locked(struct m9x_device *mdev, u32 generation,
					u64 previous_unlock_point, u64 origin,
					bool pool_scope, bool allow_two,
					struct m9_faa_graph_binding **out)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r = t->faa;

	lockdep_assert_held(&t->lock);
	if (!out)
		return -EINVAL;
	*out = NULL;
	if (t->wedged)
		return -EIO;
	if (!r || !r->graph_binding || !r->color_payload_ready ||
	    !r->color_image ||
	    (r->color_image->layout.program_count != 4 &&
	     !(allow_two && r->color_image->layout.program_count == 2)) ||
	    !r->color_attempted || !r->color_lock_retired ||
	    !r->color_lock_point || !r->normal_lock_view_seen ||
	    !r->unlock_started || !r->unlock_retired ||
	    (!pool_scope && r->command_pools && r->command_pools->rotated) ||
	    r->public_vs_image || r->draw_image ||
	    r->graph_generation != generation || t->generation != generation ||
	    r->observation_generation != generation ||
	    !r->observation_retired || !previous_unlock_point ||
	    (previous_unlock_point & 15) ||
	    lower_32_bits(previous_unlock_point) > U32_MAX - 0x100 ||
	    previous_unlock_point != r->color_lock_point + 0x20 ||
	    r->unlock_point != previous_unlock_point ||
	    r->graph_draw_point != previous_unlock_point ||
	    r->present_attempted != r->present_retired ||
	    (r->present_point && !r->present_retired) ||
	    (r->present_attempted &&
	     (!pool_scope || !r->present_retired ||
	      r->present_generation != generation ||
	      r->present_point != previous_unlock_point + 0x20 ||
	      r->present_posts != r->graph_unlock_posts + 3 ||
	      r->present_end != r->graph_unlock_end_bytes + 100)) ||
	    t->submitted_point != origin || t->completed_point != origin ||
	    t->next_fence != lower_32_bits(origin) + 0x10 ||
	    (origin == m9_faa_idle_origin(r, previous_unlock_point) &&
			     !(pool_scope && r->command_pools &&
			       r->command_pools->unlock_point ==
				       previous_unlock_point) ?
		     (r->graph_draw_end_bytes != m9_faa_idle_end(r) ||
		      r->graph_draw_posts != m9_faa_idle_posts(r) ||
		      !r->color_secondary_cursor) :
		     (!r->command_pools || !r->command_pools->ready ||
		      r->command_pools->generation != generation ||
		      r->command_pools->unlock_point != previous_unlock_point ||
		      m9_faa_frame_origin(r, previous_unlock_point) != origin ||
		      (origin != m9_faa_idle_origin(r, previous_unlock_point) &&
		       origin != m9_faa_idle_origin(r, previous_unlock_point) +
					 0x10) ||
		      r->command_pools->unlock_end != m9_faa_idle_end(r) ||
		      r->command_pools->unlock_posts != m9_faa_idle_posts(r) ||
		      r->graph_draw_end_bytes != r->command_pools->parent_end ||
		      r->graph_draw_posts !=
			      m9_faa_idle_posts(r) + r->command_pools->posts ||
		      r->color_secondary_cursor !=
			      r->command_pools->secondary_cursor)) ||
	    r->color_next_attempt_point == previous_unlock_point)
		return -ESTALE;
	if (!t->faa_source_context || !t->faa_virtual_primary ||
	    !t->engine_transitioned || !t->restore_ready ||
	    !t->transport_armed || !t->bus_master_enabled || !t->fence_cpu ||
	    t->async_stop || t->async_pending || t->queue_count)
		return -EBUSY;
	*out = m9x_faa_graph_binding_get(r->graph_binding);
	return 0;
#else
	return -EOPNOTSUPP;
#endif
}

int m9x_faa_graph_color_unlocked_get_locked(struct m9x_device *mdev,u32 generation,
 u64 previous,struct m9_faa_graph_binding **out)
{return m9_faa_color_unlocked_origin_get_locked(mdev,generation,previous,previous,false,false,out);}
/* Textured later frames continue after ordinary jobs, as depth ones do. */
int m9x_faa_graph_dual_unlocked_get_locked(struct m9x_device *mdev,
					   u32 generation, u64 previous,
					   struct m9_faa_graph_binding **out)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_faa_replay *r = mdev->transport.faa;

	if (r && m9_faa_ordinary_after(r, m9_faa_present_origin(r, previous)) &&
	    (mdev->transport.queue_count || mdev->transport.async_pending)) {
		int ret = m9x_transport_drain_locked(mdev);

		if (ret)
			return ret;
	}
	return m9_faa_color_unlocked_origin_get_locked(mdev, generation,
						       previous,
						       r ? m9_faa_frame_origin(r, previous) :
							   previous,
						       false, false, out);
#else
	return -EOPNOTSUPP;
#endif
}

int m9x_faa_graph_depth_unlocked_get_locked(struct m9x_device *mdev,
					    u32 generation, u64 previous,
					    struct m9_faa_graph_binding **out)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_faa_replay *r = mdev->transport.faa;
	u64 origin;

	/* Ordinary jobs after this idle origin retire before the FAA frame. */
	if (r && m9_faa_ordinary_after(r, m9_faa_present_origin(r, previous)) &&
	    (mdev->transport.queue_count || mdev->transport.async_pending)) {
		int ret = m9x_transport_drain_locked(mdev);

		if (ret)
			return ret;
	}
	origin = r ? m9_faa_frame_origin(r, previous) : previous;
	return m9_faa_color_unlocked_origin_get_locked(mdev, generation,
						       previous, origin, true,
						       true, out);
#else
	return -EOPNOTSUPP;
#endif
}

int m9x_faa_graph_color_next_prepare(struct m9x_device *mdev,
 struct m9_faa_graph_binding *binding,u32 generation,u64 previous_unlock_point,
 const struct m9x_faa_color_input *input,u32 clear_flags)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 struct m9x_transport *t=&mdev->transport;struct m9x_faa_replay *r;
 struct m9_faa_color_storage *n=NULL,*old=NULL;struct m9_faa_graph_binding *checked=NULL;
 struct m9x_faa_graph_draw_owner owner[31];struct m9x_faa_color_next_image image;
 const struct firmware *fw=NULL;u8 *scratch=NULL;u32 i,capacity;int ret;
 if(!binding||!input||!m9_faa_color_four_experimental||input->layout.program_count!=4||clear_flags>1)return -EOPNOTSUPP;
 if(input->layout.program_bytes>4096||input->layout.constants_bytes>16384||
  (input->record_stride!=32&&(input->record_stride!=16||input->declaration_count!=3))||
  !input->record_count||input->record_count%3||input->record_count>12288||
  input->layout.geometry_bytes>0x40000-256)return -ERANGE;
 n=kzalloc(sizeof(*n),GFP_KERNEL);if(!n)return -ENOMEM;
 n->geometry_offset=ALIGN(input->layout.constants_bytes,128);
 capacity=n->geometry_offset+input->layout.geometry_bytes+768;
 n->constants=kzalloc(capacity,GFP_KERNEL);if(!n->constants){ret=-ENOMEM;goto release;}
 scratch=kmalloc(M9_FAA_COLOR_SECONDARY_CAPACITY,GFP_KERNEL);if(!scratch){ret=-ENOMEM;goto release;}
 ret=request_firmware(&fw,M9_FAA_GRAPH_COLOR_FOUR_FIRMWARE,&mdev->pdev->dev);if(ret)goto release;
 mutex_lock(&t->lock);
 ret=m9x_faa_graph_depth_unlocked_get_locked(mdev,generation,previous_unlock_point,&checked);if(ret)goto unlock;
 r=t->faa;if(checked!=binding){ret=-ESTALE;goto unlock;}
 /* The historical color-only NEXT grammar is four-stage. Its retained
  * source does not authorize changing a two-stage first frame into a
  * four-stage successor merely because Unlock completed. */
 if(!r->color_image||r->color_image->layout.program_count!=4){ret=-EOPNOTSUPP;goto unlock;}
 if(r->draw_owner[0].bytes<input->layout.program_bytes||!r->draw_owner[0].vram){ret=-ERANGE;goto unlock;}
 for(i=0;i<31;i++){
  struct m9_faa_owner *o;
  switch(i){case 26:o=&r->draw_owner[0];break;case 27:o=m9_faa_command_owner(r,false);break;
   case 28:o=&r->draw_owner[1];break;case 29:o=m9_faa_command_owner(r,true);break;
   case 30:o=&r->owner[27];break;default:o=&r->owner[i];break;}
  if(!o){ret=-ESTALE;goto unlock;}
  owner[i]=(struct m9x_faa_graph_draw_owner){o->base,o->bytes};
 }
 image=(struct m9x_faa_color_next_image){
  .image={n->program,n->constants,n->primary,n->secondary,sizeof(n->program),capacity,sizeof(n->primary),sizeof(n->secondary),&n->secondary_bytes},
  .clear=n->clear,.clear_capacity=sizeof(n->clear),.clear_length=&n->clear_bytes,
  .secondary_scratch=scratch,.secondary_scratch_capacity=M9_FAA_COLOR_SECONDARY_CAPACITY};
 ret=m9x_faa_color_next_encode(fw->data,fw->size,owner,r->primary_bytes,
  r->color_secondary_cursor,m9_faa_frame_origin(r,previous_unlock_point),input,clear_flags,&image);if(ret)goto unlock;
 if((u64)r->graph_draw_end_bytes+n->clear_bytes+M9_FAA_COLOR_NEXT_PRIMARY_BYTES>owner[27].bytes){ret=-ENOSPC;goto unlock;}
 n->layout=input->layout;n->constants_bytes=input->layout.constants_bytes;n->geometry_bytes=128+ALIGN(input->record_count,4)*input->record_stride;
 n->program_bytes=input->layout.program_bytes;n->primary_length=M9_FAA_COLOR_NEXT_PRIMARY_BYTES;n->draw_length=60;
 n->lock_delta=0xd0+(clear_flags?0x10:0);n->secondary_cursor=r->color_secondary_cursor;
 n->previous_unlock_point=previous_unlock_point;
 old=r->color_image;r->color_image=n;n=NULL;ret=0;
unlock:mutex_unlock(&t->lock);m9x_faa_graph_binding_put(checked);
release:kfree(scratch);if(fw)release_firmware(fw);if(n)kfree(n->constants);kfree(n);if(old)kfree(old->constants);kfree(old);return ret;
#else
 return -EOPNOTSUPP;
#endif
}

int m9x_faa_graph_color_next_submit(struct m9x_device *mdev,
 struct m9_faa_graph_binding *binding,u32 generation,u64 previous_unlock_point,u64 *point,bool *posted)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 struct m9x_transport *t=&mdev->transport;struct m9x_faa_replay *r;
 struct m9_faa_color_storage *n;struct m9_faa_owner *primary,*program;
 struct m9_faa_graph_binding *checked=NULL;struct m9x_fence *fence=NULL;u8 observed[4096];
	u32 i, start, end;
	u64 stage_point = 0;
	bool stage_posted = false;
	int ret;
 if(!point||!posted||!binding)return -EINVAL;*point=0;*posted=false;
 mutex_lock(&t->lock);
 ret=m9x_faa_graph_color_unlocked_get_locked(mdev,generation,previous_unlock_point,&checked);if(ret)goto out;
 r=t->faa;n=r->color_image;
 if(checked!=binding||!n||n->previous_unlock_point!=previous_unlock_point||n->secondary_cursor!=r->color_secondary_cursor){ret=-ESTALE;goto out;}
 primary=&r->owner[M9_FAA_PRIMARY_OWNER];program=&r->draw_owner[0];start=r->graph_draw_end_bytes;
 if(!primary->cpu||!program->vram||n->program_bytes>sizeof(observed)||
  (u64)start+n->clear_bytes+n->primary_length>primary->bytes||
  (u64)n->secondary_cursor+n->secondary_bytes+48>0x3fe00){ret=-ENOSPC;goto out;}
 for(i=0;i<PARHELIA_M9_FAA_GRAPH_OWNERS;i++){
  struct drm_gem_object *bo=m9x_faa_graph_binding_owner(binding,i);dma_resv_assert_held(bo->resv);
  if(!dma_resv_test_signaled(bo->resv,DMA_RESV_USAGE_READ)){ret=-EBUSY;goto out;}}
 for(i=0;i<3;i++){
  struct m9_faa_owner *o=i?&r->draw_owner[i]:primary;
  if(!o->cpu||!drm_mm_node_allocated(&o->system_node)||o->system_node.start!=o->base){ret=-ESTALE;goto out;}
		ret = m9x_system_pages_mapped(mdev, o->base, o->dma,
					      o->bytes / PAGE_SIZE, 1u, NULL, NULL);
		if (ret)
			goto out;
	}
 ret=m9x_address_check_local_identity(mdev,program->base,program->bytes);if(ret)goto out;
 ret=m9x_triangle_system_page_admit_locked(mdev,0x40004000);if(ret)goto out;
 ret=m9x_triangle_system_page_admit_locked(mdev,0x40008000);if(ret)goto out;
 fence=m9x_fence_create_target_locked(mdev,lower_32_bits(previous_unlock_point)+n->lock_delta);
 if(!fence){ret=-ENOMEM;goto out;}
 for(i=0;i<PARHELIA_M9_FAA_GRAPH_OWNERS;i++)dma_resv_add_fence(m9x_faa_graph_binding_owner(binding,i)->resv,&fence->base,DMA_RESV_USAGE_WRITE);
 /* Invalidate the previous CPU-view entitlement before replacing its code
  * or storage. The new final Lock must re-admit the same owned F021 view. */
 r->color_next_attempt_point=previous_unlock_point;r->color_lock_retired=false;
 r->normal_lock_view_seen=false;r->unlock_started=false;r->unlock_retired=false;
 m9x_write(mdev,M9_REG_FRONTEND_KICK,1);
 ret=m9x_address_cpu_local_prepare(mdev,program->base,program->bytes);if(ret)goto out;
 iosys_map_memcpy_to(&program->map,0,n->program,n->program_bytes);wmb();
 m9x_write(mdev,M9_REG_FRONTEND_STATE,0);m9x_read(mdev,M9_REG_FRONTEND_STATE);
 iosys_map_memcpy_from(observed,&program->map,0,n->program_bytes);
 if(memcmp(observed,n->program,n->program_bytes)){ret=-EIO;goto out;}
 memcpy(r->draw_owner[1].cpu,n->constants,n->constants_bytes);
 memcpy((u8*)r->draw_owner[1].cpu+n->geometry_offset,n->constants+n->geometry_offset,n->geometry_bytes);
 memcpy((u8*)r->draw_owner[2].cpu+n->secondary_cursor,n->secondary,n->secondary_bytes);
 if(n->clear_bytes){
  end=start+n->clear_bytes;memcpy((u8*)primary->cpu+start,n->clear,n->clear_bytes);dma_wmb();
  ret=m9x_post_faa_graph_end_locked(mdev,&primary->system_node,primary->dma,start,end,false,0,&stage_point,&stage_posted);
  *posted|=stage_posted;if(stage_posted){r->graph_draw_end_bytes=end;r->graph_draw_posts++;}if(ret)goto out;start=end;
 }
 r->color_primary_start=start;r->color_posts_start=r->graph_draw_posts;
 for(i=0;i<4;i++){
  u32 old=r->graph_draw_end_bytes;
  if(!i){end=start+n->draw_length;memcpy((u8*)primary->cpu+start,n->primary,n->draw_length);}
  else if(i<3){end=start+n->primary_length-12;if(i==1)memcpy((u8*)primary->cpu+start+n->draw_length,n->primary+n->draw_length,n->primary_length-12-n->draw_length);}
  else{end=start+n->primary_length;memcpy((u8*)primary->cpu+start+n->primary_length-12,n->primary+n->primary_length-12,12);}
  dma_wmb();ret=m9x_post_faa_graph_end_locked(mdev,&primary->system_node,primary->dma,old,end,i==2,
   i==3?lower_32_bits(previous_unlock_point)+n->lock_delta:0,&stage_point,&stage_posted);
  *posted|=stage_posted;if(stage_posted){r->graph_draw_end_bytes=end;r->graph_draw_posts++;}if(ret)goto out;
 }
 if(stage_point!=previous_unlock_point+n->lock_delta){ret=-EIO;goto out;}
 *point=stage_point;r->graph_draw_point=stage_point;r->color_lock_point=stage_point;r->color_lock_retired=true;
 r->color_secondary_cursor=n->secondary_cursor+n->secondary_bytes;
 m9_faa_color_intermediates_observe(mdev,r);
 dev_info(&mdev->pdev->dev,"M9 FAA color NEXT Lock retired: previous_unlock=%#llx lock=%#llx bytes=%u posts=%u secondary_cursor=%u clear=%u\n",
  previous_unlock_point,*point,r->graph_draw_end_bytes,r->graph_draw_posts,r->color_secondary_cursor,n->clear_bytes);
out:
	if (ret)
		*point = 0;
	m9x_transport_fence_finish_locked(mdev, fence, ret, *posted);
 m9x_faa_graph_binding_put(checked);mutex_unlock(&t->lock);return ret;
#else
 return -EOPNOTSUPP;
#endif
}

/* Native attachment ownership for the source SETRENDERTARGET association.
 * Callers hold their graph file mutex and drm_dev_enter across preparation.
 * There is deliberately no ioctl activation or clear packet here until the
 * bound-depth encoder qualifies its descriptors and mapping policy. */
int m9x_faa_graph_bound_depth_prepare(struct m9x_device *mdev,
 struct m9_faa_graph_binding *binding,u32 generation,u64 previous_point,
 u32 color_policy,bool first)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 struct m9_faa_owner_spec spec[M9_FAA_DEPTH_OWNERS]={
  {0,0x50000,false,false,false,0},
  {0,0x8000,true,false,false,0},
  {0,0x98000,true,false,false,0},
 };
 struct m9x_faa_surface_layout layout;
 struct m9x_transport *t=&mdev->transport;
 struct m9_faa_graph_binding *checked=NULL;
 struct m9_faa_depth_storage *n;
 struct m9x_faa_replay *r;
 u32 i;int ret;

 /* The original color31 MDL is flags30: its compact PTE policy is B.
  * Ordinary SYSTEM policy A is intentionally not admitted for this role. */
 if(!binding||color_policy!=M9_FAA_DEPTH_COLOR_SYSTEM_B||
  !m9x_faa_graph_binding_layout(binding,&layout))return -EINVAL;
 n=kzalloc(sizeof(*n),GFP_KERNEL);if(!n)return -ENOMEM;
 n->generation=generation;n->previous_point=previous_point;n->first=first;
 n->views.color_policy=color_policy;
 n->views.depth_aux_role=M9_FAA_DEPTH_HZ;n->views.aux_depth_role=M9_FAA_BOUND_DEPTH;
 for(i=0;i<M9_FAA_DEPTH_OWNERS;i++){
  n->views.owner[i]=m9x_faa_bound_depth_profile_dimensions(i,layout.width,layout.height);
  if(!n->views.owner[i].logical_bytes){kfree(n);return -EINVAL;}
  spec[i].bytes=n->views.owner[i].capacity_bytes;
 }
 mutex_lock(&t->lock);
 ret=first?m9x_faa_graph_retained_get_locked(mdev,generation,previous_point,&checked):
  m9x_faa_graph_color_unlocked_get_locked(mdev,generation,previous_point,&checked);
 if(ret)goto unclaimed;
 r=t->faa;
 if(checked!=binding){ret=-ESTALE;goto unclaimed;}
 if(r->depth_storage){ret=-EALREADY;goto unclaimed;}
 /* Claim the lifetime before allocation. The ready flag prevents consumers
  * from observing partly constructed owners. No other graph operation can
  * steal this bundle; even failed preparation is kept until DMA shutdown. */
 r->depth_storage=n;
 mutex_unlock(&t->lock);
 m9x_faa_graph_binding_put(checked);checked=NULL;
 for(i=0;i<M9_FAA_DEPTH_OWNERS;i++){
  ret=m9_faa_allocate(mdev,&n->owner[i],&spec[i],false);
  if(ret)return ret;
 }
 /* TTM allocation/reservation above is outside TRANSPORT and the six
  * borrowed BO reservations. Address publication is under TRANSPORT only. */
 mutex_lock(&t->lock);
 ret=first?m9x_faa_graph_retained_get_locked(mdev,generation,previous_point,&checked):
  m9x_faa_graph_color_unlocked_get_locked(mdev,generation,previous_point,&checked);
 if(ret)goto retained;
 r=t->faa;
 if(checked!=binding||r->depth_storage!=n){ret=-ESTALE;goto retained;}
 if(color_policy==M9_FAA_DEPTH_COLOR_SYSTEM_B){
  ret=m9_faa_dma_prepare_wc(mdev,&n->owner[M9_FAA_BOUND_COLOR]);
  if(ret)goto retained;
 }
 ret=color_policy==M9_FAA_DEPTH_COLOR_SYSTEM_B?
  m9x_address_map_faa_system(mdev,&n->owner[0].system_node,n->owner[0].dma,n->owner[0].bytes):
  m9x_address_map_system(mdev,&n->owner[0].system_node,n->owner[0].dma,n->owner[0].bytes);
 if(ret)goto retained;
 n->owner[0].base=n->owner[0].system_node.start;
 for(i=M9_FAA_DEPTH_HZ;i<M9_FAA_DEPTH_OWNERS;i++){
  ret=m9x_address_local_prepare(mdev,n->owner[i].base,n->owner[i].bytes);
  if(ret)goto retained;
 }
 for(i=0;i<M9_FAA_DEPTH_OWNERS;i++)n->views.owner[i].base=n->owner[i].base;
 n->ready=true;
retained:
 m9x_faa_graph_binding_put(checked);
 mutex_unlock(&t->lock);
 return ret;
unclaimed:
 m9x_faa_graph_binding_put(checked);mutex_unlock(&t->lock);kfree(n);return ret;
#else
 return -EOPNOTSUPP;
#endif
}

int m9x_faa_graph_bound_depth_views_locked(struct m9x_device *mdev,
 struct m9_faa_graph_binding *binding,u32 generation,struct m9x_faa_depth_views *views)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 struct m9x_transport *t=&mdev->transport;struct m9x_faa_replay *r=t->faa;
	struct m9_faa_depth_storage *n;
	u32 i;
	int ret;
 lockdep_assert_held(&t->lock);
 if(!views||!binding)return -EINVAL;
 memset(views,0,sizeof(*views));
 if(t->wedged)return -EIO;
 if(!r||r->graph_binding!=binding||r->graph_generation!=generation||t->generation!=generation||
  !r->depth_storage||!r->depth_storage->ready)return -ESTALE;
 n=r->depth_storage;
 if(n->generation!=generation||n->views.color_policy!=M9_FAA_DEPTH_COLOR_SYSTEM_B||
  n->views.depth_aux_role!=M9_FAA_DEPTH_HZ||
  n->views.aux_depth_role!=M9_FAA_BOUND_DEPTH)return -ESTALE;
 m9_faa_frame_trace(mdev,"attachment-views-begin",n->initialized,n->owner[0].bytes,generation,0,false);
 for(i=0;i<M9_FAA_DEPTH_OWNERS;i++){
  struct m9_faa_owner *o=&n->owner[i];
  const struct m9x_faa_depth_view *v=&n->views.owner[i];
  struct m9x_faa_depth_view expected=m9x_faa_bound_depth_profile_dimensions(i,
   r->graph_layout.width,r->graph_layout.height);
  expected.base=o->base;
  if(memcmp(v,&expected,sizeof(expected)))return -ESTALE;
  if(v->base!=o->base||v->capacity_bytes!=o->bytes||!v->logical_bytes||
   v->logical_bytes>o->bytes||!IS_ALIGNED(o->base,PAGE_SIZE)||
   (u64)o->base+o->bytes>U32_MAX)return -ESTALE;
  if(i==M9_FAA_BOUND_COLOR){
   if(o->vram||!o->cpu||!drm_mm_node_allocated(&o->system_node)||
    o->system_node.start!=o->base||o->system_node.size!=o->bytes||
    o->cpu_wc!=(n->views.color_policy==M9_FAA_DEPTH_COLOR_SYSTEM_B))return -ESTALE;
			ret = m9x_system_pages_mapped(mdev, o->base, o->dma,
						      o->bytes / PAGE_SIZE,
						      n->views.color_policy ==
						      M9_FAA_DEPTH_COLOR_SYSTEM_B ?
						      1u : 0u, NULL, NULL);
			if (ret)
				return ret;
  }else{
   if(!o->vram||o->cpu||iosys_map_is_null(&o->map))return -ESTALE;
   /* GPU use can update opaque secondary-record metadata. Initial
    * publication still requires its clean image; reuse checks identities
    * without resetting those retained metadata words. */
   ret=n->initialized?m9x_address_check_local_mapping(mdev,o->base,o->bytes):
    m9x_address_check_local_identity(mdev,o->base,o->bytes);
   if(ret){m9_faa_frame_trace(mdev,"refuse-attachment-identity",31+i,o->base,o->bytes,ret,false);return ret;}
  }
 }
 *views=n->views;
 m9_faa_frame_trace(mdev,"attachment-views-end",n->initialized,n->owner[0].bytes,generation,0,false);return 0;
#else
 return -EOPNOTSUPP;
#endif
}

#ifdef M9_FAA_GRAPH_INIT_CONTROL
static int m9_faa_depth_frame_first_state(struct m9x_transport *t,
					  struct m9_faa_graph_binding *binding,
					  u32 generation, u64 previous)
{
	struct m9x_faa_replay *r = t->faa;
	u64 origin;

	lockdep_assert_held(&t->lock);
	if (t->wedged)
		return -EIO;
	if (!r || r->graph_binding != binding || !r->color_payload_ready ||
	    !r->color_image || r->color_attempted || !r->graph_draw_claimed ||
	    r->draw_image || r->public_vs_image ||
	    r->graph_generation != generation || t->generation != generation ||
	    r->observation_generation != generation ||
	    !r->observation_retired || r->observation_point != previous)
		return -ESTALE;
	/* After ordinary jobs, FIRST starts at their retired point and END. */
	origin = m9_faa_first_origin(r, previous);
	if (t->submitted_point != origin || t->completed_point != origin ||
	    r->graph_draw_point || r->graph_draw_posts ||
	    r->graph_draw_end_bytes != m9_faa_first_bytes(r, previous) ||
	    (origin & 15) || lower_32_bits(origin) > U32_MAX - 0x190 ||
	    t->next_fence != lower_32_bits(origin) + 0x10)
		return -ESTALE;
	if (!t->faa_source_context || !t->faa_virtual_primary ||
	    !t->engine_transitioned || !t->restore_ready ||
	    !t->transport_armed || !t->bus_master_enabled || !t->fence_cpu ||
	    t->async_stop || t->async_pending || t->queue_count)
		return -EBUSY;
	return 0;
}

/* A queue epoch is command-source state, distinct from the authenticated
 * prior Unlock receipt. The COLOR source preserves q+108/q+110=next_fence;
 * an independent finisher in the older public-VS control changed that state.
 * Never infer this epoch from shader arity or a historical absolute point. */
static int m9_faa_depth_frame_retained_state(struct m9x_device *mdev,
 struct m9_faa_graph_binding *binding,u32 generation,u64 previous,u64 *queue_start)
{
 struct m9x_transport *t=&mdev->transport;
 struct m9_faa_graph_binding *checked=NULL;
 u64 next;int ret;
 lockdep_assert_held(&t->lock);
 if(!queue_start)return -EINVAL;
 *queue_start=0;
 ret=m9x_faa_graph_depth_unlocked_get_locked(mdev,generation,previous,&checked);
 if(ret)return ret;
 if(checked!=binding){ret=-ESTALE;goto out;}
 /* Authenticate the extended epoch and forbid low32 rollover while the
  * bounded command grammar consumes its next 0x190 points. */
 if(previous>U64_MAX-0x1a0||lower_32_bits(previous)>U32_MAX-0x1a0){ret=-ERANGE;goto out;}
 next=(previous&~0xffffffffull)|t->next_fence;
 if(next!=m9_faa_frame_origin(t->faa,previous)+0x10){ret=-ESTALE;goto out;}
 *queue_start=next;
out:m9x_faa_graph_binding_put(checked);return ret;
}

static void m9_faa_depth_frame_owners(struct m9x_faa_replay *r,
 struct m9x_faa_graph_draw_owner owner[M9_FAA_DEPTH_DRAW_OWNERS])
{
 u32 i;
 for(i=0;i<M9_FAA_DEPTH_DRAW_OWNERS;i++){
  struct m9_faa_owner *o;
  switch(i){case 26:o=&r->draw_owner[0];break;case 27:o=m9_faa_command_owner(r,false);break;
   case 28:o=&r->draw_owner[1];break;case 29:o=m9_faa_command_owner(r,true);break;
   case 30:o=&r->owner[27];break;
   case 31:case 32:case 33:o=&r->depth_storage->owner[i-31];break;
   default:o=&r->owner[i];break;}
  owner[i]=o?(struct m9x_faa_graph_draw_owner){o->base,o->bytes}:(struct m9x_faa_graph_draw_owner){0};
 }
}
#endif

/* First attachment preparation is outside all six graph BO reservations.
 * FIRST and retained Unlock authenticate different states and queue epochs.
 * Already initialized attachments/no-clear remain refused: their later
 * caller contract does not authorize reinitializing HZ on every draw. */
int m9x_faa_graph_depth_frame_prepare(struct m9x_device *mdev,
 struct m9_faa_graph_binding *binding,u32 generation,u64 previous,bool first,
 const struct m9x_faa_color_input *input,u32 raster_flags)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 struct m9x_transport *t=&mdev->transport;struct m9x_faa_replay *r;
 struct m9_faa_depth_frame_storage *frame=NULL;
 struct m9x_faa_attachment_clear_image *attach=NULL;
 struct m9x_faa_first_depth_scratch *fresh=NULL;
 struct m9x_faa_depth_draw_image draw;
 struct m9x_faa_color_image first_image;
 struct m9_faa_color_storage *scratch=NULL;
 u8 *raster_scratch=NULL;
 struct m9x_faa_depth_views views;
 struct m9x_faa_graph_draw_owner owner[M9_FAA_DEPTH_DRAW_OWNERS];
 const struct firmware *sample_fw=NULL,*draw_fw=NULL,*first_fw=NULL;
 struct m9x_faa_color_input typed_input;
 u32 i,sample_bytes,prefix_bytes;u64 queue_start=0;bool two;int ret;
 m9_faa_frame_trace(mdev,"prepare-enter",first,0,previous,0,false);
 if(!binding||!input||(!(raster_flags&1)&&m9x_faa_color_flat_fragment_valid(input))||(!first&&(raster_flags&1))||(raster_flags&~7u)||(raster_flags&6)==6||
  !m9_faa_color_four_experimental||(input->layout.program_count!=4&&(!first||input->layout.program_count!=2))||(input->declaration_count!=3&&input->declaration_count!=5)||(!first&&input->declaration_count!=5))return -EOPNOTSUPP;
 two=input->layout.program_count==2;
 {struct m9x_faa_surface_layout layout;
 if(!m9x_faa_graph_binding_layout(binding,&layout))return -ESTALE;
 typed_input=*input;typed_input.width=layout.width;typed_input.height=layout.height;
 typed_input.fragment_flags=raster_flags&1;input=&typed_input;}
 if(previous>U64_MAX-0x1a0||lower_32_bits(previous)>U32_MAX-0x1a0)return -ERANGE;
 frame=kvzalloc(sizeof(*frame),GFP_KERNEL);attach=kmalloc(sizeof(*attach),GFP_KERNEL);
 if(!frame||!attach){ret=-ENOMEM;goto release;}
 if(raster_flags){raster_scratch=kvmalloc(M9_FAA_DEPTH_FRAME_PRIMARY_CAPACITY+M9_FAA_COLOR_SECONDARY_CAPACITY,GFP_KERNEL);if(!raster_scratch){ret=-ENOMEM;goto release;}}
 ret=m9_faa_template_request(mdev,binding,generation,0,&sample_fw);if(ret)goto release;
 if(!first){ret=request_firmware(&draw_fw,M9_FAA_DEPTH_DRAW_FIRMWARE,&mdev->pdev->dev);if(ret)goto release;}
 scratch=kzalloc(sizeof(*scratch),GFP_KERNEL);if(!scratch){ret=-ENOMEM;goto release;}
 if(first){fresh=kzalloc(sizeof(*fresh),GFP_KERNEL);if(!fresh){ret=-ENOMEM;goto release;}}
 ret=m9_faa_template_request(mdev,binding,generation,two?1:2,&first_fw);if(ret)goto release;
 /* Claims/maps the three separate native owners while INIT is still the
  * actual observed boundary. TTM operations occur outside TRANSPORT. */
 m9_faa_frame_trace(mdev,"attachments-begin",first,0,previous,0,false);
 ret=m9x_faa_graph_bound_depth_prepare(mdev,binding,generation,previous,
  M9_FAA_DEPTH_COLOR_SYSTEM_B,first);
 m9_faa_frame_trace(mdev,"attachments-end",first,0,previous,ret,false);if(ret)goto release;
 ret=first?m9x_faa_graph_color_prepare(mdev,binding,generation,previous,input):
  m9x_faa_graph_color_next_prepare(mdev,binding,generation,previous,input,0);if(ret)goto release;
 m9_faa_frame_trace(mdev,"color-input-end",first,0,previous,ret,false);
 mutex_lock(&t->lock);
 ret=first?m9_faa_depth_frame_first_state(t,binding,generation,previous):
  m9_faa_depth_frame_retained_state(mdev,binding,generation,previous,&queue_start);if(ret)goto unlock;
 r=t->faa;
	if (first)
		queue_start = m9_faa_first_origin(r, previous);
 if(!r->depth_storage||r->depth_storage->initialized||r->depth_storage->frame){ret=-ESTALE;goto unlock;}
 ret=m9x_faa_graph_bound_depth_views_locked(mdev,binding,generation,&views);if(ret)goto unlock;
 m9_faa_depth_frame_owners(r,owner);
 first_image=(struct m9x_faa_color_image){
  .program=r->color_image->program,.constants=r->color_image->constants,
  .primary=scratch->primary,.secondary=scratch->secondary,
  .program_bytes=sizeof(r->color_image->program),
  .constants_bytes=r->color_image->geometry_offset+input->layout.geometry_bytes+768,
  .primary_bytes=sizeof(scratch->primary),.secondary_bytes=sizeof(scratch->secondary),
  .secondary_length=&scratch->secondary_bytes};
	if (first) {
		ret = (two ? m9x_faa_first_depth_two_encode :
			     m9x_faa_first_depth_encode)(sample_fw->data,
							 sample_fw->size,
							 first_fw->data,
							 first_fw->size, owner,
							 &views,
							 r->graph_draw_end_bytes,
							 queue_start, input,
							 frame->primary,
							 sizeof(frame->primary),
							 frame->secondary,
							 sizeof(frame->secondary),
							 &frame->primary_bytes,
							 &frame->secondary_bytes,
							 frame->end_bytes,
							 &first_image, fresh);
		if (ret)
			goto unlock;
  frame->primary_start=r->graph_draw_end_bytes;frame->end_count=M9_FAA_FIRST_DEPTH_ENDS;
  if(raster_flags){
   u32 prefix=frame->primary_bytes-(two?3588u:M9_FAA_COLOR_PRIMARY_BYTES-64),secondary_bytes;
   struct m9x_faa_raster_input ri={
    .primary=frame->primary,.secondary=frame->secondary,
    .primary_bytes=frame->primary_bytes,.secondary_bytes=frame->secondary_bytes,
    .secondary_base=owner[29].base,
    .range_at={prefix+4,prefix+8,prefix+(two?16:52),prefix+(two?20:56)},
    .database_bytes=0x1c1d8,.no_cull_start_bytes=0x149a0,.cull_start_bytes=0x147f8,
    .flags=raster_flags};
   struct m9x_faa_raster_output ro={
    .primary=raster_scratch,.secondary=raster_scratch+M9_FAA_DEPTH_FRAME_PRIMARY_CAPACITY,
    .primary_capacity=M9_FAA_DEPTH_FRAME_PRIMARY_CAPACITY,
    .secondary_capacity=M9_FAA_COLOR_SECONDARY_CAPACITY,.secondary_bytes=&secondary_bytes};
   bool resident=false;
   for(i=0;i<ARRAY_SIZE(m9_faa_spans);i++){
    const struct m9_faa_span *span=&m9_faa_spans[i];
    if(span->owner==12&&span->offset==0&&span->bytes==ri.database_bytes)resident=true;
   }
   if(!resident||owner[12].bytes<ri.database_bytes){ret=-ESTALE;goto unlock;}
   ret=(two?m9x_faa_raster_two_encode:m9x_faa_raster_encode)(&ri,&ro);if(ret)goto unlock;
   /* Fresh FIRST already contains every profile database binding. Culling
    * changes their live S26 instruction starts and EF, never its ranges
    * or ENDs. Flat's actual FS/utility/usage fields were checked by the
    * live producer before C1 interpolation is enabled here. */
   if(secondary_bytes!=frame->secondary_bytes){ret=-EINVAL;goto unlock;}
   memcpy(frame->primary,ro.primary,frame->primary_bytes);
   memcpy(frame->secondary,ro.secondary,secondary_bytes);
  }
		frame->final_point = queue_start + (two ? 0x110 : 0x150);
		goto encoded;
 }
 ret=m9x_faa_color_attachment_clear_encode(&views,lower_32_bits(queue_start),0x3f800000,attach);if(ret)goto unlock;
 memcpy(frame->primary,attach->word,attach->bytes);
 ret=m9x_faa_sample_clear_encode(sample_fw->data,sample_fw->size,owner,&views,queue_start,
  frame->primary+attach->bytes,M9_FAA_SAMPLE_CLEAR_CAPACITY,&sample_bytes);if(ret)goto unlock;
 prefix_bytes=attach->bytes+sample_bytes;
 draw=(struct m9x_faa_depth_draw_image){.primary=frame->primary+prefix_bytes,
  .secondary=frame->secondary,.primary_capacity=M9_FAA_DEPTH_DRAW_PRIMARY_BYTES,
  .secondary_capacity=sizeof(frame->secondary)};
 /* Rebuild the live first image only as CPU scratch; the retained splice
  * owns its own state/epochs and does not publish any first-only commands. */
 ret=m9x_faa_depth_color_encode(draw_fw->data,draw_fw->size,first_fw->data,first_fw->size,
  owner,&views,r->primary_bytes,r->graph_draw_end_bytes+prefix_bytes,r->color_secondary_cursor,
  queue_start,1,input,r->color_image->geometry_offset,
  r->color_image->geometry_offset+input->layout.vertex_offset,&draw,&first_image);
 frame->secondary_bytes=M9_FAA_DEPTH_COLOR_SECONDARY_BYTES;
 if(ret)goto unlock;
 if(raster_flags){
  struct m9x_faa_depth_raster_input ri={
   .streams={.primary=draw.primary,.secondary=draw.secondary,
    .primary_bytes=M9_FAA_DEPTH_DRAW_PRIMARY_BYTES,.secondary_bytes=frame->secondary_bytes,
    .secondary_base=owner[29].base+r->color_secondary_cursor,
    .range_at={4244,4248,4292,4296},.flags=raster_flags,
    /* Source 13b2b0's S28/S26 instruction starts in the immutable INIT
     * resident library span, not source CPU pointers or GPU addresses. */
    .database_bytes=0x1c1d8,.no_cull_start_bytes=0x149a0,.cull_start_bytes=0x147f8},
   .primary_cursor=r->graph_draw_end_bytes+prefix_bytes};
  struct m9x_faa_depth_raster_output ro={
   .streams={.primary=raster_scratch,.secondary=raster_scratch+M9_FAA_DEPTH_DRAW_PRIMARY_BYTES,
    .primary_capacity=M9_FAA_DEPTH_DRAW_PRIMARY_BYTES,.secondary_capacity=M9_FAA_COLOR_SECONDARY_CAPACITY,
    .secondary_bytes=&frame->secondary_bytes}};
  bool resident=false;
  for(i=0;i<ARRAY_SIZE(m9_faa_spans);i++){
   const struct m9_faa_span *span=&m9_faa_spans[i];
   if(span->owner==12&&span->offset==0&&span->bytes==ri.streams.database_bytes)resident=true;
  }
  if(!resident||owner[12].bytes<ri.streams.database_bytes){ret=-ESTALE;goto unlock;}
  memcpy(ri.end_bytes,draw.end_bytes,sizeof(ri.end_bytes));
  ret=m9x_faa_depth_raster_encode(&ri,&ro);if(ret)goto unlock;
  memcpy(draw.primary,ro.streams.primary,M9_FAA_DEPTH_DRAW_PRIMARY_BYTES);
  memcpy(draw.secondary,ro.streams.secondary,frame->secondary_bytes);
 }
 frame->primary_bytes=prefix_bytes+M9_FAA_DEPTH_DRAW_PRIMARY_BYTES;
 if(frame->primary_bytes>owner[27].bytes-r->graph_draw_end_bytes){ret=-ENOSPC;goto unlock;}
 frame->primary_start=r->graph_draw_end_bytes;
 for(i=0;i<5;i++)frame->end_bytes[i]=frame->primary_start+attach->end_bytes[i];
 frame->end_bytes[5]=frame->primary_start+prefix_bytes;
 for(i=0;i<M9_FAA_DEPTH_DRAW_ENDS;i++)frame->end_bytes[6+i]=draw.end_bytes[i];
 if(draw.final_point!=queue_start+0x190||frame->end_bytes[12]!=frame->primary_start+frame->primary_bytes){ret=-EINVAL;goto unlock;}
 frame->end_count=M9_FAA_DEPTH_FRAME_ENDS;
 frame->final_point=draw.final_point;
encoded:
 if(first&&two){
  struct m9x_faa_depth_mode_image mode;
  struct m9x_faa_depth_mode_state before={0};
  ret=m9x_faa_depth_mode_two_plan(input->depth_flags?input->depth_flags:15,0,
   &before,owner,&views,&mode);if(ret)goto unlock;
  frame->mode_planned=true;frame->mode_before=before;frame->mode_after=mode.after;
  frame->planned_delta=0x110;frame->planned_ends=frame->end_count;
 }
 if(frame->primary_start>owner[27].bytes||frame->primary_bytes>owner[27].bytes-frame->primary_start){ret=-ENOSPC;goto unlock;}
 frame->previous_point=previous;frame->queue_start=queue_start;
 frame->secondary_cursor=first?0:r->color_secondary_cursor;frame->generation=generation;frame->first=first;
 /* Storage is ready only after every field/domain/count has been checked.
  * This metadata alone never authorizes F021 or marks attachments cleared. */
 r->depth_storage->frame=frame;frame=NULL;
 ret=0;
unlock:mutex_unlock(&t->lock);
release:
 if(sample_fw)release_firmware(sample_fw);if(draw_fw)release_firmware(draw_fw);
 if(first_fw)release_firmware(first_fw);kfree(scratch);
 kvfree(raster_scratch);kfree(fresh);kfree(attach);kvfree(frame);
 m9_faa_frame_trace(mdev,"prepare-return",first,0,previous,ret,false);return ret;
#else
 return -EOPNOTSUPP;
#endif
}

/* One source-qualified COLOR0/TEX0/TEX1 FIRST frame. Texture references are
 * acquired by the ioctl before the INIT claim and transferred to the replay
 * only after the complete live image is encoded. No command is posted here. */
int m9x_faa_graph_dual_first_prepare(struct m9x_device *mdev,
 struct m9_faa_graph_binding *binding,u32 generation,u64 init_point,
 const struct m9x_faa_color_input *input,
 struct m9x_faa_texture_owners *textures,
 const struct m9x_faa_owned_color_batch *batch)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 struct m9x_transport *t=&mdev->transport;struct m9x_faa_replay *r;
 struct m9_faa_depth_frame_storage *frame=NULL;
 struct m9x_faa_program_cache *programs=NULL;
 struct m9_faa_color_storage *scratch=NULL;
 struct m9x_faa_first_depth_scratch *prefix=NULL;
 struct m9x_faa_graph_draw_owner owner[M9_FAA_DEPTH_DRAW_OWNERS];
 struct m9x_faa_depth_views views;struct m9x_faa_color_image image;
 struct m9x_faa_surface_layout layout;
 struct m9x_faa_color_input sized_input;
 struct m9x_faa_color_input shadow_input;
 struct m9x_faa_dual_first_input dual;
 const struct firmware *sample=NULL,*color=NULL,*dual_fw=NULL;
 u8 *arena=NULL,*secondary=NULL,*composed=NULL,*job_primary=NULL,*job_secondary=NULL;
 u8 *shadow_program=NULL,*shadow_cb=NULL,*shadow_output=NULL;
 u32 slots[M9_FAA_DUAL_BATCH_MAX_DRAWS],arena_bytes=0;
 u32 primary_bytes=0,secondary_bytes=0,i,raw,span,job_bytes,ends[14];int ret;
	u64 origin;
 if(!binding||!input||!textures||!textures->gem[0]||!textures->gem[1]||
    textures->generation!=generation||input->layout.program_count!=4||
    input->declaration_count!=9||input->fragment_flags||
    (input->dual_colorpass_diagnostic&&batch))return -EOPNOTSUPP;
 if(!m9x_faa_graph_binding_layout(binding,&layout))return -ESTALE;
 sized_input=*input;
 sized_input.width=layout.width;
 sized_input.height=layout.height;
 input=&sized_input;
 if(input->dual_colorpass_diagnostic){
  if(layout.width!=320||layout.height!=240)return -EOPNOTSUPP;
  shadow_program=kmalloc(M9_FAA_COLOR_PROGRAM_CAPACITY,GFP_KERNEL);
  shadow_cb=kmalloc(16384,GFP_KERNEL);
  shadow_output=kvzalloc(0x40000,GFP_KERNEL);
  if(!shadow_program||!shadow_cb||!shadow_output){ret=-ENOMEM;goto out;}
  ret=m9x_faa_dual_colorpass_shadow(input,shadow_program,
    M9_FAA_COLOR_PROGRAM_CAPACITY,shadow_cb,16384,&shadow_input);
  if(ret)goto out;
 }
 dual=(struct m9x_faa_dual_first_input){
  .color=input->dual_colorpass_diagnostic?&shadow_input:input,
  .texture=textures->bind};
 /* A one-DRAW descriptor batch is the plain FIRST of draw[0]. */
 if(batch&&batch->count==1)
  batch=NULL;
 if(batch){
  if(!batch->count||batch->count>M9_FAA_DUAL_BATCH_MAX_DRAWS||
     batch->draw[0].input.program!=input->program)return -EOPNOTSUPP;
  for(i=0;i<batch->count;i++){
   const struct m9x_faa_color_input *job=&batch->draw[i].input;
   if(job->data_offset||job->layout.program_count!=4||
      job->declaration_count!=9||job->record_stride!=64||
      job->fragment_flags||job->layout.constants_bytes>16384||
      job->record_count>12288||!job->record_count||job->record_count%3||
      check_mul_overflow(ALIGN(job->record_count,4),64u,&raw)||
      check_add_overflow(ALIGN(job->layout.constants_bytes,128),
                         max(job->layout.geometry_bytes,128u+raw),&span))
    return -EOPNOTSUPP;
   slots[i]=ALIGN(arena_bytes,128);
   if(check_add_overflow(slots[i],span,&arena_bytes)||arena_bytes>0x40000)
    return -ENOSPC;
  }
  arena=kvzalloc(arena_bytes,GFP_KERNEL);
  secondary=kvzalloc(2880+2*M9_FAA_DUAL_APPEND_SECONDARY_BYTES,GFP_KERNEL);
  composed=kmalloc(M9_FAA_DEPTH_FRAME_PRIMARY_CAPACITY,GFP_KERNEL);
  job_primary=kmalloc(M9_FAA_DEPTH_FRAME_PRIMARY_CAPACITY,GFP_KERNEL);
  job_secondary=kmalloc(M9_FAA_DUAL_FIRST_SECONDARY_BYTES,GFP_KERNEL);
  if(!arena||!secondary||!composed||!job_primary||!job_secondary){ret=-ENOMEM;goto out;}
 }
 frame=kvzalloc(sizeof(*frame),GFP_KERNEL);
 scratch=kzalloc(sizeof(*scratch),GFP_KERNEL);
 prefix=kzalloc(sizeof(*prefix),GFP_KERNEL);
 programs=kzalloc(sizeof(*programs),GFP_KERNEL);
 if(input->dual_colorpass_diagnostic){
  job_primary=kmalloc(M9_FAA_DEPTH_FRAME_PRIMARY_CAPACITY,GFP_KERNEL);
  job_secondary=kmalloc(M9_FAA_DUAL_FIRST_SECONDARY_BYTES,GFP_KERNEL);
 }
 if(!frame||!scratch||!prefix||!programs||
    (input->dual_colorpass_diagnostic&&(!job_primary||!job_secondary))){
  ret=-ENOMEM;goto out;
 }
 ret=request_firmware(&sample,M9_FAA_SAMPLE_CLEAR_FIRMWARE,&mdev->pdev->dev);
 if(ret)goto out;
 ret=request_firmware(&color,M9_FAA_GRAPH_COLOR_FOUR_FIRMWARE,&mdev->pdev->dev);
 if(ret)goto out;
 ret=request_firmware(&dual_fw,"m9x/faa-graph-dual-first-template.bin",&mdev->pdev->dev);
 if(ret)goto out;
 ret=m9x_faa_graph_bound_depth_prepare(mdev,binding,generation,init_point,
  M9_FAA_DEPTH_COLOR_SYSTEM_B,true);
 if(ret)goto out;
 ret=m9x_faa_graph_color_prepare(mdev,binding,generation,init_point,input);
 if(ret)goto out;
 mutex_lock(&t->lock);
 ret=m9_faa_depth_frame_first_state(t,binding,generation,init_point);
 if(ret)goto unlock;
 r=t->faa;
 if(!r||!r->depth_storage||r->depth_storage->initialized||
    r->depth_storage->frame||r->dual_textures.gem[0]){ret=-ESTALE;goto unlock;}
	/* FIRST continues after ordinary jobs that followed INIT. */
	origin = m9_faa_first_origin(r, init_point);
 ret=m9x_faa_graph_bound_depth_views_locked(mdev,binding,generation,&views);
 if(ret)goto unlock;
 m9_faa_depth_frame_owners(r,owner);
 image=(struct m9x_faa_color_image){
  .program=input->dual_colorpass_diagnostic?scratch->program:r->color_image->program,
  .constants=input->dual_colorpass_diagnostic?shadow_output:r->color_image->constants,
  .primary=scratch->primary,.secondary=scratch->secondary,
  .program_bytes=sizeof(r->color_image->program),
  .constants_bytes=r->color_image->geometry_offset+input->layout.geometry_bytes+768,
  .primary_bytes=sizeof(scratch->primary),.secondary_bytes=sizeof(scratch->secondary),
  .secondary_length=&scratch->secondary_bytes};
	ret = m9x_faa_dual_first_encode(sample->data, sample->size, color->data,
					color->size, dual_fw->data, dual_fw->size,
					owner, &views, r->graph_draw_end_bytes,
					origin, &dual,
					input->dual_colorpass_diagnostic ?
					job_primary : frame->primary,
					sizeof(frame->primary),
					input->dual_colorpass_diagnostic ?
					job_secondary : frame->secondary,
					sizeof(frame->secondary), &primary_bytes,
					frame->end_bytes, &image, prefix);
	if (ret)
		goto unlock;
	if (input->dual_colorpass_diagnostic) {
		ret = m9x_faa_dual_colorpass_encode(&shadow_input, input, owner,
						    r->graph_draw_end_bytes,
						    frame->end_bytes, job_primary,
						    primary_bytes, job_secondary,
						    M9_FAA_DUAL_FIRST_SECONDARY_BYTES,
						    frame->primary,
						    sizeof(frame->primary),
						    frame->secondary,
						    sizeof(frame->secondary));
  if(ret)goto unlock;
 }
 if(primary_bytes>owner[27].bytes-r->graph_draw_end_bytes){ret=-ENOSPC;goto unlock;}
 frame->primary_bytes=primary_bytes;frame->primary_start=r->graph_draw_end_bytes;
 frame->secondary_bytes=input->dual_colorpass_diagnostic?
  M9_FAA_DUAL_COLORPASS_SECONDARY_BYTES:M9_FAA_DUAL_FIRST_SECONDARY_BYTES;
 frame->end_count=M9_FAA_DUAL_FIRST_ENDS;
	frame->final_point = origin + 0x150;
	frame->previous_point = init_point;
	frame->queue_start = origin;
 frame->generation=generation;frame->first=true;frame->dual_first=true;
 frame->dual_colorpass=input->dual_colorpass_diagnostic;
 frame->draw_count=1;
 if(batch){
  u32 first_geo=r->color_image->geometry_offset;
  if(arena_bytes>owner[28].bytes||
     first_geo>arena_bytes||
     input->layout.geometry_bytes>arena_bytes-first_geo){ret=-ERANGE;goto unlock;}
  memcpy(arena,r->color_image->constants,input->layout.constants_bytes);
  memcpy(arena+first_geo,r->color_image->constants+first_geo,
         input->layout.geometry_bytes);
  memcpy(secondary,frame->secondary,M9_FAA_DUAL_FIRST_SECONDARY_BYTES);
  secondary_bytes=M9_FAA_DUAL_FIRST_SECONDARY_BYTES;
  for(i=1;i<batch->count;i++){
   struct m9x_faa_color_input job=batch->draw[i].input;
   struct m9x_faa_color_image live={
    .program=scratch->program,.constants=arena+slots[i],
    .primary=scratch->primary,.secondary=scratch->secondary,
    .program_bytes=sizeof(scratch->program),
    .constants_bytes=arena_bytes-slots[i],
    .primary_bytes=sizeof(scratch->primary),
    .secondary_bytes=sizeof(scratch->secondary),
    .secondary_length=&scratch->secondary_bytes};
   struct m9x_faa_dual_append_image appended={
    .primary=composed,.secondary=secondary+secondary_bytes,
    .primary_capacity=M9_FAA_DEPTH_FRAME_PRIMARY_CAPACITY,
    .secondary_capacity=M9_FAA_DUAL_APPEND_SECONDARY_BYTES};
   struct m9x_faa_dual_first_input extra;
   job.data_offset=slots[i];
   job.width=layout.width;job.height=layout.height;
   extra=(struct m9x_faa_dual_first_input){.color=&job,
      .texture=textures->bind};
			ret = m9x_faa_dual_first_encode(sample->data, sample->size,
							color->data, color->size,
							dual_fw->data, dual_fw->size,
							owner, &views,
							r->graph_draw_end_bytes,
							origin, &extra, job_primary,
							M9_FAA_DEPTH_FRAME_PRIMARY_CAPACITY,
							job_secondary,
							M9_FAA_DUAL_FIRST_SECONDARY_BYTES,
							&job_bytes, ends, &live,
							prefix);
   if(ret)goto unlock;
   if(job_bytes!=primary_bytes||
      memcmp(scratch->program,r->color_image->program,input->layout.program_bytes)){
    ret=-ESTALE;goto unlock;
   }
			u32 cursor = frame->primary_start + frame->primary_bytes;

			ret = m9x_faa_dual_first_append_encode(owner,
							       r->graph_draw_end_bytes,
							       origin, i, cursor,
							       secondary_bytes,
							       frame->end_bytes,
							       frame->final_point,
							       frame->primary,
							       frame->primary_bytes,
							       frame->secondary,
							       job_secondary, true,
							       &appended);
   if(ret)goto unlock;
   memcpy(frame->primary,appended.primary,appended.primary_bytes);
   frame->primary_bytes=appended.primary_bytes;
   memcpy(frame->end_bytes,appended.end_bytes,sizeof(appended.end_bytes));
   frame->final_point=appended.final_point;
   secondary_bytes+=appended.secondary_bytes;
  }
  if(secondary_bytes!=2880+(batch->count-1)*M9_FAA_DUAL_APPEND_SECONDARY_BYTES||
     frame->primary_start>owner[27].bytes||
     frame->primary_bytes>owner[27].bytes-frame->primary_start){
   ret=-ERANGE;goto unlock;
  }
  frame->batch_data=arena;arena=NULL;
  frame->batch_data_bytes=arena_bytes;
  frame->batch_secondary=secondary;secondary=NULL;
  frame->secondary_bytes=secondary_bytes;
  frame->draw_count=batch->count;
 }
	/* The FIRST builds the dual program at owner-26 offset 0. */
	if (!input->dual_colorpass_diagnostic) {
		enum m9x_faa_later_form form;
		bool cull_state;
		u32 offset;

		ret = m9x_faa_program_cache_draw(programs,
						 PARHELIA_M9_FAA_PROGRAM_DUAL,
						 input->program,
						 input->layout.program_bytes,
						 input->raster_flags, true, &form,
						 &cull_state, &offset);
		if (ret)
			goto unlock;
		kfree(r->programs);
		r->programs = programs;
		programs = NULL;
	}
 r->dual_textures=*textures;memset(textures,0,sizeof(*textures));
 r->depth_storage->frame=frame;frame=NULL;
unlock:mutex_unlock(&t->lock);
out:release_firmware(dual_fw);release_firmware(color);release_firmware(sample);
 kvfree(arena);kvfree(secondary);kfree(composed);kfree(job_primary);
 kfree(job_secondary);kfree(prefix);kfree(scratch);kfree(programs);
 kfree(shadow_program);kfree(shadow_cb);kvfree(shadow_output);
 m9_faa_depth_frame_free(frame);return ret;
#else
 return -EOPNOTSUPP;
#endif
}

/* One retained dual DRAW after an actual Lock/Unlock. The first producer is
 * used only as an independent live stage builder; its FIRST clear and ENDs
 * are never posted. The later source splice owns the actual command image. */
static void m9_faa_mixed_templates(struct m9x_faa_mixed_templates *tpl,
				   const struct firmware *sample,
				   const struct firmware *color,
				   const struct firmware *dual,
				   const struct firmware *single)
{
	*tpl = (struct m9x_faa_mixed_templates){
		.sample = sample->data, .sample_bytes = sample->size,
		.color = color->data, .color_bytes = color->size,
		.dual = dual->data, .dual_bytes = dual->size,
		.single = single ? single->data : NULL,
		.single_bytes = single ? single->size : 0
	};
}

/* FIRST batch of up to three DRAWs, each running the dual or the single-
 * texture program (DUAL_BATCH version 2). DRAW 0 is its program's FIRST;
 * every further DRAW appends its own program's FIRST stage in the form the
 * program cache gives (source N2/N3, tests/faa-first-forms-cpu.py). The
 * cache is seeded with DRAW 0's program and committed at retirement.
 */
int m9x_faa_graph_mixed_first_prepare(struct m9x_device *mdev,
				      struct m9_faa_graph_binding *binding,
				      u32 generation, u64 init_point,
				      struct m9x_faa_texture_owners *textures,
				      struct m9x_faa_texture_owners *draw_textures,
				      const struct m9x_faa_owned_color_batch *batch)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	struct m9_faa_depth_frame_storage *frame = NULL;
	struct m9_faa_color_storage *scratch = NULL;
	struct m9x_faa_first_depth_scratch *prefix = NULL;
	struct m9x_faa_program_cache *programs = NULL;
	struct m9x_faa_graph_draw_owner owner[M9_FAA_DEPTH_DRAW_OWNERS];
	struct m9x_faa_depth_views views;
	struct m9x_faa_surface_layout layout;
	struct m9x_faa_color_input first;
	struct m9x_faa_mixed_templates tpl;
	const struct firmware *sample = NULL, *color = NULL, *dual_fw = NULL,
			      *single_fw = NULL;
	u8 *arena = NULL, *secondary = NULL, *composed = NULL, *job_primary = NULL;
	u8 *stage = NULL;
	u32 slots[M9_FAA_DUAL_BATCH_MAX_DRAWS], arena_bytes = 0, sec_bytes = 0;
	u32 i, span, ends[M9_FAA_DUAL_FIRST_ENDS], job_ends[M9_FAA_DUAL_FIRST_ENDS];
	u32 primary_bytes = 0, job_bytes, stage_bytes, fresh_length = 0;
	u32 old_start = 0;
	u64 final_point, origin;
	bool single = false;
	int ret;

	if (!binding || !textures || !textures->gem[0] || !batch ||
	    textures->generation != generation || !batch->count ||
	    batch->count > M9_FAA_DUAL_BATCH_MAX_DRAWS)
		return -EOPNOTSUPP;
	if (!m9x_faa_graph_binding_layout(binding, &layout))
		return -ESTALE;
	for (i = 0; i < batch->count; i++) {
		ret = m9x_faa_mixed_draw_check(&batch->draw[i].input,
					       batch->draw[i].program, &span);
		if (ret)
			return ret;
		single |= batch->draw[i].program ==
			  PARHELIA_M9_FAA_PROGRAM_SINGLE;
		slots[i] = ALIGN(arena_bytes, 128);
		if (check_add_overflow(slots[i], span, &arena_bytes) ||
		    arena_bytes > 0x40000)
			return -ENOSPC;
	}
	first = batch->draw[0].input;
	first.width = layout.width;
	first.height = layout.height;
	frame = kvzalloc_obj(*frame);
	scratch = kzalloc_obj(*scratch);
	prefix = kzalloc_obj(*prefix);
	programs = kzalloc_obj(*programs);
	arena = kvzalloc(arena_bytes, GFP_KERNEL);
	secondary = kvzalloc(batch->count * M9_FAA_APPEND_STAGE_MAX,
			     GFP_KERNEL);
	composed = kmalloc(M9_FAA_DEPTH_FRAME_PRIMARY_CAPACITY, GFP_KERNEL);
	job_primary = kmalloc(M9_FAA_DEPTH_FRAME_PRIMARY_CAPACITY, GFP_KERNEL);
	stage = kmalloc(M9_FAA_DUAL_FIRST_SECONDARY_BYTES, GFP_KERNEL);
	if (!frame || !scratch || !prefix || !programs || !arena ||
	    !secondary || !composed || !job_primary || !stage) {
		ret = -ENOMEM;
		goto out;
	}
	frame->stages = kvzalloc_objs(*frame->stages, batch->count);
	frame->batch_program = kvzalloc(M9_FAA_PROGRAM_PAGE_BYTES, GFP_KERNEL);
	if (!frame->stages || !frame->batch_program) {
		ret = -ENOMEM;
		goto out;
	}
	ret = request_firmware(&sample, M9_FAA_SAMPLE_CLEAR_FIRMWARE,
			       &mdev->pdev->dev);
	if (ret)
		goto out;
	ret = request_firmware(&color, M9_FAA_GRAPH_COLOR_FOUR_FIRMWARE,
			       &mdev->pdev->dev);
	if (ret)
		goto out;
	ret = request_firmware(&dual_fw, "m9x/faa-graph-dual-first-template.bin",
			       &mdev->pdev->dev);
	if (ret)
		goto out;
	if (single) {
		ret = request_firmware(&single_fw, M9_FAA_SINGLE_FIRST_FIRMWARE,
				       &mdev->pdev->dev);
		if (ret)
			goto out;
	}
	ret = m9x_faa_graph_bound_depth_prepare(mdev, binding, generation,
						init_point,
						M9_FAA_DEPTH_COLOR_SYSTEM_B,
						true);
	if (ret)
		goto out;
	ret = m9x_faa_graph_color_prepare(mdev, binding, generation,
					  init_point, &first);
	if (ret)
		goto out;
	mutex_lock(&t->lock);
	ret = m9_faa_depth_frame_first_state(t, binding, generation,
					     init_point);
	if (ret)
		goto unlock;
	r = t->faa;
	if (!r || !r->depth_storage || r->depth_storage->initialized ||
	    r->depth_storage->frame || r->dual_textures.gem[0] ||
	    arena_bytes > r->draw_owner[1].bytes) {
		ret = -ESTALE;
		goto unlock;
	}
	ret = m9x_faa_graph_bound_depth_views_locked(mdev, binding, generation,
						     &views);
	if (ret)
		goto unlock;
	m9_faa_depth_frame_owners(r, owner);
	m9_faa_mixed_templates(&tpl, sample, color, dual_fw, single_fw);
	/* FIRST continues after ordinary jobs that followed INIT. */
	origin = m9_faa_first_origin(r, init_point);
	final_point = origin + 0x150;
	for (i = 0; i < batch->count; i++) {
		struct m9x_faa_color_input job = batch->draw[i].input;
		u32 kind = batch->draw[i].program;
		const struct m9x_faa_texture_owners *own =
			i && draw_textures && draw_textures[i].gem[0] ?
			&draw_textures[i] : textures;
		struct m9x_faa_color_image live = {
			.program = scratch->program,
			.constants = arena + slots[i],
			.primary = scratch->primary,
			.secondary = scratch->secondary,
			.program_bytes = sizeof(scratch->program),
			.constants_bytes = arena_bytes - slots[i],
			.primary_bytes = sizeof(scratch->primary),
			.secondary_bytes = sizeof(scratch->secondary),
			.secondary_length = &fresh_length
		};
		struct m9x_faa_dual_append_image appended = {
			.primary = composed,
			.secondary = secondary + sec_bytes,
			.primary_capacity = M9_FAA_DEPTH_FRAME_PRIMARY_CAPACITY,
			.secondary_capacity = M9_FAA_APPEND_STAGE_MAX
		};
		enum m9x_faa_later_form form;
		bool cull_state;
		u32 offset;

		/* Each DRAW's owner set has its program's unit count. */
		if ((own->gem[1] ? PARHELIA_M9_FAA_PROGRAM_DUAL :
				   PARHELIA_M9_FAA_PROGRAM_SINGLE) != kind) {
			ret = -EINVAL;
			goto unlock;
		}
		ret = m9x_faa_program_cache_draw(programs, kind, job.program,
						 job.layout.program_bytes,
						 job.raster_flags, !i, &form,
						 &cull_state, &offset);
		if (ret)
			goto unlock;
		job.data_offset = slots[i];
		job.width = layout.width;
		job.height = layout.height;
		ret = m9x_faa_mixed_first_encode(kind, &tpl, owner, &views,
						 r->graph_draw_end_bytes, origin,
						 &job, own->bind, offset,
						 i ? job_primary : frame->primary,
						 M9_FAA_DEPTH_FRAME_PRIMARY_CAPACITY,
						 i ? stage : secondary,
						 i ? &job_bytes : &primary_bytes,
						 i ? job_ends : ends, &live,
						 prefix, &stage_bytes);
		if (ret)
			goto unlock;
		if (!i) {
			frame->stages[0].bytes = stage_bytes;
			ret = m9x_faa_first_stage_sites(stage_bytes,
							&frame->stages[0].units,
							frame->stages[0].site);
			if (ret)
				goto unlock;
			sec_bytes = stage_bytes;
			continue;
		}
		ret = m9x_faa_first_append_encode(owner, r->graph_draw_end_bytes,
						  origin, i,
						  r->graph_draw_end_bytes + primary_bytes,
						  old_start, sec_bytes, ends,
						  final_point, frame->primary,
						  primary_bytes, stage,
						  stage_bytes, form, cull_state,
						  &appended);
		if (ret)
			goto unlock;
		memcpy(frame->primary, composed, appended.primary_bytes);
		memcpy(ends, appended.end_bytes, sizeof(ends));
		primary_bytes = appended.primary_bytes;
		final_point = appended.final_point;
		frame->stages[i] = (struct m9x_faa_frame_stage){
			sec_bytes, appended.secondary_bytes, appended.units,
			{ sec_bytes + appended.site[0],
			  appended.site[1] ? sec_bytes + appended.site[1] : 0 }
		};
		old_start = sec_bytes;
		sec_bytes += appended.secondary_bytes;
	}
	if (primary_bytes > owner[27].bytes - r->graph_draw_end_bytes) {
		ret = -ENOSPC;
		goto unlock;
	}
	memcpy(frame->batch_program, programs->page, M9_FAA_PROGRAM_PAGE_BYTES);
	frame->programs = programs;
	programs = NULL;
	frame->primary_bytes = primary_bytes;
	frame->primary_start = r->graph_draw_end_bytes;
	for (i = 0; i < M9_FAA_DUAL_FIRST_ENDS; i++)
		frame->end_bytes[i] = ends[i];
	frame->end_count = M9_FAA_DUAL_FIRST_ENDS;
	frame->secondary_bytes = sec_bytes;
	frame->batch_secondary = secondary;
	secondary = NULL;
	frame->batch_data = arena;
	arena = NULL;
	frame->batch_data_bytes = arena_bytes;
	frame->final_point = final_point;
	frame->previous_point = init_point;
	frame->queue_start = origin;
	frame->generation = generation;
	frame->first = true;
	frame->dual_first = true;
	frame->draw_count = batch->count;
	r->dual_textures = *textures;
	memset(textures, 0, sizeof(*textures));
	r->depth_storage->frame = frame;
	frame = NULL;
unlock:
	mutex_unlock(&t->lock);
out:
	release_firmware(single_fw);
	release_firmware(dual_fw);
	release_firmware(color);
	release_firmware(sample);
	kvfree(arena);
	kvfree(secondary);
	kfree(composed);
	kfree(job_primary);
	kfree(stage);
	kfree(prefix);
	kfree(scratch);
	kfree(programs);
	m9_faa_depth_frame_free(frame);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

/* One retained later frame of up to 64 DRAWs, each running the dual or the
 * single-texture program. Per DRAW the vendor program cache decides whether
 * its stage builds, keeps or selects the program (PROVEN forms, see
 * tests/faa-later-forms-cpu.py); every stage is composed from that DRAW's
 * live FIRST encode. Sampler words are regenerated at submit. */
int m9x_faa_graph_dual_later_prepare(
	struct m9x_device *mdev, struct m9_faa_graph_binding *binding,
	u32 generation, u64 previous, const struct m9x_faa_color_input *input,
	struct m9x_faa_texture_owners *textures,
	struct m9x_faa_texture_owners *draw_textures, u32 clear_flags,
	const struct m9x_faa_owned_color_batch *batch)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	struct m9_faa_color_storage *next_input = NULL, *old_input = NULL,
				    *scratch = NULL;
	struct m9_faa_depth_frame_storage *frame = NULL;
	struct m9x_faa_first_depth_scratch *first_scratch = NULL;
	struct m9x_faa_dual_later_scratch *later_scratch = NULL;
	struct m9x_faa_graph_draw_owner owner[M9_FAA_DEPTH_DRAW_OWNERS];
	struct m9x_faa_depth_views views;
	struct m9x_faa_dual_later_image later;
	struct m9x_faa_texture_owners *active, *other;
	struct m9x_faa_program_cache *programs = NULL;
	struct m9x_faa_mixed_templates tpl;
	const struct firmware *sample = NULL, *color = NULL, *dual_fw = NULL,
			      *single_fw = NULL;
	u8 *fresh_primary = NULL, *fresh_secondary = NULL, *arena = NULL,
	   *secondary = NULL;
	u8 *composed = NULL;
	u32 *slots = NULL, count = batch ? batch->count : 1, arena_bytes = 0,
	    sec_bytes = 0;
	u32 i, raw, span, first_bytes, ends[14], fresh_length = 0, epoch,
						 old_start = 0;
	u64 next = 0, origin;
	bool new_pair = false, single = false;
	int ret;
	if (!binding || !input || !textures || !textures->gem[0] ||
	    textures->generation != generation || !count ||
	    count > M9_FAA_DUAL_LATER_MAX_DRAWS ||
	    (batch && batch->clear_flags != clear_flags) ||
	    (clear_flags && clear_flags != 3 && clear_flags != 2))
		return -EOPNOTSUPP;
	slots = kmalloc_array(count, sizeof(*slots), GFP_KERNEL);
	if (!slots)
		return -ENOMEM;
	for (i = 0; i < count; i++) {
		const struct m9x_faa_color_input *job =
			batch ? &batch->draw[i].input : input;
		u32 kind = batch ? batch->draw[i].program :
				   PARHELIA_M9_FAA_PROGRAM_DUAL;

		ret = m9x_faa_mixed_draw_check(job, kind, &span);
		if (ret)
			goto release;
		single |= kind == PARHELIA_M9_FAA_PROGRAM_SINGLE;
		slots[i] = ALIGN(arena_bytes, 128);
		if (check_add_overflow(slots[i], span, &arena_bytes) ||
		    arena_bytes > 0x40000) {
			ret = -ENOSPC;
			goto release;
		}
	}
	ret = m9_faa_template_request(mdev, binding, generation, 0, &sample);
	if (ret)
		goto release;
	ret = m9_faa_template_request(mdev, binding, generation, 2, &color);
	if (ret)
		goto release;
	ret = request_firmware(&dual_fw,
			       "m9x/faa-graph-dual-first-template.bin",
			       &mdev->pdev->dev);
	if (ret)
		goto release;
	if (single) {
		ret = request_firmware(&single_fw, M9_FAA_SINGLE_FIRST_FIRMWARE,
				       &mdev->pdev->dev);
		if (ret)
			goto release;
	}
	next_input = kzalloc_obj(*next_input);
	scratch = kzalloc_obj(*scratch);
	frame = kvzalloc_obj(*frame);
	first_scratch = kvzalloc_obj(*first_scratch);
	later_scratch = kvzalloc_obj(*later_scratch);
	programs = kmalloc_obj(*programs);
	fresh_primary = kvzalloc(9200, GFP_KERNEL);
	fresh_secondary =
		kmalloc(M9_FAA_DUAL_FIRST_SECONDARY_BYTES, GFP_KERNEL);
	arena = kvzalloc(arena_bytes, GFP_KERNEL);
	secondary = kvzalloc(count * M9_FAA_APPEND_STAGE_MAX, GFP_KERNEL);
	composed = kmalloc(sizeof(frame->primary), GFP_KERNEL);
	if (!next_input || !scratch || !frame || !first_scratch ||
	    !later_scratch || !programs || !fresh_primary || !fresh_secondary ||
	    !arena || !secondary || !composed) {
		ret = -ENOMEM;
		goto release;
	}
	frame->stages = kvzalloc_objs(*frame->stages, count);
	frame->batch_program = kvzalloc(M9_FAA_PROGRAM_PAGE_BYTES, GFP_KERNEL);
	if (!frame->stages || !frame->batch_program) {
		ret = -ENOMEM;
		goto release;
	}
	next_input->constants = kzalloc(arena_bytes, GFP_KERNEL);
	if (!next_input->constants) {
		ret = -ENOMEM;
		goto release;
	}
	mutex_lock(&t->lock);
	ret = m9_faa_depth_frame_retained_state(mdev, binding, generation,
						previous, &next);
	if (ret)
		goto unlock;
	r = t->faa;
	/* The frame continues from the Unlock, or from the ordinary jobs
	 * after it. A GPU presentation or pool maintenance has no source form.
	 */
	origin = m9_faa_frame_origin(r, previous);
	if ((origin != previous && !m9_faa_ordinary_after(r, previous)) ||
	    r->present_point || r->command_pools ||
	    !r->depth_storage || !r->depth_storage->initialized ||
	    r->depth_storage->frame || !r->color_image || !r->programs ||
	    (r->dual_active_second ? &r->dual_next_textures : &r->dual_textures)
			    ->generation != generation) {
		ret = -ESTALE;
		goto unlock;
	}
	active = r->dual_active_second ? &r->dual_next_textures :
					 &r->dual_textures;
	other = r->dual_active_second ? &r->dual_textures :
					&r->dual_next_textures;
	if (!active->gem[0]) {
		ret = -ESTALE;
		goto unlock;
	}
	/* A new set takes the idle slot. Its previous set belongs to an older
  * frame whose Lock retired before this graph's Unlock; release it now.
  * The original rebind changes only typed sampler words, regenerated
  * from each live owner at submit. */
	new_pair = textures->gem[0] != active->gem[0] ||
		   textures->gem[1] != active->gem[1] ||
		   memcmp(textures->bind, active->bind, sizeof(active->bind));
	if (new_pair && other->gem[0] &&
	    !m9x_faa_texture_owners_put(other, false)) {
		ret = -EBUSY;
		goto unlock;
	}
	ret = m9x_faa_graph_bound_depth_views_locked(mdev, binding, generation,
						     &views);
	if (ret)
		goto unlock;
	if (!m9x_faa_runtime_shape(views.owner[M9_FAA_BOUND_COLOR].width,
				   views.owner[M9_FAA_BOUND_COLOR].height)) {
		ret = -EOPNOTSUPP;
		goto unlock;
	}
	if (arena_bytes > r->draw_owner[1].bytes) {
		ret = -ERANGE;
		goto unlock;
	}
	m9_faa_depth_frame_owners(r, owner);
	m9_faa_mixed_templates(&tpl, sample, color, dual_fw, single_fw);
	memcpy(programs, r->programs, sizeof(*programs));
	epoch = (u32)origin - (clear_flags == 3 ? 0 :
			       clear_flags == 2 ? 0x30 :
						  0x80);
	for (i = 0; i < count; i++) {
		struct m9x_faa_color_input job = batch ? batch->draw[i].input :
							 *input;
		u32 kind = batch ? batch->draw[i].program :
				   PARHELIA_M9_FAA_PROGRAM_DUAL;
		const struct m9x_faa_texture_owners *own =
			i && draw_textures && draw_textures[i].gem[0] ?
				&draw_textures[i] :
				textures;
		struct m9x_faa_color_image live = {
			.program = scratch->program,
			.constants = arena + slots[i],
			.primary = scratch->primary,
			.secondary = scratch->secondary,
			.program_bytes = sizeof(scratch->program),
			.constants_bytes = arena_bytes - slots[i],
			.primary_bytes = sizeof(scratch->primary),
			.secondary_bytes = sizeof(scratch->secondary),
			.secondary_length = &fresh_length
		};
		struct m9x_faa_dual_append_image appended = {
			.primary = composed,
			.secondary = secondary + sec_bytes,
			.primary_capacity = sizeof(frame->primary),
			.secondary_capacity = M9_FAA_APPEND_STAGE_MAX
		};
		enum m9x_faa_later_form form;
		bool cull_state;
		u32 offset, stage_bytes;
		/* Each DRAW's owner set has its program's unit count. */
		if ((own->gem[1] ? PARHELIA_M9_FAA_PROGRAM_DUAL :
				   PARHELIA_M9_FAA_PROGRAM_SINGLE) != kind) {
			ret = -EINVAL;
			goto unlock;
		}
		ret = m9x_faa_program_cache_draw(programs, kind, job.program,
						 job.layout.program_bytes,
						 job.raster_flags, false, &form,
						 &cull_state, &offset);
		if (ret)
			goto unlock;
		job.data_offset = slots[i];
		job.width = input->width;
		job.height = input->height;
		ret = m9x_faa_mixed_first_encode(kind, &tpl, owner, &views,
						 r->primary_bytes, epoch, &job,
						 own->bind, offset,
						 fresh_primary, 9200,
						 fresh_secondary, &first_bytes,
						 ends, &live, first_scratch,
						 &stage_bytes);
		if (ret)
			goto unlock;
		if (ends[9] < r->primary_bytes ||
		    ends[9] - r->primary_bytes +
				    M9_FAA_DUAL_FIRST_PRIMARY_TAIL_BYTES !=
			    first_bytes) {
			ret = -ESTALE;
			goto unlock;
		}
		if (!i) {
			memcpy(next_input->program, job.program,
			       job.layout.program_bytes);
			later = (struct m9x_faa_dual_later_image){
				.primary = frame->primary,
				.secondary = secondary,
				.primary_capacity = sizeof(frame->primary),
				.secondary_capacity = M9_FAA_APPEND_STAGE_MAX
			};
			ret = m9x_faa_later_encode(
				sample->data, sample->size, owner, &views,
				r->graph_draw_end_bytes,
				r->color_secondary_cursor, origin, next,
				clear_flags, fresh_secondary, stage_bytes,
				fresh_primary + ends[9] - r->primary_bytes,
				form, cull_state, &later, later_scratch);
			if (ret)
				goto unlock;
			frame->stages[0] = (struct m9x_faa_frame_stage){
				0,
				later.secondary_bytes,
				later.units,
				{ later.site[0], later.site[1] }
			};
			sec_bytes = later.secondary_bytes;
			continue;
		}
		ret = m9x_faa_later_append_encode(
			owner, origin, clear_flags, i,
			r->color_secondary_cursor + old_start,
			r->color_secondary_cursor + sec_bytes, later.primary,
			later.primary_bytes, fresh_secondary, stage_bytes, form,
			cull_state, &appended);
		if (ret)
			goto unlock;
		memcpy(later.primary, composed, appended.primary_bytes);
		later.primary_bytes = appended.primary_bytes;
		frame->stages[i] = (struct m9x_faa_frame_stage){
			sec_bytes,
			appended.secondary_bytes,
			appended.units,
			{ sec_bytes + appended.site[0],
			  appended.site[1] ? sec_bytes + appended.site[1] : 0 }
		};
		old_start = sec_bytes;
		sec_bytes += appended.secondary_bytes;
		/* The DRAW END and the Lock ENDs move with the inserted pair group. */
		for (raw = 1; raw <= 4; raw++)
			later.end_bytes[later.end_count - raw] += 60;
		later.final_point += 0xa0;
	}
	if (r->color_secondary_cursor > 0x3fe00 ||
	    sec_bytes > 0x3fe00 - r->color_secondary_cursor) {
		ret = -ERANGE;
		goto unlock;
	}
	memcpy(next_input->constants, arena, arena_bytes);
	next_input->geometry_offset = 0;
	next_input->constants_bytes = arena_bytes;
	next_input->geometry_bytes = 0;
	next_input->program_bytes = input->layout.program_bytes;
	next_input->layout = input->layout;
	memcpy(next_input->secondary, fresh_secondary, 2880);
	next_input->secondary_bytes = 2880;
	memcpy(frame->batch_program, programs->page, M9_FAA_PROGRAM_PAGE_BYTES);
	frame->programs = programs;
	programs = NULL;
	frame->primary_start = r->graph_draw_end_bytes;
	frame->primary_bytes = later.primary_bytes;
	frame->secondary_cursor = r->color_secondary_cursor;
	frame->secondary_bytes = sec_bytes;
	frame->batch_data = arena;
	arena = NULL;
	frame->batch_data_bytes = arena_bytes;
	frame->batch_secondary = secondary;
	secondary = NULL;
	memcpy(frame->end_bytes, later.end_bytes,
	       later.end_count * sizeof(u32));
	frame->end_count = later.end_count;
	frame->previous_point = previous;
	frame->queue_start = next;
	frame->final_point = later.final_point;
	frame->generation = generation;
	frame->initialized_reuse = true;
	frame->clear_flags = clear_flags;
	frame->draw_count = count;
	frame->dual_first = true;
	frame->dual_later = true;
	frame->dual_next_pair = new_pair ? !r->dual_active_second :
					   r->dual_active_second;
	if (new_pair) {
		*other = *textures;
		memset(textures, 0, sizeof(*textures));
	}
	old_input = r->color_image;
	r->color_image = next_input;
	next_input = NULL;
	r->depth_storage->frame = frame;
	frame = NULL;
	ret = 0;
unlock:
	mutex_unlock(&t->lock);
release:
	kfree(slots);
	if (sample)
		release_firmware(sample);
	if (color)
		release_firmware(color);
	if (dual_fw)
		release_firmware(dual_fw);
	if (single_fw)
		release_firmware(single_fw);
	kvfree(fresh_primary);
	kfree(fresh_secondary);
	kvfree(arena);
	kvfree(secondary);
	kfree(composed);
	kfree(programs);
	kvfree(first_scratch);
	kvfree(later_scratch);
	kfree(scratch);
	m9_faa_depth_frame_free(frame);
	if (next_input)
		kfree(next_input->constants);
	kfree(next_input);
	if (old_input)
		kfree(old_input->constants);
	kfree(old_input);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

/* Provision independent command MDLs before a future idle pool transition.
 * All slot identities are true native allocations; the registry never owns
 * copied owners or rewinds a cursor. Failure keeps partial mappings/CPA state
 * in the retained graph so DMA-stop teardown can restore every allocation. */
int m9x_faa_graph_command_pool_prepare(struct m9x_device *mdev,
 struct m9_faa_graph_binding *binding,u32 generation,u64 previous)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 struct m9x_transport *t=&mdev->transport;struct m9x_faa_replay *r;
 struct m9_faa_command_pools *pool=NULL;
 struct m9x_faa_counted_secondary_owner old,selected;
 const struct m9_faa_owner_spec spec={.bytes=0x40000};
 u64 next=0;u32 i,raw;bool claimed=false;int ret;
 if(!binding)return -EINVAL;
 pool=kzalloc(sizeof(*pool),GFP_KERNEL);if(!pool)return -ENOMEM;
 mutex_lock(&t->lock);
 ret=m9_faa_depth_frame_retained_state(mdev,binding,generation,previous,&next);if(ret)goto unlock;
 r=t->faa;
 if(!r->depth_storage||!r->depth_storage->initialized||r->depth_storage->frame){ret=-ESTALE;goto unlock;}
 if(r->command_pools){ret=r->command_pools->ready&&r->command_pools->generation==generation?0:-ESTALE;goto unlock;}
 r->command_pools=pool;pool->generation=generation;claimed=true;
 mutex_unlock(&t->lock);
 for(i=0;i<2;i++){
  ret=m9_faa_allocate(mdev,&pool->spare[i],&spec,false);if(ret)goto release;
  ret=m9_faa_dma_prepare_wc(mdev,&pool->spare[i]);if(ret)goto release;
 }
 mutex_lock(&t->lock);
 ret=m9_faa_depth_frame_retained_state(mdev,binding,generation,previous,&next);if(ret)goto unlock;
 r=t->faa;
 if(r->command_pools!=pool||pool->ready){ret=-ESTALE;goto unlock;}
 for(i=0;i<2;i++){
  struct m9_faa_owner *o=&pool->spare[i];
  ret=m9x_address_map_faa_system(mdev,&o->system_node,o->dma,o->bytes);if(ret)goto unlock;
  o->base=o->system_node.start;
 }
 /* No native pool rotation preceded this registry. The source INIT lane
  * is a genuine observed low32 value from the same retained counter page;
  * later extensions are bounded by this shared submitted/observed timeline. */
 dma_rmb();raw=READ_ONCE(((u32*)t->fence_cpu)[8])&~15u;
 if(!raw){ret=-EAGAIN;goto unlock;}
 ret=m9x_faa_pool_timeline_init(&pool->timeline,generation,raw);if(ret)goto unlock;
 for(i=0;i<2;i++){
  struct m9_faa_owner *o=i?&r->draw_owner[2]:&r->owner[M9_FAA_PRIMARY_OWNER];
  struct m9x_faa_secondary_pool *manager=i?&pool->secondary:&pool->primary;
  if(!o->cpu||!o->cpu_wc||!drm_mm_node_allocated(&o->system_node)||o->system_node.start!=o->base||o->bytes!=0x40000){ret=-ESTALE;goto unlock;}
  old=(struct m9x_faa_counted_secondary_owner){.identity=(unsigned long)o,
   .gpu_base=o->base,.bytes=o->bytes,.cpu_refs=1,.mapped=true};
  ret=m9x_faa_secondary_pool_init(manager,&pool->timeline,generation,&old,
   i?r->color_secondary_cursor:r->graph_draw_end_bytes);if(ret)goto unlock;
  o=&pool->spare[i];
  selected=(struct m9x_faa_counted_secondary_owner){.identity=(unsigned long)o,
   .gpu_base=o->base,.bytes=o->bytes,.mapped=true};
  ret=m9x_faa_secondary_pool_bind(manager,&selected);if(ret)goto unlock;
 }
 pool->ready=true;ret=0;
unlock:mutex_unlock(&t->lock);
release:if(!claimed)kfree(pool);return ret;
#else
 return -EOPNOTSUPP;
#endif
}

#ifdef M9_FAA_GRAPH_INIT_CONTROL
static int m9_faa_command_owner_admit(struct m9x_device *mdev,struct m9_faa_owner *o)
{
 if(!o||o->vram||!o->cpu||!o->cpu_wc||o->bytes!=0x40000||
  !drm_mm_node_allocated(&o->system_node)||o->system_node.start!=o->base||
  o->system_node.size!=o->bytes)return -ESTALE;
	return m9x_system_pages_mapped(mdev, o->base, o->dma,
				       o->bytes / PAGE_SIZE, 1u, NULL, NULL);
}
/* A maintenance E3 belongs to the idle command-pool transaction, not to the
 * preceding application Unlock. Both original receipts remain immutable.
 * Only the newly appended footer is referenced by the parent counted range.
 * It is a new original 249a0(mode1,flags0) scope, so its end selector is0.
 * Earlier DRAW scopes (whose end selector is2) are never closed or replayed. */
static int m9_faa_secondary_rotate(struct m9x_device *mdev,
				   struct m9_faa_graph_binding *binding,
				   u32 generation, u64 previous, u32 needed)
{
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	struct m9_faa_command_pools *p;
	struct m9_faa_owner *old, *selected, *parent;
	struct m9x_faa_secondary_pool_observation observed;
	struct m9x_faa_counted_secondary_image image;
	struct m9x_faa_secondary_pool_transition token;
	u64 next = 0, point = 0;
	u32 i, raw, parent_start;
	bool posted = false;
	int ret;

	mutex_lock(&t->lock);
	ret = m9_faa_depth_frame_retained_state(mdev, binding, generation,
						previous, &next);
	if (ret)
		goto unlock;
	r = t->faa;
	if (!r->depth_storage || !r->depth_storage->initialized ||
	    r->depth_storage->frame) {
		ret = -ESTALE;
		goto unlock;
	}
	if (needed > 0x3fe00 - 48) {
		ret = -ENOSPC;
		goto unlock;
	}
	if (r->color_secondary_cursor <= 0x3fe00 &&
	    needed + 48 <= 0x3fe00 - r->color_secondary_cursor) {
		ret = 0;
		goto unlock;
	}
	mutex_unlock(&t->lock);
	ret = m9x_faa_graph_command_pool_prepare(mdev, binding, generation,
						 previous);
	if (ret)
		return ret;
	mutex_lock(&t->lock);
	ret = m9_faa_depth_frame_retained_state(mdev, binding, generation,
						previous, &next);
	if (ret)
		goto unlock;
	r = t->faa;
	p = r->command_pools;
	if (!p || !p->ready || p->generation != generation ||
	    p->maintenance_point) {
		ret = -ESTALE;
		goto unlock;
	}
	old = m9_faa_command_owner(r, true);
	parent = m9_faa_command_owner(r, false);
	selected = p->secondary.active ? &r->draw_owner[2] : &p->spare[1];
	ret = m9_faa_command_owner_admit(mdev, old);
	if (ret)
		goto unlock;
	ret = m9_faa_command_owner_admit(mdev, selected);
	if (ret)
		goto unlock;
	ret = m9_faa_command_owner_admit(mdev, parent);
	if (ret)
		goto unlock;
	if (p->secondary.cursor > r->color_secondary_cursor ||
	    p->primary.cursor > r->graph_draw_end_bytes) {
		ret = -ESTALE;
		goto unlock;
	}
	ret = m9x_faa_secondary_pool_advance(&p->secondary, p->secondary.cursor,
					     r->color_secondary_cursor -
						     p->secondary.cursor);
	if (ret)
		goto unlock;
	ret = m9x_faa_secondary_pool_advance(&p->primary, p->primary.cursor,
					     r->graph_draw_end_bytes -
						     p->primary.cursor);
	if (ret)
		goto unlock;
	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++)
		if (!dma_resv_test_signaled(m9x_faa_graph_binding_owner(binding,
									i)
						    ->resv,
					    DMA_RESV_USAGE_READ)) {
			ret = -EBUSY;
			goto unlock;
		}
	dma_rmb();
	raw = READ_ONCE(((u32 *)t->fence_cpu)[8]);
	ret = m9x_faa_secondary_pool_observe_raw(&p->secondary, raw,
						 t->completed_point, generation,
						 true, &observed);
	if (ret)
		goto unlock;
	parent_start = r->graph_draw_end_bytes;
	ret = m9x_faa_secondary_pool_plan(&p->secondary,
					  r->color_secondary_cursor,
					  parent_start, parent->bytes,
					  t->submitted_point,
					  p->timeline.submitted_point,
					  t->fence_bus, &observed, &image,
					  &token);
	if (ret)
		goto unlock;
	if (image.secondary_point != m9_faa_idle_origin(r, previous) + 0x10 ||
	    image.parent_next != parent_start + 24) {
		ret = -ESTALE;
		goto abort;
	}
	for (i = 0; i < 3; i++)
		put_unaligned_le32(image.old_footer[i],
				   (u8 *)old->cpu + r->color_secondary_cursor +
					   4 * i);
	for (i = 0; i < 6; i++)
		put_unaligned_le32(image.parent[i],
				   (u8 *)parent->cpu + parent_start + 4 * i);
	/* The WC footer and parent words precede their END post. */
	wmb();
	ret = m9x_post_faa_graph_end_locked(mdev, &parent->system_node,
					    parent->dma, parent_start,
					    image.parent_next, false,
					    lower_32_bits(image.secondary_point),
					    &point, &posted);
	if (!posted)
		goto abort;
	/* An exposed END owns both command allocations even if its wait failed. */
	if (m9x_faa_secondary_pool_posted(&p->secondary, &token)) {
		ret = -EIO;
		goto quarantine;
	}
	p->rotated = true;
	r->graph_draw_end_bytes = image.parent_next;
	r->graph_draw_posts++;
	r->color_secondary_cursor = 0;
	if (ret || point != image.secondary_point) {
		if (!ret)
			ret = -EIO;
		goto quarantine;
	}
	/* A global E3 can retire before the following physical +20 write. Require
	 * the genuine shared pool lane as well, never seed it from the submission.
	 */
	for (i = 0; i < 10000; i++) {
		dma_rmb();
		raw = READ_ONCE(((u32 *)t->fence_cpu)[8]);
		ret = m9x_faa_secondary_pool_observe_raw(&p->secondary, raw,
							 t->completed_point,
							 generation, true,
							 &observed);
		if (!ret && observed.pool_point == image.pool_point)
			break;
		if (ret && ret != -EAGAIN)
			goto quarantine;
		usleep_range(100, 200);
	}
	if (i == 10000) {
		ret = -ETIMEDOUT;
		goto quarantine;
	}
	p->unlock_point = previous;
	p->maintenance_point = point;
	p->unlock_end = m9_faa_idle_end(r);
	p->unlock_posts = m9_faa_idle_posts(r);
	p->parent_end = image.parent_next;
	p->secondary_cursor = 0;
	p->posts++;
	ret = m9x_faa_secondary_pool_advance(&p->primary, parent_start, 24);
	if (ret)
		goto quarantine;
	dev_info(&mdev->pdev->dev,
		 "M9 FAA secondary pool retired: Unlock=%#llx maintenance=%#llx pool=%#llx old=%#x selected=%#x parent=%#x\n",
		 previous, point, observed.pool_point, old->base,
		 selected->base, parent->base + image.parent_next);
	ret = 0;
	goto unlock;
abort:
	if (m9x_faa_secondary_pool_abort(&p->secondary, &token))
		ret = -EIO;
	goto unlock;
quarantine:
	m9x_faa_secondary_pool_quarantine(&p->secondary);
	m9x_faa_secondary_pool_quarantine(&p->primary);
	t->wedged = true;
	t->generation++;
unlock:
	mutex_unlock(&t->lock);
	return ret;
}
#endif

bool m9x_faa_graph_empty_primary_locked(struct m9x_device *mdev,
 const struct drm_mm_node *node,dma_addr_t dma)
{
 struct m9x_transport *t=&mdev->transport;
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 struct m9x_faa_replay *r=t->faa;struct m9_faa_command_pools *p;
 struct m9_faa_owner *o;
 lockdep_assert_held(&t->lock);
 if(!r||!(p=r->command_pools)||!p->ready||p->generation!=t->generation||
  !p->primary.serial||p->primary.cursor||p->primary.planned||p->primary.quarantined||
  p->timeline.quarantined||p->timeline.completed_point!=p->timeline.submitted_point||
  r->graph_draw_end_bytes)return false;
 o=m9_faa_command_owner(r,false);
 return o&&node==&o->system_node&&dma==o->dma;
#else
 (void)t;(void)node;(void)dma;return false;
#endif
}

/* An ordinary job continues the retained primary at the graph's last END:
 * after the retired INIT until FIRST posts, or after a retired Unlock (and
 * PRESENT). A prepared later frame or pool transition owns that cursor. At
 * capacity the drained requester restarts at the owner's base, as INIT did.
 */
#ifdef M9_FAA_GRAPH_INIT_CONTROL
/* -EBUSY while the graph owns the cursor: until its INIT retired, from FIRST
 * to Unlock, from a prepared frame through PRESENT, or after a pool event.
 */
static int m9_faa_ordinary_idle(struct m9x_device *mdev, u32 *start)
{
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r = t->faa;

	if (!r || !t->faa_virtual_primary || !t->faa_source_context ||
	    !m9x_faa_graph_active(mdev) || m9x_faa_graph_preparing(mdev))
		return -EACCES;
	if (!r->unlock_point) {
		/* An unposted FIRST frame re-checks the moved origin itself. */
		if (!r->observation_retired || !r->observation_point ||
		    r->graph_draw_posts)
			return -EBUSY;
		*start = m9_faa_first_bytes(r, r->observation_point);
		return 0;
	}
	/* A pool event in this idle period keeps its own origin. */
	if (!r->unlock_retired ||
	    r->present_attempted != r->present_retired ||
	    (r->depth_storage && r->depth_storage->frame) ||
	    (r->command_pools &&
	     (r->command_pools->primary.planned ||
	      r->command_pools->unlock_point == r->unlock_point)))
		return -EBUSY;
	*start = r->graph_draw_end_bytes;
	return 0;
}

/* Graph progress that can end an -EBUSY period. */
static u64 m9_faa_ordinary_progress(const struct m9x_transport *t)
{
	const struct m9x_faa_replay *r = t->faa;

	return t->submitted_point + t->completed_point +
	       (r ? r->graph_draw_posts + r->unlock_point + r->present_point +
		r->present_retired + (r->depth_storage &&
				      r->depth_storage->frame) : 0);
}
#endif

void m9x_faa_idle_wake(struct m9x_device *mdev)
{
	m9x_transport_progress_wake(&mdev->transport);
}

/* An ordinary submit waits for the graph's idle boundary instead of failing.
 * Called without the transport lock and before any reservation is held: the
 * graph's producers take the same objects. Driver ioctls and file close wake
 * it; it ends without graph progress for ten fence timeouts.
 */
int m9x_faa_ordinary_wait(struct m9x_device *mdev)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_transport *t = &mdev->transport;
	unsigned long deadline = 0;
	u64 progress = 0;
	u32 start, seq;
	long left;
	int ret;

	for (;;) {
		mutex_lock(&t->lock);
		seq = READ_ONCE(t->faa_idle_seq);
		ret = m9x_faa_ordinary_continue_enabled() ?
		      m9_faa_ordinary_idle(mdev, &start) : 0;
		if (ret == -EBUSY && (!deadline ||
				      m9_faa_ordinary_progress(t) != progress)) {
			progress = m9_faa_ordinary_progress(t);
			deadline = jiffies + 10 * m9x_fence_timeout_jiffies();
		}
		mutex_unlock(&t->lock);
		if (ret != -EBUSY)
			return 0;
		if (time_after_eq(jiffies, deadline))
			return -EBUSY;
		left = deadline - jiffies;
		left = wait_event_interruptible_timeout(t->faa_idle_wait,
							READ_ONCE(t->faa_idle_seq) != seq,
							left);
		if (left < 0)
			return left;
	}
#else
	return 0;
#endif
}

/* Whether the graph owns the cursor now; see m9x_faa_ordinary_wait(). */
bool m9x_faa_ordinary_blocked_locked(struct m9x_device *mdev)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	u32 start;

	lockdep_assert_held(&mdev->transport.lock);
	return m9x_faa_ordinary_continue_enabled() &&
	       m9_faa_ordinary_idle(mdev, &start) == -EBUSY;
#else
	return false;
#endif
}

int m9x_faa_ordinary_primary_locked(struct m9x_device *mdev, u32 bytes,
				    struct m9x_faa_ordinary_primary *out)
{
	struct m9x_transport *t = &mdev->transport;
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_faa_replay *r = t->faa;
	struct m9_faa_owner *o;
	u32 capacity, start;
	int ret;

	lockdep_assert_held(&t->lock);
	ret = m9_faa_ordinary_idle(mdev, &start);
	if (ret)
		return ret;
	o = m9_faa_command_owner(r, false);
	if (!o || !o->cpu || !drm_mm_node_allocated(&o->system_node) ||
	    o->system_node.start != o->base)
		return -ESTALE;
	capacity = min_t(u64, o->bytes, o->system_node.size);
	if (!bytes || bytes > capacity || start > capacity)
		return -ENOSPC;
	out->rebase = bytes > capacity - start;
	if (out->rebase) {
		/* The caller drains first. Only an owner the pool manager has
		 * not planned away restarts.
		 */
		if (!IS_ALIGNED(o->system_node.start, PAGE_SIZE) ||
		    (r->command_pools && r->command_pools->ready &&
		     (r->command_pools->primary.planned ||
		      r->command_pools->timeline.planned)))
			return -EBUSY;
		start = 0;
	}
	out->node = &o->system_node;
	out->dma = o->dma;
	out->cpu = o->cpu;
	out->offset = start;
	return 0;
#else
	lockdep_assert_held(&t->lock);
	return -EACCES;
#endif
}

void m9x_faa_ordinary_primary_commit_locked(struct m9x_device *mdev,
					    u32 end_bytes, u64 point,
					    bool rebase)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_faa_replay *r = mdev->transport.faa;
	struct m9_faa_command_pools *p = r->command_pools;
	u64 base = m9_faa_ordinary_base(r);

	lockdep_assert_held(&mdev->transport.lock);
	/* The active pool owner is consumed again from its base. */
	if (rebase && p && p->ready && p->primary.initialized)
		p->primary.cursor = 0;
	if (!m9_faa_ordinary_after(r, base))
		r->ordinary_base = base;
	r->ordinary_point = point;
	r->ordinary_end = end_bytes;
	r->graph_draw_end_bytes = end_bytes;
	r->ordinary_posts++;
#else
	lockdep_assert_held(&mdev->transport.lock);
#endif
}

#ifdef M9_FAA_GRAPH_INIT_CONTROL
static void m9_faa_stall_owner(struct m9x_device *mdev, const char *name,
			       const struct m9_faa_owner *o, u32 head, u32 end)
{
	u32 start, off, page, pte, i, first, last;
	const u32 *words;

	if (!o || !o->cpu || !drm_mm_node_allocated(&o->system_node))
		return;
	start = (u32)o->system_node.start;
	if (head - start >= o->bytes && end - start >= o->bytes)
		return;
	off = head - start < o->bytes ? head - start : end - start;
	for (page = off / PAGE_SIZE; page <= (off + 0x100) / PAGE_SIZE &&
	     page < DIV_ROUND_UP(o->bytes, PAGE_SIZE); page++) {
		int ret = m9x_address_read_pte(mdev, start + page * PAGE_SIZE,
					       &pte);

		dev_err(&mdev->pdev->dev,
			"M9 FAA stall %s base=%#x page=%u pte=%#x expected=%#x ret=%d\n",
			name, start, page, ret ? 0 : pte,
			m9x_address_pte_word(o->dma + page * PAGE_SIZE) | 1u, ret);
	}
	/* Words around the fetch and before the latched END. */
	words = o->cpu;
	first = (off & ~3u) / 4 > 16 ? (off & ~3u) / 4 - 16 : 0;
	last = min((off & ~3u) / 4 + 16, o->bytes / 4);
	for (i = first; i + 4 <= last; i += 4)
		dev_err(&mdev->pdev->dev,
			"M9 FAA stall %s +%#06x %08x %08x %08x %08x\n", name,
			i * 4, words[i], words[i + 1], words[i + 2],
			words[i + 3]);
}
#endif

/* After a timeout outside the staging slots: which retained FAA command
 * owner holds the fetch and END, its PTEs, and the words there.
 */
void m9x_faa_report_stall_locked(struct m9x_device *mdev, u32 head, u32 end)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_faa_replay *r = mdev->transport.faa;

	lockdep_assert_held(&mdev->transport.lock);
	if (!r)
		return;
	dev_err(&mdev->pdev->dev,
		"M9 FAA stall: fetch=%#x end=%#x draw_end=%u ordinary=%#llx/%u secondary=%u\n",
		head, end, r->graph_draw_end_bytes, r->ordinary_point,
		r->ordinary_end, r->color_secondary_cursor);
	m9_faa_stall_owner(mdev, "primary", &r->owner[M9_FAA_PRIMARY_OWNER],
			   head, end);
	m9_faa_stall_owner(mdev, "secondary", &r->draw_owner[2], head, end);
	if (r->command_pools) {
		m9_faa_stall_owner(mdev, "spare-primary",
				   &r->command_pools->spare[0], head, end);
		m9_faa_stall_owner(mdev, "spare-secondary",
				   &r->command_pools->spare[1], head, end);
	}
#else
	lockdep_assert_held(&mdev->transport.lock);
#endif
}

#ifdef M9_FAA_GRAPH_INIT_CONTROL
static int m9_faa_primary_rotate(struct m9x_device *mdev,
 struct m9_faa_graph_binding *binding,u32 generation,u64 previous,u32 needed)
{
 struct m9x_transport *t=&mdev->transport;struct m9x_faa_replay *r;
 struct m9_faa_command_pools *p;struct m9_faa_owner *old,*selected;
 struct m9x_faa_secondary_pool_observation observed;
 struct m9x_faa_primary_pool_image image;m9x_faa_primary_transition token;
 u64 next=0;u32 i,raw;int ret;
 mutex_lock(&t->lock);
 ret=m9_faa_depth_frame_retained_state(mdev,binding,generation,previous,&next);if(ret)goto unlock;
 r=t->faa;
 if(!r->depth_storage||!r->depth_storage->initialized||r->depth_storage->frame){ret=-ESTALE;goto unlock;}
 if(needed>0x40000-16){ret=-ENOSPC;goto unlock;}
 if(r->graph_draw_end_bytes<=0x40000&&needed+16<=0x40000-r->graph_draw_end_bytes){ret=0;goto unlock;}
 mutex_unlock(&t->lock);
 ret=m9x_faa_graph_command_pool_prepare(mdev,binding,generation,previous);if(ret)return ret;
 mutex_lock(&t->lock);
 ret=m9_faa_depth_frame_retained_state(mdev,binding,generation,previous,&next);if(ret)goto unlock;
 r=t->faa;p=r->command_pools;
 if(!p||!p->ready||p->generation!=generation||p->unlock_point){ret=-ESTALE;goto unlock;}
 old=m9_faa_command_owner(r,false);selected=p->primary.active?&r->owner[M9_FAA_PRIMARY_OWNER]:&p->spare[0];
 ret=m9_faa_command_owner_admit(mdev,old);if(ret)goto unlock;
 ret=m9_faa_command_owner_admit(mdev,selected);if(ret)goto unlock;
 if(p->primary.cursor>r->graph_draw_end_bytes){ret=-ESTALE;goto unlock;}
 ret=m9x_faa_secondary_pool_advance(&p->primary,p->primary.cursor,r->graph_draw_end_bytes-p->primary.cursor);if(ret)goto unlock;
 for(i=0;i<PARHELIA_M9_FAA_GRAPH_OWNERS;i++)
  if(!dma_resv_test_signaled(m9x_faa_graph_binding_owner(binding,i)->resv,DMA_RESV_USAGE_READ)){ret=-EBUSY;goto unlock;}
 dma_rmb();raw=READ_ONCE(((u32*)t->fence_cpu)[8]);
 ret=m9x_faa_secondary_pool_observe_raw(&p->primary,raw,t->completed_point,generation,true,&observed);if(ret)goto unlock;
 ret=m9x_faa_primary_pool_plan(&p->primary,t->submitted_point,p->timeline.submitted_point,t->fence_bus,&observed,&image,&token);if(ret)goto unlock;
 if(image.new_end!=selected->base){ret=-ESTALE;goto abort;}
 for(i=0;i<4;i++)put_unaligned_le32(image.old_link[i],(u8*)old->cpu+r->graph_draw_end_bytes+4*i);
 /* Original mode0 posts the selected base after the old owner's jump.
  * COMMAND_BASE stays intact; there is no global E3 or invented completion. */
 wmb();m9x_write(mdev,M9_REG_COMMAND_END,image.new_end);
 ret=m9x_faa_primary_pool_posted(&p->primary,&token);if(ret)goto quarantine;
 p->rotated=true;r->graph_draw_end_bytes=0;r->graph_draw_posts++;
 for(i=0;i<10000;i++){
  dma_rmb();raw=READ_ONCE(((u32*)t->fence_cpu)[8]);
  ret=m9x_faa_secondary_pool_observe_raw(&p->primary,raw,t->completed_point,generation,true,&observed);
  if(!ret&&observed.pool_point==image.pool_point)break;
  if(ret&&ret!=-EAGAIN)goto quarantine;
  usleep_range(100,200);
 }
 if(i==10000){ret=-ETIMEDOUT;goto quarantine;}
 p->unlock_point=previous;p->unlock_end=m9_faa_idle_end(r);p->unlock_posts=m9_faa_idle_posts(r);
 p->parent_end=0;p->secondary_cursor=r->color_secondary_cursor;p->posts=1;
 dev_info(&mdev->pdev->dev,"M9 FAA primary pool retired: Unlock=%#llx pool=%#llx old=%#x selected=%#x END=%#x\n",previous,observed.pool_point,old->base,selected->base,image.new_end);
 ret=0;goto unlock;
abort:if(m9x_faa_primary_pool_abort(&p->primary,&token))ret=-EIO;goto unlock;
quarantine:
 m9x_faa_secondary_pool_quarantine(&p->primary);m9x_faa_secondary_pool_quarantine(&p->secondary);
 t->wedged=true;t->generation++;
unlock:mutex_unlock(&t->lock);return ret;
}
#endif

/* Authenticate the completed frame against the same immutable Unlock and
 * separately retired presentation/maintenance journals used by preparation.
 * The ioctl must publish that origin, not infer it from a returned Lock. */
int m9x_faa_graph_depth_frame_receipt_locked(struct m9x_device *mdev,
					     u32 generation, u64 previous,
					     u64 final_point, u64 *origin,
					     u64 *maintenance, u64 *ordinary)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_faa_replay *r = mdev->transport.faa;
	struct m9_faa_depth_frame_storage *f;
	struct m9_faa_command_pools *p;
	u64 base, effective;

	lockdep_assert_held(&mdev->transport.lock);
	if (!origin || !maintenance || !ordinary)
		return -EINVAL;
	*origin = 0;
	*maintenance = 0;
	*ordinary = 0;
	if (!r || mdev->transport.wedged || r->graph_generation != generation ||
	    mdev->transport.generation != generation || !r->depth_storage)
		return -ESTALE;
	f = r->depth_storage->frame;
	if (!f || !f->retired || !r->color_lock_retired ||
	    f->previous_point != previous || f->final_point != final_point ||
	    mdev->transport.completed_point != final_point)
		return -ESTALE;
	base = previous;
	effective = previous;
	if (f->initialized_reuse) {
		if (r->unlock_point != previous ||
		    r->present_attempted != r->present_retired ||
		    (r->present_point && !r->present_retired) ||
		    (r->present_retired &&
		     (previous > U64_MAX - 0x20 ||
		      r->present_generation != generation ||
		      r->present_point != previous + 0x20 ||
		      r->present_end != r->graph_unlock_end_bytes + 100 ||
		      r->present_posts != r->graph_unlock_posts + 3)))
			return -ESTALE;
		base = m9_faa_present_origin(r, previous);
		effective = base;
		/* The frame continued after ordinary jobs on the shared requester. */
		if (m9_faa_ordinary_after(r, base)) {
			effective = r->ordinary_point;
			*ordinary = effective;
		}
		p = r->command_pools;
		if (p && p->maintenance_point) {
			if (!p->ready || p->generation != generation ||
			    p->unlock_point != previous ||
			    effective > U64_MAX - 0x10 ||
			    p->maintenance_point != effective + 0x10)
				return -ESTALE;
			effective = p->maintenance_point;
		}
		if (f->clear_resume) {
			if (base != previous || effective != previous ||
			    previous > U64_MAX - 0x160 ||
			    f->resume_prefix.previous_unlock_point !=
				    previous ||
			    f->resume_prefix.clear_point != previous + 0x80 ||
			    f->queue_start != previous + 0x90 ||
			    final_point != previous + 0x160)
				return -ESTALE;
		} else if (effective > U64_MAX - 0x10 ||
			   f->queue_start != effective + 0x10)
			return -ESTALE;
	} else if (f->first && m9_faa_ordinary_after(r, previous)) {
		/* FIRST continued after ordinary jobs that followed INIT. */
		effective = r->ordinary_point;
		if (f->queue_start != effective)
			return -ESTALE;
		*ordinary = effective;
	}
	*origin = base;
	if (effective != base && effective != *ordinary)
		*maintenance = effective;
	return 0;
#else
	return -EOPNOTSUPP;
#endif
}

int m9x_faa_graph_depth_mode_locked(struct m9x_device *mdev,
 struct m9_faa_graph_binding *binding,u32 generation,bool first,
 struct m9x_faa_depth_mode_state *out)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 struct m9x_transport *t=&mdev->transport;struct m9x_faa_replay *r=t->faa;
 if(!out||!r||r->graph_binding!=binding||r->graph_generation!=generation||t->generation!=generation||t->wedged)return -ESTALE;
 if(first){
  if(r->depth_storage&&r->depth_storage->initialized)return -ESTALE;
  *out=(struct m9x_faa_depth_mode_state){0};return 0;
 }
 if(!r->depth_storage||!r->depth_storage->initialized||!r->depth_storage->mode.valid)return -ESTALE;
 *out=r->depth_storage->mode;return 0;
#else
 return -EOPNOTSUPP;
#endif
}

int m9x_faa_graph_depth_frame_maintenance_locked(struct m9x_device *mdev,
						 u32 generation, u64 previous,
						 u64 final_point,
						 u64 *maintenance)
{
	u64 origin, ordinary;
	int ret;

	ret = m9x_faa_graph_depth_frame_receipt_locked(mdev, generation,
						       previous, final_point,
						       &origin, maintenance,
						       &ordinary);
	return ret ? ret : ordinary ? -EOPNOTSUPP : 0;
}

#ifdef M9_FAA_GRAPH_INIT_CONTROL
/* The prefix is a real separately retired transaction, not a submitted
 * value inferred from Unlock. Keep its immutable original graph/owner/range
 * journal while preparing the remaining source caller; no prefix repost. */
static int m9_faa_clear_resume_state(struct m9x_device*mdev,
 struct m9_faa_graph_binding*binding,u32 generation,u64 previous,u64*next)
{
 struct m9x_transport*t=&mdev->transport;struct m9x_faa_replay*r=t->faa;
 struct m9_faa_depth_frame_storage*f;struct drm_parhelia_m9_faa_graph_clear_prefix a;
 struct m9_faa_owner*p,*q;u32 i,raw;
 lockdep_assert_held(&t->lock);if(!next)return -EINVAL;*next=0;
 if(t->wedged)return -EIO;
 if(!r||!binding||r->graph_binding!=binding||r->graph_generation!=generation||
  t->generation!=generation||!r->depth_storage||!r->depth_storage->ready||
  !r->depth_storage->initialized||!(f=r->depth_storage->frame))return -ESTALE;
 if(f->clear_prefix){
  if(!f->attempted||!f->retired)return -ESTALE;
  memcpy(&a,f->observed,sizeof(a));
 }else if(f->clear_resume){
  if(f->attempted||f->retired)return -ESTALE;
  a=f->resume_prefix;
 }else return -ESTALE;
 p=m9_faa_command_owner(r,false);q=m9_faa_command_owner(r,true);
 if(!p||!q||f->generation!=generation||f->previous_point!=previous||
  !r->color_image||r->color_image->layout.program_count!=2||
  !r->unlock_started||!r->unlock_retired||!r->color_lock_retired||
  r->color_lock_point+0x20!=previous||r->unlock_point!=previous||r->graph_draw_point!=previous||
  a.generation!=generation||a.previous_unlock_point!=previous||a.clear_point!=previous+0x80||
  a.version!=1||a.width!=320||a.height!=240||a.flags||a.reserved||a.state!=25||a.mode_flags!=3||
  a.primary_base!=p->base||a.primary_capacity!=p->bytes||a.secondary_base!=q->base||a.secondary_capacity!=q->bytes||
  a.primary_end!=r->graph_draw_end_bytes||a.secondary_cursor!=r->color_secondary_cursor||a.end_count!=9||
  a.primary_start>=a.primary_end||a.primary_end>p->bytes||a.secondary_cursor>q->bytes||
  (f->clear_prefix?r->graph_draw_posts!=f->planned_ends+9:r->graph_draw_posts!=f->prefix_posts)||
  !r->depth_storage->mode.valid||!r->depth_storage->mode.enabled||r->depth_storage->mode.embedded_hz||
  r->depth_storage->mode.valid!=f->mode_before.valid||
  r->depth_storage->mode.enabled!=f->mode_before.enabled||r->depth_storage->mode.embedded_hz!=f->mode_before.embedded_hz||
  r->present_attempted||r->present_point||
  (r->command_pools&&(r->command_pools->rotated||r->command_pools->unlock_point||r->command_pools->maintenance_point))||
  t->submitted_point!=a.clear_point||t->completed_point!=a.clear_point||
  t->next_fence!=lower_32_bits(a.clear_point)+0x10||t->queue_count||t->async_pending||
  previous>U64_MAX-0x160||lower_32_bits(previous)>U32_MAX-0x160)return -ESTALE;
 for(i=0;i<9;i++)if(a.end_bytes[i]<=a.primary_start||a.end_bytes[i]>a.primary_end||
  (i&&a.end_bytes[i]<a.end_bytes[i-1]))return -ESTALE;
 if(a.end_bytes[8]!=a.primary_end)return -ESTALE;
 if(!t->fence_cpu)return -ESTALE;
 dma_rmb();raw=READ_ONCE(((u32*)t->fence_cpu)[0]);
 if(raw!=a.raw00||(raw&~15u)!=lower_32_bits(a.clear_point)||(raw&15u)!=1)return -ESTALE;
 *next=a.clear_point+0x10;return 0;
}
#endif
int m9x_faa_graph_clear_resume_get_locked(struct m9x_device*mdev,u32 generation,
 u64 previous,struct m9_faa_graph_binding**out)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 struct m9x_faa_replay*r=mdev->transport.faa;u64 next;int ret;
 lockdep_assert_held(&mdev->transport.lock);if(!out)return -EINVAL;*out=NULL;
 ret=m9_faa_clear_resume_state(mdev,r?r->graph_binding:NULL,generation,previous,&next);
 if(!ret)*out=m9x_faa_graph_binding_get(r->graph_binding);return ret;
#else
 return -EOPNOTSUPP;
#endif
}

/* Initialized callers own an actual prior Lock/view/Unlock. Reuse the
 * three retained mappings and source clear state; never run FIRST constructors
 * or claim initialization from a copied request. Preparation posts nothing. */
/* A refusal before submit may retry the already prepared image, but only
 * the exact immutable owned semantic upload. No CPU reconstruction or input
 * replacement is permitted under that retained prefix entitlement. */
#ifdef M9_FAA_GRAPH_INIT_CONTROL
static bool m9_faa_clear_resume_input_equal(const struct m9_faa_depth_frame_storage*f,
 const struct m9x_faa_color_input*i)
{
 const struct m9x_faa_color_input*a=&f->resume_input;
 if(!f->resume_upload||memcmp(&a->layout,&i->layout,sizeof(a->layout))||
  a->record_stride!=i->record_stride||a->record_count!=i->record_count||
  a->declaration_count!=i->declaration_count||a->depth_flags!=i->depth_flags||
  a->fragment_flags!=i->fragment_flags||a->data_offset!=i->data_offset||
  a->width!=i->width||a->height!=i->height)return false;
 return !memcmp(a->program,i->program,i->layout.program_bytes)&&
  !memcmp(a->constants,i->constants,i->layout.constants_bytes)&&
  !memcmp(a->geometry,i->geometry,i->layout.geometry_bytes)&&
  !memcmp(a->declaration,i->declaration,i->declaration_count*4);
}
#endif
static int m9_faa_depth_later_prepare(struct m9x_device *mdev,
 struct m9_faa_graph_binding *binding,u32 generation,u64 previous,
 const struct m9x_faa_color_input *input,u32 clear_flags,u32 raster_flags,bool resumed)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 struct m9x_transport *t=&mdev->transport;struct m9x_faa_replay *r;
 struct m9_faa_depth_frame_storage *frame=NULL,*old_frame=NULL;
 struct m9x_faa_clear_resume_scratch *resume_scratch=NULL;
 struct m9x_faa_later_depth_scratch *scratch=NULL;
 struct m9x_faa_depth_mode_later_scratch *mode_scratch=NULL;
 struct m9x_faa_depth_mode_state before={0},after={0};
 struct m9x_faa_depth_mode_batch_plan plan;
 struct m9x_faa_depth_mode_batch_job mode_job;
 struct m9x_faa_later_depth_image image;
 struct m9x_faa_graph_draw_owner owner[M9_FAA_LATER_DEPTH_OWNERS];
 struct m9x_faa_depth_views views;
 struct m9_faa_color_storage *next_input=NULL,*old_input=NULL;
 const struct firmware *sample=NULL,*color=NULL,*reset_fw=NULL;
 struct m9x_faa_color_input typed_input;u8 *raster_scratch=NULL;
 u64 next=0;u32 capacity=0,upload_bytes=0;bool two;int ret;
 if(!binding||!input||(raster_flags&~7u)||(raster_flags&6)==6||(clear_flags&&clear_flags!=3)||
  (input->layout.program_count!=2&&input->layout.program_count!=4)||
  (input->declaration_count!=3&&input->declaration_count!=5)||
  (!(raster_flags&1)&&m9x_faa_color_flat_fragment_valid(input))||!m9_faa_color_four_experimental)return -EOPNOTSUPP;
 two=input->layout.program_count==2;
 if(resumed&&(!input->program||!input->constants||!input->geometry||!input->declaration))return -EINVAL;
 if(resumed&&(!two||input->data_offset||clear_flags!=3||raster_flags||input->fragment_flags||!(input->depth_flags&32)))return -EOPNOTSUPP;
 {struct m9x_faa_surface_layout layout;
 if(!m9x_faa_graph_binding_layout(binding,&layout))return -ESTALE;
 typed_input=*input;typed_input.width=layout.width;typed_input.height=layout.height;
 typed_input.fragment_flags=raster_flags&1;input=&typed_input;}
 /* Prove reuse before replacing any CPU snapshot. A failed preparation
  * preserves current retirement/view/Unlock entitlements and all GPU owners. */
 mutex_lock(&t->lock);
 ret=resumed?m9_faa_clear_resume_state(mdev,binding,generation,previous,&next):m9_faa_depth_frame_retained_state(mdev,binding,generation,previous,&next);
 r=t->faa;
 if(!ret&&resumed&&r->depth_storage->frame->clear_resume){
  ret=m9_faa_clear_resume_input_equal(r->depth_storage->frame,input)?0:-ESTALE;
  mutex_unlock(&t->lock);return ret;
 }
 if(!ret&&(!r->depth_storage||!r->depth_storage->initialized||(r->depth_storage->frame&&!resumed)))ret=-ESTALE;
 if(!ret)ret=m9x_faa_graph_depth_mode_locked(mdev,binding,generation,false,&before);
 if(!ret&&two){
  mode_job=(struct m9x_faa_depth_mode_batch_job){input->depth_flags,input->declaration_count};
  if(resumed){
   plan=(struct m9x_faa_depth_mode_batch_plan){.delta=0x160,.end_count=7};
  }else ret=m9x_faa_depth_mode_batch_count(false,clear_flags,&before,&mode_job,1,&plan);
 }
 if(!ret&&!two&&before.embedded_hz)ret=-EOPNOTSUPP;
 /* Count2 stages inputs privately below; its final locked owner/PTE scan
  * still precedes encoder writes and installation of either CPU snapshot.
  * Avoid scanning the entire attachment twice around CPU-only allocations.
  * The legacy count4 NEXT producer replaces its snapshot earlier, so keep
  * that path's preallocation mapping gate. */
 if(!ret&&!two)ret=m9x_faa_graph_bound_depth_views_locked(mdev,binding,generation,&views);
 mutex_unlock(&t->lock);if(ret)return ret;
 if(two){
  /* The non-depth NEXT producer is four-stage only. Stage count2 inputs
   * privately and install them only after the distinct retained encoder
   * validates its live owners, declaration, and queue epoch. */
  if(input->layout.program_bytes>4096||input->layout.constants_bytes>16384||
     input->layout.geometry_bytes>0x40000-768||
     (input->record_stride!=32&&(input->record_stride!=16||input->declaration_count!=3))||
     !input->record_count||input->record_count%3||input->record_count>12288)return -ERANGE;
  next_input=kzalloc(sizeof(*next_input),GFP_KERNEL);
  if(!next_input)return -ENOMEM;
  next_input->geometry_offset=ALIGN(input->layout.constants_bytes,128);
  if(next_input->geometry_offset>0x40000-input->layout.geometry_bytes-768){ret=-ERANGE;goto release;}
  capacity=next_input->geometry_offset+input->layout.geometry_bytes+768;
  next_input->constants=kzalloc(capacity,GFP_KERNEL);
  if(!next_input->constants){ret=-ENOMEM;goto release;}
 }
 frame=kvzalloc(sizeof(*frame),GFP_KERNEL);scratch=kzalloc(sizeof(*scratch),GFP_KERNEL);
 if(!frame||!scratch){ret=-ENOMEM;goto release;}
 if(resumed){
  if(check_add_overflow(input->layout.program_bytes,input->layout.constants_bytes,&upload_bytes)||
   check_add_overflow(upload_bytes,input->layout.geometry_bytes,&upload_bytes)||
   check_add_overflow(upload_bytes,input->declaration_count*4,&upload_bytes)||upload_bytes>0x42000){ret=-ERANGE;goto release;}
  frame->resume_upload=kvmalloc(upload_bytes,GFP_KERNEL);
  if(!frame->resume_upload){ret=-ENOMEM;goto release;}
  frame->resume_input=*input;frame->resume_input.program=frame->resume_upload;
  frame->resume_input.constants=frame->resume_upload+input->layout.program_bytes;
  frame->resume_input.geometry=frame->resume_input.constants+input->layout.constants_bytes;
  frame->resume_input.declaration=frame->resume_input.geometry+input->layout.geometry_bytes;
  memcpy((void*)frame->resume_input.program,input->program,input->layout.program_bytes);
  memcpy((void*)frame->resume_input.constants,input->constants,input->layout.constants_bytes);
  memcpy((void*)frame->resume_input.geometry,input->geometry,input->layout.geometry_bytes);
  memcpy((void*)frame->resume_input.declaration,input->declaration,input->declaration_count*4);
  resume_scratch=kvzalloc(sizeof(*resume_scratch),GFP_KERNEL);
  if(!resume_scratch){ret=-ENOMEM;goto release;}
  ret=request_firmware(&reset_fw,M9_FAA_DEPTH_MODE_RESET_FIRMWARE,&mdev->pdev->dev);if(ret)goto release;
 }
 if(two&&!clear_flags){
  mode_scratch=kvzalloc(sizeof(*mode_scratch),GFP_KERNEL);
  if(!mode_scratch){ret=-ENOMEM;goto release;}
  if(plan.mode_changes){ret=request_firmware(&reset_fw,M9_FAA_DEPTH_MODE_RESET_FIRMWARE,&mdev->pdev->dev);if(ret)goto release;}
 }
 if(raster_flags){raster_scratch=kvmalloc(M9_FAA_DEPTH_FRAME_PRIMARY_CAPACITY+M9_FAA_COLOR_SECONDARY_CAPACITY,GFP_KERNEL);if(!raster_scratch){ret=-ENOMEM;goto release;}}
 ret=m9_faa_template_request(mdev,binding,generation,0,&sample);if(ret)goto release;
 ret=m9_faa_template_request(mdev,binding,generation,two?1:2,&color);if(ret)goto release;
 if(!two){ret=m9x_faa_graph_color_next_prepare(mdev,binding,generation,previous,input,0);if(ret)goto release;}
 mutex_lock(&t->lock);
 ret=resumed?m9_faa_clear_resume_state(mdev,binding,generation,previous,&next):m9_faa_depth_frame_retained_state(mdev,binding,generation,previous,&next);if(ret)goto unlock;
 r=t->faa;
 if(!r->depth_storage||!r->depth_storage->initialized||(r->depth_storage->frame&&!resumed)){ret=-ESTALE;goto unlock;}
 if(two&&(!r->color_image||
          r->color_image->layout.program_count!=2||
          r->draw_owner[0].bytes<input->layout.program_bytes)){ret=-ESTALE;goto unlock;}
 ret=m9x_faa_graph_bound_depth_views_locked(mdev,binding,generation,&views);if(ret)goto unlock;
 m9_faa_depth_frame_owners(r,owner);
 image=(struct m9x_faa_later_depth_image){.image={
  two?next_input->program:r->color_image->program,
  two?next_input->constants:r->color_image->constants,frame->primary,frame->secondary,
  sizeof(r->color_image->program),two?capacity:r->color_image->geometry_offset+input->layout.geometry_bytes+768,
  sizeof(frame->primary),sizeof(frame->secondary),&frame->secondary_bytes}};
 if(resumed){
  ret=m9x_faa_depth_mode_clear_resume_encode(sample->data,sample->size,color->data,color->size,
   reset_fw->data,reset_fw->size,owner,&views,r->primary_bytes,r->graph_draw_end_bytes,r->color_secondary_cursor,
   previous,previous+0x80,&before,input,&image,resume_scratch,&after);
 }else if(two&&!clear_flags){
  ret=m9x_faa_depth_mode_later_encode(sample->data,sample->size,color->data,color->size,
   reset_fw?reset_fw->data:NULL,reset_fw?reset_fw->size:0,owner,&views,
   r->primary_bytes,r->graph_draw_end_bytes,r->color_secondary_cursor,
   m9_faa_frame_origin(r,previous),next,&before,input,&image,mode_scratch,&after);
 }else ret=(two?m9x_faa_later_depth_two_encode:m9x_faa_later_depth_encode)(sample->data,sample->size,color->data,color->size,
  owner,&views,r->primary_bytes,r->graph_draw_end_bytes,r->color_secondary_cursor,
  m9_faa_frame_origin(r,previous),next,clear_flags,input,&image,scratch);
 if(ret)goto unlock;
 if(two){
  if(clear_flags&&!resumed)after=plan.after;
  frame->mode_planned=true;frame->mode_before=before;frame->mode_after=after;
  frame->planned_delta=plan.delta;frame->planned_ends=plan.end_count;
 }
 if(image.final_point!=m9_faa_frame_origin(r,previous)+
     (two?plan.delta:(clear_flags?0x150:0xd0))||
  image.end_count!=(two?plan.end_count:(clear_flags?13:4))||image.end_bytes[image.end_count-1]!=r->graph_draw_end_bytes+image.primary_length){ret=-EIO;goto unlock;}
 if(raster_flags){
  u32 secondary_bytes=0;
  struct m9x_faa_raster_input ri={
   .primary=frame->primary,.secondary=frame->secondary,
   .primary_bytes=image.primary_length,.secondary_bytes=image.secondary_length,
   .secondary_base=owner[29].base+r->color_secondary_cursor,
   .range_at={image.clear_bytes+4,image.clear_bytes+8,
              image.clear_bytes+(two?16:52),image.clear_bytes+(two?20:56)},
   .database_bytes=0x1c1d8,.no_cull_start_bytes=0x149a0,.cull_start_bytes=0x147f8,.flags=raster_flags};
  struct m9x_faa_raster_output ro={
   .primary=raster_scratch,.secondary=raster_scratch+M9_FAA_DEPTH_FRAME_PRIMARY_CAPACITY,
   .primary_capacity=M9_FAA_DEPTH_FRAME_PRIMARY_CAPACITY,.secondary_capacity=M9_FAA_COLOR_SECONDARY_CAPACITY,
   .secondary_bytes=&secondary_bytes};
  bool resident=false;u32 i;
  for(i=0;i<ARRAY_SIZE(m9_faa_spans);i++)
   if(m9_faa_spans[i].owner==12&&!m9_faa_spans[i].offset&&m9_faa_spans[i].bytes==ri.database_bytes)resident=true;
  if(!resident||owner[12].bytes<ri.database_bytes){ret=-ESTALE;goto unlock;}
  ret=(two?m9x_faa_raster_two_encode:m9x_faa_raster_encode)(&ri,&ro);if(ret)goto unlock;
  /* Full live raster state rebinds the omitted cached middle continuations.
   * Their source-proven insertion changes only secondary ranges/branches. */
  if(secondary_bytes!=image.secondary_length+(two?0:((raster_flags&6)?24:0))){ret=-EINVAL;goto unlock;}
  memcpy(frame->primary,ro.primary,image.primary_length);
  memcpy(frame->secondary,ro.secondary,secondary_bytes);image.secondary_length=secondary_bytes;
 }
 if(resumed){
  old_frame=r->depth_storage->frame;frame->clear_resume=true;
  memcpy(&frame->resume_prefix,old_frame->observed,sizeof(frame->resume_prefix));
  frame->prefix_posts=r->graph_draw_posts;
 }
 frame->primary_start=r->graph_draw_end_bytes;frame->primary_bytes=image.primary_length;
 frame->secondary_cursor=r->color_secondary_cursor;frame->secondary_bytes=image.secondary_length;
 memcpy(frame->end_bytes,image.end_bytes,image.end_count*sizeof(u32));frame->end_count=image.end_count;
 frame->previous_point=previous;frame->queue_start=next;frame->final_point=image.final_point;
 frame->generation=generation;frame->initialized_reuse=true;frame->clear_flags=clear_flags;
 if(two){
  u32 first_bytes=input->declaration_count==3?1352u:1376u;
  /* The observer reads the original FIRST-shaped target descriptors,
   * while frame->secondary is the actual retained command image. Preserve
   * the former only as a CPU snapshot; it is never submitted or replayed. */
  memcpy(next_input->secondary,resumed?resume_scratch->later.later.first_secondary:(mode_scratch?mode_scratch->later.first_secondary:scratch->first_secondary),first_bytes);
  next_input->secondary_bytes=first_bytes;
  next_input->previous_unlock_point=previous;
  next_input->layout=input->layout;next_input->constants_bytes=input->layout.constants_bytes;
  next_input->geometry_bytes=128+ALIGN(input->record_count,4)*input->record_stride;
  next_input->program_bytes=input->layout.program_bytes;
  old_input=r->color_image;r->color_image=next_input;next_input=NULL;
 }
 frame->draw_count=1;r->depth_storage->frame=frame;frame=NULL;ret=0;
unlock:mutex_unlock(&t->lock);
release:kvfree(resume_scratch);m9_faa_depth_frame_free(old_frame);if(reset_fw)release_firmware(reset_fw);kvfree(mode_scratch);if(sample)release_firmware(sample);if(color)release_firmware(color);
 kvfree(raster_scratch);kfree(scratch);m9_faa_depth_frame_free(frame);
 if(next_input)kfree(next_input->constants);kfree(next_input);
 if(old_input)kfree(old_input->constants);kfree(old_input);return ret;
#else
 return -EOPNOTSUPP;
#endif
}

int m9x_faa_graph_depth_later_prepare(struct m9x_device*mdev,struct m9_faa_graph_binding*b,
 u32 generation,u64 previous,const struct m9x_faa_color_input*in,u32 clear,u32 raster)
{return m9_faa_depth_later_prepare(mdev,b,generation,previous,in,clear,raster,false);}
int m9x_faa_graph_clear_resume_prepare(struct m9x_device*mdev,struct m9_faa_graph_binding*b,
 u32 generation,u64 previous,const struct m9x_faa_color_input*in)
{return m9_faa_depth_later_prepare(mdev,b,generation,previous,in,3,0,true);}

int m9x_faa_graph_depth_batch_prepare(struct m9x_device *mdev,
 struct m9_faa_graph_binding *binding,u32 generation,u64 previous,bool first,
 const struct m9x_faa_owned_color_batch *batch)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 struct m9x_transport *t=&mdev->transport;struct m9x_faa_replay *r;
 struct m9_faa_depth_frame_storage *frame;
 struct m9_faa_color_storage *scratch=NULL;
 struct m9x_faa_graph_draw_owner owner[M9_FAA_DEPTH_DRAW_OWNERS];struct m9x_faa_depth_views views;
 const struct firmware *color=NULL,*reset_fw=NULL;
 struct m9x_faa_depth_mode_batch_job *mode_jobs=NULL;
 struct m9x_faa_depth_mode_batch_plan mode_plan;
 struct m9x_faa_depth_mode_state mode_before={0},mode_current={0},build_state={0};
 u64 build_point=0;u32 initial_ends=0,last_secondary=0;
 u8 *reset_scratch=NULL;u32 *mode_ends=NULL;
 u8 *arena=NULL,*secondary=NULL,*composed=NULL,*job_primary=NULL,*job_secondary=NULL;
 u8 *code_page=NULL,*jobs_secondary=NULL;
 struct m9x_faa_program_arena_job *jobs=NULL;
 u32 *code_slots=NULL,*slots=NULL;
 u32 secondary_needed=0,arena_bytes=0,sec_cursor,pb,sb,i,bytes,raw,appended_bytes;
 u64 queue_start=0;bool initialized,two;int ret;
	u64 base, stage_point;
 m9_faa_frame_trace(mdev,"batch-prepare-enter",batch?batch->count:0,0,previous,0,false);
 if(!binding||!batch||!batch->count||batch->count>M9_FAA_FRAME_BATCH_MAX_DRAWS)return -EOPNOTSUPP;
 initialized=!!(batch->flags&PARHELIA_M9_FAA_COLOR_FRAME_INITIALIZED);
 two=batch->draw[0].input.layout.program_count==2;
 if(initialized&&first)return -EOPNOTSUPP;
 if(!initialized&&!first)return -EOPNOTSUPP;
 if(two){
  mode_jobs=kmalloc_array(batch->count,sizeof(*mode_jobs),GFP_KERNEL);
  if(!mode_jobs)return -ENOMEM;
  for(i=0;i<batch->count;i++)mode_jobs[i]=(struct m9x_faa_depth_mode_batch_job){
    batch->draw[i].input.depth_flags,batch->draw[i].input.declaration_count};
  mutex_lock(&t->lock);
  ret=m9x_faa_graph_depth_mode_locked(mdev,binding,generation,first,&mode_before);
  mutex_unlock(&t->lock);if(ret)goto release;
  ret=m9x_faa_depth_mode_batch_count(first,batch->clear_flags,&mode_before,mode_jobs,batch->count,&mode_plan);
  if(ret)goto release;
  {struct m9x_faa_surface_layout shape;
   if(!m9x_faa_graph_binding_layout(binding,&shape)){ret=-ESTALE;goto release;}
   if(!m9x_faa_runtime_shape(shape.width,shape.height)){ret=-EOPNOTSUPP;goto release;}
  }
  if(mode_plan.mode_changes){
   ret=request_firmware(&reset_fw,M9_FAA_DEPTH_MODE_RESET_FIRMWARE,&mdev->pdev->dev);if(ret)goto release;
   reset_scratch=kmalloc(M9_FAA_DEPTH_MODE_RESET_CAPACITY,GFP_KERNEL);
   if(!reset_scratch){ret=-ENOMEM;goto release;}
  }
  if(!reset_scratch){reset_scratch=kmalloc(M9_FAA_DEPTH_MODE_RESET_CAPACITY,GFP_KERNEL);if(!reset_scratch){ret=-ENOMEM;goto release;}}
  mode_ends=kmalloc_array(M9_FAA_DEPTH_MODE_MAX_ENDS,sizeof(*mode_ends),GFP_KERNEL);
  if(!mode_ends){ret=-ENOMEM;goto release;}
 }
 slots=kmalloc(2u*batch->count*sizeof(*slots),GFP_KERNEL);
 if(!slots){ret=-ENOMEM;goto release;}
 code_slots=slots+batch->count;
 for(i=0;i<batch->count;i++){
  const struct m9x_faa_color_input *input=&batch->draw[i].input;
  /* Count2 emits its two source-qualified 6A bindings and EF state for
   * every owned job, including NONE/CW/CCW transitions within a batch. */
  if((batch->draw[i].raster_flags&~7u)||(batch->draw[i].raster_flags&6)==6||
   (!two&&(batch->draw[i].raster_flags&6)!=(batch->draw[0].raster_flags&6))||input->data_offset||
   (input->record_stride!=32&&(input->record_stride!=16||input->declaration_count!=3))||
   !input->record_count||input->record_count>12288||input->record_count%3||
   input->layout.constants_bytes>16384||input->layout.program_count!=(two?2u:4u)||(input->declaration_count!=3&&input->declaration_count!=5)){ret=-EOPNOTSUPP;goto release;}
  slots[i]=ALIGN(arena_bytes,128);
  if(check_mul_overflow(ALIGN(input->record_count,4),input->record_stride,&raw)||
   check_add_overflow(ALIGN(input->layout.constants_bytes,128),128u,&bytes)||
   check_add_overflow(bytes,raw,&bytes)||check_add_overflow(slots[i],bytes,&arena_bytes)||arena_bytes>0x40000){ret=-ENOSPC;goto release;}
  secondary_needed+=i?(two?1484u:2676u):
   (two?1352u:(initialized?2544u:2688u));
  secondary_needed+=input->declaration_count==5?24u:0u;
 }
 if(two)secondary_needed=mode_plan.secondary_bytes;
 if(initialized){
  if(!two)secondary_needed+=batch->clear_flags?0u:4u;
  if(!two&&(batch->draw[0].raster_flags&6))secondary_needed+=24u;
 }
 if((two&&12560+mode_plan.initial_reset_bytes+mode_plan.additional_primary_bytes>M9_FAA_DEPTH_FRAME_PRIMARY_CAPACITY)||secondary_needed+48>M9_FAA_BATCH_SECONDARY_BYTES){ret=-ENOSPC;goto release;}
 if(initialized){
  ret=m9_faa_primary_rotate(mdev,binding,generation,previous,
   two?12560+mode_plan.initial_reset_bytes+mode_plan.additional_primary_bytes+96+24:
       12560+60*(batch->count-1)+96+24);if(ret)goto release;
  ret=m9_faa_secondary_rotate(mdev,binding,generation,previous,secondary_needed);if(ret)goto release;
 }
 if(batch->count==1){ret=initialized?
  m9x_faa_graph_depth_later_prepare(mdev,binding,generation,previous,&batch->draw[0].input,batch->clear_flags,batch->draw[0].raster_flags):
  m9x_faa_graph_depth_frame_prepare(mdev,binding,generation,previous,true,&batch->draw[0].input,batch->draw[0].raster_flags);goto release;}
 m9_faa_frame_trace(mdev,"batch-allocate",batch->count,arena_bytes,previous,0,false);
 arena=kvzalloc(arena_bytes,GFP_KERNEL);
 secondary=kvzalloc(secondary_needed,GFP_KERNEL);
 composed=kvmalloc(M9_FAA_DEPTH_FRAME_PRIMARY_CAPACITY,GFP_KERNEL);
 job_primary=kvmalloc(M9_FAA_DEPTH_FRAME_PRIMARY_CAPACITY,GFP_KERNEL);
 job_secondary=kmalloc(M9_FAA_COLOR_SECONDARY_CAPACITY,GFP_KERNEL);
 scratch=kzalloc(sizeof(*scratch),GFP_KERNEL);
 code_page=kvzalloc(M9_FAA_PROGRAM_PAGE_BYTES,GFP_KERNEL);
 jobs_secondary=kvzalloc(batch->count*2712,GFP_KERNEL);
 jobs=kzalloc(batch->count*sizeof(*jobs),GFP_KERNEL);
 if(!arena||!secondary||!composed||!job_primary||!job_secondary||!scratch||!code_page||!jobs_secondary||!jobs){ret=-ENOMEM;goto release;}
 m9_faa_frame_trace(mdev,"batch-allocated",batch->count,arena_bytes,previous,0,false);
 ret=m9_faa_template_request(mdev,binding,generation,two?1:2,&color);if(ret)goto release;
 /* No command is posted by preparation. Each extra stage is generated
  * from copied live inputs and completed before the reservation phase. */
 ret=initialized?m9x_faa_graph_depth_later_prepare(mdev,binding,generation,previous,&batch->draw[0].input,batch->clear_flags,batch->draw[0].raster_flags):
  m9x_faa_graph_depth_frame_prepare(mdev,binding,generation,previous,true,&batch->draw[0].input,batch->draw[0].raster_flags);if(ret)goto release;
 mutex_lock(&t->lock);
 ret=initialized?m9_faa_depth_frame_retained_state(mdev,binding,generation,previous,&queue_start):
  m9_faa_depth_frame_first_state(t,binding,generation,previous);if(ret)goto unlock;
 r=t->faa;frame=r->depth_storage?r->depth_storage->frame:NULL;
	base = initialized ? m9_faa_frame_origin(r, previous) :
			     m9_faa_first_origin(r, previous);
 if(!frame||frame->attempted||frame->retired||frame->draw_count!=(initialized?1u:0u)||frame->batch_data||frame->batch_secondary||
  frame->first!=first||frame->initialized_reuse!=initialized||
  frame->end_count!=(two?frame->planned_ends:(initialized?(batch->clear_flags?13:4):14))||
	    frame->final_point != base + (two ? frame->planned_delta :
					  initialized && !batch->clear_flags ?
					  0xd0 : 0x150) ||
  frame->secondary_bytes!=(initialized?
   (two?((batch->draw[0].input.declaration_count==3?1352u:1376u)+
         (frame->planned_ends==4?4u:0u)):
    ((batch->clear_flags?2568u:2572u)-
     (batch->draw[0].input.declaration_count==3?24u:0u)+
     ((batch->draw[0].raster_flags&6)?24u:0u))):
   (two?(batch->draw[0].input.declaration_count==3?1352u:1376u):
        (batch->draw[0].input.declaration_count==3?2688u:2712u)))||
  arena_bytes>r->draw_owner[1].bytes){ret=-ESTALE;goto unlock;}
 m9_faa_depth_frame_owners(r,owner);
 ret=m9x_faa_graph_bound_depth_views_locked(mdev,binding,generation,&views);if(ret)goto unlock;
 if(two){
  ret=m9x_faa_depth_mode_batch_plan(first,batch->clear_flags,&mode_before,owner,&views,mode_jobs,batch->count,&mode_plan);
  if(ret)goto unlock;
  mode_current=frame->mode_after;initial_ends=frame->end_count;
  last_secondary=frame->secondary_bytes;
  build_state=mode_current;build_point=frame->final_point;
 }
 memcpy(arena,r->color_image->constants,r->color_image->constants_bytes);
 memcpy(arena+r->color_image->geometry_offset,r->color_image->constants+r->color_image->geometry_offset,r->color_image->geometry_bytes);
 if(!initialized)memcpy(jobs_secondary,frame->secondary,frame->secondary_bytes);
 for(i=0;i<batch->count;i++){
  jobs[i].program=batch->draw[i].input.program;
  jobs[i].program_bytes=batch->draw[i].input.layout.program_bytes;
  memcpy(jobs[i].entry_bytes,batch->draw[i].input.layout.entry_bytes,sizeof(jobs[i].entry_bytes));
  jobs[i].secondary=jobs_secondary+i*2712;jobs[i].secondary_bytes=two?(batch->draw[i].input.declaration_count==3?1352:1376):(batch->draw[i].input.declaration_count==3?2688:2712);
  jobs[i].declaration_count=batch->draw[i].input.declaration_count;
 }
 m9_faa_frame_trace(mdev,"batch-stages-begin",batch->count,arena_bytes,previous,0,false);
 for(i=initialized?0:1;i<batch->count;i++){
  struct m9x_faa_color_input input=batch->draw[i].input;
  struct m9x_faa_color_image live={
   .program=scratch->program,.constants=arena+slots[i],
   .primary=scratch->primary,.secondary=scratch->secondary,
   .program_bytes=sizeof(scratch->program),.constants_bytes=arena_bytes-slots[i],
   .primary_bytes=sizeof(scratch->primary),.secondary_bytes=sizeof(scratch->secondary),
   .secondary_length=&scratch->secondary_bytes};

  input.width=r->graph_layout.width;input.height=r->graph_layout.height;
  input.data_offset=slots[i];
  input.fragment_flags=batch->draw[i].raster_flags&1;
  /* FIRST2 adds 0x70 before encoding its color stage. Supply its INIT-like
   * CPU epoch so the generated secondary's direct color token is exactly
   * origin+0x90 (clear3) or origin+0x10 (no clear). A retired idle
   * secondary close can advance origin by0x10 without changing the prior
   * API Unlock receipt. Neither value is
   * accepted as a GPU completion; prior Unlock remains independently gated. */
  if(two&&i){
   bool change=build_state.enabled!=!!((input.depth_flags?input.depth_flags:15)&1u);
   /* Raw color2 base is current+10 on reset, current-40 on cached draw.
    * The stage helper alone adds70, so its CPU-only origin is below. */
   if(build_point<(change?0x60u:0xb0u)){ret=-ERANGE;goto unlock;}
  }
		stage_point = base;
		if (two && i) {
			u32 depth = input.depth_flags ? input.depth_flags : 15;

			stage_point = build_point -
				      (build_state.enabled != !!(depth & 1u) ? 0x60 : 0xb0);
		} else if (two && initialized) {
			stage_point = base - (batch->clear_flags ? 0 :
					      mode_plan.initial_reset_bytes ? 0x30 : 0x80);
		}
		ret = m9x_faa_first_depth_color_stage_encode(color->data, color->size,
							     owner, &views,
							     initialized ? r->primary_bytes :
							     r->graph_draw_end_bytes,
							     stage_point, &input,
							     job_primary,
							     M9_FAA_DEPTH_FRAME_PRIMARY_CAPACITY,
							     job_secondary,
							     M9_FAA_COLOR_SECONDARY_CAPACITY,
							     &pb, &sb, &live);
		if (ret)
			goto unlock;
  if(memcmp(scratch->program,input.program,input.layout.program_bytes)){ret=-ESTALE;goto unlock;}
  if(batch->draw[i].raster_flags){
   u32 prefix_bytes=pb-(two?3588:3632),raster_bytes;
   struct m9x_faa_raster_input ri={
    .primary=job_primary,.secondary=job_secondary,
    .primary_bytes=pb,.secondary_bytes=sb,.secondary_base=owner[29].base,
    .range_at={prefix_bytes+4,prefix_bytes+8,prefix_bytes+(two?16:52),prefix_bytes+(two?20:56)},
    .database_bytes=0x1c1d8,.no_cull_start_bytes=0x149a0,.cull_start_bytes=0x147f8,
    .flags=batch->draw[i].raster_flags};
   struct m9x_faa_raster_output ro={
    .primary=composed,.secondary=scratch->secondary,
    .primary_capacity=M9_FAA_DEPTH_FRAME_PRIMARY_CAPACITY,
    .secondary_capacity=sizeof(scratch->secondary),.secondary_bytes=&raster_bytes};
   ret=(two?m9x_faa_raster_two_encode:m9x_faa_raster_encode)(&ri,&ro);if(ret)goto unlock;
   if(raster_bytes!=sb){ret=-EINVAL;goto unlock;}
   memcpy(job_secondary,ro.secondary,sb);
  }
  if(sb!=jobs[i].secondary_bytes){ret=-EIO;goto unlock;}
  memcpy(jobs[i].secondary,job_secondary,sb);
  if(two&&i){
   bool enabled=!!((input.depth_flags?input.depth_flags:15)&1u);
   build_point+=build_state.enabled!=enabled?0xb0:0x60;
   build_state=(struct m9x_faa_depth_mode_state){true,enabled,build_state.embedded_hz||!enabled};
  }
 }
 m9_faa_frame_trace(mdev,"batch-stages-end",batch->count,arena_bytes,previous,0,false);
 /* Bind every immutable code span before composing a consuming range.
  * C2 page pointers retain owner26 identity; selectors use live slot bytes. */
 m9_faa_frame_trace(mdev,"batch-code-bind",batch->count,arena_bytes,previous,0,false);
 ret=(two?m9x_faa_program_arena_two_encode:m9x_faa_program_arena_encode)(&owner[26],jobs,batch->count,
  code_page,M9_FAA_PROGRAM_PAGE_BYTES,code_slots);if(ret)goto unlock;
 /* Slot zero is always the first immutable program. The later singleton
  * already encodes its page selectors at offset zero; its different source
  * secondary grammar must not be replaced by a fresh FIRST stage. */
 if(code_slots[0]){ret=-ESTALE;goto unlock;}
 if(!initialized)memcpy(frame->secondary,jobs[0].secondary,jobs[0].secondary_bytes);
 memcpy(secondary,frame->secondary,frame->secondary_bytes);sec_cursor=frame->secondary_bytes;
 for(i=1;i<batch->count;i++){
  struct m9x_faa_color_input input=batch->draw[i].input;
  struct m9x_faa_frame_first_append_image out={
   .primary=composed,.secondary=secondary+sec_cursor,
   .primary_capacity=M9_FAA_DEPTH_FRAME_PRIMARY_CAPACITY,.secondary_capacity=2700};
  input.width=r->graph_layout.width;input.height=r->graph_layout.height;
  input.data_offset=slots[i];input.fragment_flags=batch->draw[i].raster_flags&1;
  if(two){
   struct m9x_faa_depth_mode_append_input mi={
    .owner=owner,.views=&views,.reset_template=reset_fw?reset_fw->data:NULL,
    .reset_template_bytes=reset_fw?reset_fw->size:0,
    .old_primary=frame->primary,.fresh_secondary=jobs[i].secondary,
    .old_primary_bytes=frame->primary_bytes,.old_secondary_bytes=sec_cursor,
    .old_last_secondary_bytes=last_secondary,.old_draw_count=i,
    .old_last_declaration_count=batch->draw[i-1].input.declaration_count,
    .declaration_count=input.declaration_count,.fresh_secondary_bytes=jobs[i].secondary_bytes,
    .primary_start=frame->primary_start,.depth_flags=input.depth_flags?input.depth_flags:15,
    .secondary_start=frame->secondary_cursor,.initial_end_count=initial_ends,
    .old_ends=frame->end_bytes,.old_end_count=frame->end_count,
    .old_final_point=frame->final_point,.before=mode_current};
   struct m9x_faa_depth_mode_append_image mo={
    .primary=composed,.secondary=secondary+sec_cursor,.reset_scratch=reset_scratch,
    .primary_capacity=M9_FAA_DEPTH_FRAME_PRIMARY_CAPACITY,.secondary_capacity=secondary_needed-sec_cursor,
    .reset_capacity=M9_FAA_DEPTH_MODE_RESET_CAPACITY,
    .ends=mode_ends,.end_capacity=M9_FAA_DEPTH_MODE_MAX_ENDS};
   ret=m9x_faa_depth_mode_append_encode(&mi,&mo);if(ret)goto unlock;
   memcpy(frame->primary,mo.primary,mo.primary_bytes);frame->primary_bytes=mo.primary_bytes;
   memcpy(frame->end_bytes,mo.ends,mo.end_count*sizeof(u32));frame->end_count=mo.end_count;
   frame->final_point=mo.final_point;appended_bytes=mo.secondary_bytes;
   mode_current=mo.after;last_secondary=appended_bytes;
  }else if(initialized){
   struct m9x_faa_later_depth_append_image later={
    .primary=composed,.secondary=secondary+sec_cursor,
    .primary_capacity=M9_FAA_DEPTH_FRAME_PRIMARY_CAPACITY,.secondary_capacity=2700};
   ret=(two?m9x_faa_later_depth_two_append_encode:m9x_faa_later_depth_append_encode)(owner,&views,frame->primary_start,
    frame->secondary_cursor,m9_faa_frame_origin(r,previous),batch->clear_flags,batch->draw[i].raster_flags,i,batch->draw[i-1].input.declaration_count,frame->primary,frame->primary_bytes,
    sec_cursor,frame->end_bytes,frame->end_count,frame->final_point,
    jobs[i].secondary,jobs[i].secondary_bytes,&input,&later);if(ret)goto unlock;
   memcpy(frame->primary,later.primary,later.primary_bytes);frame->primary_bytes=later.primary_bytes;
   memcpy(frame->end_bytes,later.end_bytes,later.end_count*sizeof(u32));frame->final_point=later.final_point;appended_bytes=later.secondary_bytes;
		} else {
			u32 cursor = frame->primary_start + frame->primary_bytes;

			ret = m9x_faa_frame_first_append_encode(owner,
								r->graph_draw_end_bytes,
								base, i, cursor,
								sec_cursor,
								frame->end_bytes,
								frame->final_point,
								frame->primary,
								frame->primary_bytes,
								frame->secondary,
								jobs[0].secondary_bytes,
								jobs[i].secondary,
								jobs[i].secondary_bytes,
								&input, true, &out);
			if (ret)
				goto unlock;
   memcpy(frame->primary,out.primary,out.primary_bytes);frame->primary_bytes=out.primary_bytes;
   memcpy(frame->end_bytes,out.end_bytes,sizeof(out.end_bytes));frame->final_point=out.final_point;appended_bytes=out.secondary_bytes;
  }
  if(appended_bytes!=jobs[i].secondary_bytes+(two?(last_secondary==jobs[i].secondary_bytes?0:132):-12)){ret=-EIO;goto unlock;}
  sec_cursor+=appended_bytes;
 }
	if (sec_cursor != secondary_needed ||
	    frame->secondary_cursor > M9_FAA_BATCH_SECONDARY_BYTES ||
	    sec_cursor + 48 > M9_FAA_BATCH_SECONDARY_BYTES - frame->secondary_cursor ||
	    frame->final_point != base +
	    (two ? mode_plan.delta :
	     (initialized && !batch->clear_flags ? 0xd0 : 0x150) +
	     (batch->count - 1) * 0xa0)) {
		ret = -ERANGE;
		goto unlock;
	}
 frame->batch_program=code_page;code_page=NULL;
 frame->batch_data=arena;arena=NULL;frame->batch_data_bytes=arena_bytes;
 frame->batch_secondary=secondary;secondary=NULL;frame->secondary_bytes=sec_cursor;
 frame->draw_count=batch->count;
 if(two){frame->mode_after=mode_plan.after;frame->planned_delta=mode_plan.delta;frame->planned_ends=mode_plan.end_count;}
 ret=0;
unlock:mutex_unlock(&t->lock);
release:
 kfree(mode_jobs);kfree(mode_ends);kfree(reset_scratch);
 if(reset_fw)release_firmware(reset_fw);
 kfree(slots);
 if(color)release_firmware(color);
 kvfree(code_page);kvfree(jobs_secondary);kfree(jobs);
 kvfree(arena);kvfree(secondary);kvfree(composed);kvfree(job_primary);kfree(job_secondary);kfree(scratch);
 m9_faa_frame_trace(mdev,"batch-prepare-return",batch->count,0,previous,ret,false);return ret;
#else
 return -EOPNOTSUPP;
#endif
}

/* Source-closed later CLEAR3 publication. The ordinary frame/mode/Lock
 * journal is not advanced; a prefix-only record blocks further rendering
 * until a separate source-qualified continuation exists. */
int m9x_faa_graph_clear_prefix_binding_locked(struct m9x_device *mdev,
 u32 generation,u64 previous,struct m9_faa_graph_binding **out)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 struct m9x_transport*t=&mdev->transport;struct m9x_faa_replay*r=t->faa;
 struct m9_faa_depth_frame_storage*f;
 lockdep_assert_held(&t->lock);if(!out)return -EINVAL;*out=NULL;
 if(t->wedged)return -EIO;
 if(r&&r->depth_storage&&(f=r->depth_storage->frame)&&f->clear_prefix){
  if(!f->retired||!f->attempted||f->generation!=generation||f->previous_point!=previous||
   r->graph_generation!=generation||t->generation!=generation||!r->graph_binding)return -ESTALE;
  *out=m9x_faa_graph_binding_get(r->graph_binding);return 0;
 }
 return m9x_faa_graph_depth_unlocked_get_locked(mdev,generation,previous,out);
#else
 return -EOPNOTSUPP;
#endif
}

int m9x_faa_graph_clear_prefix_submit(struct m9x_device *mdev,
 struct m9_faa_graph_binding *binding,
 const struct drm_parhelia_m9_faa_graph_clear_prefix *request,
 struct drm_parhelia_m9_faa_graph_clear_prefix *reply)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 struct m9x_transport*t=&mdev->transport;struct m9x_faa_replay*r;
 struct m9_faa_depth_frame_storage *frame=NULL,*held;
 struct m9x_faa_later_depth_scratch *scratch=NULL;
 struct m9x_faa_later_depth_clear_image image;
 struct m9x_faa_graph_draw_owner owner[34];struct m9x_faa_depth_views views;
 struct m9_faa_owner *primary,*secondary;struct m9x_fence*fence=NULL;
 const struct firmware *sample=NULL;
 struct drm_parhelia_m9_faa_graph_clear_prefix receipt;
 u64 next=0,point=0;u32 i,copied=0,start_posts=0;bool posted=false,one_posted=false,installed=false;
 int ret;
 if(!request||!reply||!binding)return -EINVAL;
 ret=m9_faa_template_request(mdev,binding,request->generation,0,&sample);if(ret)return ret;
 frame=kvzalloc(sizeof(*frame),GFP_KERNEL);scratch=kvzalloc(sizeof(*scratch),GFP_KERNEL);
 if(!frame||!scratch){ret=-ENOMEM;goto free;}
 mutex_lock(&t->lock);r=t->faa;
 if(t->wedged){ret=-EIO;goto out;}
 if(!r||r->graph_binding!=binding||r->graph_generation!=request->generation||
  t->generation!=request->generation||!r->depth_storage){ret=-ESTALE;goto out;}
 /* Receipt-only retry never reposts, even after copy_to_user failed. */
 held=r->depth_storage->frame;
 if(held&&held->clear_prefix){
  memcpy(&receipt,held->observed,sizeof(receipt));
  primary=m9_faa_command_owner(r,false);secondary=m9_faa_command_owner(r,true);
  if(!primary||!secondary||primary->base!=receipt.primary_base||primary->bytes!=receipt.primary_capacity||
   secondary->base!=receipt.secondary_base||secondary->bytes!=receipt.secondary_capacity||
   r->unlock_point!=request->previous_unlock_point||r->graph_draw_point!=request->previous_unlock_point||
   !r->unlock_started||!r->unlock_retired||!r->color_lock_retired||
   r->color_lock_point+0x20!=request->previous_unlock_point||
   !held->attempted||!held->retired||held->generation!=request->generation||
   held->previous_point!=request->previous_unlock_point||receipt.graph_id!=request->graph_id||
   receipt.width!=request->width||receipt.height!=request->height||
   t->submitted_point!=held->final_point||t->completed_point!=held->final_point||
   t->next_fence!=lower_32_bits(held->final_point)+0x10||t->queue_count||t->async_pending||
   r->graph_draw_end_bytes!=receipt.primary_end||
   r->graph_draw_posts!=held->planned_ends+9||r->color_secondary_cursor!=receipt.secondary_cursor||
   r->depth_storage->mode.valid!=held->mode_before.valid||
   r->depth_storage->mode.enabled!=held->mode_before.enabled||
   r->depth_storage->mode.embedded_hz!=held->mode_before.embedded_hz){ret=-ESTALE;goto out;}
  *reply=receipt;ret=0;goto out;
 }
 ret=m9_faa_depth_frame_retained_state(mdev,binding,request->generation,request->previous_unlock_point,&next);if(ret)goto out;
 /* Original frontier is U-only and unrotated. Reject unsupported scopes
  * before scratch installation, fences, CPU command writes or MMIO. */
 if(!r->depth_storage->initialized||!r->depth_storage->ready||held||
  !r->color_image||r->color_image->layout.program_count!=2||
  !r->depth_storage->mode.valid||!r->depth_storage->mode.enabled||
  r->depth_storage->mode.embedded_hz||r->present_attempted||r->present_point||
  (r->command_pools&&(r->command_pools->rotated||r->command_pools->unlock_point||
   r->command_pools->maintenance_point))||
  request->width!=320||request->height!=240||request->previous_unlock_point!=m9_faa_frame_origin(r,request->previous_unlock_point)){
  ret=-EOPNOTSUPP;goto out;
 }
 ret=m9x_faa_graph_bound_depth_views_locked(mdev,binding,request->generation,&views);if(ret)goto out;
 if(views.owner[0].width!=request->width||views.owner[0].height!=request->height){ret=-ESTALE;goto out;}
 primary=m9_faa_command_owner(r,false);secondary=m9_faa_command_owner(r,true);
 ret=m9_faa_command_owner_admit(mdev,primary);if(ret)goto out;
 ret=m9_faa_command_owner_admit(mdev,secondary);if(ret)goto out;
 for(i=0;i<PARHELIA_M9_FAA_GRAPH_OWNERS;i++){
  struct drm_gem_object*bo=m9x_faa_graph_binding_owner(binding,i);dma_resv_assert_held(bo->resv);
  if(!dma_resv_test_signaled(bo->resv,DMA_RESV_USAGE_READ)){ret=-EBUSY;goto out;}
 }
 /* Prefix consumes the graph PRIMARY and graph sentinel as well as the
  * bound attachments. Preserve GPU metadata while checking live roots. */
 ret=m9x_address_check_local_mapping(mdev,r->owner[1].base,r->owner[1].bytes);if(ret)goto out;
 ret=m9x_address_check_local_mapping(mdev,r->owner[8].base,r->owner[8].bytes);if(ret)goto out;
 m9_faa_depth_frame_owners(r,owner);
 image=(struct m9x_faa_later_depth_clear_image){.primary=frame->primary,.capacity=sizeof(frame->primary)};
 ret=m9x_faa_later_depth_two_clear_encode(sample->data,sample->size,owner,&views,
  r->graph_draw_end_bytes,request->previous_unlock_point,&image,scratch);if(ret)goto out;
 if(!image.bytes||image.bytes>sizeof(frame->primary)||
  image.final_point!=request->previous_unlock_point+0x80||
  image.end_bytes[8]!=r->graph_draw_end_bytes+image.bytes){ret=-ESTALE;goto out;}
 for(i=0;i<9;i++)if(image.end_bytes[i]<r->graph_draw_end_bytes||
  image.end_bytes[i]>image.end_bytes[8]||(i&&image.end_bytes[i]<image.end_bytes[i-1])){ret=-EINVAL;goto out;}
 dma_rmb();
 if((READ_ONCE(((u32*)t->fence_cpu)[0])&~15u)!=lower_32_bits(request->previous_unlock_point)){
  ret=-ESTALE;goto out;
 }
 frame->clear_prefix=true;frame->initialized_reuse=true;frame->generation=request->generation;
 frame->previous_point=request->previous_unlock_point;frame->queue_start=next;frame->final_point=image.final_point;
 frame->primary_start=r->graph_draw_end_bytes;frame->primary_bytes=image.bytes;
 frame->secondary_cursor=r->color_secondary_cursor;frame->mode_before=r->depth_storage->mode;
 memcpy(frame->end_bytes,image.end_bytes,sizeof(image.end_bytes));frame->end_count=9;
 start_posts=r->graph_draw_posts;frame->planned_ends=start_posts;
 fence=m9x_fence_create_target_locked(mdev,lower_32_bits(frame->final_point));if(!fence){ret=-ENOMEM;goto out;}
 for(i=0;i<PARHELIA_M9_FAA_GRAPH_OWNERS;i++)dma_resv_add_fence(m9x_faa_graph_binding_owner(binding,i)->resv,&fence->base,DMA_RESV_USAGE_WRITE);
 r->depth_storage->frame=frame;installed=true;frame->attempted=true;
 /* All preflight and six reservations/fences precede the first write. */
 for(i=0;i<9;i++){
  u32 end=frame->end_bytes[i],length=end-frame->primary_start;
  if(length>copied)memcpy((u8*)primary->cpu+frame->primary_start+copied,frame->primary+copied,length-copied);
  dma_wmb();one_posted=false;
  ret=m9x_post_faa_graph_end_locked(mdev,&primary->system_node,primary->dma,
   r->graph_draw_end_bytes,end,i&&end==frame->end_bytes[i-1],
   i==8?lower_32_bits(frame->final_point):0,&point,&one_posted);
  posted|=one_posted;
  if(one_posted){r->graph_draw_end_bytes=end;r->graph_draw_posts++;r->normal_lock_view_seen=false;}
  if(ret)goto out;copied=length;
 }
 dma_rmb();
 receipt=*request;receipt.raw00=READ_ONCE(((u32*)t->fence_cpu)[0]);
 receipt.raw04=READ_ONCE(((u32*)t->fence_cpu)[1]);receipt.raw20=READ_ONCE(((u32*)t->fence_cpu)[8]);
 if(point!=frame->final_point||t->completed_point!=point||t->submitted_point!=point||
  (receipt.raw00&~15u)!=lower_32_bits(point)||t->queue_count||t->async_pending){ret=-EIO;goto out;}
 receipt.clear_point=point;receipt.state=PARHELIA_M9_FAA_CLEAR_PREFIX_RETIRED;
 receipt.primary_base=primary->base;receipt.primary_capacity=primary->bytes;
 receipt.primary_start=frame->primary_start;receipt.primary_end=r->graph_draw_end_bytes;
 receipt.secondary_base=secondary->base;receipt.secondary_capacity=secondary->bytes;
 receipt.secondary_cursor=r->color_secondary_cursor;receipt.end_count=9;
 memcpy(receipt.end_bytes,frame->end_bytes,sizeof(receipt.end_bytes));
 receipt.mode_flags=PARHELIA_M9_FAA_CLEAR_PREFIX_MODE_VALID|
  (frame->mode_before.enabled?PARHELIA_M9_FAA_CLEAR_PREFIX_MODE_ENABLED:0)|
  (frame->mode_before.embedded_hz?PARHELIA_M9_FAA_CLEAR_PREFIX_MODE_EMBEDDED:0);
 memcpy(frame->observed,&receipt,sizeof(receipt));frame->retired=true;*reply=receipt;
 dev_info(&mdev->pdev->dev,"M9 FAA CLEAR3 prefix retired: graph=%#llx generation=%u Unlock=%#llx origin=%#llx target=%#llx primary=%#x/%u range=%u..%u secondary=%#x/%u cursor=%u posts=9 raw00=%#x raw04=%#x raw20=%#x mode=%#x; no DRAW/Lock\n",
  receipt.graph_id,receipt.generation,receipt.previous_unlock_point,receipt.previous_unlock_point,receipt.clear_point,
  receipt.primary_base,receipt.primary_capacity,receipt.primary_start,receipt.primary_end,
  receipt.secondary_base,receipt.secondary_capacity,receipt.secondary_cursor,receipt.raw00,receipt.raw04,receipt.raw20,receipt.mode_flags);
 ret=0;
out:
	m9x_transport_fence_finish_locked(mdev, fence, ret, posted);
 if(installed&&!posted){r->depth_storage->frame=NULL;installed=false;}
 mutex_unlock(&t->lock);
free:if(sample)release_firmware(sample);kvfree(scratch);if(!installed)kvfree(frame);return ret;
#else
 return -EOPNOTSUPP;
#endif
}

/* draw_textures: NULL, or one texture owner set per DRAW of a dual later
 * batch; a set without GEM keeps the frame's retained pair. */
static int m9_faa_depth_frame_submit(struct m9x_device *mdev,
 struct m9_faa_graph_binding *binding,u32 generation,u64 previous,
 struct m9x_faa_texture_owners *draw_textures,u64 *point,bool *posted)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 struct m9x_transport *t=&mdev->transport;struct m9x_faa_replay *r;
 struct m9_faa_depth_frame_storage *frame;struct m9_faa_color_storage *input;
 struct m9x_faa_depth_views views;struct m9_faa_owner *primary,*program;
 struct m9x_fence *fence=NULL;u32 i,page,pte,copied=0,end;u64 stage_point=0;
 const u8 *code;u32 code_bytes;
 bool stage_posted=false,two;u64 queue_start=0;u32 metadata_bad;int ret;
 if(!point||!posted||!binding)return -EINVAL;*point=0;*posted=false;
 m9_faa_frame_trace(mdev,"submit-enter",0,0,previous,0,false);
 mutex_lock(&t->lock);
 r=t->faa;
 if(!r||!r->depth_storage||!r->depth_storage->frame){m9_faa_frame_trace(mdev,"refuse-frame-owner",generation,0,previous,-ESTALE,false);ret=-ESTALE;goto out;}
 frame=r->depth_storage->frame;
 if(r->depth_storage->initialized!=frame->initialized_reuse||
  (frame->initialized_reuse&&(frame->first||frame->clear_flags>3||(frame->clear_flags&&frame->clear_flags!=3&&!(frame->dual_later&&frame->clear_flags==2))))){m9_faa_frame_trace(mdev,"refuse-initialization",r->depth_storage->initialized,frame->clear_flags,frame->initialized_reuse,-ESTALE,false);ret=-ESTALE;goto out;}
 ret=frame->first?m9_faa_depth_frame_first_state(t,binding,generation,previous):
  (frame->clear_resume?m9_faa_clear_resume_state(mdev,binding,generation,previous,&queue_start):
   m9_faa_depth_frame_retained_state(mdev,binding,generation,previous,&queue_start));if(ret){m9_faa_frame_trace(mdev,frame->first?"refuse-first-state":"refuse-retained-state",frame->generation,r->graph_draw_end_bytes,previous,ret,false);goto out;}
	if (frame->first)
		queue_start = m9_faa_first_origin(r, previous);
 input=r->color_image;
 two=input&&input->layout.program_count==2;
 if(!input||(input->layout.program_count!=2&&input->layout.program_count!=4)||
    (two&&!frame->first&&!frame->initialized_reuse)){ret=-EOPNOTSUPP;goto out;}
 if(frame->mode_planned){
  struct m9x_faa_depth_mode_state actual;
  ret=m9x_faa_graph_depth_mode_locked(mdev,binding,generation,frame->first,&actual);
  if(ret||!frame->mode_after.valid||(!frame->mode_after.enabled&&!frame->mode_after.embedded_hz)||actual.valid!=frame->mode_before.valid||actual.enabled!=frame->mode_before.enabled||
   actual.embedded_hz!=frame->mode_before.embedded_hz){ret=-ESTALE;goto out;}
 }
 metadata_bad=(frame->attempted?1u:0)|(frame->retired?2u:0)|
  (frame->generation!=generation?4u:0)|(frame->previous_point!=previous?8u:0)|
  (frame->queue_start!=queue_start?16u:0)|(frame->draw_count>M9_FAA_FRAME_BATCH_MAX_DRAWS?32u:0)|
  (frame->final_point!=(frame->mode_planned?
    (frame->first?queue_start:m9_faa_frame_origin(r,previous))+frame->planned_delta:
   (frame->initialized_reuse?
    m9_faa_frame_origin(r,previous)+(two?(frame->clear_flags?0x110:0x90):
                                          (frame->clear_flags==3?0x150:frame->clear_flags==2?0x120:0xd0)):
    queue_start+(frame->first?(two?0x110:0x150):0x190))+
    (frame->draw_count?frame->draw_count-1:0)*(two?0x60:0xa0))?64u:0)|
  (frame->mode_planned&&frame->end_count!=frame->planned_ends?4096u:0)|
  (!frame->first&&frame->secondary_cursor!=r->color_secondary_cursor?128u:0)|
  (frame->primary_start!=r->graph_draw_end_bytes?256u:0)|(!frame->end_count?512u:0)|
  (frame->end_count>M9_FAA_DEPTH_FRAME_MAX_ENDS?1024u:0);
 if(!metadata_bad&&frame->end_bytes[frame->end_count-1]!=frame->primary_start+frame->primary_bytes)metadata_bad|=2048u;
 if(metadata_bad){
  m9_faa_frame_trace(mdev,"refuse-frame-metadata",metadata_bad,frame->draw_count,frame->final_point,-ESTALE,false);
  m9_faa_frame_trace(mdev,"refuse-frame-range",frame->primary_start,r->graph_draw_end_bytes,frame->primary_bytes,-ESTALE,false);
  ret=-ESTALE;goto out;
 }

	if (frame->dual_first && frame->stages) {
		struct m9x_faa_texture_owners *tex =
			frame->dual_next_pair ? &r->dual_next_textures :
						&r->dual_textures;
		u32 at = 0;
		if (!frame->stages || !frame->programs ||
		    !frame->batch_secondary || !frame->batch_program ||
		    !frame->draw_count ||
		    frame->draw_count > M9_FAA_DUAL_LATER_MAX_DRAWS ||
		    !tex->gem[0]) {
			ret = -ESTALE;
			goto out;
		}
		/* Both texture RESVs of every set are held by the ioctl. Re-read each
   * placement and regenerate its DRAW's sampler words at the composed
   * sites before any consuming fence or post. */
		for (i = 0; i < frame->draw_count; i++) {
			const struct m9x_faa_frame_stage *st =
				&frame->stages[i];
			struct m9x_faa_texture_owners *own =
				i && draw_textures && draw_textures[i].gem[0] ?
					&draw_textures[i] :
					tex;
			if (st->offset != at ||
			    st->bytes > frame->secondary_bytes - at) {
				ret = -ESTALE;
				goto out;
			}
			ret = m9x_faa_texture_owners_place_sites_locked(
				mdev, own, generation, frame->batch_secondary,
				frame->secondary_bytes, st->units, st->site);
			if (ret)
				goto out;
			at += st->bytes;
		}
		if (at != frame->secondary_bytes) {
			ret = -ESTALE;
			goto out;
		}
	} else if (frame->dual_first) {
  /* FIRST and COLOR-pass frames: one dual set at the source sites. */
  struct m9x_faa_texture_owners *tex=&r->dual_textures;
  struct m9x_faa_texture_dual_program dual={
   .stage_count=4,.sampler_count=2,.coordinate_sets=2,
   .color_op0=frame->dual_colorpass?3:4,
   .color_op1=frame->dual_colorpass?3:4,
   .program_bytes=input->layout.program_bytes};
  for(i=0;i<4;i++)dual.entry_bytes[i]=input->layout.entry_bytes[i];
  u32 first_len=frame->dual_colorpass?M9_FAA_DUAL_COLORPASS_SECONDARY_BYTES:
   M9_FAA_DUAL_FIRST_SECONDARY_BYTES;
  u8 *sec=frame->batch_secondary?frame->batch_secondary:frame->secondary;
  if(!frame->first||!frame->draw_count||
     frame->draw_count>M9_FAA_DUAL_BATCH_MAX_DRAWS||
     frame->secondary_bytes!=first_len+
       (frame->draw_count-1)*M9_FAA_DUAL_APPEND_SECONDARY_BYTES||
     (frame->draw_count>1&&!frame->batch_secondary)||
     (frame->dual_colorpass&&frame->draw_count!=1)||
     !tex->gem[0]||!tex->gem[1]){
   ret=-ESTALE;goto out;
  }
  /* Both texture RESVs are held by the ioctl. Re-read placement and
   * regenerate their packet words before any consuming fence or post. */
  ret=frame->dual_colorpass?
    m9x_faa_texture_owners_place_colorpass_locked(mdev,tex,
      generation,&dual,sec,first_len):
    m9x_faa_texture_owners_place_locked(mdev,tex,generation,
      &dual,sec,first_len);
  if(ret)goto out;
  for(i=1;i<frame->draw_count;i++){
   ret=m9x_faa_texture_owners_place_append_locked(mdev,tex,
    generation,&dual,sec+first_len+
     (i-1)*M9_FAA_DUAL_APPEND_SECONDARY_BYTES,
    M9_FAA_DUAL_APPEND_SECONDARY_BYTES);
   if(ret)goto out;
  }
 }

 primary=m9_faa_command_owner(r,false);program=&r->draw_owner[0];
 if(!primary){ret=-ESTALE;goto out;}
 code=frame->batch_program?frame->batch_program:input->program;
 code_bytes=frame->batch_program?M9_FAA_PROGRAM_PAGE_BYTES:input->program_bytes;
 if(!primary->cpu||!program->vram||code_bytes>sizeof(frame->observed)||code_bytes>program->bytes||
  frame->primary_start>primary->bytes||primary->bytes-frame->primary_start<frame->primary_bytes||
  !frame->secondary_bytes||frame->secondary_bytes>(frame->batch_secondary?0x40000:sizeof(frame->secondary))||
  (frame->batch_data&&(frame->batch_data_bytes>r->draw_owner[1].bytes||
   !frame->batch_secondary||(!frame->batch_program&&!frame->dual_first)||
   (frame->draw_count<2&&!frame->stages)))||
  frame->secondary_cursor>0x3fe00||frame->secondary_bytes+48>0x3fe00-frame->secondary_cursor){ret=-ERANGE;goto out;}
 for(i=0;i<PARHELIA_M9_FAA_GRAPH_OWNERS;i++){
  struct drm_gem_object *bo=m9x_faa_graph_binding_owner(binding,i);dma_resv_assert_held(bo->resv);
  if(!dma_resv_test_signaled(bo->resv,DMA_RESV_USAGE_READ)){ret=-EBUSY;goto out;}}
 ret=m9x_faa_graph_bound_depth_views_locked(mdev,binding,generation,&views);if(ret){m9_faa_frame_trace(mdev,"refuse-depth-views",generation,0,previous,ret,false);goto out;}
 /* Disabled depth consumes the retained graph companion/HZ mappings.
  * Preserve GPU metadata, but authenticate their address identities before
  * any consuming fence or upload just like the bound attachment owners. */
 if(frame->mode_planned&&frame->mode_after.embedded_hz){
  for(i=4;i<=5;i++){
   ret=m9x_address_check_local_mapping(mdev,r->owner[i].base,r->owner[i].bytes);
   if(ret){m9_faa_frame_trace(mdev,"refuse-embedded-depth-identity",i,r->owner[i].base,r->owner[i].bytes,ret,false);goto out;}
  }
 }

 for(i=0;i<3;i++){
  struct m9_faa_owner *o=i==2?m9_faa_command_owner(r,true):i?&r->draw_owner[1]:primary;
  if(!o||!o->cpu||!drm_mm_node_allocated(&o->system_node)||o->system_node.start!=o->base){m9_faa_frame_trace(mdev,"refuse-owner-node",i,o?o->base:0,o?o->system_node.start:0,-ESTALE,false);ret=-ESTALE;goto out;}
		ret = m9x_system_pages_mapped(mdev, o->base, o->dma,
					      o->bytes / PAGE_SIZE, 1u, &page, &pte);
		if (ret == -ESTALE)
			m9_faa_frame_trace(mdev, "refuse-owner-pte", i, page,
					   ((u64)pte << 32) |
					   (m9x_address_pte_word(o->dma + page * PAGE_SIZE) | 1),
					   ret, false);
		else if (ret)
			m9_faa_frame_trace(mdev, "refuse-owner-pte-read", i, page,
					   o->base, ret, false);
		if (ret)
			goto out;
	}
 /* DLL 0x40004000 transition / 0x40008000 query words are controls,
  * as also classified by m9_submit. They carry no SYSTEM owner identity.
  * Every actual primary/data/secondary PTE and local code view above is
  * still authenticated before any consuming fence or command write. */
 ret=m9x_address_check_local_identity(mdev,program->base,program->bytes);if(ret){m9_faa_frame_trace(mdev,"refuse-program-identity",program->base,program->bytes,previous,ret,false);goto out;}
 /* PREPARE does not initialize sampled color through a CPU map. Capture the
  * actual borrowed tile after retired INIT and before the first dual GPU
  * write, under the same six-owner reservation set as the later read. This
  * distinguishes an existing origin word from a DRAW-produced one. */
 if(frame->dual_first&&frame->first&&m9_faa_color_observe)
  m9_faa_dual_graph_raw_observe(mdev,r,1,"sampled-pre",m9_faa_owner_specs[1].bytes);
 fence=m9x_fence_create_target_locked(mdev,lower_32_bits(frame->final_point));if(!fence){ret=-ENOMEM;goto out;}
 for(i=0;i<PARHELIA_M9_FAA_GRAPH_OWNERS;i++)dma_resv_add_fence(m9x_faa_graph_binding_owner(binding,i)->resv,&fence->base,DMA_RESV_USAGE_WRITE);
 if(frame->dual_first){
  struct m9x_faa_texture_owners *tex=frame->dual_next_pair?
   &r->dual_next_textures:&r->dual_textures;
  ret=m9x_faa_texture_owners_fence_locked(tex,&fence->base);
  if(ret)goto out;
  for(i=1;draw_textures&&frame->stages&&i<frame->draw_count;i++){
   if(!draw_textures[i].gem[0])continue;
   ret=m9x_faa_texture_owners_fence_locked(&draw_textures[i],&fence->base);
   if(ret)goto out;
  }
 }
 frame->attempted=true;r->color_attempted=true;
 if(!frame->first)r->color_next_attempt_point=previous;r->color_lock_retired=false;
 r->normal_lock_view_seen=false;r->unlock_started=false;r->unlock_retired=false;
 r->color_primary_start=frame->primary_start;r->color_posts_start=r->graph_draw_posts;
 /* All six consuming WRITE fences precede code/data and every clear post. */
 m9_faa_frame_trace(mdev,"code-prepare-begin",frame->draw_count,code_bytes,frame->final_point,0,true);
 m9x_write(mdev,M9_REG_FRONTEND_KICK,1);
 ret=m9x_address_cpu_local_prepare(mdev,program->base,program->bytes);if(ret)goto out;
 m9_faa_frame_trace(mdev,"code-upload-begin",0,code_bytes,frame->final_point,0,true);
 iosys_map_memcpy_to(&program->map,0,code,code_bytes);wmb();
 m9_faa_frame_trace(mdev,"code-upload-end",0,code_bytes,frame->final_point,0,false);
 m9x_write(mdev,M9_REG_FRONTEND_STATE,0);m9x_read(mdev,M9_REG_FRONTEND_STATE);
 m9_faa_frame_trace(mdev,"code-readback-begin",0,code_bytes,frame->final_point,0,true);
 iosys_map_memcpy_from(frame->observed,&program->map,0,code_bytes);
 if(memcmp(frame->observed,code,code_bytes)){ret=-EIO;goto out;}
 m9_faa_frame_trace(mdev,"code-readback-end",0,code_bytes,frame->final_point,0,false);
 if(frame->batch_data)memcpy(r->draw_owner[1].cpu,frame->batch_data,frame->batch_data_bytes);
 else{
  memcpy(r->draw_owner[1].cpu,input->constants,input->constants_bytes);
  memcpy((u8*)r->draw_owner[1].cpu+input->geometry_offset,input->constants+input->geometry_offset,input->geometry_bytes);
 }
 memcpy((u8*)m9_faa_command_owner(r,true)->cpu+frame->secondary_cursor,frame->batch_secondary?frame->batch_secondary:frame->secondary,frame->secondary_bytes);
 m9_faa_frame_trace(mdev,"data-published",frame->draw_count,frame->secondary_bytes,frame->final_point,0,false);
 for(i=0;i<frame->end_count;i++){
  u32 length=frame->end_bytes[i]-frame->primary_start;
  end=frame->end_bytes[i];
  if(length>frame->primary_bytes||length<copied){ret=-EINVAL;goto out;}
  if(length>copied)memcpy((u8*)primary->cpu+frame->primary_start+copied,frame->primary+copied,length-copied);
  dma_wmb();m9_faa_frame_trace(mdev,"END-begin",i,end,
   i==frame->end_count-1?frame->final_point:0,0,true);
  ret=m9x_post_faa_graph_end_locked(mdev,&primary->system_node,primary->dma,
   r->graph_draw_end_bytes,end,i&&(end==frame->end_bytes[i-1]),
   i==frame->end_count-1?lower_32_bits(frame->final_point):0,&stage_point,&stage_posted);
  m9_faa_frame_trace(mdev,"END-end",i,end,stage_point,ret,false);
  *posted|=stage_posted;if(stage_posted){r->graph_draw_end_bytes=end;r->graph_draw_posts++;}
  if(ret){
   dev_info(&mdev->pdev->dev,"M9 FAA frame END refused: index=%u end=%u final=%#llx error=%d any_posted=%u stage_posted=%u\n",
    i,end,frame->final_point,ret,*posted,stage_posted);
   goto out;
  }copied=length;
 }
 if(stage_point!=frame->final_point){ret=-EIO;goto out;}
 *point=stage_point;frame->retired=true;r->depth_storage->initialized=true;
	if (frame->dual_later)
		r->dual_active_second = frame->dual_next_pair;
	if (frame->programs) {
		kfree(r->programs);
		r->programs = frame->programs;
		frame->programs = NULL;
	}
 r->depth_storage->mode=frame->mode_planned?frame->mode_after:
  (struct m9x_faa_depth_mode_state){true,true,false};
 r->graph_draw_point=stage_point;r->color_lock_point=stage_point;r->color_lock_retired=true;
 input->primary_length=frame->primary_bytes;input->lock_delta=frame->final_point-previous;input->lock_posts=frame->end_count;
 r->color_secondary_cursor=frame->secondary_cursor+frame->secondary_bytes;
 m9_faa_color_intermediates_observe(mdev,r);
 dev_info(&mdev->pdev->dev,"M9 FAA bound depth FRAME retired: previous=%#llx queue_start=%#llx lock=%#llx bytes=%u posts=%u attachments=31/32/33\n",
  previous,queue_start,*point,r->graph_draw_end_bytes,r->graph_draw_posts);
 if(frame->draw_count>1)
  dev_info(&mdev->pdev->dev,"M9 FAA FIRST batch retired: draws=%u lock=%#llx primary_bytes=%u secondary_bytes=%u input_bytes=%u\n",
   frame->draw_count,*point,frame->primary_bytes,frame->secondary_bytes,frame->batch_data_bytes);
out:
	if (ret)
		*point = 0;
	m9x_transport_fence_finish_locked(mdev, fence, ret, *posted);
 mutex_unlock(&t->lock);
 m9_faa_frame_trace(mdev,"submit-return",0,0,*point,ret,false);return ret;
#else
 return -EOPNOTSUPP;
#endif
}

int m9x_faa_graph_depth_frame_submit(struct m9x_device *mdev,
				     struct m9_faa_graph_binding *binding,
				     u32 generation, u64 previous, u64 *point,
				     bool *posted)
{
	return m9_faa_depth_frame_submit(mdev, binding, generation, previous,
					 NULL, point, posted);
}

static_assert(M9_FAA_DUAL_LATER_MAX_DRAWS == PARHELIA_M9_FAA_DUAL_LATER_MAX_DRAWS);

/* Keep a per-DRAW owner set past its ioctl until DMA-stop teardown. */
void m9x_faa_graph_texture_owners_defer(struct m9x_device *mdev,
					struct m9x_faa_texture_owners *owners)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_texture_owners *grown;
	struct m9x_faa_replay *r;

	mutex_lock(&t->lock);
	r = t->faa;
	if (r) {
		grown = kvrealloc(r->deferred_textures,
				  (r->deferred_count + 1) * sizeof(*grown),
				  GFP_KERNEL);
		if (grown) {
			r->deferred_textures = grown;
			grown[r->deferred_count++] = *owners;
			memset(owners, 0, sizeof(*owners));
		}
	}
	mutex_unlock(&t->lock);
	if (owners->gem[0])
		dev_warn(&mdev->pdev->dev,
			 "M9 FAA: retaining an unreleasable texture set\n");
#endif
}

int m9x_faa_graph_dual_later_submit(struct m9x_device *mdev,
				    struct m9_faa_graph_binding *binding,
				    u32 generation, u64 previous,
				    struct m9x_faa_texture_owners *draw_textures,
				    u64 *point, bool *posted)
{
	return m9_faa_depth_frame_submit(mdev, binding, generation, previous,
					 draw_textures, point, posted);
}

#ifdef M9_FAA_GRAPH_INIT_CONTROL
MODULE_FIRMWARE(M9_FAA_SAMPLE_CLEAR_FIRMWARE);
MODULE_FIRMWARE(M9_FAA_DEPTH_DRAW_FIRMWARE);
MODULE_FIRMWARE(M9_FAA_DEPTH_MODE_RESET_FIRMWARE);
MODULE_FIRMWARE("m9x/faa-graph-dual-first-template.bin");
MODULE_FIRMWARE(M9_FAA_SINGLE_FIRST_FIRMWARE);
#endif

/* Fresh and retained COLOR have distinct complete source plans and queue
 * epochs, both under the diagnostic launcher opt-in until hardware review. */
bool m9x_faa_graph_depth_frame_profile_qualified(bool first)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 /* FIRST owns exact INIT; retained owns exact Lock/view/Unlock. The native
  * producers independently validate the fresh or retained state. */
 (void)first;
 return m9_faa_color_four_experimental;
#else
 return false;
#endif
}
