// SPDX-License-Identifier: GPL-2.0-only
/* Retained M9128 FAA texture references. No command or GPU read is posted. */
#ifdef M9_FAA_TEXTURE_OWNER_HOST_TEST
#include "tests/faa-texture-owner-stubs.h"
#else
#include <linux/overflow.h>
#include <linux/mm.h>
#include <linux/string.h>

#include <drm/drm_exec.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_vram_helper.h>
#include <drm/ttm/ttm_bo.h>

#include "m9_drv.h"
#endif
#include "m9_faa_graph_texture_owner.h"

static int m9_faa_texture_live_view(struct m9x_device *mdev,
	struct drm_gem_object *gem, u32 *base, u32 *bytes)
{
	u64 addr, end, limit = m9x_vram_gpu_limit(mdev);
	s64 offset;

	if (!gem || !m9x_gem_is_vram(gem) || m9x_gem_vram_tiled(gem) ||
	    !gem->size || gem->size > U32_MAX || !limit || m9x_local_gpu_base)
		return -EINVAL;
	offset = m9x_gem_vram_offset(gem);
	if (offset < 0 || !IS_ALIGNED(offset, PAGE_SIZE) ||
	    check_add_overflow((u64)offset, gem->size, &end) || end > limit ||
	    check_add_overflow(M9_VRAM_GPU_BASE, (u64)offset, &addr) ||
	    addr >= M9_ADDRESS_SYSTEM_BASE ||
	    check_add_overflow(addr, gem->size, &end) ||
	    end > M9_ADDRESS_SYSTEM_BASE)
		return -ERANGE;
	*base = addr;
	*bytes = gem->size;
	return 0;
}

bool m9x_faa_texture_owners_put(struct m9x_faa_texture_owners *owners,
	bool dma_stopped)
{
	struct drm_exec exec;
	u32 i;
	int ret = 0;

	if (!owners)
		return false;
	if (owners->retirement && !dma_stopped &&
	    !dma_fence_is_signaled(owners->retirement))
		return false;
	/* Reserve every TTM BO before unpinning any, so a failed reservation
	 * leaves the whole retained set intact for retry after DMA stop. */
	drm_exec_init(&exec, DRM_EXEC_IGNORE_DUPLICATES, 2);
	drm_exec_until_all_locked(&exec) {
		for (i = 0; i < 2; i++) {
			if (!owners->pinned[i])
				continue;
			ret = drm_exec_lock_obj(&exec, owners->gem[i]);
			drm_exec_retry_on_contention(&exec);
			if (ret)
				goto unlock;
		}
	}
	for (i = 0; i < 2; i++)
		if (owners->pinned[i])
			ttm_bo_unpin(&drm_gem_vram_of_gem(owners->gem[i])->bo);
unlock:
	drm_exec_fini(&exec);
	if (ret)
		return false;
	for (i = 0; i < 2; i++)
		if (owners->gem[i])
			drm_gem_object_put(owners->gem[i]);
	dma_fence_put(owners->retirement);
	memset(owners, 0, sizeof(*owners));
	return true;
}

int m9x_faa_texture_owners_acquire(struct m9x_device *mdev,
	struct drm_file *file, const u32 handle[2],
	const struct m9x_faa_texture_bind_input request[2],
	struct drm_gem_object *const graph_owner[6], u32 generation,
	struct m9x_faa_texture_owners *out)
{
	struct m9x_faa_texture_bind_words words;
	struct m9x_faa_texture_bind_input check;
	u32 i, j;
	int ret;

	if (!out)
		return -EINVAL;
	/* An acquired set owns GEM references and pins. Never replace it by
	 * clearing caller storage; callers initialize an empty set to zero. */
	if (out->gem[0] || out->gem[1] || out->retirement ||
	    out->pinned[0] || out->pinned[1])
		return -EBUSY;
	if (!mdev || !file || !file->minor || file->minor->dev != &mdev->drm ||
	    !handle || !request || !graph_owner ||
	    !handle[0] || !handle[1] || handle[0] == handle[1] ||
	    request[0].stage != 0 || request[1].stage != 1 ||
	    request[0].coordinate_sets != 2 || request[1].coordinate_sets != 2 ||
	    request[0].coordinate_index != 0 || request[1].coordinate_index != 1 ||
	    mdev->pdev->device != M9X_PCI_DEVICE_PHOENIX ||
	    mdev->pdev->revision != 1 ||
	    mdev->pdev->subsystem_device != 0x22c0)
		return -EINVAL;
	out->generation = generation;
	for (i = 0; i < 2; i++) {
		out->gem[i] = drm_gem_object_lookup(file, handle[i]);
		if (!out->gem[i]) {
			ret = -ENOENT;
			goto fail;
		}
		if (i && out->gem[i] == out->gem[0]) {
			ret = -EINVAL;
			goto fail;
		}
		for (j = 0; j < 6; j++)
			if (!graph_owner[j] || graph_owner[j] == out->gem[i]) {
				ret = -EINVAL;
			goto fail;
			}
		if (!m9x_gem_is_vram(out->gem[i]) ||
		    m9x_gem_vram_tiled(out->gem[i])) {
			ret = -EINVAL;
			goto fail;
		}
		ret = m9x_gem_vram_pin(out->gem[i]);
		if (ret)
			goto fail;
		out->pinned[i] = true;
		out->bind[i] = request[i];
		ret = m9_faa_texture_live_view(mdev, out->gem[i],
			&out->bind[i].owner_base, &out->bind[i].owner_bytes);
		if (ret)
			goto fail;
		out->bind[i].owner_domain = M9_FAA_TEXTURE_VRAM;
		/* The dual source uses one common f44xx descriptor producer for
		 * both physical units. Its placement helper makes these same
		 * temporary stage-0/AE selections before encoding each packet. */
		check = out->bind[i];
		check.stage = 0;
		check.raster_ae_base = 0x28207006;
		ret = m9x_faa_texture_bind_encode(&check, &words);
		if (ret)
			goto fail;
	}
	if ((u64)out->bind[0].owner_base + out->bind[0].owner_bytes >
	    out->bind[1].owner_base &&
	    (u64)out->bind[1].owner_base + out->bind[1].owner_bytes >
	    out->bind[0].owner_base) {
		ret = -EINVAL;
		goto fail;
	}
	return 0;
fail:
	/* Before any post there is no retirement fence. Keep pinned references
	 * in out if TTM reservations cannot be acquired during unwind; callers
	 * must retry put() rather than discard an occupied out. */
	if (!m9x_faa_texture_owners_put(out, false))
		return -EBUSY;
	return ret;
}

int m9x_faa_texture_owners_place_locked(struct m9x_device *mdev,
	const struct m9x_faa_texture_owners *owners, u32 generation,
	const struct m9x_faa_texture_dual_program *program,
	u8 *secondary, u32 secondary_bytes)
{
	struct m9x_faa_texture_bind_input live[2];
	u32 i;
	int ret;

	if (!mdev || !owners || !program || !secondary)
		return -EINVAL;
	lockdep_assert_held(&mdev->transport.lock);
	if (generation != owners->generation ||
	    generation != mdev->transport.generation ||
	    mdev->transport.wedged || !mdev->transport.faa_virtual_primary)
		return -ESTALE;
	for (i = 0; i < 2; i++) {
		if (!owners->gem[i] || !owners->pinned[i])
			return -ESTALE;
		dma_resv_assert_held(owners->gem[i]->resv);
		if (!dma_resv_test_signaled(owners->gem[i]->resv,
					    DMA_RESV_USAGE_READ))
			return -EBUSY;
		live[i] = owners->bind[i];
		ret = m9_faa_texture_live_view(mdev, owners->gem[i],
			&live[i].owner_base, &live[i].owner_bytes);
		if (ret || live[i].owner_base != owners->bind[i].owner_base ||
		    live[i].owner_bytes != owners->bind[i].owner_bytes)
			return -ESTALE;
	}
	return m9x_faa_texture_dual_bind_place(live, program, secondary,
					       secondary_bytes);
}

int m9x_faa_texture_owners_fence_locked(struct m9x_faa_texture_owners *owners,
	struct dma_fence *fence)
{
	u32 i;
	int ret;

	if (!owners || !fence || owners->retirement || !owners->pinned[0] ||
	    !owners->pinned[1])
		return -EINVAL;
	for (i = 0; i < 2; i++) {
		dma_resv_assert_held(owners->gem[i]->resv);
		ret = dma_resv_reserve_fences(owners->gem[i]->resv, 1);
		if (ret)
			return ret;
	}
	for (i = 0; i < 2; i++)
		dma_resv_add_fence(owners->gem[i]->resv, fence,
				   DMA_RESV_USAGE_READ);
	owners->retirement = dma_fence_get(fence);
	return 0;
}
