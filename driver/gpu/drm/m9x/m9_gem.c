// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/dma-mapping.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/sizes.h>
#include <linux/vmalloc.h>

#include <drm/drm_file.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>
#include <drm/drm_prime.h>
#include <drm/drm_gem_ttm_helper.h>
#include <drm/drm_gem_vram_helper.h>
#include <drm/ttm/ttm_resource.h>

#include "m9_drv.h"

/* The largest object each memory class can ever serve.  A mapped VRAM object
 * has to fit the CPU-visible VRAM the TTM manager owns, an engine-only one the
 * video memory above it, a class-1 addressed DMA32 object the system address
 * window, and a command object only the 32-bit bus address space its raw
 * fetches use. */
static u64 m9_bo_max_size(struct m9x_device *mdev, u32 flags)
{
	if (flags & PARHELIA_M9_BO_ENGINE_ONLY)
		return mdev->engine_vram_ready ?
			m9x_vram_gpu_limit(mdev) - mdev->vram_visible : 0;
	if (flags & (PARHELIA_M9_BO_VRAM | PARHELIA_M9_BO_TILED))
		return mdev->drm.vram_mm->vram_size;
	if (flags & PARHELIA_M9_BO_COMMAND)
		return SZ_4G;
	return m9x_address_system_span(mdev);
}

/* The driver never evicts: every VRAM object is pinned whole for as long as a
 * client holds it, so one larger than the unused VRAM can never become
 * resident.  Say so at create, while the client can still take another path,
 * instead of letting the pin fail once a job is already built. */
static bool m9_vram_free_for(struct m9x_device *mdev, u64 size)
{
	struct ttm_resource_manager *man =
		ttm_manager_type(&mdev->drm.vram_mm->bdev, TTM_PL_VRAM);

	return size <= mdev->drm.vram_mm->vram_size -
			ttm_resource_manager_usage(man);
}

static bool m9_bo_force_paged;
module_param(m9_bo_force_paged, bool, 0600);
MODULE_PARM_DESC(m9_bo_force_paged,
	"Back non-command DMA32 BOs by page arrays (fragmentation fallback test)");

/* Allocation counters, read back from sysfs to size the per-frame BO churn. */
static unsigned long m9_bo_vram_creates, m9_bo_vram_kib;
static unsigned long m9_bo_dma_creates, m9_bo_dma_kib;
static unsigned long m9_bo_reserve_creates, m9_bo_paged_creates;
static unsigned long m9_bo_engine_creates, m9_bo_engine_kib;
module_param(m9_bo_vram_creates, ulong, 0444);
module_param(m9_bo_vram_kib, ulong, 0444);
module_param(m9_bo_dma_creates, ulong, 0444);
module_param(m9_bo_dma_kib, ulong, 0444);
module_param(m9_bo_reserve_creates, ulong, 0444);
module_param(m9_bo_paged_creates, ulong, 0444);
module_param(m9_bo_engine_creates, ulong, 0444);
module_param(m9_bo_engine_kib, ulong, 0444);

/* A draw range is fetched by its raw bus address, so a BO the engine reads
 * that way has to be one physically contiguous range.  Above the costly order
 * the page allocator cannot produce one once memory is fragmented, so the
 * driver holds its own contiguous DMA32 ranges and suballocates them. */
static uint m9_dma_reserve_kib = 2048;
module_param_named(dma_reserve_kib, m9_dma_reserve_kib, uint, 0400);
MODULE_PARM_DESC(dma_reserve_kib,
	"contiguous DMA32 memory reserved per chunk, in KiB");

static struct m9x_dma_chunk *m9_dma_chunk_add(struct m9x_device *mdev,
					      size_t least)
{
	struct device *dev = drm_dev_dma_dev(&mdev->drm);
	size_t size = max_t(size_t, (size_t)m9_dma_reserve_kib << 10, least);
	struct m9x_dma_chunk *chunk;

	chunk = kzalloc_obj(*chunk);
	if (!chunk)
		return NULL;
	for (size = PAGE_ALIGN(size); size >= least && size >= PAGE_SIZE;
	     size /= 2) {
		chunk->vaddr = dma_alloc_wc(dev, size, &chunk->dma,
					    GFP_KERNEL | __GFP_RETRY_MAYFAIL |
					    __GFP_NOWARN | __GFP_ZERO);
		if (!chunk->vaddr)
			continue;
		if (!upper_32_bits(chunk->dma) && chunk->dma <= U32_MAX - size)
			break;
		dma_free_wc(dev, size, chunk->vaddr, chunk->dma);
		chunk->vaddr = NULL;
	}
	if (!chunk->vaddr) {
		kfree(chunk);
		return NULL;
	}
	chunk->size = size;
	drm_mm_init(&chunk->mm, 0, size);
	list_add(&chunk->link, &mdev->dma_chunks);
	drm_info(&mdev->drm, "reserved %zu KiB of contiguous DMA32 at %pad\n",
		 size >> 10, &chunk->dma);
	return chunk;
}

static bool m9_dma_reserve_alloc(struct m9x_device *mdev, struct m9x_bo *bo,
				 size_t size)
{
	struct m9x_dma_chunk *chunk;
	bool grown = false;

	mutex_lock(&mdev->dma_lock);
	for (;;) {
		list_for_each_entry(chunk, &mdev->dma_chunks, link)
			if (!drm_mm_insert_node_generic(&chunk->mm, &bo->reserve,
							size, PAGE_SIZE, 0,
							DRM_MM_INSERT_BEST)) {
				bo->dma.dma_addr = chunk->dma + bo->reserve.start;
				bo->dma.vaddr = chunk->vaddr + bo->reserve.start;
				memset(bo->dma.vaddr, 0, size);
				m9_bo_reserve_creates++;
				mutex_unlock(&mdev->dma_lock);
				return true;
			}
		if (grown || !m9_dma_chunk_add(mdev, size))
			break;
		grown = true;
	}
	mutex_unlock(&mdev->dma_lock);
	return false;
}

static void m9_dma_reserve_free(struct m9x_device *mdev, struct m9x_bo *bo)
{
	mutex_lock(&mdev->dma_lock);
	drm_mm_remove_node(&bo->reserve);
	mutex_unlock(&mdev->dma_lock);
}

static void m9_dma_reserve_release(struct drm_device *drm, void *unused)
{
	struct m9x_device *mdev = to_m9x(drm);
	struct m9x_dma_chunk *chunk, *next;

	list_for_each_entry_safe(chunk, next, &mdev->dma_chunks, link) {
		list_del(&chunk->link);
		drm_mm_takedown(&chunk->mm);
		dma_free_wc(drm_dev_dma_dev(drm), chunk->size, chunk->vaddr,
			    chunk->dma);
		kfree(chunk);
	}
}

/* Reserve one chunk while memory is least fragmented, before any client. */
int m9x_dma_reserve_init(struct m9x_device *mdev)
{
	int ret;

	INIT_LIST_HEAD(&mdev->dma_chunks);
	ret = drmm_mutex_init(&mdev->drm, &mdev->dma_lock);
	if (ret)
		return ret;
	ret = drmm_add_action_or_reset(&mdev->drm, m9_dma_reserve_release, NULL);
	if (ret)
		return ret;
	mutex_lock(&mdev->dma_lock);
	if (m9_dma_reserve_kib && !m9_dma_chunk_add(mdev, SZ_256K))
		drm_warn(&mdev->drm, "no contiguous DMA32 reserve available\n");
	mutex_unlock(&mdev->dma_lock);
	return 0;
}

static void m9_bo_free(struct drm_gem_object *gem)
{
	struct m9x_bo *bo = container_of(to_drm_gem_dma_obj(gem),
						   struct m9x_bo, dma);
	struct m9x_device *mdev = to_m9x(gem->dev);

	if (drm_mm_node_allocated(&bo->system)) {
		mutex_lock(&mdev->transport.lock);
	m9x_transport_drain_locked(mdev);
		m9x_address_unmap_system(mdev, &bo->system);
		mutex_unlock(&mdev->transport.lock);
	}
	if (drm_mm_node_allocated(&bo->reserve))
		m9_dma_reserve_free(mdev, bo);
	else if (bo->dma.vaddr)
		dma_free_wc(drm_dev_dma_dev(gem->dev), gem->size,
			    bo->dma.vaddr, bo->dma.dma_addr);
	drm_gem_object_release(gem);
	kfree(bo);
}

/* One contiguous range, including the sub-range case where the backing came
 * from the driver's own DMA32 reserve: that reserve is itself one coherent
 * allocation, so the pair the BO holds describes its own part of it. */
static struct sg_table *m9_bo_get_sg_table(struct drm_gem_object *gem)
{
	struct m9x_bo *bo = container_of(to_drm_gem_dma_obj(gem),
					 struct m9x_bo, dma);
	struct sg_table *sgt;
	int ret;

	sgt = kzalloc_obj(*sgt);
	if (!sgt)
		return ERR_PTR(-ENOMEM);
	ret = dma_get_sgtable(drm_dev_dma_dev(gem->dev), sgt, bo->dma.vaddr,
			      bo->dma.dma_addr, gem->size);
	if (ret) {
		kfree(sgt);
		return ERR_PTR(ret);
	}
	return sgt;
}

static const struct drm_gem_object_funcs m9_bo_funcs = {
	.free = m9_bo_free,
	.print_info = drm_gem_dma_object_print_info,
	.get_sg_table = m9_bo_get_sg_table,
	.vmap = drm_gem_dma_object_vmap,
	.mmap = drm_gem_dma_object_mmap,
	.vm_ops = &drm_gem_dma_vm_ops,
};

static void m9_paged_bo_free(struct drm_gem_object *gem)
{
	struct m9x_bo *bo = container_of(to_drm_gem_dma_obj(gem),
					 struct m9x_bo, dma);
	struct m9x_device *mdev = to_m9x(gem->dev);
	unsigned long i;

	if (drm_mm_node_allocated(&bo->system)) {
		mutex_lock(&mdev->transport.lock);
	m9x_transport_drain_locked(mdev);
		m9x_address_unmap_system(mdev, &bo->system);
		mutex_unlock(&mdev->transport.lock);
	}
	if (bo->sgt.sgl) {
		if (bo->sgt.nents)
			dma_unmap_sgtable(drm_dev_dma_dev(gem->dev), &bo->sgt,
					  DMA_BIDIRECTIONAL, 0);
		sg_free_table(&bo->sgt);
	}
	for (i = 0; bo->pages && i < gem->size >> PAGE_SHIFT; i++)
		if (bo->pages[i])
			__free_page(bo->pages[i]);
	kvfree(bo->pages);
	drm_gem_object_release(gem);
	kfree(bo);
}

static int m9_paged_bo_mmap(struct drm_gem_object *gem,
			    struct vm_area_struct *vma)
{
	struct m9x_bo *bo = container_of(to_drm_gem_dma_obj(gem),
					 struct m9x_bo, dma);

	vma->vm_pgoff -= drm_vma_node_start(&gem->vma_node);
	vm_flags_mod(vma, VM_DONTDUMP | VM_DONTEXPAND, VM_PFNMAP);
	return vm_map_pages(vma, bo->pages, gem->size >> PAGE_SHIFT);
}

static int m9_paged_bo_vmap(struct drm_gem_object *gem, struct iosys_map *map)
{
	struct m9x_bo *bo = container_of(to_drm_gem_dma_obj(gem),
					 struct m9x_bo, dma);
	void *vaddr = vmap(bo->pages, gem->size >> PAGE_SHIFT, VM_MAP,
			   PAGE_KERNEL);

	if (!vaddr)
		return -ENOMEM;
	iosys_map_set_vaddr(map, vaddr);
	return 0;
}

static void m9_paged_bo_vunmap(struct drm_gem_object *gem,
			       struct iosys_map *map)
{
	vunmap(map->vaddr);
}

/* The importer maps the pages for its own device; bo->sgt stays this
 * driver's own mapping and is not handed out. */
static struct sg_table *m9_paged_bo_get_sg_table(struct drm_gem_object *gem)
{
	struct m9x_bo *bo = container_of(to_drm_gem_dma_obj(gem),
					 struct m9x_bo, dma);

	return drm_prime_pages_to_sg(gem->dev, bo->pages,
				     gem->size >> PAGE_SHIFT);
}

static const struct drm_gem_object_funcs m9_paged_bo_funcs = {
	.free = m9_paged_bo_free,
	.get_sg_table = m9_paged_bo_get_sg_table,
	.vmap = m9_paged_bo_vmap,
	.vunmap = m9_paged_bo_vunmap,
	.mmap = m9_paged_bo_mmap,
};

bool m9x_bo_is_paged(const struct m9x_bo *bo)
{
	return bo->dma.base.funcs == &m9_paged_bo_funcs;
}

/* CPU stores through user mappings reach the device before a submit. */
void m9x_bo_sync_for_device(struct m9x_bo *bo)
{
	if (m9x_bo_is_paged(bo))
		dma_sync_sgtable_for_device(drm_dev_dma_dev(bo->dma.base.dev),
					    &bo->sgt, DMA_BIDIRECTIONAL);
}

static void m9_vram_bo_free(struct drm_gem_object *gem)
{
	struct m9x_vram_bo *bo = container_of(drm_gem_vram_of_gem(gem),
						struct m9x_vram_bo, vram);

	drm_gem_vram_put(&bo->vram);
}

static int m9_vram_bo_vmap(struct drm_gem_object *gem, struct iosys_map *map)
{
	return drm_gem_vram_vmap(drm_gem_vram_of_gem(gem), map);
}

static void m9_vram_bo_vunmap(struct drm_gem_object *gem,
			      struct iosys_map *map)
{
	drm_gem_vram_vunmap(drm_gem_vram_of_gem(gem), map);
}

/*
 * No get_sg_table: this memory is the framebuffer BAR, not pages, so there is
 * no scatterlist a foreign device could map.  Sharing it with another device
 * would need peer-to-peer DMA.  Export to a dma_buf and import back into this
 * same device -- which is what a compositor on this GPU does -- go through
 * DRM core and need no hook (MEASURED).
 */
static const struct drm_gem_object_funcs m9_vram_bo_funcs = {
	.free = m9_vram_bo_free,
	.vmap = m9_vram_bo_vmap,
	.vunmap = m9_vram_bo_vunmap,
	.mmap = drm_gem_ttm_mmap,
	.print_info = drm_gem_ttm_print_info,
};

bool m9x_gem_is_dma(struct drm_gem_object *gem)
{
	return gem && (gem->funcs == &m9_bo_funcs ||
		       gem->funcs == &m9_paged_bo_funcs);
}

bool m9x_gem_is_vram(struct drm_gem_object *gem)
{
	return gem && gem->funcs == &m9_vram_bo_funcs;
}

bool m9x_gem_vram_tiled(struct drm_gem_object *gem)
{
	return m9x_gem_is_vram(gem) &&
	       container_of(drm_gem_vram_of_gem(gem), struct m9x_vram_bo,
			    vram)->tiled;
}

struct drm_gem_object *m9x_gem_create_object(struct drm_device *drm,
					      size_t size)
{
	struct m9x_vram_bo *bo;

	bo = kzalloc_obj(*bo);
	if (!bo)
		return ERR_PTR(-ENOMEM);
	bo->vram.bo.base.funcs = &m9_vram_bo_funcs;
	return &bo->vram.bo.base;
}

int m9x_vram_pin(struct drm_gem_vram_object *bo)
{
	struct ttm_operation_ctx ctx = { false, false };
	int ret;

	ret = ttm_bo_reserve(&bo->bo, true, false, NULL);
	if (ret)
		return ret;
	if (!bo->bo.pin_count) {
		bo->placement.placement = bo->placements;
		bo->placement.num_placement = 1;
		bo->placements[0].mem_type = TTM_PL_VRAM;
		bo->placements[0].flags = 0;
		bo->placements[0].fpfn = 0;
		bo->placements[0].lpfn = 0;
		ret = ttm_bo_validate(&bo->bo, &bo->placement, &ctx);
		if (ret)
			goto out;
	}
	ttm_bo_pin(&bo->bo);
out:
	ttm_bo_unreserve(&bo->bo);
	return ret;
}

void m9x_vram_unpin(struct drm_gem_vram_object *bo)
{
	if (!ttm_bo_reserve(&bo->bo, false, false, NULL)) {
		ttm_bo_unpin(&bo->bo);
		ttm_bo_unreserve(&bo->bo);
	}
}

/*
 * Video memory above the slot 0 identity window.  The engine reaches it
 * through ordinary local addresses; no host aperture maps it permanently, so
 * an object placed here is never mmapped and never vmapped.  Every VRAM
 * object this driver creates stays pinned for as long as it lives, so a range
 * allocator is the whole manager this memory needs.
 */
struct m9x_engine_bo {
	struct drm_gem_object base;
	struct drm_mm_node node;
};

static void m9_engine_bo_free(struct drm_gem_object *gem)
{
	struct m9x_engine_bo *bo = container_of(gem, struct m9x_engine_bo, base);
	struct m9x_device *mdev = to_m9x(gem->dev);

	mutex_lock(&mdev->engine_vram_lock);
	if (drm_mm_node_allocated(&bo->node))
		drm_mm_remove_node(&bo->node);
	mutex_unlock(&mdev->engine_vram_lock);
	drm_gem_object_release(gem);
	kfree(bo);
}

/*
 * No mmap, no vmap and no export: the CPU has no window onto this memory.
 * DRM core's default export hands out a dma_buf for any object, so refuse
 * here -- MEASURED, PRIME_HANDLE_TO_FD returned a working fd for an
 * engine-only object, which no importer could ever map.
 */
static struct dma_buf *m9_engine_bo_export(struct drm_gem_object *gem,
					   int flags)
{
	return ERR_PTR(-EINVAL);
}

static const struct drm_gem_object_funcs m9_engine_bo_funcs = {
	.free = m9_engine_bo_free,
	.export = m9_engine_bo_export,
};

bool m9x_gem_is_engine_vram(struct drm_gem_object *gem)
{
	return gem && gem->funcs == &m9_engine_bo_funcs;
}

bool m9x_gem_in_vram(struct drm_gem_object *gem)
{
	return m9x_gem_is_vram(gem) || m9x_gem_is_engine_vram(gem);
}

/* An engine-only object is placed once and never moves, so pinning it is
 * already done and releasing the pin costs nothing. */
int m9x_gem_vram_pin(struct drm_gem_object *gem)
{
	if (m9x_gem_is_engine_vram(gem))
		return 0;
	return m9x_vram_pin(drm_gem_vram_of_gem(gem));
}

void m9x_gem_vram_unpin(struct drm_gem_object *gem)
{
	if (!m9x_gem_is_engine_vram(gem))
		m9x_vram_unpin(drm_gem_vram_of_gem(gem));
}

/* A kernel-owned, pinned VRAM object zeroed once through the BAR. */
struct drm_gem_object *m9x_gem_vram_zeroed(struct drm_device *drm,
					   size_t size)
{
	struct drm_gem_vram_object *vram;
	struct iosys_map map;
	int ret;

	vram = drm_gem_vram_create(drm, size, 0);
	if (IS_ERR(vram))
		return ERR_CAST(vram);
	ret = m9x_vram_pin(vram);
	if (ret)
		goto err_put;
	ret = ttm_bo_reserve(&vram->bo, true, false, NULL);
	if (ret)
		goto err_unpin;
	ret = drm_gem_vram_vmap(vram, &map);
	if (!ret) {
		iosys_map_memset(&map, 0, 0, size);
		/* The zeroes reach VRAM before the engine reads them. */
		wmb();
		drm_gem_vram_vunmap(vram, &map);
	}
	ttm_bo_unreserve(&vram->bo);
	if (ret)
		goto err_unpin;
	return &vram->bo.base;

err_unpin:
	m9x_vram_unpin(vram);
err_put:
	drm_gem_object_put(&vram->bo.base);
	return ERR_PTR(ret);
}

s64 m9x_gem_vram_offset(struct drm_gem_object *gem)
{
	if (m9x_gem_is_engine_vram(gem))
		return container_of(gem, struct m9x_engine_bo, base)->node.start;
	return drm_gem_vram_offset(drm_gem_vram_of_gem(gem));
}

/* Walk a VRAM range through the slot 1 host aperture, a window at a time. */
static int m9_engine_vram_walk(struct m9x_device *mdev, u64 start, u64 bytes,
			       void (*step)(void __iomem *win, u64 vram,
					    size_t len))
{
	u64 done;

	if (!mdev->bar0_window)
		return -ENODEV;
	mutex_lock(&mdev->transport.lock);
	for (done = 0; done < bytes; done += M9_ADDRESS_SURFACE_BYTES) {
		size_t len = min_t(u64, bytes - done, M9_ADDRESS_SURFACE_BYTES);

		m9x_address_raw_window(mdev, lower_32_bits(start + done));
		step(mdev->bar0_window, start + done, len);
		/* Complete the write-combined stores before the window moves. */
		wmb();
		m9x_address_surface_release(mdev);
	}
	mutex_unlock(&mdev->transport.lock);
	return 0;
}

static void m9_engine_vram_zero(void __iomem *win, u64 vram, size_t len)
{
	memset_io(win, 0, len);
}

/* Every dword carries the address it lives at, so a readback that lands on
 * the wrong page reports which page it actually reached. */
static void m9_engine_vram_stamp(void __iomem *win, u64 vram, size_t len)
{
	size_t i;

	for (i = 0; i < len; i += sizeof(u32))
		iowrite32(lower_32_bits(vram + i), (u8 __iomem *)win + i);
}

static u32 m9_engine_vram_probe_errors;

static void m9_engine_vram_check(void __iomem *win, u64 vram, size_t len)
{
	size_t i;

	for (i = 0; i < len; i += sizeof(u32)) {
		u32 got = ioread32((u8 __iomem *)win + i);

		if (got != lower_32_bits(vram + i)) {
			if (!m9_engine_vram_probe_errors)
				pr_err("m9x: VRAM %#llx reads %#x, not %#x\n",
				       vram + i, got, lower_32_bits(vram + i));
			m9_engine_vram_probe_errors++;
		}
	}
}

/*
 * Before the allocator hands out memory no aperture maps by default, establish
 * that it is there: stamp the first and last page of the range and one page
 * every 64 MiB with their own addresses, then read every stamp back.  Aliased
 * or absent memory reports the address it really reached.
 */
static int m9_engine_vram_probe(struct m9x_device *mdev, u64 start, u64 bytes)
{
	u64 last = start + bytes - PAGE_SIZE, at;
	int ret;

	m9_engine_vram_probe_errors = 0;
	for (at = start; at <= last; at = min(at + SZ_64M, last)) {
		ret = m9_engine_vram_walk(mdev, at, PAGE_SIZE,
					  m9_engine_vram_stamp);
		if (ret)
			return ret;
		if (at == last)
			break;
	}
	for (at = start; at <= last; at = min(at + SZ_64M, last)) {
		ret = m9_engine_vram_walk(mdev, at, PAGE_SIZE,
					  m9_engine_vram_check);
		if (ret)
			return ret;
		if (at == last)
			break;
	}
	if (m9_engine_vram_probe_errors) {
		drm_err(&mdev->drm,
			"%u of the stamped dwords above %#llx read back wrong\n",
			m9_engine_vram_probe_errors, start);
		return -EIO;
	}
	return 0;
}

static void m9_engine_vram_release(struct drm_device *drm, void *arg)
{
	struct m9x_device *mdev = to_m9x(drm);

	if (mdev->engine_vram_ready)
		drm_mm_takedown(&mdev->engine_vram);
	mdev->engine_vram_ready = false;
}

int m9x_engine_vram_init(struct m9x_device *mdev)
{
	u64 limit = m9x_vram_gpu_limit(mdev);
	int ret;

	ret = drmm_mutex_init(&mdev->drm, &mdev->engine_vram_lock);
	if (ret)
		return ret;
	if (limit <= mdev->vram_visible)
		return 0;
	ret = m9_engine_vram_probe(mdev, mdev->vram_visible,
				   limit - mdev->vram_visible);
	if (ret) {
		drm_warn(&mdev->drm,
			 "video memory above %#llx is unusable; engine-only allocation disabled\n",
			 mdev->vram_visible);
		return 0;
	}
	drm_mm_init(&mdev->engine_vram, mdev->vram_visible,
		    limit - mdev->vram_visible);
	mdev->engine_vram_ready = true;
	drm_info(&mdev->drm, "engine-only video memory [%#llx,%#llx)\n",
		 mdev->vram_visible, limit);
	return drmm_add_action_or_reset(&mdev->drm, m9_engine_vram_release,
					NULL);
}

static struct m9x_engine_bo *m9_engine_bo_create(struct m9x_device *mdev,
						 size_t size)
{
	struct m9x_engine_bo *bo;
	int ret;

	if (!mdev->engine_vram_ready)
		return ERR_PTR(-ENOMEM);
	bo = kzalloc_obj(*bo);
	if (!bo)
		return ERR_PTR(-ENOMEM);
	drm_gem_private_object_init(&mdev->drm, &bo->base, size);
	bo->base.funcs = &m9_engine_bo_funcs;
	/* Place from the top of video memory down, so the memory next to the
	 * CPU-visible window stays free for whatever the window has to grow
	 * into, and the addresses in use are the highest the engine sees. */
	mutex_lock(&mdev->engine_vram_lock);
	ret = drm_mm_insert_node_generic(&mdev->engine_vram, &bo->node, size,
					 PAGE_SIZE, 0, DRM_MM_INSERT_HIGH);
	mutex_unlock(&mdev->engine_vram_lock);
	if (ret)
		goto err_release;
	/* Hand no previous tenant's pixels to the next client. */
	ret = m9_engine_vram_walk(mdev, bo->node.start, size,
				  m9_engine_vram_zero);
	if (ret)
		goto err_release;
	/* Nothing can read this object back, so say where it went. */
	drm_info(&mdev->drm, "engine-only object at VRAM [%#llx,%#llx)\n",
		 bo->node.start, bo->node.start + size);
	return bo;

err_release:
	m9_engine_bo_free(&bo->base);
	return ERR_PTR(ret);
}

/* Individually allocated DMA32 pages, mapped as one scatter-gather table. */
static struct m9x_bo *m9_paged_bo_create(struct drm_device *drm, size_t size)
{
	unsigned long npages = size >> PAGE_SHIFT, i;
	struct scatterlist *sg;
	struct m9x_bo *bo;
	int ret;

	bo = kzalloc_obj(*bo);
	if (!bo)
		return ERR_PTR(-ENOMEM);
	ret = drm_gem_object_init(drm, &bo->dma.base, size);
	if (ret) {
		kfree(bo);
		return ERR_PTR(ret);
	}
	bo->dma.base.funcs = &m9_paged_bo_funcs;
	ret = drm_gem_create_mmap_offset(&bo->dma.base);
	if (ret)
		goto err_put;
	bo->pages = kvcalloc(npages, sizeof(*bo->pages), GFP_KERNEL);
	if (!bo->pages) {
		ret = -ENOMEM;
		goto err_put;
	}
	for (i = 0; i < npages; i++) {
		bo->pages[i] = alloc_page(GFP_KERNEL | GFP_DMA32 | __GFP_ZERO |
					  __GFP_NOWARN);
		if (!bo->pages[i]) {
			ret = -ENOMEM;
			goto err_put;
		}
	}
	ret = sg_alloc_table_from_pages(&bo->sgt, bo->pages, npages, 0, size,
					GFP_KERNEL);
	if (ret)
		goto err_put;
	ret = dma_map_sgtable(drm_dev_dma_dev(drm), &bo->sgt, DMA_BIDIRECTIONAL, 0);
	if (ret) {
		bo->sgt.nents = 0;
		goto err_put;
	}
	for_each_sgtable_dma_sg(&bo->sgt, sg, i)
		if (upper_32_bits(sg_dma_address(sg)) ||
		    sg_dma_address(sg) > U32_MAX - sg_dma_len(sg)) {
			ret = -ERANGE;
			goto err_put;
		}
	return bo;

err_put:
	drm_gem_object_put(&bo->dma.base);
	return ERR_PTR(ret);
}

static struct m9x_bo *m9_bo_create(struct drm_device *drm, size_t size)
{
	struct m9x_bo *bo;
	int ret;

	bo = kzalloc_obj(*bo);
	if (!bo)
		return ERR_PTR(-ENOMEM);
	ret = drm_gem_object_init(drm, &bo->dma.base, size);
	if (ret)
		goto err_free;
	bo->dma.base.funcs = &m9_bo_funcs;
	ret = drm_gem_create_mmap_offset(&bo->dma.base);
	if (ret)
		goto err_put;
	/* A draw range is fetched by raw bus address, so this allocation has to
	 * be physically contiguous.  Above the costly order the page allocator
	 * gives up before compacting unless it is told to retry. */
	bo->dma.vaddr = dma_alloc_wc(drm_dev_dma_dev(drm), size,
					&bo->dma.dma_addr,
					GFP_KERNEL | __GFP_RETRY_MAYFAIL |
					__GFP_NOWARN | __GFP_ZERO);
	if (bo->dma.vaddr &&
	    (upper_32_bits(bo->dma.dma_addr) ||
	     bo->dma.dma_addr > U32_MAX - size)) {
		ret = -ERANGE;
		goto err_put;
	}
	if (!bo->dma.vaddr && !m9_dma_reserve_alloc(to_m9x(drm), bo, size)) {
		ret = -ENOMEM;
		goto err_put;
	}
	return bo;

err_put:
	drm_gem_object_put(&bo->dma.base);
	return ERR_PTR(ret);
err_free:
	kfree(bo);
	return ERR_PTR(ret);
}

static struct m9x_vram_bo *
m9_vram_bo_create(struct drm_device *drm, size_t size)
{
	struct drm_gem_vram_object *vram;
	struct m9x_vram_bo *bo;
	struct iosys_map map;
	int ret;

	vram = drm_gem_vram_create(drm, size, 1);
	if (IS_ERR(vram))
		return ERR_CAST(vram);
	bo = container_of(vram, struct m9x_vram_bo, vram);
	/* A fresh BO lives in the system placement, whose pages TTM hands out
	 * zeroed; vmapping the object only to memset it again costs a second
	 * pass over every byte.  Fill only a device placement. */
	if (vram->bo.resource->mem_type != TTM_PL_SYSTEM) {
		ret = ttm_bo_reserve(&vram->bo, true, false, NULL);
		if (ret)
			goto err_put;
		ret = drm_gem_vram_vmap(vram, &map);
		if (!ret) {
			iosys_map_memset(&map, 0, 0, size);
			/* Publish zeroes before exposing the mapping cookie. */
			wmb();
			drm_gem_vram_vunmap(vram, &map);
		}
		ttm_bo_unreserve(&vram->bo);
		if (ret)
			goto err_put;
	}
	ret = drm_gem_create_mmap_offset(&vram->bo.base);
	if (ret)
		goto err_put;
	return bo;

err_put:
	drm_gem_object_put(&vram->bo.base);
	return ERR_PTR(ret);
}

struct m9x_bo *m9x_bo_lookup(struct m9x_device *mdev,
					      struct drm_file *file,
					      u32 handle)
{
	struct drm_gem_object *gem;
	struct m9x_bo *bo;

	(void)mdev;

	gem = drm_gem_object_lookup(file, handle);
	if (!gem)
		return ERR_PTR(-ENOENT);

	if (!m9x_gem_is_dma(gem)) {
		drm_gem_object_put(gem);
		return ERR_PTR(-EINVAL);
	}
	/* The handle lookup reference is returned to the caller. */
	bo = container_of(to_drm_gem_dma_obj(gem), struct m9x_bo, dma);
	return bo;
}

static int m9_create_bo_ioctl(struct drm_device *drm, void *data,
			      struct drm_file *file)
{
	struct drm_parhelia_m9_create_bo *args = data;
	struct m9x_device *mdev = to_m9x(drm);
	struct drm_gem_object *gem;
	struct m9x_bo *bo = NULL;
	size_t size;
	u32 handle;
	int ret;

	if (!m9x_enabled())
		return -ENODEV;
	if (!args->size ||
	    args->flags & ~(PARHELIA_M9_BO_COMMAND | PARHELIA_M9_BO_VRAM |
			    PARHELIA_M9_BO_TILED |
			    PARHELIA_M9_BO_ENGINE_ONLY) ||
	    ((args->flags & PARHELIA_M9_BO_COMMAND) &&
	     (args->flags & (PARHELIA_M9_BO_VRAM | PARHELIA_M9_BO_TILED))) ||
	    ((args->flags & PARHELIA_M9_BO_ENGINE_ONLY) &&
	     (args->flags & (PARHELIA_M9_BO_VRAM | PARHELIA_M9_BO_TILED)) !=
	      PARHELIA_M9_BO_VRAM) ||
	    args->handle ||
	    args->mmap_offset)
		return -EINVAL;
	if (args->size > m9_bo_max_size(mdev, args->flags))
		return -EINVAL;
	size = PAGE_ALIGN(args->size);
	mutex_lock(&mdev->transport.lock);
	m9x_transport_drain_locked(mdev);
	ret = mdev->transport.wedged ? -EIO : 0;
	mutex_unlock(&mdev->transport.lock);
	if (ret)
		return ret;
	if (args->flags & PARHELIA_M9_BO_ENGINE_ONLY) {
		struct m9x_engine_bo *engine_bo;

		engine_bo = m9_engine_bo_create(mdev, size);
		if (IS_ERR(engine_bo))
			return PTR_ERR(engine_bo);
		gem = &engine_bo->base;
		m9_bo_engine_creates++;
		m9_bo_engine_kib += size >> 10;
	} else if (args->flags & (PARHELIA_M9_BO_VRAM | PARHELIA_M9_BO_TILED)) {
		struct m9x_vram_bo *vram_bo;

		if (!m9_vram_free_for(mdev, size))
			return -ENOMEM;
		vram_bo = m9_vram_bo_create(drm, size);
		if (IS_ERR(vram_bo))
			return PTR_ERR(vram_bo);
		vram_bo->tiled = args->flags & PARHELIA_M9_BO_TILED;
		gem = &vram_bo->vram.bo.base;
		m9_bo_vram_creates++;
		m9_bo_vram_kib += size >> 10;
	} else {
		bool command = args->flags & PARHELIA_M9_BO_COMMAND;

		/* Only class-1 addressed BOs may fall back to scattered pages. */
		bo = command || !m9_bo_force_paged ? m9_bo_create(drm, size) :
						       ERR_PTR(-ENOMEM);
		if (IS_ERR(bo) && PTR_ERR(bo) == -ENOMEM && !command) {
			pr_warn("m9x: no contiguous DMA32 for %zu bytes; scattered pages\n",
				size);
			m9_bo_paged_creates++;
			bo = m9_paged_bo_create(drm, size);
		}
		if (IS_ERR(bo))
			return PTR_ERR(bo);
		bo->flags = args->flags;
		gem = &bo->dma.base;
		m9_bo_dma_creates++;
		m9_bo_dma_kib += size >> 10;
		if (!command) {
			mutex_lock(&mdev->transport.lock);
	m9x_transport_drain_locked(mdev);
			ret = m9x_bo_is_paged(bo) ?
				m9x_address_map_system_sgt(mdev, &bo->system,
							   &bo->sgt, size) :
				m9x_address_map_system(mdev, &bo->system,
						       bo->dma.dma_addr, size);
			mutex_unlock(&mdev->transport.lock);
			if (ret)
				goto out_put;
		}
	}
	ret = drm_gem_handle_create(file, gem, &handle);
	if (ret)
		goto out_put;

	args->size = size;
	args->handle = handle;
	args->mmap_offset = drm_vma_node_offset_addr(&gem->vma_node);
	drm_gem_object_put(gem);
	return 0;

out_put:
	drm_gem_object_put(gem);
	return ret;
}

int m9x_create_bo_ioctl(struct drm_device *drm, void *data,
			 struct drm_file *file)
{
	int idx, ret;

	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;
	ret = m9_create_bo_ioctl(drm, data, file);
	drm_dev_exit(idx);
	return ret;
}

static int m9_get_bo_info_ioctl(struct drm_device *drm, void *data,
				struct drm_file *file)
{
	struct drm_parhelia_m9_get_bo_info *args = data;
	struct drm_gem_object *gem;
	int ret = 0;

	if (!m9x_enabled())
		return -ENODEV;
	if (!args->handle || args->memory_class || args->size ||
	    args->mmap_offset || args->layout || args->pad)
		return -EINVAL;
	gem = drm_gem_object_lookup(file, args->handle);
	if (!gem)
		return -ENOENT;
	if (m9x_gem_in_vram(gem))
		args->memory_class = PARHELIA_M9_MEMORY_CLASS_VRAM;
	else if (m9x_gem_is_dma(gem))
		args->memory_class = PARHELIA_M9_MEMORY_CLASS_DMA32;
	else
		ret = -EINVAL;
	if (!ret)
		args->size = gem->size;
	if (!ret)
		args->mmap_offset = drm_vma_node_offset_addr(&gem->vma_node);
	if (!ret)
		args->layout = m9x_gem_vram_tiled(gem) ?
			PARHELIA_M9_LAYOUT_TILED : PARHELIA_M9_LAYOUT_LINEAR;
	drm_gem_object_put(gem);
	return ret;
}

int m9x_get_bo_info_ioctl(struct drm_device *drm, void *data,
			  struct drm_file *file)
{
	int idx, ret;

	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;
	ret = m9_get_bo_info_ioctl(drm, data, file);
	drm_dev_exit(idx);
	return ret;
}
