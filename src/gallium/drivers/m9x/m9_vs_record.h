/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_VS_RECORD_H
#define M9_VS_RECORD_H
#include <stdint.h>
#include <stdbool.h>

/* The EE19 descriptor has a logical DWORD stride, not a CPU AoS stride.
 * Retired six-field GPU records establish 64-vertex component blocks and
 * bank-bit movement inside 2048-byte quanta. Reserve complete quanta even
 * for a partial block; keep the raster index ceiling independently bounded.
 * This is an extent bound, not a general physical-layout decoder. */
#define M9_VS_RECORD_VERTICES 64u
#define M9_VS_RECORD_BANK_BYTES 2048u
#define M9_VS_RECORD_MAGIC 0x4d395653u
#define M9_VS_RECORD_HEADER_DWORDS 8u

static inline uint64_t
m9_vs_record_bytes(uint32_t vertices, uint32_t fields)
{
   if (!vertices || vertices > 65532u || fields < 4 || fields > 6)
      return 0;
   uint64_t blocks = ((uint64_t)vertices + 63u) / 64u;
   uint64_t bytes = blocks * 64u * fields * 16u;
   return (bytes + M9_VS_RECORD_BANK_BYTES - 1u) &
          ~(uint64_t)(M9_VS_RECORD_BANK_BYTES - 1u);
}
/* The chain's packed owner occupies views 8..19 of the retained arena.
 * Check the actual BO extent before submission, not just a relocation base. */
static inline bool
m9_vs_record_fits(uint32_t vertices, uint32_t fields, uint32_t view,
                  uint32_t view_bytes, uint64_t owner_bytes)
{
   uint64_t bytes = m9_vs_record_bytes(vertices, fields);
   uint64_t offset = (uint64_t)view * view_bytes;
   return bytes && view >= 8u && view < 20u && view + fields <= 20u &&
          view_bytes && !(view_bytes % M9_VS_RECORD_BANK_BYTES) &&
          bytes <= (uint64_t)fields * view_bytes &&
          offset <= owner_bytes && bytes <= owner_bytes - offset;
}
#endif
