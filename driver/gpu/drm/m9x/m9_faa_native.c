// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Private replay of the coherent surface-owned M9128 vendor FAA frame.
 * Command grammar and source validity come from the normal CPU capture.
 * This diagnostic exposes neither arbitrary indirect execution nor app MSAA.
 * Opaque stores, HZ, pool and allocation tails receive no fabricated memset.
 */
#include <linux/dma-mapping.h>
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
#include "m9_reg.h"
#include "m9_faa_output.h"
#include "m9_faa_graph_draw_encode.h"
#include "m9_faa_graph_color_encode.h"
#include "m9_faa_graph_color_next_encode.h"
#include "m9_faa_graph_depth.h"
#include "m9_faa_graph_public_vs_encode.h"
#include "m9_faa_graph_public_vs_follow_encode.h"
#include "m9_faa_graph_public_vs_draw_encode.h"
#include "m9_faa_graph_public_vs_lock_encode.h"
#include "m9_faa_graph_public_vs_next_encode.h"
#include "m9_faa_graph_unlock_encode.h"
#include "m9_faa_graph_next_encode.h"
#include "m9_faa_graph_next_finish_encode.h"
#include "m9_faa_graph_draw_tail_encode.h"
#include "m9_faa_graph_lock_encode.h"

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
#ifdef M9_FAA_GRAPH_INIT_CONTROL
struct m9_faa_depth_storage {
 struct m9_faa_owner owner[M9_FAA_DEPTH_OWNERS];
 struct m9x_faa_depth_views views;
 u32 generation;
 u64 previous_unlock_point;
 bool ready;
};
static bool m9_faa_color_four_experimental;
module_param_named(faa_color_four_experimental,m9_faa_color_four_experimental,bool,0600);
MODULE_PARM_DESC(faa_color_four_experimental,"Enable unqualified four-stage COLOR launcher (diagnostic only)");
static bool m9_faa_color_observe;
module_param_named(faa_color_observe, m9_faa_color_observe, bool, 0600);
MODULE_PARM_DESC(faa_color_observe, "Read at most four retained COLOR intermediate records after final retirement");
struct m9_faa_color_storage {
 u8 program[4096],primary[3696],secondary[2880];
 u8 clear[128];
 u32 clear_bytes,secondary_cursor;
 u64 previous_unlock_point;
 u8 *constants;u32 constants_bytes,geometry_bytes,geometry_offset,program_bytes,secondary_bytes,primary_length,draw_length,lock_delta;
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
 u8 primary[60],secondary[2548],lock_primary[M9_FAA_VS_LOCK_PRIMARY_BYTES];
 u32 vertices[24];
 u64 unlock_point;
 u32 primary_start,draw_end,lock_end;
};
struct m9_faa_public_vs_lock_storage {u8 primary[M9_FAA_VS_LOCK_PRIMARY_BYTES];};
struct m9_faa_public_vs_draw_storage {
 u8 primary[M9_FAA_VS_DRAW_PRIMARY_BYTES], secondary[M9_FAA_VS_DRAW_SECONDARY_BYTES];
 struct m9x_faa_public_vs_draw_endpoints ends;
};
struct m9_faa_public_vs_follow_storage {
 u8 primary[48],secondary[616];
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
	struct m9_faa_public_vs_storage *public_vs_image;
 struct m9_faa_color_storage *color_image;
 struct m9_faa_depth_storage *depth_storage;
 bool color_payload_ready,color_attempted,color_lock_retired;
 u64 color_lock_point;
 u32 color_primary_start,color_posts_start;
 u32 color_secondary_cursor;
 u64 color_next_attempt_point;
	bool public_vs_payload_ready, public_vs_prefix_retired;
	bool public_vs_head_attempted, public_vs_head_retired;
	struct m9x_fence *public_vs_head_fence;
 struct m9_faa_public_vs_draw_storage *public_vs_draw_image;
 struct m9_faa_public_vs_lock_storage *public_vs_lock_image;
 bool public_vs_lock_attempted,public_vs_lock_retired;
 struct m9_faa_public_vs_next_storage *public_vs_next_image;
 bool public_vs_next_attempted,public_vs_next_lock_retired,public_vs_next_view_seen;
 struct m9x_fence *public_vs_draw_fence;
 bool public_vs_draw_attempted,public_vs_draw_observed;
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
	bool graph_next_lock_claimed, graph_second_lock_retired, second_lock_view_seen;
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
	u32 graph_generation;
	bool graph_preparing;
	/* The first graph draw is a separate caller on the retained INIT
	 * context. A claim closes INIT-token reuse before any source upload;
	 * every posted END advances this cursor even if its later wait fails. */
	bool graph_draw_claimed;
	u32 graph_draw_posts, graph_draw_end_bytes;
	u64 graph_draw_point;
	/* Prepared program bytes remain CPU inputs until the original c0
	 * access boundary. Keep their source buffer through that upload. */
	const struct firmware *program_inputs;
	bool prepared, attempted, baseline_valid;
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

	if (t->wedged)
		return -EIO;
	if (!r || !r->graph_binding || !r->prepared || !r->attempted ||
	    !r->observation_retired || !r->observation_point ||
	    r->graph_preparing || r->graph_draw_claimed ||
	    r->graph_generation != generation ||
	    r->observation_generation != generation ||
	    t->generation != generation ||
	    r->observation_point != init_point ||
	    t->submitted_point != init_point ||
	    t->completed_point != init_point ||
	    lower_32_bits(init_point) > U32_MAX - 0x10 ||
	    t->next_fence != lower_32_bits(init_point) + 0x10)
		return -ESTALE;
	if (!t->faa_source_context || !t->faa_virtual_primary ||
	    !t->transport_armed || !t->bus_master_enabled ||
	    !t->engine_transitioned || !t->restore_ready || !t->fence_cpu ||
	    t->async_stop || t->async_pending || t->queue_count)
		return -EBUSY;
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
	r->graph_draw_end_bytes = r->primary_bytes;
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

/* The unchanged native origin10 is an initial CPU counter seed. Fresh
 * source admission must never call it a previous GPU retirement. */
int m9x_faa_source_init_admit_locked(struct m9x_device *mdev,
				    bool context_initialized)
{
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r = t->faa;

	lockdep_assert_held(&t->lock);
	if (!m9x_faa_source_init_enabled(mdev) || !r || !r->prepared ||
	    !t->fence_cpu || t->wedged || context_initialized)
		return -EINVAL;
	/* Repeated CPU-view preparation may reuse the admitted manager. It
	 * cannot start a second frame: the caller's attempted gate remains. */
	if (t->faa_source_context && r->attempted)
		return t->engine_transitioned && t->restore_ready &&
		       t->transport_armed && t->bus_master_enabled &&
		       !t->queue_count && !t->async_pending ? 0 : -EINVAL;
	if (r->attempted || t->faa_virtual_primary || t->submitted_point ||
	    t->completed_point || t->queue_count || t->async_pending ||
	    t->command_slot || t->command_offset || t->fence_epoch ||
	    t->last_fence || t->next_fence != 0x20 ||
	    READ_ONCE(((u32 *)t->fence_cpu)[0]) != 0x10 ||
	    READ_ONCE(((u32 *)t->fence_cpu)[1]) != 0x10)
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
	if (r->attempted && mdev->transport.bus_master_enabled)
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
  kfree(r->depth_storage);
 }
	kfree(r->draw_image);
	kfree(r->public_vs_image);
 if (r->color_image) kfree(r->color_image->constants);
 kfree(r->color_image);
	kfree(r->public_vs_follow_image);
 kfree(r->public_vs_draw_image);
 kfree(r->public_vs_lock_image);
 kfree(r->public_vs_next_image);
	kfree(r->next_image);
	kfree(r->next_finish_image);
	kfree(r->next_lock_image);
#endif
	if (r->retained_color) {
		m9x_vram_unpin(r->retained_color);
		drm_gem_vram_put(r->retained_color);
	}
	if (r->output_color)
		drm_gem_object_put(r->output_color);
	if (r->program_inputs)
		release_firmware(r->program_inputs);
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

/* Called by requester teardown only after PCI bus mastering has stopped. */
void m9x_faa_fini(struct m9x_device *mdev)
{
	if (mdev->transport.faa) {
		/* Remove address nodes under their lock, but drop the caller's
		 * GEM reference outside it: its last release takes this lock. */
		struct m9x_faa_replay *r = mdev->transport.faa;
		u32 i;

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
  for(i=0;i<M9_FAA_DEPTH_OWNERS;i++)
   if(drm_mm_node_allocated(&r->depth_storage->owner[i].system_node))
    m9x_address_unmap_system(mdev,&r->depth_storage->owner[i].system_node);
#endif
		mutex_unlock(&mdev->transport.lock);
		if (m9_faa_release(mdev, r))
			mdev->transport.faa = NULL;
	}
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
	u32 i;
	int ret;

	if (!binding || !input)
		return -EINVAL;
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
			r->primary_bytes, init_point, input, &image);
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
	ret = m9x_faa_graph_draw_claim_locked(mdev, binding, generation,
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
	for (i = 0; i < ARRAY_SIZE(r->owner); i++) {
		u32 role, address, source;

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
				r->owner[i].bytes = PAGE_ALIGN(m9_faa_owner_specs[i].bytes);
				break;
			}
			if (role != PARHELIA_M9_FAA_GRAPH_OWNERS)
				continue;
		}
		/* Only a profile with a sampled caller color substitutes that owner.
		 * An absent sentinel allocates every genuine source owner. */
		if (i == M9_FAA_COLOR_OWNER)
			continue;
		if (binding && !m9_faa_owner_specs[i].local)
			mutex_lock(&mdev->transport.lock);
		ret = m9_faa_allocate(mdev, &r->owner[i], &m9_faa_owner_specs[i],
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
		    mdev->transport.submitted_point ||
		    mdev->transport.queue_count) {
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

#if !defined(M9_FAA_HAS_COMPANION_CLEAR) || M9_FAA_HAS_COMPANION_CLEAR
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
#if defined(M9_FAA_HAS_COMPANION_CLEAR) && !M9_FAA_HAS_COMPANION_CLEAR
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
			    r->owner[source].bytes <
				PAGE_ALIGN(m9_faa_owner_specs[source].bytes)) {
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
#if defined(M9_FAA_MULTI_CLEAR)
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

	return m9_faa_submit(mdev, 0, NULL, point, posted, &retained,
			     binding);
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
	u32 target, page, pte, i;
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

		for (page = 0; page < o->bytes / PAGE_SIZE; page++) {
			ret = m9x_address_read_pte(mdev, o->base + page * PAGE_SIZE,
						  &pte);
			if (ret)
				goto out;
			if (pte != (m9x_address_pte_word(o->dma +
						page * PAGE_SIZE) | 1u)) {
				ret = -ESTALE;
				goto out;
			}
		}
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
	if (fence && ret)
		dma_fence_set_error(&fence->base, ret);
	if (ret && *posted) {
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
	u32 page, pte, i, target;
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

		for (page = 0; page < o->bytes / PAGE_SIZE; page++) {
			ret = m9x_address_read_pte(mdev, o->base + page * PAGE_SIZE, &pte);
			if (ret)
				goto out;
			if (pte != (m9x_address_pte_word(o->dma + page * PAGE_SIZE) | 1u)) {
				ret = -ESTALE;
				goto out;
			}
		}
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
			struct drm_gem_vram_object *color, u32 base, u64 bytes)
{
	struct m9x_transport *t = &mdev->transport;

#ifdef M9_FAA_GRAPH_INIT_CONTROL
	struct m9x_faa_replay *r = t->faa;
	u32 bound, source;
	s64 offset;

	lockdep_assert_held(&t->lock);
	/* Actual normal Lock returned after its 170 receipt: staging and
	 * primary share owner1, with F021/320x256 CPU view and pitch1280.
	 * Only this retained primary and this exact same-graph point qualify. */
	if (!color || bytes != 0x50000 || m9x_local_gpu_base)
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
	    !t->bus_master_enabled || !t->faa_source_context || !t->faa_virtual_primary)
		return -ENODATA;
	/* The second normal Lock uses the exact same source mapper/format but
	 * has its own actual retirement marker and full retained cursor join.
	 * Unlock/preprocessing/rendering receipts alone never admit F021. */
 if (r->color_lock_retired) {
  if(!r->color_image||!r->color_attempted||r->unlock_started||
   r->graph_draw_point!=r->color_lock_point||!r->color_lock_point||
   r->graph_draw_posts!=r->color_posts_start+4||
   r->graph_draw_end_bytes!=r->color_primary_start+r->color_image->primary_length)return -ENODATA;
 } else if (r->public_vs_next_lock_retired) {
		const struct m9_faa_public_vs_next_storage *n = r->public_vs_next_image;

		if (!n || !r->public_vs_next_attempted || r->public_vs_head_fence ||
		    r->public_vs_draw_fence || r->graph_draw_point != n->unlock_point + 0xd0 ||
		    r->unlock_point != n->unlock_point || r->graph_draw_posts != 13 ||
		    r->graph_draw_end_bytes != n->lock_end ||
		    n->lock_end != n->primary_start + 60 + M9_FAA_VS_LOCK_PRIMARY_BYTES)
			return -ENODATA;
	} else if (r->public_vs_lock_retired) {
		if (!r->public_vs_head_retired || !r->public_vs_draw_observed ||
		    !r->public_vs_lock_attempted || r->unlock_started ||
		    r->public_vs_head_fence || r->public_vs_draw_fence ||
		    r->graph_draw_point != r->observation_point + 0xe0 ||
		    r->graph_draw_posts != 6 ||
		    r->graph_draw_end_bytes != r->primary_bytes + 124 +
			M9_FAA_VS_LOCK_PRIMARY_BYTES)
			return -ENODATA;
	} else if (r->unlock_started) {
		if (!r->unlock_retired || !r->normal_lock_view_seen ||
		    !r->graph_second_lock_retired ||
		    r->graph_draw_point != r->observation_point + 0x190 ||
		    r->graph_draw_posts != 17 ||
		    r->graph_draw_end_bytes != r->primary_bytes +
			M9_FAA_DRAW_PRIMARY_SUFFIX_BYTES + M9_FAA_DRAW_TAIL_PRIMARY_BYTES +
			2 * M9_FAA_LOCK_PRIMARY_BYTES + M9_FAA_UNLOCK_PRIMARY_BYTES +
			M9_FAA_NEXT_PRIMARY_BYTES + M9_FAA_NEXT_FINISH_PRIMARY_BYTES)
			return -ENODATA;
	} else if (r->graph_second_lock_retired ||
		   r->graph_draw_point != r->observation_point + 0xc0 ||
		   r->graph_draw_posts != 8 ||
		   r->graph_draw_end_bytes != r->primary_bytes +
			M9_FAA_DRAW_PRIMARY_SUFFIX_BYTES + M9_FAA_DRAW_TAIL_PRIMARY_BYTES +
			M9_FAA_LOCK_PRIMARY_BYTES) {
		return -ENODATA;
	}

	if (m9x_faa_graph_binding_owner(r->graph_binding, PARHELIA_M9_FAA_PRIMARY) !=
	    &color->bo.base ||
	    !m9x_faa_graph_binding_address(r->graph_binding, PARHELIA_M9_FAA_PRIMARY,
					 &bound, &source) || source != 1 ||
	    bound != base || r->owner[source].base != base ||
	    r->owner[source].bytes != bytes || color->bo.base.size != bytes)
		return -ESTALE;
	offset = drm_gem_vram_offset(color);
	if (offset < 0 || (u64)offset != base || !IS_ALIGNED(base, PAGE_SIZE) ||
	    bytes > (u64)U32_MAX + 1 - base)
		return -ESTALE;
	return 0;
#elif defined(M9_FAA_PREPARED_FLOW_CONTROL) && defined(M9_FAA_NORMAL_LOCK_CPU_VIEW)
	struct m9x_faa_replay *r = t->faa;
	const struct m9_faa_owner_spec *spec = &m9_faa_owner_specs[M9_FAA_COLOR_OWNER];
	s64 offset;
	u64 physical;

	lockdep_assert_held(&t->lock);
	/* This marker follows a complete same-run normal-Lock mapper/owner
	 * join. Earlier prepared prefixes are not that contract. */
	if (
#ifdef M9_FAA_UNLOCK_PUBLICATION_CONTROL
	    M9_FAA_NORMAL_LOCK_SPLIT_TARGET != 0x180 ||
#else
	    M9_FAA_TRACKER_LAST != 0x180 ||
#endif
	    M9_FAA_NORMAL_LOCK_CPU_VIEW != 1 ||
	    !m9_faa_diag || m9x_local_gpu_base || !color || bytes != 0x50000 ||
	    !spec->local || spec->system || spec->bytes != bytes)
		return -EOPNOTSUPP;
	if (t->async_stop)
		return -ENODEV;
	if (t->wedged)
		return -EIO;
	if (!r || !r->attempted || !r->observation_retired ||
	    !r->observation_point || r->observation_generation != t->generation ||
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
			r->owner[1].base, r->owner[1].bytes);
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
			r->owner[1].base, r->owner[1].bytes);
	if (!ret)
		*out = m9x_faa_graph_binding_get(r->graph_binding);
	return ret;
#else
	return -EOPNOTSUPP;
#endif
}

int m9x_faa_graph_unlock_submit(struct m9x_device *mdev,
		struct m9_faa_graph_binding *binding, u32 generation,
		u64 lock_point, u64 *point, bool *posted)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
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
	mutex_lock(&t->lock);
	ret = m9x_faa_graph_unlock_get_locked(mdev, generation, lock_point, &checked);
	if (ret)
		goto out;
	if (checked != binding) {
		ret = -ESTALE;
		goto out;
	}
	r = t->faa;
	primary = &r->owner[M9_FAA_PRIMARY_OWNER];
	old_bytes = r->graph_draw_end_bytes;
	if (!primary->cpu || !drm_mm_node_allocated(&primary->system_node) ||
	    primary->system_node.start != primary->base || old_bytes > primary->bytes ||
	    primary->bytes - old_bytes < sizeof(image)) {
		ret = -ESTALE;
		goto out;
	}
	ret = m9x_faa_graph_unlock_encode(r->owner[1].base, r->owner[1].bytes,
			320, 240, lock_point, image, sizeof(image));
	if (ret)
		goto out;
	target = lower_32_bits(lock_point) + 0x20;
	/* Cover all borrowed surfaces before any release MMIO or END. A late
	 * failure keeps both the fence and retained binding quarantined. */
	fence = m9x_fence_create_target_locked(mdev, target);
	if (!fence) {
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < PARHELIA_M9_FAA_GRAPH_OWNERS; i++)
		dma_resv_add_fence(m9x_faa_graph_binding_owner(binding, i)->resv,
				   &fence->base, DMA_RESV_USAGE_WRITE);
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
			memcpy((u8 *)primary->cpu + old_bytes + M9_FAA_UNLOCK_FIRST_POST_BYTES,
			       image + M9_FAA_UNLOCK_FIRST_POST_BYTES,
			       M9_FAA_UNLOCK_PRIMARY_BYTES - M9_FAA_UNLOCK_FIRST_POST_BYTES);
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
	if (stage_point != lock_point + 0x20) {
		ret = -EIO;
		goto out;
	}
	*point = stage_point;
	r->graph_draw_point = stage_point;
	r->unlock_retired = true;
	r->unlock_point = stage_point;
	r->graph_unlock_end_bytes = r->graph_draw_end_bytes;
	r->graph_unlock_posts = r->graph_draw_posts;
	dev_info(&mdev->pdev->dev,
		 "M9 FAA graph Unlock retired: lock=%#llx unlock=%#llx bytes=%u posts=%u copyback=F021->F022\n",
		 lock_point, *point, r->graph_draw_end_bytes, r->graph_draw_posts);
out:
	if (fence && ret)
		dma_fence_set_error(&fence->base, ret);
	if (ret && *posted) {
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
	m9x_faa_graph_binding_put(checked);
	mutex_unlock(&t->lock);
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
/* These are the actual emitted B0/B2/B8 target pairs. Resolve their tagged
 * local addresses back to the retained owner; never publish a new CPU view.
 * The declaration splice shifts every stage after the carry declaration. */
static void m9_faa_color_intermediates_observe(struct m9x_device *mdev,
					    struct m9x_faa_replay *r)
{
	static const struct { u32 at, owner, bytes; } target[] = {
		{ 116, 24, 0 }, { 132, 24, 0 },
		{ 804, 23, 64 }, { 820, 23, 64 },
		{ 1416, 23, 64 }, { 1432, 23, 64 }, { 1448, 23, 16 },
	};
	struct m9_faa_color_storage *n = r->color_image;
	u8 data[256];
	u32 i, at, address, descriptor, offset, bytes, stride, chunk, count;
	int extra;

	lockdep_assert_held(&mdev->transport.lock);
	if (!m9_faa_color_observe || !r->color_lock_retired || !n ||
	    n->layout.program_count != 4 || n->secondary_bytes < 2688 ||
	    n->secondary_bytes > sizeof(n->secondary) ||
	    mdev->transport.wedged || mdev->transport.queue_count ||
	    mdev->transport.completed_point != r->color_lock_point ||
	    mdev->transport.generation != r->graph_generation)
		return;
	extra = (int)n->secondary_bytes - 2712;
	rmb();
	dma_rmb();
	for (i = 0; i < ARRAY_SIZE(target); i++) {
		struct m9_faa_owner *o = &r->owner[target[i].owner];

		at = target[i].at < 576 ? target[i].at : target[i].at + extra;
		if (at > n->secondary_bytes - 8)
			continue;
		address = get_unaligned_le32(n->secondary + at);
		descriptor = get_unaligned_le32(n->secondary + at + 4);
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
	}
}
#endif

int m9x_faa_graph_color_submit(struct m9x_device *mdev,
 struct m9_faa_graph_binding *binding,u32 generation,u64 init_point,u64 *point,bool *posted)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 struct m9x_transport *t=&mdev->transport;struct m9x_faa_replay *r;
 struct m9_faa_color_storage *n;struct m9_faa_owner *primary,*program;
 struct m9x_fence *fence=NULL;u8 observed[4096];
 bool stage_posted=false;u64 stage_point=0;u32 i,page,pte,end;int ret=-ESTALE;
 if(!point||!posted||!binding)return -EINVAL;*point=0;*posted=false;
 mutex_lock(&t->lock);r=t->faa;
 if(!r||r->graph_binding!=binding||!r->color_payload_ready||!r->color_image||r->color_attempted||
  r->graph_generation!=generation||t->generation!=generation||r->observation_generation!=generation||
  !r->observation_retired||r->observation_point!=init_point||!r->graph_draw_claimed||
  r->graph_draw_posts||r->graph_draw_point||r->graph_draw_end_bytes!=r->primary_bytes||
  !t->faa_source_context||!t->faa_virtual_primary||!t->engine_transitioned||!t->restore_ready||
  !t->transport_armed||!t->bus_master_enabled||!t->fence_cpu||t->async_stop||t->async_pending||t->queue_count||
  t->wedged||t->submitted_point!=init_point||t->completed_point!=init_point||
  t->next_fence!=lower_32_bits(init_point)+0x10)goto out;
 n=r->color_image;primary=&r->owner[M9_FAA_PRIMARY_OWNER];program=&r->draw_owner[0];
 if(!primary->cpu||n->program_bytes>sizeof(observed)||!program->vram||
  r->primary_bytes>primary->bytes||primary->bytes-r->primary_bytes<n->primary_length){ret=-ERANGE;goto out;}
 for(i=0;i<PARHELIA_M9_FAA_GRAPH_OWNERS;i++){
  struct drm_gem_object *bo=m9x_faa_graph_binding_owner(binding,i);dma_resv_assert_held(bo->resv);
  if(!dma_resv_test_signaled(bo->resv,DMA_RESV_USAGE_READ)){ret=-EBUSY;goto out;}}
 for(i=0;i<3;i++){
  struct m9_faa_owner *o=i?&r->draw_owner[i]:primary;
  if(!o->cpu||!drm_mm_node_allocated(&o->system_node)||o->system_node.start!=o->base){ret=-ESTALE;goto out;}
  for(page=0;page<o->bytes/PAGE_SIZE;page++){
   ret=m9x_address_read_pte(mdev,o->base+page*PAGE_SIZE,&pte);if(ret)goto out;
   if(pte!=(m9x_address_pte_word(o->dma+page*PAGE_SIZE)|1)){ret=-ESTALE;goto out;}}}
 ret=m9x_address_check_local_identity(mdev,program->base,program->bytes);if(ret)goto out;
 ret=m9x_triangle_system_page_admit_locked(mdev,0x40004000);if(ret)goto out;
 ret=m9x_triangle_system_page_admit_locked(mdev,0x40008000);if(ret)goto out;
 fence=m9x_fence_create_target_locked(mdev,lower_32_bits(init_point)+n->lock_delta);if(!fence){ret=-ENOMEM;goto out;}
 for(i=0;i<PARHELIA_M9_FAA_GRAPH_OWNERS;i++)dma_resv_add_fence(m9x_faa_graph_binding_owner(binding,i)->resv,&fence->base,DMA_RESV_USAGE_WRITE);
 r->color_primary_start=r->graph_draw_end_bytes;
 r->color_posts_start=r->graph_draw_posts;
 r->color_attempted=true;
 m9x_write(mdev,M9_REG_FRONTEND_KICK,1);
 ret=m9x_address_cpu_local_prepare(mdev,program->base,program->bytes);if(ret)goto out;
 iosys_map_memcpy_to(&program->map,0,n->program,n->program_bytes);wmb();
 m9x_write(mdev,M9_REG_FRONTEND_STATE,0);m9x_read(mdev,M9_REG_FRONTEND_STATE);
 iosys_map_memcpy_from(observed,&program->map,0,n->program_bytes);
 if(memcmp(observed,n->program,n->program_bytes)){ret=-EIO;goto out;}
 memcpy(r->draw_owner[1].cpu,n->constants,n->constants_bytes);
 memcpy((u8*)r->draw_owner[1].cpu+n->geometry_offset,n->constants+n->geometry_offset,n->geometry_bytes);
 memcpy(r->draw_owner[2].cpu,n->secondary,n->secondary_bytes);
 for(i=0;i<4;i++){
  u32 old=r->graph_draw_end_bytes;
  if(!i){end=r->primary_bytes+n->draw_length;memcpy((u8*)primary->cpu+r->primary_bytes,n->primary,n->draw_length);}
  else if(i<3){end=r->primary_bytes+n->primary_length-12;if(i==1)memcpy((u8*)primary->cpu+r->primary_bytes+n->draw_length,n->primary+n->draw_length,n->primary_length-12-n->draw_length);}
  else{end=r->primary_bytes+n->primary_length;memcpy((u8*)primary->cpu+r->primary_bytes+n->primary_length-12,n->primary+n->primary_length-12,12);}
  dma_wmb();ret=m9x_post_faa_graph_end_locked(mdev,&primary->system_node,primary->dma,old,end,i==2,
   i==3?lower_32_bits(init_point)+n->lock_delta:0,&stage_point,&stage_posted);
  *posted|=stage_posted;if(stage_posted){r->graph_draw_end_bytes=end;r->graph_draw_posts++;}if(ret)goto out;}
 if(stage_point!=init_point+n->lock_delta){ret=-EIO;goto out;}
 *point=stage_point;r->graph_draw_point=stage_point;r->color_lock_point=stage_point;r->color_lock_retired=true;r->color_secondary_cursor=n->secondary_bytes;
 m9_faa_color_intermediates_observe(mdev,r);
 dev_info(&mdev->pdev->dev,"M9 FAA color Lock retired: init=%#llx lock=%#llx bytes=%u posts=%u program=%u secondary=%u; %u live vectors\n",init_point,*point,r->graph_draw_end_bytes,r->graph_draw_posts,n->program_bytes,n->secondary_bytes,n->layout.program_count);
out:
 if(ret)*point=0;if(fence&&ret)dma_fence_set_error(&fence->base,ret);
 if(ret&&*posted){t->wedged=true;t->generation++;WARN_ON(t->quarantined_fence);t->quarantined_fence=fence;fence=NULL;}
 else if(fence)dma_fence_signal(&fence->base);if(fence)dma_fence_put(&fence->base);
 mutex_unlock(&t->lock);return ret;
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
 bool stage_posted=false;u64 stage_point=0;u32 i,page,pte,end;int ret;
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
  for(page=0;page<o->bytes/PAGE_SIZE;page++){
   ret=m9x_address_read_pte(mdev,o->base+page*PAGE_SIZE,&pte);if(ret)goto out;
   if(pte!=(m9x_address_pte_word(o->dma+page*PAGE_SIZE)|1u)){ret=-ESTALE;goto out;}
  }
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
 if(ret)*point=0;
 if(fence&&ret)dma_fence_set_error(&fence->base,ret);
 if(ret&&*posted){t->wedged=true;t->generation++;WARN_ON(t->quarantined_fence);t->quarantined_fence=fence;fence=NULL;}
 else if(fence)dma_fence_signal(&fence->base);
 if(fence)dma_fence_put(&fence->base);
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
 u32 i,page,pte,old_bytes;int ret;
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
  for(page=0;page<o->bytes/PAGE_SIZE;page++){
   ret=m9x_address_read_pte(mdev,o->base+page*PAGE_SIZE,&pte);
   if(ret)goto out;
   if(pte!=(m9x_address_pte_word(o->dma+page*PAGE_SIZE)|1u)){ret=-ESTALE;goto out;}
  }
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
 u32 i,page,pte,old_bytes,first_end,final_end;int ret;
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
 for(page=0;page<primary->bytes/PAGE_SIZE;page++){
  ret=m9x_address_read_pte(mdev,primary->base+page*PAGE_SIZE,&pte);
  if(ret)goto out;
  if(pte!=(m9x_address_pte_word(primary->dma+page*PAGE_SIZE)|1u)){ret=-ESTALE;goto out;}
 }
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
	if (fence && ret)
		dma_fence_set_error(&fence->base, ret);
	if (ret && *posted) {
		t->wedged = true;
		t->generation++;
		WARN_ON(t->quarantined_fence);
		t->quarantined_fence = fence;
		fence = NULL;
	} else if (fence)
		dma_fence_signal(&fence->base);
	if (fence)
		dma_fence_put(&fence->base);
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
	u32 i, page, pte, target, old_bytes, end;
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
		for (page = 0; page < o->bytes / PAGE_SIZE; page++) {
			ret = m9x_address_read_pte(mdev, o->base + page * PAGE_SIZE, &pte);
			if (ret)
				goto out;
			if (pte != (m9x_address_pte_word(o->dma + page * PAGE_SIZE) | 1u)) {
				ret = -ESTALE;
				goto out;
			}
		}
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
	if (fence && ret)
		dma_fence_set_error(&fence->base, ret);
	if (ret && *posted) {
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
	u32 i, page, pte, target, old_bytes;
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
		for (page = 0; page < o->bytes / PAGE_SIZE; page++) {
			ret = m9x_address_read_pte(mdev, o->base + page * PAGE_SIZE, &pte);
			if (ret)
				goto out;
			if (pte != (m9x_address_pte_word(o->dma + page * PAGE_SIZE) | 1u)) {
				ret = -ESTALE;
				goto out;
			}
		}
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
	if (fence && ret)
		dma_fence_set_error(&fence->base, ret);
	if (ret && *posted) {
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
	u32 i, page, pte, target, old_bytes;
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
		for (page = 0; page < o->bytes / PAGE_SIZE; page++) {
			ret = m9x_address_read_pte(mdev, o->base + page * PAGE_SIZE, &pte);
			if (ret)
				goto out;
			if (pte != (m9x_address_pte_word(o->dma + page * PAGE_SIZE) | 1u)) {
				ret = -ESTALE;
				goto out;
			}
		}
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
	if (fence && ret)
		dma_fence_set_error(&fence->base, ret);
	if (ret && *posted) {
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

MODULE_FIRMWARE(M9_FAA_GRAPH_COLOR_FIRMWARE);
MODULE_FIRMWARE(M9_FAA_GRAPH_COLOR_FOUR_FIRMWARE);

/* A repeated COLOR frame owns the same graph/private context. Previous Unlock
 * must have retired completely before any retained code/data owner is rewritten.
 * This admission does not rebase a timeline or clear a reservation hazard. */
int m9x_faa_graph_color_unlocked_get_locked(struct m9x_device *mdev,
 u32 generation,u64 previous_unlock_point,struct m9_faa_graph_binding **out)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 struct m9x_transport *t=&mdev->transport;struct m9x_faa_replay *r=t->faa;
 lockdep_assert_held(&t->lock);if(!out)return -EINVAL;*out=NULL;
 if(t->wedged)return -EIO;
 if(!r||!r->graph_binding||!r->color_payload_ready||!r->color_image||
  r->color_image->layout.program_count!=4||!r->color_attempted||!r->color_lock_retired||
  !r->color_lock_point||!r->normal_lock_view_seen||!r->unlock_started||!r->unlock_retired||
  r->public_vs_image||r->draw_image||r->graph_generation!=generation||t->generation!=generation||
  r->observation_generation!=generation||!r->observation_retired||
  !previous_unlock_point||(previous_unlock_point&15)||
  lower_32_bits(previous_unlock_point)>U32_MAX-0x100||
  previous_unlock_point!=r->color_lock_point+0x20||r->unlock_point!=previous_unlock_point||
  r->graph_draw_point!=previous_unlock_point||t->submitted_point!=previous_unlock_point||
  t->completed_point!=previous_unlock_point||t->next_fence!=lower_32_bits(previous_unlock_point)+0x10||
  r->graph_draw_end_bytes!=r->graph_unlock_end_bytes||r->graph_draw_posts!=r->graph_unlock_posts||
  !r->color_secondary_cursor||r->color_next_attempt_point==previous_unlock_point)return -ESTALE;
 if(!t->faa_source_context||!t->faa_virtual_primary||!t->engine_transitioned||!t->restore_ready||
  !t->transport_armed||!t->bus_master_enabled||!t->fence_cpu||t->async_stop||t->async_pending||t->queue_count)return -EBUSY;
 *out=m9x_faa_graph_binding_get(r->graph_binding);return 0;
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
 const struct firmware *fw=NULL;u32 i,capacity;int ret;
 if(!binding||!input||!m9_faa_color_four_experimental||input->layout.program_count!=4||clear_flags>1)return -EOPNOTSUPP;
 if(input->layout.program_bytes>4096||input->layout.constants_bytes>16384||
  input->record_stride!=32||!input->record_count||input->record_count%3||input->record_count>12288||
  input->layout.geometry_bytes>0x40000-256)return -ERANGE;
 n=kzalloc(sizeof(*n),GFP_KERNEL);if(!n)return -ENOMEM;
 n->geometry_offset=ALIGN(input->layout.constants_bytes,128);
 capacity=n->geometry_offset+input->layout.geometry_bytes+768;
 n->constants=kzalloc(capacity,GFP_KERNEL);if(!n->constants){ret=-ENOMEM;goto release;}
 ret=request_firmware(&fw,M9_FAA_GRAPH_COLOR_FOUR_FIRMWARE,&mdev->pdev->dev);if(ret)goto release;
 mutex_lock(&t->lock);
 ret=m9x_faa_graph_color_unlocked_get_locked(mdev,generation,previous_unlock_point,&checked);if(ret)goto unlock;
 r=t->faa;if(checked!=binding){ret=-ESTALE;goto unlock;}
 if(r->draw_owner[0].bytes<input->layout.program_bytes||!r->draw_owner[0].vram){ret=-ERANGE;goto unlock;}
 for(i=0;i<31;i++){
  struct m9_faa_owner *o;
  switch(i){case 26:o=&r->draw_owner[0];break;case 27:o=&r->owner[26];break;
   case 28:o=&r->draw_owner[1];break;case 29:o=&r->draw_owner[2];break;
   case 30:o=&r->owner[27];break;default:o=&r->owner[i];break;}
  owner[i]=(struct m9x_faa_graph_draw_owner){o->base,o->bytes};
 }
 image=(struct m9x_faa_color_next_image){
  .image={n->program,n->constants,n->primary,n->secondary,sizeof(n->program),capacity,sizeof(n->primary),sizeof(n->secondary),&n->secondary_bytes},
  .clear=n->clear,.clear_capacity=sizeof(n->clear),.clear_length=&n->clear_bytes};
 ret=m9x_faa_color_next_encode(fw->data,fw->size,owner,r->primary_bytes,
  r->color_secondary_cursor,previous_unlock_point,input,clear_flags,&image);if(ret)goto unlock;
 if((u64)r->graph_draw_end_bytes+n->clear_bytes+3632>owner[27].bytes){ret=-ENOSPC;goto unlock;}
 n->layout=input->layout;n->constants_bytes=input->layout.constants_bytes;n->geometry_bytes=128+ALIGN(input->record_count,4)*input->record_stride;
 n->program_bytes=input->layout.program_bytes;n->primary_length=3632;n->draw_length=60;
 n->lock_delta=0xd0+(clear_flags?0x10:0);n->secondary_cursor=r->color_secondary_cursor;
 n->previous_unlock_point=previous_unlock_point;
 old=r->color_image;r->color_image=n;n=NULL;ret=0;
unlock:mutex_unlock(&t->lock);m9x_faa_graph_binding_put(checked);
release:if(fw)release_firmware(fw);if(n)kfree(n->constants);kfree(n);if(old)kfree(old->constants);kfree(old);return ret;
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
 u32 i,page,pte,start,end;u64 stage_point=0;bool stage_posted=false;int ret;
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
  for(page=0;page<o->bytes/PAGE_SIZE;page++){
   ret=m9x_address_read_pte(mdev,o->base+page*PAGE_SIZE,&pte);if(ret)goto out;
   if(pte!=(m9x_address_pte_word(o->dma+page*PAGE_SIZE)|1)){ret=-ESTALE;goto out;}}}
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
 if(ret)*point=0;if(fence&&ret)dma_fence_set_error(&fence->base,ret);
 if(ret&&*posted){t->wedged=true;t->generation++;WARN_ON(t->quarantined_fence);t->quarantined_fence=fence;fence=NULL;}
 else if(fence)dma_fence_signal(&fence->base);if(fence)dma_fence_put(&fence->base);
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
 struct m9_faa_graph_binding *binding,u32 generation,u64 previous_unlock_point,
 u32 color_policy)
{
#ifdef M9_FAA_GRAPH_INIT_CONTROL
 static const struct m9_faa_owner_spec spec[M9_FAA_DEPTH_OWNERS]={
  {0,0x50000,false,false,false,0},
  {0,0x8000,true,false,false,0},
  {0,0x98000,true,false,false,0},
 };
 static const struct m9x_faa_depth_view shape[M9_FAA_DEPTH_OWNERS]={
  {0,0x50000,0x50000,320,240,4,0x7c},
  {0,0x3000,0x8000,40,60,5,0x78},
  {0,0x97000,0x98000,320,240,4,0x3d},
 };
 struct m9x_transport *t=&mdev->transport;
 struct m9_faa_graph_binding *checked=NULL;
 struct m9_faa_depth_storage *n;
 struct m9x_faa_replay *r;
 u32 i;int ret;

 /* The original color31 MDL is flags30: its compact PTE policy is B.
  * Ordinary SYSTEM policy A is intentionally not admitted for this role. */
 if(!binding||color_policy!=M9_FAA_DEPTH_COLOR_SYSTEM_B)return -EINVAL;
 n=kzalloc(sizeof(*n),GFP_KERNEL);if(!n)return -ENOMEM;
 n->generation=generation;n->previous_unlock_point=previous_unlock_point;
 n->views.color_policy=color_policy;
 n->views.depth_aux_role=M9_FAA_DEPTH_HZ;n->views.aux_depth_role=M9_FAA_BOUND_DEPTH;
 memcpy(n->views.owner,shape,sizeof(shape));
 mutex_lock(&t->lock);
 ret=m9x_faa_graph_color_unlocked_get_locked(mdev,generation,previous_unlock_point,&checked);
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
 ret=m9x_faa_graph_color_unlocked_get_locked(mdev,generation,previous_unlock_point,&checked);
 if(ret)goto retained;
 r=t->faa;
 if(checked!=binding||r->depth_storage!=n){ret=-ESTALE;goto retained;}
 if(color_policy==M9_FAA_DEPTH_COLOR_SYSTEM_B){
  ret=m9_faa_dma_prepare_wc(mdev,&n->owner[M9_FAA_BOUND_COLOR]);
  if(ret)goto retained;
 }
 ret=color_policy==M9_FAA_DEPTH_COLOR_SYSTEM_B?
  m9x_address_map_faa_system(mdev,&n->owner[0].system_node,n->owner[0].dma,0x50000):
  m9x_address_map_system(mdev,&n->owner[0].system_node,n->owner[0].dma,0x50000);
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
 struct m9_faa_depth_storage *n;u32 i,page,pte;int ret;
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
 for(i=0;i<M9_FAA_DEPTH_OWNERS;i++){
  struct m9_faa_owner *o=&n->owner[i];
  const struct m9x_faa_depth_view *v=&n->views.owner[i];
  if(v->base!=o->base||v->capacity_bytes!=o->bytes||!v->logical_bytes||
   v->logical_bytes>o->bytes||!IS_ALIGNED(o->base,PAGE_SIZE)||
   (u64)o->base+o->bytes>U32_MAX)return -ESTALE;
  if(i==M9_FAA_BOUND_COLOR){
   if(o->vram||!o->cpu||!drm_mm_node_allocated(&o->system_node)||
    o->system_node.start!=o->base||o->system_node.size!=o->bytes||
    o->cpu_wc!=(n->views.color_policy==M9_FAA_DEPTH_COLOR_SYSTEM_B))return -ESTALE;
   for(page=0;page<o->bytes/PAGE_SIZE;page++){
    ret=m9x_address_read_pte(mdev,o->base+page*PAGE_SIZE,&pte);if(ret)return ret;
    if(pte!=(m9x_address_pte_word(o->dma+page*PAGE_SIZE)|
     (n->views.color_policy==M9_FAA_DEPTH_COLOR_SYSTEM_B?1u:0u)))return -ESTALE;
   }
  }else{
   if(!o->vram||o->cpu||iosys_map_is_null(&o->map))return -ESTALE;
   ret=m9x_address_check_local_identity(mdev,o->base,o->bytes);if(ret)return ret;
  }
 }
 *views=n->views;return 0;
#else
 return -EOPNOTSUPP;
#endif
}
