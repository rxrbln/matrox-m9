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
#include "m9_faa_native_data.h"
/* The older sampled-destination template has no ordinary output owner.
 * It must never be advertised as a completed ac1f0 output contract. */
#ifndef M9_FAA_OUTPUT_OWNER
#define M9_FAA_OUTPUT_OWNER U32_MAX
#endif

struct m9_faa_owner {
	struct drm_gem_vram_object *vram;
	struct iosys_map map;
	void *cpu;
	dma_addr_t dma;
	u32 base, bytes;
	struct drm_mm_node system_node;
};
struct m9x_faa_replay {
	struct m9_faa_owner owner[ARRAY_SIZE(m9_faa_owner_specs)];
	bool prepared, attempted, baseline_valid;
	u32 primary_bytes;
	struct drm_gem_object *output_color;
	u64 output_point;
	u32 output_generation;
	bool output_retired;
	u32 before_hash[ARRAY_SIZE(m9_faa_owner_specs)];
	u32 before_nonzero[ARRAY_SIZE(m9_faa_owner_specs)];
	u32 before_nonzero_words[ARRAY_SIZE(m9_faa_owner_specs)];
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

static struct m9_faa_owner *m9_faa_output_owner(struct m9x_faa_replay *r)
{
	if (M9_FAA_OUTPUT_OWNER >= ARRAY_SIZE(r->owner))
		return NULL;
	return &r->owner[M9_FAA_OUTPUT_OWNER];
}

static void m9_faa_release(struct m9x_device *mdev, struct m9x_faa_replay *r)
{
	u32 i;

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
		for (i = 0; i < ARRAY_SIZE(r->owner); i++)
			if (drm_mm_node_allocated(&r->owner[i].system_node))
				m9x_address_unmap_system(mdev, &r->owner[i].system_node);
		mutex_unlock(&mdev->transport.lock);
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
		if (spec->system) {
			ret = spec->flags30 ?
				m9x_address_map_faa_system(mdev, &o->system_node,
						  o->dma, spec->mapped_bytes) :
				m9x_address_map_system(mdev, &o->system_node,
						      o->dma, spec->mapped_bytes);
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

	/* These TTM mappings may be WC. dma_wmb() alone is only a compiler
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

/* Bounded observations before submit and after actual retirement. Includes stores, HZ,
 * pool and sentinel, but never claims unknown initial contents were zero or
 * that a nonzero byte proves a GPU store. Caller color has its separate
 * descriptor-aware readback and is not mapped here. No allocation is written.
 */
static void m9_faa_observe(struct m9x_device *mdev,
			 struct m9x_faa_replay *r, bool before)
{
	const u32 budget = 16 * 1024 * 1024;
	u8 data[256];
	u32 i, offset, count, j, total = 0;

	for (i = 0; i < ARRAY_SIZE(r->owner); i++) {
		if (!r->owner[i].vram)
			continue;
		if (m9_faa_owner_specs[i].bytes > budget - total) {
			dev_err(&mdev->pdev->dev, "M9 FAA observation exceeds %u-byte budget\n",
				budget);
			return;
		}
		total += m9_faa_owner_specs[i].bytes;
	}
	/* Order publication/completion observations before BAR reads. */
	rmb();
	for (i = 0; i < ARRAY_SIZE(r->owner); i++) {
		struct m9_faa_owner *o = &r->owner[i];
		u32 bytes = m9_faa_owner_specs[i].bytes;
		u32 hash = 0x811c9dc5, nonzero = 0, nonzero_words = 0;

		if (!o->vram)
			continue;
		for (offset = 0; offset < bytes; offset += count) {
			count = min_t(u32, sizeof(data), bytes - offset);
			iosys_map_memcpy_from(data, &o->map, offset, count);
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
			 "M9 FAA %s BAR observation: owner=%u captured=%#x native=%#x bytes=%u fnv=%#x nonzero_bytes=%u nonzero_words=%u\n",
			 before ? "before submit" : "retired",
			 i, m9_faa_owner_specs[i].captured, o->base, bytes,
			 hash, nonzero, nonzero_words);
		if (!before && r->baseline_valid)
			dev_info(&mdev->pdev->dev,
				 "M9 FAA BAR delta: owner=%u before_fnv=%#x after_fnv=%#x hash_changed=%u before_nonzero_bytes=%u after_nonzero_bytes=%u before_nonzero_words=%u after_nonzero_words=%u\n",
				 i, r->before_hash[i], hash, r->before_hash[i] != hash,
				 r->before_nonzero[i], nonzero,
				 r->before_nonzero_words[i], nonzero_words);
	}
	if (before)
		r->baseline_valid = true;
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
	ret = m9_faa_verify_upload(mdev, r, fw);
	if (ret)
		goto free;
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
#define M9_FAA_CLEAR_OWNER 4u
#define M9_FAA_CLEAR_BYTES 0x97000u
#define M9_FAA_CLEAR_START 0x11d8u
#define M9_FAA_CLEAR_END 0x1224u
#define M9_FAA_CLEAR_MAX_WORDS 20u

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

/* Patch ordinary addresses/trackers at their captured offsets first, then
 * move the already patched suffix once. No record outside this inline clear
 * is regenerated or reinterpreted. The command buffer has an owned capacity
 * independent of its exact known/submitted prefix length.
 */
static int m9_faa_rebase_page_clear(struct m9x_faa_replay *r)
{
	struct m9_faa_owner *primary = &r->owner[M9_FAA_PRIMARY_OWNER];
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

		if (f->dst >= ARRAY_SIZE(r->owner) || f->src >= ARRAY_SIZE(r->owner)) {
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
	ret = m9_faa_rebase_page_clear(r);
	if (ret)
		goto unlock;
	dev_info(&mdev->pdev->dev,
		 "M9 FAA companion clear: base=%#x bytes=%#x inline_words=%u primary_bytes=%u\n",
		 r->owner[M9_FAA_CLEAR_OWNER].base, M9_FAA_CLEAR_BYTES,
		 (r->primary_bytes - M9_FAA_PRIMARY_BYTES) / 4 + 19, r->primary_bytes);
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
 * just retired; the two additional tracker words belong to this same
 * retained coherent common buffer, not captured host physical memory.
 * Only the actual known128-byte initializer is reproduced; the low seed
 * equals the already observed GPU retirement and does not publish success. */
	memset(t->fence_cpu, 0, 128);
	WRITE_ONCE(((u32 *)t->fence_cpu)[0], base);
	WRITE_ONCE(((u32 *)t->fence_cpu)[0x20 / 4], base);
	WRITE_ONCE(((u32 *)t->fence_cpu)[0x40 / 4], base);
	WRITE_ONCE(((u32 *)t->fence_cpu)[0x60 / 4], base);
	/* Publish coherent command/counter words and any prior WC writes. */
	wmb();
	m9x_write(mdev, M9_REG_FENCE0_ADDRESS, lower_32_bits(t->fence_bus + 0x20) | 2);
	m9x_write(mdev, M9_REG_FENCE2_ADDRESS, lower_32_bits(t->fence_bus + 0x40) | 2);
	/* Genuine normal activation selects the virtual restore descriptor.
 * Its native identity leaves were checked separately from FAA pool6. */
	m9x_write(mdev, M9_REG_RESTORE_TAGGED, ((t->restore_base + 0x200) & ~31u) | 1);
	m9x_read(mdev, M9_REG_FENCE2_ADDRESS);
	/* Baseline follows real initialization and every pointer/counter patch,
	 * but precedes this immutable stream's first GPU publication. */
	m9_faa_observe(mdev, r, true);
	ret = m9x_submit_kernel_stream_locked(mdev,
			r->owner[M9_FAA_PRIMARY_OWNER].cpu, r->primary_bytes / 4,
			target, point, &posted, false);
	*color_active = posted && ret;
	if (!ret) {
		t->next_fence = target + 0x10;
		if (m9_faa_output_owner(r)) {
			r->output_generation = t->generation;
			r->output_point = *point;
			r->output_retired = true;
		}
		m9_faa_observe(mdev, r, false);
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
		 "M9 FAA output read: point=%#llx system=%#x descriptor=c8210140/100f0 RGBA 320x240\n",
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
