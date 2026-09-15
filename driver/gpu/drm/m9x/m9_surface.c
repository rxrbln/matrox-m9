// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Linear CPU access to tiled M9 render targets through the host-aperture
 * surface window (slot 1); the slot 0 identity view stays valid meanwhile.
 */
#include <linux/dma-resv.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_vram_helper.h>

#include "m9_drv.h"

#define M9_SURFACE_MAX_SPAN	2048u
#define M9_SURFACE_FAMILY	0xcc210000u
#define M9_SURFACE_WAIT		msecs_to_jiffies(10000)

/* Vendor render allocation contract, shared with Mesa m9_internal.h. */
static bool m9_surface_layout_bytes(u32 width, u32 height, u32 cpp, u64 *size)
{
	bool small = width <= 64 && height <= 64;
	u32 height_quantum = small ? (cpp == 4 ? 8 : 4) : 32;
	u32 byte_quantum = cpp == 4 ? (small ? 0x400 : 0x1000) :
				      (small ? 0x200 : 0x800);
	u32 span, alloc_height;
	u64 elements, tail;

	if (!width || !height || width > M9_SURFACE_MAX_SPAN ||
	    height > M9_SURFACE_MAX_SPAN || (cpp != 2 && cpp != 4))
		return false;
	span = ALIGN(width, 32);
	alloc_height = ALIGN(height, height_quantum);
	if (width <= 16 && height <= 16)
		alloc_height = 16;
	elements = (u64)span * alloc_height;
	tail = (u64)(alloc_height - height) * span + span - width;
	if (tail < 8)
		elements += 8 - tail;
	*size = ALIGN(elements * cpp, (u64)byte_quantum);
	return true;
}

static int m9_surface_check(const struct drm_parhelia_m9_surface_access *args,
			    u32 *span, u32 *surface_height)
{
	u32 d0 = args->descriptor0, d1 = args->descriptor1, end;
	u64 need;

	if (args->flags || args->pad ||
	    (args->op != PARHELIA_M9_ACCESS_READ &&
	     args->op != PARHELIA_M9_ACCESS_WRITE) ||
	    !args->width || !args->height || !args->surface_width)
		return -EINVAL;
	/* Only the verified cc21 family: format 0x66, layout 1, bit 21. */
	*span = d0 & 0xffff;
	*surface_height = d1 & 0xffff;
	if ((d0 & 0xffff0000) != M9_SURFACE_FAMILY ||
	    *span != ALIGN(args->surface_width, 32) ||
	    *span > M9_SURFACE_MAX_SPAN || (d1 & 0xf0000000) ||
	    ((d1 >> 16) & 0xfff) != 1 || !*surface_height ||
	    *surface_height > M9_SURFACE_MAX_SPAN)
		return -EINVAL;
	/* Small layouts use other height quanta and are not measured yet. */
	if (args->surface_width <= 64 && *surface_height <= 64)
		return -EINVAL;
	if (check_add_overflow(args->x, args->width, &end) ||
	    end > args->surface_width ||
	    check_add_overflow(args->y, args->height, &end) ||
	    end > *surface_height ||
	    (u64)*span * 4 * *surface_height > M9_ADDRESS_SURFACE_BYTES)
		return -EINVAL;
	need = (u64)(args->height - 1) * args->data_stride + (u64)args->width * 4;
	if (args->data_stride < (u64)args->width * 4 || need > args->data_size)
		return -EINVAL;
	return 0;
}

/* Copy a de-tiled surface view; caller holds transport.lock. */
void m9x_surface_read_locked(struct m9x_device *mdev, u32 offset, u32 d0,
			     u32 d1, u8 *dst, size_t bytes)
{
	lockdep_assert_held(&mdev->transport.lock);
	m9x_address_surface_window(mdev, offset, d0, d1);
	memcpy_fromio(dst, mdev->bar0_window, bytes);
	m9x_address_surface_release(mdev);
}

static int m9_surface_access(struct m9x_device *mdev, struct drm_file *file,
			     struct drm_parhelia_m9_surface_access *args)
{
	bool write = args->op == PARHELIA_M9_ACCESS_WRITE;
	struct drm_gem_vram_object *vram;
	struct drm_gem_object *gem;
	u32 span, surface_height, row;
	size_t row_bytes;
	u64 min_size;
	s64 offset;
	long wait;
	u8 *kbuf;
	int ret;

	ret = m9_surface_check(args, &span, &surface_height);
	if (ret)
		return ret;
	if (!mdev->bar0_window)
		return -ENODEV;
	row_bytes = (size_t)args->width * 4;
	kbuf = kvmalloc_array(args->height, row_bytes, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;
	/* User copies never run under the reservation or transport lock. */
	for (row = 0; write && row < args->height; row++)
		if (copy_from_user(kbuf + row * row_bytes,
				   u64_to_user_ptr(args->data_ptr +
						   (u64)row * args->data_stride),
				   row_bytes)) {
			ret = -EFAULT;
			goto out_free;
		}
	gem = drm_gem_object_lookup(file, args->handle);
	if (!gem) {
		ret = -ENOENT;
		goto out_free;
	}
	if (!m9x_gem_is_vram(gem) || !m9x_gem_vram_tiled(gem) ||
	    !m9_surface_layout_bytes(args->surface_width, surface_height, 4,
				     &min_size) || gem->size < min_size) {
		ret = -EINVAL;
		goto out_put;
	}
	vram = drm_gem_vram_of_gem(gem);
	ret = m9x_vram_pin(vram);
	if (ret)
		goto out_put;
	ret = ttm_bo_reserve(&vram->bo, true, false, NULL);
	if (ret)
		goto out_unpin;
	wait = dma_resv_wait_timeout(gem->resv, write ? DMA_RESV_USAGE_READ :
					DMA_RESV_USAGE_WRITE, true,
				     M9_SURFACE_WAIT);
	if (wait <= 0) {
		ret = wait ? wait : -ETIMEDOUT;
		goto out_unreserve;
	}
	offset = drm_gem_vram_offset(vram);
	if (offset < 0) {
		ret = offset;
		goto out_unreserve;
	}
	mutex_lock(&mdev->transport.lock);
	if (mdev->transport.wedged) {
		ret = -EIO;
	} else {
		m9x_address_surface_window(mdev, lower_32_bits(offset),
					   args->descriptor0, args->descriptor1);
		for (row = 0; row < args->height; row++) {
			void __iomem *view = (u8 __iomem *)mdev->bar0_window +
				((u64)(args->y + row) * span + args->x) * 4;

			if (write)
				memcpy_toio(view, kbuf + row * row_bytes, row_bytes);
			else
				memcpy_fromio(kbuf + row * row_bytes, view, row_bytes);
		}
		/* Complete WC stores before the window is released. */
		if (write)
			wmb();
		m9x_address_surface_release(mdev);
	}
	mutex_unlock(&mdev->transport.lock);
out_unreserve:
	ttm_bo_unreserve(&vram->bo);
out_unpin:
	m9x_vram_unpin(vram);
out_put:
	drm_gem_object_put(gem);
	for (row = 0; !ret && !write && row < args->height; row++)
		if (copy_to_user(u64_to_user_ptr(args->data_ptr +
						 (u64)row * args->data_stride),
				 kbuf + row * row_bytes, row_bytes))
			ret = -EFAULT;
out_free:
	kvfree(kbuf);
	return ret;
}

int m9x_surface_access_ioctl(struct drm_device *drm, void *data,
			     struct drm_file *file)
{
	int idx, ret;

	if (!m9x_enabled())
		return -ENODEV;
	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;
	ret = m9_surface_access(to_m9x(drm), file, data);
	drm_dev_exit(idx);
	return ret;
}

int m9x_surface_selftest(void)
{
	struct drm_parhelia_m9_surface_access args = {
		.op = PARHELIA_M9_ACCESS_READ,
		.descriptor0 = 0xcc210140, .descriptor1 = 0x000100f0,
		.surface_width = 320, .width = 320, .height = 240,
		.data_stride = 1280, .data_size = 1280 * 240,
	};
	u32 span, height;
	u64 size;

	if (m9_surface_check(&args, &span, &height) || span != 320 ||
	    height != 240)
		return -EINVAL;
	args.descriptor0 = 0xcc210400;
	args.descriptor1 = 0x00010300;
	args.surface_width = 1024;
	args.width = 1024;
	args.height = 768;
	args.data_stride = 4096;
	args.data_size = 4096 * 768;
	if (m9_surface_check(&args, &span, &height))
		return -EINVAL;
	args.data_size--;
	if (m9_surface_check(&args, &span, &height) != -EINVAL)
		return -EINVAL;
	args.data_size++;
	args.descriptor0 = 0xc8010020;
	if (m9_surface_check(&args, &span, &height) != -EINVAL)
		return -EINVAL;
	args.descriptor0 = 0xf0210400;
	if (m9_surface_check(&args, &span, &height) != -EINVAL)
		return -EINVAL;
	args.descriptor0 = 0xcc210400;
	args.surface_width = 900;
	if (m9_surface_check(&args, &span, &height) != -EINVAL)
		return -EINVAL;
	if (!m9_surface_layout_bytes(320, 240, 4, &size) || size != 0x50000 ||
	    !m9_surface_layout_bytes(1024, 768, 4, &size) || size != 0x301000)
		return -EINVAL;
	return 0;
}
