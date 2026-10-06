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
   /* Index ef bits 31:29 are the same AA mode field on the Windows side:
    * the vendor lowerer places compiled state +0x04 bits 6:4 there
    * (STATIC, MxMD.dll 0x18233e shl 0x19 into surface shadow +0xb8, merged
    * at 0xfaef6 and emitted at 0xfc1ed in the header 0x45d7eff0 packet --
    * indices f0, ef, d7, which is the packet this driver already emits).
    * The vendor lowers the ad/af pair only when that field is neither 0 nor
    * 3 (0x18263d, 0x182646), so on this driver, which leaves it at 0, the
    * pair the engine receives is one the vendor would never have sent.
    * Index f0 bit 15 and bit 24 come from the same compiled dword. */
   int ef_mode;
   bool f0_bit15, f0_bit24;
   /* The fragment store probe.  bind_index is the command index the store's
    * address is written to; the vendor writes a1, a3 and a4 under its only
    * packet-name string, RESET_FRAG_PTR, and no captured stream carries any
    * of them, so which one binds the store is a measurement, not a fact.
    * A negative index binds nothing and allocates nothing. */
   /* Bind the store the way the vendor's emitter does: index bc takes the
    * main buffer's address and bd the sixteenth-size one (STATIC, MxMD.dll
    * 0xfbd8e, header 0x4545bdbc, relocation kinds 9 and 0x0a; the same two
    * indices and kinds appear in the M-series Linux driver at 0x4b09a3). */
   bool store_bind;
   /* The w x h companion surface the vendor allocates beside the store and
    * binds at b8/b9 in place of the ordinary depth surface (STATIC, MxMD.dll
    * 0xaab42 fills the same shadow slot the b8/b9 packet at 0xfbbbf reads).
    * A negative value keeps the depth attachment; otherwise this is the
    * descriptor format code the companion is bound with, which no static
    * evidence names and which the probe sweeps. */
   int companion_format;
   int bind_index;
   /* A surface on this engine is an address and a descriptor at the next
    * index; a negative value writes the address alone. */
   int64_t bind_desc;
   bool bind_reset;   /* write a1 = a3 = a4 = 0 beside it */
   /* Fill the store with a traceable pattern and report which dwords the
    * engine changed.  Every dword carries its own index, so a write is
    * identifiable wherever it lands, and the store is kept in the mapped
    * window so the CPU can read it back. */
   bool store_check;
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
/* Dwords the RESET_FRAG_PTR block costs: e6 twice, then a1/a3/a4. */
#define M9_FAA_RESET_DWORDS 7u
/* Dwords a store binding costs: one header and one address. */
#define M9_FAA_BIND_DWORDS 3u
/* Dwords the bc/bd store pair costs: one header and two addresses. */
#define M9_FAA_STORE_DWORDS 3u
/* Dwords and relocations this state adds to one raster pass. */
unsigned m9_faa_raster_dwords(const struct m9_faa *faa);
unsigned m9_faa_raster_relocs(const struct m9_faa *faa);
/* The pattern dword of a store word, so a write anywhere is traceable. */
uint32_t m9_faa_store_pattern(uint64_t dword_index);

/*
 * The multisample fragment store is a buffer group of its own, not an
 * enlargement of the colour allocation (STATIC, MxMD.dll 0xac7e0: the main
 * buffer is width * height * 0x158 scaled by 0.385 and divided by eight,
 * capped at 32 MiB, and a second buffer is a sixteenth of it).  That is
 * about 16.55 bytes a pixel.
 */
#define M9_FAA_FRAGMENT_CAP_BYTES 0x2000000u
uint64_t m9_faa_fragment_bytes(uint32_t width, uint32_t height);
uint64_t m9_faa_fragment_aux_bytes(uint32_t width, uint32_t height);

/* MSerX profile49, hardware format3d, admitted sample selector2. The
 * companion has its own alignment, small-surface state and end guard;
 * ordinary 32-bit depth layout cannot describe it. Recovered on M9128
 * with shared f04 fields2/1; allocation does not establish FAA rendering. */
struct m9_faa_companion_layout {
   uint64_t size;
   uint32_t row_span, allocation_height, descriptor;
};
bool m9_faa_companion_layout(uint32_t width, uint32_t height,
                             struct m9_faa_companion_layout *layout);

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
/* Every index this state edits rather than replaces, in one object: a
 * caller that fills it cannot wire one index and forget another, which is
 * how ef and f0 came to be written as zero. */
struct m9_faa_edits {
   uint32_t ae_and, ae_or;
   uint32_t ef_and, ef_or;
   uint32_t f0_and, f0_or;
};

void m9_faa_edits(const struct m9_faa *faa, struct m9_faa_edits *edits);

#endif
