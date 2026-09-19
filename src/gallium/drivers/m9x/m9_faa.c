/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */

#include <math.h>
#include <stdio.h>

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
   faa->sample_coverage = debug_get_bool_option("M9X_FAA_SAMPLE_COVERAGE",
                                                false);
   faa->alpha_to_coverage =
      debug_get_bool_option("M9X_FAA_ALPHA_TO_COVERAGE", false);
   faa->ae_mode = (int)debug_get_num_option("M9X_FAA_AE_MODE", -1);
   faa->ae_bit28 = debug_get_bool_option("M9X_FAA_AE_BIT28", false);
   faa->ef_mode = (int)debug_get_num_option("M9X_FAA_EF_MODE", -1);
   faa->f0_bit15 = debug_get_bool_option("M9X_FAA_F0_BIT15", false);
   faa->f0_bit24 = debug_get_bool_option("M9X_FAA_F0_BIT24", false);
   faa->bind_index = (int)debug_get_num_option("M9X_FAA_BIND_INDEX", -1);
   faa->bind_reset = debug_get_bool_option("M9X_FAA_BIND_RESET", false);
   faa->store_check = debug_get_bool_option("M9X_FAA_STORE_CHECK", false);
   faa->ad_force = (uint32_t)debug_get_num_option("M9X_FAA_AD", -1);
   faa->af_force = (uint32_t)debug_get_num_option("M9X_FAA_AF", -1);
   faa->ad_forced = faa->ad_force != 0xffffffffu;
   faa->af_forced = faa->af_force != 0xffffffffu;
   faa->trace = debug_get_bool_option("M9X_DEBUG", false);
   if (faa->trace)
      fprintf(stderr, "m9x: faa chip %d enable %d raster %d coverage %u "
              "invert %d sc %d a2c %d forced ad %d/%08x af %d/%08x\n",
              faa->chip, faa->enable, faa->raster,
              m9_faa_coverage_fixed(faa->coverage), faa->coverage_invert,
              faa->sample_coverage, faa->alpha_to_coverage, faa->ad_forced,
              faa->ad_force, faa->af_forced, faa->af_force);
   if (faa->trace)
      fprintf(stderr, "m9x: faa store bind index %d reset %d\n",
              faa->bind_index, faa->bind_reset);
   if (faa->trace)
      fprintf(stderr, "m9x: faa ae mode %d bit28 %d ef mode %d f0 %d/%d\n",
              faa->ae_mode, faa->ae_bit28, faa->ef_mode, faa->f0_bit15,
              faa->f0_bit24);
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

/*
 * The vendor's own arithmetic, in the order the allocator performs it: the
 * 32-bit product first, then the float scale, then the shift and the cap.
 */
uint64_t
m9_faa_fragment_bytes(uint32_t width, uint32_t height)
{
   /* 64-bit throughout: the vendor's 32-bit product wraps above about
    * 12.5 megapixels, and the cap is reached long before that anyway. */
   uint64_t product = (uint64_t)width * height * 0x158u;
   uint64_t scaled = (uint64_t)((double)product * 0.38499999046325684) >> 3;

   return scaled > M9_FAA_FRAGMENT_CAP_BYTES ? M9_FAA_FRAGMENT_CAP_BYTES
                                             : scaled;
}

uint64_t
m9_faa_fragment_aux_bytes(uint32_t width, uint32_t height)
{
   return m9_faa_fragment_bytes(width, height) >> 4;
}

void
m9_faa_edits(const struct m9_faa *faa, struct m9_faa_edits *edits)
{
   *edits = (struct m9_faa_edits) {
      .ae_and = 0xffffffffu, .ef_and = 0xffffffffu, .f0_and = 0xffffffffu,
   };
   if (!m9_faa_active(faa))
      return;
   if (faa->ae_mode >= 0) {
      edits->ae_and &= ~0x70u;
      edits->ae_or |= ((uint32_t)faa->ae_mode & 7u) << 4;
   }
   if (faa->ae_bit28)
      edits->ae_or |= 0x10000000u;
   if (faa->ef_mode >= 0) {
      edits->ef_and &= ~0xe0000000u;
      edits->ef_or |= ((uint32_t)faa->ef_mode & 7u) << 29;
   }
   if (faa->f0_bit15)
      edits->f0_or |= 0x00008000u;
   if (faa->f0_bit24)
      edits->f0_or |= 0x01000000u;
}

uint32_t
m9_faa_store_pattern(uint64_t dword_index)
{
   return 0x5a000000u | (uint32_t)(dword_index & 0xffffffu);
}

unsigned
m9_faa_raster_dwords(const struct m9_faa *faa)
{
   if (!m9_faa_active(faa))
      return 0;
   return (faa->raster ? M9_FAA_RASTER_DWORDS : 0) +
          (faa->bind_reset ? M9_FAA_RESET_DWORDS : 0) +
          (faa->bind_index >= 0 ? M9_FAA_BIND_DWORDS : 0);
}

unsigned
m9_faa_raster_relocs(const struct m9_faa *faa)
{
   return m9_faa_active(faa) && faa->bind_index >= 0 ? 1 : 0;
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
   if (faa->trace && !faa->traced) {
      ((struct m9_faa *)faa)->traced = true;
      fprintf(stderr, "m9x: faa ad %08x from %llu colour bytes, af %08x\n",
              (uint32_t)units, (unsigned long long)color_bytes,
              m9_faa_af(faa));
   }
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
      af |= 0x200u;
   if (faa->sample_coverage)
      af |= 0x400u;
   if (faa->alpha_to_coverage)
      af |= 0x800u;
   return af;
}
