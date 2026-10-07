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
#include <linux/vmalloc.h>

#ifdef CONFIG_X86_64
#include <asm/memtype.h>
#include <asm/set_memory.h>
#endif
#include <linux/uaccess.h>

#include <drm/drm_file.h>

#include "m9_drv.h"
#include "m9_reg.h"
#include "m9_faa_output.h"

struct m9_faa_owner_spec {
	u32 captured, bytes;
	bool local;
	/* Per-owner domain/policy; historical DMA-physical owners stay false.
	 * Only a source-closed generated output template may select these. */
	bool system, flags30;
	u32 mapped_bytes;
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
struct m9_faa_clear_segment {
	u32 owner, bytes, start, end;
};
#if (defined(M9_FAA_CONSTANT_SHADER_CONTROL) + \
     defined(M9_FAA_CONSTANT_RED_CONTROL) + \
     defined(M9_FAA_ORDINARY_SYSTEM_CONTROL) + \
     defined(M9_FAA_INIT_ONLY_CONTROL) + \
     defined(M9_FAA_PRECLEAR_CONTROL) + \
     defined(M9_FAA_PRECLEAR_DRAW_CONTROL) + \
     defined(M9_FAA_PRECLEAR_OUTPUT_CONTROL) + \
     defined(M9_FAA_PRECLEAR_OUTPUT_WRITE_CONTROL) + \
     defined(M9_FAA_POST_TRIANGLE_CONTROL)) > 1
#error "Select only one complete private FAA source profile"
#endif

#if defined(M9_FAA_ORDINARY_SYSTEM_CONTROL)
/* Whole ordinary public draw into its own SYSTEM owner. No sampled caller
 * color, FAA activation, companion clear or resolve is present in this flow. */
#include "m9_ordinary_system_red_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/ordinary-system-constant-red-known-inputs.bin"
#elif defined(M9_FAA_INIT_ONLY_CONTROL)
/* Independent vendor queue finish at the returned context constructor.
 * No triangle or output request, and no invented output owner. */
#include "m9_faa_init_only_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-init-only-known-inputs.bin"
#elif defined(M9_FAA_PRECLEAR_CONTROL)
/* Completed public black CLEAR42, then the original draw stopped at its
 * first GPU query. Independent finish; no completed triangle or output. */
#include "m9_faa_preclear_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-preclear-known-inputs.bin"
#elif defined(M9_FAA_PRECLEAR_DRAW_CONTROL)
/* Completed public black clear, then the original triangle preprocessing
 * launch stopped at its next GPU wait. Earlier inputs remain byte-exact. */
#include "m9_faa_preclear_draw_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-preclear-draw-known-inputs.bin"
#elif defined(M9_FAA_PRECLEAR_OUTPUT_CONTROL)
/* Returned public triangle, then the genuine output format transition
 * stopped at its next GPU wait. The output exists but is not readable. */
#include "m9_faa_preclear_output_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-preclear-output-known-inputs.bin"
#elif defined(M9_FAA_PRECLEAR_OUTPUT_WRITE_CONTROL)
/* The original output path posted its complete resolve before waiting
 * for CPU access. Preserve that posted prefix and its genuine footer. */
#include "m9_faa_preclear_output_write_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-preclear-output-write-known-inputs.bin"
#elif defined(M9_FAA_POST_TRIANGLE_CONTROL)
/* Independent vendor queue finish after the returned public triangle,
 * before output/resolve. Its final counter is in its counted secondary. */
#include "m9_faa_post_triangle_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-post-triangle-known-inputs.bin"
#elif defined(M9_FAA_CONSTANT_RED_CONTROL)
/* A distinct whole public source capture changes c0 through SETCONST.
 * Never patch captured green command or program bytes in place. */
#include "m9_faa_constant_red_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-constant-red-known-inputs.bin"
#elif defined(M9_FAA_CONSTANT_SHADER_CONTROL)
/* Whole independently captured public CREATEPS/SETCONST/SETPS flow. The
 * input blob and all owner/fixup tables are one source profile; never splice
 * shader bytes or captured secondary lengths into the normal profile. */
#include "m9_faa_constant_green_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-constant-green-known-inputs.bin"
#else
#include "m9_faa_native_data.h"
#define M9_FAA_INPUT_FIRMWARE "m9x/faa-normal-known-inputs.bin"
#endif
#ifndef M9_FAA_OUTPUT_PENDING
#define M9_FAA_OUTPUT_PENDING 0
#endif
/* The older sampled-destination template has no ordinary output owner.
 * It must never be advertised as a completed ac1f0 output contract. */
#ifndef M9_FAA_OUTPUT_OWNER
#define M9_FAA_OUTPUT_OWNER U32_MAX
#endif
#ifndef M9_FAA_COUNTER_VIRTUAL
#define M9_FAA_COUNTER_VIRTUAL 0
#endif

struct m9_faa_owner {
	struct drm_gem_vram_object *vram;
	struct iosys_map map;
	void *cpu;
	dma_addr_t dma;
	u32 base, bytes;
	/* True from the first attempted attribute change until verified WB
	 * restoration. A failed/partial CPA change still needs restoration. */
	bool cpu_wc;
	struct drm_mm_node system_node;
};
struct m9x_faa_replay {
	struct m9_faa_owner owner[ARRAY_SIZE(m9_faa_owner_specs)];
	bool prepared, attempted, baseline_valid;
	u32 primary_bytes;
	/* Checked extent of the caller's pinned/reserved color BO. This is
	 * borrowed storage, never a locally mapped owner to release. */
	u64 borrowed_color_bytes;
	bool borrowed_color_observed;
	struct drm_gem_object *output_color;
	u64 output_point;
	u32 output_generation;
	bool output_retired;
	/* An alias of the transport's existing completion page, never a new
	 * completion allocation or an independently seeded success counter. */
	struct drm_mm_node counter_node;
	u32 before_hash[ARRAY_SIZE(m9_faa_owner_specs)];
	u32 before_nonzero[ARRAY_SIZE(m9_faa_owner_specs)];
	u32 before_nonzero_words[ARRAY_SIZE(m9_faa_owner_specs)];
};

static bool m9_faa_diag;
module_param(m9_faa_diag, bool, 0400);
MODULE_PARM_DESC(m9_faa_diag,
	"Use the kernel-owned normal FAA frame for one FIRST_TRIANGLE diagnostic (M9128 only)");
static bool m9_faa_source_init;
module_param(m9_faa_source_init, bool, 0400);
MODULE_PARM_DESC(m9_faa_source_init,
	"Let the private M9128 Windows FAA stream initialize a fresh context without Linux sequence0");
static bool m9_faa_defer_output_map;
module_param(m9_faa_defer_output_map, bool, 0400);
MODULE_PARM_DESC(m9_faa_defer_output_map,
	"Publish only the private output SYSTEM mapping after source120 retirement");
static bool m9_faa_program_access_kick;
module_param(m9_faa_program_access_kick, bool, 0400);
MODULE_PARM_DESC(m9_faa_program_access_kick,
	"Preserve the private C2 CPU-access frontend kick after actual c0 retirement");
#ifdef M9_FAA_PRECLEAR_OUTPUT_WRITE_CONTROL
/* Same-module hardware control: collapsing these waits leaves main/AA
 * stores untouched; preserving them produces GPU state. Resolve is still
 * unqualified. Keep the opt-out for this bounded ordering control. */
static bool m9_faa_phased_submit = true;
#else
static bool m9_faa_phased_submit;
#endif
module_param(m9_faa_phased_submit, bool, 0400);
MODULE_PARM_DESC(m9_faa_phased_submit,
	"Wait at source c0/120/160/180 boundaries in the private complete-output diagnostic");
MODULE_FIRMWARE(M9_FAA_INPUT_FIRMWARE);

bool m9x_faa_diag_enabled(void)
{
	return m9_faa_diag;
}

bool m9x_faa_source_init_enabled(struct m9x_device *mdev)
{
	return m9_faa_diag && m9_faa_source_init && mdev->pdev->device == 0x0540;
}

/* The unchanged native origin10 is an initial CPU counter seed. Fresh
 * source admission must never call it a previous GPU retirement. */
int m9x_faa_source_init_admit_locked(struct m9x_device *mdev,
				    bool context_initialized)
{
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r = t->faa;

	lockdep_assert_held(&t->lock);
	if (!m9x_faa_source_init_enabled(mdev) || !r || !r->prepared ||
	    !t->fence_cpu || t->wedged || context_initialized)
		return -EINVAL;
	/* Repeated CPU-view preparation may reuse the admitted manager. It
	 * cannot start a second frame: the caller's attempted gate remains. */
	if (t->faa_source_context && r->attempted)
		return t->engine_transitioned && t->restore_ready &&
		       t->transport_armed && t->bus_master_enabled &&
		       !t->queue_count && !t->async_pending ? 0 : -EINVAL;
	if (r->attempted || t->faa_virtual_primary || t->submitted_point ||
	    t->completed_point || t->queue_count || t->async_pending ||
	    t->command_slot || t->command_offset || t->fence_epoch ||
	    t->last_fence || t->next_fence != 0x20 ||
	    READ_ONCE(((u32 *)t->fence_cpu)[0]) != 0x10 ||
	    READ_ONCE(((u32 *)t->fence_cpu)[1]) != 0x10)
		return -EBUSY;
	return 0;
}

static struct m9_faa_owner *m9_faa_output_owner(struct m9x_faa_replay *r)
{
	if (M9_FAA_OUTPUT_OWNER >= ARRAY_SIZE(r->owner))
		return NULL;
	return &r->owner[M9_FAA_OUTPUT_OWNER];
}

/* The captured flags30 raw-MDL owners all select MmWriteCombined through
 * real 23306c input word1=0. Linux coherent DMA may return a WB direct alias
 * even with DMA_ATTR_WRITE_COMBINE, so establish and verify that CPU contract
 * before publishing any of these private owners. Ordinary DMA and the shared
 * flags110 completion-page alias retain their existing cache attributes.
 */
static int m9_faa_dma_cache_check(struct m9_faa_owner *o, bool wc)
{
#ifdef CONFIG_X86_64
	unsigned long start = (unsigned long)o->cpu;
	unsigned long expected = cachemode2protval(wc ? _PAGE_CACHE_MODE_WC :
						     _PAGE_CACHE_MODE_WB);
	u32 offset;

	if (!pat_enabled() || !o->cpu || is_vmalloc_addr(o->cpu) ||
	    !virt_addr_valid(o->cpu) || !IS_ALIGNED(start, PAGE_SIZE) ||
	    !o->bytes || !IS_ALIGNED(o->bytes, PAGE_SIZE) ||
	    o->bytes > INT_MAX || start > ULONG_MAX - o->bytes)
		return -EOPNOTSUPP;
	for (offset = 0; offset < o->bytes; offset += PAGE_SIZE) {
		unsigned long address = start + offset;
		unsigned int level;
		pte_t *entry = lookup_address(address, &level);
		unsigned long prot;

		if (!virt_addr_valid((void *)address) || !entry ||
		    !pte_present(*entry) || level < PG_LEVEL_4K ||
		    level > PG_LEVEL_1G)
			return -EFAULT;
		prot = pte_val(*entry);
		if (level != PG_LEVEL_4K)
			prot = protval_large_2_4k(prot);
		if ((prot & _PAGE_CACHE_MASK) != expected)
			return -EUCLEAN;
	}
	return 0;
#else
	return -EOPNOTSUPP;
#endif
}

static int m9_faa_dma_prepare_wc(struct m9x_device *mdev,
				 struct m9_faa_owner *o)
{
#ifdef CONFIG_X86_64
	int ret = m9_faa_dma_cache_check(o, false);

	if (ret)
		return ret;
	/* CPA can fail after changing a subset of pages. Retain restoration
	 * ownership before entering it, including on its error paths. */
	o->cpu_wc = true;
	ret = set_memory_wc((unsigned long)o->cpu, o->bytes >> PAGE_SHIFT);
	if (!ret)
		ret = m9_faa_dma_cache_check(o, true);
	if (ret) {
		dev_err(&mdev->pdev->dev,
			"M9 FAA CPU WC preparation refused bytes=%x error=%d\n",
			o->bytes, ret);
		return ret;
	}
	dev_info(&mdev->pdev->dev,
		 "M9 FAA CPU cache verified WB->WC bytes=%x pages=%u before publication\n",
		 o->bytes, o->bytes >> PAGE_SHIFT);
	return 0;
#else
	return -EOPNOTSUPP;
#endif
}

static bool m9_faa_dma_restore_wb(struct m9_faa_owner *o)
{
	if (!o->cpu_wc)
		return true;
#ifdef CONFIG_X86_64
	if (set_memory_wb((unsigned long)o->cpu, o->bytes >> PAGE_SHIFT) ||
	    m9_faa_dma_cache_check(o, false))
		return false;
	o->cpu_wc = false;
	return true;
#else
	return false;
#endif
}

static bool m9_faa_release(struct m9x_device *mdev, struct m9x_faa_replay *r)
{
	u32 i;

	/* Failed preparation has no submitted FAA work. Attempted owners may
	 * change attributes only once requester teardown stopped bus mastering.
	 * Restore every changed allocation before freeing any owner, so a CPA
	 * failure cannot return WC or partly converted pages to the allocator.
	 */
	if (r->attempted && mdev->transport.bus_master_enabled)
		goto quarantine;
	for (i = 0; i < ARRAY_SIZE(r->owner); i++)
		if (!m9_faa_dma_restore_wb(&r->owner[i])) {
			dev_err(&mdev->pdev->dev,
				"M9 FAA CPU WB restore failed owner=%u bytes=%x; allocations retained\n",
				i, r->owner[i].bytes);
			goto quarantine;
		}

	if (drm_mm_node_allocated(&r->counter_node))
		m9x_address_unmap_system(mdev, &r->counter_node);
	for (i = 0; i < ARRAY_SIZE(r->owner); i++) {
		struct m9_faa_owner *o = &r->owner[i];

		if (drm_mm_node_allocated(&o->system_node))
			m9x_address_unmap_system(mdev, &o->system_node);
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
	if (r->output_color)
		drm_gem_object_put(r->output_color);
	kfree(r);
	return true;
quarantine:
	mdev->transport.wedged = true;
	mdev->transport.faa = r;
	dev_err(&mdev->pdev->dev,
		"M9 FAA CPU cache owners quarantined; no DMA allocation freed\n");
	return false;
}

static void m9_faa_release_failed_prepare(struct m9x_device *mdev,
					struct m9x_faa_replay *r)
{
	lockdep_assert_held(&mdev->transport.lock);
	if (mdev->transport.wedged) {
		/* Publication may already have exposed these DMA/local owners.
		 * Keep them through the freeze; requester teardown first disables
		 * PCI bus mastering, then m9x_faa_fini drops their ownership. */
		mdev->transport.faa = r;
		dev_err(&mdev->pdev->dev,
			"M9 FAA failed preparation retained until DMA shutdown\n");
		return;
	}
	m9_faa_release(mdev, r);
}

/* Called by requester teardown only after PCI bus mastering has stopped. */
void m9x_faa_fini(struct m9x_device *mdev)
{
	if (mdev->transport.faa) {
		/* Remove address nodes under their lock, but drop the caller's
		 * GEM reference outside it: its last release takes this lock. */
		struct m9x_faa_replay *r = mdev->transport.faa;
		u32 i;

		mutex_lock(&mdev->transport.lock);
		if (drm_mm_node_allocated(&r->counter_node))
			m9x_address_unmap_system(mdev, &r->counter_node);
		for (i = 0; i < ARRAY_SIZE(r->owner); i++)
			if (drm_mm_node_allocated(&r->owner[i].system_node))
				m9x_address_unmap_system(mdev, &r->owner[i].system_node);
		mutex_unlock(&mdev->transport.lock);
		if (m9_faa_release(mdev, r))
			mdev->transport.faa = NULL;
	}
}

static int m9_faa_allocate(struct m9x_device *mdev, struct m9_faa_owner *o,
			   const struct m9_faa_owner_spec *spec, bool defer_output)
{
	struct drm_gem_vram_object *vram;
	u64 end, base;
	s64 offset;
	int ret;

	o->bytes = PAGE_ALIGN(spec->bytes);
	if (!spec->local) {
		if ((spec->flags30 && !spec->system) ||
		    (spec->system && (!spec->mapped_bytes ||
		     !IS_ALIGNED(spec->mapped_bytes, PAGE_SIZE) ||
		     spec->mapped_bytes > o->bytes)))
			return -EINVAL;
		o->cpu = dma_alloc_coherent(&mdev->pdev->dev, o->bytes,
					    &o->dma, GFP_KERNEL);
		if (!o->cpu)
			return -ENOMEM;
		if (upper_32_bits(o->dma) || !IS_ALIGNED(o->dma, PAGE_SIZE) ||
		    (u64)o->dma + o->bytes > U32_MAX)
			return -ERANGE;
		o->base = lower_32_bits(o->dma);
		if (spec->system && spec->flags30) {
			ret = m9_faa_dma_prepare_wc(mdev, o);
			if (ret)
				return ret;
		}
		if (spec->system) {
			if (defer_output) {
				if (!spec->flags30 || spec->mapped_bytes != 0x50000)
					return -EINVAL;
				ret = m9x_address_reserve_faa_output(mdev,
						&o->system_node, spec->mapped_bytes);
			} else {
				ret = spec->flags30 ?
					m9x_address_map_faa_system(mdev, &o->system_node,
							  o->dma, spec->mapped_bytes) :
					m9x_address_map_system(mdev, &o->system_node,
							      o->dma, spec->mapped_bytes);
			}
			if (ret)
				return ret;
			o->base = o->system_node.start;
		}
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

/* FNV is a compact observational label; admission uses byte-exact equality. */
static u32 m9_faa_hash(u32 hash, const u8 *bytes, u32 count)
{
	u32 i;

	for (i = 0; i < count; i++)
		hash = (hash ^ bytes[i]) * 0x01000193;
	return hash;
}

/* Read only firmware-proven local spans. Unknown allocation tails remain
 * untouched and are not admitted as source bytes by this check. This checks
 * the upload through its BAR mapping; GPU identity leaves are checked later.
 */
static int m9_faa_verify_upload(struct m9x_device *mdev,
			      struct m9x_faa_replay *r,
			      const struct firmware *fw)
{
	u32 expected[ARRAY_SIZE(r->owner)] = { 0 };
	u32 observed[ARRAY_SIZE(r->owner)] = { 0 };
	u32 known[ARRAY_SIZE(r->owner)] = { 0 };
	u8 data[256];
	u32 i, offset, count, j;

	/* TTM mappings and admitted SYSTEM owners may be WC. dma_wmb()
	 * alone is only a compiler
	 * barrier on x86 and does not publish outstanding WC VRAM writes.
	 */
	wmb();
	for (i = 0; i < ARRAY_SIZE(m9_faa_spans); i++) {
		const struct m9_faa_span *s = &m9_faa_spans[i];
		struct m9_faa_owner *o = &r->owner[s->owner];

		if (!o->vram)
			continue;
		if (!known[s->owner]) {
			expected[s->owner] = 0x811c9dc5;
			observed[s->owner] = 0x811c9dc5;
		}
		for (offset = 0; offset < s->bytes; offset += count) {
			const u8 *source = fw->data + s->blob_offset + offset;

			count = min_t(u32, sizeof(data), s->bytes - offset);
			iosys_map_memcpy_from(data, &o->map, s->offset + offset, count);
			if (memcmp(data, source, count)) {
				for (j = 0; j < count; j++)
					if (data[j] != source[j])
						break;
				dev_err(&mdev->pdev->dev,
					"M9 FAA upload mismatch: owner=%u base=%#x offset=%#x expected=%#x observed=%#x\n",
					s->owner, o->base, s->offset + offset + j,
					source[j], data[j]);
				return -EIO;
			}
			expected[s->owner] = m9_faa_hash(expected[s->owner], source, count);
			observed[s->owner] = m9_faa_hash(observed[s->owner], data, count);
		}
		known[s->owner] += s->bytes;
	}
	for (i = 0; i < ARRAY_SIZE(r->owner); i++)
		if (known[i])
			dev_info(&mdev->pdev->dev,
				 "M9 FAA known upload verified: owner=%u base=%#x bytes=%u expected_fnv=%#x observed_fnv=%#x\n",
				 i, r->owner[i].base, known[i], expected[i], observed[i]);
	return 0;
}

/* Bounded observations before submit and after actual retirement. Includes
 * local stores and retained SYSTEM mapped prefixes, but never claims unknown
 * initial contents were zero or that a nonzero byte proves a GPU store.
 * Unmapped allocation tails are excluded. No allocation is written.
 */
static void m9_faa_observe(struct m9x_device *mdev,
			 struct m9x_faa_replay *r, bool before)
{
	const u32 budget = 16 * 1024 * 1024;
	u8 data[256];
	u32 i, offset, count, j, local_total = 0, system_total = 0;

	for (i = 0; i < ARRAY_SIZE(r->owner); i++) {
		const struct m9_faa_owner_spec *spec = &m9_faa_owner_specs[i];
		u32 bytes, *total;

		if (r->owner[i].vram) {
			bytes = spec->bytes;
			total = &local_total;
		} else if (spec->system && r->owner[i].cpu) {
			bytes = spec->mapped_bytes;
			total = &system_total;
		} else {
			continue;
		}
		if (bytes > r->owner[i].bytes || bytes > budget - *total) {
			dev_err(&mdev->pdev->dev, "M9 FAA observation exceeds %u-byte budget\n",
				budget);
			return;
		}
		*total += bytes;
	}
	/* Order publication/completion observations before BAR and DMA reads. */
	rmb();
	dma_rmb();
	if (mdev->transport.fence_cpu) {
		u32 *counter = mdev->transport.fence_cpu;

		/* Observe the same retained completion page selected by 106c
		 * and 1068/1070. Source query/map waits use +20/+00;
		 * observing +40/+60 does not assign them another wait role. */
		dev_info(&mdev->pdev->dev,
			 "M9 FAA %s counters: +00=%#x/%#x +20=%#x/%#x +40=%#x/%#x +60=%#x/%#x\n",
			 before ? "before submit" : "retired",
			 READ_ONCE(counter[0]), READ_ONCE(counter[1]),
			 READ_ONCE(counter[0x20 / 4]), READ_ONCE(counter[0x24 / 4]),
			 READ_ONCE(counter[0x40 / 4]), READ_ONCE(counter[0x44 / 4]),
			 READ_ONCE(counter[0x60 / 4]), READ_ONCE(counter[0x64 / 4]));
	}
	for (i = 0; i < ARRAY_SIZE(r->owner); i++) {
		struct m9_faa_owner *o = &r->owner[i];
		const struct m9_faa_owner_spec *spec = &m9_faa_owner_specs[i];
		u32 bytes = o->vram ? spec->bytes : spec->mapped_bytes;
		u32 hash = 0x811c9dc5, nonzero = 0, nonzero_words = 0;
		const char *domain = o->vram ? "BAR" : "DMA";

		if (!o->vram && (!spec->system || !o->cpu))
			continue;
		for (offset = 0; offset < bytes; offset += count) {
			count = min_t(u32, sizeof(data), bytes - offset);
			if (o->vram)
				iosys_map_memcpy_from(data, &o->map, offset, count);
			else
				memcpy(data, (u8 *)o->cpu + offset, count);
			if (!offset) {
				u32 prefix = min_t(u32, 20, count);

				/* Reuse the already bounded observation read. This
				 * is raw evidence, including unknown source bytes;
				 * no owner role or GPU record meaning is assigned. */
				dev_info(&mdev->pdev->dev,
					 "M9 FAA %s %s raw prefix: owner=%u native=%#x bytes=%u hex=%*phN\n",
					 before ? "before submit" : "retired",
					 domain, i, o->base, prefix, (int)prefix, data);
			}
			hash = m9_faa_hash(hash, data, count);
			for (j = 0; j < count; j++)
				nonzero += !!data[j];
			for (j = 0; j + sizeof(u32) <= count; j += sizeof(u32))
				nonzero_words += !!get_unaligned_le32(data + j);
		}
		if (before) {
			r->before_hash[i] = hash;
			r->before_nonzero[i] = nonzero;
			r->before_nonzero_words[i] = nonzero_words;
		}
		dev_info(&mdev->pdev->dev,
			 "M9 FAA %s %s observation: owner=%u captured=%#x native=%#x bytes=%u fnv=%#x nonzero_bytes=%u nonzero_words=%u\n",
			 before ? "before submit" : "retired",
			 domain, i, spec->captured, o->base, bytes,
			 hash, nonzero, nonzero_words);
		if (!before && r->baseline_valid)
			dev_info(&mdev->pdev->dev,
				 "M9 FAA %s delta: owner=%u before_fnv=%#x after_fnv=%#x hash_changed=%u before_nonzero_bytes=%u after_nonzero_bytes=%u before_nonzero_words=%u after_nonzero_words=%u\n",
				 domain, i, r->before_hash[i], hash, r->before_hash[i] != hash,
				 r->before_nonzero[i], nonzero,
				 r->before_nonzero_words[i], nonzero_words);
	}
	if (before)
		r->baseline_valid = true;
}

/* The sampled primary is borrowed, so the ordinary owner loop intentionally
 * skips it. Observe the actual pinned/reserved caller BO without turning it
 * into an owned allocation or extending the GPU command stream. Failure to
 * map observation data never invents a zero baseline or changes submission. */
static void m9_faa_observe_borrowed_color(struct m9x_device *mdev,
					struct m9x_faa_replay *r,
					struct drm_gem_vram_object *color_bo,
					bool before)
{
#ifdef M9_FAA_MULTI_CLEAR
	const u32 i = M9_FAA_COLOR_OWNER;
	struct iosys_map map = IOSYS_MAP_INIT_VADDR(NULL);
	u8 data[256];
	u32 bytes, offset, count, j, hash = 0x811c9dc5;
	u32 nonzero = 0, nonzero_words = 0;
	s64 local_offset;
	u64 base;
	int ret;

	if (!color_bo || i >= ARRAY_SIZE(r->owner) || r->owner[i].vram ||
	    r->owner[i].cpu || !r->borrowed_color_bytes)
		return;
	bytes = m9_faa_owner_specs[i].bytes;
	local_offset = drm_gem_vram_offset(color_bo);
	if (!bytes || bytes > 1024 * 1024 || (bytes & 3) || local_offset < 0 ||
	    bytes > r->borrowed_color_bytes || bytes > color_bo->bo.base.size ||
	    check_add_overflow(M9_VRAM_GPU_BASE, (u64)local_offset, &base) ||
	    base != r->owner[i].base)
		return;
	/* m9_submit_first_triangle owns this reservation across our call. */
	ret = drm_gem_vram_vmap(color_bo, &map);
	if (ret) {
		dev_warn(&mdev->pdev->dev, "M9 FAA borrowed observation unavailable: %d\n", ret);
		return;
	}
	rmb();
	for (offset = 0; offset < bytes; offset += count) {
		count = min_t(u32, sizeof(data), bytes - offset);
		iosys_map_memcpy_from(data, &map, offset, count);
		hash = m9_faa_hash(hash, data, count);
		for (j = 0; j < count; j++)
			nonzero += !!data[j];
		for (j = 0; j < count; j += sizeof(u32))
			nonzero_words += !!get_unaligned_le32(data + j);
	}
	drm_gem_vram_vunmap(color_bo, &map);
	if (before) {
		r->before_hash[i] = hash;
		r->before_nonzero[i] = nonzero;
		r->before_nonzero_words[i] = nonzero_words;
		r->borrowed_color_observed = true;
	}
	dev_info(&mdev->pdev->dev,
		 "M9 FAA %s BAR observation: owner=%u captured=%#x native=%#x bytes=%u fnv=%#x nonzero_bytes=%u nonzero_words=%u\n",
		 before ? "before submit" : "retired", i,
		 m9_faa_owner_specs[i].captured, r->owner[i].base, bytes,
		 hash, nonzero, nonzero_words);
	if (!before && r->borrowed_color_observed)
		dev_info(&mdev->pdev->dev,
			 "M9 FAA BAR delta: owner=%u before_fnv=%#x after_fnv=%#x hash_changed=%u before_nonzero_bytes=%u after_nonzero_bytes=%u before_nonzero_words=%u after_nonzero_words=%u\n",
			 i, r->before_hash[i], hash, r->before_hash[i] != hash,
			 r->before_nonzero[i], nonzero,
			 r->before_nonzero_words[i], nonzero_words);
#else
	(void)mdev;
	(void)r;
	(void)color_bo;
	(void)before;
#endif
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
	if (m9_faa_defer_output_map || m9_faa_program_access_kick) {
#ifndef M9_FAA_PRECLEAR_OUTPUT_WRITE_CONTROL
		return -EOPNOTSUPP;
#else
		if (!m9_faa_phased_submit || M9_FAA_OUTPUT_OWNER == U32_MAX)
			return -EINVAL;
#endif
	}
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
	ret = request_firmware(&fw, M9_FAA_INPUT_FIRMWARE, &mdev->pdev->dev);
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
		/* Only a profile with a sampled caller color substitutes that owner.
		 * An absent sentinel allocates every genuine source owner. */
		if (i == M9_FAA_COLOR_OWNER)
			continue;
		ret = m9_faa_allocate(mdev, &r->owner[i], &m9_faa_owner_specs[i],
				      m9_faa_defer_output_map && i == M9_FAA_OUTPUT_OWNER);
		if (ret)
			goto free;
	}
	if (M9_FAA_COUNTER_VIRTUAL) {
		ret = m9x_address_map_system(mdev, &r->counter_node,
					    mdev->transport.fence_bus, PAGE_SIZE);
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
	ret = m9_faa_verify_upload(mdev, r, fw);
	if (ret)
		goto free;
	r->prepared = true;
	mdev->transport.faa = r;
	dev_info(&mdev->pdev->dev, "M9 FAA private inputs: %zu owners, %u known bytes, %zu address/%zu tracker fixups\n",
		 ARRAY_SIZE(r->owner), M9_FAA_BLOB_BYTES,
		 ARRAY_SIZE(m9_faa_fixups), ARRAY_SIZE(m9_faa_trackers));
	ret = 0;
	goto release_fw;
free:
	m9_faa_release_failed_prepare(mdev, r);
release_fw:
	release_firmware(fw);
unlock:
	mutex_unlock(&mdev->transport.lock);
	return ret;
}

/* Actual normal 656c0 records preserve descriptor tag1 in DWORD9, 14 and
 * 19 (65836/6584b/65892). Ordinary Linux mode3 records use tag0 instead.
 * Convert only those three static pointers after prior jobs retire; leave
 * work shadows and every other source byte intact. Verify all originals
 * before the first write, and retain the existing identity-leaf admission.
 */
static int m9_faa_prepare_virtual_restore(struct m9x_device *mdev)
{
	struct iosys_map map;
	static const u32 word[] = { 9, 14, 19 };
	u32 base, i, j;
	int ret;

	lockdep_assert_held(&mdev->transport.lock);
	ret = m9x_address_restore_region(mdev, &map, &base);
	if (ret)
		return ret;
	if (base != mdev->transport.restore_base ||
	    !IS_ALIGNED(base, PAGE_SIZE) ||
	    PARHELIA_M9_RESTORE_BYTES < 0x700 + 0x3ff * 0x600 + 0x50 ||
	    base > U32_MAX - PARHELIA_M9_RESTORE_BYTES)
		return -ERANGE;
	for (i = 0; i < 0x400; i++) {
		u32 work = base + 0x200 + i * 0x600;
		u32 expected[] = { work, base, work + 0x440 };

		for (j = 0; j < ARRAY_SIZE(word); j++) {
			u32 offset = 0x700 + i * 0x600 + word[j] * sizeof(u32);
			u32 value = iosys_map_rd(&map, offset, u32);

			if (value != expected[j]) {
				dev_err(&mdev->pdev->dev,
					"M9 FAA virtual restore refused: record=%u word=%u observed=%#x expected=%#x\n",
					i, word[j], value, expected[j]);
				return -EINVAL;
			}
		}
	}
	for (i = 0; i < 0x400; i++) {
		u32 work = base + 0x200 + i * 0x600;
		u32 expected[] = { work, base, work + 0x440 };

		for (j = 0; j < ARRAY_SIZE(word); j++)
			iosys_map_wr(&map, 0x700 + i * 0x600 + word[j] * sizeof(u32),
				     u32, expected[j] | 1u);
	}
	wmb();
	for (i = 0; i < 0x400; i++) {
		u32 work = base + 0x200 + i * 0x600;
		u32 expected[] = { work, base, work + 0x440 };

		for (j = 0; j < ARRAY_SIZE(word); j++)
			if (iosys_map_rd(&map,
				0x700 + i * 0x600 + word[j] * sizeof(u32), u32) !=
			    (expected[j] | 1u))
				return -EIO;
	}
	dev_info(&mdev->pdev->dev,
		 "M9 FAA virtual restore: base=%#x 1024 records, DWORD9/14/19 tag1 verified\n", base);
	return 0;
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

/* Actual b5e70 clear uses resource+198 and ceil(resource+1bc/4096).
 * These are 151 logical companion pages, within its 152-page allocation.
 * The retained device+1c bit0 is zero: b6028/b6172 select no tail padding.
 * Page masks must follow the new allocation alignment, not captured masks.
 */
#ifndef M9_FAA_CLEAR_OWNER
#define M9_FAA_CLEAR_OWNER 4u
#define M9_FAA_CLEAR_BYTES 0x97000u
#define M9_FAA_CLEAR_START 0x11d8u
#define M9_FAA_CLEAR_END 0x1224u
#endif
#define M9_FAA_CLEAR_MAX_WORDS 20u

#if !defined(M9_FAA_HAS_COMPANION_CLEAR) || M9_FAA_HAS_COMPANION_CLEAR
static int m9_faa_clear_page_words(u32 base, u32 bytes, u32 *words,
				  u32 capacity, u32 *count)
{
	u32 page, remaining, leading, value, n = 0;

	if (!bytes || !IS_ALIGNED(base, PAGE_SIZE) ||
	    !IS_ALIGNED(bytes, PAGE_SIZE) || (u64)base + bytes > (u64)U32_MAX + 1)
		return -ERANGE;
	page = base >> PAGE_SHIFT;
	remaining = bytes >> PAGE_SHIFT;
	leading = min_t(u32, remaining, (-page) & 7);
	if (!!leading + DIV_ROUND_UP(remaining - leading, 8) > capacity)
		return -ENOSPC;
	value = (((page & 0xffff8u) | 0xd00000u) << 5);
	if (leading) {
		u32 mask = remaining == leading ?
			(0xffu >> (8 - leading)) << (page & 7) :
			0xffu << (8 - leading);

		words[n++] = value | (mask & 0xff);
		value = (value & ~0x1ffff00u) | ((value + 0x100) & 0x1ffff00u);
		remaining -= leading;
	}
	while (remaining >= 8) {
		words[n++] = value | 0xff;
		value = (value & ~0x1ffff00u) | ((value + 0x100) & 0x1ffff00u);
		remaining -= 8;
	}
	if (remaining)
		words[n++] = value | (0xffu >> (8 - remaining));
	*count = n;
	return 0;
}
#endif

/* Patch ordinary addresses/trackers at their captured offsets first, then
 * move the already patched suffix once. No record outside this inline clear
 * is regenerated or reinterpreted. The command buffer has an owned capacity
 * independent of its exact known/submitted prefix length.
 */
#ifdef M9_FAA_MULTI_CLEAR
static int m9_faa_bind_borrowed_color(struct m9x_faa_replay *r, u32 color,
				    struct drm_gem_vram_object *color_bo)
{
	struct m9_faa_owner *owner = &r->owner[M9_FAA_COLOR_OWNER];
	u64 base, bytes;
	s64 offset;

	/* The caller holds this BO's pin and reservation throughout submit;
	 * its existing fence/quarantine path retains it through retirement. */
	if (!color_bo || owner->vram || owner->cpu || owner->bytes ||
	    !m9_faa_owner_specs[M9_FAA_COLOR_OWNER].local)
		return -EINVAL;
	offset = drm_gem_vram_offset(color_bo);
	bytes = color_bo->bo.base.size;
	if (offset < 0 || check_add_overflow(M9_VRAM_GPU_BASE, (u64)offset, &base) ||
	    base != color || !IS_ALIGNED(base, PAGE_SIZE) ||
	    bytes < m9_faa_owner_specs[M9_FAA_COLOR_OWNER].bytes ||
	    bytes > (u64)U32_MAX + 1 - base)
		return -ERANGE;
	owner->base = color;
	r->borrowed_color_bytes = bytes;
	return 0;
}
#endif

/* The public one-target raster producer leaves inactive AB/AC halfwords
 * as stack residue. Preserve its active target mask, but derive neutral
 * masks for the three absent targets instead of replaying that residue.
 * This is separate from address relocation and never alters the capture. */
static int m9_faa_normalize_raster_masks(struct m9x_faa_replay *r)
{
#ifdef M9_FAA_RASTER_MASK_OWNER
	struct m9_faa_owner *owner;
	u8 *data;
	u32 ab = M9_FAA_RASTER_MASK_AB_OFFSET;
	u32 route = M9_FAA_RASTER_MASK_89_OFFSET;

	if (M9_FAA_RASTER_MASK_OWNER >= ARRAY_SIZE(r->owner))
		return -EINVAL;
	owner = &r->owner[M9_FAA_RASTER_MASK_OWNER];
	data = owner->cpu;
	if (!data || owner->vram || ab < 8 || route < 12 ||
	    (u64)ab + 8 > owner->bytes || (u64)route + 4 > owner->bytes ||
	    get_unaligned_le32(data + ab - 8) != 3 ||
	    get_unaligned_le32(data + ab - 4) != 0x45aeacab ||
	    get_unaligned_le32(data + route - 12) != 4 ||
	    get_unaligned_le32(data + route - 8) != 0xc18a8988 ||
	    get_unaligned_le32(data + route) != 0xfe207ff8 ||
	    (get_unaligned_le32(data + ab) & 0xffff) != 0xffff)
		return -EINVAL;
	put_unaligned_le32(get_unaligned_le32(data + ab) | 0xffff0000,
			   data + ab);
	put_unaligned_le32(0xffffffff, data + ab + 4);
#else
	(void)r;
#endif
	return 0;
}

static int m9_faa_rebase_page_clear(struct m9x_faa_replay *r)
{
	struct m9_faa_owner *primary = &r->owner[M9_FAA_PRIMARY_OWNER];
#if defined(M9_FAA_HAS_COMPANION_CLEAR) && !M9_FAA_HAS_COMPANION_CLEAR
	/* This complete source profile contains no inline companion clear.
	 * Preserve its exact primary and independent secondary finish. */
	if (!primary->cpu || primary->vram ||
	    primary->bytes < M9_FAA_PRIMARY_BYTES ||
	    M9_FAA_CLEAR_OWNER != U32_MAX || M9_FAA_CLEAR_BYTES ||
	    M9_FAA_CLEAR_START || M9_FAA_CLEAR_END)
		return -EINVAL;
	r->primary_bytes = M9_FAA_PRIMARY_BYTES;
	return 0;
#elif defined(M9_FAA_MULTI_CLEAR)
	u32 replacement[ARRAY_SIZE(m9_faa_clear_segments)][M9_FAA_CLEAR_MAX_WORDS];
	u32 counts[ARRAY_SIZE(m9_faa_clear_segments)];
	u32 original[M9_FAA_CLEAR_MAX_WORDS];
	u64 bytes = M9_FAA_PRIMARY_BYTES;
	u8 *data = primary->cpu;
	u32 i, j, original_count;
	int ret;

	/* This profile contains two independently observed b5e70 calls. Check
	 * every owner, packet and final extent before modifying any bytes. */
	if (ARRAY_SIZE(m9_faa_clear_segments) != 2 || !data || primary->vram ||
	    primary->bytes < M9_FAA_PRIMARY_BYTES)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(m9_faa_clear_segments); i++) {
		const struct m9_faa_clear_segment *s = &m9_faa_clear_segments[i];
		struct m9_faa_owner *owner;

		if (s->owner >= ARRAY_SIZE(r->owner) || !s->bytes ||
		    !IS_ALIGNED(s->start, 4) || !IS_ALIGNED(s->end, 4) ||
		    s->start < 28 || s->start >= s->end ||
		    (u64)s->end + 8 > M9_FAA_PRIMARY_BYTES ||
		    (i && s->start - 28 < m9_faa_clear_segments[i - 1].end + 8))
			return -EINVAL;
		owner = &r->owner[s->owner];
		if (s->owner == M9_FAA_COLOR_OWNER) {
			if (owner->vram || owner->cpu || owner->bytes ||
			    r->borrowed_color_bytes < s->bytes)
				return -EINVAL;
		} else if (!owner->vram || owner->bytes < s->bytes) {
			return -EINVAL;
		}
		if (m9_faa_owner_specs[s->owner].bytes < s->bytes ||
		    get_unaligned_le32(data + s->start - 28) != 0x4545ace6 ||
		    get_unaligned_le32(data + s->start - 24) != 2 ||
		    get_unaligned_le32(data + s->start - 20) != 0 ||
		    get_unaligned_le32(data + s->start - 16) != 0x45e62b2a ||
		    get_unaligned_le32(data + s->start - 12) != 0x454545e6 ||
		    get_unaligned_le32(data + s->start - 8) != 0xffff ||
		    get_unaligned_le32(data + s->end) != 0x4545e3e6 ||
		    get_unaligned_le32(data + s->end + 4) != 1)
			return -EINVAL;
		ret = m9_faa_clear_page_words(m9_faa_owner_specs[s->owner].captured,
			s->bytes, original, ARRAY_SIZE(original), &original_count);
		if (ret)
			return ret;
		if (original_count * 4 != s->end - s->start ||
		    get_unaligned_le32(data + s->start - 4) !=
			(0x20000000 | (original_count - 1)))
			return -EINVAL;
		for (j = 0; j < original_count; j++)
			if (get_unaligned_le32(data + s->start + j * 4) != original[j])
				return -EINVAL;
		ret = m9_faa_clear_page_words(owner->base, s->bytes,
			replacement[i], ARRAY_SIZE(replacement[i]), &counts[i]);
		if (ret)
			return ret;
	}
	/* Intermediate suffix growth must fit too, even when the earlier
	 * segment later shrinks and the final length would fit the allocation. */
	for (i = ARRAY_SIZE(m9_faa_clear_segments); i-- > 0;) {
		const struct m9_faa_clear_segment *s = &m9_faa_clear_segments[i];

		bytes = bytes - (s->end - s->start) + counts[i] * 4;
		if (bytes > primary->bytes)
			return -ENOSPC;
	}
	/* Address and timeline fixups have already been applied at their
	 * original offsets. Rebuild from the last segment backwards: shifting
	 * an earlier suffix then preserves the already rebuilt later clear. */
	bytes = M9_FAA_PRIMARY_BYTES;
	for (i = ARRAY_SIZE(m9_faa_clear_segments); i-- > 0;) {
		const struct m9_faa_clear_segment *s = &m9_faa_clear_segments[i];

		memmove(data + s->start + counts[i] * 4, data + s->end,
			bytes - s->end);
		put_unaligned_le32(0x20000000 | (counts[i] - 1),
			data + s->start - 4);
		for (j = 0; j < counts[i]; j++)
			put_unaligned_le32(replacement[i][j], data + s->start + j * 4);
		bytes = bytes - (s->end - s->start) + counts[i] * 4;
	}
	r->primary_bytes = bytes;
	return 0;
#else
	struct m9_faa_owner *companion = &r->owner[M9_FAA_CLEAR_OWNER];
	u32 original[M9_FAA_CLEAR_MAX_WORDS], replacement[M9_FAA_CLEAR_MAX_WORDS];
	u32 original_count, count, bytes, i;
	u8 *data = primary->cpu;
	int ret;

	if (!data || primary->vram || primary->bytes < M9_FAA_PRIMARY_BYTES ||
	    companion->bytes < M9_FAA_CLEAR_BYTES || !companion->vram ||
	    M9_FAA_CLEAR_END > M9_FAA_PRIMARY_BYTES ||
	    get_unaligned_le32(data + 0x11bc) != 0x4545ace6 ||
	    get_unaligned_le32(data + 0x11c0) != 2 ||
	    get_unaligned_le32(data + 0x11c4) != 0 ||
	    get_unaligned_le32(data + 0x11c8) != 0x45e62b2a ||
	    get_unaligned_le32(data + 0x11cc) != 0x454545e6 ||
	    get_unaligned_le32(data + 0x11d0) != 0xffff ||
	    get_unaligned_le32(data + M9_FAA_CLEAR_END) != 0x4545e3e6 ||
	    get_unaligned_le32(data + M9_FAA_CLEAR_END + 4) != 1)
		return -EINVAL;
	ret = m9_faa_clear_page_words(m9_faa_owner_specs[M9_FAA_CLEAR_OWNER].captured,
		M9_FAA_CLEAR_BYTES, original, ARRAY_SIZE(original), &original_count);
	if (ret)
		return ret;
	if (original_count * sizeof(u32) != M9_FAA_CLEAR_END - M9_FAA_CLEAR_START ||
	    get_unaligned_le32(data + M9_FAA_CLEAR_START - 4) !=
		(0x20000000 | (original_count - 1)))
		return -EINVAL;
	for (i = 0; i < original_count; i++)
		if (get_unaligned_le32(data + M9_FAA_CLEAR_START + i * 4) != original[i])
			return -EINVAL;
	ret = m9_faa_clear_page_words(companion->base, M9_FAA_CLEAR_BYTES,
		replacement, ARRAY_SIZE(replacement), &count);
	if (ret)
		return ret;
	bytes = M9_FAA_PRIMARY_BYTES - original_count * 4 + count * 4;
	if (bytes > primary->bytes)
		return -ENOSPC;
	memmove(data + M9_FAA_CLEAR_START + count * 4, data + M9_FAA_CLEAR_END,
		M9_FAA_PRIMARY_BYTES - M9_FAA_CLEAR_END);
	put_unaligned_le32(0x20000000 | (count - 1), data + M9_FAA_CLEAR_START - 4);
	for (i = 0; i < count; i++)
		put_unaligned_le32(replacement[i], data + M9_FAA_CLEAR_START + i * 4);
	r->primary_bytes = bytes;
	return 0;
#endif
}

#ifdef M9_FAA_PRECLEAR_OUTPUT_WRITE_CONTROL
/* Actual 2cb40 waits for the program resource before its unconditional
 * 2cba8 frontend kick. Its later CPU-aperture mapping is a separate branch.
 * The two entry-offset encoders identify the retained C2 owner; no captured
 * resource address or CPU alias is installed by this bounded control. */
static int m9_faa_program_access(struct m9x_device *mdev,
				 struct m9x_faa_replay *r, u64 point)
{
	u32 i, program = U32_MAX, entries = 0, offsets = 0;
	const struct m9_faa_owner_spec *spec;
	struct m9_faa_owner *owner;
	int ret;

	for (i = 0; i < ARRAY_SIZE(m9_faa_fixups); i++) {
		const struct m9_faa_fixup *f = &m9_faa_fixups[i];

		if (f->encoder != 0x248ab)
			continue;
		if (!i || f->offset < 4 ||
		    m9_faa_fixups[i - 1].encoder != 0x24886 ||
		    m9_faa_fixups[i - 1].dst != f->dst ||
		    m9_faa_fixups[i - 1].offset != f->offset - 4 ||
		    m9_faa_fixups[i - 1].src != f->src ||
		    m9_faa_fixups[i - 1].addend != f->addend ||
		    m9_faa_fixups[i - 1].tag != f->tag ||
		    (f->addend != 0 && f->addend != 32) ||
		    f->src >= ARRAY_SIZE(r->owner) ||
		    (program != U32_MAX && program != f->src))
			return -EINVAL;
		program = f->src;
		offsets |= f->addend ? 2 : 1;
		entries++;
	}
	if (entries != 2 || offsets != 3)
		return -EINVAL;
	spec = &m9_faa_owner_specs[program];
	owner = &r->owner[program];
	if (!spec->local || spec->system || spec->bytes != 0x1080 ||
	    !owner->vram || owner->cpu || owner->bytes != PAGE_ALIGN(spec->bytes))
		return -EINVAL;
	ret = m9x_address_check_local_identity(mdev, owner->base, owner->bytes);
	if (ret)
		return ret;
	wmb();
	m9x_write(mdev, M9_REG_FRONTEND_KICK, 1);
	dev_info(&mdev->pdev->dev,
		 "M9 FAA program access kick: point=%#llx owner=%u gpu=%#x bytes=%u\n",
		 point, program, owner->base, owner->bytes);
	return 0;
}

static int m9_faa_publish_output_phase(struct m9x_device *mdev, u32 phase,
                                      u64 point, void *data)
{
	struct m9x_faa_replay *r = data;
	struct m9_faa_owner *output;
	int ret;

	if (phase == 0 && m9_faa_program_access_kick)
		return m9_faa_program_access(mdev, r, point);
	if (phase != 1 || !m9_faa_defer_output_map)
		return 0;
	output = m9_faa_output_owner(r);
	if (!output || !output->cpu || output->vram ||
	    !m9_faa_owner_specs[M9_FAA_OUTPUT_OWNER].flags30 ||
	    m9_faa_owner_specs[M9_FAA_OUTPUT_OWNER].mapped_bytes != 0x50000)
		return -EINVAL;
	/* Called after the actual120 wait and before posting the160 suffix.
	 * Existing publisher derives every PTE and C24 word from this owner. */
	ret = m9x_address_publish_faa_output(mdev, &output->system_node, output->dma);

	if (!ret)
		dev_info(&mdev->pdev->dev,
			 "M9 FAA late output mapping: point=%#llx gpu=%#x pages=80\n",
			 point, output->base);
	return ret;
}
#endif

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
	if (M9_FAA_COUNTER_VIRTUAL &&
	    (!drm_mm_node_allocated(&r->counter_node) ||
	     r->counter_node.size != PAGE_SIZE || r->counter_node.start > U32_MAX)) {
		ret = -EINVAL;
		goto unlock;
	}
	base = t->next_fence - 0x10;
	target = base + M9_FAA_TRACKER_LAST - 0x10;
	/* Reuse an already observed retirement, or the unchanged CPU seed10
	 * on explicitly admitted fresh source initialization. No CPU advance. */
	if ((READ_ONCE(((u32 *)t->fence_cpu)[0]) & ~15u) != base) {
		ret = -EAGAIN;
		goto unlock;
	}
	ret = m9x_address_check_local_identity(mdev, t->restore_base,
					 PARHELIA_M9_RESTORE_BYTES);
	if (ret)
		goto unlock;
#if !defined(M9_FAA_HAS_SAMPLED_COLOR) || M9_FAA_HAS_SAMPLED_COLOR
#ifdef M9_FAA_MULTI_CLEAR
	ret = m9_faa_bind_borrowed_color(r, color, color_bo);
	if (ret)
		goto unlock;
#else
	r->owner[M9_FAA_COLOR_OWNER].base = color;
#endif
#else
	if (M9_FAA_COLOR_OWNER != U32_MAX) {
		ret = -EINVAL;
		goto unlock;
	}
#endif
	for (i = 0; i < ARRAY_SIZE(r->owner); i++) {
		if (!m9_faa_owner_specs[i].local)
			continue;
		ret = m9x_address_local_prepare(mdev, r->owner[i].base,
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
	ret = m9_faa_prepare_virtual_restore(mdev);
	if (ret)
		goto unlock;
	for (i = 0; i < ARRAY_SIZE(m9_faa_fixups); i++) {
		const struct m9_faa_fixup *f = &m9_faa_fixups[i];
		struct m9_faa_owner *dst;
		u32 address, value;

		if (f->dst >= ARRAY_SIZE(r->owner) || f->src >= ARRAY_SIZE(r->owner) ||
		    (M9_FAA_COUNTER_VIRTUAL && f->src == M9_FAA_PRIMARY_OWNER)) {
			ret = -EINVAL;
			goto unlock;
		}
		dst = &r->owner[f->dst];
		if (dst->vram || !dst->cpu || !IS_ALIGNED(f->offset, 4) ||
		    f->offset > m9_faa_owner_specs[f->dst].bytes - 4 ||
		    f->addend > (m9_faa_owner_specs[f->src].system ?
			m9_faa_owner_specs[f->src].mapped_bytes :
			m9_faa_owner_specs[f->src].bytes) ||
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
	ret = m9_faa_normalize_raster_masks(r);
	if (ret)
		goto unlock;
	ret = m9_faa_rebase_page_clear(r);
	if (ret)
		goto unlock;
#if defined(M9_FAA_MULTI_CLEAR)
	for (i = 0; i < ARRAY_SIZE(m9_faa_clear_segments); i++)
		dev_info(&mdev->pdev->dev,
			 "M9 FAA page clear: segment=%u owner=%u base=%#x bytes=%#x primary_bytes=%u\n",
			 i, m9_faa_clear_segments[i].owner,
			 r->owner[m9_faa_clear_segments[i].owner].base,
			 m9_faa_clear_segments[i].bytes, r->primary_bytes);
#elif !defined(M9_FAA_HAS_COMPANION_CLEAR) || M9_FAA_HAS_COMPANION_CLEAR
	dev_info(&mdev->pdev->dev,
		 "M9 FAA companion clear: base=%#x bytes=%#x inline_words=%u primary_bytes=%u\n",
		 r->owner[M9_FAA_CLEAR_OWNER].base, M9_FAA_CLEAR_BYTES,
		 (r->primary_bytes - M9_FAA_PRIMARY_BYTES) / 4 + 19, r->primary_bytes);
#endif
	/* Only a generated source-closed ordinary-output template owns this
	 * object. A successful older sampled resolve is not an output read. */
	if (m9_faa_output_owner(r)) {
		struct m9_faa_owner *output = m9_faa_output_owner(r);

		if (!m9_faa_owner_specs[M9_FAA_OUTPUT_OWNER].system ||
		    !m9_faa_owner_specs[M9_FAA_OUTPUT_OWNER].flags30 ||
		    output->bytes != 0x50000 || !output->cpu ||
		    !drm_mm_node_allocated(&output->system_node)) {
			ret = -EINVAL;
			goto unlock;
		}
		drm_gem_object_get(&color_bo->bo.base);
		r->output_color = &color_bo->bo.base;
	}
	fence = m9x_fence_create_target_locked(mdev, target);
	if (!fence) {
		ret = -ENOMEM;
		goto unlock;
	}
	dma_resv_add_fence(color_bo->bo.base.resv, &fence->base, DMA_RESV_USAGE_WRITE);
	/* Actual 23a3b/23aaf normal activation contracts. The ordinary context
 * retired, or fresh source admission retained the initial CPU seed;
 * the two additional tracker words belong to this same
 * retained coherent common buffer, not captured host physical memory.
 * Only the actual known128-byte initializer is reproduced. The low seed
 * retains observed retirement, or the fresh CPU origin10; it never advances
 * a completion counter or publishes GPU success. */
	memset(t->fence_cpu, 0, 128);
	WRITE_ONCE(((u32 *)t->fence_cpu)[0], base);
	WRITE_ONCE(((u32 *)t->fence_cpu)[0x20 / 4], base);
	WRITE_ONCE(((u32 *)t->fence_cpu)[0x40 / 4], base);
	WRITE_ONCE(((u32 *)t->fence_cpu)[0x60 / 4], base);
	if (t->faa_source_context)
		dev_info(&mdev->pdev->dev,
			 "M9 FAA source context: CPU seed=%#x target=%#x no prior GPU submission\n",
			 base, target);
	/* Publish coherent command/counter words and any prior WC writes. */
	wmb();
	m9x_write(mdev, M9_REG_FENCE0_ADDRESS, lower_32_bits(t->fence_bus + 0x20) | 2);
	/* Actual23a47..23a94: the source110/PTEa proxy publishes the same
	 * transport completion page through class1. Other selectors retain
	 * their separately proven physical-tag2 common-buffer fields. */
	if (M9_FAA_COUNTER_VIRTUAL)
		m9x_write(mdev, M9_REG_FENCE1_ADDRESS, (u32)r->counter_node.start | 1);
	m9x_write(mdev, M9_REG_FENCE2_ADDRESS, lower_32_bits(t->fence_bus + 0x40) | 2);
	/* Genuine normal activation selects the virtual restore descriptor.
 * Its native identity leaves were checked separately from FAA pool6. */
	m9x_write(mdev, M9_REG_RESTORE_TAGGED, ((t->restore_base + 0x200) & ~31u) | 1);
	m9x_read(mdev, M9_REG_FENCE2_ADDRESS);
	/* Baseline follows real initialization and every pointer/counter patch,
	 * but precedes this immutable stream's first GPU publication. */
	m9_faa_observe(mdev, r, true);
	m9_faa_observe_borrowed_color(mdev, r, color_bo, true);
	/* Source23a09/2461e consume this live class1/B owner directly. Its
	 * final size includes the regenerated inline clear and shifted suffix.
	 * No transport clone substitutes a physical domain for this primary. */
	if (!m9_faa_owner_specs[M9_FAA_PRIMARY_OWNER].system ||
	    !m9_faa_owner_specs[M9_FAA_PRIMARY_OWNER].flags30 ||
	    r->owner[M9_FAA_PRIMARY_OWNER].vram ||
	    !r->owner[M9_FAA_PRIMARY_OWNER].cpu ||
	    r->owner[M9_FAA_PRIMARY_OWNER].base !=
			r->owner[M9_FAA_PRIMARY_OWNER].system_node.start ||
	    r->owner[M9_FAA_PRIMARY_OWNER].system_node.size !=
			m9_faa_owner_specs[M9_FAA_PRIMARY_OWNER].mapped_bytes) {
		ret = -EINVAL;
		goto unlock;
	}
	if (m9_faa_phased_submit) {
#ifdef M9_FAA_PRECLEAR_OUTPUT_WRITE_CONTROL
		/* These four source-exported endpoints are after both variable
		 * clear segments, so they all move by the same signed suffix
		 * delta. No captured address or desired completion is replayed. */
		static const struct m9x_faa_submit_phase source[] = {
			{ 4980, 0xc0 }, { 5068, 0x120 },
			{ 5224, 0x160 }, { 5724, 0x180 },
		};
		struct m9x_faa_submit_phase phases[ARRAY_SIZE(source)];
		s64 delta = (s64)r->primary_bytes - M9_FAA_PRIMARY_BYTES;

		if (M9_FAA_PRIMARY_BYTES != 5724 || M9_FAA_TRACKER_LAST != 0x180) {
			ret = -EINVAL;
			goto unlock;
		}
		for (i = 0; i < ARRAY_SIZE(m9_faa_clear_segments); i++)
			if (m9_faa_clear_segments[i].end > source[0].bytes) {
				ret = -EINVAL;
				goto unlock;
			}
		for (i = 0; i < ARRAY_SIZE(source); i++) {
			if ((s64)source[i].bytes + delta <= 0 ||
			    (s64)source[i].bytes + delta > U32_MAX) {
				ret = -ERANGE;
				goto unlock;
			}
			phases[i].bytes = source[i].bytes + delta;
			phases[i].target = base + source[i].target - 0x10;
		}
		ret = m9x_submit_faa_virtual_phases_locked(mdev,
				&r->owner[M9_FAA_PRIMARY_OWNER].system_node,
				r->owner[M9_FAA_PRIMARY_OWNER].dma, phases,
				ARRAY_SIZE(phases), point, &posted,
				(m9_faa_defer_output_map || m9_faa_program_access_kick) ?
					m9_faa_publish_output_phase : NULL,
				r);
#else
		ret = -EOPNOTSUPP;
#endif
	} else {
		ret = m9x_submit_faa_virtual_primary_locked(mdev,
				&r->owner[M9_FAA_PRIMARY_OWNER].system_node,
				r->owner[M9_FAA_PRIMARY_OWNER].dma, r->primary_bytes,
				target, point, &posted);
	}
	*color_active = posted && ret;
	if (!ret) {
		t->next_fence = target + 0x10;
		/* An output allocation may already exist at an earlier vendor
		 * format-transition wait. Retirement of that prefix does not
		 * complete the later resolve or make its pixels readable. */
		if (!M9_FAA_OUTPUT_PENDING && m9_faa_output_owner(r)) {
			r->output_generation = t->generation;
			r->output_point = *point;
			r->output_retired = true;
		}
		m9_faa_observe(mdev, r, false);
		m9_faa_observe_borrowed_color(mdev, r, color_bo, false);
		dev_info(&mdev->pdev->dev,
			 "M9 FAA private frame retired: point=%#llx sampled_primary=%#x ordinary_output=%#x 320x240\n",
			 *point, color, m9_faa_output_owner(r) ? m9_faa_output_owner(r)->base : 0);
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

/* Explicit internal ac1f0 output contract, below unjoined public Lock
 * admission. This never reinterprets the sampled F022 host-aperture view.
 * The actual kind6 flags30 system owner is read through its retained DMA
 * alias after real retirement; 8b020/8c4a0 supplies each source address. */
int m9x_faa_output_read(struct m9x_device *mdev, struct drm_file *file,
		       const struct drm_parhelia_m9_surface_access *args)
{
	struct m9x_transport *t = &mdev->transport;
	struct m9x_faa_replay *r;
	struct m9_faa_owner *output;
	struct drm_gem_object *gem;
	u8 *pixels;
	u64 end;
	u32 x, y, offset;
	int ret = 0;

	if (!m9_faa_diag || args->flags != PARHELIA_M9_ACCESS_FAA_OUTPUT ||
	    args->op != PARHELIA_M9_ACCESS_READ || args->offset ||
	    args->descriptor0 != 0xc8210140 || args->descriptor1 != 0x100f0 ||
	    args->surface_width != 320 || args->width != 320 || args->height != 240 ||
	    args->x || args->y || args->data_stride != 1280 ||
	    args->data_size < 320 * 240 * 4 ||
	    check_add_overflow(args->data_ptr, (u64)(320 * 240 * 4), &end))
		return -EINVAL;
	gem = drm_gem_object_lookup(file, args->handle);
	if (!gem)
		return -ENOENT;
	pixels = kvmalloc(320 * 240 * 4, GFP_KERNEL);
	if (!pixels) {
		ret = -ENOMEM;
		goto put;
	}
	mutex_lock(&t->lock);
	r = t->faa;
	output = r ? m9_faa_output_owner(r) : NULL;
	if (t->wedged || !r || !output || !r->output_retired ||
	    r->output_color != gem || r->output_generation != t->generation ||
	    !r->output_point || !output->cpu || output->bytes != 0x50000 ||
	    !drm_mm_node_allocated(&output->system_node) ||
	    output->system_node.size != output->bytes ||
	    output->system_node.start != output->base) {
		ret = t->wedged ? -EIO : -ENODATA;
		goto unlock;
	}
	/* Actual aff90 calls 2cb40(resource, 0, 1). After its query/write
	 * waits succeed, 2cba8 kicks the frontend before selecting the kind1
	 * CPU alias and copying pixels. Keep that transition after the live
	 * retirement/owner gates; a pre-submit kick is a different ordering.
	 */
	m9x_write(mdev, M9_REG_FRONTEND_KICK, 1);
	/* WC loads need a CPU read barrier after the retained retirement
	 * gate; dma_rmb alone is a compiler barrier on x86. */
	rmb();
	dma_rmb();
	for (y = 0; y < 240; y++)
		for (x = 0; x < 320; x++) {
			ret = m9_faa_output_offset(x, y, &offset);
			if (ret || offset > output->bytes - 4) {
				ret = -ERANGE;
				goto unlock;
			}
			/* Hardware format64 is A8B8G8R8: little-endian RGBA.
			 * There is no CPU color synthesis or sampled-data decoder. */
			memcpy(pixels + (y * 320 + x) * 4,
			       (u8 *)output->cpu + offset, 4);
		}
	dev_info(&mdev->pdev->dev,
		 "M9 FAA output read: point=%#llx system=%#x descriptor=c8210140/100f0 RGBA 320x240 post_wait_kick=1\n",
		 r->output_point, output->base);
unlock:
	mutex_unlock(&t->lock);
	if (!ret && copy_to_user(u64_to_user_ptr(args->data_ptr), pixels, 320 * 240 * 4))
		ret = -EFAULT;
	kvfree(pixels);
put:
	drm_gem_object_put(gem);
	return ret;
}
