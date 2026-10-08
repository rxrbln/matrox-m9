// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* Original M9128 dual-texture FIRST and retained later frames: append one
 * DRAW before normal Lock.
 */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include "m9_cmd.h"
#include "m9_faa_graph_dual_first_append.h"
#include "m9_faa_graph_dual_later_encode.h"
#include "m9_faa_graph_later_stage.h"

static const u32 save_prefix[36] = {
	1,	    0x45454524, 0,	    3,		0x453ce6e6, 0x6dc00000,
	0x42000000, 0xa1,	9,	    0x44444444, 0x30081898, 0x30081898,
	0x30081898, 0x30081898, 0x44444444, 0x30081898, 0x30081898, 0x30081898,
	0x30081898, 2,		0x45452f2e, 0,		0,	    2,
	0x4545421a, 0,		0xa1000009, 2,		0x4545421a, 0,
	0xa3000009, 3,		0x45e6421a, 0,		0xa4000009, 0x40008000,
};

static const u16 lock_at[] = { 128, 184, 320, 3084, 3540, 3560 };
static const u16 lock_delta[] = { 0x130, 0x140, 0x140, 0x140, 0x130, 0x150 };
static const u8 lock_tag[] = { 1, 1, 0, 0, 0, 1 };
static const u16 save_at[] = { 8, 84, 88, 100, 116, 132 };
static const u8 save_owner[] = { 22, 22, 22, 8, 8, 8 };
static const u16 save_offset[] = { 0, 640, 704, 8, 12, 16 };
static const u8 save_tag[] = { 1, 5, 5, 1, 1, 1 };

/* Shared append after the last DRAW pair group at split. The old DRAW's
 * secondary is [old_start, secondary_cursor); tail_extra is the CLEAR3
 * later tail's 8-byte C0 group behind the pair and before the Lock words.
 * extra is the new DRAW program's live FIRST stage; first_secondary, when
 * given, is the FIRST of a batch whose program/textures/cull must repeat.
 */
static int append(const struct m9x_faa_graph_draw_owner owner[31],
		  u32 old_epoch, u32 split, u32 tail_extra, u32 old_start,
		  u32 secondary_cursor, const u8 *old_primary,
		  u32 old_primary_bytes, const u8 *first_secondary,
		  const u8 *extra, u32 extra_bytes,
		  enum m9x_faa_later_form form, bool cull_state,
		  struct m9x_faa_dual_append_image *out)
{
	struct m9x_faa_later_stage stage;
	u32 new_bytes, i, at, old_length, old_mid, stage_bytes, sel;
	u8 *dst;
	int ret;

	ret = m9x_faa_later_stage_encode(extra, extra_bytes, old_epoch + 0xa0,
					 form, true, true, cull_state, NULL, 0,
					 &stage);
	if (ret)
		return ret;
	stage_bytes = sizeof(save_prefix) + stage.bytes;
	if (old_primary_bytes > owner[27].bytes ||
	    old_primary_bytes > U32_MAX - 60)
		return -ERANGE;
	new_bytes = old_primary_bytes + 60;
	if (owner[27].bytes - old_primary_bytes < 60 ||
	    secondary_cursor > owner[29].bytes ||
	    owner[29].bytes - secondary_cursor < stage_bytes ||
	    (u64)owner[29].base + secondary_cursor + stage_bytes > U32_MAX ||
	    owner[8].bytes < 20 || owner[22].bytes < 712 ||
	    out->primary_capacity < new_bytes ||
	    out->secondary_capacity < stage_bytes)
		return -ERANGE;
	if (m9_cmd_overlap(out->primary, new_bytes, out->secondary,
			   stage_bytes) ||
	    m9_cmd_overlap(out->primary, new_bytes, old_primary,
			   old_primary_bytes) ||
	    m9_cmd_overlap(out->primary, new_bytes, extra, extra_bytes) ||
	    m9_cmd_overlap(out->secondary, stage_bytes, old_primary,
			   old_primary_bytes) ||
	    m9_cmd_overlap(out->secondary, stage_bytes, extra, extra_bytes) ||
	    (first_secondary &&
	     (m9_cmd_overlap(out->primary, new_bytes, first_secondary, 2880) ||
	      m9_cmd_overlap(out->secondary, stage_bytes, first_secondary,
			     2880))))
		return -EINVAL;
	if (split < 60 || split > old_primary_bytes ||
	    old_primary_bytes - split != 3564 + tail_extra ||
	    old_start > secondary_cursor ||
	    m9_cmd_get(old_primary + split) != 0x45454524 ||
	    m9_cmd_get(old_primary + split - 60) != M9_CMD_LINK ||
	    m9_cmd_get(old_primary + split - 12) != M9_CMD_LINK)
		return -ESTALE;
	/* The previous DRAW's ranges: dual stages end 968 bytes behind their
	 * second range, single ones 880.
	 */
	old_length = secondary_cursor - old_start;
	old_mid = (m9_cmd_get(old_primary + split - 60 + 8) & ~3u) -
		  owner[29].base - old_start;
	if (old_mid >= old_length ||
	    (old_length - old_mid != 968 && old_length - old_mid != 880) ||
	    m9_cmd_get(old_primary + split - 60 + 4) !=
		    ((owner[29].base + old_start) | 1) ||
	    m9_cmd_get(old_primary + split - 60 + 8) !=
		    ((owner[29].base + old_start + old_mid) | 2) ||
	    m9_cmd_get(old_primary + split - 60 + 52) !=
		    ((owner[29].base + old_start + old_mid) | 1) ||
	    m9_cmd_get(old_primary + split - 60 + 56) !=
		    ((owner[29].base + secondary_cursor) | 2))
		return -ESTALE;
	for (i = 0; i < ARRAY_SIZE(lock_at); i++) {
		at = split + lock_at[i] + (lock_at[i] >= 3288 ? tail_extra : 0);
		if (m9_cmd_get(old_primary + at) !=
		    ((old_epoch + lock_delta[i]) | lock_tag[i]))
			return -ESTALE;
	}
	/* A FIRST batch repeats one program, texture pair and cull state. */
	if (first_secondary) {
		if (extra_bytes != M9_FAA_DUAL_FIRST_SECONDARY_BYTES ||
		    form != M9_FAA_LATER_PLAIN || cull_state)
			return -EINVAL;
		for (i = 0; i < 4; i++) {
			static const u16 c2[] = { 364, 1032, 1660, 2432 };

			if (m9_cmd_get(first_secondary + c2[i]) !=
				    m9_cmd_get(extra + c2[i]) ||
			    m9_cmd_get(first_secondary + c2[i] + 4) !=
				    m9_cmd_get(extra + c2[i] + 4))
				return -EINVAL;
		}
		for (i = 0; i < 2; i++) {
			static const u16 tex[] = { 2536, 2636 };

			if (m9_cmd_get(first_secondary + tex[i]) !=
			    m9_cmd_get(extra + tex[i]))
				return -EINVAL;
		}
		sel = (m9_cmd_get(extra + 1992) >> 24) & 3;
		if (sel != ((m9_cmd_get(first_secondary + 1992) >> 24) & 3) ||
		    m9_cmd_get(extra + 636) != (sel ? 0x28ff0000u : 0x29340000u) ||
		    m9_cmd_get(extra + 2844) != m9_cmd_get(extra + 636))
			return -EINVAL;
	}
	for (i = 0; i < ARRAY_SIZE(save_at); i++)
		if ((owner[save_owner[i]].base & 7) ||
		    (u64)owner[save_owner[i]].base + save_offset[i] +
				    save_tag[i] >
			    U32_MAX)
			return -ERANGE;
	/* All refusals precede output writes; the composer was dry-run above. */
	dst = out->secondary;
	ret = m9x_faa_later_stage_encode(extra, extra_bytes, old_epoch + 0xa0,
					 form, true, true, cull_state,
					 dst + sizeof(save_prefix),
					 stage.bytes, &stage);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(save_prefix); i++)
		m9_cmd_put(dst + i * 4, save_prefix[i]);
	for (i = 0; i < ARRAY_SIZE(save_at); i++)
		m9_cmd_put(dst + save_at[i],
			   (owner[save_owner[i]].base + save_offset[i]) |
				   save_tag[i]);
	memcpy(out->primary, old_primary, split);
	memcpy(out->primary + split, old_primary + split - 60, 60);
	m9_cmd_put(out->primary + split + 4,
		   (owner[29].base + secondary_cursor) | 1);
	m9_cmd_put(out->primary + split + 8,
		   (owner[29].base + secondary_cursor + sizeof(save_prefix) +
		    stage.split) | 2);
	m9_cmd_put(out->primary + split + 52,
		   (owner[29].base + secondary_cursor + sizeof(save_prefix) +
		    stage.split) | 1);
	m9_cmd_put(out->primary + split + 56,
		   (owner[29].base + secondary_cursor + stage_bytes) | 2);
	memcpy(out->primary + split + 60, old_primary + split,
	       3564 + tail_extra);
	for (i = 0; i < ARRAY_SIZE(lock_at); i++)
		m9_cmd_put(out->primary + split + 60 + lock_at[i] +
				   (lock_at[i] >= 3288 ? tail_extra : 0),
			   (old_epoch + 0xa0 + lock_delta[i]) | lock_tag[i]);
	out->primary_bytes = new_bytes;
	out->secondary_bytes = stage_bytes;
	out->units = stage.units;
	for (i = 0; i < 2; i++)
		out->site[i] = stage.site[i] ?
			sizeof(save_prefix) + stage.site[i] : 0;
	return 0;
}

/* FIRST batch append after old_draw_count DRAWs; the previous DRAW
 * occupies [old_start, secondary_cursor).
 */
static int first_append(const struct m9x_faa_graph_draw_owner owner[31],
			u32 init_bytes, u64 init_point, u32 old_draw_count,
			u32 primary_cursor, u32 old_start, u32 secondary_cursor,
			const u32 old_end_bytes[M9_FAA_DUAL_FIRST_ENDS],
			u64 old_final_point, const u8 *old_primary,
			u32 old_primary_bytes, const u8 *first_secondary,
			const u8 *extra, u32 extra_bytes,
			enum m9x_faa_later_form form, bool cull_state,
			struct m9x_faa_dual_append_image *out)
{
	u32 low = (u32)init_point, split, prefix, i;
	int ret;

	if (!owner || !old_end_bytes || !old_primary || !extra || !out ||
	    !out->primary || !out->secondary || old_draw_count < 1 ||
	    old_draw_count > 2 || (init_point & 15) || low < 0xb0 ||
	    init_point > ~(u64)0 - 0x150 - 2 * 0xa0 ||
	    low > U32_MAX - 0x150 - 2 * 0xa0 ||
	    /* 320x240 (and its widened INIT) or 1024x768 INIT, or later after
	     * ordinary jobs that followed INIT.
	     */
	    init_bytes < 4880 || (init_bytes & 3) ||
	    (u64)primary_cursor != (u64)init_bytes + old_primary_bytes ||
	    old_primary_bytes < 3624 + (old_draw_count - 1) * 60 ||
	    old_final_point != init_point + 0x150 + (old_draw_count - 1) * 0xa0)
		return -EINVAL;
	if (m9x_faa_graph_draw_owners_validate(owner))
		return -EINVAL;
	if (primary_cursor > owner[27].bytes ||
	    owner[27].bytes - primary_cursor < 60 ||
	    (u64)owner[27].base + primary_cursor + 60 > U32_MAX)
		return -ERANGE;
	prefix = old_primary_bytes - 3624 - (old_draw_count - 1) * 60;
	/* The source prefix is 4652 bytes; regenerated sampled page runs may
	 * widen it. The authenticated END[9] pins the actual live prefix.
	 */
	if (prefix < 4652 || prefix > M9_FAA_FIRST_DEPTH_PREFIX_CAPACITY ||
	    (prefix & 3) || old_end_bytes[9] != init_bytes + prefix)
		return -ESTALE;
	split = old_primary_bytes - 3564;
	if (old_end_bytes[10] != init_bytes + split ||
	    old_end_bytes[13] != primary_cursor)
		return -ESTALE;
	for (i = 0; i < M9_FAA_DUAL_FIRST_ENDS; i++)
		if (old_end_bytes[i] < init_bytes ||
		    old_end_bytes[i] > primary_cursor ||
		    (i && old_end_bytes[i] < old_end_bytes[i - 1]))
			return -ESTALE;
	ret = append(owner, low + (old_draw_count - 1) * 0xa0, split, 0,
		     old_start, secondary_cursor, old_primary,
		     old_primary_bytes, first_secondary, extra, extra_bytes,
		     form, cull_state, out);
	if (ret)
		return ret;
	for (i = 0; i < M9_FAA_DUAL_FIRST_ENDS; i++)
		out->end_bytes[i] = old_end_bytes[i] + (i < 10 ? 0 : 60);
	out->final_point = old_final_point + 0xa0;
	return 0;
}

int m9x_faa_dual_first_append_encode(const struct m9x_faa_graph_draw_owner
					     owner[31],
				     u32 init_bytes, u64 init_point,
				     u32 old_draw_count, u32 primary_cursor,
				     u32 secondary_cursor,
				     const u32 old_end_bytes
					     [M9_FAA_DUAL_FIRST_ENDS],
				     u64 old_final_point, const u8 *old_primary,
				     u32 old_primary_bytes,
				     const u8 *first_secondary,
				     const u8 *extra_first_secondary,
				     bool programs_and_textures_retained,
				     struct m9x_faa_dual_append_image *out)
{
	if (!first_secondary || !programs_and_textures_retained ||
	    old_draw_count < 1 ||
	    secondary_cursor != 2880 + (old_draw_count - 1) * 2868)
		return -EINVAL;
	return first_append(owner, init_bytes, init_point, old_draw_count,
			    primary_cursor,
			    old_draw_count == 1 ?
				    0 : 2880 + (old_draw_count - 2) * 2868,
			    secondary_cursor, old_end_bytes, old_final_point,
			    old_primary, old_primary_bytes, first_secondary,
			    extra_first_secondary,
			    M9_FAA_DUAL_FIRST_SECONDARY_BYTES,
			    M9_FAA_LATER_PLAIN, false, out);
}

int m9x_faa_first_append_encode(const struct m9x_faa_graph_draw_owner owner[31],
				u32 init_bytes, u64 init_point,
				u32 old_draw_count, u32 primary_cursor,
				u32 old_start, u32 secondary_cursor,
				const u32 old_end_bytes[M9_FAA_DUAL_FIRST_ENDS],
				u64 old_final_point, const u8 *old_primary,
				u32 old_primary_bytes, const u8 *extra_first,
				u32 extra_first_bytes,
				enum m9x_faa_later_form form, bool cull_state,
				struct m9x_faa_dual_append_image *out)
{
	return first_append(owner, init_bytes, init_point, old_draw_count,
			    primary_cursor, old_start, secondary_cursor,
			    old_end_bytes, old_final_point, old_primary,
			    old_primary_bytes, NULL, extra_first,
			    extra_first_bytes, form, cull_state, out);
}

/* Retained later frame: the source N2/N3 later batches insert exactly the
 * FIRST append's 60-byte pair group and stage at the same seam, with the
 * later epoch (Unlock, Unlock-0x30 depth-only or Unlock-0x80 no-clear).
 */
int m9x_faa_later_append_encode(const struct m9x_faa_graph_draw_owner owner[31],
				u64 previous_unlock, u32 clear_flags,
				u32 old_draw_count, u32 old_start,
				u32 secondary_cursor, const u8 *old_primary,
				u32 old_primary_bytes, const u8 *extra_first,
				u32 extra_first_bytes,
				enum m9x_faa_later_form form, bool cull_state,
				struct m9x_faa_dual_append_image *out)
{
	u32 tail = clear_flags == 3 ? 3632 : 3624, epoch;

	if (!owner || !old_primary || !extra_first || !out || !out->primary ||
	    !out->secondary ||
	    (clear_flags && clear_flags != 3 && clear_flags != 2) ||
	    old_draw_count < 1 ||
	    old_draw_count >= M9_FAA_DUAL_LATER_MAX_DRAWS ||
	    (previous_unlock & 15) || previous_unlock < 0x130 ||
	    previous_unlock >
		    U32_MAX - 0x150 - M9_FAA_DUAL_LATER_MAX_DRAWS * 0xa0 ||
	    old_primary_bytes < tail + (old_draw_count - 1) * 60)
		return -EINVAL;
	if (m9x_faa_graph_draw_owners_validate(owner))
		return -EINVAL;
	epoch = (u32)previous_unlock -
		(clear_flags == 3 ? 0 : clear_flags == 2 ? 0x30 : 0x80) +
		(old_draw_count - 1) * 0xa0;
	return append(owner, epoch, old_primary_bytes - (tail - 60),
		      tail - 3624, old_start, secondary_cursor, old_primary,
		      old_primary_bytes, NULL, extra_first, extra_first_bytes,
		      form, cull_state, out);
}

int m9x_faa_dual_later_append_encode(const struct m9x_faa_graph_draw_owner
					     owner[31],
				     u64 previous_unlock, u32 clear_flags,
				     u32 old_draw_count,
				     u32 first_secondary_cursor,
				     u32 secondary_cursor,
				     const u8 *old_primary,
				     u32 old_primary_bytes,
				     const u8 *first_secondary,
				     const u8 *extra_first_secondary,
				     struct m9x_faa_dual_append_image *out)
{
	u32 first = clear_flags == 3 ? 2736 : 2740, old_start;
	u32 sel;

	if (!first_secondary || !extra_first_secondary)
		return -EINVAL;
	/* A culled first later stage keeps its 24 bytes of 6A groups; same-cull
	 * appends behind it drop theirs.
	 */
	sel = m9_cmd_get(first_secondary + 1992) >> 24 & 3;
	if (sel != (m9_cmd_get(extra_first_secondary + 1992) >> 24 & 3) ||
	    secondary_cursor != first_secondary_cursor + first +
					(sel ? M9_FAA_DUAL_LATER_CULL_BYTES : 0) +
					(old_draw_count - 1) * 2868)
		return -EINVAL;
	old_start = old_draw_count == 1 ? first_secondary_cursor :
					  secondary_cursor - 2868;
	return m9x_faa_later_append_encode(owner, previous_unlock, clear_flags,
					   old_draw_count, old_start,
					   secondary_cursor, old_primary,
					   old_primary_bytes,
					   extra_first_secondary,
					   M9_FAA_DUAL_FIRST_SECONDARY_BYTES,
					   M9_FAA_LATER_PLAIN, false, out);
}
