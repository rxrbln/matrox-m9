/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef M9_FAA_GRAPH_FRAME_APPEND_ENCODE_H
#define M9_FAA_GRAPH_FRAME_APPEND_ENCODE_H
#include "m9_faa_graph_color_encode.h"

#define M9_FAA_FRAME_APPEND_PRIMARY_INSERT_BYTES 60u
#define M9_FAA_FRAME_APPEND_LOCK_TAIL_BYTES 3572u
#define M9_FAA_FRAME_APPEND_SECONDARY_BYTES 2700u

struct m9x_faa_frame_append_image {
	u8 *primary, *secondary;
	u32 primary_capacity, secondary_capacity;
};

/* Pure one-additional-DRAW source profile. The primary output is the entire
 * first primary image with a new 60-byte DRAW inserted before its 3572-byte
 * Lock tail. Posting only its last 60 bytes would replay a Lock footer and is
 * invalid. The two input command images must
 * be produced from the same live COLOR4 shader and retained owners by the
 * qualified first/depth encoders; the caller retains all earlier input ranges
 * and six graph fences until a later final Lock retirement. No GPU work here.
 * The original two-output, 3-record case is source byte-exact. The live
 * one-output and larger-record forms compose already qualified first-stage
 * encoders but remain experimental until their own hardware image receipt.
 * No later caller after Lock/Unlock or changed program is authorized here.
 */
int m9x_faa_frame_append_encode(
	const struct m9x_faa_graph_draw_owner owner[31],
	u32 primary_cursor, u32 secondary_cursor, u64 live_next_epoch,
	u32 cb_owner_offset, u32 vertex_owner_offset,
	const struct m9x_faa_color_input *input, bool reused_program,
	const u8 *first_color_secondary, u32 first_color_bytes,
	const u8 *bound_first_primary, u32 bound_primary_bytes,
	const u8 *bound_first_secondary, u32 bound_secondary_bytes,
	const struct m9x_faa_frame_append_image *output);
#endif
