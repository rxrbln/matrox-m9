/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_JOB_BOS_H
#define M9_JOB_BOS_H

/* M9_MAX_BOS is the submission ABI's object ceiling, defined by the
 * including m9_internal.h. The set contains live GEM handles, not resource
 * pointers: imported views and mip/proxy owners may name the same object. */
struct m9_job_bos {
   uint32_t handle[M9_MAX_BOS];
   unsigned count;
};

static inline bool
m9_job_bos_add(struct m9_job_bos *set, uint32_t handle)
{
   if (!handle)
      return true;
   for (unsigned i = 0; i < set->count; i++)
      if (set->handle[i] == handle)
         return true;
   if (set->count == M9_MAX_BOS)
      return false;
   set->handle[set->count++] = handle;
   return true;
}

/* Stable owners are joined by handle. Owners selected or grown only at
 * submission contribute their proved maximum number, without guessing the
 * future arena handles or collapsing two possibly independent objects. */
static inline bool
m9_job_bos_fit(const struct m9_job_bos *sampled,
                const struct m9_job_bos *stable, unsigned changing)
{
   if (changing > M9_MAX_BOS)
      return false;
   if (sampled->count + stable->count + changing <= M9_MAX_BOS)
      return true;
   struct m9_job_bos joined = *sampled;

   for (unsigned i = 0; i < stable->count; i++)
      if (!m9_job_bos_add(&joined, stable->handle[i]))
         return false;
   return joined.count <= M9_MAX_BOS - changing;
}

#endif
