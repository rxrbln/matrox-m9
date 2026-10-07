/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * NIR to native M9148 vertex program backend.  The compiled program replaces
 * the transform head of the captured vendor program; its tail -- the
 * reciprocal of w, the perspective divide, the viewport and the clip code --
 * is appended verbatim, because one of its instructions is undecoded and it
 * is the same 25 slots in every capture
 * (docs/parhelia-m9-native-vertex-isa.md).
 */
#ifndef M9_VS_COMPILE_H
#define M9_VS_COMPILE_H

#include <stdbool.h>
#include <stdint.h>

#include "util/u_dynarray.h"

struct nir_shader;
struct nir_shader_compiler_options;

/* The options the vertex stage's NIR is produced under; the screen names
 * them and the offline gate compiles against the same struct. */
extern const struct nir_shader_compiler_options m9_vs_nir_options;

/* An index-23 word is dword << 24 | attr << 20 | stream << 16 | selectors:
 * bits 19:16 name the source stream, which the raster declaration builder
 * already writes and this backend leaves 0 for its one interleaved record.
 * Attribute a is register pair 0x46 + 8 * (a - 1), CAPTURED for a = 1..4
 * (the vendor's fs-* preprocessing passes declare four and read through
 * 0x5e/0x5f).  The vendor's own numbering skips 5..8 -- those are the four
 * texture coordinate sets -- and resumes at 9, whose register 0x46 + 64
 * does not fit the seven-bit operand field, so the rule stops at four.
 * Index 22 bits 11:8 carry the word count minus one, so a pass takes
 * sixteen words; nine, four attributes' worth, is well inside it. */
#define M9_VS_MAX_ATTRIBUTES 4u
/* Attributes a preprocessing pass is MEASURED to feed correctly.  A third
 * used to draw from neither its own value nor a decoy; the register rule
 * above was right all along and the declaration was not, because the pass
 * stated a fixed count of two in index 0x88 bits 27:24.  With that count
 * driven from the declaration all three attributes read exactly.  The
 * fourth reads exactly too (MEASURED on the M9128: attr4-r5e 100% of every
 * triangle interior, and the three lower registers each hold their own
 * attribute's decoy), so the draw path reaches the four the register rule
 * and the four-bit declaration field allow.
 * docs/parhelia-m9-vertex-output-target-re.md */
#define M9_VS_HW_ATTRIBUTES  4u
#define M9_VS_MAX_DECL       16u
/* Output pair (0x11 + 4j, 0x13 + 4j) goes to the target bound at index
 * 0xb0 + 2j and register 0x21 to 0xb8, with index 0xef bits 19/21/23
 * enabling targets 1/2/3 and bit 15 the clip code (CAPTURED, 310 of 310
 * preprocessing passes: docs/parhelia-m9-vertex-output-target-re.md).  The
 * appended tail takes three of the four targets -- 0 the varying, 1 the clip
 * position, 2 the viewport position.  Target 3 cannot be the fourth: a pass
 * that enables it and the clip code together stalls the engine, which is
 * why no captured pass carries both (MEASURED, five configurations).  What
 * the raster stage does not need is the **clip position**, attribute 13 --
 * dropping its declaration renders the same pixels (MEASURED) -- so target
 * 1 is the one the fused pass has spare, and the two tail slots that write
 * it are pointed at the tail's own scratch instead.  The fused pass then
 * carries two varyings in one draw, in the captured three-target shape with
 * ef and index 88 byte for byte unchanged.  A program with more is compiled
 * a second time into a tail-free head that writes varyings 2..4, and
 * m9_draw.c runs that head as a preprocessing draw of its own ahead of the
 * fused one. */
#define M9_VS_MAX_VARYINGS   5u
/* Varyings the fused pass carries itself, on output targets 0 and 1. */
#define M9_VS_FUSED_VARYINGS 2u
/* The output target the fused pass's second varying takes, the clip
 * position's. */
#define M9_VS_FUSED_PAIR     1u
/* Varyings the tail-free second head carries, at its own targets 0 upward. */
#define M9_VS_EXTRA_VARYINGS (M9_VS_MAX_VARYINGS - M9_VS_FUSED_VARYINGS)
/* Compiled programs one vertex shader keeps, one an output layout: a shader
 * drawn with many fragment variants asks for one per distinct stream set. */
#define M9_VS_MAX_VARIANTS 64u
/* Specialisations on distinct inlinable uniform value sets one shader keeps.
 * A further set falls back to the draw module rather than evicting a program
 * a queued job still names. */
#define M9_VS_MAX_SPECS 16u
/* Constant pairs 9..21 of the span belong to the appended tail, which names
 * them by absolute index; user pairs are 0..8 and 22 upward. */
#define M9_VS_TAIL_FIRST_PAIR 9u
#define M9_VS_TAIL_LAST_PAIR  21u
#define M9_VS_TAIL_PAIRS      (M9_VS_TAIL_LAST_PAIR + 1u - M9_VS_TAIL_FIRST_PAIR)
#define M9_VS_MAX_CONSTS      0x7fu
/* Constant pairs a preprocessing pass may carry.  MEASURED on the M9128: a
 * 63-pair span (vsconst:21) renders every pixel exactly and a 65-pair one
 * fails all 76800 and stops the engine -- twice, on two programs of
 * different length -- while a 5161-slot program with 27 pairs is exact, so
 * the span and not the length is what the ceiling belongs to.  It is the
 * span the job hands the engine and not the constants the program names:
 * an 81-slot program naming 27 pairs is exact with the span padded to 63
 * and stops the engine at 65 (M9X_VS_CONST_PAD, which changes index 8c's
 * count and 8e's end address and nothing else).  Index 8c carries the count
 * in eight bits and the span may run to pair 93 before it reaches the
 * linked record, so neither field nor layout is what stops it.
 * docs/parhelia-m9-vertex-isa-gaps.md */
#define M9_VS_MAX_CONST_PAIRS 63u
/* Offset of a constant scalar the driver resolves to 0.0. */
#define M9_VS_UNIFORM_ZERO    0xffffffffu

/* The tail reads the clip position from r00/r01 and the varying from r02.
 * r00, r01 and r02 are ordinary temporaries -- no bound target ever receives
 * them; the tail's own "r11 = mov r02" is what carries r02 to target 0.
 * r13 is target 0's high half, which the head writes itself because the tail
 * does not. */
#define REG_POS_XY 0x00u
#define REG_POS_ZW 0x01u
#define REG_VAR_XY 0x02u
#define REG_VAR_ZW 0x13u
/* Output pair j of a preprocessing pass, low register then high; the fused
 * pass's varying is pair 0, whose low half the tail writes from r02. */
#define REG_OUT_LO(j) (0x11u + 4u * (j))
#define REG_OUT_HI(j) (0x13u + 4u * (j))
/* Route k of a record-fed draw arrives in this register pair, components 0
 * and 1 in the low register and 2 and 3 in the high one (CAPTURED,
 * docs/parhelia-m9-varying-transport-re.md section 3). */
#define M9_VS_ROUTE_LO(k) (0x00u + 4u * (k))
#define M9_VS_ROUTE_HI(k) (0x02u + 4u * (k))
/* Routes a preprocessing draw can bind, indices 0x74..0x77. */
#define M9_VS_MAX_ROUTES 4u
#define REG_ATTR0  0x46u
#define M9_VS_REGS 0x60u

/* Slots 14..38 of the captured program, then the stop pair. */
#define M9_VS_TAIL_FIRST_SLOT 14u
#define M9_VS_TAIL_SLOTS      25u
/* The tail's own scratch register and the two tail slots that write the
 * clip position into output pair 1.  Nothing reads those two destinations
 * again, and the scratch is written afresh at tail slot 10 before anything
 * reads it, so pointing them there frees the pair for a varying and leaves
 * index 88's pair mask exactly the captured 0xbf00. */
#define M9_VS_TAIL_SCRATCH   0x1cu
#define M9_VS_TAIL_CLIPPOS_LO 5u
#define M9_VS_TAIL_CLIPPOS_HI 7u

/* Instruction forms.  Bits 20:14 (the constant index) and 3:0 (the high
 * bits of the a operand) are clear; w1_make() fills them in. */
#define VS_MOV      0xa2803f00u  /* CAPTURED s16: d = c[swz << 4]          */
#define VS_MOVK     0xa2803300u  /* CAPTURED s29: d = k[kswz << 4]         */
#define VS_MUL_RK   0xa0200010u  /* CAPTURED s30: b[swz<<6] * k[kswz<<8]   */
#define VS_MUL_RR   0xac200000u  /* CAPTURED s23: a[swz<<8] * b[swz<<6]    */
#define VS_MAD_RKK  0xb0400010u  /* CAPTURED s26: b*k + constant pair in c */
#define VS_MAD_RKR  0xa0400c00u  /* fragment L_MAD: b*k + c[swz << 4]      */
#define VS_ADD_RR   0xac800c00u  /* fragment L_ADD_RR, negate at bits 7:6  */
#define VS_NEG      0xa2a03f00u
#define VS_FRACT    0xa2800f00u
#define VS_FLOOR    0xa2800cc0u
#define VS_CEIL     0xa2800c00u
#define VS_MINMAX   0xa3200d50u  /* class 2 minimum, 3 maximum             */
#define VS_CMP      0x90000010u  /* CAPTURED s14: constant pair in c, e=5  */
#define VS_SEL      0xaf400c00u
#define VS_DOT_P    0xbe000d00u  /* CAPTURED s5:  d = b . k - kc.s0 (c pair) */
#define VS_DOT_RR   0xada00c00u  /* CAPTURED vs-math s6: d.s0 = a . b + k.s0, c=a */
/* Tail s36 carries this form with the program-finish bit (W1 bit 30).
 * That marker belongs only four slots before the end, never in the head. */
#define VS_DOT_C    0xadc00c00u  /* CAPTURED s7:  d = b . k + c[swz << 4]  */
#define VS_RCP_SEED 0xae203000u  /* CAPTURED s15                           */
#define VS_RCP_RES  0x8c26ac10u  /* CAPTURED s17, class 5, e already set   */
#define VS_RCP_STEP 0xa2600f00u  /* CAPTURED s18                           */
/* The vertex unit's own square-root chain was not decoded; these are the
 * fragment unit's measured words. */
#define VS_RSQ_SHUF 0xa28ffc00u
#define VS_RSQ_SEED 0x9e26b090u
#define VS_RSQ_MUL  0xac2fcd00u
#define VS_RSQ_COPY 0xa28fff30u
#define VS_RSQ_STEP 0xa2603f00u
/* exp2.  CAPTURED in the vendor's own vertex programs (ff-fog-exp and
 * ff-fog-exp2): the seed splits the argument, three slots evaluate a
 * minimax polynomial whose coefficient pairs are the fragment unit's to the
 * bit, and a multiply applies the split-off scale.  Unlike the forms above
 * these words carry their own bits 20:14, so w1_make() is handed 0 for the
 * three that name no user constant. */
#define VS_EXP2_SEED 0x9e851d40u  /* argument select at bits 5:4 */
#define VS_EXP2_POLY 0x982082d0u  /* first coefficient pair in the c field */
#define VS_EXP2_STEP 0xa2600cf0u  /* coefficient pair in e, writes scalar 1 */
#define VS_EXP2_SCALE 0xac2fc0c0u
/* Recovered through the vendor's vertex frontend and actual packer;
 * reads a.scalar0, writes exponent/fraction. */
#define VS_LOG2_SEED 0x96852ec0u

enum m9_vs_const_kind {
   M9_VS_CONST_LITERAL,
   M9_VS_CONST_UNIFORM,    /* two constant-buffer components */
   /* The coefficients of a coordinate stream, which the driver resolves per
    * job from the sampler's own transform and the addressed span. */
   M9_VS_CONST_COORD,
};

/* Which half of a coordinate stream's multiply-add a constant pair is. */
#define M9_VS_COORD_SCALE 0u
#define M9_VS_COORD_BIAS  1u

/* Synthetic attribute semantic of a coordinate stream's coefficient slot,
 * past every gl_vert_attrib, so the register model tells it from a real
 * input.  Stream j takes M9_VS_COEFF_ATTRIB + j. */
#define M9_VS_COEFF_ATTRIB 0x100u

struct m9_vs_const {
   enum m9_vs_const_kind kind;
   float value[2];
   uint32_t offset[2];     /* constant buffer 0 byte offsets */
   uint8_t coord_stream;   /* M9_VS_CONST_COORD: the stream it belongs to */
   uint8_t coord_part;     /* M9_VS_COORD_SCALE or M9_VS_COORD_BIAS */
};

/* One stream a compiled program writes to an output target.  A varying
 * stream carries four components of the varying, the shape the raster stage
 * interpolates as an ordinary input.  A coordinate stream carries the two
 * components a sampler reads, taken from wherever in the varying they sit
 * and put through the multiply-add that used to run per vertex on the CPU:
 * the sampler's own affine transform and the render target's addressed
 * span.  Its coefficients are constant pairs the driver fills per job. */
struct m9_vs_output {
   uint8_t location;       /* gl_varying_slot the value comes from */
   uint8_t component;      /* first of the pair; coordinate streams only */
   /* Which coordinate stream of which raster pass this is.  Two streams may
    * take the same pair of the same varying and differ only in the
    * transform the sampler folds into it, and every pass numbers its own
    * coordinate streams from zero, so the pair is what names one. */
   uint8_t coord;
   uint8_t pass;
   bool coordinate;
   /* Take the multiply-add's coefficients from a source-record slot the
    * driver fills per draw instead of from the per-job constant span.  A
    * draw that samples a differently addressed target then appends records
    * to the pending job rather than ending it. */
   bool coeff_record;
   /* An identity coordinate pair in the upper half of this same stream.
    * It comes from its own varying, rather than duplicating the low pair. */
   bool paired;
   uint8_t pair_location, pair_component, pair_coord, pair_pass;
};

struct m9_vs_outputs {
   struct m9_vs_output stream[M9_VS_MAX_VARYINGS];
   unsigned count;
};

/* One declared attribute: four selectors over one element of the source
 * record, which is the index-23 word pair.  Each slot selects a record
 * component; the record always carries four, the GL default (0, 0, 0, 1)
 * filling in beyond what the shader reads. */
struct m9_vs_attr {
   unsigned location;      /* gl_vert_attrib semantic of the input */
   unsigned driver_location;  /* its index among the shader's inputs, which is
                               * the pipe_vertex_element the draw binds */
   unsigned offset;        /* dword offset of the element in the record */
   unsigned components;    /* components the shader reads; the rest default */
   uint8_t select[4];
   /* A slot no vertex element feeds: the driver writes its four floats into
    * every record of the draw.  coeff_stream names the coordinate stream
    * whose scale (components 0, 1) and bias (2, 3) it carries. */
   bool constant;
   uint8_t coeff_stream;
};

struct m9_vs_program {
   struct util_dynarray slots;    /* uint32_t W0, W1 of the compiled head */
   struct util_dynarray consts;   /* struct m9_vs_const, user pairs in order */
   struct m9_vs_attr attrs[M9_VS_MAX_ATTRIBUTES];
   unsigned attr_count;
   /* Bounded source-fed sampler input: one direct XY vertex attribute.
    * The runtime admits only level-zero point 2-D storage until a wider
    * vertex sampler/coordinate producer contract is qualified. */
   unsigned texture_count;
   unsigned texture_sampler;
   unsigned texture_location;
   uint64_t inputs_read;          /* gl_vert_attrib mask of the shader */
   uint32_t record_bytes;
   /* Past two streams: a source-fed first producer and source-fed extra
    * continuation pack every output. Gather extracts the extra streams;
    * slots is then a route-fed final head plus the appended epilogue. */
   struct util_dynarray extra;    /* uint32_t W0, W1 of the second head */
   /* The route-fed draw that de-interleaves the head's record into the
    * stream views the raster stage reads, one register-pair move a
    * varying (CAPTURED, vs-varyings-4's c3=27). */
   struct util_dynarray gather;
   /* Source-fed first draw: clip position and the two primary streams,
    * written as fields 0..2 of one retained packed record. */
   struct util_dynarray producer;
   /* Output streams, one an output target.  varying_count is their number,
    * whatever kind they are. */
   unsigned varying_count;
   unsigned varying_location[M9_VS_MAX_VARYINGS];   /* gl_varying_slot */
   unsigned varying_components[M9_VS_MAX_VARYINGS];
   struct m9_vs_outputs outputs;
};

/* The program's index for a varying slot, or -1 when it writes none.  Index
 * 0 lands on transient view 0 and index k on the extra pass's target k - 1. */
int m9_vs_varying_index(const struct m9_vs_program *program,
                        unsigned location);

/* The stream index a raster input arrives on, or -1 when the program writes
 * no such stream: a varying is named by its location and a coordinate by
 * the raster pass it belongs to and that pass's own stream index.  Index 0
 * lands on transient view 0 and index k on the extra pass's target k - 1. */
int m9_vs_stream_index(const struct m9_vs_program *program, unsigned location,
                       unsigned pass, unsigned coord, bool coordinate);

/* Returns false with a static reason when the shader is not covered.
 * outputs names the streams the program must write; NULL asks for one
 * varying stream per varying the shader stores, in store order. */
bool m9_vs_compile(struct nir_shader *nir,
                   const struct m9_vs_outputs *outputs,
                   struct m9_vs_program *program, const char **reason);
/* Compile normally, then try coefficient vectorization on a clone only if
 * the result exceeds max_const_pairs while fitting max_slots. An unsuccessful
 * retry preserves the original program for the caller's ordinary admission
 * checks. No NIR or resource pointer is retained by a compiled program. */
bool m9_vs_compile_bounded(struct nir_shader *nir,
                           const struct m9_vs_outputs *outputs,
                           struct m9_vs_program *program, const char **reason,
                           unsigned max_slots, unsigned max_const_pairs);

/* The caller owns a unique specialization clone through *nir. On adoption,
 * exchange that graph with its bounded vector/constant trial; admitted and
 * failed trials preserve the ordinary source/program ownership. */
bool m9_vs_compile_bounded_source(struct nir_shader **nir,
                                   const struct m9_vs_outputs *outputs,
                                   struct m9_vs_program *program,
                                   const char **reason, unsigned max_slots,
                                   unsigned max_const_pairs);

void m9_vs_program_fini(struct m9_vs_program *program);

/* The vertex element feeding attribute i.  The state tracker compacts the
 * inputs the shader reads and builds one element per input in that order
 * (st_atom_array: bitcount of inputs_read below the attribute), so this is
 * the index rather than anything the driver may have assigned. */
unsigned m9_vs_attr_element(const struct m9_vs_program *program, unsigned i);
/* Constant span pair holding user constant index i. */
unsigned m9_vs_const_pair(unsigned index);
/* Pairs the whole span needs, the appended tail included. */
unsigned m9_vs_const_count(const struct m9_vs_program *program);
/* Instruction slots of the whole program, tail and stop pair included. */
unsigned m9_vs_slot_count(const struct m9_vs_program *program);
/* Instruction slots of the second head, stop pair included; 0 when the
 * program has at most one varying and needs no second preprocessing draw. */
unsigned m9_vs_extra_slot_count(const struct m9_vs_program *program);
/* Instruction slots of the gather draw, stop pair included; 0 with no
 * second head. */
unsigned m9_vs_gather_slot_count(const struct m9_vs_program *program);
unsigned m9_vs_producer_slot_count(const struct m9_vs_program *program);
/* Register-pair availability of a source-fed, tail-free output producer. */
uint16_t m9_vs_source_mask(const struct m9_vs_program *program, bool primary);
uint16_t m9_vs_fused_source_mask(const struct m9_vs_program *p);
/* Writes head, tail and stop pair; returns the dwords written, 0 if max is
 * too small.  fused_pair moves the tail's scratch off target 3's low half,
 * which the caller asks for exactly when it binds that target. */
unsigned m9_vs_emit(const struct m9_vs_program *program, uint32_t *out,
                    unsigned max_dwords, bool fused_pair);
/* Assembles the C2 image: fragment, final vertex, continuation, gather,
 * first producer. Reports each native entry independently; execution order
 * comes from the live preprocessing chain, not image order. Returns the
 * dwords written, 0 if max is too small. */
unsigned m9_vs_c2_image(const struct m9_vs_program *program,
                        const uint32_t *fs_slots, unsigned fs_slot_count,
                        uint32_t *out, unsigned max_dwords, bool fused_pair,
                        unsigned *vertex_entry, unsigned *extra_entry,
                        unsigned *gather_entry, unsigned *producer_entry);

/* Resolves the user constants into the preprocessing span, pair p at
 * out[2 * p], leaving pairs 9..21 untouched for the caller's viewport and
 * clip words.  Returns the pairs the span needs. */
unsigned m9_vs_constants(const struct m9_vs_program *program,
                         const float *uniforms, unsigned uniform_floats,
                         uint32_t *out, unsigned max_dwords);
/* The same placement for constants the caller has already resolved: two
 * floats per user constant, in program order.  Returns the pairs the span
 * needs, 0 when the span or the values do not fit. */
unsigned m9_vs_const_span(const struct m9_vs_program *program,
                          const float *values, unsigned value_floats,
                          uint32_t *out, unsigned max_dwords);

/* Writes the index-23 source declaration; returns the word count. */
unsigned m9_vs_decl_words(const struct m9_vs_program *program, uint32_t *out,
                          unsigned max_words);

#endif
