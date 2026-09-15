// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 Rene Rebe <rene@exactco.de> */
/*
 * M9148 compact translation table and address-window ownership.
 *
 * The Linux vendor driver reserves one monolithic low-VRAM image.  Its
 * hardware addresses are offsets from VRAM.  Ordinary system render
 * resources retain their public 0x40000000 GPU addresses: m9x328 indexes
 * those page numbers directly from the compact origin at 0x801000.
 * Reserving the image before any KMS BO gives the clean-room driver a BAR0
 * CPU mapping without changing the low addresses consumed by the manager.
 */
#include <linux/iosys-map.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include "m9_drv.h"
#include "m9_reg.h"

#define M9_ADDRESS_ROOT_BYTES		0x00d41000u
#define M9_ADDRESS_PTE_OFFSET		0x008e1000u
#define M9_ADDRESS_PTE_BYTES		0x00060000u
#define M9_ADDRESS_PTE_LIMIT		(M9_ADDRESS_PTE_OFFSET + \
					 M9_ADDRESS_PTE_BYTES - PAGE_SIZE)
#define M9_ADDRESS_COMPACT_ORIGIN	0x00801000u
#define M9_ADDRESS_TRANSLATION_BASE	0x00861000u
#define M9_ADDRESS_TRANSLATION_LIMIT	0x008e0000u
#define M9_ADDRESS_PRIMARY_BYTES	(M9_ADDRESS_TRANSLATION_BASE - \
					 M9_ADDRESS_COMPACT_ORIGIN)
#define M9_ADDRESS_TRANSLATION_BYTES	0x00040000u
#define M9_ADDRESS_ROOT_PAGES		(M9_ADDRESS_ROOT_BYTES >> PAGE_SHIFT)
#define M9_ADDRESS_SYSTEM_BASE		0x40000000u
#define M9_ADDRESS_SYSTEM_END		0x50000000u
#define M9_ADDRESS_PRIMARY_BASE		0x00801000u
#define M9_ADDRESS_PRIMARY_LIMIT	0x00860000u
#define M9_ADDRESS_SERVICE_SLOTS	20u
#define M9_ADDRESS_SERVICE_STRIDE	0x10u
#define M9_ADDRESS_SERVICE_INVALID	0x0000ffffu

struct m9x_address_binding {
	u32 gpuva;
	u32 bytes;
	dma_addr_t dma;
	bool published;
};

struct m9x_address_space {
	struct drm_gem_vram_object *root;
	struct iosys_map map;
	struct m9x_address_binding *bindings;
	u32 binding_count;
	u32 binding_capacity;
	bool mapped;
	bool service_ready;
	bool windows_published;
	bool bindings_dirty;
	bool manager_ready;
	bool service_finalized;
};

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

static u32 m9_address_pte_word(dma_addr_t dma)
{
	return (lower_32_bits(dma) & 0xfffff000u) |
	       ((dma >> 28) & 0x00000ff0u) | 0xau;
}

static u32 m9_address_root_pte_word(u32 page)
{
	return (page << PAGE_SHIFT) | 0x8u;
}

static int m9_address_slot(u32 gpuva, u32 *offset)
{
	u32 page, byte;

	if (gpuva < M9_ADDRESS_SYSTEM_BASE || gpuva >= M9_ADDRESS_SYSTEM_END ||
	    !IS_ALIGNED(gpuva, PAGE_SIZE))
		return -ERANGE;
	/*
	 * m9x887 reads this same public resource address from object +0x48 and
	 * stores its page number in the descriptor consumed by m9x328.
	 */
	page = gpuva >> PAGE_SHIFT;
	if (check_mul_overflow(page, (u32)sizeof(u32), &byte) ||
	    check_add_overflow(byte, M9_ADDRESS_COMPACT_ORIGIN, &byte) ||
	    byte < M9_ADDRESS_PTE_OFFSET ||
	    byte >= M9_ADDRESS_PTE_OFFSET + M9_ADDRESS_PTE_BYTES)
		return -ERANGE;
	*offset = byte;
	return 0;
}

static int m9_address_selftest(void)
{
	u32 offset;

	if (M9_ADDRESS_PRIMARY_BASE != 0x00801000 ||
	    M9_ADDRESS_PRIMARY_LIMIT != 0x00860000 ||
	    M9_ADDRESS_PTE_LIMIT != 0x00940000 ||
	    m9_address_slot(0x40000000, &offset) || offset != 0x00901000 ||
	    m9_address_slot(0x4ffff000, &offset) || offset != 0x00940ffc ||
	    !m9_address_slot(0x3ffff000, &offset) ||
	    !m9_address_slot(0x50000000, &offset) ||
	    m9_address_pte_word((dma_addr_t)0x0232a000) != 0x0232a00a ||
	    m9_address_pte_word((dma_addr_t)0xab12345000ULL) != 0x12345aba ||
	    M9_ADDRESS_ROOT_PAGES != 0x0d41 ||
	    m9_address_root_pte_word(0) != 0x00000008 ||
	    m9_address_root_pte_word(0x901) != 0x00901008 ||
	    m9_address_root_pte_word(M9_ADDRESS_ROOT_PAGES - 1) !=
		0x00d40008)
		return -EINVAL;
	return 0;
}

int m9x_address_init(struct m9x_device *mdev)
{
	struct m9x_address_space *address;
	struct drm_gem_vram_object *root;
	s64 offset;
	int ret;

	ret = m9_address_selftest();
	if (ret)
		return ret;
	if (mdev->transport.address)
		return 0;
	address = devm_kzalloc(&mdev->pdev->dev, sizeof(*address), GFP_KERNEL);
	if (!address)
		return -ENOMEM;
	root = drm_gem_vram_create(&mdev->drm, M9_ADDRESS_ROOT_BYTES, 1);
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
	iosys_map_memset(&address->map, 0, 0, M9_ADDRESS_ROOT_BYTES);
	/* Publish the cleared low-VRAM manager image before handing it to HW. */
	wmb();
	mdev->transport.address = address;
	dev_info(&mdev->pdev->dev,
		 "M9 reserved vendor low-VRAM root [0,%#x), active PTE [%#x,%#x)\n",
		 M9_ADDRESS_ROOT_BYTES, M9_ADDRESS_PTE_OFFSET,
		 M9_ADDRESS_PTE_OFFSET + M9_ADDRESS_PTE_BYTES);
	return 0;

out_unpin:
	m9x_vram_unpin(root);
out_put:
	drm_gem_vram_put(root);
	return ret;
}

int m9x_address_bind_dma(struct m9x_device *mdev, u32 gpuva,
			 dma_addr_t dma, u32 bytes)
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
	    gpuva_end > M9_ADDRESS_SYSTEM_END ||
	    check_add_overflow((u64)dma, (u64)span, &dma_end) ||
	    dma_end > (u64)U32_MAX + 1)
		return -ERANGE;
	/*
	 * Validate both endpoints before staging a binding, so a rejected bind
	 * cannot leave a partial mapping behind.
	 */
	if (m9_address_slot(gpuva, &slot) ||
	    m9_address_slot(gpuva_end - PAGE_SIZE, &slot))
		return -ERANGE;
	for (i = 0; i < address->binding_count; i++) {
		const struct m9x_address_binding *binding =
			&address->bindings[i];
		u32 binding_end = binding->gpuva + binding->bytes;

		if (binding->gpuva == gpuva && binding->bytes == span &&
		    binding->dma == dma)
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
		};
	address->bindings_dirty = true;
	return 0;
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
	address->service_ready = true;
	return 0;
}

int m9x_address_publish(struct m9x_device *mdev)
{
	struct m9x_address_space *address = mdev->transport.address;
	int ret;

	if (!address || !address->mapped)
		return -EINVAL;
	ret = m9x_address_prepare(mdev);
	if (ret)
		return ret;

	/* Exact direct-MMIO range publication for a 256 MiB M9148. */
	m9x_write(mdev, M9_REG_SURFACE_BASE2, M9_ADDRESS_PRIMARY_BASE);
	m9x_write(mdev, M9_REG_SURFACE_BASE3, M9_ADDRESS_PRIMARY_BASE);
	m9x_write(mdev, M9_REG_SURFACE_LIMIT1, M9_ADDRESS_PRIMARY_LIMIT);
	m9x_write(mdev, M9_REG_TRANSLATION_BASE,
		  M9_ADDRESS_TRANSLATION_BASE & 0xfffff00cu);
	m9x_write(mdev, M9_REG_TRANSLATION_LIMIT,
		  M9_ADDRESS_TRANSLATION_LIMIT);
	m9x_write(mdev, M9_REG_PTE_BASE, M9_ADDRESS_PTE_OFFSET);
	m9x_write(mdev, M9_REG_PTE_LIMIT, M9_ADDRESS_PTE_LIMIT);
	m9x_write(mdev, M9_REG_LOCAL_BASE0, M9_ADDRESS_PRIMARY_BASE);
	m9x_write(mdev, M9_REG_LOCAL_BASE1, M9_ADDRESS_PRIMARY_BASE);
	m9x_write(mdev, M9_REG_LOCAL_LIMIT, M9_ADDRESS_PRIMARY_LIMIT);

	/*
	 * The caller completes m9x421's final E6/22 stream before starting the
	 * later m9x53 compact-table phase.
	 */
	m9x_read(mdev, M9_REG_LOCAL_LIMIT);
	address->windows_published = true;
	return 0;
}

int m9x_address_construct_manager(struct m9x_device *mdev)
{
	struct m9x_address_space *address = mdev->transport.address;
	u32 page;
	u32 value;

	if (!address || !address->mapped || !address->service_ready ||
	    !address->windows_published)
		return -EINVAL;
	if (address->manager_ready)
		return 0;

	/*
	 * Exact fresh-service m9x53 construction for the captured 256 MiB M9148
	 * profile.  m9x1027 first clears the primary child.  Selector-zero
	 * m9x328 then CPU-mirrors one identity entry for every page in the
	 * 0xd41000-byte root allocation into the child selected by 0x360/0x364.
	 */
	iosys_map_memset(&address->map, M9_ADDRESS_COMPACT_ORIGIN, 0,
			 M9_ADDRESS_PRIMARY_BYTES);
	for (page = 0; page < M9_ADDRESS_ROOT_PAGES; page++)
		iosys_map_wr(&address->map,
			     M9_ADDRESS_TRANSLATION_BASE + page * sizeof(u32),
			     u32, m9_address_root_pte_word(page));
	/* Make the BAR/WC identity table visible before its frontend activate. */
	wmb();
	m9x_write(mdev, M9_REG_FRONTEND_STATE, 0x0f);
	m9x_write(mdev, M9_REG_FRONTEND_STATE, 0x01);
	m9x_write(mdev, M9_REG_FRONTEND_STATE, 0x00);
	m9x_read(mdev, M9_REG_FRONTEND_STATE);

	m9x_write(mdev, M9_REG_SERVICE_CONTROL, 0x1000);
	iosys_map_memset(&address->map,
			 M9_ADDRESS_TRANSLATION_BASE +
			 M9_ADDRESS_TRANSLATION_BYTES,
			 0, M9_ADDRESS_PTE_OFFSET -
			 (M9_ADDRESS_TRANSLATION_BASE +
			  M9_ADDRESS_TRANSLATION_BYTES));
	iosys_map_memset(&address->map, M9_ADDRESS_PTE_OFFSET, 0,
			 M9_ADDRESS_PTE_BYTES);
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

	if (binding->published)
		return 0;
	for (page = 0; page < pages; page++) {
		u32 offset;
		int ret;

		ret = m9_address_slot(binding->gpuva + page * PAGE_SIZE,
				      &offset);
		if (ret)
			return ret;
		iosys_map_wr(&address->map, offset, u32,
			     m9_address_pte_word(binding->dma +
						     page * PAGE_SIZE));
	}
	/* Make this allocation's BAR/WC leaves visible before activation. */
	wmb();
	m9x_write(mdev, M9_REG_FRONTEND_STATE, 0x0f);
	m9x_write(mdev, M9_REG_FRONTEND_STATE, 0x01);
	m9x_write(mdev, M9_REG_FRONTEND_STATE, 0x00);
	m9x_read(mdev, M9_REG_FRONTEND_STATE);
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

int m9x_address_finalize_service(struct m9x_device *mdev)
{
	struct m9x_address_space *address = mdev->transport.address;

	if (!address || !address->service_ready || !address->manager_ready)
		return -EINVAL;
	if (address->service_finalized)
		return 0;
	/* m9x1141 publishes once; the explicit m9x582 tail repeats m9x115. */
	m9_address_invalidate_service_bank(mdev);
	m9_address_invalidate_service_bank(mdev);
	address->service_finalized = true;
	return 0;
}

int m9x_address_read_pte(struct m9x_device *mdev, u32 gpuva, u32 *value)
{
	struct m9x_address_space *address = mdev->transport.address;
	u32 offset;
	int ret;

	if (!address || !address->mapped || !value)
		return -EINVAL;
	ret = m9_address_slot(gpuva, &offset);
	if (ret)
		return ret;
	*value = iosys_map_rd(&address->map, offset, u32);
	return 0;
}

void m9x_address_fini(struct m9x_device *mdev)
{
	struct m9x_address_space *address = mdev->transport.address;
	int ret;

	if (!address)
		return;
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
	kfree(address->bindings);
	mdev->transport.address = NULL;
}
