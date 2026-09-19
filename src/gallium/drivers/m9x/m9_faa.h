/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FAA_H
#define M9_FAA_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Multisampling state of the M-series engine.  The vendor's Direct3D user
 * mode driver builds exactly two dwords for it and writes them to command
 * indices ad and af -- ad four times, af once (STATIC, MxMD.dll emitter
 * 0xfb920, header words 0xadadadad and 0x454545af; this driver's grammar
 * pads a header with 0x45, so those are four ad values and one af value).
 * Neither index takes a relocation in the kernel validator, so no address
 * reaches the unit through them.
 *
 * Every field below is STATIC evidence from MxMD.dll unless a comment says
 * otherwise.  Nothing here is on by default: M9X_FAA gates the whole path
 * and the caps stay single-sampled until a measurement earns a change.
 */

enum m9_faa_chip {
   M9_FAA_CHIP_NONE,   /* family with no recovered multisample unit */
   M9_FAA_CHIP_M9,     /* 0x0540, the M-series this driver drives */
};

struct m9_faa {
   enum m9_faa_chip chip;
   bool enable;
   /* Emit the pair in the raster range as well as the preprocessing one.
    * The captured vendor stream carries it in preprocessing only; a sticky
    * register makes that sufficient, which is what M9X_FAA_RASTER tests. */
   bool raster;
   /* af[8:0] is auxiliary state 25, a float in 1.8 fixed point whose reset
    * value 1.0f gives 0x100, fed by the vendor OpenGL driver from
    * glSampleCoverage; af[9] is auxiliary state 24, that call's invert
    * flag; af[10] and af[11] are bits 2 and 1 of auxiliary state 23, which
    * the vendor ICD packs from GL_SAMPLE_COVERAGE and
    * GL_SAMPLE_ALPHA_TO_COVERAGE.  The master multisample enable is bit 0
    * of that same auxiliary word and does NOT reach af; it is carried by a
    * dword this driver does not yet write.
    *
    * MEASURED on the M9128: every af bit is inert except bit 11, which
    * stops the engine -- so the word is live silicon, and what is missing
    * is the enable, not the af construction. */
   float coverage;
   bool coverage_invert;    /* af bit 9 */
   bool sample_coverage;    /* af bit 10 */
   bool alpha_to_coverage;  /* af bit 11, stops the engine on its own */
   /* Raw overrides, for probes that sweep a field the meaning of which is
    * not yet known.  A sweep must be able to reach a value the formula
    * cannot produce, or it cannot test the formula. */
   /* Index ae bits 6:4 are the engine's AA mode selector: the vendor's
    * compiler writes 7 with multisampling off, 4 with it on, 5 with it on
    * beside a depth companion, and 3 when the target has no allocation
    * (STATIC, MxMD.dll 0x139521..0x139594 and the M-series Linux driver's
    * m9x29045 at 0x3c82d0..0x3c8310, which agree row for row).  Bit 28 is
    * set by the same lowerer.  A negative mode leaves the captured word
    * alone, which is the default. */
   int ae_mode;
   bool ae_bit28;
   bool ad_forced, af_forced;
   uint32_t ad_force, af_force;
   bool trace, traced;
};

/* ad and af as the engine is left when multisampling is off: the seed form
 * the vendor writes before a backed surface is known, and zero.  These are
 * the values the captured vendor stream carries in every pass. */
#define M9_FAA_AD_OFF 0x00000020u
#define M9_FAA_AF_OFF 0x00000000u

/* Dwords a raster range costs when it restates the pair: four ad values and
 * one af value, one packet header each. */
#define M9_FAA_RASTER_DWORDS 7u

void m9_faa_init(struct m9_faa *faa, uint16_t device_id);
/* Whether a job has to carry the pair at all: a raw override drives it
 * without the enable, so a probe can sweep a field the formula cannot
 * reach. */
bool m9_faa_active(const struct m9_faa *faa);
/* round(coverage * 256), the magic-bias conversion the vendor applies to
 * auxiliary state 25 before masking it into af[8:0] (magic double
 * 26388279066624.0 = 1.5 * 2^44, so the ulp is 1/256). */
uint32_t m9_faa_coverage_fixed(float coverage);
uint32_t m9_faa_ad(const struct m9_faa *faa, uint64_t color_bytes);
uint32_t m9_faa_af(const struct m9_faa *faa);
/* The and/or pair that edits the AA mode into an index-ae word. */
void m9_faa_ae_edit(const struct m9_faa *faa, uint32_t *and_mask,
                    uint32_t *or_mask);

#endif
