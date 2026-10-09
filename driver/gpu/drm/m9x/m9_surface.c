// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Linear CPU access to tiled M9 render targets through the host-aperture
 * surface window (slot 1); the slot 0 identity view stays valid meanwhile.
 */
#include <linux/dma-mapping.h>
#include <linux/dma-resv.h>
#include <linux/ktime.h>
#include <linux/moduleparam.h>
#include <linux/overflow.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_vram_helper.h>

#include "m9_drv.h"
#include "m9_engine_read_encode.h"
#include "m9_reg.h"

#define M9_SURFACE_MAX_SPAN	2048u
#define M9_SURFACE_FAMILY	0xcc210000u
/* Bits the format code occupies in a surface descriptor dword. */
#define M9_SURFACE_CODE_MASK	0xfe000000u
#define M9_SURFACE_WAIT		msecs_to_jiffies(10000)
/* Texture storage kinds: small (<= 64x64) and 32-aligned large levels. */
#define M9_TEXTURE_KIND_SMALL	0x01u
#define M9_TEXTURE_KIND_LARGE	0x21u
/* Rows of the widest surface one 32-row chunk needs (MxMD.dll 0xb14f3). */
#define M9_ENGINE_READ_MIN_RING	(M9_SURFACE_MAX_SPAN * 4 * 32)
#define M9_ENGINE_READ_MAX_RING	SZ_64M

static uint m9_surface_engine_read;
module_param_named(surface_engine_read, m9_surface_engine_read, uint, 0644);
MODULE_PARM_DESC(surface_engine_read,
		 "Read render targets by engine copy to system memory: 0 window, 1 with the vendor 2-D state, 2 copy registers only (UNTESTED)");
static uint m9_surface_engine_min_bytes = SZ_64K;
module_param_named(surface_engine_min_bytes, m9_surface_engine_min_bytes, uint,
		   0644);
MODULE_PARM_DESC(surface_engine_min_bytes,
		 "Smallest read box the engine copy takes, in bytes");
/* The vendor's two DdBlt rings are 2 MiB each (ring heap +0x4258). */
static uint m9_surface_engine_ring_kib = 2048;
module_param_named(surface_engine_ring_kib, m9_surface_engine_ring_kib, uint,
		   0444);
MODULE_PARM_DESC(surface_engine_ring_kib,
		 "Size of each of the two engine readback rings, KiB");

struct m9_surface_view {
	bool captured_cpu_view;
	bool lock_cpu_view;
	u32 span;
	u32 height;
	u32 cpp;
	/* Addressing units of one row and of the surface.  For an ordinary
	 * format these are the texel span and height; for an S3TC one they
	 * are the block counts, because a compressed surface is addressed by
	 * whole 4x4 blocks (docs/parhelia-m9-s3tc-compressed-textures.md). */
	u32 elements;
	u32 rows;
	u64 bytes;
};

/* Bytes one S3TC block occupies, 0 when the code is not compressed.  DXT1 is
 * 4 bits a texel and DXT2..5 are 8 (79[31:24] = 0x20, 0x2a, 0x2e). */
static u32 m9_compressed_block_bytes(u8 format)
{
	switch (format) {
	case 0x20:
		return 8;
	case 0x2a: case 0x2e:
		return 16;
	default:
		return 0;
	}
}

/* Packed 32-bit colour codes a render surface may carry: A8R8G8B8 and the
 * two GigaColor formats, which are the same storage class 2. */
static bool m9_surface_code_is_packed32(u8 code)
{
	return code == 0x66 || code == 0x68 || code == 0x6a;
}

/* Element size per captured texture format byte, 0 when unsupported. */
static u32 m9_texture_cpp(u8 format)
{
	switch (format) {
	case 0xc8: case 0xc9: case 0xcc:
		return 4;
	/* 10-bit GigaColor is storage class 2 at 32 bits per texel, tiled
	 * exactly like the BGRA8 family (parhelia-m9-gigacolor-10bit.md). */
	case 0xd0: case 0xd4:
		return 4;
	case 0x9d: case 0x8c: case 0x94: case 0xa2:
		return 2;
	case 0x66: case 0x61:
		return 1;
	default:
		return 0;
	}
}

/* Vendor render allocation contract, shared with Mesa m9_internal.h. */
bool m9x_surface_layout_bytes(u32 width, u32 height, u32 cpp, u64 *size)
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

/*
 * The window descriptor carries a level's 32-aligned span, not its width, and
 * the allocation contract's spare-element rule depends on that width: a level
 * narrower than its span already has the spare, and one as wide as its span
 * is allocated more.  Require only what the narrower case needs, so a valid
 * object is never refused.
 */
static bool m9_surface_min_bytes(u32 span, u32 height, u32 cpp, u64 *size)
{
	bool small = span <= 64 && height <= 64;
	u32 height_quantum = small ? (cpp == 4 ? 8 : 4) : 32;
	u32 byte_quantum = cpp == 4 ? (small ? 0x400 : 0x1000) :
				      (small ? 0x200 : 0x800);
	u32 alloc_height;

	if (!span || !height || span > M9_SURFACE_MAX_SPAN ||
	    height > M9_SURFACE_MAX_SPAN || (cpp != 2 && cpp != 4))
		return false;
	alloc_height = ALIGN(height, height_quantum);
	if (span <= 16 && height <= 16)
		alloc_height = 16;
	*size = ALIGN((u64)span * alloc_height * cpp, (u64)byte_quantum);
	return true;
}

static int m9_surface_check(const struct drm_parhelia_m9_surface_access *args,
			    struct m9_surface_view *view)
{
	bool texture = args->flags & PARHELIA_M9_ACCESS_TEXTURE;
	u32 d0 = args->descriptor0, d1 = args->descriptor1, end;
	u64 need;

	view->captured_cpu_view = false;
	view->lock_cpu_view = false;

	if ((args->flags & ~PARHELIA_M9_ACCESS_TEXTURE) ||
	    (!texture && args->offset) ||
	    (args->op != PARHELIA_M9_ACCESS_READ &&
	     args->op != PARHELIA_M9_ACCESS_WRITE) ||
	    !args->width || !args->height || !args->surface_width)
		return -EINVAL;
	view->span = d0 & 0xffff;
	view->height = d1 & 0xffff;
	if ((d1 & 0xf0000000) || ((d1 >> 16) & 0xfff) != 1 || !view->height ||
	    view->height > M9_SURFACE_MAX_SPAN || !view->span ||
	    view->span > M9_SURFACE_MAX_SPAN)
		return -EINVAL;
	view->elements = view->span;
	view->rows = view->height;
	if (texture && m9_compressed_block_bytes(d0 >> 24)) {
		/*
		 * An S3TC level is addressed by whole blocks: the descriptor
		 * still counts texels, but the window's linear view is the
		 * block image, so x/y/width/height come in blocks.  This is
		 * the probe path for the compressed tiling; nothing in the
		 * driver stack uses it yet.
		 */
		view->cpp = m9_compressed_block_bytes(d0 >> 24);
		view->elements = view->span / 4;
		view->rows = view->height / 4;
		u8 kind = (d0 >> 16) & 0xff;

		if (!IS_ALIGNED(args->offset, 0x100) ||
		    view->span != args->surface_width ||
		    !IS_ALIGNED(view->height, 4) ||
		    (kind == M9_TEXTURE_KIND_SMALL ?
		     view->span > 64 || view->height > 64 :
		     kind != M9_TEXTURE_KIND_LARGE ||
		     !IS_ALIGNED(view->span, 32)))
			return -EINVAL;
		view->bytes = (u64)view->elements * view->rows * view->cpp;
		view->bytes = ALIGN(view->bytes, 0x1000);
	} else if (texture) {
		u8 kind = (d0 >> 16) & 0xff;

		/* Texture levels: captured format bytes and storage kinds only. */
		view->cpp = m9_texture_cpp(d0 >> 24);
		if (!view->cpp || !IS_ALIGNED(args->offset, 0x100) ||
		    view->span != args->surface_width ||
		    (kind == M9_TEXTURE_KIND_SMALL ?
		     view->span > 64 || view->height > 64 :
		     kind != M9_TEXTURE_KIND_LARGE ||
		     !IS_ALIGNED(view->span, 32)))
			return -EINVAL;
		/*
		 * A level is allocated by the same contract as a render
		 * surface, so size it that way.  The small kind quantises its
		 * height by eight, and demanding a multiple of 32 refused
		 * every short level: a 64x4 level is allocated 2 KiB and this
		 * asked for 8 KiB, so the level could never be stored and the
		 * texture sampled as black.
		 */
		if (!m9_surface_min_bytes(view->span, view->height,
					  view->cpp, &view->bytes))
			view->bytes = max_t(u64, (u64)ALIGN(view->span, 32) *
					    ALIGN(view->height, 32), 32 * 16) *
				      view->cpp;
	} else {
		/*
		 * Render targets: the verified 21 family at bit 21, with a
		 * packed 32-bit colour code -- 0x66 A8R8G8B8 and the two
		 * 10-bit GigaColor codes, which share its storage class.
		 */
		view->cpp = 4;
		bool faa_probe = m9x_faa_diag_enabled() && d0 == 0xcc220140 &&
			args->surface_width == 320 && view->height == 240;
		/* Actual 2cb40 -> 27170 -> 26830 CPU mapper, selector 1:
		 * the sampled CPU format differs from engine B1, and its
		 * extent includes the allocation's 256 rows. */
		view->captured_cpu_view = m9x_faa_diag_enabled() &&
			d0 == 0xf0220140 && d1 == 0x00010100 &&
			args->surface_width == 320 && args->width == 320 &&
			args->height == 240 && !args->x && !args->y;
		/* Actual returned normal-Lock view is ordinary kind21 even
		 * though its retained backing is the sampled primary. Only a
		 * read of the retired, source-qualified borrowed owner may use it. */
		view->lock_cpu_view = (m9x_faa_diag_enabled() ||
			m9x_faa_graph_profile_enabled()) &&
			((d0 == 0xf0210140 && d1 == 0x00010100 &&
              args->surface_width == 320 && args->width == 320 && args->height == 240) ||
             (d0 == 0xf0210400 && d1 == 0x00010300 &&
              args->surface_width == 1024 && args->width == 1024 && args->height == 768)) &&
			args->op == PARHELIA_M9_ACCESS_READ &&
			!args->x && !args->y && args->data_stride == args->width * 4;
		view->captured_cpu_view |= view->lock_cpu_view;

		if ((!faa_probe && !view->captured_cpu_view &&
		     (d0 & ~M9_SURFACE_CODE_MASK & 0xffff0000) !=
		     (M9_SURFACE_FAMILY & ~M9_SURFACE_CODE_MASK & 0xffff0000)) ||
		    (!view->captured_cpu_view &&
		     !m9_surface_code_is_packed32(d0 >> 25)) ||
		    view->span != ALIGN(args->surface_width, 32))
			return -EINVAL;
		/* The small family allocates with other height and byte quanta;
		 * m9_surface_layout_bytes below sizes the object from that
		 * contract, and the window extent stays the linear view. */
		view->bytes = (u64)view->span * 4 * view->height;
	}
	/* A render target keeps its declared width as the bound; only a
	 * texture's addressing units are the view's elements, which for a
	 * compressed level are blocks rather than texels. */
	if (check_add_overflow(args->x, args->width, &end) ||
	    end > (texture ? view->elements : args->surface_width) ||
	    check_add_overflow(args->y, args->height, &end) ||
	    end > (texture ? view->rows : view->height) ||
	    view->bytes > M9_ADDRESS_SURFACE_BYTES)
		return -EINVAL;
	need = (u64)(args->height - 1) * args->data_stride +
	       (u64)args->width * view->cpp;
	if (args->data_stride < (u64)args->width * view->cpp ||
	    need > args->data_size)
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

/* Both rings and the stream image, allocated once and kept until detach. */
static int m9_engine_read_staging_locked(struct m9x_device *mdev)
{
	struct m9x_engine_read_staging *s = &mdev->engine_read;
	struct device *dev = &mdev->pdev->dev;
	u32 bytes, i;

	lockdep_assert_held(&mdev->transport.lock);
	if (s->words)
		return 0;
	if (s->failed)
		return -ENOMEM;
	s->failed = true;
	bytes = min_t(u64, (u64)m9_surface_engine_ring_kib * SZ_1K,
		      M9_ENGINE_READ_MAX_RING);
	bytes = PAGE_ALIGN(max_t(u32, bytes, M9_ENGINE_READ_MIN_RING));
	for (i = 0; i < M9X_ENGINE_READ_RINGS; i++) {
		s->cpu[i] = dmam_alloc_coherent(dev, bytes, &s->bus[i],
						GFP_KERNEL);
		if (!s->cpu[i])
			goto fail;
		/* b6 takes a 32-bit bus address with the tag in its low bits. */
		if (upper_32_bits(s->bus[i] + bytes - 1)) {
			dmam_free_coherent(dev, bytes, s->cpu[i], s->bus[i]);
			s->cpu[i] = NULL;
			goto fail;
		}
	}
	s->capacity = PARHELIA_M9_COMMAND_BYTES / sizeof(u32) / 2;
	s->max_chunks = s->capacity / M9X_ENGINE_READ_CHUNK_WORDS;
	s->chunks = devm_kcalloc(dev, s->max_chunks, sizeof(*s->chunks),
				 GFP_KERNEL);
	s->words = devm_kcalloc(dev, s->capacity, sizeof(*s->words),
				GFP_KERNEL);
	if (!s->chunks || !s->words)
		goto fail;
	s->bytes = bytes;
	s->failed = false;
	dev_info(dev, "M9 engine readback rings: %#llx %#llx, %u bytes each\n",
		 (u64)s->bus[0], (u64)s->bus[1], bytes);
	return 0;
fail:
	for (i = 0; i < M9X_ENGINE_READ_RINGS; i++)
		if (s->cpu[i])
			dmam_free_coherent(dev, bytes, s->cpu[i], s->bus[i]);
	if (s->chunks)
		devm_kfree(dev, s->chunks);
	if (s->words)
		devm_kfree(dev, s->words);
	memset(s->cpu, 0, sizeof(s->cpu));
	s->chunks = NULL;
	s->words = NULL;
	return -ENOMEM;
}

static bool m9_engine_read_admitted_locked(struct m9x_device *mdev)
{
	struct m9x_transport *t = &mdev->transport;

	/* Only the Linux ordinary context on the staging slots, idle. */
	return !t->wedged && !t->faa && !t->faa_retiring &&
	       !t->faa_virtual_primary && !t->faa_source_context &&
	       t->bus_master_enabled && t->transport_armed &&
	       !t->queue_count && !t->async_pending &&
	       m9x_triangle_context_ready_locked(mdev) &&
	       !m9x_faa_ordinary_blocked_locked(mdev) &&
	       !m9x_faa_graph_preparing(mdev);
}

/*
 * Copy a render-target box to @kbuf through the vendor DdBlt video->system
 * engine copy.  -EOPNOTSUPP means nothing was posted: use the window.
 */
static int m9_surface_engine_read_locked(struct m9x_device *mdev, s64 offset,
					 const struct drm_parhelia_m9_surface_access *args,
					 const struct m9_surface_view *view,
					 u8 *kbuf, size_t row_bytes)
{
	struct m9x_engine_read_staging *s = &mdev->engine_read;
	struct m9x_surface_read_stats *st = &mdev->surface_stats;
	struct m9x_transport *t = &mdev->transport;
	struct m9x_engine_read_request req = {
		.descriptor0 = args->descriptor0,
		.descriptor1 = args->descriptor1,
		.x = args->x,
		.width = args->width,
		.fence_bus = lower_32_bits(t->fence_bus),
		.state = m9_surface_engine_read == 1,
	};
	struct m9x_engine_read_plan plan;
	u64 start = ktime_get_ns(), gpu, batches = 0, chunks = 0;
	u32 row = 0, i, r;
	bool posted;
	int ret;

	lockdep_assert_held(&t->lock);
	if (!m9_engine_read_admitted_locked(mdev)) {
		ret = -EBUSY;
		goto refuse;
	}
	gpu = M9_VRAM_GPU_BASE + offset;
	if (offset < 0 || offset + view->bytes > m9x_vram_gpu_limit(mdev) ||
	    upper_32_bits(gpu) || upper_32_bits(t->fence_bus)) {
		ret = -ERANGE;
		goto refuse;
	}
	ret = m9_engine_read_staging_locked(mdev);
	if (ret)
		goto refuse;
	req.source = lower_32_bits(gpu);
	for (i = 0; i < M9X_ENGINE_READ_RINGS; i++)
		req.ring[i] = (struct m9x_engine_read_ring) {
			.bus = lower_32_bits(s->bus[i]), .bytes = s->bytes,
		};
	while (row < args->height) {
		req.y = args->y + row;
		req.height = args->height - row;
		req.token = t->next_fence;
		ret = m9x_engine_read_encode(&req, s->words, s->capacity,
					     s->chunks, s->max_chunks, &plan);
		if (ret)
			goto refuse;
		posted = false;
		ret = m9x_submit_kernel_stream_locked(mdev, s->words,
						      plan.dwords, plan.final,
						      NULL, &posted, false);
		if (ret && !posted && !t->wedged)
			goto refuse;
		if (ret)
			return ret;
		t->next_fence = plan.final + 0x10;
		if (!st->engine_reads && !batches)
			dev_info(&mdev->pdev->dev,
				 "M9 engine readback: first batch retired, %u dwords %u chunks pitch %u final %#x\n",
				 plan.dwords, plan.chunks, plan.pitch,
				 plan.final);
		for (i = 0; i < plan.chunks; i++) {
			const struct m9x_engine_read_chunk *c = &s->chunks[i];
			const u8 *src = (const u8 *)s->cpu[c->ring] + c->offset;

			for (r = 0; r < c->rows; r++)
				memcpy(kbuf + (size_t)(row + c->row + r) * row_bytes,
				       src + (size_t)r * plan.pitch, row_bytes);
		}
		row += plan.rows;
		batches++;
		chunks += plan.chunks;
	}
	st->engine_reads++;
	st->engine_bytes += (u64)args->height * row_bytes;
	st->engine_batches += batches;
	st->engine_chunks += chunks;
	st->engine_ns += ktime_get_ns() - start;
	return 0;
refuse:
	st->refused++;
	st->last_refusal = ret;
	dev_info_ratelimited(&mdev->pdev->dev,
			     "M9 engine readback refused (%d), window read\n", ret);
	return -EOPNOTSUPP;
}

static int m9_surface_access(struct m9x_device *mdev, struct drm_file *file,
			     struct drm_parhelia_m9_surface_access *args)
{
	bool write = args->op == PARHELIA_M9_ACCESS_WRITE;
	bool texture = args->flags & PARHELIA_M9_ACCESS_TEXTURE;
	bool engine;
	struct drm_gem_vram_object *vram;
	struct m9_surface_view view;
	struct drm_gem_object *gem;
	size_t row_bytes;
	u64 min_size;
	s64 offset;
	long wait;
	u32 row;
	u8 *kbuf;
	int ret;

	if (args->flags & PARHELIA_M9_ACCESS_FAA_OUTPUT)
		return m9x_faa_output_read(mdev, file, args);
	ret = m9_surface_check(args, &view);
	if (ret)
		return ret;
	if (!mdev->bar0_window)
		return -ENODEV;
	row_bytes = (size_t)args->width * view.cpp;
	engine = m9_surface_engine_read && !write && !texture &&
		 !view.captured_cpu_view &&
		 (u64)args->height * row_bytes >= m9_surface_engine_min_bytes;
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
	if (texture)
		min_size = (u64)args->offset + view.bytes;
	else if (view.captured_cpu_view)
		/* The selector window is independently bounded below; native Lock
		 * admission checks the complete retained backing allocation. */
		min_size = view.bytes;
	else if (!m9x_surface_layout_bytes(args->surface_width, view.height, 4,
					  &min_size))
		min_size = U64_MAX;
	if (!m9x_gem_is_vram(gem) || (!texture && !m9x_gem_vram_tiled(gem)) ||
	    gem->size < min_size) {
		ret = -EINVAL;
		goto out_put;
	}
	if (view.captured_cpu_view && !view.lock_cpu_view) {
		/* Refuse before preparation can touch the active Lock sequence. */
		mutex_lock(&mdev->transport.lock);
		ret = m9x_faa_direct_view_admit_locked(mdev);
		mutex_unlock(&mdev->transport.lock);
		if (ret)
			goto out_put;
		/* The tagged vendor host map needs the compact identity manager.
		 * Prepare and activate it before reserving the caller's color BO;
		 * CPU mapping may precede the first diagnostic submission. */
		ret = m9x_faa_prepare_diag(mdev);
		if (ret && ret != -EALREADY)
			goto out_put;
		mutex_lock(&mdev->transport.lock);
		ret = m9x_faa_direct_view_admit_locked(mdev);
		if (!ret) {
			m9x_transport_drain_locked(mdev);
			ret = m9x_engine_ready_locked(mdev);
		}
		mutex_unlock(&mdev->transport.lock);
		if (ret)
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
	/* Submission can start between preflight and the BO reservation.
	 * Recheck before either draining or changing the global CPU view. */
	if (view.captured_cpu_view && !view.lock_cpu_view) {
		ret = m9x_faa_direct_view_admit_locked(mdev);
		if (ret)
			goto out_transport;
	}
	if (!view.lock_cpu_view)
		m9x_transport_drain_locked(mdev);
	if (mdev->transport.wedged)
		ret = -EIO;
	else if (engine)
		ret = m9_surface_engine_read_locked(mdev, offset, args, &view,
						    kbuf, row_bytes);
	else
		ret = -EOPNOTSUPP;
	/* The engine either retired the copy or failed after posting it. */
	if (ret == -EOPNOTSUPP) {
		u32 surface = lower_32_bits(offset + args->offset);
		u64 start = ktime_get_ns();

		ret = 0;
		if (view.captured_cpu_view) {
			if (view.lock_cpu_view) {
				ret = m9x_faa_lock_view_admit_locked(mdev, vram,
							 surface, view.bytes);
				if (ret)
					goto out_transport;
				/* Actual 2cba8 after the original write wait. */
				m9x_write(mdev, M9_REG_FRONTEND_KICK, 1);
			}
			/* The owned new color range needs its secondary mapping
			 * before the full identity contract can be checked. */
			ret = m9x_address_cpu_local_prepare(mdev, surface, view.bytes);
			if (ret)
				goto out_transport;
			surface |= 1;
		}
		if (view.lock_cpu_view && m9x_faa_graph_profile_enabled()) {
			ret = m9x_address_surface_window_bounded(mdev, surface,
				args->descriptor0, args->descriptor1, view.bytes);
			if (ret)
				goto out_transport;
		} else {
			m9x_address_surface_window(mdev, surface,
				args->descriptor0, args->descriptor1);
		}
		for (row = 0; row < args->height; row++) {
			void __iomem *rows = (u8 __iomem *)mdev->bar0_window +
				((u64)(args->y + row) * view.elements +
				 args->x) * view.cpp;

			if (write)
				memcpy_toio(rows, kbuf + row * row_bytes, row_bytes);
			else
				memcpy_fromio(kbuf + row * row_bytes, rows, row_bytes);
		}
		/* Complete WC stores before the window is released. */
		if (write)
			wmb();
		m9x_address_surface_release(mdev);
		if (!write) {
			mdev->surface_stats.window_reads++;
			mdev->surface_stats.window_bytes +=
				(u64)args->height * row_bytes;
			mdev->surface_stats.window_ns += ktime_get_ns() - start;
		}
	}
out_transport:
	mutex_unlock(&mdev->transport.lock);
out_unreserve:
	ttm_bo_unreserve(&vram->bo);
out_unpin:
	m9x_vram_unpin(vram);
	/* User memory may fault through TTM. Copy after releasing the BO, then
	 * reacquire it to validate the same retained view before publishing Lock. */
	for (row = 0; !ret && !write && row < args->height; row++)
		if (copy_to_user(u64_to_user_ptr(args->data_ptr +
						 (u64)row * args->data_stride),
				 kbuf + row * row_bytes, row_bytes))
			ret = -EFAULT;
	if (!ret && view.lock_cpu_view) {
		ret = ttm_bo_reserve(&vram->bo, true, false, NULL);
		if (!ret) {
			offset = drm_gem_vram_offset(vram);
			mutex_lock(&mdev->transport.lock);
			ret = offset < 0 ? offset :
				m9x_faa_lock_view_admit_locked(mdev, vram,
					lower_32_bits(offset + args->offset), view.bytes);
			if (!ret)
				ret = m9x_faa_lock_view_observed_locked(mdev, vram);
			mutex_unlock(&mdev->transport.lock);
			ttm_bo_unreserve(&vram->bo);
		}
	}
out_put:
	drm_gem_object_put(gem);
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

int m9x_surface_stats_show(struct m9x_device *mdev, struct seq_file *m)
{
	struct m9x_surface_read_stats st;
	u32 ring;

	mutex_lock(&mdev->transport.lock);
	st = mdev->surface_stats;
	ring = mdev->engine_read.words ? mdev->engine_read.bytes : 0;
	mutex_unlock(&mdev->transport.lock);
	seq_printf(m, "mode=%u min_bytes=%u ring_bytes=%u\n",
		   m9_surface_engine_read, m9_surface_engine_min_bytes, ring);
	seq_printf(m, "engine reads=%llu bytes=%llu batches=%llu chunks=%llu ns=%llu\n",
		   st.engine_reads, st.engine_bytes, st.engine_batches,
		   st.engine_chunks, st.engine_ns);
	seq_printf(m, "window reads=%llu bytes=%llu ns=%llu\n",
		   st.window_reads, st.window_bytes, st.window_ns);
	seq_printf(m, "refused=%llu last=%d\n", st.refused, st.last_refusal);
	return 0;
}

/* The encoder against a 320x240 target and its refusals. */
static int m9_engine_read_selftest(void)
{
	struct m9x_engine_read_request req = {
		.source = 0x00f01000, .descriptor0 = 0xcc210140,
		.descriptor1 = 0x000100f0, .width = 320, .height = 240,
		.fence_bus = 0x18d51000, .token = 0x30, .state = true,
		.ring = { { 0x01001000, SZ_2M }, { 0x01202000, SZ_2M } },
	};
	u32 capacity = PARHELIA_M9_COMMAND_BYTES / sizeof(u32) / 2, *w, at;
	struct m9x_engine_read_chunk *chunks;
	struct m9x_engine_read_plan plan;
	int ret = -EINVAL;

	w = kcalloc(capacity, sizeof(*w), GFP_KERNEL);
	chunks = kcalloc(capacity, sizeof(*chunks), GFP_KERNEL);
	if (!w || !chunks) {
		ret = -ENOMEM;
		goto out;
	}
	if (m9x_engine_read_encode(&req, w, capacity, chunks, capacity, &plan) ||
	    plan.chunks != 8 || plan.rows != 240 || plan.pitch != 1280 ||
	    plan.final != 0xe0 || w[0] != 0x4545451b || w[1] != 0x18d51002 ||
	    w[plan.dwords - 1] != 0xe1)
		goto out;
	/* Copy setup follows the bank pair and the two serialising tokens. */
	at = plan.dwords - 3 - 7 - 4 - 8 * M9X_ENGINE_READ_CHUNK_WORDS - 8;
	if (w[at] != 0x7a7978f0 || w[at + 1] != 0x280c1c0c ||
	    w[at + 2] != 0x00f01001 || w[at + 3] != 0xf0210140 ||
	    w[at + 4] != 0x000100f0 || w[at + 6] != 0 ||
	    w[at + 7] != 0xf0000140 || w[at + 9] != 0x01001002 ||
	    w[at + 16] != 0x01202002 || w[at + 23] != 0x0100b002 ||
	    w[at + 24] != 0x00400000 || w[at + 14] != 0x61 ||
	    w[at + 8 + 7 * 7 + 3] != 0x00100140)
		goto out;
	req.y = 200;
	req.height = 40;
	req.state = false;
	if (m9x_engine_read_encode(&req, w, capacity, chunks, capacity, &plan) ||
	    plan.chunks != 2 || chunks[1].row != 32 || chunks[1].rows != 8 ||
	    plan.dwords != 31 + 2 * M9X_ENGINE_READ_CHUNK_WORDS)
		goto out;
	req.descriptor0 = 0xd0210140;
	if (m9x_engine_read_encode(&req, w, capacity, chunks, capacity,
				   &plan) != -EINVAL)
		goto out;
	req.descriptor0 = 0xcc210140;
	req.ring[1].bus |= 0x40;
	if (m9x_engine_read_encode(&req, w, capacity, chunks, capacity,
				   &plan) != -EINVAL)
		goto out;
	req.ring[1].bus &= ~0x40u;
	if (m9x_engine_read_encode(&req, w, 31 + 6, chunks, capacity,
				   &plan) != -ENOSPC)
		goto out;
	req.token = 0xffffffc0;
	if (m9x_engine_read_encode(&req, w, capacity, chunks, capacity,
				   &plan) != -ERANGE)
		goto out;
	ret = 0;
out:
	kfree(chunks);
	kfree(w);
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
	struct m9_surface_view view;
	u64 size;

	if (m9_surface_check(&args, &view) || view.span != 320 ||
	    view.height != 240 || view.cpp != 4)
		return -EINVAL;
	args.descriptor0 = 0xcc210400;
	args.descriptor1 = 0x00010300;
	args.surface_width = 1024;
	args.width = 1024;
	args.height = 768;
	args.data_stride = 4096;
	args.data_size = 4096 * 768;
	if (m9_surface_check(&args, &view))
		return -EINVAL;
	args.data_size--;
	if (m9_surface_check(&args, &view) != -EINVAL)
		return -EINVAL;
	args.data_size++;
	args.descriptor0 = 0xc8010020;
	if (m9_surface_check(&args, &view) != -EINVAL)
		return -EINVAL;
	args.descriptor0 = 0xf0210400;
	if (m9x_faa_diag_enabled() || m9x_faa_graph_profile_enabled()) {
		if (m9_surface_check(&args, &view) || !view.lock_cpu_view ||
		    view.bytes != 0x300000 || view.height != 768)
			return -EINVAL;
	} else if (m9_surface_check(&args, &view) != -EINVAL)
		return -EINVAL;
	/* The two 10-bit GigaColor codes share the 32-bit storage class. */
	args.descriptor0 = 0xd0210400;
	if (m9_surface_check(&args, &view) || view.cpp != 4)
		return -EINVAL;
	args.descriptor0 = 0xd4210400;
	if (m9_surface_check(&args, &view) || view.cpp != 4)
		return -EINVAL;
	args.descriptor0 = 0xcc210400;
	args.surface_width = 900;
	if (m9_surface_check(&args, &view) != -EINVAL)
		return -EINVAL;
	/* A 64x64 RGB565 texture level at a 256-aligned offset. */
	args.flags = PARHELIA_M9_ACCESS_TEXTURE;
	args.descriptor0 = 0x9d010040;
	args.descriptor1 = 0x00010040;
	args.surface_width = 64;
	args.width = 64;
	args.height = 64;
	args.data_stride = 128;
	args.data_size = 128 * 64;
	args.offset = 0x800;
	if (m9_surface_check(&args, &view) || view.cpp != 2)
		return -EINVAL;
	args.offset = 0x801;
	if (m9_surface_check(&args, &view) != -EINVAL)
		return -EINVAL;
	args.offset = 0x800;
	args.descriptor0 = 0x9d210040;
	if (m9_surface_check(&args, &view))
		return -EINVAL;
	args.descriptor0 = 0x77010040;
	if (m9_surface_check(&args, &view) != -EINVAL)
		return -EINVAL;
	if (!m9x_surface_layout_bytes(320, 240, 4, &size) || size != 0x50000 ||
	    !m9x_surface_layout_bytes(1024, 768, 4, &size) || size != 0x301000)
		return -EINVAL;
	return m9_engine_read_selftest();
}
