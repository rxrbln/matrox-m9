/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * M9 rendering context.  The Gallium draw module transforms vertices on the
 * CPU and hands window-space primitives to a vbuf render, which batches them
 * into two-pass pass-through jobs (m9_draw.c) rasterized by the hardware.
 * Fragment shaders run as native programs from m9_fs_compile; the verified
 * interpolated-color pass-through keeps its captured image.
 */
#include "m9_screen.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include "draw/draw_context.h"
#include "draw/draw_vbuf.h"
#include "draw/draw_vertex.h"
#include "indices/u_indices.h"
#include "m9_batch.h"
#include "m9_draw.h"
#include "m9_vs_record.h"
#include "m9_format.h"
#include "m9_srgb.h"
#include "m9_fs_compile.h"
#include "m9_specialise.h"
#include "m9_internal.h"
#include "m9_indirect.h"
#include "m9_faa_application.h"
#include "m9_faa_graph_uapi.h"
#include "m9_vs_texture.h"
#include "m9_weld.h"
#include "m9_vs_source.h"
#include "m9_passthrough.h"
#include "m9_query.h"
#include "m9_state.h"
#include "nir.h"
#include "nir_builder.h"
#include "nir/tgsi_to_nir.h"
#include "tgsi/tgsi_from_mesa.h"
#include "util/u_blitter.h"
#include "util/u_debug.h"
#include "util/u_debug_cb.h"
#include "util/format/u_format.h"
#include "util/u_draw.h"
#include "util/u_framebuffer.h"
#include "util/u_helpers.h"
#include "util/u_inlines.h"
#include "util/os_time.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_pack_color.h"
#include "util/u_prim.h"
#include "util/u_surface.h"
#include "util/u_transfer.h"
#include "util/u_upload_mgr.h"

/* Source record: x, y bottom-up, z in ortho object space, r, g, b, a. */
/* x, y, z, w, r, g, b, a: the w slot carries 1/w_clip for the setup engine. */
#define M9_RECORD_FLOATS 8u

enum m9_warning {
   M9_WARN_TARGET = 1u << 0,
   M9_WARN_FS = 1u << 1,
   M9_WARN_BLEND = 1u << 2,
   M9_WARN_DEPTH = 1u << 3,
   M9_WARN_STENCIL = 1u << 4,
   M9_WARN_POINTS = 1u << 5,
   M9_WARN_BLIT = 1u << 6,
   M9_WARN_CLEAR = 1u << 7,
   M9_WARN_TRANSFER = 1u << 8,
   M9_WARN_MEMORY = 1u << 9,
   M9_WARN_TEXTURE = 1u << 10,
   M9_WARN_WRAP = 1u << 11,
   M9_WARN_SUBMIT = 1u << 12,
};

struct m9_transfer {
   struct pipe_transfer base;
   uint8_t *staging;  /* de-tiled rows of a tiled render target */
};

struct m9_fs_state {
   struct draw_fragment_shader *draw;
   nir_shader *nir;
   bool passthrough;
   bool kill_all;                      /* every fragment is discarded */
   bool native, native_tried, warned;
   const char *reason;
   struct util_dynarray variants;      /* struct m9_fs_variant * */
};

/* CPU views a split shader may need: one per interpolated input and one per
 * distinct texture coordinate of every pass. */
#define M9_FS_MAX_EXTRA (M9_TRANSIENT_VIEWS - M9_TRANSIENT_CPU_VIEW)

/* Streams and units of one raster pass of a fragment shader. */
struct m9_fs_pass_streams {
   unsigned coord_count;               /* distinct texture coordinate streams */
   uint8_t coord_location[M9_FS_MAX_TEXTURES];
   uint8_t coord_component[M9_FS_MAX_TEXTURES];  /* first component the stream carries */
   int8_t coord_extra[M9_FS_MAX_TEXTURES];     /* CPU view of a coordinate stream, -1 for COL0 */
   uint8_t texture_coord[M9_FS_MAX_TEXTURES];  /* coordinate stream per texel pair */
   int8_t varying_extra[M9_FS_MAX_VARYINGS];   /* CPU view of an interpolated input */
   /* Job unit pool entry of each texel slot; M9_PASS_UNIT_PLACEHOLDER binds
    * the opaque-black texture a record-writing pass without one still needs,
    * and a slot fed by a record binds no texture at all. */
   uint8_t unit[M9_FS_MAX_TEXTURES];
};

/* m9_internal.h states the colour target bound as M9_MAX_COLOR because it
 * cannot include m9_draw.h; the two must agree. */
static_assert(M9_MAX_COLOR == M9_DRAW_MAX_COLOR,
              "colour target bound stated twice");

/* A fragment state specialised on blend state and inlined uniform values. */
struct m9_fs_variant {
   struct m9_fs_blend blend;
   /* Colour targets bound when this variant was compiled: gl_FragColor is
    * broadcast to all of them, so the program is specialised on the count. */
   unsigned targets;
   /* 1 + the one colour output an MRT blend pass keeps, 0 for all. */
   unsigned select;
   uint32_t inline_values[MAX_INLINABLE_UNIFORMS];
   uint8_t swizzle[PIPE_MAX_SAMPLERS][4];  /* view swizzle per sampler, applied in NIR */
   uint32_t depth_texture;             /* samplers bound to a depth surface */
   bool face_flip;                     /* gl_FrontFacing inverted: CW front */
   uint32_t srgb_texture;              /* samplers whose view decodes sRGB */
   bool ok;
   const char *reason;                 /* why the variant is not drawable */
   /* The raster passes the shader runs as; one for everything the engine
    * takes in a single pass. */
   struct m9_fs_passes passes;
   unsigned entry[M9_FS_MAX_PASSES];   /* c2 slot each pass enters at */
   unsigned record_dwords;             /* stride of the inter-pass record */
   uint8_t record_offset[M9_FS_MAX_PASSES];  /* each producer's slot start */
   struct m9_resource c2;              /* fragment slots, then vertex program */
   /* The same fragment slots with a compiled vertex program behind them. */
   struct m9_resource native_c2;
   uint64_t native_c2_serial;
   struct util_dynarray native_c2_images; /* struct m9_native_image */
   unsigned vertex_entry;
   unsigned extra_count;               /* CPU view locations: varyings, then coordinates */
   uint8_t extra_location[M9_FS_MAX_EXTRA];
   struct m9_fs_pass_streams stream[M9_FS_MAX_PASSES];
   /* Application texture slots of every pass, in binding order. */
   unsigned slot_count;
   uint8_t slot_pass[M9_DRAW_MAX_UNITS], slot_index[M9_DRAW_MAX_UNITS];
   /* Command budget of the job this variant builds: every pass emits its
    * own preprocessing and raster range. */
   unsigned relocs_base, relocs_draw, dwords_base, dwords_draw;
};

/* Words and relocations the passes of a variant cost for this many draws. */
static unsigned
m9_variant_relocs(const struct m9_fs_variant *v, unsigned draws)
{
   return v->relocs_base + draws * v->relocs_draw;
}

static unsigned
m9_variant_dwords(const struct m9_fs_variant *v, unsigned draws)
{
   return v->dwords_base + (draws - 1) * v->dwords_draw;
}

/* The pass that writes the colour; it carries the blend the state asked for. */
static inline const struct m9_fs_program *
m9_fs_final(const struct m9_fs_variant *v)
{
   return &v->passes.pass[v->passes.count - 1];
}

struct m9_velems {
   unsigned count;
   struct pipe_vertex_element elements[PIPE_MAX_ATTRIBS];
};

struct m9_vbuf_render {
   struct vbuf_render base;
   struct m9_context *ctx;
   struct vertex_info vinfo;
   enum mesa_prim prim;
   uint8_t *vertices;
   size_t capacity;
   uint16_t vertex_size;
   bool has_color;
   unsigned extra_count;
   /* One per CPU view of the whole variant, not of one pass: a split
    * shader has a view per interpolated input and per coordinate stream of
    * every pass, which a one-pass bound is eight short of. */
   int extra_offset[M9_FS_MAX_EXTRA];  /* float offset, -1 if unwritten */
   struct util_dynarray indices;
   struct util_dynarray quads;         /* lines expanded to triangles */
};

static int m9_submit_job(struct m9_context *ctx);
static int m9_aggregate_flush(struct m9_context *ctx);
static void m9_proxy_writeback(struct m9_context *ctx);
static void m9_proxy_reseed(struct m9_context *ctx);
static void m9_deferred_run(struct m9_context *ctx);
/* Back-end objects swapped rather than zeroed under a job in flight. */
static uint64_t m9_be_swaps;

static inline struct m9_context *
m9_context(struct pipe_context *pctx)
{
   return (struct m9_context *)pctx;
}

static inline struct m9_resource *
m9_res(struct pipe_resource *pres)
{
   return (struct m9_resource *)pres;
}

static void
m9_warn_once(struct m9_context *ctx, uint32_t bit, const char *message)
{
   if (ctx->warned & bit)
      return;
   ctx->warned |= bit;
   fprintf(stderr, "m9x: %s\n", message);
}

static void
m9_skip_report(struct m9_context *ctx)
{
   fprintf(stderr, "m9x: context %u: %u seen, %u skipped, %u reasons\n",
           ctx->context_id, ctx->submitted_draws, ctx->skipped_draws,
           ctx->skip_reasons);
   fprintf(stderr, "m9x: window %u reads %" PRIu64 " KiB, %u writes %" PRIu64
           " KiB, map usage %#x, read usage %#x, explicit reads %u\n",
           ctx->window_reads, ctx->window_read_bytes >> 10, ctx->window_writes,
           ctx->window_write_bytes >> 10, ctx->map_usage_seen,
           ctx->window_read_usage, ctx->window_reads_explicit);
   fprintf(stderr, "m9x: %u copies and blits on the engine, %u split draws "
           "padded to their target's extent, %" PRIu64 " state swaps\n",
           ctx->engine_blits, ctx->padded_draws, m9_be_swaps);
   fprintf(stderr, "m9x: %u render-target clears on the engine\n",
           ctx->engine_clears);
   for (unsigned i = 0; i < ctx->skip_reasons; i++)
      fprintf(stderr, "m9x: context %u: %u draws skipped: %s\n",
              ctx->context_id, ctx->skip_reason[i].draws,
              ctx->skip_reason[i].reason);
}

/* Warnings are printed once per reason; the tally keeps the draw count. */
static bool
m9_skip_reason(struct m9_context *ctx, const char *reason)
{
   unsigned i;

   for (i = 0; i < ctx->skip_reasons; i++)
      if (ctx->skip_reason[i].reason == reason) {
         ctx->skip_reason[i].draws++;
         return false;
      }
   if (ctx->skip_reasons < M9_MAX_SKIP_REASONS) {
      ctx->skip_reason[ctx->skip_reasons].reason = reason;
      ctx->skip_reason[ctx->skip_reasons++].draws = 1;
   }
   return false;
}

/* M9X_DEBUG_TEXTURES traces the level state of every sampled texture and
 * M9X_TEXTURE_DUMP writes each uploaded level 0 out as a PPM. */
static bool m9_debug_textures;
static const char *m9_texture_dump;
static bool m9_texture_addrcode;
/* Opt-in window receipt for blit stage captures.  Transfer unmap is void,
 * so a staging write can otherwise be mistaken for a completed CPU blit. */
static bool m9_blit_window_trace;

static void
m9_blit_window_receipt(const struct m9_resource *resource,
                       const struct drm_parhelia_m9_surface_access *access,
                       int ret)
{
   if (m9_blit_window_trace)
      fprintf(stderr, "m9x: window %s handle %u image %ux%u box %u,%u %ux%u "
              "d0 %08x d1 %08x flags %#x capacity %#" PRIx64 " ret %d\n",
              access->op == PARHELIA_M9_ACCESS_WRITE ? "WRITE" : "READ",
              resource->handle, resource->width, resource->height,
              access->x, access->y, access->width, access->height,
              access->descriptor0, access->descriptor1, access->flags,
              resource->size, ret);
}

/* M9X_PROFILE=N reports where the frame went every N frames.  The phases
 * nest: draw contains emit, emit contains submit, submit contains the
 * varying gather, the record copy and the submit ioctl. */
enum m9_prof {
   M9_PROF_DRAW, M9_PROF_EMIT, M9_PROF_SUBMIT, M9_PROF_VARYING,
   M9_PROF_RECORDS, M9_PROF_IOCTL, M9_PROF_TARGET, M9_PROF_CLEAR,
   M9_PROF_TRANSFER, M9_PROF_BE, M9_PROF_BATCH, M9_PROF_EXTENT,
   M9_PROF_COUNT,
};

static const char *const m9_prof_name[M9_PROF_COUNT] = {
   "draw", "emit", "submit", "varying", "records", "ioctl", "target",
   "clear", "transfer", "be", "batch", "extent",
};

static unsigned m9_profile_period;
/* M9X_JOB_DRAWS ends a job after that many draws: the frame time against the
 * job count weighs what one job costs. */
static unsigned m9_job_draws;
/* M9X_JOB_VERTICES sets the source records one job preprocesses.  Every draw
 * of a mesh scene fills the ceiling and so ends its job, which is what makes
 * one job a draw; the vendor's own 5760 is its partition of a context
 * resource rather than an engine extent (m9_draw_params.h).  Setting it back
 * to 5760 restores one job per draw in the same binary; the frame profile
 * names the ceiling in force. */
static uint64_t m9_vs_serial;

/* M9X_VS_VARYINGS caps the varyings a native preprocessing job may carry.
 * The default is 2, which the single fused draw carries on its own: the
 * second varying takes output target 1, the clip position's, whose two tail
 * slots are pointed at the tail's own scratch, and the raster pass then
 * declares no attribute 13 -- which it renders the same pixels without
 * (MEASURED on the M9128, fixture vary3 exact and native).
 * A third still needs the second preprocessing draw, and that draw has
 * never delivered a stream on this board: five shapes have been run and
 * none delivers (MEASURED 2026-09-19) -- the chain role in ef, separate
 * stream views with the slot marker every captured program carries, a
 * record descriptor on those views, the captured chain itself, and the
 * fourth output target, which the engine refuses beside the clip code.
 * Raising the knob past 2 emits the extra draw; 5 is what the vertex
 * backend compiles.  docs/parhelia-m9-vertex-output-target-re.md */
static unsigned m9_native_vs_varyings = M9_VS_FUSED_VARYINGS;
/* Native preprocessing produces texture coordinates by default; =0 is
 * the CPU diagnostic arm.  The measured coordinate-sharing and record
 * coefficient paths retain GPU transforms without the older extra stream
 * and uniform-split costs.  docs/parhelia-m9-native-vertex-backend.md */
static bool m9_native_vs_texture;
/* A coordinate beside another stream takes the measured second output
 * target by default; =0 retains the CPU diagnostic arm.  M9128 ffarray is
 * pixel-exact through this form and unit-projector Quake3 moves about 63%
 * of its draws onto the GPU.  Its new per-object uniform splits currently
 * offset the CPU transform saving, so FPS is unchanged; retain GPU coverage
 * and address that submission cost. */
static bool m9_native_vs_coord_pair;
/* M9X_NO_CULL=1 clears the ef cull bits of a native draw.  A diagnostic:
 * the draw module removes back-facing triangles on the CPU before it hands
 * a draw over and the native path does not, so this says whether the engine
 * removes them itself. */
static bool m9_no_cull;
/* M9X_COORD_SHARE=0 stops a sampler reading its pair out of a varying the
 * same raster pass already interpolates, through the declaration's
 * component selectors, and gives it a coordinate stream of its own again.
 * Sharing applies only where the coordinate needs no scale or bias, because
 * a shared stream carries the varying's own values.  No captured raster
 * declaration uses a non-zero first selector; this one is MEASURED on the
 * M9128 -- six textured fixtures exact, glmark2 texture native at the draw
 * module's own frame rate. */
static bool m9_coord_share;
/* Two identity coordinate pairs can occupy XY and ZW of one native stream;
 * =0 keeps the separate-output diagnostic arm. */
static bool m9_coord_pack;
/* M9X_VS_SPLIT=1 lets a native preprocessing job carry a fragment program
 * the engine runs as several raster passes.  The layout below is the union
 * over the passes, which for a shader that hands a coordinate between them
 * is one stream -- glmark2's shadow is that shape -- but the hardware
 * refuses it: the coordroute fixture, a two-pass job whose second pass
 * takes its coordinate from the inter-pass record, renders nothing at all
 * (MEASURED on the M9128, 76800 of 76800 pixels differ, no stream timeout,
 * the card alive afterwards).  Off until that is understood. */
static bool m9_native_vs_split;
/* GPU padding/multipass transport is a separate hardware qualification. */
static bool m9_native_vs_gpu_padding;
/* M9X_VS_SPECIALISE=0 compiles a vertex shader without substituting the
 * inlinable uniform values, which is the form the backend refused for
 * control flow before those values were used.  A diagnostic: it is what a
 * measurement of the specialisation is read against. */
static bool m9_vs_specialise = true;
/* M9X_VS_MAX_SLOTS bounds the compiled program a native preprocessing job
 * will run; a longer one stays in the draw module.  It is a cost bound and
 * not a correctness one: the engine runs a 5161-slot program and renders
 * every pixel of it exactly (MEASURED, vschain:1024).  What it bounds is a
 * step in the vertex stage's own cost, MEASURED with vsperf:N over 20000
 * vertices a draw -- 188 slots costs 0.910 to 1.290 ms a draw depending on
 * the fragment program beside it and 191 costs 1.78 to 2.38, about twice as
 * much, and the step lands between the same two lengths whether the
 * fragment program in the image is four slots or forty.  Past it the engine
 * loses to the draw module outright (201 slots: 1.87 against 1.30 ms), and
 * glmark2's 299-slot buffer scene loses 36% of its frame rate with the host
 * at zero -- 6.40 -> 10.03 ms a frame, draw 2.96 and emit 1.92 both to 0.00.
 * Below the step native wins at every length measured.
 * docs/parhelia-m9-vertex-isa-gaps.md */
static unsigned m9_vs_max_slots = 188;
/* M9X_VS_MAX_CONST_PAIRS bounds the constant span a native preprocessing job
 * carries; past it the engine stops rather than drawing (M9_VS_MAX_CONST_PAIRS
 * records the measurement).  M9X_VS_CONST_PAD appends that many unnamed pairs
 * to the span, which is how the span's own size is measured apart from the
 * count of constants a program names. */
static unsigned m9_vs_max_const_pairs = M9_VS_MAX_CONST_PAIRS;
static unsigned m9_vs_const_pad;
/* Initially diagnostic until the matrix A/B/A native readback proves the
 * captured constant update between measured numeric preprocessing runs. */
static bool m9_vs_uniform_batch;
/* M9X_NATIVE_VS=0 keeps every vertex shader in the draw module.  On by
 * default since the whole hardware fixture corpus passes either way
 * (m9-mesa-vs 14/14, m9-mesa-glsl 90/90, 2026-09-17). */
static bool m9_native_vs;

/* M9X_PASS_LOOP emits even a one-pass shader through the multi-pass loop. */
static bool m9_pass_loop;
/* M9X_SPLIT_STAGE isolates where a split job stops the engine, one binding
 * at a time.  1 leaves the record unbound and the routes idle, 2 binds the
 * record and keeps the routes idle, 3 is the real schedule, 4 binds the
 * record object with an ordinary colour descriptor, 5 with the captured
 * two-dword producer descriptor, 6 with the record format over the
 * colour target's row span and 7 with the colour descriptor and the plain
 * colour-pass state words, which leaves the bound object as the only
 * difference from stage 1. */
static unsigned m9_split_stage;
/* M9X_SPLIT_PREPROCESS runs the preprocessing range ahead of every pass. */
static bool m9_split_preprocess;
/* M9X_SPLIT_DEPTH: 1 the caller's depth state in the first raster pass only,
 * 0 no depth at all in a split job, 2 the schedule that stops the engine. */
static unsigned m9_split_depth;
/* M9X_SPLIT_SUBMIT=0 puts every pass of a split shader in one submission. */
static bool m9_split_submit;
/* M9X_SPLIT_PASSES=N emits only the first N passes of a split shader. */
static unsigned m9_split_passes;
/* M9X_SPLIT_SKIP_ABOVE / M9X_SPLIT_SKIP_BELOW drop a split job by the area of
 * its target, and M9X_SPLIT_TRACE names every split job it emits; 2 names
 * every job, so a scene's split jobs can be counted against its whole. */
static uint64_t m9_split_skip_above, m9_split_skip_below;
static unsigned m9_split_trace;
/* Round a screen coordinate out to the grain and hold it inside the window. */
static uint32_t
m9_clip_floor(float v, uint32_t grain, uint32_t lo, uint32_t hi)
{
   uint32_t u = v > 0 ? (uint32_t)v / grain * grain : 0;

   return CLAMP(u, lo, hi);
}

static uint32_t
m9_clip_ceil(float v, uint32_t grain, uint32_t lo, uint32_t hi)
{
   uint32_t u = v > 0 ? ((uint32_t)(v + 0.9999f) + grain - 1) / grain * grain : 0;

   return CLAMP(u, MIN2(lo, hi), hi);
}

/* M9X_SPLIT_CLIP=N narrows a split job's clip window to the column span its
 * geometry covers, rounded out to N pixels: a split draw that leaves part of
 * a row of its target uncovered stops the engine (MEASURED). */
static unsigned m9_split_clip;
/* M9X_SPLIT_PARTIAL=1 submits a multi-pass job whose draw covers only part of
 * its render target, which is the schedule that stops the engine. */
static bool m9_split_partial;
/* M9X_SPLIT_SCRATCH=0 refuses such a job instead of rendering it into a
 * scratch surface of the box it covers and copying that into place. */
static bool m9_split_scratch;
/* M9X_SPLIT_SEED=0 leaves the scratch surface unseeded, which separates the
 * copy in from the copy out; the image is then right only where the geometry
 * fills its own box.  M9X_SPLIT_COPY=2 copies through CPU transfers rather
 * than on the engine, which separates the copies from the job between
 * them. */
static bool m9_split_seed;
static unsigned m9_split_copy;
/* The engine's back-end object ahead of a multi-pass job.  3 clears the one
 * every multi-pass job binds, which is what a scene drawing several targets a
 * frame needs; 2 and 1 narrow it to the scratch path's own objects and 0
 * leaves it alone. */
static unsigned m9_split_be;
/* M9X_SPLIT_PAD=0 refuses a partial multi-pass draw to the scratch surface
 * instead of padding it out to its target's extent and narrowing the clip
 * window to the pixels it covers, which is what the engine counts as covering
 * a target (MEASURED, conv3pad / conv3frame / conv3subpad).  Padding is on by
 * default because it carries a draw a scratch surface cannot -- one whose
 * geometry does not fill the box it spans -- and it needs M9X_SPLIT_BE=3
 * beside it, which stops the engine without it (MEASURED). */
static bool m9_split_pad_enable = true;
/* M9X_RECORD_DUMP names a file the inter-pass record is written to after
 * every split job, so the slots the engine touched can be read back and
 * the record's granularity measured rather than assumed. */
static const char *m9_record_dump;
static const char *m9_vs_record_dump;
static bool m9_dump_native_inputs_enable;
/* Raster draws one split job restates.  Two of them stop the engine
 * (MEASURED, docs/parhelia-m9-fragment-pass-hardware.md), and one draw
 * also bounds the record: its fragment counter then covers a single
 * draw's fragments rather than a whole job's.  M9X_SPLIT_DRAWS raises
 * it so the limit can be re-measured. */
static unsigned m9_split_draws;
/* M9X_SPLIT_COVERAGE=0 keeps the plain coverage words on a record pass. */
static bool m9_split_coverage;
/* M9X_RECORD_89 replaces index 0x89 of a record pass, whose low twelve bits
 * are one three-bit render format class per bound target. */
static unsigned m9_record_89;
static bool m9_fragdepth_state;
/* M9X_SPLIT_SEQ=0 clears raster ef bits 10:9, which a pass submitted on its
 * own carries although its range holds no other pass. */
static bool m9_split_seq;
/* M9X_SPLIT_30=1 restates index 30 by the pass's place in the sequence
 * rather than its place inside the submission. */
static bool m9_split_30;
/* M9X_BE_CLEAR=1 clears the engine state object ahead of every job, 2 only
 * when the target's shape changed since the job before it. */
static unsigned m9_be_clear;
/* The engine keeps per-target state in the object bound at index be, and a
 * target revisited after one of another shape stops the engine when it finds
 * the state of that other shape there (MEASURED: splitpair:84x84:320x240:20,
 * splitsize3:6 and splitpair:168x168:640x480:20 all stop the engine with one
 * object a context and all render with one a target).  M9X_BE_PER_TARGET=0
 * restores the single object, which is how the fixture above was measured. */
/* M9X_BE_REUSE=0 clears the back-end object ahead of every split job, which
 * is what it did before the shape was remembered. */
static bool m9_be_reuse;
/* M9X_BE_SHAPE_CLIP=0 keeps the clip window out of that comparison. */
static bool m9_be_shape_clip = true;
/* Bytes of the back-end object a clear writes; 0 is the whole of it.  The
 * object is a megabyte of VRAM and the clear is CPU write-combined, so how
 * much of it the engine actually reads is worth a frame rate. */
static unsigned m9_be_clear_bytes;
static bool m9_be_per_target = true;
/* A posted job retires only at the next submission, so the engine can still
 * be reading the object a CPU clear overwrites.  M9X_BE_SWAP=0 zeroes it in
 * place instead of swapping to a spare. */
static bool m9_be_swap = true;
/* M9X_RECORD_LIVENESS=0 gives every hand-over a slot of its own, which is
 * what the record cost before slots were reused. */
static bool m9_record_liveness = true;
/* M9X_BLIT_ENGINE=0 keeps every blit off the engine, which is the only way
 * to reach the CPU resampler from an application that renders into ordinary
 * colour targets. */
static bool m9_blit_on_engine = true;
/* M9X_LEVEL_TARGET=0 refuses a mip level above zero as a render target, so
 * one binary measures a chain built by the engine against the same chain
 * built by the CPU transfer path. */
static bool m9_level_targets = true;
/* Keep the CPU clear available for image/cost comparisons. */
static bool m9_clear_on_engine = true;
/* M9X_BE_DUMP names a file the engine state object is written to after every
 * job, so what it accumulates can be read rather than assumed. */
static const char *m9_be_dump;
static unsigned m9_be_seq;
/* M9X_TRANSITION=0 drops the vendor's inter-draw engine transition from a
 * split job, which separates a fault in that block from one in the record. */
static bool m9_transition;
/* M9X_SPLIT=0 refuses the multi-pass schedule instead of emitting it.  It is
 * on: with each pass placed in its sequence at index ef the engine takes the
 * inter-pass record as a render target, and a five-fetch shader renders
 * pixel-exact (MEASURED). */
static bool m9_split_enable;
/* M9X_INDEXED=0 falls back to one record per index. */
static bool m9_indexed_draws = true;
static uint64_t m9_prof_ns[M9_PROF_COUNT];
static uint64_t m9_prof_hits[M9_PROF_COUNT];
static uint64_t m9_prof_records, m9_prof_jobs, m9_prof_varying_bytes;
static uint64_t m9_prof_draws, m9_prof_split[8];
static uint64_t m9_prof_indexed, m9_prof_referenced, m9_prof_welded_corners;
static uint64_t m9_prof_refused, m9_prof_skipped, m9_prof_relocs;
/* Draws the native vertex path took and the draws it left to the draw
 * module, since the last profile line: a benchmark that never destroys its
 * drawing context reports nothing at all otherwise. */
static uint64_t m9_prof_native, m9_prof_module;
static const char *m9_native_fallback_dump;
static unsigned m9_native_fallback_dump_count;

/* Reasons have static storage.  Keep cumulative context totals for teardown
 * and a separate cursor for the bounded profile interval report. */
struct m9_native_vs_reason {
   const char *reason;
   uint32_t count;
   uint32_t profile_count;
   bool dumped;
};
/* M9X_NO_CLEAR drops the clear quad; M9X_CLEAR_REPEAT draws it that many
 * extra times, which weighs one screen of fill with the frame still correct. */
static bool m9_no_clear;
/* M9X_MRT_MASK_ALL leaves a bound target the shader does not write open, so
 * a destination sweep sees where an output lands rather than where the mask
 * lets it.  docs/parhelia-m9-mrt-routing.md */
static bool m9_mrt_mask_all;
static unsigned m9_clear_repeat;
static uint64_t m9_prof_frames, m9_prof_wall;

static inline uint64_t
m9_prof_begin(void)
{
   return m9_profile_period ? os_time_get_nano() : 0;
}

static inline void
m9_prof_end(enum m9_prof phase, uint64_t start)
{
   if (m9_profile_period) {
      m9_prof_ns[phase] += os_time_get_nano() - start;
      m9_prof_hits[phase]++;
   }
}

/* Called once per frame with the wall time of the whole frame. */
static void
m9_prof_frame(struct m9_context *ctx)
{
   static uint64_t last;
   uint64_t now;
   double f;

   if (!m9_profile_period)
      return;
   now = os_time_get_nano();
   if (last)
      m9_prof_wall += now - last;
   last = now;
   if (++m9_prof_frames < m9_profile_period)
      return;
   f = (double)m9_prof_frames;
   fprintf(stderr, "m9x: %.0f frames %.2f ms/frame (job vertices %u): ", f,
           m9_prof_wall / f / 1e6, m9_draw_max_source_vertices());
   for (unsigned i = 0; i < M9_PROF_COUNT; i++)
      fprintf(stderr, "%s %.2f ", m9_prof_name[i], m9_prof_ns[i] / f / 1e6);
   fprintf(stderr, "| jobs %.0f draws %.0f records %.0f varying KiB %.1f "
           "refused %.0f skipped %.0f relocs %.0f "
           "indexed %.0f referenced %.0f welded %.0f splits target %.0f program %.0f "
           "depth %.0f mask %.0f clip %.0f uniforms %.0f vertices %.0f "
           "stream %.0f | native %.0f draw module %.0f\n",
           m9_prof_jobs / f, m9_prof_draws / f, m9_prof_records / f,
           m9_prof_varying_bytes / f / 1024.0, m9_prof_refused / f,
           m9_prof_skipped / f, (double)m9_prof_relocs, m9_prof_indexed / f,
           m9_prof_referenced / f, m9_prof_welded_corners / f,
           m9_prof_split[0] / f,
           m9_prof_split[1] / f, m9_prof_split[2] / f, m9_prof_split[3] / f,
           m9_prof_split[4] / f, m9_prof_split[5] / f, m9_prof_split[6] / f,
           m9_prof_split[7] / f, (double)m9_prof_native, (double)m9_prof_module);
   util_dynarray_foreach(&ctx->native_vs_reasons, struct m9_native_vs_reason,
                         it) {
      uint32_t count = it->count - it->profile_count;

      if (count)
         fprintf(stderr, "m9x: profile draw module %u: %s\n", count, it->reason);
      it->profile_count = it->count;
   }
   memset(m9_prof_ns, 0, sizeof(m9_prof_ns));
   memset(m9_prof_hits, 0, sizeof(m9_prof_hits));
   m9_prof_records = m9_prof_jobs = m9_prof_varying_bytes = 0;
   m9_prof_draws = m9_prof_indexed = m9_prof_referenced = 0;
   m9_prof_welded_corners = 0;
   m9_prof_refused = m9_prof_skipped = m9_prof_relocs = 0;
   m9_prof_native = m9_prof_module = 0;
   memset(m9_prof_split, 0, sizeof(m9_prof_split));
   m9_prof_frames = 0;
   m9_prof_wall = 0;
}


static void m9_retired_variants_free(struct m9_context *ctx);

static void
m9_context_sync_ordinary(struct m9_context *ctx)
{
   if (ctx->draw)
      draw_flush(ctx->draw);
   m9_submit_job(ctx);
   m9_aggregate_flush(ctx);
   /* Outside the draw module: a job held for the scratch path runs here,
    * before anything this sync was called for reads the target. */
   m9_deferred_run(ctx);
   m9_aggregate_flush(ctx);
   m9_retired_variants_free(ctx);
}

static void
m9_context_sync(struct m9_context *ctx)
{
   m9_context_sync_ordinary(ctx);
   if (ctx->faa_frame.count) {
      const char *why = NULL;
      if (m9_context_faa_flush_frame(ctx, &why))
         fprintf(stderr, "m9x: FAA frame failed: %s\n", why ? why : "submission failed");
   }
}

static void
m9_fill_dwords(struct m9_resource *resource, uint32_t value)
{
   uint32_t *words = resource->map;

   for (uint64_t i = 0; i < resource->size / 4; i++)
      words[i] = value;
}

/* Transfers */

static int
m9_surface_access(struct m9_context *ctx, const struct m9_resource *resource,
                  uint32_t op, const struct pipe_box *box, uint8_t *data)
{
   const struct m9_surface_desc desc = {
      .row_span = resource->row_span,
      .height = m9_surface_even_height(resource->height),
      .layers = 1, .layout = 1, .address_offset = true,
      .format = resource->format,
   };
   /* At most 64 in both dimensions the ordinary tiled descriptor addresses
    * the surface wrongly - the content comes back band structured - while
    * the small 2-D kind reproduces it exactly, which is the same kind the
    * sampler binds for that size. */
   bool small = resource->width <= 64 && resource->height <= 64;
   /* The small kind addresses its rows at the 32-aligned span as well; at
    * the texel width a 60x60 target read back sheared (MEASURED, zprobe
    * cmap at 60x60, 20x60 and 33x17). */
   uint32_t small_span = resource->row_span;
   struct drm_parhelia_m9_surface_access access = {
      .handle = resource->handle,
      .op = op,
      .descriptor0 = small ?
         M9_TEXTURE_CODE_BGRA << 24 | M9_TEXTURE_KIND_2D << 16 |
         small_span : m9_surface_desc0(&desc),
      .descriptor1 = small ?
         1u << 16 | m9_surface_even_height(resource->height) :
         m9_surface_desc1(&desc),
      .flags = small ? PARHELIA_M9_ACCESS_TEXTURE : 0,
      .surface_width = small ? small_span : resource->width,
      .x = box->x,
      .y = box->y,
      .width = box->width,
      .height = box->height,
      .data_stride = box->width * 4,
      .data_ptr = (uintptr_t)data,
      .data_size = (uint64_t)box->width * box->height * 4,
   };

   int ret = ioctl(ctx->screen->fd, DRM_IOCTL_PARHELIA_M9_SURFACE_ACCESS,
                   &access) ? -errno : 0;

   m9_blit_window_receipt(resource, &access, ret);
   return ret;
}

static bool
m9_depth_windowed(const struct m9_resource *resource)
{
   return resource->kind == M9_RESOURCE_LINEAR_DEPTH &&
          resource->format == M9_DEPTH_FORMAT_D24S8 && resource->cpp == 4 &&
          !resource->texture.levels;
}

/*
 * A D24S8 surface is stored in the render tiling, so the raw mapping shows
 * 8x4 blocks rather than rows (MEASURED, zprobe).  It is reached through the
 * window in the kind the sampler binds it with, at the depth row span, and
 * the ioctl also drains the job in flight, which a raw read never waited
 * for.  The kernel window takes the colour code, whose 32-bit
 * element is moved verbatim: depth and stencil read back exact at 1x1 to
 * 640x240 (MEASURED).
 */
static int
m9_depth_access(struct m9_context *ctx, const struct m9_resource *resource,
                uint32_t op, const struct pipe_box *box, uint8_t *data,
                unsigned stride)
{
   bool small = resource->width <= 64 && resource->height <= 64;
   uint32_t span = m9_depth_row_span(resource->width, resource->height);
   struct drm_parhelia_m9_surface_access access = {
      .handle = resource->handle,
      .op = op,
      .descriptor0 = M9_TEXTURE_CODE_BGRA << 24 |
                     (small ? M9_TEXTURE_KIND_2D : M9_TEXTURE_KIND_RENDER) << 16 |
                     span,
      .descriptor1 = 1u << 16 | m9_surface_even_height(resource->height),
      .flags = PARHELIA_M9_ACCESS_TEXTURE,
      .surface_width = span,
      .x = box->x,
      .y = box->y,
      .width = box->width,
      .height = box->height,
      .data_stride = stride,
      .data_ptr = (uintptr_t)data,
      .data_size = (uint64_t)stride * box->height,
   };
   int ret = ioctl(ctx->screen->fd, DRM_IOCTL_PARHELIA_M9_SURFACE_ACCESS,
                   &access) ? -errno : 0;

   if (ret && ctx->debug)
      fprintf(stderr, "m9x: depth window %s %ux%u box %d,%d %dx%d d0 %08x "
              "d1 %08x: %s\n", op == PARHELIA_M9_ACCESS_WRITE ? "store" :
              "fetch", resource->width, resource->height, box->x, box->y,
              box->width, box->height, access.descriptor0, access.descriptor1,
              strerror(-ret));
   return ret;
}

/*
 * MEASURED (fixture dxt1:WxH): a compressed level is stored through the
 * window at its 32-aligned span, and its height has to reach a whole
 * microtile -- 16 texel rows for the 4-bit family and 8 for the 8-bit one
 * (docs/parhelia-m9-s3tc-compressed-textures.md 12.1).  128x68 and 68x36
 * came back wrong along their last rows until the descriptor stated the
 * padded height; the rows past the level are simply never written.
 */
static uint32_t
m9_compressed_span(uint32_t width)
{
   return (width + 31) & ~31u;
}


static uint32_t
m9_compressed_lines(uint32_t height, uint32_t block_bytes)
{
   uint32_t grain = block_bytes == 8 ? 16u : 8u;

   return (height + grain - 1) & ~(grain - 1);
}

/* MEASURED: the storage kind is the one the level was written through --
 * the small 2-D kind up to 64 in both dimensions, the ordinary tiled kind
 * above it.  M9X_DXT_LEVEL_KIND forces one of the two, which is how a
 * chain's levels were checked against both. */
static enum m9_texture_kind
m9_compressed_kind(uint32_t span, uint32_t lines)
{
   int force = debug_get_num_option("M9X_DXT_LEVEL_KIND", 0);

   if (force)
      return force == 1 ? M9_TEXTURE_KIND_2D : M9_TEXTURE_KIND_RENDER;
   return span <= 64 && lines <= 64 ? M9_TEXTURE_KIND_2D :
                                      M9_TEXTURE_KIND_RENDER;
}

/*
 * A compressed level is moved through the very same window, in whole 4x4
 * blocks: the descriptor counts texels and the window's linear view is the
 * block image, which the hardware places itself (MEASURED,
 * docs/parhelia-m9-s3tc-compressed-textures.md 12.1).  Nothing is converted
 * -- the blocks travel verbatim, as section 8 requires.
 */
static int
m9_texture_window_blocks(struct m9_context *ctx,
                         const struct m9_resource *resource, unsigned level,
                         bool write, const struct pipe_box *box,
                         uint8_t *data, unsigned stride)
{
   const struct m9_texture_level *lvl = &resource->texture.level[level];
   uint32_t block = resource->texture.block_bytes;
   /* A tail level is a window into the shared 32x16 surface at its own
    * origin, not a surface of its own. */
   uint32_t span = lvl->in_tail ? M9_COMPRESSED_TAIL_WIDTH :
                                  m9_compressed_span(lvl->width);
   uint32_t lines = lvl->in_tail ? M9_COMPRESSED_TAIL_HEIGHT :
                                   m9_compressed_lines(lvl->height, block);
   enum m9_texture_kind kind = m9_compressed_kind(span, lines);
   uint32_t cols = ((uint32_t)box->width + 3) / 4;
   uint32_t rows = ((uint32_t)box->height + 3) / 4;

   size_t bytes = (size_t)cols * rows * block;
   uint8_t *image = MALLOC(bytes);
   struct drm_parhelia_m9_surface_access access = {
      .handle = resource->handle,
      .op = write ? PARHELIA_M9_ACCESS_WRITE : PARHELIA_M9_ACCESS_READ,
      .descriptor0 = (uint32_t)resource->texture_code << 24 |
                     (uint32_t)kind << 16 | span,
      .descriptor1 = 1u << 16 | lines,
      .surface_width = span,
      .x = ((uint32_t)box->x + lvl->tail_x) / 4,
      .y = ((uint32_t)box->y + lvl->tail_y) / 4,
      .width = cols,
      .height = rows,
      .data_stride = cols * block,
      .data_ptr = (uintptr_t)image,
      .data_size = bytes,
      .flags = PARHELIA_M9_ACCESS_TEXTURE,
      .offset = (uint32_t)lvl->offset,
   };
   int ret = image && cols && rows && !(box->x & 3) && !(box->y & 3) &&
             lvl->offset <= UINT32_MAX ? 0 : -EINVAL;

   /* MEASURED: a level of 64 columns or less that is too tall for the small
    * kind does not come back from the window -- 64x128 and 32x128 DXT1
    * return the wrong texels in fifteen of every sixteen -- so its content
    * cannot be stored and the level is refused rather than sampled wrong. */
   if (!ret && kind != M9_TEXTURE_KIND_2D && span <= 64)
      ret = -ENOTSUP;

   if (!ret && write)
      for (uint32_t r = 0; r < rows; r++)
         memcpy(image + (size_t)r * cols * block, data + (size_t)r * stride,
                (size_t)cols * block);
   if (!ret && write) {
      ctx->window_writes++;
      ctx->window_write_bytes += bytes;
   } else if (!ret) {
      ctx->window_reads++;
      ctx->window_read_bytes += bytes;
   }
   if (!ret) {
      ret = ioctl(ctx->screen->fd, DRM_IOCTL_PARHELIA_M9_SURFACE_ACCESS,
                  &access) ? -errno : 0;
      m9_blit_window_receipt(resource, &access, ret);
   }
   if (!ret && !write)
      for (uint32_t r = 0; r < rows; r++)
         memcpy(data + (size_t)r * stride, image + (size_t)r * cols * block,
                (size_t)cols * block);
   FREE(image);
   return ret;
}

static int
m9_texture_window(struct m9_context *ctx, const struct m9_resource *resource,
                  unsigned level, bool write, const struct pipe_box *box,
                  uint8_t *data, unsigned stride)
{
   const struct m9_texture_level *lvl = &resource->texture.level[level];
   /* Every level is allocated with a 32-aligned row span, so that span is
    * what the window descriptor must carry: a 16-wide level lives in the
    * first 16 columns of a 32-wide surface.  Passing the texel width instead
    * left narrow levels with no window shape at all and their draws were
    * dropped. */
   /* An sRGB image stores its UNORM twin's bytes; the fetch decodes. */
   enum pipe_format texel = util_format_linear(resource->base.format);
   uint32_t span = (lvl->width + 31) & ~31u;
   bool small = span <= 64 && lvl->height <= 64;
   size_t bytes = (size_t)box->width * box->height * 4;
   uint8_t *rows = MALLOC(bytes);
   struct drm_parhelia_m9_surface_access access = {
      .handle = resource->handle,
      .op = write ? PARHELIA_M9_ACCESS_WRITE : PARHELIA_M9_ACCESS_READ,
      /* The kernel window takes the small kind with both dimensions up to
       * 64, and the large kind only with a 32-aligned span. */
      /* The window places 32-bit elements alike whatever they hold, and
       * the kernel takes only colour codes for it. */
      .descriptor0 = (uint32_t)(resource->texture_code ==
                                M9_TEXTURE_CODE_DEPTH24 ? 0xc8 :
                                resource->texture_code) << 24 |
                     (small ? M9_TEXTURE_KIND_2D :
                              M9_TEXTURE_KIND_RENDER) << 16 | span,
      .descriptor1 = 1u << 16 | m9_surface_even_height(lvl->height),
      .surface_width = span,
      .x = box->x,
      .y = box->y,
      .width = box->width,
      .height = box->height,
      .data_stride = box->width * 4,
      .data_ptr = (uintptr_t)rows,
      .data_size = bytes,
      .flags = PARHELIA_M9_ACCESS_TEXTURE,
      .offset = (uint32_t)lvl->offset,
   };
   int ret = rows && lvl->offset <= UINT32_MAX ? 0 : -ENOMEM;

   if (!ret && write &&
       !util_format_translate(resource->texture_storage, rows, box->width * 4,
                              0, 0, texel, data, stride, 0, 0,
                              box->width, box->height))
      ret = -EINVAL;
   if (!ret) {
      if (write) {
         ctx->window_writes++;
         ctx->window_write_bytes += bytes;
      } else {
         ctx->window_reads++;
         ctx->window_read_bytes += bytes;
      }
   }
   if (!ret) {
      ret = ioctl(ctx->screen->fd, DRM_IOCTL_PARHELIA_M9_SURFACE_ACCESS,
                  &access) ? -errno : 0;
      m9_blit_window_receipt(resource, &access, ret);
   }
   if (ret) {
      if (ctx->debug)
         fprintf(stderr, "m9x: window %s level %u %ux%u span %u off %#" PRIx64
                 " box %d,%d %ux%u d0 %08x d1 %08x: %s\n",
                 write ? "store" : "fetch", level, lvl->width, lvl->height,
                 span, (uint64_t)lvl->offset, box->x, box->y, box->width,
                 box->height, access.descriptor0, access.descriptor1,
                 strerror(-ret));
   }
   if (!ret && !write &&
       !util_format_translate(texel, data, stride, 0, 0,
                              resource->texture_storage, rows, box->width * 4,
                              0, 0, box->width, box->height))
      ret = -EINVAL;
   FREE(rows);
   return ret;
}

/* Rows in the texture's pipe format to and from tiled texture storage. */
static int
m9_texture_access(struct m9_context *ctx, struct m9_resource *resource,
                  unsigned level, bool write, const struct pipe_box *box,
                  uint8_t *data, unsigned stride)
{
   const struct m9_texture_layout *layout = &resource->texture;
   enum pipe_format texel = util_format_linear(resource->base.format);

   if (layout->block_bytes)
      return m9_texture_window_blocks(ctx, resource, level, write, box, data,
                                      stride);
   if (!m9_texture_level_placed(layout, level))
      return m9_texture_window(ctx, resource, level, write, box, data, stride);
   return write ?
      m9_texture_store(layout, resource->map, resource->size,
                       resource->texture_storage, level, 0, box,
                       texel, data, stride) :
      m9_texture_fetch(layout, resource->map, resource->size,
                       resource->texture_storage, level, 0, box,
                       texel, data, stride);
}

static uint64_t
m9_transfer_offset(const struct m9_resource *resource, unsigned level,
                   const struct pipe_box *box, unsigned *stride)
{
   if (resource->base.target == PIPE_BUFFER) {
      *stride = 0;
      return box->x;
   }
   if (resource->kind == M9_RESOURCE_LINEAR_TEXTURE) {
      const struct m9_mip_level *mip = &resource->mip.level[level];

      if (mip->in_tail) {
         *stride = 32 * resource->cpp;
         return mip->offset +
                ((uint64_t)(mip->tail_y + box->y) * 32 +
                 mip->tail_x + box->x) * resource->cpp;
      }
      *stride = mip->row_span * resource->cpp;
      return mip->offset +
             ((uint64_t)box->y * mip->row_span + box->x) * resource->cpp;
   }
   *stride = resource->row_span * resource->cpp;
   return ((uint64_t)box->y * resource->row_span + box->x) * resource->cpp;
}

static bool
m9_transfer_sync(struct m9_context *ctx, const struct m9_resource *resource,
                  unsigned usage)
{
   if (usage & PIPE_MAP_UNSYNCHRONIZED)
      return true;
   if (resource->base.target == PIPE_BUFFER && !resource->handle) {
      /* Gallium buffers are CPU allocations. Native draws have already
       * copied their inputs into job-owned records, indices and constants.
       * Consume any draw-module CPU reads before the application can write,
       * but keep those independent GPU jobs batched and in flight. */
      if (ctx->draw)
         draw_flush(ctx->draw);
   } else if (resource->base.target != PIPE_BUFFER ||
              ctx->screen->queued_submit) {
      m9_context_sync(ctx);
   }
   if (ctx->screen->queued_submit) {
      if (ctx->lost)
         return false;
      if (!(resource->base.target == PIPE_BUFFER && !resource->handle) &&
          m9_wait_for_point(ctx->screen, ctx->last_point)) {
         ctx->lost = true;
         return false;
      }
   }
   return true;
}

static void *
m9_transfer_map_locked(struct pipe_context *pctx, struct pipe_resource *pres,
                       unsigned level, unsigned usage,
                       const struct pipe_box *box, struct pipe_transfer **out)
{
   struct m9_context *ctx = m9_context(pctx);
   struct m9_resource *resource = (struct m9_resource *)pres;
   struct m9_transfer *transfer;
   uint64_t offset = 0, last;
   unsigned width, height, stride;
   /* Layers of this level: cube faces, volume slices, one for a plane. */
   unsigned layers = resource && level < resource->texture.levels ?
                     resource->texture.faces *
                     resource->texture.level[level].depth : 1;

   if (!resource || !resource->map ||
       ((usage & PIPE_MAP_WRITE) && !resource->map_writable) ||
       !box || !out || box->x < 0 ||
       box->y < 0 || box->z < 0 || box->width < 0 || box->height < 0 ||
       box->depth < 1 || (unsigned)box->z + box->depth > layers ||
       level > pres->last_level) {
      if (ctx->debug && resource && box)
         fprintf(stderr, "m9x: map refused: %s %ux%u target %u level %u/%u "
                 "box %d,%d,%d %dx%dx%d usage %#x map %p writable %d kind %u\n",
                 util_format_short_name(pres->format), pres->width0,
                 pres->height0, pres->target, level, pres->last_level,
                 box->x, box->y, box->z, box->width, box->height, box->depth,
                 usage, resource->map, resource->map_writable, resource->kind);
      return NULL;
   }
   width = pres->target == PIPE_BUFFER ? pres->width0 :
           (pres->width0 >> level ? pres->width0 >> level : 1);
   height = pres->target == PIPE_BUFFER ? 1 :
            (pres->height0 >> level ? pres->height0 >> level : 1);
   if ((unsigned)box->x > width || (unsigned)box->width > width - box->x ||
       (unsigned)box->y > height || (unsigned)box->height > height - box->y)
      return NULL;
   if (!m9_transfer_sync(ctx, resource, usage))
      return NULL;
   transfer = CALLOC_STRUCT(m9_transfer);
   if (!transfer)
      return NULL;
   if (resource->texture.levels) {
      ctx->map_usage_seen |= usage;
      /* Storage is read and written through the box alone, so a write-only
       * map never observes the old content: the read back is dead work. */
      bool preserve = (usage & PIPE_MAP_READ) ||
                      (!(usage & (PIPE_MAP_DISCARD_RANGE |
                                  PIPE_MAP_DISCARD_WHOLE_RESOURCE)) &&
                       !(usage & PIPE_MAP_WRITE));

      /* A compressed box is in texels and its rows are block rows. */
      stride = util_format_get_stride(pres->format, box->width);
      transfer->staging = box->width && box->height ?
                          MALLOC((size_t)stride *
                                 util_format_get_nblocksy(pres->format,
                                                          box->height) *
                                 box->depth) :
                          NULL;
      if (preserve) {
         ctx->window_read_usage |= usage;
         ctx->window_reads_explicit += !!(usage & PIPE_MAP_READ);
      }
      if (!transfer->staging ||
          (preserve && m9_texture_access(ctx, resource, level, false, box,
                                         transfer->staging, stride))) {
         m9_warn_once(ctx, M9_WARN_TRANSFER, "texture storage read failed");
         FREE(transfer->staging);
         FREE(transfer);
         return NULL;
      }
   } else if (resource->tiled || m9_depth_windowed(resource)) {
      bool preserve = (usage & PIPE_MAP_READ) ||
                      (!(usage & (PIPE_MAP_DISCARD_RANGE |
                                  PIPE_MAP_DISCARD_WHOLE_RESOURCE)) &&
                       !(usage & PIPE_MAP_WRITE));

      /* Tiled storage is only reachable as rows through the kernel. */
      stride = box->width * 4;
      transfer->staging = box->width && box->height ?
                          MALLOC((size_t)stride * box->height) : NULL;
      if (!transfer->staging ||
          (preserve && (resource->tiled ?
                        m9_surface_access(ctx, resource,
                                          PARHELIA_M9_ACCESS_READ, box,
                                          transfer->staging) :
                        m9_depth_access(ctx, resource,
                                        PARHELIA_M9_ACCESS_READ, box,
                                        transfer->staging, stride)))) {
         m9_warn_once(ctx, M9_WARN_TRANSFER,
                      "de-tiled render target access failed");
         FREE(transfer->staging);
         FREE(transfer);
         return NULL;
      }
   } else {
      offset = m9_transfer_offset(resource, level, box, &stride);
      last = offset;
      if (pres->target == PIPE_BUFFER)
         last += box->width;
      else if (box->width && box->height)
         last += (uint64_t)(box->height - 1) * stride +
                 (uint64_t)box->width * resource->cpp;
      if (last > resource->size) {
         if (ctx->debug)
            fprintf(stderr, "m9x: map beyond BO: %s %ux%u level %u box %d,%d "
                    "%dx%d offset %#" PRIx64 " last %#" PRIx64 " size %#"
                    PRIx64 " kind %u\n", util_format_short_name(pres->format),
                    pres->width0, pres->height0, level, box->x, box->y,
                    box->width, box->height, offset, last, resource->size,
                    resource->kind);
         FREE(transfer);
         return NULL;
      }
   }
   pipe_resource_reference(&transfer->base.resource, pres);
   transfer->base.level = level;
   transfer->base.usage = usage;
   transfer->base.box = *box;
   transfer->base.stride = stride;
   transfer->base.layer_stride = pres->target == PIPE_BUFFER ? 0 :
                                 (uintptr_t)stride * box->height;
   *out = &transfer->base;
   return transfer->staging ? transfer->staging :
          (uint8_t *)resource->map + offset;
}

static void *
m9_transfer_map(struct pipe_context *pctx, struct pipe_resource *pres,
                unsigned level, unsigned usage, const struct pipe_box *box,
                struct pipe_transfer **out)
{
   uint64_t t_transfer = m9_prof_begin();
   /* The proxy of a bound level holds what was rendered there. */
   if (pres == m9_context(pctx)->proxy.tex)
      m9_proxy_writeback(m9_context(pctx));
   void *map = m9_transfer_map_locked(pctx, pres, level, usage, box, out);

   if (!map && m9_context(pctx)->debug)
      fprintf(stderr, "m9x: transfer map failed: %s %ux%ux%u target %u bind "
              "%#x level %u usage %#x box %d,%d,%d %dx%dx%d\n",
              util_format_short_name(pres->format), pres->width0,
              pres->height0, pres->depth0, pres->target, pres->bind, level,
              usage, box->x, box->y, box->z, box->width, box->height,
              box->depth);

   /* A writable mapping moves the contents on, both when it is handed out
    * and when the writer gives it back: an entry built while the mapping was
    * open is retired by the second bump. */
   if (map && (usage & PIPE_MAP_WRITE))
      m9_res(pres)->generation++;
   m9_prof_end(M9_PROF_TRANSFER, t_transfer);
   return map;
}

static void
m9_transfer_unmap(struct pipe_context *pctx, struct pipe_transfer *ptransfer)
{
   struct m9_transfer *transfer = (struct m9_transfer *)ptransfer;

   if (ptransfer->usage & PIPE_MAP_WRITE)
      m9_res(ptransfer->resource)->generation++;
   if (transfer->staging && m9_res(ptransfer->resource)->texture.levels) {
      struct m9_resource *resource = m9_res(ptransfer->resource);
      uint32_t bit = 1u << ptransfer->level;

      if (ptransfer->usage & PIPE_MAP_WRITE) {
         int store = m9_texture_access(m9_context(pctx), resource,
                                       ptransfer->level, true, &ptransfer->box,
                                       transfer->staging, ptransfer->stride);

         /* M9X_TEXTURE_DUMP=dir writes each uploaded level 0 as a PPM, so
          * the content a surface samples can be looked at directly. */
         if (m9_texture_dump && !ptransfer->level) {
            const uint8_t *p = transfer->staging;
            char path[512];
            FILE *f;

            snprintf(path, sizeof(path), "%s/tex%03u_%ux%u.ppm",
                     m9_texture_dump, resource->handle, ptransfer->box.width,
                     ptransfer->box.height);
            f = fopen(path, "wb");
            if (f) {
               fprintf(f, "P6\n%u %u\n255\n", ptransfer->box.width,
                       ptransfer->box.height);
               for (unsigned y = 0; y < ptransfer->box.height; y++)
                  for (unsigned x = 0; x < ptransfer->box.width; x++) {
                     const uint8_t *t = p + (size_t)y * ptransfer->stride +
                                        (size_t)x * 4;

                     fwrite((uint8_t[]){ t[0], t[1], t[2] }, 1, 3, f);
                  }
               fclose(f);
            }
         }

         if (m9_debug_textures)
            fprintf(stderr, "m9x: store texture %u %s level %u box %d,%d,%d "
                    "%dx%dx%d: %d\n", resource->handle,
                    util_format_short_name(resource->base.format),
                    ptransfer->level, ptransfer->box.x, ptransfer->box.y,
                    ptransfer->box.z, ptransfer->box.width,
                    ptransfer->box.height, ptransfer->box.depth, store);
         /* M9X_TEXTURE_ADDRCODE=1 overwrites the whole object with each
          * dword's own index, so a sampled colour names the address the
          * sampler read (a placement probe, not a rendering mode). */
         if (m9_texture_addrcode && !store) {
            uint32_t *w = resource->map;

            for (uint64_t i = 0; i < resource->size / 4; i++)
               w[i] = (uint32_t)(i & 0xffffff) | 0xff000000u;
         }
         if (store) {
            resource->texture_unplaced |= bit;
            if (!(m9_context(pctx)->warned & M9_WARN_TRANSFER))
               fprintf(stderr, "m9x: texture %u %ux%u %s level %u (%ux%u) "
                       "cannot be stored (%d); it will not be sampled\n",
                       resource->handle, resource->width, resource->height,
                       util_format_short_name(resource->base.format),
                       ptransfer->level, resource->texture.level[ptransfer->level].width,
                       resource->texture.level[ptransfer->level].height, store);
            m9_warn_once(m9_context(pctx), M9_WARN_TRANSFER,
                         "texture level cannot be stored: no captured "
                         "placement and no kernel texture window");
         } else {
            resource->texture_unplaced &= ~bit;
            resource->levels_written |= bit;
         }
      }
      FREE(transfer->staging);
   } else if (transfer->staging && !m9_res(ptransfer->resource)->tiled) {
      struct m9_resource *zs = m9_res(ptransfer->resource);

      if (ptransfer->usage & PIPE_MAP_WRITE) {
         if (m9_depth_access(m9_context(pctx), zs, PARHELIA_M9_ACCESS_WRITE,
                             &ptransfer->box, transfer->staging,
                             ptransfer->stride))
            m9_warn_once(m9_context(pctx), M9_WARN_TRANSFER,
                         "depth/stencil window write failed");
         /* The vendor resets the hierarchical plane after a CPU depth write
          * (docs/parhelia-m9-hierarchical-z-static-closure.md); the window
          * write drained the engine, so nothing reads it meanwhile. */
         if (zs->hz)
            m9_fill_dwords(zs->hz, M9_HZ_CLEAR);
      }
      FREE(transfer->staging);
   } else if (transfer->staging) {
      if ((ptransfer->usage & PIPE_MAP_WRITE) &&
          m9_surface_access(m9_context(pctx), m9_res(ptransfer->resource),
                            PARHELIA_M9_ACCESS_WRITE, &ptransfer->box,
                            transfer->staging))
         m9_warn_once(m9_context(pctx), M9_WARN_TRANSFER,
                      "de-tiled render target write failed");
      FREE(transfer->staging);
   } else if (ptransfer->usage & PIPE_MAP_WRITE) {
      __sync_synchronize();
   }
   if ((ptransfer->usage & PIPE_MAP_WRITE) &&
       ptransfer->resource == m9_context(pctx)->proxy.tex)
      m9_proxy_reseed(m9_context(pctx));
   pipe_resource_reference(&ptransfer->resource, NULL);
   FREE(transfer);
}

/* Two-pass jobs */

/* No point exists for a deferred job. This marker can never be sent to
 * WAIT_POINT: flush first, then replace every held arena with the real point. */
#define M9_AGGREGATE_PENDING UINT64_MAX

static int
m9_aggregate_flush(struct m9_context *ctx)
{
   struct m9_batch *b = ctx->aggregate;
   uint64_t point = 0;
   int ret = 0;
   if (!b || !b->aggregate_jobs) return 0;
   unsigned jobs = b->aggregate_jobs;
   b->sealed = true;
   ret = ctx->lost ? -EIO :
      m9_batch_submit(ctx->screen, &ctx->command, b, ctx->context_id, &point);
   if (ret) {
      /* A posted failure is quarantined by the kernel. Never rebuild or
       * overwrite a deferred arena after an unproved completion. */
      ctx->lost = true;
   } else {
      ctx->last_point = point;
      if (ctx->arena_point == M9_AGGREGATE_PENDING) ctx->arena_point = point;
      if (ctx->arena_alt_point == M9_AGGREGATE_PENDING) ctx->arena_alt_point = point;
      for (unsigned i=0;i<ARRAY_SIZE(ctx->queue_arena);i++)
         if (ctx->queue_arena[i].point == M9_AGGREGATE_PENDING)
            ctx->queue_arena[i].point = point;
      for (unsigned i=0;i<jobs;i++) ctx->aggregate_be[i]->be_point = point;
      if (ctx->vs_record_vertices && !ctx->vs_record_point) ctx->vs_record_point = point;
   }
   /* Success transferred GEM refs/pins to the real kernel fence; failure
    * loses this context, and any posted readers remain kernel-owned. */
   util_dynarray_foreach(&ctx->aggregate_resources, struct pipe_resource *, held)
      pipe_resource_reference(held, NULL);
   util_dynarray_clear(&ctx->aggregate_resources);
   m9_batch_init(b);
   return ret;
}

static int
m9_aggregate_enqueue(struct m9_context *ctx, struct m9_resource *be)
{
   size_t old_size = ctx->aggregate_resources.size;
   void *held = NULL;
   if (ctx->job_textures.size) {
      held = util_dynarray_grow_bytes(&ctx->aggregate_resources, 1, ctx->job_textures.size);
      if (!held) return -ENOMEM;
   }
   int ret = m9_batch_aggregate_append(ctx->aggregate, &ctx->batch);
   if (ret) {
      ctx->aggregate_resources.size = old_size;
      if (ret == -ENOSPC && ctx->aggregate->aggregate_jobs) {
         ret = m9_aggregate_flush(ctx);
         if (!ret) return m9_aggregate_enqueue(ctx, be);
      }
      return ret;
   }
   if (held) memcpy(held,ctx->job_textures.data,ctx->job_textures.size);
   util_dynarray_clear(&ctx->job_textures); /* transfer existing references */
   ctx->aggregate_be[ctx->aggregate->aggregate_jobs-1] = be;
   ctx->arena_point = M9_AGGREGATE_PENDING;
   be->be_point = M9_AGGREGATE_PENDING;
   return 0;
}

/* Swap in the other set of per-job arenas; see m9_context's declaration. */
static int
m9_arena_rotate(struct m9_context *ctx)
{
   struct m9_resource tmp;
   struct m9_job_arena *bank = NULL;
   uint64_t *parked_point = &ctx->arena_alt_point;
   uint64_t point;
   int ret;

   if (ctx->screen->queued_submit) {
      unsigned slot = ctx->arena_cursor++ % 4;

      if (slot) {
         bank = &ctx->queue_arena[slot - 1];
         parked_point = &bank->point;
      }
      if (*parked_point == M9_AGGREGATE_PENDING && m9_aggregate_flush(ctx))
         return -EIO;
      ret = m9_wait_for_point(ctx->screen, *parked_point);
      if (ret) {
         ctx->lost = true;
         return ret;
      }
   }

#define ARENA_SWAP(a, b) do { tmp = (a); (a) = (b); (b) = tmp; } while (0)
   if (bank) {
      ARENA_SWAP(ctx->source, bank->source);
      ARENA_SWAP(ctx->system, bank->system);
      ARENA_SWAP(ctx->index, bank->index);
      ARENA_SWAP(ctx->cpu_streams, bank->cpu_streams);
   } else {
      ARENA_SWAP(ctx->source, ctx->source_alt);
      ARENA_SWAP(ctx->system, ctx->system_alt);
      ARENA_SWAP(ctx->index, ctx->index_alt);
      ARENA_SWAP(ctx->cpu_streams, ctx->cpu_streams_alt);
   }
#undef ARENA_SWAP
   point = ctx->arena_point;
   ctx->arena_point = *parked_point;
   *parked_point = point;
   return 0;
}

static int
m9_arena_reserve(struct m9_context *ctx, struct m9_resource *arena,
                 uint64_t bytes)
{
   uint64_t size;

   if (bytes <= arena->size)
      return 0;
   size = MAX2((bytes + 0xfff) & ~0xfffull, 2 * arena->size);
   m9_bo_destroy(ctx->screen, arena);
   return m9_bo_create(ctx->screen, size, 0, M9_RESOURCE_LINEAR_BUFFER,
                       arena);
}

static bool
m9_varying_is_color(unsigned location)
{
   return location == VARYING_SLOT_COL0 || location == VARYING_SLOT_COL1;
}

/* The transient view a varying the preprocessing stage writes arrives on,
 * or -1 when nothing writes it and the CPU has to.  Transient view 0 is the
 * fused pass's output target 0 -- the captured pass-through program puts
 * the primary colour there, a native program its first varying -- and the
 * views above M9_TRANSIENT_VS_EXTRA_VIEW take the rest: varying 1 from the
 * fused pass's own target 3, varyings 2..4 from the second preprocessing
 * draw's targets 0..2. */
static int
m9_pass_varying_view(const struct m9_job_key *key, unsigned location,
                     unsigned pass, unsigned coord, bool coordinate)
{
   int index;

   if (!key->vs)
      return location == VARYING_SLOT_COL0 ? 0 : -1;
   index = m9_vs_stream_index(key->vs, location, pass, coord, coordinate);
   return index <= 0 ? index :
          (int)M9_TRANSIENT_VS_EXTRA_VIEW + index - 1;
}

/* The per-fragment record a split shader hands its values through.  Its
 * granularity is not decoded, so m9_record_bytes covers one record per
 * covered pixel rounded out to whole 64-pixel blocks; the object is grown
 * rather than reallocated per job.
 *
 * The object's shape is not what stops the engine on a split draw that
 * follows one into a target of another size: with a record allocated for
 * that draw's own target the fixture stalls just the same (MEASURED,
 * deskblurt:3).  M9X_DEBUG_SPLIT traces what each split draw gets. */
static bool
m9_record_grow(struct m9_context *ctx, unsigned dwords, const char **reason)
{
   struct m9_screen *screen = ctx->screen;
   const bool trace = debug_get_bool_option("M9X_DEBUG_SPLIT", false);
   uint64_t bytes;
   unsigned want;
   bool ok = true;

   /* M9X_RECORD_FRESH=1 gives every split job an object of its own, which
    * separates the record from the rest of the per-target state. */
   const bool fresh = debug_get_bool_option("M9X_RECORD_FRESH", false);

   simple_mtx_lock(&screen->record_lock);
   want = MAX2(dwords, screen->record_dwords);
   bytes = m9_record_bytes(ctx->params.width, ctx->params.height, want);
   if (!bytes) {
      *reason = "the inter-pass record has no size";
      ok = false;
   } else if (fresh || !screen->record.handle || screen->record.size < bytes ||
              screen->record_dwords < dwords) {
      bytes = MAX2(bytes, screen->record.size);
      /* The old object goes first: two records of this size do not fit
       * beside each other. */
      m9_bo_destroy(screen, &screen->record);
      if (m9_bo_create_engine_only(screen, bytes, M9_RESOURCE_LINEAR_COLOR,
                                   &screen->record)) {
         /* Name the size: a record is tens of MiB and which stride asked
          * for it is the difference between a driver fault and the
          * hardware's own cost. */
         fprintf(stderr, "m9x: inter-pass record of %u MiB for %ux%u stride "
                 "%u refused\n", (unsigned)(bytes >> 20),
                 ctx->params.width, ctx->params.height, want);
         *reason = "cannot allocate the inter-pass record";
         screen->record_dwords = 0;
         ok = false;
      } else {
         screen->record_dwords = want;
         if (trace)
            fprintf(stderr, "m9x record: %ux%u stride %u -> %u MiB, new "
                    "object\n", ctx->params.width, ctx->params.height, want,
                    (unsigned)(bytes >> 20));
      }
   } else if (trace) {
      fprintf(stderr, "m9x record: %ux%u stride %u wants %u MiB, kept %u "
              "MiB\n", ctx->params.width, ctx->params.height, want,
              (unsigned)(bytes >> 20), (unsigned)(screen->record.size >> 20));
   }
   simple_mtx_unlock(&screen->record_lock);
   return ok;
}

/*
 * The multisample fragment store.  Its size is the vendor's own (STATIC,
 * MxMD.dll 0xac7e0): about 16.55 bytes a pixel capped at 32 MiB, and a
 * second buffer a sixteenth of that.  It is allocated only while the probe
 * requests a store or companion, and grows with the largest target seen.
 */
static bool
m9_faa_store_grow_locked(struct m9_context *ctx)
{
   struct m9_screen *screen = ctx->screen;
   uint32_t width, height;
   uint64_t bytes, aux, companion_bytes;
   struct m9_resource store = {0}, store_aux = {0}, companion = {0};
   bool ok = true;

   simple_mtx_assert_locked(&screen->faa_store_lock);
   if (screen->faa.bind_index < 0 && !screen->faa.store_bind &&
       screen->faa.companion_format < 0)
      return true;
   width = MAX2(ctx->params.width, screen->faa_store_width);
   height = MAX2(ctx->params.height, screen->faa_store_height);
   bytes = (m9_faa_fragment_bytes(width, height) + 127) & ~127ull;
   aux = (m9_faa_fragment_aux_bytes(width, height) + 127) & ~127ull;
   companion_bytes = (uint64_t)width * height * 8;
   if (screen->faa.companion_format == 0x3d) {
      struct m9_faa_companion_layout layout;

      if (!m9_faa_companion_layout(width, height, &layout))
         ok = false;
      else
         companion_bytes = layout.size;
   }
   if (!bytes || !aux) {
      ok = false;
   } else if (ok && (!screen->faa_store.handle || screen->faa_store.size < bytes ||
                     !screen->faa_store_aux.handle || screen->faa_store_aux.size < aux)) {
      /* A checked store has to be readable, so it goes in the mapped
       * window rather than the engine-only video memory above it. */
      if ((screen->faa.store_check ?
           m9_bo_create(screen, bytes, PARHELIA_M9_BO_VRAM,
                        M9_RESOURCE_LINEAR_BUFFER, &store) :
           m9_bo_create_engine_only(screen, bytes, M9_RESOURCE_LINEAR_BUFFER,
                                    &store)) ||
          m9_bo_create_engine_only(screen, aux, M9_RESOURCE_LINEAR_BUFFER,
                                   &store_aux)) {
         fprintf(stderr, "m9x: multisample fragment store of %u KiB for "
                 "%ux%u refused\n", (unsigned)(bytes >> 10), width, height);
         ok = false;
      }
   }
   /* At the fragment-store cap, the target and companion can still grow.
    * A failed companion allocation can also need a retry with no store
    * growth. Test its ownership independently of the store's size. */
   if (ok && screen->faa.companion_format >= 0 &&
       (!screen->faa_companion.handle ||
        screen->faa_companion.size < companion_bytes)) {
      if (m9_bo_create_engine_only(screen, companion_bytes,
                                  M9_RESOURCE_LINEAR_DEPTH, &companion)) {
         fprintf(stderr, "m9x: multisample companion for %ux%u refused\n",
                 width, height);
         ok = false;
      }
   }
   /* Publish the complete requested group only after every allocation
    * succeeds. Failure preserves the previous usable group and its extent;
    * a later draw can retry without dangling or partially replaced handles. */
   if (ok) {
      if (store.handle) {
         if (screen->faa.store_check && store.map) {
            uint32_t *words = store.map;

            for (uint64_t k = 0; k < store.size / 4; k++)
               words[k] = m9_faa_store_pattern(k);
         }
         m9_bo_destroy(screen, &screen->faa_store);
         m9_bo_destroy(screen, &screen->faa_store_aux);
         screen->faa_store = store;
         screen->faa_store_aux = store_aux;
         screen->faa_store_point = 0;
         screen->faa_store_dirty = true;
         screen->faa_store_failed = false;
         store = (struct m9_resource){0};
         store_aux = (struct m9_resource){0};
         if (screen->faa.trace)
            fprintf(stderr, "m9x: faa store %ux%u -> %u + %u bytes\n",
                    width, height, (unsigned)bytes, (unsigned)aux);
      }
      if (companion.handle) {
         m9_bo_destroy(screen, &screen->faa_companion);
         screen->faa_companion = companion;
         companion = (struct m9_resource){0};
         if (screen->faa.trace)
            fprintf(stderr, "m9x: faa companion %ux%u -> %llu bytes handle %u\n",
                    width, height,
                    (unsigned long long)screen->faa_companion.size,
                    screen->faa_companion.handle);
      }
      screen->faa_store_width = width;
      screen->faa_store_height = height;
   }
   m9_bo_destroy(screen, &store);
   m9_bo_destroy(screen, &store_aux);
   m9_bo_destroy(screen, &companion);
   return ok;
}

/* The caller holds the store lock until every submission has looked up the
 * handles. Bind the retained group and its logical descriptor extent together.
 * The GEM's page padding and the unrelated color allocation do not determine AD.
 * Explicit AD overrides retain their diagnostic value, but still need every
 * requested store owner and its logical capacity. */
static bool
m9_faa_store_bind_job_locked(struct m9_context *ctx, struct m9_draw_job *job)
{
   struct m9_screen *screen = ctx->screen;
   bool ok = true;

   simple_mtx_assert_locked(&screen->faa_store_lock);
   job->faa.store_bind = screen->faa.store_bind;
   job->faa.store = screen->faa_store.handle;
   job->faa.store_aux = screen->faa_store_aux.handle;
   if (job->faa.store_bind) {
      uint64_t main = (m9_faa_fragment_bytes(screen->faa_store_width,
                                             screen->faa_store_height) + 127) &
                       ~127ull;
      uint64_t auxiliary = (m9_faa_fragment_aux_bytes(screen->faa_store_width,
                                                       screen->faa_store_height) +
                            127) & ~127ull;

      if (!main || !auxiliary || !job->faa.store || !job->faa.store_aux ||
          main > screen->faa_store.size ||
          auxiliary > screen->faa_store_aux.size)
         ok = false;
      else if (!screen->faa.ad_forced)
         ok = m9_faa_store_ad(0, main, &job->faa.ad);
   }
   return ok;
}

/* Called under the owner lock after every real submission attempt, including
 * split passes. A failure may already have posted work without returning its
 * final point: never infer CPU access from an older successful submission. */
static void
m9_faa_store_submitted_locked(struct m9_screen *screen,
                             const struct m9_draw_job *job,
                             int ret, uint64_t point)
{
   simple_mtx_assert_locked(&screen->faa_store_lock);
   if (!job->faa.store || (!job->faa.store_bind && job->faa.bind_index < 0))
      return;
   if (ret || !point || job->faa.store != screen->faa_store.handle) {
      screen->faa_store_failed = true;
      return;
   }
   screen->faa_store_point = MAX2(screen->faa_store_point, point);
   screen->faa_store_dirty = true;
}

/* Raster constants, CPU varying views and per-pass state of a native
 * fragment job.  A shader the engine runs in one pass produces one entry,
 * which the emitter drives through its single-pass path unchanged. */
static int
m9_job_native(struct m9_context *ctx, const struct m9_job_key *key,
              unsigned records, unsigned index_bytes,
              struct m9_raster_pass *out)
{
   const struct m9_fs_variant *fs = key->variant;
   const float *extra = ctx->extra.data;
   unsigned cpu_streams = key->vs ? 0 : fs->extra_count;
   uint64_t const_offset = M9_RASTER_CONST_OFFSET;
   uint64_t t_varying;
   int ret;

   /* The spans of the passes lie consecutively in one arena, as the
    * captured two-pass and five-sampler schedules lay them out. */
   memcpy((uint8_t *)ctx->system.map + M9_RASTER_CONST_OFFSET,
          ctx->job_consts.data, ctx->job_consts.size);
   /* Each pass draws its own copy of the geometry, one record range after
    * another, so its record index is the first copy's plus i * records.  The
    * gather is written once at the tail of the view and each pass's base is
    * pulled back by its own copy's offset, which lands every pass on that one
    * copy: what used to be passes copies of the varyings is now one. */
   uint64_t gather_at = (uint64_t)(fs->passes.count - 1) * records *
                        M9_TRANSIENT_VERTEX_BYTES;

   t_varying = m9_prof_begin();
   for (unsigned j = 0; j < cpu_streams; j++) {
      uint8_t *view = (uint8_t *)ctx->cpu_streams.map +
                      j * m9_transient_view_bytes() + gather_at;

      for (unsigned r = 0; r < records; r++)
         memcpy(view + r * M9_TRANSIENT_VERTEX_BYTES,
                extra + (r * fs->extra_count + j) * 4, 4 * sizeof(float));
   }
   m9_prof_varying_bytes += (uint64_t)records * cpu_streams *
                            M9_TRANSIENT_VERTEX_BYTES;
   m9_prof_end(M9_PROF_VARYING, t_varying);

   for (unsigned i = 0; i < fs->passes.count; i++) {
      const struct m9_fs_program *p = &fs->passes.pass[i];
      const struct m9_fs_pass_streams *st = &fs->stream[i];
      uint32_t views[M9_FS_MAX_VARYINGS], coords[M9_FS_MAX_TEXTURES];
      uint32_t coord_select[M9_FS_MAX_TEXTURES] = { 0 };
      bool flat[M9_FS_MAX_VARYINGS];
      unsigned consts = util_dynarray_num_elements(&p->consts,
                                                   struct m9_fs_const);
      struct m9_raster_pass *pass = &out[i];

      memset(pass, 0, sizeof(*pass));
      /* A varying the preprocessing pass itself writes comes from stream
       * view 0; everything else the CPU gathers into a view of its own. */
      for (unsigned k = 0; k < p->varying_count; k++) {
         int view = m9_pass_varying_view(key, p->varyings[k].location, i, 0,
                                         false);

         flat[k] = m9_varying_is_color(p->varyings[k].location);
         views[k] = st->varying_extra[k] < 0 || view >= 0 ?
                    MAX2(view, 0) : M9_TRANSIENT_CPU_VIEW +
                                    st->varying_extra[k];
      }
      for (unsigned c = 0; c < st->coord_count; c++) {
         int view = m9_pass_varying_view(key, st->coord_location[c], i, c,
                                         true);
         int shared = view >= 0 ? -1 :
                      m9_pass_varying_view(key, st->coord_location[c], i, 0,
                                           false);

         /* A coordinate the native program wrote no stream for rides the
          * varying's own stream, read through the declaration's component
          * selectors instead of an output target of its own. */
         if (key->vs && view < 0 && shared >= 0) {
            coords[c] = shared;
            coord_select[c] = m9_coord_select(st->coord_component[c]);
            continue;
         }
         coords[c] = st->coord_extra[c] < 0 || view >= 0 ?
                     MAX2(view, 0) : M9_TRANSIENT_CPU_VIEW +
                                     st->coord_extra[c];
         if (key->vs && view >= 0) {
            int index = m9_vs_stream_index(key->vs, st->coord_location[c],
                                            i, c, true);
            const struct m9_vs_output *o = index >= 0 ?
                                           &key->vs->outputs.stream[index] : NULL;

            if (o && o->paired && o->pair_pass == i && o->pair_coord == c)
               coord_select[c] = m9_coord_select(2);
         }
      }
      ret = m9_raster_fs_init_textured(&pass->fs, fs->vertex_entry,
                                       m9_fs_raster_88(p), consts,
                                       p->varying_count, flat, views,
                                       st->coord_count, coords, coord_select,
                                       p->texture_count, st->texture_coord,
                                       !key->vs_fused_pair);
      if (ret)
         return ret;
      pass->entry = fs->entry[i];
      pass->vertex_offset = i * records;
      pass->index_offset = i * index_bytes;
      pass->cpu_stream_offset = gather_at -
                                (uint64_t)i * records *
                                M9_TRANSIENT_VERTEX_BYTES;
      pass->const_offset = const_offset;
      const_offset += 8ull * consts;
      pass->writes_record = p->writes_record && m9_split_stage != 1;
      /* Every producer owns a four-dword slot of the record, so a pass
       * reads the slots of the passes before it while writing its own. */
      /* Stage 4 binds the record object with an ordinary colour
       * descriptor, which separates the 0x77 record format from the
       * allocation it names. */
      pass->record_desc = !pass->writes_record ? 0 :
                          m9_split_stage == 4 ||
                          m9_split_stage == 7 ? ctx->params.color_desc :
                          m9_split_stage == 5 ?
                          m9_record_desc(M9_RECORD_PAIR, 2, 0) :
                          m9_split_stage == 6 ?
                          m9_record_desc(M9_RECORD_SLOT, fs->record_dwords,
                                         fs->record_offset[i]) |
                          (ctx->params.color_desc & 0xffffu) :
                          m9_record_desc(M9_RECORD_SLOT, fs->record_dwords,
                                         fs->record_offset[i]);
      /* Only the first pass of a split sequence drives the depth unit.
       * Leaving the test on in a continuation pass stops the engine
       * (MEASURED: conv3nowrited and conv3d both time out, conv3notestd
       * renders), and the captured schedules restate a8 with the write bit
       * cleared after the first pass, so the sequence evaluates depth once
       * and the passes behind it inherit that coverage.  M9X_SPLIT_DEPTH
       * selects the schedule: 1 the first pass only, 0 no depth at all,
       * 2 the schedule that stops the engine. */
      bool pass_depth = m9_split_depth == 0 ? fs->passes.count < 2 : !i;

      pass->depth_control = pass_depth ? key->depth_control :
                            m9_depth_control(false, false, PIPE_FUNC_LESS);
      pass->depth_ef = pass_depth ? key->depth_ef : 0;
      if (m9_split_depth == 2 && i) {
         pass->depth_control = key->depth_control & ~0x2u;
         pass->depth_ef = key->depth_ef & ~0x8000u;
      }
      /* Bits 10:9 of ef place a raster pass in its sequence: 1 the first of
       * several, 2 a middle one, 3 the last.  Every single-pass draw leaves
       * them clear (CAPTURED, 0x200/0x600 in all seven two-pass cases and
       * 0x200/0x400/0x400/0x400/0x600 in the five-sampler schedule). */
      pass->sequence_ef = fs->passes.count < 2 || !m9_split_seq ? 0 :
                          !i ? 0x200u :
                          i + 1 < fs->passes.count ? 0x400u : 0x600u;
      pass->pass_index = i;
      /* Index 0x89 holds one three-bit render format class per bound target
       * at bit 3*s: class 4 for the record and 0 for a colour target
       * (CAPTURED, fe007ffc against fe007ff8 in both schedules). */
      /* Stage 7 keeps the colour pass's own state words, so only the
       * object bound at b0 separates it from an ordinary draw. */
      bool record_state = pass->writes_record && m9_split_stage != 7;

      pass->r89 = m9_record_89 && pass->writes_record ? m9_record_89 :
                  record_state ? 0xfe007ffcu : 0xfe207ff8u;
      /* A program writing gl_FragDepth sets the fifth class field to 1 and
       * takes the depth unit from 0x7000 to 0x6800 with test and write on
       * (CAPTURED, fs-fragdepth: 89 fe001ff8, ef 6800).  M9X_FRAGDEPTH=0
       * leaves both as for interpolated depth, for measurement. */
      if (p->fragdepth && m9_fragdepth_state) {
         pass->r89 = (pass->r89 & ~0x7000u) | 0x1000u;
         pass->fs.r89 = pass->r89;
         if (pass->depth_ef)
            pass->depth_ef = (pass->depth_ef & ~0x9000u) | 0x0800u;
      }
      /* The stencil test runs with the depth test, in the first pass. */
      pass->a9 = (pass_depth && key->stencil ? key->stencil_a9 :
                                               M9_STENCIL_A9_OFF) |
                 (record_state && m9_split_coverage ? 0x80000000u : 0);
      pass->aa = pass_depth && key->stencil ? key->stencil_aa :
                                              M9_STENCIL_AA_OFF;
      pass->ae = record_state && m9_split_coverage ? 0x32444442u :
                                                     0x22444442u;
      pass->unit_count = p->texture_count;
      for (unsigned k = 0; k < M9_TEXTURE_MAX_UNITS; k++)
         pass->routes[k].word = 0x100;
      for (unsigned k = 0; k < p->texture_count; k++) {
         unsigned src;

         pass->unit_index[k] = st->unit[k];
         if (m9_split_stage == 1 || m9_split_stage == 2 ||
             m9_split_stage == 4)
            continue;
         /* A coordinate route redirects where this unit samples and leaves
          * its texture and sampler alone: the record's first dword is the
          * descriptor's own offset and the second is named in the word
          * (CAPTURED, docs/parhelia-m9-fragment-pass-routes.md). */
         if (p->texcoord_source[k] != M9_FS_ROUTE_NONE) {
            src = fs->record_offset[p->texcoord_source[k]];
            pass->routes[k].word =
               m9_record_route(true, k, src + 1, M9_RECORD_ABSENT, 0);
            pass->routes[k].desc = m9_record_desc(M9_RECORD_PAIR,
                                                  fs->record_dwords, src);
            pass->route_count = MAX2(pass->route_count, k + 1);
            continue;
         }
         if (p->texture_source[k] == M9_FS_ROUTE_NONE)
            continue;
         /* A routed value arrives in the texel registers of its own slot,
          * so route index and texel slot are the same k. */
         src = fs->record_offset[p->texture_source[k]];
         pass->routes[k].word =
            m9_record_route(false, k, src + 1, src + 2, src + 3);
         pass->routes[k].desc = m9_record_desc(M9_RECORD_SLOT,
                                               fs->record_dwords, src);
         pass->route_count = MAX2(pass->route_count, k + 1);
      }
      /* A record-writing pass that samples nothing still binds unit 0:
       * setting 88 bit 20 without one stops the engine. */
      if (!p->texture_count) {
         pass->unit_index[0] = M9_PASS_UNIT_PLACEHOLDER;
         pass->unit_count = 1;
      }
   }
   return 0;
}

/* Select the exact immutable C2 owner named by this job, including an
 * older queued image. Never substitute the most recently bound program. */
static const struct m9_resource *
m9_job_c2_owner(const struct m9_context *ctx, const struct m9_fs_variant *fs,
                 uint32_t handle)
{
   if (!handle)
      return NULL;
   if (fs) {
      if (fs->native_c2.handle == handle)
         return &fs->native_c2;
      util_dynarray_foreach(&fs->native_c2_images, struct m9_native_image, image)
         if (image->image.handle == handle)
            return &image->image;
   }
   if (ctx->vs_c2.handle == handle)
      return &ctx->vs_c2;
   util_dynarray_foreach(&ctx->vs_c2_images, struct m9_native_image, image)
      if (image->image.handle == handle)
         return &image->image;
   return NULL;
}

/* Read-only proof is bounded independently of application/environment input.
 * Malformed or missing owners refuse the diagnostic, never replace data. */
static bool
m9_native_inputs_valid(const struct m9_context *ctx,
                       const struct m9_draw_job *job,
                       const struct m9_resource *image, unsigned slots)
{
   uint64_t source_bytes = (uint64_t)job->source_vertices * job->vs_record_bytes;
   bool packed = job->vs_producer_entry != 0;
   /* A complete packed chain or the ordinary fused draw. Do not admit
    * truncated chains or the separate legacy extra-head protocol here. */
   if ((packed ? !job->vs_gather_entry :
                 (job->vs_extra_entry || job->vs_gather_entry ||
                  job->vs_extra_targets)) ||
       !job->source_vertices || job->source_vertices % 4 ||
       job->source_vertices > 65532u || !job->vs_record_bytes ||
       job->vs_record_bytes % 16 ||
       job->vs_record_bytes > M9_VS_MAX_ATTRIBUTES * 16u ||
       !ctx->source.map || ctx->source.handle != job->source ||
       !job->source || source_bytes > ctx->source.size ||
       !image || !image->map || image->handle != job->program ||
       !job->program || !slots || slots > 4096u || 8ull * slots > image->size ||
       !ctx->system.map || !job->system || ctx->system.handle != job->system ||
       !job->vs_const_pairs || job->vs_const_pairs > M9_VS_CONST_MAX_PAIRS ||
       M9_VS_CONST_OFFSET > ctx->system.size ||
       8ull * job->vs_const_pairs > ctx->system.size - M9_VS_CONST_OFFSET ||
       !job->vs_decl || !job->vs_decl_count || job->vs_decl_count > M9_VS_MAX_DECL ||
       !job->draws || !job->draw_count || job->draw_count > 4096u ||
       (job->pass_count && (!job->passes || job->pass_count > M9_FS_MAX_PASSES)) ||
       job->index_count > 65536u ||
       (job->index_count && (!ctx->index.map || !job->index ||
        ctx->index.handle != job->index || job->index_base > ctx->index.size ||
        2ull * job->index_count > ctx->index.size - job->index_base)) ||
       !job->transient || ctx->transient.handle != job->transient ||
       (packed && !m9_vs_record_fits(job->source_vertices, job->vs_extra_targets + 3u,
                          job->vs_record_view, m9_transient_view_bytes(),
                          ctx->transient.size)))
      return false;
   if (job->vs_const_run_count &&
       (!job->vs_const_runs || job->vs_const_run_count > M9_VS_MAX_CONST_RUNS))
      return false;
   unsigned end = 0;
   for (unsigned i = 0; i < job->vs_const_run_count; i++) {
      const struct m9_vs_const_run *run = &job->vs_const_runs[i];
      if (run->first != end || !run->count || run->count % 4 ||
          end > job->source_vertices || run->count > job->source_vertices - end ||
          !run->const_pairs || run->const_pairs > M9_VS_CONST_MAX_PAIRS ||
          run->const_offset % 8 || run->const_offset > ctx->system.size ||
          8ull * run->const_pairs > ctx->system.size - run->const_offset)
         return false;
      end += run->count;
   }
   if (job->vs_const_run_count && end != job->source_vertices)
      return false;
   for (unsigned i = 0; i < job->draw_count; i++) {
      const struct m9_draw_record *d = &job->draws[i];
      if (!d->count || d->start > job->source_vertices ||
          d->count > job->source_vertices - d->start)
         return false;
      if (d->index_count) {
         uint64_t halves = (uint64_t)d->index_count + (d->index_count & 1u) +
                           M9_INDEX_TAIL;
         if (d->index_first > job->index_count ||
             halves > job->index_count - d->index_first ||
             ((uint64_t)job->index_base + 2ull * d->index_first) % 4)
            return false;
      }
   }
   return true;
}

static bool
m9_dump_native_inputs(const struct m9_context *ctx,
                       const struct m9_draw_job *job,
                       const struct m9_resource *image, unsigned slots,
                       unsigned seq)
{
   char path[4096];
   if (!ctx->dump_path || !m9_native_inputs_valid(ctx, job, image, slots))
      return false;
   int n = snprintf(path, sizeof(path), "%s.native-context-%u-job-%u.inputs",
                    ctx->dump_path, ctx->context_id, seq);
   if (n < 0 || (size_t)n >= sizeof(path))
      return false;
   /* A second context/process cannot replace an earlier proof file. */
   FILE *out = fopen(path, "wx");
   if (!out)
      return false;
   bool packed = job->vs_producer_entry != 0;
   fprintf(out, "NATIVE_INPUTS %u context %u job %u vertices %u stride %u "
           "fields %u record-view %u source %u index %u index-base %u "
           "index-halves %u C2 %u slots %u entries %u/%u/%u\n",
           packed ? 1u : 2u, ctx->context_id, seq, job->source_vertices, job->vs_record_bytes,
           packed ? job->vs_extra_targets + 3u : 0u,
           packed ? job->vs_record_view : 0u, job->source,
           job->index, job->index_base, job->index_count, job->program, slots,
           job->vs_producer_entry, job->vs_extra_entry, job->vs_gather_entry);
   if (job->vs_sampled_position)
      fprintf(out, "SAMPLED_POSITION first %u fields %u/%u head %u fields %u/%u/%u/%u\n",
              job->vs_producer_targets, job->vs_producer_fields[0],
              job->vs_producer_fields[1], job->vs_head_targets,
              job->vs_head_fields[0], job->vs_head_fields[1],
              job->vs_head_fields[2], job->vs_head_fields[3]);
   if (!packed)
      fprintf(out, "MODE ordinary-fused input-only vertex-entry %u\n",
              job->fs ? job->fs->vertex_entry :
              job->pass_count ? job->passes[0].fs.vertex_entry : 4u);
   fprintf(out, "OWNER SOURCE %u bytes %" PRIu64 " SYSTEM %u bytes %" PRIu64
           " INDEX %u bytes %" PRIu64 " C2 %u bytes %" PRIu64
           " TRANSIENT %u bytes %" PRIu64 "\n",
           ctx->source.handle, ctx->source.size, ctx->system.handle, ctx->system.size,
           ctx->index.handle, ctx->index.size, image->handle, image->size,
           ctx->transient.handle, ctx->transient.size);
   for (unsigned i = 0; i < job->vs_decl_count; i++)
      fprintf(out, "DECL %u %08x\n", i, job->vs_decl[i]);
   const uint32_t *words = image->map;
   for (unsigned i = 0; i < slots; i++)
      fprintf(out, "C2 %u %08x %08x\n", i, words[2*i], words[2*i+1]);
   words = ctx->source.map;
   unsigned source_dwords = job->source_vertices * (job->vs_record_bytes / 4);
   for (unsigned i = 0; i < source_dwords; i++)
      fprintf(out, "SOURCE %u %08x\n", i, words[i]);
   unsigned runs = MAX2(job->vs_const_run_count, 1u);
   for (unsigned i = 0; i < runs; i++) {
      struct m9_vs_const_run single = { .first = 0, .count = job->source_vertices,
         .const_offset = M9_VS_CONST_OFFSET, .const_pairs = job->vs_const_pairs };
      const struct m9_vs_const_run *run = job->vs_const_run_count ?
                                         &job->vs_const_runs[i] : &single;
      fprintf(out, "RUN %u first %u count %u offset %u pairs %u\n", i,
              run->first, run->count, run->const_offset, run->const_pairs);
      words = (const uint32_t *)((const uint8_t *)ctx->system.map + run->const_offset);
      for (unsigned k = 0; k < 2u * run->const_pairs; k++)
         fprintf(out, "CONST %u %u %08x\n", i, k, words[k]);
   }
   for (unsigned i = 0; i < job->draw_count; i++) {
      const struct m9_draw_record *d = &job->draws[i];
      fprintf(out, "DRAW %u start %u count %u index-first %u index-count %u "
              "topology %u cull %08x flat %u lines %u\n", i, d->start, d->count,
              d->index_first, d->index_count, d->topology, d->cull, d->flat, d->lines);
   }
   const uint16_t *indices = job->index_count ?
      (const uint16_t *)((const uint8_t *)ctx->index.map + job->index_base) : NULL;
   unsigned outside = 0;
   for (unsigned i = 0; i < job->index_count; i++) {
      fprintf(out, "INDEX %u %u\n", i, indices[i]);
      outside += indices[i] >= job->source_vertices;
   }
   fprintf(out, "END source-dwords %u index-halves %u index-outside-source %u\n",
           source_dwords, job->index_count, outside);
   bool written = !ferror(out);
   if (fclose(out)) written = false;
   if (!written) unlink(path);
   return written;
}

static bool
m9_clear_native_retirement(const char *record)
{
   char path[4096];
   int n = snprintf(path, sizeof(path), "%s.meta", record);
   return n >= 0 && (size_t)n < sizeof(path) &&
          (!unlink(path) || errno == ENOENT);
}

/* Called only after the real wait succeeded and the actual owner was read. */
static bool
m9_dump_native_retirement(const struct m9_context *ctx, const char *record,
                          uint64_t bytes)
{
   char path[4096];
   int n = snprintf(path, sizeof(path), "%s.meta", record);
   if (n < 0 || (size_t)n >= sizeof(path))
      return false;
   FILE *out = fopen(path, "w");
   if (!out)
      return false;
   fprintf(out, "RETIRED_NATIVEVS 1 context %u job %u producer-point %" PRIu64
           " waited-point %" PRIu64 " vertices %u fields %u view %u bytes %" PRIu64
           " source %u index %u C2 %u inputs-dumped %u\n",
           ctx->context_id, ctx->vs_record_job_seq, ctx->vs_record_point,
           ctx->last_point, ctx->vs_record_vertices, ctx->vs_record_fields,
           ctx->vs_record_view, bytes, ctx->vs_record_source,
           ctx->vs_record_index, ctx->vs_record_program, ctx->vs_record_inputs_dumped);
   if (ctx->dump_path && ctx->vs_record_inputs_dumped)
      fprintf(out, "INPUTS %s.native-context-%u-job-%u.inputs\n", ctx->dump_path,
              ctx->context_id, ctx->vs_record_job_seq);
   bool written = !ferror(out);
   if (fclose(out)) written = false;
   if (!written) unlink(path);
   return written;
}

/* Job words, relocations, fragment slots and constants for offline decode. */
static void
m9_dump_job(struct m9_context *ctx, const struct m9_fs_variant *fs,
            const struct m9_draw_job *job)
{
   const struct m9_batch *b = &ctx->batch;
   /* M9X_DUMP_FROM / M9X_DUMP_COUNT select a window of jobs by their running
    * number, so a long run leaves one decodable stretch from mid-game. */
   unsigned seq = ctx->dump_seq++;
   ctx->dump_native_valid = false;
   ctx->dump_native_seq = seq;
   FILE *out;

   if (seq < ctx->dump_from || seq >= ctx->dump_from + ctx->dump_count)
      return;
   out = fopen(ctx->dump_path, seq == ctx->dump_from ? "w" : "a");
   if (!out)
      return;
   fprintf(out, "JOB %u dwords %u relocs %u\n", seq,
           b->command_count, b->reloc_count);
   for (unsigned i = 0; i < b->command_count; i++)
      fprintf(out, "%08x%c", b->command[i], i % 8 == 7 ? '\n' : ' ');
   fputc('\n', out);
   for (unsigned i = 0; i < b->reloc_count; i++)
      fprintf(out, "RELOC dword %u handle %u offset %#" PRIx64 " kind %u\n",
              b->relocs[i].command_dword, b->relocs[i].handle,
              (uint64_t)b->relocs[i].target_offset, b->relocs[i].kind);
   /* The kernel compares each ref's flags against the union of its relocs,
    * and every target offset against the object size. */
   for (unsigned i = 0; i < b->bo_count; i++) {
      uint64_t size = 0;

      if (b->bos[i].handle == ctx->command.handle) size = ctx->command.size;
      else if (b->bos[i].handle == ctx->system.handle) size = ctx->system.size;
      else if (b->bos[i].handle == ctx->program.handle) size = ctx->program.size;
      else if (b->bos[i].handle == ctx->fragment_db.handle) size = ctx->fragment_db.size;
      else if (b->bos[i].handle == ctx->transient.handle) size = ctx->transient.size;
      else if (b->bos[i].handle == ctx->cpu_streams.handle) size = ctx->cpu_streams.size;
      else if (b->bos[i].handle == ctx->be.handle) size = ctx->be.size;
      else if (b->bos[i].handle == ctx->source.handle) size = ctx->source.size;
      else if (b->bos[i].handle == ctx->index.handle) size = ctx->index.size;
      fprintf(out, "BO handle %u flags %u size %#" PRIx64 "\n",
              b->bos[i].handle, b->bos[i].flags, size);
   }
   if (ctx->job_key.vs) {
      const struct m9_vs_program *vs = ctx->job_key.vs;
      const struct m9_resource *image = m9_job_c2_owner(ctx, fs, job->program);
      unsigned final_entry = job->fs ? job->fs->vertex_entry :
                             job->pass_count ? job->passes[0].fs.vertex_entry : 4u;
      unsigned slots = job->vs_producer_entry ?
         job->vs_producer_entry + m9_vs_producer_slot_count(vs) :
         final_entry + m9_vs_slot_count(vs);
      if (job->vs_extra_entry && !job->vs_producer_entry)
         slots = job->vs_extra_entry + m9_vs_extra_slot_count(vs);
      fprintf(out, "VSOUTPUTS count %u fused-pair %u\n",
              vs->outputs.count, ctx->job_key.vs_fused_pair);
      for (unsigned k = 0; k < vs->outputs.count; k++) {
         const struct m9_vs_output *o = &vs->outputs.stream[k];
         fprintf(out, "VSOUTPUT %u location %u component %u coord %u pass %u "
                 "coordinate %u coeff-record %u paired %u\n", k,
                 o->location, o->component, o->coord, o->pass, o->coordinate,
                 o->coeff_record, o->paired);
      }
      if (m9_dump_native_inputs_enable) {
         ctx->dump_native_valid = m9_dump_native_inputs(ctx, job, image, slots, seq);
         if (!ctx->dump_native_valid)
            fprintf(stderr, "m9x: native input proof refused context %u job %u\n",
                    ctx->context_id, seq);
      }
      fprintf(out, "NATIVEVS vertices %u stride %u C2 %u slots %u "
              "entries %u/%u/%u/%u masks %04x/%04x record-view %u fields %u\n",
              job->source_vertices, job->vs_record_bytes,
              job->program, slots, job->vs_producer_entry, job->vs_extra_entry,
              job->vs_gather_entry, final_entry, job->vs_producer_mask, job->vs_extra_mask,
              job->vs_producer_entry ? job->vs_record_view : 0u,
              job->vs_producer_entry ? job->vs_extra_targets + 3u : 0u);
      /* These are the actual mapped submitted image bytes before ioctl,
       * not reconstructed opcodes or a claim of GPU fetch visibility. */
      if (image && image->map && 8ull * slots <= image->size) {
         const uint32_t *w = image->map;
         for (unsigned i = 0; i < slots; i++)
            fprintf(out, "C2 %u %08x %08x\n", i, w[2*i], w[2*i+1]);
      } else {
         fprintf(out, "C2 OWNER MISSING\n");
      }
      uint64_t bytes = (uint64_t)MIN2(job->source_vertices, 16u) * job->vs_record_bytes;
      if (ctx->source.handle == job->source && ctx->source.map &&
          bytes <= ctx->source.size) {
         const uint32_t *w = ctx->source.map;
         fprintf(out, "SOURCE vertices %u of %u bytes %" PRIu64 "\n",
                 MIN2(job->source_vertices, 16u), job->source_vertices, bytes);
         for (unsigned i = 0; i < bytes / 4; i++)
            fprintf(out, "SOURCE %u %08x\n", i, w[i]);
      }
      uint64_t const_bytes = (uint64_t)job->vs_const_pairs * 8;
      if (ctx->system.handle == job->system && ctx->system.map &&
          M9_VS_CONST_OFFSET <= ctx->system.size &&
          const_bytes <= ctx->system.size - M9_VS_CONST_OFFSET) {
         const uint32_t *w = (const uint32_t *)((const uint8_t *)ctx->system.map +
                                               M9_VS_CONST_OFFSET);
         for (unsigned i = 0; i < const_bytes / 4; i++)
            fprintf(out, "VSCONST %u %08x\n", i, w[i]);
      }
   }
   if (fs) {
      const float *k = ctx->job_consts.data;
      unsigned base = 0;

      for (unsigned pi = 0; pi < fs->passes.count; pi++) {
         const struct m9_fs_program *p = &fs->passes.pass[pi];
         const uint32_t *w = p->slots.data;

         for (unsigned i = 0; i < m9_fs_slot_count(p); i++)
            fprintf(out, "SLOT %u %08x %08x\n", base + i, w[2 * i],
                    w[2 * i + 1]);
         base += m9_fs_slot_count(p);
      }
      for (unsigned i = 0; i < ctx->job_consts.size / sizeof(float); i++)
         fprintf(out, "CONST %u %g\n", i, k[i]);
   }
   fclose(out);
}

static void
m9_dynarray_swap(struct util_dynarray *a, struct util_dynarray *b)
{
   struct util_dynarray t = *a;

   *a = *b;
   *b = t;
}

/* Exchange the pending job with one held aside, so the driver can build and
 * submit a job while another waits. */
static void
m9_job_state_swap(struct m9_context *ctx, struct m9_job_state *s)
{
   struct m9_job_key key = ctx->job_key;
   unsigned floats = ctx->record_floats, dwords = ctx->job_unit_dwords;

   ctx->job_key = s->key;
   ctx->record_floats = s->record_floats;
   ctx->job_unit_dwords = s->job_unit_dwords;
   s->key = key;
   s->record_floats = floats;
   s->job_unit_dwords = dwords;
   m9_dynarray_swap(&ctx->records, &s->records);
   m9_dynarray_swap(&ctx->indices, &s->indices);
   m9_dynarray_swap(&ctx->draws, &s->draws);
   m9_dynarray_swap(&ctx->extra, &s->extra);
   m9_dynarray_swap(&ctx->job_textures, &s->job_textures);
   m9_dynarray_swap(&ctx->job_consts, &s->job_consts);
   m9_dynarray_swap(&ctx->job_vs_consts, &s->job_vs_consts);
   m9_dynarray_swap(&ctx->job_vs_runs, &s->job_vs_runs);
   m9_dynarray_swap(&ctx->job_vs_values, &s->job_vs_values);
}

static void
m9_job_state_fini(struct m9_job_state *s)
{
   util_dynarray_foreach(&s->job_textures, struct pipe_resource *, held)
      pipe_resource_reference(held, NULL);
   util_dynarray_fini(&s->records);
   util_dynarray_fini(&s->indices);
   util_dynarray_fini(&s->draws);
   util_dynarray_fini(&s->extra);
   util_dynarray_fini(&s->job_textures);
   util_dynarray_fini(&s->job_consts);
   util_dynarray_fini(&s->job_vs_consts);
   util_dynarray_fini(&s->job_vs_runs);
   util_dynarray_fini(&s->job_vs_values);
   memset(s, 0, sizeof(*s));
}

/* Release what the held job holds, without charging it to anything. */
static void
m9_job_state_clear(struct m9_context *ctx)
{
   util_dynarray_foreach(&ctx->job_textures, struct pipe_resource *, held)
      pipe_resource_reference(held, NULL);
   util_dynarray_clear(&ctx->job_textures);
   ctx->job_unit_dwords = 0;
   util_dynarray_clear(&ctx->job_vs_runs);
   util_dynarray_clear(&ctx->job_vs_values);
   util_dynarray_clear(&ctx->records);
   util_dynarray_clear(&ctx->indices);
   util_dynarray_clear(&ctx->draws);
   util_dynarray_clear(&ctx->extra);
}

/* Drop the job held in this state with a named reason, releasing what it
 * holds; the caller has already swapped it into the context. */
static void
m9_job_state_drop(struct m9_context *ctx, const char *warn, const char *why)
{
   unsigned draws = util_dynarray_num_elements(&ctx->draws,
                                               struct m9_draw_record);

   m9_warn_once(ctx, M9_WARN_FS, warn);
   m9_skip_reason(ctx, why);
   ctx->skipped_draws += draws;
   m9_prof_skipped += draws;
   m9_job_state_clear(ctx);
}

/* The padding a partial draw reaches its target's extent with: four
 * rectangles a pixel wide along the four edges, each as two triangles of each
 * winding so that no cull state can remove them. */
#define M9_PAD_RECTS 4u
#define M9_PAD_RECORDS (M9_PAD_RECTS * 6u)
#define M9_PAD_INDICES (M9_PAD_RECTS * 12u)

static bool
m9_push_tri(struct util_dynarray *a, unsigned i0, unsigned i1, unsigned i2)
{
   uint16_t *w = util_dynarray_grow(a, uint16_t, 3);

   if (!w)
      return false;
   w[0] = i0;
   w[1] = i1;
   w[2] = i2;
   return true;
}

/* The triangle list a draw's topology describes, as record indices. */
static bool
m9_pad_expand(struct util_dynarray *out, const struct m9_draw_record *d,
              const uint16_t *src)
{
   unsigned n = d->index_count ? d->index_count : d->count;

#define AT(k) (src ? src[d->index_first + (k)] : d->start + (k))
   switch (d->topology) {
   case M9_TOPOLOGY_TRIANGLES:
      for (unsigned k = 0; k + 3 <= n; k += 3)
         if (!m9_push_tri(out, AT(k), AT(k + 1), AT(k + 2)))
            return false;
      return true;
   case M9_TOPOLOGY_TRIANGLE_STRIP:
      for (unsigned k = 0; k + 3 <= n; k++)
         if (!m9_push_tri(out, AT(k + (k & 1)), AT(k + 1 - (k & 1)),
                          AT(k + 2)))
            return false;
      return true;
   case M9_TOPOLOGY_TRIANGLE_FAN:
      for (unsigned k = 1; k + 2 <= n; k++)
         if (!m9_push_tri(out, AT(0), AT(k), AT(k + 1)))
            return false;
      return true;
   case M9_TOPOLOGY_QUADS:
      for (unsigned k = 0; k + 4 <= n; k += 4)
         if (!m9_push_tri(out, AT(k), AT(k + 1), AT(k + 2)) ||
             !m9_push_tri(out, AT(k), AT(k + 2), AT(k + 3)))
            return false;
      return true;
   case M9_TOPOLOGY_QUAD_STRIP:
      for (unsigned k = 0; k + 4 <= n; k += 2)
         if (!m9_push_tri(out, AT(k), AT(k + 1), AT(k + 3)) ||
             !m9_push_tri(out, AT(k), AT(k + 3), AT(k + 2)))
            return false;
      return true;
   default:
      return false;
   }
#undef AT
}

/* One padding rectangle, six records in triangle-list order. */
static void
m9_pad_records(float *out, unsigned floats, float x0, float y0, float x1,
               float y1)
{
   static const uint8_t corner[6][2] = {
      { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 },
   };

   for (unsigned i = 0; i < 6; i++) {
      float *p = out + i * floats;

      memset(p, 0, floats * sizeof(float));
      p[0] = corner[i][0] ? x1 : x0;
      p[1] = corner[i][1] ? y1 : y0;
      p[3] = 1.0f;   /* w: the padding does not project */
   }
}

static bool
m9_gpu_padding_bounds(const struct m9_context *ctx, const struct m9_job_key *key,
                      const float scale[3], const float bias[3],
                      struct m9_vs_padding_bounds *b)
{
   if (!m9_native_vs_gpu_padding || !key->variant ||
       key->variant->passes.count < 2 || ctx->lost || ctx->lines ||
       m9_faa_active(&ctx->screen->faa) || ctx->faa_surface.graph.initialized ||
       m9_split_depth != 1 || !m9_split_seq || m9_split_stage != 3 ||
       !m9_split_submit || m9_split_passes || !m9_split_coverage ||
       m9_record_89 || m9_vs_const_pad ||
       key->depth_bias)
      return false;
   *b = (struct m9_vs_padding_bounds) {
      .width = ctx->params.width, .height = ctx->params.height,
      .clip_min = key->clip_min, .clip_max = key->clip_max,
      .normalized_less_depth =
         (key->depth_control & ~2u) == m9_depth_control(true, false, PIPE_FUNC_LESS),
      .stencil_enabled = key->stencil,
      .query_active = ctx->query_active || ctx->query_begin || ctx->query_end ||
                      ctx->query_paused || ctx->query_exclude,
   };
   memcpy(b->viewport_scale, scale, sizeof(b->viewport_scale));
   memcpy(b->viewport_translate, bias, sizeof(b->viewport_translate));
   for (unsigned i = 0; i < key->variant->passes.count; i++)
      b->fragment_depth |= key->variant->passes.pass[i].fragdepth;
   return m9_vs_padding_bounds_valid(b);
}

/* Build only CPU-owned append state here. Nothing is mapped or posted on
 * failure; pending source/texture snapshots remain owned by the job. */
static bool
m9_gpu_padding_append(struct m9_context *ctx, unsigned *records)
{
   const struct m9_job_key *key = &ctx->job_key;
   struct m9_vs_padding_bounds bounds;
   if (!key->vs || !key->vs->gpu_padding || ctx->job_vs_runs.size ||
       util_dynarray_num_elements(&ctx->draws, struct m9_draw_record) != 1 ||
       !m9_gpu_padding_bounds(ctx, key, key->viewport_scale, key->viewport_bias, &bounds))
      return false;
   struct m9_draw_record draw = *(struct m9_draw_record *)ctx->draws.data;
   if (draw.lines || ctx->record_floats * 4 != key->vs->record_bytes)
      return false;
   uint32_t constants[2 * M9_VS_MAX_CONST_PAIRS] = {0};
   struct m9_draw_params params = ctx->params;
   uint32_t prefix[M9_SYSTEM_PREFIX_DWORDS];
   memcpy(prefix, params.system_prefix, sizeof(prefix));
   m9_draw_params_viewport(&params, key->viewport_scale, key->viewport_bias, prefix);
   memcpy(constants, (uint8_t *)prefix + M9_VS_CONST_OFFSET,
          MIN2(sizeof(constants), sizeof(prefix) - M9_VS_CONST_OFFSET));
   unsigned pairs = m9_vs_const_span(key->vs, ctx->job_vs_consts.data,
                         ctx->job_vs_consts.size / sizeof(float),
                         constants, ARRAY_SIZE(constants));
   struct m9_vs_padding_upload upload;
   unsigned copies = key->variant->passes.count;
   if (!m9_vs_padding_upload_build(key->vs, &bounds, *records,
          m9_draw_max_source_vertices() / copies, constants, pairs, &upload))
      return false;
   struct util_dynarray indices = {0};
   if (!m9_pad_expand(&indices, &draw, ctx->indices.size ? ctx->indices.data : NULL))
      goto fail;
   uint16_t *added = util_dynarray_grow(&indices, uint16_t, M9_VS_PADDING_INDICES);
   if (!added)
      goto fail;
   memcpy(added, upload.indices, M9_VS_PADDING_INDICES * sizeof(uint16_t));
   unsigned count = util_dynarray_num_elements(&indices, uint16_t);
   uint16_t *tail = util_dynarray_grow(&indices, uint16_t, (count & 1) + M9_INDEX_TAIL);
   if (!tail)
      goto fail;
   for (unsigned i = 0; i < (count & 1) + M9_INDEX_TAIL; i++)
      tail[i] = ((uint16_t *)indices.data)[count - 1];
   size_t before = ctx->records.size;
   void *out = util_dynarray_grow_bytes(&ctx->records, 1,
                        M9_VS_PADDING_VERTICES * upload.record_bytes);
   if (!out)
      goto fail;
   memcpy(out, upload.records, M9_VS_PADDING_VERTICES * upload.record_bytes);
   struct m9_vs_const_run *runs = util_dynarray_grow(&ctx->job_vs_runs,
                                                    struct m9_vs_const_run, 2 * copies);
   if (!runs) {
      ctx->records.size = before;
      goto fail;
   }
   for (unsigned i = 0; i < 2 * copies; i++)
      runs[i] = (struct m9_vs_const_run) {
         .first = (i / 2) * upload.source_vertices + upload.runs[i % 2].first,
         .count = upload.runs[i % 2].count,
         .const_offset = i % 2 ? M9_VS_RUN_CONST_OFFSET : M9_VS_CONST_OFFSET,
         .const_pairs = pairs,
      };
   util_dynarray_fini(&ctx->indices);
   ctx->indices = indices;
   draw.topology = M9_TOPOLOGY_TRIANGLES;
   draw.index_first = 0; draw.index_count = count;
   *(struct m9_draw_record *)ctx->draws.data = draw;
   *records = upload.source_vertices;
   ctx->padded_draws++;
   return true;
fail:
   util_dynarray_fini(&indices);
   return false;
}

/*
 * Pad a partial multi-pass draw out to its render target's extent and narrow
 * the clip window to the pixels it covers.  The engine counts the geometry a
 * job *rasterises*, not the pixels it writes: a draw covering the whole target
 * renders with its writes scissored down to a rectangle, a draw padded only
 * along the target's four edges renders too, and a draw covering everything
 * but a rectangle in the middle renders -- so interior coverage is not what is
 * counted (MEASURED, conv3pad, conv3frame, conv3subpad and conv3hole against
 * conv3sub).  The padding is four rectangles a pixel wide at the edges, two
 * triangles of each winding apiece so that no cull state removes them, and the
 * clip window keeps their fragments out of the image.  A strip at an edge
 * the retained clip touches is omitted, since the original geometry already
 * reaches that edge and its padding would overwrite the real fragments.
 */
static bool
m9_split_pad(struct m9_context *ctx, const struct m9_job_key *key,
             unsigned *records, uint32_t *cx0, uint32_t *cy0, uint32_t *cx1,
             uint32_t *cy1, float x0, float y0, float x1, float y1,
             bool *empty)
{
   struct m9_draw_record *draw = ctx->draws.data;
   unsigned floats = ctx->record_floats;
   unsigned extra_floats = 4 * (key->variant && !key->vs ?
                                key->variant->extra_count : 0);
   unsigned copies = key->variant && key->fs ? key->variant->passes.count : 1;
   uint32_t w = ctx->params.width, h = ctx->params.height;
   struct util_dynarray idx = {0};
   uint32_t bx0, by0, bx1, by1;
   unsigned base = *records, count, pad_mask;
   uint16_t *tail;
   float *out;

   /* The padding is written in screen space, which is what a record carries
    * only when the draw module transformed it. */
   if (!m9_split_pad_enable || key->vs || ctx->lost ||
       util_dynarray_num_elements(&ctx->draws, struct m9_draw_record) != 1 ||
       (base + M9_PAD_RECORDS) * copies > m9_draw_max_source_vertices()) {
      if (m9_split_trace)
         fprintf(stderr, "m9x pad: refused enable %d vs %d lost %d draws %u "
                 "records %u copies %u max %u\n", m9_split_pad_enable,
                 key->vs != NULL, ctx->lost,
                 (unsigned)util_dynarray_num_elements(&ctx->draws,
                                                      struct m9_draw_record),
                 base, copies, m9_draw_max_source_vertices());
      return false;
   }
   /* The clip window keeps the pixels the geometry covers and nothing else:
    * the centres its extent encloses, held inside the window in force. */
   bx0 = MAX2((int32_t)ceilf(x0 - 0.5f), 0);
   bx1 = MIN2((uint32_t)MAX2((int32_t)ceilf(x1 - 0.5f), 0), w);
   by0 = MAX2((int32_t)ceilf((float)h - y1 - 0.5f), 0);
   by1 = MIN2((uint32_t)MAX2((int32_t)ceilf((float)h - y0 - 0.5f), 0), h);
   bx0 = MAX2(bx0, *cx0);
   by0 = MAX2(by0, *cy0);
   bx1 = MIN2(bx1, *cx1);
   by1 = MIN2(by1, *cy1);
   /* The geometry encloses no pixel centre -- MEASURED on terrain, a band
    * 0.37 pixels high -- so the draw writes nothing and there is nothing to
    * pad it out to.  Dropping it loses no pixel, and it is not a draw the
    * driver failed to carry. */
   if (bx1 <= bx0 || by1 <= by0) {
      if (m9_split_trace)
         fprintf(stderr, "m9x pad: empty box %u,%u..%u,%u from "
                 "%.2f,%.2f..%.2f,%.2f\n", bx0, by0, bx1, by1, x0, y0, x1, y1);
      *empty = true;
      return false;
   }
   pad_mask = m9_split_pad_mask(w, h, bx0, by0, bx1, by1);
   if (!m9_pad_expand(&idx, draw, ctx->indices.size ? ctx->indices.data : NULL))
      goto fail;
   for (unsigned r = 0; r < M9_PAD_RECTS; r++) {
      unsigned p = base + r * 6;

      if (!(pad_mask & BITFIELD_BIT(r)))
         continue;
      if (!m9_push_tri(&idx, p, p + 1, p + 2) ||
          !m9_push_tri(&idx, p + 3, p + 4, p + 5) ||
          !m9_push_tri(&idx, p + 2, p + 1, p) ||
          !m9_push_tri(&idx, p + 5, p + 4, p + 3))
         goto fail;
   }
   count = util_dynarray_num_elements(&idx, uint16_t);
   /* The engine reads one index half past the last of a run (M9_INDEX_TAIL). */
   tail = util_dynarray_grow(&idx, uint16_t, (count & 1) + M9_INDEX_TAIL);
   if (!tail)
      goto fail;
   for (unsigned i = 0; i < (count & 1) + M9_INDEX_TAIL; i++)
      tail[i] = ((const uint16_t *)idx.data)[count - 1];
   out = util_dynarray_grow(&ctx->records, float, M9_PAD_RECORDS * floats);
   if (!out)
      goto fail;
   m9_pad_records(out, floats, 0, (float)h - 1, (float)w, (float)h);
   m9_pad_records(out + 6 * floats, floats, 0, 0, (float)w, 1);
   m9_pad_records(out + 12 * floats, floats, 0, 0, 1, (float)h);
   m9_pad_records(out + 18 * floats, floats, (float)w - 1, 0, (float)w,
                  (float)h);
   if (extra_floats) {
      float *ex = util_dynarray_grow(&ctx->extra, float,
                                     M9_PAD_RECORDS * extra_floats);

      if (!ex)
         goto fail;
      memset(ex, 0, M9_PAD_RECORDS * extra_floats * sizeof(float));
   }
   util_dynarray_fini(&ctx->indices);
   ctx->indices = idx;
   draw->topology = M9_TOPOLOGY_TRIANGLES;
   draw->index_first = 0;
   draw->index_count = count;
   *records = base + M9_PAD_RECORDS;
   *cx0 = bx0;
   *cy0 = by0;
   *cx1 = bx1;
   *cy1 = by1;
   ctx->padded_draws++;
   return true;
fail:
   if (m9_split_trace)
      fprintf(stderr, "m9x pad: failed topology %u indices %u count %u "
              "records %u\n", draw->topology, draw->index_count, draw->count,
              base);
   util_dynarray_fini(&idx);
   return false;
}

/*
 * Whether the geometry fills the box it spans.  A scratch surface is only a
 * target the job covers whole if the geometry is a screen-aligned rectangle:
 * every record sits on a corner of its own extent and all four corners are
 * drawn, which covers a strip, a fan, a quad and a rectangle cut into two
 * triangles alike.  A mesh spans a box it does not fill -- refract hands over
 * 156 records and terrain 576 (MEASURED, M9X_SPLIT_TRACE) -- and rendering one
 * on a scratch surface of its extent would leave that surface partly uncovered
 * in exactly the way that stops the engine.
 */
static bool
m9_split_fills_box(const struct m9_context *ctx, unsigned records,
                   float x0, float y0, float x1, float y1)
{
   const struct m9_draw_record *draw = ctx->draws.data;
   const float *v = ctx->records.data;
   unsigned corners = 0;

   if (util_dynarray_num_elements(&ctx->draws, struct m9_draw_record) != 1 ||
       (draw->topology != M9_TOPOLOGY_TRIANGLE_STRIP &&
        draw->topology != M9_TOPOLOGY_TRIANGLE_FAN &&
        draw->topology != M9_TOPOLOGY_TRIANGLES &&
        draw->topology != M9_TOPOLOGY_QUADS &&
        draw->topology != M9_TOPOLOGY_QUAD_STRIP) ||
       records < 4 || records > 6 || x1 <= x0 || y1 <= y0)
      return false;
   for (unsigned k = 0; k < records; k++) {
      const float *p = v + (uint64_t)k * ctx->record_floats;
      float iw = p[3] > 0.0f ? 1.0f / p[3] : 1.0f;
      float x = p[0] * iw, y = p[1] * iw;

      /* The extent is the minimum and maximum of these very values, so a
       * corner compares exactly and anything inside the box does not. */
      if ((x != x0 && x != x1) || (y != y0 && y != y1))
         return false;
      corners |= 1u << ((x == x1) | (y == y1) << 1);
   }
   return corners == 0xf;
}

/*
 * Hold a multi-pass job whose geometry covers part of its render target
 * aside, to be rendered into a scratch surface of the pixels it does cover
 * and copied into place.  The box is the pixel centres its screen extent
 * encloses, which is what the geometry covers exactly however it is offset
 * inside a pixel, and its origin is whole so that the translated records keep
 * the target's own pixel grid.  A record counts rows from the bottom where
 * the box counts them from the top.
 */
static const char *
m9_split_defer(struct m9_context *ctx, const struct m9_job_key *key,
               unsigned records, float x0, float y0, float x1, float y1)
{
   struct m9_deferred_job *d = &ctx->deferred;
   int32_t w = ctx->params.width, h = ctx->params.height;
   int32_t max = ctx->screen->base.caps.max_texture_2d_size;
   int32_t bx0, by0, bx1, by1;

   /* Named one by one, because the reason a draw is refused is what the skip
    * report has to carry. */
   if (!m9_split_scratch)
      return "a multi-pass draw over part of its target, "
             "with the scratch surface turned off";
   /* The geometry is asked about first, because a draw that does not fill
    * the box it spans has no scratch surface it could cover whatever else
    * it carries: the reason a draw is refused then names the one property
    * that a scratch surface of its own would answer. */
   if (!m9_split_fills_box(ctx, records, x0, y0, x1, y1))
      return "a multi-pass draw over part of its target "
             "whose geometry does not fill the box it spans";
   if (key->depth_ef)
      return "a multi-pass draw over part of its target "
             "that tests or writes depth";
   if (key->vs)
      return "a multi-pass draw over part of its target "
             "whose geometry a program of its own preprocesses";
   if (key->clip_min != ctx->params.clip_min ||
       key->clip_max != ctx->params.clip_max)
      return "a multi-pass draw over part of its target "
             "that carries a scissor of its own";
   if (d->pending || ctx->in_scratch)
      return "a multi-pass draw over part of its target "
             "while another is already held";
   if (!ctx->blitter || ctx->query_active || !ctx->color || !records ||
       ctx->lost)
      return "a multi-pass draw over part of its target "
             "with no scratch surface to copy into place";
   /* Every record has to be in front of the eye: the translation divides by
    * the slot the perspective divide multiplies. */
   for (unsigned k = 0; k < records; k++)
      if (((const float *)ctx->records.data)[(uint64_t)k * ctx->record_floats +
                                             3] <= 0.0f)
         return "a multi-pass draw over part of its target "
                "with a vertex behind the eye";
   bx0 = CLAMP((int32_t)ceilf(x0 - 0.5f), 0, w);
   bx1 = CLAMP((int32_t)ceilf(x1 - 0.5f), 0, w);
   by0 = CLAMP((int32_t)ceilf((float)h - y1 - 0.5f), 0, h);
   by1 = CLAMP((int32_t)ceilf((float)h - y0 - 0.5f), 0, h);
   if (bx1 <= bx0 || by1 <= by0)
      return "a multi-pass draw over part of its target "
             "covering no whole pixel";
   if (bx1 - bx0 > max || by1 - by0 > max)
      return "a multi-pass draw over part of its target "
             "covering more pixels than an image holds";
   pipe_resource_reference(&d->dst, ctx->color);
   d->x = bx0;
   d->y = by0;
   d->width = bx1 - bx0;
   d->height = by1 - by0;
   d->target_height = ctx->params.height;
   /* The state swapped in is empty, but its record stride divides in
    * m9_submit_job before that job's draw count is looked at. */
   d->state.record_floats = ctx->record_floats;
   m9_job_state_swap(ctx, &d->state);
   d->pending = true;
   return NULL;
}

/*
 * Present the engine with an empty back-end state object.  A job posted
 * asynchronously retires only at the *next* submission, so the engine can
 * still be reading the object a CPU clear overwrites -- the scratch path's
 * seed copy is a draw into the very surface whose object is cleared next.
 * Zeroing the object in place is therefore a race, and there is no drain to
 * take: the kernel's no-op fence writes the shared command image without
 * retiring the posted job first.  So a target keeps two objects and a clear
 * swaps to the spare and zeroes that, which the engine cannot be reading
 * because the job in flight bound the other one.
 *
 * M9X_BE_SWAP=0 zeroes in place, which is what the clear did before.
 */
/* Bytes of the back-end object a clear has to write.  Its mask extent is
 * linear in the target's area -- 306 dwords at 84x84, 334 at 88x88 and 3149
 * at 320x240, which fits area / 24.495 + 20 to within three dwords (DERIVED
 * from the dumps, docs/parhelia-m9-glmark2-status.md) -- so a full-screen
 * target uses about 367 KiB of the megabyte the object is allocated at.  The
 * span below is that relation with the divisor rounded down and 16 KiB of
 * margin, clamped to the allocation; M9X_BE_CLEAR_BYTES overrides it. */
static uint64_t
m9_be_clear_span(const struct m9_context *ctx, const struct m9_resource *be)
{
   const struct m9_target_depth *entry = ctx->target_entry;
   uint64_t area = entry && entry->width && entry->height ?
                   (uint64_t)entry->width * entry->height :
                   (uint64_t)ctx->params.width * ctx->params.height;
   uint64_t bytes = m9_be_clear_bytes ? m9_be_clear_bytes :
                    (area / 6 + (16u << 10) + 0xfff) & ~0xfffull;

   return MIN2(bytes, be->size);
}

/* Compared field by field rather than with memcmp, whose answer would depend
 * on the padding of an aggregate initialiser.  M9X_BE_SHAPE_CLIP=0 leaves the
 * clip window out of the comparison: a padded split job narrows it to the
 * pixels its own geometry covers, so a mesh cut into many jobs changes it
 * every time and nothing else about the shape moves. */
static bool
m9_be_shape_same(const struct m9_be_shape *a, const struct m9_be_shape *b)
{
   return a->variant == b->variant && a->color == b->color &&
          a->color_offset == b->color_offset && a->depth == b->depth &&
          a->aux == b->aux && a->color_desc == b->color_desc &&
          a->depth_desc == b->depth_desc && a->color_count == b->color_count &&
          a->passes == b->passes &&
          (!m9_be_shape_clip ||
           (a->clip_min == b->clip_min && a->clip_max == b->clip_max));
}

/* Where the shape of the object in force is remembered: a target that owns a
 * back-end pair keeps its own, the context keeps the one for ctx->be. */
static struct m9_be_shape *
m9_be_shape_slot(struct m9_context *ctx)
{
   struct m9_target_depth *entry = ctx->target_entry;

   return entry && m9_be_per_target ? &entry->be_shape : &ctx->be_shape;
}

static void
m9_be_zero(struct m9_context *ctx)
{
   struct m9_target_depth *entry = ctx->target_entry;
   struct m9_resource *be;
   unsigned selected = entry ? entry->be_slot : 0;
   uint64_t t_be = m9_prof_begin();

   if (m9_aggregate_flush(ctx)) {
      m9_prof_end(M9_PROF_BE, t_be);
      return;
   }
   if (ctx->lost) {
      m9_prof_end(M9_PROF_BE, t_be);
      return;
   }

   if (entry && m9_be_per_target && m9_be_swap) {
      unsigned spare = entry->be_slot ^ 1u;

      /* The spare is allocated the first time a shape wants a clear, so a
       * target that never clears costs one object as before. */
      if (!entry->be[spare].handle &&
          m9_bo_create(ctx->screen, M9_BE_BYTES, PARHELIA_M9_BO_VRAM,
                       M9_RESOURCE_SYSTEM_STATE, &entry->be[spare]))
         entry->be[spare].handle = 0;
      if (entry->be[spare].handle)
         selected = spare;
   }
   be = entry && m9_be_per_target ? &entry->be[selected] : &ctx->be;
   /* A queued reader can still use either object. Wait for the selected
    * owner's actual last submit, not the context's newer unrelated work.
    * Pending CPU draw/held-job states have no BE binding: submit_job chooses
    * it here, before emitting its batch, then records every successful post.
    * A failed spare allocation falls back to the current owner's same wait.
    * Do not publish a new slot or invalidate the shape on a failed wait. */
   if (ctx->screen->queued_submit &&
       m9_wait_for_point(ctx->screen, be->be_point)) {
      ctx->lost = true;
      m9_prof_end(M9_PROF_BE, t_be);
      return;
   }
   m9_be_shape_slot(ctx)->valid = false;
   if (entry && m9_be_per_target && selected != entry->be_slot) {
      entry->be_slot = selected;
      m9_be_swaps++;
   }
   if (be->map)
      memset(be->map, 0, m9_be_clear_span(ctx, be));
   if (entry && m9_be_per_target)
      ctx->target_be = be->handle ? be : NULL;
   m9_prof_end(M9_PROF_BE, t_be);
}

/* Only a successful real submit establishes this owner's completion point.
 * This also runs after every separately submitted raster pass. A failed post
 * loses the context before any later CPU clear can use unproved ownership. */
static void
m9_job_submitted(struct m9_context *ctx, struct m9_resource *be, uint64_t point)
{
   ctx->last_point = ctx->arena_point = point;
   be->be_point = point;
}

static int
m9_submit_job(struct m9_context *ctx)
{
   unsigned draws = util_dynarray_num_elements(&ctx->draws,
                                               struct m9_draw_record);
   unsigned records = util_dynarray_num_elements(&ctx->records, float) /
                      ctx->record_floats;
   const struct m9_job_key *key = &ctx->job_key;
   uint32_t vs_decl[M9_VS_MAX_DECL];
   unsigned vs_decl_count = 0, vs_const_pairs = 0;
   const struct m9_fs_state *fs = key->fs;
   const struct m9_draw_record *first_draw = ctx->draws.data;
   struct m9_raster_pass passes[M9_FS_MAX_PASSES];
   struct m9_texture_unit placeholder;
   unsigned pass_count;
   struct m9_draw_job job;
   uint32_t split_x0 = key->clip_min & 0xffffu;
   uint32_t split_x1 = key->clip_max & 0xffffu;
   uint32_t split_y0 = key->clip_min >> 16;
   uint32_t split_y1 = key->clip_max >> 16;
   /* A record holds x and y multiplied by w with w itself in the fourth slot
    * (m9_render_record), so the screen extent needs the quotient -- the
    * product is the same number only where w is 1, which is every draw that
    * does not project.  The clip window counts rows from the top and the
    * record from the bottom. */
   float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
   bool partial = false;
   uint64_t point = 0;
   uint64_t t_submit;
   bool built;
   bool faa_store_locked = false;
   bool aggregate_job = false, deferred_submit = false;
   bool gpu_padding = false;
   int ret;

   /* An empty job's key may name a shader or variant deleted since the
    * last submit; nothing reads it until a draw replaces the key. */
   if (!draws)
      return 0;
   if (ctx->lost)
      return -EIO;
   pass_count = fs ? key->variant->passes.count : 0;
   gpu_padding = key->vs && key->vs->gpu_padding;
   if (gpu_padding && !m9_gpu_padding_append(ctx, &records)) {
      m9_job_state_drop(ctx, "GPU multipass padding could not retain its inputs",
                             "the live padding/viewport contract was refused");
      return 0;
   }
   if (m9_split_trace > 1)
      fprintf(stderr, "m9x job %llu: %ux%u passes %u draws %u records %u "
              "colour %08x depth %08x clip %08x\n",
              (unsigned long long)m9_prof_jobs + 1, ctx->params.width,
              ctx->params.height, pass_count, draws, records,
              ctx->params.color_desc, ctx->params.depth_desc,
              ctx->params.clip_max);
   t_submit = m9_prof_begin();
   m9_prof_jobs++;
   m9_prof_records += records;
   /* One clear of the 35 KiB batch per job: the build below is its only
    * writer, so clearing it again on the way out was dead work. */
   m9_batch_init(&ctx->batch);
   /* M9X_SPLIT_SKIP_ABOVE / M9X_SPLIT_SKIP_BELOW drop a split job whose
    * target area is outside the range, which names which of a scene's split
    * shapes stops the engine.  M9X_SPLIT_TRACE names every split job with the
    * shape, clip window and draw extent it carries. */
   if (pass_count > 1) {
      uint64_t area = (uint64_t)ctx->params.width * ctx->params.height;

      if (!gpu_padding && (m9_split_trace || m9_split_clip || !m9_split_partial)) {
         uint64_t t_extent = m9_prof_begin();

         if (records) {
            const float *v = ctx->records.data;

            x0 = x1 = v[3] > 0.0f ? v[0] / v[3] : v[0];
            y0 = y1 = v[3] > 0.0f ? v[1] / v[3] : v[1];
            for (unsigned k = 1; k < records; k++) {
               const float *p = v + (uint64_t)k * ctx->record_floats;
               float iw = p[3] > 0.0f ? 1.0f / p[3] : 1.0f;
               float x = p[0] * iw, y = p[1] * iw;

               x0 = MIN2(x0, x); x1 = MAX2(x1, x);
               y0 = MIN2(y0, y); y1 = MAX2(y1, y);
            }
         }
         if (m9_split_clip) {
            uint32_t lox = key->clip_min & 0xffffu;
            uint32_t hix = key->clip_max & 0xffffu;
            uint32_t loy = key->clip_min >> 16;
            uint32_t hiy = key->clip_max >> 16;
            float top = (float)ctx->params.height - y1;
            float bot = (float)ctx->params.height - y0;
            uint32_t a = m9_split_clip;

            split_x0 = m9_clip_floor(x0, a, lox, hix);
            split_x1 = m9_clip_ceil(x1, a, split_x0 + a, hix);
            split_y0 = m9_clip_floor(top, a, loy, hiy);
            split_y1 = m9_clip_ceil(bot, a, split_y0 + a, hiy);
         }
         if (m9_split_trace)
            fprintf(stderr, "m9x splitjob %llu: %ux%u passes %u draws %u "
                    "records %u extent %.0f,%.0f..%.0f,%.0f clip %08x\n",
                    (unsigned long long)m9_prof_jobs, ctx->params.width,
                    ctx->params.height, pass_count, draws, records,
                    x0, y0, x1, y1, ctx->params.clip_max);
         /* A multi-pass job whose geometry leaves part of its render target
          * uncovered stops the engine, and no binding it carries changes
          * that: the record unbound, the sequence words cleared, the
          * back-end object cleared, a target and a record of its own and
          * the clip window narrowed to the geometry all stall alike
          * (MEASURED, splitdesk:32:1:5:1920x1200:5:420x420 against the
          * whole-target case, which renders).  The draw is skipped with a
          * reason rather than submitted; M9X_SPLIT_PARTIAL=1 submits it, so
          * the probe that reproduces the fault still can. */
         partial = !records || x0 > 0.5f || y0 > 0.5f ||
                   x1 < (float)ctx->params.width - 0.5f ||
                   y1 < (float)ctx->params.height - 0.5f;
         m9_prof_end(M9_PROF_EXTENT, t_extent);
      }
      /* Three answers to a draw that covers part of its target, in the order
       * they cost: pad it out to the target's extent and narrow the clip
       * window to the pixels it covers, which is what the engine counts;
       * hold it for a scratch surface of those pixels and copy that into
       * place; or refuse it by name.  M9X_SPLIT_PAD=0 and M9X_SPLIT_SCRATCH=0
       * step down the list, and M9X_SPLIT_PARTIAL=1 submits it as it
       * stands. */
      bool empty = false;

      if (partial && !m9_split_partial &&
          !m9_split_pad(ctx, key, &records, &split_x0, &split_y0, &split_x1,
                        &split_y1, x0, y0, x1, y1, &empty)) {
         const char *held;

         if (empty) {
            m9_job_state_clear(ctx);
            return 0;
         }
         held = m9_split_defer(ctx, key, records, x0, y0, x1, y1);
         if (!held)
            return 0;
         m9_job_state_drop(ctx,
                           "a multi-pass fragment shader needs a draw that "
                           "covers its whole render target; draws skipped",
                           held);
         return 0;
      }
      if ((m9_split_skip_above && area > m9_split_skip_above) ||
          (m9_split_skip_below && area < m9_split_skip_below)) {
         util_dynarray_foreach(&ctx->job_textures, struct pipe_resource *,
                               held)
            pipe_resource_reference(held, NULL);
         util_dynarray_clear(&ctx->job_textures);
         ctx->job_unit_dwords = 0;
         util_dynarray_clear(&ctx->job_vs_runs);
         util_dynarray_clear(&ctx->job_vs_values);
         util_dynarray_clear(&ctx->records);
         util_dynarray_clear(&ctx->indices);
         util_dynarray_clear(&ctx->draws);
         util_dynarray_clear(&ctx->extra);
         return 0;
      }
   }
   /* M9X_ONLY_FIRST / M9X_ONLY_AFTER drop every job of a frame outside that
    * range, which isolates the draws a screen region comes from. */
   if (ctx->frame_jobs++ < ctx->only_after ||
       ctx->frame_jobs > ctx->only_first) {
      util_dynarray_foreach(&ctx->job_textures, struct pipe_resource *, held)
         pipe_resource_reference(held, NULL);
      util_dynarray_clear(&ctx->job_textures);
      ctx->job_unit_dwords = 0;
      util_dynarray_clear(&ctx->job_vs_runs);
      util_dynarray_clear(&ctx->job_vs_values);
      util_dynarray_clear(&ctx->records);
      util_dynarray_clear(&ctx->indices);
      util_dynarray_clear(&ctx->draws);
      util_dynarray_clear(&ctx->extra);
      return 0;
   }
   unsigned index_count = util_dynarray_num_elements(&ctx->indices, uint16_t);
   /* Copies of the geometry the job preprocesses: one a raster pass. */
   unsigned copies = MAX2(pass_count, 1u);
   uint64_t index_base = 0;

   /* Complete-job aggregation retains independent immutable CPU arenas.
    * The first scope excludes mutable CPU clears, queries and split passes. */
   aggregate_job = ctx->aggregate && pass_count == 1 && !ctx->dry_run &&
      !ctx->in_scratch && !m9_pass_loop && !m9_be_clear && !m9_be_dump &&
      !m9_vs_record_dump && !m9_record_dump &&
      !m9_faa_active(&ctx->screen->faa) && !ctx->faa_surface.graph.initialized &&
      !ctx->query_active && !ctx->query_begin && !ctx->query_end &&
      !ctx->query_paused && !ctx->query_exclude;
   if ((!aggregate_job || ctx->aggregate->aggregate_jobs == 4) &&
       m9_aggregate_flush(ctx))
      return -EIO;
   /* Before a single byte of this job is written. */
   ret = m9_arena_rotate(ctx);
   if (ret)
      return ret;
   /* M9X_BE_CLEAR clears the engine's own state object, which separates
    * state the engine keeps there from the target that selects it.  It
    * clears the object the job actually binds: with a back-end object a
    * target, clearing the context's own left the bound one untouched. */
   struct m9_resource *be_bound = ctx->target_be ? ctx->target_be : &ctx->be;

   if (be_bound->map &&
       (m9_be_clear == 1 ||
        (m9_be_clear == 2 && ctx->be_clip != ctx->params.clip_max))) {
      m9_be_zero(ctx);
      be_bound = ctx->target_be ? ctx->target_be : &ctx->be;
      if (!ctx->lost)
         ctx->be_clip = ctx->params.clip_max;
   }
   /* The object a multi-pass job binds is cleared ahead of it: one that finds
    * what draws into another target left behind stops the engine, and a scene
    * with four windows walks between targets several times a frame (MEASURED,
    * splitdesk:8:4:11:1920x1200:1:420x420 stalls without this and renders 32
    * frames with it).  M9X_SPLIT_BE=0 leaves it alone. */
   {
      /* Every job the engine runs leaves its state in the object it binds, so
       * the shape is remembered for each of them and a split job clears only
       * when what is there is not its own.  M9X_BE_REUSE=0 clears ahead of
       * every split job, which is what it did before (a megabyte of VRAM
       * writes a job: 82 of refract's 200 ms a frame, MEASURED). */
      struct m9_be_shape shape = {
         .valid = true,
         .variant = key->variant,
         .color = ctx->color ? m9_res(ctx->color)->handle : 0,
         .color_offset = (uint32_t)ctx->color_offset,
         .depth = key->depth ? key->depth->handle : 0,
         .aux = key->aux ? key->aux->handle : 0,
         .color_desc = ctx->params.color_desc,
         .depth_desc = ctx->params.depth_desc,
         .clip_min = split_y0 << 16 | split_x0,
         .clip_max = split_y1 << 16 | split_x1,
         .color_count = ctx->color_count,
         .passes = pass_count,
      };
      struct m9_be_shape *held = m9_be_shape_slot(ctx);
      bool differs = !held->valid || !m9_be_shape_same(held, &shape);

      if (m9_split_be > 2 && pass_count > 1 &&
          (differs || !m9_be_reuse || !ctx->target_be)) {
         m9_be_zero(ctx);
         held = m9_be_shape_slot(ctx);
         differs = true;
      }
      be_bound = ctx->target_be ? ctx->target_be : &ctx->be;
      if (!ctx->lost && differs)
         *held = be_bound->map ? shape : (struct m9_be_shape) {0};
   }
   ret = ctx->lost ? -EIO :
         m9_arena_reserve(ctx, &ctx->source,
                          (uint64_t)records * copies * ctx->record_floats *
                          sizeof(float));
   if (!ret && ctx->indices.size)
      ret = m9_arena_reserve(ctx, &ctx->index, ctx->indices.size * copies);
   /* The CPU stream views are reserved for the varyings this job actually
    * gathers, so the job's record ceiling costs system memory only where a
    * fragment program reads them. */
   if (!ret && fs && key->variant->extra_count)
      ret = m9_arena_reserve(ctx, &ctx->cpu_streams,
                             (uint64_t)key->variant->extra_count *
                             m9_transient_view_bytes());
   if (!ret) {
      uint64_t t_records = m9_prof_begin();

      /* A split shader draws its geometry once a pass, each from its own
       * copy of the preprocessed records. */
      for (unsigned i = 0; i < copies; i++)
         memcpy((uint8_t *)ctx->source.map + i * ctx->records.size,
                ctx->records.data, ctx->records.size);
      for (unsigned i = 0; i < (ctx->indices.size ? copies : 0); i++) {
         uint16_t *out = (uint16_t *)ctx->index.map + i * index_count;
         const uint16_t *in = ctx->indices.data;

         for (unsigned k = 0; k < index_count; k++)
            out[k] = in[k] + i * records;
      }
      memcpy(ctx->system.map, ctx->params.system_prefix,
             sizeof(ctx->params.system_prefix));
      memcpy((uint8_t *)ctx->system.map + M9_LINKED_OFFSET,
             ctx->params.linked_prefix, sizeof(ctx->params.linked_prefix));
      m9_prof_end(M9_PROF_RECORDS, t_records);
      if (key->vs) {
         /* The compiled program replaces the transform: its own constants
          * take the matrix pairs, and the tail applies the viewport in
          * force rather than the target's own. */
         m9_draw_params_viewport(&ctx->params, key->viewport_scale,
                                 key->viewport_bias, ctx->system.map);
         vs_decl_count = m9_vs_decl_words(key->vs, vs_decl,
                                          ARRAY_SIZE(vs_decl));
         vs_const_pairs =
            m9_vs_const_span(key->vs, ctx->job_vs_consts.data,
                             util_dynarray_num_elements(&ctx->job_vs_consts,
                                                        float),
                             (uint32_t *)((uint8_t *)ctx->system.map +
                                          M9_VS_CONST_OFFSET),
                             2 * M9_VS_CONST_MAX_PAIRS);
         if (vs_const_pairs && m9_vs_const_pad) {
            unsigned pad = MIN2(vs_const_pairs + m9_vs_const_pad,
                                M9_VS_CONST_MAX_PAIRS);

            memset((uint8_t *)ctx->system.map + M9_VS_CONST_OFFSET +
                   8 * vs_const_pairs, 0, 8 * (pad - vs_const_pairs));
            vs_const_pairs = pad;
         }
         if (!vs_decl_count || !vs_const_pairs)
            ret = -EINVAL;
         unsigned runs = util_dynarray_num_elements(&ctx->job_vs_runs,
                                                    struct m9_vs_const_run);
         struct m9_vs_const_run *run = ctx->job_vs_runs.data;
         if (!ret && gpu_padding) {
            struct m9_vs_padding_bounds bounds;
            struct m9_vs_padding_upload upload;
            unsigned app = records - M9_VS_PADDING_VERTICES;
            if (!m9_gpu_padding_bounds(ctx, key, key->viewport_scale,
                                       key->viewport_bias, &bounds) ||
                !m9_vs_padding_upload_build(key->vs, &bounds, app,
                       m9_draw_max_source_vertices() / copies,
                       (uint32_t *)((uint8_t *)ctx->system.map + M9_VS_CONST_OFFSET),
                       vs_const_pairs, &upload) ||
                runs != 2 * copies ||
                memcmp((uint8_t *)ctx->records.data + app * upload.record_bytes,
                       upload.records, M9_VS_PADDING_VERTICES * upload.record_bytes))
               ret = -EINVAL;
            else {
               memcpy((uint8_t *)ctx->system.map + M9_VS_CONST_OFFSET,
                      upload.constants, 8 * vs_const_pairs);
               memcpy((uint8_t *)ctx->system.map + M9_VS_RUN_CONST_OFFSET,
                      upload.constants + 2 * vs_const_pairs, 8 * vs_const_pairs);
            }
         }
         for (unsigned i = 0; !ret && !gpu_padding && i < runs; i++) {
            uint32_t offset = i ? M9_VS_RUN_CONST_OFFSET +
                                 (i - 1) * M9_VS_RUN_CONST_BYTES :
                                 M9_VS_CONST_OFFSET;
            uint8_t *dst = (uint8_t *)ctx->system.map + offset;
            if (i) {
               /* Copy every tail pair, including guard/viewport values.
                * m9_vs_const_span replaces only the shader's own pairs. */
               memcpy(dst, (uint8_t *)ctx->system.map + M9_VS_CONST_OFFSET,
                      8 * vs_const_pairs);
               if (!m9_vs_const_span(key->vs,
                     (float *)((uint8_t *)ctx->job_vs_values.data +
                               i * ctx->job_vs_consts.size),
                     ctx->job_vs_consts.size / sizeof(float),
                     (uint32_t *)dst, 2 * M9_VS_CONST_MAX_PAIRS))
                  ret = -EINVAL;
            }
            run[i].count = (i + 1 < runs ? run[i + 1].first : records) -
                           run[i].first;
            run[i].const_offset = offset;
            run[i].const_pairs = vs_const_pairs;
         }
      }
      if (fs) {
         const char *why = NULL;

         /* A split shader needs the record before its first pass runs. */
         if (pass_count > 1 &&
             !m9_record_grow(ctx, key->variant->record_dwords, &why)) {
            m9_warn_once(ctx, M9_WARN_MEMORY, why);
            m9_skip_reason(ctx, why);
            ret = -EINVAL;
         } else {
            ret = m9_job_native(ctx, key, records,
                                index_count * sizeof(uint16_t), passes);
         }
      }
      job = (struct m9_draw_job) {
         .params = &ctx->params,
         .decl = &ctx->decl,
         .source = ctx->source.handle,
         .index = ctx->index.handle,
         .system = ctx->system.handle,
         .program = ctx->program.handle,
         .fragment_db = ctx->fragment_db.handle,
         .transient = ctx->transient.handle,
         .cpu_streams = ctx->cpu_streams.handle,
         .color = m9_res(ctx->color)->handle,
         .color_offset = ctx->color_offset,
         .depth = key->depth->handle,
         .aux = key->aux->handle,
         .be = be_bound->handle,
         .source_vertices = records * copies,
         .index_base = (uint32_t)index_base,
         .index_count = index_count * copies,
         .draws = ctx->draws.data,
         .draw_count = draws,
         .vs_const_runs = ctx->job_vs_runs.data,
         .vs_const_run_count = util_dynarray_num_elements(&ctx->job_vs_runs,
                                                        struct m9_vs_const_run),
         .vs_gpu_padding = gpu_padding,
         .depth_control = key->depth_control,
         /* A single pass carries its own depth bits for gl_FragDepth. */
         .depth_ef = pass_count == 1 && !ret ? passes[0].depth_ef :
                                                key->depth_ef,
         .depth_bias = key->depth_bias,
         .depth_bias_units = key->depth_bias_units,
         .depth_bias_factor = key->depth_bias_factor,
         .stencil = key->stencil,
         .stencil_a9 = key->stencil_a9,
         .stencil_aa = key->stencil_aa,
         .color_mask = key->color_mask,
         .color_mask_hi = key->color_mask_hi,
         .color_count = key->single_target ? MIN2(ctx->color_count, 1u) :
                                             ctx->color_count,
         .clip_min = split_y0 << 16 | split_x0,
         .clip_max = split_y1 << 16 | split_x1,
         /* One pass is emitted through the single-pass path, so the
          * captured stream is unchanged; more become a pass loop.
          * M9X_PASS_LOOP drives a one-pass shader through the loop too,
          * which separates a fault in the loop from one in the record. */
         .fs = pass_count == 1 && !m9_pass_loop ? &passes[0].fs : NULL,
         .split_30 = m9_split_30,
         .passes = pass_count > 1 || (pass_count && m9_pass_loop) ?
                   passes : NULL,
         .pass_count = pass_count > 1 || (pass_count && m9_pass_loop) ?
                       pass_count : 0,
         .units = first_draw->unit_count ? first_draw->units : NULL,
         .unit_count = first_draw->unit_count,
      };
      for (unsigned s = 1; s < job.color_count; s++) {
         job.color_extra[s - 1] = m9_res(ctx->color_extra[s - 1])->handle;
         job.color_extra_desc[s - 1] = ctx->color_extra_desc[s - 1];
      }
      memcpy(job.color_class, ctx->color_class, sizeof(job.color_class));
      if (m9_faa_active(&ctx->screen->faa)) {
         job.faa.set = true;
         job.faa.raster = ctx->screen->faa.raster;
         job.faa.ad = ctx->screen->faa.store_bind &&
                      !ctx->screen->faa.ad_forced ? 0 :
                      m9_faa_ad(&ctx->screen->faa,
                         (m9_faa_fragment_bytes(ctx->params.width,
                                                ctx->params.height) + 127) &
                         ~127ull);
         job.faa.af = m9_faa_af(&ctx->screen->faa);
         /* One call fills every edited index: wiring ae alone left ef and
          * f0 written as zero, which cleared the cull, depth and format
          * bits the pass built and rendered nothing (MEASURED). */
         m9_faa_edits(&ctx->screen->faa, &job.faa.edit);
         /* Batches retain integer handles only. Prevent another context from
          * closing/replacing this group before all of this job's ioctls take
          * their kernel references, including separately submitted passes. */
         simple_mtx_lock(&ctx->screen->faa_store_lock);
         faa_store_locked = true;
         if (!m9_faa_store_grow_locked(ctx) ||
             !m9_faa_store_bind_job_locked(ctx, &job)) {
            /* An explicitly requested FAA group must never degrade to AA
             * mode bits without its backing resources. Do not emit or
             * submit this job, even when an older smaller group survives. */
            ret = -EINVAL;
         } else {
            if (ctx->screen->faa.companion_format >= 0) {
               struct m9_surface_desc desc = {
                  .row_span = ctx->params.depth_desc & 0xffffu,
                  .height = ctx->params.height, .layers = 1, .layout = 1,
                  .format = (uint8_t)ctx->screen->faa.companion_format,
                  .address_offset = true,
               };

               job.faa.companion = ctx->screen->faa_companion.handle;
               job.faa.companion_desc = m9_surface_desc0(&desc);
               if (ctx->screen->faa.companion_format == 0x3d) {
                  struct m9_faa_companion_layout layout;

                  if (!m9_faa_companion_layout(ctx->params.width,
                                               ctx->params.height, &layout))
                     ret = -EINVAL;
                  else
                     job.faa.companion_desc = layout.descriptor;
               }
            }
            job.faa.bind_index = ctx->screen->faa.bind_index;
            job.faa.bind_desc = ctx->screen->faa.bind_desc;
            job.faa.bind_reset = ctx->screen->faa.bind_reset;
         }
      }
      if (fs) {
         job.program = key->variant->c2.handle;
         job.blend_dest = m9_fs_final(key->variant)->blend_dest;
         job.logic_op = m9_fs_final(key->variant)->logic_op;
      }
      if (pass_count > 1 || (pass_count && m9_pass_loop)) {
         /* The unit pool of a split job is the key's, not a draw's: its
          * passes index the pool and a draw cannot restate it. */
         job.units = key->unit_count ? key->units : NULL;
         job.unit_count = key->unit_count;
         job.record = ctx->screen->record.handle;
         /* M9X_SPLIT_PREPROCESS=0 keeps one preprocessing range for the
          * whole job, which is what the vendor emits; the default runs one
          * ahead of every raster pass. */
         job.preprocess_per_pass = m9_split_preprocess;
         if (pass_count > 1 && m9_split_submit)
            job.pass_count = 1;
         m9_texture_unit_placeholder(&placeholder,
                                     ctx->placeholder_texture.handle);
         job.placeholder = &placeholder;
         /* The vendor's inter-draw engine transition, which a single-pass
          * job never carries so the captured stream stays byte for byte. */
         job.transition = m9_transition;
      }
      if (key->vs) {
         job.program = key->vs_program;
         job.vs_decl = vs_decl;
         job.vs_decl_count = vs_decl_count;
         job.vs_record_bytes = key->vs->record_bytes;
         job.vs_unit = key->vs_texture ? &key->vs_unit : NULL;
         job.vs_unit_second = key->vs_texture_second ? &key->vs_unit_second : NULL;
         job.vs_texture_head_only = key->vs && key->vs->texture_head_only;
         job.vs_sampled_position = key->vs->sampled_position;
         if (job.vs_sampled_position) {
            job.vs_producer_targets = 2;
            job.vs_head_targets = 4;
            memcpy(job.vs_producer_fields, key->vs->producer_fields,
                   sizeof(job.vs_producer_fields));
            memcpy(job.vs_head_fields, key->vs->extra_fields,
                   sizeof(job.vs_head_fields));
         }
         job.vs_const_pairs = vs_const_pairs;
         /* Nothing writes the CPU views of a native job, so its stream
          * slots stay local and the second preprocessing draw can render
          * varyings 1..4 into the views above M9_TRANSIENT_VS_EXTRA_VIEW. */
         job.cpu_streams = 0;
         job.vs_extra_entry = key->vs_extra_entry;
         job.vs_producer_entry = key->vs_producer_entry;
         job.vs_producer_mask = m9_vs_source_mask(key->vs, true);
         job.vs_fused_mask_set = !key->vs_producer_entry;
         job.vs_fused_mask = m9_vs_fused_source_mask(key->vs);
         job.vs_extra_mask = key->vs_producer_entry ?
                             m9_vs_source_mask(key->vs, false) : 0xffff;
         /* Only the packed record needs the draw that de-interleaves it. */
         job.vs_gather_entry = m9_vs_record_mode() >= 2 ?
                               key->vs_gather_entry : 0;
         job.vs_extra_targets = key->vs_extra_entry ?
                                key->vs_extra_targets : 0;
         job.vs_record_view = M9_TRANSIENT_VS_RECORD_VIEW;
         job.vs_fused_view = key->vs_fused_pair ?
                             M9_TRANSIENT_VS_EXTRA_VIEW : 0;
         for (unsigned t = 0; t < job.vs_extra_targets; t++)
            job.vs_extra_view[t] = M9_TRANSIENT_VS_EXTRA_VIEW +
                                   M9_VS_FUSED_VARYINGS - 1 + t;
      }
      if (!ret && job.vs_producer_entry &&
          !m9_vs_record_fits(job.source_vertices, job.vs_extra_targets + 3u,
                             job.vs_record_view, m9_transient_view_bytes(),
                             ctx->transient.size))
         ret = -EINVAL;
      /* A query's snapshot records bracket this job's draws inside its own
       * stream, where the closing one can wait for them to retire. */
      if (!ret)
         ret = m9_query_job_prologue(ctx, &ctx->batch);
      if (!ret) {
         uint64_t t_batch = m9_prof_begin();

         ret = m9_batch_emit_draw_job(&ctx->batch, &job);
         m9_prof_end(M9_PROF_BATCH, t_batch);
      }
      if (!ret)
         ret = m9_query_job_epilogue(ctx, &ctx->batch,
                                     m9_draw_job_final_dependency(&job));
   }
   built = !ret;
   if (!ret) {
      uint64_t t_ioctl;

      ctx->batch.sealed = true;
      if (ctx->dump_path)
         m9_dump_job(ctx, fs ? key->variant : NULL, &job);
      t_ioctl = m9_prof_begin();
      if (aggregate_job) {
         ret = m9_aggregate_enqueue(ctx, be_bound);
         deferred_submit = !ret;
         /* An unsupported single job stays on the established path. */
         if (ret == -EINVAL || ret == -EOPNOTSUPP || ret == -ENOSPC) {
            ret = m9_aggregate_flush(ctx);
            if (!ret) ret = m9_batch_submit(ctx->screen, &ctx->command,
                                           &ctx->batch, ctx->context_id, &point);
         }
      } else {
         ret = ctx->dry_run ?
               m9_batch_validate(ctx->screen, &ctx->command, &ctx->batch,
                                 ctx->context_id) :
               m9_batch_submit(ctx->screen, &ctx->command, &ctx->batch,
                               ctx->context_id, &point);
      }
      if (!ret && !ctx->dry_run && !deferred_submit)
         m9_job_submitted(ctx, be_bound, point);
      if (faa_store_locked && !ctx->dry_run)
         m9_faa_store_submitted_locked(ctx->screen, &job, ret, point);
      /* A second raster pass inside one submission stalls the engine
       * (MEASURED): the vendor separates its passes with an engine
       * transition the kernel does not accept.  Every pass after the
       * first is therefore submitted on its own, which the transport
       * serialises against the one before it. */
      unsigned emit = m9_split_passes ? MIN2(m9_split_passes, copies) :
                      copies;

      for (unsigned pi = 1; !ret && m9_split_submit && pi < emit; pi++) {
         uint64_t t_batch = m9_prof_begin();

         m9_batch_init(&ctx->batch);
         job.passes = &passes[pi];
         job.pass_count = 1;
         ret = m9_batch_emit_draw_job(&ctx->batch, &job);
         m9_prof_end(M9_PROF_BATCH, t_batch);
         if (ret)
            break;
         ctx->batch.sealed = true;
         if (ctx->dump_path)
            m9_dump_job(ctx, key->variant, &job);
         ret = ctx->dry_run ?
               m9_batch_validate(ctx->screen, &ctx->command, &ctx->batch,
                                 ctx->context_id) :
               m9_batch_submit(ctx->screen, &ctx->command, &ctx->batch,
                               ctx->context_id, &point);
         if (!ret && !ctx->dry_run)
            m9_job_submitted(ctx, be_bound, point);
         if (faa_store_locked && !ctx->dry_run)
            m9_faa_store_submitted_locked(ctx->screen, &job, ret, point);
      }
      m9_prof_end(M9_PROF_IOCTL, t_ioctl);
      if (m9_be_dump && ctx->be.map) {
         char path[256];
         FILE *f;

         snprintf(path, sizeof(path), "%s.%u", m9_be_dump, m9_be_seq++);
         f = fopen(path, "w");
         if (f) {
            fwrite(ctx->be.map, 1, ctx->be.size, f);
            fclose(f);
         }
      }
   }
   if (faa_store_locked)
      simple_mtx_unlock(&ctx->screen->faa_store_lock);
   if (ret == -EINVAL) {
      /* A refused job is one unsupported state, not a broken context. */
      const char *who = built ? "the kernel refused a job; those draws are "
                                "skipped" :
                                "a job could not be built; those draws are "
                                "skipped";

      m9_warn_once(ctx, M9_WARN_SUBMIT, who);
      m9_skip_reason(ctx, who);
      ctx->skipped_draws++;
      m9_prof_refused++;
      m9_prof_skipped += draws;
      /* Name the refused job so the failing validator check is identifiable. */
      if (ctx->debug)
         fprintf(stderr, "m9x: refused job (%s): words %u relocs %u bos %u "
                 "records %u draws %u units %u fs %d blend_dest %d "
                 "indices %u base %#x first %u+%u\n",
                 built ? "kernel" : "build",
                 ctx->batch.command_count, ctx->batch.reloc_count,
                 ctx->batch.bo_count, records, draws, key->unit_count,
                 !!fs, fs ? (int)m9_fs_final(key->variant)->blend_dest : -1,
                 index_count, (unsigned)index_base, first_draw->index_first,
                 first_draw->index_count);
   } else if (ret) {
      if (!ctx->lost)
         fprintf(stderr, "m9x: two-pass job failed (%s); context lost\n",
                 strerror(-ret));
      ctx->lost = true;
   } else {
      if (!deferred_submit) ctx->last_point = point;
      ctx->record_written |= pass_count > 1;
      if (key->vs_producer_entry && !ctx->dry_run) {
         ctx->vs_record_vertices = job.source_vertices;
         ctx->vs_record_fields = job.vs_extra_targets + 3;
         ctx->vs_record_view = job.vs_record_view;
         ctx->vs_record_job_seq = ctx->dump_native_seq;
         ctx->vs_record_inputs_dumped = ctx->dump_native_valid;
         ctx->vs_record_point = point;
         ctx->vs_record_source = job.source;
         ctx->vs_record_index = job.index;
         ctx->vs_record_program = job.program;
         if (ctx->debug)
            fprintf(stderr, "m9x: native route chain fields %u vertices %u "
                    "entries %u/%u/%u/%u masks %04x/%04x -> %#" PRIx64 "\n",
                    ctx->vs_record_fields, ctx->vs_record_vertices,
                    job.vs_producer_entry, job.vs_extra_entry, job.vs_gather_entry,
                    job.fs ? job.fs->vertex_entry : passes[0].fs.vertex_entry,
                    job.vs_producer_mask, job.vs_extra_mask, point);
      } else {
         ctx->vs_record_vertices = 0;
      }
      m9_prof_relocs = MAX2(m9_prof_relocs, ctx->batch.reloc_count);
      if (ctx->debug)
         fprintf(stderr, "m9x: job %u records %u draws vsruns %u a8 %08x ef %08x "
                 "ab %08x clip %08x/%08x -> %#" PRIx64 "\n", records, draws,
                 job.vs_const_run_count,
                 key->depth_control, key->depth_ef, key->color_mask,
                 key->clip_min, key->clip_max, point);
   }
   util_dynarray_foreach(&ctx->job_textures, struct pipe_resource *, held)
      pipe_resource_reference(held, NULL);
   util_dynarray_clear(&ctx->job_textures);
   ctx->job_unit_dwords = 0;
   util_dynarray_clear(&ctx->job_vs_runs);
   util_dynarray_clear(&ctx->job_vs_values);
   util_dynarray_clear(&ctx->records);
   util_dynarray_clear(&ctx->indices);
   util_dynarray_clear(&ctx->draws);
   util_dynarray_clear(&ctx->extra);
   m9_prof_end(M9_PROF_SUBMIT, t_submit);
   return ret;
}

/* Space for vertices source records of one draw in a job with this key. */
static bool
m9_consts_differ(const struct util_dynarray *a, const struct util_dynarray *b)
{
   return a->size != b->size || (a->size && memcmp(a->data, b->data, a->size));
}

static bool
m9_vs_uniforms_batchable(const struct m9_job_key *key)
{
   return m9_vs_uniform_batch && key->vs && !key->vs_extra_entry &&
          (!key->fs || key->variant->passes.count == 1);
}

/* Compare against the latest snapshot while keeping job_vs_consts as the
 * first run's values for the original single-span submission path. */
static bool
m9_vs_uniforms_differ(struct m9_context *ctx,
                      const struct util_dynarray *values)
{
   if (values->size != ctx->job_vs_consts.size)
      return true;
   const void *last = ctx->job_vs_values.size ?
      (uint8_t *)ctx->job_vs_values.data + ctx->job_vs_values.size -
         values->size : ctx->job_vs_consts.data;
   return values->size && memcmp(values->data, last, values->size);
}

/* Source records one job can hold under this key.  A split shader
 * preprocesses one copy of the geometry per raster pass, so its share of the
 * transient stream views is the ceiling divided by the pass count, less the
 * padding a partial draw reaches its target's extent with.  Every producer
 * bounds its runs by this, not by the whole ceiling: a run sized for the
 * whole one is refused by m9_job_reserve below, and the geometry is then
 * lost. */
static unsigned
m9_job_max_vertices(const struct m9_job_key *key)
{
   unsigned copies = key->variant && key->fs ? key->variant->passes.count : 1;
   unsigned max = m9_draw_max_source_vertices() / MAX2(copies, 1u);

   if (copies > 1)
      max = max > M9_PAD_RECORDS ? max - M9_PAD_RECORDS : 0;
   return max & ~3u;
}

static float *
m9_job_reserve(struct m9_context *ctx, const struct m9_job_key *key,
               const struct util_dynarray *consts,
               const struct util_dynarray *vs_consts, unsigned record_floats,
               unsigned vertices, uint32_t *first, float **extra)
{
   unsigned padded = m9_draw_preprocess_vertices(vertices);
   unsigned extra_count = key->variant && !key->vs ?
                          key->variant->extra_count : 0;
   unsigned dwords;
   unsigned draws = util_dynarray_num_elements(&ctx->draws,
                                               struct m9_draw_record);
   unsigned records = draws ?
                      util_dynarray_num_elements(&ctx->records, float) /
                      ctx->record_floats : 0;
   float *out;

   /* A split shader preprocesses one copy of the geometry a pass. */
   unsigned copies = key->variant && key->fs ? key->variant->passes.count : 1;
   unsigned ceiling = m9_job_max_vertices(key);

   if (padded > ceiling || !record_floats)
      return NULL;
   /* The bound textures are restated per draw, so they need not end a job;
    * only their extra stream words have to fit the command budget. */
   unsigned passes = copies;
   unsigned pre_copies = m9_split_preprocess ? MAX2(passes, 1u) : 1u;
   bool vs_batchable = m9_vs_uniforms_batchable(key);
   bool vs_changed = key->vs && m9_vs_uniforms_differ(ctx, vs_consts);
   unsigned vs_runs = util_dynarray_num_elements(&ctx->job_vs_runs,
                                                 struct m9_vs_const_run);
   unsigned new_vs_runs = vs_batchable ?
                         vs_runs + (!vs_runs || vs_changed) : 0;
   bool units_differ = key->unit_count != ctx->job_key.unit_count ||
                       memcmp(key->units, ctx->job_key.units,
                              key->unit_count * sizeof(key->units[0]));
   unsigned unit_dwords = ctx->job_unit_dwords +
                          (draws && units_differ && passes == 1 ?
                           m9_draw_units_dwords(key->unit_count) : 0);

   dwords = unit_dwords +
            (key->fs ?
             (passes > 1 ? m9_variant_dwords(key->variant, draws + 1) :
              m9_draw_job_dwords_fs(draws + 1, key->unit_count)) :
             m9_draw_job_dwords(draws + 1)) +
            m9_draw_decl_dwords(key->vs ? 2 * key->vs->attr_count +
                                        key->vs->texture_count + 1 : 5) +
            (key->vs && key->vs_extra_entry ?
             pre_copies * (m9_draw_vs_extra_dwords(key->vs_extra_targets,
                                     2 * key->vs->attr_count + 1) +
             (key->vs_producer_entry ?
              m9_draw_vs_producer_dwords(2 * key->vs->attr_count + 1) : 0)) : 0) +
            m9_faa_raster_dwords(&ctx->screen->faa) * MAX2(passes, 1u) +
            (key->depth_bias ? M9_DEPTH_BIAS_DWORDS : 0);
   /* Every additional run adds a five-word constant packet and a numeric
    * three-word launch. Framing is charged by the existing upper bound. */
   dwords += new_vs_runs ? 8 * (new_vs_runs - 1) : 0;
   if (key->vs && key->vs->gpu_padding) {
      unsigned stages = key->vs_producer_entry ? 4 : key->vs_extra_entry ? 3 : 1;
      dwords += 8 * (2 * copies - 1) * stages * pre_copies;
   }
   dwords += pre_copies * 18 * (key->vs_texture + key->vs_texture_second);
   if (draws) {
      bool state = memcmp(key, &ctx->job_key,
                          offsetof(struct m9_job_key, units)) != 0;

      if (state && (ctx->debug || m9_profile_period)) {
         const struct m9_job_key *o = &ctx->job_key;

         ctx->split_target += key->depth != o->depth || key->aux != o->aux;
         ctx->split_program += key->fs != o->fs || key->variant != o->variant;
         ctx->split_depth += key->depth_control != o->depth_control ||
                             key->depth_ef != o->depth_ef;
         ctx->split_mask += key->color_mask != o->color_mask;
         ctx->split_clip += key->clip_min != o->clip_min ||
                            key->clip_max != o->clip_max;
         m9_prof_split[0] += key->depth != o->depth || key->aux != o->aux;
         m9_prof_split[1] += key->fs != o->fs || key->variant != o->variant;
         m9_prof_split[2] += key->depth_control != o->depth_control ||
                             key->depth_ef != o->depth_ef;
         m9_prof_split[3] += key->color_mask != o->color_mask;
         m9_prof_split[4] += key->clip_min != o->clip_min ||
                             key->clip_max != o->clip_max;
      }
      bool uniforms = (key->fs && m9_consts_differ(consts, &ctx->job_consts)) ||
                      (vs_changed && (!vs_batchable ||
                       vs_consts->size != ctx->job_vs_consts.size)) ||
                      record_floats != ctx->record_floats;
      bool vertices_full = records + padded > ceiling;
      unsigned relocs = (passes > 1 ?
                         m9_variant_relocs(key->variant, draws + 1) :
                         m9_draw_job_relocs(draws + 1, key->unit_count)) +
                        (key->vs && key->vs_extra_entry ?
                         pre_copies * (m9_draw_vs_extra_relocs(key->vs_extra_targets) +
                         (key->vs_producer_entry ?
                          m9_draw_vs_producer_relocs() : 0)) : 0) +
                        m9_faa_raster_relocs(&ctx->screen->faa) *
                        MAX2(passes, 1u);
      relocs += new_vs_runs ? 2 * (new_vs_runs - 1) : 0;
      if (key->vs && key->vs->gpu_padding) {
         unsigned stages = key->vs_producer_entry ? 4 : key->vs_extra_entry ? 3 : 1;
         relocs += 2 * (2 * copies - 1) * stages * pre_copies;
      }
      relocs += pre_copies * (key->vs_texture + key->vs_texture_second);
      /* A split shader binds one unit pool for the whole job, so a draw
       * that needs another set starts a job of its own. */
      bool stream_full = (key->vs && key->vs->gpu_padding) ||
                         dwords > M9_MAX_COMMAND_DWORDS ||
                         m9_draw_framed_dwords(dwords, draws) >
                         M9_MAX_FRAMED_DWORDS ||
                         relocs > M9_MAX_RELOCS ||
                         (passes > 1 && units_differ) ||
                         (passes > 1 && draws >= m9_split_draws) ||
                         (m9_job_draws && draws >= m9_job_draws);
      stream_full |= new_vs_runs > M9_VS_MAX_CONST_RUNS;

      if (state || uniforms || vertices_full || stream_full) {
         m9_prof_split[5] += !state && uniforms;
         m9_prof_split[6] += !state && !uniforms && vertices_full;
         m9_prof_split[7] += !state && !uniforms && !vertices_full;
         ctx->split_state += state;
         ctx->split_uniforms += !state && uniforms;
         ctx->split_vertices += !state && !uniforms && vertices_full;
         ctx->split_stream += !state && !uniforms && !vertices_full;
         m9_submit_job(ctx);
         records = 0;
         unit_dwords = 0;
      }
   }
   if (ctx->lost)
      return NULL;
   if (!draws || records == 0) {
      util_dynarray_clear(&ctx->job_consts);
      if (key->fs && consts->size &&
          !util_dynarray_grow_bytes(&ctx->job_consts, 1, consts->size))
         return NULL;
      if (key->fs && consts->size)
         memcpy(ctx->job_consts.data, consts->data, consts->size);
      util_dynarray_clear(&ctx->job_vs_consts);
      if (key->vs && vs_consts->size &&
          !util_dynarray_grow_bytes(&ctx->job_vs_consts, 1, vs_consts->size))
         return NULL;
      if (key->vs && vs_consts->size)
         memcpy(ctx->job_vs_consts.data, vs_consts->data, vs_consts->size);
      ctx->record_floats = record_floats;
   }
   ctx->job_key = *key;
   ctx->job_unit_dwords = unit_dwords;
   for (unsigned k = 0; k < key->unit_count; k++) {
      struct pipe_resource **held =
         util_dynarray_grow(&ctx->job_textures, struct pipe_resource *, 1);

      if (!held)
         return NULL;
      *held = NULL;
      pipe_resource_reference(held, key->unit_res[k]);
   }
   for (unsigned unit = 0; unit < key->vs_texture + key->vs_texture_second; unit++) {
      struct pipe_resource **held =
         util_dynarray_grow(&ctx->job_textures, struct pipe_resource *, 1);
      if (!held)
         return NULL;
      *held = NULL;
      pipe_resource_reference(held, unit ? key->vs_unit_res_second : key->vs_unit_res);
   }
   *extra = NULL;
   if (extra_count) {
      *extra = util_dynarray_grow(&ctx->extra, float,
                                  padded * 4 * extra_count);
      if (!*extra)
         return NULL;
   }
   out = util_dynarray_grow(&ctx->records, float, padded * record_floats);
   if (!out) {
      m9_warn_once(ctx, M9_WARN_MEMORY, "out of memory for source records");
      return NULL;
   }
   if (vs_batchable) {
      vs_runs = util_dynarray_num_elements(&ctx->job_vs_runs,
                                          struct m9_vs_const_run);
      if (!vs_runs || m9_vs_uniforms_differ(ctx, vs_consts)) {
         size_t before = ctx->job_vs_values.size;
         void *values = vs_consts->size ?
            util_dynarray_grow_bytes(&ctx->job_vs_values, 1,
                                     vs_consts->size) : NULL;
         struct m9_vs_const_run *run = NULL;
         if (!vs_consts->size || values)
            run = util_dynarray_grow(&ctx->job_vs_runs,
                                     struct m9_vs_const_run, 1);
         if (!run) {
            ctx->job_vs_values.size = before;
            ctx->records.size = (size_t)records * record_floats * sizeof(float);
            if (extra_count)
               ctx->extra.size = (size_t)records * 4 * extra_count * sizeof(float);
            return NULL;
         }
         if (vs_consts->size)
            memcpy(values, vs_consts->data, vs_consts->size);
         *run = (struct m9_vs_const_run) { .first = records };
      }
   }
   *first = records;
   return out;
}

static void
m9_job_commit(struct m9_context *ctx, float *out, float *extra,
              unsigned vertices, uint32_t first, uint32_t index_first,
              uint32_t index_count, enum m9_topology topology,
              uint32_t cull, bool lines, bool flat)
{
   unsigned padded = m9_draw_preprocess_vertices(vertices);
   unsigned extra_floats = 4 * (ctx->job_key.variant && !ctx->job_key.vs ?
                                ctx->job_key.variant->extra_count : 0);
   unsigned floats = ctx->record_floats;
   struct m9_draw_record *draw;

   /* Pad to the next four-record boundary with the last vertex. */
   for (unsigned i = vertices; i < padded; i++) {
      memcpy(out + i * floats, out + (vertices - 1) * floats,
             floats * sizeof(float));
      if (extra)
         memcpy(extra + i * extra_floats, extra + (vertices - 1) * extra_floats,
                extra_floats * sizeof(float));
   }
   m9_prof_draws++;
   draw = util_dynarray_grow(&ctx->draws, struct m9_draw_record, 1);
   if (!draw) {
      /* Shrinking never reallocates; drop the orphaned records. */
      ctx->records.size = (size_t)first * floats * sizeof(float);
      ctx->extra.size = (size_t)first * extra_floats * sizeof(float);
      ctx->indices.size = (size_t)index_first * sizeof(uint16_t);
      while (ctx->job_vs_runs.size) {
         struct m9_vs_const_run *runs = ctx->job_vs_runs.data;
         unsigned n = util_dynarray_num_elements(&ctx->job_vs_runs,
                                                  struct m9_vs_const_run);
         if (runs[n - 1].first < first)
            break;
         ctx->job_vs_runs.size -= sizeof(*runs);
      }
      ctx->job_vs_values.size =
         util_dynarray_num_elements(&ctx->job_vs_runs,
                                    struct m9_vs_const_run) *
         ctx->job_vs_consts.size;
      m9_warn_once(ctx, M9_WARN_MEMORY, "out of memory for draw records");
      return;
   }
   /* M9X_DEBUG_TEXTURES: the screen extent a draw covers and the texture it
    * samples, so a region of the image can be traced back to its texture. */
   if (m9_debug_textures && !ctx->job_key.vs) {
      float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;

      /* Records hold window coordinates premultiplied by 1/w. */
      for (unsigned i = 0; i < vertices; i++) {
         const float *r = out + i * M9_RECORD_FLOATS;
         float iw = r[3] != 0.0f ? 1.0f / r[3] : 0.0f;
         float x = r[0] * iw, y = ctx->params.height - r[1] * iw;

         x0 = MIN2(x0, x); x1 = MAX2(x1, x);
         y0 = MIN2(y0, y); y1 = MAX2(y1, y);
      }
      fprintf(stderr, "m9x: draw %.0f,%.0f-%.0f,%.0f tex %u/%u units %u\n",
              x0, y0, x1, y1, ctx->job_key.unit_count ?
              ctx->job_key.units[0].handle : 0,
              ctx->job_key.unit_count > 1 ? ctx->job_key.units[1].handle : 0,
              ctx->job_key.unit_count);
   }
   *draw = (struct m9_draw_record) {
      .start = first,
      .count = vertices,
      .index_first = index_first,
      .index_count = index_count,
      .topology = topology,
      .cull = cull,
      .lines = lines,
      .flat = flat,
      /* A draw restates the units of a single-pass shader; a split one
       * binds one pool for the whole job, which the job carries. */
      .unit_count = MIN2(ctx->job_key.unit_count, M9_TEXTURE_MAX_UNITS),
   };
   memcpy(draw->units, ctx->job_key.units,
          draw->unit_count * sizeof(draw->units[0]));
}

static struct m9_resource *
m9_context_depth(struct m9_context *ctx)
{
   const struct pipe_surface *zsbuf = &ctx->framebuffer.zsbuf;
   struct m9_resource *zs = zsbuf->texture ? m9_res(zsbuf->texture) : NULL;

   return zs && zs->hz && !zsbuf->level && zs->width == ctx->params.width &&
          zs->height == ctx->params.height ? zs : NULL;
}

static bool
m9_zs_has_stencil(const struct m9_context *ctx)
{
   return util_format_has_stencil(
      util_format_description(ctx->framebuffer.zsbuf.format));
}

static void
m9_stencil_face(const struct pipe_stencil_state *s, uint8_t ref,
                struct m9_stencil_face *f)
{
   *f = (struct m9_stencil_face) {
      .func = s->func, .fail_op = s->fail_op, .zfail_op = s->zfail_op,
      .zpass_op = s->zpass_op, .ref = ref, .valuemask = s->valuemask,
      .writemask = s->writemask,
   };
}

static bool
m9_scissor_key(struct m9_context *ctx, const struct pipe_scissor_state *s,
               struct m9_job_key *key)
{
   uint32_t minx = MIN2(s->minx, ctx->params.width);
   uint32_t miny = MIN2(s->miny, ctx->params.height);
   uint32_t maxx = MIN2(s->maxx, ctx->params.width);
   uint32_t maxy = MIN2(s->maxy, ctx->params.height);

   if (minx >= maxx || miny >= maxy)
      return m9_skip_reason(ctx, "the scissor rectangle is empty; "
                            "draw skipped");
   m9_scissor_words(minx, miny, maxx, maxy, &key->clip_min, &key->clip_max);
   return true;
}

/* One coefficient of a coordinate transform against constant buffer 0. */
static float
m9_coord_term(const struct m9_context *ctx, const struct m9_fs_coord_term *t)
{
   const struct pipe_constant_buffer *cb = &ctx->fs_constants;
   const uint8_t *data = cb->user_buffer ? cb->user_buffer :
                         cb->buffer ? m9_res(cb->buffer)->map : NULL;
   float v = 0.0f;

   if (t->offset == M9_FS_UNIFORM_ZERO || !data ||
       (size_t)t->offset + sizeof(float) > cb->buffer_size)
      return t->literal;
   memcpy(&v, data + cb->buffer_offset + t->offset, sizeof(v));
   if (t->reciprocal) {
      if (v == 0.0f)
         return t->literal;
      v = 1.0f / v;
   }
   return t->literal + v * t->factor;
}

/* Raster constant span values for the current constant buffer 0. */
static bool
m9_fs_resolve_consts(struct m9_context *ctx, const struct m9_fs_variant *fs,
                     struct util_dynarray *out)
{
   const struct pipe_constant_buffer *cb = &ctx->fs_constants;
   const uint8_t *data = cb->user_buffer ? cb->user_buffer :
                         cb->buffer ? m9_res(cb->buffer)->map : NULL;
   size_t size = data ? cb->buffer_size : 0;

   if (data)
      data += cb->buffer_offset;
   util_dynarray_clear(out);
   for (unsigned pi = 0; pi < fs->passes.count; pi++)
   util_dynarray_foreach(&fs->passes.pass[pi].consts, struct m9_fs_const, k) {
      for (unsigned lane = 0; lane < 2; lane++) {
         float v = 0.0f;

         switch (k->kind) {
         case M9_FS_CONST_LITERAL:
            v = k->value[lane];
            break;
         case M9_FS_CONST_UNIFORM:
            if (k->offset[lane] == M9_FS_UNIFORM_ZERO)
               break;
            if ((size_t)k->offset[lane] + sizeof(float) > size) {
               m9_warn_once(ctx, M9_WARN_FS, "fragment constants outside "
                            "the bound constant buffer; draw skipped");
               return m9_skip_reason(ctx, "fragment constants outside the "
                                     "bound constant buffer; draw skipped");
            }
            memcpy(&v, data + k->offset[lane], sizeof(v));
            break;
         case M9_FS_CONST_FRAG_Y:
            v = lane ? (float)ctx->params.height - 1.0f : 0.0f;
            break;
         case M9_FS_CONST_ALPHA_REF:
            if ((size_t)k->offset[0] + sizeof(float) > size) {
               m9_warn_once(ctx, M9_WARN_FS, "alpha reference outside the "
                            "bound constant buffer; draw skipped");
               return m9_skip_reason(ctx, "alpha reference outside the "
                                     "bound constant buffer; draw skipped");
            }
            memcpy(&v, data + k->offset[0], sizeof(v));
            v *= 256.0f;
            break;
         case M9_FS_CONST_BLEND_COLOR:
         case M9_FS_CONST_BLEND_COLOR_INV:
            /* GL clamps the colour for a fixed-point target. */
            v = SATURATE(ctx->blend_color.color[k->offset[lane] & 3]);
            if (k->kind == M9_FS_CONST_BLEND_COLOR_INV)
               v = 1.0f - v;
            break;
         }
         util_dynarray_append_typed(out, float, v);
      }
   }
   return true;
}

/* Name the level the sampler cannot reach, once a resource: a texture that
 * silently loses its upper levels renders a wrong image rather than nothing. */
static void
m9_texture_level_refused(struct m9_resource *res, unsigned level)
{
   if (res->level_refused)
      return;
   res->level_refused = true;
   fprintf(stderr, "m9x: texture %u %ux%u %s: level %u holds no content; the "
           "sampler is clamped below it\n", res->handle, res->width,
           res->height, util_format_short_name(res->base.format), level);
}

/* Unit words of a sampler view, or the reason it cannot be sampled. */
static struct m9_context *m9_debug_ctx;

static const char *
m9_texture_bind_view(const struct pipe_sampler_view *view,
                     struct m9_texture_bind *bind)
{
   struct m9_resource *res;
   unsigned first, last;

   static unsigned res_debug_once;

   if (!view || !view->texture)
      return "no texture on a sampled unit; draw skipped";
   if (!res_debug_once++ && debug_get_bool_option("M9X_DEBUG", false))
      fprintf(stderr, "m9x: sampling %s %ux%u kind %u tiled %d levels %u\n",
              util_format_short_name(view->texture->format),
              m9_res(view->texture)->width, m9_res(view->texture)->height,
              m9_res(view->texture)->kind, m9_res(view->texture)->tiled,
              m9_res(view->texture)->texture.levels);
   res = m9_res(view->texture);
   /* A line is stored as a plane one texel high. */
   if (view->target != PIPE_TEXTURE_2D && view->target != PIPE_TEXTURE_RECT &&
       view->target != PIPE_TEXTURE_CUBE && view->target != PIPE_TEXTURE_3D &&
       view->target != PIPE_TEXTURE_1D)
      return "only line, plane, cube and volume sampling is implemented; "
             "draw skipped";
   /* View swizzles are lowered into the fragment variant (m9_ctx_variant).
    * A depth view names the sampleable part of a depth image, so compare
    * the descriptor code rather than the pipe format. */
   if (view->format != res->base.format) {
      struct m9_format_info a, b;

      if (!m9_format_info(view->format, &a) ||
          !m9_format_info(res->base.format, &b) || a.code != b.code)
         return "sampler view format differs from the texture; draw skipped";
   }
   /*
    * A base level above 0 is a LOD clamp, not a second allocation: the
    * descriptor addresses every level from one base, so pinning 7d's low
    * bound to the view's first level samples that level alone.  Only a
    * chain carries other levels; every other storage holds level 0.
    */
   first = view->u.tex.first_level;
   if (first && (!res->texture.levels || first > res->base.last_level))
      return "a base level above 0 needs a mip chain; draw skipped";
   last = MAX2(MIN2(view->u.tex.last_level, res->base.last_level), first);
   /* The tail levels sample from the shared atlas wherever the chain
    * places it (MEASURED, every level of 32x32 to 256x64 and the NPOT
    * chains 17x9 .. 128x40), once a small level advances the chain by its
    * aperture pages rather than by the surface contract. */
   if (first > last)
      return "a re-based view beyond the chain; draw skipped";
   if (res->texture.levels) {
      /* A level whose content could not be stored samples whatever the
       * object was allocated with.  Refuse that level rather than the whole
       * draw: clamp the view to the last level below the first unplaced one
       * and keep sampling the levels that do hold their content. */
      for (unsigned l = first; l <= last; l++)
         if (res->texture_unplaced & (1u << l)) {
            if (l == first)
               return "the view's base texture level could not be stored; "
                      "draw skipped";
            m9_texture_level_refused(res, l);
            last = l - 1;
            break;
         }
      /* A level nothing ever wrote samples whatever the object was
       * allocated with, which is why such a texture comes out black. */
      if (m9_debug_textures && !res->bind_logged) {
         ((struct m9_resource *)res)->bind_logged = true;
         fprintf(stderr, "m9x: bind texture %u %ux%u %s levels %u written "
                 "%#x kind %u\n", res->handle, res->width, res->height,
                 util_format_short_name(res->base.format),
                 res->base.last_level + 1, res->levels_written, res->kind);
      }
      /* The storage kind must be the one the level was written through:
       * the small kind up to 64 in both dimensions, the ordinary tiled kind
       * above it.  Reading 128x128 and larger textures as the small kind
       * returned a single constant texel. */
      /* A compressed level is stored and sampled through the ordinary
       * tiled kind, at its texel extent rounded up to whole blocks
       * (docs/parhelia-m9-s3tc-compressed-textures.md 5.1). */
      if (res->texture.block_bytes) {
         /* MEASURED: the storage kind is the one level 0 was written
          * through, while the extent stays the logical one rounded up to
          * whole blocks.  dxt1:132x64, whose logical width is not its
          * 32-aligned span, samples exactly, which is what settles section
          * 5.2's open question in favour of the logical width. */
         uint32_t span = m9_compressed_span(res->width);
         uint32_t lines = m9_compressed_lines(res->height,
                                              res->texture.block_bytes);

         *bind = (struct m9_texture_bind) {
            .code = res->texture_code,
            .kind = m9_compressed_kind(span, lines),
            .width = (res->width + 3) & ~3u,
            .height = (res->height + 3) & ~3u,
            .depth = 1,
            .last_level = res->base.last_level, .view_first_level = first,
            .view_last_level = last,
         };
         return NULL;
      }
      *bind = (struct m9_texture_bind) {
         .code = res->texture_code,
         .kind = m9_texture_bind_kind(&res->texture, res->width, res->height),
         .width = res->width, .height = res->height,
         .depth = res->texture.kind == M9_TEXTURE_KIND_3D ? res->depth : 1,
         .last_level = res->base.last_level, .view_first_level = first,
         .view_last_level = last,
      };
      return NULL;
   }
   /* A depth surface samples through the descriptor the raster pass binds
    * it with; shadowmap_d24 binds the very same word in both roles
    * (CAPTURED, captures/m9x-vendor-openbits-20260917). */
   if (res->kind == M9_RESOURCE_LINEAR_DEPTH && res->format) {
      bool small = res->width <= 64 && res->height <= 64;

      *bind = (struct m9_texture_bind) {
         .code = (uint8_t)(res->format << 1),
         .kind = small ? M9_TEXTURE_KIND_2D : M9_TEXTURE_KIND_RENDER,
         .width = m9_depth_row_span(res->width, res->height),
         .height = res->height, .depth = 1,
      };
      return NULL;
   }
   /* Captured 79=c8210080 FBO textures: ordinary tiled render targets.  The
    * descriptor carries the 32-aligned row span; other widths sample through
    * scaled coordinates (m9_job_textures). */
   if (res->tiled && res->kind == M9_RESOURCE_LINEAR_COLOR &&
       (res->base.format == PIPE_FORMAT_B8G8R8A8_UNORM ||
        res->base.format == PIPE_FORMAT_B8G8R8X8_UNORM ||
        res->base.format == PIPE_FORMAT_R10G10B10A2_UNORM ||
        res->base.format == PIPE_FORMAT_B10G10R10A2_UNORM ||
        res->base.format == PIPE_FORMAT_R10G10B10X2_UNORM ||
        res->base.format == PIPE_FORMAT_B10G10R10X2_UNORM)) {
      /* The small family is addressed by the 2-D kind, like a small texture:
       * reading it as the ordinary tiled kind returns only the first 16x16
       * texels, which is 1/16 of a 64x64 surface and 1/4 of a 32x32 one. */
      bool small = res->width <= 64 && res->height <= 64;

      /* The descriptor byte is the surface's own 7-bit code doubled, which
       * is 0xcc for the BGRA8 family and 0xd0/0xd4 for the 10-bit one. */
      *bind = (struct m9_texture_bind) {
         .code = (uint8_t)(res->format << 1),
         .kind = small ? M9_TEXTURE_KIND_2D : M9_TEXTURE_KIND_RENDER,
         .width = small ? res->width : (res->width + 31) & ~31u,
         .height = res->height, .depth = 1,
      };
      return NULL;
   }
   return "texture is not stored in a captured sampler layout; draw skipped";
}

static bool
m9_job_textures(struct m9_context *ctx, const struct m9_fs_variant *fs,
                struct m9_job_key *key)
{
   /* One binding per texture slot of every raster pass, in the order the
    * variant pooled them; a slot a record feeds binds nothing. */
   for (unsigned u = 0; u < fs->slot_count; u++) {
      const struct m9_fs_program *p = &fs->passes.pass[fs->slot_pass[u]];
      unsigned k = fs->slot_index[u];
      unsigned slot = p->textures[k];
      struct pipe_sampler_view *view = slot < PIPE_MAX_SAMPLERS ?
                                       ctx->fs_views[slot] : NULL;
      const struct pipe_sampler_state *sampler = slot < PIPE_MAX_SAMPLERS ?
                                                 ctx->fs_samplers[slot] : NULL;
      struct m9_texture_bind bind;
      const char *reason;
      float span[2];

      /* An incomplete unit samples the opaque black placeholder. */
      if (!view || !view->texture) {
         if (m9_debug_textures)
            fprintf(stderr, "m9x: unit %u slot %u has no view: placeholder "
                    "(handle %u), textures %u\n", u, slot,
                    ctx->placeholder_texture.handle, p->texture_count);
         m9_texture_unit_placeholder(&key->units[u],
                                     ctx->placeholder_texture.handle);
         key->unit_scale[u][0] = key->unit_scale[u][1] = 1.0f;
         for (unsigned i = 0; i < 4; i++) {
            key->coord_scale[u][i] = 1.0f;
            key->coord_bias[u][i] = 0.0f;
         }
         continue;
      }
      reason = m9_texture_bind_view(view, &bind);
      bind.srgb = !reason && util_format_is_srgb(view->format);

      if (reason && view && view->texture && ctx->debug)
         fprintf(stderr, "m9x: unit %u view %s on %s swizzle %u%u%u%u: %s\n",
                 u, util_format_short_name(view->format),
                 util_format_short_name(view->texture->format),
                 view->swizzle_r, view->swizzle_g, view->swizzle_b,
                 view->swizzle_a, reason);
      if (!reason && !sampler)
         reason = "sampled unit has no sampler state; draw skipped";
      /* The backend samples a line at a constant t: clamping t to the one
       * row keeps a border colour or a wrap out of it. */
      struct pipe_sampler_state line_sampler, biased;

      if (!reason && view->target == PIPE_TEXTURE_1D) {
         line_sampler = *sampler;
         line_sampler.wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
         sampler = &line_sampler;
      }
      /* The fetch's own bias is constant over the draw, so it adds to the
       * sampler's; GL clamps the sum to the advertised range. */
      if (!reason && p->texcoord_biased[k]) {
         float max = ctx->screen->base.caps.max_texture_lod_bias;

         biased = *sampler;
         biased.lod_bias = CLAMP(sampler->lod_bias +
                                 m9_coord_term(ctx, &p->texcoord_lod_bias[k]),
                                 -max, max);
         sampler = &biased;
      }
      /* The comparison needs no compensation: a depth surface the engine
       * rendered compares against the plain window depth, exactly, for
       * every one of the eight functions (MEASURED, m9-mesa-glsl
       * shadow:FUNC).  The vendor ICD's complemented readback is not what
       * this pipeline stores. */
      if (!reason && m9_texture_unit_init(&key->units[u],
                                          m9_res(view->texture)->handle,
                                          &bind, sampler))
         reason = "sampler wrap mode is not implemented; draw skipped";
      if (reason) {
         m9_warn_once(ctx, M9_WARN_TEXTURE, reason);
         return m9_skip_reason(ctx, reason);
      }
      if (m9_debug_textures)
         fprintf(stderr, "m9x: unit %u handle %u 70=%08x 79=%08x 7a=%08x "
                 "7c=%08x 7d=%08x levels %u..%u\n", u, key->units[u].handle,
                 key->units[u].w70, key->units[u].w79, key->units[u].w7a,
                 key->units[u].w7c, key->units[u].w7d,
                 view->u.tex.first_level, view->u.tex.last_level);
      key->unit_res[u] = view->texture;
      for (unsigned i = 0; i < 4; i++) {
         key->coord_scale[u][i] =
            m9_coord_term(ctx, &p->texcoord_affine[k].scale[i]);
         key->coord_bias[u][i] =
            m9_coord_term(ctx, &p->texcoord_affine[k].bias[i]);
      }
      /* Content over the span the descriptor addresses: a render target of
       * unaligned width carries padding the coordinate has to skip. */
      span[0] = (float)m9_res(view->texture)->width / bind.width;
      span[1] = (float)m9_res(view->texture)->height / bind.height;
      /* A rectangle target carries its coordinate in texels.  The engine has
       * no unnormalized addressing mode - the vendor's 13x7 rectangle emits
       * the descriptor words of a 13x7 2-D texture and folds 1/13 and 1/7
       * into a shader constant instead (CAPTURED) - so the reciprocal of the
       * addressed span is folded into the coordinate here. */
      if (view->target == PIPE_TEXTURE_RECT) {
         key->unit_scale[u][0] = 1.0f / bind.width;
         key->unit_scale[u][1] = 1.0f / bind.height;
      } else {
         key->unit_scale[u][0] = span[0];
         key->unit_scale[u][1] = span[1];
      }
      /* m9_scale_coords() applies the scale per vertex to a CPU coordinate
       * stream.  A coordinate an earlier raster pass produced reaches the
       * fetch through the inter-pass record instead, where there is no
       * vertex to apply it to, so a unit that needs a scale and is steered
       * by a route is refused rather than sampled at the wrong texel.  This
       * covers every rectangle and the render target of unaligned width that
       * could already reach it. */
      if (key->unit_scale[u][0] != 1.0f || key->unit_scale[u][1] != 1.0f) {
         const struct m9_fs_pass_streams *st = &fs->stream[fs->slot_pass[u]];
         unsigned c = st->texture_coord[k];

         if (c == M9_RASTER_COORD_NONE || st->coord_extra[c] < 0)
            reason = "a scaled texture coordinate that no CPU stream carries "
                     "cannot be applied; draw skipped";
      }
      /* Every wrap mode agrees inside the content; beyond it the hardware
       * wraps or clamps at the aligned span, including its padding.  A
       * rectangle only ever clamps and its content edge is the one the
       * extension defines, so a padded span is reported for it as well. */
      if (span[0] != 1.0f &&
          (view->target == PIPE_TEXTURE_RECT ||
           (sampler->wrap_s != PIPE_TEX_WRAP_CLAMP_TO_EDGE &&
            sampler->wrap_s != PIPE_TEX_WRAP_CLAMP_TO_BORDER &&
            sampler->wrap_s != PIPE_TEX_WRAP_CLAMP)))
         m9_warn_once(ctx, M9_WARN_WRAP, "render targets of unaligned width "
                      "wrap at their 32-aligned span; samples outside the "
                      "content read padding");
      for (unsigned j = 0; !reason && j < u; j++)
         if (fs->slot_pass[j] == fs->slot_pass[u] &&
             fs->stream[fs->slot_pass[j]].texture_coord[fs->slot_index[j]] ==
             fs->stream[fs->slot_pass[u]].texture_coord[k] &&
             memcmp(key->unit_scale[j], key->unit_scale[u],
                    sizeof(key->unit_scale[u])))
            reason = "textures of different spans share a coordinate; "
                     "draw skipped";
      if (reason) {
         m9_warn_once(ctx, M9_WARN_TEXTURE, reason);
         return m9_skip_reason(ctx, reason);
      }
   }
   key->unit_count = fs->slot_count;
   if (!fs->slot_count) {
      m9_texture_unit_placeholder(&key->units[0], ctx->placeholder_texture.handle);
      key->unit_count = 1;
   }
   return true;
}

static bool
m9_blend_factor(enum pipe_blendfactor f, uint8_t *out)
{
   static const struct { enum pipe_blendfactor pipe; uint8_t m9; } map[] = {
      { PIPE_BLENDFACTOR_ZERO, M9_FS_ZERO },
      { PIPE_BLENDFACTOR_ONE, M9_FS_ONE },
      { PIPE_BLENDFACTOR_SRC_ALPHA, M9_FS_SRC_ALPHA },
      { PIPE_BLENDFACTOR_INV_SRC_ALPHA, M9_FS_INV_SRC_ALPHA },
      { PIPE_BLENDFACTOR_DST_ALPHA, M9_FS_DST_ALPHA },
      { PIPE_BLENDFACTOR_INV_DST_ALPHA, M9_FS_INV_DST_ALPHA },
      { PIPE_BLENDFACTOR_SRC_COLOR, M9_FS_SRC_COLOR },
      { PIPE_BLENDFACTOR_INV_SRC_COLOR, M9_FS_INV_SRC_COLOR },
      { PIPE_BLENDFACTOR_DST_COLOR, M9_FS_DST_COLOR },
      { PIPE_BLENDFACTOR_INV_DST_COLOR, M9_FS_INV_DST_COLOR },
      { PIPE_BLENDFACTOR_CONST_COLOR, M9_FS_CONST_COLOR },
      { PIPE_BLENDFACTOR_INV_CONST_COLOR, M9_FS_INV_CONST_COLOR },
      { PIPE_BLENDFACTOR_CONST_ALPHA, M9_FS_CONST_ALPHA },
      { PIPE_BLENDFACTOR_INV_CONST_ALPHA, M9_FS_INV_CONST_ALPHA },
      { PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE, M9_FS_SRC_ALPHA_SATURATE },
   };

   for (unsigned i = 0; i < ARRAY_SIZE(map); i++) {
      if (map[i].pipe == f) {
         *out = map[i].m9;
         return true;
      }
   }
   return false;
}

static bool
m9_blend_func(enum pipe_blend_func f, uint8_t *out)
{
   switch (f) {
   case PIPE_BLEND_ADD: *out = M9_FS_FUNC_ADD; return true;
   case PIPE_BLEND_SUBTRACT: *out = M9_FS_FUNC_SUBTRACT; return true;
   case PIPE_BLEND_REVERSE_SUBTRACT:
      *out = M9_FS_FUNC_REVERSE_SUBTRACT;
      return true;
   case PIPE_BLEND_MIN: *out = M9_FS_FUNC_MIN; return true;
   case PIPE_BLEND_MAX: *out = M9_FS_FUNC_MAX; return true;
   default: return false;
   }
}

/* What a factor means in the alpha channel. */
static enum pipe_blendfactor
m9_alpha_equivalent(enum pipe_blendfactor f)
{
   switch (f) {
   case PIPE_BLENDFACTOR_SRC_COLOR: return PIPE_BLENDFACTOR_SRC_ALPHA;
   case PIPE_BLENDFACTOR_INV_SRC_COLOR: return PIPE_BLENDFACTOR_INV_SRC_ALPHA;
   case PIPE_BLENDFACTOR_DST_COLOR: return PIPE_BLENDFACTOR_DST_ALPHA;
   case PIPE_BLENDFACTOR_INV_DST_COLOR: return PIPE_BLENDFACTOR_INV_DST_ALPHA;
   case PIPE_BLENDFACTOR_CONST_COLOR: return PIPE_BLENDFACTOR_CONST_ALPHA;
   case PIPE_BLENDFACTOR_INV_CONST_COLOR: return PIPE_BLENDFACTOR_INV_CONST_ALPHA;
   case PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE: return PIPE_BLENDFACTOR_ONE;
   default: return f;
   }
}

/* One channel group's equation; MIN and MAX ignore the factors. */
struct m9_blend_part {
   enum pipe_blend_func func;
   enum pipe_blendfactor src, dst;
};

/* A target without alpha reads its destination alpha as one. */
static enum pipe_blendfactor
m9_dst_alpha_one(enum pipe_blendfactor f)
{
   switch (f) {
   case PIPE_BLENDFACTOR_DST_ALPHA: return PIPE_BLENDFACTOR_ONE;
   case PIPE_BLENDFACTOR_INV_DST_ALPHA: return PIPE_BLENDFACTOR_ZERO;
   case PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE: return PIPE_BLENDFACTOR_ZERO;
   default: return f;
   }
}

static struct m9_blend_part
m9_blend_part(enum pipe_blend_func func, enum pipe_blendfactor src,
              enum pipe_blendfactor dst, bool alpha, bool no_dst_alpha)
{
   if (func == PIPE_BLEND_MIN || func == PIPE_BLEND_MAX)
      return (struct m9_blend_part){ func, PIPE_BLENDFACTOR_ONE,
                                     PIPE_BLENDFACTOR_ONE };
   if (alpha) {
      src = m9_alpha_equivalent(src);
      dst = m9_alpha_equivalent(dst);
   }
   if (no_dst_alpha) {
      src = m9_dst_alpha_one(src);
      dst = m9_dst_alpha_one(dst);
   }
   return (struct m9_blend_part){ func, src, dst };
}

/* ZERO * source + ONE * destination leaves the channel as it was. */
static bool
m9_blend_keeps_dst(const struct m9_blend_part *p)
{
   return (p->func == PIPE_BLEND_ADD ||
           p->func == PIPE_BLEND_REVERSE_SUBTRACT) &&
          p->src == PIPE_BLENDFACTOR_ZERO && p->dst == PIPE_BLENDFACTOR_ONE;
}

static const char *
m9_blend_part_key(const struct m9_blend_part *p, uint8_t *src, uint8_t *dst,
                  uint8_t *func)
{
   if (!m9_blend_func(p->func, func))
      return "unknown blend equation; draw skipped";
   if (!m9_blend_factor(p->src, src) || !m9_blend_factor(p->dst, dst))
      return "dual-source blend factors are not implemented; draw skipped";
   return NULL;
}

/* A logic op that needs the fragment tail: everything but COPY. */
static bool
m9_logic_tail(const struct pipe_blend_state *blend)
{
   return blend && blend->logicop_enable &&
          blend->logicop_func != PIPE_LOGICOP_COPY;
}

/* Blend state as lowered factors and the channels the draw writes, or the
 * reason draws are skipped.  A group that keeps the destination is masked
 * off in the raster state, which keeps the program to one equation; A gets
 * its own only where both groups write. */
static const char *
m9_blend_key(const struct pipe_blend_state *blend, struct m9_fs_blend *out,
             unsigned *colormask, bool no_dst_alpha,
             enum pipe_format color_format)
{
   const struct pipe_rt_blend_state *rt = blend ? &blend->rt[0] : NULL;
   struct m9_blend_part rgb, a, rgb_a;
   const char *reason;

   /* GL's logic op replaces blending; COPY is the plain write. */
   if (blend && blend->logicop_enable) {
      if (blend->logicop_func == PIPE_LOGICOP_COPY)
         return NULL;
      if (color_format != PIPE_FORMAT_B8G8R8A8_UNORM &&
          color_format != PIPE_FORMAT_B8G8R8X8_UNORM)
         return "the logic-op tail is captured for 8-bit targets only; "
                "draw skipped";
      out->logic = true;
      out->logic_func = blend->logicop_func;
      return NULL;
   }
   if (!rt || !rt->blend_enable)
      return NULL;
   rgb = m9_blend_part(rt->rgb_func, rt->rgb_src_factor, rt->rgb_dst_factor,
                       false, no_dst_alpha);
   a = m9_blend_part(rt->alpha_func, rt->alpha_src_factor,
                     rt->alpha_dst_factor, true, no_dst_alpha);
   rgb_a = m9_blend_part(rt->rgb_func, rt->rgb_src_factor, rt->rgb_dst_factor,
                         true, no_dst_alpha);
   if (rgb_a.func != a.func || rgb_a.src != a.src || rgb_a.dst != a.dst) {
      if (m9_blend_keeps_dst(&a)) {
         if (colormask)
            *colormask &= ~PIPE_MASK_A;
      } else if (m9_blend_keeps_dst(&rgb)) {
         if (colormask)
            *colormask &= ~PIPE_MASK_RGB;
         rgb = a;
      } else {
         out->separate = true;
      }
   }
   reason = m9_blend_part_key(&rgb, &out->src, &out->dst, &out->func);
   if (!reason && out->separate)
      reason = m9_blend_part_key(&a, &out->alpha_src, &out->alpha_dst,
                                 &out->alpha_func);
   return reason;
}

static bool
m9_fs_variant_upload(struct m9_context *ctx, struct m9_fs_variant *v,
                     const char **reason)
{
   const unsigned vs_slots = ARRAY_SIZE(m9_passthrough_c2_program) / 2 - 4;
   unsigned slots = 0;
   uint64_t bytes;

   /* One image holds the fragment program of every raster pass back to
    * back, each entered at its own c3, then the vertex program. */
   for (unsigned i = 0; i < v->passes.count; i++) {
      v->entry[i] = slots;
      slots += m9_fs_slot_count(&v->passes.pass[i]);
   }
   bytes = align64(8ull * (slots + vs_slots), 0x1000);
   if (bytes > M9_PROGRAM_MAX_BYTES) {
      *reason = "program exceeds the largest c2 image the driver allocates";
      return false;
   }
   if (m9_bo_create(ctx->screen, bytes, PARHELIA_M9_BO_VRAM,
                    M9_RESOURCE_SYSTEM_STATE, &v->c2)) {
      *reason = "cannot allocate the c2 image";
      return false;
   }
   memset(v->c2.map, 0, v->c2.size);
   for (unsigned i = 0; i < v->passes.count; i++)
      memcpy((uint8_t *)v->c2.map + 8 * v->entry[i],
             v->passes.pass[i].slots.data,
             8 * m9_fs_slot_count(&v->passes.pass[i]));
   memcpy((uint8_t *)v->c2.map + 8 * slots, m9_passthrough_c2_program + 8,
          8 * vs_slots);
   v->vertex_entry = slots;
   return true;
}

/* CPU view order, pass by pass: non-colour varyings, then the pass's own
 * distinct coordinate streams.  Passes share no stream: each interpolates
 * what it reads, and a value handed on between them travels through the
 * record instead. */
static bool
m9_fs_streams(struct m9_fs_variant *v)
{
   for (unsigned i = 0; i < v->passes.count; i++) {
      const struct m9_fs_program *p = &v->passes.pass[i];
      struct m9_fs_pass_streams *st = &v->stream[i];
      /* Two vec2 coordinate sets are packed into one varying slot and a pair
       * may straddle the halves, so a stream is keyed by the component it
       * starts at: keying it by location alone gave both textures the same
       * coordinates, and keying it by the slot half made a pair at
       * component 0 and one at component 1 share a stream they do not. */
      uint8_t c_owner[M9_FS_MAX_TEXTURES] = { 0 };

      for (unsigned k = 0; k < p->varying_count; k++) {
         st->varying_extra[k] = -1;
         if (p->varyings[k].location == VARYING_SLOT_COL0)
            continue;
         if (v->extra_count >= M9_FS_MAX_EXTRA)
            return false;
         st->varying_extra[k] = v->extra_count;
         v->extra_location[v->extra_count++] = p->varyings[k].location;
      }
      for (unsigned t = 0; t < p->texture_count; t++) {
         unsigned first, c = 0;

         /* A slot an earlier pass feeds through the record interpolates
          * nothing and binds no texture of its own. */
         if (p->texture_source[t] != M9_FS_ROUTE_NONE) {
            st->texture_coord[t] = M9_RASTER_COORD_NONE;
            st->unit[t] = M9_PASS_UNIT_NONE;
            continue;
         }
         /* A unit the record steers samples its own texture and declares no
          * coordinate of its own. */
         if (p->texcoord_source[t] != M9_FS_ROUTE_NONE) {
            st->texture_coord[t] = M9_RASTER_COORD_NONE;
            if (v->slot_count >= M9_DRAW_MAX_UNITS)
               return false;
            st->unit[t] = v->slot_count;
            v->slot_pass[v->slot_count] = i;
            v->slot_index[v->slot_count++] = t;
            continue;
         }
         first = p->texcoord_components[t];
         /* A unit that transforms its coordinate needs a stream of its own:
          * sharing one would sample both units at the same place. */
         while (c < st->coord_count &&
                (st->coord_location[c] != p->texcoords[t] ||
                 st->coord_component[c] != first ||
                 p->texcoord_projective[c_owner[c]] !=
                 p->texcoord_projective[t] ||
                 !m9_fs_coord_affine_same(&p->texcoord_affine[c_owner[c]],
                                         &p->texcoord_affine[t])))
            c++;
         if (c == st->coord_count) {
            c_owner[c] = t;
            st->coord_extra[c] = -1;
            st->coord_component[c] = first;
            st->coord_location[st->coord_count++] = p->texcoords[t];
            if (p->texcoords[t] != VARYING_SLOT_COL0) {
               if (v->extra_count >= M9_FS_MAX_EXTRA)
                  return false;
               st->coord_extra[c] = v->extra_count;
               v->extra_location[v->extra_count++] = p->texcoords[t];
            }
         }
         st->texture_coord[t] = c;
         if (v->slot_count >= M9_DRAW_MAX_UNITS)
            return false;
         st->unit[t] = v->slot_count;
         v->slot_pass[v->slot_count] = i;
         v->slot_index[v->slot_count++] = t;
      }
   }
   return true;
}

/* A plain fetch of a depth surface returns the window depth in the texel's
 * second scalar, not the first; a comparing fetch returns its result in the
 * first (MEASURED, m9-mesa-glsl depthchan).  Splat the second one over the
 * result, which is what every GL_DEPTH_TEXTURE_MODE then selects from. */
static bool
m9_depth_channel_instr(nir_builder *b, nir_instr *instr, void *data)
{
   uint32_t mask = *(const uint32_t *)data;
   nir_tex_instr *tex;
   nir_def *depth;

   if (instr->type != nir_instr_type_tex)
      return false;
   tex = nir_instr_as_tex(instr);
   if (tex->is_shadow || tex->def.num_components < 2 ||
       tex->sampler_index >= 32 || !(mask & BITFIELD_BIT(tex->sampler_index)))
      return false;
   b->cursor = nir_after_instr(instr);
   depth = nir_channel(b, &tex->def, 1);
   nir_def_rewrite_uses_after(&tex->def,
                              nir_vec4(b, depth, depth, depth, depth));
   return true;
}

/* Program of fs for a blend state and inlined uniform values, compiled on
 * first use; refused variants stay cached with their reason. */
/*
 * Where each producing pass writes in the inter-pass record.  A hand-over is
 * live from the pass that writes it to the last pass that reads it, so slots
 * are reused once nothing reads them: a chain, where every pass reads only
 * its predecessor, needs two slots whatever its length instead of one a
 * producer.  The descriptor carries stride and offset as independent fields
 * (docs/parhelia-m9-varying-transport-re.md 3), so this asks nothing new of
 * the engine, and it halves both the record and the traffic through it for
 * the four- and five-pass shaders that are bandwidth bound.
 *
 * A slot stays four dwords wide: narrowing a one-pair producer to the two
 * the captures show stops the engine (MEASURED).
 */
static void
m9_record_layout(struct m9_fs_variant *v)
{
   unsigned count = v->passes.count;
   /* The last pass that reads producer i, and the first producer index at
    * which a slot is free again. */
   unsigned last[M9_FS_MAX_PASSES], free_at[M9_FS_MAX_PASSES];
   unsigned slots = 0;

   memset(v->record_offset, 0, sizeof(v->record_offset));
   if (count < 2) {
      v->record_dwords = 0;
      return;
   }
   for (unsigned i = 0; i < count; i++) {
      last[i] = i;
      free_at[i] = 0;
   }
   for (unsigned j = 0; j < count; j++) {
      const struct m9_fs_program *p = &v->passes.pass[j];

      for (unsigned k = 0; k < p->texture_count; k++) {
         uint8_t src = p->texture_source[k];

         if (src != M9_FS_ROUTE_NONE && src < count)
            last[src] = MAX2(last[src], j);
         src = p->texcoord_source[k];
         if (src != M9_FS_ROUTE_NONE && src < count)
            last[src] = MAX2(last[src], j);
      }
   }
   for (unsigned i = 0; i + 1 < count; i++) {
      unsigned slot = 0;

      if (!v->passes.pass[i].writes_record)
         continue;
      if (m9_record_liveness) {
         while (slot < slots && free_at[slot] > i)
            slot++;
      } else {
         slot = slots;
      }
      if (slot == slots)
         slots++;
      free_at[slot] = last[i] + 1;
      v->record_offset[i] = 4 * slot;
   }
   v->record_dwords = 4 * slots;
}

static void
m9_fs_variant_free(struct m9_context *ctx, struct m9_fs_variant *v)
{
   util_dynarray_foreach(&v->native_c2_images, struct m9_native_image, entry)
      m9_bo_destroy(ctx->screen, &entry->image);
   util_dynarray_fini(&v->native_c2_images);
   if (v->ok) {
      m9_bo_destroy(ctx->screen, &v->c2);
      m9_bo_destroy(ctx->screen, &v->native_c2);
      m9_fs_passes_fini(&v->passes);
   }
   FREE(v);
}

static void
m9_retired_variants_free(struct m9_context *ctx)
{
   util_dynarray_foreach(&ctx->retired_fs_variants, struct m9_fs_variant *, it)
      m9_fs_variant_free(ctx, *it);
   util_dynarray_clear(&ctx->retired_fs_variants);
}

/* Keep the one colour output an MRT blend pass draws, as output 0. */
static bool
m9_select_store(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   const unsigned keep = *(const unsigned *)data - 1;
   nir_io_semantics sem;

   (void)b;
   if (intr->intrinsic != nir_intrinsic_store_output)
      return false;
   sem = nir_intrinsic_io_semantics(intr);
   if (sem.location < FRAG_RESULT_DATA0)
      return false;
   if (sem.location != FRAG_RESULT_DATA0 + keep) {
      nir_instr_remove(&intr->instr);
      return true;
   }
   sem.location = FRAG_RESULT_DATA0;
   nir_intrinsic_set_io_semantics(intr, sem);
   return true;
}

static const struct m9_fs_variant *
m9_fs_variant(struct m9_context *ctx, struct m9_fs_state *fs,
              const struct m9_fs_variant *want, const char **reason)
{
   const size_t inline_bytes = fs->nir->info.num_inlinable_uniforms *
                               sizeof(uint32_t);
   struct m9_fs_variant *v, **slot;
   nir_shader *clone;

   util_dynarray_foreach(&fs->variants, struct m9_fs_variant *, it) {
      if (m9_fs_blend_same(&(*it)->blend, &want->blend) &&
          (*it)->targets == want->targets &&
          (*it)->select == want->select &&
          !memcmp((*it)->inline_values, want->inline_values, inline_bytes) &&
          (*it)->depth_texture == want->depth_texture &&
          (*it)->face_flip == want->face_flip &&
          (*it)->srgb_texture == want->srgb_texture &&
          !memcmp((*it)->swizzle, want->swizzle, sizeof(want->swizzle))) {
         struct m9_fs_variant **last =
            util_dynarray_last_ptr(&fs->variants, struct m9_fs_variant *);

         v = *it;
         /* Least recently used first, so eviction takes the oldest. */
         if (it != last) {
            memmove(it, it + 1, (char *)last - (char *)it);
            *last = v;
         }
         *reason = v->reason;
         return v->ok ? v : NULL;
      }
   }
   /* Blend state and uniform specialisation each ask for variants, so the
    * cache is capped and the least recently used one is evicted. */
   if (util_dynarray_num_elements(&fs->variants, struct m9_fs_variant *) >=
       M9_FS_MAX_VARIANTS) {
      struct m9_fs_variant **first = fs->variants.data;

      /* The draw module may be mid-draw here, so a pending or held job that
       * names it keeps it until the next sync. */
      util_dynarray_append_typed(&ctx->retired_fs_variants,
                                 struct m9_fs_variant *, *first);
      memmove(first, first + 1, fs->variants.size - sizeof(*first));
      fs->variants.size -= sizeof(*first);
   }

   /* M9X_FS_INLINE_TRACE=1 names the uniforms a shader is specialised on and
    * the values the state tracker sent, which is what a refusal for control
    * flow has to be read against. */
   if (debug_get_bool_option("M9X_FS_INLINE_TRACE", false)) {
      fprintf(stderr, "m9x: fs variant: %u inlinable uniform(s)",
              fs->nir->info.num_inlinable_uniforms);
      for (unsigned i = 0; i < fs->nir->info.num_inlinable_uniforms; i++)
         fprintf(stderr, " dw%u=%08x",
                 fs->nir->info.inlinable_uniform_dw_offsets[i],
                 want->inline_values[i]);
      fprintf(stderr, "\n");
   }
   *reason = "out of memory";
   v = CALLOC_STRUCT(m9_fs_variant);
   clone = v ? nir_shader_clone(NULL, fs->nir) : NULL;
   if (!clone) {
      FREE(v);
      return NULL;
   }
   util_dynarray_init(&v->native_c2_images, NULL);
   v->blend = want->blend;
   v->targets = want->targets;
   v->select = want->select;
   if (v->select)
      NIR_PASS(_, clone, nir_shader_intrinsics_pass, m9_select_store,
               nir_metadata_control_flow, &v->select);
   m9_fs_lower_fragcolor(clone, v->targets);
   memcpy(v->inline_values, want->inline_values, inline_bytes);
   memcpy(v->swizzle, want->swizzle, sizeof(v->swizzle));
   v->depth_texture = want->depth_texture;
   v->face_flip = want->face_flip;
   if (v->face_flip)
      m9_fs_flip_face(clone);
   v->srgb_texture = want->srgb_texture;
   if (inline_bytes)
      m9_nir_inline_uniforms(clone, v->inline_values);
   {
      nir_lower_tex_options tex = { 0 };

      for (unsigned s = 0; s < PIPE_MAX_SAMPLERS; s++) {
         if (v->swizzle[s][0] == PIPE_SWIZZLE_X &&
             v->swizzle[s][1] == PIPE_SWIZZLE_Y &&
             v->swizzle[s][2] == PIPE_SWIZZLE_Z &&
             v->swizzle[s][3] == PIPE_SWIZZLE_W)
            continue;
         /* PIPE_SWIZZLE_ZERO/ONE are nir_lower_tex's 4 and 5 */
         tex.swizzle_result |= BITFIELD_BIT(s);
         memcpy(tex.swizzles[s], v->swizzle[s], 4);
      }
      if (tex.swizzle_result)
         NIR_PASS(_, clone, nir_lower_tex, &tex);
   }
   /* Decode before the view swizzle: it rewrites the uses after the fetch,
    * the swizzle among them, so GL's swizzle reads the decoded texel. */
   m9_fs_lower_srgb_texture(clone, v->srgb_texture);
   /* After the view swizzle: nir_lower_tex rewrites every use of the fetch,
    * this one included, so a channel pick before it reads the swizzled
    * value instead of the texel. */
   if (v->depth_texture)
      NIR_PASS(_, clone, nir_shader_instructions_pass, m9_depth_channel_instr,
               nir_metadata_control_flow, &v->depth_texture);
   if (ctx->debug)
      nir_print_shader(clone, stderr);
   v->ok = m9_fs_compile_passes(clone, &v->blend, &v->passes, &v->reason);
   if (v->ok && v->passes.count > 1 && !m9_split_enable) {
      m9_fs_passes_fini(&v->passes);
      v->reason = "the multi-pass schedule is not enabled (M9X_SPLIT)";
      v->ok = false;
   }
   /* M9X_FS_REFUSED=1 prints the shader a variant was refused for, which
    * M9X_DEBUG also does but behind the whole job trace. */
   if (!v->ok && (ctx->debug ||
                  debug_get_bool_option("M9X_FS_REFUSED", false))) {
      fprintf(stderr, "m9x: variant refused: %s\n", v->reason);
      if (!ctx->debug)
         nir_print_shader(clone, stderr);
   }
   ralloc_free(clone);
   if (v->ok) {
      m9_record_layout(v);
      /* M9X_SPLIT_TRACE names the schedule the driver actually built, which
       * the offline test cannot: st hands the backend scalarised NIR. */
      if (m9_split_trace) {
         fprintf(stderr, "m9x schedule: %u passes, record %u dwords\n",
                 v->passes.count, v->record_dwords);
         for (unsigned i = 0; i < v->passes.count; i++) {
            const struct m9_fs_program *p = &v->passes.pass[i];

            fprintf(stderr, "m9x   pass %u slots %u units %u varyings %u "
                    "record %d offset %u routes", i, m9_fs_slot_count(p),
                    p->texture_count, p->varying_count, (int)p->writes_record,
                    v->record_offset[i]);
            for (unsigned k = 0; k < p->texture_count; k++)
               fprintf(stderr, " %d", p->texture_source[k] == M9_FS_ROUTE_NONE ?
                       -1 : (int)p->texture_source[k]);
            fprintf(stderr, " coord");
            for (unsigned k = 0; k < p->texture_count; k++)
               fprintf(stderr, " %d",
                       p->texcoord_source[k] == M9_FS_ROUTE_NONE ? -1 :
                       (int)p->texcoord_source[k]);
            fprintf(stderr, "\n");
         }
      }
      if (!m9_fs_streams(v)) {
         v->reason = "more CPU varying streams or texture slots than a job "
                     "binds";
         v->ok = false;
      } else if (v->extra_count > M9_FS_MAX_EXTRA) {
         v->reason = "more CPU varying streams than transient views";
         v->ok = false;
      }
      /* A varying on view 0 costs a raster stream without costing a CPU
       * view, so the check above does not cover a pass's own limit;
       * without this the job builds and its draws are dropped instead of
       * the shader falling back. */
      for (unsigned i = 0; v->ok && i < v->passes.count; i++)
         if (!m9_raster_fs_layout_ok(v->passes.pass[i].varying_count,
                                     v->stream[i].coord_count,
                                     v->passes.pass[i].texture_count)) {
            v->reason = "more raster streams than the pass has";
            v->ok = false;
         }
      if (v->ok) {
         for (unsigned i = 0; i < v->passes.count; i++) {
            const struct m9_fs_program *p = &v->passes.pass[i];
            unsigned units = MAX2(m9_fs_sampler_count(p), 1u);
            unsigned routes = p->texture_count - m9_fs_sampler_count(p);

            v->relocs_base += m9_draw_job_relocs(0, units) + routes;
            v->relocs_draw += units + 2;
            v->dwords_base += m9_draw_pass_dwords(1, units, routes);
            /* Plus the engine transition around each restated draw, the
             * same fourteen dwords m9_draw_pass_dwords budgets. */
            v->dwords_draw += M9_JOB_DRAW_DWORDS + 14;
         }
         if (m9_variant_relocs(v, 1) > M9_MAX_RELOCS) {
            v->reason = "the raster passes of this shader need more "
                        "relocations than one submission carries";
            v->ok = false;
         } else if (m9_variant_dwords(v, 1) > M9_MAX_COMMAND_DWORDS) {
            v->reason = "the raster passes of this shader need more "
                        "command words than one submission carries";
            v->ok = false;
         }
      }
      if (v->ok)
         v->ok = m9_fs_variant_upload(ctx, v, &v->reason);
      if (!v->ok)
         m9_fs_passes_fini(&v->passes);
   }
   slot = util_dynarray_grow(&fs->variants, struct m9_fs_variant *, 1);
   if (!slot) {
      if (v->ok) {
         m9_bo_destroy(ctx->screen, &v->c2);
         m9_fs_passes_fini(&v->passes);
      }
      FREE(v);
      return NULL;
   }
   *slot = v;
   *reason = v->reason;
   return v->ok ? v : NULL;
}

/* The view swizzle relative to what the sampler returns.  A format kept in
 * the 8-bit RGBA storage holds its natural RGBA value there (an L8 texel is
 * stored L,L,L,1, and the alpha-less code returns alpha 1), so a view
 * channel that selects the value the format puts there anyway -- st's XXX1
 * on L8, XYZ1 on an X8 format -- is the identity and needs no lowering in
 * the fragment program. */
static void
m9_view_swizzle(const struct pipe_sampler_view *view, uint8_t out[4])
{
   const uint8_t swz[4] = { view->swizzle_r, view->swizzle_g,
                            view->swizzle_b, view->swizzle_a };
   const struct util_format_description *desc =
      util_format_description(view->format);
   const struct m9_resource *res = view->texture ? m9_res(view->texture) : NULL;
   uint8_t code = res && res->texture.levels && !res->texture.block_bytes ?
                  res->texture_code : 0;
   bool plain = (code == 0xc8 || code == 0xc9 || code == M9_TEXTURE_CODE_BGRA) &&
                desc && view->format == res->base.format;

   for (unsigned c = 0; c < 4; c++) {
      unsigned src = swz[c] <= PIPE_SWIZZLE_W ? desc->swizzle[swz[c]] : swz[c];

      out[c] = plain && src == desc->swizzle[c] ? PIPE_SWIZZLE_X + c : swz[c];
   }
}

/* Variant of the bound fragment state for the current blend and inlined
 * uniform values. */
static const struct m9_fs_variant *
m9_ctx_variant(struct m9_context *ctx, const char **reason,
               unsigned *colormask)
{
   struct m9_fs_variant want = { .blend = { M9_FS_ONE, M9_FS_ZERO },
                                 .targets = MAX2(ctx->color_count, 1u),
                                 .select = ctx->mrt_select };

   enum pipe_format cf = ctx->framebuffer.cbufs[0].format;

   *reason = m9_blend_key(ctx->blend, &want.blend, colormask,
                          cf != PIPE_FORMAT_NONE && !util_format_has_alpha(cf),
                          cf);
   if (*reason)
      return NULL;
   want.blend.srgb = util_format_is_srgb(cf);
   memcpy(want.inline_values, ctx->fs_inline, sizeof(want.inline_values));
   /* The facing register's front is GL's counter-clockwise; st states the
    * winding against a viewport it inverts for window buffers. */
   if (ctx->rasterizer &&
       (ctx->fs->nir->info.inputs_read & VARYING_BIT_FACE))
      want.face_flip = ctx->rasterizer->front_ccw ==
                       (ctx->viewport.scale[1] > 0.0f);
   for (unsigned s = 0; s < PIPE_MAX_SAMPLERS; s++) {
      const struct pipe_sampler_view *view = ctx->fs_views[s];

      if (view && BITSET_TEST(ctx->fs->nir->info.textures_used, s)) {
         if (view->texture &&
             util_format_is_depth_or_stencil(view->texture->format))
            want.depth_texture |= BITFIELD_BIT(s);
         if (util_format_is_srgb(view->format))
            want.srgb_texture |= BITFIELD_BIT(s);
         m9_view_swizzle(view, want.swizzle[s]);
      } else {
         for (unsigned c = 0; c < 4; c++)
            want.swizzle[s][c] = PIPE_SWIZZLE_X + c;
      }
   }
   return m9_fs_variant(ctx, ctx->fs, &want, reason);
}

static void m9_fs_native_init(struct m9_context *ctx, struct m9_fs_state *fs);

/* The captured pass-through image cannot blend: a blended draw of that
 * shader compiles it for the native backend, whose tail does, except for
 * lines. */
static bool
m9_fs_use_native(struct m9_context *ctx)
{
   struct m9_fs_state *fs = ctx->fs;

   if (!fs || !fs->passthrough)
      return fs && fs->native;
   /* Blended lines have no captured raster class; they stay opaque.
    * Logic-op lines are drawn as quads (m9_render_line_quads). */
   if (!util_format_is_srgb(ctx->framebuffer.cbufs[0].format) &&
       !m9_logic_tail(ctx->blend) &&
       (!ctx->blend || !ctx->blend->rt[0].blend_enable || ctx->lines))
      return false;
   if (!fs->native_tried) {
      fs->native_tried = true;
      m9_fs_native_init(ctx, fs);
   }
   return fs->native;
}

static bool
m9_job_key_current(struct m9_context *ctx, struct m9_job_key *key)
{
   const struct pipe_depth_stencil_alpha_state *dsa = ctx->dsa;
   const struct pipe_blend_state *blend = ctx->blend;
   const struct pipe_rasterizer_state *rast = ctx->rasterizer;
   struct m9_resource *zs;
   unsigned colormask;
   bool test, write, stencil;

   if (ctx->lost)
      return m9_skip_reason(ctx, "the context is lost; draw skipped");
   if (!ctx->params_valid)
      return m9_skip_reason(ctx, "no valid raster parameters; draw skipped");
   zs = m9_context_depth(ctx);
   test = dsa && dsa->depth_enabled;
   if (test && !zs) {
      m9_warn_once(ctx, M9_WARN_DEPTH,
                   "depth test needs a D24S8 attachment of the target size; "
                   "drawing without depth");
      test = false;
   }
   write = test && dsa->depth_writemask;
   stencil = dsa && dsa->stencil[0].enabled;
   if (stencil && (!zs || !m9_zs_has_stencil(ctx))) {
      m9_warn_once(ctx, M9_WARN_STENCIL,
                   "stencil test needs a D24S8 attachment of the target "
                   "size; drawing without stencil");
      stencil = false;
   }
   /* st lowers the alpha test into the shader (caps->alpha_test). */
   if (dsa && dsa->alpha_enabled)
      m9_warn_once(ctx, M9_WARN_STENCIL, "a DSA alpha test is ignored");
   memset(key, 0, sizeof(*key));
   key->depth = zs ? zs : ctx->target_depth;
   key->aux = zs ? zs->hz : ctx->target_aux;
   key->depth_control = m9_depth_control(test, write,
                                         dsa ? dsa->depth_func : PIPE_FUNC_LESS);
   key->depth_ef = m9_depth_ef(test || stencil, write);
   key->stencil_a9 = M9_STENCIL_A9_OFF;
   key->stencil_aa = M9_STENCIL_AA_OFF;
   if (stencil) {
      struct m9_stencil_face front, back;
      bool two = dsa->stencil[1].enabled;

      m9_stencil_face(&dsa->stencil[0], ctx->stencil_ref.ref_value[0], &front);
      m9_stencil_face(&dsa->stencil[1], ctx->stencil_ref.ref_value[1], &back);
      key->stencil = true;
      m9_stencil_words(key->depth_control, &front, two ? &back : NULL,
                       !rast || rast->front_ccw, &key->depth_control,
                       &key->stencil_a9, &key->stencil_aa);
   }
   colormask = blend ? blend->rt[0].colormask : PIPE_MASK_RGBA;
   key->color_mask = m9_color_mask(colormask);
   key->color_mask_hi = 0xffffffffu;
   if (util_format_is_srgb(ctx->framebuffer.cbufs[0].format) &&
       (ctx->color_count != 1 || ctx->lines || !m9_fs_use_native(ctx)))
      return m9_skip_reason(ctx, "sRGB RT0 needs a native single-target "
                                  "triangle fragment program; draw skipped");
   for (unsigned p = 1; p < ctx->color_count; p++) {
      if (util_format_is_srgb(ctx->framebuffer.cbufs[p].format))
         return m9_skip_reason(ctx, "sRGB rendering past RT0 is not "
                                     "lowered; draw skipped");
   }
   if (m9_fs_use_native(ctx)) {
      const char *reason;

      key->fs = ctx->fs;
      key->variant = m9_ctx_variant(ctx, &reason, &colormask);
      if (!key->variant) {
         m9_warn_once(ctx, M9_WARN_BLEND, reason);
         return m9_skip_reason(ctx, reason);
      }
      key->color_mask = m9_color_mask(colormask);
      if (!m9_fs_resolve_consts(ctx, key->variant, &ctx->key_consts) ||
          !m9_job_textures(ctx, key->variant, key))
         return false;
   } else if (blend && blend->rt[0].blend_enable) {
      m9_warn_once(ctx, M9_WARN_BLEND, "blending needs a native fragment "
                   "program; drawing opaque");
   }
   /* GL discards colour writes into a framebuffer without a colour
    * attachment, and the surface bound for one is scratch. */
   if (ctx->color_discard)
      key->color_mask = m9_color_mask(0);
   /* ab/ac gate whole byte lanes, which cannot describe a 10-bit channel:
    * the vendor writes 0xffff for such a target and expresses the mask in
    * the fragment output instead (docs/parhelia-m9-gigacolor-10bit.md 4.2).
    * Until that lowering exists, a partial mask is refused rather than
    * applied to the wrong bits. */
   if (ctx->color_count < 2) {
      if (ctx->color &&
          m9_res(ctx->color)->format != M9_COLOR_FORMAT_A8R8G8B8 &&
          key->color_mask != 0xffffffffu)
         return m9_skip_reason(ctx, "a partial colour mask on a 10-bit "
                               "target needs the fragment output lowering; "
                               "draw skipped");
   } else {
      /* One sixteen-bit mask a target, targets 0 and 1 in ab and 2 and 3 in
       * ac.  A bound target the shader leaves unwritten keeps its contents:
       * every byte lane of its mask is gated off, so binding it is safe
       * without the program naming it. */
      const struct m9_fs_program *final = key->variant ?
                                          m9_fs_final(key->variant) : NULL;
      uint32_t masks[M9_DRAW_MAX_COLOR];

      if (!final)
         return m9_skip_reason(ctx, "more than one colour target needs a "
                               "native fragment program; draw skipped");
      if (final->target_count > ctx->color_count)
         return m9_skip_reason(ctx, "the shader writes a colour target the "
                               "framebuffer does not bind; draw skipped");
      for (unsigned s = 0; s < ctx->color_count; s++) {
         unsigned cm = blend && blend->independent_blend_enable ?
                       blend->rt[s].colormask : colormask;

         masks[s] = m9_color_mask((final->target_mask & BITFIELD_BIT(s)) ||
                                  m9_mrt_mask_all ? cm : 0) & 0xffffu;
         if (ctx->color_format[s] != M9_COLOR_FORMAT_A8R8G8B8 &&
             masks[s] != 0xffffu)
            return m9_skip_reason(ctx, "a partial colour mask on a 10-bit "
                                  "target needs the fragment output "
                                  "lowering; draw skipped");
      }
      key->color_mask = m9_color_mask_pair(masks, ctx->color_count, 0);
      key->color_mask_hi = m9_color_mask_pair(masks, ctx->color_count, 2);
   }
   if (rast && rast->scissor)
      return m9_scissor_key(ctx, &ctx->scissor, key);
   key->clip_min = ctx->params.clip_min;
   key->clip_max = ctx->params.clip_max;
   return true;
}

/* The native pass does not clip to the view volume, so x and y are held to
 * the viewport rectangle: pixel centres outside it belong to no primitive. */
static bool
m9_viewport_clip(const struct m9_context *ctx, struct m9_job_key *key)
{
   const struct pipe_viewport_state *vp = &ctx->viewport;
   float ax = fabsf(vp->scale[0]), ay = fabsf(vp->scale[1]);
   float x0 = ceilf(vp->translate[0] - ax - 0.5f);
   float x1 = ceilf(vp->translate[0] + ax - 0.5f);
   float y0 = ceilf(vp->translate[1] - ay - 0.5f);
   float y1 = ceilf(vp->translate[1] + ay - 0.5f);
   uint32_t lox = key->clip_min & 0xffffu, loy = key->clip_min >> 16;
   uint32_t hix = key->clip_max & 0xffffu, hiy = key->clip_max >> 16;

   lox = MAX2(lox, (uint32_t)CLAMP(x0, 0.0f, (float)hix));
   loy = MAX2(loy, (uint32_t)CLAMP(y0, 0.0f, (float)hiy));
   hix = MIN2(hix, (uint32_t)CLAMP(x1, (float)lox, (float)hix));
   hiy = MIN2(hiy, (uint32_t)CLAMP(y1, (float)loy, (float)hiy));
   m9_scissor_words(lox, loy, hix, hiy, &key->clip_min, &key->clip_max);
   return lox < hix && loy < hiy;
}

/* vbuf render */

static inline struct m9_vbuf_render *
m9_vbuf_render(struct vbuf_render *render)
{
   return (struct m9_vbuf_render *)render;
}

static const struct vertex_info *
m9_render_get_vertex_info(struct vbuf_render *render)
{
   struct m9_vbuf_render *r = m9_vbuf_render(render);
   int position = draw_find_shader_output(r->ctx->draw,
                                          TGSI_SEMANTIC_POSITION, 0);
   int color = draw_find_shader_output(r->ctx->draw, TGSI_SEMANTIC_COLOR, 0);
   const struct m9_fs_state *fs = r->ctx->fs;
   const struct m9_fs_variant *v = NULL;
   const char *reason;

   memset(&r->vinfo, 0, sizeof(r->vinfo));
   draw_emit_vertex_attr(&r->vinfo, EMIT_4F, MAX2(position, 0));
   r->has_color = color >= 0;
   if (r->has_color)
      draw_emit_vertex_attr(&r->vinfo, EMIT_4F, color);
   r->extra_count = 0;
   if (m9_fs_use_native(r->ctx))
      v = m9_ctx_variant(r->ctx, &reason, NULL);
   if (v) {
      unsigned offset = r->has_color ? 8 : 4;

      for (unsigned j = 0; j < v->extra_count; j++) {
         unsigned location = v->extra_location[j];
         unsigned name = 0, index = 0;
         int output;

         /* The draw module scans shader outputs with need_texcoord set. */
         if (location == M9_FS_COORD_CONSTANT) {
            output = -1;
         } else {
            tgsi_get_gl_varying_semantic(location, true, &name, &index);
            output = draw_find_shader_output(r->ctx->draw, name, index);
         }
         if (r->ctx->debug)
            fprintf(stderr, "m9x: CPU varying %u location %u semantic %u/%u "
                    "draw output %d\n", j, location, name, index, output);
         r->extra_offset[j] = output >= 0 ? (int)offset : -1;
         if (output >= 0) {
            draw_emit_vertex_attr(&r->vinfo, EMIT_4F, output);
            offset += 4;
         }
      }
      r->extra_count = v->extra_count;
   }
   draw_compute_vertex_size(&r->vinfo);
   return &r->vinfo;
}

static bool
m9_render_allocate_vertices(struct vbuf_render *render, uint16_t vertex_size,
                            uint16_t count)
{
   struct m9_vbuf_render *r = m9_vbuf_render(render);
   size_t bytes = (size_t)vertex_size * count;

   if (bytes > r->capacity) {
      uint8_t *vertices = realloc(r->vertices, bytes);

      if (!vertices)
         return false;
      r->vertices = vertices;
      r->capacity = bytes;
   }
   r->vertex_size = vertex_size;
   return true;
}

static void *
m9_render_map_vertices(struct vbuf_render *render)
{
   return m9_vbuf_render(render)->vertices;
}

static void
m9_render_unmap_vertices(struct vbuf_render *render, uint16_t min_index,
                         uint16_t max_index)
{
   (void)render;
   (void)min_index;
   (void)max_index;
}

static void
m9_render_set_primitive(struct vbuf_render *render, enum mesa_prim prim)
{
   m9_vbuf_render(render)->prim = prim;
   m9_vbuf_render(render)->ctx->lines =
      u_reduced_prim(prim) == MESA_PRIM_LINES;
}

static void
m9_render_record(const struct m9_vbuf_render *r, unsigned index, float *out,
                 float *extra)
{
   const float *v = (const float *)(r->vertices + (size_t)index * r->vertex_size);
   static const float missing[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

   for (unsigned j = 0; extra && j < r->extra_count; j++)
      memcpy(extra + 4 * j, r->extra_offset[j] >= 0 ?
             v + r->extra_offset[j] : missing, 4 * sizeof(float));

   /* The captured ortho program flips window y and maps z = 1 - 2 z_win.
    * The draw module already divided by w and left 1/w_clip in position w,
    * so the window coordinates are multiplied back out: the program's own
    * divide restores them and the setup engine keeps 1/w for
    * perspective-correct varyings. */
   const float w = v[3] > 0.0f ? 1.0f / v[3] : 1.0f;

   out[0] = v[0] * w;
   out[1] = ((float)r->ctx->params.height - v[1]) * w;
   out[2] = (1.0f - 2.0f * v[2]) * w;
   out[3] = w;
   for (unsigned c = 0; c < 4; c++)
      out[4 + c] = r->has_color ? CLAMP(v[4 + c], 0.0f, 1.0f) : 1.0f;
}

/* An extra slot m9_scale_coords divides by a projector: the vertex
 * component the projector reads and the transform it takes first. */
struct m9_proj_slot {
   unsigned j, comp;
   float scale, bias;
};

static unsigned
m9_proj_slots(const struct m9_job_key *key,
              struct m9_proj_slot out[M9_DRAW_MAX_UNITS])
{
   const struct m9_fs_variant *v = key->variant;
   uint32_t seen = 0;
   unsigned n = 0;

   for (unsigned u = 0; v && u < v->slot_count; u++) {
      const struct m9_fs_program *p = &v->passes.pass[v->slot_pass[u]];
      const struct m9_fs_pass_streams *st = &v->stream[v->slot_pass[u]];
      unsigned k = v->slot_index[u];
      const struct m9_fs_coord_affine *a = &p->texcoord_affine[k];
      int j;

      if (st->texture_coord[k] == M9_RASTER_COORD_NONE ||
          !p->texcoord_projective[k])
         continue;
      j = st->coord_extra[st->texture_coord[k]];
      if (j < 0 || (seen & BITFIELD_BIT(j)))
         continue;
      seen |= BITFIELD_BIT(j);
      out[n].j = j;
      out[n].comp = a->swizzled ? a->comp[3] : 3;
      out[n].scale = a->present ? key->coord_scale[u][3] : 1.0f;
      out[n].bias = a->present ? key->coord_bias[u][3] : 0.0f;
      n++;
   }
   return n;
}

/* Vertices one primitive of this topology owns alone, or zero for a strip,
 * fan or polygon: those share vertices, so a value that is constant over
 * every primitive of the run is constant over the whole run. */
static unsigned
m9_topology_group(enum m9_topology topology)
{
   switch (topology) {
   case M9_TOPOLOGY_LINES: return 2;
   case M9_TOPOLOGY_TRIANGLES: return 3;
   case M9_TOPOLOGY_QUADS: return 4;
   default: return 0;
   }
}

/* Projective coordinates are divided per vertex: exact while q is constant
 * over a primitive, which m9_proj_constant checks before the job is built.
 * Render-target coordinates address the aligned span. */
static void
m9_scale_coords(const struct m9_job_key *key, float *extra)
{
   const struct m9_fs_variant *v = key->variant;
   uint32_t divided = 0, folded = 0, scaled = 0;

   uint32_t packed = 0;

   for (unsigned u = 0; u < v->slot_count; u++) {
      const struct m9_fs_program *p = &v->passes.pass[v->slot_pass[u]];
      const struct m9_fs_pass_streams *st = &v->stream[v->slot_pass[u]];
      unsigned k = v->slot_index[u];
      unsigned coord = st->texture_coord[k];
      int j = coord == M9_RASTER_COORD_NONE ? -1 : st->coord_extra[coord];
      unsigned c = p->texcoord_components[k];

      if (j < 0)
         continue;
      /* Components in another order are gathered from the vertex. */
      if (p->texcoord_affine[k].swizzled && !(packed & BITFIELD_BIT(j))) {
         float v[4];

         for (unsigned i = 0; i < 4; i++)
            v[i] = extra[4 * j + p->texcoord_affine[k].comp[i]];
         memcpy(extra + 4 * j, v, sizeof(v));
         packed |= BITFIELD_BIT(j);
      }
      /* A stream carries one pair; anything but the low one is moved down,
       * since the program reads its coordinate attribute as xy. */
      if (c && !(packed & BITFIELD_BIT(j))) {
         extra[4 * j] = extra[4 * j + c];
         extra[4 * j + 1] = extra[4 * j + c + 1];
         packed |= BITFIELD_BIT(j);
      }
      if (p->texcoord_affine[k].present && !(folded & BITFIELD_BIT(j))) {
         unsigned n = p->texcoord_shadow[k] || p->texcoord_volume[k] ? 3 : 2;

         for (unsigned i = 0; i < 4; i++) {
            /* A constant term ignores the stream, whatever it holds. */
            if (i >= n && (i < 3 || !p->texcoord_projective[k]))
               continue;
            extra[4 * j + i] = key->coord_scale[u][i] == 0.0f ?
                               key->coord_bias[u][i] :
                               extra[4 * j + i] * key->coord_scale[u][i] +
                               key->coord_bias[u][i];
         }
         folded |= BITFIELD_BIT(j);
      }
      if (p->texcoord_projective[k] && !(divided & BITFIELD_BIT(j))) {
         float q = extra[4 * j + 3];

         if (q != 0.0f && q != 1.0f) {
            extra[4 * j] /= q;
            extra[4 * j + 1] /= q;
            if (p->texcoord_shadow[k] || p->texcoord_volume[k])
               extra[4 * j + 2] /= q;
            extra[4 * j + 3] = 1.0f;
         }
         divided |= BITFIELD_BIT(j);
      }
      /* The stream is shared, so its scale is applied once like every other
       * step here: two units on one coordinate scaled it twice. */
      if (!(scaled & BITFIELD_BIT(j))) {
         extra[4 * j] *= key->unit_scale[u][0];
         extra[4 * j + 1] *= key->unit_scale[u][1];
         scaled |= BITFIELD_BIT(j);
      }
   }
}

static inline uint32_t
m9_index_at(const void *indices, unsigned index_size, unsigned i)
{
   return index_size == 4 ? ((const uint32_t *)indices)[i] :
                            ((const uint16_t *)indices)[i];
}

/* The projector divide below is per vertex, which is exact only while the
 * projector is constant over a primitive: the engine interpolates s/q and
 * t/q, and a q that varies makes that differ from the S/Q the fetch needs.
 * MEASURED: a projector that quadruples across a quad puts 75373 of 76800
 * pixels at the wrong texel (fixture projtexz), so a draw that breaks the
 * precondition is refused instead of drawn.  The comparison carries a
 * relative tolerance because a projector a vertex shader recomputes per
 * vertex is only bitwise equal when the arithmetic is. */
#define M9_PROJ_TOLERANCE 1.0e-6f

static const char m9_proj_reason[] =
   "a projective texture coordinate whose projector varies over a primitive "
   "is divided per vertex, not per pixel; draw skipped";

static bool
m9_proj_constant(const struct m9_job_key *key, enum m9_topology topology,
                 const float *extra, unsigned extra_count,
                 const void *indices, unsigned index_size, unsigned offset,
                 uint32_t lo, unsigned count)
{
   struct m9_proj_slot slot[M9_DRAW_MAX_UNITS];
   unsigned slots = m9_proj_slots(key, slot);
   unsigned span = m9_topology_group(topology);

   if (!slots || !extra_count)
      return true;
   if (!span)
      span = count;
   for (unsigned base = 0; base + span <= count; base += span) {
      for (unsigned s = 0; s < slots; s++) {
         const struct m9_proj_slot *ps = &slot[s];
         unsigned i0 = indices ?
                       m9_index_at(indices, index_size, offset + base) - lo :
                       base;
         float q = ps->scale == 0.0f ? ps->bias :
                   extra[(size_t)i0 * 4 * extra_count + 4 * ps->j + ps->comp] *
                   ps->scale + ps->bias;
         float tol = M9_PROJ_TOLERANCE * MAX2(fabsf(q), 1.0f);

         if (ps->scale == 0.0f)
            continue;
         for (unsigned k = 1; k < span; k++) {
            unsigned i = indices ?
                         m9_index_at(indices, index_size,
                                     offset + base + k) - lo :
                         base + k;
            float qk = extra[(size_t)i * 4 * extra_count + 4 * ps->j +
                             ps->comp] * ps->scale + ps->bias;

            if (fabsf(qk - q) > tol)
               return false;
         }
      }
   }
   return true;
}


/* Lowest and highest vertex a run of indices refers to. */
static void
m9_index_range(const void *indices, unsigned index_size, unsigned first,
               unsigned count, uint32_t *lo, uint32_t *hi)
{
   uint32_t a = UINT32_MAX, b = 0;

   for (unsigned i = 0; i < count; i++) {
      uint32_t v = m9_index_at(indices, index_size, first + i);

      a = MIN2(a, v);
      b = MAX2(b, v);
   }
   *lo = a;
   *hi = b;
}

/* Geometry a producer could not place in a job.  It used to be dropped
 * without a word, which is how a mesh under a split shader lost every run but
 * its last; the skip report names it now. */
static void
m9_render_drop(struct m9_context *ctx)
{
   m9_warn_once(ctx, M9_WARN_FS,
                "a draw's records did not fit one job; draws skipped");
   m9_skip_reason(ctx, "a draw's records did not fit one job");
   ctx->skipped_draws++;
   m9_prof_skipped++;
}

/* An index list preprocesses the vertices it spans once and rasterises them
 * through a stored index run, instead of one record per index. */
static void
m9_render_append_indexed(struct m9_vbuf_render *r, const struct m9_job_key *key,
                         enum m9_topology topology, bool lines, uint32_t cull,
                         const void *indices, unsigned index_size,
                         unsigned count)
{
   struct m9_context *ctx = r->ctx;
   const unsigned max = m9_job_max_vertices(key);
   /* A list splits only on a whole primitive. */
   unsigned grain = topology == M9_TOPOLOGY_TRIANGLES ? 3 :
                    topology == M9_TOPOLOGY_LINES ? 2 : count;

   for (unsigned done = 0; done < count;) {
      unsigned n = count - done, vertices;
      uint32_t lo, hi, first, index_first;
      float *out, *extra;
      uint16_t *words;

      m9_index_range(indices, index_size, done, n, &lo, &hi);
      /* Halve the run until the vertices it spans fit one job's records. */
      while (hi - lo + 1 > max) {
         n = n / 2 / grain * grain;
         if (!n) {
            m9_render_drop(ctx);
            return;
         }
         m9_index_range(indices, index_size, done, n, &lo, &hi);
      }
      vertices = hi - lo + 1;
      out = m9_job_reserve(ctx, key, &ctx->key_consts, NULL, M9_RECORD_FLOATS,
                           vertices, &first, &extra);
      if (!out || (extra && r->extra_count != key->variant->extra_count)) {
         m9_render_drop(ctx);
         return;
      }
      /* An odd run is padded to a whole dword so every run stays aligned,
       * and M9_INDEX_TAIL halves beyond that repeat the last index. */
      index_first = util_dynarray_num_elements(&ctx->indices, uint16_t);
      words = util_dynarray_grow(&ctx->indices, uint16_t,
                                 n + (n & 1) + M9_INDEX_TAIL);
      if (!words) {
         ctx->records.size = (size_t)first * M9_RECORD_FLOATS * sizeof(float);
         if (extra)
            ctx->extra.size = (size_t)first * 4 * r->extra_count * sizeof(float);
         m9_warn_once(ctx, M9_WARN_MEMORY, "out of memory for raster indices");
         return;
      }
      for (unsigned v = 0; v < vertices; v++)
         m9_render_record(r, lo + v, out + v * M9_RECORD_FLOATS,
                          extra ? extra + v * 4 * r->extra_count : NULL);
      if (extra &&
          !m9_proj_constant(key, topology, extra, r->extra_count, indices,
                            index_size, done, lo, n)) {
         ctx->records.size = (size_t)first * M9_RECORD_FLOATS * sizeof(float);
         ctx->extra.size = (size_t)first * 4 * r->extra_count * sizeof(float);
         ctx->indices.size = (size_t)index_first * sizeof(uint16_t);
         m9_warn_once(ctx, M9_WARN_FS, m9_proj_reason);
         return;
      }
      for (unsigned v = 0; extra && v < vertices; v++)
         m9_scale_coords(key, extra + v * 4 * r->extra_count);
      for (unsigned i = 0; i < n; i++)
         words[i] = first +
                    m9_index_at(indices, index_size, done + i) - lo;
      for (unsigned i = n; i < n + (n & 1) + M9_INDEX_TAIL; i++)
         words[i] = words[n - 1];
      m9_prof_indexed += n;
      m9_prof_referenced += vertices;
      m9_job_commit(ctx, out, extra, vertices, first, index_first, n, topology,
                    cull, lines,
                    ctx->rasterizer && ctx->rasterizer->flatshade);
      done += n;
   }
}

static void
m9_render_append(struct m9_vbuf_render *r, const struct m9_job_key *key,
                 enum m9_topology topology, bool lines, uint32_t cull,
                 const void *indices, unsigned index_size, unsigned start,
                 unsigned count)
{
   const unsigned max = m9_job_max_vertices(key);
   bool list = topology == M9_TOPOLOGY_LINES ||
               topology == M9_TOPOLOGY_TRIANGLES;
   /* A list splits only on a whole primitive: the ceiling is a multiple of
    * four, which is not a multiple of three for every pass count. */
   unsigned grain = topology == M9_TOPOLOGY_TRIANGLES ? 3 :
                    topology == M9_TOPOLOGY_LINES ? 2 : 1;

   if (indices && m9_indexed_draws) {
      m9_render_append_indexed(r, key, topology, lines, cull, indices,
                               index_size, count);
      return;
   }
   /* Native connected primitives are bounded by max_vertex_buffer_bytes;
    * lists split on any multiple of six. */
   if (count > max && !list) {
      m9_render_drop(r->ctx);
      return;
   }
   for (unsigned done = 0; done < count;) {
      unsigned n = MIN2(count - done, max / grain * grain);
      uint32_t first;
      float *extra;
      float *out = m9_job_reserve(r->ctx, key, &r->ctx->key_consts, NULL,
                                  M9_RECORD_FLOATS, n, &first, &extra);

      if (!out || (extra && r->extra_count != key->variant->extra_count)) {
         m9_render_drop(r->ctx);
         return;
      }
      for (unsigned i = 0; i < n; i++) {
         unsigned index = !indices ? start + done + i :
                          m9_index_at(indices, index_size, done + i);

         m9_render_record(r, index, out + i * M9_RECORD_FLOATS,
                          extra ? extra + i * 4 * r->extra_count : NULL);
      }
      /* The records are already in primitive order here, indexed or not. */
      if (extra &&
          !m9_proj_constant(key, topology, extra, r->extra_count, NULL, 0, 0,
                            0, n)) {
         r->ctx->records.size =
            (size_t)first * M9_RECORD_FLOATS * sizeof(float);
         r->ctx->extra.size =
            (size_t)first * 4 * r->extra_count * sizeof(float);
         m9_warn_once(r->ctx, M9_WARN_FS, m9_proj_reason);
         return;
      }
      for (unsigned i = 0; extra && i < n; i++)
         m9_scale_coords(key, extra + i * 4 * r->extra_count);
      m9_job_commit(r->ctx, out, extra, n, first, 0, 0, topology, cull, lines,
                    r->ctx->rasterizer && r->ctx->rasterizer->flatshade);
      done += n;
   }
}

/* The provoking vertex flat shading takes; quads keep the last one whatever
 * the convention, since quads_follow_provoking_vertex_convention is unset. */
static unsigned
m9_flat_pv(const struct pipe_rasterizer_state *rast, enum mesa_prim prim)
{
   if (prim == MESA_PRIM_QUADS || prim == MESA_PRIM_QUAD_STRIP)
      return PV_LAST;
   return rast && rast->flatshade && rast->flatshade_first ? PV_FIRST :
                                                             PV_LAST;
}

static bool
m9_native_topology(enum mesa_prim prim, unsigned in_pv,
                   enum m9_topology *topology)
{
   switch (prim) {
   case MESA_PRIM_LINES: *topology = M9_TOPOLOGY_LINES; break;
   case MESA_PRIM_LINE_STRIP: *topology = M9_TOPOLOGY_LINE_STRIP; break;
   case MESA_PRIM_TRIANGLES: *topology = M9_TOPOLOGY_TRIANGLES; break;
   case MESA_PRIM_TRIANGLE_STRIP: *topology = M9_TOPOLOGY_TRIANGLE_STRIP; break;
   case MESA_PRIM_TRIANGLE_FAN: *topology = M9_TOPOLOGY_TRIANGLE_FAN; break;
   case MESA_PRIM_QUADS: *topology = M9_TOPOLOGY_QUADS; break;
   case MESA_PRIM_QUAD_STRIP: *topology = M9_TOPOLOGY_QUAD_STRIP; break;
   case MESA_PRIM_POLYGON: *topology = M9_TOPOLOGY_POLYGON; return true;
   default: return false;
   }
   /* Hardware flat shading provokes with the last vertex. */
   return in_pv == PV_LAST;
}

static void *
m9_render_scratch(struct m9_vbuf_render *r, size_t bytes)
{
   util_dynarray_clear(&r->indices);
   return util_dynarray_grow_bytes(&r->indices, 1, bytes);
}

/* The line raster class with a destination read is not captured, so a
 * logic-op line is drawn as the quad draw's wide-line stage builds: offset
 * across the major axis, shifted half a pixel along it, two triangles. */
static void
m9_render_line_quads(struct m9_vbuf_render *r, const struct m9_job_key *key,
                     const uint16_t *indices, unsigned start, unsigned count)
{
   const struct pipe_rasterizer_state *rast = r->ctx->rasterizer;
   const unsigned in_pv = m9_flat_pv(rast, r->prim);
   const bool flat = rast && rast->flatshade;
   const bool center = !rast || rast->half_pixel_center;
   const float half = 0.5f * (rast ? rast->line_width : 1.0f);
   const float bias = center ? 0.125f : 0.0f;
   const unsigned vs = r->vertex_size;
   enum mesa_prim out_prim;
   unsigned out_size, out_count;
   uint8_t *saved, *out;
   void *list;

   if (indices) {
      u_translate_func translate;

      if (u_index_translator(BITFIELD_BIT(MESA_PRIM_LINES), r->prim, 2, count,
                             in_pv, PV_FIRST, PR_DISABLE, &out_prim, &out_size,
                             &out_count, &translate) == U_TRANSLATE_ERROR ||
          !(list = m9_render_scratch(r, (size_t)out_count * out_size)))
         return;
      translate(indices, 0, count, out_count, 0, list);
   } else {
      u_generate_func generate;

      if (u_index_generator(BITFIELD_BIT(MESA_PRIM_LINES), r->prim, start,
                            count, in_pv, PV_FIRST, &out_prim, &out_size,
                            &out_count, &generate) == U_TRANSLATE_ERROR ||
          !(list = m9_render_scratch(r, (size_t)out_count * out_size)))
         return;
      generate(start, out_count, list);
   }
   util_dynarray_clear(&r->quads);
   out = util_dynarray_grow_bytes(&r->quads, 6 * (out_count / 2), vs);
   if (!out) {
      m9_warn_once(r->ctx, M9_WARN_MEMORY, "out of memory for line quads");
      return;
   }
   for (unsigned l = 0; l + 1 < out_count; l += 2) {
      const uint8_t *v[2] = {
         r->vertices + (size_t)m9_index_at(list, out_size, l) * vs,
         r->vertices + (size_t)m9_index_at(list, out_size, l + 1) * vs,
      };
      const float *p0 = (const float *)v[0], *p1 = (const float *)v[1];
      const bool xmajor = fabsf(p0[0] - p1[0]) > fabsf(p0[1] - p1[1]);
      const unsigned along = xmajor ? 0 : 1;
      const float shift = !center ? 0.0f : p0[along] < p1[along] ? -0.5f : 0.5f;
      /* Corners 0, 1 at the first end, 2, 3 at the second; tris 023, 031. */
      static const uint8_t order[6] = { 0, 2, 3, 0, 3, 1 };

      for (unsigned i = 0; i < 6; i++) {
         unsigned corner = order[i];
         uint8_t *dst = out + ((size_t)l * 3 + i) * vs;
         float *pos = (float *)dst;
         float side = corner & 1 ? half : -half;

         memcpy(dst, v[corner >> 1], vs);
         /* The first vertex provokes after the translation above. */
         if (flat)
            memcpy(dst + 4 * sizeof(float), v[0] + 4 * sizeof(float),
                   vs - 4 * sizeof(float));
         pos[along] += shift;
         pos[!along] += xmajor ? side - bias : side + bias;
      }
   }
   saved = r->vertices;
   r->vertices = r->quads.data;
   m9_render_append(r, key, M9_TOPOLOGY_TRIANGLES, false, 0, NULL, 0, 0,
                    3 * out_count);
   r->vertices = saved;
}

static void
m9_render_emit(struct m9_vbuf_render *r, const uint16_t *indices,
               unsigned start, unsigned count)
{
   struct m9_context *ctx = r->ctx;
   const struct pipe_rasterizer_state *rast = ctx->rasterizer;
   const unsigned lists = BITFIELD_BIT(MESA_PRIM_LINES) |
                          BITFIELD_BIT(MESA_PRIM_TRIANGLES);
   enum mesa_prim prim = r->prim, out_prim = r->prim;
   unsigned in_pv = m9_flat_pv(rast, prim);
   unsigned out_size = 0, out_count = count;
   enum m9_topology topology;
   const void *list = NULL;
   struct m9_job_key key;
   uint64_t t_emit = m9_prof_begin();
   uint32_t cull = 0;

   if (!count)
      return;
   if (!m9_job_key_current(ctx, &key)) {
      /* Every inner path records its own reason; this names the rest. */
      m9_skip_reason(ctx, "the job state is unsupported; draw skipped");
      ctx->skipped_draws++;
      return;
   }
   /* A killed client never destroys its context, so report as we go. */
   if (ctx->debug && ++ctx->submitted_draws % 200 == 0)
      fprintf(stderr, "m9x: %u draws submitted, %u skipped\n",
              ctx->submitted_draws, ctx->skipped_draws);
   if (u_reduced_prim(prim) == MESA_PRIM_POINTS) {
      m9_warn_once(ctx, M9_WARN_POINTS, "unexpanded points are not drawn");
      return;
   }
   if (u_reduced_prim(prim) == MESA_PRIM_LINES &&
       m9_logic_tail(ctx->blend)) {
      m9_render_line_quads(r, &key, indices, start, count);
      m9_prof_end(M9_PROF_EMIT, t_emit);
      return;
   }
   if (u_reduced_prim(prim) == MESA_PRIM_TRIANGLES && rast) {
      if (rast->cull_face == PIPE_FACE_FRONT_AND_BACK)
         return;
      cull = m9_cull_ef(rast->cull_face, rast->front_ccw);
   }
   if (indices || !m9_native_topology(prim, in_pv, &topology)) {
      void *buffer;

      if (indices) {
         u_translate_func translate;

         if (u_index_translator(lists, prim, 2, count, in_pv, PV_LAST,
                                PR_DISABLE, &out_prim, &out_size, &out_count,
                                &translate) == U_TRANSLATE_ERROR)
            return;
         buffer = m9_render_scratch(r, (size_t)out_count * out_size);
         if (!buffer)
            return;
         translate(indices, 0, count, out_count, 0, buffer);
      } else {
         u_generate_func generate;

         if (u_index_generator(lists, prim, start, count, in_pv, PV_LAST,
                               &out_prim, &out_size, &out_count,
                               &generate) == U_TRANSLATE_ERROR)
            return;
         buffer = m9_render_scratch(r, (size_t)out_count * out_size);
         if (!buffer)
            return;
         generate(start, out_count, buffer);
      }
      list = buffer;
      topology = out_prim == MESA_PRIM_LINES ? M9_TOPOLOGY_LINES :
                                               M9_TOPOLOGY_TRIANGLES;
   }
   m9_render_append(r, &key, topology,
                    u_reduced_prim(out_prim) == MESA_PRIM_LINES, cull, list,
                    out_size, start, out_count);
   m9_prof_end(M9_PROF_EMIT, t_emit);
}

static void
m9_render_draw_elements(struct vbuf_render *render, const uint16_t *indices,
                        unsigned count)
{
   m9_render_emit(m9_vbuf_render(render), indices, 0, count);
}

static void
m9_render_draw_arrays(struct vbuf_render *render, unsigned start,
                      unsigned count)
{
   m9_render_emit(m9_vbuf_render(render), NULL, start, count);
}

static void
m9_render_release_vertices(struct vbuf_render *render)
{
   (void)render;
}

static void
m9_render_destroy(struct vbuf_render *render)
{
   struct m9_vbuf_render *r = m9_vbuf_render(render);

   util_dynarray_fini(&r->indices);
   util_dynarray_fini(&r->quads);
   free(r->vertices);
   FREE(r);
}

static struct m9_vbuf_render *
m9_render_create(struct m9_context *ctx)
{
   struct m9_vbuf_render *r = CALLOC_STRUCT(m9_vbuf_render);

   if (!r)
      return NULL;
   r->ctx = ctx;
   /* A draw's vertices must fit the transient stream views. */
   r->base.max_indices = m9_draw_max_source_vertices();
   r->base.max_vertex_buffer_bytes =
      m9_draw_max_source_vertices() * M9_TRANSIENT_VERTEX_BYTES;
   r->base.get_vertex_info = m9_render_get_vertex_info;
   r->base.allocate_vertices = m9_render_allocate_vertices;
   r->base.map_vertices = m9_render_map_vertices;
   r->base.unmap_vertices = m9_render_unmap_vertices;
   r->base.set_primitive = m9_render_set_primitive;
   r->base.draw_elements = m9_render_draw_elements;
   r->base.draw_arrays = m9_render_draw_arrays;
   r->base.release_vertices = m9_render_release_vertices;
   r->base.destroy = m9_render_destroy;
   util_dynarray_init(&r->indices, NULL);
   util_dynarray_init(&r->quads, NULL);
   return r;
}

/* Drawing */

/* Native vertex path: a covered vertex shader runs as the preprocessing
 * program over the raw attributes, so the CPU neither transforms nor clips.
 * The appended tail divides by w, applies the viewport in force and writes
 * the four streams the raster pass reads; the one varying it carries is
 * stream 0, which is why only a primary-colour varying and no CPU-written
 * texture coordinate can be served this way. */

/* Fragment slots of the captured pass-through image; the vertex program
 * starts behind them, which is why its preprocessing entry is 4. */
#define M9_PASSTHROUGH_FS_SLOTS 4u

/* Vertex buffers of one native draw, mapped as the draw module maps them. */
struct m9_native_vs {
   struct m9_vs_source_layout layout;
   const uint8_t *map[PIPE_MAX_ATTRIBS];
   unsigned size[PIPE_MAX_ATTRIBS];
   unsigned buffer_count;
   const void *indices;       /* NULL runs a numeric record range */
   unsigned index_size;
   int index_bias;
   uint32_t first_vertex, vertex_count;   /* the range the records cover */
   enum m9_topology topology;
   uint32_t cull;
   bool lines, flat;
};

/* The texel slot of the final raster pass a coordinate stream feeds, or ~0.
 * Two units sharing a stream agree in transform and span -- m9_job_key
 * refuses a draw where they do not -- so the first is enough. */
static unsigned
m9_coord_stream_slot(const struct m9_fs_variant *v, unsigned pass,
                     unsigned coord)
{
   const struct m9_fs_pass_streams *st = &v->stream[pass];
   const struct m9_fs_program *fp = &v->passes.pass[pass];

   for (unsigned k = 0; k < fp->texture_count; k++)
      if (st->texture_coord[k] == coord && st->unit[k] < M9_DRAW_MAX_UNITS)
         return k;
   return ~0u;
}

/* The multiply-add a coordinate stream applies, which is what m9_scale_coords
 * does per vertex on the CPU: the sampler's own affine transform, then the
 * span the render target's descriptor addresses.  The transform's own terms
 * are only resolved for a unit that carries one, so a plain fetch scales by
 * the span alone. */
static void
m9_vs_coord_coefficients(const struct m9_job_key *key, unsigned pass,
                         unsigned coord, unsigned part, float *out)
{
   unsigned k = key->variant && pass < key->variant->passes.count ?
                m9_coord_stream_slot(key->variant, pass, coord) : ~0u;
   const struct m9_fs_program *fp;
   bool affine;
   unsigned u;

   out[0] = out[1] = part == M9_VS_COORD_SCALE ? 1.0f : 0.0f;
   if (k == ~0u)
      return;
   fp = &key->variant->passes.pass[pass];
   affine = fp->texcoord_affine[k].present;
   u = key->variant->stream[pass].unit[k];
   for (unsigned i = 0; i < 2; i++) {
      float span = key->unit_scale[u][i];

      out[i] = part == M9_VS_COORD_SCALE ?
               (affine ? key->coord_scale[u][i] : 1.0f) * span :
               (affine ? key->coord_bias[u][i] : 0.0f) * span;
   }
}

/* Constant pairs the compiled program names, resolved against constant
 * buffer 0: two floats per user constant, in program order. */
static bool
m9_vs_resolve_consts(struct m9_context *ctx, const struct m9_job_key *key,
                     const struct m9_vs_program *p, struct util_dynarray *out)
{
   const struct pipe_constant_buffer *cb = &ctx->vs_constants[0];
   const uint8_t *data = cb->user_buffer ? cb->user_buffer :
                         cb->buffer ? m9_res(cb->buffer)->map : NULL;
   size_t size = data ? cb->buffer_size : 0;

   if (data)
      data += cb->buffer_offset;
   util_dynarray_clear(out);
   util_dynarray_foreach(&p->consts, struct m9_vs_const, k) {
      float pair[2] = { 0.0f, 0.0f };

      if (k->kind == M9_VS_CONST_COORD) {
         const struct m9_vs_output *o = &p->outputs.stream[k->coord_stream];

         m9_vs_coord_coefficients(key, o->pass, o->coord, k->coord_part,
                                  pair);
         util_dynarray_append_typed(out, float, pair[0]);
         util_dynarray_append_typed(out, float, pair[1]);
         continue;
      }
      for (unsigned lane = 0; lane < 2; lane++) {
         float v = 0.0f;

         if (k->kind == M9_VS_CONST_UNIFORM &&
             k->offset[lane] != M9_VS_UNIFORM_ZERO) {
            if ((size_t)k->offset[lane] + sizeof(float) > size)
               return m9_skip_reason(ctx, "vertex constants outside the "
                                     "bound constant buffer; draw skipped");
            memcpy(&v, data + k->offset[lane], sizeof(v));
         } else if (k->kind == M9_VS_CONST_LITERAL) {
            v = k->value[lane];
         }
         util_dynarray_append_typed(out, float, v);
      }
   }
   return true;
}

/* The fused pass carries a second varying on its own output target 3, which
 * costs the tail's scratch a register.  Below the cap nothing changes: the
 * tail stays byte for byte the captured one and the pass binds three
 * targets, the shape every hardware fixture has run. */
static bool
m9_vs_fused_pair(const struct m9_vs_program *p)
{
   return p->varying_count > 1 && m9_native_vs_varyings > 1;
}


/* The c2 image a native preprocessing job binds: the fragment slots the
 * raster pass enters, with the compiled vertex program behind them instead
 * of the captured one.  One image per fragment variant, rebuilt when a
 * different vertex program binds. */
static uint32_t
m9_native_vs_c2(struct m9_context *ctx, struct m9_fs_variant *v,
                const struct m9_vs_variant *vs,
                unsigned *extra_entry, unsigned *gather_entry,
                unsigned *producer_entry)
{
   struct m9_resource *image = v ? &v->native_c2 : &ctx->vs_c2;
   struct m9_resource fresh = {0};
   struct util_dynarray *images = v ? &v->native_c2_images : &ctx->vs_c2_images;
   uint64_t *serial = v ? &v->native_c2_serial : &ctx->vs_c2_serial;
   const uint32_t *fs_slots = v ? m9_fs_final(v)->slots.data :
                                  m9_passthrough_c2_program;
   unsigned fs_slots_count = v ? m9_fs_slot_count(m9_fs_final(v)) :
                                 M9_PASSTHROUGH_FS_SLOTS;
   unsigned entry = 0, extra = 0, gather = 0, producer = 0;
   uint64_t bytes = align64(8ull * (fs_slots_count +
                                    m9_vs_slot_count(&vs->program) +
                                    m9_vs_extra_slot_count(&vs->program) +
                                    m9_vs_gather_slot_count(&vs->program) +
                                    m9_vs_producer_slot_count(&vs->program)),
                            0x1000);

   /* The second head sits behind the first and the gather behind that, so
    * both slots follow from the fragment slots this variant put in front. */
   *extra_entry = m9_vs_extra_slot_count(&vs->program) ?
                  fs_slots_count + m9_vs_slot_count(&vs->program) : 0;
   *gather_entry = m9_vs_gather_slot_count(&vs->program) ?
                   fs_slots_count + m9_vs_slot_count(&vs->program) +
                   m9_vs_extra_slot_count(&vs->program) : 0;
   *producer_entry = m9_vs_producer_slot_count(&vs->program) ?
      fs_slots_count + m9_vs_slot_count(&vs->program) +
      m9_vs_extra_slot_count(&vs->program) +
      m9_vs_gather_slot_count(&vs->program) : 0;
   if (ctx->screen->queued_submit) {
      /* This runs before a changed job key flushes earlier recorded draws.
       * Keep their handle alive as well as the kernel-owned posted readers;
       * a fresh image per serial avoids both CPU rewrites and forced waits. */
      util_dynarray_foreach(images, struct m9_native_image, entry)
         if (entry->serial == vs->serial)
            return entry->image.handle;
      image = &fresh;
   }
   if (!ctx->screen->queued_submit && image->handle && *serial == vs->serial)
      return image->handle;
   if (bytes > M9_PROGRAM_MAX_BYTES)
      return 0;
   /* A different vertex program binding behind the same fragment slots can
    * need a longer image than the one already cached. */
   if (image->handle && image->size < bytes)
      m9_bo_destroy(ctx->screen, image);
   if (!image->handle &&
       m9_bo_create(ctx->screen, bytes, PARHELIA_M9_BO_VRAM,
                    M9_RESOURCE_SYSTEM_STATE, image))
      return 0;
   memset(image->map, 0, image->size);
   if (!m9_vs_c2_image(&vs->program, fs_slots, fs_slots_count, image->map,
                       image->size / sizeof(uint32_t),
                       m9_vs_fused_pair(&vs->program), &entry, &extra,
                       &gather, &producer) ||
       entry != fs_slots_count || extra != *extra_entry ||
       gather != *gather_entry || producer != *producer_entry) {
      if (ctx->screen->queued_submit)
         m9_bo_destroy(ctx->screen, image);
      return 0;
   }
   if (ctx->screen->queued_submit) {
      struct m9_native_image *entry =
         util_dynarray_grow(images, struct m9_native_image, 1);

      if (!entry) {
         m9_bo_destroy(ctx->screen, image);
         return 0;
      }
      *entry = (struct m9_native_image) { .serial = vs->serial, .image = fresh };
      if (ctx->debug)
         fprintf(stderr, "m9x: queued C2 owner %p VS serial %" PRIu64
                 " handle %u images %u\n", (void *)images, vs->serial,
                 fresh.handle,
                 (unsigned)util_dynarray_num_elements(images, struct m9_native_image));
      return fresh.handle;
   }
   *serial = vs->serial;
   return image->handle;
}

static uint32_t
m9_native_index_at(const void *indices, unsigned size, unsigned i)
{
   return size == 4 ? ((const uint32_t *)indices)[i] :
          size == 2 ? ((const uint16_t *)indices)[i] :
                      ((const uint8_t *)indices)[i];
}

static void
m9_native_index_range(const void *indices, unsigned size, unsigned first,
                      unsigned count, uint32_t *lo, uint32_t *hi)
{
   uint32_t a = UINT32_MAX, b = 0;

   for (unsigned i = 0; i < count; i++) {
      uint32_t v = m9_native_index_at(indices, size, first + i);

      a = MIN2(a, v);
      b = MAX2(b, v);
   }
   *lo = a;
   *hi = b;
}

/* Reject shaders whose inputs cannot fit before resolving the source layout
 * and job key.  Uniform specialisation can remove entire input locations,
 * so its unspecialised input mask is not a lower bound on the streams a draw
 * needs.  Those shaders reach m9_native_vs_layout, which checks the actual
 * compiled variant against the same output-target cap. */
static bool
m9_native_vs_fs_cheap(const struct m9_context *ctx)
{
   const struct shader_info *info;

   if (!ctx->fs || !ctx->fs->nir)
      return false;
   info = &ctx->fs->nir->info;
   unsigned inputs = util_bitcount64(info->inputs_read &
                          ~(VARYING_BIT_POS | VARYING_BIT_FACE |
                            VARYING_BIT_PNTC));

   return info->num_inlinable_uniforms || inputs <= m9_native_vs_varyings ||
          (m9_coord_pack && inputs == m9_native_vs_varyings + 1 &&
           !BITSET_IS_EMPTY(info->textures_used));
}

/* Can this draw run the compiled vertex program?  NULL when it can, and a
 * static reason when the draw module has to keep it.  Everything the emit
 * path then needs is resolved here, so that path cannot fail on state. */
static const char *
m9_native_vs_resolve(struct m9_context *ctx, const struct pipe_draw_info *info,
                     const struct pipe_draw_start_count_bias *draw,
                     struct m9_native_vs *out)
{
   const struct pipe_rasterizer_state *rast = ctx->rasterizer;
   const struct m9_vs_program *p;
   unsigned in_pv, reduced;
   int64_t low;

   if (!ctx->vs || !ctx->vs->cur || !ctx->vs->cur->native)
      return ctx->vs && ctx->vs->cur ? ctx->vs->cur->reason :
             "no native vertex program";
   if (!ctx->velems)
      return "no vertex elements are bound";
   p = &ctx->vs->cur->program;
   if (p->attr_count > M9_VS_HW_ATTRIBUTES)
      return "more preprocessing attributes than the declaration can name";
   if (info->instance_count != 1 || info->start_instance)
      return "instanced draws are not preprocessed natively";
   if (info->primitive_restart)
      return "primitive restart is not preprocessed natively";
   in_pv = m9_flat_pv(rast, info->mode);
   if (!m9_native_topology(info->mode, in_pv, &out->topology))
      return "the primitive needs the draw module's index translation";
   reduced = u_reduced_prim(info->mode);
   out->cull = 0;
   if (reduced == MESA_PRIM_TRIANGLES && rast) {
      if (rast->cull_face == PIPE_FACE_FRONT_AND_BACK)
         return "both faces are culled";
      out->cull = m9_no_cull ? 0 :
                  m9_cull_ef(rast->cull_face, rast->front_ccw);
   }
   out->lines = reduced == MESA_PRIM_LINES;
   out->flat = rast && rast->flatshade;
   if (rast && (rast->rasterizer_discard || rast->clip_plane_enable ||
                rast->clip_halfz || !rast->depth_clip_near ||
                !rast->depth_clip_far ||
                rast->offset_line || rast->offset_point ||
                rast->poly_stipple_enable ||
                rast->fill_front != PIPE_POLYGON_MODE_FILL ||
                rast->fill_back != PIPE_POLYGON_MODE_FILL))
      return "a rasterizer state the hardware pass does not carry";
   if (rast && rast->offset_tri && reduced == MESA_PRIM_TRIANGLES &&
       ctx->dsa && ctx->dsa->depth_enabled) {
      uint32_t units, factor;

      if (!ctx->screen->native_depth_bias)
         return "the kernel does not admit native depth bias";
      if (!m9_context_depth(ctx))
         return "native polygon offset needs a D24 depth attachment";
      if (rast->offset_clamp || rast->offset_units_unscaled)
         return "native polygon offset needs unclamped scaled D24 units";
      if (!m9_draw_depth_bias(rast->offset_scale, rast->offset_units,
                              &units, &factor))
         return "native polygon offset needs finite factor and units";
   }
   /* The back colours are chosen per primitive, which the draw module's
    * two-sided stage does and the preprocessing program does not. */
   if (rast && rast->light_twoside)
      return "two-sided colour is chosen in the draw module";
   if (out->lines && rast &&
       (rast->line_width > 1.0f || rast->line_smooth ||
        rast->line_stipple_enable))
      return "wide, smooth or stippled lines run through the draw module";
   if (out->lines && m9_logic_tail(ctx->blend))
      return "logic-op lines are drawn as quads through the draw module";
   out->buffer_count = ctx->num_vertex_buffers;
   for (unsigned i = 0; i < out->buffer_count && i < PIPE_MAX_ATTRIBS; i++) {
      const struct pipe_vertex_buffer *vb = &ctx->vertex_buffers[i];

      /* A user buffer has no size the driver knows; the draw module reads
       * it under the same contract. */
      out->map[i] = vb->is_user_buffer ? vb->buffer.user :
                    vb->buffer.resource ? m9_res(vb->buffer.resource)->map :
                                          NULL;
      out->size[i] = vb->is_user_buffer || !vb->buffer.resource ? UINT_MAX :
                     vb->buffer.resource->width0;
   }
   out->indices = NULL;
   out->index_size = info->index_size;
   out->index_bias = draw->index_bias;
   if (info->index_size) {
      const uint8_t *base = info->has_user_indices ? info->index.user :
                            info->index.resource ?
                            m9_res(info->index.resource)->map : NULL;
      uint32_t lo, hi;

      if (!base)
         return "the index buffer is not mapped";
      out->indices = base + (size_t)draw->start * info->index_size;
      if (!info->has_user_indices &&
          (uint64_t)(draw->start + draw->count) * info->index_size >
          info->index.resource->width0)
         return "the index run leaves the index buffer";
      m9_native_index_range(out->indices, info->index_size, 0, draw->count,
                            &lo, &hi);
      low = (int64_t)lo + draw->index_bias;
      if (low < 0)
         return "a biased index is negative";
      out->first_vertex = (uint32_t)low;
      out->vertex_count = hi - lo + 1;
   } else {
      out->first_vertex = draw->start;
      out->vertex_count = draw->count;
   }
   return NULL;
}

/* The source records of one draw, built from the program the chosen output
 * layout compiled: a coefficient slot has no vertex element, and its four
 * floats are the multiply-add the sampler's transform and the target's
 * addressed span come to. */
static const char *
m9_native_vs_records(struct m9_context *ctx, const struct m9_job_key *key,
                     const struct m9_vs_program *p, struct m9_native_vs *out)
{
   const char *reason = "the vertex elements do not feed the declared "
                        "attributes";

   if (p->attr_count > M9_VS_HW_ATTRIBUTES)
      return "more preprocessing attributes than the declaration can name";
   if (!m9_vs_source_layout_init(p, ctx->velems->elements, ctx->velems->count,
                                 ctx->vertex_buffers, ctx->num_vertex_buffers,
                                 &out->layout, &reason))
      return reason;
   for (unsigned i = 0; i < out->layout.attr_count; i++) {
      struct m9_vs_source_attr *a = &out->layout.attr[i];
      const struct m9_vs_output *o;

      if (!a->constant)
         continue;
      o = &p->outputs.stream[a->coeff_stream];
      m9_vs_coord_coefficients(key, o->pass, o->coord, M9_VS_COORD_SCALE,
                               a->value);
      m9_vs_coord_coefficients(key, o->pass, o->coord, M9_VS_COORD_BIAS,
                               a->value + 2);
   }
   if (!m9_vs_source_in_range(&out->layout, out->size, out->buffer_count,
                              out->first_vertex, out->vertex_count))
      return "the draw reads outside the bound vertex buffers";
   return NULL;
}

/* Stream targets the second preprocessing draw has to bind: one a stream
 * past the two the fused pass carries on its own targets 0 and 1.  Zero
 * leaves the job at its single preprocessing draw.  The layout carries only
 * streams the raster pass reads, so its count is what the targets cost. */
static unsigned
m9_native_vs_extra_targets(const struct m9_vs_program *p)
{
   /* Sampled-position always owns the source-qualified six-field record,
    * including explicit default fields when the live linker packs fewer
    * streams. GATHER publishes three views; unused views are not FS inputs. */
   if (p->sampled_position)
      return p->outputs.count >= 3 && p->outputs.count <= M9_VS_MAX_VARYINGS ?
             M9_VS_EXTRA_VARYINGS : 0;
   return p->outputs.count > M9_VS_FUSED_VARYINGS ?
          p->outputs.count - M9_VS_FUSED_VARYINGS : 0;
}

/* Adds one stream to the layout, or names why it does not fit. */
static const char *
m9_native_vs_add_stream(struct m9_vs_outputs *out,
                        const struct m9_vs_output *stream)
{
   for (unsigned j = 0; j < out->count; j++)
      if (!memcmp(&out->stream[j], stream, sizeof(*stream)))
         return NULL;
   if (out->count >= M9_VS_MAX_VARYINGS ||
       out->count >= m9_native_vs_varyings)
      return "the varying read is past the M9X_VS_VARYINGS cap";
   out->stream[out->count++] = *stream;
   return NULL;
}

/* The output streams a native preprocessing job has to write: one an
 * interpolated input of the raster pass, and one a texture coordinate --
 * which the vertex program places and transforms itself, so the CPU views
 * the draw module fills are not needed.  NULL when the layout is buildable,
 * a static reason otherwise.  p is the shader's own program, consulted only
 * for the varyings it writes; the layout is what a second compile of it
 * produces. */
static const char *
m9_native_vs_layout(struct m9_context *ctx, const struct m9_job_key *key,
                    const struct m9_vs_program *p,
                    struct m9_vs_outputs *out)
{
   const struct m9_fs_pass_streams *st;
   const struct m9_fs_program *fp;
   const char *reason;
   unsigned coeff = 0;   /* record slots the coordinate streams have taken */

   memset(out, 0, sizeof(*out));
   /* The pass-through colour image reads the primary colour from stream 0. */
   if (!key->fs) {
      if (m9_vs_varying_index(p, VARYING_SLOT_COL0) < 0)
         return "the pass-through fragment image needs a primary colour";
      out->stream[out->count++] = (struct m9_vs_output) {
         .location = VARYING_SLOT_COL0,
      };
      return NULL;
   }
   if (key->variant->passes.count > 1 && !m9_native_vs_split && !m9_native_vs_gpu_padding)
      return "the fragment program runs as several raster passes";
   /* Every raster pass interpolates what it reads, so the streams are the
    * union over the passes: a value handed between them travels through the
    * inter-pass record and costs none. */
   for (unsigned i = 0; i < key->variant->passes.count; i++) {
      fp = &key->variant->passes.pass[i];
      st = &key->variant->stream[i];
      if (fp->varying_count > M9_VS_MAX_VARYINGS)
         return "the fragment program reads more varyings than the "
                "preprocessing pass writes";
      for (unsigned k = 0; k < fp->varying_count; k++) {
         struct m9_vs_output stream = { .location = fp->varyings[k].location };

         if (m9_vs_varying_index(p, stream.location) < 0)
            return "the vertex program does not write a varying the fragment "
                   "program reads";
         reason = m9_native_vs_add_stream(out, &stream);
         if (reason)
            return reason;
      }
      /* A projector varies per fragment and the stream can only carry a
       * per-vertex divide, and a shadow or volume fetch reads a third
       * interpolated component the two-scalar stream does not hold.  A slot
       * an earlier pass feeds -- its texel or its coordinate -- interpolates
       * nothing and needs no stream. */
      for (unsigned k = 0; k < fp->texture_count; k++) {
         if (st->texture_coord[k] == M9_RASTER_COORD_NONE)
            continue;
         if (fp->texcoord_projective[k]) {
            const struct m9_fs_coord_affine *a = &fp->texcoord_affine[k];
            unsigned u = st->unit[k];
            bool one = u < key->unit_count &&
                       (!a->present ||
                        key->coord_scale[u][3] + key->coord_bias[u][3] == 1.0f) &&
                       m9_vs_output_is_one(ctx->vs->cur->nir, p,
                          ctx->velems->elements, ctx->velems->count,
                          fp->texcoords[k], a->swizzled ? a->comp[3] : 3);

            /* Fixed-function two-component arrays provide q=1 through
             * their source format, even though the FS still names a
             * projector.  The actual VS producer and bound format must
             * prove that value before the per-vertex divide is omitted. */
            if (!one)
               return "the texture coordinate is divided by a projector";
         }
         if (fp->texcoord_affine[k].swizzled)
            return "the texture coordinate reorders its components";
         if (fp->texcoord_shadow[k] || fp->texcoord_volume[k])
            return "the texture coordinate carries a third component";
      }
      if (st->coord_count && !m9_native_vs_texture)
         return "the fragment program samples at a coordinate stream "
                "(M9X_VS_TEXTURE)";
      for (unsigned c = 0; c < st->coord_count; c++) {
         struct m9_vs_output stream = { .location = st->coord_location[c],
                                        .component = st->coord_component[c],
                                        .coord = c, .pass = i,
                                        .coordinate = true };
         /* Whether the coefficients move is a property of the draw, not of
          * the shader, and a layout that followed them would recompile --
          * and split the job -- every time a scene mixed a padded render
          * target with an ordinary texture.  So the record slot is spent
          * whenever one is free, and only a shader that fills the four
          * attributes falls back to the per-job constant pair. */
         unsigned had = out->count;
         float sc[2], bi[2];

         /* The pair is already in a stream this pass interpolates, and it
          * needs no transform: the raster declaration can select it out of
          * that stream and the coordinate costs no output target. */
         m9_vs_coord_coefficients(key, i, c, M9_VS_COORD_SCALE, sc);
         m9_vs_coord_coefficients(key, i, c, M9_VS_COORD_BIAS, bi);
         if (m9_coord_share && sc[0] == 1.0f && sc[1] == 1.0f &&
             bi[0] == 0.0f && bi[1] == 0.0f && stream.component + 2 <= 4) {
            bool interpolated = false;

            for (unsigned j = 0; j < out->count; j++)
               interpolated |= !out->stream[j].coordinate &&
                               out->stream[j].location == stream.location;
            if (interpolated)
               continue;
         }
         /* The producer's high pair has no consumer yet.  Identity
          * coordinates need no extra coefficient record: emit the second
          * raw pair there and let its sampler select components 2 and 3.
          * Preserve separate streams for every transformed coordinate. */
         if (m9_coord_pack && sc[0] == 1.0f && sc[1] == 1.0f &&
             bi[0] == 0.0f && bi[1] == 0.0f) {
            bool paired = false;

            for (unsigned j = 0; j < out->count; j++) {
               struct m9_vs_output *o = &out->stream[j];
               float os[2], ob[2];

               if (!o->coordinate || o->paired)
                  continue;
               m9_vs_coord_coefficients(key, o->pass, o->coord,
                                         M9_VS_COORD_SCALE, os);
               m9_vs_coord_coefficients(key, o->pass, o->coord,
                                         M9_VS_COORD_BIAS, ob);
               if (os[0] != 1.0f || os[1] != 1.0f ||
                   ob[0] != 0.0f || ob[1] != 0.0f)
                  continue;
               if (m9_vs_varying_index(p, stream.location) < 0)
                  return "the vertex program does not write the varying a "
                         "texture coordinate comes from";
               o->paired = true;
               o->pair_location = stream.location;
               o->pair_component = stream.component;
               o->pair_coord = stream.coord;
               o->pair_pass = stream.pass;
               coeff -= o->coeff_record;
               o->coeff_record = false;
               paired = true;
               break;
            }
            if (paired)
               continue;
         }
         stream.coeff_record = p->attr_count + coeff < M9_VS_MAX_ATTRIBUTES;
         if (m9_vs_varying_index(p, stream.location) < 0)
            return "the vertex program does not write the varying a texture "
                   "coordinate comes from";
         reason = m9_native_vs_add_stream(out, &stream);
         if (reason)
            return reason;
         coeff += stream.coeff_record && out->count > had;
         if (out->count > 1 && !m9_native_vs_coord_pair)
            return "a texture coordinate beside another stream needs the "
                   "second output target (M9X_VS_COORD_PAIR)";
      }
   }
   return out->count > M9_VS_FUSED_VARYINGS && m9_vs_record_mode() < 2 ?
          "multi-stream route-fed output needs M9X_VS_RECORD=2" : NULL;
}

/* Why the engine would not run this compiled program, or NULL.  Both bounds
 * are measured ends rather than decoded fields, so both are knobs. */
static const char *
m9_vs_program_refusal(const struct m9_vs_program *p)
{
   /* The tail-free producer must meet the same ALU bound as the ordinary
    * source-plus-tail shader. Routing cannot evade the admission limit. */
   if (m9_vs_slot_count(p) > m9_vs_max_slots ||
       (m9_vs_producer_slot_count(p) &&
        m9_vs_producer_slot_count(p) + M9_VS_TAIL_SLOTS > m9_vs_max_slots) ||
       m9_vs_extra_slot_count(p) > m9_vs_max_slots)
      return "the compiled vertex program is past M9X_VS_MAX_SLOTS";
   if (m9_vs_const_count(p) > m9_vs_max_const_pairs)
      return "the vertex program's constant span is past "
             "M9X_VS_MAX_CONST_PAIRS";
   return NULL;
}

/* The compiled program for one output layout, compiled on first use and
 * kept with the shader; a refused layout is kept with its reason so the
 * same pair is not compiled again every draw. */
static struct m9_vs_variant *
m9_native_vs_variant(struct m9_vs_spec *vs, const struct m9_vs_outputs *out,
                     bool gpu_padding)
{
   struct m9_vs_variant *v;

   util_dynarray_foreach(&vs->variants, struct m9_vs_variant *, it)
      if ((*it)->gpu_padding == gpu_padding &&
          !memcmp(&(*it)->outputs, out, sizeof(*out)))
         return *it;
   /* One layout a fragment variant this shader is drawn with; a further one
    * falls back rather than compiling without bound. */
   if (util_dynarray_num_elements(&vs->variants, struct m9_vs_variant *) >=
       M9_VS_MAX_VARIANTS)
      return NULL;
   v = CALLOC_STRUCT(m9_vs_variant);
   if (!v)
      return NULL;
   v->outputs = *out;
   v->gpu_padding = gpu_padding;
   v->serial = ++m9_vs_serial;
   if (gpu_padding)
      v->ok = vs->nir && m9_vs_compile_gpu_padding(vs->nir, out,
                           vs->program.sampled_position, m9_vs_max_slots,
                           m9_vs_max_const_pairs, &v->program, &v->reason);
   else if (vs->program.sampled_position) {
      nir_shader *trial = vs->nir ? nir_shader_clone(NULL, vs->nir) : NULL;
      v->ok = trial && m9_vs_compile_sampled_position_source(&trial, out,
                           &v->program, &v->reason, m9_vs_max_slots,
                           m9_vs_max_const_pairs);
      ralloc_free(trial);
   } else
      v->ok = vs->nir && m9_vs_compile_bounded_source(&vs->nir, out, &v->program,
                                              &v->reason, m9_vs_max_slots,
                                              m9_vs_max_const_pairs);
   /* A coordinate stream costs the layout constants the specialisation had
    * none of, so the bounds are taken again here. */
   if (v->ok && (v->reason = m9_vs_program_refusal(&v->program))) {
      m9_vs_program_fini(&v->program);
      v->ok = false;
   }
   if (!v->reason)
      v->reason = "the vertex program was not compiled for these streams";
   util_dynarray_append_typed(&vs->variants, struct m9_vs_variant *, v);
   return v;
}

/* A welded draw kept across frames.  Welding costs one hash pass over every
 * corner, so it pays only where the same buffer is drawn again: the entry
 * names the vertex buffers it was built from and the generation each carried,
 * and any write through this driver's transfer path retires it.  An entry
 * whose reuse did not repay the indices it costs is kept as a refusal, so a
 * buffer that cannot be welded is hashed once rather than every frame. */
struct m9_weld_entry {
   struct m9_weld weld;
   struct m9_vs_source_layout layout;
   struct pipe_resource *buffer[PIPE_MAX_ATTRIBS];
   uint64_t generation[PIPE_MAX_ATTRIBS];
   unsigned buffer_count;
   uint32_t first_vertex, count;
   uint64_t used;
   bool refused;
};

/* M9X_WELD=0 draws every unindexed run as it arrives, which is the setting
 * the welded frame time is reported against. */
static bool m9_weld_enable = true;
/* Corners below this are not worth a cache slot: the hash pass costs more
 * than the records it would save. */
#define M9_WELD_MIN_CORNERS 512u
/* Records the welded form must save to be worth its index run. */
#define M9_WELD_MIN_RATIO 2u
/* Bytes of welded index and source runs the context keeps. */
#define M9_WELD_BUDGET (16u << 20)

static uint64_t m9_weld_bytes(const struct m9_weld_entry *e)
{
   return ((uint64_t)e->weld.count + e->weld.unique) * sizeof(uint32_t);
}

static void
m9_weld_entry_free(struct m9_weld_entry *entry)
{
   for (unsigned i = 0; i < entry->buffer_count; i++)
      pipe_resource_reference(&entry->buffer[i], NULL);
   m9_weld_fini(&entry->weld);
   FREE(entry);
}

static void
m9_weld_cache_fini(struct m9_context *ctx)
{
   util_dynarray_foreach(&ctx->weld_cache, struct m9_weld_entry *, e)
      m9_weld_entry_free(*e);
   util_dynarray_fini(&ctx->weld_cache);
}

/* The entry matches when it was built from the same records: the same layout
 * over the same run of the same buffers, each still at the generation it
 * carried.  A buffer with no resource of its own -- the state tracker's
 * upload of a client array -- has no identity to key on and is never welded. */
static bool
m9_weld_entry_current(const struct m9_context *ctx,
                      const struct m9_weld_entry *e,
                      const struct m9_native_vs *n, unsigned count)
{
   if (e->first_vertex != n->first_vertex || e->count != count ||
       e->buffer_count != n->buffer_count ||
       memcmp(&e->layout, &n->layout, sizeof(e->layout)))
      return false;
   for (unsigned i = 0; i < e->buffer_count; i++) {
      const struct pipe_vertex_buffer *vb = &ctx->vertex_buffers[i];

      if (!e->buffer[i])
         continue;
      if (vb->is_user_buffer || vb->buffer.resource != e->buffer[i] ||
          m9_res(e->buffer[i])->generation != e->generation[i])
         return false;
   }
   return true;
}

/* Every buffer the layout reads, so a write to one this draw does not read
 * cannot retire the entry. */
static bool
m9_weld_buffers_keyable(const struct m9_context *ctx,
                        const struct m9_native_vs *n, bool *read)
{
   memset(read, 0, sizeof(*read) * PIPE_MAX_ATTRIBS);
   for (unsigned a = 0; a < n->layout.attr_count; a++) {
      unsigned b = n->layout.attr[a].buffer;

      if (b >= n->buffer_count || b >= PIPE_MAX_ATTRIBS)
         return false;
      /* A client array is uploaded afresh, so its identity says nothing. */
      if (ctx->vertex_buffers[b].is_user_buffer ||
          !ctx->vertex_buffers[b].buffer.resource)
         return false;
      read[b] = true;
   }
   return n->layout.attr_count != 0;
}

static void
m9_weld_trim(struct m9_context *ctx, uint64_t want)
{
   uint64_t held = 0;

   util_dynarray_foreach(&ctx->weld_cache, struct m9_weld_entry *, e)
      held += m9_weld_bytes(*e);
   while (held + want > M9_WELD_BUDGET) {
      struct m9_weld_entry **all = ctx->weld_cache.data;
      unsigned n = util_dynarray_num_elements(&ctx->weld_cache,
                                              struct m9_weld_entry *);
      unsigned oldest = 0;

      if (!n)
         return;
      for (unsigned i = 1; i < n; i++)
         if (all[i]->used < all[oldest]->used)
            oldest = i;
      held -= m9_weld_bytes(all[oldest]);
      m9_weld_entry_free(all[oldest]);
      all[oldest] = all[n - 1];
      ctx->weld_cache.size -= sizeof(*all);
   }
}

/* The welded form of this draw, or NULL to draw it as it arrived. */
static const struct m9_weld *
m9_weld_lookup(struct m9_context *ctx, const struct m9_native_vs *n,
               unsigned count)
{
   bool read[PIPE_MAX_ATTRIBS];
   struct m9_weld_entry *entry;
   float *records;
   uint64_t bytes;

   if (!m9_weld_enable || count < M9_WELD_MIN_CORNERS ||
       !m9_weld_buffers_keyable(ctx, n, read))
      return NULL;
   util_dynarray_foreach(&ctx->weld_cache, struct m9_weld_entry *, held) {
      if (!m9_weld_entry_current(ctx, *held, n, count))
         continue;
      (*held)->used = ++ctx->weld_clock;
      return (*held)->refused ? NULL : &(*held)->weld;
   }
   entry = CALLOC_STRUCT(m9_weld_entry);
   records = entry ? malloc((size_t)count * n->layout.record_dwords *
                            sizeof(float)) : NULL;
   if (!records ||
       !m9_vs_source_records(&n->layout, n->map, n->size, n->buffer_count,
                             n->first_vertex, count, records) ||
       !m9_weld_build(&entry->weld, records, count,
                      n->layout.record_dwords)) {
      free(records);
      FREE(entry);
      return NULL;
   }
   free(records);
   /* A run whose records barely repeat costs an index for every corner and
    * saves little; the entry stays as a refusal so it is not rebuilt. */
   entry->refused = (uint64_t)entry->weld.unique * M9_WELD_MIN_RATIO >
                    entry->weld.count;
   if (entry->refused)
      m9_weld_fini(&entry->weld);
   entry->layout = n->layout;
   entry->first_vertex = n->first_vertex;
   entry->count = count;
   entry->buffer_count = n->buffer_count;
   for (unsigned i = 0; i < n->buffer_count && i < PIPE_MAX_ATTRIBS; i++) {
      if (!read[i])
         continue;
      pipe_resource_reference(&entry->buffer[i],
                              ctx->vertex_buffers[i].buffer.resource);
      entry->generation[i] = m9_res(entry->buffer[i])->generation;
   }
   bytes = m9_weld_bytes(entry);
   m9_weld_trim(ctx, bytes);
   entry->used = ++ctx->weld_clock;
   {
      struct m9_weld_entry **slot =
         util_dynarray_grow(&ctx->weld_cache, struct m9_weld_entry *, 1);

      if (!slot) {
         m9_weld_entry_free(entry);
         return NULL;
      }
      *slot = entry;
   }
   return entry->refused ? NULL : &entry->weld;
}

/* One welded run: the unique records of the window a chunk of corners spans,
 * drawn through an index run over them. */
static bool
m9_native_vs_append_welded(struct m9_context *ctx,
                           const struct m9_job_key *key,
                           const struct m9_native_vs *n,
                           const struct m9_weld *weld, unsigned count)
{
   const unsigned max = m9_job_max_vertices(key);
   unsigned floats = n->layout.record_dwords;
   unsigned grain = n->topology == M9_TOPOLOGY_TRIANGLES ? 3 :
                    n->topology == M9_TOPOLOGY_LINES ? 2 : count;

   /* A job cannot carry a primitive whose own window of unique records is
    * wider than its record ceiling, and a mesh's corner order is not its
    * adjacency order: bunny.obj has a triangle joining vertices 34729 apart
    * in first-use numbering, so welding it is infeasible below that and
    * exact at one job above it (MEASURED offline, tools weldsim over the
    * model's own records).  Decline before anything is committed, so the
    * caller draws the run unwelded instead. */
   if (grain < count ? weld->widest > max : weld->unique > max)
      return false;
   for (unsigned done = 0; done < count;) {
      unsigned k, vertices;
      uint32_t lo, hi, first, index_first;
      float *extra, *out;
      uint16_t *words;

      k = m9_weld_run(weld, done, max, grain, &lo, &hi);
      if (!k) {
         m9_render_drop(ctx);
         return true;
      }
      vertices = hi - lo + 1;
      out = m9_job_reserve(ctx, key, &ctx->key_consts, &ctx->key_vs_consts,
                           floats, vertices, &first, &extra);
      if (!out ||
          !m9_vs_source_records_gather(&n->layout, n->map, n->size,
                                       n->buffer_count, n->first_vertex,
                                       weld->source + lo, vertices, out)) {
         m9_render_drop(ctx);
         return true;
      }
      index_first = util_dynarray_num_elements(&ctx->indices, uint16_t);
      words = util_dynarray_grow(&ctx->indices, uint16_t,
                                 k + (k & 1) + M9_INDEX_TAIL);
      if (!words) {
         ctx->records.size = (size_t)first * floats * sizeof(float);
         m9_warn_once(ctx, M9_WARN_MEMORY, "out of memory for raster indices");
         return true;
      }
      for (unsigned i = 0; i < k; i++)
         words[i] = first + (weld->index[done + i] - lo);
      for (unsigned i = k; i < k + (k & 1) + M9_INDEX_TAIL; i++)
         words[i] = words[k - 1];
      m9_prof_indexed += k;
      m9_prof_referenced += vertices;
      /* Counted per use, not per cache build: the frame profile then says
       * how many corners a frame are drawn welded rather than how many were
       * hashed once at startup. */
      m9_prof_welded_corners += k;
      m9_job_commit(ctx, out, NULL, vertices, first, index_first, k,
                    n->topology, n->cull, n->lines, n->flat);
      done += k;
   }
   return true;
}

static bool
m9_native_vs_append(struct m9_context *ctx, const struct m9_job_key *key,
                    const struct m9_native_vs *n, unsigned count)
{
   const unsigned max = m9_job_max_vertices(key);
   unsigned floats = n->layout.record_dwords;
   bool list = n->topology == M9_TOPOLOGY_LINES ||
               n->topology == M9_TOPOLOGY_TRIANGLES;
   unsigned grain = n->topology == M9_TOPOLOGY_TRIANGLES ? 3 :
                    n->topology == M9_TOPOLOGY_LINES ? 2 : count;

   if (!n->indices) {
      const struct m9_weld *weld = m9_weld_lookup(ctx, n, count);

      /* A welded run draws its unique records through an index run, so the
       * whole mesh is not rebuilt one corner at a time. */
      if (weld && m9_native_vs_append_welded(ctx, key, n, weld, count))
         return true;
      if (count > max && !list)
         return false;
      for (unsigned done = 0; done < count;) {
         unsigned k = MIN2(count - done, max / grain * grain);
         uint32_t first;
         float *extra, *out;

         out = m9_job_reserve(ctx, key, &ctx->key_consts, &ctx->key_vs_consts,
                              floats, k, &first, &extra);
         if (!out ||
             !m9_vs_source_records(&n->layout, n->map, n->size,
                                   n->buffer_count, n->first_vertex + done, k,
                                   out)) {
            m9_render_drop(ctx);
            return true;
         }
         m9_job_commit(ctx, out, NULL, k, first, 0, 0, n->topology, n->cull,
                       n->lines, n->flat);
         done += k;
      }
      return true;
   }
   for (unsigned done = 0; done < count;) {
      unsigned k = count - done, vertices;
      uint32_t lo, hi, first, index_first;
      float *extra, *out;
      uint16_t *words;

      m9_native_index_range(n->indices, n->index_size, done, k, &lo, &hi);
      /* Halve the run until the vertices it spans fit one job's records. */
      while (hi - lo + 1 > max) {
         k = k / 2 / grain * grain;
         if (!k) {
            m9_render_drop(ctx);
            return true;
         }
         m9_native_index_range(n->indices, n->index_size, done, k, &lo, &hi);
      }
      vertices = hi - lo + 1;
      out = m9_job_reserve(ctx, key, &ctx->key_consts, &ctx->key_vs_consts,
                           floats, vertices, &first, &extra);
      if (!out ||
          !m9_vs_source_records(&n->layout, n->map, n->size, n->buffer_count,
                                lo + n->index_bias, vertices, out)) {
         m9_render_drop(ctx);
         return true;
      }
      index_first = util_dynarray_num_elements(&ctx->indices, uint16_t);
      words = util_dynarray_grow(&ctx->indices, uint16_t,
                                 k + (k & 1) + M9_INDEX_TAIL);
      if (!words) {
         ctx->records.size = (size_t)first * floats * sizeof(float);
         m9_warn_once(ctx, M9_WARN_MEMORY, "out of memory for raster indices");
         return true;
      }
      for (unsigned i = 0; i < k; i++)
         words[i] = first +
                    m9_native_index_at(n->indices, n->index_size, done + i) -
                    lo;
      for (unsigned i = k; i < k + (k & 1) + M9_INDEX_TAIL; i++)
         words[i] = words[k - 1];
      m9_prof_indexed += k;
      m9_prof_referenced += vertices;
      m9_job_commit(ctx, out, NULL, vertices, first, index_first, k,
                    n->topology, n->cull, n->lines, n->flat);
      done += k;
   }
   return true;
}

/* Tallies why a draw went to the draw module.  The reasons are static
 * strings, so the pointer identifies one and the list stays as long as the
 * distinct reasons a run meets. */
static bool
m9_native_vs_dump_reason(struct m9_context *ctx, const char *reason)
{
   char path[1024];
   FILE *f;
   nir_shader *nir;

   /* One representative of each reason, after a completed profile interval
    * when profiling is enabled.  Opt-in and bounded across contexts: enough
    * to join a failed layout to its actual
    * shaders and input defaults without tracing thousands of draw calls. */
   if (!m9_native_fallback_dump || m9_native_fallback_dump_count >= 8 ||
       !ctx->vs || !ctx->vs->cur || !ctx->fs)
      return false;
   /* Compile/admission refusals free their specialized clone. Reuse the
    * shader's retained source instead of keeping extra refused clones, and
    * record the actual inlinable words needed to reconstruct this spec. */
   nir = ctx->vs->cur->nir ? ctx->vs->cur->nir : ctx->vs->src;
   if (!nir)
      return false;
   if (snprintf(path, sizeof(path), "%s-%u.txt", m9_native_fallback_dump,
                m9_native_fallback_dump_count++) >= sizeof(path))
      return false;
   f = fopen(path, "w");
   if (!f)
      return false;
   fprintf(f, "reason: %s\nvertex elements:\n", reason);
   fprintf(f, "vertex NIR provenance: %s\n",
           ctx->vs->cur->nir ? "retained specialized compiler input" :
                              "retained unspecialized shader source");
   fprintf(f, "vertex specialization: native %u slots %u consts %u "
           "inlinable words %u\n", ctx->vs->cur->native,
           ctx->vs->cur->slots, ctx->vs->cur->consts, ctx->vs->inline_count);
   for (unsigned i = 0; ctx->vs->src && i < ctx->vs->inline_count; i++)
      fprintf(f, "  inline dw%u %08x\n",
              ctx->vs->src->info.inlinable_uniform_dw_offsets[i],
              ctx->vs->cur->values[i]);
   if (ctx->rasterizer) {
      const struct pipe_rasterizer_state *r = ctx->rasterizer;
      /* Only in the bounded fallback dump: name every native admission
       * flag, so a rare aggregated rasterizer refusal has an actual cause. */
      fprintf(f, "rasterizer: discard=%u clip_planes=%#x halfz=%u "
              "depth_clip_near=%u depth_clip_far=%u "
              "offset_tri=%u offset_line=%u offset_point=%u "
              "poly_stipple=%u fill_front=%u fill_back=%u "
              "offset_units=%.9g offset_scale=%.9g offset_clamp=%.9g "
              "offset_units_unscaled=%u\n",
              r->rasterizer_discard, r->clip_plane_enable, r->clip_halfz,
              r->depth_clip_near, r->depth_clip_far,
              r->offset_tri, r->offset_line, r->offset_point,
              r->poly_stipple_enable, r->fill_front, r->fill_back,
              r->offset_units, r->offset_scale, r->offset_clamp,
              r->offset_units_unscaled);
   }
   for (unsigned i = 0; ctx->velems && i < ctx->velems->count; i++) {
      const struct pipe_vertex_element *e = &ctx->velems->elements[i];

      fprintf(f, "  %u format %s buffer %u offset %u stride %u divisor %u\n",
              i, util_format_name(e->src_format), e->vertex_buffer_index,
              e->src_offset, e->src_stride, e->instance_divisor);
   }
   if (ctx->vs->cur->native) {
      const struct m9_vs_program *p = &ctx->vs->cur->program;

      fprintf(f, "native attributes: inputs %#" PRIx64 "\n", p->inputs_read);
      for (unsigned i = 0; i < p->attr_count; i++)
         fprintf(f, "  %u element %u components %u constant %u\n", i,
                 m9_vs_attr_element(p, i), p->attrs[i].components,
                 p->attrs[i].constant);
   }
   for (unsigned stage = 0; stage < 2; stage++) {
      const struct pipe_constant_buffer *cb = stage ? &ctx->fs_constants :
                                                   &ctx->vs_constants[0];
      const uint8_t *data = cb->user_buffer ? cb->user_buffer :
                           cb->buffer ? m9_res(cb->buffer)->map : NULL;
      unsigned words = data ? MIN2(cb->buffer_size / 4, 256u) : 0;

      fprintf(f, "%s constants: bytes %u captured words %u\n",
              stage ? "fragment" : "vertex", cb->buffer_size, words);
      for (unsigned i = 0; i < words; i++) {
         uint32_t bits;

         memcpy(&bits, data + cb->buffer_offset + 4 * i, sizeof(bits));
         fprintf(f, "  dw%u %08x\n", i, bits);
      }
   }
   fprintf(f, "\nvertex NIR:\n");
   nir_print_shader(nir, f);
   fprintf(f, "\nfragment NIR before specialization:\n");
   nir_print_shader(ctx->fs->nir, f);
   fclose(f);
   fprintf(stderr, "m9x: native fallback shader/state -> %s\n", path);
   return true;
}

/* Keep aggregate performance counters intact. Qualification can separately
 * require user shaders to use native vertices while observing utility copies. */
static void
m9_native_vs_count(struct m9_context *ctx, bool native)
{
   bool internal = ctx->blitter && ctx->blitter->running;
   if (native) {
      ctx->native_vs_draws++;
      ctx->native_vs_blitter_draws += internal;
   } else {
      ctx->native_vs_fallbacks++;
      ctx->native_vs_blitter_fallbacks += internal;
   }
}

static void
m9_native_vs_note(struct m9_context *ctx, const char *reason)
{
   m9_native_vs_count(ctx, false);
   m9_prof_module++;
   if (!reason)
      return;
   util_dynarray_foreach(&ctx->native_vs_reasons, struct m9_native_vs_reason,
                         it)
      if (it->reason == reason) {
         it->count++;
         if (m9_native_fallback_dump && !it->dumped &&
             (!m9_profile_period || it->profile_count))
            it->dumped = m9_native_vs_dump_reason(ctx, reason);
         return;
      }
   struct m9_native_vs_reason add = { reason, 1, 0, false };

   if (!m9_profile_period)
      add.dumped = m9_native_vs_dump_reason(ctx, reason);

   util_dynarray_append_typed(&ctx->native_vs_reasons,
                              struct m9_native_vs_reason, add);
   if (ctx->debug)
      fprintf(stderr, "m9x: native vertex fallback: %s\n", reason);
}

/* Native mode-1 point sampling is source-derived from the GL compiler's
 * two-component descriptor. Keep wider coordinate producers, formats and
 * sampler policy separate until they have their own native pixel proof. */
static const char *
m9_native_vs_bind_texture(struct m9_context *ctx, struct m9_job_key *key,
                     const struct m9_vs_program *program)
{
   if (!program->texture_count)
      return NULL;
   if (!debug_get_bool_option("M9X_NATIVE_VS_TEXTURE", true))
      return "native vertex texture is disabled";
   if (program->texture_count > 2 ||
       !key->variant || (key->variant->passes.count != 1 && !program->gpu_padding))
      return "vertex texture requires at most two samplers and one raster pass";
   /* The second-fetch hardware image and packed-output stages are distinct
    * qualification boundaries. Keep the new fused slice opt-in. */
   if (program->texture_count == 2 &&
       !debug_get_bool_option("M9X_NATIVE_VS_TEXTURE_TWO", false))
      return "dual vertex texture is disabled";
   if (program->sampled_position &&
       !debug_get_bool_option("M9X_NATIVE_VS_TEXTURE_POSITION", false))
      return "sampled vertex position is disabled";
   if (program->texture_head_only) {
      if (!debug_get_bool_option("M9X_NATIVE_VS_TEXTURE_PACKED", false))
         return "packed vertex texture is disabled";
      if (!program->producer.size || !program->extra.size || !program->gather.size ||
          m9_vs_record_mode() < 2)
         return "packed vertex texture needs retained FIRST/CONTINUE/gather routes";
   } else if (program->producer.size || program->extra.size || program->gather.size)
      return "vertex texture requires the bounded fused output path";
   for (unsigned unit = 0; unit < program->texture_count; unit++) {
      unsigned slot = unit ? program->texture_sampler_second : program->texture_sampler;
      struct pipe_sampler_view *view = m9_vs_texture_view(&ctx->base, slot);
      const struct pipe_sampler_state *sampler =
         m9_vs_texture_state(&ctx->base, slot);
      if (!view || !view->texture || !sampler)
         return "vertex texture has no retained sampler/view";
      if (view->target != PIPE_TEXTURE_2D || view->texture->target != PIPE_TEXTURE_2D ||
          view->u.tex.first_level || view->texture->last_level ||
          util_format_is_srgb(view->format) || sampler->unnormalized_coords ||
          sampler->min_img_filter != PIPE_TEX_FILTER_NEAREST ||
          sampler->mag_img_filter != PIPE_TEX_FILTER_NEAREST ||
          sampler->min_mip_filter != PIPE_TEX_MIPFILTER_NONE ||
          sampler->compare_mode != PIPE_TEX_COMPARE_NONE || sampler->lod_bias ||
          sampler->wrap_s != PIPE_TEX_WRAP_REPEAT ||
          sampler->wrap_t != PIPE_TEX_WRAP_REPEAT)
         return "vertex texture needs level-zero normalized point/wrap 2-D state";
      uint8_t swizzle[4];
      m9_view_swizzle(view, swizzle);
      for (unsigned c = 0; c < 4; c++)
         if (swizzle[c] != PIPE_SWIZZLE_X + c)
            return "vertex texture view needs a shader swizzle";
      if (view->texture == ctx->color || view->texture == ctx->depth ||
          (key->aux && view->texture == &key->aux->base))
         return "vertex texture framebuffer feedback";
      for (unsigned c = 0; c < ARRAY_SIZE(ctx->color_extra); c++)
         if (view->texture == ctx->color_extra[c])
            return "vertex texture framebuffer feedback";
      struct m9_texture_bind bind;
      const char *reason = m9_texture_bind_view(view, &bind);
      if (reason)
         return reason;
      if (bind.width != m9_res(view->texture)->width ||
          bind.height != m9_res(view->texture)->height)
         return "vertex texture needs unscaled live storage dimensions";
      if (bind.code != M9_TEXTURE_CODE_BGRA && bind.code != 0xc8 && bind.code != 0xc9)
         return "vertex texture format has no bounded native sample proof";
      if (bind.kind != M9_TEXTURE_KIND_2D && bind.kind != M9_TEXTURE_KIND_RENDER)
         return "vertex texture storage has no bounded native sample proof";
      struct m9_texture_unit *native = unit ? &key->vs_unit_second : &key->vs_unit;
      if (m9_texture_unit_init(native, m9_res(view->texture)->handle,
                               &bind, sampler))
         return "vertex texture sampler words could not be derived";
      if (ctx->debug)
         fprintf(stderr, "m9x: native vertex sampler slot %u handle %u 70=%08x "
                 "79=%08x 7a=%08x\n", slot, native->handle,
                 native->w70, native->w79, native->w7a);
      if (unit) {
         key->vs_texture_second = true;
         key->vs_unit_res_second = view->texture;
      } else {
         key->vs_texture = true;
         key->vs_unit_res = view->texture;
      }
   }
   return NULL;
}

static void m9_vs_select_spec(struct m9_context *ctx);

/* True when the draw was taken by the native path; false leaves it to the
 * draw module with no change in behaviour. */
static bool
m9_native_vs_draw(struct m9_context *ctx, const struct pipe_draw_info *info,
                  const struct pipe_draw_start_count_bias *draw)
{
   struct m9_native_vs n;
   struct m9_job_key key;
   const char *reason;

   if (m9_native_vs)
      m9_vs_select_spec(ctx);
   if (!m9_native_vs || !ctx->vs || !ctx->vs->cur || !ctx->vs->cur->native ||
       !m9_native_vs_fs_cheap(ctx)) {
      if (m9_native_vs)
         m9_native_vs_note(ctx, !ctx->vs ? "no vertex shader is bound" :
                                !ctx->vs->cur ?
                                "more vertex shader specialisations than the "
                                "cache holds" :
                                !ctx->vs->cur->native ?
                                ctx->vs->cur->reason :
                                "unspecialised fragment inputs exceed the "
                                "native output cap");
      return false;
   }
   ctx->lines = u_reduced_prim(info->mode) == MESA_PRIM_LINES;
   reason = m9_native_vs_resolve(ctx, info, draw, &n);
   if (reason || !m9_job_key_current(ctx, &key)) {
      if (!reason) {
         /* The state is unsupported for either path; m9_job_key_current
          * has named it. */
         ctx->skipped_draws++;
         return true;
      }
      m9_native_vs_note(ctx, reason);
      return false;
   }
   struct m9_vs_outputs outputs;
   struct m9_vs_variant *variant;

   if (!m9_viewport_clip(ctx, &key))
      return true;

   reason = m9_native_vs_layout(ctx, &key, &ctx->vs->cur->program, &outputs);
   if (reason) {
      m9_native_vs_note(ctx, reason);
      return false;
   }
   bool gpu_padding = key.variant && key.variant->passes.count > 1 && m9_native_vs_gpu_padding;
   if (gpu_padding) {
      struct m9_vs_padding_bounds bounds;
      if (n.lines || !m9_gpu_padding_bounds(ctx, &key, ctx->viewport.scale,
                                          ctx->viewport.translate, &bounds)) {
         m9_native_vs_note(ctx, "GPU multipass padding needs a suppressed full-target frame");
         return false;
      }
   }
   variant = m9_native_vs_variant(ctx->vs->cur, &outputs, gpu_padding);
   if (!variant || !variant->ok) {
      m9_native_vs_note(ctx, variant ? variant->reason :
                        "the vertex program could not be compiled");
      return false;
   }
   reason = m9_native_vs_bind_texture(ctx, &key, &variant->program);
   if (reason) {
      m9_native_vs_note(ctx, reason);
      return false;
   }
   reason = m9_native_vs_records(ctx, &key, &variant->program, &n);
   if (reason) {
      m9_native_vs_note(ctx, reason);
      return false;
   }
   if (ctx->rasterizer && ctx->rasterizer->offset_tri &&
       u_reduced_prim(info->mode) == MESA_PRIM_TRIANGLES) {
      bool depth_test = ctx->dsa && ctx->dsa->depth_enabled;

      /* Without a depth test the offset cannot affect a colour-only draw.
       * Keep those transforms native, but preserve the draw module when
       * fragment position is read: its z includes the pre-shading offset. */
      if (!depth_test && key.variant)
         for (unsigned i = 0; i < key.variant->passes.count; i++)
            if (key.variant->passes.pass[i].fragcoord) {
               m9_native_vs_note(ctx,
                  "depth-disabled polygon offset reads fragment position");
               return false;
            }
      /* The captured scalar pair applies to one hardware raster pass.
       * Preserve the draw module for split/explicit-fragment-depth paths
       * until their interaction with raster bias has been captured. */
      if (depth_test && key.variant && (key.variant->passes.count > 1 ||
                                       key.variant->passes.pass[0].fragdepth)) {
         m9_native_vs_note(ctx,
                          "native polygon offset needs one raster depth pass");
         return false;
      }
      if (depth_test) {
         key.depth_bias = m9_draw_depth_bias(ctx->rasterizer->offset_scale,
                                            ctx->rasterizer->offset_units,
                                            &key.depth_bias_units,
                                            &key.depth_bias_factor);
         key.depth_ef |= M9_DEPTH_BIAS_EF;
      }
   }
   key.vs = &variant->program;
   key.vs_program = m9_native_vs_c2(ctx, (struct m9_fs_variant *)key.variant,
                                    variant, &key.vs_extra_entry,
                                    &key.vs_gather_entry, &key.vs_producer_entry);
   key.vs_fused_pair = m9_vs_fused_pair(key.vs);
   key.vs_extra_targets = m9_native_vs_extra_targets(key.vs);
   if (!key.vs_extra_targets)
      key.vs_extra_entry = key.vs_gather_entry = key.vs_producer_entry = 0;
   if (!key.vs_program) {
      m9_native_vs_note(ctx, "the vertex program image could not be built");
      return false;
   }
   for (unsigned i = 0; i < 3; i++) {
      key.viewport_scale[i] = ctx->viewport.scale[i];
      key.viewport_bias[i] = ctx->viewport.translate[i];
   }
   if (!m9_vs_resolve_consts(ctx, &key, key.vs, &ctx->key_vs_consts)) {
      ctx->skipped_draws++;
      return true;
   }
   if (!m9_native_vs_append(ctx, &key, &n, draw->count)) {
      m9_native_vs_note(ctx, "the native job would not take the draw");
      return false;
   }
   if (ctx->debug && !ctx->native_vs_draws) {
      fprintf(stderr, "m9x: native vertex draw taken, %u varyings, fused "
              "pair %d, %u attributes\n", key.vs->varying_count,
              (int)key.vs_fused_pair, key.vs->attr_count);
      for (unsigned j = 0; j < key.vs->outputs.count; j++) {
         const struct m9_vs_output *o = &key.vs->outputs.stream[j];

         fprintf(stderr, "m9x:   stream %u %s varying %u component %u%s\n", j,
                 o->coordinate ? "coordinate" : "interpolated", o->location,
                 o->component, o->coeff_record ? " coefficients in record" :
                 o->coordinate ? " coefficients in the constant span" : "");
      }
   }
   m9_native_vs_count(ctx, true);
   m9_prof_native++;
   return true;
}

static bool
m9_faa_application_enabled(void)
{
   return debug_get_bool_option("M9X_FAA_APPLICATION", false);
}

static void
m9_draw_vbo(struct pipe_context *pctx, const struct pipe_draw_info *info,
            unsigned drawid_offset,
            const struct pipe_draw_indirect_info *indirect,
            const struct pipe_draw_start_count_bias *draws,
            unsigned num_draws);

/* Snapshot live indirect arguments before invoking the ordinary direct path.
 * Its CPU and native vertex producers then retain their existing behavior.
 * pipe_buffer_map_range performs the same resource synchronization as any
 * application read map; no captured command or address is replayed. */
static bool
m9_draw_indirect(struct pipe_context *pctx, const struct pipe_draw_info *info,
                 unsigned drawid_offset,
                 const struct pipe_draw_indirect_info *indirect)
{
   struct m9_indirect_args *args = NULL;
   struct pipe_transfer *transfer = NULL;
   const uint8_t *map;
   uint32_t count = indirect->draw_count, span, stride;

   if (!indirect->buffer ||
       indirect->buffer->target != PIPE_BUFFER ||
       indirect->count_from_stream_output)
      return false;
   if (indirect->indirect_draw_count) {
      struct pipe_resource *res = indirect->indirect_draw_count;
      struct pipe_transfer *ct = NULL;
      const uint8_t *cm;
      uint32_t live_count;

      if (res->target != PIPE_BUFFER ||
          (indirect->indirect_draw_count_offset & 3) ||
          indirect->indirect_draw_count_offset > res->width0 ||
          res->width0 - indirect->indirect_draw_count_offset < 4)
         return false;
      cm = pipe_buffer_map_range(pctx, res,
                                 indirect->indirect_draw_count_offset,
                                 4, PIPE_MAP_READ, &ct);
      if (!cm)
         return false;
      memcpy(&live_count, cm, 4);
      pipe_buffer_unmap(pctx, ct);
      count = MIN2(count, live_count);
   }
   if (!m9_indirect_span(indirect->buffer->width0, indirect->offset,
                         indirect->stride, count, info->index_size != 0,
                         &span))
      return false;
   if (!count)
      return true;
   if ((uint64_t)count * sizeof(*args) > SIZE_MAX)
      return false;
   args = MALLOC((size_t)count * sizeof(*args));
   if (!args)
      return false;
   map = pipe_buffer_map_range(pctx, indirect->buffer, indirect->offset,
                               span, PIPE_MAP_READ, &transfer);
   if (!map) {
      FREE(args);
      return false;
   }
   stride = indirect->stride ? indirect->stride : info->index_size ? 20 : 16;
   for (uint32_t i = 0; i < count; i++)
      m9_indirect_decode(map + (size_t)i * stride, info->index_size != 0,
                         &args[i]);
   pipe_buffer_unmap(pctx, transfer);

   for (uint32_t i = 0; i < count; i++) {
      struct pipe_draw_info direct = *info;
      struct pipe_draw_start_count_bias draw = {
         .start = args[i].start,
         .count = args[i].count,
         .index_bias = args[i].index_bias,
      };

      direct.instance_count = args[i].instance_count;
      direct.start_instance = args[i].start_instance;
      /* Gallium's indirect expansion advances DrawID for every command;
       * increment_draw_id controls direct multidraws instead. */
      m9_draw_vbo(pctx, &direct, drawid_offset + i, NULL, &draw, 1);
   }
   FREE(args);
   return true;
}

/*
 * The blend tail reads the destination of colour target 0 alone; where the
 * other targets' destinations reach the program is not captured.  A draw
 * that blends into several targets is therefore drawn once a target, each
 * pass binding that target alone and keeping that output.  Passes after the
 * first test depth for equality without writing it and stay out of an
 * occlusion query, so they shade exactly the fragments the first one kept.
 */
static bool
m9_mrt_blend_split(struct m9_context *ctx, const struct pipe_draw_info *info,
                   unsigned drawid_offset,
                   const struct pipe_draw_start_count_bias *draw)
{
   const struct pipe_blend_state *blend = ctx->blend;
   const struct pipe_depth_stencil_alpha_state *dsa = ctx->dsa;
   struct pipe_framebuffer_state saved = {0}, fb;
   uint64_t outputs;
   bool any = false, first = true;
   unsigned n = ctx->color_count;

   if (ctx->mrt_select || n < 2 || !blend || !ctx->fs)
      return false;
   for (unsigned s = 0; s < n; s++)
      any |= blend->rt[blend->independent_blend_enable ? s : 0].blend_enable;
   if (!any)
      return false;
   if (dsa && (dsa->stencil[0].enabled || dsa->stencil[1].enabled)) {
      m9_skip_reason(ctx, "blending into several colour targets under a "
                     "stencil test; draw skipped");
      ctx->skipped_draws++;
      return true;
   }
   outputs = ctx->fs->nir->info.outputs_written;
   util_copy_framebuffer_state(&saved, &ctx->framebuffer);
   for (unsigned p = 0; p < n; p++) {
      if (!(outputs & (BITFIELD64_BIT(FRAG_RESULT_COLOR) |
                       BITFIELD64_BIT(FRAG_RESULT_DATA0 + p))))
         continue;
      fb = saved;
      fb.nr_cbufs = 1;
      fb.cbufs[0] = saved.cbufs[p];
      ctx->mrt_blend = *blend;
      ctx->mrt_blend.independent_blend_enable = false;
      ctx->mrt_blend.rt[0] = blend->rt[blend->independent_blend_enable ? p :
                                                                       0];
      ctx->blend = &ctx->mrt_blend;
      if (!first) {
         if (dsa && dsa->depth_enabled) {
            ctx->mrt_dsa = *dsa;
            ctx->mrt_dsa.depth_writemask = 0;
            ctx->mrt_dsa.depth_func = PIPE_FUNC_EQUAL;
            ctx->dsa = &ctx->mrt_dsa;
         }
         m9_query_pause(ctx);
      }
      ctx->base.set_framebuffer_state(&ctx->base, &fb);
      ctx->mrt_select = p + 1;
      m9_draw_vbo(&ctx->base, info, drawid_offset, NULL, draw, 1);
      ctx->mrt_select = 0;
      if (!first)
         m9_query_resume(ctx);
      ctx->blend = blend;
      ctx->dsa = dsa;
      first = false;
   }
   ctx->base.set_framebuffer_state(&ctx->base, &saved);
   util_unreference_framebuffer_state(&saved);
   return true;
}

static void
m9_draw_vbo(struct pipe_context *pctx, const struct pipe_draw_info *info,
            unsigned drawid_offset,
            const struct pipe_draw_indirect_info *indirect,
            const struct pipe_draw_start_count_bias *draws,
            unsigned num_draws)
{
   struct m9_context *ctx = m9_context(pctx);
   bool indexed = info->index_size;

   /* A job held for the scratch path reaches the engine before this draw,
    * which was recorded after it. */
   m9_deferred_run(ctx);
   if (indirect) {
      if (num_draws && !m9_draw_indirect(pctx, info, drawid_offset, indirect)) {
         m9_skip_reason(ctx, "indirect draw arguments are invalid or "
                             "unavailable; skipped");
         ctx->skipped_draws++;
      }
      return;
   }
   if (num_draws > 1) {
      util_draw_multi(pctx, info, drawid_offset, NULL, draws, num_draws);
      return;
   }
   /* A discarded fragment updates nothing -- colour, depth, stencil or the
    * occlusion count -- so a shader that discards all of them draws
    * nothing, exactly, without the kill slot's lane artifact (M9_BRINGUP). */
   if (ctx->fs && ctx->fs->kill_all)
      return;
   if (m9_faa_application_enabled()) {
      /* This mode consumes the draw explicitly. A refused state cannot fall
       * into ordinary rasterization and masquerade as FAA output. */
      if (!num_draws || !draws[0].count || !info->instance_count)
         return;
      const char *why = NULL;
      ctx->submitted_draws++;
      if (m9_context_faa_submit_draw(ctx, info, &draws[0], &why)) {
         ctx->skipped_draws++;
         m9_skip_reason(ctx, why ? why : "FAA application draw failed");
         fprintf(stderr, "m9x: FAA application draw refused: %s\n",
                 why ? why : "authenticated submission/presentation failed");
      }
      return;
   }
   if (num_draws && !indirect &&
       m9_mrt_blend_split(ctx, info, drawid_offset, draws))
      return;
   /* The byte-lane write mask cannot gate a 10-bit channel, and the vendor
    * masks such a target in the program instead (gigacolor-10bit.md 4.2).
    * Without blending that is a blend by the constant mask itself: each
    * channel takes src * m + dst * (1 - m) with m exactly 0 or 1. */
   if (!ctx->mrt_select && ctx->color_count == 1 && ctx->color &&
       m9_res(ctx->color)->format != M9_COLOR_FORMAT_A8R8G8B8 &&
       ctx->blend && !ctx->blend->rt[0].blend_enable &&
       ctx->blend->rt[0].colormask &&
       ctx->blend->rt[0].colormask != PIPE_MASK_RGBA) {
      const struct pipe_blend_state *blend = ctx->blend;
      const struct pipe_blend_color color = ctx->blend_color;
      unsigned mask = blend->rt[0].colormask;

      ctx->mrt_blend = *blend;
      ctx->mrt_blend.rt[0] = (struct pipe_rt_blend_state) {
         .blend_enable = 1,
         .rgb_func = PIPE_BLEND_ADD,
         .rgb_src_factor = PIPE_BLENDFACTOR_CONST_COLOR,
         .rgb_dst_factor = PIPE_BLENDFACTOR_INV_CONST_COLOR,
         .alpha_func = PIPE_BLEND_ADD,
         .alpha_src_factor = PIPE_BLENDFACTOR_CONST_ALPHA,
         .alpha_dst_factor = PIPE_BLENDFACTOR_INV_CONST_ALPHA,
         .colormask = PIPE_MASK_RGBA,
      };
      for (unsigned c = 0; c < 4; c++)
         ctx->blend_color.color[c] = mask & (1u << c) ? 1.0f : 0.0f;
      ctx->blend = &ctx->mrt_blend;
      m9_draw_vbo(pctx, info, drawid_offset, indirect, draws, num_draws);
      ctx->blend = blend;
      ctx->blend_color = color;
      return;
   }
   /* Count at the entry point: a draw dropped here never reaches the
    * vbuf render, and a killed client never destroys its context. */
   /* Counted for every run, not only a debug one: the per-frame skip rate
    * below is the only signal an application gets that it is rendering
    * nothing. */
   if (++ctx->submitted_draws % 200 == 0 && ctx->debug) {
      fprintf(stderr, "m9x: %u draws seen, %u dropped before submission\n",
              ctx->submitted_draws, ctx->skipped_draws);
      m9_skip_report(ctx);
   }
   if (!num_draws || !draws[0].count || !info->instance_count ||
       ctx->lost || !ctx->params_valid) {
      m9_skip_reason(ctx, !num_draws || !draws[0].count ||
                     !info->instance_count ? "the draw is empty; skipped" :
                     ctx->lost ? "the context is lost; draw skipped" :
                     "no valid raster parameters; draw skipped");
      ctx->skipped_draws++;
      if (ctx->debug && ctx->skipped_draws % 200 == 0)
         fprintf(stderr, "m9x: draw dropped: draws %u indirect %d count %u "
                 "instances %u lost %d params %d\n", num_draws, !!indirect,
                 num_draws ? draws[0].count : 0, info->instance_count,
                 ctx->lost, ctx->params_valid);
      return;
   }
   if (ctx->fs && (ctx->fs->passthrough || ctx->fs->native) &&
       m9_native_vs_draw(ctx, info, &draws[0]))
      return;
   if (!ctx->fs || (!ctx->fs->passthrough && !ctx->fs->native)) {
      m9_skip_reason(ctx, ctx->fs && ctx->fs->reason ? ctx->fs->reason :
                     "no fragment shader is bound; draw skipped");
      ctx->skipped_draws++;
      if (ctx->fs && !ctx->fs->warned) {
         fprintf(stderr, "m9x: fragment shader not supported by the native "
                 "backend (%s); draws skipped\n", ctx->fs->reason);
         ctx->fs->warned = true;
      }
      return;
   }
   /* Native vertex fetches retain the GPU image directly. Only the draw
    * module needs a synchronized linear CPU copy of its sampled inputs. */
   m9_vs_texture_refresh(pctx);
   for (unsigned i = 0; i < ctx->num_vertex_buffers; i++) {
      const struct pipe_vertex_buffer *vb = &ctx->vertex_buffers[i];
      const void *map = vb->is_user_buffer ? vb->buffer.user :
                        vb->buffer.resource ?
                        m9_res(vb->buffer.resource)->map : NULL;

      draw_set_mapped_vertex_buffer(ctx->draw, i, map,
                                    vb->is_user_buffer || !vb->buffer.resource ?
                                    ~(size_t)0 : vb->buffer.resource->width0);
   }
   if (indexed)
      draw_set_indexes(ctx->draw, info->has_user_indices ? info->index.user :
                       m9_res(info->index.resource)->map, info->index_size,
                       info->has_user_indices ? ~0u :
                       info->index.resource->width0);
   for (unsigned i = 0; i < PIPE_MAX_CONSTANT_BUFFERS; i++) {
      const struct pipe_constant_buffer *cb = &ctx->vs_constants[i];
      const uint8_t *data = cb->user_buffer ? cb->user_buffer :
                            cb->buffer ? m9_res(cb->buffer)->map : NULL;

      draw_set_mapped_constant_buffer(ctx->draw, MESA_SHADER_VERTEX, i,
                                      data ? data + cb->buffer_offset : NULL,
                                      data ? cb->buffer_size : 0);
   }
   uint64_t t_draw = m9_prof_begin();

   draw_prepare_shader_outputs(ctx->draw);
   draw_vbo(ctx->draw, info, drawid_offset, NULL, draws, 1, 0);
   draw_flush(ctx->draw);
   m9_prof_end(M9_PROF_DRAW, t_draw);
   for (unsigned i = 0; i < ctx->num_vertex_buffers; i++)
      draw_set_mapped_vertex_buffer(ctx->draw, i, NULL, 0);
   if (indexed)
      draw_set_indexes(ctx->draw, NULL, 0, 0);
}

/* Clears */

static bool
m9_scissor_full(const struct m9_context *ctx,
                const struct pipe_scissor_state *s)
{
   return !s || (!s->minx && !s->miny && s->maxx >= ctx->params.width &&
                 s->maxy >= ctx->params.height);
}

/* The vendor rectangle-draw clear: a full-target quad inside the scissor.
 * Stencil is replaced through the stencil unit, smask as its write mask. */
static void
m9_clear_quad(struct m9_context *ctx, const struct pipe_scissor_state *scissor,
              unsigned colormask, const float *rgba, bool depth, double z,
              bool stencil, unsigned sref, uint8_t smask)
{
   const float w = ctx->params.width, h = ctx->params.height;
   const float corner[4][2] = { { 0, 0 }, { w, 0 }, { w, h }, { 0, h } };
   struct m9_resource *zs = m9_context_depth(ctx);
   struct m9_job_key key;
   uint32_t first;
   float *out, *extra;
   float encoded[4];

   if (ctx->lost || !ctx->params_valid)
      return;
   /* The fixed clear shader writes bytes directly.  The vendor encodes its
    * linear RGB input on the CPU for an sRGB RT0; alpha stays linear. */
   if (colormask && util_format_is_srgb(ctx->framebuffer.cbufs[0].format)) {
      for (unsigned c = 0; c < 3; c++)
         encoded[c] = m9_srgb_encode_clear(rgba[c]);
      encoded[3] = rgba[3];
      rgba = encoded;
   }
   depth = depth && zs;
   stencil = stencil && zs && smask && m9_zs_has_stencil(ctx);
   memset(&key, 0, sizeof(key));
   key.depth = zs ? zs : ctx->target_depth;
   key.aux = zs ? zs->hz : ctx->target_aux;
   key.depth_control = m9_depth_control(depth, depth, PIPE_FUNC_ALWAYS);
   key.depth_ef = m9_depth_ef(depth || stencil, depth);
   key.stencil_a9 = M9_STENCIL_A9_OFF;
   key.stencil_aa = M9_STENCIL_AA_OFF;
   if (stencil) {
      const struct m9_stencil_face face = {
         .func = PIPE_FUNC_ALWAYS, .fail_op = PIPE_STENCIL_OP_REPLACE,
         .zfail_op = PIPE_STENCIL_OP_REPLACE,
         .zpass_op = PIPE_STENCIL_OP_REPLACE,
         .ref = sref & 0xff, .valuemask = 0xff, .writemask = smask,
      };

      key.stencil = true;
      m9_stencil_words(key.depth_control, &face, NULL, true,
                       &key.depth_control, &key.stencil_a9, &key.stencil_aa);
   }
   key.color_mask = m9_color_mask(colormask);
   /* Only a multiple-target framebuffer needs the flag, so a single-target
    * clear keeps the key it had and still merges with the draws around it. */
   key.single_target = ctx->color_count > 1;
   key.clip_min = ctx->params.clip_min;
   key.clip_max = ctx->params.clip_max;
   if (scissor && !m9_scissor_key(ctx, scissor, &key))
      return;
   out = m9_job_reserve(ctx, &key, NULL, NULL, M9_RECORD_FLOATS, 4, &first,
                        &extra);
   if (!out)
      return;
   for (unsigned i = 0; i < 4; i++) {
      float *rec = out + i * M9_RECORD_FLOATS;

      rec[0] = corner[i][0];
      rec[1] = corner[i][1];
      rec[2] = 1.0f - 2.0f * (float)CLAMP(z, 0.0, 1.0);
      rec[3] = 1.0f;
      for (unsigned c = 0; c < 4; c++)
         rec[4 + c] = CLAMP(rgba[c], 0.0f, 1.0f);
   }
   m9_job_commit(ctx, out, NULL, 4, first, 0, 0, M9_TOPOLOGY_QUADS, 0, false,
                 true);
}

static void
m9_clear_depth_cpu(struct m9_resource *zs, unsigned buffers, double depth,
                   unsigned stencil)
{
   uint32_t z = (uint32_t)(CLAMP(depth, 0.0, 1.0) * 0xffffff + 0.5) << 8;
   uint32_t *words = zs->map;

   /* D24S8: depth in bits 31:8, stencil in the low byte. */
   for (uint64_t i = 0; i < zs->size / 4; i++) {
      uint32_t v = words[i];

      if (buffers & PIPE_CLEAR_DEPTH)
         v = (v & 0xff) | z;
      if (buffers & PIPE_CLEAR_STENCIL)
         v = (v & ~0xffu) | (stencil & 0xff);
      words[i] = v;
   }
   if ((buffers & PIPE_CLEAR_DEPTH) && zs->hz)
      m9_fill_dwords(zs->hz, M9_HZ_CLEAR);
}

static void m9_clear_render_target(struct pipe_context *pctx,
                                   struct pipe_surface *dst,
                                   const union pipe_color_union *color,
                                   unsigned dstx, unsigned dsty,
                                   unsigned width, unsigned height,
                                   bool render_condition_enabled);

/* A clear that keeps some channels: read, merge the cleared channels' bits
 * of the packed colour and write back, through the transfer path that
 * knows every storage the attachment may have. */
static void
m9_clear_masked_cpu(struct m9_context *ctx, struct pipe_surface *dst,
                    const union pipe_color_union *color, unsigned mask,
                    unsigned x, unsigned y, unsigned w, unsigned h)
{
   const struct util_format_description *desc =
      util_format_description(dst->format);
   struct pipe_transfer *xfer;
   union util_color uc;
   struct pipe_box box;
   uint32_t bits = 0;
   uint8_t *map;

   if (!mask || !w || !h)
      return;
   if (!desc || desc->block.bits != 32 ||
       desc->layout != UTIL_FORMAT_LAYOUT_PLAIN) {
      m9_warn_once(ctx, M9_WARN_CLEAR, "a masked clear of a colour "
                   "attachment past the first needs a 32-bit format");
      return;
   }
   for (unsigned c = 0; c < 4; c++) {
      unsigned ch = desc->swizzle[c];

      if ((mask & (1u << c)) && ch <= PIPE_SWIZZLE_W)
         bits |= BITFIELD_RANGE(desc->channel[ch].shift,
                                desc->channel[ch].size);
   }
   util_pack_color(color->f, dst->format, &uc);
   u_box_3d(x, y, dst->first_layer, w, h, 1, &box);
   map = ctx->base.texture_map(&ctx->base, dst->texture, dst->level,
                               PIPE_MAP_READ | PIPE_MAP_WRITE, &box, &xfer);
   if (!map) {
      m9_warn_once(ctx, M9_WARN_CLEAR, "a masked clear of a colour "
                   "attachment past the first could not map it");
      return;
   }
   for (unsigned row = 0; row < h; row++) {
      uint32_t *p = (uint32_t *)(map + (size_t)row * xfer->stride);

      for (unsigned col = 0; col < w; col++)
         p[col] = (p[col] & ~bits) | (uc.ui[0] & bits);
   }
   ctx->base.texture_unmap(&ctx->base, xfer);
}

/* Colour attachments past the first.  The quad clear runs the fixed-function
 * program, whose one colour output reaches target 0 alone, so the rest are
 * filled on the CPU: a solid colour is the same word in tiled order as in
 * linear order, which is what makes the fill correct without the engine.  A
 * single-target framebuffer -- every frame of every benchmark here -- does
 * none of this. */
static void
m9_clear_color_extra(struct pipe_context *pctx, unsigned buffers,
                     uint32_t color_clear_mask,
                     const struct pipe_scissor_state *scissor,
                     const union pipe_color_union *color, bool full)
{
   struct m9_context *ctx = m9_context(pctx);

   for (unsigned p = 1; p < ctx->color_count; p++) {
      struct pipe_surface *dst = &ctx->framebuffer.cbufs[p];

      if (!(buffers & (PIPE_CLEAR_COLOR0 << p)) || !dst->texture)
         continue;
      /* Four channel mask bits a draw buffer. */
      unsigned mask = (color_clear_mask >> (4 * p)) & 0xf;

      if (mask != PIPE_MASK_RGBA) {
         m9_clear_masked_cpu(ctx, dst, color, mask,
                             full ? 0 : scissor->minx,
                             full ? 0 : scissor->miny,
                             full ? ctx->framebuffer.width :
                                    scissor->maxx - scissor->minx,
                             full ? ctx->framebuffer.height :
                                    scissor->maxy - scissor->miny);
         continue;
      }
      m9_clear_render_target(pctx, dst, color,
                             full ? 0 : scissor->minx,
                             full ? 0 : scissor->miny,
                             full ? ctx->framebuffer.width :
                                    scissor->maxx - scissor->minx,
                             full ? ctx->framebuffer.height :
                                    scissor->maxy - scissor->miny, false);
   }
}

static void
m9_clear_locked(struct pipe_context *pctx, unsigned buffers,
                uint32_t color_clear_mask, uint8_t stencil_clear_mask,
                const struct pipe_scissor_state *scissor,
                const union pipe_color_union *color, double depth,
                unsigned stencil)
{
   static const float zero[4];
   struct m9_context *ctx = m9_context(pctx);
   struct pipe_resource *cbuf = ctx->framebuffer.nr_cbufs ?
                                ctx->framebuffer.cbufs[0].texture : NULL;
   struct m9_resource *zs = m9_context_depth(ctx);
   bool full = m9_scissor_full(ctx, scissor);

   bool colour = (buffers & PIPE_CLEAR_COLOR0) && cbuf;
   bool zclear = zs && (buffers & PIPE_CLEAR_DEPTH);
   bool sclear = zs && (buffers & PIPE_CLEAR_STENCIL) && stencil_clear_mask &&
                 m9_zs_has_stencil(ctx);
   const struct pipe_scissor_state *rect = full ? NULL : scissor;

   if (!ctx->params_valid || m9_no_clear)
      return;
   /* One quad writes every attachment: a clear of colour and depth is what
    * a frame starts with, and two full-screen quads is twice the fill. */
   if (colour && (zclear || sclear)) {
      m9_clear_quad(ctx, rect, color_clear_mask, color->f, zclear, depth,
                    sclear, stencil, stencil_clear_mask);
      m9_clear_color_extra(pctx, buffers, color_clear_mask, scissor, color,
                           full);
      return;
   }
   /* The quad clear rasterises in tiled space and covers the drawable, not
    * the whole object: a dword fill of the object costs three megabytes of
    * uncached writes per frame and reaches past the drawable into the rest
    * of a shared screen surface. */
   if (colour)
      m9_clear_quad(ctx, rect, color_clear_mask, color->f, false, 0,
                    false, 0, 0);
   m9_clear_color_extra(pctx, buffers, color_clear_mask, scissor, color,
                        full);
   if (!(buffers & PIPE_CLEAR_DEPTHSTENCIL))
      return;
   if (!zs) {
      m9_warn_once(ctx, M9_WARN_CLEAR, "depth clear needs a D24S8 attachment "
                   "of the target size");
      return;
   }
   /* The engine keeps the hierarchical buffer with the depth it writes;
    * only a CPU depth write would have to reset it by hand. */
   if (zclear || sclear)
      m9_clear_quad(ctx, rect, 0, zero, zclear, depth, sclear, stencil,
                    stencil_clear_mask);
}

static void
m9_clear(struct pipe_context *pctx, unsigned buffers,
         uint32_t color_clear_mask, uint8_t stencil_clear_mask,
         const struct pipe_scissor_state *scissor,
         const union pipe_color_union *color, double depth, unsigned stencil)
{
   struct m9_context *ctx = m9_context(pctx);
   uint64_t t_clear = m9_prof_begin();
   if (m9_faa_application_enabled() && buffers) {
      if (ctx->faa_frame.count) m9_context_sync(ctx);
      if (ctx->lost) { m9_prof_end(M9_PROF_CLEAR, t_clear); return; }
      ctx->faa_application_clear.pending = true;
      ctx->faa_application_clear.buffers |= buffers;
      /* Color and depth clears can arrive separately. Arguments for an
       * attachment absent from buffers are not part of this request. Merge
       * component masks so a later masked color clear retains earlier
       * component values, just as sequential full-surface clears do. */
      if (buffers & PIPE_CLEAR_COLOR0) {
         /* Gallium's mask has four bits per draw buffer. The state tracker
          * passes the complete mask (normally 0xffffffff), even when this
          * clear names only RT0. Bits for unrequested targets do not alter
          * the RT0 clear; buffers below still rejects an actual extra RT. */
         uint32_t rt0_mask = color_clear_mask & PIPE_MASK_RGBA;
         ctx->faa_application_clear.color_mask |= rt0_mask;
         if (color)
            for (unsigned i = 0; i < 4; i++)
               if (rt0_mask & (1u << i))
                  ctx->faa_application_clear.color.ui[i] = color->ui[i];
      }
      if (buffers & PIPE_CLEAR_DEPTH)
         ctx->faa_application_clear.depth = depth;
      if (buffers & PIPE_CLEAR_STENCIL) {
         ctx->faa_application_clear.stencil_mask |= stencil_clear_mask;
         ctx->faa_application_clear.stencil =
            (ctx->faa_application_clear.stencil & ~stencil_clear_mask) |
            (stencil & stencil_clear_mask);
      }
      /* The bounded native clear is full-surface only. An unrelated later
       * clear must not erase a pending scissored operation's refusal. */
      /* A supplied rectangle covering the complete target is a full clear.
       * Preserve a prior partial clear as a sticky refusal. */
      ctx->faa_application_clear.scissored |= !m9_scissor_full(ctx, scissor);
      if (scissor) ctx->faa_application_clear.scissor = *scissor;
   }
   if (m9_faa_application_enabled()) {
      /* The retained graph owns the GPU command state. Sending an ordinary
       * clear quad here would replace that state before the FAA frame ioctl.
       * Keep the clear pending until its actual FAA transaction succeeds. */
      m9_prof_end(M9_PROF_CLEAR, t_clear);
      return;
   }

   /* A job held for the scratch path is copied into place before the clear
    * that follows it, not over it. */
   m9_deferred_run(ctx);
   /* A clear passes no samples. */
   m9_query_pause(ctx);
   for (unsigned i = 0; i <= m9_clear_repeat; i++)
      m9_clear_locked(pctx, buffers, color_clear_mask, stencil_clear_mask,
                      scissor, color, depth, stencil);
   m9_query_resume(ctx);
   m9_prof_end(M9_PROF_CLEAR, t_clear);
}

static bool
m9_clear_render_target_engine(struct m9_context *ctx, struct pipe_surface *dst,
                              const union pipe_color_union *color,
                              unsigned x, unsigned y,
                              unsigned width, unsigned height,
                              bool render_condition_enabled);

static void
m9_clear_render_target(struct pipe_context *pctx, struct pipe_surface *dst,
                       const union pipe_color_union *color,
                       unsigned dstx, unsigned dsty,
                       unsigned width, unsigned height,
                       bool render_condition_enabled)
{
   struct m9_context *ctx = m9_context(pctx);
   struct m9_resource *resource = m9_res(dst->texture);
   union util_color uc;

   if (!width || !height)
      return;
   /* The same raster path as engine blits keeps preceding readers/writers
    * ordered in the queue, without waiting for them to CPU-fill VRAM. */
   if (m9_clear_render_target_engine(ctx, dst, color, dstx, dsty,
                                     width, height, render_condition_enabled))
      return;
   if (dst->level || resource->cpp != 4 || !resource->map)
      return;
   m9_context_sync(ctx);
   util_pack_color(color->f, dst->format, &uc);
   if (ctx->screen->queued_submit &&
       (ctx->lost || m9_wait_for_point(ctx->screen, ctx->last_point))) {
      ctx->lost = true;
      return;
   }
   if (!dstx && !dsty && width >= resource->width &&
       height >= resource->height) {
      /* A mip chain follows level 0 in the same object, so a full clear of
       * the object would wipe the chain with it. */
      uint64_t bytes = resource->texture.levels ?
                       resource->texture.level[0].face_bytes : resource->size;
      uint32_t *words = resource->map;

      for (uint64_t i = 0; i < bytes / 4; i++)
         words[i] = uc.ui[0];
   } else if (resource->tiled) {
      const struct pipe_box box = {
         .x = dstx, .y = dsty, .width = width, .height = height, .depth = 1,
      };
      uint32_t *rows = MALLOC((size_t)width * height * 4);

      for (size_t i = 0; rows && i < (size_t)width * height; i++)
         rows[i] = uc.ui[0];
      if (!rows || m9_surface_access(ctx, resource, PARHELIA_M9_ACCESS_WRITE,
                                     &box, (uint8_t *)rows))
         m9_warn_once(ctx, M9_WARN_CLEAR, "partial render target clear failed");
      FREE(rows);
   } else {
      m9_resource_clear_linear(resource, uc.ui[0], dstx, dsty, width, height);
   }
}

static void
m9_clear_depth_stencil(struct pipe_context *pctx, struct pipe_surface *dst,
                       unsigned clear_flags, double depth, unsigned stencil,
                       unsigned dstx, unsigned dsty,
                       unsigned width, unsigned height,
                       bool render_condition_enabled)
{
   struct m9_context *ctx = m9_context(pctx);
   struct m9_resource *resource = m9_res(dst->texture);

   (void)render_condition_enabled;
   if (dst->level || !resource->map || !width || !height)
      return;
   /* The bound attachment clears on the engine, inside the rectangle. */
   if (m9_context_depth(ctx) == resource && ctx->params_valid &&
       dst->format == ctx->framebuffer.zsbuf.format) {
      const struct pipe_scissor_state rect = {
         .minx = dstx, .miny = dsty,
         .maxx = dstx + width, .maxy = dsty + height,
      };
      static const float zero[4];

      m9_clear_quad(ctx, &rect, 0, zero, clear_flags & PIPE_CLEAR_DEPTH,
                    depth, clear_flags & PIPE_CLEAR_STENCIL, stencil, 0xff);
      return;
   }
   if (resource->format != M9_DEPTH_FORMAT_D24S8) {
      m9_warn_once(ctx, M9_WARN_CLEAR, "a non-D24S8 depth clear is not "
                   "implemented");
      return;
   }
   m9_context_sync(ctx);
   if (!dstx && !dsty && width >= resource->width &&
       height >= resource->height) {
      if (ctx->screen->queued_submit &&
          (ctx->lost || m9_wait_for_point(ctx->screen, ctx->last_point))) {
         ctx->lost = true;
         return;
      }
      m9_clear_depth_cpu(resource, clear_flags, depth, stencil);
      return;
   }
   /* The CPU sees neither the engine's D24S8 element order (stencil
    * readback is wrong, MEASURED) nor the hierarchical buffer. */
   m9_warn_once(ctx, M9_WARN_CLEAR, "a partial depth/stencil clear of an "
                "unbound surface is not implemented");
}

/* The three storage kinds m9_texture_bind_view accepts for a plane: a tiled
 * texture chain, whose level is re-based by the LOD clamp, a depth surface
 * and a tiled colour target, which both hold level 0 alone. */
static bool
m9_resource_samplable(const struct m9_resource *res, unsigned level)
{
   if (!res || (res->base.target != PIPE_TEXTURE_2D &&
                res->base.target != PIPE_TEXTURE_RECT))
      return false;
   if (res->texture.levels)
      return level < res->texture.levels && level <= res->base.last_level &&
             !(res->texture_unplaced & (1u << level));
   if (level)
      return false;
   if (res->kind == M9_RESOURCE_LINEAR_DEPTH && res->format)
      return true;
   return res->tiled && res->kind == M9_RESOURCE_LINEAR_COLOR &&
          (res->base.format == PIPE_FORMAT_B8G8R8A8_UNORM ||
           res->base.format == PIPE_FORMAT_B8G8R8X8_UNORM);
}

/*
 * A copy on the raster back end.  The engine writes whatever layout its
 * destination descriptor names, so a copy is a textured quad: the destination
 * binds as colour target 0 and the source as a texture unit.  CAPTURED in the
 * vendor's own glamor CopyArea and its shadow map; see
 * docs/parhelia-m9-texture-upload-and-copy-re.md.
 */
static bool
m9_engine_copy_ready(const struct m9_context *ctx,
                     const struct pipe_resource *dst, unsigned dst_level,
                     const struct pipe_resource *src, unsigned src_level)
{
   const struct m9_resource *d = m9_res((struct pipe_resource *)dst);
   const struct m9_resource *sr = m9_res((struct pipe_resource *)src);

   /* A destination level is a render surface when the chain laid it out as
    * one; the packed tail levels are not (m9_texture_renders_level). */
   return ctx->blitter && !ctx->lost && !ctx->query_active &&
          dst && src &&
          dst->nr_samples <= 1 && src->nr_samples <= 1 &&
          dst->target != PIPE_BUFFER && src->target != PIPE_BUFFER &&
          dst->depth0 == 1 && dst->array_size == 1 &&
          src_level <= src->last_level &&
          !util_format_is_depth_or_stencil(dst->format) &&
          !util_format_is_depth_or_stencil(src->format) &&
          (!dst_level || m9_level_targets) &&
          m9_resource_is_color_target(d, dst_level) &&
          m9_resource_samplable(sr, src_level);
}

/* Save the state util_blitter overwrites.  The driver keeps no geometry,
 * tessellation or stream-output state, so those savers are not called. */
static void
m9_blitter_save(struct m9_context *ctx)
{
   struct blitter_context *b = ctx->blitter;

   util_blitter_save_vertex_buffers(b, ctx->vertex_buffers,
                                    ctx->num_vertex_buffers);
   util_blitter_save_vertex_elements(b, ctx->velems);
   util_blitter_save_vertex_shader(b, ctx->vs);
   util_blitter_save_fragment_shader(b, ctx->fs_handle);
   util_blitter_save_rasterizer(b, (void *)ctx->rasterizer);
   util_blitter_save_viewport(b, &ctx->viewport);
   util_blitter_save_scissor(b, &ctx->scissor);
   util_blitter_save_blend(b, (void *)ctx->blend);
   util_blitter_save_depth_stencil_alpha(b, (void *)ctx->dsa);
   util_blitter_save_framebuffer(b, &ctx->framebuffer);
   util_blitter_save_fragment_sampler_states(b, M9_TEXTURE_MAX_UNITS,
                                             (void **)ctx->fs_samplers);
   util_blitter_save_fragment_sampler_views(b, M9_TEXTURE_MAX_UNITS,
                                            ctx->fs_views);
}

/*
 * Run one blitter operation and report whether the engine actually drew it.
 * A draw the backend cannot express is dropped with a warning rather than
 * failing, which would leave the destination silently unwritten, so the
 * caller falls back to the CPU path whenever the skip count moved.
 */
static bool
m9_engine_drew(struct m9_context *ctx, uint32_t skipped_before)
{
   return !ctx->lost && ctx->skipped_draws == skipped_before;
}

static bool
m9_clear_render_target_engine(struct m9_context *ctx, struct pipe_surface *dst,
                              const union pipe_color_union *color,
                              unsigned x, unsigned y,
                              unsigned width, unsigned height,
                              bool render_condition_enabled)
{
   struct m9_resource *res = m9_res(dst->texture);
   if (dst->level > res->base.last_level)
      return false;
   unsigned w = u_minify(res->base.width0, dst->level);
   unsigned h = u_minify(res->base.height0, dst->level);

   /* Use only storage already admitted as an ordinary colour target. Query
    * and conditional clears retain the old path, as do formatted FAA views,
    * layered images and the packed mip tail. Wide colour engine writes have
    * a separately observed rounding difference (see m9_blit_engine). */
   if (!m9_clear_on_engine || !ctx->blitter || ctx->blitter->running ||
       ctx->blitter->cb_slot != 0 ||
       ctx->lost || ctx->query_active || render_condition_enabled ||
       ctx->faa_surface.graph.initialized || ctx->screen->faa.prepare ||
       m9_faa_active(&ctx->screen->faa) ||
       (res->base.target != PIPE_TEXTURE_2D &&
        res->base.target != PIPE_TEXTURE_RECT) ||
       res->base.nr_samples > 1 || res->base.depth0 != 1 ||
       res->base.array_size != 1 || dst->first_layer || dst->last_layer ||
       dst->format != res->base.format || res->cpp != 4 ||
       util_format_is_pure_integer(dst->format) ||
       util_format_get_component_bits(dst->format,
                                      UTIL_FORMAT_COLORSPACE_RGB, 0) > 8 ||
       (dst->level && !m9_level_targets) ||
       !m9_resource_is_color_target(res, dst->level) ||
       !width || !height || x >= w || y >= h ||
       width > w - x || height > h - y)
      return false;

   uint32_t skipped = ctx->skipped_draws;
   m9_blitter_save(ctx);
   /* Unlike a copy shader, the clear shader replaces fragment CB0. */
   util_blitter_save_fragment_constant_buffer_slot(ctx->blitter,
                                                   &ctx->fs_constants);
   util_blitter_clear_render_target(ctx->blitter, dst, color,
                                    x, y, width, height);
   if (!m9_engine_drew(ctx, skipped))
      return false;
   ctx->engine_clears++;
   return true;
}

/* One bilinear 8-bit texel, with the edge clamped: the reduction a filtered
 * blit performs when the engine cannot draw it. */
static void
m9_bilinear_texel(const uint8_t *src, int width, int height, float u, float v,
                  uint8_t *out)
{
   int x0 = (int)floorf(u), y0 = (int)floorf(v);
   float fx = u - x0, fy = v - y0;
   int xa = CLAMP(x0, 0, width - 1), xb = CLAMP(x0 + 1, 0, width - 1);
   int ya = CLAMP(y0, 0, height - 1), yb = CLAMP(y0 + 1, 0, height - 1);
   const uint8_t *p00 = src + ((size_t)ya * width + xa) * 4;
   const uint8_t *p10 = src + ((size_t)ya * width + xb) * 4;
   const uint8_t *p01 = src + ((size_t)yb * width + xa) * 4;
   const uint8_t *p11 = src + ((size_t)yb * width + xb) * 4;

   for (unsigned c = 0; c < 4; c++) {
      float top = p00[c] + (p10[c] - p00[c]) * fx;
      float bot = p01[c] + (p11[c] - p01[c]) * fx;

      out[c] = (uint8_t)CLAMP(lroundf(top + (bot - top) * fy), 0, 255);
   }
}

/* Unscaled colour blit with format conversion through CPU transfers. */
static bool
m9_blit_convert(struct pipe_context *pctx, const struct pipe_blit_info *info)
{
   const struct pipe_box *s = &info->src.box, *d = &info->dst.box;
   struct pipe_transfer *st = NULL, *dt = NULL;
   uint8_t *src, *dst;
   bool ok;

   unsigned blocksize;
   uint8_t *scaled = NULL;

   /* util_format_translate widens 8-bit channels by bit replication, GL
    * rounds c * 1023 / 255; the float path of m9_blit_cpu does that. */
   if (util_format_get_component_bits(info->dst.format,
                                      UTIL_FORMAT_COLORSPACE_RGB, 0) > 8)
      return false;
   if (info->mask != PIPE_MASK_RGBA || info->scissor_enable ||
       info->render_condition_enable || info->alpha_blend ||
       s->depth != 1 || d->depth != 1 || s->width <= 0 || s->height <= 0 ||
       d->width <= 0 || d->height <= 0 || info->swizzle_enable)
      return false;
   src = pipe_texture_map(pctx, info->src.resource, info->src.level, s->z,
                          PIPE_MAP_READ, s->x, s->y, s->width, s->height, &st);
   dst = src ? pipe_texture_map(pctx, info->dst.resource, info->dst.level, d->z,
                                PIPE_MAP_WRITE, d->x, d->y, d->width,
                                d->height, &dt) : NULL;
   blocksize = util_format_get_blocksize(info->dst.format);
   if (src && dst && (s->width != d->width || s->height != d->height)) {
      /* Translate the source, then resample it.  A filtered blit has to be
       * filtered here too: picking one texel of four is a decimation, not a
       * reduction, and it aliases silently. */
      bool linear = info->filter == PIPE_TEX_FILTER_LINEAR && blocksize == 4;

      scaled = MALLOC((size_t)s->width * s->height * blocksize);
      ok = scaled &&
           util_format_translate(info->dst.format, scaled,
                                 s->width * blocksize, 0, 0, info->src.format,
                                 src, st->stride, 0, 0, s->width, s->height);
      for (int y = 0; ok && y < d->height; y++) {
         float fv = ((float)y + 0.5f) * s->height / d->height - 0.5f;
         int sy = (int)(((int64_t)y * s->height + s->height / 2) / d->height);

         for (int x = 0; x < d->width; x++) {
            float fu = ((float)x + 0.5f) * s->width / d->width - 0.5f;
            int sx = (int)(((int64_t)x * s->width + s->width / 2) / d->width);
            uint8_t *out = (uint8_t *)dst + (size_t)y * dt->stride +
                           (size_t)x * blocksize;

            if (linear) {
               m9_bilinear_texel(scaled, s->width, s->height, fu, fv, out);
               continue;
            }
            memcpy(out, scaled + ((size_t)MIN2(sy, s->height - 1) * s->width +
                                  MIN2(sx, s->width - 1)) * blocksize,
                   blocksize);
         }
      }
   } else {
      ok = src && dst &&
           util_format_translate(info->dst.format, dst, dt->stride, 0, 0,
                                 info->src.format, src, st->stride, 0, 0,
                                 s->width, s->height);
   }
   FREE(scaled);
   if (dt)
      pipe_texture_unmap(pctx, dt);
   if (st)
      pipe_texture_unmap(pctx, st);
   return ok;
}

/* The source coordinate of destination column (or row) i: GL samples a blit
 * at the pixel centre mapped through the two boxes, mirrored when their
 * extents differ in sign. */
static float
m9_blit_coord(int i, int d0, int dsize, int s0, int ssize)
{
   return s0 + ((float)i + 0.5f - d0) * (float)ssize / (float)dsize;
}

static int
m9_blit_tap(float u, int limit)
{
   return CLAMP((int)floorf(u), 0, limit - 1);
}

/* Inclusive source range a destination span [a, b) reads, one texel wider
 * on each side for a filtered blit. */
static void
m9_blit_src_span(int a, int b, int d0, int dsize, int s0, int ssize,
                 int limit, bool linear, int *lo, int *hi)
{
   float u0 = m9_blit_coord(a, d0, dsize, s0, ssize);
   float u1 = m9_blit_coord(b - 1, d0, dsize, s0, ssize);
   int pad = linear ? 1 : 0;

   *lo = m9_blit_tap(MIN2(u0, u1) - pad, limit);
   *hi = m9_blit_tap(MAX2(u0, u1) + pad, limit);
}

/*
 * Every blit the engine and the copy paths refuse, on the CPU: depth and
 * stencil, partial channel masks, scissors, mirroring and scaling.  The
 * source rectangle is unpacked first, so a copy within one surface (CopyPixels)
 * reads the old content however the two boxes overlap.
 */
static bool
m9_blit_cpu(struct pipe_context *pctx, const struct pipe_blit_info *info)
{
   const struct pipe_box *s = &info->src.box, *d = &info->dst.box;
   enum pipe_format sf = info->src.format, df = info->dst.format;
   struct pipe_resource *sres = info->src.resource, *dres = info->dst.resource;
   int sw = u_minify(sres->width0, info->src.level);
   int sh = u_minify(sres->height0, info->src.level);
   int dw = u_minify(dres->width0, info->dst.level);
   int dh = u_minify(dres->height0, info->dst.level);
   bool zs = info->mask & (PIPE_MASK_Z | PIPE_MASK_S);
   bool do_z = (info->mask & PIPE_MASK_Z) &&
               util_format_has_depth(util_format_description(sf)) &&
               util_format_has_depth(util_format_description(df));
   bool do_s = (info->mask & PIPE_MASK_S) &&
               util_format_has_stencil(util_format_description(sf)) &&
               util_format_has_stencil(util_format_description(df));
   unsigned cmask = info->mask & PIPE_MASK_RGBA;
   bool linear = info->filter == PIPE_TEX_FILTER_LINEAR && !zs &&
                 !util_format_is_pure_integer(sf);
   int x0 = MIN2(d->x, d->x + d->width), x1 = MAX2(d->x, d->x + d->width);
   int y0 = MIN2(d->y, d->y + d->height), y1 = MAX2(d->y, d->y + d->height);
   int sx0, sx1, sy0, sy1, w, h, rw, rh;
   struct pipe_transfer *st = NULL, *dt = NULL;
   uint32_t *src = NULL, *row = NULL, *z = NULL;
   uint8_t *sbuf = NULL, *s8 = NULL;
   const uint8_t *smap;
   uint8_t *dmap;
   bool ok = false;

   if (s->depth != 1 || d->depth != 1 || !s->width || !s->height ||
       !d->width || !d->height || info->swizzle_enable ||
       info->alpha_blend || (zs && cmask) || (!zs && !cmask) ||
       (zs && !do_z && !do_s) ||
       util_format_is_compressed(sf) || util_format_is_compressed(df) ||
       (!zs && (util_format_is_depth_or_stencil(sf) ||
                util_format_is_depth_or_stencil(df) ||
                util_format_is_pure_integer(sf) !=
                util_format_is_pure_integer(df))))
      return false;
   x0 = MAX2(x0, 0);
   y0 = MAX2(y0, 0);
   x1 = MIN2(x1, dw);
   y1 = MIN2(y1, dh);
   if (info->scissor_enable) {
      x0 = MAX2(x0, (int)info->scissor.minx);
      y0 = MAX2(y0, (int)info->scissor.miny);
      x1 = MIN2(x1, (int)info->scissor.maxx);
      y1 = MIN2(y1, (int)info->scissor.maxy);
   }
   if (x0 >= x1 || y0 >= y1)
      return true;
   w = x1 - x0;
   h = y1 - y0;
   m9_blit_src_span(x0, x1, d->x, d->width, s->x, s->width, sw, linear,
                    &sx0, &sx1);
   m9_blit_src_span(y0, y1, d->y, d->height, s->y, s->height, sh, linear,
                    &sy0, &sy1);
   rw = sx1 - sx0 + 1;
   rh = sy1 - sy0 + 1;

   smap = pipe_texture_map(pctx, sres, info->src.level, s->z, PIPE_MAP_READ,
                           sx0, sy0, rw, rh, &st);
   if (!smap)
      return false;
   /* The source as unpacked words: four per texel for colour, one 32-bit
    * normalised depth and one stencil byte for depth/stencil. */
   if (zs) {
      z = do_z ? MALLOC((size_t)rw * rh * 4) : NULL;
      sbuf = do_s ? MALLOC((size_t)rw * rh) : NULL;
      ok = (!do_z || z) && (!do_s || sbuf);
      for (int y = 0; ok && y < rh; y++) {
         const uint8_t *p = smap + (size_t)y * st->stride;

         if (do_z)
            util_format_unpack_z_32unorm(sf, z + (size_t)y * rw, p, rw);
         if (do_s)
            util_format_unpack_s_8uint(sf, sbuf + (size_t)y * rw, p, rw);
      }
   } else {
      src = MALLOC((size_t)rw * rh * 16);
      ok = src;
      if (ok)
         util_format_unpack_rgba_rect(sf, src, rw * 16, smap, st->stride,
                                      rw, rh);
   }
   pipe_texture_unmap(pctx, st);
   /* Read back what the blit keeps: the other half of a packed depth/stencil
    * texel, or the channels outside the mask. */
   dmap = ok ? pipe_texture_map(pctx, dres, info->dst.level, d->z,
                                PIPE_MAP_READ_WRITE, x0, y0, w, h, &dt) : NULL;
   ok = dmap != NULL;
   row = ok ? MALLOC((size_t)w * 16) : NULL;
   s8 = ok && do_s ? MALLOC(w) : NULL;
   ok = row && (!do_s || s8);
   for (int y = 0; ok && y < h; y++) {
      uint8_t *p = dmap + (size_t)y * dt->stride;
      float v = m9_blit_coord(y0 + y, d->y, d->height, s->y, s->height);
      int ty = m9_blit_tap(v, sh) - sy0;

      if (zs) {
         for (int x = 0; x < w; x++) {
            float u = m9_blit_coord(x0 + x, d->x, d->width, s->x, s->width);
            size_t i = (size_t)ty * rw + m9_blit_tap(u, sw) - sx0;

            if (do_z)
               row[x] = z[i];
            if (do_s)
               s8[x] = sbuf[i];
         }
         if (do_z)
            util_format_pack_z_32unorm(df, p, row, w);
         if (do_s)
            util_format_pack_s_8uint(df, p, s8, w);
         continue;
      }
      if (cmask != PIPE_MASK_RGBA)
         util_format_unpack_rgba(df, row, p, w);
      for (int x = 0; x < w; x++) {
         float u = m9_blit_coord(x0 + x, d->x, d->width, s->x, s->width);
         uint32_t texel[4];

         if (linear) {
            /* Bilinear with the edge clamped, on unpacked floats. */
            float fu = u - 0.5f, fv = v - 0.5f;
            int ax = m9_blit_tap(fu, sw) - sx0, ay = m9_blit_tap(fv, sh) - sy0;
            int bx = m9_blit_tap(fu + 1.0f, sw) - sx0;
            int by = m9_blit_tap(fv + 1.0f, sh) - sy0;
            float wx = fu - floorf(fu), wy = fv - floorf(fv);
            const float *f = (const float *)src;

            for (unsigned c = 0; c < 4; c++) {
               float t = f[((size_t)ay * rw + ax) * 4 + c] * (1 - wx) +
                         f[((size_t)ay * rw + bx) * 4 + c] * wx;
               float b = f[((size_t)by * rw + ax) * 4 + c] * (1 - wx) +
                         f[((size_t)by * rw + bx) * 4 + c] * wx;
               float r = t * (1 - wy) + b * wy;

               memcpy(&texel[c], &r, 4);
            }
         } else {
            memcpy(texel, src + ((size_t)ty * rw + m9_blit_tap(u, sw) - sx0) * 4,
                   16);
         }
         for (unsigned c = 0; c < 4; c++)
            if (cmask & (1u << c))
               row[x * 4 + c] = texel[c];
      }
      util_format_pack_rgba(df, p, row, w);
   }
   if (dt)
      pipe_texture_unmap(pctx, dt);
   FREE(s8);
   FREE(row);
   FREE(src);
   FREE(z);
   FREE(sbuf);
   return ok;
}

/* The engine samples the source, so it filters, scales and scissors; the CPU
 * path does none of those and silently sampled a filtered blit nearest. */
static bool
m9_blit_engine(struct pipe_context *pctx, const struct pipe_blit_info *info)
{
   struct m9_context *ctx = m9_context(pctx);
   uint32_t skipped;
   /* The engine's writes into a 10-bit surface are one code high in a
    * quarter of the texels, by position (MEASURED, 8-bit RGBA uploads into
    * GL_RGB10/GL_RGB10_A2), so an unscaled copy into one runs on the CPU. */
   bool wide_copy =
      util_format_get_component_bits(info->dst.format,
                                     UTIL_FORMAT_COLORSPACE_RGB, 0) > 8 &&
      info->src.box.width == info->dst.box.width &&
      info->src.box.height == info->dst.box.height;

   if (!m9_blit_on_engine || wide_copy ||
       !m9_engine_copy_ready(ctx, info->dst.resource, info->dst.level,
                             info->src.resource, info->src.level) ||
       info->dst.format != info->dst.resource->format ||
       info->src.format != info->src.resource->format ||
       (info->mask & PIPE_MASK_RGBA) != PIPE_MASK_RGBA ||
       info->swizzle_enable || info->render_condition_enable ||
       info->src.box.depth != 1 || info->dst.box.depth != 1)
      return false;
   skipped = ctx->skipped_draws;
   m9_blitter_save(ctx);
   util_blitter_blit(ctx->blitter, info, NULL);
   ctx->engine_blits++;
   if (ctx->debug)
      fprintf(stderr, "m9x: engine blit %dx%d -> %dx%d filter %u: %s\n",
              info->src.box.width, info->src.box.height, info->dst.box.width,
              info->dst.box.height, info->filter,
              m9_engine_drew(ctx, skipped) ? "drawn" : "skipped");
   return m9_engine_drew(ctx, skipped);
}

/*
 * u_blitter brackets its draws with this, and calls it unconditionally.  The
 * flag is recorded rather than acted on: the occlusion counter is job state
 * this driver only sets while a query is live, and the engine copy path
 * refuses to run inside a live query at all (m9_engine_copy_ready), so no
 * internal draw can reach a counted job.
 */
static void
m9_set_active_query_state(struct pipe_context *pctx, bool enable)
{
   m9_context(pctx)->queries_enabled = enable;
}

static void
m9_resource_copy_region(struct pipe_context *pctx, struct pipe_resource *dst,
                        unsigned dst_level, unsigned dstx, unsigned dsty,
                        unsigned dstz, struct pipe_resource *src,
                        unsigned src_level, const struct pipe_box *src_box)
{
   struct m9_context *ctx = m9_context(pctx);
   uint32_t skipped;

   if (src == ctx->proxy.tex || dst == ctx->proxy.tex)
      m9_proxy_writeback(ctx);
   if (src_box && !dstz && !src_box->z && src_box->depth == 1 &&
       m9_engine_copy_ready(ctx, dst, dst_level, src, src_level) &&
       util_format_get_blocksize(dst->format) ==
       util_format_get_blocksize(src->format)) {
      skipped = ctx->skipped_draws;
      m9_blitter_save(ctx);
      util_blitter_copy_texture(ctx->blitter, dst, dst_level, dstx, dsty,
                                dstz, src, src_level, src_box);
      ctx->engine_blits++;
      if (m9_engine_drew(ctx, skipped)) {
         if (dst == ctx->proxy.tex)
            m9_proxy_reseed(ctx);
         return;
      }
   }
   m9_res(dst)->generation++;
   util_resource_copy_region(pctx, dst, dst_level, dstx, dsty, dstz, src,
                             src_level, src_box);
}

/* The scratch colour surface of exactly this shape, allocated once and kept:
 * a split job has to cover the whole of the target it renders into, so the
 * surface is the size of the box and not a larger one reused. */
static struct pipe_resource *
m9_scratch_target(struct m9_context *ctx, uint32_t w, uint32_t h,
                  enum pipe_format format)
{
   struct m9_scratch_target *entry = NULL, **oldest = NULL;
   uint64_t total = 0, bytes = (uint64_t)w * h * 4;
   struct pipe_resource templ = {0};

   util_dynarray_foreach(&ctx->scratch_targets, struct m9_scratch_target *,
                         it) {
      const struct pipe_resource *res = (*it)->res;

      total += (uint64_t)res->width0 * res->height0 * 4;
      if (res->width0 == w && res->height0 == h && res->format == format)
         entry = *it;
      else if (!oldest || (*it)->used < (*oldest)->used)
         oldest = it;
   }
   /* Release the shape used longest ago before adding another, so a context
    * that walks through many boxes does not grow without end. */
   while (!entry && oldest && total + bytes > M9_SCRATCH_BUDGET) {
      struct m9_scratch_target *evict = *oldest;

      total -= (uint64_t)evict->res->width0 * evict->res->height0 * 4;
      pipe_resource_reference(&evict->res, NULL);
      *oldest = *util_dynarray_pop_ptr(&ctx->scratch_targets,
                                       struct m9_scratch_target *);
      FREE(evict);
      oldest = NULL;
      util_dynarray_foreach(&ctx->scratch_targets, struct m9_scratch_target *,
                            it)
         if (!oldest || (*it)->used < (*oldest)->used)
            oldest = it;
   }
   if (!entry) {
      templ.target = PIPE_TEXTURE_2D;
      templ.format = format;
      templ.width0 = w;
      templ.height0 = h;
      templ.depth0 = 1;
      templ.array_size = 1;
      templ.usage = PIPE_USAGE_DEFAULT;
      templ.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW;
      entry = CALLOC_STRUCT(m9_scratch_target);
      if (!entry)
         return NULL;
      entry->res = ctx->screen->base.resource_create(&ctx->screen->base,
                                                     &templ);
      if (!entry->res) {
         FREE(entry);
         return NULL;
      }
      util_dynarray_append_typed(&ctx->scratch_targets,
                                 struct m9_scratch_target *, entry);
   }
   entry->used = ++ctx->scratch_clock;
   return entry->res;
}

/* One copy of the scratch path, on the engine or through CPU transfers. */
static void
m9_scratch_copy(struct m9_context *ctx, struct pipe_resource *dst,
                unsigned dx, unsigned dy, struct pipe_resource *src,
                const struct pipe_box *box)
{
   struct pipe_context *pctx = &ctx->base;

   if (m9_split_copy == 2)
      util_resource_copy_region(pctx, dst, 0, dx, dy, 0, src, 0, box);
   else
      pctx->resource_copy_region(pctx, dst, 0, dx, dy, 0, src, 0, box);
}

/* Move a held job's records into the scratch surface's own screen space.  A
 * record carries x and y multiplied by w with w itself beside them, so a
 * screen translation multiplies by that slot, and it counts rows from the
 * bottom where the box counts them from the top. */
static void
m9_scratch_records(struct m9_context *ctx, const struct m9_deferred_job *d)
{
   unsigned records = util_dynarray_num_elements(&ctx->records, float) /
                      ctx->record_floats;
   float dx = (float)d->x;
   float dy = (float)d->height + (float)d->y - (float)d->target_height;

   for (unsigned k = 0; k < records; k++) {
      float *p = (float *)ctx->records.data + (uint64_t)k * ctx->record_floats;
      float w = p[3] > 0.0f ? p[3] : 1.0f;

      p[0] -= dx * w;
      p[1] += dy * w;
   }
}

/*
 * Render a held multi-pass job into a scratch surface it covers whole and
 * copy it into place.  The destination seeds the scratch first, so blending,
 * a partial colour mask and geometry that does not fill its own box all read
 * and keep what the destination held.  This runs where the driver is outside
 * the draw module, because the two copies are ordinary blitter draws: the job
 * pending now is held aside in turn, since it was recorded after this one and
 * has to reach the engine after it.
 */
static void
m9_deferred_run(struct m9_context *ctx)
{
   struct m9_deferred_job *d = &ctx->deferred;
   struct pipe_context *pctx = &ctx->base;
   struct pipe_framebuffer_state target_fb = {0}, fb = {0};
   struct m9_job_state saved = {0};
   struct pipe_resource *scratch;
   struct pipe_box box;
   const char *why = NULL;
   bool engine = false;

   if (!d->pending || ctx->in_scratch)
      return;
   d->pending = false;
   ctx->in_scratch = true;
   saved.record_floats = ctx->record_floats;
   m9_job_state_swap(ctx, &saved);
   util_copy_framebuffer_state(&target_fb, &ctx->framebuffer);
   scratch = ctx->lost ? NULL :
             m9_scratch_target(ctx, d->width, d->height, d->dst->format);
   if (!scratch) {
      why = "no scratch surface for a multi-pass draw over part of its "
            "render target";
   } else {
      u_box_2d(d->x, d->y, d->width, d->height, &box);
      if (m9_split_seed)
         m9_scratch_copy(ctx, scratch, 0, 0, d->dst, &box);
      fb.width = d->width;
      fb.height = d->height;
      fb.nr_cbufs = 1;
      fb.cbufs[0].texture = scratch;
      fb.cbufs[0].format = scratch->format;
      /* The depth pair and the engine's back-end object this binds are the
       * scratch surface's own: the scene's own targets of the same shape --
       * desktop's window is the size of the box its blur covers -- would
       * otherwise share the engine state with it. */
      ctx->scratch_bind = true;
      pctx->set_framebuffer_state(pctx, &fb);
      ctx->scratch_bind = false;
      if (!ctx->params_valid)
         why = "a scratch surface for a multi-pass draw over part of its "
               "render target is not a valid render target";
   }
   m9_job_state_swap(ctx, &d->state);
   if (why) {
      m9_job_state_drop(ctx, why, why);
   } else {
      /* The scratch surface brings its own depth pair, back-end object and
       * clip window, and the geometry moves with it. */
      ctx->job_key.depth = ctx->target_depth;
      ctx->job_key.aux = ctx->target_aux;
      ctx->job_key.clip_min = ctx->params.clip_min;
      ctx->job_key.clip_max = ctx->params.clip_max;
      m9_scratch_records(ctx, d);
      /* The engine keeps per-target state in the back-end object, and a
       * multi-pass job that finds in it what the draws into another target
       * left behind stops the engine: the scratch path stalls at the ninth
       * draw without this and renders 32 with it (MEASURED, splitdesk:32:1:5:
       * 1920x1200:5:420x420 against M9X_BE_CLEAR).  The object the job binds
       * is cleared ahead of it. */
      if (m9_split_be)
         m9_be_zero(ctx);
      m9_submit_job(ctx);
   }
   m9_job_state_swap(ctx, &d->state);
   if (scratch) {
      pctx->set_framebuffer_state(pctx, &target_fb);
      if (m9_split_be > 1)
         m9_be_zero(ctx);
      if (!why) {
         uint32_t drew = ctx->engine_blits;

         u_box_2d(0, 0, d->width, d->height, &box);
         m9_scratch_copy(ctx, d->dst, d->x, d->y, scratch, &box);
         engine = ctx->engine_blits > drew;
      }
   }
   util_unreference_framebuffer_state(&target_fb);
   pipe_resource_reference(&d->dst, NULL);
   m9_job_state_swap(ctx, &saved);
   m9_job_state_fini(&saved);
   ctx->in_scratch = false;
   if (ctx->debug)
      fprintf(stderr, "m9x: split job %ux%u at %u,%u of %u rows: %s\n",
              d->width, d->height, d->x, d->y, d->target_height,
              why ? why : engine ? "drawn on a scratch surface, copied back "
              "on the engine" : "drawn on a scratch surface, copied back "
              "through transfers");
}

static void
m9_blit(struct pipe_context *pctx, const struct pipe_blit_info *info)
{
   /* A layered blit -- st's upload of a whole volume, say -- is one blit
    * per slice: nothing below takes a box deeper than one layer. */
   if (info->src.box.depth == info->dst.box.depth && info->dst.box.depth > 1) {
      struct pipe_blit_info slice = *info;

      slice.src.box.depth = slice.dst.box.depth = 1;
      for (int k = 0; k < info->dst.box.depth; k++) {
         slice.src.box.z = info->src.box.z + k;
         slice.dst.box.z = info->dst.box.z + k;
         m9_blit(pctx, &slice);
      }
      return;
   }
   struct m9_context *ctx = m9_context(pctx);
   bool proxied = info->dst.resource == ctx->proxy.tex;

   if (proxied || info->src.resource == ctx->proxy.tex)
      m9_proxy_writeback(ctx);
   if (!m9_blit_engine(pctx, info) &&
       !util_try_blit_via_copy_region(pctx, info, false) &&
       !m9_blit_convert(pctx, info) &&
       !m9_blit_cpu(pctx, info))
      m9_warn_once(m9_context(pctx), M9_WARN_BLIT, "blit of a compressed, "
                   "swizzled or blended kind is not implemented");
   if (proxied)
      m9_proxy_reseed(ctx);
}

static void
m9_flush_resource(struct pipe_context *pctx, struct pipe_resource *resource)
{
   (void)resource;
   m9_context_sync(m9_context(pctx));
}

/* State objects */

static void *
m9_create_blend_state(struct pipe_context *pctx,
                      const struct pipe_blend_state *state)
{
   (void)pctx;
   return mem_dup(state, sizeof(*state));
}

static void
m9_bind_blend_state(struct pipe_context *pctx, void *state)
{
   m9_context(pctx)->blend = state;
}

static void *
m9_create_dsa_state(struct pipe_context *pctx,
                    const struct pipe_depth_stencil_alpha_state *state)
{
   (void)pctx;
   return mem_dup(state, sizeof(*state));
}

static void
m9_bind_dsa_state(struct pipe_context *pctx, void *state)
{
   m9_context(pctx)->dsa = state;
}

static void *
m9_create_rasterizer_state(struct pipe_context *pctx,
                           const struct pipe_rasterizer_state *state)
{
   (void)pctx;
   return mem_dup(state, sizeof(*state));
}

static void
m9_bind_rasterizer_state(struct pipe_context *pctx, void *state)
{
   struct m9_context *ctx = m9_context(pctx);

   /* draw may rebind its own state; set it before recording ours. */
   draw_set_rasterizer_state(ctx->draw, state, state);
   ctx->rasterizer = state;
}

static void *
m9_create_sampler_state(struct pipe_context *pctx,
                        const struct pipe_sampler_state *state)
{
   (void)pctx;
   return mem_dup(state, sizeof(*state));
}

static void
m9_bind_sampler_states(struct pipe_context *pctx, mesa_shader_stage shader,
                       unsigned start, unsigned count, void **states)
{
   struct m9_context *ctx = m9_context(pctx);

   if (shader == MESA_SHADER_VERTEX)
      m9_vs_texture_states(pctx, start, count, states);
   if (shader != MESA_SHADER_FRAGMENT)
      return;
   for (unsigned i = 0; i < count && start + i < PIPE_MAX_SAMPLERS; i++)
      ctx->fs_samplers[start + i] = states ? states[i] : NULL;
}

static void
m9_delete_state(struct pipe_context *pctx, void *state)
{
   (void)pctx;
   FREE(state);
}

static struct pipe_sampler_view *
m9_create_sampler_view(struct pipe_context *pctx, struct pipe_resource *texture,
                       const struct pipe_sampler_view *templ)
{
   struct pipe_sampler_view *view = CALLOC_STRUCT(pipe_sampler_view);

   if (!view)
      return NULL;
   *view = *templ;
   pipe_reference_init(&view->reference, 1);
   view->texture = NULL;
   pipe_resource_reference(&view->texture, texture);
   view->context = pctx;
   return view;
}

static void
m9_sampler_view_destroy(struct pipe_context *pctx,
                        struct pipe_sampler_view *view)
{
   (void)pctx;
   pipe_resource_reference(&view->texture, NULL);
   FREE(view);
}

static void
m9_set_sampler_views(struct pipe_context *pctx, mesa_shader_stage shader,
                     unsigned start, unsigned count, unsigned unbind_trailing,
                     struct pipe_sampler_view **views)
{
   struct m9_context *ctx = m9_context(pctx);

   if (shader == MESA_SHADER_VERTEX)
      m9_vs_texture_views(pctx, start, count, unbind_trailing, views);
   if (shader != MESA_SHADER_FRAGMENT)
      return;
   for (unsigned i = 0; i < count + unbind_trailing &&
                        start + i < PIPE_MAX_SAMPLERS; i++)
      pipe_sampler_view_reference(&ctx->fs_views[start + i],
                                  views && i < count ? views[i] : NULL);
}

/* Program identity for the cached c2 images; never zero, never reused. */

static unsigned
m9_vs_type_size(const struct glsl_type *type, bool bindless)
{
   return glsl_count_vec4_slots(type, false, bindless);
}

static void
m9_vs_spec_free(struct m9_vs_spec *spec)
{
   util_dynarray_foreach(&spec->variants, struct m9_vs_variant *, it) {
      if ((*it)->ok)
         m9_vs_program_fini(&(*it)->program);
      FREE(*it);
   }
   util_dynarray_fini(&spec->variants);
   if (spec->native)
      m9_vs_program_fini(&spec->program);
   if (spec->nir)
      ralloc_free(spec->nir);
   FREE(spec);
}

static void
m9_vs_state_fini(struct m9_vs_state *vs)
{
   util_dynarray_foreach(&vs->specs, struct m9_vs_spec *, it)
      m9_vs_spec_free(*it);
   util_dynarray_fini(&vs->specs);
   if (vs->src)
      ralloc_free(vs->src);
   FREE(vs);
}

/* Compile the native preprocessing program from a clone: the shader the
 * state tracker handed over belongs to the draw module, which still runs
 * every vertex.  Nothing reads the result yet -- binding it needs the
 * source records and the varying stream to come from the vertex program
 * (docs/parhelia-m9-native-vertex-backend.md section 8). */
static void
m9_vs_native_init(struct m9_vs_state *vs, struct nir_shader *src)
{
   nir_shader *nir = nir_shader_clone(NULL, src);
   unsigned location = 0;

   if (!nir)
      return;
   if (!nir->info.io_lowered) {
      nir_foreach_shader_in_variable(var, nir)
         var->data.driver_location = location++;
      location = 0;
      nir_foreach_shader_out_variable(var, nir)
         var->data.driver_location = location++;
      NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
               m9_vs_type_size, (nir_lower_io_options)0);
   }
   /* The screen declares no integer support, so Mesa stores an integer
    * uniform as a float; the clone is lowered to match before a loop bound
    * can read as the number it is.  The draw module runs the state
    * tracker's own shader, which gallivm lowers for itself. */
   m9_nir_lower_integers(nir);
   vs->src = nir;
   if (m9_vs_specialise)
      m9_vs_find_uniform_gates(nir, &vs->uniform_gates);
   vs->inline_count = m9_vs_specialise ?
                      MIN2(src->info.num_inlinable_uniforms,
                           MAX_INLINABLE_UNIFORMS) : 0;
}

/* The specialization for one set of values and bound-format GL defaults.
 * Each retained graph owns its output variants; no cache entry is evicted
 * while a queued job can still name its program. */
static struct m9_vs_spec *
m9_vs_spec_for(struct m9_vs_state *vs, const uint32_t *values,
                const struct m9_vs_format_key *format_key,
                const struct m9_vs_gate_key *gate_key)
{
   const size_t bytes = vs->inline_count * sizeof(*values);
   struct m9_vs_spec *spec;
   nir_shader *nir;

   util_dynarray_foreach(&vs->specs, struct m9_vs_spec *, it)
      if (!memcmp((*it)->values, values, bytes) &&
          !memcmp(&(*it)->format_key, format_key, sizeof(*format_key)) &&
          !memcmp(&(*it)->gate_key, gate_key, sizeof(*gate_key)))
         return *it;
   /* A shader specialised on a uniform can be asked for one per value, so
    * the cache is capped and a further set is refused rather than evicting
    * a program a queued job still names. */
   if (util_dynarray_num_elements(&vs->specs, struct m9_vs_spec *) >=
       M9_VS_MAX_SPECS)
      return NULL;
   spec = CALLOC_STRUCT(m9_vs_spec);
   if (!spec)
      return NULL;
   util_dynarray_init(&spec->variants, NULL);
   memcpy(spec->values, values, bytes);
   spec->format_key = *format_key;
   spec->gate_key = *gate_key;
   nir = vs->src ? nir_shader_clone(NULL, vs->src) : NULL;
   if (nir) {
      if (vs->inline_count)
         m9_nir_inline_uniforms(nir, spec->values);
      /* After the loop unrolls, so the copies of its body are fused too:
       * Mesa fuses no multiply and add on its own and the backend folds
       * only the product the hardware's multiply-add can take. */
      NIR_PASS(_, nir, nir_opt_algebraic_late);
      NIR_PASS(_, nir, nir_opt_dce);
      /* Original admission mutates the graph. Save the optional partition
       * candidate before that call; ordinary accepted programs stay exact. */
      nir_shader *sampled = debug_get_bool_option("M9X_NATIVE_VS_TEXTURE_POSITION", false) ?
                            nir_shader_clone(NULL, nir) : NULL;
      spec->native = m9_vs_compile_gate_source(&nir, NULL, &spec->program,
                                         &spec->reason, m9_vs_max_slots,
                                         m9_vs_max_const_pairs, format_key,
                                         &vs->uniform_gates, gate_key);
      if (!spec->native && sampled &&
          m9_vs_compile_sampled_position_source(&sampled, NULL, &spec->program,
                        NULL, m9_vs_max_slots, m9_vs_max_const_pairs)) {
         ralloc_free(nir);
         nir = sampled;
         sampled = NULL;
         spec->reason = NULL;
         spec->native = true;
      }
      ralloc_free(sampled);
   }
   if (spec->native) {
      spec->slots = m9_vs_slot_count(&spec->program) +
                    m9_vs_extra_slot_count(&spec->program) +
                    m9_vs_gather_slot_count(&spec->program) +
                    m9_vs_producer_slot_count(&spec->program);
      spec->consts = m9_vs_const_count(&spec->program);
      util_dynarray_foreach(&spec->program.consts, struct m9_vs_const, k)
         if (k->kind == M9_VS_CONST_LITERAL)
            spec->literals++;
         else if (k->kind == M9_VS_CONST_UNIFORM)
            spec->uniforms++;
   }
   if (spec->native && (spec->reason = m9_vs_program_refusal(&spec->program))) {
      m9_vs_program_fini(&spec->program);
      spec->native = false;
   }
   if (!spec->reason)
      spec->reason = "no native vertex program";
   if (spec->native)
      spec->nir = nir;
   else if (nir)
      ralloc_free(nir);
   if (vs->src && getenv("M9X_DEBUG")) {
      fprintf(stderr, "m9x vs: %s", spec->native ? "native" : spec->reason);
      if (spec->slots)
         fprintf(stderr, " slots=%u consts=%u lit=%u uni=%u", spec->slots,
                 spec->consts, spec->literals, spec->uniforms);
      for (unsigned i = 0; i < vs->inline_count; i++)
         fprintf(stderr, " dw%u=%08x",
                 vs->src->info.inlinable_uniform_dw_offsets[i],
                 spec->values[i]);
      fprintf(stderr, "\n");
   }
   util_dynarray_append_typed(&vs->specs, struct m9_vs_spec *, spec);
   return spec;
}

/* This bridge owns a complete live upload but submits nothing. A typed FAA
 * draw transaction must admit upload.state before binding its GPU program.
 * CPU-only Gallium buffers and caller user pointers are snapshotted while
 * bound; borrowed NIR/maps/CB pointers never escape this call. */
bool
m9_context_faa_build_draw(struct m9_context *ctx,
                          const struct pipe_draw_info *info,
                          const struct pipe_draw_start_count_bias *draw,
                          struct m9_faa_application_upload *upload,
                          const char **reason)
{
   const char *why = "FAA application draw has no retained live shader/state";
   nir_shader *vs = NULL;
   struct m9_vs_outputs linkage = {.count = 1};
   const uint8_t *maps[PIPE_MAX_ATTRIBS] = {0};
   unsigned sizes[PIPE_MAX_ATTRIBS] = {0};
   struct m9_faa_application_draw source = {0};
   if (!ctx || ctx->lost || !info || !draw || !upload || upload->shader.storage ||
       !ctx->vs || !ctx->vs->src || !ctx->fs || !ctx->fs->nir ||
       !ctx->velems || !ctx->rasterizer || !ctx->dsa ||
       ctx->color_count != 1 || !ctx->faa_surface.graph.initialized ||
       !ctx->faa_surface.graph.prepared || !ctx->faa_surface.graph.prepared_id ||
       !ctx->faa_surface.graph.init_point || (ctx->faa_surface.graph.init_point & 15) ||
       ctx->faa_surface.destination != ctx->color ||
       ctx->faa_surface.destination_offset != ctx->color_offset ||
       ctx->faa_surface.width != ctx->framebuffer_width ||
       ctx->faa_surface.height != ctx->framebuffer_height)
      goto fail;
   const struct pipe_rasterizer_state *r = ctx->rasterizer;
   const struct pipe_depth_stencil_alpha_state *z = ctx->dsa;
   if (ctx->query_active || ctx->query_begin || ctx->query_end ||
       z->stencil[0].enabled || z->stencil[1].enabled || z->alpha_enabled ||
       (ctx->blend && (ctx->blend->logicop_enable ||
                      ctx->blend->rt[0].blend_enable ||
                      ctx->blend->rt[0].colormask != PIPE_MASK_RGBA)) ||
       r->rasterizer_discard || r->clip_plane_enable || r->clip_halfz ||
       !r->depth_clip_near || !r->depth_clip_far || r->light_twoside ||
       r->offset_tri || r->poly_stipple_enable ||
       r->fill_front != PIPE_POLYGON_MODE_FILL ||
       r->fill_back != PIPE_POLYGON_MODE_FILL) {
      why = "FAA application raster/depth/blend effects need a typed draw contract";
      goto fail;
   }
   m9_context_sync_ordinary(ctx); /* FAA draws already own their CPU inputs. */
   if (ctx->lost) { why = "FAA application context was lost while flushing"; goto fail; }
   source.info = info;
   source.draw = draw;
   source.fs = ctx->fs->nir;
   source.state = (struct m9_faa_application_state){
      .width = ctx->framebuffer_width, .height = ctx->framebuffer_height,
      .viewport = ctx->viewport, .scissor = ctx->scissor,
      .scissor_enabled = r->scissor, .depth_enabled = z->depth_enabled,
      .depth_write = z->depth_writemask, .depth_func = z->depth_func,
      .cull_face = r->cull_face, .front_ccw = r->front_ccw,
      .flatshade = r->flatshade, .flatshade_first = r->flatshade_first,
   };
   for (unsigned i = 0; i < ctx->num_vertex_buffers; i++) {
      if (i >= PIPE_MAX_ATTRIBS) goto fail;
      const struct pipe_vertex_buffer *b = &ctx->vertex_buffers[i];
      if (b->is_user_buffer) {
         maps[i] = b->buffer.user;
         sizes[i] = UINT_MAX; /* Gallium caller owns the user-pointer extent. */
      } else if (b->buffer.resource) {
         const struct m9_resource *owner = m9_res(b->buffer.resource);
         if (b->buffer.resource->target != PIPE_BUFFER || owner->handle ||
             owner->size > UINT_MAX) {
            why = "FAA input snapshot requires a CPU-only Gallium buffer";
            goto fail;
         }
         maps[i] = owner->map;
         sizes[i] = MIN2(owner->size, b->buffer.resource->width0);
      }
   }
   const struct pipe_constant_buffer *cb = &ctx->vs_constants[0];
   const uint8_t *data = cb->user_buffer;
   if (!data && cb->buffer) {
      const struct m9_resource *owner = m9_res(cb->buffer);
      uint64_t available = MIN2(owner->size, cb->buffer->width0);
      if (cb->buffer->target != PIPE_BUFFER || owner->handle ||
          cb->buffer_offset > available ||
          cb->buffer_size > available - cb->buffer_offset) {
         why = "FAA constant snapshot leaves its CPU-only owner";
         goto fail;
      }
      data = owner->map;
   }
   if (data) data += cb->buffer_offset;
   else if (cb->buffer_size) goto fail;
   if (info->index_size) {
      if (info->has_user_indices) {
         source.index_map = info->index.user;
         source.index_bytes = SIZE_MAX;
      } else if (info->index.resource) {
         const struct m9_resource *owner = m9_res(info->index.resource);
         if (info->index.resource->target != PIPE_BUFFER || owner->handle) {
            why = "FAA index snapshot requires a CPU-only Gallium buffer";
            goto fail;
         }
         source.index_map = owner->map;
         source.index_bytes = MIN2(owner->size, info->index.resource->width0);
      }
   }
   /* Ordinary admission may rewrite its own retained specialized graph.
    * Start from original lowered VS NIR and actual inline values instead. */
   vs = nir_shader_clone(NULL, ctx->vs->src);
   if (!vs) { why = "FAA live vertex clone allocation failed"; goto fail; }
   if (ctx->vs->inline_count) m9_nir_inline_uniforms(vs, ctx->vs_inline);
   linkage.stream[0].location = VARYING_SLOT_COL0;
   source.live = (struct m9_faa_live_gl){
      .vs = vs, .linkage = &linkage, .cb0 = data,
      .cb0_bytes = cb->buffer_size, .elements = ctx->velems->elements,
      .element_count = ctx->velems->count, .buffers = ctx->vertex_buffers,
      .buffer_count = ctx->num_vertex_buffers, .maps = maps, .map_bytes = sizes,
   };
   bool ok = m9_faa_application_build(&source, upload, &why);
   if (ok) {
      upload->graph_id = ctx->faa_surface.graph.prepared_id;
      upload->init_point = ctx->faa_surface.graph.init_point;
      upload->generation = ctx->faa_surface.graph.prepared_generation;
   }
   ralloc_free(vs);
   if (reason) *reason = why;
   return ok;
fail:
   ralloc_free(vs);
   if (reason) *reason = why;
   return false;
}

/* Preserve application clear/depth/raster semantics across the owned draw.
 * The FRAME client independently admits only its implemented native packet
 * profiles. Pending clears are consumed after real Lock/Unlock presentation. */
int
m9_context_faa_submit_draw(struct m9_context *ctx,
                           const struct pipe_draw_info *info,
                           const struct pipe_draw_start_count_bias *draw,
                           const char **reason)
{
   struct m9_faa_application_upload owned = {0};
   const char *why = "FAA application draw has no qualified graph/state";
   int ret = -EOPNOTSUPP;
   uint32_t clear_flags = 0;
   if (!ctx || ctx->lost || !ctx->screen) goto out;
   if (ctx->faa_application_clear.pending) {
      /* The current native full reset has a black color/depth-one profile.
       * Never substitute the old TARGET-only packet, which leaves sampled
       * edge state behind. Separate color/depth calls are merged by clear. */
      if (ctx->faa_application_clear.buffers != (PIPE_CLEAR_COLOR0 | PIPE_CLEAR_DEPTH) ||
          ctx->faa_application_clear.scissored ||
          ctx->faa_application_clear.color_mask != PIPE_MASK_RGBA ||
          ctx->faa_application_clear.depth != 1.0 ||
          ctx->faa_application_clear.color.f[0] != 0.0f ||
          ctx->faa_application_clear.color.f[1] != 0.0f ||
          ctx->faa_application_clear.color.f[2] != 0.0f ||
          ctx->faa_application_clear.color.f[3] != 0.0f) {
         if (getenv("M9X_DEBUG"))
            fprintf(stderr, "m9x: FAA reset request buffers=%#x mask=%#x scissored=%u rgba=%08x/%08x/%08x/%08x depth=%.17g\n",
                    ctx->faa_application_clear.buffers,
                    ctx->faa_application_clear.color_mask,
                    ctx->faa_application_clear.scissored,
                    ctx->faa_application_clear.color.ui[0],
                    ctx->faa_application_clear.color.ui[1],
                    ctx->faa_application_clear.color.ui[2],
                    ctx->faa_application_clear.color.ui[3],
                    ctx->faa_application_clear.depth);
         why = "FAA full reset requires full black color and depth-one clear";
         goto out;
      }
      clear_flags = PARHELIA_M9_FAA_COLOR_CLEAR_TARGET | PARHELIA_M9_FAA_COLOR_CLEAR_DEPTH;
   }
   if (ctx->faa_surface.destination_needs_clear && !clear_flags) {
      why = "FAA destination change requires a full sampled reset";
      goto out;
   }
   if (!m9_context_faa_build_draw(ctx, info, draw, &owned, &why))
      goto out;
   const struct m9_faa_application_state *state = &owned.state;
   if (state->width != 320 || state->height != 240 ||
       (state->depth_enabled && (!state->depth_write || state->depth_func != PIPE_FUNC_LESS)) ||
       (state->cull_face != PIPE_FACE_NONE && state->cull_face != PIPE_FACE_FRONT &&
        state->cull_face != PIPE_FACE_BACK) || state->scissor_enabled ||
       state->viewport.scale[0] != 160 || state->viewport.scale[1] != -120 ||
       state->viewport.translate[0] != 160 || state->viewport.translate[1] != 120 ||
       owned.shader.layout.count != 4 || owned.shader.record_bytes != 32) {
      why = "FAA application depth/cull/scissor/view state is not implemented";
      goto out;
   }
   struct m9_faa_graph *graph = &ctx->faa_surface.graph;
   if (owned.graph_id != graph->prepared_id ||
       owned.generation != graph->prepared_generation ||
       owned.init_point != graph->init_point) {
      why = "FAA application upload no longer names its retained graph";
      ret = -ESTALE;
      goto out;
   }
   if (!m9_faa_application_frame_append(&ctx->faa_frame, &owned,
                                         clear_flags, 0, &why)) {
      ret = -ENOMEM;
      goto out;
   }
   ret = 0;
   why = NULL;
out:
   if (ret && ctx && ctx->faa_frame.count) {
      /* A refused later draw invalidates the whole queued frame. Rendering
       * only its accepted prefix would silently omit application geometry. */
      ctx->lost = true;
      m9_faa_application_frame_reset(&ctx->faa_frame);
   }
   m9_faa_application_fini(&owned);
   if (reason) *reason = why;
   return ret;
}

/* Complete an application frame only at a visibility boundary. The queue
 * does not replace a multi-draw transaction with separately resolved draws:
 * that would lose the shared sampled depth/fragment state between draws. */
int
m9_context_faa_flush_frame(struct m9_context *ctx, const char **reason)
{
   const char *why = NULL;
   int ret = 0;
   if (!ctx) { ret = -EINVAL; why = "FAA frame has no context"; goto out; }
   struct m9_faa_application_frame *frame = &ctx->faa_frame;
   if (!frame->count) goto out;
   if (ctx->lost) { ret = -EIO; why = "FAA frame context was lost"; goto done; }
   struct m9_faa_graph *graph = &ctx->faa_surface.graph;
   const struct m9_faa_application_upload *first = &frame->draws[0];
   if (first->graph_id != graph->prepared_id ||
       first->generation != graph->prepared_generation ||
       first->init_point != graph->init_point ||
       first->state.width != ctx->faa_surface.width ||
       first->state.height != ctx->faa_surface.height) {
      ret = -ESTALE; why = "FAA queued frame no longer names the retained graph";
      goto done;
   }
   ret = m9_faa_surface_color_batch_kernel(ctx->screen, &ctx->faa_surface, frame);
   if (ret) { why = "FAA authenticated frame submission failed"; goto done; }
   ctx->last_point = MAX2(ctx->last_point, graph->color_lock_point);
   ret = m9_context_faa_present_lock(ctx, first->graph_id, first->generation,
                                    graph->color_lock_point);
   if (ret) { why = "FAA frame Lock/Unlock presentation failed"; goto done; }
   ctx->last_point = MAX2(ctx->last_point, graph->color_unlock_point);
   if (frame->clear_flags) {
      memset(&ctx->faa_application_clear, 0, sizeof(ctx->faa_application_clear));
      ctx->faa_surface.destination_needs_clear = false;
   }
done:
   /* CPU snapshots may be freed after the synchronous kernel has copied
    * inputs; GPU owners remain retained by its graph even on failure. */
   if (ret) ctx->lost = true;
   m9_faa_application_frame_reset(frame);
out:
   if (reason) *reason = why;
   return ret;
}

/* A returned COLOR Lock grants a source-qualified F021 row view, not an
 * ordinary mapping of the sampled allocation. Transfer that view verbatim
 * into owned rows, authenticate source Unlock, then transfer those rows
 * through the destination's live surface descriptor. An intervening ordinary
 * aperture mapping would replace the mapper retained by source Unlock.
 * The opt-in draw route rejects unsupported clear/depth/raster state. */
int
m9_context_faa_present_lock(struct m9_context *ctx, uint64_t graph_id,
                            uint32_t generation, uint64_t lock_point)
{
   struct pipe_resource *retained = NULL;
   uint8_t *rows = NULL;
   if (!ctx || ctx->lost || !ctx->screen || !ctx->params_valid ||
       ctx->color_count != 1 || ctx->color_discard || !ctx->color ||
       ctx->faa_surface.destination != ctx->color ||
       ctx->faa_surface.destination_offset != ctx->color_offset ||
       ctx->faa_surface.width != ctx->framebuffer_width ||
       ctx->faa_surface.height != ctx->framebuffer_height)
      return -EINVAL;
   struct m9_faa_surface *surface = &ctx->faa_surface;
   struct m9_faa_graph *graph = &surface->graph;
   if (!graph_id || !lock_point || (lock_point & 15) ||
       !graph->prepared || !graph->initialized ||
       graph->prepared_id != graph_id || graph->prepared_generation != generation ||
       !graph->color_lock_attempted || graph->color_lock_point != lock_point ||
       graph->color_unlock_attempted || graph->color_unlock_point)
      return -ESTALE;
   /* Only this shape/view has an original Lock mapper and pixel proof.
    * The kernel separately checks the exact owner, epoch and retirement. */
   struct m9_faa_primary_layout primary;
   if (surface->width != 320 || surface->height != 240 || surface->format != 0x66 ||
       !m9_faa_primary_layout(surface->width, surface->height, surface->format, &primary) ||
       surface->primary_layout.size != primary.size ||
       surface->primary_layout.row_span != primary.row_span ||
       surface->primary_layout.allocation_height != primary.allocation_height ||
       surface->primary_layout.descriptor != primary.descriptor ||
       !surface->primary.handle || surface->primary.handle == m9_res(ctx->color)->handle ||
       surface->primary.size < primary.size || surface->primary.cpp != 4 ||
       !surface->primary.tiled || surface->primary.domain != M9_BO_VRAM)
      return -EOPNOTSUPP;
   struct m9_resource *destination = m9_res(ctx->color);
   unsigned level = ctx->framebuffer.cbufs[0].texture == ctx->color ?
                    ctx->framebuffer.cbufs[0].level : 0;
   bool texture = m9_texture_renders_level(destination, level);
   uint32_t width = texture ? destination->texture.level[level].width : destination->width;
   uint32_t height = texture ? destination->texture.level[level].height : destination->height;
   uint64_t offset = texture ? destination->texture.level[level].offset : 0;
   struct m9_render_layout target;
   if (!m9_resource_is_color_target(destination, level) ||
       (destination->base.format != PIPE_FORMAT_B8G8R8A8_UNORM &&
        destination->base.format != PIPE_FORMAT_B8G8R8X8_UNORM) ||
       destination->format != M9_COLOR_FORMAT_A8R8G8B8 ||
       !m9_render_surface_layout(width, height, 4, &target) ||
       width < surface->width || height < surface->height ||
       ctx->color_offset != offset || offset > UINT32_MAX || (offset & 255) ||
       offset > destination->size || target.size > destination->size - offset)
      return -EOPNOTSUPP;
   size_t bytes = (size_t)surface->width * surface->height * 4;
   rows = MALLOC(bytes);
   if (!rows) return -ENOMEM;
   pipe_resource_reference(&retained, ctx->color);
   m9_context_sync_ordinary(ctx);
   int ret = ctx->lost ? -EIO : 0;
   const struct m9_surface_desc source_desc = {
      .row_span = primary.row_span, .height = primary.allocation_height,
      .layers = 1, .layout = 1, .format = 0x78, .address_offset = true,
   };
   struct drm_parhelia_m9_surface_access access = {
      .handle = surface->primary.handle, .op = PARHELIA_M9_ACCESS_READ,
      .descriptor0 = m9_surface_desc0(&source_desc),
      .descriptor1 = m9_surface_desc1(&source_desc),
      .surface_width = surface->width, .width = surface->width, .height = surface->height,
      .data_stride = surface->width * 4, .data_ptr = (uintptr_t)rows, .data_size = bytes,
   };
   if (!ret) {
      ctx->window_reads++;
      ctx->window_read_bytes += bytes;
      ret = ioctl(ctx->screen->fd, DRM_IOCTL_PARHELIA_M9_SURFACE_ACCESS, &access) ? -errno : 0;
      m9_blit_window_receipt(&surface->primary, &access, ret);
   }
   if (!ret)
      ret = m9_faa_surface_color_unlock_kernel(ctx->screen, surface);
   if (!ret) {
      const struct m9_surface_desc target_desc = {
         .row_span = target.row_span, .height = m9_surface_even_height(height),
         .layers = 1, .layout = 1, .format = destination->format, .address_offset = true,
      };
      access.handle = destination->handle;
      access.op = PARHELIA_M9_ACCESS_WRITE;
      access.descriptor0 = m9_surface_desc0(&target_desc);
      access.descriptor1 = m9_surface_desc1(&target_desc);
      access.surface_width = width;
      access.flags = texture ? PARHELIA_M9_ACCESS_TEXTURE : 0;
      if (texture) access.surface_width = target.row_span;
      access.offset = offset;
      /* Invalidate dependent snapshots even if a window write is partial. */
      destination->generation++;
      ctx->window_writes++;
      ctx->window_write_bytes += bytes;
      ret = ioctl(ctx->screen->fd, DRM_IOCTL_PARHELIA_M9_SURFACE_ACCESS, &access) ? -errno : 0;
      m9_blit_window_receipt(destination, &access, ret);
   }
   pipe_resource_reference(&retained, NULL);
   FREE(rows);
   return ret;
}

/* Point the shader at the specialisation the context's uniform values ask
 * for, which is what every native decision below is then taken against. */
static void
m9_vs_select_spec(struct m9_context *ctx)
{
   struct m9_vs_state *vs = ctx->vs;

   if (!vs)
      return;
   struct m9_vs_format_key format_key = {0};
   if (vs->src && ctx->velems)
      m9_vs_format_key_init(vs->src->info.inputs_read, ctx->velems->elements,
                             ctx->velems->count, &format_key);
   struct m9_vs_gate_key gate_key = {0};
   const struct pipe_constant_buffer *cb = &ctx->vs_constants[0];
   const uint8_t *data = cb->user_buffer;
   size_t bytes = cb->buffer_size;
   if (!data && cb->buffer) {
      const struct m9_resource *r = m9_res(cb->buffer);
      uint64_t available = MIN2(r->size,cb->buffer->width0);
      if (cb->buffer->target == PIPE_BUFFER && cb->buffer_offset <= available &&
          cb->buffer_size <= available - cb->buffer_offset)
         data = r->map;
   }
   if (data)
      m9_vs_gate_key_init(&vs->uniform_gates, data + cb->buffer_offset,
                            bytes, &gate_key);
   if (vs->cur && !memcmp(vs->cur->values, ctx->vs_inline,
                          vs->inline_count * sizeof(*ctx->vs_inline)) &&
       !memcmp(&vs->cur->format_key, &format_key, sizeof(format_key)) &&
       !memcmp(&vs->cur->gate_key, &gate_key, sizeof(gate_key)))
      return;
   vs->cur = m9_vs_spec_for(vs, ctx->vs_inline, &format_key, &gate_key);
}

static void *
m9_create_vs_state(struct pipe_context *pctx,
                   const struct pipe_shader_state *templ)
{
   struct m9_context *ctx = m9_context(pctx);
   struct m9_vs_state *vs = CALLOC_STRUCT(m9_vs_state);

   if (!vs)
      return NULL;
   util_dynarray_init(&vs->specs, NULL);
   if (templ->type == PIPE_SHADER_IR_NIR && templ->ir.nir)
      m9_vs_native_init(vs, templ->ir.nir);
   vs->serial = ++m9_vs_serial;
   vs->draw = draw_create_vertex_shader(ctx->draw, templ);
   if (!vs->draw) {
      m9_vs_state_fini(vs);
      return NULL;
   }
   return vs;
}

static void
m9_bind_vs_state(struct pipe_context *pctx, void *state)
{
   struct m9_context *ctx = m9_context(pctx);
   struct m9_vs_state *vs = state;

   ctx->vs = vs;
   draw_bind_vertex_shader(ctx->draw, vs ? vs->draw : NULL);
}

static bool
m9_vs_state_owns_program(const struct m9_vs_state *vs,
                         const struct m9_vs_program *program)
{
   if (!program)
      return false;
   util_dynarray_foreach(&vs->specs, struct m9_vs_spec *, spec) {
      if (program == &(*spec)->program)
         return true;
      util_dynarray_foreach(&(*spec)->variants, struct m9_vs_variant *, variant) {
         if (program == &(*variant)->program)
            return true;
      }
   }
   return false;
}

static void
m9_delete_vs_state(struct pipe_context *pctx, void *state)
{
   struct m9_context *ctx = m9_context(pctx);
   struct m9_vs_state *vs = state;

   m9_aggregate_flush(ctx);
   /* Native records bypass draw's flush. Encode and submit their job while
    * its specialization/layout data is still alive, as FS deletion does.
    * Posted C2 images retain their separate BO lifetime. */
   if (m9_vs_state_owns_program(vs, ctx->job_key.vs))
      m9_context_sync(ctx);
   if (ctx->vs == vs)
      ctx->vs = NULL;
   draw_delete_vertex_shader(ctx->draw, vs->draw);
   m9_vs_state_fini(vs);
}

/* True when def is the unmodified four-component primary color input. */
static bool
m9_fs_is_primary_color(nir_def *def, unsigned depth)
{
   nir_instr *instr = nir_def_instr(def);

   if (depth > 8 || def->num_components != 4)
      return false;
   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
      nir_variable *var;

      switch (intr->intrinsic) {
      case nir_intrinsic_load_input:
      case nir_intrinsic_load_interpolated_input:
         return nir_intrinsic_io_semantics(intr).location == VARYING_SLOT_COL0 &&
                !nir_intrinsic_component(intr);
      case nir_intrinsic_load_deref:
         var = nir_intrinsic_get_var(intr, 0);
         return var && var->data.mode == nir_var_shader_in &&
                var->data.location == VARYING_SLOT_COL0;
      default:
         return false;
      }
   }
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);

      if (alu->op == nir_op_mov || alu->op == nir_op_fsat) {
         for (unsigned c = 0; c < 4; c++)
            if (alu->src[0].swizzle[c] != c)
               return false;
         return m9_fs_is_primary_color(alu->src[0].src.ssa, depth + 1);
      }
      if (alu->op == nir_op_vec4) {
         nir_def *src = alu->src[0].src.ssa;

         for (unsigned c = 0; c < 4; c++)
            if (alu->src[c].src.ssa != src || alu->src[c].swizzle[0] != c)
               return false;
         return m9_fs_is_primary_color(src, depth + 1);
      }
   }
   return false;
}

/* The raster pass interpolates vertex color only: accept exactly that. */
static bool
m9_fs_passthrough(nir_shader *nir)
{
   bool written = false;

   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            nir_intrinsic_instr *intr;
            nir_variable *var;
            unsigned location;

            if (instr->type == nir_instr_type_deref ||
                instr->type == nir_instr_type_load_const ||
                instr->type == nir_instr_type_alu)
               continue;
            if (instr->type != nir_instr_type_intrinsic)
               return false;
            intr = nir_instr_as_intrinsic(instr);
            switch (intr->intrinsic) {
            case nir_intrinsic_load_input:
            case nir_intrinsic_load_interpolated_input:
            case nir_intrinsic_load_barycentric_pixel:
            case nir_intrinsic_load_barycentric_centroid:
            case nir_intrinsic_load_barycentric_sample:
            case nir_intrinsic_load_deref:
               break;
            case nir_intrinsic_store_output:
               location = nir_intrinsic_io_semantics(intr).location;
               if ((location != FRAG_RESULT_COLOR &&
                    location != FRAG_RESULT_DATA0) ||
                   !m9_fs_is_primary_color(intr->src[0].ssa, 0))
                  return false;
               written = true;
               break;
            case nir_intrinsic_store_deref:
               var = nir_intrinsic_get_var(intr, 0);
               if (!var || var->data.mode != nir_var_shader_out ||
                   (var->data.location != FRAG_RESULT_COLOR &&
                    var->data.location != FRAG_RESULT_DATA0) ||
                   !m9_fs_is_primary_color(intr->src[1].ssa, 0))
                  return false;
               written = true;
               break;
            default:
               return false;
            }
         }
      }
   }
   return written;
}

static void
m9_fs_native_init(struct m9_context *ctx, struct m9_fs_state *fs)
{
   struct m9_fs_variant opaque = { .blend = { M9_FS_ONE, M9_FS_ZERO } };

   for (unsigned s = 0; s < PIPE_MAX_SAMPLERS; s++)
      for (unsigned c = 0; c < 4; c++)
         opaque.swizzle[s][c] = PIPE_SWIZZLE_X + c;
   util_dynarray_init(&fs->variants, NULL);
   /* Shaders specialised on inlined uniforms compile per value set. */
   fs->native = fs->nir->info.num_inlinable_uniforms ||
                m9_fs_variant(ctx, fs, &opaque, &fs->reason);
}

static void *
m9_create_fs_state(struct pipe_context *pctx,
                   const struct pipe_shader_state *templ)
{
   struct m9_fs_state *fs = CALLOC_STRUCT(m9_fs_state);

   if (!fs)
      return NULL;
   /* u_blitter and the other Gallium helpers build their shaders with ureg,
    * which hands them over as TGSI.  Without this they reached neither the
    * passthrough nor the native backend and every draw using one was
    * dropped, which is what kept copies and blits off the engine. */
   if (templ->type == PIPE_SHADER_IR_TGSI && templ->tokens) {
      fs->nir = tgsi_to_nir(templ->tokens, pctx->screen, false);
      /* The converter leaves IO as variables and derefs; the backend reads
       * the lowered intrinsics the state tracker hands it. */
      if (fs->nir && !fs->nir->info.io_lowered) {
         NIR_PASS(_, fs->nir, nir_lower_vars_to_ssa);
         nir_lower_io_passes(fs->nir, false);
         NIR_PASS(_, fs->nir, nir_remove_dead_variables,
                  nir_var_shader_in | nir_var_shader_out, NULL);
      }
   } else if (templ->type == PIPE_SHADER_IR_NIR)
      fs->nir = templ->ir.nir;
   if (fs->nir) {
      fs->kill_all = m9_fs_kills_all(fs->nir);
      fs->passthrough = m9_fs_passthrough(fs->nir);
      if (!fs->passthrough)
         m9_fs_native_init(m9_context(pctx), fs);
   }
   fs->draw = draw_create_fragment_shader(m9_context(pctx)->draw, templ);
   if (!fs->draw) {
      ralloc_free(fs->nir);
      FREE(fs);
      return NULL;
   }
   return fs;
}

static void
m9_bind_fs_state(struct pipe_context *pctx, void *state)
{
   struct m9_context *ctx = m9_context(pctx);
   struct m9_fs_state *fs = state;

   ctx->fs = fs;
   draw_bind_fragment_shader(ctx->draw, fs ? fs->draw : NULL);
}

/* Outermost fragment shader bind: records the handle the blitter restores. */
static void
m9_bind_fs_handle(struct pipe_context *pctx, void *state)
{
   struct m9_context *ctx = m9_context(pctx);

   ctx->fs_handle = state;
   ctx->bind_fs_wrapped(pctx, state);
}

static void
m9_delete_fs_state(struct pipe_context *pctx, void *state)
{
   struct m9_context *ctx = m9_context(pctx);
   struct m9_fs_state *fs = state;

   m9_aggregate_flush(ctx);
   if (ctx->job_key.fs == fs)
      m9_context_sync(ctx);
   util_dynarray_foreach(&fs->variants, struct m9_fs_variant *, it)
      m9_fs_variant_free(ctx, *it);
   util_dynarray_fini(&fs->variants);
   draw_delete_fragment_shader(ctx->draw, fs->draw);
   ralloc_free(fs->nir);
   FREE(fs);
}

static void *
m9_create_vertex_elements_state(struct pipe_context *pctx, unsigned count,
                                const struct pipe_vertex_element *elements)
{
   struct m9_velems *velems;

   (void)pctx;
   if (count > PIPE_MAX_ATTRIBS)
      return NULL;
   velems = CALLOC_STRUCT(m9_velems);
   if (!velems)
      return NULL;
   velems->count = count;
   memcpy(velems->elements, elements, count * sizeof(*elements));
   return velems;
}

static void
m9_bind_vertex_elements_state(struct pipe_context *pctx, void *state)
{
   struct m9_velems *velems = state;

   m9_context(pctx)->velems = velems;
   draw_set_vertex_elements(m9_context(pctx)->draw,
                            velems ? velems->count : 0,
                            velems ? velems->elements : NULL);
}

static void
m9_set_vertex_buffers(struct pipe_context *pctx, unsigned count,
                      const struct pipe_vertex_buffer *buffers)
{
   struct m9_context *ctx = m9_context(pctx);

   util_set_vertex_buffers_count(ctx->vertex_buffers, &ctx->num_vertex_buffers,
                                 buffers, count);
   draw_set_vertex_buffers(ctx->draw, count, buffers);
}

static void
m9_set_constant_buffer(struct pipe_context *pctx, mesa_shader_stage shader,
                       uint index, const struct pipe_constant_buffer *cb)
{
   if (shader == MESA_SHADER_VERTEX && index < PIPE_MAX_CONSTANT_BUFFERS)
      util_copy_constant_buffer(&m9_context(pctx)->vs_constants[index], cb);
   else if (shader == MESA_SHADER_FRAGMENT && index == 0)
      util_copy_constant_buffer(&m9_context(pctx)->fs_constants, cb);
}

static void
m9_set_inlinable_constants(struct pipe_context *pctx, mesa_shader_stage shader,
                           uint num_values, uint32_t *values)
{
   struct m9_context *ctx = m9_context(pctx);
   const unsigned n = MIN2(num_values, MAX_INLINABLE_UNIFORMS);

   if (shader == MESA_SHADER_FRAGMENT) {
      /* Inlining can remove a fetch or change the raster pass schedule, so
       * it also changes the CPU varying/coordinate record layout returned
       * by m9_render_get_vertex_info.  A backend-only draw_flush keeps the
       * prepared frontend and its emit translation: rebinding the same FS
       * invalidates that layout before the new variant is selected. */
      if (n && memcmp(ctx->fs_inline, values, n * sizeof(*values)))
         draw_bind_fragment_shader(ctx->draw, ctx->fs ? ctx->fs->draw : NULL);
      memcpy(ctx->fs_inline, values, n * sizeof(*values));
   } else if (shader == MESA_SHADER_VERTEX)
      memcpy(ctx->vs_inline, values, n * sizeof(*values));
}

/* The depth and HZ pair for a target of this size, allocated once and kept.
 * Depth testing is refused outright when the target has no attachment of its
 * own, so the content is never read and a reused pair needs no refill; only
 * a fresh one is filled, the way a fresh one always was. */
static bool
m9_target_depth_bind(struct m9_context *ctx, uint32_t w, uint32_t h,
                     uint64_t bytes)
{
   struct m9_target_depth *entry = NULL, **oldest = NULL;
   uint64_t total = 0;

   /* M9X_TARGET_FRESH=1 refuses to reuse a depth and HZ surface, which
    * separates that pair from the record and the colour target. */
   const bool fresh = debug_get_bool_option("M9X_TARGET_FRESH", false);

   util_dynarray_foreach(&ctx->target_depths, struct m9_target_depth *, it) {
      total += (*it)->depth.size + (*it)->aux.size + (*it)->be[0].size +
               (*it)->be[1].size;
      if (!fresh && (*it)->width == w && (*it)->height == h &&
          (*it)->scratch == ctx->scratch_bind)
         entry = *it;
      else if (!oldest || (*it)->used < (*oldest)->used)
         oldest = it;
   }
   if (!entry) {
      /* Release the size used longest ago before adding another, so a
       * context that walks through many sizes does not grow without end. */
      while (oldest && total + bytes + m9_hz_bytes(w, h) >
             M9_TARGET_DEPTH_BUDGET) {
         struct m9_target_depth *evict = *oldest;

         total -= evict->depth.size + evict->aux.size + evict->be[0].size +
                  evict->be[1].size;
         m9_bo_destroy(ctx->screen, &evict->depth);
         m9_bo_destroy(ctx->screen, &evict->aux);
         m9_bo_destroy(ctx->screen, &evict->be[0]);
         m9_bo_destroy(ctx->screen, &evict->be[1]);
         *oldest = *util_dynarray_pop_ptr(&ctx->target_depths,
                                          struct m9_target_depth *);
         FREE(evict);
         oldest = NULL;
         util_dynarray_foreach(&ctx->target_depths,
                               struct m9_target_depth *, it)
            if (!oldest || (*it)->used < (*oldest)->used)
               oldest = it;
      }
      entry = CALLOC_STRUCT(m9_target_depth);
      if (!entry ||
          m9_bo_create(ctx->screen, bytes, PARHELIA_M9_BO_VRAM,
                       M9_RESOURCE_LINEAR_DEPTH, &entry->depth) ||
          m9_bo_create(ctx->screen, m9_hz_bytes(w, h), PARHELIA_M9_BO_VRAM,
                       M9_RESOURCE_LINEAR_DEPTH, &entry->aux) ||
          (m9_be_per_target &&
           m9_bo_create(ctx->screen, M9_BE_BYTES, PARHELIA_M9_BO_VRAM,
                        M9_RESOURCE_SYSTEM_STATE, &entry->be[0]))) {
         if (entry) {
            m9_bo_destroy(ctx->screen, &entry->depth);
            m9_bo_destroy(ctx->screen, &entry->aux);
            m9_bo_destroy(ctx->screen, &entry->be[0]);
            FREE(entry);
         }
         m9_warn_once(ctx, M9_WARN_MEMORY, "cannot allocate target depth");
         return false;
      }
      entry->scratch = ctx->scratch_bind;
      entry->width = entry->depth.width = w;
      entry->height = entry->depth.height = h;
      m9_fill_dwords(&entry->depth, 0xffffffff);
      m9_fill_dwords(&entry->aux, M9_HZ_CLEAR);
      if (entry->be[0].map)
         memset(entry->be[0].map, 0, entry->be[0].size);
      util_dynarray_append_typed(&ctx->target_depths,
                                 struct m9_target_depth *, entry);
   }
   entry->used = ++ctx->target_clock;
   ctx->target_depth = &entry->depth;
   ctx->target_aux = &entry->aux;
   ctx->target_entry = entry;
   ctx->target_be = entry->be[entry->be_slot].handle ?
                    &entry->be[entry->be_slot] : NULL;
   return true;
}

/* Every raster parameter is derived from a colour surface
 * (m9_draw_params_init), so a framebuffer with a depth attachment alone is
 * given one out of the scratch pool.  GL discards colour writes into such a
 * framebuffer, so the job masks them off and nothing reads the surface. */
static struct m9_resource *
m9_depth_only_color(struct m9_context *ctx)
{
   struct pipe_resource *res;

   if (!ctx->framebuffer.zsbuf.texture || !ctx->framebuffer.width ||
       !ctx->framebuffer.height)
      return NULL;
   res = m9_scratch_target(ctx, ctx->framebuffer.width,
                           ctx->framebuffer.height,
                           PIPE_FORMAT_B8G8R8A8_UNORM);
   if (!res)
      m9_warn_once(ctx, M9_WARN_MEMORY, "no colour surface for a depth-only "
                   "framebuffer; draws skipped");
   return res ? m9_res(res) : NULL;
}

/* Colour attachments past the first.  Every target renders through the same
 * clip window and viewport, so they share one extent; each keeps its own
 * descriptor and render format class (raster 89), which is how a target of
 * another packed 32-bit format is bound alongside an A8R8G8B8 one.
 * docs/parhelia-m9-mrt-routing.md */
static bool
m9_update_color_extra(struct m9_context *ctx, uint32_t w, uint32_t h)
{
   unsigned bound = MIN2(ctx->framebuffer.nr_cbufs, M9_DRAW_MAX_COLOR);

   for (unsigned s = 1; s < bound; s++) {
      const struct pipe_surface *cbuf = &ctx->framebuffer.cbufs[s];
      struct m9_resource *res = cbuf->texture ? m9_res(cbuf->texture) : NULL;
      uint32_t desc;

      /* A gap in the attachment list would leave a bound target no surface
       * answers for, so the run stops at the first one. */
      if (!res || cbuf->level || res->width != w || res->height != h ||
          !(res->tiled || m9_texture_renders_level(res, 0)) ||
          m9_color_surface_desc(w, h, res->format, &desc)) {
         m9_warn_once(ctx, M9_WARN_TARGET, "a colour attachment past the "
                      "first is not a tiled render surface of the target "
                      "size; draws skipped");
         return false;
      }
      pipe_resource_reference(&ctx->color_extra[s - 1], cbuf->texture);
      ctx->color_extra_desc[s - 1] = desc;
      ctx->color_format[s] = res->format;
      ctx->color_count = s + 1;
   }
   if (ctx->framebuffer.nr_cbufs > M9_DRAW_MAX_COLOR) {
      m9_warn_once(ctx, M9_WARN_TARGET, "more colour attachments than the "
                   "engine binds; draws skipped");
      return false;
   }
   return true;
}

/* Copy the proxy's content into the level it stands for. */
static void
m9_proxy_writeback(struct m9_context *ctx)
{
   struct pipe_resource *scratch = ctx->proxy.scratch;
   struct pipe_box box;

   if (!ctx->proxy.bound || ctx->proxy.busy)
      return;
   ctx->proxy.busy = true;
   u_box_2d(0, 0, scratch->width0, scratch->height0, &box);
   util_resource_copy_region(&ctx->base, ctx->proxy.tex, ctx->proxy.level, 0,
                             0, ctx->proxy.layer, scratch, 0, &box);
   ctx->proxy.generation = m9_res(ctx->proxy.tex)->generation;
   ctx->proxy.busy = false;
}

/* Copy the level into the proxy again, after something else wrote it. */
static void
m9_proxy_reseed(struct m9_context *ctx)
{
   struct pipe_resource *scratch = ctx->proxy.scratch;
   struct pipe_box box;

   if (!ctx->proxy.bound || ctx->proxy.busy)
      return;
   ctx->proxy.busy = true;
   u_box_3d(0, 0, ctx->proxy.layer, scratch->width0, scratch->height0, 1,
            &box);
   util_resource_copy_region(&ctx->base, scratch, 0, 0, 0, 0, ctx->proxy.tex,
                             ctx->proxy.level, &box);
   ctx->proxy.generation = m9_res(ctx->proxy.tex)->generation;
   ctx->proxy.busy = false;
}

static void
m9_proxy_release(struct m9_context *ctx)
{
   m9_proxy_writeback(ctx);
   ctx->proxy.bound = false;
}

/* The tiled surface standing in for a colour level the engine cannot render
 * in place.  Its content stays authoritative while the texture's generation
 * is the one it was last matched to, so rebinding it costs no seed. */
static struct m9_resource *
m9_proxy_bind(struct m9_context *ctx, struct pipe_resource *tex,
              unsigned level, unsigned layer)
{
   struct m9_format_info info;
   uint32_t w = u_minify(tex->width0, level), h = u_minify(tex->height0, level);
   struct pipe_resource *scratch = ctx->proxy.scratch;
   bool same;

   if (m9_res(tex)->kind != M9_RESOURCE_LINEAR_TEXTURE ||
       m9_res(tex)->texture.block_bytes || level > tex->last_level ||
       !m9_format_info(tex->format, &info) || !(info.usage & M9_FORMAT_COLOR))
      return NULL;
   if (!scratch || scratch->width0 != w || scratch->height0 != h ||
       scratch->format != tex->format) {
      struct pipe_resource templ = {
         .target = PIPE_TEXTURE_2D, .format = tex->format,
         .width0 = w, .height0 = h, .depth0 = 1, .array_size = 1,
         .usage = PIPE_USAGE_DEFAULT,
         .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW,
      };

      pipe_resource_reference(&ctx->proxy.scratch, NULL);
      pipe_resource_reference(&ctx->proxy.tex, NULL);
      scratch = ctx->screen->base.resource_create(&ctx->screen->base, &templ);
      if (!scratch)
         return NULL;
      ctx->proxy.scratch = scratch;
   }
   same = ctx->proxy.tex == tex && ctx->proxy.level == level &&
          ctx->proxy.layer == layer &&
          ctx->proxy.generation == m9_res(tex)->generation;
   pipe_resource_reference(&ctx->proxy.tex, tex);
   ctx->proxy.level = level;
   ctx->proxy.layer = layer;
   ctx->proxy.bound = true;
   if (!same)
      m9_proxy_reseed(ctx);
   return m9_res(scratch);
}

static void
m9_update_target(struct m9_context *ctx)
{
   const struct pipe_surface *cbuf = &ctx->framebuffer.cbufs[0];
   struct m9_resource *color = ctx->framebuffer.nr_cbufs && cbuf->texture ?
                               m9_res(cbuf->texture) : NULL;
   struct m9_render_layout layout;
   uint64_t t_target = m9_prof_begin();
   uint32_t w, h;
   unsigned level;

   m9_proxy_release(ctx);
   ctx->params_valid = false;
   ctx->color_discard = false;
   pipe_resource_reference(&ctx->color, NULL);
   for (unsigned s = 0; s < ARRAY_SIZE(ctx->color_extra); s++)
      pipe_resource_reference(&ctx->color_extra[s], NULL);
   memset(ctx->color_class, 0, sizeof(ctx->color_class));
   memset(ctx->color_format, 0, sizeof(ctx->color_format));
   ctx->color_count = 0;
   ctx->color_offset = 0;
   if (!color) {
      color = m9_depth_only_color(ctx);
      if (!color)
         return;
      ctx->color_discard = true;
   }
   /* Every level of a mipmapped image outside the packed tail is laid out as
    * a render surface of its own extent at its chain offset, so it is a
    * target as it stands (m9_texture_renders_level); level 0 is additionally
    * the whole of a plain tiled surface. */
   level = ctx->color_discard ? 0 : cbuf->level;
   if (!ctx->color_discard &&
       (cbuf->first_layer ||
        (level ? !(m9_level_targets &&
                   m9_texture_renders_level(color, level)) :
                 !(color->tiled || m9_texture_renders_level(color, 0))))) {
      struct m9_resource *proxy = m9_proxy_bind(ctx, cbuf->texture, level,
                                                cbuf->first_layer);

      if (proxy) {
         color = proxy;
         level = 0;
      }
   }
   if (level && !(m9_level_targets && m9_texture_renders_level(color, level))) {
      m9_warn_once(ctx, M9_WARN_TARGET, "mip level is not a render surface; "
                   "draws skipped");
      return;
   }
   w = level ? color->texture.level[level].width : color->width;
   h = level ? color->texture.level[level].height : color->height;
   if (!(level || color->tiled || m9_texture_renders_level(color, 0)) ||
       m9_context_set_framebuffer(ctx, color, NULL, level, w, h) ||
       m9_draw_params_init(&ctx->params, w, h, color->format) ||
       !m9_render_surface_layout(w, h, 4, &layout)) {
      m9_warn_once(ctx, M9_WARN_TARGET, "render target is not a tiled "
                   "A8R8G8B8 surface; draws skipped");
      return;
   }
   /* M9X_CLIP_ROUND=1 clips to the allocation's row span and height instead
    * of the declared extent, which separates the clip window from the
    * viewport constants a target's shape also sets.  The viewport bounds
    * the geometry, so the image a draw produces is unchanged. */
   if (debug_get_bool_option("M9X_CLIP_ROUND", false))
      ctx->params.clip_max = layout.row_span |
                             layout.allocation_height << 16;
   /* Depth-less targets still bind a depth and HZ BO of their size. */
   if (!m9_depth_surface_layout(w, h, &layout) ||
       !m9_target_depth_bind(ctx, w, h, layout.size))
      return;
   ctx->color_count = 1;
   ctx->color_format[0] = color->format;
   if (!m9_update_color_extra(ctx, w, h))
      return;
   if (ctx->screen->faa.prepare && !ctx->color_discard &&
       ctx->color_count == 1) {
      struct m9_resource *depth = ctx->framebuffer.zsbuf.texture ?
         m9_res(ctx->framebuffer.zsbuf.texture) : NULL;

      if (m9_faa_surface_prepare(ctx->screen, color, depth,
                                 ctx->color_offset, w, h, &ctx->faa_surface))
         m9_warn_once(ctx, M9_WARN_MEMORY,
                      "FAA surface preparation failed; ordinary target retained");
      else if (m9_faa_surface_prepare_kernel(ctx->screen, &ctx->faa_surface))
         m9_warn_once(ctx, M9_WARN_MEMORY,
                      "FAA kernel owner preparation unavailable; ordinary target retained");
      else if (ctx->screen->faa.init_graph &&
               m9_faa_surface_init_kernel(ctx->screen, &ctx->faa_surface))
         m9_warn_once(ctx, M9_WARN_MEMORY,
                      "FAA kernel graph initialization unavailable; ordinary target retained");
   } else if (!ctx->faa_surface.graph.initialized) {
      m9_faa_surface_destroy(ctx->screen, &ctx->faa_surface);
   }
   ctx->params_valid = true;
   m9_prof_end(M9_PROF_TARGET, t_target);
}

static void
m9_set_framebuffer_state(struct pipe_context *pctx,
                         const struct pipe_framebuffer_state *fb)
{
   struct m9_context *ctx = m9_context(pctx);

   m9_context_sync(ctx);
   util_copy_framebuffer_state(&ctx->framebuffer, fb);
   draw_set_zs_format(ctx->draw, fb->zsbuf.texture ? fb->zsbuf.format :
                                                     PIPE_FORMAT_NONE);
   m9_update_target(ctx);
}

static void
m9_set_viewport_states(struct pipe_context *pctx, unsigned start,
                       unsigned count, const struct pipe_viewport_state *vp)
{
   if (!start && count)
      m9_context(pctx)->viewport = vp[0];
   draw_set_viewport_states(m9_context(pctx)->draw, start, count, vp);
}

static void
m9_set_scissor_states(struct pipe_context *pctx, unsigned start,
                      unsigned count, const struct pipe_scissor_state *s)
{
   if (!start && count)
      m9_context(pctx)->scissor = s[0];
}

static void
m9_set_clip_state(struct pipe_context *pctx, const struct pipe_clip_state *clip)
{
   draw_set_clip_state(m9_context(pctx)->draw, clip);
}

static void
m9_set_blend_color(struct pipe_context *pctx,
                   const struct pipe_blend_color *color)
{
   /* Resolved into the fragment constants at the next draw; no recompile. */
   m9_context(pctx)->blend_color = *color;
}

static void
m9_set_stencil_ref(struct pipe_context *pctx, const struct pipe_stencil_ref ref)
{
   m9_context(pctx)->stencil_ref = ref;
}

static void
m9_set_sample_mask(struct pipe_context *pctx, unsigned mask)
{
   (void)pctx;
   (void)mask;
}

static void
m9_set_polygon_stipple(struct pipe_context *pctx,
                       const struct pipe_poly_stipple *stipple)
{
   (void)pctx;
   (void)stipple;
}

/* Context */

/*
 * A frame that skips most of its draws renders nothing while the application
 * burns CPU for it, and one line at the first skip does not say that.  Report
 * the share of the frame that was dropped, and its dominant reason, on the
 * first bad frame and then at a widening interval, so the signal is
 * proportional to the damage without becoming the damage itself.
 */
static void
m9_frame_skip_report(struct m9_context *ctx)
{
   unsigned seen = ctx->submitted_draws - ctx->frame_draws_base;
   unsigned skipped = ctx->skipped_draws - ctx->frame_skipped_base;
   const char *worst = NULL;
   uint32_t worst_draws = 0;
   unsigned run;

   ctx->frame_draws_base = ctx->submitted_draws;
   ctx->frame_skipped_base = ctx->skipped_draws;
   if (seen < 8 || skipped * 4 < seen * 3) {
      ctx->skipped_frames = 0;
      return;
   }
   run = ++ctx->skipped_frames;
   if (run & (run - 1))
      return;
   for (unsigned i = 0; i < ctx->skip_reasons; i++)
      if (ctx->skip_reason[i].draws > worst_draws) {
         worst_draws = ctx->skip_reason[i].draws;
         worst = ctx->skip_reason[i].reason;
      }
   fprintf(stderr, "m9x: frame %u skipped %u of its %u draws (%u frames in a "
           "row); most often: %s\n", ctx->flush_count, skipped, seen,
           run, worst ? worst : "unrecorded");
}

/* Submits the pending draws, so that a record emitted after this call runs
 * behind them; occlusion queries bracket their range with one. */
int
m9_context_flush_job(struct pipe_context *pctx)
{
   struct m9_context *ctx = m9_context(pctx);

   m9_context_sync(ctx);
   return ctx->lost ? -EIO : 0;
}

static void
m9_context_flush(struct pipe_context *pctx,
                 struct pipe_fence_handle **out_fence, unsigned flags)
{
   struct m9_context *ctx = m9_context(pctx);
   struct pipe_fence_handle *fence = NULL;

   (void)flags;
   m9_prof_frame(ctx);
   m9_frame_skip_report(ctx);
   if (ctx->debug && ++ctx->flush_count % 300 == 0)
      fprintf(stderr, "m9x: after %u frames job splits: state %u uniforms %u "
              "vertices %u stream %u | target %u program %u depth %u mask %u "
              "clip %u\n", ctx->flush_count, ctx->split_state,
              ctx->split_uniforms, ctx->split_vertices, ctx->split_stream,
              ctx->split_target, ctx->split_program, ctx->split_depth,
              ctx->split_mask, ctx->split_clip);
   ctx->frame_jobs = 0;
   m9_context_sync(ctx);
   if (m9_record_dump && ctx->record_written && ctx->screen->record.map) {
      const struct m9_resource *rec = &ctx->screen->record;
      FILE *f = fopen(m9_record_dump, "wb");

      if (f) {
         fwrite(rec->map, 1, rec->size, f);
         fclose(f);
      }
      fprintf(stderr, "m9x: record %u bytes stride %u target %ux%u -> %s\n",
              (unsigned)rec->size, ctx->screen->record_dwords,
              ctx->params.width, ctx->params.height, m9_record_dump);
      ctx->record_written = false;
   }
   if (m9_dump_native_inputs_enable && m9_vs_record_dump &&
       !ctx->lost && ctx->vs_record_vertices) {
      /* An unsuccessful new retirement must not inherit an older job link. */
      if (!m9_clear_native_retirement(m9_vs_record_dump))
         fprintf(stderr, "m9x: could not remove prior native retirement metadata\n");
   }
   if (m9_vs_record_dump && !ctx->lost && ctx->vs_record_vertices &&
       m9_wait_for_point(ctx->screen, ctx->last_point)) {
      fprintf(stderr, "m9x: native packed record completion failed; context lost\n");
      ctx->lost = true;
      ctx->vs_record_vertices = 0;
   }
   if (m9_vs_record_dump && !ctx->lost && ctx->vs_record_vertices &&
       ctx->transient.map) {
      uint32_t fields = ctx->vs_record_fields;
      uint64_t offset = (uint64_t)ctx->vs_record_view * m9_transient_view_bytes();
      uint64_t bytes = m9_vs_record_bytes(ctx->vs_record_vertices, fields);
      if (ctx->vs_record_vertices <= m9_draw_max_source_vertices() &&
          m9_vs_record_fits(ctx->vs_record_vertices, fields, ctx->vs_record_view,
                            m9_transient_view_bytes(), ctx->transient.size)) {
         /* sync submitted pending work and the explicit wait retired the
          * last writer of this live arena. This
          * opt-in dump only reads GPU output; no native vertex result is
          * filled or transformed by the CPU. */
         uint32_t header[M9_VS_RECORD_HEADER_DWORDS] = {
            M9_VS_RECORD_MAGIC, fields, ctx->vs_record_vertices,
            ctx->vs_record_view, M9_VS_RECORD_VERTICES,
            M9_VS_RECORD_BANK_BYTES, (uint32_t)bytes, (uint32_t)(bytes >> 32)
         };
         FILE *f = fopen(m9_vs_record_dump, "wb");
         if (f) {
            bool written = fwrite(header, sizeof(header), 1, f) == 1 &&
               fwrite((const uint8_t *)ctx->transient.map + offset, bytes, 1, f) == 1;
            if (fclose(f)) written = false;
            if (written && m9_dump_native_inputs_enable &&
                !m9_dump_native_retirement(ctx, m9_vs_record_dump, bytes))
               fprintf(stderr, "m9x: native retired record metadata publication failed\n");
            if (written)
               fprintf(stderr, "m9x: native packed record fields %u vertices %u "
                       "bytes %" PRIu64 " -> %s\n", fields, ctx->vs_record_vertices,
                       bytes, m9_vs_record_dump);
         }
      }
      ctx->vs_record_vertices = 0;
   }
   if (out_fence) {
      if (!ctx->lost)
         fence = m9_fence_create(ctx->last_point, !ctx->last_point);
      pctx->screen->fence_reference(pctx->screen, out_fence, fence);
      if (fence)
         pctx->screen->fence_reference(pctx->screen, &fence, NULL);
   }
}

static int
m9_context_init_fixed(struct m9_context *ctx)
{
   struct m9_screen *screen = ctx->screen;
   uint64_t db_bytes = MAX2((screen->fragment_db_size + 0xfff) & ~0xfffull,
                            M9_FRAGMENT_DB_MIN_BYTES);
   int ret;

   ret = m9_bo_create(screen, M9_SYSTEM_BYTES, 0, M9_RESOURCE_SYSTEM_STATE,
                      &ctx->system);
   /* The second set of the two arenas that are sized once rather than
    * grown per job; source and index are reserved on demand. */
   if (!ret)
      ret = m9_bo_create(screen, M9_SYSTEM_BYTES, 0, M9_RESOURCE_SYSTEM_STATE,
                         &ctx->system_alt);
   if (!ret)
      ret = m9_bo_create(screen, M9_PROGRAM_BYTES, PARHELIA_M9_BO_VRAM,
                         M9_RESOURCE_SYSTEM_STATE, &ctx->program);
   if (!ret)
      ret = m9_bo_create(screen, db_bytes, PARHELIA_M9_BO_VRAM,
                         M9_RESOURCE_SYSTEM_STATE, &ctx->fragment_db);
   if (!ret)
      ret = m9_bo_create(screen,
                         M9_TRANSIENT_VIEWS * m9_transient_view_bytes(),
                         PARHELIA_M9_BO_VRAM, M9_RESOURCE_SYSTEM_STATE,
                         &ctx->transient);
   if (!ret)
      ret = m9_bo_create(screen, 4096, 0, M9_RESOURCE_LINEAR_BUFFER,
                         &ctx->cpu_streams);
   if (!ret)
      ret = m9_bo_create(screen, 4096, 0, M9_RESOURCE_LINEAR_BUFFER,
                         &ctx->cpu_streams_alt);
   for (unsigned i = 0; !ret && screen->queued_submit &&
                        i < ARRAY_SIZE(ctx->queue_arena); i++) {
      struct m9_job_arena *bank = &ctx->queue_arena[i];

      ret = m9_bo_create(screen, M9_SYSTEM_BYTES, 0, M9_RESOURCE_SYSTEM_STATE,
                         &bank->system);
      if (!ret)
         ret = m9_bo_create(screen, 4096, 0, M9_RESOURCE_LINEAR_BUFFER,
                            &bank->cpu_streams);
   }
   if (!ret)
      ret = m9_bo_create(screen, M9_BE_BYTES, PARHELIA_M9_BO_VRAM,
                         M9_RESOURCE_SYSTEM_STATE, &ctx->be);
   if (!ret)
      ret = m9_bo_create(screen, 4096, 0, M9_RESOURCE_LINEAR_BUFFER,
                         &ctx->source);
   if (!ret)
      ret = m9_bo_create(screen, M9_TEXTURE_TAIL_BYTES, PARHELIA_M9_BO_VRAM,
                         M9_RESOURCE_LINEAR_TEXTURE, &ctx->placeholder_texture);
   if (ret)
      return ret;
   /* Opaque black: an incomplete GL texture samples (0, 0, 0, 1). */
   m9_fill_dwords(&ctx->placeholder_texture, 0xff000000u);
   memset(ctx->program.map, 0, ctx->program.size);
   memcpy(ctx->program.map, m9_passthrough_c2_program,
          sizeof(m9_passthrough_c2_program));
   memset(ctx->fragment_db.map, 0, ctx->fragment_db.size);
   memcpy(ctx->fragment_db.map, screen->fragment_db, screen->fragment_db_size);
   memset(ctx->be.map, 0, ctx->be.size);
   return 0;
}

static void
m9_context_destroy(struct pipe_context *pctx)
{
   struct m9_context *ctx = m9_context(pctx);
   struct m9_screen *screen = ctx->screen;

   /* M9X_SKIP_REPORT=1 names what a context dropped without the whole job
    * trace M9X_DEBUG carries, which is the only way to read the reasons out
    * of a benchmark run. */
   if (ctx->debug || debug_get_bool_option("M9X_SKIP_REPORT", false))
      m9_skip_report(ctx);
   m9_vs_texture_fini(pctx);
   if (ctx->blitter) {
      util_blitter_destroy(ctx->blitter);
      ctx->blitter = NULL;
   }
   if (ctx->draw) {
      m9_context_sync(ctx);
      m9_proxy_release(ctx);
      draw_destroy(ctx->draw);
      ctx->draw = NULL;
   }
   if (screen->queued_submit)
      m9_wait_for_point(screen, ctx->last_point);
   m9_faa_store_report(screen);
   if (ctx->base.stream_uploader)
      u_upload_destroy(ctx->base.stream_uploader);
   m9_weld_cache_fini(ctx);
   util_unreference_framebuffer_state(&ctx->framebuffer);
   util_set_vertex_buffers_count(ctx->vertex_buffers, &ctx->num_vertex_buffers,
                                 NULL, 0);
   for (unsigned i = 0; i < PIPE_MAX_CONSTANT_BUFFERS; i++)
      pipe_resource_reference(&ctx->vs_constants[i].buffer, NULL);
   pipe_resource_reference(&ctx->fs_constants.buffer, NULL);
   for (unsigned i = 0; i < PIPE_MAX_SAMPLERS; i++)
      pipe_sampler_view_reference(&ctx->fs_views[i], NULL);
   pipe_resource_reference(&ctx->proxy.tex, NULL);
   pipe_resource_reference(&ctx->proxy.scratch, NULL);
   m9_faa_application_frame_fini(&ctx->faa_frame);
   m9_faa_surface_destroy(ctx->screen, &ctx->faa_surface);
   m9_context_state_cleanup(ctx);
   m9_batch_fini(&ctx->batch);
   if (ctx->aggregate) { m9_batch_fini(ctx->aggregate); FREE(ctx->aggregate); }
   util_dynarray_fini(&ctx->aggregate_resources);
   m9_command_bo_destroy(screen, &ctx->command);
   m9_bo_destroy(screen, &ctx->system);
   m9_bo_destroy(screen, &ctx->program);
   m9_bo_destroy(screen, &ctx->fragment_db);
   m9_bo_destroy(screen, &ctx->transient);
   m9_bo_destroy(screen, &ctx->cpu_streams);
   m9_bo_destroy(screen, &ctx->be);
   m9_bo_destroy(screen, &ctx->source);
   m9_bo_destroy(screen, &ctx->index);
   m9_bo_destroy(screen, &ctx->system_alt);
   m9_bo_destroy(screen, &ctx->cpu_streams_alt);
   m9_bo_destroy(screen, &ctx->source_alt);
   m9_bo_destroy(screen, &ctx->index_alt);
   for (unsigned i = 0; i < ARRAY_SIZE(ctx->queue_arena); i++) {
      struct m9_job_arena *bank = &ctx->queue_arena[i];

      m9_bo_destroy(screen, &bank->source);
      m9_bo_destroy(screen, &bank->system);
      m9_bo_destroy(screen, &bank->index);
      m9_bo_destroy(screen, &bank->cpu_streams);
   }
   util_dynarray_foreach(&ctx->target_depths, struct m9_target_depth *, it) {
      m9_bo_destroy(screen, &(*it)->depth);
      m9_bo_destroy(screen, &(*it)->aux);
      m9_bo_destroy(screen, &(*it)->be[0]);
      m9_bo_destroy(screen, &(*it)->be[1]);
      FREE(*it);
   }
   util_dynarray_fini(&ctx->target_depths);
   ctx->target_depth = ctx->target_aux = NULL;
   ctx->target_be = NULL;
   ctx->target_entry = NULL;
   util_dynarray_foreach(&ctx->scratch_targets, struct m9_scratch_target *,
                         it) {
      pipe_resource_reference(&(*it)->res, NULL);
      FREE(*it);
   }
   util_dynarray_fini(&ctx->scratch_targets);
   pipe_resource_reference(&ctx->deferred.dst, NULL);
   m9_job_state_fini(&ctx->deferred.state);
   m9_bo_destroy(screen, &ctx->placeholder_texture);
   m9_bo_destroy(screen, &ctx->vs_c2);
   util_dynarray_foreach(&ctx->vs_c2_images, struct m9_native_image, entry)
      m9_bo_destroy(screen, &entry->image);
   util_dynarray_fini(&ctx->vs_c2_images);
   if (ctx->debug)
      fprintf(stderr, "m9x: job splits: state %u uniforms %u vertices %u "
              "stream %u\n", ctx->split_state, ctx->split_uniforms,
              ctx->split_vertices, ctx->split_stream);
   if (ctx->debug || m9_native_vs) {
      fprintf(stderr, "m9x: native vertex draws %u, draw module %u\n",
              ctx->native_vs_draws, ctx->native_vs_fallbacks);
      fprintf(stderr, "m9x: native vertex scope user %u module %u blitter %u module %u\n",
              ctx->native_vs_draws - ctx->native_vs_blitter_draws,
              ctx->native_vs_fallbacks - ctx->native_vs_blitter_fallbacks,
              ctx->native_vs_blitter_draws, ctx->native_vs_blitter_fallbacks);
      util_dynarray_foreach(&ctx->native_vs_reasons,
                            struct m9_native_vs_reason, it)
         fprintf(stderr, "m9x:   draw module %8u: %s\n", it->count,
                 it->reason);
   }
   util_dynarray_fini(&ctx->native_vs_reasons);
   util_dynarray_foreach(&ctx->job_textures, struct pipe_resource *, held)
      pipe_resource_reference(held, NULL);
   util_dynarray_fini(&ctx->job_textures);
   util_dynarray_fini(&ctx->records);
   util_dynarray_fini(&ctx->indices);
   util_dynarray_fini(&ctx->draws);
   util_dynarray_fini(&ctx->extra);
   m9_retired_variants_free(ctx);
   util_dynarray_fini(&ctx->retired_fs_variants);
   util_dynarray_fini(&ctx->key_consts);
   util_dynarray_fini(&ctx->job_consts);
   util_dynarray_fini(&ctx->key_vs_consts);
   util_dynarray_fini(&ctx->job_vs_consts);
   util_dynarray_fini(&ctx->job_vs_runs);
   util_dynarray_fini(&ctx->job_vs_values);
   FREE(ctx);
}

struct pipe_context *
m9_context_create(struct pipe_screen *pscreen, void *priv, unsigned flags)
{
   struct m9_screen *screen = (struct m9_screen *)pscreen;
   struct m9_vbuf_render *render;
   struct draw_stage *stage;
   struct m9_context *ctx;

   if (flags & PIPE_CONTEXT_COMPUTE_ONLY)
      return NULL;
   if (!screen->execute_submit || !screen->fragment_db) {
      fprintf(stderr, "m9x: rendering needs kernel user submit "
              "(m9_user_submit=1) and firmware m9x/rsfragptrs-normal.bin\n");
      return NULL;
   }
   ctx = CALLOC_STRUCT(m9_context);
   if (!ctx)
      return NULL;
   m9_context_state_init(ctx, screen);
   ctx->base.priv = priv;
   ctx->debug = debug_get_bool_option("M9X_DEBUG", false);
   m9_profile_period = debug_get_num_option("M9X_PROFILE", 0);
   m9_job_draws = debug_get_num_option("M9X_JOB_DRAWS", 0);
   m9_draw_set_job_vertices(debug_get_num_option("M9X_JOB_VERTICES",
                                                 M9_JOB_VERTICES_DEFAULT));
   m9_draw_set_vs_record(debug_get_num_option("M9X_VS_RECORD", 1));
   m9_indexed_draws = debug_get_bool_option("M9X_INDEXED", true);
   m9_weld_enable = debug_get_bool_option("M9X_WELD", true);
   m9_native_vs = debug_get_bool_option("M9X_NATIVE_VS", true);
   m9_native_fallback_dump = debug_get_option("M9X_NATIVE_FALLBACK_DUMP", NULL);
   m9_vs_max_slots = debug_get_num_option("M9X_VS_MAX_SLOTS", 188);
   m9_vs_max_const_pairs = debug_get_num_option("M9X_VS_MAX_CONST_PAIRS",
                                                M9_VS_MAX_CONST_PAIRS);
   m9_vs_const_pad = debug_get_num_option("M9X_VS_CONST_PAD", 0);
   m9_vs_uniform_batch = debug_get_bool_option("M9X_VS_UNIFORM_BATCH", true);
   m9_vs_specialise = debug_get_bool_option("M9X_VS_SPECIALISE", true);
   m9_native_vs_varyings = MIN2((unsigned)
      debug_get_num_option("M9X_VS_VARYINGS", M9_VS_FUSED_VARYINGS),
      M9_VS_MAX_VARYINGS);
   m9_native_vs_texture = debug_get_bool_option("M9X_VS_TEXTURE", true);
   m9_native_vs_coord_pair = debug_get_bool_option("M9X_VS_COORD_PAIR", true);
   m9_no_cull = debug_get_bool_option("M9X_NO_CULL", false);
   m9_coord_share = debug_get_bool_option("M9X_COORD_SHARE", true);
   m9_coord_pack = debug_get_bool_option("M9X_COORD_PACK", true);
   m9_native_vs_split = debug_get_bool_option("M9X_VS_SPLIT", false);

   m9_no_clear = debug_get_bool_option("M9X_NO_CLEAR", false);
   m9_mrt_mask_all = debug_get_bool_option("M9X_MRT_MASK_ALL", false);
   m9_clear_repeat = debug_get_num_option("M9X_CLEAR_REPEAT", 0);
   ctx->dump_path = debug_get_option("M9X_DUMP", NULL);
   m9_debug_textures = debug_get_bool_option("M9X_DEBUG_TEXTURES", false);
   m9_blit_window_trace =
      debug_get_bool_option("M9X_BLIT_WINDOW_TRACE", false);
   m9_texture_dump = debug_get_option("M9X_TEXTURE_DUMP", NULL);
   m9_texture_addrcode = debug_get_bool_option("M9X_TEXTURE_ADDRCODE", false);
   m9_debug_ctx = ctx;
   m9_pass_loop = debug_get_bool_option("M9X_PASS_LOOP", false);
   m9_split_stage = debug_get_num_option("M9X_SPLIT_STAGE", 3);
   m9_split_preprocess = debug_get_bool_option("M9X_SPLIT_PREPROCESS", false);
   m9_split_depth = debug_get_num_option("M9X_SPLIT_DEPTH", 1);
   m9_split_submit = debug_get_bool_option("M9X_SPLIT_SUBMIT", true);
   m9_split_passes = debug_get_num_option("M9X_SPLIT_PASSES", 0);
   m9_split_skip_above = debug_get_num_option("M9X_SPLIT_SKIP_ABOVE", 0);
   m9_split_skip_below = debug_get_num_option("M9X_SPLIT_SKIP_BELOW", 0);
   m9_split_trace = debug_get_num_option("M9X_SPLIT_TRACE", 0);
   m9_split_clip = debug_get_num_option("M9X_SPLIT_CLIP", 0);
   m9_split_partial = debug_get_bool_option("M9X_SPLIT_PARTIAL", false);
   m9_split_scratch = debug_get_bool_option("M9X_SPLIT_SCRATCH", true);
   m9_split_seed = debug_get_bool_option("M9X_SPLIT_SEED", true);
   m9_split_copy = debug_get_num_option("M9X_SPLIT_COPY", 1);
   m9_split_be = debug_get_num_option("M9X_SPLIT_BE", 3);
   m9_split_pad_enable = debug_get_bool_option("M9X_SPLIT_PAD", true);
   m9_native_vs_gpu_padding = debug_get_bool_option("M9X_NATIVE_VS_MULTIPASS", false);
   m9_record_dump = debug_get_option("M9X_RECORD_DUMP", NULL);
   m9_vs_record_dump = debug_get_option("M9X_VS_RECORD_DUMP", NULL);
   m9_dump_native_inputs_enable = debug_get_bool_option("M9X_DUMP_NATIVE_INPUTS", false);
   m9_split_draws = debug_get_num_option("M9X_SPLIT_DRAWS", 1);
   m9_record_89 = debug_get_num_option("M9X_RECORD_89", 0);
   m9_fragdepth_state = debug_get_bool_option("M9X_FRAGDEPTH", true);
   m9_split_coverage = debug_get_bool_option("M9X_SPLIT_COVERAGE", true);
   m9_transition = debug_get_bool_option("M9X_TRANSITION", true);
   m9_split_seq = debug_get_bool_option("M9X_SPLIT_SEQ", true);
   m9_split_30 = debug_get_bool_option("M9X_SPLIT_30", false);
   m9_be_clear = debug_get_num_option("M9X_BE_CLEAR", 0);
   m9_be_dump = debug_get_option("M9X_BE_DUMP", NULL);
   m9_be_per_target = debug_get_bool_option("M9X_BE_PER_TARGET", true);
   m9_be_swap = debug_get_bool_option("M9X_BE_SWAP", true);
   m9_be_reuse = debug_get_bool_option("M9X_BE_REUSE", true);
   m9_be_shape_clip = debug_get_bool_option("M9X_BE_SHAPE_CLIP", true);
   m9_be_clear_bytes = debug_get_num_option("M9X_BE_CLEAR_BYTES", 0);
   m9_record_liveness = debug_get_bool_option("M9X_RECORD_LIVENESS", true);
   m9_split_enable = debug_get_bool_option("M9X_SPLIT", true);
   m9_level_targets = debug_get_bool_option("M9X_LEVEL_TARGET", true);
   m9_clear_on_engine = debug_get_bool_option("M9X_CLEAR_ENGINE", true);
   m9_blit_on_engine = debug_get_bool_option("M9X_BLIT_ENGINE", true);
   ctx->dump_from = debug_get_num_option("M9X_DUMP_FROM", 0);
   ctx->dump_count = debug_get_num_option("M9X_DUMP_COUNT", 4096);
   ctx->only_first = debug_get_num_option("M9X_ONLY_FIRST", UINT_MAX);
   ctx->only_after = debug_get_num_option("M9X_ONLY_AFTER", 0);
   ctx->dry_run = debug_get_bool_option("M9X_DRYRUN", false);
   util_dynarray_init(&ctx->aggregate_resources, NULL);
   if (screen->aggregate_submit) ctx->aggregate = CALLOC_STRUCT(m9_batch);
   util_dynarray_init(&ctx->records, NULL);
   util_dynarray_init(&ctx->vs_c2_images, NULL);
   util_dynarray_init(&ctx->native_vs_reasons, NULL);
   util_dynarray_init(&ctx->target_depths, NULL);
   util_dynarray_init(&ctx->indices, NULL);
   util_dynarray_init(&ctx->draws, NULL);
   util_dynarray_init(&ctx->extra, NULL);
   util_dynarray_init(&ctx->key_consts, NULL);
   util_dynarray_init(&ctx->retired_fs_variants, NULL);
   util_dynarray_init(&ctx->job_consts, NULL);
   util_dynarray_init(&ctx->job_vs_runs, NULL);
   util_dynarray_init(&ctx->job_vs_values, NULL);

   m9_vs_texture_init(&ctx->base);
   ctx->base.destroy = m9_context_destroy;
   ctx->base.flush = m9_context_flush;
   ctx->base.draw_vbo = m9_draw_vbo;
   ctx->base.clear = m9_clear;
   ctx->base.clear_render_target = m9_clear_render_target;
   ctx->base.clear_depth_stencil = m9_clear_depth_stencil;
   ctx->base.blit = m9_blit;
   ctx->base.resource_copy_region = m9_resource_copy_region;
   ctx->base.flush_resource = m9_flush_resource;
   ctx->base.create_blend_state = m9_create_blend_state;
   ctx->base.bind_blend_state = m9_bind_blend_state;
   ctx->base.delete_blend_state = m9_delete_state;
   ctx->base.create_depth_stencil_alpha_state = m9_create_dsa_state;
   ctx->base.bind_depth_stencil_alpha_state = m9_bind_dsa_state;
   ctx->base.delete_depth_stencil_alpha_state = m9_delete_state;
   ctx->base.create_rasterizer_state = m9_create_rasterizer_state;
   ctx->base.bind_rasterizer_state = m9_bind_rasterizer_state;
   ctx->base.delete_rasterizer_state = m9_delete_state;
   ctx->base.create_sampler_state = m9_create_sampler_state;
   ctx->base.bind_sampler_states = m9_bind_sampler_states;
   ctx->base.delete_sampler_state = m9_delete_state;
   ctx->base.create_sampler_view = m9_create_sampler_view;
   ctx->base.sampler_view_destroy = m9_sampler_view_destroy;
   ctx->base.sampler_view_release = u_default_sampler_view_release;
   ctx->base.set_sampler_views = m9_set_sampler_views;
   ctx->base.create_vs_state = m9_create_vs_state;
   ctx->base.bind_vs_state = m9_bind_vs_state;
   ctx->base.delete_vs_state = m9_delete_vs_state;
   ctx->base.create_fs_state = m9_create_fs_state;
   ctx->base.bind_fs_state = m9_bind_fs_state;
   ctx->base.delete_fs_state = m9_delete_fs_state;
   ctx->base.create_vertex_elements_state = m9_create_vertex_elements_state;
   ctx->base.bind_vertex_elements_state = m9_bind_vertex_elements_state;
   ctx->base.delete_vertex_elements_state = m9_delete_state;
   ctx->base.set_vertex_buffers = m9_set_vertex_buffers;
   ctx->base.set_constant_buffer = m9_set_constant_buffer;
   ctx->base.set_inlinable_constants = m9_set_inlinable_constants;
   ctx->base.set_framebuffer_state = m9_set_framebuffer_state;
   ctx->base.set_viewport_states = m9_set_viewport_states;
   ctx->base.set_scissor_states = m9_set_scissor_states;
   ctx->base.set_clip_state = m9_set_clip_state;
   ctx->base.set_blend_color = m9_set_blend_color;
   ctx->base.set_stencil_ref = m9_set_stencil_ref;
   ctx->base.set_sample_mask = m9_set_sample_mask;
   ctx->base.set_active_query_state = m9_set_active_query_state;
   ctx->base.set_polygon_stipple = m9_set_polygon_stipple;
   ctx->base.buffer_map = m9_transfer_map;
   ctx->base.buffer_unmap = m9_transfer_unmap;
   ctx->base.texture_map = m9_transfer_map;
   ctx->base.texture_unmap = m9_transfer_unmap;
   ctx->base.transfer_flush_region = u_default_transfer_flush_region;
   ctx->base.buffer_subdata = u_default_buffer_subdata;
   ctx->base.texture_subdata = u_default_texture_subdata;
   ctx->base.clear_buffer = u_default_clear_buffer;
   ctx->base.set_debug_callback = u_default_set_debug_callback;
   ctx->base.resource_release = u_default_resource_release;
   m9_query_init(&ctx->base);

   ctx->record_floats = M9_RECORD_FLOATS;
   if (m9_vertex_decl_init(&ctx->decl, 4, 4) ||
       m9_command_bo_create(screen, &ctx->command) ||
       m9_context_init_fixed(ctx))
      goto fail;
   ctx->draw = draw_create(&ctx->base);
   if (!ctx->draw)
      goto fail;
   render = m9_render_create(ctx);
   if (!render)
      goto fail;
   stage = draw_vbuf_stage(ctx->draw, &render->base);
   if (!stage) {
      m9_render_destroy(&render->base);
      goto fail;
   }
   draw_set_rasterize_stage(ctx->draw, stage);
   draw_set_render(ctx->draw, &render->base);
   /* Points become two triangles; the native point path is unverified. */
   draw_wide_point_threshold(ctx->draw, 0.0f);
   /* Smooth lines become quads whose shader computes their coverage. */
   draw_install_aaline_stage(ctx->draw, &ctx->base);
   ctx->bind_fs_wrapped = ctx->base.bind_fs_state;
   ctx->base.bind_fs_state = m9_bind_fs_handle;
   draw_set_constant_buffer_stride(ctx->draw, sizeof(float));
   ctx->base.stream_uploader = u_upload_create_default(&ctx->base);
   if (!ctx->base.stream_uploader)
      goto fail;
   ctx->base.const_uploader = ctx->base.stream_uploader;
   /* Copies and blits draw, so the blitter is built last, once every entry
    * point it calls is installed.  A context without one still works: every
    * caller falls back to the CPU transfer path. */
   /* M9X_ENGINE_BLIT=0 keeps every copy and blit on the CPU transfer path. */
   if (debug_get_bool_option("M9X_ENGINE_BLIT", true))
      ctx->blitter = util_blitter_create(&ctx->base);
   return &ctx->base;
fail:
   m9_context_destroy(&ctx->base);
   return NULL;
}
