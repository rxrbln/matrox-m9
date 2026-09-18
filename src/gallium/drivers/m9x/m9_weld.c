/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Welds the repeated records of an unindexed draw, exactly: only records whose
 * every byte agrees are merged, so what the preprocessing pass and the
 * rasteriser see is unchanged and the corner order -- and with it the
 * provoking vertex of a flat-shaded primitive -- is preserved.
 */
#include "m9_weld.h"

#include <stdlib.h>
#include <string.h>

/* Bit patterns rather than values: -0.0 and 0.0 hash apart and so are never
 * merged, which errs towards keeping a record rather than losing one. */
static uint32_t
m9_weld_hash(const uint32_t *words, unsigned dwords)
{
   uint32_t h = 2166136261u;

   for (unsigned i = 0; i < dwords; i++) {
      h = (h ^ words[i]) * 16777619u;
      h ^= h >> 15;
   }
   return h;
}

static uint32_t
m9_weld_table_size(uint32_t count)
{
   uint32_t size = 16;

   /* Half load at most, so a probe walk stays short. */
   while (size < count * 2 && size < (1u << 31))
      size *= 2;
   return size;
}

bool
m9_weld_build(struct m9_weld *weld, const float *records, uint32_t count,
              unsigned record_dwords)
{
   const uint32_t *words = (const uint32_t *)records;
   size_t stride = (size_t)record_dwords * sizeof(uint32_t);
   uint32_t size, mask, *table;

   if (!weld || !records || !count || !record_dwords)
      return false;
   memset(weld, 0, sizeof(*weld));
   size = m9_weld_table_size(count);
   mask = size - 1;
   /* One past the unique index, so a zero entry is an empty slot. */
   table = calloc(size, sizeof(*table));
   weld->index = malloc((size_t)count * sizeof(*weld->index));
   weld->source = malloc((size_t)count * sizeof(*weld->source));
   if (!table || !weld->index || !weld->source) {
      free(table);
      m9_weld_fini(weld);
      return false;
   }
   for (uint32_t i = 0; i < count; i++) {
      const uint32_t *record = words + (size_t)i * record_dwords;
      uint32_t slot = m9_weld_hash(record, record_dwords) & mask;

      for (;;) {
         uint32_t held = table[slot];

         if (!held) {
            table[slot] = weld->unique + 1;
            weld->source[weld->unique] = i;
            weld->index[i] = weld->unique++;
            break;
         }
         if (!memcmp(words + (size_t)weld->source[held - 1] * record_dwords,
                     record, stride)) {
            weld->index[i] = held - 1;
            break;
         }
         slot = (slot + 1) & mask;
      }
   }
   free(table);
   weld->count = count;
   for (uint32_t i = 0; i < count; i++) {
      uint32_t lo, hi, n = count - i < 3 ? count - i : 3;

      if (m9_weld_span(weld, i, n, &lo, &hi) && hi - lo + 1 > weld->widest)
         weld->widest = hi - lo + 1;
   }
   return true;
}

void
m9_weld_fini(struct m9_weld *weld)
{
   if (!weld)
      return;
   free(weld->index);
   free(weld->source);
   memset(weld, 0, sizeof(*weld));
}

bool
m9_weld_span(const struct m9_weld *weld, uint32_t first, uint32_t count,
             uint32_t *lo, uint32_t *hi)
{
   uint32_t a, b;

   if (!weld || !weld->index || !lo || !hi || !count ||
       first > weld->count || count > weld->count - first)
      return false;
   a = b = weld->index[first];
   for (uint32_t i = 1; i < count; i++) {
      uint32_t v = weld->index[first + i];

      if (v < a)
         a = v;
      if (v > b)
         b = v;
   }
   *lo = a;
   *hi = b;
   return true;
}

uint32_t
m9_weld_run(const struct m9_weld *weld, uint32_t first, uint32_t max,
            unsigned grain, uint32_t *lo, uint32_t *hi)
{
   uint32_t a = 0, b = 0, k = 0;

   if (!weld || !weld->index || !lo || !hi || !grain || !max ||
       first >= weld->count)
      return 0;
   while (first + k + grain <= weld->count) {
      uint32_t na = a, nb = b;

      for (unsigned c = 0; c < grain; c++) {
         uint32_t v = weld->index[first + k + c];

         if (!k && !c) {
            na = nb = v;
            continue;
         }
         if (v < na)
            na = v;
         if (v > nb)
            nb = v;
      }
      if (nb - na + 1 > max)
         break;
      a = na;
      b = nb;
      k += grain;
   }
   /* A tail shorter than one whole primitive is not drawn on its own. */
   if (!k)
      return 0;
   *lo = a;
   *hi = b;
   return k;
}
