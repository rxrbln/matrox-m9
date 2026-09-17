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
#include <drm/drm_gem_ttm_helper.h>
#include <drm/drm_gem_vram_helper.h>

#include "m9_drv.h"

#define M9_BO_MAX_SIZE	SZ_32M

static bool m9_bo_force_paged;
module_param(m9_bo_force_paged, bool, 0600);
MODULE_PARM_DESC(m9_bo_force_paged,
	"Back non-command DMA32 BOs by page arrays (fragmentation fallback test)");

/* Allocation counters, read back from sysfs to size the per-frame BO churn. */
static unsigned long m9_bo_vram_creates, m9_bo_vram_kib;
static unsigned long m9_bo_dma_creates, m9_bo_dma_kib;
module_param(m9_bo_vram_creates, ulong, 0444);
module_param(m9_bo_vram_kib, ulong, 0444);
module_param(m9_bo_dma_creates, ulong, 0444);
module_param(m9_bo_dma_kib, ulong, 0444);

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
	if (bo->dma.vaddr)
		dma_free_wc(drm_dev_dma_dev(gem->dev), gem->size,
			    bo->dma.vaddr, bo->dma.dma_addr);
	drm_gem_object_release(gem);
	kfree(bo);
}

static const struct drm_gem_object_funcs m9_bo_funcs = {
	.free = m9_bo_free,
	.print_info = drm_gem_dma_object_print_info,
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

static const struct drm_gem_object_funcs m9_paged_bo_funcs = {
	.free = m9_paged_bo_free,
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
	bo->dma.vaddr = dma_alloc_wc(drm_dev_dma_dev(drm), size,
					&bo->dma.dma_addr,
					GFP_KERNEL | __GFP_NOWARN | __GFP_ZERO);
	if (!bo->dma.vaddr) {
		ret = -ENOMEM;
		goto err_put;
	}
	if (upper_32_bits(bo->dma.dma_addr) || bo->dma.dma_addr > U32_MAX - size) {
		ret = -ERANGE;
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
	if (!args->size || args->size > M9_BO_MAX_SIZE ||
	    args->flags & ~(PARHELIA_M9_BO_COMMAND | PARHELIA_M9_BO_VRAM |
			    PARHELIA_M9_BO_TILED) ||
	    ((args->flags & PARHELIA_M9_BO_COMMAND) &&
	     (args->flags & (PARHELIA_M9_BO_VRAM | PARHELIA_M9_BO_TILED))) ||
	    args->handle ||
	    args->mmap_offset)
		return -EINVAL;
	size = PAGE_ALIGN(args->size);
	mutex_lock(&mdev->transport.lock);
	m9x_transport_drain_locked(mdev);
	ret = mdev->transport.wedged ? -EIO : 0;
	mutex_unlock(&mdev->transport.lock);
	if (ret)
		return ret;
	if (args->flags & (PARHELIA_M9_BO_VRAM | PARHELIA_M9_BO_TILED)) {
		struct m9x_vram_bo *vram_bo;

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
		if (IS_ERR(bo) && PTR_ERR(bo) == -ENOMEM && !command)
			bo = m9_paged_bo_create(drm, size);
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
	if (m9x_gem_is_vram(gem))
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
