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
/* Attributes a preprocessing pass is MEASURED to feed correctly.  The
 * register rule above is measured for attributes 1 and 2; a third draws
 * geometry from neither its own value nor a decoy, both in the userspace
 * probe (docs/parhelia-m9-native-vertex-isa.md section 2) and through this
 * driver (m9-mesa-vs attr3, 2026-09-17), so the draw path stops at two
 * until the third attribute's register is decoded.  The backend still
 * compiles four, which is what the offline corpus covers. */
#define M9_VS_HW_ATTRIBUTES  2u
#define M9_VS_MAX_DECL       16u
/* One output target beyond the position carries a varying; the register to
 * target rule for any other target set is undecoded. */
#define M9_VS_MAX_VARYINGS   1u
/* Constant pairs 9..21 of the span belong to the appended tail, which names
 * them by absolute index; user pairs are 0..8 and 22 upward. */
#define M9_VS_TAIL_FIRST_PAIR 9u
#define M9_VS_TAIL_LAST_PAIR  21u
#define M9_VS_TAIL_PAIRS      (M9_VS_TAIL_LAST_PAIR + 1u - M9_VS_TAIL_FIRST_PAIR)
#define M9_VS_MAX_CONSTS      0x7fu
/* Offset of a constant scalar the driver resolves to 0.0. */
#define M9_VS_UNIFORM_ZERO    0xffffffffu

/* The tail reads the clip position from r00/r01 and the varying from r02;
 * r13 is an output register the head writes itself, as the vendor does. */
#define REG_POS_XY 0x00u
#define REG_POS_ZW 0x01u
#define REG_VAR_XY 0x02u
#define REG_VAR_ZW 0x13u
/* The partial dot names register 3 in its c field and nothing writes it. */
#define REG_DOT_C  0x03u
#define REG_ATTR0  0x46u
#define M9_VS_REGS 0x60u

/* Slots 14..38 of the captured program, then the stop pair. */
#define M9_VS_TAIL_FIRST_SLOT 14u
#define M9_VS_TAIL_SLOTS      25u

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
#define VS_DOT_P    0xbe000d00u  /* CAPTURED s5:  partial, d = b . k       */
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

enum m9_vs_const_kind {
   M9_VS_CONST_LITERAL,
   M9_VS_CONST_UNIFORM,    /* two constant-buffer components */
};

struct m9_vs_const {
   enum m9_vs_const_kind kind;
   float value[2];
   uint32_t offset[2];     /* constant buffer 0 byte offsets */
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
};

struct m9_vs_program {
   struct util_dynarray slots;    /* uint32_t W0, W1 of the compiled head */
   struct util_dynarray consts;   /* struct m9_vs_const, user pairs in order */
   struct m9_vs_attr attrs[M9_VS_MAX_ATTRIBUTES];
   unsigned attr_count;
   uint64_t inputs_read;          /* gl_vert_attrib mask of the shader */
   uint32_t record_bytes;
   bool has_varying;
   unsigned varying_location;     /* gl_varying_slot of the one varying */
   unsigned varying_components;
};

/* Returns false with a static reason when the shader is not covered. */
bool m9_vs_compile(struct nir_shader *nir, struct m9_vs_program *program,
                   const char **reason);
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
/* Writes head, tail and stop pair; returns the dwords written, 0 if max is
 * too small. */
unsigned m9_vs_emit(const struct m9_vs_program *program, uint32_t *out,
                    unsigned max_dwords);
/* Assembles the c2 image the two-pass job binds: fragment slots first, the
 * compiled vertex program behind them, as m9_fs_variant_upload lays it out
 * for the captured program.  Reports the vertex entry slot, which is what
 * index c3 carries.  Returns the dwords written, 0 if max is too small. */
unsigned m9_vs_c2_image(const struct m9_vs_program *program,
                        const uint32_t *fs_slots, unsigned fs_slot_count,
                        uint32_t *out, unsigned max_dwords,
                        unsigned *vertex_entry);

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
