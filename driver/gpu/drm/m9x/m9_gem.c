// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/dma-mapping.h>
#include <linux/sizes.h>

#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_ttm_helper.h>
#include <drm/drm_gem_vram_helper.h>

#include "m9_drv.h"

#define M9_BO_MAX_SIZE	SZ_32M

static void m9_bo_free(struct drm_gem_object *gem)
{
	struct m9x_bo *bo = container_of(to_drm_gem_dma_obj(gem),
						   struct m9x_bo, dma);

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

static void m9_vram_bo_free(struct drm_gem_object *gem)
{
	struct m9x_vram_bo *bo = container_of(drm_gem_vram_of_gem(gem),
						struct m9x_vram_bo, vram);

	if (bo->user_pinned)
		m9x_vram_unpin(&bo->vram);
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
	return gem && gem->funcs == &m9_bo_funcs;
}

bool m9x_gem_is_vram(struct drm_gem_object *gem)
{
	return gem && gem->funcs == &m9_vram_bo_funcs;
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
	INIT_LIST_HEAD(&bo->link);
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
	ret = m9x_vram_pin(vram);
	if (ret)
		goto err_put;
	bo->user_pinned = true;
	ret = ttm_bo_reserve(&vram->bo, true, false, NULL);
	if (ret)
		goto err_put;
	ret = drm_gem_vram_vmap(vram, &map);
	if (!ret) {
		iosys_map_memset(&map, 0, 0, size);
		/* Publish zeroes before exposing the mapping cookie to userspace. */
		wmb();
		drm_gem_vram_vunmap(vram, &map);
	}
	ttm_bo_unreserve(&vram->bo);
	if (ret)
		goto err_put;
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
	struct m9x_bo *bo, *found = NULL;

	gem = drm_gem_object_lookup(file, handle);
	if (!gem)
		return ERR_PTR(-ENOENT);

	mutex_lock(&mdev->transport.lock);
	list_for_each_entry(bo, &mdev->transport.bos, link) {
		if (&bo->dma.base == gem && bo->file == file) {
			found = bo;
			break;
		}
	}
	mutex_unlock(&mdev->transport.lock);
	if (!found) {
		drm_gem_object_put(gem);
		return ERR_PTR(-EINVAL);
	}
	/* The handle lookup reference is returned to the caller. */
	return found;
}

int m9x_create_bo_ioctl(struct drm_device *drm, void *data,
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
	    args->flags & ~(PARHELIA_M9_BO_COMMAND | PARHELIA_M9_BO_VRAM) ||
	    (args->flags & (PARHELIA_M9_BO_COMMAND | PARHELIA_M9_BO_VRAM)) ==
		(PARHELIA_M9_BO_COMMAND | PARHELIA_M9_BO_VRAM) || args->handle ||
	    args->mmap_offset)
		return -EINVAL;
	size = PAGE_ALIGN(args->size);
	if (args->flags & PARHELIA_M9_BO_VRAM) {
		struct m9x_vram_bo *vram_bo;

		vram_bo = m9_vram_bo_create(drm, size);
		if (IS_ERR(vram_bo))
			return PTR_ERR(vram_bo);
		gem = &vram_bo->vram.bo.base;
	} else {
		bo = m9_bo_create(drm, size);
		if (IS_ERR(bo))
			return PTR_ERR(bo);
		bo->flags = args->flags;
		gem = &bo->dma.base;
	}
	ret = drm_gem_handle_create(file, gem, &handle);
	if (ret)
		goto out_put;

	mutex_lock(&mdev->transport.lock);
	if (mdev->transport.wedged) {
		ret = -EIO;
	} else if (bo) {
		drm_gem_object_get(gem);
		bo->file = file;
		list_add_tail(&bo->link, &mdev->transport.bos);
	}
	mutex_unlock(&mdev->transport.lock);
	if (ret)
		goto out_handle;

	args->size = size;
	args->handle = handle;
	args->mmap_offset = drm_vma_node_offset_addr(&gem->vma_node);
	drm_gem_object_put(gem);
	return 0;

out_handle:
	drm_gem_handle_delete(file, handle);
out_put:
	drm_gem_object_put(gem);
	return ret;
}

int m9x_get_bo_info_ioctl(struct drm_device *drm, void *data,
			  struct drm_file *file)
{
	struct drm_parhelia_m9_get_bo_info *args = data;
	struct drm_gem_object *gem;
	int ret = 0;

	if (!m9x_enabled())
		return -ENODEV;
	if (!args->handle || args->memory_class || args->size ||
	    args->mmap_offset)
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
	drm_gem_object_put(gem);
	return ret;
}

void m9x_postclose(struct m9x_device *mdev, struct drm_file *file)
{
	struct m9x_bo *bo, *tmp;
	LIST_HEAD(dead);

	mutex_lock(&mdev->transport.lock);
	list_for_each_entry_safe(bo, tmp, &mdev->transport.bos, link) {
		if (bo->file == file)
			list_move_tail(&bo->link, &dead);
	}
	mutex_unlock(&mdev->transport.lock);

	list_for_each_entry_safe(bo, tmp, &dead, link) {
		list_del_init(&bo->link);
		drm_gem_object_put(&bo->dma.base);
	}
}
