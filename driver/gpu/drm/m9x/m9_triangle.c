// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 Rene Rebe <rene@exactco.de> */
/*
 * Bounded, kernel-owned M9148 first-triangle preparation.
 *
 * The diagnostic advances through independently fenced engine and first
 * context-activation stages.  Later draws reuse that live context and publish
 * only the captured incremental fixed-function operation.
 */
#include <linux/dma-mapping.h>
#include <linux/iosys-map.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include "m9_drv.h"
#include "m9_reg.h"

#define M9_TRIANGLE_WIDTH		320u
#define M9_TRIANGLE_HEIGHT		240u
#define M9_TRIANGLE_SURFACE_HEIGHT	ALIGN(M9_TRIANGLE_HEIGHT, 32)
#define M9_TRIANGLE_SURFACE_BYTES	(M9_TRIANGLE_WIDTH * \
					 M9_TRIANGLE_SURFACE_HEIGHT * sizeof(u32))
#define M9_TRIANGLE_AUX_DEPTH_WIDTH	ALIGN(DIV_ROUND_UP(M9_TRIANGLE_WIDTH, 8), 8)
#define M9_TRIANGLE_AUX_DEPTH_HEIGHT	ALIGN(DIV_ROUND_UP(M9_TRIANGLE_HEIGHT, 4) + 8, 8)
#define M9_TRIANGLE_AUX_DEPTH_BYTES	(M9_TRIANGLE_AUX_DEPTH_WIDTH * \
					 M9_TRIANGLE_AUX_DEPTH_HEIGHT * sizeof(u32))
#define M9_TRIANGLE_SOURCE_BYTES	0x50u
#define M9_TRIANGLE_SYSTEM_BYTES	0x20080u
#define M9_SEQ0_SECONDARY_IMAGE_BYTES	0xfa0u
#define M9_TRIANGLE_SECONDARY_BYTES	SZ_32K
#define M9_TRIANGLE_SECONDARY_PREFIX	0x8u
#define M9_TRIANGLE_SECONDARY_START_TAG	0x1u
#define M9_TRIANGLE_SECONDARY_END_TAG	0x2u
#define M9_TRIANGLE_GPUVA_TAG		0x1u
#define M9_TRIANGLE_COMPLETION_TAG	0x2u
#define M9_TRIANGLE_LINKED_OFFSET	0x300u
#define M9_TRIANGLE_LINKED_BYTES		0x200u
#define M9_TRIANGLE_C2_BYTES		0x1080u
#define M9_TRIANGLE_C2_LEADING_BYTES	SZ_4K
#define M9_TRIANGLE_C2_BACKING_BYTES	(M9_TRIANGLE_C2_LEADING_BYTES + \
					 M9_TRIANGLE_C2_BYTES)
#define M9_TRIANGLE_RASTER_SHADE_CONTROL_DW 17u
#define M9_TRIANGLE_RASTER_SHADE_LINK0_DW 93u
#define M9_TRIANGLE_RASTER_SHADE_LINK1_DW 94u
#define M9_TRIANGLE_FRAMED_SHADE_CONTROL 0x0c28u
#define M9_TRIANGLE_FRAMED_SHADE_LINK0 0x0dbcu
#define M9_TRIANGLE_FRAMED_SHADE_LINK1 0x0dc0u
#define M9_TRIANGLE_FRAMED_MRT0	0x0c94u
#define M9_TRIANGLE_FRAMED_MRT1	0x0c98u
#define M9_TRIANGLE_BE_BYTES		SZ_1M
#define M9_TRIANGLE_TRANSIENT_BYTES	0x16800u
#define M9_TRIANGLE_VRAM_BASE		0x10000000ull
#define M9_TRIANGLE_SYSTEM_GPUVA_BASE	0x40000000u
#define M9_TRIANGLE_SYSTEM_GPUVA_END	0x50000000u
#define M9_TRIANGLE_MAX_SYSTEM_PTES	44u
#define M9_SEQ0_TARGET_COUNT		9u
#define M9_SEQ0_FINAL_TARGET_OFFSET	0x80u
#define M9_SEQ0_NEXT_TARGET_OFFSET	0x90u

static bool m9_triangle_diag;
module_param(m9_triangle_diag, bool, 0400);
MODULE_PARM_DESC(m9_triangle_diag,
	"Run the staged kernel-owned M9 first-triangle diagnostic");

static bool m9_triangle_gouraud;
module_param(m9_triangle_gouraud, bool, 0400);
MODULE_PARM_DESC(m9_triangle_gouraud,
	"Use the captured red/green/blue Gouraud source instead of flat green");

enum m9_triangle_resource_id {
	M9_TRI_SOURCE,
	M9_TRI_SYSTEM,
	M9_TRI_SECONDARY,
	M9_TRI_C2,
	M9_TRI_BE,
	M9_TRI_TRANSIENT0,
	M9_TRI_TRANSIENT1,
	M9_TRI_TRANSIENT2,
	M9_TRI_TRANSIENT3,
	M9_TRI_DEPTH,
	M9_TRI_AUX_DEPTH,
	M9_TRI_SEQ0_SCRATCH26,
	M9_TRI_SEQ0_SCRATCH27,
	M9_TRI_SEQ0_SCRATCH38,
	M9_TRI_RESOURCE_COUNT,
};

struct m9_triangle_resource_spec {
	const char *name;
	u32 payload_bytes;
	u32 alignment;
	u32 fill;
	bool local;
};

struct m9_triangle_resource {
	void *cpu;
	dma_addr_t bus;
	u32 gpuva;
	u32 payload_bytes;
	u32 alloc_bytes;
	u32 guard_bytes;
	u32 guard;
	struct drm_gem_vram_object *vram;
	struct iosys_map map;
};

struct m9_triangle_job_addresses {
	u32 secondary;
	u32 source;
	u32 system_pre_start;
	u32 system_pre_end;
	u32 system_raster_start;
	u32 system_raster_end;
	u32 linked;
	u32 c2;
	u32 be;
	u32 transient[4];
	u32 color;
	u32 depth;
	u32 aux_depth;
	u32 fence;
	u32 transition[6];
};

enum m9_triangle_operand {
	M9_TRI_OP_SOURCE,
	M9_TRI_OP_SYSTEM_PRE_START,
	M9_TRI_OP_SYSTEM_PRE_END,
	M9_TRI_OP_SYSTEM_RASTER_START,
	M9_TRI_OP_SYSTEM_RASTER_END,
	M9_TRI_OP_LINKED,
	M9_TRI_OP_C2,
	M9_TRI_OP_BE,
	M9_TRI_OP_TRANSIENT0,
	M9_TRI_OP_TRANSIENT1,
	M9_TRI_OP_TRANSIENT2,
	M9_TRI_OP_TRANSIENT3,
	M9_TRI_OP_COLOR,
	M9_TRI_OP_DEPTH,
	M9_TRI_OP_AUX_DEPTH,
	M9_TRI_OP_FENCE,
};

struct m9_triangle_reloc_site {
	u8 range;
	u8 index;
	u16 dword;
	u8 operand;
};

struct m9_triangle_commands {
	u32 preprocess[597];
	u32 raster[592];
	u32 completion[8];
	u32 primary[9];
	u32 sequence0_primary[9];
};

struct m9x_triangle_diag {
	struct m9_triangle_resource resource[M9_TRI_RESOURCE_COUNT];
	u32 pte_count;
	struct m9_triangle_commands commands;
	bool context_initialized;
	bool prepared;
};

enum m9_seq0_operand {
	M9_SEQ0_TARGET,
	M9_SEQ0_SCRATCH26,
	M9_SEQ0_SCRATCH27,
	M9_SEQ0_SCRATCH38,
	M9_SEQ0_COMPLETION,
};

struct m9_seq0_patch {
	u16 offset;
	u8 index;
	u8 operand;
	u8 step;
	u8 tag;
	u32 captured;
};

struct m9_seq0_job {
	u32 secondary;
	u32 target;
	u32 scratch26;
	u32 scratch27;
	u32 scratch38;
	u32 completion;
};

/* Exact address-bearing sites in the count-free 597/592/8-dword operation. */
static const struct m9_triangle_reloc_site m9_triangle_reloc_sites[] = {
	{ 0, 0xb0, 15, M9_TRI_OP_TRANSIENT0 },
	{ 0, 0xb2, 18, M9_TRI_OP_TRANSIENT3 },
	{ 0, 0xb4, 21, M9_TRI_OP_TRANSIENT1 },
	{ 0, 0xb8, 24, M9_TRI_OP_TRANSIENT2 },
	{ 0, 0x00, 57, M9_TRI_OP_SOURCE },
	{ 0, 0xc2, 61, M9_TRI_OP_C2 },
	{ 0, 0x8d, 66, M9_TRI_OP_SYSTEM_PRE_START },
	{ 0, 0x8e, 67, M9_TRI_OP_SYSTEM_PRE_END },
	{ 0, 0x6c, 590, M9_TRI_OP_LINKED },
	{ 1, 0xb0, 22, M9_TRI_OP_COLOR },
	{ 1, 0xb8, 25, M9_TRI_OP_DEPTH },
	{ 1, 0xba, 28, M9_TRI_OP_AUX_DEPTH },
	{ 1, 0xbe, 31, M9_TRI_OP_BE },
	{ 1, 0x00, 53, M9_TRI_OP_TRANSIENT0 },
	{ 1, 0x01, 54, M9_TRI_OP_TRANSIENT1 },
	{ 1, 0x02, 57, M9_TRI_OP_TRANSIENT2 },
	{ 1, 0x03, 58, M9_TRI_OP_TRANSIENT3 },
	{ 1, 0xc2, 61, M9_TRI_OP_C2 },
	{ 1, 0x8d, 66, M9_TRI_OP_SYSTEM_RASTER_START },
	{ 1, 0x8e, 67, M9_TRI_OP_SYSTEM_RASTER_END },
	{ 1, 0x6c, 588, M9_TRI_OP_LINKED },
	{ 2, 0x1b, 1, M9_TRI_OP_FENCE },
};

struct m9_triangle_transition_site {
	u8 range;
	u8 index;
	u16 dword;
	u8 transition;
	bool tag;
};

static const struct m9_triangle_transition_site m9_triangle_transition_sites[] = {
	{ 0, 0xe3, 2, 0, false },
	{ 0, 0xe3, 13, 1, false },
	{ 1, 0xe3, 2, 2, false },
	{ 1, 0xe3, 7, 3, false },
	{ 1, 0x43, 9, 3, false },
	{ 1, 0xe3, 20, 4, false },
	{ 2, 0xe3, 5, 5, true },
	{ 2, 0x43, 7, 5, false },
};

static const struct m9_triangle_resource_spec m9_triangle_specs[] = {
	[M9_TRI_SOURCE] = { "source", M9_TRIANGLE_SOURCE_BYTES, 4, 0 },
	[M9_TRI_SYSTEM] = { "system", M9_TRIANGLE_SYSTEM_BYTES, 0x80, 0 },
	[M9_TRI_SECONDARY] = { "secondary", M9_TRIANGLE_SECONDARY_BYTES,
			       0x80, 0 },
	[M9_TRI_C2] = { "c2-backing", M9_TRIANGLE_C2_BACKING_BYTES,
			 SZ_4K, 0, true },
	[M9_TRI_BE] = { "be", M9_TRIANGLE_BE_BYTES, SZ_4K, 0, true },
	[M9_TRI_TRANSIENT0] = { "transient0", M9_TRIANGLE_TRANSIENT_BYTES,
				  0x100, 0x54520000, true },
	[M9_TRI_TRANSIENT1] = { "transient1", M9_TRIANGLE_TRANSIENT_BYTES,
				  0x100, 0x54520001, true },
	[M9_TRI_TRANSIENT2] = { "transient2", M9_TRIANGLE_TRANSIENT_BYTES,
				  0x100, 0x54520002, true },
	[M9_TRI_TRANSIENT3] = { "transient3", M9_TRIANGLE_TRANSIENT_BYTES,
				  0x100, 0x54520003, true },
	[M9_TRI_DEPTH] = { "depth", M9_TRIANGLE_SURFACE_BYTES, 0x100,
			     U32_MAX, true },
	[M9_TRI_AUX_DEPTH] = { "aux-depth", M9_TRIANGLE_AUX_DEPTH_BYTES, 0x100,
				 0x007ff000, true },
	[M9_TRI_SEQ0_SCRATCH26] = { "sequence0-scratch26", 0x800, SZ_4K,
					 0, true },
	[M9_TRI_SEQ0_SCRATCH27] = { "sequence0-scratch27", 0x20080, SZ_4K,
					 0, true },
	[M9_TRI_SEQ0_SCRATCH38] = { "sequence0-scratch38", 0x400, SZ_4K,
					 0, true },
};

static const u32 m9_triangle_flat_source[] = {
	0x41a00000, 0x41a00000, 0, 0x3f800000, 0,
	0x43960000, 0x41a00000, 0, 0x3f800000, 0,
	0x43200000, 0x435c0000, 0, 0x3f800000, 0,
	0x43200000, 0x435c0000, 0, 0x3f800000, 0,
};

static const u32 m9_triangle_gouraud_source[] = {
	0x41a00000, 0x41a00000, 0x3f800000, 0, 0,
	0x43960000, 0x41a00000, 0, 0x3f800000, 0,
	0x43200000, 0x435c0000, 0, 0, 0x3f800000,
	0x43200000, 0x435c0000, 0, 0, 0x3f800000,
};

/* Exact 320x240 GL viewport, depth and glOrtho constants consumed through
 * 8d/8e.  The first vec4 is the raster range; the following eleven vec4s are
 * the preprocessing range.  The vendor stores 2/width and 2/height at dwords
 * 9 and 14, then copies its viewport/depth constructor at dwords 32..47.
 */
static const u32 m9_triangle_system_prefix[] = {
	0x00000000, 0x3f800000, 0x3f800000, 0x3f000000,
	0x00000000, 0xbf800000, 0x00000000, 0x3f800000,
	0x00000000, 0x3bcccccd, 0x00000000, 0x3f800000,
	0x00000000, 0xbf800000, 0x3c088889, 0x00000000,
	0x00000000, 0x00000000, 0xbf800000, 0x00000000,
	0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x00000000, 0x3f800000, 0x00000000, 0x3f800000,
	0x00000000, 0x00000000, 0x3f800000, 0x00000000,
	0x43200000, 0xc2f00000, 0x3f000000, 0x3f800000,
	0x43200000, 0x42f00000, 0x3f000000, 0x43700000,
	0x00000000, 0x40000000, 0xc67f8000, 0x467f7c00,
	0x00000000, 0x00000000, 0x3f800000, 0x3f000000,
};

/* Exact vendor viewport/depth transform copied by m9x5091 into the pool-5
 * fixed-function record: {w/2, -h/2, .5, 0}, {w/2, h/2, .5, 0}.
 * Gouraud needs no following linked-native payload; bytes [0x20,0x200) are
 * deterministically zero.
 */
static const u32 m9_triangle_linked_prefix[] = {
	0x43200000, 0xc2f00000, 0x3f000000, 0,
	0x43200000, 0x42f00000, 0x3f000000, 0,
};

#define M9_TRIANGLE_LINKED_ARENA_LIMIT	0x00800000u
#define M9_TRIANGLE_LINKED_FIRST_BACKOFF	0x00000780u
#define M9_TRIANGLE_LINKED_REF_STRIDE	0x00000028u

#include "m9_triangle_framed.h"
#include "m9_sequence0_framed.h"

static const struct m9_seq0_patch m9_seq0_patches[] = {
	{ 0x014, 0xe3, M9_SEQ0_TARGET, 0, 0, 0x00000030 },
	{ 0x084, 0xe3, M9_SEQ0_TARGET, 1, 0, 0x00000040 },
	{ 0x184, 0xe3, M9_SEQ0_TARGET, 1, 0, 0x00000040 },
	{ 0x54c, 0xe3, M9_SEQ0_TARGET, 2, 0, 0x00000050 },
	{ 0x5a0, 0xe3, M9_SEQ0_TARGET, 3, 0, 0x00000060 },
	{ 0x5ac, 0xb0, M9_SEQ0_SCRATCH26, 0, 0, 0x11e19000 },
	{ 0x648, 0xe3, M9_SEQ0_TARGET, 3, 0, 0x00000060 },
	{ 0xcc0, 0xe3, M9_SEQ0_TARGET, 3, 0, 0x00000060 },
	{ 0xd74, 0xe3, M9_SEQ0_TARGET, 4, 0, 0x00000070 },
	{ 0xdbc, 0xe3, M9_SEQ0_TARGET, 5, 0, 0x00000080 },
	{ 0xdc8, 0xb0, M9_SEQ0_SCRATCH27, 0, 0, 0x11e21000 },
	{ 0xe88, 0xe3, M9_SEQ0_TARGET, 6, 0, 0x00000090 },
	{ 0xed0, 0xe3, M9_SEQ0_TARGET, 7, 0, 0x000000a0 },
	{ 0xedc, 0xb0, M9_SEQ0_SCRATCH38, 0, 0, 0x11ed9000 },
	{ 0xf7c, 0x1b, M9_SEQ0_COMPLETION, 0, 2, 0x13861002 },
	{ 0xf90, 0xe3, M9_SEQ0_TARGET, 8, 1, 0x000000b1 },
	{ 0xf9c, 0x43, M9_SEQ0_TARGET, 8, 0, 0x000000b0 },
};

/* Count words have been removed; packet grouping and write order are exact. */
static const u32 m9_triangle_range_0[] = {
	0x4545e3e6, 0x00000003, 0x00000270, 0x454545e6, 0x00000003,
	0x45d7eff0, 0xf0000400, 0x00288800, 0x000050ae, 0x45454522,
	0x0008040e, 0x4545e3e6, 0x00000003, 0x00000280, 0x4545b1b0,
	0x113c1000, 0x68002000, 0x4545b3b2, 0x113d7800, 0x68002000,
	0x4545b5b4, 0x113ee000, 0x68002000, 0x4545b9b8, 0x11404800,
	0xb8002000, 0x4545bbba, 0x00000020, 0xf0010040, 0x454545be,
	0x00000003, 0xadadadad, 0x00000020, 0x00000020, 0x00000020,
	0x00000020, 0x454545af, 0x00000000, 0x45aaa9a8, 0x0000001f,
	0x00000000, 0x00000000, 0x45aeacab, 0xffffffff, 0xffffffff,
	0x22444442, 0xc5c4f6f5, 0x00000000, 0x7fff7fff, 0x00000000,
	0x7fff7fff, 0xc18a8988, 0x0300bf00, 0xfe201e92, 0x00000000,
	0x00000000, 0x45100100, 0x40f52001, 0xf2fe014f, 0x00000014,
	0x458fc3c2, 0x11f91000, 0x00000004, 0x00000000, 0x8f8e8d8c,
	0x00501600, 0x40c3a011, 0x40c3a0c1, 0x00000000, 0x45454574,
	0x00000100, 0x45454575, 0x00000100, 0x45454576, 0x00000100,
	0x45454577, 0x00000100, 0x45454570, 0x00000000, 0x45454571,
	0x00000000, 0x45454572, 0x00000000, 0x45454573, 0x00000000,
	0xebeaf4ec, 0x00000000, 0x00010004, 0x00000000, 0x46000000,
	0x45454530, 0x00000000, 0x45454528, 0x00000000, 0x23232323,
	0x00100dac, 0x00100236, 0x02200d88, 0x02200ab6, 0x23232323,
	0x00004b08, 0x00000000, 0x00000000, 0x00000000, 0x45454568,
	0x80000000, 0x63626160, 0x03f0183d, 0xa0318fff, 0x41000000,
	0x00403200, 0x67666564, 0x42000000, 0x00403400, 0x00020000,
	0x024a2800, 0x45454568, 0x80000020, 0x63626160, 0x13f0187f,
	0x9034fb3f, 0x23e018bf, 0x9b3bffef, 0x67666564, 0x03f2003f,
	0x80fd1c00, 0x03f201bf, 0x80fd2c40, 0x45454568, 0x80000040,
	0x63626160, 0x13e200bf, 0x9b33fffe, 0x4000005c, 0x02471c00,
	0x67666564, 0x33f2007f, 0x80fcfb40, 0x33e201ff, 0x9034cffe,
	0x45454568, 0x80000060, 0x63626160, 0x23f2013f, 0x80fcfb80,
	0x4001005d, 0x02489c00, 0x67666564, 0x33f200ff, 0x8bffffc0,
	0x4002005e, 0x02491c00, 0x45454568, 0x80000080, 0x63626160,
	0x33f2017f, 0x8bffffc0, 0x03fc3fff, 0x8bffffc0, 0x67666564,
	0x00000008, 0x00400000, 0x00000004, 0x00402000, 0x45454568,
	0x800000a0, 0x63626160, 0x07fc303f, 0x8bffffc0, 0x00000043,
	0x04c00016, 0x67666564, 0x00000001, 0x00402217, 0x00000001,
	0x01482017, 0x45454568, 0x800000c0, 0x63626160, 0x73feafff,
	0x80fddfc0, 0x03f01e3f, 0x8bffffc0, 0x67666564, 0x13f01e3f,
	0x8bffffc0, 0x23f01e3f, 0x80fdae80, 0x45454568, 0x800000e0,
	0x63626160, 0x33f01f3f, 0x8bffffc0, 0x00000000, 0x00000000,
	0x67666564, 0x03f20acf, 0x8cfff1c0, 0x53f00ae8, 0x8afd20d2,
	0x45454568, 0x80000100, 0x63626160, 0x53f01e2a, 0x8afd32d3,
	0x00000022, 0x04c00010, 0x67666564, 0x00000027, 0x04c00011,
	0x00020000, 0x01c3ba11, 0x45454568, 0x80000120, 0x63626160,
	0x13f20ad2, 0x81fff4c0, 0x4bfea4ff, 0x8bffffd3, 0x67666564,
	0x0000ffff, 0x01c7b600, 0x63f18aff, 0x8dfffd52, 0x45454568,
	0x80000140, 0x63626160, 0x3f800000, 0x024db600, 0x0003ffff,
	0x01c38e00, 0x67666564, 0xff5fffff, 0x9b4bffd2, 0x00000007,
	0x020db600, 0x45454568, 0x80000160, 0x63626160, 0x00010000,
	0x024db612, 0x63f19f0b, 0x81fff0d3, 0x67666564, 0x6bfea38a,
	0x81fff092, 0xffffffc9, 0x75fff07f, 0x45454568, 0x80000180,
	0x63626160, 0xfd2fffc8, 0x91fff035, 0xfd2fffcc, 0x91fff135,
	0x67666564, 0xfd2fffcd, 0x91fff175, 0xfd2ffff6, 0x96fd75f5,
	0x45454568, 0x800001a0, 0x63626160, 0xfd2ffff6, 0x98fd65b5,
	0xfd2ffff6, 0x98fd5575, 0x67666564, 0xfffe6d76, 0x88fd4500,
	0xfffffff6, 0x74fd863f, 0x45454568, 0x800001c0, 0x63626160,
	0xfffffff6, 0x74fd967f, 0xfffcf6ff, 0x8bffffc0, 0x67666564,
	0xfffd16ff, 0x8bffffc0, 0x6ffc86ff, 0x8bffffc0, 0x45454568,
	0x800001e0, 0x63626160, 0xffffff00, 0x01cd3400, 0x00010000,
	0x01cd8100, 0x67666564, 0x00010000, 0x00ce380a, 0xfffdffff,
	0x01cdb612, 0x45454568, 0x80000200, 0x63626160, 0x00020000,
	0x01ce8000, 0x00000000, 0x020db613, 0x67666564, 0x53f21e7f,
	0x8bffffc0, 0x00000006, 0x04c00012, 0x45454568, 0x80000220,
	0x63626160, 0x63f21e7f, 0x8bffffc0, 0xfffcc6ff, 0x8bffffc0,
	0x67666564, 0x00000001, 0x00c8a200, 0x23f21e7f, 0x8bffffc0,
	0x45454568, 0x80000240, 0x63626160, 0x33f21e7f, 0x8bffffc0,
	0xfffcc3ff, 0x8bffffc0, 0x67666564, 0x00000001, 0x00c8a200,
	0x00010000, 0x01c7a700, 0x45454568, 0x80000260, 0x63626160,
	0x00010000, 0x00ce380a, 0x00000001, 0x01482100, 0x67666564,
	0xffffffc7, 0x04c0000b, 0xfffcdfff, 0x8bffffca, 0x45454568,
	0x80000280, 0x63626160, 0x43fea47f, 0x8bffffca, 0x00000004,
	0x0148a50a, 0x67666564, 0x00000006, 0x04c0000c, 0x00000000,
	0x0040200c, 0x45454568, 0x800002a0, 0x63626160, 0x03f01c3f,
	0x8bffffcd, 0x03f01e3f, 0x8bffffcd, 0x67666564, 0x13f1193f,
	0x8bffffc0, 0x0000001d, 0x021d8e00, 0x45454568, 0x800002c0,
	0x63626160, 0xffffffba, 0x04c00012, 0x03f21e7f, 0x8bffffc0,
	0x67666564, 0x13f21e7f, 0x8bffffc0, 0x13f2ff3f, 0x8bffffc0,
	0x45454568, 0x800002e0, 0x63626160, 0x00000000, 0x00402013,
	0x00000004, 0x01482300, 0x67666564, 0x0000001c, 0x04c0000c,
	0x0000001f, 0x0500000d, 0x45454568, 0x80000300, 0x63626160,
	0x00000003, 0x01482000, 0x23f10c3f, 0x8bffffc0, 0x67666564,
	0x33f10c7f, 0x8bffffc0, 0x0000001b, 0x05000000, 0x45454568,
	0x80000320, 0x63626160, 0x13f8033f, 0x80fcb100, 0x03f8003f,
	0x80fcf140, 0x67666564, 0x53fc4fff, 0x8bffffc0, 0xfbffffff,
	0x01ca2800, 0x45454568, 0x80000340, 0x63626160, 0xfffc353f,
	0x8bffffc0, 0x00000015, 0x05000000, 0x67666564, 0x13e80704,
	0x914542c4, 0x03e80405, 0x914953c5, 0x45454568, 0x80000360,
	0x63626160, 0x00000000, 0x00000000, 0x53f802c4, 0x81fd8440,
	0x67666564, 0xffffffc5, 0x75fd94bf, 0xffffffcb, 0x75fdc13f,
	0x45454568, 0x80000380, 0x63626160, 0xffffffcf, 0x75fdd17f,
	0x00000001, 0x01482100, 0x67666564, 0x63f806ff, 0x8bffffc0,
	0xfffffff2, 0x04c0000b, 0x45454568, 0x800003a0, 0x63626160,
	0x13f80b3f, 0x8bffffc0, 0x73f80aff, 0x8bffffc0, 0x67666564,
	0x87f8083f, 0x8bffffcb, 0x33f0083f, 0x8bffffc0, 0x45454568,
	0x800003c0, 0x63626160, 0x43f0087f, 0x8bffffc0, 0x53f008bf,
	0x8bffffc0, 0x67666564, 0x8ff8083f, 0xcbffffc0, 0x33f8003f,
	0x8bffffc0, 0x45454568, 0x800003e0, 0x63626160, 0x43f8043f,
	0x8bffffc0, 0x53f8083f, 0x8bffffc0, 0x67666564, 0x8bf9f03f,
	0xcbffffc0, 0x00000000, 0x00000000, 0x45454568, 0x80000400,
	0x63626160, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x67666564, 0x03f01e3f, 0x8bffffc0, 0x13f01e3f, 0x8dfc3ec0,
	0x45454568, 0x80000420, 0x63626160, 0x000000ff, 0x01dd0700,
	0x00000009, 0x04c0000b, 0x67666564, 0x075c31bf, 0x9bffffc0,
	0xfc3fffff, 0x9bffffc1, 0x45454568, 0x80000440, 0x63626160,
	0x183c4fff, 0x9bffffc2, 0xfc8fffff, 0x9bfffff6, 0x67666564,
	0xfc9fffff, 0x9bfffff6, 0xfcafffff, 0x9bfffff6, 0x45454568,
	0x80000460, 0x63626160, 0xfc3fffcc, 0x90100d84, 0xfc3fffcd,
	0x90141d85, 0x67666564, 0xffffffce, 0x74fc2dbf, 0x00000000,
	0x06400000, 0x45454568, 0x80000480, 0x63626160, 0x00000000,
	0x00000000, 0x00000000, 0x00000000, 0x67666564, 0x03f0f97f,
	0x8bffffcb, 0xffbfffff, 0x9b13ffc4, 0x45454568, 0x800004a0,
	0x63626160, 0x0031197f, 0x9b17ffc5, 0x00000000, 0x06400000,
	0x67666564, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x45454568, 0x800004c0, 0x63626160, 0x00000000, 0x00000000,
	0x00000000, 0x10000000, 0x67666564, 0xfffc0fff, 0x8bffffc0,
	0x00000000, 0x00000000, 0x45454568, 0x800004e0, 0x63626160,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x67666564,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x45456a69,
	0x00000004, 0x00990000, 0x456c6b68, 0x00000000, 0x00001030,
	0x40c3a301, 0x45451819, 0x00000000, 0x7fffffff, 0x23000001,
	0x00000000, 0x00000004,
};

static const u32 m9_triangle_range_1[] = {
	0x4545e3e6, 0x00000003, 0x00000290, 0x454545e6, 0x00000003,
	0x4545e3e6, 0x00000003, 0x000002a0, 0x45454543, 0x000002a0,
	0x454545e6, 0x42000000, 0x45d7eff0, 0xc4008400, 0x00000000,
	0x200058a3, 0x45454522, 0x00006403, 0x4545e3e6, 0x00000003,
	0x000002b0, 0x4545b1b0, 0x11ee9000, 0xcc210140, 0x4545b9b8,
	0x11f39000, 0xfa210140, 0x4545bbba, 0x11f89000, 0xf0010140,
	0x454545be, 0x112c1000, 0x45aaa9a8, 0x0038038a, 0x08ffff00,
	0x00ffff00, 0x45aeacab, 0x6be9ffff, 0x00002add, 0x22444442,
	0xc5c4f6f5, 0x00000000, 0x00f00140, 0x00000000, 0x00f00140,
	0x454545f1, 0x00000000, 0xc18a8988, 0x0100dff0, 0xfe207ff8,
	0x00000000, 0x00000000, 0x45100100, 0x113c1000, 0x113ee000,
	0x00100010, 0x45110302, 0x11404800, 0x113d7800, 0x00100006,
	0x458fc3c2, 0x11f91000, 0x00000000, 0x00000000, 0x8f8e8d8c,
	0x00500200, 0x40c3a001, 0x40c3a011, 0x00000000, 0x45454574,
	0x00000100, 0x45454575, 0x00000100, 0x45454576, 0x00000100,
	0x45454577, 0x00000100, 0x45454570, 0x00000000, 0x45454571,
	0x00000000, 0x45454572, 0x00000000, 0x45454573, 0x00000000,
	0xebeaf4ec, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x45454528, 0x00000000, 0x23232323, 0x80100d88, 0x801006b6,
	0x00010688, 0x00c231b6, 0x23232323, 0x00d30688, 0x00000000,
	0x00000000, 0x00000000, 0x45454568, 0x80000000, 0x63626160,
	0x03f0183d, 0xa0318fff, 0x41000000, 0x00403200, 0x67666564,
	0x42000000, 0x00403400, 0x00020000, 0x024a2800, 0x45454568,
	0x80000020, 0x63626160, 0x13f0187f, 0x9034fb3f, 0x23e018bf,
	0x9b3bffef, 0x67666564, 0x03f2003f, 0x80fd1c00, 0x03f201bf,
	0x80fd2c40, 0x45454568, 0x80000040, 0x63626160, 0x13e200bf,
	0x9b33fffe, 0x4000005c, 0x02471c00, 0x67666564, 0x33f2007f,
	0x80fcfb40, 0x33e201ff, 0x9034cffe, 0x45454568, 0x80000060,
	0x63626160, 0x23f2013f, 0x80fcfb80, 0x4001005d, 0x02489c00,
	0x67666564, 0x33f200ff, 0x8bffffc0, 0x4002005e, 0x02491c00,
	0x45454568, 0x80000080, 0x63626160, 0x33f2017f, 0x8bffffc0,
	0x03fc3fff, 0x8bffffc0, 0x67666564, 0x00000008, 0x00400000,
	0x00000004, 0x00402000, 0x45454568, 0x800000a0, 0x63626160,
	0x07fc303f, 0x8bffffc0, 0x00000043, 0x04c00016, 0x67666564,
	0x00000001, 0x00402217, 0x00000001, 0x01482017, 0x45454568,
	0x800000c0, 0x63626160, 0x73feafff, 0x80fddfc0, 0x03f01e3f,
	0x8bffffc0, 0x67666564, 0x13f01e3f, 0x8bffffc0, 0x23f01e3f,
	0x80fdae80, 0x45454568, 0x800000e0, 0x63626160, 0x33f01f3f,
	0x8bffffc0, 0x00000000, 0x00000000, 0x67666564, 0x03f20acf,
	0x8cfff1c0, 0x53f00ae8, 0x8afd20d2, 0x45454568, 0x80000100,
	0x63626160, 0x53f01e2a, 0x8afd32d3, 0x00000022, 0x04c00010,
	0x67666564, 0x00000027, 0x04c00011, 0x00020000, 0x01c3ba11,
	0x45454568, 0x80000120, 0x63626160, 0x13f20ad2, 0x81fff4c0,
	0x4bfea4ff, 0x8bffffd3, 0x67666564, 0x0000ffff, 0x01c7b600,
	0x63f18aff, 0x8dfffd52, 0x45454568, 0x80000140, 0x63626160,
	0x3f800000, 0x024db600, 0x0003ffff, 0x01c38e00, 0x67666564,
	0xff5fffff, 0x9b4bffd2, 0x00000007, 0x020db600, 0x45454568,
	0x80000160, 0x63626160, 0x00010000, 0x024db612, 0x63f19f0b,
	0x81fff0d3, 0x67666564, 0x6bfea38a, 0x81fff092, 0xffffffc9,
	0x75fff07f, 0x45454568, 0x80000180, 0x63626160, 0xfd2fffc8,
	0x91fff035, 0xfd2fffcc, 0x91fff135, 0x67666564, 0xfd2fffcd,
	0x91fff175, 0xfd2ffff6, 0x96fd75f5, 0x45454568, 0x800001a0,
	0x63626160, 0xfd2ffff6, 0x98fd65b5, 0xfd2ffff6, 0x98fd5575,
	0x67666564, 0xfffe6d76, 0x88fd4500, 0xfffffff6, 0x74fd863f,
	0x45454568, 0x800001c0, 0x63626160, 0xfffffff6, 0x74fd967f,
	0xfffcf6ff, 0x8bffffc0, 0x67666564, 0xfffd16ff, 0x8bffffc0,
	0x6ffc86ff, 0x8bffffc0, 0x45454568, 0x800001e0, 0x63626160,
	0xffffff00, 0x01cd3400, 0x00010000, 0x01cd8100, 0x67666564,
	0x00010000, 0x00ce380a, 0xfffdffff, 0x01cdb612, 0x45454568,
	0x80000200, 0x63626160, 0x00020000, 0x01ce8000, 0x00000000,
	0x020db613, 0x67666564, 0x53f21e7f, 0x8bffffc0, 0x00000006,
	0x04c00012, 0x45454568, 0x80000220, 0x63626160, 0x63f21e7f,
	0x8bffffc0, 0xfffcc6ff, 0x8bffffc0, 0x67666564, 0x00000001,
	0x00c8a200, 0x23f21e7f, 0x8bffffc0, 0x45454568, 0x80000240,
	0x63626160, 0x33f21e7f, 0x8bffffc0, 0xfffcc3ff, 0x8bffffc0,
	0x67666564, 0x00000001, 0x00c8a200, 0x00010000, 0x01c7a700,
	0x45454568, 0x80000260, 0x63626160, 0x00010000, 0x00ce380a,
	0x00000001, 0x01482100, 0x67666564, 0xffffffc7, 0x04c0000b,
	0xfffcdfff, 0x8bffffca, 0x45454568, 0x80000280, 0x63626160,
	0x43fea47f, 0x8bffffca, 0x00000004, 0x0148a50a, 0x67666564,
	0x00000006, 0x04c0000c, 0x00000000, 0x0040200c, 0x45454568,
	0x800002a0, 0x63626160, 0x03f01c3f, 0x8bffffcd, 0x03f01e3f,
	0x8bffffcd, 0x67666564, 0x13f1193f, 0x8bffffc0, 0x0000001d,
	0x021d8e00, 0x45454568, 0x800002c0, 0x63626160, 0xffffffba,
	0x04c00012, 0x03f21e7f, 0x8bffffc0, 0x67666564, 0x13f21e7f,
	0x8bffffc0, 0x13f2ff3f, 0x8bffffc0, 0x45454568, 0x800002e0,
	0x63626160, 0x00000000, 0x00402013, 0x00000004, 0x01482300,
	0x67666564, 0x0000001c, 0x04c0000c, 0x0000001f, 0x0500000d,
	0x45454568, 0x80000300, 0x63626160, 0x00000003, 0x01482000,
	0x23f10c3f, 0x8bffffc0, 0x67666564, 0x33f10c7f, 0x8bffffc0,
	0x0000001b, 0x05000000, 0x45454568, 0x80000320, 0x63626160,
	0x13f8033f, 0x80fcb100, 0x03f8003f, 0x80fcf140, 0x67666564,
	0x53fc4fff, 0x8bffffc0, 0xfbffffff, 0x01ca2800, 0x45454568,
	0x80000340, 0x63626160, 0xfffc353f, 0x8bffffc0, 0x00000015,
	0x05000000, 0x67666564, 0x13e80704, 0x914542c4, 0x03e80405,
	0x914953c5, 0x45454568, 0x80000360, 0x63626160, 0x00000000,
	0x00000000, 0x53f802c4, 0x81fd8440, 0x67666564, 0xffffffc5,
	0x75fd94bf, 0xffffffcb, 0x75fdc13f, 0x45454568, 0x80000380,
	0x63626160, 0xffffffcf, 0x75fdd17f, 0x00000001, 0x01482100,
	0x67666564, 0x63f806ff, 0x8bffffc0, 0xfffffff2, 0x04c0000b,
	0x45454568, 0x800003a0, 0x63626160, 0x13f80b3f, 0x8bffffc0,
	0x73f80aff, 0x8bffffc0, 0x67666564, 0x87f8083f, 0x8bffffcb,
	0x33f0083f, 0x8bffffc0, 0x45454568, 0x800003c0, 0x63626160,
	0x43f0087f, 0x8bffffc0, 0x53f008bf, 0x8bffffc0, 0x67666564,
	0x8ff8083f, 0xcbffffc0, 0x33f8003f, 0x8bffffc0, 0x45454568,
	0x800003e0, 0x63626160, 0x43f8043f, 0x8bffffc0, 0x53f8083f,
	0x8bffffc0, 0x67666564, 0x8bf9f03f, 0xcbffffc0, 0x00000000,
	0x00000000, 0x45454568, 0x80000400, 0x63626160, 0x00000000,
	0x00000000, 0x00000000, 0x00000000, 0x67666564, 0x03f01e3f,
	0x8bffffc0, 0x13f01e3f, 0x8dfc3ec0, 0x45454568, 0x80000420,
	0x63626160, 0x000000ff, 0x01dd0700, 0x00000009, 0x04c0000b,
	0x67666564, 0x075c31bf, 0x9bffffc0, 0xfc3fffff, 0x9bffffc1,
	0x45454568, 0x80000440, 0x63626160, 0x183c4fff, 0x9bffffc2,
	0xfc8fffff, 0x9bfffff6, 0x67666564, 0xfc9fffff, 0x9bfffff6,
	0xfcafffff, 0x9bfffff6, 0x45454568, 0x80000460, 0x63626160,
	0xfc3fffcc, 0x90100d84, 0xfc3fffcd, 0x90141d85, 0x67666564,
	0xffffffce, 0x74fc2dbf, 0x00000000, 0x06400000, 0x45454568,
	0x80000480, 0x63626160, 0x00000000, 0x00000000, 0x00000000,
	0x00000000, 0x67666564, 0x03f0f97f, 0x8bffffcb, 0xffbfffff,
	0x9b13ffc4, 0x45454568, 0x800004a0, 0x63626160, 0x0031197f,
	0x9b17ffc5, 0x00000000, 0x06400000, 0x67666564, 0x00000000,
	0x00000000, 0x00000000, 0x00000000, 0x45454568, 0x800004c0,
	0x63626160, 0x00000000, 0x00000000, 0x00000000, 0x10000000,
	0x67666564, 0xfffc0fff, 0x8bffffc0, 0x00000000, 0x00000000,
	0x45454568, 0x800004e0, 0x63626160, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x67666564, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x45456a69, 0x00000004, 0x00990000,
	0x456c6b68, 0x00000000, 0x00001030, 0x40c3a301, 0x23000001,
	0x00000000, 0x00000003,
};

static const u32 m9_triangle_range_2[] = {
	0x4545451b, 0x13861002, 0x45e3e6e6, 0x00000003,
	0x00000002, 0x000002c1, 0x45454543, 0x000002c0,
};

/* Absolute byte offsets in the private BmBuffer image.  They parallel the
 * semantic count-free sites above, but retain every private count dword. */
static const u16 m9_triangle_framed_reloc_offsets[] = {
	0x05c, 0x06c, 0x07c, 0x08c, 0x130, 0x144, 0x15c, 0x160,
	0xba8, 0xc44, 0xc54, 0xc64, 0xc74, 0xce4, 0xce8, 0xcf8,
	0xcfc, 0xd0c, 0xd24, 0xd28, 0x1764, 0x177c,
};

static const u16 m9_triangle_framed_transition_offsets[] = {
	0x014, 0x050, 0xbd4, 0xbf0, 0xbfc, 0xc38, 0x1790, 0x179c,
};

static u64 m9_triangle_template_fnv1a(const u32 *words, size_t count)
{
	u64 hash = 0xcbf29ce484222325ULL;
	unsigned int byte;
	size_t i;

	for (i = 0; i < count; i++)
		for (byte = 0; byte < sizeof(words[i]); byte++) {
			hash ^= (words[i] >> (byte * 8)) & 0xff;
			hash *= 0x100000001b3ULL;
		}
	return hash;
}

static u32 m9_triangle_guard(enum m9_triangle_resource_id id)
{
	return 0xd9a60000u | id;
}

static int m9_triangle_alloc_vram(struct m9x_device *mdev,
				  enum m9_triangle_resource_id id,
				  u32 alloc_bytes)
{
	const struct m9_triangle_resource_spec *spec = &m9_triangle_specs[id];
	struct m9_triangle_resource *resource =
		&mdev->transport.triangle->resource[id];
	struct drm_gem_vram_object *vram;
	s64 offset;
	u64 end;
	u32 word;
	int ret;

	vram = drm_gem_vram_create(&mdev->drm, alloc_bytes,
				    max_t(u32, spec->alignment / PAGE_SIZE, 1));
	if (IS_ERR(vram))
		return PTR_ERR(vram);
	ret = m9x_vram_pin(vram);
	if (ret)
		goto out_put;
	offset = drm_gem_vram_offset(vram);
	if (offset < 0 || check_add_overflow((u64)offset, (u64)alloc_bytes,
					     &end) || end > mdev->vram_size ||
	    check_add_overflow(M9_TRIANGLE_VRAM_BASE, (u64)offset, &end) ||
	    end > U32_MAX || (end & (spec->alignment - 1))) {
		ret = offset < 0 ? (int)offset : -ERANGE;
		goto out_unpin;
	}
	ret = ttm_bo_reserve(&vram->bo, true, false, NULL);
	if (ret)
		goto out_unpin;
	ret = drm_gem_vram_vmap(vram, &resource->map);
	ttm_bo_unreserve(&vram->bo);
	if (ret)
		goto out_unpin;

	resource->vram = vram;
	resource->bus = end;
	resource->payload_bytes = spec->payload_bytes;
	resource->alloc_bytes = alloc_bytes;
	resource->guard_bytes = alloc_bytes - spec->payload_bytes;
	resource->guard = m9_triangle_guard(id);
	for (word = 0; word < spec->payload_bytes / sizeof(u32); word++)
		iosys_map_wr(&resource->map, word * sizeof(u32), u32, spec->fill);
	for (word = 0; word < resource->guard_bytes / sizeof(u32); word++)
		iosys_map_wr(&resource->map,
			     resource->payload_bytes + word * sizeof(u32), u32,
			     resource->guard);
	return 0;

out_unpin:
	m9x_vram_unpin(vram);
out_put:
	drm_gem_vram_put(vram);
	return ret;
}

static int m9_triangle_alloc_one(struct m9x_device *mdev,
				 enum m9_triangle_resource_id id)
{
	const struct m9_triangle_resource_spec *spec = &m9_triangle_specs[id];
	struct m9_triangle_resource *resource =
		&mdev->transport.triangle->resource[id];
	struct device *dev = &mdev->pdev->dev;
	u32 guard_offset, alloc_bytes;
	u64 end;

	if (resource->cpu || resource->vram)
		return 0;
	/*
	 * BE, the class-1 BmBuffer, and C2 backing have exact vendor extents.
	 * Every byte after the submitted secondary image through 0x8000 is part
	 * of its zero tail.  Other diagnostic resources retain a guard page.
	 */
	if (id == M9_TRI_BE || id == M9_TRI_SECONDARY ||
	    id == M9_TRI_C2)
		guard_offset = spec->payload_bytes;
	else if (check_add_overflow(spec->payload_bytes, (u32)PAGE_SIZE,
				    &guard_offset))
		return -EOVERFLOW;
	alloc_bytes = PAGE_ALIGN(guard_offset);
	if (spec->local)
		return m9_triangle_alloc_vram(mdev, id, alloc_bytes);
	resource->cpu = dmam_alloc_coherent(dev, alloc_bytes, &resource->bus,
					    GFP_KERNEL | __GFP_ZERO);
	if (!resource->cpu)
		return -ENOMEM;
	if (upper_32_bits(resource->bus) ||
	    check_add_overflow((u64)resource->bus, (u64)alloc_bytes, &end) ||
	    end > (u64)U32_MAX + 1 || resource->bus & (spec->alignment - 1)) {
		dmam_free_coherent(dev, alloc_bytes, resource->cpu, resource->bus);
		resource->cpu = NULL;
		return -ERANGE;
	}

	resource->payload_bytes = spec->payload_bytes;
	resource->alloc_bytes = alloc_bytes;
	resource->guard_bytes = alloc_bytes - spec->payload_bytes;
	resource->guard = m9_triangle_guard(id);
	memset32(resource->cpu, spec->fill,
		 spec->payload_bytes / sizeof(u32));
	memset32((u32 *)((u8 *)resource->cpu + spec->payload_bytes),
		 resource->guard, resource->guard_bytes / sizeof(u32));
	return 0;
}

static int m9_triangle_verify_guards(const struct m9x_triangle_diag *diag)
{
	unsigned int id;

	for (id = 0; id < M9_TRI_RESOURCE_COUNT; id++) {
		const struct m9_triangle_resource *resource = &diag->resource[id];
		unsigned int word;

		for (word = 0; word < resource->guard_bytes / sizeof(u32); word++) {
			u32 value;

			if (resource->vram)
				value = iosys_map_rd(&resource->map,
					resource->payload_bytes + word * sizeof(u32),
					u32);
			else
				value = READ_ONCE(*(const u32 *)((const u8 *)
					resource->cpu + resource->payload_bytes +
					word * sizeof(u32)));
			if (value != resource->guard)
				return -EOVERFLOW;
		}
	}
	return 0;
}

static int m9_triangle_prepare_gpuva(struct m9x_device *mdev,
				     struct m9x_triangle_diag *diag)
{
	/*
	 * A class-1 BmBuffer's dispatcher address is its compact-PTE GPUVA.
	 * Its raw DMA address is only the backing-store/mmap address.
	 */
	static const u8 mapped[] = {
		M9_TRI_SOURCE, M9_TRI_SYSTEM, M9_TRI_SECONDARY,
	};
	u32 gpuva = M9_TRIANGLE_SYSTEM_GPUVA_BASE;
	u32 pte_count = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mapped); i++) {
		struct m9_triangle_resource *resource =
			&diag->resource[mapped[i]];
		u32 page_count = resource->alloc_bytes >> PAGE_SHIFT;
		u32 next_gpuva, next_pte_count;
		int ret;

		if (!resource->cpu || !resource->alloc_bytes ||
		    !IS_ALIGNED(resource->alloc_bytes, PAGE_SIZE) ||
		    !IS_ALIGNED(resource->bus, PAGE_SIZE) ||
		    check_add_overflow(pte_count, page_count, &next_pte_count) ||
		    next_pte_count > M9_TRIANGLE_MAX_SYSTEM_PTES ||
		    check_add_overflow(gpuva, resource->alloc_bytes, &next_gpuva) ||
		    next_gpuva > M9_TRIANGLE_SYSTEM_GPUVA_END)
			return -ERANGE;

		resource->gpuva = gpuva;
		ret = m9x_address_bind_dma(mdev, resource->gpuva, resource->bus,
					   resource->alloc_bytes);
		if (ret)
			return ret;
		pte_count = next_pte_count;
		gpuva = next_gpuva;
	}
	diag->pte_count = pte_count;
	return 0;
}

static int m9_triangle_prepare_resources(struct m9x_device *mdev)
{
	struct m9x_triangle_diag *diag = mdev->transport.triangle;
	static const u8 allocation_order[] = {
		M9_TRI_BE,
		M9_TRI_DEPTH, M9_TRI_AUX_DEPTH,
		M9_TRI_SYSTEM,
		M9_TRI_TRANSIENT0, M9_TRI_TRANSIENT1,
		M9_TRI_TRANSIENT2, M9_TRI_TRANSIENT3,
		M9_TRI_C2, M9_TRI_SEQ0_SCRATCH26,
		M9_TRI_SEQ0_SCRATCH27, M9_TRI_SEQ0_SCRATCH38,
		M9_TRI_SECONDARY, M9_TRI_SOURCE,
	};
	unsigned int id;
	int ret;

	if (diag && diag->prepared)
		return m9_triangle_verify_guards(diag);
	if (!diag) {
		diag = devm_kzalloc(&mdev->pdev->dev, sizeof(*diag), GFP_KERNEL);
		if (!diag)
			return -ENOMEM;
		mdev->transport.triangle = diag;
	}
	for (id = 0; id < ARRAY_SIZE(allocation_order); id++) {
		enum m9_triangle_resource_id resource = allocation_order[id];

		ret = m9_triangle_alloc_one(mdev, resource);
		if (ret) {
			dev_err(&mdev->pdev->dev,
				"M9 triangle %s allocation failed: %d\n",
				m9_triangle_specs[resource].name, ret);
			return ret;
		}
	}
	ret = m9_triangle_prepare_gpuva(mdev, diag);
	if (ret)
		return ret;
	memcpy(diag->resource[M9_TRI_SYSTEM].cpu, m9_triangle_system_prefix,
	       sizeof(m9_triangle_system_prefix));
	if (m9_triangle_gouraud)
		memcpy(diag->resource[M9_TRI_SOURCE].cpu,
		       m9_triangle_gouraud_source,
		       sizeof(m9_triangle_gouraud_source));
	else
		memcpy(diag->resource[M9_TRI_SOURCE].cpu,
		       m9_triangle_flat_source, sizeof(m9_triangle_flat_source));
	dma_wmb();
	diag->prepared = true;
	dev_info(&mdev->pdev->dev,
		 "M9 triangle resources: source=%pad/%#x system=%pad/%#x secondary=%pad/%#x, %u PTEs staged\n",
		 &diag->resource[M9_TRI_SOURCE].bus,
		 diag->resource[M9_TRI_SOURCE].gpuva,
		 &diag->resource[M9_TRI_SYSTEM].bus,
		 diag->resource[M9_TRI_SYSTEM].gpuva,
		 &diag->resource[M9_TRI_SECONDARY].bus,
		 diag->resource[M9_TRI_SECONDARY].gpuva, diag->pte_count);
	return m9_triangle_verify_guards(diag);
}

static u32 m9_triangle_bus(const struct m9x_triangle_diag *diag,
			   enum m9_triangle_resource_id id)
{
	return lower_32_bits(diag->resource[id].bus);
}

static u32 m9_triangle_gpuva(const struct m9x_triangle_diag *diag,
			     enum m9_triangle_resource_id id)
{
	return diag->resource[id].gpuva;
}

static int m9_seq0_build_job(const struct m9x_device *mdev, u32 target,
			     struct m9_seq0_job *job)
{
	const struct m9x_triangle_diag *diag = mdev->transport.triangle;
	u64 secondary_end;

	job->secondary = m9_triangle_gpuva(diag, M9_TRI_SECONDARY);
	job->target = target;
	job->scratch26 = m9_triangle_bus(diag, M9_TRI_SEQ0_SCRATCH26);
	job->scratch27 = m9_triangle_bus(diag, M9_TRI_SEQ0_SCRATCH27);
	job->scratch38 = m9_triangle_bus(diag, M9_TRI_SEQ0_SCRATCH38);
	job->completion = lower_32_bits(mdev->transport.fence_bus);

	if (!IS_ALIGNED(target, 0x10) ||
	    !IS_ALIGNED(job->secondary, sizeof(u32)) ||
	    check_add_overflow((u64)job->secondary,
			       (u64)M9_TRIANGLE_SECONDARY_BYTES,
			       &secondary_end) || secondary_end > (u64)U32_MAX + 1 ||
	    !IS_ALIGNED(job->scratch26, SZ_4K) ||
	    !IS_ALIGNED(job->scratch27, SZ_4K) ||
	    !IS_ALIGNED(job->scratch38, SZ_4K) ||
	    !IS_ALIGNED(job->completion, 0x10))
		return -ERANGE;
	return 0;
}

static u32 m9_seq0_patch_value(const struct m9_seq0_job *job,
			       const struct m9_seq0_patch *patch)
{
	u32 value;

	switch (patch->operand) {
	case M9_SEQ0_TARGET:
		value = job->target + patch->step * 0x10;
		break;
	case M9_SEQ0_SCRATCH26:
		value = job->scratch26;
		break;
	case M9_SEQ0_SCRATCH27:
		value = job->scratch27;
		break;
	case M9_SEQ0_SCRATCH38:
		value = job->scratch38;
		break;
	case M9_SEQ0_COMPLETION:
		value = job->completion;
		break;
	default:
		return 0;
	}
	return value | patch->tag;
}

static void m9_seq0_build_secondary(u8 *image,
				    const struct m9_seq0_job *job)
{
	unsigned int i;

	memset(image, 0, M9_TRIANGLE_SECONDARY_BYTES);
	memcpy(image + M9_TRIANGLE_SECONDARY_PREFIX,
	       m9_seq0_framed_payload, sizeof(m9_seq0_framed_payload));
	for (i = 0; i < ARRAY_SIZE(m9_seq0_patches); i++)
		put_unaligned_le32(m9_seq0_patch_value(job,
						       &m9_seq0_patches[i]),
				   image + m9_seq0_patches[i].offset);
}

static void m9_seq0_build_primary(const struct m9_seq0_job *job, u32 *primary)
{
	static const u16 bounds[][2] = {
		{ 0x008, 0x540 },
		{ 0x540, 0xf74 },
		{ 0xf74, 0xfa0 },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(bounds); i++) {
		primary[i * 3] = 0x45452d2c;
		primary[i * 3 + 1] = (job->secondary + bounds[i][0]) |
			M9_TRIANGLE_SECONDARY_START_TAG;
		primary[i * 3 + 2] = (job->secondary + bounds[i][1]) |
			M9_TRIANGLE_SECONDARY_END_TAG;
	}
}

static int m9_triangle_build_addresses(struct m9x_device *mdev, u32 color,
				       u32 target,
				       struct m9_triangle_job_addresses *job)
{
	const struct m9x_triangle_diag *diag = mdev->transport.triangle;
	unsigned int i;

	if (!m9_triangle_gpuva(diag, M9_TRI_SOURCE) ||
	    !m9_triangle_gpuva(diag, M9_TRI_SYSTEM) ||
	    !IS_ALIGNED(color, 0x100))
		return -EINVAL;
	job->source = m9_triangle_gpuva(diag, M9_TRI_SOURCE) |
		      M9_TRIANGLE_GPUVA_TAG;
	job->secondary = m9_triangle_gpuva(diag, M9_TRI_SECONDARY);
	job->system_pre_start = (m9_triangle_gpuva(diag, M9_TRI_SYSTEM) + 0x10) |
				M9_TRIANGLE_GPUVA_TAG;
	job->system_pre_end = (m9_triangle_gpuva(diag, M9_TRI_SYSTEM) + 0xc0) |
			      M9_TRIANGLE_GPUVA_TAG;
	job->system_raster_start = m9_triangle_gpuva(diag, M9_TRI_SYSTEM) |
				   M9_TRIANGLE_GPUVA_TAG;
	job->system_raster_end = (m9_triangle_gpuva(diag, M9_TRI_SYSTEM) + 0x10) |
				 M9_TRIANGLE_GPUVA_TAG;
	job->linked = (m9_triangle_gpuva(diag, M9_TRI_SYSTEM) +
		       M9_TRIANGLE_LINKED_OFFSET) | M9_TRIANGLE_GPUVA_TAG;
	job->c2 = m9_triangle_bus(diag, M9_TRI_C2) +
		  M9_TRIANGLE_C2_LEADING_BYTES;
	job->be = m9_triangle_bus(diag, M9_TRI_BE);
	for (i = 0; i < ARRAY_SIZE(job->transient); i++)
		job->transient[i] = m9_triangle_bus(diag,
						M9_TRI_TRANSIENT0 + i);
	job->color = color;
	job->depth = m9_triangle_bus(diag, M9_TRI_DEPTH);
	job->aux_depth = m9_triangle_bus(diag, M9_TRI_AUX_DEPTH);
	job->fence = lower_32_bits(mdev->transport.fence_bus) |
		     M9_TRIANGLE_COMPLETION_TAG;
	for (i = 0; i < ARRAY_SIZE(job->transition); i++)
		job->transition[i] = target + i * 0x10;
	return 0;
}

static u32 m9_triangle_operand_value(const struct m9_triangle_job_addresses *job,
				     enum m9_triangle_operand operand)
{
	switch (operand) {
	case M9_TRI_OP_SOURCE: return job->source;
	case M9_TRI_OP_SYSTEM_PRE_START: return job->system_pre_start;
	case M9_TRI_OP_SYSTEM_PRE_END: return job->system_pre_end;
	case M9_TRI_OP_SYSTEM_RASTER_START: return job->system_raster_start;
	case M9_TRI_OP_SYSTEM_RASTER_END: return job->system_raster_end;
	case M9_TRI_OP_LINKED: return job->linked;
	case M9_TRI_OP_C2: return job->c2;
	case M9_TRI_OP_BE: return job->be;
	case M9_TRI_OP_TRANSIENT0: return job->transient[0];
	case M9_TRI_OP_TRANSIENT1: return job->transient[1];
	case M9_TRI_OP_TRANSIENT2: return job->transient[2];
	case M9_TRI_OP_TRANSIENT3: return job->transient[3];
	case M9_TRI_OP_COLOR: return job->color;
	case M9_TRI_OP_DEPTH: return job->depth;
	case M9_TRI_OP_AUX_DEPTH: return job->aux_depth;
	case M9_TRI_OP_FENCE: return job->fence;
	}
	return 0;
}

static u32 *m9_triangle_command_range(struct m9_triangle_commands *commands,
				      u8 range)
{
	if (range == 0)
		return commands->preprocess;
	if (range == 1)
		return commands->raster;
	return commands->completion;
}

static void m9_triangle_build_commands(
		const struct m9_triangle_job_addresses *job,
		bool gouraud,
		struct m9_triangle_commands *commands)
{
	unsigned int i;

	memcpy(commands->preprocess, m9_triangle_range_0,
	       sizeof(commands->preprocess));
	memcpy(commands->raster, m9_triangle_range_1,
	       sizeof(commands->raster));
	memcpy(commands->completion, m9_triangle_range_2,
	       sizeof(commands->completion));
	/*
	 * The capture leaked inactive MRT stack residue into AB/AC.  The vendor
	 * one-render-target compiler state is deterministic all-ones.
	 */
	commands->raster[37] = U32_MAX;
	commands->raster[38] = U32_MAX;
	if (gouraud) {
		commands->raster[M9_TRIANGLE_RASTER_SHADE_CONTROL_DW] = 0x00002503;
		commands->raster[M9_TRIANGLE_RASTER_SHADE_LINK0_DW] = 0x00100d88;
		commands->raster[M9_TRIANGLE_RASTER_SHADE_LINK1_DW] = 0x001006b6;
	}
	for (i = 0; i < ARRAY_SIZE(m9_triangle_reloc_sites); i++) {
		const struct m9_triangle_reloc_site *site =
			&m9_triangle_reloc_sites[i];

		m9_triangle_command_range(commands, site->range)[site->dword] =
			m9_triangle_operand_value(job, site->operand);
	}
	for (i = 0; i < ARRAY_SIZE(m9_triangle_transition_sites); i++) {
		const struct m9_triangle_transition_site *site =
			&m9_triangle_transition_sites[i];
		u32 value = job->transition[site->transition];

		if (site->tag)
			value |= 1;
		m9_triangle_command_range(commands, site->range)[site->dword] = value;
	}
}

static void m9_triangle_build_framed_secondary(
		u8 *image,
		const struct m9_triangle_job_addresses *job,
		bool gouraud)
{
	unsigned int i;

	/* The saved BmBuffer's first eight bytes are private host bookkeeping and
	 * are never included in a GPU range.  Keep the same displacement while
	 * refusing to retain the captured process pointer. */
	memset(image, 0, M9_TRIANGLE_SECONDARY_BYTES);
	memcpy(image + M9_TRIANGLE_SECONDARY_PREFIX,
	       m9_triangle_framed_payload, sizeof(m9_triangle_framed_payload));
	put_unaligned_le32(U32_MAX, image + M9_TRIANGLE_FRAMED_MRT0);
	put_unaligned_le32(U32_MAX, image + M9_TRIANGLE_FRAMED_MRT1);
	if (gouraud) {
		put_unaligned_le32(0x00002503,
				   image + M9_TRIANGLE_FRAMED_SHADE_CONTROL);
		put_unaligned_le32(0x00100d88,
				   image + M9_TRIANGLE_FRAMED_SHADE_LINK0);
		put_unaligned_le32(0x001006b6,
				   image + M9_TRIANGLE_FRAMED_SHADE_LINK1);
	}
	for (i = 0; i < ARRAY_SIZE(m9_triangle_reloc_sites); i++)
		put_unaligned_le32(m9_triangle_operand_value(job,
			m9_triangle_reloc_sites[i].operand),
			image + m9_triangle_framed_reloc_offsets[i]);
	for (i = 0; i < ARRAY_SIZE(m9_triangle_transition_sites); i++) {
		const struct m9_triangle_transition_site *site =
			&m9_triangle_transition_sites[i];
		u32 value = job->transition[site->transition];

		if (site->tag)
			value |= 1;
		put_unaligned_le32(value,
			image + m9_triangle_framed_transition_offsets[i]);
	}
}

static void m9_triangle_build_primary(
		const struct m9_triangle_job_addresses *job, u32 *primary)
{
	static const u16 bounds[][2] = {
		{ 0x008, M9_TRIANGLE_FRAMED_RANGE0_END },
		{ M9_TRIANGLE_FRAMED_RANGE0_END,
		  M9_TRIANGLE_FRAMED_RANGE1_END },
		{ M9_TRIANGLE_FRAMED_RANGE1_END,
		  M9_TRIANGLE_FRAMED_RANGE2_END },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(bounds); i++) {
		primary[i * 3] = 0x45452d2c;
		primary[i * 3 + 1] = (job->secondary + bounds[i][0]) |
			M9_TRIANGLE_SECONDARY_START_TAG;
		primary[i * 3 + 2] = (job->secondary + bounds[i][1]) |
			M9_TRIANGLE_SECONDARY_END_TAG;
	}
}

static int m9_triangle_build_initial_transition(u32 target, u32 *primary)
{
	if (!IS_ALIGNED(target, 0x10))
		return -EINVAL;
	primary[0] = 0x454545e6;
	primary[1] = 0x00000002;
	primary[2] = M9_PACKET_FENCE_HEADER;
	primary[3] = 3;
	primary[4] = target | 1;
	return 0;
}

static int m9_triangle_build_final_transition(u32 context_state,
					      u32 barrier, u32 done,
					      u32 *primary)
{
	if (!IS_ALIGNED(barrier, 0x10) || !IS_ALIGNED(done, 0x10) ||
	    done != barrier + 0x10)
		return -EINVAL;
	primary[0] = 0x45454747;
	primary[1] = 0x000011a4;
	primary[2] = 0x00001308;
	primary[3] = M9_PACKET_FENCE_HEADER;
	primary[4] = 3;
	primary[5] = barrier | 1;
	primary[6] = 0x45454543;
	primary[7] = barrier;
	primary[8] = 0x454522e6;
	primary[9] = 0x40000000;
	primary[10] = context_state | 0x20000000;
	primary[11] = M9_PACKET_FENCE_HEADER;
	primary[12] = 3;
	primary[13] = done | 1;
	return 0;
}

static int m9_triangle_transition_selftest(void)
{
	static const u32 expected_initial[] = {
		0x454545e6, 0x00000002,
		0x4545e3e6, 0x00000003, 0x00000011,
	};
	static const u32 expected_final[] = {
		0x45454747, 0x000011a4, 0x00001308,
		0x4545e3e6, 0x00000003, 0x00000021,
		0x45454543, 0x00000020,
		0x454522e6, 0x40000000, 0x32345678,
		0x4545e3e6, 0x00000003, 0x00000031,
	};
	u32 initial[ARRAY_SIZE(expected_initial)];
	u32 final[ARRAY_SIZE(expected_final)];

	if (m9_triangle_build_initial_transition(0x10, initial) ||
	    memcmp(initial, expected_initial, sizeof(initial)) ||
	    !m9_triangle_build_initial_transition(0x11, initial) ||
	    m9_triangle_build_final_transition(0x12345678, 0x20, 0x30,
					       final) ||
	    memcmp(final, expected_final, sizeof(final)) ||
	    !m9_triangle_build_final_transition(0x12345678, 0x20, 0x20,
						final))
		return -EINVAL;
	return 0;
}

static u32 m9_triangle_captured_operand(enum m9_triangle_operand operand)
{
	switch (operand) {
	case M9_TRI_OP_SOURCE: return 0x40f52001;
	case M9_TRI_OP_SYSTEM_PRE_START: return 0x40c3a011;
	case M9_TRI_OP_SYSTEM_PRE_END: return 0x40c3a0c1;
	case M9_TRI_OP_SYSTEM_RASTER_START: return 0x40c3a001;
	case M9_TRI_OP_SYSTEM_RASTER_END: return 0x40c3a011;
	case M9_TRI_OP_LINKED: return 0x40c3a301;
	case M9_TRI_OP_C2: return 0x11f91000;
	case M9_TRI_OP_BE: return 0x112c1000;
	case M9_TRI_OP_TRANSIENT0: return 0x113c1000;
	case M9_TRI_OP_TRANSIENT1: return 0x113ee000;
	case M9_TRI_OP_TRANSIENT2: return 0x11404800;
	case M9_TRI_OP_TRANSIENT3: return 0x113d7800;
	case M9_TRI_OP_COLOR: return 0x11ee9000;
	case M9_TRI_OP_DEPTH: return 0x11f39000;
	case M9_TRI_OP_AUX_DEPTH: return 0x11f89000;
	case M9_TRI_OP_FENCE: return 0x13861002;
	}
	return 0;
}

static void m9_triangle_build_linked_record(u8 *record, bool gouraud)
{
	/* Pool type 5 copies this backing byte-for-byte.  Linux allocates 0x200
	 * bytes, but only the first 0x80 are semantic; the apparent host pointers
	 * in captured upper bytes were uninitialized allocator residue. */
	memset(record, 0, M9_TRIANGLE_LINKED_BYTES);
	memcpy(record, m9_triangle_linked_prefix,
	       sizeof(m9_triangle_linked_prefix));
	if (!gouraud) {
		unsigned int i;

		/* Thirteen independent fresh-process captures prove this bounded
		 * first-draw schedule for the case-B linked image, including flat
		 * shading.  Each of the three arena references is duplicated. */
		for (i = 0; i < 3; i++) {
			u32 ref = M9_TRIANGLE_LINKED_ARENA_LIMIT -
				  M9_TRIANGLE_LINKED_FIRST_BACKOFF +
				  i * M9_TRIANGLE_LINKED_REF_STRIDE;

			put_unaligned_le32(ref, record + 0x24 + i * 0x28);
			put_unaligned_le32(ref, record + 0x2c + i * 0x28);
		}
	}

}

static int m9_triangle_serialize_linked_record(struct m9x_device *mdev,
				const struct m9_triangle_job_addresses *job)
{
	struct m9_triangle_resource *system =
		&mdev->transport.triangle->resource[M9_TRI_SYSTEM];
	u8 *record = (u8 *)system->cpu + M9_TRIANGLE_LINKED_OFFSET;

	m9_triangle_build_linked_record(record, m9_triangle_gouraud);
	(void)job;
	dma_wmb();
	return 0;
}

static int m9_triangle_run_manager_transition(struct m9x_device *mdev,
					      u64 *point)
{
	u32 initial_primary[5];
	u32 transition_primary[14];
	u32 barrier, context_state, done, target;
	int ret;

	/*
	 * m9x421 has two real flush boundaries: the initial E6 packet precedes
	 * the direct window writes, and the final transition follows them.
	 */
	target = mdev->transport.next_fence;
	ret = m9_triangle_build_initial_transition(target, initial_primary);
	if (ret)
		return ret;
	ret = m9x_submit_kernel_stream_locked(mdev, initial_primary,
					      ARRAY_SIZE(initial_primary),
					      target, point);
	if (ret)
		return ret;
	mdev->transport.next_fence = target + 0x10;

	ret = m9x_address_publish(mdev);
	if (ret)
		return ret;

	barrier = mdev->transport.next_fence;
	done = barrier + 0x10;
	context_state = m9x_read(mdev, M9_REG_CONTEXT_STATE);
	ret = m9_triangle_build_final_transition(context_state, barrier, done,
						 transition_primary);
	if (ret)
		return ret;
	ret = m9x_submit_kernel_stream_locked(mdev, transition_primary,
					      ARRAY_SIZE(transition_primary),
					      done, point);
	if (ret)
		return ret;
	mdev->transport.next_fence = done + 0x10;
	return 0;
}

int m9x_submit_triangle_diag(struct m9x_device *mdev, u32 color,
			     struct drm_gem_vram_object *color_bo, u64 *point,
			     bool *color_active)
{
	struct m9_triangle_job_addresses job = {};
	struct m9_seq0_job sequence0 = {};
	struct m9x_triangle_diag *diag;
	struct m9x_fence *out_fence = NULL;
	u32 noop_primary[3];
	u32 draw_target;
	u32 sequence0_final = 0;
	u32 target;
	bool initialized_now = false;
	int ret;

	*point = 0;
	*color_active = false;
	if (!m9_triangle_diag)
		return -EACCES;
	mutex_lock(&mdev->transport.lock);
	if (mdev->transport.wedged) {
		ret = -EIO;
		goto out_unlock;
	}
	ret = m9_triangle_prepare_resources(mdev);
	if (ret)
		goto out_unlock;
	ret = m9x_address_prepare(mdev);
	if (ret)
		goto out_unlock;
	diag = mdev->transport.triangle;
	if (!mdev->transport.engine_transitioned) {
		/*
		 * The vendor arms the stable requester, completion and restore
		 * addresses before executing m9x421.  Its first complete transition
		 * precedes compact publication and m9x1187 executes the second one
		 * afterward.  A preceding diagnostic no-op may have armed the
		 * requester with restore disabled, so publish the recovered register
		 * tuple again before the first transition.
		 */
		mdev->transport.restore_ready = true;
		ret = m9x_transport_rearm_locked(mdev);
		if (ret)
			goto out_unlock;
		ret = m9_triangle_run_manager_transition(mdev, point);
		if (ret)
			goto out_unlock;
		ret = m9x_address_commit_bindings(mdev);
		if (ret)
			goto out_unlock;
		ret = m9_triangle_run_manager_transition(mdev, point);
		if (ret)
			goto out_unlock;
		mdev->transport.engine_transitioned = true;
	}

	if (!diag->context_initialized) {
		/*
		 * Complete traces contain this large first ExecuteDma envelope
		 * exactly once per context.  Its three scratch objects remain live
		 * with the context; replaying it before every draw does not match the
		 * vendor TaskMgr and can destroy incremental state.
		 *
		 * Keep the captured inert completion between engine publication and
		 * first activation.  The secondary BO is shared, so activation must
		 * retire before the draw image replaces it.
		 */
		target = mdev->transport.next_fence;
		noop_primary[0] = M9_PACKET_FENCE_HEADER;
		noop_primary[1] = 3;
		noop_primary[2] = target | 1;
		ret = m9x_submit_kernel_stream_locked(mdev, noop_primary,
						      ARRAY_SIZE(noop_primary),
						      target, point);
		if (ret)
			goto out_unlock;
		mdev->transport.next_fence = target + 0x10;

		ret = m9_seq0_build_job(mdev, mdev->transport.next_fence,
					&sequence0);
		if (ret)
			goto out_unlock;
		sequence0_final = sequence0.target +
					  M9_SEQ0_FINAL_TARGET_OFFSET;
		m9_seq0_build_secondary(diag->resource[M9_TRI_SECONDARY].cpu,
					&sequence0);
		m9_seq0_build_primary(&sequence0,
				      diag->commands.sequence0_primary);
		WRITE_ONCE(*(u32 *)mdev->transport.fence_cpu, ~sequence0_final);
		ret = m9x_submit_kernel_stream_locked(mdev,
						      diag->commands.sequence0_primary,
						      ARRAY_SIZE(diag->commands.sequence0_primary),
						      sequence0_final, point);
		if (ret)
			goto out_unlock;
		mdev->transport.next_fence = sequence0.target +
					    M9_SEQ0_NEXT_TARGET_OFFSET;
		ret = m9_triangle_verify_guards(diag);
		if (ret) {
			mdev->transport.wedged = true;
			mdev->transport.generation++;
			goto out_unlock;
		}
		diag->context_initialized = true;
		initialized_now = true;
	}

	draw_target = mdev->transport.next_fence;
	ret = m9_triangle_build_addresses(mdev, color, draw_target, &job);
	if (ret)
		goto out_unlock;
	m9_triangle_build_commands(&job, m9_triangle_gouraud, &diag->commands);
	m9_triangle_build_primary(&job, diag->commands.primary);
	ret = m9_triangle_serialize_linked_record(mdev, &job);
	if (ret)
		goto out_unlock;

	/*
	 * First context activation, when needed, has retired before this path,
	 * so the shared BmBuffer can hold the exact three-range draw image.  Its
	 * final in-stream completion retires every referenced allocation.
	 */
	m9_triangle_build_framed_secondary(diag->resource[M9_TRI_SECONDARY].cpu,
					   &job, m9_triangle_gouraud);
	dma_wmb();
	dma_resv_assert_held(color_bo->bo.base.resv);
	out_fence = m9x_fence_create_target_locked(mdev, job.transition[5]);
	if (!out_fence) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	dma_resv_add_fence(color_bo->bo.base.resv, &out_fence->base,
			   DMA_RESV_USAGE_WRITE);
	*color_active = true;
	ret = m9x_submit_kernel_stream_locked(mdev, diag->commands.primary,
					      ARRAY_SIZE(diag->commands.primary),
					      job.transition[5], point);
	if (ret)
		goto out_unlock;
	*color_active = false;
	mdev->transport.next_fence = job.transition[5] + 0x10;
	ret = m9_triangle_verify_guards(diag);
	if (ret) {
		mdev->transport.wedged = true;
		mdev->transport.generation++;
		goto out_unlock;
	}
	if (initialized_now)
		dev_info(&mdev->pdev->dev,
			 "M9 context first activation retired: setup=%#x..%#x\n",
			 sequence0.target, sequence0_final);
	dev_info(&mdev->pdev->dev,
		 "M9 triangle retired: draw=%#x..%#x\n", draw_target,
		 job.transition[5]);
out_unlock:
	if (out_fence && ret)
		dma_fence_set_error(&out_fence->base, ret);
	if (out_fence && *color_active) {
		if (!mdev->transport.wedged) {
			mdev->transport.wedged = true;
			mdev->transport.generation++;
		}
		WARN_ON(mdev->transport.quarantined_fence ||
			mdev->transport.quarantined_color);
		mdev->transport.quarantined_fence = out_fence;
		mdev->transport.quarantined_color = color_bo;
		out_fence = NULL;
	} else if (out_fence) {
		dma_fence_signal(&out_fence->base);
	}
	if (out_fence)
		dma_fence_put(&out_fence->base);
	mutex_unlock(&mdev->transport.lock);
	return ret;
}

static int m9_seq0_selftest(void)
{
	struct m9_triangle_job_addresses draw = {};
	struct m9_seq0_job job = {
		.secondary = 0x02000000,
		.target = 0x00000100,
		.scratch26 = 0x11000000,
		.scratch27 = 0x11200000,
		.scratch38 = 0x11400000,
		.completion = 0x02400000,
	};
	static const u32 expected_primary[] = {
		0x45452d2c, 0x02000009, 0x02000542,
		0x45452d2c, 0x02000541, 0x02000f76,
		0x45452d2c, 0x02000f75, 0x02000fa2,
	};
	u32 primary[ARRAY_SIZE(expected_primary)];
	u8 *image;
	unsigned int i;
	int ret = -EINVAL;

	if (ARRAY_SIZE(m9_seq0_patches) != 17 ||
	    ARRAY_SIZE(m9_seq0_patch_offsets) !=
		ARRAY_SIZE(m9_seq0_patches) ||
	    sizeof(m9_seq0_framed_payload) !=
		M9_SEQ0_SECONDARY_IMAGE_BYTES -
		M9_TRIANGLE_SECONDARY_PREFIX ||
	    m9_triangle_template_fnv1a(m9_seq0_framed_payload,
		ARRAY_SIZE(m9_seq0_framed_payload)) !=
		0xd4169ed545a19999ULL)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(m9_seq0_patches); i++) {
		const struct m9_seq0_patch *patch = &m9_seq0_patches[i];
		u32 captured = get_unaligned_le32(
			(const u8 *)m9_seq0_framed_payload + patch->offset -
			M9_TRIANGLE_SECONDARY_PREFIX);

		if (patch->offset != m9_seq0_patch_offsets[i] ||
		    patch->offset < M9_TRIANGLE_SECONDARY_PREFIX ||
		    patch->offset >
			M9_SEQ0_SECONDARY_IMAGE_BYTES - sizeof(u32) ||
		    patch->operand > M9_SEQ0_COMPLETION ||
		    patch->step >= M9_SEQ0_TARGET_COUNT ||
		    captured != patch->captured)
			return -EINVAL;
		if ((patch->operand == M9_SEQ0_TARGET &&
		     patch->index != 0xe3 && patch->index != 0x43) ||
		    (patch->operand >= M9_SEQ0_SCRATCH26 &&
		     patch->operand <= M9_SEQ0_SCRATCH38 &&
		     patch->index != 0xb0) ||
		    (patch->operand == M9_SEQ0_COMPLETION &&
		     patch->index != 0x1b))
			return -EINVAL;
	}

	image = kzalloc(M9_TRIANGLE_SECONDARY_BYTES, GFP_KERNEL);
	if (!image)
		return -ENOMEM;
	m9_seq0_build_secondary(image, &job);
	m9_seq0_build_primary(&job, primary);
	if (memcmp(primary, expected_primary, sizeof(primary)) ||
	    m9_triangle_template_fnv1a((const u32 *)image,
		M9_SEQ0_SECONDARY_IMAGE_BYTES / sizeof(u32)) !=
		0xfd653b6594219969ULL)
		goto out;
	for (i = 0; i < ARRAY_SIZE(m9_seq0_patches); i++)
		if (get_unaligned_le32(image + m9_seq0_patches[i].offset) !=
		    m9_seq0_patch_value(&job, &m9_seq0_patches[i]))
			goto out;
	for (i = M9_SEQ0_SECONDARY_IMAGE_BYTES;
	     i < M9_TRIANGLE_SECONDARY_BYTES; i++)
		if (image[i])
			goto out;

	/* Keep the separately captured draw serializer covered even though the
	 * shared secondary BO cannot receive it until sequence 0 retires. */
	m9_triangle_build_framed_secondary(image, &draw, false);
	if (get_unaligned_le32(image) || get_unaligned_le32(image + 4) ||
	    get_unaligned_le32(image + M9_TRIANGLE_FRAMED_MRT0) != U32_MAX ||
	    get_unaligned_le32(image + M9_TRIANGLE_FRAMED_MRT1) != U32_MAX ||
	    get_unaligned_le32(image + M9_TRIANGLE_FRAMED_SHADE_CONTROL) !=
		0x00006403 ||
	    get_unaligned_le32(image + M9_TRIANGLE_FRAMED_SHADE_LINK0) !=
		0x80100d88 ||
	    get_unaligned_le32(image + M9_TRIANGLE_FRAMED_SHADE_LINK1) !=
		0x801006b6)
		goto out;
	m9_triangle_build_framed_secondary(image, &draw, true);
	if (get_unaligned_le32(image + M9_TRIANGLE_FRAMED_SHADE_CONTROL) !=
		0x00002503 ||
	    get_unaligned_le32(image + M9_TRIANGLE_FRAMED_SHADE_LINK0) !=
		0x00100d88 ||
	    get_unaligned_le32(image + M9_TRIANGLE_FRAMED_SHADE_LINK1) !=
		0x001006b6)
		goto out;
	for (i = M9_TRIANGLE_FRAMED_RANGE2_END;
	     i < M9_TRIANGLE_SECONDARY_BYTES; i++)
		if (image[i])
			goto out;
	ret = 0;
out:
	kfree(image);
	return ret;
}

static int
m9_triangle_stream_selftest(struct m9_triangle_commands *commands)
{
	struct m9_triangle_job_addresses job = {};
	u8 linked[M9_TRIANGLE_LINKED_BYTES] = {};
	u32 target = 0xfffffff0;
	u64 linked_hash;
	u64 system_hash;
	unsigned int i;
	int ret;

	ret = m9_seq0_selftest();
	if (ret)
		return ret;
	ret = m9_triangle_transition_selftest();
	if (ret)
		return ret;

	if (sizeof(m9_triangle_flat_source) != M9_TRIANGLE_SOURCE_BYTES ||
	    sizeof(m9_triangle_gouraud_source) != M9_TRIANGLE_SOURCE_BYTES ||
	    sizeof(m9_triangle_system_prefix) != 0xc0 ||
	    M9_TRIANGLE_MAX_SYSTEM_PTES != 44 ||
	    M9_TRIANGLE_AUX_DEPTH_BYTES != 0x2d00 ||
	    m9_triangle_specs[M9_TRI_SECONDARY].payload_bytes != SZ_32K ||
	    m9_triangle_specs[M9_TRI_C2].payload_bytes !=
		M9_TRIANGLE_C2_BACKING_BYTES ||
	    ARRAY_SIZE(m9_triangle_range_0) != ARRAY_SIZE(commands->preprocess) ||
	    ARRAY_SIZE(m9_triangle_range_1) != ARRAY_SIZE(commands->raster) ||
	    ARRAY_SIZE(m9_triangle_range_2) != ARRAY_SIZE(commands->completion) ||
	    M9_TRIANGLE_SURFACE_BYTES != 0x50000 ||
	    M9_TRIANGLE_LINKED_OFFSET & 0x7f ||
	    M9_TRIANGLE_LINKED_OFFSET + M9_TRIANGLE_LINKED_BYTES >
		M9_TRIANGLE_SYSTEM_BYTES ||
	    ARRAY_SIZE(m9_triangle_specs) != M9_TRI_RESOURCE_COUNT ||
	    ARRAY_SIZE(m9_triangle_reloc_sites) != 22 ||
	    ARRAY_SIZE(m9_triangle_transition_sites) != 8 ||
	    ARRAY_SIZE(m9_triangle_framed_reloc_offsets) !=
		ARRAY_SIZE(m9_triangle_reloc_sites) ||
	    ARRAY_SIZE(m9_triangle_framed_transition_offsets) !=
		ARRAY_SIZE(m9_triangle_transition_sites) ||
	    sizeof(m9_triangle_framed_payload) !=
		M9_TRIANGLE_FRAMED_RANGE2_END - M9_TRIANGLE_SECONDARY_PREFIX ||
	    m9_triangle_specs[M9_TRI_SEQ0_SCRATCH26].payload_bytes != 0x800 ||
	    m9_triangle_specs[M9_TRI_SEQ0_SCRATCH27].payload_bytes != 0x20080 ||
	    m9_triangle_specs[M9_TRI_SEQ0_SCRATCH38].payload_bytes != 0x400 ||
	    m9_triangle_specs[M9_TRI_BE].payload_bytes != SZ_1M)
		return -EINVAL;
	linked_hash = m9_triangle_template_fnv1a(m9_triangle_linked_prefix,
						 ARRAY_SIZE(m9_triangle_linked_prefix));
	if (linked_hash != 0xf671348f86ae78e5ULL)
		return -EINVAL;
	system_hash = m9_triangle_template_fnv1a(m9_triangle_system_prefix,
						 ARRAY_SIZE(m9_triangle_system_prefix));
	if (system_hash != 0xab2075c3793cfdc5ULL)
		return -EINVAL;
	m9_triangle_build_linked_record(linked, false);
	if (get_unaligned_le32(linked + 0x24) != 0x007ff880 ||
	    get_unaligned_le32(linked + 0x2c) != 0x007ff880 ||
	    get_unaligned_le32(linked + 0x4c) != 0x007ff8a8 ||
	    get_unaligned_le32(linked + 0x54) != 0x007ff8a8 ||
	    get_unaligned_le32(linked + 0x74) != 0x007ff8d0 ||
	    get_unaligned_le32(linked + 0x7c) != 0x007ff8d0)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(m9_triangle_specs); i++)
		if (m9_triangle_specs[i].payload_bytes & 3 ||
		    !is_power_of_2(m9_triangle_specs[i].alignment))
			return -EINVAL;
	memcpy(commands->preprocess, m9_triangle_range_0,
	       sizeof(commands->preprocess));
	memcpy(commands->raster, m9_triangle_range_1, sizeof(commands->raster));
	memcpy(commands->completion, m9_triangle_range_2,
	       sizeof(commands->completion));
	for (i = 0; i < ARRAY_SIZE(m9_triangle_reloc_sites); i++) {
		const struct m9_triangle_reloc_site *site =
			&m9_triangle_reloc_sites[i];
		u16 framed = m9_triangle_framed_reloc_offsets[i];

		if (m9_triangle_reloc_sites[i].range > 2 ||
		    m9_triangle_reloc_sites[i].operand > M9_TRI_OP_FENCE ||
		    m9_triangle_command_range(commands, site->range)[site->dword] !=
			m9_triangle_captured_operand(site->operand) ||
		    framed < M9_TRIANGLE_SECONDARY_PREFIX ||
		    framed > M9_TRIANGLE_FRAMED_RANGE2_END - sizeof(u32) ||
		    get_unaligned_le32((const u8 *)m9_triangle_framed_payload +
			framed - M9_TRIANGLE_SECONDARY_PREFIX) !=
			m9_triangle_captured_operand(site->operand))
			return -EINVAL;
	}
	for (i = 0; i < ARRAY_SIZE(m9_triangle_transition_sites); i++) {
		const struct m9_triangle_transition_site *site =
			&m9_triangle_transition_sites[i];
		u32 captured = 0x270 + site->transition * 0x10;
		u16 framed = m9_triangle_framed_transition_offsets[i];

		if (m9_triangle_transition_sites[i].range > 2 ||
		    m9_triangle_transition_sites[i].transition > 5 ||
		    m9_triangle_command_range(commands, site->range)[site->dword] !=
			(captured | site->tag) ||
		    framed < M9_TRIANGLE_SECONDARY_PREFIX ||
		    framed > M9_TRIANGLE_FRAMED_RANGE2_END - sizeof(u32) ||
		    get_unaligned_le32((const u8 *)m9_triangle_framed_payload +
			framed - M9_TRIANGLE_SECONDARY_PREFIX) !=
			(captured | site->tag))
			return -EINVAL;
	}
	if (m9_triangle_template_fnv1a(m9_triangle_range_0,
				       ARRAY_SIZE(m9_triangle_range_0)) !=
				       0x46aefda60abba6c0ULL ||
	    m9_triangle_template_fnv1a(m9_triangle_range_1,
				       ARRAY_SIZE(m9_triangle_range_1)) !=
				       0x261e2794915380e0ULL ||
	    m9_triangle_template_fnv1a(m9_triangle_range_2,
				       ARRAY_SIZE(m9_triangle_range_2)) !=
				       0x726d161270583420ULL ||
	    m9_triangle_template_fnv1a(m9_triangle_framed_payload,
				       ARRAY_SIZE(m9_triangle_framed_payload)) !=
				       0xf1c29f08b6b50182ULL)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(job.transition); i++)
		job.transition[i] = target + i * 0x10;
	job.secondary = 0x12000000;
	if (job.transition[0] != 0xfffffff0 || job.transition[1] != 0 ||
	    job.transition[5] != 0x40 ||
	    (job.transition[5] | 1) != 0x41)
		return -EINVAL;
	m9_triangle_build_commands(&job, false, commands);
	m9_triangle_build_primary(&job, commands->primary);
	if (commands->raster[37] != U32_MAX ||
	    commands->raster[38] != U32_MAX ||
	    commands->raster[M9_TRIANGLE_RASTER_SHADE_CONTROL_DW] != 0x00006403 ||
	    commands->raster[M9_TRIANGLE_RASTER_SHADE_LINK0_DW] != 0x80100d88 ||
	    commands->raster[M9_TRIANGLE_RASTER_SHADE_LINK1_DW] != 0x801006b6 ||
	    memcmp(commands->primary, (const u32[]) {
		0x45452d2c, 0x12000009, 0x12000bca,
		0x45452d2c, 0x12000bc9, 0x12001776,
		0x45452d2c, 0x12001775, 0x120017a2,
	}, sizeof(commands->primary)))
		return -EINVAL;
	m9_triangle_build_commands(&job, true, commands);
	if (commands->raster[M9_TRIANGLE_RASTER_SHADE_CONTROL_DW] != 0x00002503 ||
	    commands->raster[M9_TRIANGLE_RASTER_SHADE_LINK0_DW] != 0x00100d88 ||
	    commands->raster[M9_TRIANGLE_RASTER_SHADE_LINK1_DW] != 0x001006b6)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(m9_triangle_reloc_sites); i++) {
		const struct m9_triangle_reloc_site *site =
			&m9_triangle_reloc_sites[i];

		if (m9_triangle_command_range(commands, site->range)[site->dword] !=
		    m9_triangle_operand_value(&job, site->operand))
			return -EINVAL;
	}
	for (i = 0; i < ARRAY_SIZE(m9_triangle_transition_sites); i++) {
		const struct m9_triangle_transition_site *site =
			&m9_triangle_transition_sites[i];
		u32 expected = job.transition[site->transition] | site->tag;

		if (m9_triangle_command_range(commands, site->range)[site->dword] !=
		    expected)
			return -EINVAL;
	}
	return 0;
}

int m9x_triangle_selftest(void)
{
	struct m9_triangle_commands *commands;
	int ret;

	commands = kzalloc_obj(*commands);
	if (!commands)
		return -ENOMEM;
	ret = m9_triangle_stream_selftest(commands);
	kfree(commands);
	return ret;
}

void m9x_triangle_fini(struct m9x_device *mdev)
{
	struct m9x_triangle_diag *diag = mdev->transport.triangle;
	unsigned int id;

	if (!diag)
		return;
	for (id = 0; id < M9_TRI_RESOURCE_COUNT; id++) {
		struct m9_triangle_resource *resource = &diag->resource[id];
		struct drm_gem_vram_object *vram = resource->vram;

		if (!vram)
			continue;
		if (!ttm_bo_reserve(&vram->bo, false, false, NULL)) {
			drm_gem_vram_vunmap(vram, &resource->map);
			ttm_bo_unreserve(&vram->bo);
		}
		m9x_vram_unpin(vram);
		drm_gem_vram_put(vram);
		resource->vram = NULL;
	}
}
