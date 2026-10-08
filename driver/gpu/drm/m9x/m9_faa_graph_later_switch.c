// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* Original M9128 later DRAW that switches fragment program (source 13ce05). */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include "m9_cmd.h"
#include "m9_faa_graph_later_switch.h"
#include "m9_faa_graph_single_first_encode.h"

const u32 m9x_faa_null_target[24] = {
	0x00040118, 3,		0x457a7978, 0,		0xcc010001, 0x00010001,
	3,	    0x45877d7c, 0x200,	    0x0ff80000, 0,	    4,
	0x85848382, 0,		0,	    0,		0,	    4,
	0x7f7e8180, 0xffffffff, 0,	    0,		0,	    1,
};

/* FIRST stage size, idle stage-0 0x70 value word, input mask word and the
 * second primary range of the admitted programs.
 */
static const struct {
	u16 bytes, reload, mask, split;
} profiles[] = {
	{ M9_FAA_SINGLE_FIRST_SECONDARY_BYTES, 440, 304,
	  M9_FAA_SINGLE_FIRST_SPLIT },
	{ M9_FAA_DUAL_FIRST_SECONDARY_BYTES, 456, 320, 1912 },
};

int m9x_faa_switch_stage_encode(const u8 *first, u32 first_bytes, u32 epoch,
				bool e3, u8 *out, u32 capacity, u32 *bytes,
				u32 *split)
{
	u32 grow = M9_FAA_SWITCH_RELOAD_BYTES + (e3 ? M9_FAA_SWITCH_E3_BYTES : 0);
	u32 i, j, r, at, mask, head;
	u8 *d;

	if (!first || !out || !bytes || !split || epoch > 0xfffffeafu ||
	    (epoch & 15))
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(profiles); i++)
		if (profiles[i].bytes == first_bytes)
			break;
	if (i == ARRAY_SIZE(profiles) || capacity < first_bytes + grow ||
	    m9_cmd_overlap(out, first_bytes + grow, first, first_bytes))
		return -EINVAL;
	r = profiles[i].reload;
	head = m9_cmd_get(first + 28);
	mask = m9_cmd_get(first + profiles[i].mask);
	/* Fresh FIRST of this epoch with the idle stage-0 unit before stage 1. */
	if (m9_cmd_get(first + 4) != 0x454545c7 || (head >> 16) != 0x49d0 ||
	    28 + 4 * (head & 0xffff) <= r || m9_cmd_get(first + 44) != 2 ||
	    m9_cmd_get(first + 48) != 0x4545e3e6 ||
	    m9_cmd_get(first + 52) != 1 ||
	    m9_cmd_get(first + 56) != ((epoch + 0x90) | 1) ||
	    m9_cmd_get(first + 104) != ((epoch + 0xa0) | 1) ||
	    m9_cmd_get(first + r - 8) != 1 ||
	    m9_cmd_get(first + r - 4) != 0x45454570 || m9_cmd_get(first + r) ||
	    m9_cmd_get(first + r + 4) != 1 ||
	    m9_cmd_get(first + r + 8) != 0x45454571 ||
	    (mask & 0xff000000) == 0)
		return -ESTALE;
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
	memcpy(d, first + 56, r - 56);
	d += r - 56;
	for (j = 0; j < ARRAY_SIZE(m9x_faa_null_target); j++, d += 4)
		m9_cmd_put(d, m9x_faa_null_target[j]);
	m9_cmd_put(d, 0x454545c0);
	m9_cmd_put(d + 4, 4);
	d += 8;
	memcpy(d, first + r + 4, first_bytes - r - 4);
	m9_cmd_put(out + 28, head + grow / 4);
	at = profiles[i].mask + (e3 ? M9_FAA_SWITCH_E3_BYTES : 0);
	/* Source f25338 re-entry mask, as on every retained later stage. */
	m9_cmd_put(out + at, ((mask & 0xff1fffffu) | 0x00100000u) & ~3u);
	*bytes = first_bytes + grow;
	*split = profiles[i].split + grow;
	return 0;
}
