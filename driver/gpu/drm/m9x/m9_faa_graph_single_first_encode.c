// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* Source-qualified M9128 FIRST-depth COLOR0*TEX0 command producer. */
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/overflow.h>
#include <crypto/sha2.h>
#include "m9_cmd.h"
#include "m9_faa_graph_single_first_encode.h"
#include "m9_faa_graph_vertex_banks.h"

/* Owner roles as in the dual table: 31 is the bound depth; owner 28
 * offsets: VERTEX raw records, GEO header.
 * Owner 23 offsets are bank indices.
 */
#define VERTEX 0xffffffffu
#define GEO 0xfffffffeu

struct addr_field {
	u16 at;
	u8 owner, tag;
	u32 offset;
};

static const struct addr_field addresses[] = {
	{ 116, 24, 1, 0 },   { 132, 24, 1, 0 },	   { 148, 24, 1, 0 },
	{ 328, 28, 1, VERTEX }, { 584, 12, 1, 0 }, { 616, 28, 1, GEO },
	{ 804, 23, 1, 0 },   { 820, 23, 1, 1 },	   { 836, 23, 1, 4 },
	{ 1032, 24, 1, 0 },  { 1060, 24, 1, 0 },   { 1220, 12, 1, 0 },
	{ 1252, 28, 1, GEO }, { 1416, 23, 1, 2 },  { 1432, 23, 1, 3 },
	{ 1448, 23, 1, 4 },  { 1644, 24, 1, 0 },   { 1780, 12, 1, 0 },
	{ 1812, 28, 1, GEO }, { 2028, 8, 1, 0 },   { 2032, 8, 0, 20 },
	{ 2060, 1, 1, 0 },   { 2076, 31, 1, 0 },   { 2108, 25, 1, 0 },
	{ 2120, 22, 1, 0 },  { 2152, 2, 1, 0 },	   { 2156, 3, 1, 0 },
	{ 2300, 23, 1, 0 },  { 2304, 23, 1, 1 },   { 2320, 23, 1, 3 },
	{ 2324, 23, 1, 4 },  { 2340, 23, 1, 2 },   { 2672, 12, 1, 0 },
	{ 2704, 28, 1, GEO }
};

struct epoch_field {
	u16 at;
	u16 delta;
	u8 tag;
};

static const struct epoch_field epochs[] = {
	{ 56, 0x90, 1 },    { 104, 0xa0, 1 },	{ 732, 0xb0, 1 },
	{ 744, 0xb0, 0 },   { 792, 0xc0, 1 },	{ 1344, 0xd0, 1 },
	{ 1356, 0xd0, 0 },  { 1404, 0xe0, 1 },	{ 1852, 0xf0, 1 },
	{ 1880, 0x100, 1 }, { 1892, 0x100, 0 }, { 1952, 0x110, 1 },
	{ 2048, 0x120, 1 }
};

static const u16 guard_at[] = { 76, 764, 1376, 1924 };
static const u16 cull_at[] = { 596, 1232, 1792, 2684 };

static const u8 template_sha[SHA256_DIGEST_SIZE] = {
	0xe5, 0xc5, 0x09, 0x2e, 0xc4, 0x11, 0x50, 0xfe, 0x46, 0x92, 0xd6,
	0xbf, 0x3b, 0xe2, 0x68, 0x78, 0x74, 0x4f, 0xa6, 0xa5, 0xf5, 0x47,
	0xaf, 0x9f, 0xe1, 0x64, 0x21, 0x01, 0x30, 0x2b, 0x38, 0x6c
};

static bool sha_ok(const u8 *data, u32 bytes, const u8 expected[32])
{
	u8 digest[32];

	sha256(data, bytes, digest);
	return !memcmp(digest, expected, 32);
}

int m9x_faa_single_first_encode(const u8 *sample, size_t sample_bytes,
				const u8 *color_template,
				size_t color_template_bytes,
				const u8 *single_template,
				size_t single_template_bytes,
				const struct m9x_faa_graph_draw_owner owner[31],
				const struct m9x_faa_depth_views *views,
				u32 init_bytes, u64 init_point,
				const struct m9x_faa_single_first_input *input,
				u8 *primary, size_t primary_capacity,
				u8 *secondary, size_t secondary_capacity,
				u32 *primary_bytes, u32 *ends,
				const struct m9x_faa_color_image *color_scratch,
				struct m9x_faa_first_depth_scratch *prefix_scratch)
{
	const u32 bytes = M9_FAA_SINGLE_FIRST_SECONDARY_BYTES;
	const u32 tail = M9_FAA_DUAL_FIRST_PRIMARY_TAIL_BYTES;
	const struct m9x_faa_color_input *c;
	struct m9x_faa_texture_dual_link_input link = { 0 };
	struct m9x_faa_texture_dual_program program = { 0 };
	struct m9x_faa_texture_fragment_iface frag;
	struct m9x_faa_surface_layout layout;
	u32 length, raw, geo, padded, base, limit, off, value, bank, i, j;
	u8 *stage;
	int ret;

	if (!sample || !color_template || !single_template || !owner ||
	    !views || !input || !input->color || !input->texture ||
	    !primary || !secondary || !primary_bytes || !ends ||
	    !color_scratch || !color_scratch->secondary ||
	    color_scratch->secondary_bytes < bytes || !prefix_scratch ||
	    single_template_bytes != bytes || secondary_capacity < bytes ||
	    primary_capacity < M9_FAA_FIRST_DEPTH_PREFIX_CAPACITY + tail ||
	    (init_point & 15) || init_point < 0xb0 ||
	    init_point > ~(u64)0 - 0x150 || (u32)init_point > 0xfffffeafu ||
	    !sha_ok(single_template, bytes, template_sha))
		return -EINVAL;
	c = input->color;
	if (!m9x_faa_surface_layout(views->owner[M9_FAA_BOUND_COLOR].width,
				    views->owner[M9_FAA_BOUND_COLOR].height,
				    &layout) ||
	    (c->raster_flags & ~6u) || (c->raster_flags & 6) == 6)
		return -EINVAL;
	if (!c->program || !c->constants || !c->geometry || !c->declaration ||
	    !c->single_base_only || c->dual_base_only ||
	    c->dual_colorpass_diagnostic || c->layout.program_count != 4 ||
	    c->layout.entry_bytes[0] ||
	    c->layout.entry_bytes[2] <= c->layout.entry_bytes[1] ||
	    c->layout.entry_bytes[3] - c->layout.entry_bytes[2] != 40 ||
	    c->layout.program_bytes < c->layout.entry_bytes[3] ||
	    c->layout.program_bytes - c->layout.entry_bytes[3] != 200 ||
	    c->declaration_count != 7 ||
	    (c->record_stride != 24 && c->record_stride != 32 &&
	     c->record_stride != 48 && c->record_stride != 64) ||
	    c->layout.constants_bytes > 16384 ||
	    c->layout.constant_begin[0] > c->layout.constant_end[0] ||
	    c->layout.constant_end[0] > c->layout.constants_bytes ||
	    c->layout.constant_begin[3] > c->layout.constant_end[3] ||
	    c->layout.constant_end[3] - c->layout.constant_begin[3] != 104 ||
	    c->layout.constant_end[3] > c->layout.constants_bytes ||
	    c->layout.vertex_offset != 128 || c->fragment_flags ||
	    !m9x_faa_depth_flags_valid(c->depth_flags) ||
	    m9x_faa_texture_fragment_check(M9_FAA_PROGRAM_SINGLE,
					   input->fragment, c->program,
					   c->layout.entry_bytes[1],
					   c->constants +
					   c->layout.constant_begin[0],
					   c->layout.constant_end[0] -
					   c->layout.constant_begin[0], &frag) ||
	    check_mul_overflow(c->record_stride, c->record_count, &raw) ||
	    raw != c->layout.vertex_bytes || c->record_count > 12288 ||
	    c->data_offset > owner[28].bytes)
		return -EINVAL;
	geo = (c->layout.constants_bytes + 127) & ~127u;
	padded = (c->record_count + 3) & ~3u;
	bank = m9x_faa_vertex_bank_stride(M9_FAA_VERTEX_BANK_EXTENT,
					  M9_FAA_SINGLE_FIRST_VERTEX_BANKS);
	if (geo > owner[28].bytes - c->data_offset ||
	    owner[28].bytes - c->data_offset - geo <
		    128 + padded * c->record_stride ||
	    !m9x_faa_vertex_banks_fit(owner[23].base, owner[23].bytes,
				      M9_FAA_SINGLE_FIRST_VERTEX_BANKS) ||
	    owner[24].bytes < 16 || owner[27].bytes < 0x6004 ||
	    owner[29].bytes < bytes ||
	    input->program_offset > owner[26].bytes ||
	    c->layout.program_bytes > owner[26].bytes - input->program_offset ||
	    (input->program_offset & 7))
		return -ERANGE;
	ret = m9x_faa_graph_draw_owners_validate(owner);
	if (ret)
		return ret;
	/* Refusing builders write caller scratch only, as in the dual FIRST. */
	ret = m9x_faa_color_four_encode(color_template, color_template_bytes,
					owner, init_bytes, init_point + 0x70, c,
					color_scratch);
	if (ret)
		return ret;
	if (*color_scratch->secondary_length != 2712)
		return -EINVAL;
	stage = color_scratch->secondary;
	/* The three-target live producer derived the declaration/carry mask. */
	value = m9_cmd_get(stage + 304);
	memcpy(stage, single_template, bytes);
	m9_cmd_put(stage + 304, value);
	m9_cmd_put(stage + 88, 0x0008000e | ((c->declaration_count - 1) << 8));
	m9_cmd_put(stage + 336, c->record_stride);
	m9_cmd_put(stage + 644, padded);
	m9_cmd_put(stage + 1264, padded);
	m9_cmd_put(stage + 1824, padded);
	m9_cmd_put(stage + 2716, c->record_count);
	for (i = 0; i < 2; i++)
		for (j = 0; j < 4; j++)
			m9_cmd_put(stage + 536 + i * 24 + j * 4,
				   j + i * 4 < c->declaration_count ?
					   m9_cmd_get(c->declaration +
						      (i * 4 + j) * 4) :
					   0);
	/* The fragment program's mode and register usage words. */
	m9_cmd_put(stage + 1916, frag.raster_f0);
	m9_cmd_put(stage + 2276, frag.usage);
	/* Inactive AC CPU scratch: zero, as in the dual FIRST. */
	m9_cmd_put(stage + 2220, 0xffff);
	m9_cmd_put(stage + 2224, 0);
	m9_cmd_put(stage + 2244, layout.height << 16 | layout.width);
	m9_cmd_put(stage + 2252, layout.height << 16 | layout.width);
	m9_cmd_put(stage + 2064, layout.primary_descriptor);
	m9_cmd_put(stage + 2080, layout.companion_descriptor);
	for (i = 0; i < 4; i++)
		m9_cmd_put(stage + 2168 + i * 4, layout.ad);
	/* Position constants k[19]/k[21] are the live x/y guard coefficients. */
	value = m9x_faa_guard_field(m9_cmd_get(c->constants +
					       c->layout.constant_begin[3] +
					       76),
				    m9_cmd_get(c->constants +
					       c->layout.constant_begin[3] +
					       84));
	for (i = 0; i < ARRAY_SIZE(guard_at); i++)
		m9x_faa_guard_field_put(stage + guard_at[i], value);
	m9_cmd_put(stage + 2200, m9x_faa_depth_a8(c->depth_flags));
	/* Culling: four 6A selectors and the raster cull field (CW 3, CCW 2). */
	for (i = 0; i < ARRAY_SIZE(cull_at); i++)
		m9_cmd_put(stage + cull_at[i],
			   c->raster_flags ? 0x28ff0000 : 0x29340000);
	m9_cmd_put(stage + 1920, 0xd8004055 |
		   (!c->raster_flags ? 0 :
		    c->raster_flags & 2 ? 0x03000000 : 0x02000000));
	for (i = 0; i < 3; i++) {
		m9_cmd_put(stage + 668 + i * 4, M9_FAA_E6_CONTROL_IMMEDIATE);
		m9_cmd_put(stage + 1288 + i * 4, M9_FAA_E6_CONTROL_IMMEDIATE);
	}
	for (i = 0; i < ARRAY_SIZE(epochs); i++)
		m9_cmd_put(stage + epochs[i].at,
			   ((u32)init_point + epochs[i].delta) | epochs[i].tag);
	for (i = 0; i < ARRAY_SIZE(addresses); i++) {
		const struct addr_field *f = &addresses[i];

		off = f->offset;
		if (f->owner == 31) {
			base = views->owner[M9_FAA_BOUND_DEPTH].base;
			limit = views->owner[M9_FAA_BOUND_DEPTH].capacity_bytes;
		} else {
			base = owner[f->owner].base;
			limit = owner[f->owner].bytes;
		}
		if (f->owner == 23)
			off *= bank;
		if (f->owner == 28)
			off = (f->offset == VERTEX ? geo + 128 : geo) +
			      c->data_offset;
		if (off > limit || limit - off < 4 ||
		    check_add_overflow(base, off, &value) || (value & 3))
			return -ERANGE;
		m9_cmd_put(stage + f->at, value | f->tag);
	}
	link.program_base = owner[26].base;
	link.program_owner_bytes = owner[26].bytes;
	link.program_offset = input->program_offset;
	link.program_bytes = c->layout.program_bytes;
	link.constants_base = owner[28].base;
	link.constants_owner_bytes = owner[28].bytes;
	link.data_offset = c->data_offset;
	link.constants_bytes = c->layout.constants_bytes;
	link.constants_tag = 1;
	for (i = 0; i < 4; i++) {
		link.entry_bytes[i] = c->layout.entry_bytes[i];
		link.constant_begin[i] = c->layout.constant_begin[i];
		link.constant_end[i] = c->layout.constant_end[i];
		program.entry_bytes[i] = c->layout.entry_bytes[i];
	}
	program.stage_count = 4;
	program.sampler_count = 1;
	program.coordinate_sets = 1;
	program.color_op0 = 4;
	program.program_bytes = c->layout.program_bytes;
	ret = m9x_faa_texture_single_link_place(&link, stage, bytes);
	if (ret)
		return ret;
	ret = m9x_faa_texture_single_bind_place(input->texture, &program, stage,
						bytes,
						M9_FAA_SINGLE_FIRST_SAMPLER_SITE);
	if (ret)
		return ret;
	ret = m9x_faa_first_depth_prefix_encode(sample, sample_bytes, owner,
						views, init_point, primary,
						primary_capacity, &length, ends,
						prefix_scratch);
	if (ret)
		return ret;
	/* Same primary tail as the dual FIRST (one GPU43 pair fewer than the
	 * four-stage COLOR FIRST); only the secondary ranges differ.
	 */
	memcpy(primary + length, color_scratch->primary + 64, 3348);
	memcpy(primary + length + 3348, color_scratch->primary + 64 + 3348 + 8,
	       tail - 3348);
	m9_cmd_put(primary + length + 8,
		   (owner[29].base + M9_FAA_SINGLE_FIRST_SPLIT) | 2);
	m9_cmd_put(primary + length + 52,
		   (owner[29].base + M9_FAA_SINGLE_FIRST_SPLIT) | 1);
	m9_cmd_put(primary + length + 56, (owner[29].base + bytes) | 2);
	for (i = 0; i < M9_FAA_FIRST_DEPTH_PREFIX_ENDS; i++)
		ends[i] += init_bytes;
	ends[10] = init_bytes + length + 60;
	ends[11] = init_bytes + length + tail - 12;
	ends[12] = init_bytes + length + tail - 12;
	ends[13] = init_bytes + length + tail;
	memcpy(secondary, stage, bytes);
	*primary_bytes = length + tail;
	return 0;
}
