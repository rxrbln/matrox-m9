// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* Original M9128 retained bound-depth LESS/write draw and normal Lock.
 * This bounded encoder emits only the suffix after a separately qualified
 * attach-and-clear prefix; it never posts commands or claims retirement. */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <crypto/sha2.h>
#include "m9_faa_graph_depth_draw_encode.h"
#include "m9_faa_graph_depth_draw_fields.h"

/* Template guard band setup words (original 13b900 field). */
static const u16 guard_at[] = { 76, 764, 1252, 1680 };
static const u8 depth_template_sha[32] = {
	0xa4,0xb2,0x07,0xb4,0xa5,0x93,0xe1,0x7f,
	0x1e,0xba,0xdf,0x16,0xb2,0xe3,0x12,0x4f,
	0x04,0x68,0x1c,0x33,0x66,0x37,0xdd,0x6a,
	0x66,0xdb,0x68,0x89,0x9b,0xb0,0x39,0xdb,
};
static const u16 depth_end_delta[M9_FAA_DEPTH_DRAW_ENDS] = {
	4212, 4212, 4240, 4300, 7860, 7860, 7872,
};
static const u16 depth_dimension_primary[] = {648, 4464, 7856};
static const u16 depth_dimension_secondary[] = {2000, 2008};
static const u16 depth_entry_at[4] = {2100, 352, 880, 1372};
static const u16 depth_pair_at[4] = {2116, 368, 896, 1388};
static const u32 depth_source_declaration[5] = {
	0x00100d88, 0x001006b6, 0x04200d88, 0x042006b6, 0x00004b08,
};

static u32 read32(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 |
	       (u32)p[3] << 24;
}

static void write32(u8 *p, u32 value)
{
	p[0] = value;
	p[1] = value >> 8;
	p[2] = value >> 16;
	p[3] = value >> 24;
}

static u32 constants_offset(const struct depth_field *f,
		const struct m9x_faa_color_input *in, u32 geometry_header,
		u32 vertex_records)
{
	/* These are source producer roles, not literal source upload offsets.
	 * Four stages read live constant spans. The viewport/header is 128 B;
	 * the raw vertex records may follow it immediately or after padding. */
	switch (f->at) {
	case 328: return vertex_records;
	case 372: return in->layout.constant_begin[1];
	case 376: return in->layout.constant_end[1];
	case 616: case 1128: case 1568: case 2428: return geometry_header;
	case 900: return in->layout.constant_begin[2];
	case 904: return in->layout.constant_end[2];
	case 1392: return in->layout.constant_begin[3];
	case 1396: return in->layout.constant_end[3];
	case 2120: return 0; /* utility prefix plus fragment constant */
	case 2124: return in->layout.constant_end[0];
	default: return U32_MAX;
	}
}

static bool source_layout(const struct m9x_faa_color_input *in)
{
	static const u32 entry[] = {0, 32, 88, 128};
	static const u32 begin[] = {16, 32, 48, 48};
	static const u32 end[] = {32, 48, 48, 152};
	u32 i;

	if (!in || !in->program || !in->constants || !in->geometry ||
	    !in->declaration || in->layout.program_count != 4 ||
	    in->layout.program_bytes != 328 ||
	    in->layout.constants_bytes != 152 ||
	    in->layout.geometry_bytes != 224 ||
	    in->layout.vertex_offset != 128 || in->layout.vertex_bytes != 96 ||
	    in->record_stride != 32 || in->record_count != 3 ||
	    in->declaration_count != 5)
		return false;
	for (i = 0; i < 4; i++)
		if (in->layout.entry_bytes[i] != entry[i] ||
		    in->layout.constant_begin[i] != begin[i] ||
		    in->layout.constant_end[i] != end[i])
			return false;
	for (i = 0; i < 5; i++)
		if (read32(in->declaration + i * 4) !=
		    depth_source_declaration[i])
			return false;
	for (i = 0; i < in->layout.vertex_bytes / 4; i++)
		if ((read32(in->geometry + in->layout.vertex_offset + i * 4) &
		     0x7f800000u) == 0x7f800000u)
			return false;
	return true;
}

bool m9x_faa_depth_draw_input_valid(const struct m9x_faa_color_input *in)
{
 return source_layout(in);
}

static bool views_match(const struct m9x_faa_depth_views *views,
	const struct m9x_faa_graph_draw_owner owner[M9_FAA_DEPTH_DRAW_OWNERS])
{
	u32 i, j;

	if (!views || views->color_policy != M9_FAA_DEPTH_COLOR_SYSTEM_B ||
	    views->depth_aux_role != M9_FAA_DEPTH_HZ ||
	    views->aux_depth_role != M9_FAA_BOUND_DEPTH)
		return false;
	for (i = 0; i < M9_FAA_DEPTH_OWNERS; i++) {
		struct m9x_faa_depth_view expected =
			m9x_faa_bound_depth_profile(i);
		const struct m9x_faa_depth_view *view = &views->owner[i];

		expected.base = view->base;
		if (memcmp(&expected, view, sizeof(expected)) ||
		    owner[31 + i].base != view->base ||
		    owner[31 + i].bytes != view->capacity_bytes ||
		    !view->base || (view->base & 4095) ||
		    (u64)view->base + view->capacity_bytes > U32_MAX ||
		    (i ? (u64)view->base + view->capacity_bytes > 0x40000000ull :
			view->base < 0x40000000u))
			return false;
		/* The three attachments are newly retained owners. The source
		 * compares their identities; aliasing any existing graph owner
		 * would invalidate the command-resource lifetime. */
		for (j = 0; j < 31 + i; j++)
			if ((u64)owner[j].base <
			    (u64)view->base + view->capacity_bytes &&
			    (u64)view->base <
			    (u64)owner[j].base + owner[j].bytes)
				return false;
	}
	return true;
}

static int depth_encode_impl(const u8 *blob, size_t bytes,
	const struct m9x_faa_graph_draw_owner owner[M9_FAA_DEPTH_DRAW_OWNERS],
	const struct m9x_faa_depth_views *views,
	u32 primary_cursor, u32 secondary_cursor, u64 previous_unlock_point,
	u32 clear_flags,
	const struct m9x_faa_color_input *in,
	u32 geometry_header_offset, u32 vertex_record_offset,
	struct m9x_faa_depth_draw_image *out, bool live_color)
{
	u8 hash[32];
	u32 i, low, address, offset, geometry_header_end, vertex_end, raw_bytes;

	if (!blob || bytes != M9_FAA_DEPTH_DRAW_TEMPLATE_BYTES ||
	    !owner || !out || !out->primary || !out->secondary ||
	    clear_flags > 1 ||
	    ((!live_color && !source_layout(in)) ||
	    (live_color && (!in || in->layout.program_count != 4 ||
		in->record_count > 12288 ||
		(in->record_stride != 32 &&
		 !(in->record_stride == 16 &&
		   in->declaration_count == 3))))) ||
	    !views_match(views, owner) ||
	    m9x_faa_graph_draw_owners_validate(owner))
		return -EINVAL;
	sha256(blob, bytes, hash);
	if (memcmp(hash, depth_template_sha, sizeof(hash)))
		return -EINVAL;
	raw_bytes = ((in->record_count + 3u) & ~3u) * in->record_stride;
	low = (u32)previous_unlock_point;
	if ((previous_unlock_point & 15) || low < 0x10 ||
	    low > U32_MAX - (clear_flags ? 0x190 : 0x130) ||
	    (primary_cursor & 3) ||
	    (secondary_cursor & 3) ||
	    out->primary_capacity < M9_FAA_DEPTH_DRAW_PRIMARY_BYTES ||
	    out->secondary_capacity < M9_FAA_DEPTH_DRAW_SECONDARY_BYTES ||
	    primary_cursor > owner[27].bytes ||
	    owner[27].bytes - primary_cursor < M9_FAA_DEPTH_DRAW_PRIMARY_BYTES ||
	    secondary_cursor > owner[29].bytes ||
	    owner[29].bytes - secondary_cursor <
		M9_FAA_DEPTH_DRAW_SECONDARY_BYTES ||
	    check_add_overflow(owner[27].base, primary_cursor +
		M9_FAA_DEPTH_DRAW_PRIMARY_BYTES, &address) ||
	    check_add_overflow(owner[29].base, secondary_cursor +
		M9_FAA_DEPTH_DRAW_SECONDARY_BYTES, &address) ||
	    (geometry_header_offset & 127) ||
	    (vertex_record_offset & 127) ||
	    geometry_header_offset <
		((in->layout.constants_bytes + 127u) & ~127u) ||
	    check_add_overflow(geometry_header_offset, 128u,
		&geometry_header_end) ||
	    check_add_overflow(vertex_record_offset, raw_bytes, &vertex_end) ||
	    vertex_record_offset < geometry_header_end ||
	    vertex_end > owner[28].bytes)
		return -ERANGE;
	/* Audit every live base/offset/tag and domain before writing output. */
	for (i = 0; i < ARRAY_SIZE(depth_fields); i++) {
		const struct depth_field *f = &depth_fields[i];
		const struct m9x_faa_graph_draw_owner *src = &owner[f->owner];

		offset = f->offset;
		if (f->owner == 29)
			offset = secondary_cursor + offset - 2688;
		else if (f->owner == 28)
			offset = constants_offset(f, in,
				geometry_header_offset, vertex_record_offset);
		if (offset == U32_MAX || offset > src->bytes ||
		    src->bytes - offset < 4 ||
		    check_add_overflow(src->base, offset, &address) ||
		    (address & 3) ||
		    (f->writer == 0x248a0 && (address & 4095)))
			return -ERANGE;
	}
	/* The fixed SYSTEM virtual root cells are native requester mappings. The
	 * caller must admit both 0x40004000 and 0x40008000 before any post. */
	memcpy(out->primary, blob, M9_FAA_DEPTH_DRAW_PRIMARY_BYTES);
	memcpy(out->secondary, blob + M9_FAA_DEPTH_DRAW_PRIMARY_BYTES,
	       M9_FAA_DEPTH_DRAW_SECONDARY_BYTES);
	for (i = 0; i < ARRAY_SIZE(guard_at); i++)
		m9x_faa_guard_field_put(out->secondary + guard_at[i],
			m9x_faa_guard_field_size(
				views->owner[M9_FAA_BOUND_COLOR].width,
				views->owner[M9_FAA_BOUND_COLOR].height));
	for (i = 0; i < ARRAY_SIZE(depth_fields); i++) {
		const struct depth_field *f = &depth_fields[i];
		u8 *dst = f->dst ? out->secondary : out->primary;

		offset = f->offset;
		if (f->owner == 29)
			offset = secondary_cursor + offset - 2688;
		else if (f->owner == 28)
			offset = constants_offset(f, in,
				geometry_header_offset, vertex_record_offset);
		address = owner[f->owner].base + offset;
		if (f->writer == 0x248a0)
			address = (address & 0xfffff000u) | f->tag;
		else if (f->writer == 0x2490e)
			address = (address | f->tag) & ~3u;
		else
			address |= f->tag;
		write32(dst + f->at, address);
	}
	for (i = 0; i < ARRAY_SIZE(depth_epochs); i++) {
		const struct depth_epoch *e = &depth_epochs[i];
		u8 *dst = e->dst ? out->secondary : out->primary;

		write32(dst + e->at,
			(low + e->delta - (clear_flags ? 0 : 0x60)) | e->tag);
	}
	for (i = 0; i < ARRAY_SIZE(depth_relatives); i++)
		write32(out->secondary + depth_relatives[i].at,
			0x49d00000u |
			((depth_relatives[i].target - depth_relatives[i].at) / 4));
	for (i = 0; i < ARRAY_SIZE(depth_dimension_primary); i++)
		write32(out->primary + depth_dimension_primary[i],
			(views->owner[M9_FAA_BOUND_DEPTH].height << 16) |
			views->owner[M9_FAA_BOUND_DEPTH].width);
	for (i = 0; i < ARRAY_SIZE(depth_dimension_secondary); i++)
		write32(out->secondary + depth_dimension_secondary[i],
			(views->owner[M9_FAA_BOUND_DEPTH].height << 16) |
			views->owner[M9_FAA_BOUND_DEPTH].width);
	for (i = 0; i < 4; i++) {
		u32 pairs = (in->layout.constant_end[i] -
			     in->layout.constant_begin[i]) / 8;

		if (!i) pairs = in->layout.constant_end[0] / 8;
		write32(out->secondary + depth_entry_at[i],
			in->layout.entry_bytes[i] / 8);
		write32(out->secondary + depth_pair_at[i],
			0x00500000u | (pairs << 8));
	}
	for (i = 0; i < M9_FAA_DEPTH_DRAW_ENDS; i++)
		out->end_bytes[i] =
			primary_cursor + depth_end_delta[i];
	out->final_point = previous_unlock_point +
		(clear_flags ? 0x190 : 0x130);
	return 0;
}

int m9x_faa_depth_draw_encode(const u8 *blob, size_t bytes,
	const struct m9x_faa_graph_draw_owner owner[M9_FAA_DEPTH_DRAW_OWNERS],
	const struct m9x_faa_depth_views *views,
	u32 primary_cursor, u32 secondary_cursor, u64 previous_unlock_point,
	u32 clear_flags, const struct m9x_faa_color_input *in,
	u32 geometry_header_offset, u32 vertex_record_offset,
	struct m9x_faa_depth_draw_image *out)
{
	return depth_encode_impl(blob, bytes, owner, views, primary_cursor,
		secondary_cursor, previous_unlock_point, clear_flags, in,
		geometry_header_offset, vertex_record_offset, out, false);
}

int m9x_faa_depth_color_encode(const u8 *depth_blob, size_t depth_bytes,
	const u8 *first_blob, size_t first_bytes,
	const struct m9x_faa_graph_draw_owner owner[M9_FAA_DEPTH_DRAW_OWNERS],
	const struct m9x_faa_depth_views *views,
	u32 init_bytes, u32 primary_cursor, u32 secondary_cursor,
	u64 live_next_epoch, u32 clear_flags,
	const struct m9x_faa_color_input *in,
	u32 geometry_header_offset, u32 vertex_record_offset,
	struct m9x_faa_depth_draw_image *out,
	struct m9x_faa_color_image *first)
{
	/* Exact source first-to-bound COLOR4 packet slices. The first image is
	 * generated from live program/constants/geometry and actual owners. */
	static const struct { u16 from, end, to; } span[] = {
		{0, 844, 0}, {944, 1212, 844}, {1236, 1440, 1112},
		{1516, 1536, 1316}, {1556, 1772, 1336},
		{1796, 2712, 1552},
	};
	u32 i, first_length, end;
	int ret;

	/* This candidate admits the source-matched two-declaration-group shape;
	 * one real gather output is accepted by the live first encoder. */
	if (!owner || !in || (in->depth_flags && !m9x_faa_depth_flags_valid(in->depth_flags)) || !out || !first || !first->secondary ||
	    !first->secondary_length || first->secondary == out->secondary ||
	    in->declaration_count != 5 || clear_flags != 1 ||
	    geometry_header_offset !=
		((in->layout.constants_bytes + 127u) & ~127u) ||
	    vertex_record_offset != geometry_header_offset + 128u ||
	    out->secondary_capacity < M9_FAA_DEPTH_COLOR_SECONDARY_BYTES ||
	    secondary_cursor > owner[29].bytes ||
	    owner[29].bytes - secondary_cursor <
		M9_FAA_DEPTH_COLOR_SECONDARY_BYTES ||
	    (u64)owner[29].base + secondary_cursor +
		M9_FAA_DEPTH_COLOR_SECONDARY_BYTES > U32_MAX)
		return -EINVAL;
	ret = m9x_faa_color_four_encode(first_blob, first_bytes, owner,
		init_bytes, live_next_epoch, in, first);
	if (ret)
		return ret;
	first_length = *first->secondary_length;
	if (first_length != 2712)
		return -EINVAL;
	ret = depth_encode_impl(depth_blob, depth_bytes, owner, views,
		primary_cursor, secondary_cursor, live_next_epoch, clear_flags,
		in, geometry_header_offset, vertex_record_offset, out, true);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(span); i++)
		memcpy(out->secondary + span[i].to,
		       first->secondary + span[i].from,
		       span[i].end - span[i].from);
	/* The original depth suffix's 13 GPU timeline fields, two relative
	 * branches, bound depth descriptor and LESS/write state supersede the
	 * first-only values. Every location is source-differential verified. */
	for (i = 0; i < ARRAY_SIZE(depth_epochs); i++) {
		const struct depth_epoch *e = &depth_epochs[i];
		if (e->dst != 1)
			continue;
		write32(out->secondary + e->at,
			((u32)live_next_epoch + e->delta -
			 (clear_flags ? 0 : 0x60)) | e->tag);
	}
	for (i = 0; i < ARRAY_SIZE(depth_relatives); i++)
		write32(out->secondary + depth_relatives[i].at,
			0x49d00000u |
			((depth_relatives[i].target - depth_relatives[i].at) / 4));
	/* b5030's bound-depth producer clears the first-stage standalone
	 * target bit while preserving the live COLOR route/arity bit. */
	write32(out->secondary + 1676,
		read32(first->secondary + 1920) & ~0x20000000u);
	write32(out->secondary + 1832, owner[33].base | 1u);
	write32(out->secondary + 1956, m9x_faa_depth_a8(in->depth_flags));
	/* Four primary indirect words refer to the live secondary slice. The
	 * final target grows by 24 bytes for the COLOR raster declaration. */
	end = secondary_cursor + M9_FAA_DEPTH_COLOR_SECONDARY_BYTES;
	write32(out->primary + 4296, (owner[29].base + end) | 2u);
	return 0;
}
