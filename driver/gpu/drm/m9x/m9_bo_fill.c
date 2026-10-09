// SPDX-License-Identifier: GPL-2.0-only
// Copyright 2026 René Rebe <rene@exactco.de>
/*
 * BO_FILL: zero a byte range of a VRAM object by an engine copy from a
 * kernel-owned zeroed source, queued behind every job posted before it.
 */
#include <linux/dma-resv.h>
#include <linux/ktime.h>
#include <linux/moduleparam.h>
#include <linux/overflow.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include <drm/drm_drv.h>
#include <drm/drm_exec.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>

#include "m9_drv.h"
#include "m9_engine_fill_encode.h"

#define M9_FILL_ROW_BYTES	(M9X_ENGINE_FILL_SPAN * 4u)
/* Tile rows of a large tiled surface (m9x_surface_layout_bytes). */
#define M9_FILL_ROW_QUANTUM	32u

static uint m9_bo_fill_mode = 1;
module_param_named(bo_fill, m9_bo_fill_mode, uint, 0644);
MODULE_PARM_DESC(bo_fill,
		 "Zero VRAM ranges by a queued engine copy (BO_FILL ioctl): 0 refuses with -EOPNOTSUPP, 1 enabled (default)");
static uint m9_bo_fill_source_rows = 512;
module_param_named(bo_fill_source_rows, m9_bo_fill_source_rows, uint, 0644);
MODULE_PARM_DESC(bo_fill_source_rows,
		 "Most 4 KiB rows the zeroed fill source grows to; larger fills take several launches");

int m9x_bo_fill_init(struct m9x_device *mdev)
{
	return drmm_mutex_init(&mdev->drm, &mdev->bo_fill.lock);
}

void m9x_bo_fill_fini(struct m9x_device *mdev)
{
	struct m9x_bo_fill *f = &mdev->bo_fill;

	if (!f->source)
		return;
	m9x_gem_vram_unpin(f->source);
	drm_gem_object_put(f->source);
	f->source = NULL;
	f->source_rows = 0;
}

/*
 * The source is sized to the largest fill seen, up to the parameter; one
 * that is too small is replaced, queued fills keep their own reference.
 */
static int m9_bo_fill_source_locked(struct m9x_device *mdev, u32 rows)
{
	struct m9x_bo_fill *f = &mdev->bo_fill;
	u32 cap = clamp(m9_bo_fill_source_rows, M9_FILL_ROW_QUANTUM,
			M9X_ENGINE_FILL_MAX_ROWS);
	u32 want = ALIGN(min(rows, cap), M9_FILL_ROW_QUANTUM);
	struct drm_gem_object *source;
	u64 bytes;

	lockdep_assert_held(&f->lock);
	if (f->source && f->source_rows >= want)
		return 0;
	if (!m9x_surface_layout_bytes(M9X_ENGINE_FILL_SPAN, want, 4, &bytes))
		return -EINVAL;
	source = m9x_gem_vram_zeroed(&mdev->drm, bytes);
	if (IS_ERR(source))
		return PTR_ERR(source);
	m9x_bo_fill_fini(mdev);
	f->source = source;
	f->source_rows = want;
	drm_info(&mdev->drm, "M9 fill source: %u rows, %llu bytes\n", want,
		 bytes);
	return 0;
}

static bool m9_bo_fill_admitted_locked(struct m9x_device *mdev)
{
	struct m9x_transport *t = &mdev->transport;

	/* The Linux ordinary context on the staging slots, as engine reads. */
	return !t->wedged && !t->faa && !t->faa_retiring &&
	       !t->faa_virtual_primary && !t->faa_source_context &&
	       t->bus_master_enabled && t->transport_armed &&
	       m9x_triangle_context_ready_locked(mdev) &&
	       !m9x_faa_ordinary_blocked_locked(mdev) &&
	       !m9x_faa_graph_preparing(mdev);
}

/* Fences of other contexts; the own timeline is ordered by the stream. */
static int m9_bo_fill_wait_foreign(struct m9x_device *mdev,
				   struct drm_gem_object *gem)
{
	struct dma_resv_iter cursor;
	struct dma_fence *fence;

	dma_resv_for_each_fence(&cursor, gem->resv, dma_resv_usage_rw(true),
				fence) {
		long waited;
		int status;

		if (fence->context == mdev->transport.fence_context)
			continue;
		waited = dma_fence_wait_timeout(fence, true,
						MAX_SCHEDULE_TIMEOUT);
		if (waited <= 0)
			return waited < 0 ? waited : -ETIMEDOUT;
		status = dma_fence_get_status(fence);
		if (status < 0)
			return status;
	}
	return 0;
}

static int m9_bo_fill_gpu(struct m9x_device *mdev, struct drm_gem_object *gem,
			  u64 offset, u64 bytes, u32 *address)
{
	s64 base = m9x_gem_vram_offset(gem);
	u64 start, end;

	if (base < 0)
		return base;
	if (check_add_overflow((u64)base, offset, &start) ||
	    check_add_overflow(start, bytes, &end) ||
	    end > m9x_vram_gpu_limit(mdev) ||
	    check_add_overflow(start, M9_VRAM_GPU_BASE, &start) ||
	    upper_32_bits(start + bytes - 1))
		return -ERANGE;
	*address = lower_32_bits(start);
	return 0;
}

static int m9_bo_fill(struct m9x_device *mdev, struct drm_file *file,
		      struct drm_parhelia_m9_bo_fill *args, u32 *launches)
{
	struct m9x_transport *t = &mdev->transport;
	struct m9x_bo_fill *f = &mdev->bo_fill;
	struct m9x_engine_fill_request req = {
		.bytes = args->size,
		.span = M9X_ENGINE_FILL_SPAN,
		.fence_bus = lower_32_bits(t->fence_bus),
	};
	struct m9x_engine_fill_plan plan;
	struct drm_gem_object **bos;
	struct m9_retired *retired;
	struct m9x_fence *fence = NULL;
	struct drm_exec exec;
	u32 capacity, slot, *words = NULL;
	u64 end, issued;
	bool *pinned, posted = false;
	int ret;

	lockdep_assert_held(&f->lock);
	bos = kcalloc(2, sizeof(*bos), GFP_KERNEL);
	pinned = kcalloc(2, sizeof(*pinned), GFP_KERNEL);
	retired = kzalloc_obj(*retired);
	if (!bos || !pinned || !retired) {
		ret = -ENOMEM;
		goto out;
	}
	bos[0] = drm_gem_object_lookup(file, args->handle);
	if (!bos[0]) {
		ret = -ENOENT;
		goto out;
	}
	if (!m9x_gem_in_vram(bos[0]) ||
	    check_add_overflow(args->offset, args->size, &end) ||
	    end > bos[0]->size) {
		ret = -EINVAL;
		goto out;
	}
	ret = m9_bo_fill_source_locked(mdev, args->size / M9_FILL_ROW_BYTES);
	if (ret)
		goto out;
	bos[1] = f->source;
	drm_gem_object_get(bos[1]);
	req.source_rows = f->source_rows;
	capacity = m9x_engine_fill_capacity(DIV_ROUND_UP(args->size /
							 M9_FILL_ROW_BYTES,
							 req.source_rows));
	/* Half a staging slot, as the engine readback's stream image. */
	if (capacity > PARHELIA_M9_COMMAND_BYTES / sizeof(u32) / 2) {
		ret = -E2BIG;
		goto out;
	}
	words = kvmalloc_array(capacity, sizeof(*words), GFP_KERNEL);
	if (!words) {
		ret = -ENOMEM;
		goto out;
	}
	for (u32 i = 0; i < 2; i++) {
		ret = m9x_gem_vram_pin(bos[i]);
		if (ret)
			goto out;
		pinned[i] = true;
	}

	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT, 1);
	drm_exec_until_all_locked(&exec) {
		ret = drm_exec_prepare_obj(&exec, bos[0], 1);
		drm_exec_retry_on_contention(&exec);
		if (ret)
			goto out_exec;
	}
	ret = m9_bo_fill_wait_foreign(mdev, bos[0]);
	if (ret)
		goto out_exec;
	mutex_lock(&t->lock);
	if (t->async_stop) {
		ret = -ENODEV;
		goto out_unlock;
	}
	if (!m9_bo_fill_admitted_locked(mdev)) {
		ret = -EBUSY;
		goto out_unlock;
	}
	ret = m9_bo_fill_gpu(mdev, bos[0], args->offset, args->size,
			     &req.dest);
	if (!ret)
		ret = m9_bo_fill_gpu(mdev, bos[1], 0, bos[1]->size,
				     &req.source);
	if (ret)
		goto out_unlock;
	ret = m9x_transport_queue_room_locked(mdev, &slot);
	if (ret)
		goto out_unlock;
	if (t->wedged) {
		ret = -EIO;
		goto out_unlock;
	}
	req.token = t->next_fence;
	ret = m9x_engine_fill_encode(&req, words, capacity, &plan);
	if (ret)
		goto out_unlock;
	issued = ((u64)(t->fence_epoch + (plan.final < t->last_fence)) << 32) |
		 plan.final;
	fence = m9x_fence_create_target_locked(mdev, plan.final);
	if (!fence) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	dma_resv_add_fence(bos[0]->resv, &fence->base, DMA_RESV_USAGE_WRITE);
	ret = m9x_submit_kernel_queue_locked(mdev, words, plan.dwords,
					     plan.final, NULL, &posted);
	if (posted) {
		/* No secondary slot: the copy runs from the primary itself. */
		struct m9_queued_job job = {
			.point = issued, .target = plan.final,
			.primary_slot = t->command_slot,
			.secondary_slot = M9_QUEUE_SLOTS,
			.bos = bos, .pinned = pinned, .count = 2,
			.fence = fence, .retire = retired,
		};

		/* A post-doorbell error keeps every reader until DMA stops. */
		if (ret && !t->wedged) {
			t->wedged = true;
			t->generation++;
		}
		m9x_transport_queue_commit_locked(mdev, &job);
		t->next_fence = plan.final + 0x10;
		fence = NULL;
		bos = NULL;
		pinned = NULL;
		retired = NULL;
		if (!ret) {
			args->out_point = issued;
			*launches = plan.launches;
			if (!f->fills)
				drm_info(&mdev->drm,
					 "M9 first fill queued: %u dwords %u launches dest %#x source %#x final %#x\n",
					 plan.dwords, plan.launches, req.dest,
					 req.source, plan.final);
		}
	}
out_unlock:
	if (fence) {
		dma_fence_set_error(&fence->base, ret ? ret : -ECANCELED);
		dma_fence_signal(&fence->base);
		dma_fence_put(&fence->base);
	}
	mutex_unlock(&t->lock);
out_exec:
	drm_exec_fini(&exec);
out:
	for (u32 i = 0; bos && i < 2; i++) {
		if (!bos[i])
			continue;
		if (pinned && pinned[i])
			m9x_gem_vram_unpin(bos[i]);
		drm_gem_object_put(bos[i]);
	}
	kvfree(words);
	kfree(bos);
	kfree(pinned);
	kfree(retired);
	return ret;
}

int m9x_bo_fill_ioctl(struct drm_device *drm, void *data,
		      struct drm_file *file)
{
	struct drm_parhelia_m9_bo_fill *args = data;
	struct m9x_device *mdev = to_m9x(drm);
	struct m9x_bo_fill *f = &mdev->bo_fill;
	u64 start = ktime_get_ns();
	u32 launches = 0;
	int idx, ret;

	if (!m9x_enabled())
		return -ENODEV;
	if (!m9_bo_fill_mode)
		return -EOPNOTSUPP;
	if (args->flags & ~PARHELIA_M9_BO_FILL_QUERY || args->value ||
	    args->pad || args->out_point)
		return -EINVAL;
	if (args->flags)
		return args->handle || args->offset || args->size ? -EINVAL : 0;
	if (!args->handle || !args->size || args->size > U32_MAX ||
	    !IS_ALIGNED(args->offset | args->size, M9_FILL_ROW_BYTES))
		return -EINVAL;
	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;
	mutex_lock(&f->lock);
	ret = m9_bo_fill(mdev, file, args, &launches);
	if (!ret) {
		f->fills++;
		f->bytes += args->size;
		f->launches += launches;
		f->ns += ktime_get_ns() - start;
	} else {
		f->refused++;
		f->last_refusal = ret;
	}
	mutex_unlock(&f->lock);
	drm_dev_exit(idx);
	return ret;
}

int m9x_bo_fill_stats_show(struct m9x_device *mdev, struct seq_file *m)
{
	struct m9x_bo_fill *f = &mdev->bo_fill;

	mutex_lock(&f->lock);
	seq_printf(m, "mode=%u source_rows=%u max_rows=%u\n", m9_bo_fill_mode,
		   f->source_rows, m9_bo_fill_source_rows);
	seq_printf(m, "fills=%llu bytes=%llu launches=%llu ns=%llu\n",
		   f->fills, f->bytes, f->launches, f->ns);
	seq_printf(m, "refused=%llu last=%d\n", f->refused, f->last_refusal);
	mutex_unlock(&f->lock);
	return 0;
}

/* The encoder at two placements, a multi-launch range and its refusals. */
int m9x_bo_fill_selftest(void)
{
	struct m9x_engine_fill_request req = {
		.dest = 0x01e00000, .bytes = SZ_1M, .source = 0x02f0e000,
		.source_rows = 512, .span = M9X_ENGINE_FILL_SPAN,
		.fence_bus = 0x18d51000, .token = 0x40,
	};
	u32 capacity = m9x_engine_fill_capacity(4), *w;
	struct m9x_engine_fill_plan plan;
	int ret = -EINVAL;

	w = kcalloc(capacity, sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;
	if (m9x_engine_fill_encode(&req, w, capacity, &plan) ||
	    plan.launches != 1 || plan.rows != 256 || plan.final != 0x70 ||
	    w[0] != 0x4545451b || w[1] != 0x18d51002 ||
	    w[2] != 0x4545e3e6 || w[4] != 0x41 || w[6] != 0x40 ||
	    w[plan.dwords - 1] != 0x70 || w[plan.dwords - 3] != 0x71)
		goto out;
	req.bytes = 1300 * M9_FILL_ROW_BYTES;
	req.source_rows = 512;
	if (m9x_engine_fill_encode(&req, w, capacity, &plan) ||
	    plan.launches != 3 || plan.final != 0x90 ||
	    plan.dwords != capacity - M9X_ENGINE_FILL_LAUNCH_WORDS)
		goto out;
	req.dest |= 0x800;
	if (m9x_engine_fill_encode(&req, w, capacity, &plan) != -EINVAL)
		goto out;
	req.dest &= ~0x800u;
	req.bytes += 0x100;
	if (m9x_engine_fill_encode(&req, w, capacity, &plan) != -EINVAL)
		goto out;
	req.bytes = SZ_1M;
	req.token = 0xffffffd0;
	if (m9x_engine_fill_encode(&req, w, capacity, &plan) != -ERANGE)
		goto out;
	req.token = 0x40;
	if (m9x_engine_fill_encode(&req, w, capacity - 100, &plan) != -ENOSPC)
		goto out;
	ret = 0;
out:
	kfree(w);
	return ret;
}
