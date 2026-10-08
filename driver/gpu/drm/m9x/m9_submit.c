// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/overflow.h>
#include <linux/sched.h>
#include <linux/uaccess.h>

#include <drm/drm_file.h>
#include <drm/drm_exec.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_vram_helper.h>
#include <drm/drm_print.h>

#include "m9_drv.h"
#include "m9_reg.h"
#include "m9_resource69_inline.h"

/* Posting a job and retiring it at the next one lets the next job be built
 * while this one runs; the objects it reads are held until then. */
static bool m9_async_submit = true;
module_param_named(async_submit, m9_async_submit, bool, 0644);
MODULE_PARM_DESC(async_submit, "retire a job at the next submission");

/* Qualified only after an explicit aggregate pixel test; ordinary submits
 * and older userspace keep their existing path. */
static bool m9_aggregate_submit;
module_param_named(aggregate_submit, m9_aggregate_submit, bool, 0400);
MODULE_PARM_DESC(aggregate_submit, "admit bounded complete-job aggregation");

#define M9_TRIANGLE_COLOR_BYTES	0x00050000u

/* Dynamic debug is not built on this target, so name the refusing check
 * behind a parameter instead. */
/* Hierarchical Z is bound as the ba/bb auxiliary pair.  Clearing this emits
 * the vendor's disabled ba form instead, so its worth can be measured. */
static bool m9_hierarchical_z = true;
module_param_named(hierarchical_z, m9_hierarchical_z, bool, 0644);
MODULE_PARM_DESC(hierarchical_z, "bind the depth auxiliary (ba/bb) plane");

static bool m9_submit_trace;
module_param_named(submit_trace, m9_submit_trace, bool, 0644);
MODULE_PARM_DESC(submit_trace, "log the validator check that refuses a job");

#define m9_refuse(err) ({ \
	if (m9_submit_trace) \
		pr_warn_ratelimited("m9x: submit refused at %s:%d (%d)\n", \
				    __func__, __LINE__, (err)); \
	(err); })

static bool m9_user_submit;
module_param(m9_user_submit, bool, 0400);
MODULE_PARM_DESC(m9_user_submit,
	"Execute validated userspace M9 command streams (bring-up only)");

/*
 * Command indices the multisample fragment-store probe may write.  The list
 * is empty by default, so the validator is byte for byte what it ships.
 *
 * The vendor writes a1, a3 and a4 under the only packet-name string in its
 * image, RESET_FRAG_PTR (STATIC, MxMD.dll 0xfab30, 0xacadb), and no saved
 * vendor stream writes any of them (CAPTURED, 1206 decoded submissions).
 * MEASURED on the M9128: with the unit enabled the engine accepts the store's
 * address at every one of a0..a6, renders identically and writes nothing into
 * the store, so none of them binds it and none has earned a permanent entry
 * in m9_index_allowed().  The parameter stays so the search can continue, and
 * goes away with the index that closes it.
 */
#define M9_FAA_PROBE_MAX 16
static ushort m9_faa_probe_indices[M9_FAA_PROBE_MAX];
static unsigned int m9_faa_probe_count;
module_param_array_named(faa_probe_indices, m9_faa_probe_indices, ushort,
			 &m9_faa_probe_count, 0644);
MODULE_PARM_DESC(faa_probe_indices,
	"command indices the multisample fragment-store probe may write");

static bool m9_faa_probe_index(u8 index)
{
	unsigned int i;

	for (i = 0; i < m9_faa_probe_count && i < M9_FAA_PROBE_MAX; i++)
		if (m9_faa_probe_indices[i] == index)
			return true;
	return false;
}

/*
 * This is deliberately the captured-triangle whitelist, not all 256 indexed
 * registers. General execution remains disabled while the masks for these
 * state words are being reduced.
 */
static bool m9_index_allowed(u8 index)
{
	if (m9_faa_probe_index(index))
		return true;
	switch (index) {
	case 0x00 ... 0x09:
	case 0x10 ... 0x14: case 0x18: case 0x19:
	/* The occlusion snapshot address; see m9_query_packet. */
	case 0x1e:
	case 0x22: case 0x23: case 0x28: case 0x30:
	case 0x68 ... 0x6c:
	case 0x70 ... 0x87:
	case 0x88 ... 0x8a: case 0x8c ... 0x9f:
	/* The occlusion counter enable pair, a7 with c6. */
	case 0xa7:
	case 0xa8 ... 0xaf:
	case 0xb0 ... 0xbb: case 0xbe:
	case 0xc1 ... 0xc5: case 0xc6:
	case 0xd7: case 0xea ... 0xec: case 0xef ... 0xf1:
	case 0xf4 ... 0xf6:
		return true;
	default:
		return false;
	}
}

#define M9_RELOC_BIT(kind)	BIT(PARHELIA_M9_RELOC_##kind)
#define M9_RELOC_KIND_MASK	GENMASK(PARHELIA_M9_RELOC_FAA_POOL, 0)
/* Non-reloc markers in the per-dword expectation mask. */
#define M9_EXPECT_DEPENDENCY	BIT(31)
#define M9_EXPECT_OPTIONAL	BIT(30)
/* c3 selects the native program entry slot inside the c2 resource. */
#define M9_EXPECT_PROGRAM_ENTRY	BIT(29)

static u32 m9_index_reloc_kinds(u8 index)
{
	switch (index) {
	/* Stream slots 00..09: the vendor's widest captured raster pass reads
	 * nine of them -- six interpolated inputs plus position, c and d --
	 * with the packed strides in 10..14. */
	case 0x00 ... 0x09:
		return M9_RELOC_BIT(SYSTEM) | M9_RELOC_BIT(STREAM);
	case 0x1e:
		return M9_RELOC_BIT(QUERY);
	case 0x69:
		return M9_RELOC_BIT(RESOURCE_69);
	case 0x6c:
		return M9_RELOC_BIT(RESOURCE_6C);
	case 0x78:
		return M9_RELOC_BIT(TEXTURE);
	case 0x8d: case 0x8e:
		return M9_RELOC_BIT(SYSTEM);
	case 0xb0: case 0xb2: case 0xb4: case 0xb6:
		return M9_RELOC_BIT(COLOR) | M9_RELOC_BIT(STREAM);
	case 0xb8:
		return M9_RELOC_BIT(DEPTH) | M9_RELOC_BIT(STREAM);
	case 0xba:
		return M9_RELOC_BIT(DEPTH);
	case 0xbe:
		return M9_RELOC_BIT(STREAM);
	case 0xc2:
		return M9_RELOC_BIT(PROGRAM);
	default:
		/* A probed index may carry the store's address or a literal. */
		return m9_faa_probe_index(index) ?
			M9_RELOC_BIT(FRAGMENT) : 0;
	}
}

/*
 * The vendor emits small non-address literals for disabled ba/be/c2 forms, and
 * preprocessing passes carry a source control word in 01..04.
 */
static bool m9_index_literal(u8 index, u32 value)
{
	if (index >= 0x01 && index <= 0x04)
		return true;
	/* A probed index carries either a store address or a literal: the
	 * vendor writes zero at it, and a surface pair puts a descriptor at
	 * the index behind the address. */
	if (m9_faa_probe_index(index))
		return true;
	return (index == 0xba || index == 0xbe || index == 0xc2) &&
	       value < PAGE_SIZE;
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

/* A textured 88 combiner form without a bound texture stops the engine. */
static bool m9_textured_combiner(u32 value)
{
	return (value & 0xf) == 0x3;
}

/*
 * The engine transition the vendor puts between two draws of a native
 * fragment job: an epilogue closing the draw that has ended and a prologue
 * opening the next (CAPTURED, decoded in
 * docs/parhelia-m9-fragment-pass-splitting.md section 6).  Every word is a
 * fixed control literal with no address field.
 *
 *   epilogue  e6 = 0x38000000            end the block
 *             e6 = 0x40004000, 44, 44    publish what it wrote
 *             e6 = 0x0c000000            only behind a record-writing draw
 *   prologue  e6 = 0x60600060            open a range's first block
 *             e6 = 0x604fff9f            open a continuation block
 *             e6 = 0x2c000002 / 0x2c000000   the matching second word
 *             e6 = 0x49d00000 | length   see m9_transition_length
 *             e6 = 0x08000000            start the block
 */
static bool m9_transition_control(u32 value)
{
	switch (value) {
	case 0x38000000: case 0x40004000: case 0x0c000000:
	case 0x60600060: case 0x604fff9f:
	case 0x2c000000: case 0x2c000002:
	case 0x08000000:
		return true;
	default:
		return false;
	}
}

static bool m9_transition_length(u32 value)
{
	return (value & 0xffff0000) == 0x49d00000;
}

/*
 * The occlusion snapshot record (CAPTURED, the begin and end packets of
 * captures/m9x-linux-gl2-core-results-20260914 query-visible, query-partial
 * and query-hidden, each query-draw/bm.bin):
 * one indexed packet of exactly four values -- 1e naming the address the
 * engine publishes the counter pair to, then three fixed e6 control literals
 * selecting source pair 0/1 and ending the record.  Matching the shape and
 * the literals by position keeps both out of the rest of the stream.
 */
#define M9_QUERY_INDEX		0x1eu
/*
 * A snapshot is two published dwords.  The vendor's own allocation aligns and
 * sizes it by 0x20 (CAPTURED, allocation records 44..46 of the same capture),
 * so that is what a query target has to leave inside the object.
 */
#define M9_QUERY_BYTES		0x20u

static const u32 m9_query_literal[3] = {
	0x64000004, 0x64000101, 0x40008000,
};

static bool m9_query_packet(const u8 *index, unsigned int values)
{
	return index[0] == M9_QUERY_INDEX && values == 4 &&
	       index[1] == 0xe6 && index[2] == 0xe6 && index[3] == 0xe6;
}

/*
 * The pair that names the occlusion counter.  The vendor states it inside
 * every range of a job carrying a query and inside no other (CAPTURED); with
 * it the record publishes the sample count, without it a zero (MEASURED).
 * Neither word carries an address, and only the exact raster-range pair is
 * accepted.
 */
static bool m9_query_enable(u8 index, u32 value)
{
	if (index == 0xa7)
		return value == 0x00ffff7f;
	return index == 0xc6 && value == 0x00ffff09;
}

/* M9 vendor raster capture and MSerX 0xf7280 emit D0/D1 as one
 * scalar pair: normalized D24 units followed by slope factor. Neither
 * word is an address. Keep both outside the general index whitelist. */
static bool m9_depth_bias_packet(const u8 *index, unsigned int values)
{
	return values == 2 && index[0] == 0xd0 && index[1] == 0xd1;
}

/* Actual MSerX fb920 -> 25680/24870 with allocated pool6 and stores.
 * Index24 and the three E6 literals form one indivisible binding record.
 * This does not admit the separate indirect context-restore/save grammar:
 * its command backing needs a kernel-owned initialization/lifetime contract. */
#define M9_FAA_SURFACE_DWORDS 9u
#define M9_FAA_POOL_BYTES 0x580u

static int m9_parse_faa_surface(const u32 *words, u32 count, u32 *expected)
{
	if (count < M9_FAA_SURFACE_DWORDS ||
	    words[0] != 0x45454524 || words[2] != 0x45e6e6e6 ||
	    words[3] != 0x6dc00000 || words[4] != 1 || words[5] != 1 ||
	    words[6] != 0x4545bdbc)
		return m9_refuse(-EPERM);
	expected[1] = M9_RELOC_BIT(FAA_POOL);
	expected[7] = expected[8] = M9_RELOC_BIT(FRAGMENT);
	return 0;
}

static int m9_parse_commands(const u32 *words, u32 count, u32 *expected,
			     u32 program_type)
{
	bool textured = false, texture = false;
	u32 cursor = 0;

	while (cursor < count) {
		u32 header = words[cursor++], family;
		u8 index[4];
		unsigned int values = 4, i;

		if (header == 0x45454545)
			continue;
		if (header == 0x45454524) {
			int ret = m9_parse_faa_surface(words + cursor - 1,
						     count - cursor + 1,
							     expected + cursor - 1);

			if (ret)
				return ret;
			cursor += M9_FAA_SURFACE_DWORDS - 1;
			continue;
		}
		if (header == PARHELIA_M9_RANGE_BREAK) {
			if (cursor >= count || words[cursor] ||
			    (textured && !texture))
				return m9_refuse(-EINVAL);
			textured = texture = false;
			cursor++;
			continue;
		}
		if (m9_draw_header(header, &family)) {
			if (count - cursor < 2)
				return m9_refuse(-EINVAL);
			if (family == 0x03000001) {
				if (words[cursor + 1] < words[cursor])
					return m9_refuse(-EINVAL);
			} else if (family == 0x07000001) {
				expected[cursor] = M9_RELOC_BIT(DRAW32_START);
				expected[cursor + 1] = M9_RELOC_BIT(DRAW32_END);
			} else {
				expected[cursor] = M9_RELOC_BIT(DRAW16_START);
				expected[cursor + 1] = M9_RELOC_BIT(DRAW16_END);
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
			return m9_refuse(-EINVAL);
		for (i = values; i < 4; i++)
			if (index[i] != M9_PACKET_STOP)
				return m9_refuse(-EINVAL);
		for (i = 0; i < values; i++) {
			u32 value = words[cursor + i];
			bool inline_private = program_type ==
				PARHELIA_M9_PROGRAM_FIXED_P00_S28_INLINE &&
				cursor + i < M9_RESOURCE69_COUNT_FREE_DWORDS &&
				index[i] >= 0x60 && index[i] <= 0x67;

			if (index[i] == 0xd0 || index[i] == 0xd1) {
				if (!m9_depth_bias_packet(index, values) ||
				    (value & 0x7f800000) == 0x7f800000)
					return m9_refuse(-EPERM);
				/* Finite IEEE-754 scalars admit no relocation. */
				expected[cursor + i] = 0;
				continue;
			}
			/* Dependency tokens are rebased onto the kernel target. */
			if (index[i] == 0xe3 || index[i] == 0x43) {
				if (value & 0xf ||
				    value >= PARHELIA_M9_DEPENDENCY_LIMIT)
					return m9_refuse(-EPERM);
				expected[cursor + i] = M9_EXPECT_DEPENDENCY;
				continue;
			}
			if (index[i] == 0x88 && m9_textured_combiner(value))
				textured = true;
			if (index[i] == 0x78)
				texture = true;
			if (index[i] == 0xc3)
				expected[cursor + i] |= M9_EXPECT_PROGRAM_ENTRY;
			/* A query record's three literals are fixed and
			 * positional, and 1e occurs in no other record. */
			if (m9_query_packet(index, values)) {
				if (i) {
					if (value != m9_query_literal[i - 1])
						return m9_refuse(-EPERM);
					continue;
				}
			} else if (index[i] == M9_QUERY_INDEX) {
				return m9_refuse(-EPERM);
			}
			if ((index[i] == 0xa7 || index[i] == 0xc6) &&
			    !m9_query_enable(index[i], value))
				return m9_refuse(-EPERM);
			/* Only the observed pass-separator controls of e6. */
			if (index[i] == 0xe6) {
				if (m9_transition_length(value)) {
					/*
					 * The inter-draw prologue's third word
					 * is 0x49d00000 | n, n the length in
					 * dwords of the command block it opens,
					 * counted from this packet's own header
					 * (CAPTURED, exact for all 343 vendor
					 * prologues in
					 * captures/m9x-vendor-glsl-programs-20260915
					 * and captures/m9x-vendor-openbits-20260917).
					 * The engine follows it through the
					 * command stream, so bound it by the
					 * stream the kernel has validated.
					 */
					if (cursor - 1 + (value & 0xffff) >
					    count)
						return m9_refuse(-EPERM);
					continue;
				}
				if (!m9_transition_control(value) &&
				    value != 0x00000003 &&
				    value != 0x42000000)
					return m9_refuse(-EPERM);
				continue;
			}
			/*
			 * Index 44 occurs only inside that transition, always
			 * as the pair behind e6 = 0x40004000 and always with
			 * that same literal (CAPTURED, 460 writes, no other
			 * value).  It carries no address and no length, so the
			 * one literal is all a client can ask for.
			 */
			if (index[i] == 0x44) {
				if (value != 0x40004000)
					return m9_refuse(-EPERM);
				continue;
			}
			/* Fence and launch ownership stays in the kernel. */
			if (index[i] == 0x1a || index[i] == 0x1b ||
			    index[i] == 0x1c ||
			    (index[i] >= 0xc8 && index[i] <= 0xcd) ||
			    (!inline_private && !m9_index_allowed(index[i])))
				return m9_refuse(-EPERM);
			/* The exact inline atom ends with literal 69=4,6a=0x990000. */
			if (program_type ==
				PARHELIA_M9_PROGRAM_FIXED_P00_S28_INLINE &&
			    index[i] == 0x69 && cursor + i ==
				M9_RESOURCE69_COUNT_FREE_DWORDS - 2)
				continue;
			expected[cursor + i] = m9_index_reloc_kinds(index[i]) |
				(expected[cursor + i] & M9_EXPECT_PROGRAM_ENTRY);
			if (m9_index_literal(index[i], value))
				expected[cursor + i] |= M9_EXPECT_OPTIONAL;
		}
		cursor += values;
	}
	return textured && !texture ? m9_refuse(-EINVAL) : 0;
}

static struct drm_gem_object *
m9_find_ref(struct drm_gem_object **bos,
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
	case PARHELIA_M9_RELOC_PROGRAM:
	case PARHELIA_M9_RELOC_STREAM:
	/* The store is the engine's own scratch: it writes and reads it. */
	case PARHELIA_M9_RELOC_FRAGMENT:
	case PARHELIA_M9_RELOC_FAA_POOL:
		return PARHELIA_M9_BO_READ | PARHELIA_M9_BO_WRITE;
	/* The engine only publishes the counter pair; it reads nothing back. */
	case PARHELIA_M9_RELOC_QUERY:
		return PARHELIA_M9_BO_WRITE;
	case PARHELIA_M9_RELOC_SYSTEM:
	case PARHELIA_M9_RELOC_TEXTURE:
	case PARHELIA_M9_RELOC_RESOURCE_69:
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
				return m9_refuse(-EINVAL);
			if (family == 0x03000001) {
				cursor += 2;
				continue;
			}
			start = by_dword[cursor];
			end = by_dword[cursor + 1];
			if (!start || !end || start->handle != end->handle ||
			    end->target_offset < start->target_offset)
				return m9_refuse(-EINVAL);
			if (family == 0x07000001) {
				if (start->kind != PARHELIA_M9_RELOC_DRAW32_START ||
				    end->kind != PARHELIA_M9_RELOC_DRAW32_END ||
				    (start->target_offset | end->target_offset) & 3)
					return m9_refuse(-EINVAL);
			} else {
				if (start->kind != PARHELIA_M9_RELOC_DRAW16_START ||
				    end->kind != PARHELIA_M9_RELOC_DRAW16_END ||
				    (start->target_offset | end->target_offset) & 3 ||
				    ((end->flags & PARHELIA_M9_RELOC_PACKED16_ODD) &&
				     end->target_offset == start->target_offset))
					return m9_refuse(-EINVAL);
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
			return m9_refuse(-EINVAL);
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
	case PARHELIA_M9_RELOC_PROGRAM:
	/* Page alignment until a measurement says what the unit needs. */
	case PARHELIA_M9_RELOC_FRAGMENT:
		return 4096;
	case PARHELIA_M9_RELOC_RESOURCE_6C:
		return 128;
	case PARHELIA_M9_RELOC_QUERY:
		return M9_QUERY_BYTES;
	case PARHELIA_M9_RELOC_FAA_POOL:
		return 128;
	default:
		return 4;
	}
}

static bool m9_reloc_uses_vram(u32 kind)
{
	return kind == PARHELIA_M9_RELOC_COLOR ||
	       kind == PARHELIA_M9_RELOC_DEPTH ||
	       kind == PARHELIA_M9_RELOC_TEXTURE ||
	       kind == PARHELIA_M9_RELOC_RESOURCE_69 ||
	       kind == PARHELIA_M9_RELOC_PROGRAM ||
	       kind == PARHELIA_M9_RELOC_STREAM ||
	       kind == PARHELIA_M9_RELOC_QUERY ||
	       kind == PARHELIA_M9_RELOC_FRAGMENT ||
	       kind == PARHELIA_M9_RELOC_FAA_POOL;
}

static bool m9_reloc_tagged_vram(u32 kind)
{
	/* Actual kind9/a store and kind2b pool getters return GPU address|1,
	 * even though the backing allocation kind is VRAM. This tag must not
	 * select the SYSTEM compact-leaf address domain. */
	return kind == PARHELIA_M9_RELOC_FRAGMENT ||
	       kind == PARHELIA_M9_RELOC_FAA_POOL;
}

static bool m9_reloc_executable(u32 kind)
{
	/* Pool6 participates in indirect context commands. The traced binding
	 * is available for validation, but arbitrary client-owned backing must
	 * not execute before a kernel-owned pool producer is implemented. */
	return kind != PARHELIA_M9_RELOC_FAA_POOL;
}

static u32 m9_reloc_minimum_bytes(u32 kind)
{
	if (kind == PARHELIA_M9_RELOC_QUERY)
		return M9_QUERY_BYTES;
	if (kind == PARHELIA_M9_RELOC_FAA_POOL)
		return M9_FAA_POOL_BYTES;
	return 0;
}

static int m9_vram_gpu_address(u64 limit, u64 offset, u64 target_offset,
			       u64 *address)
{
	if (offset >= limit ||
	    check_add_overflow(offset, target_offset, address) ||
	    *address >= limit ||
	    check_add_overflow(*address, M9_VRAM_GPU_BASE, address))
		return m9_refuse(-EOVERFLOW);
	return 0;
}

static int m9_submit_first_triangle(struct m9x_device *mdev,
				    struct drm_file *file,
				    struct drm_parhelia_m9_submit *args)
{
	struct drm_parhelia_m9_bo_ref ref;
	struct drm_gem_vram_object *vram;
	struct drm_gem_object *gem;
	bool color_active, color_retained = false;
	long wait;
	s64 offset;
	u64 address;
	int ret;

	if (args->command_handle || args->command_offset ||
	    args->command_length || args->relocation_count ||
	    args->relocations_ptr || args->bo_count != 1 || !args->bos_ptr ||
	    args->program_type)
		return m9_refuse(-EINVAL);
	if (copy_from_user(&ref, u64_to_user_ptr(args->bos_ptr), sizeof(ref)))
		return -EFAULT;
	if (!ref.handle || ref.flags !=
		(PARHELIA_M9_BO_READ | PARHELIA_M9_BO_WRITE))
		return m9_refuse(-EINVAL);
	/* Allocate and pin every private diagnostic BO before reserving the
	 * userspace color BO.  Later transport work then performs no nested TTM
	 * reservation under the color reservation. */
	ret = m9x_faa_diag_enabled() ? m9x_faa_prepare_diag(mdev) :
		m9x_triangle_prepare_diag(mdev);
	if (ret)
		return ret;
	gem = drm_gem_object_lookup(file, ref.handle);
	if (!gem)
		return -ENOENT;
	if (!m9x_gem_is_vram(gem) || gem->size < M9_TRIANGLE_COLOR_BYTES) {
		ret = m9_refuse(-EINVAL);
		goto out_put;
	}
	vram = drm_gem_vram_of_gem(gem);
	ret = m9x_vram_pin(vram);
	if (ret)
		goto out_put;
	ret = ttm_bo_reserve(&vram->bo, true, false, NULL);
	if (ret)
		goto out_unpin;
	offset = drm_gem_vram_offset(vram);
	if (offset < 0) {
		ret = offset;
		goto out_unreserve;
	}
	ret = m9_vram_gpu_address(m9x_vram_gpu_limit(mdev), offset,
				  M9_TRIANGLE_COLOR_BYTES - 1, &address);
	if (ret)
		goto out_unreserve;
	address -= M9_TRIANGLE_COLOR_BYTES - 1;

	/*
	 * Do not wait on the intentionally unsignaled fence retained by a prior
	 * timed-out job.  All lock nesting in this path is COLOR RESV ->
	 * TRANSPORT.
	 */
	mutex_lock(&mdev->transport.lock);
	m9x_transport_drain_locked(mdev);
	ret = mdev->transport.wedged ? -EIO : 0;
	mutex_unlock(&mdev->transport.lock);
	if (ret)
		goto out_unreserve;
	wait = dma_resv_wait_timeout(gem->resv, dma_resv_usage_rw(true), true,
				     MAX_SCHEDULE_TIMEOUT);
	if (wait <= 0) {
		ret = wait < 0 ? wait : -ETIMEDOUT;
		goto out_unreserve;
	}
	ret = dma_resv_reserve_fences(gem->resv, 1);
	if (ret)
		goto out_unreserve;
	ret = m9x_faa_diag_enabled() ?
		m9x_faa_submit_diag(mdev, lower_32_bits(address), vram,
				  &args->out_point, &color_active, &color_retained) :
		m9x_submit_triangle_diag(mdev, lower_32_bits(address), vram,
					&args->out_point, &color_active);
	if (color_active || color_retained) {
		/* Posted failures transfer the pin/reference to quarantine.
		 * Successful FAA diagnostics retain them for later readback,
		 * including profiles without an ordinary output allocation.
		 * Both owners release only after requester DMA shutdown.
		 */
		ttm_bo_unreserve(&vram->bo);
		return ret;
	}
	ttm_bo_unreserve(&vram->bo);
	goto out_unpin;
out_unreserve:
	ttm_bo_unreserve(&vram->bo);
out_unpin:
	m9x_vram_unpin(vram);
out_put:
	drm_gem_object_put(gem);
	return ret;
}

/* Both later public caller boundaries continue the already-owned private
 * primary. The first submit retains its backing across these temporary pins. */
static int m9_submit_faa_continuation(struct m9x_device *mdev,
			       struct drm_file *file,
			       struct drm_parhelia_m9_submit *args, bool relock)
{
	struct drm_parhelia_m9_bo_ref ref;
	struct drm_gem_vram_object *vram;
	struct drm_gem_object *gem;
	bool posted = false;
	long wait;
	s64 offset;
	u64 address;
	int ret;

	if (!m9x_faa_diag_enabled())
		return -EOPNOTSUPP;
	if (args->command_handle || args->command_offset ||
	    args->command_length || args->relocation_count ||
	    args->relocations_ptr || args->bo_count != 1 || !args->bos_ptr ||
	    args->program_type)
		return m9_refuse(-EINVAL);
	if (copy_from_user(&ref, u64_to_user_ptr(args->bos_ptr), sizeof(ref)))
		return -EFAULT;
	if (!ref.handle || ref.flags !=
	    (PARHELIA_M9_BO_READ | PARHELIA_M9_BO_WRITE))
		return m9_refuse(-EINVAL);
	gem = drm_gem_object_lookup(file, ref.handle);
	if (!gem)
		return -ENOENT;
	if (!m9x_gem_is_vram(gem) || !m9x_gem_vram_tiled(gem) ||
	    gem->size != 0x50000) {
		ret = m9_refuse(-EINVAL);
		goto out_put;
	}
	vram = drm_gem_vram_of_gem(gem);
	ret = m9x_vram_pin(vram);
	if (ret)
		goto out_put;
	ret = ttm_bo_reserve(&vram->bo, true, false, NULL);
	if (ret)
		goto out_unpin;
	/* Wait for the previous public caller's completed write before adding
	 * the next continuation fence; posted failure fences stay quarantined. */
	mutex_lock(&mdev->transport.lock);
	m9x_transport_drain_locked(mdev);
	ret = mdev->transport.wedged ? -EIO : 0;
	mutex_unlock(&mdev->transport.lock);
	if (ret)
		goto out_unreserve;
	wait = dma_resv_wait_timeout(gem->resv, dma_resv_usage_rw(true), true,
				     MAX_SCHEDULE_TIMEOUT);
	if (wait <= 0) {
		ret = wait < 0 ? wait : -ETIMEDOUT;
		goto out_unreserve;
	}
	offset = drm_gem_vram_offset(vram);
	if (offset < 0) {
		ret = offset;
		goto out_unreserve;
	}
	ret = m9_vram_gpu_address(m9x_vram_gpu_limit(mdev), offset,
				  0x50000 - 1, &address);
	if (ret)
		goto out_unreserve;
	address -= 0x50000 - 1;
	ret = dma_resv_reserve_fences(gem->resv, 1);
	if (ret)
		goto out_unreserve;
	ret = relock ?
		m9x_faa_relock_diag(mdev, lower_32_bits(address), vram,
				     &args->out_point, &posted) :
		m9x_faa_unlock_diag(mdev, lower_32_bits(address), vram,
				     &args->out_point, &posted);
out_unreserve:
	ttm_bo_unreserve(&vram->bo);
out_unpin:
	m9x_vram_unpin(vram);
out_put:
	drm_gem_object_put(gem);
	return ret;
}

/* CPU-only state for one ioctl. Nothing here escapes into a queued owner.
 * Relocation validation finishes before framing, so their pointer/word
 * scratch can share storage. GEM object/pin arrays, fences and retirement records
 * remain separate allocations with their existing GPU lifetime. */
struct m9_submit_scratch {
	void *storage;
	size_t bytes;
	u32 *image, *expected, *required_access, *range_ends, *framed;
	const struct drm_parhelia_m9_reloc **by_dword;
	struct drm_parhelia_m9_reloc *relocs;
	struct drm_parhelia_m9_bo_ref *refs;
};

static int m9_scratch_reserve(size_t *total, size_t count, size_t size,
			      size_t alignment, size_t *offset)
{
	size_t aligned, bytes;

	if (check_add_overflow(*total, alignment - 1, &aligned) ||
	    check_mul_overflow(count, size, &bytes))
		return -EOVERFLOW;
	aligned &= ~(alignment - 1);
	if (check_add_overflow(aligned, bytes, total))
		return -EOVERFLOW;
	*offset = aligned;
	return 0;
}

static int m9_submit_scratch_init(struct m9_submit_scratch *scratch,
				u32 dwords, u32 bo_count, u32 reloc_count)
{
	size_t total = 0, image, expected, refs, relocs, shared, access;
	size_t shared_bytes, access_words;
	u8 *base;
	int ret;

	if (!dwords || dwords > PARHELIA_M9_COMMAND_BYTES / sizeof(u32) ||
	    bo_count > PARHELIA_M9_MAX_BOS ||
	    reloc_count > PARHELIA_M9_MAX_RELOCS || reloc_count > dwords)
		return -EINVAL;
	shared_bytes = max_t(size_t, dwords * sizeof(*scratch->by_dword),
				 (2 * dwords + 44) * sizeof(*scratch->framed));
	access_words = max_t(size_t, bo_count, dwords / 2 + 5);
#define M9_SCRATCH_REGION(field, count, type) do { \
	ret = m9_scratch_reserve(&total, count, sizeof(type), \
				 __alignof__(type), &field); \
	if (ret) \
		return ret; \
} while (0)
	M9_SCRATCH_REGION(image, dwords, u32);
	M9_SCRATCH_REGION(expected, dwords, u32);
	M9_SCRATCH_REGION(refs, bo_count, struct drm_parhelia_m9_bo_ref);
	M9_SCRATCH_REGION(relocs, reloc_count, struct drm_parhelia_m9_reloc);
	M9_SCRATCH_REGION(shared, shared_bytes / sizeof(u64), u64);
	M9_SCRATCH_REGION(access, access_words, u32);
#undef M9_SCRATCH_REGION
	/* Initial masks/pointer slots are zero, just as the former per-array
	 * callocs. Every later framed/range word is assigned before use. */
	base = kvzalloc(total, GFP_KERNEL);
	if (!base)
		return -ENOMEM;
	*scratch = (struct m9_submit_scratch) {
		.storage = base, .bytes = total,
		.image = (u32 *)(base + image),
		.expected = (u32 *)(base + expected),
		.refs = (void *)(base + refs),
		.relocs = (void *)(base + relocs),
		.by_dword = (void *)(base + shared),
		.framed = (u32 *)(base + shared),
		.required_access = (u32 *)(base + access),
		.range_ends = (u32 *)(base + access),
	};
	return 0;
}

static int
m9_validate_relocs(u32 *image, u32 dwords, u32 *expected,
		   const struct drm_parhelia_m9_reloc *relocs, u32 reloc_count,
		   struct drm_gem_object **bos, struct m9x_device *mdev,
		   const struct drm_parhelia_m9_bo_ref *refs, u32 bo_count,
		   const struct m9_submit_scratch *scratch)
{
	const struct drm_parhelia_m9_reloc **by_dword = scratch->by_dword;
	u32 *required_access = scratch->required_access;
	u32 i;
	int ret = 0;

	for (i = 0; i < reloc_count; i++) {
		const struct drm_parhelia_m9_reloc *reloc = &relocs[i];
		struct drm_gem_object *gem;
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
		    reloc->kind > PARHELIA_M9_RELOC_FAA_POOL ||
		    !(expected[reloc->command_dword] & BIT(reloc->kind)) ||
		    reloc->kind == PARHELIA_M9_RELOC_FENCE ||
		    reloc->kind == PARHELIA_M9_RELOC_SPECIAL ||
		    !access || reloc->flags & ~permitted_flags ||
		    (reloc->flags & (PARHELIA_M9_BO_READ |
				      PARHELIA_M9_BO_WRITE)) != access ||
		    by_dword[reloc->command_dword] ||
		    reloc->pad[0] || reloc->pad[1]) {
			drm_dbg(&mdev->drm, "M9 reloc %u kind %u at dword %u rejected\n",
				i, reloc->kind, reloc->command_dword);
			ret = m9_refuse(-EINVAL);
			goto out;
		}
		gem = m9_find_ref(bos, refs, bo_count, reloc->handle, access);
		end_reloc = reloc->kind == PARHELIA_M9_RELOC_DRAW32_END ||
			    reloc->kind == PARHELIA_M9_RELOC_DRAW16_END ||
			    reloc->kind == PARHELIA_M9_RELOC_SYSTEM;
		if (!gem || reloc->target_offset > gem->size ||
		    (!end_reloc && reloc->target_offset == gem->size)) {
			ret = m9_refuse(-EINVAL);
			goto out;
		}
		/* Snapshot and pool records need their complete owned extent,
		 * including the pool's128-byte guard, beyond the target offset. */
		if (gem->size - reloc->target_offset <
		    m9_reloc_minimum_bytes(reloc->kind)) {
			ret = m9_refuse(-EINVAL);
			goto out;
		}
		for (bo_index = 0; bo_index < bo_count; bo_index++)
			if (bos[bo_index] == gem)
				break;
		if (bo_index == bo_count) {
			ret = m9_refuse(-EINVAL);
			goto out;
		}
		required_access[bo_index] |= access;
		alignment = m9_reloc_alignment(reloc->kind);
		if (reloc->target_offset & (alignment - 1)) {
			ret = m9_refuse(-EINVAL);
			goto out;
		}
		if (m9_reloc_uses_vram(reloc->kind)) {
			s64 offset;

			if (!m9x_gem_in_vram(gem)) {
				ret = m9_refuse(-EINVAL);
				goto out;
			}
			offset = m9x_gem_vram_offset(gem);
			if (offset < 0) {
				ret = offset;
				goto out;
			}
			ret = m9_vram_gpu_address(m9x_vram_gpu_limit(mdev),
						  offset, reloc->target_offset,
						  &address);
			if (ret)
				goto out;
		} else {
			if (!m9x_gem_is_dma(gem)) {
				ret = m9_refuse(-EINVAL);
				goto out;
			}
			bo = container_of(to_drm_gem_dma_obj(gem),
					  struct m9x_bo, dma);
			/* Class-1 dispatch addresses are compact-leaf GPUVAs. */
			if (reloc->kind == PARHELIA_M9_RELOC_SYSTEM ||
			    reloc->kind == PARHELIA_M9_RELOC_RESOURCE_6C) {
				if (!drm_mm_node_allocated(&bo->system)) {
					ret = m9_refuse(-EINVAL);
					goto out;
				}
				address = bo->system.start;
			} else if (m9x_bo_is_paged(bo)) {
				/* Raw bus addresses need a contiguous allocation. */
				if (m9_submit_trace)
					pr_warn_ratelimited("m9x: paged BO handle %u size %zu kind %u dword %u\n",
						reloc->handle, gem->size,
						reloc->kind, reloc->command_dword);
				ret = m9_refuse(-EINVAL);
				goto out;
			} else {
				address = bo->dma.dma_addr;
			}
			if (check_add_overflow(address, reloc->target_offset,
					       &address) ||
			    address > U32_MAX) {
				ret = m9_refuse(-EOVERFLOW);
				goto out;
			}
		}
		if (reloc->kind == PARHELIA_M9_RELOC_REQUESTER ||
		    reloc->kind == PARHELIA_M9_RELOC_DRAW32_START ||
		    reloc->kind == PARHELIA_M9_RELOC_DRAW32_END ||
		    reloc->kind == PARHELIA_M9_RELOC_DRAW16_START ||
		    reloc->kind == PARHELIA_M9_RELOC_DRAW16_END)
			address |= M9_COMMAND_SYSTEM_TAG;
		else if (reloc->kind == PARHELIA_M9_RELOC_RESOURCE_6C ||
			 reloc->kind == PARHELIA_M9_RELOC_SYSTEM ||
			 m9_reloc_tagged_vram(reloc->kind))
			address |= 1;
		if (reloc->kind == PARHELIA_M9_RELOC_DRAW16_END &&
		    reloc->flags & PARHELIA_M9_RELOC_PACKED16_ODD)
			address |= 2;
		/* The entry slot of this packet's c3 must lie inside the program BO. */
		if (reloc->kind == PARHELIA_M9_RELOC_PROGRAM) {
			u32 d;

			for (d = reloc->command_dword + 1;
			     d < dwords && d <= reloc->command_dword + 3; d++)
				if ((expected[d] & M9_EXPECT_PROGRAM_ENTRY) &&
				    ((u64)image[d] + 1) * 8 > gem->size - reloc->target_offset) {
					ret = m9_refuse(-EINVAL);
					goto out;
				}
		}
		/* Only index ba expects DEPTH alone; b8 expects DEPTH|STREAM. */
		if (!m9_hierarchical_z &&
		    (expected[reloc->command_dword] & M9_RELOC_KIND_MASK) ==
		    M9_RELOC_BIT(DEPTH)) {
			pr_info_once("m9x: hierarchical Z unbound at dword %u\n",
				     reloc->command_dword);
			address = 0;
		}
		image[reloc->command_dword] = lower_32_bits(address);
		drm_dbg(&mdev->drm, "M9 reloc dword %u kind %u -> %#llx\n",
			reloc->command_dword, reloc->kind, address);
		by_dword[reloc->command_dword] = reloc;
		expected[reloc->command_dword] = 0;
	}
	for (i = 0; i < dwords; i++)
		if ((expected[i] & M9_RELOC_KIND_MASK) &&
		    !(expected[i] & M9_EXPECT_OPTIONAL)) {
			drm_dbg(&mdev->drm, "M9 dword %u lacks a %#x relocation\n",
				i, expected[i]);
			ret = m9_refuse(-EINVAL);
			goto out;
		}
	for (i = 0; i < bo_count; i++)
		if (refs[i].flags != required_access[i]) {
			ret = m9_refuse(-EINVAL);
			goto out;
		}
	ret = m9_validate_draw_reloc_pairs(image, dwords, by_dword);
out:
	return ret;
}

static void m9_put_bos(struct drm_gem_object **bos, u32 count)
{
	u32 i;

	if (!bos)
		return;
	for (i = 0; i < count; i++)
		if (bos[i])
			drm_gem_object_put(bos[i]);
}

/*
 * The engine consumes counted records: a count, the four-index header and
 * that many values.  Numeric draw triples stay uncounted, as in bm-12.
 * Dependency tokens are rebased onto target; range breaks split ranges.
 */
static u32 *m9_frame_records(const u32 *image, const u32 *expected, u32 dwords,
			     u32 target, u32 *out_dwords, u32 *range_ends,
			     u32 *ranges, u32 *max_token, u32 *framed)
{
	u32 cursor = 0, out = 0;

	*ranges = 0;
	*max_token = 0;
	while (cursor < dwords) {
		u32 header = image[cursor], family;
		unsigned int values = 4, i;

		if (header == 0x45454545) {
			cursor++;
			continue;
		}
		if (header == PARHELIA_M9_RANGE_BREAK) {
			if (out && (!*ranges || range_ends[*ranges - 1] < out))
				range_ends[(*ranges)++] = out;
			cursor += 2;
			continue;
		}
		if (m9_draw_header(header, &family)) {
			memcpy(framed + out, image + cursor, 3 * sizeof(u32));
			out += 3;
			cursor += 3;
			continue;
		}
		for (i = 0; i < 4; i++)
			if (((header >> (8 * i)) & 0xff) == M9_PACKET_STOP) {
				values = i;
				break;
			}
		framed[out++] = values;
		framed[out++] = header;
		for (i = 1; i <= values; i++) {
			u32 value = image[cursor + i];

			if (expected[cursor + i] & M9_EXPECT_DEPENDENCY) {
				*max_token = max(*max_token, value);
				value += target;
			}
			framed[out++] = value;
		}
		cursor += values + 1;
	}
	if (out && (!*ranges || range_ends[*ranges - 1] < out))
		range_ends[(*ranges)++] = out;
	*out_dwords = out;
	return framed;
}

/*
 * Execute a validated image.  A posted job takes over its objects inside the
 * same hold of the transport lock that posts it: released and taken again,
 * another submitter could drain this job and post its own in between, and the
 * objects would be handed to that job or released while this one still reads
 * them.
 */
#include "m9_submit_aggregate.h"

/* Recognize markers only at top-level packet boundaries. Parse each job
 * independently, then retain global relocation indices and the BO access
 * union. No query or fixed-inline-program state crosses a complete job. */
static int
m9_parse_aggregate(u32 *image, u32 dwords, u32 *expected,
                   struct m9_complete_job *jobs, u32 *count)
{
 u32 at = 0, start = 0, n = 0;
 while (at < dwords) {
  u32 header = image[at], family;
  unsigned values = 0;
  if (header == PARHELIA_M9_JOB_BREAK) {
   int ret;
   if (dwords - at < 2 || image[at + 1] || at == start ||
       n >= M9_COMPLETE_MAX_JOBS - 1)
    return -EINVAL;
   jobs[n++] = (struct m9_complete_job){image + start, expected + start, at - start};
   ret = m9_parse_commands(image + start, at - start, expected + start,
                           PARHELIA_M9_PROGRAM_NONE);
   if (ret) return ret;
   image[at] = PARHELIA_M9_RANGE_BREAK;
   at += 2; start = at; continue;
  }
  if (header == 0x45454524) return -EOPNOTSUPP;
  if (header == 0x45454545) { at++; continue; }
  if (header == PARHELIA_M9_RANGE_BREAK) {
   if (dwords - at < 2 || image[at + 1]) return -EINVAL;
   at += 2; continue;
  }
  if (m9_draw_header(header, &family)) {
   if (dwords - at < 3) return -EINVAL;
   at += 3; continue;
  }
  while (values < 4 && ((header >> (8 * values)) & 255) != 0x45) {
   if (((header >> (8 * values)) & 255) == 0x1e) return -EOPNOTSUPP;
   values++;
  }
  if (!values || dwords - at < values + 1) return -EINVAL;
  at += values + 1;
 }
 if (start == dwords || !n) return -EINVAL;
 jobs[n++] = (struct m9_complete_job){image + start, expected + start, dwords - start};
 {
  int ret = m9_parse_commands(image + start, dwords - start, expected + start,
                             PARHELIA_M9_PROGRAM_NONE);
  if (ret) return ret;
 }
 *count = n;
 return 0;
}

static int m9_wait_foreign_fences(struct m9x_device *mdev,
				struct drm_gem_object **bos,
				const struct drm_parhelia_m9_bo_ref *refs, u32 count)
{
	u32 i;

	for (i = 0; i < count; i++) {
		struct dma_resv_iter cursor;
		struct dma_fence *fence;
		bool write = refs[i].flags & PARHELIA_M9_BO_WRITE;

		dma_resv_for_each_fence(&cursor, bos[i]->resv,
				       dma_resv_usage_rw(write), fence) {
			long waited;
			int status;

			/* Our primary stream is ordered; the following transport
			 * drain/owner handoff establishes this dependency. Waiting
			 * it here would unnecessarily serialize CPU preparation. */
			if (fence->context == mdev->transport.fence_context)
				continue;
			waited = dma_fence_wait_timeout(fence, true,
						      MAX_SCHEDULE_TIMEOUT);
			if (waited <= 0)
				return waited < 0 ? waited : -ETIMEDOUT;
			status = dma_fence_get_status(fence);
			if (status < 0)
				return status;
		}
	}
	return 0;
}

static int m9_submit_execute(struct m9x_device *mdev, const u32 *image,
			     const u32 *expected, u32 dwords, u64 *point,
			     bool async, bool queued, struct drm_gem_object ***bos,
			     bool **pinned,
			     const struct drm_parhelia_m9_bo_ref *refs, u32 count,
			     const struct m9_submit_scratch *scratch,
                             const struct m9_complete_job *jobs, u32 job_count)
{
	struct drm_exec exec;
	struct m9x_fence *fence = NULL;
	struct m9_retired *retired;
	u32 *framed, *range_ends, framed_dwords, ranges, max_token, target, final;
	u32 secondary_slot = M9_QUEUE_SLOTS;
	u64 issued;
	bool posted = false;
	int ret;

	range_ends = scratch->range_ends;
	retired = kzalloc(sizeof(*retired), GFP_KERNEL);
	if (!retired)
		return -ENOMEM;
	ret = queued ? m9x_triangle_prepare_queue(mdev) :
		m9x_triangle_prepare_user(mdev);
	if (ret)
		goto out_retired;
	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT |
		      DRM_EXEC_IGNORE_DUPLICATES, count);
	drm_exec_until_all_locked(&exec) {
		u32 i;

		for (i = 0; i < count; i++) {
			ret = drm_exec_prepare_obj(&exec, (*bos)[i], 1);
			drm_exec_retry_on_contention(&exec);
			if (ret)
				goto out_exec;
		}
	}
	ret = m9_wait_foreign_fences(mdev, *bos, refs, count);
	if (ret)
		goto out_exec;
	mutex_lock(&mdev->transport.lock);
	if (mdev->transport.async_stop) {
		ret = -ENODEV;
		goto out_unlock;
	}
	ret = queued ? m9x_transport_queue_room_locked(mdev, &secondary_slot) :
		m9x_transport_drain_locked(mdev);
	if (ret)
		goto out_unlock;
	ret = m9x_ordinary_ready_locked(mdev);
	if (ret)
		goto out_unlock;
	target = mdev->transport.next_fence;
	if (job_count) {
		ret = m9_complete_frame(jobs, job_count, target,
			lower_32_bits(mdev->transport.fence_bus), scratch->framed,
			2 * dwords + 44, range_ends, dwords / 2 + 5,
			&framed_dwords, &ranges, &final);
		if (ret) goto out_unlock;
		/* The existing posting helper adds exactly this last footer. */
		framed_dwords -= M9_COMPLETE_FOOTER_WORDS;
		ranges--;
		framed = scratch->framed;
	} else {
		framed = m9_frame_records(image, expected, dwords, target,
				  &framed_dwords, range_ends, &ranges,
				  &max_token, scratch->framed);
		/* The completion target follows every userspace dependency token. */
		final = target + max_token + 0x10;
	}
	issued = ((u64)(mdev->transport.fence_epoch +
			 (final < mdev->transport.last_fence)) << 32) | final;
	fence = m9x_fence_create_target_locked(mdev, final);
	if (!fence) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	/* Every reservation is held and has a reserved slot before publication;
	 * consumers can never see the new framebuffer without its dependency. */
	for (u32 i = 0; i < count; i++)
		dma_resv_add_fence((*bos)[i]->resv, &fence->base,
				  refs[i].flags & PARHELIA_M9_BO_WRITE ?
				  DMA_RESV_USAGE_WRITE : DMA_RESV_USAGE_READ);
	ret = m9x_submit_user_stream_locked(mdev, framed, framed_dwords,
					    range_ends, ranges,
					    final, point, async, &posted, secondary_slot);
	/* Synchronous failure after the real doorbell can also leave DMA
	 * active. A failure before posting owns no GPU readers; a retired
	 * job whose guard check failed can likewise release its objects. */
	if (posted && queued) {
		struct m9_queued_job job = {
			.point = issued, .target = final,
			.primary_slot = mdev->transport.command_slot,
			.secondary_slot = secondary_slot,
			.bos = *bos, .pinned = *pinned, .count = count,
			.fence = fence, .retire = retired,
		};

		/* Publication and owner transfer share the posting lock. Even a
		 * post-doorbell error must retain all GPU readers until DMA stops. */
		if (ret && !mdev->transport.wedged) {
			mdev->transport.wedged = true;
			mdev->transport.generation++;
		}
		m9x_transport_queue_commit_locked(mdev, &job);
		fence = NULL;
		retired = NULL;
		*bos = NULL;
		*pinned = NULL;
	} else if (posted && mdev->transport.completed_point < issued &&
	    !mdev->transport.async_bos) {
		mdev->transport.async_pending = true;
		mdev->transport.async_target = final;
		mdev->transport.async_point = issued;
		mdev->transport.submitted_point = issued;
		mdev->transport.async_bos = *bos;
		mdev->transport.async_pinned = *pinned;
		mdev->transport.async_count = count;
		mdev->transport.async_fence = fence;
		mdev->transport.async_retire = retired;
		fence = NULL;
		retired = NULL;
		*bos = NULL;
		*pinned = NULL;
		if (ret && !mdev->transport.wedged) {
			mdev->transport.wedged = true;
			mdev->transport.generation++;
		}
		if (!mdev->transport.wedged)
			schedule_work(&mdev->transport.async_complete);
	}
out_unlock:
	if (fence) {
		if (ret)
			dma_fence_set_error(&fence->base, ret);
		dma_fence_signal(&fence->base);
		dma_fence_put(&fence->base);
	}
	mutex_unlock(&mdev->transport.lock);
out_exec:
	drm_exec_fini(&exec);
out_retired:
	kfree(retired);
	return ret;
}

static int m9_submit_validate(struct m9x_device *mdev,
			      struct drm_file *file,
			      struct drm_parhelia_m9_submit *args, bool execute)
{
	struct m9_submit_scratch scratch = {};
	struct m9_complete_job jobs[M9_COMPLETE_MAX_JOBS] = {};
	u32 job_count = 0;
	struct drm_parhelia_m9_reloc *relocs = NULL;
	struct drm_parhelia_m9_bo_ref *refs = NULL;
	struct drm_gem_object **bos = NULL;
	bool *vram_pinned = NULL;
	struct m9x_bo *command_bo;
	u32 *expected = NULL;
	u32 *image = NULL;
	u64 command_end;
	u32 dwords, i;
	int ret = 0;

	if (!args->command_handle || !args->command_length ||
	    args->command_length > PARHELIA_M9_COMMAND_BYTES ||
	    args->command_length & 3 ||
	    args->relocation_count > PARHELIA_M9_MAX_RELOCS ||
	    args->relocation_count > args->command_length / sizeof(u32) ||
	    args->bo_count > PARHELIA_M9_MAX_BOS ||
	    (!!args->bo_count != !!args->bos_ptr) ||
	    (!!args->relocation_count != !!args->relocations_ptr)) {
		if (m9_submit_trace)
			pr_warn_ratelimited("m9x: submit args len %u relocs %u bos %u ptrs %llu/%llu\n",
				args->command_length, args->relocation_count,
				args->bo_count, args->bos_ptr,
				args->relocations_ptr);
		return m9_refuse(-EINVAL);
	}
	command_bo = m9x_bo_lookup(mdev, file, args->command_handle);
	if (IS_ERR(command_bo))
		return PTR_ERR(command_bo);
	if (!(command_bo->flags & PARHELIA_M9_BO_COMMAND) ||
	    check_add_overflow(args->command_offset, (u64)args->command_length,
			       &command_end) || command_end > command_bo->dma.base.size) {
		ret = m9_refuse(-EINVAL);
		goto out_command;
	}
	dwords = args->command_length / sizeof(u32);
	ret = m9_submit_scratch_init(&scratch, dwords, args->bo_count,
				     args->relocation_count);
	if (ret)
		goto out;
	image = scratch.image;
	expected = scratch.expected;
	refs = scratch.refs;
	relocs = scratch.relocs;
	memcpy(image, (u8 *)command_bo->dma.vaddr + args->command_offset,
	       args->command_length);
	if ((args->bo_count &&
	     copy_from_user(refs, u64_to_user_ptr(args->bos_ptr),
			    array_size(args->bo_count, sizeof(*refs)))) ||
	    (args->relocation_count &&
	     copy_from_user(relocs, u64_to_user_ptr(args->relocations_ptr),
			    array_size(args->relocation_count, sizeof(*relocs))))) {
		ret = -EFAULT;
		goto out;
	}
	if (execute)
		for (i = 0; i < args->relocation_count; i++)
			if (!m9_reloc_executable(relocs[i].kind) ||
			    ((args->flags & PARHELIA_M9_SUBMIT_AGGREGATE) &&
			     (relocs[i].kind == PARHELIA_M9_RELOC_QUERY ||
			      relocs[i].kind == PARHELIA_M9_RELOC_FRAGMENT ||
			      relocs[i].kind == PARHELIA_M9_RELOC_FAA_POOL))) {
				ret = m9_refuse(-EOPNOTSUPP);
				goto out;
			}
	if (args->bo_count) {
		bos = kcalloc(args->bo_count, sizeof(*bos), GFP_KERNEL);
		vram_pinned = kcalloc(args->bo_count, sizeof(*vram_pinned),
				      GFP_KERNEL);
		if (!bos || !vram_pinned) {
			ret = -ENOMEM;
			goto out;
		}
	}
	for (i = 0; i < args->bo_count; i++) {
		u32 j;

		if (!refs[i].handle || !refs[i].flags ||
		    refs[i].flags & ~(PARHELIA_M9_BO_READ | PARHELIA_M9_BO_WRITE)) {
			ret = m9_refuse(-EINVAL);
			goto out;
		}
		for (j = 0; j < i; j++)
			if (refs[j].handle == refs[i].handle) {
				ret = m9_refuse(-EINVAL);
				goto out;
			}
		bos[i] = drm_gem_object_lookup(file, refs[i].handle);
		if (!bos[i]) {
			ret = -ENOENT;
			goto out;
		}
		if (m9x_gem_is_dma(bos[i])) {
			struct m9x_bo *bo;

			/* Keep the private per-file DMA allocation registry check. */
			drm_gem_object_put(bos[i]);
			bo = m9x_bo_lookup(mdev, file, refs[i].handle);
			if (IS_ERR(bo)) {
				ret = PTR_ERR(bo);
				bos[i] = NULL;
				goto out;
			}
			bos[i] = &bo->dma.base;
			m9x_bo_sync_for_device(bo);
		} else if (m9x_gem_in_vram(bos[i])) {
			ret = m9x_gem_vram_pin(bos[i]);
			if (ret)
				goto out;
			vram_pinned[i] = true;
		} else {
			ret = m9_refuse(-EINVAL);
			goto out;
		}
	}
	if (args->program_type == PARHELIA_M9_PROGRAM_FIXED_P00_S28_INLINE &&
	    (dwords < M9_RESOURCE69_COUNT_FREE_DWORDS ||
	     memcmp(image, m9_resource69_inline_count_free,
		    sizeof(m9_resource69_inline_count_free)))) {
		ret = m9_refuse(-EINVAL);
		goto out;
	}
	if (args->flags & PARHELIA_M9_SUBMIT_AGGREGATE)
		ret = m9_parse_aggregate(image, dwords, expected, jobs, &job_count);
	else
		ret = m9_parse_commands(image, dwords, expected, args->program_type);
	if (!ret)
		ret = m9_validate_relocs(image, dwords, expected, relocs,
					 args->relocation_count, bos, mdev, refs,
					 args->bo_count, &scratch);
	/*
	 * A synchronous job holds its objects for the call.  An asynchronous
	 * one hands them to the transport, which releases them when the job
	 * retires: the point of posting without waiting is that the next job
	 * is built while this one still reads them.
	 */
	if (!ret && execute)
		ret = m9_submit_execute(mdev, image, expected, dwords,
					&args->out_point,
					m9_async_submit &&
					!mdev->transport.wedged,
					args->flags & PARHELIA_M9_SUBMIT_QUEUED,
					&bos, &vram_pinned, refs, args->bo_count,
					&scratch, jobs, job_count);
out:
	if (vram_pinned)
		for (i = 0; i < args->bo_count; i++)
			if (vram_pinned[i])
				m9x_gem_vram_unpin(bos[i]);
	kfree(vram_pinned);
	m9_put_bos(bos, args->bo_count);
	kfree(bos);
	kvfree(scratch.storage);
out_command:
	drm_gem_object_put(&command_bo->dma.base);
	return ret;
}

static int m9_submit_ioctl(struct drm_device *drm, void *data,
			   struct drm_file *file)
{
	struct drm_parhelia_m9_submit *args = data;
	struct m9x_device *mdev = to_m9x(drm);
	u32 flags = args->flags;

	if (!m9x_enabled())
		return -ENODEV;
	if (args->context_id || args->out_syncobj ||
	    args->program_type > PARHELIA_M9_PROGRAM_FIXED_P00_S28_INLINE ||
	    args->out_point ||
	    flags & ~(PARHELIA_M9_SUBMIT_VALIDATE_ONLY |
		      PARHELIA_M9_SUBMIT_NOOP_FENCE |
		      PARHELIA_M9_SUBMIT_FIRST_TRIANGLE |
		      PARHELIA_M9_SUBMIT_QUEUED |
		      PARHELIA_M9_SUBMIT_FAA_UNLOCK |
		      PARHELIA_M9_SUBMIT_FAA_RELOCK |
		      PARHELIA_M9_SUBMIT_AGGREGATE))
		return m9_refuse(-EINVAL);
	if (flags & PARHELIA_M9_SUBMIT_AGGREGATE) {
		u32 allowed = PARHELIA_M9_SUBMIT_AGGREGATE |
			PARHELIA_M9_SUBMIT_QUEUED | PARHELIA_M9_SUBMIT_VALIDATE_ONLY;
		if (!m9_aggregate_submit || flags & ~allowed ||
		    args->program_type != PARHELIA_M9_PROGRAM_NONE ||
		    m9x_faa_diag_enabled())
			return -EOPNOTSUPP;
		if (flags == (PARHELIA_M9_SUBMIT_VALIDATE_ONLY |
			      PARHELIA_M9_SUBMIT_AGGREGATE) &&
		    !args->command_handle && !args->command_offset &&
		    !args->command_length && !args->relocation_count &&
		    !args->relocations_ptr && !args->bo_count && !args->bos_ptr)
			return 0;
		if (!(flags & PARHELIA_M9_SUBMIT_VALIDATE_ONLY) && !m9_user_submit)
			return -EOPNOTSUPP;
		if ((flags & PARHELIA_M9_SUBMIT_QUEUED) && !m9_async_submit)
			return -EOPNOTSUPP;
		return m9_submit_validate(mdev, file, args,
			!(flags & PARHELIA_M9_SUBMIT_VALIDATE_ONLY));
	}
	if (flags == PARHELIA_M9_SUBMIT_NOOP_FENCE) {
		if (args->command_handle || args->command_offset ||
		    args->command_length || args->relocation_count ||
		    args->relocations_ptr || args->bo_count || args->bos_ptr ||
		    args->program_type)
			return m9_refuse(-EINVAL);
		return m9x_submit_noop(mdev, &args->out_point);
	}
	if (flags == PARHELIA_M9_SUBMIT_FIRST_TRIANGLE) {
		return m9_submit_first_triangle(mdev, file, args);
	}
	if (flags == PARHELIA_M9_SUBMIT_FAA_UNLOCK)
		return m9_submit_faa_continuation(mdev, file, args, false);
	if (flags == PARHELIA_M9_SUBMIT_FAA_RELOCK)
		return m9_submit_faa_continuation(mdev, file, args, true);
	if (!flags || flags == PARHELIA_M9_SUBMIT_QUEUED) {
		/* The private capture intentionally owns a complete FAA context.
		 * It is a one-shot diagnostic, not a context-switch implementation. */
		if (m9x_faa_diag_enabled())
			return -EACCES;
		if (!m9_user_submit)
			return -EOPNOTSUPP;
		if (flags && !m9_async_submit)
			return -EOPNOTSUPP;
		return m9_submit_validate(mdev, file, args, true);
	}
	if (flags != PARHELIA_M9_SUBMIT_VALIDATE_ONLY &&
	    flags != (PARHELIA_M9_SUBMIT_VALIDATE_ONLY | PARHELIA_M9_SUBMIT_QUEUED))
		return -EOPNOTSUPP;
	if ((flags & PARHELIA_M9_SUBMIT_QUEUED) && !m9_async_submit)
		return -EOPNOTSUPP;
	return m9_submit_validate(mdev, file, args, false);
}

int m9x_submit_ioctl(struct drm_device *drm, void *data,
		     struct drm_file *file)
{
	int idx, ret;

	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;
	ret = m9_submit_ioctl(drm, data, file);
	drm_dev_exit(idx);
	return ret;
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
	u32 dependency[] = { 0x4545e3e6, 3, 0x20, PARHELIA_M9_RANGE_BREAK, 0,
			     0x45454543, 0x20 };
	u32 bad_dependency[] = { 0x454545e3, 0x21 };
	u32 untextured_combiner[] = { 0x45454588, 0x0110dfe3 };
	u32 query[] = { 0xe6e6e61e, 0, 0x64000004, 0x64000101, 0x40008000 };
	u32 bad_query[] = { 0xe6e6e61e, 0, 0x64000004, 0x64000101, 0x40008001 };
	u32 bare_query[] = { 0x4545451e, 0 };
	u32 depth_bias[] = { 0x4545d1d0, 0x33800001, 0xc0000000 };
	u32 bad_bias[][4] = {
		{ 0x454545d0, 0x34000001 }, /* unpaired units */
		{ 0x4545d0d1, 0, 0 },     /* wrong order */
		{ 0x45d1d022, 0, 0, 0 },  /* mixed state packet */
		{ 0x4545d1d0, 0x7f800000, 0 }, /* infinite units */
		{ 0x4545d1d0, 0, 0xffc00000 }, /* NaN factor */
	};
	u32 expected[ARRAY_SIZE(dependency)] = {};
	u32 *inline_expected;
	int ret;
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
	u64 address;

	if (m9_vram_gpu_address(SZ_256M, 0, 0, &address) ||
	    address != M9_VRAM_GPU_BASE ||
	    m9_vram_gpu_address(SZ_256M, 0x01ee9000, 0, &address) ||
	    address != M9_VRAM_GPU_BASE + 0x01ee9000 ||
	    m9_vram_gpu_address(SZ_256M, SZ_256M - 0x100, 0x100,
				&address) != -EOVERFLOW)
		return m9_refuse(-EINVAL);

	if (m9_reloc_access(PARHELIA_M9_RELOC_COLOR) !=
	    (PARHELIA_M9_BO_READ | PARHELIA_M9_BO_WRITE) ||
	    m9_reloc_access(PARHELIA_M9_RELOC_DEPTH) !=
	    (PARHELIA_M9_BO_READ | PARHELIA_M9_BO_WRITE) ||
	    m9_reloc_access(PARHELIA_M9_RELOC_TEXTURE) !=
	    PARHELIA_M9_BO_READ ||
	    m9_reloc_access(PARHELIA_M9_RELOC_RESOURCE_69) !=
	    PARHELIA_M9_BO_READ ||
	    m9_reloc_access(PARHELIA_M9_RELOC_FENCE) ||
	    m9_reloc_access(PARHELIA_M9_RELOC_QUERY) != PARHELIA_M9_BO_WRITE ||
	    m9_reloc_alignment(PARHELIA_M9_RELOC_QUERY) != M9_QUERY_BYTES ||
	    !m9_reloc_uses_vram(PARHELIA_M9_RELOC_QUERY) ||
	    !m9_reloc_uses_vram(PARHELIA_M9_RELOC_COLOR) ||
	    !m9_reloc_uses_vram(PARHELIA_M9_RELOC_DEPTH) ||
	    !m9_reloc_uses_vram(PARHELIA_M9_RELOC_TEXTURE) ||
	    !m9_reloc_uses_vram(PARHELIA_M9_RELOC_RESOURCE_69) ||
	    m9_reloc_uses_vram(PARHELIA_M9_RELOC_DRAW32_START))
		return m9_refuse(-EINVAL);
	if (m9_parse_commands(depth_bias, ARRAY_SIZE(depth_bias), expected,
			      PARHELIA_M9_PROGRAM_NONE) ||
	    expected[1] || expected[2])
		return m9_refuse(-EINVAL);
	for (unsigned int i = 0; i < ARRAY_SIZE(bad_bias); i++) {
		unsigned int n = i == 0 ? 2 : i == 2 ? 4 : 3;

		memset(expected, 0, sizeof(expected));
		if (m9_parse_commands(bad_bias[i], n, expected,
				      PARHELIA_M9_PROGRAM_NONE) != -EPERM)
			return m9_refuse(-EINVAL);
	}
	memset(expected, 0, sizeof(expected));
	if (m9_parse_commands(good, ARRAY_SIZE(good), expected,
			      PARHELIA_M9_PROGRAM_NONE))
		return m9_refuse(-EINVAL);
	memset(expected, 0, sizeof(expected));
	if (m9_parse_commands(dependency, ARRAY_SIZE(dependency), expected,
			      PARHELIA_M9_PROGRAM_NONE) ||
	    expected[2] != M9_EXPECT_DEPENDENCY ||
	    expected[6] != M9_EXPECT_DEPENDENCY || expected[4])
		return m9_refuse(-EINVAL);
	memset(expected, 0, sizeof(expected));
	if (m9_parse_commands(bad_dependency, ARRAY_SIZE(bad_dependency),
			      expected, PARHELIA_M9_PROGRAM_NONE) != -EPERM)
		return m9_refuse(-EINVAL);
	memset(expected, 0, sizeof(expected));
	if (m9_parse_commands(query, ARRAY_SIZE(query), expected,
			      PARHELIA_M9_PROGRAM_NONE) ||
	    expected[1] != M9_RELOC_BIT(QUERY) || expected[2] || expected[3] ||
	    expected[4])
		return m9_refuse(-EINVAL);
	memset(expected, 0, sizeof(expected));
	if (m9_parse_commands(bad_query, ARRAY_SIZE(bad_query), expected,
			      PARHELIA_M9_PROGRAM_NONE) != -EPERM)
		return m9_refuse(-EINVAL);
	memset(expected, 0, sizeof(expected));
	if (m9_parse_commands(bare_query, ARRAY_SIZE(bare_query), expected,
			      PARHELIA_M9_PROGRAM_NONE) != -EPERM)
		return m9_refuse(-EINVAL);
	memset(expected, 0, sizeof(expected));
	if (m9_parse_commands(untextured_combiner, ARRAY_SIZE(untextured_combiner),
			      expected, PARHELIA_M9_PROGRAM_NONE) != -EINVAL)
		return m9_refuse(-EINVAL);
	memset(expected, 0, sizeof(expected));
	if (m9_parse_commands(privileged, ARRAY_SIZE(privileged), expected,
			      PARHELIA_M9_PROGRAM_NONE) != -EPERM)
		return m9_refuse(-EINVAL);
	memset(expected, 0, sizeof(expected));
	if (m9_parse_commands(private_program, ARRAY_SIZE(private_program),
			      expected, PARHELIA_M9_PROGRAM_NONE) != -EPERM)
		return m9_refuse(-EINVAL);
	memset(expected, 0, sizeof(expected));
	if (m9_parse_commands(state_af, ARRAY_SIZE(state_af), expected,
			      PARHELIA_M9_PROGRAM_NONE))
		return m9_refuse(-EINVAL);
	memset(expected, 0, sizeof(expected));
	if (m9_parse_commands(truncated, ARRAY_SIZE(truncated), expected,
			      PARHELIA_M9_PROGRAM_NONE) != -EINVAL)
		return m9_refuse(-EINVAL);
	memset(expected, 0, sizeof(expected));
	if (m9_parse_commands(draw32, ARRAY_SIZE(draw32), expected,
			      PARHELIA_M9_PROGRAM_NONE) ||
	    m9_validate_draw_reloc_pairs(draw32, ARRAY_SIZE(draw32), by_dword))
		return m9_refuse(-EINVAL);
	inline_expected = kcalloc(M9_RESOURCE69_COUNT_FREE_DWORDS,
				  sizeof(*inline_expected), GFP_KERNEL);
	if (!inline_expected)
		return -ENOMEM;
	ret = m9_parse_commands(m9_resource69_inline_count_free,
				M9_RESOURCE69_COUNT_FREE_DWORDS, inline_expected,
				PARHELIA_M9_PROGRAM_FIXED_P00_S28_INLINE) ||
	      memchr_inv(inline_expected, 0,
			 M9_RESOURCE69_COUNT_FREE_DWORDS *
			 sizeof(*inline_expected)) ? -EINVAL : 0;
	kfree(inline_expected);
	if (ret)
		return ret;
	draw_relocs[1].target_offset = 0;
	draw_relocs[0].target_offset = 16;
	if (m9_validate_draw_reloc_pairs(draw32, ARRAY_SIZE(draw32), by_dword) !=
	    -EINVAL)
		return m9_refuse(-EINVAL);
	draw_relocs[0].target_offset = 0;
	draw_relocs[1].target_offset = 4;
	draw_relocs[0].kind = PARHELIA_M9_RELOC_DRAW16_START;
	draw_relocs[1].kind = PARHELIA_M9_RELOC_DRAW16_END;
	draw_relocs[1].flags |= PARHELIA_M9_RELOC_PACKED16_ODD;
	if (m9_validate_draw_reloc_pairs(draw16, ARRAY_SIZE(draw16), by_dword))
		return m9_refuse(-EINVAL);
	draw_relocs[1].target_offset = 0;
	if (m9_validate_draw_reloc_pairs(draw16, ARRAY_SIZE(draw16), by_dword) !=
	    -EINVAL)
		return m9_refuse(-EINVAL);
	return 0;
}
