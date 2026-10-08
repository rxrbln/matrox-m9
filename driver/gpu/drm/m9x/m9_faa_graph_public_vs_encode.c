// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* Bounded public CREATEVS first-stage shape on the retained INIT graph.
 * Source plan aed95531...: originalf0 unread, independent100 unqualified.
 * Neither public DRAW return nor later gather/position/raster is inferred.
 */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include <crypto/sha2.h>
#include "m9_faa_graph_public_vs_encode.h"

static const u8 template_sha[32] = {
	0x0c, 0x1c, 0xa4, 0xc9, 0x36, 0x86, 0xca, 0xbf, 0xdd, 0xa8, 0xf5,
	0xa7, 0xd5, 0x55, 0x3c, 0xe5, 0xd1, 0xe6, 0xa7, 0x35, 0x81, 0x97,
	0x76, 0xff, 0xce, 0x9c, 0x92, 0x31, 0xe7, 0xb2, 0x11, 0x9c,
};

static const u8 program_sha[32] = {
	0x42, 0x59, 0x09, 0x6c, 0x35, 0x53, 0x1f, 0x6d, 0x9a, 0x5e, 0xd6,
	0x59, 0x7b, 0x5c, 0x6e, 0xc9, 0xfe, 0xb3, 0x94, 0x2a, 0x43, 0x1f,
	0xba, 0x5a, 0x99, 0xd6, 0xcb, 0x60, 0xea, 0xf3, 0x8d, 0x76,
};

/* Static utility/compiler/position pairs; the live shader c0[32,48) is
 * masked before this initial code-shape guard. New shaders need qualification.
 */
static const u8 constants_sha[32] = {
	0x3c, 0x18, 0x0e, 0xc3, 0x42, 0xdb, 0x88, 0xfb, 0x2e, 0xca, 0x65,
	0x96, 0x16, 0x18, 0xcc, 0x83, 0x17, 0xc3, 0xb0, 0xcf, 0x1c, 0xbd,
	0xaf, 0x38, 0x97, 0xe6, 0x1a, 0x91, 0x0f, 0xd9, 0x1c, 0x2f,
};

struct vs_fixup {
	u16 dst, at, src;
	u32 addend, pc, tag;
};

static const struct vs_fixup fixups[] = {
	{ 27, 16, 8, 0, 0x24930, 1 },	  { 29, 116, 24, 0, 0x24930, 1 },
	{ 29, 132, 24, 0, 0x24930, 1 },	  { 29, 148, 24, 0, 0x24930, 1 },
	{ 29, 328, 28, 384, 0x24930, 1 }, { 29, 348, 26, 32, 0x24886, 1 },
	{ 29, 352, 26, 32, 0x248ab, 1 },  { 29, 372, 28, 32, 0x248c8, 1 },
	{ 29, 376, 28, 48, 0x248c8, 1 },  { 29, 584, 12, 0, 0x24886, 1 },
	{ 29, 616, 28, 256, 0x248c8, 1 }, { 27, 68, 29, 0, 0x256f1, 1 },
	{ 27, 72, 29, 752, 0x256f1, 2 },
};

struct vs_token {
	u16 dst, at;
	u32 delta, flags;
};

static const struct vs_token tokens[] = {
	{ 27, 8, 0x10, 1 },   { 29, 56, 0x20, 1 },  { 29, 104, 0x30, 1 },
	{ 29, 732, 0x40, 1 }, { 29, 748, 0x50, 1 },
};

static const struct m9x_faa_public_vs_upload_layout source_layout = {
	.program_bytes = 328,
	.program_count = 4,
	.entry_bytes = { 0, 32, 88, 128 },
	.constants_bytes = 152,
	.constant_begin = { 16, 32, 48, 48 },
	.constant_end = { 32, 48, 48, 152 },
	.geometry_bytes = 224,
	.vertex_offset = 128,
	.vertex_bytes = 96,
	.declaration = { 0x100d88, 0x1006b6, 0x4200d88, 0x42006b6, 0x4b08 },
};

static u32 get32(const u8 *p)
{
	return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static void put32(u8 *p, u32 w)
{
	p[0] = w;
	p[1] = w >> 8;
	p[2] = w >> 16;
	p[3] = w >> 24;
}

static bool ranges_overlap(const void *a, size_t na, const void *b, size_t nb)
{
	unsigned long x = (unsigned long)a, y = (unsigned long)b, ex, ey;

	return check_add_overflow(x, na, &ex) ||
	       check_add_overflow(y, nb, &ey) || (x < ey && y < ex);
}

static bool digest_equal(const u8 *p, size_t bytes, const u8 expected[32])
{
	u8 digest[32];

	sha256(p, bytes, digest);
	return !memcmp(digest, expected, 32);
}

bool m9x_faa_public_vs_template_valid(const u8 *blob, size_t bytes)
{
	return blob && bytes == M9_FAA_PUBLIC_VS_TEMPLATE_BYTES &&
	       digest_equal(blob, bytes, template_sha);
}

static int input_valid(const struct m9x_faa_public_vs_input *in)
{
	static const u32 header[32] = { 0x43200000, 0xc2f00000, 0x3f800000,
					0,	    0x43200000, 0x42f00000 };
	u8 constants[152];
	u32 i, j;

	if (!in || !in->program || !in->constants || !in->geometry ||
	    in->program_bytes != 328 || in->constants_bytes != 152 ||
	    in->geometry_bytes != 224 ||
	    memcmp(&in->layout, &source_layout, sizeof(source_layout)) ||
	    !digest_equal(in->program, 328, program_sha))
		return -EINVAL;
	memcpy(constants, in->constants, sizeof(constants));
	memset(constants + 32, 0, 16);
	if (!digest_equal(constants, sizeof(constants), constants_sha))
		return -EINVAL;
	/* Current graph INIT is320x240/default depth. Integer bit checks avoid
	 * kernel floating point and admit only normalized finite live inputs.
	 */
	for (i = 0; i < 32; i++)
		if (get32(in->geometry + 4 * i) != header[i])
			return -EINVAL;
	for (i = 0; i < 4; i++)
		if (get32(in->constants + 32 + 4 * i) > 0x3f800000u)
			return -ERANGE;
	for (i = 0; i < 3; i++) {
		const u8 *v = in->geometry + 128 + 32 * i;

		for (j = 0; j < 2; j++)
			if ((get32(v + 4 * j) & 0x7fffffffu) > 0x3f800000u)
				return -ERANGE;
		if (get32(v + 8) > 0x3f800000u || get32(v + 12) != 0x3f800000u)
			return -ERANGE;
		for (j = 4; j < 8; j++)
			if (get32(v + 4 * j) > 0x3f800000u)
				return -ERANGE;
	}
	return 0;
}

int m9x_faa_public_vs_encode(const u8 *blob, size_t bytes,
			     const struct m9x_faa_graph_draw_owner
				     owner[M9_FAA_DRAW_SOURCE_OWNERS],
			     u32 live_init_primary_bytes, u64 init_point,
			     const struct m9x_faa_public_vs_input *in,
			     const struct m9x_faa_public_vs_image *out,
			     struct m9x_faa_public_vs_endpoints *ends)
{
	u32 words[ARRAY_SIZE(fixups)], i, j, address, first, final;
	void *dest[5];
	size_t size[5];
	int ret;

	if (!owner || !out || !ends || !out->program || !out->constants ||
	    !out->secondary || !out->primary || out->program_bytes < 328 ||
	    out->constants_bytes < 480 || out->secondary_bytes < 752 ||
	    out->primary_bytes < 112 ||
	    !m9x_faa_public_vs_template_valid(blob, bytes) ||
	    (live_init_primary_bytes != 4880 &&
	     live_init_primary_bytes != 4884) ||
	    (u32)init_point < 0xb0 || (init_point & 15) ||
	    (u32)init_point > U32_MAX - 0x50 ||
	    check_add_overflow(live_init_primary_bytes, 64u, &first) ||
	    check_add_overflow(live_init_primary_bytes, 112u, &final))
		return -EINVAL;
	ret = input_valid(in);
	if (ret)
		return ret;
	ret = m9x_faa_graph_draw_owners_validate(owner);
	if (ret)
		return ret;
	if (final > owner[27].bytes || owner[26].bytes < 328 ||
	    owner[28].bytes < 480 || owner[29].bytes < 752)
		return -ERANGE;
	/* The plan collapses aliases into one owner. Distinct retained allocations
	 * may not overlap in the encoded GPU address namespace.
	 */
	for (i = 0; i < 31; i++)
		for (j = i + 1; j < 31; j++)
			if ((u64)owner[i].base + owner[i].bytes >
				    owner[j].base &&
			    (u64)owner[j].base + owner[j].bytes > owner[i].base)
				return -ERANGE;
	dest[0] = out->program;
	size[0] = 328;
	dest[1] = out->constants;
	size[1] = 480;
	dest[2] = out->secondary;
	size[2] = 752;
	dest[3] = out->primary;
	size[3] = 112;
	dest[4] = ends;
	size[4] = sizeof(*ends);
	for (i = 0; i < 5; i++) {
		if (ranges_overlap(dest[i], size[i], blob, bytes) ||
		    ranges_overlap(dest[i], size[i], in, sizeof(*in)) ||
		    ranges_overlap(dest[i], size[i], out, sizeof(*out)) ||
		    ranges_overlap(dest[i], size[i], owner,
				   31 * sizeof(*owner)) ||
		    ranges_overlap(dest[i], size[i], in->program, 328) ||
		    ranges_overlap(dest[i], size[i], in->constants, 152) ||
		    ranges_overlap(dest[i], size[i], in->geometry, 224))
			return -EINVAL;
		for (j = i + 1; j < 5; j++)
			if (ranges_overlap(dest[i], size[i], dest[j], size[j]))
				return -EINVAL;
	}
	for (i = 0; i < ARRAY_SIZE(fixups); i++) {
		const struct vs_fixup *f = &fixups[i];
		u32 addend = f->addend;
		/* Derive compiler/layout-dependent references from the supplied layout. */
		if (f->src == 26)
			addend = in->layout.entry_bytes[1];
		if (f->dst == 29 && f->at == 328)
			addend = 256 + in->layout.vertex_offset;
		if (f->dst == 29 && f->at == 372)
			addend = in->layout.constant_begin[1];
		if (f->dst == 29 && f->at == 376)
			addend = in->layout.constant_end[1];
		if (addend > owner[f->src].bytes ||
		    owner[f->src].bytes - addend < 4 ||
		    check_add_overflow(owner[f->src].base, addend, &address) ||
		    (address & 3) || (f->pc == 0x248ab && (address & 7)))
			return -ERANGE;
		switch (f->pc) {
		case 0x24886:
			words[i] = (address & 0xfffff000u) | f->tag |
				   (!address ? 4u : 0u);
			break;
		case 0x248ab:
			words[i] = (address & 0xfffu) >> 3;
			break;
		case 0x24930:
		case 0x248c8:
		case 0x256f1:
			words[i] = address | f->tag;
			break;
		default:
			return -EINVAL;
		}
	}
	memcpy(out->program, in->program, 328);
	memcpy(out->constants, in->constants, 152);
	memcpy(out->constants + 256, in->geometry, 224);
	memcpy(out->primary, blob, 112);
	memcpy(out->secondary, blob + 112, 752);
	for (i = 0; i < ARRAY_SIZE(fixups); i++)
		put32((fixups[i].dst == 27 ? out->primary : out->secondary) +
			      fixups[i].at,
		      words[i]);
	for (i = 0; i < ARRAY_SIZE(tokens); i++)
		put32((tokens[i].dst == 27 ? out->primary : out->secondary) +
			      tokens[i].at,
		      ((u32)init_point + tokens[i].delta) | tokens[i].flags);
	/* 248f2: same owned secondary. Source high12 opcode is preserved; no
	 * native GPU/CPU alias participates. Deferred marker704 stays untouched.
	 */
	put32(out->secondary + 28, 0x49d00000u | ((660u - 28u) >> 2));
	put32(out->secondary + 368,
	      0x00500000u | ((in->layout.constant_end[1] -
			      in->layout.constant_begin[1]) /
				     8u
			     << 8));
	for (i = 0; i < 5; i++) {
		static const u16 declaration_at[5] = { 536, 540, 544, 548,
						       560 };
		put32(out->secondary + declaration_at[i],
		      in->layout.declaration[i]);
	}
	*ends = (struct m9x_faa_public_vs_endpoints){ first, final,
						      init_point + 0x40,
						      init_point + 0x50 };
	return 0;
}
