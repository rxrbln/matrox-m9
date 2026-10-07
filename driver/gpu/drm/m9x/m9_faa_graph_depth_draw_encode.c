// SPDX-License-Identifier: GPL-2.0-only
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
		const struct m9x_faa_color_input *in, u32 old_geometry,
		u32 next_geometry)
{
	/* These are source producer roles, not literal source upload offsets.
	 * Four stages read live constant spans; the retained geometry and the
	 * next owned record are separate until the final draw has retired. */
	switch (f->at) {
	case 328: return next_geometry;
	case 372: return in->layout.constant_begin[1];
	case 376: return in->layout.constant_end[1];
	case 616: case 1128: case 1568: case 2428: return old_geometry;
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

int m9x_faa_depth_draw_encode(const u8 *blob, size_t bytes,
	const struct m9x_faa_graph_draw_owner owner[M9_FAA_DEPTH_DRAW_OWNERS],
	const struct m9x_faa_depth_views *views,
	u32 primary_cursor, u32 secondary_cursor, u64 previous_unlock_point,
	const struct m9x_faa_color_input *in,
	u32 previous_geometry_offset, u32 next_geometry_offset,
	struct m9x_faa_depth_draw_image *out)
{
	u8 hash[32];
	u32 i, low, address, offset, geometry_end;

	if (!blob || bytes != M9_FAA_DEPTH_DRAW_TEMPLATE_BYTES ||
	    !owner || !out || !out->primary || !out->secondary ||
	    !source_layout(in) || !views_match(views, owner) ||
	    m9x_faa_graph_draw_owners_validate(owner))
		return -EINVAL;
	sha256(blob, bytes, hash);
	if (memcmp(hash, depth_template_sha, sizeof(hash)))
		return -EINVAL;
	low = (u32)previous_unlock_point;
	if ((previous_unlock_point & 15) || low < 0x1b0 ||
	    low > U32_MAX - 0x190 ||
	    (primary_cursor & 3) || primary_cursor < 13352 ||
	    (secondary_cursor & 3) || secondary_cursor < 2688 ||
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
	    (previous_geometry_offset & 127) ||
	    (next_geometry_offset & 127) ||
	    check_add_overflow(next_geometry_offset, 256u, &geometry_end) ||
	    geometry_end > owner[28].bytes ||
	    previous_geometry_offset > owner[28].bytes ||
	    owner[28].bytes - previous_geometry_offset < 256 ||
	    previous_geometry_offset + 256 > next_geometry_offset)
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
				previous_geometry_offset, next_geometry_offset);
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
	for (i = 0; i < ARRAY_SIZE(depth_fields); i++) {
		const struct depth_field *f = &depth_fields[i];
		u8 *dst = f->dst ? out->secondary : out->primary;

		offset = f->offset;
		if (f->owner == 29)
			offset = secondary_cursor + offset - 2688;
		else if (f->owner == 28)
			offset = constants_offset(f, in,
				previous_geometry_offset, next_geometry_offset);
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

		write32(dst + e->at, (low + e->delta) | e->tag);
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
	out->final_point = previous_unlock_point + 0x190;
	return 0;
}
