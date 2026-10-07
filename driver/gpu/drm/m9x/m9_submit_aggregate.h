/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/* Inputs are separately parsed, relocated immutable jobs. This helper only
 * frames owned records; submission and lifetime remain with m9_submit.c. */
#define M9_COMPLETE_MAX_JOBS 4u
#define M9_COMPLETE_FOOTER_WORDS 11u
#define M9_COMPLETE_MAX_WORDS ((32768u - 8u) / 4u)

struct m9_complete_job {
   const u32 *image, *expected;
   u32 dwords;
};

static int
m9_complete_shape(const struct m9_complete_job *job, u32 *words,
                  u32 *ranges, u32 *token)
{
   u32 at = 0, out = 0, end = 0, nr = 0, highest = 0;
   if (!job || !job->image || !job->expected || !job->dwords ||
       job->dwords > 8192)
      return -EINVAL;
   while (at < job->dwords) {
      u32 header = job->image[at], family;
      unsigned values = 0;
      if (header == 0x45454545) { at++; continue; }
      if (header == PARHELIA_M9_RANGE_BREAK) {
         if (job->dwords - at < 2 || job->image[at + 1])
            return -EINVAL;
         if (out > end) { nr++; end = out; }
         at += 2;
         continue;
      }
      if (m9_draw_header(header, &family)) {
         if (job->dwords - at < 3) return -EINVAL;
         out += 3; at += 3; continue;
      }
      while (values < 4 && ((header >> (8 * values)) & 255) != 0x45)
         values++;
      if (!values || job->dwords - at < values + 1) return -EINVAL;
      for (unsigned i = 1; i <= values; i++)
         if (job->expected[at + i] & M9_EXPECT_DEPENDENCY) {
            u32 value = job->image[at + i];
            if ((value & 15) || value >= PARHELIA_M9_DEPENDENCY_LIMIT)
               return -EINVAL;
            highest = max(highest, value);
         }
      out += values + 2;
      at += values + 1;
   }
   if (out > end) nr++;
   if (!out || !nr) return -EINVAL;
   *words = out; *ranges = nr; *token = highest;
   return 0;
}

/* The three counted records of the existing kernel-owned bm-12 footer.
 * Both address and point derive from current owner/timeline arguments. */
static void
m9_complete_footer(u32 *out, u32 fence_bus, u32 final)
{
   const u32 words[] = { 1, 0x4545451b, fence_bus | 2,
                        3, 0x45e3e6e6, 3, 2, final | 1,
                        1, 0x45454543, final };
   memcpy(out, words, sizeof(words));
}

/* Preflight the complete result before touching any destination or output
 * metadata. Preserve every range and footer, and use the same next-target
 * advance as independently submitted complete jobs. No wrap admission here. */
static int
m9_complete_frame(const struct m9_complete_job *jobs, u32 count,
                  u32 base, u32 fence_bus, u32 *framed, u32 capacity,
                  u32 *ends, u32 range_capacity,
                  u32 *out_words, u32 *out_ranges, u32 *final)
{
   u32 nw[M9_COMPLETE_MAX_JOBS], nr[M9_COMPLETE_MAX_JOBS];
   u32 token[M9_COMPLETE_MAX_JOBS], span = 0, total = 0, ranges = 0;
   u32 out = 0, rend = 0, target = base;
   if (!jobs || !count || count > M9_COMPLETE_MAX_JOBS || !base ||
       (base & 15) || !fence_bus || (fence_bus & 15) ||
       fence_bus > U32_MAX - 4 || !framed || !ends ||
       !out_words || !out_ranges || !final)
      return -EINVAL;
   for (u32 i = 0; i < count; i++) {
      int ret = m9_complete_shape(&jobs[i], &nw[i], &nr[i], &token[i]);
      if (ret) return ret;
      if (span > PARHELIA_M9_DEPENDENCY_LIMIT - token[i] - 0x10 ||
          base > U32_MAX - span - token[i] - 0x10)
         return -E2BIG;
      span += token[i] + 0x10;
      if (i + 1 < count) {
         if (span > PARHELIA_M9_DEPENDENCY_LIMIT - 0x10) return -E2BIG;
         span += 0x10;
      }
      total += nw[i] + M9_COMPLETE_FOOTER_WORDS;
      ranges += nr[i] + 1;
   }
   if (total > M9_COMPLETE_MAX_WORDS || total > capacity ||
       ranges > range_capacity)
      return -E2BIG;
   for (u32 i = 0; i < count; i++) {
      u32 actual_words, actual_ranges, actual_token;
      m9_frame_records(jobs[i].image, jobs[i].expected, jobs[i].dwords,
                       target, &actual_words, ends + rend,
                       &actual_ranges, &actual_token, framed + out);
      /* Shape and framing share the already-validated packet walk. */
      for (u32 j = 0; j < actual_ranges; j++) ends[rend + j] += out;
      out += actual_words; rend += actual_ranges;
      u32 reached = target + actual_token + 0x10;
      m9_complete_footer(framed + out, fence_bus, reached);
      out += M9_COMPLETE_FOOTER_WORDS;
      ends[rend++] = out;
      target = reached + 0x10;
   }
   *out_words = out; *out_ranges = rend; *final = base + span;
   return 0;
}
