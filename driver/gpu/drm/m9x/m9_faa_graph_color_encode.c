// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/overflow.h>
#include <crypto/sha2.h>
#include "m9_cmd.h"
#include "m9_faa_graph_color_encode.h"
#include "m9_faa_fragment_state.h"

struct color_field {
	u32 dst, at, owner, offset, pc, tag;
};

static const struct color_field fields[] = {
	{ 27, 16, 8, 0, 149810, 1 },
	{ 27, 68, 29, 0, 150706, 1 },
	{ 27, 72, 29, 580, 150741, 2 },
	{ 27, 80, 29, 580, 150706, 1 },
	{ 27, 84, 29, 1352, 150741, 2 },
	{ 27, 92, 22, 0, 149810, 1 },
	{ 27, 156, 22, 644, 149810, 1 },
	{ 27, 160, 22, 704, 149791, 5 },
	{ 27, 168, 8, 8, 153331, 1 },
	{ 27, 180, 8, 12, 153331, 1 },
	{ 27, 192, 8, 16, 153331, 1 },
	{ 27, 280, 1, 0, 149810, 1 },
	{ 27, 3308, 1, 0, 149810, 1 },
	{ 29, 72, 23, 0, 149810, 1 },
	{ 29, 88, 23, 1079296, 149810, 1 },
	{ 29, 104, 23, 2158592, 149810, 1 },
	{ 29, 120, 23, 3237888, 149810, 1 },
	{ 29, 284, 28, 384, 149810, 1 },
	{ 29, 304, 26, 0, 149664, 1 },
	{ 29, 328, 28, 16, 149713, 1 },
	{ 29, 332, 28, 136, 149713, 1 },
	{ 29, 516, 12, 0, 149664, 1 },
	{ 29, 548, 28, 256, 149713, 1 },
	{ 29, 768, 8, 0, 149810, 1 },
	{ 29, 772, 8, 20, 149774, 0 },
	{ 29, 800, 1, 0, 149810, 1 },
	{ 29, 816, 4, 0, 149810, 1 },
	{ 29, 832, 5, 0, 149810, 1 },
	{ 29, 848, 25, 0, 149810, 1 },
	{ 29, 860, 22, 0, 149810, 1 },
	{ 29, 892, 2, 0, 149810, 1 },
	{ 29, 896, 3, 0, 149810, 1 },
	{ 29, 1040, 23, 0, 149810, 1 },
	{ 29, 1044, 23, 2158592, 149810, 1 },
	{ 29, 1060, 23, 3237888, 149810, 1 },
	{ 29, 1064, 23, 1079296, 149810, 1 },
	{ 29, 1080, 26, 0, 149664, 1 },
	{ 29, 1104, 28, 0, 149713, 1 },
	{ 29, 1108, 28, 16, 149713, 1 },
	{ 29, 1304, 12, 0, 149664, 1 },
	{ 29, 1336, 28, 256, 149713, 1 }
};

struct color_token {
	u32 dst, at, delta, tag;
};

static const struct color_token tokens[] = {
	{ 27, 8, 16, 1 },     { 27, 216, 128, 1 },  { 27, 272, 144, 1 },
	{ 27, 408, 144, 0 },  { 27, 3172, 144, 0 }, { 27, 3628, 128, 0 },
	{ 27, 3648, 160, 1 }, { 29, 12, 32, 1 },    { 29, 60, 48, 1 },
	{ 29, 592, 64, 1 },   { 29, 620, 80, 1 },   { 29, 632, 80, 0 },
	{ 29, 692, 96, 1 },   { 29, 788, 112, 1 }
};

struct color_dim {
	u32 dst, at;
};

static const struct color_dim dims[] = { { 27, 252 },
					 { 27, 3636 },
					 { 29, 984 },
					 { 29, 992 } };
static const u8 template_sha[32] = { 0x56, 0x0f, 0x8b, 0xeb, 0xb7, 0xc9, 0x6b,
				     0x22, 0x27, 0xd5, 0xe1, 0x63, 0x08, 0xe5,
				     0xfe, 0x88, 0x97, 0x10, 0x88, 0x7c, 0x27,
				     0x3e, 0x38, 0x43, 0x7f, 0xbe, 0x59, 0x66,
				     0xb2, 0x02, 0x31, 0xc4 };
/* Original 150dc0 bound groups for fused POSITION/COLOR. Occupied scratch
 * halves clear availability; output halves are coupled only within the
 * three real tail groups. Clipcode r21 lies outside the 16-pair field.
 */
int m9x_faa_fused_source_mask(const u8 *program, u32 bytes, u32 begin,
			      u16 *mask)
{
	u32 i, seen = 0;
	u16 available = 0xbf00;
	bool stopped = false, clip = false;

	if (!program || !mask || (bytes & 7) || (begin & 7) || begin >= bytes ||
	    bytes - begin < 8)
		return -EINVAL;
	for (i = begin; i < bytes; i += 8) {
		u32 a = m9_cmd_get(program + i),
		    b = m9_cmd_get(program + i + 4), d = (a >> 8) & 127;
		bool output = false;

		if (a == 0xffff801c && b == 0xa01fc00f) {
			stopped = true;
			continue;
		}
		if (stopped)
			return -EINVAL;
		for (u32 j = 0; j < 3; j++) {
			if (d == 0x11 + 4 * j) {
				seen |= 1u << (2 * j);
				output = true;
			} else if (d == 0x13 + 4 * j) {
				seen |= 1u << (2 * j + 1);
				output = true;
			}
		}
		if (!output) {
			if (d == 0x21)
				clip = true;
			else if (d < 0x20)
				available &= ~(1u << (d / 2));
			else
				return -EINVAL;
		}
	}
	if (!stopped || seen != 0x3f || !clip)
		return -EINVAL;
	for (i = 0; i < 3; i++) {
		u16 complete = (1u << ((0x11 + 4 * i) / 2)) |
			       (1u << ((0x13 + 4 * i) / 2));
		if ((available & complete) != complete)
			available &= ~complete;
	}
	*mask = available;
	return 0;
}

bool m9x_faa_color_template_valid(const u8 *b, size_t n)
{
	u8 h[32];

	if (!b || n != 5004)
		return false;
	sha256(b, n, h);
	return !memcmp(h, template_sha, 32);
}

static u32 moved(u32 at, u32 extra)
{
	return at >= 508 ? at + extra : at;
}

int m9x_faa_color_encode(const u8 *blob, size_t bytes,
			 const struct m9x_faa_graph_draw_owner owner[31],
			 u32 init_bytes, u64 init_point,
			 const struct m9x_faa_color_input *in,
			 const struct m9x_faa_color_image *out)
{
	struct m9x_faa_surface_layout dimensions;
	u32 padded, extra, secondary, offset, address, value, i, j, pairs, raw,
		padded_records, geometry_offset;
	u16 fused_mask;
	struct m9_faa_fragment_state effect_state;
	struct m9_faa_fragment_interface effect_interface;
	bool effects = false;

	if (in && in->layout.program_count == 4)
		return m9x_faa_color_four_encode(blob, bytes, owner, init_bytes,
						 init_point, in, out);
	if (in && ((in->data_offset & 127) || (in->fragment_flags & ~1u)))
		return -EINVAL;
	if (!in || !out || !owner || !in->program || !in->constants ||
	    !in->geometry || !in->declaration || !out->program ||
	    !out->constants || !out->primary || !out->secondary ||
	    !out->secondary_length ||
	    !m9x_faa_color_template_valid(blob, bytes) ||
	    !m9x_faa_color_dimensions(in, init_bytes, &dimensions) ||
	    (init_point & 15) || (u32)init_point < 0xb0 ||
	    (u32)init_point > U32_MAX - 0xb0 || in->layout.program_count != 2 ||
	    in->layout.program_bytes > 4096 || (in->layout.program_bytes & 7) ||
	    in->layout.program_bytes < 16 || in->layout.entry_bytes[0] ||
	    !in->layout.entry_bytes[1] || (in->layout.entry_bytes[1] & 7) ||
	    in->layout.entry_bytes[1] >= in->layout.program_bytes ||
	    in->layout.entry_bytes[2] || in->layout.entry_bytes[3] ||
	    !in->layout.constants_bytes || in->layout.constants_bytes > 16384 ||
	    (in->layout.constants_bytes & 7) || in->layout.constant_begin[2] ||
	    in->layout.constant_begin[3] || in->layout.constant_end[2] ||
	    in->layout.constant_end[3] || in->record_stride < 16 ||
	    in->record_stride > 256 || (in->record_stride & 15) ||
	    !in->record_count || in->record_count % 3 ||
	    in->record_count > 12288 || !in->declaration_count ||
	    in->declaration_count > 17 || in->layout.vertex_offset != 128 ||
	    in->layout.geometry_bytes > 0x40000 - 256 ||
	    check_mul_overflow(in->record_stride, in->record_count, &raw) ||
	    raw != in->layout.vertex_bytes ||
	    in->layout.geometry_bytes != 128 + raw)
		return -EINVAL;
	for (i = 0; i < 2; i++) {
		if ((in->layout.constant_begin[i] & 7) ||
		    (in->layout.constant_end[i] & 7) ||
		    in->layout.constant_begin[i] >=
			    in->layout.constant_end[i] ||
		    in->layout.constant_end[i] > in->layout.constants_bytes ||
		    in->layout.constant_end[i] - in->layout.constant_begin[i] >
			    63 * 8)
			return -ERANGE;
	}
	for (i = 0; i < in->record_count; i++)
		for (j = 0; j < in->record_stride / 4; j++)
			if ((m9_cmd_get(in->geometry + 128 +
					i * in->record_stride + 4 * j) &
			     0x7f800000) == 0x7f800000)
				return -ERANGE;
	/* The terminal links one raw vertex declaration; paired words describe
	 * only finite FLOAT4 slots within the owned interleaved record.
	 */
	if (in->declaration_count < 3 || !(in->declaration_count & 1) ||
	    m9_cmd_get(in->declaration + 4 * (in->declaration_count - 1)) !=
		    0x4b08)
		return -EINVAL;
	for (i = 0; i + 1 < in->declaration_count; i++) {
		u32 d = m9_cmd_get(in->declaration + 4 * i);

		if (((d >> 16) & 15) ||
		    ((d >> 24) * 4 + 16 > in->record_stride) ||
		    ((d >> 20) & 15) != (i / 2 + 1) || ((d >> 12) & 15))
			return -ERANGE;
	}
	if (m9x_faa_color_flat_fragment_valid(in) != !!in->fragment_flags)
		return -EINVAL;
	/* Original170ff0/170800 alter the complete COLOR0 allocation. Do not
	 * classify an arbitrary same-sized FS by a blend/test flag or byte count.
	 */
	if (in->layout.entry_bytes[1] >= 80) {
		if (in->fragment_flags ||
		    !m9x_faa_fragment_state_decode((const u32 *)in->program,
						   in->layout.entry_bytes[1],
						   (const u32 *)(in->constants +
								 in->layout.constant_begin[0]),
						   in->layout.constant_end[0] -
						   in->layout.constant_begin[0],
						   &effect_state, &effect_interface))
			return -EINVAL;
		effects = true;
	}
	if (m9x_faa_fused_source_mask(in->program, in->layout.program_bytes,
				      in->layout.entry_bytes[1], &fused_mask))
		return -EINVAL;
	padded = (in->declaration_count + 3) & ~3u;
	extra = (padded / 4 - 1) * 24;
	secondary = 1352 + extra;
	padded_records = (in->record_count + 3) & ~3u;
	geometry_offset = (in->layout.constants_bytes + 127) & ~127u;
	if (m9x_faa_graph_draw_owners_validate(owner) ||
	    !m9x_faa_color_dimension_owners(owner, &dimensions) ||
	    init_bytes > owner[27].bytes ||
	    owner[27].bytes - init_bytes < 3652 ||
	    owner[26].bytes < in->layout.program_bytes ||
	    owner[29].bytes < secondary + 560 ||
	    in->data_offset > owner[28].bytes ||
	    owner[28].bytes - in->data_offset <
		    geometry_offset + 128 +
			    padded_records * in->record_stride ||
	    out->program_bytes < in->layout.program_bytes ||
	    out->constants_bytes < geometry_offset + 128 +
					   padded_records * in->record_stride ||
	    out->primary_bytes < 3652 || out->secondary_bytes < secondary)
		return -ERANGE;
	/* Dry-run every live substitution before changing an image. */
	for (i = 0; i < ARRAY_SIZE(fields); i++) {
		const struct color_field *f = &fields[i];

		offset = f->offset;
		if (f->owner == 29)
			offset = moved(offset, extra);
		if (f->owner == 28 && f->offset == 256)
			offset = geometry_offset;
		if (f->dst == 29 && f->at == 284)
			offset = geometry_offset + in->layout.vertex_offset;
		if (f->dst == 29 && f->at == 328)
			offset = in->layout.constant_begin[1];
		if (f->dst == 29 && f->at == 332)
			offset = in->layout.constant_end[1];
		if (f->dst == 29 && f->at == 1104)
			offset = in->layout.constant_begin[0];
		if (f->dst == 29 && f->at == 1108)
			offset = in->layout.constant_end[0];
		if (f->owner == 28 &&
		    check_add_overflow(offset, in->data_offset, &offset))
			return -ERANGE;
		if (offset > owner[f->owner].bytes ||
		    owner[f->owner].bytes - offset < 4 ||
		    check_add_overflow(owner[f->owner].base, offset,
				       &address) ||
		    (address & 3))
			return -ERANGE;
	}
	memcpy(out->program, in->program, in->layout.program_bytes);
	memcpy(out->constants, in->constants, in->layout.constants_bytes);
	memcpy(out->constants + geometry_offset, in->geometry,
	       in->layout.geometry_bytes);
	/* Four-aligned transform launch; repeat the final owned record only for
	 * unused padding, preserving the caller's raster record count.
	 */
	for (i = in->record_count; i < padded_records; i++)
		memcpy(out->constants + geometry_offset + 128 +
			       i * in->record_stride,
		       in->geometry + 128 +
			       (in->record_count - 1) * in->record_stride,
		       in->record_stride);
	memcpy(out->primary, blob, 3652);
	memcpy(out->secondary, blob + 3652, 484);
	for (i = 0; i < padded; i += 4) {
		u8 *p = out->secondary + 484 + i / 4 * 24;

		m9_cmd_put(p, 4);
		m9_cmd_put(p + 4, 0x23232323);
		for (j = 0; j < 4; j++)
			m9_cmd_put(p + 8 + j * 4,
				   i + j < in->declaration_count ?
					   m9_cmd_get(in->declaration +
						      (i + j) * 4) :
					   0);
	}
	memcpy(out->secondary + 508 + extra, blob + 3652 + 508, 1352 - 508);
	/* Guard band setup field of the live target, never the template's 320. */
	m9x_faa_guard_field_put(out->secondary + 32,
				m9x_faa_guard_field_size(dimensions.width,
							 dimensions.height));
	m9x_faa_guard_field_put(out->secondary + moved(664, extra),
				m9x_faa_guard_field_size(dimensions.width,
							 dimensions.height));
	for (i = 0; i < ARRAY_SIZE(fields); i++) {
		const struct color_field *f = &fields[i];

		offset = f->offset;
		if (f->owner == 29)
			offset = moved(offset, extra);
		if (f->owner == 28 && f->offset == 256)
			offset = geometry_offset;
		if (f->dst == 29 && f->at == 284)
			offset = geometry_offset + in->layout.vertex_offset;
		if (f->dst == 29 && f->at == 328)
			offset = in->layout.constant_begin[1];
		if (f->dst == 29 && f->at == 332)
			offset = in->layout.constant_end[1];
		if (f->dst == 29 && f->at == 1104)
			offset = in->layout.constant_begin[0];
		if (f->dst == 29 && f->at == 1108)
			offset = in->layout.constant_end[0];
		if (f->owner == 28)
			offset += in->data_offset;
		address = owner[f->owner].base + offset;
		value = f->pc == 0x248a0 ? (address & 0xfffff000) | f->tag :
			f->pc == 0x2490e ? (address | f->tag) & 0xfffffffc :
					   address | f->tag;
		m9_cmd_put((f->dst == 27 ?
				    out->primary + f->at :
				    out->secondary + moved(f->at, extra)),
			   value);
	}
	for (i = 0; i < ARRAY_SIZE(tokens); i++)
		m9_cmd_put(tokens[i].dst == 27 ?
				   out->primary + tokens[i].at :
				   out->secondary + moved(tokens[i].at, extra),
			   ((u32)init_point + tokens[i].delta) | tokens[i].tag);
	for (i = 0; i < ARRAY_SIZE(dims); i++)
		m9_cmd_put(dims[i].dst == 27 ?
				   out->primary + dims[i].at :
				   out->secondary + moved(dims[i].at, extra),
			   (dimensions.height << 16) | dimensions.width);
	/* Selector9/13/12 source descriptors are dimension fields, separate
	 * from their preceding live resource addresses. AD derives from the
	 * main resource logical extent; it is not a command-buffer capacity.
	 */
	m9_cmd_put(out->primary + 284, 0xf0210000u | dimensions.width);
	m9_cmd_put(out->primary + 3312, 0xf0220000u | dimensions.width);
	m9_cmd_put(out->primary + 3316, 0x00010000u | dimensions.height);
	m9_cmd_put(out->secondary + moved(804, extra),
		   dimensions.primary_descriptor);
	m9_cmd_put(out->secondary + moved(820, extra),
		   dimensions.companion_descriptor);
	m9_cmd_put(out->secondary + moved(836, extra),
		   dimensions.hz_descriptor);
	for (i = 0; i < 4; i++)
		m9_cmd_put(out->secondary + moved(908 + i * 4, extra),
			   dimensions.ad);
	m9_cmd_put(out->secondary + 308, in->layout.entry_bytes[1] / 8);
	m9_cmd_put(out->secondary + moved(1084, extra),
		   in->layout.entry_bytes[0] / 8);
	for (i = 0; i < 2; i++) {
		offset = i ? 324 : 1100;
		pairs = (in->layout.constant_end[i] -
			 in->layout.constant_begin[i]) /
			8;
		m9_cmd_put(out->secondary + moved(offset, extra),
			   0x00500000 | pairs << 8);
	}
	/* Register88's high byte is the source raw FLOAT4 input mask. Its
	 * low mask is the separately qualified fused position/COLOR interface.
	 */
	m9_cmd_put(out->secondary + 260,
		   (((1u << ((in->declaration_count - 1) / 2)) - 1) << 24) |
			   fused_mask);
	/* Original13a884..13a89e packs the number of live declaration halves
	 * (terminal excluded) into bits12:8. fafcc/fafe1 merges this into the
	 * raw stage's register22 control before fc2df emits it. The captured
	 * one-input8020e cannot describe a two-input8040e launch.
	 */
	m9_cmd_put(out->secondary + 44,
		   (m9_cmd_get(out->secondary + 44) & ~0x1f00u) |
			   ((in->declaration_count - 1) << 8));
	/* Original stride16/32 controls keep this setup family at32. Only
	 * f513d's raw fetch word292 follows the caller's record stride.
	 */
	for (i = 0; i < 4; i++)
		m9_cmd_put(out->secondary + 164 + i * 4, 32);
	/* f57c8 indexed C2 header at296 has three payload words. It is not
	 * the draw count; only the23000001 launch payloads carry that count.
	 */
	m9_cmd_put(out->secondary + 292, in->record_stride);
	m9_cmd_put(out->secondary + 296, 3);
	m9_cmd_put(out->secondary + moved(568, extra), 0x23000001);
	m9_cmd_put(out->secondary + moved(576, extra), padded_records);
	{
		static const u32 fsdecl[5] = { 0x00100d88, 0x001006b6,
					       0x00010688, 0x00c231b6,
					       0x00d30688 };
		m9_cmd_put(out->secondary + moved(1248, extra), 4);
		m9_cmd_put(out->secondary + moved(1272, extra), 4);
		for (i = 0; i < 4; i++)
			m9_cmd_put(out->secondary + moved(1256 + i * 4, extra),
				   fsdecl[i]);
		m9_cmd_put(out->secondary + moved(1280, extra), fsdecl[4]);
	}
	if (effects) {
		/* These are the compiler allocation and destination/alpha-test producer
		 * fields observed at181fa0 and17f04f/f5056, paired with the validated ISA.
		 * Live attachment/depth/cull fields and all ranges remain independent.
		 */
		u32 f0 = m9_cmd_get(out->secondary + moved(656, extra));

		f0 &= ~0x01000400u;
		if (effect_interface.reads_destination)
			f0 |= 0x01000000u;
		m9_cmd_put(out->secondary + moved(656, extra), f0);
		m9_cmd_put(out->secondary + moved(1016, extra),
			   (m9_cmd_get(out->secondary + moved(1016, extra)) &
			    ~0xffffu) |
				   effect_interface.available_register_mask);
	}
	/* Original182500 publishes one active COLOR input in AB slot0. Other
	 * AB/AC slots copied by182541/182549/fbee8 are inactive stack residue.
	 */
	m9_cmd_put(out->secondary + moved(960, extra), 0x0000ffffu);
	m9_cmd_put(out->secondary + moved(964, extra), 0);
	if (in->fragment_flags) {
		/* Same original compiler-shadow producers as COLOR4: fc203 lowers
		 * compiler397 bit24 into EF20000, f5056 publishes usage at1016, and
		 * f70fa/f7102 copy the flat IR declaration pair at1256/1260. The
		 * exact FS32/utility interface above is required; C1 interpolation is
		 * emitted separately from the caller's typed raster state.
		 */
		m9_cmd_put(out->secondary + moved(660, extra),
			   m9_cmd_get(out->secondary + moved(660, extra)) &
				   ~0x20000u);
		m9_cmd_put(out->secondary + moved(1016, extra),
			   (m9_cmd_get(out->secondary + moved(1016, extra)) &
			    ~0x8ffffu) |
				   0xfff0u);
		m9_cmd_put(out->secondary + moved(1256, extra),
			   m9_cmd_get(out->secondary + moved(1256, extra)) |
				   0x80000000u);
		m9_cmd_put(out->secondary + moved(1260, extra),
			   m9_cmd_get(out->secondary + moved(1260, extra)) |
				   0x80000000u);
	}
	m9_cmd_put(out->secondary + moved(1340, extra), 0x23000001);
	m9_cmd_put(out->secondary + moved(1348, extra), in->record_count);
	*out->secondary_length = secondary;
	return 0;
}
