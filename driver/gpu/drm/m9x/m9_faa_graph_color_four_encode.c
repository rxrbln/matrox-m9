// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* Original two-varying M9128 control; one-varying shape is experimental,
 * derived from independent target/mask/record rules and real gather arity.
 */
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/overflow.h>
#include <crypto/sha2.h>
#include "m9_cmd.h"
#include "m9_faa_graph_color_encode.h"

/* Original M9128 150dc0 availability mask for the live carry head. The
 * program's terminal STOP is padding, not a scratch-register write.
 */
int m9x_faa_carry_source_mask(const u8 *program, u32 program_bytes, u32 begin,
			      u32 end, u32 targets, u16 *mask)
{
	u32 i, pairs = 0, seen = 0;
	u16 available;
	bool stopped = false;

	if (!program || !mask || !targets || targets > 4 || (begin & 7) ||
	    (end & 7) || begin >= end || end > program_bytes ||
	    end - begin < 3 * 8)
		return -EINVAL;
	for (i = begin; i < end; i += 8) {
		u32 w0 = m9_cmd_get(program + i);
		u32 w1 = m9_cmd_get(program + i + 4);
		u32 d = (w0 >> 8) & 0x7f;
		bool output = false;

		if (w0 == 0xffff801c && w1 == 0xa01fc00f) {
			stopped = true;
			continue;
		}
		if (stopped)
			return -EINVAL;
		for (u32 j = 0; j < targets; j++) {
			if (d == 0x11 + 4 * j) {
				seen |= 1u << (2 * j);
				output = true;
			} else if (d == 0x13 + 4 * j) {
				seen |= 1u << (2 * j + 1);
				output = true;
			}
		}
		if (!output) {
			if (d >= 0x20)
				return -EINVAL;
			if (d / 2 + 1 > pairs)
				pairs = d / 2 + 1;
		}
	}
	if (!stopped || seen != (1u << (2 * targets)) - 1)
		return -EINVAL;
	available = (u16)(~((1u << pairs) - 1));
	for (i = 0; i < targets; i++) {
		u16 complete = (u16)((1u << ((0x11 + 4 * i) / 2)) |
				     (1u << ((0x13 + 4 * i) / 2)));
		if ((available & complete) != complete)
			available &= ~complete;
	}
	*mask = available;
	return 0;
}

struct field {
	u32 dst, at, owner, offset, pc, tag;
};

static const struct field fields[] = { { 27, 16, 8, 0, 149810, 1 },
				       { 27, 68, 29, 0, 150706, 1 },
				       { 27, 72, 29, 1840, 150741, 2 },
				       { 27, 116, 29, 1840, 150706, 1 },
				       { 27, 120, 29, 2712, 150741, 2 },
				       { 27, 128, 22, 0, 149810, 1 },
				       { 27, 192, 22, 644, 149810, 1 },
				       { 27, 196, 22, 704, 149791, 5 },
				       { 27, 204, 8, 8, 153331, 1 },
				       { 27, 216, 8, 12, 153331, 1 },
				       { 27, 228, 8, 16, 153331, 1 },
				       { 27, 316, 1, 0, 149810, 1 },
				       { 27, 3344, 1, 0, 149810, 1 },
				       { 29, 116, 24, 0, 149810, 1 },
				       { 29, 132, 24, 0, 149810, 1 },
				       { 29, 148, 24, 0, 149810, 1 },
				       { 29, 328, 28, 384, 149810, 1 },
				       { 29, 348, 26, 0, 149664, 1 },
				       { 29, 372, 28, 16, 149713, 1 },
				       { 29, 376, 28, 32, 149713, 1 },
				       { 29, 584, 12, 0, 149664, 1 },
				       { 29, 616, 28, 256, 149713, 1 },
				       { 29, 804, 23, 0, 149810, 1 },
				       { 29, 820, 23, 873472, 149810, 1 },
				       { 29, 836, 23, 3493888, 149810, 1 },
				       { 29, 976, 26, 0, 149664, 1 },
				       { 29, 1000, 28, 32, 149713, 1 },
				       { 29, 1004, 28, 32, 149713, 1 },
				       { 29, 1032, 24, 0, 149810, 1 },
				       { 29, 1060, 24, 0, 149810, 1 },
				       { 29, 1220, 12, 0, 149664, 1 },
				       { 29, 1252, 28, 256, 149713, 1 },
				       { 29, 1416, 23, 1746944, 149810, 1 },
				       { 29, 1432, 23, 2620416, 149810, 1 },
				       { 29, 1448, 23, 3493888, 149810, 1 },
				       { 29, 1588, 26, 0, 149664, 1 },
				       { 29, 1612, 28, 32, 149713, 1 },
				       { 29, 1616, 28, 136, 149713, 1 },
				       { 29, 1644, 24, 0, 149810, 1 },
				       { 29, 1780, 12, 0, 149664, 1 },
				       { 29, 1812, 28, 256, 149713, 1 },
				       { 29, 2028, 8, 0, 149810, 1 },
				       { 29, 2032, 8, 20, 149774, 0 },
				       { 29, 2060, 1, 0, 149810, 1 },
				       { 29, 2076, 4, 0, 149810, 1 },
				       { 29, 2092, 5, 0, 149810, 1 },
				       { 29, 2108, 25, 0, 149810, 1 },
				       { 29, 2120, 22, 0, 149810, 1 },
				       { 29, 2152, 2, 0, 149810, 1 },
				       { 29, 2156, 3, 0, 149810, 1 },
				       { 29, 2300, 23, 873472, 149810, 1 },
				       { 29, 2304, 23, 2620416, 149810, 1 },
				       { 29, 2320, 23, 3493888, 149810, 1 },
				       { 29, 2324, 23, 1746944, 149810, 1 },
				       { 29, 2340, 26, 0, 149664, 1 },
				       { 29, 2364, 28, 0, 149713, 1 },
				       { 29, 2368, 28, 16, 149713, 1 },
				       { 29, 2664, 12, 0, 149664, 1 },
				       { 29, 2696, 28, 256, 149713, 1 } };
struct token {
	u32 dst, at, delta, tag;
};

static const struct token tokens[] = {
	{ 27, 8, 16, 1 },     { 27, 252, 192, 1 },  { 27, 308, 208, 1 },
	{ 27, 444, 208, 0 },  { 27, 3208, 208, 0 }, { 27, 3672, 192, 0 },
	{ 27, 3692, 224, 1 }, { 29, 56, 32, 1 },    { 29, 104, 48, 1 },
	{ 29, 732, 64, 1 },   { 29, 744, 64, 0 },   { 29, 792, 80, 1 },
	{ 29, 1344, 96, 1 },  { 29, 1356, 96, 0 },  { 29, 1404, 112, 1 },
	{ 29, 1852, 128, 1 }, { 29, 1880, 144, 1 }, { 29, 1892, 144, 0 },
	{ 29, 1952, 160, 1 }, { 29, 2048, 176, 1 }
};

struct pair {
	u32 a, b;
};

static const struct pair dims[] = {
	{ 27, 288 }, { 27, 3680 }, { 29, 2244 }, { 29, 2252 }
};

static const struct pair metadata[] = {
	    { 208, 32 },	  { 212, 32 },		{ 216, 32 },
	    { 220, 32 },	  { 272, 4 },		{ 304, 50397183 },
	    { 308, 4261445412 },  { 320, 3 },		{ 336, 32 },
	    { 352, 8 },		  { 368, 5243392 },	{ 392, 256 },
	    { 404, 256 },	  { 416, 256 },		{ 428, 256 },
	    { 436, 1162167664 },  { 440, 0 },		{ 448, 1162167665 },
	    { 452, 0 },		  { 460, 1162167666 },	{ 464, 0 },
	    { 472, 1162167667 },  { 476, 0 },		{ 480, 4 },
	    { 484, 3958043884 },  { 488, 0 },		{ 492, 65540 },
	    { 500, 1174405120 },  { 528, 4 },		{ 536, 1052040 },
	    { 540, 1050294 },	  { 544, 69209480 },	{ 548, 69207734 },
	    { 552, 4 },		  { 560, 19208 },	{ 564, 0 },
	    { 568, 0 },		  { 572, 0 },		{ 600, 3 },
	    { 612, 4144 },	  { 880, 32 },		{ 884, 32 },
	    { 888, 32 },	  { 892, 32 },		{ 952, 3211263 },
	    { 956, 4261445586 },  { 980, 15 },		{ 996, 5242880 },
	    { 1020, 117835020 },  { 1036, 3994616844 }, { 1048, 185207052 },
	    { 1064, 3994617868 }, { 1076, 256 },	{ 1088, 256 },
	    { 1096, 1162167664 }, { 1100, 0 },		{ 1108, 1162167665 },
	    { 1112, 0 },	  { 1120, 1162167666 }, { 1124, 0 },
	    { 1132, 1162167667 }, { 1136, 0 },		{ 1140, 4 },
	    { 1144, 3958043884 }, { 1148, 0 },		{ 1152, 65540 },
	    { 1160, 1174405120 }, { 1188, 4 },		{ 1196, 19208 },
	    { 1200, 0 },	  { 1204, 0 },		{ 1208, 0 },
	    { 1236, 3 },	  { 1248, 4144 },	{ 1492, 32 },
	    { 1496, 32 },	  { 1500, 32 },		{ 1504, 32 },
	    { 1564, 1109763 },	  { 1568, 4261421010 }, { 1592, 20 },
	    { 1608, 5246208 },	  { 1632, 50462988 },	{ 1648, 3994615820 },
	    { 1660, 256 },	  { 1672, 256 },	{ 1684, 256 },
	    { 1692, 1162167664 }, { 1696, 0 },		{ 1704, 1162167665 },
	    { 1708, 0 },	  { 1716, 1162167666 }, { 1720, 0 },
	    { 1728, 1162167667 }, { 1732, 0 },		{ 1736, 4 },
	    { 1740, 3958043884 }, { 1744, 0 },		{ 1748, 65540 },
	    { 1756, 1174405120 }, { 1796, 3 },		{ 1808, 4144 },
	    { 2168, 39684 },	  { 2172, 39684 },	{ 2176, 39684 },
	    { 2180, 39684 },	  { 2232, 4 },		{ 2276, 18391024 },
	    { 2280, 4263542776 }, { 2292, 3 },		{ 2308, 1048592 },
	    { 2312, 3 },	  { 2328, 1048582 },	{ 2344, 0 },
	    { 2360, 5243392 },	  { 2384, 256 },	{ 2396, 256 },
	    { 2408, 256 },	  { 2420, 256 },	{ 2464, 512 },
	    { 2468, 267911168 },  { 2476, 4 },		{ 2500, 4 },
	    { 2508, 4294967295 }, { 2540, 1162167665 }, { 2544, 0 },
	    { 2552, 1162167666 }, { 2556, 0 },		{ 2564, 1162167667 },
	    { 2568, 0 },	  { 2572, 4 },		{ 2576, 3958043884 },
	    { 2580, 0 },	  { 2584, 0 },		{ 2592, 0 },
	    { 2608, 4 },	  { 2616, 1052040 },	{ 2620, 1050294 },
	    { 2624, 67208 },	  { 2628, 12726710 },	{ 2632, 4 },
	    { 2640, 13829768 },	  { 2644, 0 },		{ 2648, 0 },
	    { 2652, 0 },	  { 2680, 3 },		{ 2692, 4144 }
};

static const u8 digest[32] = { 0xa7, 0xc8, 0xb9, 0xa2, 0xb6, 0x9f, 0x0c, 0x51,
			       0x9a, 0x3b, 0x76, 0xd4, 0xb6, 0x43, 0x4d, 0xb8,
			       0xee, 0xc2, 0x60, 0x0f, 0x4b, 0x89, 0x36, 0x50,
			       0x63, 0x06, 0xc8, 0x58, 0x27, 0x80, 0x57, 0x43 };
bool m9x_faa_color_four_template_valid(const u8 *b, size_t n)
{
	u8 h[32];

	if (!b || n != 6408)
		return false;
	sha256(b, n, h);
	return !memcmp(h, digest, 32);
}

/* Classify the complete bounded flat interface. Other 32-byte fragment
 * programs (including the existing constant-color source control) are not
 * flat merely because their vector has this length.
 */
bool m9x_faa_color_flat_fragment_valid(const struct m9x_faa_color_input *in)
{
	static const u32 fs[] = { 0xffe2ad9e, 0xe29fff1f, 0xffe22d9d,
				  0xa29fff1f, 0xffff801c, 0xa01fc00f,
				  0xffff801c, 0xa01fc00f };
	static const u32 utility[] = { 0, 0x3f800000, 0x3f800000, 0x3f000000 };
	u32 i, begin;

	if (!in || !in->program || !in->constants ||
	    in->layout.program_bytes < 32 || in->layout.entry_bytes[1] != 32 ||
	    in->layout.constant_end[0] < in->layout.constant_begin[0] ||
	    in->layout.constant_end[0] - in->layout.constant_begin[0] != 16 ||
	    in->layout.constant_end[0] > in->layout.constants_bytes)
		return false;
	begin = in->layout.constant_begin[0];
	for (i = 0; i < 8; i++)
		if (m9_cmd_get(in->program + 4 * i) != fs[i])
			return false;
	for (i = 0; i < 4; i++)
		if (m9_cmd_get(in->constants + begin + 4 * i) != utility[i])
			return false;
	return true;
}

/* Replace the two four-DWORD source declaration groups with owned semantic
 * input groups. All later fixups, endpoint fields and lengths follow it.
 */
static u32 moved(u32 x, int extra)
{
	return x >= 576 ? x + extra : x;
}

static u32 route(u32 targets, bool packed, bool clip)
{
	u32 x = 0xfe000000;

	for (u32 i = 0; i < 4; i++)
		x |= (i < targets ? (packed ? 4 : 2) : 7) << (3 * i);
	return x | (clip ? 1 : 7) << 12;
}

static u32 ef(u32 targets, u32 role)
{
	for (u32 i = 1; i < targets; i++)
		role |= 0x80000 << (2 * (i - 1));
	return role;
}

int m9x_faa_color_four_encode(const u8 *blob, size_t bytes,
			      const struct m9x_faa_graph_draw_owner owner[31],
			      u32 init_bytes, u64 init_point,
			      const struct m9x_faa_color_input *in,
			      const struct m9x_faa_color_image *out)
{
	struct m9x_faa_surface_layout dimensions;
	u32 i, j, raw, padded, geo, groups, secondary, arity, gatherbytes,
		pairs, address, offset, value;
	u16 carry_mask;
	bool dual;
	int extra;

	if (!in || !out || !owner || !in->program || !in->constants ||
	    !in->geometry || !in->declaration || !out->program ||
	    !out->constants || !out->primary || !out->secondary ||
	    !out->secondary_length ||
	    !m9x_faa_color_four_template_valid(blob, bytes) ||
	    !m9x_faa_color_dimensions(in, init_bytes, &dimensions) ||
	    (init_point & 15) || (u32)init_point < 0xb0 ||
	    (u32)init_point > U32_MAX - 0xf0 || in->layout.program_count != 4 ||
	    !in->layout.program_bytes || in->layout.program_bytes > 4096 ||
	    (in->layout.program_bytes & 7) || in->layout.entry_bytes[0] ||
	    !in->layout.constants_bytes || in->layout.constants_bytes > 16384 ||
	    (in->layout.constants_bytes & 7) ||
	    (in->record_stride != 32 &&
	     !(in->record_stride == 16 && in->declaration_count == 3) &&
	     !(in->dual_base_only &&
	       (in->record_stride == 36 || in->record_stride == 64)) &&
	     !(in->single_base_only &&
	       (in->record_stride == 24 || in->record_stride == 64))) ||
	    !in->record_count || in->record_count % 3 ||
	    in->record_count > 12288 || (in->data_offset & 127) ||
	    in->data_offset > owner[28].bytes || (in->fragment_flags & ~1u) ||
	    in->declaration_count < 3 || in->declaration_count > 17 ||
	    !(in->declaration_count & 1) || in->layout.vertex_offset != 128 ||
	    check_mul_overflow(in->record_stride, in->record_count, &raw) ||
	    raw != in->layout.vertex_bytes ||
	    in->layout.geometry_bytes != 128 + raw ||
	    in->layout.geometry_bytes > 0x40000 - 256)
		return -EINVAL;
	for (i = 0; i < 4; i++) {
		if ((in->layout.entry_bytes[i] & 7) ||
		    in->layout.entry_bytes[i] >= in->layout.program_bytes ||
		    (i && in->layout.entry_bytes[i] <=
				  in->layout.entry_bytes[i - 1]) ||
		    (in->layout.constant_begin[i] & 7) ||
		    (in->layout.constant_end[i] & 7) ||
		    in->layout.constant_begin[i] > in->layout.constant_end[i] ||
		    in->layout.constant_end[i] > in->layout.constants_bytes ||
		    in->layout.constant_end[i] - in->layout.constant_begin[i] >
			    504)
			return -ERANGE;
	}
	gatherbytes = in->layout.entry_bytes[3] - in->layout.entry_bytes[2];
	if (gatherbytes != 32 && gatherbytes != 40 && gatherbytes != 56)
		return -EINVAL;
	arity = gatherbytes == 32 ? 1 : gatherbytes == 40 ? 2 : 3;
	/* Three varyings require a fourth packed target and two texture units.
	 * This helper only builds their common base. Its image is not a complete
	 * hardware command and may only be consumed by the dual constructor.
	 */
	dual = arity == 3;
	if (dual != !!in->dual_base_only ||
	    (in->single_base_only &&
	     (arity != 2 || in->declaration_count != 7)) ||
	    (arity == 3 &&
	     (in->declaration_count < 9 ||
	      (in->record_stride != 32 && in->record_stride != 36 &&
	       in->record_stride != 64))))
		return -EINVAL;
	if (m9x_faa_carry_source_mask(in->program, in->layout.program_bytes,
				      in->layout.entry_bytes[1],
				      in->layout.entry_bytes[2], 1 + arity,
				      &carry_mask))
		return -EINVAL;
	if ((in->layout.program_bytes - in->layout.entry_bytes[3] != 200 &&
	     !(arity == 3 &&
	       in->layout.program_bytes - in->layout.entry_bytes[3] == 192)) ||
	    in->layout.constant_begin[2] != in->layout.constant_end[2] ||
	    in->layout.constant_end[3] - in->layout.constant_begin[3] != 104 ||
	    /* The authenticated dual MODULATE FS72 reads COLOR and two samples but
	     * has no FS constant input. Its C3 range is empty. The ordinary four-stage
	     * COLOR frontends still require their source-proven utility vector.
	     */
	    (!in->dual_base_only &&
	     in->layout.constant_end[0] - in->layout.constant_begin[0] < 16))
		return -EINVAL;
	/* Pair only the classified flat interface with flat usage metadata. */
	if (m9x_faa_color_flat_fragment_valid(in) != !!in->fragment_flags)
		return -EINVAL;
	/* Exact address-free RECORDS move grammar, including output register halves,
	 * source logical indices, scheduling bit, saturation and STOP padding.
	 */
	for (i = 0; i < gatherbytes / 8; i++) {
		u32 a, b;

		if (i < 2 * arity) {
			u32 t = i / 2, h = i & 1;

			a = 0xffc00000 | ((4 * t + 2 * h) << 15) |
			    ((0x11 + 4 * t + 2 * h) << 8) | 0x1c;
			b = 0xa29fff1f;
		} else {
			a = 0xffff801c;
			b = 0xa01fc00f;
		}
		if (i == gatherbytes / 8 - 4)
			b |= 0x40000000;
		if (m9_cmd_get(in->program + in->layout.entry_bytes[2] +
			       i * 8) != a ||
		    m9_cmd_get(in->program + in->layout.entry_bytes[2] + i * 8 +
			       4) != b)
			return -EINVAL;
	}
	if (m9_cmd_get(in->declaration + 4 * (in->declaration_count - 1)) !=
	    0x4b08)
		return -EINVAL;
	for (i = 0; i + 1 < in->declaration_count; i++) {
		/* Original public FVF 0x242/0x244 both use packed D3DCOLOR and FLOAT2
		 * coordinates. Their offsets differ with FLOAT3 XYZ vs FLOAT4 XYZRHW.
		 * Keep exact source declarations; the live vec4x4 route remains the
		 * separate 64-byte admission.
		 */
		static const u32 rhw_decl[] = {
			0x00100d88, 0x001006b6, 0x0420cd8a, 0x0420c636,
			0x05300d88, 0x05300b36, 0x07400d88, 0x07400b36
		};
		static const u32 xyz_decl[] = {
			0x00100daa, 0x00100236, 0x0320cd8a, 0x0320c636,
			0x04300d88, 0x04300b36, 0x06400d88, 0x06400b36
		};
		u32 q = m9_cmd_get(in->declaration + 4 * i);

		if (arity == 3 &&
		    (in->record_stride == 32 || in->record_stride == 36)) {
			if (in->declaration_count != 9 ||
			    q != (in->record_stride == 32 ? xyz_decl[i] :
							    rhw_decl[i]))
				return -EINVAL;
		} else if (in->single_base_only && in->record_stride != 64) {
			/* Original FVF 0x142/0x242 single stage: UV0 at +16. */
			if (q != xyz_decl[i])
				return -EINVAL;
		} else if (((q >> 16) & 15) ||
			   ((q >> 24) * 4 + 16 > in->record_stride) ||
			   ((q >> 20) & 15) != i / 2 + 1 || ((q >> 12) & 15))
			return -EINVAL;
	}
	for (i = 0; i < in->record_count; i++)
		for (j = 0; j < in->record_stride / 4; j++)
			if ((m9_cmd_get(in->geometry + 128 +
					i * in->record_stride + 4 * j) &
			     0x7f800000) == 0x7f800000)
				return -ERANGE;
	groups = (in->declaration_count + 3) / 4;
	extra = ((int)groups - 2) * 24;
	secondary = 2712 + extra;
	geo = (in->layout.constants_bytes + 127) & ~127u;
	padded = (in->record_count + 3) & ~3u;
	if (m9x_faa_graph_draw_owners_validate(owner) ||
	    !m9x_faa_color_dimension_owners(owner, &dimensions) ||
	    init_bytes > owner[27].bytes ||
	    owner[27].bytes - init_bytes < 3696 ||
	    owner[26].bytes < in->layout.program_bytes ||
	    owner[28].bytes - in->data_offset <
		    geo + 128 + padded * in->record_stride ||
	    owner[29].bytes < secondary ||
	    out->program_bytes < in->layout.program_bytes ||
	    out->constants_bytes < geo + 128 + padded * in->record_stride ||
	    out->primary_bytes < 3696 || out->secondary_bytes < secondary)
		return -ERANGE;
	/* Dry-run domains/offsets before the first destination change. */
	for (i = 0; i < ARRAY_SIZE(fields); i++) {
		const struct field *f = &fields[i];

		offset = f->offset;
		/* One real COLOR stream is gathered into target0. The raster COLOR
		 * plane must follow that producer, rather than the disabled target1.
		 */
		if (arity == 1 && f->owner == 23 && f->at == 2300)
			offset = 0;
		if (f->owner == 29)
			offset = moved(offset, extra);
		if (f->owner == 28) {
			if (offset == 256)
				offset = geo;
			else if (f->at == 328)
				offset = geo + 128;
			else if (f->at == 372 || f->at == 376)
				offset = f->at == 372 ?
						 in->layout.constant_begin[1] :
						 in->layout.constant_end[1];
			else if (f->at == 1000 || f->at == 1004)
				offset = in->layout.constant_begin[2];
			else if (f->at == 1612 || f->at == 1616)
				offset = f->at == 1612 ?
						 in->layout.constant_begin[3] :
						 in->layout.constant_end[3];
			else if (f->at == 2364 || f->at == 2368)
				offset = f->at == 2364 ?
						 in->layout.constant_begin[0] :
						 in->layout.constant_end[0];
			offset += in->data_offset;
		}
		if (offset > owner[f->owner].bytes ||
		    owner[f->owner].bytes - offset < 4 ||
		    check_add_overflow(owner[f->owner].base, offset,
				       &address) ||
		    (address & 3))
			return -ERANGE;
	}
	memcpy(out->program, in->program, in->layout.program_bytes);
	memcpy(out->constants, in->constants, in->layout.constants_bytes);
	memcpy(out->constants + geo, in->geometry, in->layout.geometry_bytes);
	for (i = in->record_count; i < padded; i++)
		memcpy(out->constants + geo + 128 + i * in->record_stride,
		       in->geometry + 128 +
			       (in->record_count - 1) * in->record_stride,
		       in->record_stride);
	memcpy(out->primary, blob, 3696);
	memcpy(out->secondary, blob + 3696, 528);
	for (i = 0; i < groups; i++) {
		u8 *p = out->secondary + 528 + i * 24;

		m9_cmd_put(p, 4);
		m9_cmd_put(p + 4, 0x23232323);
		for (j = 0; j < 4; j++)
			m9_cmd_put(p + 8 + j * 4,
				   i * 4 + j < in->declaration_count ?
					   m9_cmd_get(in->declaration +
						      4 * (i * 4 + j)) :
					   0);
	}
	memcpy(out->secondary + 576 + extra, blob + 3696 + 576, 2712 - 576);
	for (i = 0; i < 4; i++) {
		static const u16 guard_at[] = { 76, 764, 1376, 1924 };

		m9x_faa_guard_field_put(out->secondary + moved(guard_at[i], extra),
					m9x_faa_guard_field_size(dimensions.width,
								 dimensions.height));
	}
	for (i = 0; i < ARRAY_SIZE(metadata); i++)
		if (metadata[i].a < 528 || metadata[i].a >= 576)
			m9_cmd_put(out->secondary + moved(metadata[i].a, extra),
				   metadata[i].b);
	for (i = 0; i < ARRAY_SIZE(fields); i++) {
		const struct field *f = &fields[i];

		offset = f->offset;
		/* One real COLOR stream is gathered into target0. The raster COLOR
		 * plane must follow that producer, rather than the disabled target1.
		 */
		if (arity == 1 && f->owner == 23 && f->at == 2300)
			offset = 0;
		if (f->owner == 29)
			offset = moved(offset, extra);
		if (f->owner == 28) {
			if (offset == 256)
				offset = geo;
			else if (f->at == 328)
				offset = geo + 128;
			else if (f->at == 372 || f->at == 376)
				offset = f->at == 372 ?
						 in->layout.constant_begin[1] :
						 in->layout.constant_end[1];
			else if (f->at == 1000 || f->at == 1004)
				offset = in->layout.constant_begin[2];
			else if (f->at == 1612 || f->at == 1616)
				offset = f->at == 1612 ?
						 in->layout.constant_begin[3] :
						 in->layout.constant_end[3];
			else if (f->at == 2364 || f->at == 2368)
				offset = f->at == 2364 ?
						 in->layout.constant_begin[0] :
						 in->layout.constant_end[0];
			offset += in->data_offset;
		}
		address = owner[f->owner].base + offset;
		value = f->pc == 0x248a0 ? (address & 0xfffff000) | f->tag :
			f->pc == 0x2490e ? (address | f->tag) & 0xfffffffc :
					   address | f->tag;
		m9_cmd_put(f->dst == 27 ? out->primary + f->at :
					  out->secondary + moved(f->at, extra),
			   value);
	}
	for (i = 0; i < ARRAY_SIZE(tokens); i++)
		m9_cmd_put(tokens[i].dst == 27 ?
				   out->primary + tokens[i].at :
				   out->secondary + moved(tokens[i].at, extra),
			   ((u32)init_point + tokens[i].delta) | tokens[i].tag);
	for (i = 0; i < ARRAY_SIZE(dims); i++)
		m9_cmd_put(dims[i].a == 27 ?
				   out->primary + dims[i].b :
				   out->secondary + moved(dims[i].b, extra),
			   (dimensions.height << 16) | dimensions.width);
	m9_cmd_put(out->primary + 320, 0xf0210000u | dimensions.width);
	m9_cmd_put(out->primary + 3348, 0xf0220000u | dimensions.width);
	m9_cmd_put(out->primary + 3352, 0x00010000u | dimensions.height);
	m9_cmd_put(out->secondary + moved(2064, extra),
		   dimensions.primary_descriptor);
	m9_cmd_put(out->secondary + moved(2080, extra),
		   dimensions.companion_descriptor);
	m9_cmd_put(out->secondary + moved(2096, extra),
		   dimensions.hz_descriptor);
	for (i = 0; i < 4; i++)
		m9_cmd_put(out->secondary + moved(2168 + i * 4, extra),
			   dimensions.ad);
	{
		const u32 slot[] = { 28, 704, 1316 },
			  target[] = { 660, 1280, 1828 };
		for (i = 0; i < 3; i++)
			m9_cmd_put(out->secondary + moved(slot[i], extra),
				   0x49d00000 | ((moved(target[i], extra) -
						  moved(slot[i], extra)) /
						 4));
	}
	{
		const u32 c2[] = { 2344, 352, 980, 1592 },
			  c3[] = { 2360, 368, 996, 1608 };
		for (i = 0; i < 4; i++) {
			m9_cmd_put(out->secondary + moved(c2[i], extra),
				   in->layout.entry_bytes[i] / 8);
			pairs = (in->layout.constant_end[i] -
				 in->layout.constant_begin[i]) /
				8;
			m9_cmd_put(out->secondary + moved(c3[i], extra),
				   0x00500000 | (pairs << 8));
		}
	}
	/* Genuine 3-to6 source differential identifies launch payloads,
	 * not their preceding packet word counts (which remain3/4).
	 */
	m9_cmd_put(out->secondary + 336, in->record_stride);
	/* Same original13a884 declaration-half count as the fused raw stage.
	 * Change only the caller-input head; gather/position controls describe
	 * their own generated declarations and retain their producer values.
	 */
	m9_cmd_put(out->secondary + 88,
		   (m9_cmd_get(out->secondary + 88) & ~0x1f00u) |
			   ((in->declaration_count - 1) << 8));
	for (i = 0; i < 3; i++) {
		const u32 padded_at[3] = { 644, 1264, 1824 };

		m9_cmd_put(out->secondary + moved(padded_at[i], extra), padded);
	}
	m9_cmd_put(out->secondary + moved(2708, extra), in->record_count);
	/* The COLOR raster frontend declares exactly one input, mapped toslot0.
	 * Source182500 populates only its AB halfword; other AB/AC halfwords
	 * copied by182541/182549 andfbee8 are uninitialized inactive residue.
	 * Give inactive slots defined zero masks rather than captured CPU residue.
	 */
	m9_cmd_put(out->secondary + moved(2220, extra), 0x0000ffff);
	m9_cmd_put(out->secondary + moved(2224, extra), 0);
	if (in->fragment_flags) {
		/* 1815cd lowers usage bit24 -> EF20000. 17f040 lowers compiler
		 * usage bits into the low mask and80000. 13a83d marks the flat
		 * COLOR varying pair's interpolation bit31. Preserve other live bits.
		 */
		m9_cmd_put(out->secondary + moved(1920, extra),
			   m9_cmd_get(out->secondary + moved(1920, extra)) &
				   ~0x20000u);
		m9_cmd_put(out->secondary + moved(2276, extra),
			   (m9_cmd_get(out->secondary + moved(2276, extra)) &
			    ~0x8ffffu) |
				   0xfff0u);
		m9_cmd_put(out->secondary + moved(2616, extra),
			   m9_cmd_get(out->secondary + moved(2616, extra)) |
				   0x80000000u);
		m9_cmd_put(out->secondary + moved(2620, extra),
			   m9_cmd_get(out->secondary + moved(2620, extra)) |
				   0x80000000u);
	}
	m9_cmd_put(out->secondary + 72, ef(1 + arity, 0x200));
	m9_cmd_put(out->secondary + moved(760, extra), ef(arity, 0x600));
	m9_cmd_put(out->secondary + 304,
		   (((1u << ((in->declaration_count - 1) / 2)) - 1) << 24) |
			   carry_mask);
	m9_cmd_put(out->secondary + 308, route(1 + arity, true, false));
	m9_cmd_put(out->secondary + moved(952, extra),
		   ((1u << arity) - 1) << 20 | 0xffff);
	m9_cmd_put(out->secondary + moved(956, extra),
		   route(arity, false, false));
	/* Packed carry record stride is four DWORDs per real target. */
	if (arity == 1)
		for (i = 0; i < 2; i++)
			m9_cmd_put(out->secondary + 120 + i * 16,
				   0xee190000 | 8 | (4 * i << 8));
	if (arity == 1) {
		/* Both downstream readers consume the same two-field packed carry
		 * record. The independent m9_record_desc SLOT grammar couples stride
		 * and field offset to the producer: eight DWORDs, COLOR at+4/POS at0.
		 */
		m9_cmd_put(out->secondary + moved(1036, extra), 0xee190408);
		m9_cmd_put(out->secondary + moved(1648, extra), 0xee190008);
		m9_cmd_put(out->secondary + 148, 0);
		m9_cmd_put(out->secondary + 152, 0xf0010040);
		m9_cmd_put(out->secondary + moved(820, extra), 0);
		m9_cmd_put(out->secondary + moved(824, extra), 0xf0010040);
		m9_cmd_put(out->secondary + moved(1048, extra), 0);
		m9_cmd_put(out->secondary + moved(1060, extra), 0);
		m9_cmd_put(out->secondary + moved(1064, extra), 0xf0010040);
	}
	*out->secondary_length = secondary;
	return 0;
}
