// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/dma-mapping.h>
#include <linux/sizes.h>

#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_dma_helper.h>

#include "m9_drv.h"

#define M9_BO_MAX_SIZE	SZ_16M

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
	struct m9x_bo *bo;
	size_t size;
	u32 handle;
	int ret;

	if (!m9x_enabled())
		return -ENODEV;
	if (!args->size || args->size > M9_BO_MAX_SIZE ||
	    args->flags & ~PARHELIA_M9_BO_COMMAND || args->handle ||
	    args->mmap_offset)
		return -EINVAL;
	size = PAGE_ALIGN(args->size);
	bo = m9_bo_create(drm, size);
	if (IS_ERR(bo))
		return PTR_ERR(bo);
	bo->flags = args->flags;
	ret = drm_gem_handle_create(file, &bo->dma.base, &handle);
	if (ret)
		goto out_put;

	mutex_lock(&mdev->transport.lock);
	if (mdev->transport.wedged) {
		ret = -EIO;
	} else {
		drm_gem_object_get(&bo->dma.base);
		bo->file = file;
		list_add_tail(&bo->link, &mdev->transport.bos);
	}
	mutex_unlock(&mdev->transport.lock);
	if (ret)
		goto out_handle;

	args->size = size;
	args->handle = handle;
	args->mmap_offset = drm_vma_node_offset_addr(&bo->dma.base.vma_node);
	drm_gem_object_put(&bo->dma.base);
	return 0;

out_handle:
	drm_gem_handle_delete(file, handle);
out_put:
	drm_gem_object_put(&bo->dma.base);
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
