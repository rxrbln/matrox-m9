/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */

#include <math.h>

#include "util/u_debug.h"

#include "m9_faa.h"

void
m9_faa_init(struct m9_faa *faa, uint16_t device_id)
{
   *faa = (struct m9_faa) { 0 };
   /* Dispatch on the family, not on "this is the only card we have": a
    * second family reaches this with a chip of its own or with NONE. */
   faa->chip = device_id == 0x0540 ? M9_FAA_CHIP_M9 : M9_FAA_CHIP_NONE;
   if (faa->chip == M9_FAA_CHIP_NONE)
      return;
   faa->enable = debug_get_bool_option("M9X_FAA", false);
   faa->raster = debug_get_bool_option("M9X_FAA_RASTER", false);
   faa->coverage = (float)debug_get_num_option("M9X_FAA_COVERAGE", 256) /
                   256.0f;
   faa->coverage_invert = debug_get_bool_option("M9X_FAA_INVERT", false);
   faa->af_flags = (uint32_t)debug_get_num_option("M9X_FAA_AF_FLAGS", 0) &
                   3u;
   faa->ad_force = (uint32_t)debug_get_num_option("M9X_FAA_AD", -1);
   faa->af_force = (uint32_t)debug_get_num_option("M9X_FAA_AF", -1);
   faa->ad_forced = faa->ad_force != 0xffffffffu;
   faa->af_forced = faa->af_force != 0xffffffffu;
}

bool
m9_faa_active(const struct m9_faa *faa)
{
   return faa->chip != M9_FAA_CHIP_NONE &&
          (faa->enable || faa->ad_forced || faa->af_forced);
}

uint32_t
m9_faa_coverage_fixed(float coverage)
{
   float clamped = coverage < 0.0f ? 0.0f : coverage > 1.0f ? 1.0f : coverage;

   return (uint32_t)lrintf(clamped * 256.0f) & 0x1ffu;
}

uint32_t
m9_faa_ad(const struct m9_faa *faa, uint64_t color_bytes)
{
   uint64_t units;

   if (faa->ad_forced)
      return faa->ad_force;
   if (faa->chip == M9_FAA_CHIP_NONE || !faa->enable)
      return M9_FAA_AD_OFF;
   /* ad[20:0] is the colour allocation's byte extent in 32-byte units,
    * saturated (STATIC, MxMD.dll 0x1395a4..0x1395dc).  The replacement
    * overwrites the seed's bit 5, so nothing of the off form survives. */
   units = color_bytes >> 5;
   if (units > 0xfffffu)
      units = 0xfffffu;
   return (uint32_t)units;
}

uint32_t
m9_faa_af(const struct m9_faa *faa)
{
   uint32_t af;

   if (faa->af_forced)
      return faa->af_force;
   if (faa->chip == M9_FAA_CHIP_NONE || !faa->enable)
      return M9_FAA_AF_OFF;
   af = m9_faa_coverage_fixed(faa->coverage);
   if (faa->coverage_invert)
      af |= 0x800u;
   return af | faa->af_flags << 12;
}
