// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 Rene Rebe <rene@exactco.de> */
/*
 * Bounded, kernel-owned M9148 first-triangle preparation.
 *
 * This file intentionally stops before command publication.  The captured
 * fixed-function selector-6c object contains stale host pointers, and the
 * semantic serializer for a fresh record is not yet recovered.  Keeping the
 * gate here makes the resource, address and lifetime work testable without
 * ever making the incomplete operation reachable by the GPU.
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
#define M9_TRIANGLE_SURFACE_BYTES	(M9_TRIANGLE_WIDTH * \
					 M9_TRIANGLE_HEIGHT * sizeof(u32))
#define M9_TRIANGLE_SOURCE_BYTES	0x50u
#define M9_TRIANGLE_SYSTEM_BYTES	0x20080u
#define M9_TRIANGLE_SECONDARY_BYTES	0x4f0u
#define M9_TRIANGLE_SECONDARY_PREFIX	0x8u
#define M9_TRIANGLE_SECONDARY_START_TAG	0x1u
#define M9_TRIANGLE_LINKED_OFFSET	0x300u
#define M9_TRIANGLE_LINKED_BYTES		0x200u
#define M9_TRIANGLE_RESOURCE69_BYTES	0x1b080u
#define M9_TRIANGLE_C2_BYTES		0x1080u
#define M9_TRIANGLE_BE_BYTES		SZ_1M
#define M9_TRIANGLE_TRANSIENT_BYTES	0x16800u
#define M9_TRIANGLE_VRAM_BASE		0x10000000ull

static bool m9_triangle_diag;
module_param(m9_triangle_diag, bool, 0400);
MODULE_PARM_DESC(m9_triangle_diag,
	"Prepare the kernel-owned M9 first-triangle resources (execution remains mapping/window-gated)");

static bool m9_triangle_gouraud;
module_param(m9_triangle_gouraud, bool, 0400);
MODULE_PARM_DESC(m9_triangle_gouraud,
	"Use the captured red/green/blue Gouraud source instead of flat green");

enum m9_triangle_resource_id {
	M9_TRI_SOURCE,
	M9_TRI_SYSTEM,
	M9_TRI_SECONDARY,
	M9_TRI_RESOURCE69,
	M9_TRI_C2,
	M9_TRI_BE,
	M9_TRI_TRANSIENT0,
	M9_TRI_TRANSIENT1,
	M9_TRI_TRANSIENT2,
	M9_TRI_TRANSIENT3,
	M9_TRI_DEPTH,
	M9_TRI_AUX_DEPTH,
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
	u32 resource69;
	u32 c2;
	u32 be;
	u32 transient[4];
	u32 color;
	u32 depth;
	u32 aux_depth;
	u32 fence;
	u32 transition[6];
	u32 final_e3;
	u32 final_43;
};

enum m9_triangle_operand {
	M9_TRI_OP_SOURCE,
	M9_TRI_OP_SYSTEM_PRE_START,
	M9_TRI_OP_SYSTEM_PRE_END,
	M9_TRI_OP_SYSTEM_RASTER_START,
	M9_TRI_OP_SYSTEM_RASTER_END,
	M9_TRI_OP_LINKED,
	M9_TRI_OP_RESOURCE69,
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
	u32 preprocess[118];
	u32 raster[113];
	u32 completion[8];
	u32 primary[9];
};

struct m9x_triangle_diag {
	struct m9_triangle_resource resource[M9_TRI_RESOURCE_COUNT];
	struct m9_triangle_commands commands;
	bool prepared;
};

/* Exact address-bearing sites in the count-free 118/113/8-dword operation. */
static const struct m9_triangle_reloc_site m9_triangle_reloc_sites[] = {
	{ 0, 0xb0, 15, M9_TRI_OP_TRANSIENT0 },
	{ 0, 0xb2, 18, M9_TRI_OP_TRANSIENT3 },
	{ 0, 0xb4, 21, M9_TRI_OP_TRANSIENT1 },
	{ 0, 0xb8, 24, M9_TRI_OP_TRANSIENT2 },
	{ 0, 0x00, 57, M9_TRI_OP_SOURCE },
	{ 0, 0xc2, 61, M9_TRI_OP_C2 },
	{ 0, 0x8d, 66, M9_TRI_OP_SYSTEM_PRE_START },
	{ 0, 0x8e, 67, M9_TRI_OP_SYSTEM_PRE_END },
	{ 0, 0x69, 105, M9_TRI_OP_RESOURCE69 },
	{ 0, 0x6c, 111, M9_TRI_OP_LINKED },
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
	{ 1, 0x69, 103, M9_TRI_OP_RESOURCE69 },
	{ 1, 0x6c, 109, M9_TRI_OP_LINKED },
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
	[M9_TRI_RESOURCE69] = { "resource69", M9_TRIANGLE_RESOURCE69_BYTES,
				  SZ_4K, 0x69000000, true },
	[M9_TRI_C2] = { "c2", M9_TRIANGLE_C2_BYTES, SZ_4K, 0, true },
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
	[M9_TRI_AUX_DEPTH] = { "aux-depth", M9_TRIANGLE_SURFACE_BYTES, 0x100,
				 0xa5a5a5a5, true },
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

/* Exact defined prefix of the pool-5 fixed-function record.  Gouraud needs
 * no linked-native payload; bytes [0x20,0x200) are deterministically zero. */
static const u32 m9_triangle_gouraud_linked_prefix[] = {
	0x43000000, 0xc3000000, 0x3f000000, 0,
	0x43000000, 0x43000000, 0x3f000000, 0,
};

#define M9_TRIANGLE_LINKED_ARENA_LIMIT	0x00800000u
#define M9_TRIANGLE_LINKED_FIRST_BACKOFF	0x00000780u
#define M9_TRIANGLE_LINKED_REF_STRIDE	0x00000028u

#include "m9_triangle_framed.h"

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
	0x00004b08, 0x00000000, 0x00000000, 0x00000000, 0x45454569,
	0x11e42000, 0x4545456a, 0x25010000, 0x456c6b68, 0x00000000,
	0x00001030, 0x40c3a301, 0x45451819, 0x00000000, 0x7fffffff,
	0x23000001, 0x00000000, 0x00000004,
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
	0x00000000, 0x00000000, 0x45454569, 0x11e42000, 0x4545456a,
	0x25010000, 0x456c6b68, 0x00000000, 0x00001030, 0x40c3a301,
	0x23000001, 0x00000000, 0x00000003,
};

static const u32 m9_triangle_range_2[] = {
	0x4545451b, 0x13861002, 0x45e3e6e6, 0x00000003,
	0x00000002, 0x000002c1, 0x45454543, 0x000002c0,
};

/* Absolute byte offsets in the private BmBuffer image.  They parallel the
 * semantic count-free sites above, but retain every private count dword. */
static const u16 m9_triangle_framed_reloc_offsets[] = {
	0x05c, 0x06c, 0x07c, 0x08c, 0x130, 0x144, 0x15c, 0x160,
	0x230, 0x250, 0x2ec, 0x2fc, 0x30c, 0x31c, 0x38c, 0x390,
	0x3a0, 0x3a4, 0x3b4, 0x3cc, 0x3d0, 0x494, 0x4b4, 0x4cc,
};

static const u16 m9_triangle_framed_transition_offsets[] = {
	0x014, 0x050, 0x27c, 0x298, 0x2a4, 0x2e0, 0x4e0, 0x4ec,
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
	/* BE is an exact, context-owned 1 MiB image.  Keep that allocation at its
	 * recovered extent; all other diagnostic resources retain a guard page. */
	if (id == M9_TRI_BE)
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

static int m9_triangle_prepare_resources(struct m9x_device *mdev)
{
	struct m9x_triangle_diag *diag = mdev->transport.triangle;
	static const u8 allocation_order[] = {
		M9_TRI_BE,
		M9_TRI_DEPTH, M9_TRI_AUX_DEPTH,
		M9_TRI_SYSTEM, M9_TRI_RESOURCE69,
		M9_TRI_TRANSIENT0, M9_TRI_TRANSIENT1,
		M9_TRI_TRANSIENT2, M9_TRI_TRANSIENT3,
		M9_TRI_C2, M9_TRI_SECONDARY, M9_TRI_SOURCE,
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
		 "M9 triangle DMA resources: source=%pad system=%pad secondary=%pad\n",
		 &diag->resource[M9_TRI_SOURCE].bus,
		 &diag->resource[M9_TRI_SYSTEM].bus,
		 &diag->resource[M9_TRI_SECONDARY].bus);
	return m9_triangle_verify_guards(diag);
}

static u32 m9_triangle_bus(const struct m9x_triangle_diag *diag,
			   enum m9_triangle_resource_id id)
{
	return lower_32_bits(diag->resource[id].bus);
}

static int m9_triangle_build_addresses(struct m9x_device *mdev, u32 color,
				       struct m9_triangle_job_addresses *job)
{
	const struct m9x_triangle_diag *diag = mdev->transport.triangle;
	u32 target = mdev->transport.next_fence;
	unsigned int i;

	job->source = m9_triangle_bus(diag, M9_TRI_SOURCE) |
		      M9_COMMAND_SYSTEM_TAG;
	job->secondary = m9_triangle_bus(diag, M9_TRI_SECONDARY);
	job->system_pre_start = (m9_triangle_bus(diag, M9_TRI_SYSTEM) + 0x10) |
				M9_COMMAND_SYSTEM_TAG;
	job->system_pre_end = (m9_triangle_bus(diag, M9_TRI_SYSTEM) + 0xc0) |
			      M9_COMMAND_SYSTEM_TAG;
	job->system_raster_start = m9_triangle_bus(diag, M9_TRI_SYSTEM) |
				   M9_COMMAND_SYSTEM_TAG;
	job->system_raster_end = (m9_triangle_bus(diag, M9_TRI_SYSTEM) + 0x10) |
				 M9_COMMAND_SYSTEM_TAG;
	job->linked = (m9_triangle_bus(diag, M9_TRI_SYSTEM) +
		       M9_TRIANGLE_LINKED_OFFSET) | M9_COMMAND_SYSTEM_TAG;
	job->resource69 = m9_triangle_bus(diag, M9_TRI_RESOURCE69);
	job->c2 = m9_triangle_bus(diag, M9_TRI_C2);
	job->be = m9_triangle_bus(diag, M9_TRI_BE);
	for (i = 0; i < ARRAY_SIZE(job->transient); i++)
		job->transient[i] = m9_triangle_bus(diag,
						M9_TRI_TRANSIENT0 + i);
	job->color = color;
	job->depth = m9_triangle_bus(diag, M9_TRI_DEPTH);
	job->aux_depth = m9_triangle_bus(diag, M9_TRI_AUX_DEPTH);
	job->fence = lower_32_bits(mdev->transport.fence_bus) | 2;
	for (i = 0; i < ARRAY_SIZE(job->transition); i++)
		job->transition[i] = target + i * 0x10;
	job->final_e3 = job->transition[5] | 1;
	job->final_43 = job->transition[5];
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
	case M9_TRI_OP_RESOURCE69: return job->resource69;
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
		struct m9_triangle_commands *commands)
{
	unsigned int i;

	memcpy(commands->preprocess, m9_triangle_range_0,
	       sizeof(commands->preprocess));
	memcpy(commands->raster, m9_triangle_range_1,
	       sizeof(commands->raster));
	memcpy(commands->completion, m9_triangle_range_2,
	       sizeof(commands->completion));
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
		struct m9x_triangle_diag *diag,
		const struct m9_triangle_job_addresses *job)
{
	struct m9_triangle_resource *secondary =
		&diag->resource[M9_TRI_SECONDARY];
	u8 *image = secondary->cpu;
	unsigned int i;

	/* The saved BmBuffer's first eight bytes are private host bookkeeping and
	 * are never included in a GPU range.  Keep the same displacement while
	 * refusing to retain the captured process pointer. */
	memset(image, 0, M9_TRIANGLE_SECONDARY_PREFIX);
	memcpy(image + M9_TRIANGLE_SECONDARY_PREFIX,
	       m9_triangle_framed_payload, sizeof(m9_triangle_framed_payload));
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
		{ 0x008, 0x270 },
		{ 0x270, 0x4c4 },
		{ 0x4c4, 0x4f0 },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(bounds); i++) {
		primary[i * 3] = 0x45452d2c;
		primary[i * 3 + 1] = (job->secondary + bounds[i][0]) |
			M9_TRIANGLE_SECONDARY_START_TAG;
		primary[i * 3 + 2] = (job->secondary + bounds[i][1]) |
			M9_COMMAND_SYSTEM_TAG;
	}
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
	case M9_TRI_OP_RESOURCE69: return 0x11e42000;
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
	memcpy(record, m9_triangle_gouraud_linked_prefix,
	       sizeof(m9_triangle_gouraud_linked_prefix));
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

int m9x_submit_triangle_diag(struct m9x_device *mdev, u32 color, u64 *point)
{
	struct m9_triangle_job_addresses job = {};
	int ret;

	if (!m9_triangle_diag)
		return -EACCES;
	*point = 0;
	mutex_lock(&mdev->transport.lock);
	if (mdev->transport.wedged) {
		ret = -EIO;
		goto out_unlock;
	}
	ret = m9_triangle_prepare_resources(mdev);
	if (ret)
		goto out_unlock;
	ret = m9_triangle_build_addresses(mdev, color, &job);
	if (ret)
		goto out_unlock;
	m9_triangle_build_commands(&job, &mdev->transport.triangle->commands);
	m9_triangle_build_framed_secondary(mdev->transport.triangle, &job);
	m9_triangle_build_primary(&job,
		mdev->transport.triangle->commands.primary);
	ret = m9_triangle_serialize_linked_record(mdev, &job);
	if (ret)
		goto out_unlock;
	/* The bounded first-draw pool-5 image is complete.  Execution remains gated
	 * until the system-domain source/linked mappings and all m9x421 ranges
	 * have exact owned inputs; local resources are already in VRAM. */
	dev_warn_once(&mdev->pdev->dev,
		"M9 triangle fully prepared but not submitted: "
		"system-domain mappings and engine windows remain gated\n");
	ret = -EOPNOTSUPP;
out_unlock:
	mutex_unlock(&mdev->transport.lock);
	return ret;
}

int m9x_triangle_selftest(void)
{
	struct m9_triangle_job_addresses job = {};
	struct m9_triangle_commands commands;
	u8 linked[M9_TRIANGLE_LINKED_BYTES] = {};
	u32 target = 0xfffffff0;
	unsigned int i;

	if (sizeof(m9_triangle_flat_source) != M9_TRIANGLE_SOURCE_BYTES ||
	    sizeof(m9_triangle_gouraud_source) != M9_TRIANGLE_SOURCE_BYTES ||
	    ARRAY_SIZE(m9_triangle_range_0) != ARRAY_SIZE(commands.preprocess) ||
	    ARRAY_SIZE(m9_triangle_range_1) != ARRAY_SIZE(commands.raster) ||
	    ARRAY_SIZE(m9_triangle_range_2) != ARRAY_SIZE(commands.completion) ||
	    M9_TRIANGLE_SURFACE_BYTES != 0x4b000 ||
	    M9_TRIANGLE_LINKED_OFFSET & 0x7f ||
	    M9_TRIANGLE_LINKED_OFFSET + M9_TRIANGLE_LINKED_BYTES >
		M9_TRIANGLE_SYSTEM_BYTES ||
	    ARRAY_SIZE(m9_triangle_specs) != M9_TRI_RESOURCE_COUNT ||
	    ARRAY_SIZE(m9_triangle_reloc_sites) != 24 ||
	    ARRAY_SIZE(m9_triangle_transition_sites) != 8 ||
	    ARRAY_SIZE(m9_triangle_framed_reloc_offsets) !=
		ARRAY_SIZE(m9_triangle_reloc_sites) ||
	    ARRAY_SIZE(m9_triangle_framed_transition_offsets) !=
		ARRAY_SIZE(m9_triangle_transition_sites) ||
	    sizeof(m9_triangle_framed_payload) !=
		M9_TRIANGLE_SECONDARY_BYTES - M9_TRIANGLE_SECONDARY_PREFIX ||
	    m9_triangle_specs[M9_TRI_BE].payload_bytes != SZ_1M)
		return -EINVAL;
	if (m9_triangle_template_fnv1a(m9_triangle_gouraud_linked_prefix,
		ARRAY_SIZE(m9_triangle_gouraud_linked_prefix)) !=
		0x80b075ef8b975c45ULL)
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
	memcpy(commands.preprocess, m9_triangle_range_0,
	       sizeof(commands.preprocess));
	memcpy(commands.raster, m9_triangle_range_1, sizeof(commands.raster));
	memcpy(commands.completion, m9_triangle_range_2,
	       sizeof(commands.completion));
	for (i = 0; i < ARRAY_SIZE(m9_triangle_reloc_sites); i++) {
		const struct m9_triangle_reloc_site *site =
			&m9_triangle_reloc_sites[i];
		u16 framed = m9_triangle_framed_reloc_offsets[i];

		if (m9_triangle_reloc_sites[i].range > 2 ||
		    m9_triangle_reloc_sites[i].operand > M9_TRI_OP_FENCE ||
		    m9_triangle_command_range(&commands, site->range)[site->dword] !=
			m9_triangle_captured_operand(site->operand) ||
		    framed < M9_TRIANGLE_SECONDARY_PREFIX ||
		    framed > M9_TRIANGLE_SECONDARY_BYTES - sizeof(u32) ||
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
		    m9_triangle_command_range(&commands, site->range)[site->dword] !=
			(captured | site->tag) ||
		    framed < M9_TRIANGLE_SECONDARY_PREFIX ||
		    framed > M9_TRIANGLE_SECONDARY_BYTES - sizeof(u32) ||
		    get_unaligned_le32((const u8 *)m9_triangle_framed_payload +
			framed - M9_TRIANGLE_SECONDARY_PREFIX) !=
			(captured | site->tag))
			return -EINVAL;
	}
	if (m9_triangle_template_fnv1a(m9_triangle_range_0,
				       ARRAY_SIZE(m9_triangle_range_0)) !=
				       0x2f0d9c8c5afce09bULL ||
	    m9_triangle_template_fnv1a(m9_triangle_range_1,
				       ARRAY_SIZE(m9_triangle_range_1)) !=
				       0x7ea7088342b7d71bULL ||
	    m9_triangle_template_fnv1a(m9_triangle_range_2,
				       ARRAY_SIZE(m9_triangle_range_2)) !=
				       0x726d161270583420ULL ||
	    m9_triangle_template_fnv1a(m9_triangle_framed_payload,
				       ARRAY_SIZE(m9_triangle_framed_payload)) !=
				       0xf891d548016a1ee6ULL)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(job.transition); i++)
		job.transition[i] = target + i * 0x10;
	job.secondary = 0x12000000;
	if (job.transition[0] != 0xfffffff0 || job.transition[1] != 0 ||
	    job.transition[5] != 0x40 ||
	    (job.transition[5] | 1) != 0x41)
		return -EINVAL;
	m9_triangle_build_commands(&job, &commands);
	m9_triangle_build_primary(&job, commands.primary);
	if (memcmp(commands.primary, (const u32[]) {
		0x45452d2c, 0x12000009, 0x12000272,
		0x45452d2c, 0x12000271, 0x120004c6,
		0x45452d2c, 0x120004c5, 0x120004f2,
	}, sizeof(commands.primary)))
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(m9_triangle_reloc_sites); i++) {
		const struct m9_triangle_reloc_site *site =
			&m9_triangle_reloc_sites[i];

		if (m9_triangle_command_range(&commands, site->range)[site->dword] !=
		    m9_triangle_operand_value(&job, site->operand))
			return -EINVAL;
	}
	for (i = 0; i < ARRAY_SIZE(m9_triangle_transition_sites); i++) {
		const struct m9_triangle_transition_site *site =
			&m9_triangle_transition_sites[i];
		u32 expected = job.transition[site->transition] | site->tag;

		if (m9_triangle_command_range(&commands, site->range)[site->dword] !=
		    expected)
			return -EINVAL;
	}
	return 0;
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
