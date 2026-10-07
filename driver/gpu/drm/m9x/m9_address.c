// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 Rene Rebe <rene@exactco.de> */
/*
 * M9 translation tables and address-window ownership.
 *
 * The Linux vendor driver reserves one monolithic low-VRAM image sized from
 * the PInS memory size, not from BAR0.  Its hardware addresses are offsets
 * from VRAM.  Ordinary system render resources retain their public
 * 0x40000000 GPU addresses: m9x328 indexes those page numbers directly from
 * the compact origin at 0x801000, inside the primary child.  Reserving the
 * image before any KMS BO gives the clean-room driver a BAR0 CPU mapping
 * without changing the low addresses consumed by the manager.
 */
#include <linux/iosys-map.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include <drm/drm_print.h>

#include "m9_drv.h"
#include "m9_reg.h"

#define M9_ADDRESS_COMPACT_ORIGIN	0x00801000u
#define M9_ADDRESS_FAA_COMPACT_ORIGIN	0x00401000u
#define M9_ADDRESS_FAA_PRIMARY_BYTES	0x00104000u
#define M9_ADDRESS_SECONDARY_STRIDE	0x10u
/* The primary child indexes every VRAM page plus 0x8000 further pages. */
#define M9_ADDRESS_EXTRA_PAGES		0x8000u
#define M9_ADDRESS_TRANSLATION_WINDOW	0x00080000u
#define M9_ADDRESS_TRANSLATION_BYTES	0x00040000u
/* The restore pool starts this reserve between the tail and the local pools. */
#define M9_ADDRESS_RESTORE_RESERVE	0x00400000u
#define M9_ADDRESS_SERVICE_SLOTS	20u
#define M9_ADDRESS_SERVICE_STRIDE	0x10u
#define M9_ADDRESS_SERVICE_INVALID	0x0000ffffu
#define M9_ADDRESS_SERVICE_POOLS	4u
/* Slot d0 = BAR0 start_page | end_page << 16 (inclusive); lowest slot wins. */
#define M9_ADDRESS_CPU_WINDOW		0x0000u
#define M9_ADDRESS_CPU_WINDOW_RANGE	((((M9_ADDRESS_CPU_BYTES >> PAGE_SHIFT) - 1) << 16))
#define M9_ADDRESS_CPU_WINDOW_FORMAT	0xf0000000u
#define M9_ADDRESS_CPU_WINDOW_EXTENT	0x00010001u
#define M9_ADDRESS_SURFACE_WINDOW	0x0010u
#define M9_ADDRESS_SURFACE_WINDOW_RANGE \
	((M9_ADDRESS_CPU_BYTES >> PAGE_SHIFT) | \
	 (((M9_ADDRESS_CPU_BYTES + M9_ADDRESS_SURFACE_BYTES) >> PAGE_SHIFT) - 1) << 16)
#define M9_ADDRESS_SLOT_EMPTY		0x0000ffffu

/* m9x926's four mode-0 resources in a fresh m9x565 local range. */
static const u32 m9_service_pool_bytes[M9_ADDRESS_SERVICE_POOLS] = {
	0x00400000, 0x00100000, 0x00060000, 0x00000500,
};

struct m9_address_layout {
	u32 primary_bytes;
	u32 primary_limit;
	u32 translation_base;
	u32 translation_limit;
	u32 pte_base;
	u32 pte_bytes;
	u32 pte_limit;
	u32 restore_base;
	u32 root_bytes;
	u32 system_end;
	u32 pool_base[M9_ADDRESS_SERVICE_POOLS];
	u32 primary_base;
	u32 secondary_base;
	u32 secondary_bytes;
};

struct m9x_address_binding {
	u32 gpuva;
	u32 bytes;
	dma_addr_t dma;
	bool flags30;
	bool published;
};

struct m9x_address_space {
	struct m9_address_layout layout;
	struct drm_gem_vram_object *root;
	struct drm_gem_vram_object *service_pools[M9_ADDRESS_SERVICE_POOLS];
	struct iosys_map map;
	struct m9x_address_binding *bindings;
	struct drm_mm system_mm;
	bool system_mm_ready;
	u32 local_pages;
	u32 binding_count;
	u32 binding_capacity;
	bool mapped;
	bool service_ready;
	bool windows_published;
	bool bindings_dirty;
	bool manager_ready;
	bool restore_published;
	bool service_finalized;
};

static int m9_address_layout_init(u64 vram_total, u64 local_bytes,
				  bool faa, struct m9_address_layout *layout)
{
	u64 origin, primary, secondary, translation, pte, restore, root, base, end = 0;
	unsigned int i;

	if (!vram_total || !IS_ALIGNED(vram_total, SZ_1M) ||
	    vram_total > SZ_4G)
		return -EINVAL;
	/* The private profile is the actual M9128 Windows MmFlags=100f8/101f8
	 * constructor under the retained CPU fixture's 16-MiB system policy.
	 * That policy observation does not measure the target's installed RAM.
	 * It is deliberately bounded; other profiles retain the Linux layout. */
	if (faa && vram_total != SZ_1G)
		return -EOPNOTSUPP;
	origin = faa ? M9_ADDRESS_FAA_COMPACT_ORIGIN : M9_ADDRESS_COMPACT_ORIGIN;
	primary = faa ? M9_ADDRESS_FAA_PRIMARY_BYTES :
		((vram_total >> PAGE_SHIFT) + M9_ADDRESS_EXTRA_PAGES) * sizeof(u32);
	secondary = faa ? primary * 4 : 0;
	translation = origin + primary + secondary;
	pte = translation + M9_ADDRESS_TRANSLATION_WINDOW;
	restore = pte + primary;
	root = restore + M9_ADDRESS_RESTORE_RESERVE;
	base = root;
	for (i = 0; i < M9_ADDRESS_SERVICE_POOLS; i++) {
		layout->pool_base[i] = base;
		end = base + m9_service_pool_bytes[i];
		base = PAGE_ALIGN(end);
	}
	if (end > local_bytes)
		return -ERANGE;

	layout->primary_bytes = primary;
	layout->primary_base = origin;
	layout->primary_limit = origin + primary - PAGE_SIZE;
	layout->secondary_base = secondary ? origin + primary : 0;
	layout->secondary_bytes = secondary;
	layout->translation_base = translation;
	layout->translation_limit = pte - PAGE_SIZE;
	layout->pte_base = pte;
	layout->pte_bytes = primary;
	layout->pte_limit = restore - PAGE_SIZE;
	layout->restore_base = restore;
	layout->root_bytes = root;
	layout->system_end = min_t(u64, (primary / sizeof(u32)) << PAGE_SHIFT,
				   U32_MAX & PAGE_MASK);
	return 0;
}

static void m9_address_free_service_pools(struct m9x_address_space *address)
{
	u32 i;

	for (i = 0; i < M9_ADDRESS_SERVICE_POOLS; i++) {
		if (!address->service_pools[i])
			continue;
		m9x_vram_unpin(address->service_pools[i]);
		drm_gem_vram_put(address->service_pools[i]);
		address->service_pools[i] = NULL;
	}
}

/*
 * The vendor pools sit above the BAR; the clean-room driver keeps them in
 * CPU-visible VRAM so the GPU never reads uninitialized memory.
 */
static int m9_address_clear_bo(struct drm_gem_vram_object *bo, u32 bytes)
{
	struct iosys_map map;
	int ret;

	ret = ttm_bo_reserve(&bo->bo, true, false, NULL);
	if (ret)
		return ret;
	ret = drm_gem_vram_vmap(bo, &map);
	if (!ret) {
		iosys_map_memset(&map, 0, 0, bytes);
		drm_gem_vram_vunmap(bo, &map);
	}
	ttm_bo_unreserve(&bo->bo);
	return ret;
}

static int m9_address_reserve_service_pools(struct m9x_device *mdev,
					     struct m9x_address_space *address)
{
	u32 i;
	int ret;

	/* Reserve before KMS/user BOs can consume the fresh vendor local range. */
	for (i = 0; i < M9_ADDRESS_SERVICE_POOLS; i++) {
		struct drm_gem_vram_object *pool;
		u32 bytes = PAGE_ALIGN(m9_service_pool_bytes[i]);
		s64 offset;

		pool = drm_gem_vram_create(&mdev->drm, bytes, 1);
		if (IS_ERR(pool)) {
			ret = PTR_ERR(pool);
			goto fail;
		}
		ret = m9x_vram_pin(pool);
		if (ret) {
			drm_gem_vram_put(pool);
			goto fail;
		}
		address->service_pools[i] = pool;
		offset = drm_gem_vram_offset(pool);
		if (offset != address->layout.pool_base[i]) {
			ret = offset < 0 ? (int)offset : -ERANGE;
			goto fail;
		}
		ret = m9_address_clear_bo(pool, bytes);
		if (ret)
			goto fail;
	}
	return 0;

fail:
	m9_address_free_service_pools(address);
	return ret;
}

/*
 * Map one surface de-tiled at BAR0 M9_ADDRESS_CPU_BYTES through slot 1; the
 * slot 0 identity view of VRAM stays valid for concurrent CPU access.
 */
void m9x_address_surface_window(struct m9x_device *mdev, u32 surface,
				u32 format, u32 extent)
{
	m9x_write(mdev, M9_ADDRESS_SURFACE_WINDOW, 0);
	m9x_write(mdev, M9_ADDRESS_SURFACE_WINDOW + 0x4, surface);
	m9x_write(mdev, M9_ADDRESS_SURFACE_WINDOW + 0x8, format);
	m9x_write(mdev, M9_ADDRESS_SURFACE_WINDOW + 0xc, extent);
	m9x_write(mdev, M9_ADDRESS_SURFACE_WINDOW,
		  M9_ADDRESS_SURFACE_WINDOW_RANGE);
	m9x_read(mdev, M9_ADDRESS_SURFACE_WINDOW);
}

/* The same slot carrying the untranslated identity format: a linear view of
 * any video memory address, including memory no permanent window reaches. */
void m9x_address_raw_window(struct m9x_device *mdev, u32 vram)
{
	m9x_address_surface_window(mdev, vram, M9_ADDRESS_CPU_WINDOW_FORMAT,
				   M9_ADDRESS_CPU_WINDOW_EXTENT);
}

void m9x_address_surface_release(struct m9x_device *mdev)
{
	m9x_write(mdev, M9_ADDRESS_SURFACE_WINDOW, M9_ADDRESS_SLOT_EMPTY);
	m9x_read(mdev, M9_ADDRESS_SURFACE_WINDOW);
}

static void m9_address_invalidate_service_bank(struct m9x_device *mdev)
{
	u32 slot;

	/* A null m9x115 entry writes only dword zero, committing it last. */
	for (slot = 0; slot < M9_ADDRESS_SERVICE_SLOTS; slot++)
		m9x_write(mdev, slot * M9_ADDRESS_SERVICE_STRIDE,
			  M9_ADDRESS_SERVICE_INVALID);
	m9x_read(mdev, (M9_ADDRESS_SERVICE_SLOTS - 1) *
		 M9_ADDRESS_SERVICE_STRIDE);
}

static void m9_address_activate(struct m9x_device *mdev)
{
	struct m9x_address_space *address = mdev->transport.address;
	bool secondary = address && address->layout.secondary_bytes;

	/* Make the BAR/WC leaves visible before the front-end activation. */
	wmb();
	m9x_write(mdev, M9_REG_FRONTEND_STATE, 0x0f);
	m9x_write(mdev, M9_REG_FRONTEND_STATE, secondary ? 0 : 1);
	m9x_write(mdev, M9_REG_FRONTEND_STATE, secondary ? 1 : 0);
	m9x_read(mdev, M9_REG_FRONTEND_STATE);
}

u32 m9x_address_pte_word(dma_addr_t dma)
{
	return (lower_32_bits(dma) & 0xfffff000u) |
	       ((dma >> 28) & 0x00000ff0u) | 0xau;
}

static u32 m9_address_root_pte_word(u32 page)
{
	return (page << PAGE_SHIFT) | 0x8u;
}

static u32 m9_address_identity_slot(const struct m9_address_layout *layout,
				    u32 address)
{
	return layout->primary_base +
	       (address >> PAGE_SHIFT) * sizeof(u32);
}

static int m9_address_slot(const struct m9_address_layout *layout, u32 gpuva,
			   u32 *offset)
{
	if (gpuva < M9_ADDRESS_SYSTEM_BASE || gpuva >= layout->system_end ||
	    !IS_ALIGNED(gpuva, PAGE_SIZE))
		return -ERANGE;
	/*
	 * m9x887 reads this same public resource address from object +0x48 and
	 * stores its page number in the descriptor consumed by m9x328.
	 */
	*offset = m9_address_identity_slot(layout, gpuva);
	return 0;
}

/* Same source descriptor as the compact publisher, with a 16-byte stride.
 * Local mappings produce DWORD0..2 and retain the unknown DWORD3. Genuine
 * scatter mappings instead produce all four words: PTE,0,page_count,0. */
static int m9_address_secondary_slot(const struct m9_address_layout *layout,
				     u32 gpuva, u32 *slot)
{
	u64 offset = (u64)layout->secondary_base +
		     (gpuva >> PAGE_SHIFT) * M9_ADDRESS_SECONDARY_STRIDE;
	u64 end = (u64)layout->secondary_base + layout->secondary_bytes;

	if (!layout->secondary_bytes || !IS_ALIGNED(gpuva, PAGE_SIZE) ||
	    offset + M9_ADDRESS_SECONDARY_STRIDE > end ||
	    end > layout->translation_base)
		return -ERANGE;
	*slot = offset;
	return 0;
}

static int m9_address_secondary_publish(struct m9x_device *mdev,
				       u32 gpuva, u32 bytes, bool dma,
				       bool invalidate)
{
	struct m9x_address_space *a = mdev->transport.address;
	u32 pages = bytes >> PAGE_SHIFT, slot, page;
	u64 end = (u64)gpuva + bytes;

	if (!a->layout.secondary_bytes)
		return 0;
	if (!bytes || !IS_ALIGNED(bytes, PAGE_SIZE) ||
	    end > (u64)U32_MAX + 1 ||
	    m9_address_secondary_slot(&a->layout, gpuva, &slot) ||
	    m9_address_secondary_slot(&a->layout, end - PAGE_SIZE, &slot))
		return -ERANGE;
	if (!invalidate) {
		for (page = 0; page < pages; page++) {
			u32 source = m9_address_identity_slot(&a->layout,
				gpuva + page * PAGE_SIZE);
			u32 pte = iosys_map_rd(&a->map, source, u32);

			/* Typed DMA publication accepts flags110 nibblea or the
			 * separately admitted flags30 output nibbleb. The packed
			 * leaf was checked against its binding before this copy. */
			if (dma ? ((pte & 0xf) != 0xa && (pte & 0xf) != 0xb) :
			    (pte & 0xf) != 0x8)
				return -EINVAL;
		}
	}
	/* Actual secondary pre/post lists 10000005 / 20000028. */
	wmb();
	m9x_write(mdev, M9_REG_FRONTEND_STATE, 5);
	for (page = 0; page < pages; page++) {
		u32 address = gpuva + page * PAGE_SIZE;
		u32 source = m9_address_identity_slot(&a->layout, address);
		u32 destination = a->layout.secondary_base +
			(address >> PAGE_SHIFT) * M9_ADDRESS_SECONDARY_STRIDE;

		iosys_map_wr(&a->map, destination, u32,
			invalidate ? 0 : iosys_map_rd(&a->map, source, u32));
		if (!invalidate) {
			iosys_map_wr(&a->map, destination + 4, u32, 0);
			iosys_map_wr(&a->map, destination + 8, u32, dma ? pages : 0);
			if (dma)
				iosys_map_wr(&a->map, destination + 12, u32, 0);
		}
	}
	wmb();
	/* Admit every produced word before the secondary post-list. The local
	 * fourth word is unknown and deliberately excluded from this readback. */
	for (page = 0; page < pages; page++) {
		u32 address = gpuva + page * PAGE_SIZE;
		u32 source = m9_address_identity_slot(&a->layout, address);
		u32 destination = a->layout.secondary_base +
			(address >> PAGE_SHIFT) * M9_ADDRESS_SECONDARY_STRIDE;
		u32 expected = invalidate ? 0 : iosys_map_rd(&a->map, source, u32);
		u32 words = invalidate ? 1 : (dma ? 4 : 3);
		u32 values[] = { expected, 0, dma ? pages : 0, 0 };
		u32 word;

		for (word = 0; word < words; word++) {
			u32 observed = iosys_map_rd(&a->map,
				destination + word * sizeof(u32), u32);

			if (observed == values[word])
				continue;
			dev_err(&mdev->pdev->dev,
				"M9 companion refused: GPU=%#x pages=%u page=%u word=%u observed=%#x expected=%#x\n",
				gpuva, pages, page, word, observed, values[word]);
			/* Keep the pre-list freeze after a corrupt mirror. No
			 * success post-list or later job may observe it. */
			mdev->transport.wedged = true;
			mdev->transport.generation++;
			return -EIO;
		}
	}
	m9x_write(mdev, M9_REG_FRONTEND_STATE, 0);
	m9x_write(mdev, M9_REG_FRONTEND_STATE, 2);
	m9x_read(mdev, M9_REG_FRONTEND_STATE);
	return 0;
}

static int m9_address_selftest(void)
{
	static const struct {
		u64 vram;
		struct m9_address_layout expect;
	} cases[] = {
		{ SZ_256M, { 0x60000, 0x860000, 0x861000, 0x8e0000, 0x8e1000,
			     0x60000, 0x940000, 0x941000, 0xd41000, 0x18000000,
			     { 0xd41000, 0x1141000, 0x1241000, 0x12a1000 },
			     0x801000, 0, 0 } },
		/* Live vendor M9148-E1024LAF readback. */
		{ SZ_1G, { 0x120000, 0x920000, 0x921000, 0x9a0000, 0x9a1000,
			   0x120000, 0xac0000, 0xac1000, 0xec1000, 0x48000000,
			   { 0xec1000, 0x12c1000, 0x13c1000, 0x1421000 },
			   0x801000, 0, 0 } },
	};
	struct m9_address_layout layout;
	u32 offset;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++)
		if (m9_address_layout_init(cases[i].vram, SZ_256M, false, &layout) ||
		    memcmp(&layout, &cases[i].expect, sizeof(layout)))
			return -EINVAL;
	if (m9_address_slot(&layout, 0x40000000, &offset) ||
	    offset != 0x00901000 ||
	    m9_address_slot(&layout, 0x400f2000, &offset) ||
	    offset != 0x009013c8 ||
	    m9_address_slot(&layout, 0x47fff000, &offset) ||
	    offset != 0x00920ffc ||
	    !m9_address_slot(&layout, 0x3ffff000, &offset) ||
	    !m9_address_slot(&layout, 0x48000000, &offset) ||
	    m9_address_identity_slot(&layout, 0x10ec1000) != 0x00844b04 ||
	    m9_address_identity_slot(&layout, 0x00ac1000) != 0x00803b04 ||
	    m9x_address_pte_word((dma_addr_t)0x0232a000) != 0x0232a00a ||
	    m9x_address_pte_word((dma_addr_t)0xab12345000ULL) != 0x12345aba ||
	    m9_address_root_pte_word(0x923) != 0x00923008 ||
	    m9_address_root_pte_word(0xec0) != 0x00ec0008)
		return -EINVAL;
	if (m9_address_layout_init(SZ_1G, SZ_256M, true, &layout) ||
	    layout.primary_base != 0x401000 || layout.primary_bytes != 0x104000 ||
	    layout.primary_limit != 0x504000 ||
	    layout.secondary_base != 0x505000 || layout.secondary_bytes != 0x410000 ||
	    layout.translation_base != 0x915000 || layout.translation_limit != 0x994000 ||
	    layout.pte_base != 0x995000 || layout.pte_bytes != 0x104000 ||
	    layout.pte_limit != 0xa98000 || layout.restore_base != 0xa99000 ||
	    layout.root_bytes != 0xe99000 || layout.system_end != 0x41000000 ||
	    m9_address_slot(&layout, 0x40000000, &offset) || offset != 0x501000 ||
	    m9_address_slot(&layout, 0x40fff000, &offset) || offset != 0x504ffc ||
	    !m9_address_slot(&layout, 0x41000000, &offset) ||
	    offset >= layout.secondary_base)
		return -EINVAL;
	if (m9_address_secondary_slot(&layout, 0x40000000, &offset) ||
	    offset != 0x905000 ||
	    m9_address_secondary_slot(&layout, 0x40fff000, &offset) ||
	    offset != 0x914ff0 ||
	    !m9_address_secondary_slot(&layout, 0x41000000, &offset) ||
	    !m9_address_layout_init(SZ_256M, SZ_256M, true, &layout))
		return -EINVAL;
	return 0;
}

/*
 * BAR0 is a CPU window translated by the 20-slot host-aperture descriptor
 * bank.  m9x115 invalidates every slot, so publish the linear identity
 * descriptor vendor X uses for its VRAM test before any CPU VRAM access.
 */
static void m9_address_publish_cpu_window(struct m9x_device *mdev)
{
	m9x_write(mdev, M9_ADDRESS_CPU_WINDOW + 0x4, 0);
	m9x_write(mdev, M9_ADDRESS_CPU_WINDOW + 0x8, M9_ADDRESS_CPU_WINDOW_FORMAT);
	m9x_write(mdev, M9_ADDRESS_CPU_WINDOW + 0xc, M9_ADDRESS_CPU_WINDOW_EXTENT);
	m9x_write(mdev, M9_ADDRESS_CPU_WINDOW, M9_ADDRESS_CPU_WINDOW_RANGE);
	m9x_read(mdev, M9_ADDRESS_CPU_WINDOW);
}

/*
 * m9x24643: accept an all-ones 0x0f28 or status bits 16/20 == 01, then
 * destructively verify the first 0x1ffff dwords of the aperture with their
 * index and index << 16.  One bounded attempt instead of the vendor's 2000.
 */
static int m9_address_vram_selftest(struct m9x_device *mdev,
				    struct iosys_map *map)
{
	u32 status = m9x_read(mdev, M9_REG_MEMORY_STATUS);
	u32 i, value;

	/* Bit 20 stays set after GPU use, so on a warm reload the status is advisory. */
	if (status != U32_MAX && ((status >> 16) & 0x11) != 1)
		dev_info(&mdev->pdev->dev,
			 "M9 memory status %#x; verifying VRAM\n", status);
	for (i = 0; i < 0xffff; i++)
		iosys_map_wr(map, i * sizeof(u32), u32, i);
	for (i = 0x10000; i < 0x1ffff; i++)
		iosys_map_wr(map, i * sizeof(u32), u32, i << 16);
	wmb();
	for (i = 0; i < 0x1ffff; i++) {
		if (i == 0xffff)
			continue;
		value = iosys_map_rd(map, i * sizeof(u32), u32);
		if (value != (i < 0x10000 ? i : i << 16)) {
			dev_err(&mdev->pdev->dev,
				"M9 VRAM self-test failed at %#x: %#x, expected %#x\n",
				i * 4, value, i < 0x10000 ? i : i << 16);
			return -EIO;
		}
	}
	return 0;
}

static void m9_address_clear_root_image(struct m9x_address_space *address)
{
	const struct m9_address_layout *layout = &address->layout;

	if (layout->secondary_bytes) {
		/* The secondary constructor clears only DWORD0 of each record.
		 * Keep its unknown backing outside the preliminary root clear. */
		iosys_map_memset(&address->map, 0, 0, layout->secondary_base);
		iosys_map_memset(&address->map,
			layout->secondary_base + layout->secondary_bytes, 0,
			layout->root_bytes - layout->secondary_base - layout->secondary_bytes);
	} else {
		iosys_map_memset(&address->map, 0, 0, layout->root_bytes);
	}
}

int m9x_address_init(struct m9x_device *mdev)
{
	struct m9x_address_space *address;
	struct m9_address_layout *layout;
	struct drm_gem_vram_object *root;
	s64 offset;
	int ret;

	ret = m9_address_selftest();
	if (ret)
		return ret;
	if (mdev->transport.address)
		return 0;
	m9_address_publish_cpu_window(mdev);
	address = devm_kzalloc(&mdev->pdev->dev, sizeof(*address), GFP_KERNEL);
	if (!address)
		return -ENOMEM;
	layout = &address->layout;
	if (m9x_faa_diag_enabled() &&
	    (mdev->pdev->device != 0x0540 || mdev->pdev->revision != 1 ||
	     mdev->pdev->subsystem_device != 0x22c0 ||
	     mdev->bar0_bytes != SZ_256M || m9x_local_gpu_base))
		return -EOPNOTSUPP;
	/* The service pools and the translation root live in low video memory,
	 * so the layout is bounded by the memory itself, not by the aperture. */
	address->local_pages = m9x_vram_gpu_limit(mdev) >> PAGE_SHIFT;
	ret = m9_address_layout_init(mdev->vram_total,
				     (u64)address->local_pages << PAGE_SHIFT,
				     m9x_faa_diag_enabled(), layout);
	if (ret)
		return ret;
	if (layout->system_end <= M9_ADDRESS_SYSTEM_BASE)
		dev_warn(&mdev->pdev->dev,
			 "M9 %#llx-byte layout has no class-1 system slots\n",
			 mdev->vram_total);
	/* Kernel surface transfers map the slot 1 window above the identity. */
	if (mdev->bar0_bytes >= M9_ADDRESS_CPU_BYTES + M9_ADDRESS_SURFACE_BYTES) {
		mdev->bar0_window = devm_ioremap_wc(&mdev->pdev->dev,
				pci_resource_start(mdev->pdev, 0) + M9_ADDRESS_CPU_BYTES,
				M9_ADDRESS_SURFACE_BYTES);
		if (!mdev->bar0_window)
			return -ENOMEM;
	}
	root = drm_gem_vram_create(&mdev->drm, layout->root_bytes, 1);
	if (IS_ERR(root))
		return PTR_ERR(root);
	ret = m9x_vram_pin(root);
	if (ret)
		goto out_put;
	offset = drm_gem_vram_offset(root);
	if (offset != 0) {
		ret = offset < 0 ? (int)offset : -ERANGE;
		goto out_unpin;
	}
	ret = ttm_bo_reserve(&root->bo, true, false, NULL);
	if (ret)
		goto out_unpin;
	ret = drm_gem_vram_vmap(root, &address->map);
	ttm_bo_unreserve(&root->bo);
	if (ret)
		goto out_unpin;

	address->root = root;
	address->mapped = true;
	ret = m9_address_vram_selftest(mdev, &address->map);
	if (!ret)
		ret = m9_address_reserve_service_pools(mdev, address);
	if (ret) {
		/* The common teardown owns both the root mapping and its pin. */
		mdev->transport.address = address;
		m9x_address_fini(mdev);
		return ret;
	}
	m9_address_clear_root_image(address);
	/* Publish the cleared low-VRAM manager image before handing it to HW. */
	wmb();
	if (layout->system_end > M9_ADDRESS_SYSTEM_BASE) {
		drm_mm_init(&address->system_mm, M9_ADDRESS_SYSTEM_BASE,
			    layout->system_end - M9_ADDRESS_SYSTEM_BASE);
		address->system_mm_ready = true;
	}
	mdev->transport.address = address;
	dev_info(&mdev->pdev->dev,
		 "M9 %#llx-byte VRAM layout: root [0,%#x) primary %#x/%#x translation %#x/%#x tail %#x/%#x restore %#x system <%#x\n",
		 mdev->vram_total, layout->root_bytes, layout->primary_base,
		 layout->primary_limit, layout->translation_base,
		 layout->translation_limit, layout->pte_base, layout->pte_limit,
		 layout->restore_base, layout->system_end);
	if (layout->secondary_bytes)
		dev_info(&mdev->pdev->dev,
			 "M9 private companion view: GPU=%#x bytes=%#x stride=16 local-known=12 local-unknown=4 DMA-known=16\n",
			 layout->secondary_base, layout->secondary_bytes);
	return 0;

out_unpin:
	m9x_vram_unpin(root);
out_put:
	drm_gem_vram_put(root);
	return ret;
}

static int m9_address_bind_dma_policy(struct m9x_device *mdev, u32 gpuva,
			 dma_addr_t dma, u32 bytes, bool flags30)
{
	struct m9x_address_space *address = mdev->transport.address;
	struct m9x_address_binding *bindings;
	u32 gpuva_end, pages, slot, span;
	u64 dma_end;
	u32 i;

	if (!address || !address->mapped || !bytes ||
	    !IS_ALIGNED(gpuva, PAGE_SIZE) || !IS_ALIGNED(dma, PAGE_SIZE))
		return -EINVAL;
	pages = DIV_ROUND_UP(bytes, PAGE_SIZE);
	if (check_mul_overflow(pages, (u32)PAGE_SIZE, &span) ||
	    check_add_overflow(gpuva, span, &gpuva_end) ||
	    gpuva_end > address->layout.system_end ||
	    check_add_overflow((u64)dma, (u64)span, &dma_end) ||
	    dma_end > (u64)U32_MAX + 1)
		return -ERANGE;
	/*
	 * Validate both endpoints before staging a binding, so a rejected bind
	 * cannot leave a partial mapping behind.
	 */
	if (m9_address_slot(&address->layout, gpuva, &slot) ||
	    m9_address_slot(&address->layout, gpuva_end - PAGE_SIZE, &slot))
		return -ERANGE;
	for (i = 0; i < address->binding_count; i++) {
		const struct m9x_address_binding *binding =
			&address->bindings[i];
		u32 binding_end = binding->gpuva + binding->bytes;

		if (binding->gpuva == gpuva && binding->bytes == span &&
		    binding->dma == dma && binding->flags30 == flags30)
			return 0;
		if (gpuva < binding_end && binding->gpuva < gpuva_end)
			return -EEXIST;
	}
	if (address->binding_count == address->binding_capacity) {
		u32 capacity = address->binding_capacity ?
			address->binding_capacity * 2 : 8;

		if (capacity < address->binding_capacity)
			return -EOVERFLOW;
		bindings = krealloc_array(address->bindings, capacity,
					  sizeof(*bindings), GFP_KERNEL);
		if (!bindings)
			return -ENOMEM;
		address->bindings = bindings;
		address->binding_capacity = capacity;
	}
	address->bindings[address->binding_count++] =
		(struct m9x_address_binding) {
			.gpuva = gpuva,
			.bytes = span,
			.dma = dma,
			.flags30 = flags30,
		};
	address->bindings_dirty = true;
	return 0;
}

int m9x_address_bind_dma(struct m9x_device *mdev, u32 gpuva,
                         dma_addr_t dma, u32 bytes)
{
	return m9_address_bind_dma_policy(mdev, gpuva, dma, bytes, false);
}

static u32 m9_address_binding_pte(const struct m9x_address_binding *binding,
                                u32 page)
{
	/* Actual 5b750: flags110 encodes a; actual ac1f0 temp flags30 encodes b.
	 * Only the admitted private output owner selects this cache policy. */
	return m9x_address_pte_word(binding->dma + page * PAGE_SIZE) |
	       (binding->flags30 ? 1u : 0u);
}

int m9x_address_prepare(struct m9x_device *mdev)
{
	struct m9x_address_space *address = mdev->transport.address;
	u32 value;

	if (!address || !address->mapped)
		return -EINVAL;
	if (address->service_ready)
		return 0;

	/* Exact one-time Linux M9148 service construction. */
	value = m9x_read(mdev, M9_REG_APERTURE_PROPERTY);
	m9x_write(mdev, M9_REG_APERTURE_PROPERTY, value & 0xcfffffffu);
	m9x_write(mdev, M9_REG_SERVICE_CLEAR0, 0);
	m9x_write(mdev, M9_REG_SERVICE_CLEAR1, 0);
	m9x_write(mdev, M9_REG_SERVICE_CLEAR2, 0);
	m9x_write(mdev, M9_REG_SERVICE_CLEAR3, 0);
	m9x_write(mdev, M9_REG_SERVICE_MAP, 0x03020100);
	m9x_write(mdev, M9_REG_SERVICE_LANE0, 0);
	m9x_write(mdev, M9_REG_SERVICE_LANE1, 0x40);
	m9x_write(mdev, M9_REG_SERVICE_LANE2, 0x80);
	m9x_write(mdev, M9_REG_SERVICE_LANE3, 0xc0);
	m9x_write(mdev, M9_REG_SERVICE_BASE, 0x1000);
	m9x_write(mdev, M9_REG_SERVICE_CONTROL, 0);
	value = m9x_read(mdev, M9_REG_SERVICE_MODE);
	m9x_write(mdev, M9_REG_SERVICE_MODE,
		  (value & 0xfffcffffu) | 0x00010000u);
	/* Initial m9x1172 -> m9x115 invalidates all 20 fresh null slots. */
	m9_address_invalidate_service_bank(mdev);
	m9_address_publish_cpu_window(mdev);
	address->service_ready = true;
	return 0;
}

int m9x_address_publish(struct m9x_device *mdev)
{
	struct m9x_address_space *address = mdev->transport.address;
	const struct m9_address_layout *layout;
	int ret;

	if (!address || !address->mapped)
		return -EINVAL;
	ret = m9x_address_prepare(mdev);
	if (ret)
		return ret;
	layout = &address->layout;

	/* Exact direct-MMIO range publication (m9x53/m9x421). */
	m9x_write(mdev, M9_REG_SURFACE_BASE2, layout->primary_base);
	m9x_write(mdev, M9_REG_SURFACE_BASE3, layout->primary_base);
	m9x_write(mdev, M9_REG_SURFACE_LIMIT1, layout->primary_limit);
	if (layout->secondary_bytes) {
		m9x_write(mdev, M9_REG_SURFACE_BASE0, layout->secondary_base);
		m9x_write(mdev, M9_REG_SURFACE_BASE1, layout->secondary_base);
		m9x_write(mdev, M9_REG_SURFACE_LIMIT0,
			  layout->secondary_base + layout->secondary_bytes - PAGE_SIZE);
	}
	m9x_write(mdev, M9_REG_TRANSLATION_BASE,
		  layout->translation_base & 0xfffff00cu);
	m9x_write(mdev, M9_REG_TRANSLATION_LIMIT, layout->translation_limit);
	m9x_write(mdev, M9_REG_PTE_BASE, layout->pte_base);
	m9x_write(mdev, M9_REG_PTE_LIMIT, layout->pte_limit);
	m9x_write(mdev, M9_REG_LOCAL_BASE0, layout->primary_base);
	m9x_write(mdev, M9_REG_LOCAL_BASE1, layout->primary_base);
	m9x_write(mdev, M9_REG_LOCAL_LIMIT, layout->primary_limit);

	/* The cold m9x421 path only writes MMIO; it emits no E6/22 stream. */
	m9x_read(mdev, M9_REG_LOCAL_LIMIT);
	address->windows_published = true;
	return 0;
}

int m9x_address_construct_manager(struct m9x_device *mdev)
{
	struct m9x_address_space *address = mdev->transport.address;
	const struct m9_address_layout *layout;
	u32 page, value, cpu_root_pages;

	if (!address || !address->mapped || !address->service_ready ||
	    !address->windows_published)
		return -EINVAL;
	if (address->manager_ready)
		return 0;
	layout = &address->layout;

	/*
	 * Exact fresh-service m9x53 construction.  m9x1027 first clears the
	 * primary child.  The M9128 5b6c2 initializer publishes direct CPU-root
	 * identity entries over all 256 MiB of BAR0, not just the root allocation.
	 * Restrict the native extent to supported local BAR pages and the
	 * observed 0x40000-byte root child.  The capture uses MmFlags=0x101b8;
	 * The ordinary Linux profile retains its compact-only construction;
	 * the private bit-6 companion profile uses the separately proven sparse
	 * secondary initializer before the same direct CPU-root identity loop.
	 */
	cpu_root_pages = min_t(u64, address->local_pages,
			min_t(u64, mdev->bar0_bytes >> PAGE_SHIFT,
			      M9_ADDRESS_TRANSLATION_BYTES / sizeof(u32)));
	if (cpu_root_pages < layout->root_bytes >> PAGE_SHIFT)
		return -ERANGE;
	iosys_map_memset(&address->map, layout->primary_base, 0,
			 layout->primary_bytes);
	/* Genuine 5bcf0 constructor: clear only the first DWORD at stride16,
	 * without the per-allocation pre/post lists. */
	for (page = 0; page < layout->secondary_bytes / M9_ADDRESS_SECONDARY_STRIDE;
	     page++)
		iosys_map_wr(&address->map,
			layout->secondary_base + page * M9_ADDRESS_SECONDARY_STRIDE,
			u32, 0);
	for (page = 0; page < cpu_root_pages; page++)
		iosys_map_wr(&address->map,
			     layout->translation_base + page * sizeof(u32),
			     u32, m9_address_root_pte_word(page));
	/*
	 * Local GPU addresses resolve through identity leaves.  The vendor
	 * publishes them per local allocation; publish the whole BAR-visible
	 * range up front.
	 */
	for (page = 0; page < address->local_pages; page++) {
		u32 local = (u32)M9_VRAM_GPU_BASE + (page << PAGE_SHIFT);

		iosys_map_wr(&address->map, m9_address_identity_slot(layout, local),
			     u32, m9_address_root_pte_word(local >> PAGE_SHIFT));
	}
	m9_address_activate(mdev);

	m9x_write(mdev, M9_REG_SERVICE_CONTROL, 0x1000);
	iosys_map_memset(&address->map,
			 layout->translation_base + M9_ADDRESS_TRANSLATION_BYTES,
			 0, layout->pte_base - (layout->translation_base +
						M9_ADDRESS_TRANSLATION_BYTES));
	iosys_map_memset(&address->map, layout->pte_base, 0, layout->pte_bytes);
	/* Finish the vendor child clears before the terminal manager activate. */
	wmb();
	m9x_write(mdev, M9_REG_FRONTEND_STATE, 0x0c);
	m9x_write(mdev, M9_REG_FRONTEND_STATE, 0x01);
	m9x_read(mdev, M9_REG_FRONTEND_STATE);
	address->manager_ready = true;

	/* System\\TlbPrefCnt=1 is published after m9x53 returns. */
	value = m9x_read(mdev, M9_REG_APERTURE_CONTROL);
	m9x_write(mdev, M9_REG_APERTURE_CONTROL,
		  (value & 0xfffffc3fu) | 0x40u);
	m9x_read(mdev, M9_REG_APERTURE_CONTROL);
	return 0;
}

static int m9_address_publish_binding(struct m9x_device *mdev,
				      struct m9x_address_space *address,
				      struct m9x_address_binding *binding)
{
	u32 pages = binding->bytes >> PAGE_SHIFT;
	u32 page;
	int ret;

	if (binding->published)
		return 0;
	for (page = 0; page < pages; page++) {
		u32 offset;
		int ret;

		ret = m9_address_slot(&address->layout,
				      binding->gpuva + page * PAGE_SIZE,
				      &offset);
		if (ret)
			return ret;
		iosys_map_wr(&address->map, offset, u32,
			     m9_address_binding_pte(binding, page));
	}
	if (address->layout.secondary_bytes) {
		wmb();
		for (page = 0; page < pages; page++) {
			u32 slot;

			ret = m9_address_slot(&address->layout,
				binding->gpuva + page * PAGE_SIZE, &slot);
			if (ret)
				return ret;
			if (iosys_map_rd(&address->map, slot, u32) !=
			    m9_address_binding_pte(binding, page))
				return -EIO;
		}
	}
	m9_address_activate(mdev);
	ret = m9_address_secondary_publish(mdev, binding->gpuva,
					   binding->bytes, true, false);
	if (ret)
		return ret;
	/* m9x810 completes every mode-1 allocation with this front-end kick. */
	m9x_write(mdev, M9_REG_FRONTEND_KICK, 1);
	binding->published = true;
	return 0;
}

int m9x_address_commit_bindings(struct m9x_device *mdev)
{
	struct m9x_address_space *address = mdev->transport.address;
	u32 binding_index;

	if (!address || !address->mapped || !address->service_ready ||
	    !address->windows_published || !address->manager_ready)
		return -EINVAL;
	if (!address->bindings_dirty)
		return 0;

	for (binding_index = 0; binding_index < address->binding_count;
	     binding_index++) {
		struct m9x_address_binding *binding =
			&address->bindings[binding_index];
		int ret;

		if (binding->published)
			continue;
		/*
		 * Every vendor m9x1030 allocation completes its own m9x887 ->
		 * m9x328 CPU mirror and activation before constructing the next
		 * allocation.  Do not batch independently allocated resources.
		 */
		ret = m9_address_publish_binding(mdev, address, binding);
		if (ret)
			return ret;
	}
	address->bindings_dirty = false;

	return 0;
}

int m9x_address_commit_gpuva(struct m9x_device *mdev, u32 gpuva)
{
	struct m9x_address_space *address = mdev->transport.address;
	u32 binding_index;

	if (!address || !address->mapped || !address->service_ready ||
	    !address->windows_published || !address->manager_ready)
		return -EINVAL;
	for (binding_index = 0; binding_index < address->binding_count;
	     binding_index++) {
		struct m9x_address_binding *binding =
			&address->bindings[binding_index];

		if (binding->gpuva != gpuva)
			continue;
		return m9_address_publish_binding(mdev, address, binding);
	}
	return -ENOENT;
}

/* Span of the class-1 system window: the largest DMA32 object it can name. */
u64 m9x_address_system_span(struct m9x_device *mdev)
{
	struct m9x_address_space *address = mdev->transport.address;

	if (!address || !address->system_mm_ready)
		return 0;
	return address->layout.system_end - M9_ADDRESS_SYSTEM_BASE;
}

static int m9_address_reserve_system(struct m9x_device *mdev,
				     struct drm_mm_node *node, u32 bytes)
{
	struct m9x_address_space *address = mdev->transport.address;

	lockdep_assert_held(&mdev->transport.lock);
	if (mdev->transport.wedged)
		return -EIO;
	if (!address || !address->system_mm_ready)
		return -ENODEV;
	if (!bytes)
		return -EINVAL;
	return drm_mm_insert_node_in_range(&address->system_mm, node,
					   PAGE_ALIGN(bytes), PAGE_SIZE, 0,
					   M9_ADDRESS_SYSTEM_BASE,
					   address->layout.system_end,
					   DRM_MM_INSERT_LOW);
}

static int m9_address_finish_system(struct m9x_device *mdev,
				    struct drm_mm_node *node, int ret)
{
	struct m9x_address_space *address = mdev->transport.address;

	if (!ret && address->manager_ready)
		ret = m9x_address_commit_bindings(mdev);
	drm_dbg(&mdev->drm, "M9 system map %#llx bytes %#llx: %d (manager %d)\n",
		node->start, node->size, ret, address->manager_ready);
	if (ret)
		m9x_address_unmap_system(mdev, node);
	return ret;
}

/* Reserve a class-1 address for a DMA32 allocation and stage its leaves. */
int m9x_address_map_system(struct m9x_device *mdev, struct drm_mm_node *node,
			   dma_addr_t dma, u32 bytes)
{
	int ret = m9_address_reserve_system(mdev, node, bytes);

	if (ret)
		return ret;
	return m9_address_finish_system(mdev, node,
			m9x_address_bind_dma(mdev, node->start, dma, bytes));
}

/* Source-owned 101f8 flags30 class1 owners: command buffers, geometry and
 * ordinary kind6 output. Their actual retained mapped prefixes are bounded
 * by 80 pages; backing allocation tails are not promoted to GPU inputs.
 * Ordinary coherent/SG/counter flags110 mappings retain their A policy. */
int m9x_address_map_faa_system(struct m9x_device *mdev,
                             struct drm_mm_node *node,
                             dma_addr_t dma, u32 bytes)
{
	struct m9x_address_space *a = mdev->transport.address;
	int ret;

	lockdep_assert_held(&mdev->transport.lock);
	if (!m9x_faa_diag_enabled() || !a || !a->layout.secondary_bytes ||
	    !bytes || bytes > 0x50000 || !IS_ALIGNED(bytes, PAGE_SIZE) ||
	    mdev->pdev->device != 0x0540 ||
	    mdev->pdev->revision != 1 || mdev->pdev->subsystem_device != 0x22c0)
		return -EINVAL;
	ret = m9_address_reserve_system(mdev, node, bytes);
	if (ret)
		return ret;
	return m9_address_finish_system(mdev, node,
		m9_address_bind_dma_policy(mdev, node->start, dma, bytes, true));
}

/* The private phased source allocates this owner only after preprocessing.
 * Reserve its live address early for relocation, without staging a binding
 * that engine initialization could prematurely publish. */
int m9x_address_reserve_faa_output(struct m9x_device *mdev,
                                  struct drm_mm_node *node, u32 bytes)
{
	struct m9x_address_space *a = mdev->transport.address;

	lockdep_assert_held(&mdev->transport.lock);
	if (!m9x_faa_diag_enabled() || !a || !a->layout.secondary_bytes ||
	    bytes != 0x50000 || mdev->pdev->device != 0x0540 ||
	    mdev->pdev->revision != 1 || mdev->pdev->subsystem_device != 0x22c0)
		return -EINVAL;
	return m9_address_reserve_system(mdev, node, bytes);
}

int m9x_address_publish_faa_output(struct m9x_device *mdev,
                                  const struct drm_mm_node *node,
                                  dma_addr_t dma)
{
	struct m9x_address_space *a = mdev->transport.address;
	u32 i;
	int ret;

	lockdep_assert_held(&mdev->transport.lock);
	if (!m9x_faa_diag_enabled() || !a || !a->layout.secondary_bytes ||
	    !a->manager_ready || !a->windows_published || !a->service_ready ||
	    !node || !drm_mm_node_allocated(node) || node->size != 0x50000 ||
	    node->start < M9_ADDRESS_SYSTEM_BASE || node->start > U32_MAX ||
	    !IS_ALIGNED(node->start, PAGE_SIZE) ||
	    node->start + node->size > a->layout.system_end ||
	    mdev->pdev->device != 0x0540 || mdev->pdev->revision != 1 ||
	    mdev->pdev->subsystem_device != 0x22c0)
		return -EINVAL;
	if (mdev->transport.wedged)
		return -EIO;
	/* A reserved-only owner must not silently accept earlier publication. */
	for (i = 0; i < a->binding_count; i++)
		if (a->bindings[i].gpuva >= node->start &&
		    a->bindings[i].gpuva < node->start + node->size)
			return -EALREADY;
	ret = m9_address_bind_dma_policy(mdev, node->start, dma, node->size, true);
	if (ret)
		return ret;
	/* Do not use finish_system: a post-GPU failure must retain this node and
	 * any partially published binding until bus mastering has stopped. */
	return m9x_address_commit_gpuva(mdev, node->start);
}

/* The output-only entry retains its stricter 320x256 allocation contract. */
int m9x_address_map_faa_output(struct m9x_device *mdev,
			     struct drm_mm_node *node, dma_addr_t dma, u32 bytes)
{
	if (bytes != 0x50000)
		return -EINVAL;
	return m9x_address_map_faa_system(mdev, node, dma, bytes);
}

/* The same for a mapped page array: one binding per DMA segment. */
int m9x_address_map_system_sgt(struct m9x_device *mdev,
			       struct drm_mm_node *node, struct sg_table *sgt,
			       u32 bytes)
{
	struct scatterlist *sg;
	u32 offset = 0;
	int ret, i;

	ret = m9_address_reserve_system(mdev, node, bytes);
	if (ret)
		return ret;
	for_each_sgtable_dma_sg(sgt, sg, i) {
		ret = m9x_address_bind_dma(mdev, node->start + offset,
					   sg_dma_address(sg), sg_dma_len(sg));
		if (ret)
			break;
		offset += sg_dma_len(sg);
	}
	if (!ret && offset < bytes)
		ret = -EINVAL;
	return m9_address_finish_system(mdev, node, ret);
}

void m9x_address_unmap_system(struct m9x_device *mdev, struct drm_mm_node *node)
{
	struct m9x_address_space *address = mdev->transport.address;
	u32 i, page;

	lockdep_assert_held(&mdev->transport.lock);
	if (!address || !drm_mm_node_allocated(node))
		return;
	for (i = 0; i < address->binding_count;) {
		struct m9x_address_binding *binding = &address->bindings[i];

		if (binding->gpuva < node->start ||
		    binding->gpuva >= node->start + node->size) {
			i++;
			continue;
		}
		/* A failed companion readback leaves FRONTEND_STATE=5 frozen.
		 * Removing CPU bookkeeping must not activate that corrupt image,
		 * including later bindings after an invalidation itself wedges.
		 * Teardown stops bus mastering before releasing reachable owners. */
		if (binding->published && !mdev->transport.wedged) {
			for (page = 0; page < binding->bytes >> PAGE_SHIFT; page++)
				iosys_map_wr(&address->map,
					     m9_address_identity_slot(&address->layout, binding->gpuva +
								      page * PAGE_SIZE),
					     u32, 0);
			m9_address_activate(mdev);
			m9_address_secondary_publish(mdev, binding->gpuva,
						     binding->bytes, true, true);
		}
		address->bindings[i] = address->bindings[--address->binding_count];
	}
	drm_mm_remove_node(node);
}

int m9x_address_restore_region(struct m9x_device *mdev,
			       struct iosys_map *map, u32 *base)
{
	struct m9x_address_space *address = mdev->transport.address;

	if (!address || !address->mapped || !map || !base)
		return -EINVAL;
	*base = address->layout.restore_base;
	*map = IOSYS_MAP_INIT_OFFSET(&address->map, *base);
	return 0;
}

int m9x_address_publish_restore(struct m9x_device *mdev)
{
	struct m9x_address_space *address = mdev->transport.address;
	u32 base, page;
	int ret;

	if (!address || !address->mapped || !address->service_ready ||
	    !address->windows_published || !address->manager_ready)
		return -EINVAL;
	if (address->restore_published)
		return 0;
	/* m9x182's mode-3 restore pool is local: identity leaves, then a kick. */
	base = address->layout.restore_base;
	for (page = 0; page < PARHELIA_M9_RESTORE_BYTES >> PAGE_SHIFT; page++)
		iosys_map_wr(&address->map,
			     m9_address_identity_slot(&address->layout, base) + page * sizeof(u32),
			     u32, m9_address_root_pte_word((base >> PAGE_SHIFT) +
							  page));
	m9_address_activate(mdev);
	ret = m9_address_secondary_publish(mdev, base,
					 PARHELIA_M9_RESTORE_BYTES, false, false);
	if (ret)
		return ret;
	m9x_write(mdev, M9_REG_FRONTEND_KICK, 1);
	address->restore_published = true;
	return 0;
}

int m9x_address_finalize_service(struct m9x_device *mdev)
{
	struct m9x_address_space *address = mdev->transport.address;
	u32 i;
	int ret;

	if (!address || !address->service_ready || !address->manager_ready ||
	    !mdev->transport.transport_armed)
		return -EINVAL;
	if (address->service_finalized)
		return 0;
	/*
	 * m9x926 calls m9x1030 directly in mode 0 after m9x109. Applied
	 * System\\MmFlags=0x100e8 enables m9x535 -> m9x887 -> m9x328.
	 * No scatter list: each local resource receives identity PTEs with
	 * nibble 8 at its local-domain address 0x10000000 + offset in the
	 * primary child.  Each pool ends with its own 31c triplet, without 314.
	 */
	for (i = 0; i < M9_ADDRESS_SERVICE_POOLS; i++) {
		u32 local = address->layout.pool_base[i];
		u32 pages = DIV_ROUND_UP(m9_service_pool_bytes[i], PAGE_SIZE);
		u32 slot = m9_address_identity_slot(&address->layout, local);
		u32 page;

		for (page = 0; page < pages; page++)
			iosys_map_wr(&address->map, slot + page * sizeof(u32),
				     u32, m9_address_root_pte_word(
					(local >> PAGE_SHIFT) + page));
		m9_address_activate(mdev);
		ret = m9_address_secondary_publish(mdev, local,
						   pages << PAGE_SHIFT, false, false);
		if (ret)
			return ret;
		dev_info(&mdev->pdev->dev,
			 "M9 mode-0 service pool %u: GPU=%#x bytes=%#x PTE=%#x/%#x\n",
			 i, local, m9_service_pool_bytes[i], slot,
			 iosys_map_rd(&address->map, slot, u32));
	}
	/* m9x1141 publishes once; the explicit m9x582 tail repeats m9x115. */
	m9_address_invalidate_service_bank(mdev);
	m9_address_invalidate_service_bank(mdev);
	m9_address_publish_cpu_window(mdev);
	address->service_finalized = true;
	return 0;
}

/* The private FAA capture uses selector1 virtual local resources. The
 * manager already publishes identity leaves for every BAR-visible VRAM
 * page. Verify that explicit join, rather than assuming raw local and
 * virtual addresses have the same meaning. No mapping is changed here. */
static int m9_address_check_local_compact(struct m9x_device *mdev,
					 u32 base, u32 bytes)
{
	struct m9x_address_space *a = mdev->transport.address;
	u64 end = (u64)base + bytes;
	u32 address, slot, root_slot, page;

	lockdep_assert_held(&mdev->transport.lock);
	if (!a || !a->manager_ready || !a->mapped || M9_VRAM_GPU_BASE || !bytes ||
	    !IS_ALIGNED(base, PAGE_SIZE) || !IS_ALIGNED(bytes, PAGE_SIZE) ||
	    end > ((u64)a->local_pages << PAGE_SHIFT))
		return -ERANGE;
	for (address = base; address < end; address += PAGE_SIZE) {
		slot = m9_address_identity_slot(&a->layout, address);
		if (slot > a->layout.primary_base + a->layout.primary_bytes - sizeof(u32) ||
		    iosys_map_rd(&a->map, slot, u32) != (address | 8u))
			return -EFAULT;
		page = slot >> PAGE_SHIFT;
		root_slot = a->layout.translation_base + page * sizeof(u32);
		if (root_slot > a->layout.translation_limit - sizeof(u32) ||
		    iosys_map_rd(&a->map, root_slot, u32) != ((page << PAGE_SHIFT) | 8u))
			return -EFAULT;
	}
	return 0;
}

int m9x_address_check_local_identity(struct m9x_device *mdev, u32 base, u32 bytes)
{
	struct m9x_address_space *a = mdev->transport.address;
	u64 end = (u64)base + bytes;
	u32 address, slot;
	int ret = m9_address_check_local_compact(mdev, base, bytes);

	if (ret || !a->layout.secondary_bytes)
		return ret;
	for (address = base; address < end; address += PAGE_SIZE) {
		if (m9_address_secondary_slot(&a->layout, address, &slot) ||
		    iosys_map_rd(&a->map, slot, u32) != (address | 8u) ||
		    iosys_map_rd(&a->map, slot + 4, u32) ||
		    iosys_map_rd(&a->map, slot + 8, u32))
			return -EFAULT;
	}
	return 0;
}

/* The private caller owns each pinned local range. Reproduce its actual
 * compact then secondary allocation publication, keeping DWORD3 untouched.
 * Validate every first word before changing any mapping. */
int m9x_address_local_prepare(struct m9x_device *mdev, u32 base, u32 bytes)
{
	struct m9x_address_space *a = mdev->transport.address;
	u64 end = (u64)base + bytes;
	u32 address, slot, value;
	int ret = m9_address_check_local_compact(mdev, base, bytes);

	if (ret || !a->layout.secondary_bytes)
		return ret;
	/* Matching known records already satisfy this owned publication. */
	if (!m9x_address_check_local_identity(mdev, base, bytes))
		return 0;
	for (address = base; address < end; address += PAGE_SIZE) {
		ret = m9_address_secondary_slot(&a->layout, address, &slot);
		if (ret)
			return ret;
		value = iosys_map_rd(&a->map, slot, u32);
		if (value && value != (address | 8u))
			return -EEXIST;
	}
	for (address = base; address < end; address += PAGE_SIZE)
		iosys_map_wr(&a->map, m9_address_identity_slot(&a->layout, address),
			     u32, address | 8u);
	m9_address_activate(mdev);
	ret = m9_address_secondary_publish(mdev, base, bytes, false, false);
	if (!ret)
		ret = m9x_address_check_local_identity(mdev, base, bytes);
	return ret;
}

/* Genuine M9128 26830 -> 597a0 -> 59410 CPU-map publication copies
 * primary leaves into the 0360 root child at the surface page indices.
 * Those direct CPU entries differ from the root entries which map the
 * primary table's own storage.  A local GPU identity check alone proves
 * only the latter.  Caller owns the pinned surface and transport lock. */
int m9x_address_cpu_local_prepare(struct m9x_device *mdev, u32 base, u32 bytes)
{
	struct m9x_address_space *a = mdev->transport.address;
	u64 end = (u64)base + bytes;
	u32 address, slot, root_slot, leaf, installed;
	int ret;

	lockdep_assert_held(&mdev->transport.lock);
	ret = m9x_address_local_prepare(mdev, base, bytes);
	if (ret)
		return ret;
	/* The real CPU child is 0x40000 bytes (65536 page entries).  The
	 * published window can include a second child, which is not this join. */
	if (end > (u64)M9_ADDRESS_TRANSLATION_BYTES / sizeof(u32) * PAGE_SIZE ||
	    a->layout.translation_base > a->layout.translation_limit ||
	    a->layout.translation_limit - a->layout.translation_base <
					M9_ADDRESS_TRANSLATION_BYTES - sizeof(u32))
		return -ERANGE;
	if (m9x_read(mdev, M9_REG_TRANSLATION_BASE) !=
			(a->layout.translation_base & 0xfffff00cu) ||
	    m9x_read(mdev, M9_REG_TRANSLATION_LIMIT) !=
			a->layout.translation_limit)
		return -ESTALE;
	/* Validate every destination before changing any entry.  Preserve a
	 * live nonidentity namespace rather than replacing its mapping. */
	for (address = base; address < end; address += PAGE_SIZE) {
		slot = m9_address_identity_slot(&a->layout, address);
		leaf = iosys_map_rd(&a->map, slot, u32);
		root_slot = a->layout.translation_base +
				(address >> PAGE_SHIFT) * sizeof(u32);
		installed = iosys_map_rd(&a->map, root_slot, u32);
		if (installed && installed != leaf)
			return -EEXIST;
	}
	for (address = base; address < end; address += PAGE_SIZE) {
		slot = m9_address_identity_slot(&a->layout, address);
		root_slot = a->layout.translation_base +
				(address >> PAGE_SHIFT) * sizeof(u32);
		iosys_map_wr(&a->map, root_slot, u32,
			     iosys_map_rd(&a->map, slot, u32));
	}
	/* Actual 594b6 -> 59af0 packed token 0x30000c8f emits f,0,c.
	 * Complete the CPU mirror before publishing a tagged aperture slot. */
	wmb();
	m9x_write(mdev, M9_REG_FRONTEND_STATE, 0x0f);
	m9x_write(mdev, M9_REG_FRONTEND_STATE, 0x00);
	m9x_write(mdev, M9_REG_FRONTEND_STATE, 0x0c);
	m9x_read(mdev, M9_REG_FRONTEND_STATE);
	return 0;
}

int m9x_address_compact_slot(struct m9x_device *mdev, u32 gpuva, u32 *slot)
{
	struct m9x_address_space *a = mdev->transport.address;

	if (!a || !a->mapped || !slot)
		return -EINVAL;
	return m9_address_slot(&a->layout, gpuva, slot);
}

int m9x_address_read_pte(struct m9x_device *mdev, u32 gpuva, u32 *value)
{
	struct m9x_address_space *address = mdev->transport.address;
	u32 offset;
	int ret;

	if (!address || !address->mapped || !value)
		return -EINVAL;
	ret = m9_address_slot(&address->layout, gpuva, &offset);
	if (ret)
		return ret;
	*value = iosys_map_rd(&address->map, offset, u32);
	return 0;
}

int m9x_address_read_root_pte(struct m9x_device *mdev, u32 gpuva, u32 *value)
{
	struct m9x_address_space *address = mdev->transport.address;
	u32 offset, root_page, root_offset;
	int ret;

	if (!value || !address || !address->mapped)
		return -EINVAL;
	ret = m9_address_slot(&address->layout, gpuva, &offset);
	if (ret)
		return ret;
	root_page = offset >> PAGE_SHIFT;
	if (check_mul_overflow(root_page, (u32)sizeof(u32), &root_offset) ||
	    check_add_overflow(root_offset, address->layout.translation_base,
			       &root_offset) ||
	    root_offset > address->layout.translation_limit - sizeof(u32))
		return -ERANGE;
	*value = iosys_map_rd(&address->map, root_offset, u32);
	return 0;
}

void m9x_address_fini(struct m9x_device *mdev)
{
	struct m9x_address_space *address = mdev->transport.address;
	int ret;

	if (!address)
		return;
	m9_address_free_service_pools(address);
	if (address->mapped) {
		ret = ttm_bo_reserve(&address->root->bo, false, false, NULL);
		if (!ret) {
			drm_gem_vram_vunmap(address->root, &address->map);
			ttm_bo_unreserve(&address->root->bo);
			address->mapped = false;
		} else {
			dev_warn(&mdev->pdev->dev,
				 "cannot reserve M9 address root for unmap: %d\n",
				 ret);
			return;
		}
	}
	if (address->root) {
		m9x_vram_unpin(address->root);
		drm_gem_vram_put(address->root);
	}
	if (address->system_mm_ready) {
		struct drm_mm_node *node, *next;

		/* BOs outliving the device see unallocated nodes on free. */
		drm_mm_for_each_node_safe(node, next, &address->system_mm)
			drm_mm_remove_node(node);
		drm_mm_takedown(&address->system_mm);
		address->system_mm_ready = false;
	}
	kfree(address->bindings);
	mdev->transport.address = NULL;
}
