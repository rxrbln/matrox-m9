/* SPDX-License-Identifier: MIT */
#ifndef M9_FAA_GRAPH_BATCH_LIMITS_H
#define M9_FAA_GRAPH_BATCH_LIMITS_H
/* Current retained secondary owner reserves its final 512 bytes for pool
 * closure and 48 bytes for a following scope. The smallest admitted source
 * profile uses 1352 bytes for its first job, then 1484 for every extra job.
 * This is an absolute admission ceiling, not a promise that mixed jobs fit:
 * native preparation checks actual secondary, data and code extents too. */
#define M9_FAA_BATCH_SECONDARY_BYTES 0x3fe00u
#define M9_FAA_FRAME_BATCH_MAX_DRAWS \
 ((M9_FAA_BATCH_SECONDARY_BYTES - 48u - 1352u) / 1484u + 1u)
#endif
