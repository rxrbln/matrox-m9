// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Private replay of the coherent surface-owned M9128 vendor FAA frame.
 * Command grammar and source validity come from the normal CPU capture.
 * This diagnostic exposes neither arbitrary indirect execution nor app MSAA.
 * Opaque stores, HZ, pool and allocation tails receive no fabricated memset.
 */
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/iosys-map.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include "m9_drv.h"
#include "m9_reg.h"

struct m9_faa_owner_spec {
	u32 captured, bytes;
	bool local;
};
struct m9_faa_span {
	u32 owner, offset, blob_offset, bytes;
};
struct m9_faa_fixup {
	u32 dst, offset, src, addend, encoder, tag, captured;
};
struct m9_faa_tracker {
	u32 owner, offset, captured;
};
#include "m9_faa_native_data.h"

struct m9_faa_owner {
	struct drm_gem_vram_object *vram;
	struct iosys_map map;
	void *cpu;
	dma_addr_t dma;
	u32 base, bytes;
};
struct m9x_faa_replay {
	struct m9_faa_owner owner[ARRAY_SIZE(m9_faa_owner_specs)];
	bool prepared, attempted;
};

static bool m9_faa_diag;
module_param(m9_faa_diag, bool, 0400);
MODULE_PARM_DESC(m9_faa_diag,
	"Use the kernel-owned normal FAA frame for one FIRST_TRIANGLE diagnostic (M9128 only)");
MODULE_FIRMWARE("m9x/faa-normal-known-inputs.bin");

bool m9x_faa_diag_enabled(void)
{
	return m9_faa_diag;
}

static void m9_faa_release(struct m9x_device *mdev, struct m9x_faa_replay *r)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(r->owner); i++) {
		struct m9_faa_owner *o = &r->owner[i];

		if (o->vram) {
			ttm_bo_reserve(&o->vram->bo, false, false, NULL);
			drm_gem_vram_vunmap(o->vram, &o->map);
			ttm_bo_unreserve(&o->vram->bo);
			m9x_vram_unpin(o->vram);
			drm_gem_vram_put(o->vram);
		} else if (o->cpu) {
			dma_free_coherent(&mdev->pdev->dev, o->bytes, o->cpu, o->dma);
		}
	}
	kfree(r);
}

/* Called by requester teardown only after PCI bus mastering has stopped. */
void m9x_faa_fini(struct m9x_device *mdev)
{
	if (mdev->transport.faa) {
		m9_faa_release(mdev, mdev->transport.faa);
		mdev->transport.faa = NULL;
	}
}

static int m9_faa_allocate(struct m9x_device *mdev, struct m9_faa_owner *o,
			   const struct m9_faa_owner_spec *spec)
{
	struct drm_gem_vram_object *vram;
	u64 end, base;
	s64 offset;
	int ret;

	o->bytes = PAGE_ALIGN(spec->bytes);
	if (!spec->local) {
		o->cpu = dma_alloc_coherent(&mdev->pdev->dev, o->bytes,
					    &o->dma, GFP_KERNEL);
		if (!o->cpu)
			return -ENOMEM;
		if (upper_32_bits(o->dma) || !IS_ALIGNED(o->dma, PAGE_SIZE) ||
		    (u64)o->dma + o->bytes > U32_MAX)
			return -ERANGE;
		o->base = lower_32_bits(o->dma);
		return 0;
	}
	vram = drm_gem_vram_create(&mdev->drm, o->bytes, 1);
	if (IS_ERR(vram))
		return PTR_ERR(vram);
	ret = m9x_vram_pin(vram);
	if (ret)
		goto put;
	offset = drm_gem_vram_offset(vram);
	if (offset < 0 || check_add_overflow((u64)offset, (u64)o->bytes, &end) ||
	    end > m9x_vram_gpu_limit(mdev) ||
	    check_add_overflow(M9_VRAM_GPU_BASE, (u64)offset, &base) ||
	    base + o->bytes > U32_MAX || !IS_ALIGNED(base, PAGE_SIZE)) {
		ret = offset < 0 ? offset : -ERANGE;
		goto unpin;
	}
	ret = ttm_bo_reserve(&vram->bo, true, false, NULL);
	if (ret)
		goto unpin;
	ret = drm_gem_vram_vmap(vram, &o->map);
	ttm_bo_unreserve(&vram->bo);
	if (ret)
		goto unpin;
	o->vram = vram;
	o->base = base;
	return 0;
unpin:
	m9x_vram_unpin(vram);
put:
	drm_gem_vram_put(vram);
	return ret;
}

static void m9_faa_write(struct m9_faa_owner *o, u32 offset,
			 const void *source, u32 bytes)
{
	if (o->vram)
		iosys_map_memcpy_to(&o->map, offset, source, bytes);
	else
		memcpy((u8 *)o->cpu + offset, source, bytes);
}

/* Allocations precede the caller's color BO reservation, as in ordinary
 * FIRST_TRIANGLE. Every private owner survives both publication and timeout. */
int m9x_faa_prepare_diag(struct m9x_device *mdev)
{
	const struct firmware *fw = NULL;
	struct m9x_faa_replay *r;
	u32 i, hash = 0x811c9dc5;
	int ret;

	if (!m9_faa_diag || mdev->pdev->device != 0x0540)
		return -EACCES;
	/* This replay joins tagged virtual fetches to the driver's identity
 * leaves. A diagnostic local-base override has no such proven contract. */
	if (m9x_local_gpu_base)
		return -EOPNOTSUPP;
	mutex_lock(&mdev->transport.lock);
	if (mdev->transport.wedged) {
		ret = -EIO;
		goto unlock;
	}
	if (mdev->transport.faa) {
		ret = mdev->transport.faa->attempted ? -EALREADY : 0;
		goto unlock;
	}
	/* Engine prerequisite allocations must also finish outside COLOR RESV. */
	mutex_unlock(&mdev->transport.lock);
	ret = m9x_triangle_prepare_user(mdev);
	if (ret)
		return ret;
	mutex_lock(&mdev->transport.lock);
	if (mdev->transport.faa || mdev->transport.wedged) {
		ret = mdev->transport.wedged ? -EIO :
			(mdev->transport.faa->attempted ? -EALREADY : 0);
		goto unlock;
	}
	ret = request_firmware(&fw, "m9x/faa-normal-known-inputs.bin", &mdev->pdev->dev);
	if (ret)
		goto unlock;
	if (fw->size != M9_FAA_BLOB_BYTES) {
		ret = -EINVAL;
		goto release_fw;
	}
	for (i = 0; i < fw->size; i++)
		hash = (hash ^ fw->data[i]) * 0x01000193;
	if (hash != M9_FAA_BLOB_FNV) {
		ret = -EINVAL;
		goto release_fw;
	}
	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r) {
		ret = -ENOMEM;
		goto release_fw;
	}
	for (i = 0; i < ARRAY_SIZE(r->owner); i++) {
		/* The pinned/reserved caller's exact 320x240 BO owns the primary. */
		if (i == M9_FAA_COLOR_OWNER)
			continue;
		ret = m9_faa_allocate(mdev, &r->owner[i], &m9_faa_owner_specs[i]);
		if (ret)
			goto free;
	}
	for (i = 0; i < ARRAY_SIZE(m9_faa_spans); i++) {
		const struct m9_faa_span *s = &m9_faa_spans[i];

		if (s->owner >= ARRAY_SIZE(r->owner) ||
		    s->offset > m9_faa_owner_specs[s->owner].bytes ||
		    s->bytes > m9_faa_owner_specs[s->owner].bytes - s->offset ||
		    s->blob_offset > fw->size || s->bytes > fw->size - s->blob_offset ||
		    s->owner == M9_FAA_COLOR_OWNER) {
			ret = -EINVAL;
			goto free;
		}
		m9_faa_write(&r->owner[s->owner], s->offset,
			     fw->data + s->blob_offset, s->bytes);
	}
	r->prepared = true;
	mdev->transport.faa = r;
	dev_info(&mdev->pdev->dev, "M9 FAA private inputs: %zu owners, %u known bytes, 53 address/26 tracker fixups\n",
		 ARRAY_SIZE(r->owner), M9_FAA_BLOB_BYTES);
	ret = 0;
	goto release_fw;
free:
	m9_faa_release(mdev, r);
release_fw:
	release_firmware(fw);
unlock:
	mutex_unlock(&mdev->transport.lock);
	return ret;
}

static u32 m9_faa_encode(u32 pc, u32 address, u32 tag)
{
	switch (pc) {
	case 0x24886: return (address & 0xfffff000) | tag | (!address ? 4 : 0);
	case 0x248ab: return ((address & 0xfff) >> 3) & 0xfffff;
	case 0x248c8: return (address & ~3u) | tag;
	case 0x248dc: return (address & 0xffffff84) | tag | 4;
	case 0x24909: return (address | tag) & ~3u;
	case 0x24919: return (address & 0xfffffffd) | 1;
	case 0x24930:
	case 0x256f1: return address | tag;
	default: return 0;
	}
}

int m9x_faa_submit_diag(struct m9x_device *mdev, u32 color,
			struct drm_gem_vram_object *color_bo, u64 *point,
			bool *color_active)
{
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	struct m9x_fence *fence = NULL;
	u32 i, base, target;
	bool posted = false;
	int ret;

	*point = 0;
	*color_active = false;
	mutex_lock(&t->lock);
	r = t->faa;
	if (!m9_faa_diag || !r || !r->prepared || r->attempted || t->wedged ||
	    !IS_ALIGNED(color, PAGE_SIZE)) {
		ret = t->wedged ? -EIO : -EINVAL;
		goto unlock;
	}
	/* No live job may observe this probe's counter destinations or state.
 * The earlier ioctl-side drain does not cover the intervening lock gap. */
	ret = m9x_transport_drain_locked(mdev);
	if (ret)
		goto unlock;
	ret = m9x_engine_ready_locked(mdev);
	if (ret)
		goto unlock;
	/* The ordinary transport owns 0x400, the common counters and the
 * default restore area. Do not copy captured MMIO physical addresses. */
	if (!t->restore_ready || !t->transport_armed || t->next_fence < 0x10 ||
	    t->next_fence > U32_MAX - M9_FAA_TRACKER_LAST) {
		ret = -ERANGE;
		goto unlock;
	}
	base = t->next_fence - 0x10;
	target = base + M9_FAA_TRACKER_LAST - 0x10;
	/* Retain a real observed retirement as the new common-buffer seed.
 * Never advance a completion counter on the CPU to fit the replay. */
	if ((READ_ONCE(((u32 *)t->fence_cpu)[0]) & ~15u) != base) {
		ret = -EAGAIN;
		goto unlock;
	}
	ret = m9x_address_check_local_identity(mdev, t->restore_base,
					 PARHELIA_M9_RESTORE_BYTES);
	if (ret)
		goto unlock;
	r->owner[M9_FAA_COLOR_OWNER].base = color;
	for (i = 0; i < ARRAY_SIZE(r->owner); i++) {
		if (!m9_faa_owner_specs[i].local)
			continue;
		ret = m9x_address_check_local_identity(mdev, r->owner[i].base,
				PAGE_ALIGN(m9_faa_owner_specs[i].bytes));
		if (ret) {
			dev_err(&mdev->pdev->dev, "M9 FAA identity leaf refused: owner=%u base=%#x ret=%d\n",
				i, r->owner[i].base, ret);
			goto unlock;
		}
	}
	/* Once patching begins this immutable capture is consumed, even if a
 * later admission fails. A retry must not reinterpret already rebased
 * words as their original captured addresses. */
	r->attempted = true;
	for (i = 0; i < ARRAY_SIZE(m9_faa_fixups); i++) {
		const struct m9_faa_fixup *f = &m9_faa_fixups[i];
		struct m9_faa_owner *dst;
		u32 address, value;

		if (f->dst >= ARRAY_SIZE(r->owner) || f->src >= ARRAY_SIZE(r->owner)) {
			ret = -EINVAL;
			goto unlock;
		}
		dst = &r->owner[f->dst];
		if (dst->vram || !dst->cpu || !IS_ALIGNED(f->offset, 4) ||
		    f->offset > m9_faa_owner_specs[f->dst].bytes - 4 ||
		    f->addend > m9_faa_owner_specs[f->src].bytes ||
		    check_add_overflow(r->owner[f->src].base, f->addend, &address) ||
		    get_unaligned_le32((u8 *)dst->cpu + f->offset) != f->captured) {
			ret = -EINVAL;
			goto unlock;
		}
		value = m9_faa_encode(f->encoder, address, f->tag);
		if (m9_faa_encode(f->encoder,
			m9_faa_owner_specs[f->src].captured + f->addend,
			f->tag) != f->captured) {
			ret = -EINVAL;
			goto unlock;
		}
		put_unaligned_le32(value, (u8 *)dst->cpu + f->offset);
	}
	for (i = 0; i < ARRAY_SIZE(m9_faa_trackers); i++) {
		const struct m9_faa_tracker *f = &m9_faa_trackers[i];
		struct m9_faa_owner *dst;
		u32 value = base + (f->captured & ~15u) - 0x10;

		if (f->owner >= ARRAY_SIZE(r->owner)) {
			ret = -EINVAL;
			goto unlock;
		}
		dst = &r->owner[f->owner];
		if (dst->vram || !dst->cpu || !IS_ALIGNED(f->offset, 4) ||
		    f->offset > m9_faa_owner_specs[f->owner].bytes - 4 ||
		    get_unaligned_le32((u8 *)dst->cpu + f->offset) != f->captured) {
			ret = -EINVAL;
			goto unlock;
		}
		put_unaligned_le32(value | (f->captured & 15), (u8 *)dst->cpu + f->offset);
	}
	fence = m9x_fence_create_target_locked(mdev, target);
	if (!fence) {
		ret = -ENOMEM;
		goto unlock;
	}
	dma_resv_add_fence(color_bo->bo.base.resv, &fence->base, DMA_RESV_USAGE_WRITE);
	/* Actual 23a3b/23aaf normal activation contracts. The ordinary context
 * just retired; the two additional tracker words belong to this same
 * retained coherent common buffer, not captured host physical memory.
 * Only the actual known128-byte initializer is reproduced; the low seed
 * equals the already observed GPU retirement and does not publish success. */
	memset(t->fence_cpu, 0, 128);
	WRITE_ONCE(((u32 *)t->fence_cpu)[0], base);
	WRITE_ONCE(((u32 *)t->fence_cpu)[0x20 / 4], base);
	WRITE_ONCE(((u32 *)t->fence_cpu)[0x40 / 4], base);
	WRITE_ONCE(((u32 *)t->fence_cpu)[0x60 / 4], base);
	dma_wmb();
	m9x_write(mdev, M9_REG_FENCE0_ADDRESS, lower_32_bits(t->fence_bus + 0x20) | 2);
	m9x_write(mdev, M9_REG_FENCE2_ADDRESS, lower_32_bits(t->fence_bus + 0x40) | 2);
	/* Genuine normal activation selects the virtual restore descriptor.
 * Its native identity leaves were checked separately from FAA pool6. */
	m9x_write(mdev, M9_REG_RESTORE_TAGGED, ((t->restore_base + 0x200) & ~31u) | 1);
	m9x_read(mdev, M9_REG_FENCE2_ADDRESS);
	ret = m9x_submit_kernel_stream_locked(mdev,
			r->owner[M9_FAA_PRIMARY_OWNER].cpu, M9_FAA_PRIMARY_BYTES / 4,
			target, point, &posted, false);
	*color_active = posted && ret;
	if (!ret) {
		t->next_fence = target + 0x10;
		dev_info(&mdev->pdev->dev,
			 "M9 FAA normal frame retired: point=%#llx primary=%#x descriptor=cc220140 320x240\n",
			 *point, color);
	}
unlock:
	if (fence && ret)
		dma_fence_set_error(&fence->base, ret);
	if (fence && *color_active) {
		t->wedged = true;
		t->generation++;
		WARN_ON(t->quarantined_fence || t->quarantined_color);
		t->quarantined_fence = fence;
		t->quarantined_color = color_bo;
		fence = NULL;
	} else if (fence) {
		dma_fence_signal(&fence->base);
	}
	if (fence)
		dma_fence_put(&fence->base);
	mutex_unlock(&t->lock);
	return ret;
}
