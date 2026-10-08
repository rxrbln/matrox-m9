// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* Original M9128 later DRAW stages: plain, cached and built programs. */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include "m9_cmd.h"
#include "m9_faa_graph_later_stage.h"
#include "m9_faa_graph_single_first_encode.h"

const u32 m9x_faa_null_target[24] = {
	0x00040118, 3,		0x457a7978, 0,		0xcc010001, 0x00010001,
	3,	    0x45877d7c, 0x200,	    0x0ff80000, 0,	    4,
	0x85848382, 0,		0,	    0,		0,	    4,
	0x7f7e8180, 0xffffffff, 0,	    0,		0,	    1,
};

/* A FIRST-only packet group: offset, length, its count/header words and
 * whether it is a 6A cull group.
 */
struct group {
	u16 at, bytes;
	u32 count, header;
	bool cull;
};

struct epoch {
	u16 at, delta;
	u8 tag;
};

/* Per program FIRST stage: idle stage-0 0x70 value, input mask, second
 * primary range, block length headers, sampler sites, raster cull word,
 * the 19/18 group, the groups later stages drop, and the epochs.
 */
static const struct profile {
	u16 bytes, reload, mask, split, cull;
	u16 head[3];
	u16 site[2];
	u8 units;
	struct group g1918;
	struct group drop[7];
	struct epoch epoch[13];
} profiles[M9_FAA_PROGRAM_KINDS] = {
	[M9_FAA_PROGRAM_DUAL] = {
		2880, 456, 320, 1912, 1992, { 28, 744, 1388 },
		{ 2520, 2620 }, 2,
		{ 660, 16, 2, 0x45451819 },
		{ { 900, 100, 2, 0x4545bbba }, { 1284, 12, 1, 0x45454569 },
		  { 1296, 12, 1, 0x4545456a, true },
		  { 1512, 76, 2, 0x4545b9b8 }, { 1608, 20, 3, 0x45aeacab },
		  { 1844, 12, 1, 0x45454569 },
		  { 1856, 12, 1, 0x4545456a, true } },
		{ { 56, 0x90, 1 },    { 104, 0xa0, 1 },	  { 772, 0xb0, 1 },
		  { 784, 0xb0, 0 },   { 832, 0xc0, 1 },	  { 1416, 0xd0, 1 },
		  { 1428, 0xd0, 0 },  { 1476, 0xe0, 1 },  { 1924, 0xf0, 1 },
		  { 1952, 0x100, 1 }, { 1964, 0x100, 0 }, { 2024, 0x110, 1 },
		  { 2120, 0x120, 1 } },
	},
	[M9_FAA_PROGRAM_SINGLE] = {
		2720, 440, 304, 1840, 1920, { 28, 704, 1316 },
		{ 2448, 0 }, 1,
		{ 620, 16, 2, 0x45451819 },
		{ { 844, 100, 2, 0x4545bbba }, { 1212, 12, 1, 0x45454569 },
		  { 1224, 12, 1, 0x4545456a, true },
		  { 1440, 76, 2, 0x4545b9b8 }, { 1536, 20, 3, 0x45aeacab },
		  { 1772, 12, 1, 0x45454569 },
		  { 1784, 12, 1, 0x4545456a, true } },
		{ { 56, 0x90, 1 },    { 104, 0xa0, 1 },	  { 732, 0xb0, 1 },
		  { 744, 0xb0, 0 },   { 792, 0xc0, 1 },	  { 1344, 0xd0, 1 },
		  { 1356, 0xd0, 0 },  { 1404, 0xe0, 1 },  { 1852, 0xf0, 1 },
		  { 1880, 0x100, 1 }, { 1892, 0x100, 0 }, { 1952, 0x110, 1 },
		  { 2048, 0x120, 1 } },
	},
};

static bool dropped(const struct group *drop, u32 n, u32 off)
{
	u32 i;

	for (i = 0; i < n; i++)
		if (off >= drop[i].at && off < drop[i].at + drop[i].bytes)
			return true;
	return false;
}

/* Output offset of the kept FIRST word at off. */
static u32 moved(const struct profile *p, const struct group *drop, u32 n,
		 bool e3, u32 off)
{
	u32 i, to = off + (e3 && off >= 56 ? M9_FAA_SWITCH_E3_BYTES : 0) +
		    (off > p->reload ? M9_FAA_SWITCH_RELOAD_BYTES : 0);

	for (i = 0; i < n; i++)
		if (drop[i].at + drop[i].bytes <= off)
			to -= drop[i].bytes;
	return to;
}

int m9x_faa_later_stage_encode(const u8 *first, u32 first_bytes, u32 epoch,
			       enum m9x_faa_later_form form, bool append,
			       bool e3, bool cull_state, u8 *out, u32 capacity,
			       struct m9x_faa_later_stage *stage)
{
	struct group drop[8];
	const struct profile *p = NULL;
	u32 i, n = 0, bytes, base, end, mask, head, at;
	u8 *d;

	if (!first || !stage || (epoch & 15) ||
	    epoch > 0xfffffeafu || form > M9_FAA_LATER_BUILD)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(profiles); i++)
		if (profiles[i].bytes == first_bytes)
			p = &profiles[i];
	if (!p)
		return -EINVAL;
	/* An appended plain DRAW drops the 19/18 group; a cached one and the
	 * first DRAW of a frame keep it. BUILD keeps the whole FIRST stage.
	 */
	if (form != M9_FAA_LATER_BUILD) {
		if (append && form == M9_FAA_LATER_PLAIN)
			drop[n++] = p->g1918;
		for (i = 0; i < ARRAY_SIZE(p->drop); i++)
			if (!cull_state || !p->drop[i].cull)
				drop[n++] = p->drop[i];
	}
	bytes = first_bytes + M9_FAA_SWITCH_RELOAD_BYTES +
		(e3 ? M9_FAA_SWITCH_E3_BYTES : 0);
	for (i = 0; i < n; i++)
		bytes -= drop[i].bytes;
	if (out && (capacity < bytes ||
		    m9_cmd_overlap(out, bytes, first, first_bytes)))
		return -EINVAL;
	/* A genuine live FIRST image of one epoch, with its idle stage-0
	 * unit before stage 1 and every dropped group where it belongs.
	 */
	base = (m9_cmd_get(first + 56) & ~15u) - 0x90;
	if (m9_cmd_get(first + 4) != 0x454545c7 ||
	    m9_cmd_get(first + 44) != 2 ||
	    m9_cmd_get(first + 48) != 0x4545e3e6 ||
	    m9_cmd_get(first + 52) != 1 ||
	    m9_cmd_get(first + p->reload - 8) != 1 ||
	    m9_cmd_get(first + p->reload - 4) != 0x45454570 ||
	    m9_cmd_get(first + p->reload) ||
	    m9_cmd_get(first + p->reload + 4) != 1 ||
	    m9_cmd_get(first + p->reload + 8) != 0x45454571 ||
	    !(m9_cmd_get(first + p->mask) & 0xff000000))
		return -ESTALE;
	for (i = 0; i < ARRAY_SIZE(p->epoch); i++)
		if (m9_cmd_get(first + p->epoch[i].at) !=
		    ((base + p->epoch[i].delta) | p->epoch[i].tag))
			return -ESTALE;
	for (i = 0; i < n; i++)
		if (m9_cmd_get(first + drop[i].at) != drop[i].count ||
		    m9_cmd_get(first + drop[i].at + 4) != drop[i].header)
			return -ESTALE;
	for (i = 0; i < ARRAY_SIZE(p->head); i++) {
		head = m9_cmd_get(first + p->head[i]);
		end = p->head[i] + 4 * (head & 0xffff);
		if ((head >> 16) != 0x49d0 || end >= first_bytes ||
		    dropped(drop, n, end))
			return -ESTALE;
	}
	stage->bytes = bytes;
	stage->split = moved(p, drop, n, e3, p->split);
	stage->units = p->units;
	for (i = 0; i < 2; i++)
		stage->site[i] = i < p->units ?
			moved(p, drop, n, e3, p->site[i]) : 0;
	/* Without out, a validated dry run that only sizes the stage. */
	if (!out)
		return 0;
	d = out;
	memcpy(d, first, 44);
	d += 44;
	if (e3) {
		/* Source no-clear/append form: E3 carries one more word. */
		m9_cmd_put(d, 3);
		m9_cmd_put(d + 4, 0x45e3e6e6);
		m9_cmd_put(d + 8, 1);
		m9_cmd_put(d + 12, 2);
		d += 16;
	} else {
		memcpy(d, first + 44, 12);
		d += 12;
	}
	for (at = 56; at < first_bytes; at += 4) {
		if (dropped(drop, n, at))
			continue;
		if (at == p->reload) {
			for (i = 0; i < ARRAY_SIZE(m9x_faa_null_target); i++)
				m9_cmd_put(d + 4 * i, m9x_faa_null_target[i]);
			d += sizeof(m9x_faa_null_target);
			m9_cmd_put(d, 0x454545c0);
			m9_cmd_put(d + 4, 4);
			d += 8;
			continue;
		}
		memcpy(d, first + at, 4);
		d += 4;
	}
	for (i = 0; i < ARRAY_SIZE(p->head); i++) {
		head = m9_cmd_get(first + p->head[i]);
		end = moved(p, drop, n, e3, p->head[i] + 4 * (head & 0xffff));
		m9_cmd_put(out + moved(p, drop, n, e3, p->head[i]),
			   0x49d00000 |
			   ((end - moved(p, drop, n, e3, p->head[i])) / 4));
	}
	/* Source f25338 re-entry mask, as on every retained later stage. */
	mask = m9_cmd_get(first + p->mask);
	m9_cmd_put(out + moved(p, drop, n, e3, p->mask),
		   ((mask & 0xff1fffffu) | 0x00100000u) & ~3u);
	for (i = 0; i < ARRAY_SIZE(p->epoch); i++)
		m9_cmd_put(out + moved(p, drop, n, e3, p->epoch[i].at),
			   (epoch + p->epoch[i].delta) | p->epoch[i].tag);
	return 0;
}

int m9x_faa_program_cache_draw(struct m9x_faa_program_cache *cache, u32 kind,
			       const u8 *program, u32 bytes, u32 raster,
			       bool first, enum m9x_faa_later_form *form,
			       bool *cull_state, u32 *offset)
{
	u32 at;

	if (!cache || !program || !form || !cull_state || !offset ||
	    kind >= M9_FAA_PROGRAM_KINDS || !bytes || (bytes & 7) ||
	    bytes > M9_FAA_PROGRAM_PAGE_BYTES || (raster & ~6u) ||
	    (raster & 6) == 6 || first != !cache->built)
		return -EINVAL;
	if (cache->built & (1u << kind)) {
		at = cache->offset[kind];
		if (cache->bytes[kind] != bytes ||
		    memcmp(cache->page + at, program, bytes))
			return -ESTALE;
		*form = cache->active == kind ? M9_FAA_LATER_PLAIN :
						 M9_FAA_LATER_CACHED;
		*cull_state = cache->cull[kind] != raster;
	} else {
		at = (cache->page_bytes + 63) & ~63u;
		if (at > M9_FAA_PROGRAM_PAGE_BYTES - bytes)
			return -ERANGE;
		memcpy(cache->page + at, program, bytes);
		cache->built |= 1u << kind;
		cache->offset[kind] = at;
		cache->bytes[kind] = bytes;
		cache->page_bytes = at + bytes;
		*form = M9_FAA_LATER_BUILD;
		*cull_state = false;
	}
	cache->cull[kind] = raster;
	cache->active = kind;
	*offset = at;
	return 0;
}
