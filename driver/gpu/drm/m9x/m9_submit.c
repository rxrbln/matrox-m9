// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/overflow.h>
#include <linux/uaccess.h>

#include <drm/drm_file.h>
#include <drm/drm_gem.h>

#include "m9_drv.h"
#include "m9_reg.h"

/*
 * This is deliberately the captured-triangle whitelist, not all 256 indexed
 * registers. General execution remains disabled while the masks for these
 * state words are being reduced.
 */
static bool m9_index_allowed(u8 index)
{
	switch (index) {
	case 0x00 ... 0x03:
	case 0x10: case 0x11: case 0x18: case 0x19:
	case 0x22: case 0x23: case 0x28: case 0x30:
	case 0x68 ... 0x6c:
	case 0x70 ... 0x7a:
	case 0x88 ... 0x8a: case 0x8c ... 0x9f:
	case 0xa8 ... 0xaf:
	case 0xb0 ... 0xbb: case 0xbe:
	case 0xc1 ... 0xc5:
	case 0xd7: case 0xea ... 0xec: case 0xef ... 0xf1:
	case 0xf4 ... 0xf6:
		return true;
	default:
		return false;
	}
}

static int m9_index_reloc_kind(u8 index)
{
	switch (index) {
	case 0x00 ... 0x03:
		return PARHELIA_M9_RELOC_SPECIAL;
	case 0x69:
		return PARHELIA_M9_RELOC_RESOURCE_69;
	case 0x6c:
		return PARHELIA_M9_RELOC_RESOURCE_6C;
	case 0x78:
		return PARHELIA_M9_RELOC_TEXTURE;
	case 0xb0: case 0xb2: case 0xb4: case 0xb6:
		return PARHELIA_M9_RELOC_COLOR;
	case 0xb8: case 0xba:
		return PARHELIA_M9_RELOC_DEPTH;
	default:
		return -1;
	}
}

static bool m9_draw_header(u32 header, u32 *family)
{
	u32 launch = header & 0x60000000;
	u32 base = header & ~0x60000000;

	if (launch != 0x20000000 && launch != 0x60000000)
		return false;
	if (base != 0x03000001 && base != 0x06000001 && base != 0x07000001)
		return false;
	*family = base;
	return true;
}

static int m9_parse_commands(const u32 *words, u32 count, u8 *expected)
{
	u32 cursor = 0;

	while (cursor < count) {
		u32 header = words[cursor++], family;
		u8 index[4];
		unsigned int values = 4, i;

		if (header == 0x45454545)
			continue;
		if (m9_draw_header(header, &family)) {
			if (count - cursor < 2)
				return -EINVAL;
			if (family == 0x03000001) {
				if (words[cursor + 1] < words[cursor])
					return -EINVAL;
			} else if (family == 0x07000001) {
				expected[cursor] = PARHELIA_M9_RELOC_DRAW32_START + 1;
				expected[cursor + 1] = PARHELIA_M9_RELOC_DRAW32_END + 1;
			} else {
				expected[cursor] = PARHELIA_M9_RELOC_DRAW16_START + 1;
				expected[cursor + 1] = PARHELIA_M9_RELOC_DRAW16_END + 1;
			}
			cursor += 2;
			continue;
		}

		index[0] = header;
		index[1] = header >> 8;
		index[2] = header >> 16;
		index[3] = header >> 24;
		for (i = 0; i < 4; i++) {
			if (index[i] == M9_PACKET_STOP) {
				values = i;
				break;
			}
		}
		if (!values || count - cursor < values)
			return -EINVAL;
		for (i = values; i < 4; i++)
			if (index[i] != M9_PACKET_STOP)
				return -EINVAL;
		for (i = 0; i < values; i++) {
			int kind;

			/* Fence, dependency and launch ownership stays in the kernel. */
			if (index[i] == 0x1a || index[i] == 0x1b ||
			    index[i] == 0x1c || index[i] == 0x43 ||
			    index[i] == 0xe3 || index[i] == 0xe6 ||
			    (index[i] >= 0xc8 && index[i] <= 0xcd) ||
			    !m9_index_allowed(index[i]))
				return -EPERM;
			kind = m9_index_reloc_kind(index[i]);
			if (kind >= 0)
				expected[cursor + i] = kind + 1;
		}
		cursor += values;
	}
	return 0;
}

static struct m9x_bo *
m9_find_ref(struct m9x_bo **bos,
	    const struct drm_parhelia_m9_bo_ref *refs, u32 count, u32 handle,
	    u32 access)
{
	u32 i;

	for (i = 0; i < count; i++)
		if (refs[i].handle == handle && (refs[i].flags & access) == access)
			return bos[i];
	return NULL;
}

static u32 m9_reloc_access(u32 kind)
{
	switch (kind) {
	case PARHELIA_M9_RELOC_COLOR:
		/*
		 * Destination reads depend on blend/logic-op state.  Keep the
		 * attachment reserved for both directions until that state is part
		 * of a kernel-selected executable template.
		 */
		return PARHELIA_M9_BO_READ | PARHELIA_M9_BO_WRITE;
	case PARHELIA_M9_RELOC_DEPTH:
	case PARHELIA_M9_RELOC_RESOURCE_69:
		return PARHELIA_M9_BO_READ | PARHELIA_M9_BO_WRITE;
	case PARHELIA_M9_RELOC_TEXTURE:
	case PARHELIA_M9_RELOC_DRAW32_START:
	case PARHELIA_M9_RELOC_DRAW32_END:
	case PARHELIA_M9_RELOC_DRAW16_START:
	case PARHELIA_M9_RELOC_DRAW16_END:
	case PARHELIA_M9_RELOC_SPECIAL:
	case PARHELIA_M9_RELOC_RESOURCE_6C:
		return PARHELIA_M9_BO_READ;
	default:
		return 0;
	}
}

static int
m9_validate_draw_reloc_pairs(const u32 *image, u32 dwords,
			     const struct drm_parhelia_m9_reloc * const *by_dword)
{
	u32 cursor = 0;

	while (cursor < dwords) {
		u32 header = image[cursor++], family;
		u8 index[4];
		unsigned int values = 4, i;

		if (header == 0x45454545)
			continue;
		if (m9_draw_header(header, &family)) {
			const struct drm_parhelia_m9_reloc *start, *end;

			if (dwords - cursor < 2)
				return -EINVAL;
			if (family == 0x03000001) {
				cursor += 2;
				continue;
			}
			start = by_dword[cursor];
			end = by_dword[cursor + 1];
			if (!start || !end || start->handle != end->handle ||
			    end->target_offset < start->target_offset)
				return -EINVAL;
			if (family == 0x07000001) {
				if (start->kind != PARHELIA_M9_RELOC_DRAW32_START ||
				    end->kind != PARHELIA_M9_RELOC_DRAW32_END ||
				    (start->target_offset | end->target_offset) & 3)
					return -EINVAL;
			} else {
				if (start->kind != PARHELIA_M9_RELOC_DRAW16_START ||
				    end->kind != PARHELIA_M9_RELOC_DRAW16_END ||
				    (start->target_offset | end->target_offset) & 3 ||
				    ((end->flags & PARHELIA_M9_RELOC_PACKED16_ODD) &&
				     end->target_offset == start->target_offset))
					return -EINVAL;
			}
			cursor += 2;
			continue;
		}
		index[0] = header;
		index[1] = header >> 8;
		index[2] = header >> 16;
		index[3] = header >> 24;
		for (i = 0; i < 4; i++)
			if (index[i] == M9_PACKET_STOP) {
				values = i;
				break;
			}
		if (!values || dwords - cursor < values)
			return -EINVAL;
		cursor += values;
	}
	return 0;
}

static u32 m9_reloc_alignment(u32 kind)
{
	switch (kind) {
	case PARHELIA_M9_RELOC_COLOR:
	case PARHELIA_M9_RELOC_DEPTH:
	case PARHELIA_M9_RELOC_TEXTURE:
		return 256;
	case PARHELIA_M9_RELOC_RESOURCE_69:
		return 4096;
	case PARHELIA_M9_RELOC_RESOURCE_6C:
		return 128;
	default:
		return 4;
	}
}

static int
m9_validate_relocs(u32 *image, u32 dwords, u8 *expected,
		   const struct drm_parhelia_m9_reloc *relocs, u32 reloc_count,
		   struct m9x_bo **bos,
		   const struct drm_parhelia_m9_bo_ref *refs, u32 bo_count)
{
	const struct drm_parhelia_m9_reloc **by_dword;
	u32 *required_access;
	u32 i;
	int ret = 0;

	by_dword = kcalloc(dwords, sizeof(*by_dword), GFP_KERNEL);
	required_access = kcalloc(bo_count, sizeof(*required_access), GFP_KERNEL);
	if (!by_dword || !required_access) {
		ret = -ENOMEM;
		goto out;
	}

	for (i = 0; i < reloc_count; i++) {
		const struct drm_parhelia_m9_reloc *reloc = &relocs[i];
		struct m9x_bo *bo;
		u64 address;
		u32 alignment;
		u32 access, permitted_flags;
		u32 bo_index;
		bool end_reloc;

		access = m9_reloc_access(reloc->kind);
		permitted_flags = access;
		if (reloc->kind == PARHELIA_M9_RELOC_DRAW16_END)
			permitted_flags |= PARHELIA_M9_RELOC_PACKED16_ODD;
		if (reloc->command_dword >= dwords ||
		    !expected[reloc->command_dword] ||
		    expected[reloc->command_dword] - 1 != reloc->kind ||
		    reloc->kind > PARHELIA_M9_RELOC_RESOURCE_6C ||
		    reloc->kind == PARHELIA_M9_RELOC_FENCE ||
		    !access || reloc->flags & ~permitted_flags ||
		    (reloc->flags & (PARHELIA_M9_BO_READ |
				      PARHELIA_M9_BO_WRITE)) != access ||
		    by_dword[reloc->command_dword] ||
		    reloc->pad[0] || reloc->pad[1]) {
			ret = -EINVAL;
			goto out;
		}
		bo = m9_find_ref(bos, refs, bo_count, reloc->handle, access);
		end_reloc = reloc->kind == PARHELIA_M9_RELOC_DRAW32_END ||
			    reloc->kind == PARHELIA_M9_RELOC_DRAW16_END;
		if (!bo || reloc->target_offset > bo->dma.base.size ||
		    (!end_reloc && reloc->target_offset == bo->dma.base.size)) {
			ret = -EINVAL;
			goto out;
		}
		for (bo_index = 0; bo_index < bo_count; bo_index++)
			if (bos[bo_index] == bo)
				break;
		if (bo_index == bo_count) {
			ret = -EINVAL;
			goto out;
		}
		required_access[bo_index] |= access;
		alignment = m9_reloc_alignment(reloc->kind);
		if (reloc->target_offset & (alignment - 1)) {
			ret = -EINVAL;
			goto out;
		}
		if (check_add_overflow((u64)bo->dma.dma_addr,
				       reloc->target_offset, &address) ||
		    address > U32_MAX) {
			ret = -EOVERFLOW;
			goto out;
		}
		if (reloc->kind == PARHELIA_M9_RELOC_REQUESTER ||
		    reloc->kind == PARHELIA_M9_RELOC_DRAW32_START ||
		    reloc->kind == PARHELIA_M9_RELOC_DRAW32_END ||
		    reloc->kind == PARHELIA_M9_RELOC_DRAW16_START ||
		    reloc->kind == PARHELIA_M9_RELOC_DRAW16_END)
			address |= M9_COMMAND_SYSTEM_TAG;
		else if (reloc->kind == PARHELIA_M9_RELOC_RESOURCE_6C)
			address |= 1;
		if (reloc->kind == PARHELIA_M9_RELOC_DRAW16_END &&
		    reloc->flags & PARHELIA_M9_RELOC_PACKED16_ODD)
			address |= 2;
		image[reloc->command_dword] = lower_32_bits(address);
		by_dword[reloc->command_dword] = reloc;
		expected[reloc->command_dword] = 0;
	}
	for (i = 0; i < dwords; i++)
		if (expected[i]) {
			ret = -EINVAL;
			goto out;
		}
	for (i = 0; i < bo_count; i++)
		if (refs[i].flags != required_access[i]) {
			ret = -EINVAL;
			goto out;
		}
	ret = m9_validate_draw_reloc_pairs(image, dwords, by_dword);
out:
	kfree(required_access);
	kfree(by_dword);
	return ret;
}

static void m9_put_bos(struct m9x_bo **bos, u32 count)
{
	u32 i;

	if (!bos)
		return;
	for (i = 0; i < count; i++)
		if (bos[i])
			drm_gem_object_put(&bos[i]->dma.base);
}

static int m9_submit_validate(struct m9x_device *mdev,
			      struct drm_file *file,
			      const struct drm_parhelia_m9_submit *args)
{
	struct drm_parhelia_m9_reloc *relocs = NULL;
	struct drm_parhelia_m9_bo_ref *refs = NULL;
	struct m9x_bo **bos = NULL;
	struct m9x_bo *command_bo;
	u8 *expected = NULL;
	u32 *image = NULL;
	u64 command_end;
	u32 dwords, i;
	int ret = 0;

	if (!args->command_handle || !args->command_length ||
	    args->command_length > PARHELIA_M9_COMMAND_BYTES ||
	    args->command_length & 3 ||
	    args->relocation_count > PARHELIA_M9_MAX_RELOCS ||
	    args->bo_count > PARHELIA_M9_MAX_BOS ||
	    (!!args->bo_count != !!args->bos_ptr) ||
	    (!!args->relocation_count != !!args->relocations_ptr))
		return -EINVAL;
	command_bo = m9x_bo_lookup(mdev, file, args->command_handle);
	if (IS_ERR(command_bo))
		return PTR_ERR(command_bo);
	if (!(command_bo->flags & PARHELIA_M9_BO_COMMAND) ||
	    check_add_overflow(args->command_offset, (u64)args->command_length,
			       &command_end) || command_end > command_bo->dma.base.size) {
		ret = -EINVAL;
		goto out_command;
	}
	dwords = args->command_length / sizeof(u32);
	image = kmemdup((u8 *)command_bo->dma.vaddr + args->command_offset,
			args->command_length, GFP_KERNEL);
	expected = kcalloc(dwords, sizeof(*expected), GFP_KERNEL);
	if (args->bo_count)
		refs = memdup_user(u64_to_user_ptr(args->bos_ptr),
				   array_size(args->bo_count, sizeof(*refs)));
	if (!image || !expected) {
		ret = -ENOMEM;
		goto out;
	}
	if (IS_ERR(refs)) {
		ret = PTR_ERR(refs);
		refs = NULL;
		goto out;
	}
	if (args->relocation_count) {
		relocs = memdup_user(u64_to_user_ptr(args->relocations_ptr),
				     array_size(args->relocation_count,
						sizeof(*relocs)));
		if (IS_ERR(relocs)) {
			ret = PTR_ERR(relocs);
			relocs = NULL;
			goto out;
		}
	}
	if (args->bo_count) {
		bos = kcalloc(args->bo_count, sizeof(*bos), GFP_KERNEL);
		if (!bos) {
			ret = -ENOMEM;
			goto out;
		}
	}
	for (i = 0; i < args->bo_count; i++) {
		u32 j;

		if (!refs[i].handle || !refs[i].flags ||
		    refs[i].flags & ~(PARHELIA_M9_BO_READ | PARHELIA_M9_BO_WRITE)) {
			ret = -EINVAL;
			goto out;
		}
		for (j = 0; j < i; j++)
			if (refs[j].handle == refs[i].handle) {
				ret = -EINVAL;
				goto out;
			}
		bos[i] = m9x_bo_lookup(mdev, file, refs[i].handle);
		if (IS_ERR(bos[i])) {
			ret = PTR_ERR(bos[i]);
			bos[i] = NULL;
			goto out;
		}
	}
	ret = m9_parse_commands(image, dwords, expected);
	if (!ret)
		ret = m9_validate_relocs(image, dwords, expected, relocs,
					 args->relocation_count, bos, refs,
					 args->bo_count);
	/*
	 * The patched image is intentionally discarded. A later, independently
	 * gated triangle path will copy it to coherent command memory only after
	 * register masks and BO reservation fences are complete.
	 */
out:
	m9_put_bos(bos, args->bo_count);
	kfree(bos);
	kfree(relocs);
	kfree(refs);
	kfree(expected);
	kfree(image);
out_command:
	drm_gem_object_put(&command_bo->dma.base);
	return ret;
}

int m9x_submit_ioctl(struct drm_device *drm, void *data,
		     struct drm_file *file)
{
	struct drm_parhelia_m9_submit *args = data;
	struct m9x_device *mdev = to_m9x(drm);
	u32 flags = args->flags;

	if (!m9x_enabled())
		return -ENODEV;
	if (args->context_id || args->out_syncobj || args->pad || args->out_point ||
	    flags & ~(PARHELIA_M9_SUBMIT_VALIDATE_ONLY |
		      PARHELIA_M9_SUBMIT_NOOP_FENCE))
		return -EINVAL;
	if (flags == PARHELIA_M9_SUBMIT_NOOP_FENCE) {
		if (args->command_handle || args->command_offset ||
		    args->command_length || args->relocation_count ||
		    args->relocations_ptr || args->bo_count || args->bos_ptr)
			return -EINVAL;
		return m9x_submit_noop(mdev, &args->out_point);
	}
	if (flags != PARHELIA_M9_SUBMIT_VALIDATE_ONLY)
		return -EOPNOTSUPP;
	return m9_submit_validate(mdev, file, args);
}

int m9x_submit_selftest(void)
{
	u32 good[] = { 0x45454522, 0x00006403,
		       0x23000001, 0, 3, 0x45454545 };
	u32 privileged[] = { 0x454545e3, 0x11 };
	u32 private_program[] = { 0x454545c8, 0 };
	u32 state_af[] = { 0x454545af, 0 };
	u32 draw32[] = { 0x27000001, 0, 0 };
	u32 draw16[] = { 0x26000001, 0, 0 };
	u32 truncated[] = { 0x45451011, 1 };
	u8 expected[ARRAY_SIZE(good)] = {};
	struct drm_parhelia_m9_reloc draw_relocs[2] = {
		{
			.command_dword = 1,
			.handle = 7,
			.target_offset = 0,
			.kind = PARHELIA_M9_RELOC_DRAW32_START,
			.flags = PARHELIA_M9_BO_READ,
		}, {
			.command_dword = 2,
			.handle = 7,
			.target_offset = 16,
			.kind = PARHELIA_M9_RELOC_DRAW32_END,
			.flags = PARHELIA_M9_BO_READ,
		},
	};
	const struct drm_parhelia_m9_reloc *by_dword[3] = {
		NULL, &draw_relocs[0], &draw_relocs[1],
	};

	if (m9_reloc_access(PARHELIA_M9_RELOC_COLOR) !=
	    (PARHELIA_M9_BO_READ | PARHELIA_M9_BO_WRITE) ||
	    m9_reloc_access(PARHELIA_M9_RELOC_DEPTH) !=
	    (PARHELIA_M9_BO_READ | PARHELIA_M9_BO_WRITE) ||
	    m9_reloc_access(PARHELIA_M9_RELOC_TEXTURE) !=
	    PARHELIA_M9_BO_READ ||
	    m9_reloc_access(PARHELIA_M9_RELOC_FENCE))
		return -EINVAL;
	if (m9_parse_commands(good, ARRAY_SIZE(good), expected))
		return -EINVAL;
	memset(expected, 0, sizeof(expected));
	if (m9_parse_commands(privileged, ARRAY_SIZE(privileged), expected) != -EPERM)
		return -EINVAL;
	memset(expected, 0, sizeof(expected));
	if (m9_parse_commands(private_program, ARRAY_SIZE(private_program),
			      expected) != -EPERM)
		return -EINVAL;
	memset(expected, 0, sizeof(expected));
	if (m9_parse_commands(state_af, ARRAY_SIZE(state_af), expected))
		return -EINVAL;
	memset(expected, 0, sizeof(expected));
	if (m9_parse_commands(truncated, ARRAY_SIZE(truncated), expected) != -EINVAL)
		return -EINVAL;
	memset(expected, 0, sizeof(expected));
	if (m9_parse_commands(draw32, ARRAY_SIZE(draw32), expected) ||
	    m9_validate_draw_reloc_pairs(draw32, ARRAY_SIZE(draw32), by_dword))
		return -EINVAL;
	draw_relocs[1].target_offset = 0;
	draw_relocs[0].target_offset = 16;
	if (m9_validate_draw_reloc_pairs(draw32, ARRAY_SIZE(draw32), by_dword) !=
	    -EINVAL)
		return -EINVAL;
	draw_relocs[0].target_offset = 0;
	draw_relocs[1].target_offset = 4;
	draw_relocs[0].kind = PARHELIA_M9_RELOC_DRAW16_START;
	draw_relocs[1].kind = PARHELIA_M9_RELOC_DRAW16_END;
	draw_relocs[1].flags |= PARHELIA_M9_RELOC_PACKED16_ODD;
	if (m9_validate_draw_reloc_pairs(draw16, ARRAY_SIZE(draw16), by_dword))
		return -EINVAL;
	draw_relocs[1].target_offset = 0;
	if (m9_validate_draw_reloc_pairs(draw16, ARRAY_SIZE(draw16), by_dword) !=
	    -EINVAL)
		return -EINVAL;
	return 0;
}
