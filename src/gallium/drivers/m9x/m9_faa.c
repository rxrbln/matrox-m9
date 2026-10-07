/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */

#include <math.h>
#include <stdio.h>

#include "util/u_debug.h"

#include "m9_faa.h"
#include "m9_draw_params.h"

const uint32_t m9_faa_sentinel_init[M9_FAA_SENTINEL_INIT_DWORDS] = {
   0xa4a3a1a1, 0x28041968, 0, 0, 0,
};

void
m9_faa_init(struct m9_faa *faa, uint16_t device_id)
{
   int64_t companion_format;

   *faa = (struct m9_faa) { 0 };
   /* Dispatch on the family, not on "this is the only card we have": a
    * second family reaches this with a chip of its own or with NONE. */
   faa->chip = device_id == 0x0540 ? M9_FAA_CHIP_M9 : M9_FAA_CHIP_NONE;
   if (faa->chip == M9_FAA_CHIP_NONE)
      return;
   faa->enable = debug_get_bool_option("M9X_FAA", false);
   faa->prepare = debug_get_bool_option("M9X_FAA_PREPARE", false);
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
   faa->store_bind = debug_get_bool_option("M9X_FAA_STORE", false);
   companion_format = debug_get_num_option("M9X_FAA_COMPANION", -1);
   /* Validate before narrowing: the descriptor has seven format bits, and
    * an alias must not bypass the source-derived 0x3d layout guard. */
   if (companion_format < -1 || companion_format > 127) {
      fprintf(stderr, "m9x: invalid M9X_FAA_COMPANION=%lld; expected -1 or "
              "0..127; FAA probe disabled\n", (long long)companion_format);
      *faa = (struct m9_faa) { .chip = M9_FAA_CHIP_NONE };
      return;
   }
   faa->companion_format = (int)companion_format;
   faa->bind_index = (int)debug_get_num_option("M9X_FAA_BIND_INDEX", -1);
   faa->bind_desc = debug_get_num_option("M9X_FAA_BIND_DESC", -1);
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
      fprintf(stderr, "m9x: faa store bind index %d desc %lld reset %d\n",
              faa->bind_index, (long long)faa->bind_desc, faa->bind_reset);
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
static uint64_t
m9_faa_fragment_budget(uint32_t width, uint32_t height)
{
   /* 64-bit throughout: the vendor's 32-bit product wraps above about
    * 12.5 megapixels, and the cap is reached long before that anyway. */
   uint64_t product = (uint64_t)width * height * 0x158u;
   uint64_t scaled = (uint64_t)((double)product * 0.38499999046325684) >> 3;

   return scaled > M9_FAA_FRAGMENT_CAP_BYTES ? M9_FAA_FRAGMENT_CAP_BYTES
                                             : scaled;
}

static uint64_t
m9_faa_fragment_extent(uint64_t budget)
{
   /* Actual ac7e0 uses profile12/format66, a 1024-element linear row and
    * max(1, ((budget+1023)>>10)/4) rows. Genuine45eb0 appends32 bytes,
    * including at the32MiB budget cap. This is not a tiled depth layout. */
   uint64_t rows = (budget + 1023) >> 12;

   return (rows ? rows : 1) * 4096 + 32;
}

uint64_t
m9_faa_fragment_bytes(uint32_t width, uint32_t height)
{
   return width && height ?
      m9_faa_fragment_extent(m9_faa_fragment_budget(width, height)) : 0;
}

uint64_t
m9_faa_fragment_aux_bytes(uint32_t width, uint32_t height)
{
   /* The sixteenth applies to the byte budget, before row rounding and
    * the auxiliary resource's independent guard. */
   return width && height ?
      m9_faa_fragment_extent(m9_faa_fragment_budget(width, height) >> 4) : 0;
}

bool
m9_faa_companion_layout(uint32_t width, uint32_t height,
                         struct m9_faa_companion_layout *layout)
{
   if (!layout || !width || !height || width > 2048 || height > 2048)
      return false;
   uint32_t row = (width + 63) & ~63u;
   uint32_t rows = (height + 15) & ~15u;
   uint64_t tail = (uint64_t)(rows - height) * row + row - width;
   uint64_t elements = (uint64_t)row * rows;

   /* Genuine45eb0 reserves four 64-bit elements beyond the last visible
    * pixel. Existing row/height padding already supplies some or all of
    * that guard:319x240 needs24 bytes,320x240 needs32,321x240 needs none. */
   if (tail < 4)
      elements += 4 - tail;
   *layout = (struct m9_faa_companion_layout) {
      .size = elements * 8,
      .row_span = row,
      .allocation_height = rows,
      /* Genuine467e0 selector13: format3d, sample-layout2, ordinary tile
       * state except the width<=64,height<=32 small allocation family. */
      .descriptor = 0x7a020000u | row |
                    (width <= 64 && height <= 32 ? 0 : 0x00200000u),
   };
   return true;
}

bool
m9_faa_primary_layout(uint32_t width, uint32_t height, uint8_t format,
                        struct m9_faa_primary_layout *layout)
{
   if (!layout || !width || !height || width > 2048 || height > 2048 ||
       (format != M9_COLOR_FORMAT_A8R8G8B8 &&
        format != M9_COLOR_FORMAT_A2B10G10R10 &&
        format != M9_COLOR_FORMAT_A2R10G10B10))
      return false;
   bool small = width <= 64 && height <= 64;
   uint32_t row = (width + 31) & ~31u;
   uint32_t quantum = small ? 4 : 32;
   uint32_t rows = (height + quantum - 1) & ~(quantum - 1);
   uint64_t byte_quantum = small ? 1024 : 4096;
   if (width <= 16 && height <= 16)
      rows = 16;
   uint64_t elements = (uint64_t)row * rows;
   uint64_t tail = (uint64_t)(rows - height) * row + row - width;
   if (tail < 8)
      elements += 8 - tail;
   *layout = (struct m9_faa_primary_layout) {
      .size = (elements * 4 + byte_quantum - 1) & ~(byte_quantum - 1),
      .row_span = row, .allocation_height = rows,
      .descriptor = ((uint32_t)format << 25) | 0x00020000u |
                    (small ? 0 : 0x00200000u) | row,
   };
   return true;
}

bool
m9_faa_graph_layout(uint32_t width, uint32_t height,
                     struct m9_faa_graph_layout *layout)
{
   struct m9_faa_companion_layout companion;

   if (!layout || !m9_faa_companion_layout(width, height, &companion))
      return false;
   *layout = (struct m9_faa_graph_layout) {
      .width = width, .height = height,
      /* Genuine backing extent includes its independent128-byte rounding.
       * Keep it here even when the kernel rounds the GEM to a larger page. */
      .main_bytes = (m9_faa_fragment_bytes(width, height) + 127) & ~127ull,
      .auxiliary_bytes = (m9_faa_fragment_aux_bytes(width, height) + 127) & ~127ull,
      .companion_bytes = companion.size,
      .hz_bytes = m9_hz_bytes(width, height),
      .companion_row_span = companion.row_span,
      .companion_desc = companion.descriptor,
      .hz_desc = 0xf0010000u | m9_depth_row_span(width, height),
   };
   return true;
}

bool
m9_faa_store_ad(uint32_t seed, uint64_t logical_bytes, uint32_t *ad)
{
   if (!ad || !logical_bytes || (logical_bytes & 127) ||
       logical_bytes > M9_FAA_FRAGMENT_CAP_BYTES + 128ull)
      return false;
   *ad = (seed & ~0x1fffffu) | (uint32_t)(logical_bytes >> 5);
   return true;
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

bool
m9_faa_lower_compiled04(uint32_t compiled04, struct m9_faa_edits *edits)
{
   uint32_t mode = (compiled04 >> 4) & 7u;
   *edits = (struct m9_faa_edits) {
      .ae_and = 0xffffffffu,
      .ef_and = ~0xe0000000u, .ef_or = mode << 29,
      .f0_and = ~0x01008000u,
      .f0_or = ((compiled04 & 1u) << 15) |
               ((compiled04 & 6u) ? 0x01000000u : 0),
   };
   return mode != 0 && mode != 3;
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
          (faa->store_bind ? M9_FAA_STORE_DWORDS : 0) +
          (faa->bind_reset ? M9_FAA_RESET_DWORDS : 0) +
          (faa->bind_index >= 0 ? M9_FAA_BIND_DWORDS : 0);
}

unsigned
m9_faa_raster_relocs(const struct m9_faa *faa)
{
   return m9_faa_active(faa) ?
          (faa->bind_index >= 0 ? 1u : 0u) + (faa->store_bind ? 2u : 0u) : 0;
}

uint32_t
m9_faa_ad(const struct m9_faa *faa, uint64_t logical_bytes)
{
   uint32_t ad;

   if (faa->ad_forced)
      return faa->ad_force;
   if (faa->chip == M9_FAA_CHIP_NONE || !faa->enable)
      return M9_FAA_AD_OFF;
   /* The low21 bits describe the logical fragment store, including its
    * 128-byte backing alignment, not the color BO or GEM page padding.
    * At the vendor budget cap the independent guard yields 0x100004;
    * saturating to 20 bits silently truncates that valid extent. */
   if (!m9_faa_store_ad(0, logical_bytes, &ad))
      return M9_FAA_AD_OFF;
   if (faa->trace && !faa->traced) {
      ((struct m9_faa *)faa)->traced = true;
      fprintf(stderr, "m9x: faa ad %08x from %llu logical store bytes, af %08x\n",
              ad, (unsigned long long)logical_bytes, m9_faa_af(faa));
   }
   return ad;
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
