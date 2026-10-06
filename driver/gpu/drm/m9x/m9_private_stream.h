/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef __M9_PRIVATE_STREAM_H__
#define __M9_PRIVATE_STREAM_H__

#include <linux/errno.h>
#include <linux/types.h>

/* Internal kernel-owned secondary ranges, never a userspace address ABI.
 * Class1 is the existing private BmBuffer GPUVA. DMA32 is the actual normal
 * FAA24c73 export: physical DMA address selector2, unchanged counted bytes,
 * and saved kind1 end-mode2. Callers retain the allocation and every operand
 * owner through the final resolve completion; encoding is not retirement.
 */
enum m9_private_stream_domain {
	M9_PRIVATE_STREAM_CLASS1 = 1,
	M9_PRIVATE_STREAM_DMA32 = 2,
};

struct m9_private_stream_owner {
	u64 address;
	u32 bytes;
	enum m9_private_stream_domain domain;
};

static inline int m9_private_stream_range(u32 words[3],
				 const struct m9_private_stream_owner *owner,
				 u32 start, u32 end)
{
	u64 first, limit;

	if (!owner || !words || !owner->bytes ||
	    (owner->domain != M9_PRIVATE_STREAM_CLASS1 &&
	     owner->domain != M9_PRIVATE_STREAM_DMA32) ||
	    (owner->address & 3) || (start & 3) || (end & 3) ||
	    start >= end || end > owner->bytes)
		return -EINVAL;
	/* Validate the complete immutable owner, not just this selected range.
	 * End is exclusive but still has to fit its tagged 32-bit export word.
	 */
	if (owner->address > 0xfffffffcULL ||
	    owner->bytes > 0xfffffffcULL - owner->address)
		return -ERANGE;
	first = owner->address + start;
	limit = owner->address + end;
	words[0] = 0x45452d2c;
	words[1] = (u32)first | owner->domain;
	words[2] = (u32)limit | 2u;
	return 0;
}

#endif
