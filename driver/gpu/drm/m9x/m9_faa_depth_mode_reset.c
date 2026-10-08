// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/string.h>
#include <crypto/sha2.h>
#include "m9_cmd.h"
#include "m9_faa_depth_mode_reset.h"
#include "m9_faa_graph_depth_encode.h"

static const u8 digest[32] = { 0x68, 0xd8, 0x5c, 0xf3, 0xb1, 0xf9, 0xc9, 0x6f,
			       0xe2, 0xe3, 0x12, 0xe3, 0xad, 0x7e, 0x39, 0xe4,
			       0x6b, 0xc1, 0x61, 0x5e, 0xb5, 0x13, 0x6a, 0xaa,
			       0x75, 0x5f, 0x96, 0xac, 0x33, 0xb3, 0x8b, 0x16 };
int m9x_faa_depth_mode_reset_encode(const u8 *blob, size_t bytes,
				    const struct m9x_faa_graph_draw_owner *o,
				    const struct m9x_faa_depth_views *v,
				    const struct m9x_faa_depth_mode_state
					    *before,
				    bool resolved_primary, u64 epoch,
				    struct m9x_faa_depth_mode_reset_image *out)
{
	struct m9x_faa_depth_mode_image route;
	struct m9x_faa_depth_stage_image stages;
	struct m9x_faa_surface_layout layout;
	u8 hash[32], *image;
	u32 n, flags, i;
	static const u32 token_at[8] = { 8, 92, 128, 312, 712, 768, 904, 3668 };
	static const u32 token_delta[8] = { 1,	  0x11, 0x21, 0x20,
					    0x31, 0x41, 0x40, 0x40 };
	if (!blob || bytes != M9_FAA_DEPTH_MODE_RESET_TEMPLATE_BYTES || !o ||
	    !v || !before || !before->valid || !out || !out->primary ||
	    (epoch & 15) || epoch > ~(u64)0 - 0x40 || (u32)epoch > 0xffffffbfu)
		return -EINVAL;
	n = M9_FAA_DEPTH_MODE_RESET_TEMPLATE_BYTES +
	    (before->embedded_hz ? 16 : 0);
	if (out->capacity < n)
		return -ERANGE;
	sha256(blob, bytes, hash);
	if (memcmp(hash, digest, sizeof(hash)))
		return -EINVAL;
	if (m9x_faa_graph_draw_owners_validate(o) ||
	    m9x_faa_depth_stages_encode(v, 0x3f800000, &stages))
		return -EINVAL;
	if (!m9x_faa_runtime_shape(v->owner[0].width, v->owner[0].height) ||
	    !m9x_faa_surface_layout(v->owner[0].width, v->owner[0].height,
				    &layout))
		return -EINVAL;
	if (o[1].bytes < layout.owner[M9_FAA_SURFACE_PRIMARY].capacity ||
	    o[2].bytes < layout.owner[M9_FAA_SURFACE_MAIN].capacity ||
	    o[3].bytes < layout.owner[M9_FAA_SURFACE_AUXILIARY].capacity ||
	    o[8].bytes < 128 || o[22].bytes < 712)
		return -ERANGE;
	flags = before->enabled ? 15 : 32 | 14;
	if (m9x_faa_depth_mode_two_plan(flags, 0x20004000, before, o, v,
					&route))
		return -EINVAL;
	/* All preflight refusals precede any caller output mutation. */
	image = out->primary;
	memmove(image, blob, bytes);
	/* source25680/24870 joins, not numerical address classification */
	m9_cmd_put(image + 72, o[8].base | 1);
	m9_cmd_put(image + 76, o[8].base + 20);
	m9_cmd_put(image + 136, o[1].base | 1);
	m9_cmd_put(image + 148, route.a9);
	/* fbb19/fbbd3 descriptor widths; aab86..aaba3 main backing extent>>5
	 * feeds shadow+28 then fbe16..fbe31. Use source logical/backing extent,
	 * never native page-rounded GEM capacity or captured dimension words.
	 */
	m9_cmd_put(image + 140, layout.primary_descriptor);
	m9_cmd_put(image + 152, layout.companion_descriptor);
	for (i = 208; i <= 220; i += 4)
		m9_cmd_put(image + i, layout.ad);
	m9_cmd_put(image + 580, layout.height << 16 | layout.width);
	m9_cmd_put(image + 160, route.ba);
	m9_cmd_put(image + 164, route.ba_descriptor);
	m9_cmd_put(image + 172, o[22].base | 1);
	m9_cmd_put(image + 196, o[2].base | 1);
	m9_cmd_put(image + 200, o[3].base | 1);
	m9_cmd_put(image + 588, o[22].base | 1);
	m9_cmd_put(image + 652, ((o[22].base + 644) & 0xffffff84u) | 5);
	m9_cmd_put(image + 656, ((o[22].base + 708) & 0xfffffffdu) | 1);
	m9_cmd_put(image + 664, (o[8].base + 8) | 1);
	m9_cmd_put(image + 676, (o[8].base + 12) | 1);
	m9_cmd_put(image + 688, (o[8].base + 16) | 1);
	m9_cmd_put(image + 776, o[8].base | 1);
	m9_cmd_put(image + 3804, resolved_primary ? o[1].base | 1 : 0);
	m9_cmd_put(image + 3808,
		   resolved_primary ? 0xf0210000u | v->owner[0].width : 0);
	m9_cmd_put(image + 3812,
		   resolved_primary ? 0x10000u | v->owner[0].height : 0);
	for (i = 0; i < 8; i++)
		m9_cmd_put(image + token_at[i], (u32)epoch + token_delta[i]);
	if (before->embedded_hz) {
		/* Original two finite25270 dependencies when the embedded BA/HZ route
		 * was already activated. They depend on reset's own+10/+30 producers.
		 */
		m9_cmd_put(image + 104, 0x20007000);
		memmove(image + 724, image + 716, bytes - 716);
		m9_cmd_put(image + 716, 0x45454543);
		m9_cmd_put(image + 720, (u32)epoch + 0x30);
		memmove(image + 104, image + 96, bytes + 8 - 96);
		m9_cmd_put(image + 96, 0x45454543);
		m9_cmd_put(image + 100, (u32)epoch + 0x10);
	}
	out->bytes = n;
	out->end_bytes[0] = n - 28;
	out->end_bytes[1] = n - 28;
	out->end_bytes[2] = n;
	out->final_point = epoch + 0x40;
	return 0;
}

int m9x_faa_depth_mode_save_encode(const struct m9x_faa_graph_draw_owner *o,
				   u8 *out, size_t capacity)
{
	u32 image[29] = { 0x45454524, 0,	  0x453ce6e6, 0x6dc00000,
			  0x42000000, 0xa1,	  0x44444444, 0x30081898,
			  0x30081898, 0x30081898, 0x30081898, 0x44444444,
			  0x30081898, 0x30081898, 0x30081898, 0x30081898,
			  0x45452f2e, 0,	  0,	      0x4545421a,
			  0,	      0xa1000009, 0x4545421a, 0,
			  0xa3000009, 0x45e6421a, 0,	      0xa4000009,
			  0x40008000 };
	if (!o || !out || m9x_faa_graph_draw_owners_validate(o))
		return -EINVAL;
	if (capacity < sizeof(image) || o[8].bytes < 20 || o[22].bytes < 712)
		return -ERANGE;
	image[1] = o[22].base | 1;
	image[17] = ((o[22].base + 644) & 0xffffff84u) | 5;
	image[18] = ((o[22].base + 708) & 0xfffffffdu) | 1;
	image[20] = (o[8].base + 8) | 1;
	image[23] = (o[8].base + 12) | 1;
	image[26] = (o[8].base + 16) | 1;
	memcpy(out, image, sizeof(image));
	return 0;
}
