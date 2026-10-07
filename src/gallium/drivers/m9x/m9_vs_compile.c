/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * NIR to native M9148 vertex programs.
 *
 * A native register holds two scalars and every instruction carries a
 * destination scalar mask, so a vector value occupies one register per
 * component pair -- the model the fragment backend already uses, because the
 * two units share the instruction encoding field for field
 * (docs/parhelia-m9-native-vertex-isa.md section 5).
 *
 * What is compiled here is the head of the program: it reads the declared
 * attribute registers, computes the clip position into r00/r01 and the first
 * varying into r02/r13.  A shader with more varyings is compiled a second
 * time into a tail-free head that writes varyings 1..4 to the four output
 * pairs, which m9_draw.c runs as a preprocessing draw of its own ahead of
 * the fused one.  m9_vs_emit() appends the vendor tail -- the
 * reciprocal of w, the perspective divide, the viewport scale and bias and
 * the clip code -- verbatim, because one of its instructions is undecoded
 * and it is the same 25 slots in every capture.  The tail names constant
 * pairs 9..21 by absolute index, so user constants take pairs 0..8 and 22
 * upward.
 *
 * Instruction words marked CAPTURED below are reconstructed byte for byte
 * from m9_passthrough_c2_program, or -- the exp2 chain -- from the vendor's
 * ff-fog-exp vertex programs; the rest are the fragment unit's own measured
 * forms, which the shared encoding says the vertex unit takes too.
 *
 * The vendor vertex frontend also emits the logarithm seed.  Its compact
 * polynomial instructions remain undecoded, so logarithm uses ordinary
 * multiply/add forms with the vendor coefficients instead (see
 * tests/native-vs-logarithm.md for the evidence and validation boundary).
 */
#include "m9_vs_compile.h"
#include "m9_draw_params.h"

#include "m9_passthrough.h"
#include "m9_specialise.h"

#define NONE       0x7fu
#define STOP_W0    0xffff801cu
#define STOP_W1    0xa01fc00fu
#define FOLD_NEG   2

#include <assert.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"
#include "nir_schedule.h"
#include "util/u_debug.h"
#include "util/u_math.h"
#include "util/u_memory.h"

/* The NIR the vertex stage is handed, here rather than in the screen so the
 * offline gate compiles the very options the driver does. */
const nir_shader_compiler_options m9_vs_nir_options = {
   .lower_bitops = true,
   .lower_scmp = true,
   .lower_flrp32 = true,
   .lower_flrp64 = true,
   .lower_fsat = true,
   .lower_bitfield_insert = true,
   .lower_bitfield_extract = true,
   .lower_fdph = true,
   .lower_fmod = true,
   .lower_hadd = true,
   .lower_uadd_sat = true,
   .lower_usub_sat = true,
   .lower_iadd_sat = true,
   .lower_pack_snorm_2x16 = true,
   .lower_pack_snorm_4x8 = true,
   .lower_pack_unorm_2x16 = true,
   .lower_pack_unorm_4x8 = true,
   .lower_pack_half_2x16 = true,
   .lower_pack_split = true,
   .lower_unpack_snorm_2x16 = true,
   .lower_unpack_snorm_4x8 = true,
   .lower_unpack_unorm_2x16 = true,
   .lower_unpack_unorm_4x8 = true,
   .lower_unpack_half_2x16 = true,
   .lower_extract_byte = true,
   .lower_extract_word = true,
   .lower_uadd_carry = true,
   .lower_usub_borrow = true,
   .lower_mul_2x32_64 = true,
   .lower_ifind_msb = true,
   .max_unroll_iterations = 32,
   .lower_uniforms_to_ubo = true,
   .lower_device_index_to_zero = true,
   .support_indirect_inputs = (uint8_t)BITFIELD_MASK(MESA_SHADER_STAGES),
   .support_indirect_outputs = (uint8_t)BITFIELD_MASK(MESA_SHADER_STAGES),
   .no_integers = true,
   .has_fused_comp_and_csel = true,
};

enum held_kind {
   HELD_NONE,
   HELD_INPUT,     /* a component of a vertex attribute, or a declared
                    * literal slot of one */
   HELD_DEF,       /* a component of a NIR definition */
   HELD_LIT,
   HELD_UNIFORM,   /* a constant buffer component, by byte offset */
   HELD_TEMP,      /* an intermediate of the instruction being emitted */
};

struct held {
   enum held_kind kind;
   unsigned a, b;   /* INPUT: location, component; DEF: index, component;
                     * UNIFORM: byte offset; TEMP: sequence number */
   float lit;
};

struct kkey {
   enum m9_vs_const_kind kind;
   float v[2];
   uint32_t off[2];
   uint8_t coord_stream, coord_part;
   bool pair;
};

struct const_seed {
   const struct m9_vs_program *program;
   const bool *identity;
};

struct ctx {
   bool compact;
   bool faa;
   bool sampled_position, unsampled_stage;
   bool *identity_keys;
   nir_shader *nir;
   struct m9_vs_program *p;
   struct util_dynarray *out;   /* the head being emitted */
   struct util_dynarray keys;
   struct held holds[M9_VS_REGS][2];
   unsigned *last_use;      /* per definition, the instruction that ends it */
   uint32_t *head_live;     /* component demand of the split head's pure ALU */
   int8_t *fold;            /* product's constant side, or FOLD_NEG */
   unsigned def_count;
   nir_tex_instr *texture[2];
   unsigned index;          /* instruction being emitted */
   /* Lanes whose result the output registers take directly. */
   struct out_pin {
      unsigned def, lane, reg;
   } pins[2 * M9_VS_MAX_ROUTES];
   unsigned pin_count;
   bool packed_outputs;    /* EE19 source targets publish high before low */
   unsigned temp_seq;
   /* Temporaries of earlier instructions are dead: only one raised past
    * this mark belongs to the instruction being emitted. */
   unsigned temp_base;
   bool temp_live;
   /* Output registers of the head being emitted, kept out of the allocation
    * pool: the tail-free head writes its four pairs at the end and nothing
    * may take one back as a temporary. */
   uint8_t reserved[M9_VS_REGS];
   /* Registers handed out while the current instruction is being emitted.
    * An operand materialised into one holds a value the liveness model does
    * not track -- an attribute component, a literal, a uniform -- so without
    * this the next allocation of the same instruction takes the register
    * back and both operands read the second value (MEASURED offline: the
    * (z, z) lane of any three-component add read its second source twice). */
   uint8_t busy[M9_VS_REGS];
   const char *reason;
};

static bool
fail_at(struct ctx *c, const char *reason)
{
   if (!c->reason)
      c->reason = reason;
   return false;
}
#define fail(c, r) fail_at((c), (r))

static uint32_t
w0_make(unsigned a, unsigned b, unsigned cf, unsigned d, unsigned cls,
        unsigned op0)
{
   return (a & 7u) << 29 | (b & 0x7fu) << 22 | (cf & 0x7fu) << 15 |
          (d & 0x7fu) << 8 | (cls & 7u) << 5 | (op0 & 0x1fu);
}

static uint32_t
w1_make(uint32_t base, unsigned e, unsigned a)
{
   return base | (e & 0x7fu) << 14 | (a & 0x7fu) >> 3;
}

static void compile_gather(struct m9_vs_program *p, unsigned varyings);

static void
emit(struct ctx *c, uint32_t w0, uint32_t w1)
{
   util_dynarray_append_typed(c->out, uint32_t, w0);
   util_dynarray_append_typed(c->out, uint32_t, w1);
}

/* Constants */

static int
key_add(struct ctx *c, const struct kkey *k)
{
   util_dynarray_foreach(&c->keys, struct kkey, it) {
      if (!memcmp(it, k, sizeof(*k)))
         return it - (struct kkey *)c->keys.data;
   }
   if (util_dynarray_num_elements(&c->keys, struct kkey) >= M9_VS_MAX_CONSTS)
      return -1;
   util_dynarray_append_typed(&c->keys, struct kkey, *k);
   return util_dynarray_num_elements(&c->keys, struct kkey) - 1;
}

static int
key_literal(struct ctx *c, float a, float b)
{
   struct kkey k;

   memset(&k, 0, sizeof(k));
   k.kind = M9_VS_CONST_LITERAL;
   k.v[0] = a;
   k.v[1] = b;
   k.pair = a != b;
   return key_add(c, &k);
}

static int
key_uniform(struct ctx *c, uint32_t off0, uint32_t off1)
{
   struct kkey k;

   memset(&k, 0, sizeof(k));
   k.kind = M9_VS_CONST_UNIFORM;
   k.off[0] = off0;
   k.off[1] = off1;
   k.pair = off0 != off1;
   return key_add(c, &k);
}

/* One coefficient pair of a coordinate stream: the driver resolves its two
 * floats per job from the sampler transform and the addressed span. */
static int
key_coord(struct ctx *c, unsigned stream, unsigned part)
{
   struct kkey k;

   memset(&k, 0, sizeof(k));
   k.kind = M9_VS_CONST_COORD;
   k.coord_stream = stream;
   k.coord_part = part;
   k.pair = true;
   return key_add(c, &k);
}

/* A constant pair holding the two scalars, and the operand swizzle that
 * reads them; a single-valued pair is read with the first scalar. */
static bool
const_pair_values(struct ctx *c, const struct held *a, const struct held *b,
           int *key, unsigned *swz)
{
   if (a->kind == HELD_LIT && b->kind == HELD_LIT)
      *key = key_literal(c, a->lit, b->lit);
   else if (a->kind == HELD_UNIFORM && b->kind == HELD_UNIFORM)
      *key = key_uniform(c, a->a, b->a);
   /* A uniform paired with a zero literal keeps the pair addressable. */
   else if (a->kind == HELD_UNIFORM && b->kind == HELD_LIT && b->lit == 0.0f)
      *key = key_uniform(c, a->a, M9_VS_UNIFORM_ZERO);
   else if (a->kind == HELD_LIT && a->lit == 0.0f && b->kind == HELD_UNIFORM)
      *key = key_uniform(c, M9_VS_UNIFORM_ZERO, b->a);
   else
      return false;
   *swz = *key >= 0 && ((struct kkey *)c->keys.data)[*key].pair ? 1 : 0;
   return *key >= 0;
}

/* Track consumers without a proved high-lane swizzle separately. */
static bool
const_pair(struct ctx *c, const struct held *a, const struct held *b,
           int *key, unsigned *swz)
{
   bool ok = const_pair_values(c, a, b, key, swz);

   if (ok && c->identity_keys)
      c->identity_keys[*key] = true;
   return ok;
}

/* These callers encode a constant swizzle. Its captured zero form broadcasts
 * lane zero, so (x,x) can read an existing (x,y) without allocating another
 * pair. Compare literal bits: signed zero and NaN payloads are not aliases.
 * Identity-only DOT/MINMAX and MAD's constant addend keep const_pair(). */
static bool
const_pair_swizzled(struct ctx *c, const struct held *a, const struct held *b,
                    int *key, unsigned *swz)
{
   bool literal = a->kind == HELD_LIT && b->kind == HELD_LIT &&
                  !memcmp(&a->lit, &b->lit, sizeof(a->lit));
   bool uniform = a->kind == HELD_UNIFORM && b->kind == HELD_UNIFORM &&
                  a->a == b->a;

   if (literal || uniform) {
      util_dynarray_foreach(&c->keys, struct kkey, it) {
         if ((literal && it->kind == M9_VS_CONST_LITERAL &&
              !memcmp(&it->v[0], &a->lit, sizeof(a->lit))) ||
             (uniform && it->kind == M9_VS_CONST_UNIFORM &&
              it->off[0] == a->a)) {
            *key = it - (struct kkey *)c->keys.data;
            *swz = 0;
            return true;
         }
         /* Actual vertex MOVK/MUL_RK/MAD multiplier y controls encode
          * high-lane broadcast 3. Only the bounded seeded trial enables it;
          * literal and coordinate keys remain on their existing paths. */
         if (c->compact && uniform && it->kind == M9_VS_CONST_UNIFORM &&
             it->pair && it->off[1] == a->a) {
            *key = it - (struct kkey *)c->keys.data;
            *swz = 3;
            return true;
         }
      }
   }
   return const_pair_values(c, a, b, key, swz);
}

static bool
held_is_const(const struct held *h)
{
   return h->kind == HELD_LIT || h->kind == HELD_UNIFORM;
}

/* Registers */

/* The output-pair registers 0x11, 0x15, 0x17, 0x19, 0x1b and 0x1f read
 * back wrong as temporaries (MEASURED: a reciprocal seeded in any of them,
 * a scalar moved into one from a register, multiply-adds through 0x11 and
 * 0x15; the same in 0x12, 0x14, 0x16, 0x18, 0x1a, 0x1c..0x1e or 0x20 is
 * right).  0x13 was never in the pool. */
#define M9_VS_BAD_TEMPS (BITFIELD64_BIT(0x11) | BITFIELD64_BIT(0x15) | \
                         BITFIELD64_BIT(0x17) | BITFIELD64_BIT(0x19) | \
                         BITFIELD64_BIT(0x1b) | BITFIELD64_BIT(0x1f))

static bool
reg_in_pool(const struct ctx *c, unsigned r)
{
   return ((r >= 0x04u && r <= 0x12u) || (r >= 0x14u && r <= 0x21u)) &&
          !(M9_VS_BAD_TEMPS & BITFIELD64_BIT(r)) &&
          !c->reserved[r] && !c->busy[r];
}

static bool
held_live(struct ctx *c, const struct held *h)
{
   switch (h->kind) {
   case HELD_DEF:
      return h->a < c->def_count && c->last_use[h->a] >= c->index;
   case HELD_TEMP:
      return c->temp_live && h->a > c->temp_base;
   default:
      return false;
   }
}

static int
reg_alloc(struct ctx *c)
{
   for (unsigned r = 0; r < M9_VS_REGS; r++) {
      if (!reg_in_pool(c, r) || held_live(c, &c->holds[r][0]) ||
          held_live(c, &c->holds[r][1]))
         continue;
      memset(c->holds[r], 0, sizeof(c->holds[r]));
      c->busy[r] = 1;
      return r;
   }
   return -1;
}

static void
hold_def(struct ctx *c, unsigned reg, unsigned scalar, const struct held *h)
{
   c->holds[reg][scalar] = *h;
}

/* An allocation is only safe once something live occupies it, or the next
 * allocation of the same instruction hands out the same register. */
static int
reg_alloc_pinned(struct ctx *c, const struct held *h)
{
   int r = reg_alloc(c);

   if (r >= 0) {
      hold_def(c, r, 0, h);
      hold_def(c, r, 1, h);
   }
   return r;
}

static struct held
held_temp(struct ctx *c)
{
   struct held h;

   memset(&h, 0, sizeof(h));
   h.kind = HELD_TEMP;
   h.a = ++c->temp_seq;
   return h;
}

static bool
held_same(const struct held *a, const struct held *b)
{
   if (a->kind != b->kind || a->kind == HELD_NONE)
      return false;
   if (a->kind == HELD_LIT)
      return a->lit == b->lit;
   return a->a == b->a && (a->kind == HELD_UNIFORM || a->kind == HELD_TEMP ||
                           a->b == b->b);
}

/* NIR scalars */

static struct held
held_of(nir_scalar s)
{
   struct held h;
   nir_intrinsic_instr *intr;

   memset(&h, 0, sizeof(h));
   s = nir_scalar_chase_movs(s);
   if (nir_scalar_is_const(s)) {
      h.kind = HELD_LIT;
      h.lit = nir_scalar_as_float(s);
      return h;
   }
   if (nir_scalar_is_intrinsic(s)) {
      intr = nir_def_as_intrinsic(s.def);
      if (intr->intrinsic == nir_intrinsic_load_input) {
         h.kind = HELD_INPUT;
         h.a = nir_intrinsic_io_semantics(intr).location;
         h.b = nir_intrinsic_component(intr) + s.comp;
         return h;
      }
      if (intr->intrinsic == nir_intrinsic_load_ubo) {
         if (!nir_src_is_const(intr->src[1]))
            return h;
         h.kind = HELD_UNIFORM;
         h.a = (unsigned)nir_src_as_uint(intr->src[1]) + 4 * s.comp;
         return h;
      }
      if (intr->intrinsic == nir_intrinsic_load_ubo_vec4) {
         h.kind = HELD_UNIFORM;
         h.a = 16 * (nir_intrinsic_base(intr) +
                     (unsigned)nir_src_as_uint(intr->src[1])) +
               4 * (nir_intrinsic_component(intr) + s.comp);
         return h;
      }
      h.kind = HELD_NONE;
      return h;
   }
   h.kind = HELD_DEF;
   h.a = s.def->index;
   h.b = s.comp;
   return h;
}

static struct held
held_lit(float v)
{
   struct held h;

   memset(&h, 0, sizeof(h));
   h.kind = HELD_LIT;
   h.lit = v;
   return h;
}

/* Operands */

/* A register whose scalars sa and sb hold the two values, and the operand
 * swizzle reading them: bit 1 names the source scalar of result scalar 0. */
static bool
find_pair(struct ctx *c, const struct held *a, const struct held *b,
          bool identity_only, unsigned *reg, unsigned *swz)
{
   for (unsigned pass = 0; pass < 2; pass++) {
      for (unsigned r = 0; r < M9_VS_REGS; r++) {
         for (unsigned sa = 0; sa < 2; sa++) {
            for (unsigned sb = 0; sb < 2; sb++) {
               unsigned s = sa << 1 | sb;

               if (pass == 0 ? s != 1 : s == 1)
                  continue;
               if (identity_only && s != 1)
                  continue;
               if (!held_same(&c->holds[r][sa], a) ||
                   !held_same(&c->holds[r][sb], b))
                  continue;
               *reg = r;
               *swz = s;
               return true;
            }
         }
      }
   }
   return false;
}

/* A register whose first scalar holds the value, whatever the second is. */
static bool
find_first(struct ctx *c, const struct held *a, unsigned *reg)
{
   for (unsigned r = 0; r < M9_VS_REGS; r++) {
      if (held_same(&c->holds[r][0], a)) {
         *reg = r;
         return true;
      }
   }
   return false;
}

static void
emit_move(struct ctx *c, unsigned dst, unsigned op0, unsigned src,
          unsigned swz)
{
   emit(c, w0_make(NONE, NONE, src, dst, 0, op0),
        w1_make(VS_MOV | swz << 4, NONE, NONE));
}

static void
emit_move_const(struct ctx *c, unsigned dst, unsigned op0, unsigned e,
                unsigned kswz)
{
   emit(c, w0_make(NONE, NONE, NONE, dst, 0, op0),
        w1_make(VS_MOVK | kswz << 4, e, NONE));
}

static int const_index(struct ctx *c, int key);

/* Writes one scalar of a register from wherever the value is. */
static bool
write_scalar(struct ctx *c, unsigned dst, unsigned scalar,
             const struct held *h)
{
   unsigned op0 = scalar ? 0x1eu : 0x1du, reg, swz;
   int key, e;

   if (held_is_const(h)) {
      if (!const_pair_swizzled(c, h, h, &key, &swz))
         return fail(c, "constant operand");
      e = const_index(c, key);
      if (e < 0)
         return fail(c, "constant span overflow");
      emit_move_const(c, dst, op0, e, swz);
   } else if (find_pair(c, h, h, false, &reg, &swz)) {
      emit_move(c, dst, op0, reg, swz);
   } else {
      return fail(c, "value never written");
   }
   hold_def(c, dst, scalar, h);
   return true;
}

/* A register operand holding the two values, built with up to two moves
 * when no register already carries them. */
static bool
operand(struct ctx *c, const struct held *a, const struct held *b,
        bool identity_only, unsigned *reg, unsigned *swz)
{
   int dst;

   if (find_pair(c, a, b, identity_only, reg, swz)) {
      /* A reused input/literal pair has no SSA last-use protection.  Keep
       * the next operand's materialization from taking it back. */
      c->busy[*reg] = 1;
      return true;
   }
   dst = reg_alloc(c);
   if (dst < 0)
      return fail(c, "out of temporaries");
   if (!write_scalar(c, dst, 0, a) || !write_scalar(c, dst, 1, b))
      return false;
   *reg = dst;
   *swz = 1;
   return true;
}

unsigned
m9_vs_attr_element(const struct m9_vs_program *p, unsigned i)
{
   /* A coefficient slot is fed by the driver, not by an element. */
   if (!p || i >= p->attr_count || p->attrs[i].constant)
      return ~0u;
   return util_bitcount64(p->inputs_read &
                          BITFIELD64_MASK(p->attrs[i].location));
}

/* Constant span placement: the tail names pairs 9..21, so user pairs run
 * 0..8 and then 22 upward. */
unsigned
m9_vs_const_pair(unsigned index)
{
   return index < M9_VS_TAIL_FIRST_PAIR ? index : index + M9_VS_TAIL_PAIRS;
}

static int
const_index(struct ctx *c, int key)
{
   unsigned pair;

   if (key < 0)
      return -1;
   pair = c->faa ? (unsigned)key : m9_vs_const_pair(key);
   return pair <= M9_VS_MAX_CONSTS ? (int)pair : -1;
}

/* Instruction forms */

static bool
emit_mul(struct ctx *c, unsigned dst, const struct held *a0,
         const struct held *a1, const struct held *b0, const struct held *b1)
{
   unsigned ra, rb, sa, sb, kswz;
   int key, e;

   if (held_is_const(b0) && held_is_const(b1) &&
       const_pair_swizzled(c, b0, b1, &key, &kswz)) {
      if (!operand(c, a0, a1, false, &ra, &sa))
         return false;
      e = const_index(c, key);
      if (e < 0)
         return fail(c, "constant span overflow");
      emit(c, w0_make(NONE, ra, NONE, dst, 0, 0x1c),
           w1_make(VS_MUL_RK | kswz << 8 | sa << 6, e, NONE));
      return true;
   }
   if (held_is_const(a0) && held_is_const(a1))
      return emit_mul(c, dst, b0, b1, a0, a1);
   if (!operand(c, a0, a1, false, &ra, &sa) ||
       !operand(c, b0, b1, false, &rb, &sb))
      return false;
   emit(c, w0_make(ra, rb, NONE, dst, 0, 0x1c),
        w1_make(VS_MUL_RR | sa << 8 | sb << 6, NONE, ra));
   return true;
}

/* b * multiplier + addend, with the multiplier a constant pair and the
 * addend either a constant pair (the captured viewport form) or a
 * register. */
static bool
emit_mad(struct ctx *c, unsigned dst, const struct held *b0,
         const struct held *b1, const struct held *k0, const struct held *k1,
         const struct held *c0, const struct held *c1)
{
   unsigned rb, sb, rc, sc, kswz, cswz;
   int key, kc, e, ec;

   if (!const_pair_swizzled(c, k0, k1, &key, &kswz))
      return false;
   e = const_index(c, key);
   if (e < 0)
      return fail(c, "constant span overflow");
   if (!operand(c, b0, b1, false, &rb, &sb))
      return false;
   if (held_is_const(c0) && held_is_const(c1) &&
       const_pair(c, c0, c1, &kc, &cswz)) {
      ec = const_index(c, kc);
      if (ec < 0)
         return fail(c, "constant span overflow");
      emit(c, w0_make(NONE, rb, ec, dst, 0, 0x1c),
           w1_make(VS_MAD_RKK | kswz << 8 | sb << 6, e, NONE));
      return true;
   }
   if (!operand(c, c0, c1, false, &rc, &sc))
      return false;
   emit(c, w0_make(NONE, rb, rc, dst, 0, 0x1c),
        w1_make(VS_MAD_RKR | kswz << 8 | sb << 6 | sc << 4, e, NONE));
   return true;
}

/* a + b, or a - b with negate.  The subtract form computes the a operand
 * minus the c one: MEASURED on the M9128, and the fragment backend, whose
 * own subtract is measured correct, encodes it that way too
 * (m9_fs_compile.c, N_SUB).  The vertex backend used to swap the two
 * fields, which inverted every register comparison, every complement and
 * both halves of a two-register minimum and maximum. */
static bool
emit_add(struct ctx *c, unsigned dst, const struct held *a0,
         const struct held *a1, const struct held *b0, const struct held *b1,
         bool negate)
{
   const struct held *f0 = a0, *f1 = a1, *g0 = b0, *g1 = b1;
   unsigned ra, rb, sa, sb;

   /* A constant addend rides the captured multiply-add against one. */
   if (!negate && held_is_const(b0) && held_is_const(b1)) {
      struct held one = held_lit(1.0f);

      if (emit_mad(c, dst, a0, a1, &one, &one, b0, b1))
         return true;
      if (c->reason)
         return false;
   }
   if (!operand(c, f0, f1, true, &ra, &sa) ||
       !operand(c, g0, g1, true, &rb, &sb))
      return false;
   emit(c, w0_make(ra, NONE, rb, dst, 0, 0x1c),
        w1_make(VS_ADD_RR | sa << 8 | (negate ? 0xc0u : 0) | sb << 4,
                NONE, ra));
   return true;
}

static bool
emit_unary(struct ctx *c, unsigned dst, uint32_t base, unsigned cls,
           const struct held *a0, const struct held *a1, bool swizzled)
{
   unsigned ra, sa;

   if (!operand(c, a0, a1, !swizzled, &ra, &sa))
      return false;
   emit(c, w0_make(NONE, NONE, ra, dst, cls, 0x1c),
        w1_make(base | (swizzled ? sa : 1u) << 4, NONE, NONE));
   return true;
}

/* The compare unit writes 1.0 or 0.0; its constant operand is a pair index
 * in the c field, as every UNIT 1 instruction takes it. */
static bool
emit_cmp(struct ctx *c, unsigned dst, unsigned cls, const struct held *a0,
         const struct held *a1, const struct held *b0, const struct held *b1)
{
   unsigned ra, sa, kswz;
   int key, e;

   if (!held_is_const(b0) || !held_is_const(b1) ||
       !(c->compact ? const_pair(c, b0, b1, &key, &kswz) :
                      const_pair_swizzled(c, b0, b1, &key, &kswz)))
      return fail(c, "compare against a register");
   if (c->identity_keys)
      c->identity_keys[key] = true;
   e = const_index(c, key);
   if (e < 0)
      return fail(c, "constant span overflow");
   if (!operand(c, a0, a1, false, &ra, &sa))
      return false;
   emit(c, w0_make(NONE, ra, e, dst, cls, 0x1c),
        w1_make(VS_CMP | kswz << 8 | sa << 6, 0x05, NONE));
   return true;
}

static bool
emit_sel(struct ctx *c, unsigned dst, unsigned cls, const struct held *h[3][2])
{
   unsigned r[3], s[3];

   for (unsigned i = 0; i < 3; i++)
      if (!operand(c, h[i][0], h[i][1], false, &r[i], &s[i]))
         return false;
   emit(c, w0_make(r[0], r[1], r[2], dst, cls, 0x1c),
        w1_make(VS_SEL | s[0] << 8 | s[1] << 6 | s[2] << 4, NONE, r[0]));
   return true;
}

/* min and max take one register and one constant; two registers become
 * a + min(b - a, 0) and a + max(b - a, 0), which is exact. */
static bool
emit_minmax(struct ctx *c, unsigned dst, bool maximum, const struct held *a0,
            const struct held *a1, const struct held *b0,
            const struct held *b1)
{
   unsigned ra, sa, kswz;
   int key, e, t;
   struct held zero = held_lit(0.0f), th;

   if (held_is_const(a0) && held_is_const(a1)) {
      const struct held *s0 = a0, *s1 = a1;

      a0 = b0; a1 = b1; b0 = s0; b1 = s1;
   }
   if (held_is_const(b0) && held_is_const(b1)) {
      if (!const_pair(c, b0, b1, &key, &kswz))
         return false;
      e = const_index(c, key);
      if (e < 0)
         return fail(c, "constant span overflow");
      if (!operand(c, a0, a1, true, &ra, &sa))
         return false;
      emit(c, w0_make(NONE, ra, ra, dst, maximum ? 3 : 2, 0x1c),
           w1_make(VS_MINMAX, e, NONE));
      return true;
   }
   th = held_temp(c);
   t = reg_alloc_pinned(c, &th);
   if (t < 0)
      return fail(c, "out of temporaries");
   if (!emit_add(c, t, b0, b1, a0, a1, true))
      return false;
   if (!emit_minmax(c, t, maximum, &th, &th, &zero, &zero))
      return false;
   return emit_add(c, dst, &th, &th, a0, a1, false);
}

static bool write_pair(struct ctx *c, unsigned reg, const struct held *h0,
                       const struct held *h1);

/* The transcendental chains take a register operand and refine it; a
 * compile-time value needs neither, and passing one through the chains
 * produced a wrong reciprocal (offline case literal-xy, MEASURED). */
static bool
fold_transcendental(nir_op op, float v, float *out)
{
   switch (op) {
   case nir_op_frcp: *out = v != 0.0f ? 1.0f / v : INFINITY; break;
   case nir_op_frsq: *out = v > 0.0f ? 1.0f / sqrtf(v) : INFINITY; break;
   case nir_op_fsqrt: *out = v > 0.0f ? sqrtf(v) : 0.0f; break;
   case nir_op_fexp2: *out = exp2f(v); break;
   default: return false;
   }
   return true;
}

/* Reciprocal: the captured seed, two residuals and two Newton steps
 * against the constant pair (0, 2).  Each step writes the first scalar
 * only, exactly as the vendor program does. */
static bool
emit_rcp(struct ctx *c, unsigned dst, const struct held *a)
{
   unsigned ra, sa;
   int k = key_literal(c, 0.0f, 2.0f), e, t0, t1;
   struct held h0, h1;

   e = const_index(c, k);
   if (e < 0)
      return fail(c, "constant span overflow");
   if (!operand(c, a, a, false, &ra, &sa))
      return false;
   h0 = held_temp(c);
   t0 = reg_alloc_pinned(c, &h0);
   if (t0 < 0)
      return fail(c, "out of temporaries");
   emit(c, w0_make(ra, NONE, NONE, t0, 0, 0x1d),
        w1_make(VS_RCP_SEED | sa << 8, NONE, ra));
   h1 = held_temp(c);
   t1 = reg_alloc_pinned(c, &h1);
   if (t1 < 0)
      return fail(c, "out of temporaries");
   for (unsigned i = 0; i < 2; i++) {
      unsigned d = i ? dst : (unsigned)t0;

      emit(c, w0_make(ra, t0, NONE, t1, 5, 0x1c),
           w1_make(VS_RCP_RES | sa << 8, 0, ra));
      emit(c, w0_make(t0, t1, t1, d, 0, 0x1d), w1_make(VS_RCP_STEP, e, t0));
      t0 = d;
   }
   return true;
}

/* Reciprocal square root: the fragment unit's measured chain.  The seed
 * writes a pair whose second scalar every refinement needs again, so each
 * step copies it across and writes only the first scalar itself. */
static bool
emit_rsq(struct ctx *c, unsigned dst, const struct held *a)
{
   const unsigned scratch_base = c->temp_seq;
   unsigned ra, sa;
   int kc = key_literal(c, uif(0x7f800000), uif(0xff800000));
   int kk = key_literal(c, 0.0f, 3.0f);
   int ec, ek, p, t0, t1;
   struct held h;

   ec = const_index(c, kc);
   ek = const_index(c, kk);
   if (ec < 0 || ek < 0)
      return fail(c, "constant span overflow");
   if (!operand(c, a, a, false, &ra, &sa))
      return false;
   h = held_temp(c);
   p = reg_alloc_pinned(c, &h);
   if (p < 0)
      return fail(c, "out of temporaries");
   emit(c, w0_make(NONE, NONE, ra, p, 0, 0x1c),
        w1_make(VS_RSQ_SHUF | sa << 4, 0, NONE));
   h = held_temp(c);
   t0 = reg_alloc_pinned(c, &h);
   if (t0 < 0)
      return fail(c, "out of temporaries");
   emit(c, w0_make(p, p, ec, t0, 5, 0x1c), w1_make(VS_RSQ_SEED, 0, p));
   for (unsigned i = 0; i < 2; i++) {
      struct held hd = held_temp(c);
      int d;

      h = held_temp(c);
      t1 = reg_alloc_pinned(c, &h);
      d = i ? (int)dst : reg_alloc_pinned(c, &hd);
      if (t1 < 0 || d < 0)
         return fail(c, "out of temporaries");
      emit(c, w0_make(t0, t0, NONE, t1, 0, 0x1c), w1_make(VS_RSQ_MUL, 0, t0));
      emit(c, w0_make(NONE, NONE, t0, d, 0, 0x1e),
           w1_make(VS_RSQ_COPY, 0, NONE));
      emit(c, w0_make(t0, t1, t1, d, 7, 0x1d),
           w1_make(VS_RSQ_STEP, ek, t0));
      t0 = d;
   }
   /* Both refinements are complete.  Retain the caller's result and source
    * identities, freeing only pairs created inside this scalar chain. */
   for (unsigned r = 0; r < M9_VS_REGS; r++) {
      if (c->holds[r][0].kind != HELD_TEMP ||
          c->holds[r][1].kind != HELD_TEMP ||
          c->holds[r][0].a <= scratch_base ||
          c->holds[r][1].a <= scratch_base)
         continue;
      memset(c->holds[r], 0, sizeof(c->holds[r]));
      c->busy[r] = 0;
   }
   return true;
}

/* One scalar of a reciprocal or reciprocal square root, as a literal when
 * the argument is one, else in the first scalar of a fresh temporary. */
static bool
scalar_chain(struct ctx *c, nir_op op, const struct held *a, struct held *out)
{
   int r;

   if (a->kind == HELD_LIT) {
      float v;

      fold_transcendental(op, a->lit, &v);
      *out = held_lit(v);
      return true;
   }
   *out = held_temp(c);
   r = reg_alloc_pinned(c, out);
   if (r < 0)
      return fail(c, "out of temporaries");
   if (!(op == nir_op_frcp ? emit_rcp(c, r, a) : emit_rsq(c, r, a)))
      return false;
   memset(&c->holds[r][1], 0, sizeof(c->holds[r][1]));
   return true;
}

/* exp2: the vendor's seed splits the argument, three slots evaluate a
 * minimax polynomial over the fraction and a multiply applies the scale the
 * seed split off.  CAPTURED in the vendor's own vertex programs ff-fog-exp
 * and ff-fog-exp2, words, operand wiring and coefficient pairs alike, so
 * this is a vertex-unit form rather than one borrowed from the fragment
 * unit -- the two carry the same bytes here. */
static bool
emit_exp2(struct ctx *c, unsigned dst, const struct held *a)
{
   static const uint32_t poly[3][2] = {
      { 0x3af71c15, 0x3c130514 }, { 0x3d64b436, 0x3e75ea9e },
      { 0x3f317271, 0x3f800000 },
   };
   unsigned ra, sa;
   int e[3], t[4];

   for (unsigned i = 0; i < 3; i++) {
      e[i] = const_index(c, key_literal(c, uif(poly[i][0]), uif(poly[i][1])));
      if (e[i] < 0)
         return fail(c, "constant span overflow");
   }
   if (!operand(c, a, a, false, &ra, &sa))
      return false;
   for (unsigned i = 0; i < 4; i++) {
      struct held h = held_temp(c);

      t[i] = reg_alloc_pinned(c, &h);
      if (t[i] < 0)
         return fail(c, "out of temporaries");
   }
   emit(c, w0_make(ra, NONE, NONE, t[0], 0, 0x1c),
        w1_make(VS_EXP2_SEED | sa << 4, 0, ra));
   emit(c, w0_make(t[0], t[0], e[0], t[1], 0, 0x1c),
        w1_make(VS_EXP2_POLY, 0, t[0]));
   emit(c, w0_make(t[0], t[1], t[1], t[2], 0, 0x1e),
        w1_make(VS_EXP2_STEP, e[1], t[0]));
   emit(c, w0_make(t[0], t[2], t[1], t[3], 0, 0x1e),
        w1_make(VS_EXP2_STEP, e[2], t[0]));
   emit(c, w0_make(t[0], t[3], NONE, dst, 0, 0x1c),
        w1_make(VS_EXP2_SCALE, 0, t[0]));
   /* The final scale consumed every scratch value in this scalar chain.
    * These four pinned HELD_TEMP pairs are distinct from the caller's
    * result and every source SSA value; retain those older identities while
    * letting the next vector component reuse just this chain's scratch. */
   for (unsigned i = 0; i < ARRAY_SIZE(t); i++) {
      memset(c->holds[t[i]], 0, sizeof(c->holds[t[i]]));
      c->busy[t[i]] = 0;
   }
   return true;
}

/* The stage-1 vendor frontend emits VS_LOG2_SEED, reading scalar 0 and
 * splitting x = 2^exponent * (1 + fraction).  Rebuild its degree-seven
 * polynomial with the ordinary GPU ALU: log2(x) = exponent + fraction *
 * P(fraction).  No compact polynomial instruction or implicit carry is
 * needed.  Both seed scalars stay live under distinct held identities.
 * Reusing the product register bounds the scratch cost to four pairs. */
static bool
emit_log2(struct ctx *c, unsigned dst, const struct held *a)
{
   static const uint32_t poly[8] = {
      0xbc188576, 0x3d553bd2, 0xbe0cd266, 0x3e77ab67,
      0xbeb1d173, 0x3ef51608, 0xbf389e52, 0x3fb8aa10,
   };
   struct held input = held_temp(c), exponent = held_temp(c);
   struct held fraction = held_temp(c), acc = held_temp(c);
   struct held product = held_temp(c);
   unsigned ra, sa;
   int rp, rs, rh, rm;

   if (!operand(c, a, a, false, &ra, &sa))
      return false;
   rp = reg_alloc_pinned(c, &input);
   rs = reg_alloc_pinned(c, &exponent);
   rh = reg_alloc_pinned(c, &acc);
   rm = reg_alloc_pinned(c, &product);
   if (rp < 0 || rs < 0 || rh < 0 || rm < 0)
      return fail(c, "out of temporaries");
   emit_move(c, rp, 0x1d, ra, sa);
   emit(c, w0_make(rp, NONE, NONE, rs, 0, 0x1c),
        w1_make(VS_LOG2_SEED, 0, rp));
   hold_def(c, rs, 1, &fraction);
   {
      const struct held k0 = held_lit(uif(poly[0]));
      const struct held k1 = held_lit(uif(poly[1]));

      if (!emit_mad(c, rh, &fraction, &fraction, &k0, &k0, &k1, &k1))
         return false;
   }
   for (unsigned i = 2; i < ARRAY_SIZE(poly); i++) {
      const struct held k = held_lit(uif(poly[i]));

      if (!emit_mul(c, rm, &fraction, &fraction, &acc, &acc) ||
          !emit_add(c, rh, &product, &product, &k, &k, false))
         return false;
   }
   if (!emit_mul(c, rm, &fraction, &fraction, &acc, &acc) ||
       !emit_add(c, dst, &product, &product, &exponent, &exponent, false))
      return false;
   /* These scratch pairs have no users after this scalar chain.  Release
    * them before another component of the same NIR instruction runs. */
   const unsigned scratch[] = { (unsigned)rp, (unsigned)rs,
                                (unsigned)rh, (unsigned)rm };
   for (unsigned i = 0; i < ARRAY_SIZE(scratch); i++) {
      memset(c->holds[scratch[i]], 0, sizeof(c->holds[scratch[i]]));
      c->busy[scratch[i]] = 0;
   }
   return true;
}

/* A zero base with a positive exponent has a defined zero result.  Select
 * it on the GPU after the ordinary log/multiply/exp chain, so it does not
 * depend on either special seed's undocumented behavior at zero/infinity.
 * Other nonpositive-base cases are outside pow's defined GLSL domain. */
static bool
emit_pow(struct ctx *c, unsigned dst, const struct held *a,
         const struct held *b)
{
   const unsigned scratch_base = c->temp_seq;
   struct held logarithm = held_temp(c), product = held_temp(c);
   struct held result = held_temp(c), condition = held_temp(c);
   const struct held zero = held_lit(0.0f);
   const struct held *sel[3][2] = {
      { &condition, &condition }, { &zero, &zero }, { &result, &result },
   };
   int rl = reg_alloc_pinned(c, &logarithm);
   int rm = reg_alloc_pinned(c, &product);
   int rr = reg_alloc_pinned(c, &result);
   int rc = reg_alloc_pinned(c, &condition);

   if (rl < 0 || rm < 0 || rr < 0 || rc < 0)
      return fail(c, "out of temporaries");
   if (!emit_log2(c, rl, a) ||
       !emit_mul(c, rm, &logarithm, &logarithm, b, b) ||
       !emit_exp2(c, rr, &product) ||
       !emit_cmp(c, rc, 4, a, a, &zero, &zero) ||
       !emit_sel(c, dst, 5, sel))
      return false;
   /* The final select has consumed this scalar chain's intermediates,
    * including exp2's four scratch pairs.  The caller allocated dst and
    * its earlier component results before scratch_base; live NIR SSA
    * values have HELD_DEF identities.  Release only pairs whose two held
    * identities were created inside this power chain. */
   for (unsigned r = 0; r < M9_VS_REGS; r++) {
      if (c->holds[r][0].kind != HELD_TEMP ||
          c->holds[r][1].kind != HELD_TEMP ||
          c->holds[r][0].a <= scratch_base ||
          c->holds[r][1].a <= scratch_base)
         continue;
      memset(c->holds[r], 0, sizeof(c->holds[r]));
      c->busy[r] = 0;
   }
   return true;
}

/* sine and cosine: no instruction computes either.  The vendor's own
 * fragment program fs-sin reduces the argument modulo 2*pi with a fract and
 * evaluates a degree-four minimax polynomial in x*x, and the coefficients it
 * loads sit in the Windows compiler's constant pool right beside the exp2 and
 * log2 ones (docs/parhelia-m9-vertex-isa-gaps.md).  The same reduction and
 * polynomial are rebuilt here from forms the vertex unit already runs, so
 * this needs no encoding the backend did not already emit.
 *
 * The vendor holds the polynomial halved, because its combining instruction
 * doubles; folded back to sin(x) = x * P(x*x) the coefficients are these,
 * worst error 6.9e-5 against the C library over the whole reduced range. */
static const float m9_vs_sin_poly[5] = {
   2.14787088e-06f, -1.92649924e-04f, 8.30898434e-03f,
   -0.166624382f, 0.999958754f,
};

static bool
emit_sincos(struct ctx *c, unsigned dst, const struct held *a0,
            const struct held *a1, bool cosine)
{
   /* The vendor's own reduction constants, byte for byte. */
   const struct held inv = held_lit(0.159154937f);
   const struct held phase = held_lit(cosine ? 0.75f : 0.5f);
   const struct held two_pi = held_lit(6.28318548f);
   const struct held neg_pi = held_lit(-3.14159274f);
   struct held ht = held_temp(c), hx = held_temp(c), hy = held_temp(c);
   struct held hp = held_temp(c);
   int rt, rx, ry, rp;

   rt = reg_alloc_pinned(c, &ht);
   rx = reg_alloc_pinned(c, &hx);
   ry = reg_alloc_pinned(c, &hy);
   rp = reg_alloc_pinned(c, &hp);
   if (rt < 0 || rx < 0 || ry < 0 || rp < 0)
      return fail(c, "out of temporaries");
   /* cos(x) is sin(x + pi/2), which the phase of the reduction carries. */
   if (!emit_mad(c, rt, a0, a1, &inv, &inv, &phase, &phase) ||
       !emit_unary(c, rt, VS_FRACT, 0, &ht, &ht, true) ||
       !emit_mad(c, rx, &ht, &ht, &two_pi, &two_pi, &neg_pi, &neg_pi) ||
       !emit_mul(c, ry, &hx, &hx, &hx, &hx))
      return false;
   {
      const struct held k0 = held_lit(m9_vs_sin_poly[0]);
      const struct held k1 = held_lit(m9_vs_sin_poly[1]);

      if (!emit_mad(c, rp, &hy, &hy, &k0, &k0, &k1, &k1))
         return false;
   }
   for (unsigned i = 2; i < 5; i++) {
      const struct held k = held_lit(m9_vs_sin_poly[i]);
      struct held hm = held_temp(c);
      int rm = reg_alloc_pinned(c, &hm);

      if (rm < 0)
         return fail(c, "out of temporaries");
      if (!emit_mul(c, rm, &hp, &hp, &hy, &hy) ||
          !emit_add(c, rp, &hm, &hm, &k, &k, false))
         return false;
   }
   return emit_mul(c, dst, &hx, &hx, &hp, &hp);
}

static bool
pairable(const struct held *a, const struct held *b)
{
   if (a->kind == HELD_LIT && b->kind == HELD_LIT)
      return true;
   if (a->kind == HELD_UNIFORM && b->kind == HELD_UNIFORM)
      return true;
   if (a->kind == HELD_UNIFORM && b->kind == HELD_LIT && b->lit == 0.0f)
      return true;
   return a->kind == HELD_LIT && a->lit == 0.0f && b->kind == HELD_UNIFORM;
}

static bool
dot_pairable(const struct held b[4], unsigned n)
{
   struct held zero = held_lit(0.0f);

   if (!pairable(&b[0], &b[1]))
      return false;
   return n <= 2 || pairable(&b[2], n > 3 ? &b[3] : &zero);
}

/* A dot product of a register pair against a constant pair: the partial
 * writes the first scalar and the completion adds it.  The partial's c field
 * names a constant pair whose first scalar it subtracts (MEASURED), so it
 * names a zero pair; the vendor's c = 3 relied on its pair 3 being zero. */
static bool
emit_dot(struct ctx *c, unsigned dst, const struct held a[4],
         const struct held b[4], unsigned n)
{
   struct held zero = held_lit(0.0f);
   const struct held *a2 = n > 2 ? &a[2] : &zero;
   const struct held *a3 = n > 3 ? &a[3] : (n > 2 ? &a[2] : &zero);
   const struct held *b2 = n > 2 ? &b[2] : &zero;
   const struct held *b3 = n > 3 ? &b[3] : &zero;
   unsigned ra, sa, kswz;
   int key, e, ez, t;
   struct held h;

   if (!const_pair(c, &b[0], &b[1], &key, &kswz))
      return false;
   e = const_index(c, key);
   if (!const_pair(c, &zero, &zero, &key, &kswz))
      return false;
   ez = const_index(c, key);
   if (e < 0 || ez < 0)
      return fail(c, "constant span overflow");
   if (!operand(c, &a[0], &a[1], true, &ra, &sa))
      return false;
   if (n <= 2) {
      emit(c, w0_make(NONE, ra, ez, dst, 0, 0x1d),
           w1_make(VS_DOT_P, e, NONE));
      return true;
   }
   h = held_temp(c);
   t = reg_alloc_pinned(c, &h);
   if (t < 0)
      return fail(c, "out of temporaries");
   emit(c, w0_make(NONE, ra, ez, t, 0, 0x1d), w1_make(VS_DOT_P, e, NONE));
   if (!const_pair(c, b2, b3, &key, &kswz))
      return false;
   e = const_index(c, key);
   if (e < 0)
      return fail(c, "constant span overflow");
   /* The dot reads its register operand whole, so the completion needs the
    * two components side by side; a three-component dot weights the second
    * of them with zero and only its first scalar has to match. */
   if (n == 3 && find_first(c, a2, &ra))
      sa = 0;
   else if (!operand(c, a2, a3, true, &ra, &sa))
      return false;
   emit(c, w0_make(NONE, ra, t, dst, 0, 0x1d), w1_make(VS_DOT_C, e, NONE));
   return true;
}

/* Definitions */

static struct held
src_held(nir_alu_instr *alu, unsigned i, unsigned comp)
{
   return held_of(nir_get_scalar(alu->src[i].src.ssa,
                                 alu->src[i].swizzle[comp]));
}

static void
hold_lane(struct ctx *c, unsigned reg, nir_def *def, unsigned c0, unsigned c1,
          bool pair)
{
   struct held h;

   memset(&h, 0, sizeof(h));
   h.kind = HELD_DEF;
   h.a = def->index;
   h.b = c0;
   hold_def(c, reg, 0, &h);
   h.b = c1;
   if (pair)
      hold_def(c, reg, 1, &h);
   else
      memset(&c->holds[reg][1], 0, sizeof(c->holds[reg][1]));
}

/* A dot product expanded into the scalar forms, for operands the captured
 * partial and completion cannot take. */
static bool
dot_expand(struct ctx *c, unsigned dst, const struct held a[4],
           const struct held b[4], unsigned n)
{
   struct held acc = held_temp(c);
   int t = reg_alloc_pinned(c, &acc);

   if (t < 0)
      return fail(c, "out of temporaries");
   if (!emit_mul(c, t, &a[0], &a[0], &b[0], &b[0]))
      return false;
   for (unsigned j = 1; j < n; j++) {
      unsigned d = j + 1 == n ? dst : (unsigned)t;

      if (held_is_const(&b[j])) {
         if (!emit_mad(c, d, &a[j], &a[j], &b[j], &b[j], &acc, &acc))
            return false;
      } else if (held_is_const(&a[j])) {
         if (!emit_mad(c, d, &b[j], &b[j], &a[j], &a[j], &acc, &acc))
            return false;
      } else {
         struct held ht = held_temp(c);
         int u = reg_alloc_pinned(c, &ht);

         if (u < 0)
            return fail(c, "out of temporaries");
         if (!emit_mul(c, u, &a[j], &a[j], &b[j], &b[j]) ||
             !emit_add(c, d, &ht, &ht, &acc, &acc, false))
            return false;
      }
      if (j + 1 < n) {
         acc = held_temp(c);
         hold_def(c, t, 0, &acc);
         hold_def(c, t, 1, &acc);
      }
   }
   return true;
}

static bool
build_dot(struct ctx *c, nir_alu_instr *alu, unsigned n, unsigned dst,
          bool *pair)
{
   struct held a[4], b[4];
   bool a_const = true, b_const = true;

   for (unsigned j = 0; j < n; j++) {
      a[j] = src_held(alu, 0, j);
      b[j] = src_held(alu, 1, j);
      a_const &= held_is_const(&a[j]);
      b_const &= held_is_const(&b[j]);
   }
   *pair = false;
   if (b_const && dot_pairable(b, n))
      return emit_dot(c, dst, a, b, n);
   if (a_const && dot_pairable(a, n))
      return emit_dot(c, dst, b, a, n);
   if (n == 2) {
      /* Actual vendor vertex tail s36 and vs-math/lighting use this form.
       * Both pairs are read whole; the c field must remain equal to a. */
      unsigned ra, rb, sa, sb;
      int zero = key_literal(c, 0.0f, 0.0f), e = const_index(c, zero);
      if (e < 0)
         return fail(c, "constant span overflow");
      if (!operand(c, &a[0], &a[1], true, &ra, &sa) ||
          !operand(c, &b[0], &b[1], true, &rb, &sb))
         return false;
      emit(c, w0_make(ra, rb, ra, dst, 0, 0x1d),
           w1_make(VS_DOT_RR, e, ra));
      return true;
   }
   *pair = true;
   return dot_expand(c, dst, a, b, n);
}

/* A lane a single instruction writes whole can land in the output register
 * itself, which saves the two moves that would copy it there. */
static bool
pair_writing_op(nir_scalar s)
{
   if (!nir_scalar_is_alu(s))
      return false;
   switch (nir_scalar_alu_op(s)) {
   case nir_op_fdot2:
   case nir_op_fdot3:
   case nir_op_fdot4:
   case nir_op_frcp:
   case nir_op_frsq:
      return false;
   default:
      return !nir_op_is_vec_or_mov(nir_scalar_alu_op(s));
   }
}

static void
pin_output(struct ctx *c, nir_intrinsic_instr *store, unsigned first,
           unsigned reg)
{
   unsigned base = store ? nir_intrinsic_component(store) : 0;
   nir_scalar s[2];

   if (!store)
      return;
   for (unsigned j = 0; j < 2; j++) {
      unsigned comp = first + j;

      if (comp < base || comp - base >= store->src[0].ssa->num_components ||
          !(nir_intrinsic_write_mask(store) & 1u << (comp - base)))
         return;
      s[j] = nir_scalar_chase_movs(nir_get_scalar(store->src[0].ssa,
                                                  comp - base));
   }
   if (s[0].def != s[1].def || s[1].comp != s[0].comp + 1 ||
       s[0].comp % 2 || !pair_writing_op(s[0]))
      return;
   for (unsigned i = 0; i < c->pin_count; i++)
      if (c->pins[i].def == s[0].def->index &&
          c->pins[i].lane == s[0].comp / 2)
         return;
   c->pins[c->pin_count].def = s[0].def->index;
   c->pins[c->pin_count].lane = s[0].comp / 2;
   c->pins[c->pin_count++].reg = reg;
}

static int
pin_lookup(struct ctx *c, unsigned def, unsigned lane)
{
   for (unsigned i = 0; i < c->pin_count; i++)
      if (c->pins[i].def == def && c->pins[i].lane == lane)
         return c->pins[i].reg;
   return -1;
}

/* Keep direct ALU pins when both halves can be scheduled together. Independent
 * halves wait for the final stores. The captured GLSL source producers write
 * EE19 high halves before low halves; this is not a rule for flat targets or
 * every other vendor program family. */
static void
pin_packed_output(struct ctx *c, nir_intrinsic_instr *store,
                  unsigned lo, unsigned hi)
{
   if (!store || nir_intrinsic_component(store) ||
       store->src[0].ssa->num_components != 4 ||
       nir_intrinsic_write_mask(store) != 15)
      return;
   nir_scalar s[4];
   for (unsigned i = 0; i < 4; i++)
      s[i] = nir_scalar_chase_movs(nir_get_scalar(store->src[0].ssa, i));
   for (unsigned i = 1; i < 4; i++)
      if (s[i].def != s[0].def || s[i].comp != s[0].comp + i)
         return;
   /* Published output registers cannot serve a later ALU or another output.
    * Keep a normal temporary unless this full definition has one direct
    * store owner. The ordinary pinning policy remains unchanged. */
   if (s[0].comp || s[0].def != store->src[0].ssa ||
       !list_is_singular(&s[0].def->uses))
      return;
   pin_output(c, store, 2, hi);
   if (pin_lookup(c, s[0].def->index, 1) != (int)hi)
      return;
   pin_output(c, store, 0, lo);
}

/* Mesa has no pass that fuses a multiply and an add, and the hardware's
 * multiply-add takes its multiplier from a constant pair, which is what a
 * matrix column is: fold one here when the product feeds nothing else. */
static nir_alu_instr *
fusable_mul(nir_alu_instr *alu, unsigned i, unsigned n)
{
   nir_def *def = alu->src[i].src.ssa;
   nir_instr *instr = nir_def_instr(def);
   nir_alu_instr *mul;

   if (instr->type != nir_instr_type_alu || !list_is_singular(&def->uses))
      return NULL;
   mul = nir_instr_as_alu(instr);
   if (mul->op != nir_op_fmul && mul->op != nir_op_fmulz)
      return NULL;
   if (mul->def.num_components != n)
      return NULL;
   for (unsigned j = 0; j < n; j++)
      if (alu->src[i].swizzle[j] != j)
         return NULL;
   return mul;
}

/* Subtraction is the existing measured a - c form.  A single-use negate
 * can ride it without writing a separate negative register pair. */
static nir_alu_instr *
fusable_neg(nir_alu_instr *alu, unsigned i, unsigned n)
{
   nir_def *def = alu->src[i].src.ssa;
   nir_instr *instr = nir_def_instr(def);
   if (instr->type != nir_instr_type_alu || !list_is_singular(&def->uses))
      return NULL;
   nir_alu_instr *neg = nir_instr_as_alu(instr);
   if (neg->op != nir_op_fneg || neg->def.num_components != n)
      return NULL;
   for (unsigned j = 0; j < n; j++)
      if (alu->src[i].swizzle[j] != j)
         return NULL;
   return neg;
}

/* The multiplier operand of a fusable product, or -1. */
static int
mul_const_side(nir_alu_instr *mul, unsigned c0, unsigned c1)
{
   for (unsigned k = 0; k < 2; k++) {
      struct held k0 = src_held(mul, k, c0), k1 = src_held(mul, k, c1);

      if (held_is_const(&k0) && held_is_const(&k1) && pairable(&k0, &k1))
         return k;
   }
   return -1;
}

static bool
build_alu(struct ctx *c, nir_alu_instr *alu)
{
   unsigned n = alu->def.num_components;
   unsigned lanes = (n + 1) / 2;

   if (nir_op_is_vec_or_mov(alu->op) || c->fold[alu->def.index] >= 0)
      return true;
   if (alu->def.bit_size != 32 && alu->def.bit_size != 1)
      return fail(c, "narrow definition");
   int low_pin = pin_lookup(c, alu->def.index, 0);
   bool high_first = c->packed_outputs && lanes == 2 &&
                     low_pin >= (int)REG_OUT_LO(0) &&
                     low_pin <= (int)REG_OUT_LO(M9_VS_MAX_ROUTES - 1) &&
                     (low_pin - REG_OUT_LO(0)) % 4 == 0 &&
                     pin_lookup(c, alu->def.index, 1) == low_pin + 2;
   for (unsigned step = 0; step < lanes; step++) {
      unsigned lane = high_first ? lanes - 1 - step : step;
      unsigned c0 = 2 * lane, c1 = MIN2(2 * lane + 1, n - 1);
      struct held a0, a1, b0, b1, x0, x1;
      bool pair = true;
      struct held pin = held_temp(c);
      int dst = pin_lookup(c, alu->def.index, lane);

      if (dst < 0)
         dst = reg_alloc_pinned(c, &pin);
      if (dst < 0)
         return fail(c, "out of temporaries");
      switch (alu->op) {
      case nir_op_fmul:
      case nir_op_fmulz:
         a0 = src_held(alu, 0, c0); a1 = src_held(alu, 0, c1);
         b0 = src_held(alu, 1, c0); b1 = src_held(alu, 1, c1);
         if (!emit_mul(c, dst, &a0, &a1, &b0, &b1))
            return false;
         break;
      case nir_op_fadd: {
         nir_alu_instr *mul = NULL;
         unsigned other = 1;
         int side = -1;

         for (unsigned i = 0; i < 2 && side < 0; i++) {
            mul = fusable_mul(alu, i, n);
            side = mul && c->fold[mul->def.index] < FOLD_NEG ?
                   c->fold[mul->def.index] : -1;
            other = 1 - i;
         }
         if (side >= 0) {
            struct held k0 = src_held(mul, side, c0);
            struct held k1 = src_held(mul, side, c1);

            a0 = src_held(mul, 1 - side, c0);
            a1 = src_held(mul, 1 - side, c1);
            b0 = src_held(alu, other, c0);
            b1 = src_held(alu, other, c1);
            if (!emit_mad(c, dst, &a0, &a1, &k0, &k1, &b0, &b1))
               return false;
            break;
         }
         nir_alu_instr *neg = NULL;
         for (unsigned i = 0; i < 2; i++) {
            neg = fusable_neg(alu, i, n);
            if (neg && c->fold[neg->def.index] == FOLD_NEG) {
               other = 1 - i;
               break;
            }
            neg = NULL;
         }
         if (neg) {
            a0 = src_held(alu, other, c0); a1 = src_held(alu, other, c1);
            b0 = src_held(neg, 0, c0); b1 = src_held(neg, 0, c1);
         } else {
            a0 = src_held(alu, 0, c0); a1 = src_held(alu, 0, c1);
            b0 = src_held(alu, 1, c0); b1 = src_held(alu, 1, c1);
         }
         if (!emit_add(c, dst, &a0, &a1, &b0, &b1, neg != NULL))
            return false;
         break;
      }
      case nir_op_ffma:
      case nir_op_ffmaz:
         a0 = src_held(alu, 0, c0); a1 = src_held(alu, 0, c1);
         b0 = src_held(alu, 1, c0); b1 = src_held(alu, 1, c1);
         x0 = src_held(alu, 2, c0); x1 = src_held(alu, 2, c1);
         if (held_is_const(&b0) && held_is_const(&b1)) {
            if (!emit_mad(c, dst, &a0, &a1, &b0, &b1, &x0, &x1))
               return false;
         } else if (held_is_const(&a0) && held_is_const(&a1)) {
            if (!emit_mad(c, dst, &b0, &b1, &a0, &a1, &x0, &x1))
               return false;
         } else {
            struct held ht = held_temp(c);
            int t = reg_alloc_pinned(c, &ht);

            if (t < 0)
               return fail(c, "out of temporaries");
            if (!emit_mul(c, t, &a0, &a1, &b0, &b1) ||
                !emit_add(c, dst, &ht, &ht, &x0, &x1, false))
               return false;
         }
         break;
      case nir_op_fneg:
         a0 = src_held(alu, 0, c0); a1 = src_held(alu, 0, c1);
         if (!emit_unary(c, dst, VS_NEG, 0, &a0, &a1, false))
            return false;
         break;
      case nir_op_fsat:
         a0 = src_held(alu, 0, c0); a1 = src_held(alu, 0, c1);
         if (!emit_unary(c, dst, VS_MOV, 4, &a0, &a1, false))
            return false;
         break;
      case nir_op_ffract:
      case nir_op_ffloor:
      case nir_op_fceil: {
         uint32_t base = alu->op == nir_op_ffract ? VS_FRACT :
                         alu->op == nir_op_ffloor ? VS_FLOOR : VS_CEIL;

         a0 = src_held(alu, 0, c0); a1 = src_held(alu, 0, c1);
         if (!emit_unary(c, dst, base, 0, &a0, &a1, true))
            return false;
         break;
      }
      case nir_op_fmin:
      case nir_op_fmax:
         a0 = src_held(alu, 0, c0); a1 = src_held(alu, 0, c1);
         b0 = src_held(alu, 1, c0); b1 = src_held(alu, 1, c1);
         if (!emit_minmax(c, dst, alu->op == nir_op_fmax, &a0, &a1, &b0, &b1))
            return false;
         break;
      case nir_op_fsign: {
         /* The existing compare writes an exact Boolean.  Turn x < 0
          * into -1/+1, then select zero for either signed zero.  This is
          * also the sign used by Mesa's asin lowering for generated UVs. */
         const struct held zero = held_lit(0.0f);
         const struct held one = held_lit(1.0f), minus_two = held_lit(-2.0f);
         struct held sign = held_temp(c);
         const struct held *sel[3][2];
         int r = reg_alloc_pinned(c, &sign);

         if (r < 0)
            return fail(c, "out of temporaries");
         a0 = src_held(alu, 0, c0); a1 = src_held(alu, 0, c1);
         sel[0][0] = &a0; sel[0][1] = &a1;
         sel[1][0] = sel[1][1] = &zero;
         sel[2][0] = sel[2][1] = &sign;
         if (!emit_cmp(c, r, 0, &a0, &a1, &zero, &zero) ||
             !emit_mad(c, r, &sign, &sign, &minus_two, &minus_two, &one, &one) ||
             !emit_sel(c, dst, 4, sel))
            return false;
         /* Only the private intermediate has ended; source SSA and the
          * caller's destination remain live across the next component. */
         memset(c->holds[r], 0, sizeof(c->holds[r]));
         c->busy[r] = 0;
         break;
      }
      case nir_op_fabs: {
         /* No modifier for absolute value is captured; the compare and
          * select classes express it: negate where the value is below
          * zero. */
         struct held zero = held_lit(0.0f), hc, hn;
         const struct held *sel[3][2];
         int rc, rn;

         a0 = src_held(alu, 0, c0); a1 = src_held(alu, 0, c1);
         hc = held_temp(c);
         rc = reg_alloc_pinned(c, &hc);
         hn = held_temp(c);
         rn = reg_alloc_pinned(c, &hn);
         if (rc < 0 || rn < 0)
            return fail(c, "out of temporaries");
         if (!emit_cmp(c, rc, 0, &a0, &a1, &zero, &zero) ||
             !emit_unary(c, rn, VS_NEG, 0, &a0, &a1, false))
            return false;
         sel[0][0] = sel[0][1] = &hc;
         sel[1][0] = &a0; sel[1][1] = &a1;
         sel[2][0] = sel[2][1] = &hn;
         if (!emit_sel(c, dst, 4, sel))
            return false;
         break;
      }
      case nir_op_flt:
      case nir_op_fge:
      case nir_op_feq:
      case nir_op_fneu: {
         unsigned cls = alu->op == nir_op_flt ? 0 :
                        alu->op == nir_op_fge ? 1 :
                        alu->op == nir_op_feq ? 4 : 5;

         a0 = src_held(alu, 0, c0); a1 = src_held(alu, 0, c1);
         b0 = src_held(alu, 1, c0); b1 = src_held(alu, 1, c1);
         if (held_is_const(&b0) && held_is_const(&b1)) {
            if (!emit_cmp(c, dst, cls, &a0, &a1, &b0, &b1))
               return false;
         } else {
            /* Against a register the difference carries the predicate. */
            struct held zero = held_lit(0.0f), ht = held_temp(c);
            int t = reg_alloc_pinned(c, &ht);

            if (t < 0)
               return fail(c, "out of temporaries");
            if (!emit_add(c, t, &a0, &a1, &b0, &b1, true) ||
                !emit_cmp(c, dst, cls, &ht, &ht, &zero, &zero))
               return false;
         }
         break;
      }
      case nir_op_bcsel:
      case nir_op_fcsel:
      case nir_op_fcsel_gt:
      case nir_op_fcsel_ge: {
         const struct held *sel[3][2];
         const bool fused = alu->op == nir_op_fcsel_gt ||
                            alu->op == nir_op_fcsel_ge;
         const unsigned cls = alu->op == nir_op_fcsel_gt ? 3 :
                              alu->op == nir_op_fcsel_ge ? 1 : 4;

         a0 = src_held(alu, 0, c0); a1 = src_held(alu, 0, c1);
         b0 = src_held(alu, 1, c0); b1 = src_held(alu, 1, c1);
         x0 = src_held(alu, 2, c0); x1 = src_held(alu, 2, c1);
         /* The shared select compares its first source against zero.
          * Fused > and >= use classes 3 and 1 directly; Boolean/nonzero
          * selects use class 4 with reversed branches. */
         sel[0][0] = &a0; sel[0][1] = &a1;
         sel[1][0] = fused ? &b0 : &x0;
         sel[1][1] = fused ? &b1 : &x1;
         sel[2][0] = fused ? &x0 : &b0;
         sel[2][1] = fused ? &x1 : &b1;
         if (!emit_sel(c, dst, cls, sel))
            return false;
         break;
      }
      case nir_op_inot: {
         /* A condition holds 1.0 or 0.0, so its complement is one minus
          * it.  Only a Boolean is accepted: there is no integer ALU. */
         struct held one = held_lit(1.0f);

         if (alu->def.bit_size != 1)
            return fail(c, "integer not");
         a0 = src_held(alu, 0, c0); a1 = src_held(alu, 0, c1);
         if (!emit_add(c, dst, &one, &one, &a0, &a1, true))
            return false;
         break;
      }
      case nir_op_iand:
      case nir_op_ior:
         /* And is the product of two conditions, or the larger of them;
          * both are exact for 1.0 and 0.0 and stay conditions. */
         if (alu->def.bit_size != 1)
            return fail(c, "integer bitwise operation");
         a0 = src_held(alu, 0, c0); a1 = src_held(alu, 0, c1);
         b0 = src_held(alu, 1, c0); b1 = src_held(alu, 1, c1);
         if (alu->op == nir_op_iand ?
             !emit_mul(c, dst, &a0, &a1, &b0, &b1) :
             !emit_minmax(c, dst, true, &a0, &a1, &b0, &b1))
            return false;
         break;
      case nir_op_b2f16:
      case nir_op_b2f32:
      case nir_op_mov:
         a0 = src_held(alu, 0, c0); a1 = src_held(alu, 0, c1);
         if (!emit_unary(c, dst, VS_MOV, 0, &a0, &a1, false))
            return false;
         break;
      case nir_op_frcp:
      case nir_op_frsq:
         a0 = src_held(alu, 0, c0); a1 = src_held(alu, 0, c1);
         if (!held_same(&a0, &a1)) {
            /* The chain writes one scalar: each component runs it. */
            if (!scalar_chain(c, alu->op, &a0, &x0) ||
                !scalar_chain(c, alu->op, &a1, &x1) ||
                !emit_unary(c, dst, VS_MOV, 0, &x0, &x1, true))
               return false;
            break;
         }
         if (a0.kind == HELD_LIT) {
            struct held f;
            float v;

            fold_transcendental(alu->op, a0.lit, &v);
            f = held_lit(v);
            if (!write_pair(c, dst, &f, &f))
               return false;
            break;
         }
         if (!(alu->op == nir_op_frcp ? emit_rcp(c, dst, &a0) :
                                        emit_rsq(c, dst, &a0)))
            return false;
         pair = false;
         break;
      case nir_op_fsqrt: {
         /* The argument times its reciprocal square root, with the zero
          * guard the vendor emits, since rsq(0) is an infinity. */
         struct held zero = held_lit(0.0f), hr = held_temp(c);
         struct held hc = held_temp(c), hm = held_temp(c);
         const struct held *sel[3][2];
         int rr, rc, rm;

         struct held hr1;

         a0 = src_held(alu, 0, c0); a1 = src_held(alu, 0, c1);
         if (a0.kind == HELD_LIT && a1.kind == HELD_LIT) {
            struct held f0, f1;
            float v0, v1;

            fold_transcendental(alu->op, a0.lit, &v0);
            fold_transcendental(alu->op, a1.lit, &v1);
            f0 = held_lit(v0);
            f1 = held_lit(v1);
            if (!write_pair(c, dst, &f0, &f1))
               return false;
            break;
         }
         if (!held_same(&a0, &a1)) {
            if (!scalar_chain(c, nir_op_frsq, &a0, &hr) ||
                !scalar_chain(c, nir_op_frsq, &a1, &hr1))
               return false;
            rc = reg_alloc_pinned(c, &hc);
            rm = reg_alloc_pinned(c, &hm);
            if (rc < 0 || rm < 0)
               return fail(c, "out of temporaries");
         } else {
            rr = reg_alloc_pinned(c, &hr);
            rc = reg_alloc_pinned(c, &hc);
            rm = reg_alloc_pinned(c, &hm);
            if (rr < 0 || rc < 0 || rm < 0)
               return fail(c, "out of temporaries");
            if (!emit_rsq(c, rr, &a0))
               return false;
            memset(&c->holds[rr][1], 0, sizeof(c->holds[rr][1]));
            hr1 = hr;
         }
         if (!emit_cmp(c, rc, 4, &a0, &a1, &zero, &zero) ||
             !emit_mul(c, rm, &hr, &hr1, &a0, &a1))
            return false;
         sel[0][0] = sel[0][1] = &hc;
         sel[1][0] = sel[1][1] = &hm;
         sel[2][0] = &a0;
         sel[2][1] = &a1;
         if (!emit_sel(c, dst, 4, sel))
            return false;
         break;
      }
      case nir_op_flog2:
      case nir_op_fexp2: {
         struct held h0 = held_temp(c), h1;
         int r0, r1;
         bool (*chain)(struct ctx *, unsigned, const struct held *) =
            alu->op == nir_op_flog2 ? emit_log2 : emit_exp2;

         a0 = src_held(alu, 0, c0); a1 = src_held(alu, 0, c1);
         /* The chain writes both scalars with one result, so a lane whose
          * two components differ runs it twice and gathers the pair. */
         if (held_same(&a0, &a1)) {
            if (!chain(c, dst, &a0))
               return false;
            break;
         }
         r0 = reg_alloc_pinned(c, &h0);
         if (r0 < 0)
            return fail(c, "out of temporaries");
         if (!chain(c, r0, &a0))
            return false;
         h1 = held_temp(c);
         r1 = reg_alloc_pinned(c, &h1);
         if (r1 < 0)
            return fail(c, "out of temporaries");
         if (!chain(c, r1, &a1) ||
             !emit_unary(c, dst, VS_MOV, 0, &h0, &h1, true))
            return false;
         break;
      }
      case nir_op_fpow: {
         struct held h0 = held_temp(c), h1;
         int r0, r1;

         a0 = src_held(alu, 0, c0); a1 = src_held(alu, 0, c1);
         b0 = src_held(alu, 1, c0); b1 = src_held(alu, 1, c1);
         if (held_same(&a0, &a1) && held_same(&b0, &b1)) {
            if (!emit_pow(c, dst, &a0, &b0))
               return false;
            break;
         }
         r0 = reg_alloc_pinned(c, &h0);
         if (r0 < 0 || !emit_pow(c, r0, &a0, &b0))
            return fail(c, "out of temporaries");
         h1 = held_temp(c);
         r1 = reg_alloc_pinned(c, &h1);
         if (r1 < 0 || !emit_pow(c, r1, &a1, &b1))
            return fail(c, "out of temporaries");
         if (!emit_unary(c, dst, VS_MOV, 0, &h0, &h1, true))
            return false;
         break;
      }
      case nir_op_fsin:
      case nir_op_fcos: {
         /* MEASURED on the M9128: worst error 7.085e-05 against the C
          * library over 1200 arguments spanning [-4pi, 4pi] and the edges
          * of the reduction (docs/parhelia-m9-vertex-isa-gaps.md).  The
          * knob still refuses it, which is how the fixture names the path
          * that drew a picture. */
         if (!debug_get_bool_option("M9X_VS_SINCOS", true))
            return fail(c, nir_op_infos[alu->op].name);
         a0 = src_held(alu, 0, c0); a1 = src_held(alu, 0, c1);
         if (a0.kind == HELD_LIT && a1.kind == HELD_LIT) {
            struct held f0 = held_lit(alu->op == nir_op_fsin ?
                                      sinf(a0.lit) : cosf(a0.lit));
            struct held f1 = held_lit(alu->op == nir_op_fsin ?
                                      sinf(a1.lit) : cosf(a1.lit));

            if (!write_pair(c, dst, &f0, &f1))
               return false;
            break;
         }
         if (!emit_sincos(c, dst, &a0, &a1, alu->op == nir_op_fcos))
            return false;
         break;
      }
      case nir_op_fdiv: {
         struct held hr = held_temp(c);
         int rr;

         a0 = src_held(alu, 0, c0); a1 = src_held(alu, 0, c1);
         b0 = src_held(alu, 1, c0); b1 = src_held(alu, 1, c1);
         if (!held_same(&b0, &b1)) {
            /* Each divisor component gets its own reciprocal. */
            if (!scalar_chain(c, nir_op_frcp, &b0, &x0) ||
                !scalar_chain(c, nir_op_frcp, &b1, &x1) ||
                !emit_mul(c, dst, &a0, &a1, &x0, &x1))
               return false;
            break;
         }
         if (b0.kind == HELD_LIT) {
            struct held f;
            float v;

            fold_transcendental(nir_op_frcp, b0.lit, &v);
            f = held_lit(v);
            if (!emit_mul(c, dst, &a0, &a1, &f, &f))
               return false;
            break;
         }
         rr = reg_alloc_pinned(c, &hr);
         if (rr < 0)
            return fail(c, "out of temporaries");
         if (!emit_rcp(c, rr, &b0))
            return false;
         memset(&c->holds[rr][1], 0, sizeof(c->holds[rr][1]));
         if (!emit_mul(c, dst, &a0, &a1, &hr, &hr))
            return false;
         break;
      }
      case nir_op_fdot2:
      case nir_op_fdot3:
      case nir_op_fdot4: {
         unsigned size = alu->op == nir_op_fdot2 ? 2 :
                         alu->op == nir_op_fdot3 ? 3 : 4;

         if (!build_dot(c, alu, size, dst, &pair))
            return false;
         break;
      }
      default:
         /* nir_op_infos names are static strings */
         return fail(c, nir_op_infos[alu->op].name);
      }
      hold_lane(c, dst, &alu->def, c0, c1, pair);
   }
   return true;
}

/* Attributes and outputs */

struct input_use {
   unsigned location;
   unsigned driver_location;
   unsigned components;
   /* A coefficient slot no vertex element feeds, and the coordinate stream
    * it belongs to; -1 for an ordinary input. */
   int coeff_stream;
};

static bool
note_input(struct ctx *c, struct input_use *used, unsigned *count,
           nir_intrinsic_instr *intr)
{
   unsigned location = nir_intrinsic_io_semantics(intr).location;
   unsigned comps = nir_intrinsic_component(intr) + intr->def.num_components;
   unsigned driver = nir_intrinsic_base(intr);

   if (!nir_src_is_const(intr->src[0]) || nir_src_as_uint(intr->src[0]))
      return fail(c, "indirect vertex input");
   if (comps > 4)
      return fail(c, "wide vertex input");
   for (unsigned i = 0; i < *count; i++) {
      if (used[i].location == location) {
         used[i].components = MAX2(used[i].components, comps);
         return true;
      }
   }
   if (*count >= M9_VS_MAX_SOURCE_ATTRIBUTES)
      return fail(c, "more than sixteen logical vertex attributes");
   used[*count].location = location;
   used[*count].driver_location = driver;
   used[*count].coeff_stream = -1;
   used[(*count)++].components = comps;
   return true;
}

/* The GL compiler's two-component texture descriptor selects mode 1;
 * sample results arrive in r00/r02 before the source-fed ALU head. The
 * runtime additionally requires one resident level and point/no-mip state.
 * Original two-owner GLSL descriptor controls produce that same mode for
 * implicit fetches; vertex implicit LOD selects the base level.
 * Computed coordinates need the vendor's separate coordinate producer. */
static bool
note_texture(struct ctx *c, nir_tex_instr *tex)
{
   int ci = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   int li = nir_tex_instr_src_index(tex, nir_tex_src_lod);
   if (c->texture[1])
      return fail(c, "more than two vertex texture fetches");
   if ((tex->op != nir_texop_tex && tex->op != nir_texop_txl) ||
       tex->sampler_dim != GLSL_SAMPLER_DIM_2D || tex->is_array ||
       tex->is_shadow || tex->coord_components != 2 || ci < 0 ||
       tex->src[ci].src.ssa->bit_size != 32 ||
       tex->src[ci].src.ssa->num_components != 2 ||
       tex->def.num_components != 4 || tex->def.bit_size != 32 ||
       tex->dest_type != nir_type_float32 ||
       tex->texture_index != tex->sampler_index || tex->texture_index >= 8 ||
       tex->num_srcs != (tex->op == nir_texop_txl ? 2 : 1))
      return fail(c, "unsupported vertex texture operation");
   if (tex->op == nir_texop_txl &&
       (li < 0 || !nir_src_is_const(tex->src[li].src) ||
        nir_src_as_float(tex->src[li].src) != 0.0f))
      return fail(c, "vertex texture LOD must be zero");
   nir_scalar x = nir_scalar_chase_movs(nir_get_scalar(tex->src[ci].src.ssa, 0));
   nir_scalar y = nir_scalar_chase_movs(nir_get_scalar(tex->src[ci].src.ssa, 1));
   if (!nir_scalar_is_intrinsic(x) || !nir_scalar_is_intrinsic(y))
      return fail(c, "vertex texture coordinates need a separate producer");
   nir_intrinsic_instr *input = nir_def_as_intrinsic(x.def);
   nir_intrinsic_instr *other = nir_def_as_intrinsic(y.def);
   if (input->intrinsic != nir_intrinsic_load_input ||
       other->intrinsic != nir_intrinsic_load_input ||
       x.def->bit_size != 32 || y.def->bit_size != 32 ||
       nir_intrinsic_component(input) + x.comp != 0 ||
       nir_intrinsic_component(other) + y.comp != 1 ||
       nir_intrinsic_io_semantics(input).location !=
          nir_intrinsic_io_semantics(other).location ||
       nir_intrinsic_base(input) != nir_intrinsic_base(other) ||
       !nir_src_is_const(input->src[0]) || nir_src_as_uint(input->src[0]) ||
       !nir_src_is_const(other->src[0]) || nir_src_as_uint(other->src[0]))
      return fail(c, "vertex texture coordinates must be direct input XY");
   unsigned unit = c->p->texture_count;
   unsigned location = nir_intrinsic_io_semantics(input).location;
   if (unit && location != c->p->texture_location)
      return fail(c, "two vertex textures require the same direct XY input");
   c->texture[unit] = tex;
   c->p->texture_count++;
   if (unit)
      c->p->texture_sampler_second = tex->texture_index;
   else {
      c->p->texture_sampler = tex->texture_index;
      c->p->texture_location = location;
   }
   return true;
}

/* Attribute a is register pair 0x46 + 8 * (a - 1) and the next register;
 * the first holds selectors 0 and 1, the second selectors 2 and 3.  The
 * declaration pads a short input with the default (0, 0, 0, 1), which makes
 * vec4(attribute.xyz, 1.0) a register the program already has.
 *
 * Four attributes are the limit: the vendor's fs-* preprocessing passes
 * declare exactly this, attributes 1..4 at dword offsets of one record, and
 * their programs read 0x46/0x47 through 0x5e/0x5f (CAPTURED).  Attribute 5
 * is texture coordinate set 0 and the vendor's own numbering jumps 4 -> 9,
 * whose register the seven-bit operand field cannot name, so the register
 * rule does not continue past four. */
static void
declare_attributes(struct ctx *c, const struct input_use *used,
                   unsigned count)
{
   struct m9_vs_program *p = c->p;
   unsigned dword = 0;

   memset(p->attrs, 0, sizeof(p->attrs));
   unsigned physical = 0, scalar_slot = ~0u;
   bool packing = count > M9_VS_MAX_ATTRIBUTES;
   for (unsigned input = 0; input < count; input++) {
      unsigned i = physical;
      if (packing && used[input].components == 1 && used[input].coeff_stream < 0) {
         if (scalar_slot == ~0u || p->attrs[scalar_slot].scalar_count == 4) {
            scalar_slot = physical++;
            p->attrs[scalar_slot].location = used[input].location;
            p->attrs[scalar_slot].driver_location = used[input].driver_location;
            p->attrs[scalar_slot].offset = dword;
            p->attrs[scalar_slot].components = 4;
            unsigned reg = REG_ATTR0 + 8 * scalar_slot;
            for (unsigned lane = 0; lane < 4; lane++) {
               p->attrs[scalar_slot].select[lane] = lane;
               struct held h = held_lit(lane == 3 ? 1.0f : 0.0f);
               hold_def(c, reg + lane / 2, lane % 2, &h);
            }
            dword += 4;
         }
         struct m9_vs_attr *a = &p->attrs[scalar_slot];
         unsigned lane = a->scalar_count++;
         a->scalar_location[lane] = used[input].location;
         struct held h = {.kind = HELD_INPUT, .a = used[input].location, .b = 0};
         unsigned reg = REG_ATTR0 + 8 * scalar_slot;
         hold_def(c, reg + lane / 2, lane % 2, &h);
         continue;
      }
      const struct input_use *u = &used[input];
      physical++;
      unsigned reg = REG_ATTR0 + 8 * i;

      p->attrs[i].location = u->location;
      p->attrs[i].driver_location = u->driver_location;
      p->attrs[i].offset = dword;
      /* Every slot selects a record component; the record writer puts the
       * GL default (0, 0, 0, 1) in the components the shader does not read,
       * so the padded literals the head reuses rest on data rather than on
       * declaration selectors 4 and 5, which no captured vertex program is
       * known to read. */
      for (unsigned j = 0; j < 4; j++) {
         struct held h;

         memset(&h, 0, sizeof(h));
         p->attrs[i].select[j] = j;
         if (j < u->components) {
            h.kind = HELD_INPUT;
            h.a = u->location;
            h.b = j;
         } else {
            h.kind = HELD_LIT;
            h.lit = j == 3 ? 1.0f : 0.0f;
         }
         hold_def(c, reg + j / 2, j % 2, &h);
      }
      p->attrs[i].components = u->components;
      p->attrs[i].constant = u->coeff_stream >= 0;
      p->attrs[i].coeff_stream = (uint8_t)MAX2(u->coeff_stream, 0);
      /* One 16-byte slot per attribute: every captured vendor record puts
       * its four attributes at dword offsets 0, 4, 8 and 12. */
      dword += 4;
   }
   p->attr_count = physical;
   p->inputs_read = c->nir->info.inputs_read;
   p->record_bytes = 4 * dword;
}

static struct held
out_held(nir_intrinsic_instr *store, unsigned comp, float dflt)
{
   unsigned base = store ? nir_intrinsic_component(store) : 0;

   if (store && comp >= base && comp - base < store->src[0].ssa->num_components &&
       (nir_intrinsic_write_mask(store) & 1u << (comp - base)))
      return held_of(nir_get_scalar(store->src[0].ssa, comp - base));
   return held_lit(dflt);
}

/* One move covers an output register whose two scalars are one constant
 * pair, or sit side by side in a register already. */
static bool
write_pair(struct ctx *c, unsigned reg, const struct held *h0,
           const struct held *h1)
{
   unsigned src, swz;
   int key, e;

   if (held_same(&c->holds[reg][0], h0) && held_same(&c->holds[reg][1], h1))
      return true;
   if (held_is_const(h0) && held_is_const(h1) &&
       const_pair_swizzled(c, h0, h1, &key, &swz) &&
       (e = const_index(c, key)) >= 0) {
      emit_move_const(c, reg, 0x1c, e, swz);
   } else if (find_pair(c, h0, h1, false, &src, &swz)) {
      emit_move(c, reg, 0x1c, src, swz);
   } else if (c->packed_outputs && c->reserved[reg] &&
              reg >= REG_OUT_LO(0) &&
              reg <= REG_OUT_HI(M9_VS_MAX_ROUTES - 1) && (reg & 1u)) {
      /* Captured GLSL EE19 source producers publish register copies as
       * complete pairs. Mixed scalars must first meet in a normal temporary;
       * scalar register-MOVs into a packed output have no source witness.
       * Partial ALU/constant output stores are separate captured forms. */
      if (!operand(c, h0, h1, false, &src, &swz))
         return false;
      emit_move(c, reg, 0x1c, src, swz);
   } else {
      return write_scalar(c, reg, 0, h0) && write_scalar(c, reg, 1, h1);
   }
   hold_def(c, reg, 0, h0);
   hold_def(c, reg, 1, h1);
   return true;
}

static bool
write_output(struct ctx *c, nir_intrinsic_instr *store, unsigned reg_xy,
             unsigned reg_zw, const float dflt[4])
{
   struct held h[4];

   for (unsigned j = 0; j < 4; j++)
      h[j] = out_held(store, j, dflt[j]);
   if (c->packed_outputs)
      return write_pair(c, reg_zw, &h[2], &h[3]) &&
             write_pair(c, reg_xy, &h[0], &h[1]);
   return write_pair(c, reg_xy, &h[0], &h[1]) &&
          write_pair(c, reg_zw, &h[2], &h[3]);
}

/* The coefficient slot a coordinate stream takes in the source record, or
 * -1 when its pair is a constant of the job's span instead. */
static int
coeff_attr(const struct m9_vs_program *p, unsigned stream)
{
   for (unsigned i = 0; i < p->attr_count; i++)
      if (p->attrs[i].constant && p->attrs[i].coeff_stream == stream)
         return (int)i;
   return -1;
}

static struct held
held_coeff(unsigned stream, unsigned comp)
{
   struct held h;

   memset(&h, 0, sizeof(h));
   h.kind = HELD_INPUT;
   h.a = M9_VS_COEFF_ATTRIB + stream;
   h.b = comp;
   return h;
}

/* A coordinate stream: the pair the sampler reads, taken from wherever in
 * the varying it sits and scaled and biased by the driver's own pair, which
 * is the packing, the affine fold and the render-target span the CPU used
 * to apply per vertex.  The high register is never read; it is written all
 * the same so the stream entry carries no stale register. */
static bool
write_coord_output(struct ctx *c, nir_intrinsic_instr *store, unsigned stream,
                   const struct m9_vs_output *output,
                   unsigned reg_xy, unsigned reg_zw)
{
   struct held h0, h1;
   unsigned rb, sb;
   int scale, bias, es, eb;
   unsigned component = output->component;
   unsigned result_reg = reg_xy;
   if (c->packed_outputs) {
      /* Keep the packed low output as the final store; do not read it back
       * while preparing its companion high half. */
      struct held result = held_temp(c);
      int reg = reg_alloc_pinned(c, &result);
      if (reg < 0)
         return fail(c, "out of temporaries");
      result_reg = reg;
   }

   /* Both scalars have to be values the shader stores; a component it
    * leaves at the GL default is not a coordinate. */
   if (!store || component < nir_intrinsic_component(store) ||
       component + 2 > nir_intrinsic_component(store) +
                       store->src[0].ssa->num_components)
      return fail(c, "the texture coordinate reaches past the varying the "
                  "vertex shader writes");
   h0 = out_held(store, component, 0.0f);
   h1 = out_held(store, component + 1, 0.0f);

   /* Coefficients in the record: the multiply and the add are both
    * register forms, which costs one slot over the constant multiply-add
    * and lets two draws of different spans share a job. */
   if (coeff_attr(c->p, stream) >= 0) {
      struct held s0 = held_coeff(stream, 0), s1 = held_coeff(stream, 1);
      struct held b0 = held_coeff(stream, 2), b1 = held_coeff(stream, 3);
      struct held ht = held_temp(c);
      int t = reg_alloc_pinned(c, &ht);

      if (t < 0)
         return fail(c, "out of temporaries");
      if (!emit_mul(c, t, &h0, &h1, &s0, &s1) ||
          !emit_add(c, result_reg, &ht, &ht, &b0, &b1, false))
         return false;
   } else {
      scale = key_coord(c, stream, M9_VS_COORD_SCALE);
      bias = key_coord(c, stream, M9_VS_COORD_BIAS);
      es = const_index(c, scale);
      eb = const_index(c, bias);
      if (es < 0 || eb < 0)
         return fail(c, "constant span overflow");
      if (!operand(c, &h0, &h1, false, &rb, &sb))
         return false;
      emit(c, w0_make(NONE, rb, eb, result_reg, 0, 0x1c),
           w1_make(VS_MAD_RKK | 1u << 8 | sb << 6, es, NONE));
   }
   memset(c->holds[result_reg], 0, sizeof(c->holds[result_reg]));
   if (output->paired) {
      nir_function_impl *impl = nir_shader_get_entrypoint(c->nir);
      nir_intrinsic_instr *pair = NULL;

      nir_foreach_instr(instr, nir_start_block(impl)) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

         if (intr->intrinsic == nir_intrinsic_store_output &&
             nir_intrinsic_io_semantics(intr).location == output->pair_location)
            pair = intr;
      }
      unsigned pc = output->pair_component;

      if (!pair || pc < nir_intrinsic_component(pair) ||
          pc + 2 > nir_intrinsic_component(pair) + pair->src[0].ssa->num_components)
         return fail(c, "the packed texture pair reaches past its varying");
      struct held p0 = out_held(pair, pc, 0.0f);
      struct held p1 = out_held(pair, pc + 1, 0.0f);

      /* This companion has an identity transform, proved by the layout
       * resolver.  The measured high output pair is a normal two-scalar
       * target; the sampler reads it with declaration selectors 2 and 3. */
      if (!write_pair(c, reg_zw, &p0, &p1))
         return false;
      if (c->packed_outputs) {
         emit_move(c, reg_xy, 0x1c, result_reg, 1);
         memset(c->holds[reg_xy], 0, sizeof(c->holds[reg_xy]));
      }
      return true;
   }
   emit_move(c, reg_zw, 0x1c, result_reg, 1);
   memset(c->holds[reg_zw], 0, sizeof(c->holds[reg_zw]));
   if (c->packed_outputs) {
      emit_move(c, reg_xy, 0x1c, result_reg, 1);
      memset(c->holds[reg_xy], 0, sizeof(c->holds[reg_xy]));
   }
   return true;
}

/* Compilation */

struct use_state {
   struct ctx *c;
   unsigned idx;
};

static bool
note_src(nir_src *src, void *data)
{
   struct use_state *st = data;
   unsigned i = src->ssa->index;

   if (i < st->c->def_count && st->c->last_use[i] < st->idx)
      st->c->last_use[i] = st->idx;
   return true;
}

static void
head_src(struct ctx *c, nir_src *src, unsigned mask, unsigned index)
{
   if (!mask)
      return;
   unsigned def = src->ssa->index;
   c->head_live[def] |= mask;
   c->last_use[def] = MAX2(c->last_use[def], index);
}

static void
head_root(struct ctx *c, nir_intrinsic_instr *store, unsigned final,
          unsigned mask)
{
   if (!store)
      return;
   head_src(c, &store->src[0], mask & nir_intrinsic_write_mask(store), final);
}

/* Only the coordinate pair is read. Invalid pairs keep conservative demand
 * here and retain write_coord_output's existing bounds/refusal path. */
static void
head_pair_root(struct ctx *c, nir_intrinsic_instr *store, unsigned final,
               unsigned component)
{
   if (!store)
      return;
   unsigned base = nir_intrinsic_component(store);
   unsigned n = store->src[0].ssa->num_components;
   unsigned mask = nir_intrinsic_write_mask(store);
   if (component >= base && component - base < n && n - (component - base) >= 2)
      mask = 3u << (component - base);
   head_root(c, store, final, mask);
}

/* Walk from actual output components through the validated single SSA block.
 * Vector/move aliases carry only the requested lanes, so coordinates do not
 * evaluate an unrelated varying packed into the other half. Real ALU still
 * computes its whole vector; keep every source lane its unchanged emitter
 * reads. Original NIR, declarations and intrinsic/control-flow guards stay
 * intact. Each head also keeps its own consumed-source lifetimes. */
static void
head_dependencies(struct ctx *c, nir_block *block, unsigned final,
                  nir_intrinsic_instr *position,
                  nir_intrinsic_instr *const *var,
                  const struct m9_vs_output *ostream, unsigned nvar)
{
   if (!c->head_live)
      return;
   memset(c->head_live, 0, (c->def_count + 1) * sizeof(*c->head_live));
   memset(c->last_use, 0, (c->def_count + 1) * sizeof(*c->last_use));
   head_root(c, position, final, ~0u);
   unsigned count = position ? MIN2(nvar, M9_VS_FUSED_VARYINGS) : nvar;

   for (unsigned j = 0; j < count; j++) {
      if (ostream[j].coordinate)
         head_pair_root(c, var[j], final, ostream[j].component);
      else
         head_root(c, var[j], final, ~0u);
      /* A coordinate route can carry a pair from a different varying.
       * write_coord_output reads that stored source as well. */
      if (ostream[j].coordinate && ostream[j].paired) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_store_output &&
                nir_intrinsic_io_semantics(intr).location ==
                ostream[j].pair_location)
               head_pair_root(c, intr, final, ostream[j].pair_component);
         }
      }
   }
   nir_foreach_instr_reverse(instr, block) {
      if (instr->type != nir_instr_type_alu)
         continue;
      nir_alu_instr *alu = nir_instr_as_alu(instr);
      unsigned wanted = c->head_live[alu->def.index];
      if (!wanted)
         continue;
      unsigned index = instr->index;
      if (c->fold[alu->def.index] >= 0 || nir_op_is_vec_or_mov(alu->op))
         index = c->last_use[alu->def.index];
      for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++) {
         unsigned mask = 0;
         if (nir_op_is_vec(alu->op)) {
            if (wanted & (1u << i))
               mask = 1u << alu->src[i].swizzle[0];
         } else {
            unsigned fixed = nir_op_infos[alu->op].input_sizes[i];
            unsigned lanes = fixed ? fixed : alu->def.num_components;
            for (unsigned lane = 0; lane < lanes; lane++)
               if (fixed || alu->op != nir_op_mov || (wanted & (1u << lane)))
                  mask |= 1u << alu->src[i].swizzle[lane];
         }
         head_src(c, &alu->src[i].src, mask, index);
      }
   }
}

/* Output stream j of this head, whichever kind it is. */
static bool
write_stream(struct ctx *c, nir_intrinsic_instr *store,
             const struct m9_vs_output *ostream, unsigned base, unsigned j,
             unsigned reg_xy, unsigned reg_zw, bool present)
{
   static const float dflt[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

   if (present && ostream[j].coordinate)
      return write_coord_output(c, store, base + j, &ostream[j],
                                reg_xy, reg_zw);
   return write_output(c, present ? store : NULL, reg_xy, reg_zw, dflt);
}

/* One head: the register file starts at the declared attributes, its live
 * ALU dependencies are emitted, and the outputs land last.  A program
 * with several varyings runs this twice -- once for the fused pass, once for
 * the tail-free extra pass -- because each is a program of its own and the
 * hardware has no path from one to the other. */
static bool
compile_head(struct ctx *c, nir_block *block, unsigned final,
             const struct input_use *used, unsigned inputs,
             nir_intrinsic_instr *position,
             nir_intrinsic_instr *const *var,
             const struct m9_vs_output *ostream, unsigned base, unsigned nvar,
             bool packed_primary)
{
   static const float dflt[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

   memset(c->holds, 0, sizeof(c->holds));
   memset(c->reserved, 0, sizeof(c->reserved));
   memset(c->busy, 0, sizeof(c->busy));
   c->pin_count = 0;
   c->packed_outputs = packed_primary || !position;
   c->temp_seq = 0;
   c->temp_base = 0;
   c->index = 0;
   c->temp_live = false;
   head_dependencies(c, block, final, position, var, ostream, nvar);
   if (c->p->producer.size || packed_primary || c->sampled_position) {
      /* Tail-free source draws have no clip-code target. Keep its register
       * and pair 16 outside the 16-bit availability mask out of the pool. */
      c->reserved[0x20] = c->reserved[0x21] = 1;
   }
   if (packed_primary) {
      for (unsigned j = 0; j < M9_VS_MAX_ROUTES; j++) {
         c->reserved[REG_OUT_LO(j)] = 1;
         c->reserved[REG_OUT_HI(j)] = 1;
      }
      pin_packed_output(c, position, REG_OUT_LO(0), REG_OUT_HI(0));
      for (unsigned j = 0; j < M9_VS_FUSED_VARYINGS; j++)
         if (!ostream[j].coordinate) {
            pin_packed_output(c, var[j], REG_OUT_LO(j + 1), REG_OUT_HI(j + 1));
         }
   } else if (position) {
      bool coord0 = nvar && ostream[0].coordinate;

      pin_output(c, position, 0, REG_POS_XY);
      pin_output(c, position, 2, REG_POS_ZW);
      pin_output(c, nvar && !coord0 ? var[0] : NULL, 0, REG_VAR_XY);
      pin_output(c, nvar && !coord0 ? var[0] : NULL, 2, REG_VAR_ZW);
      /* The second stream takes the clip position's output pair, which
       * m9_vs_emit frees by pointing the tail's two writes to it at the
       * tail's own scratch. */
      if (nvar > 1) {
         c->reserved[REG_OUT_LO(M9_VS_FUSED_PAIR)] = 1;
         c->reserved[REG_OUT_HI(M9_VS_FUSED_PAIR)] = 1;
         if (!ostream[1].coordinate) {
            pin_output(c, var[1], 0, REG_OUT_LO(M9_VS_FUSED_PAIR));
            pin_output(c, var[1], 2, REG_OUT_HI(M9_VS_FUSED_PAIR));
         }
      }
   } else {
      /* Every output pair is reserved, not just the ones this head writes:
       * a pair above its varying count belongs to an unbound target and
       * must not end up holding a temporary. */
      for (unsigned j = 0; j < M9_VS_MAX_ROUTES; j++) {
         c->reserved[REG_OUT_LO(j)] = 1;
         c->reserved[REG_OUT_HI(j)] = 1;
      }
      for (unsigned j = 0; j < nvar; j++)
         if (!ostream[j].coordinate) {
            pin_packed_output(c, var[j], REG_OUT_LO(j), REG_OUT_HI(j));
         }
   }
   declare_attributes(c, used, inputs);
   if (c->p->texture_head_only && (packed_primary || c->unsampled_stage)) {
      for (unsigned unit = 0; unit < c->p->texture_count; unit++)
         if (c->head_live[c->texture[unit]->def.index])
            return fail(c, "packed FIRST requires texture-independent outputs");
   }
   if (c->texture[0] && !c->unsampled_stage &&
       (!c->p->texture_head_only || !packed_primary)) {
      /* Unit 0 supplies r00/r02 and unit 1 r04/r06. Keep every raw bank
       * protected until all sampled SSA definitions have been preserved:
       * the ordinary allocator's first temporary is r04. */
      if (c->texture[1])
         c->reserved[4] = c->reserved[6] = 1;
      for (unsigned unit = 0; unit < c->p->texture_count; unit++) {
         for (unsigned pair = 0; pair < 2; pair++) {
            int dst = reg_alloc(c);
            if (dst < 0)
               return fail(c, "register pressure preserving a vertex texture");
            emit_move(c, dst, 0x1c, 4 * unit + 2 * pair, 1);
            struct held h0 = { .kind = HELD_DEF,
                              .a = c->texture[unit]->def.index, .b = 2 * pair };
            struct held h1 = h0;
            h1.b++;
            hold_def(c, dst, 0, &h0);
            hold_def(c, dst, 1, &h1);
         }
      }
      if (c->texture[1])
         c->reserved[4] = c->reserved[6] = 0;
   }
   nir_foreach_instr(instr, block) {
      c->index = instr->index;
      c->temp_live = true;
      c->temp_base = c->temp_seq;
      memset(c->busy, 0, sizeof(c->busy));
      if (instr->type == nir_instr_type_alu &&
          (!c->head_live || c->head_live[nir_instr_as_alu(instr)->def.index]) &&
          !build_alu(c, nir_instr_as_alu(instr)))
         return false;
      c->temp_live = false;
   }
   c->index = final;
   c->temp_live = true;
   c->temp_base = c->temp_seq;
   memset(c->busy, 0, sizeof(c->busy));
   if (packed_primary) {
      if (!write_output(c, position, REG_OUT_LO(0), REG_OUT_HI(0), dflt))
         return false;
      for (unsigned j = 0; j < M9_VS_FUSED_VARYINGS; j++)
         if (!write_stream(c, var[j], ostream, base, j, REG_OUT_LO(j + 1),
                           REG_OUT_HI(j + 1), true))
            return false;
   } else if (position) {
      if (!write_output(c, position, REG_POS_XY, REG_POS_ZW, dflt))
         return false;
      if (!write_stream(c, nvar ? var[0] : NULL, ostream, base, 0,
                        REG_VAR_XY, REG_VAR_ZW, nvar != 0))
         return false;
      if (nvar > 1 &&
          !write_stream(c, var[1], ostream, base, 1,
                        REG_OUT_LO(M9_VS_FUSED_PAIR),
                        REG_OUT_HI(M9_VS_FUSED_PAIR), true))
         return false;
   } else {
      for (unsigned j = 0; j < nvar; j++)
         if (!write_stream(c, var[j], ostream, base, j, REG_OUT_LO(j),
                           REG_OUT_HI(j), true))
            return false;
   }
   c->temp_live = false;
   return true;
}

static bool
compile_impl(struct ctx *c, nir_function_impl *impl,
             const struct m9_vs_outputs *want)
{
   struct input_use used[M9_VS_MAX_SOURCE_ATTRIBUTES];
   nir_intrinsic_instr *position = NULL;
   nir_intrinsic_instr *stored[M9_VS_MAX_VARYINGS] = { NULL };
   nir_intrinsic_instr *varying[M9_VS_MAX_VARYINGS] = { NULL };
   struct m9_vs_outputs out;
   unsigned nstored = 0, nvar = 0;
   unsigned inputs = 0, final;
   nir_block *block;

   if (exec_list_length(&impl->body) != 1)
      return fail(c, "control flow");
   nir_index_ssa_defs(impl);
   nir_index_instrs(impl);
   c->def_count = impl->ssa_alloc;
   c->last_use = calloc(c->def_count + 1, sizeof(*c->last_use));
   c->fold = malloc(c->def_count + 1);
   if (!c->last_use || !c->fold)
      return fail(c, "out of memory");
   memset(c->fold, -1, c->def_count + 1);
   block = nir_start_block(impl);
   final = exec_list_length(&block->instr_list);

   nir_foreach_instr(instr, block) {
      nir_intrinsic_instr *intr;

      if (instr->type == nir_instr_type_alu ||
          instr->type == nir_instr_type_load_const)
         continue;
      if (instr->type == nir_instr_type_tex) {
         if (c->faa)
            return fail(c, "FAA vertex sampler interface");
         if (!note_texture(c, nir_instr_as_tex(instr)))
            return false;
         continue;
      }
      if (instr->type != nir_instr_type_intrinsic)
         return fail(c, "unsupported instruction");
      intr = nir_instr_as_intrinsic(instr);
      switch (intr->intrinsic) {
      case nir_intrinsic_load_input:
         if (!note_input(c, used, &inputs, intr))
            return false;
         break;
      case nir_intrinsic_load_ubo:
      case nir_intrinsic_load_ubo_vec4:
         if (!nir_src_is_const(intr->src[0]) || nir_src_as_uint(intr->src[0]) ||
             !nir_src_is_const(intr->src[1]))
            return fail(c, "indirect constant buffer read");
         break;
      case nir_intrinsic_store_output: {
         unsigned location = nir_intrinsic_io_semantics(intr).location;

         if (!nir_src_is_const(intr->src[1]) || nir_src_as_uint(intr->src[1]))
            return fail(c, "indirect vertex output");
         if (location == VARYING_SLOT_POS) {
            if (position)
               return fail(c, "more than one position");
            position = intr;
         } else if (location == VARYING_SLOT_PSIZ) {
            /* The preprocessing pass has no point-size output; the linker
             * writes the default for every compatibility shader, and a
             * constant one changes nothing. */
            nir_scalar s = nir_scalar_resolved(intr->src[0].ssa, 0);

            if (!nir_scalar_is_const(s) || nir_scalar_as_float(s) != 1.0f)
               return fail(c, "point size");
         } else if (location >= VARYING_SLOT_CLIP_DIST0 &&
                    location <= VARYING_SLOT_CULL_DIST1) {
            return fail(c, "clip distance");
         } else {
            unsigned v;

            for (v = 0; v < nstored; v++)
               if (nir_intrinsic_io_semantics(stored[v]).location == location)
                  return fail(c, "a varying stored more than once");
            if (nstored >= M9_VS_MAX_VARYINGS)
               return fail(c, "more than five varyings");
            stored[nstored++] = intr;
         }
         break;
      }
      default:
         /* intrinsic names are static strings */
         return fail(c, nir_intrinsic_infos[intr->intrinsic].name);
      }
   }
   if (!position)
      return fail(c, "no position output");
   /* The streams this program writes: the caller's, or one an output the
    * shader stores, which is what every non-textured job asks for. */
   memset(&out, 0, sizeof(out));
   if (want) {
      if (want->count > M9_VS_MAX_VARYINGS)
         return fail(c, "more output streams than the passes carry");
      out = *want;
   } else {
      for (unsigned v = 0; v < nstored; v++)
         out.stream[out.count++] = (struct m9_vs_output) {
            .location = nir_intrinsic_io_semantics(stored[v]).location,
         };
   }
   for (unsigned j = 0; j < out.count; j++) {
      for (unsigned v = 0; v < nstored; v++)
         if (nir_intrinsic_io_semantics(stored[v]).location ==
             out.stream[j].location)
            varying[j] = stored[v];
      if (!varying[j])
         return fail(c, "a stream the shader writes no varying for");
   }
   nvar = out.count;
   if (c->sampled_position) {
      if (nvar < 3 || nvar > 5 || c->p->texture_count != 2)
         return fail(c, "sampled position requires two fetches and three to five streams");
      for (unsigned j = 0; j < nvar; j++) {
         unsigned components = varying[j]->src[0].ssa->num_components;
         if (out.stream[j].coordinate || out.stream[j].coeff_record ||
             out.stream[j].paired || nir_intrinsic_component(varying[j]) ||
             varying[j]->src[0].ssa->bit_size != 32 ||
             components < 2 || components > 4 ||
             nir_intrinsic_write_mask(varying[j]) != (1u << components) - 1u)
            return fail(c, "sampled position requires direct vector streams");
         /* The existing packed writer publishes a complete 16-byte field:
          * live lanes retain the shader's values, missing Z/W use (0,1).
          * GATHER/LAST and the immutable field maps keep that same layout;
          * varying_components records only the live semantic width. */
      }
      if (nir_intrinsic_component(position) ||
          nir_intrinsic_write_mask(position) != 15 ||
          position->src[0].ssa->num_components != 4)
         return fail(c, "sampled position requires a complete clip position");
   }
   if (c->faa) {
      if (nvar < 1 || nvar > 2)
         return fail(c, "FAA requires one or two direct output streams");
      if (nir_intrinsic_component(position) ||
          nir_intrinsic_write_mask(position) != 0xf ||
          position->src[0].ssa->num_components != 4)
         return fail(c, "FAA requires a complete clip position");
      for (unsigned j = 0; j < nvar; j++) {
         if (out.stream[j].coordinate || out.stream[j].coeff_record ||
             out.stream[j].paired)
            return fail(c, "FAA coordinate stream interface");
         if (nir_intrinsic_component(varying[j]) ||
             nir_intrinsic_write_mask(varying[j]) != 0xf ||
             varying[j]->src[0].ssa->num_components != 4)
            return fail(c, "FAA requires complete vec4 streams");
      }
   }
   if (c->texture[0] && nvar > M9_VS_FUSED_VARYINGS) {
      c->p->texture_head_only = true;
   }
   if (nvar > M9_VS_FUSED_VARYINGS) {
      c->head_live = calloc(c->def_count + 1, sizeof(*c->head_live));
      if (!c->head_live)
         return fail(c, "out of memory");
   }
   /* A coordinate stream whose coefficients move between draws reads them
    * from a record slot of its own, so that a draw sampling a differently
    * addressed target appends records to the pending job instead of ending
    * it on a changed constant span. */
   for (unsigned j = 0; j < out.count; j++) {
      if (!out.stream[j].coordinate || !out.stream[j].coeff_record)
         continue;
      if (inputs >= M9_VS_MAX_ATTRIBUTES)
         return fail(c, "no record slot left for a coordinate's "
                     "coefficients");
      used[inputs].location = M9_VS_COEFF_ATTRIB + j;
      used[inputs].driver_location = 0;
      used[inputs].components = 4;
      used[inputs++].coeff_stream = (int)j;
   }

   if (inputs > M9_VS_MAX_ATTRIBUTES) {
      unsigned scalar = 0;
      for (unsigned i = 0; i < inputs; i++)
         scalar += used[i].components == 1 && used[i].coeff_stream < 0;
      unsigned physical = inputs - scalar + DIV_ROUND_UP(scalar, 4);
      /* Existing four-slot programs remain byte-exact. FAA and vertex
       * sampling keep their separately source-qualified input interfaces. */
      if (c->faa || c->texture[0] || physical > M9_VS_MAX_ATTRIBUTES)
         return fail(c, "more than four vertex attributes");
   }

   nir_foreach_instr(instr, block) {
      nir_alu_instr *alu;

      if (instr->type != nir_instr_type_alu)
         continue;
      alu = nir_instr_as_alu(instr);
      if (alu->op != nir_op_fadd)
         continue;
      bool fused = false;
      for (unsigned i = 0; i < 2; i++) {
         nir_alu_instr *mul = fusable_mul(alu, i, alu->def.num_components);
         unsigned lanes = (alu->def.num_components + 1) / 2;
         int side = -1;

         for (unsigned lane = 0; mul && lane < lanes; lane++) {
            unsigned c0 = 2 * lane;
            unsigned c1 = MIN2(c0 + 1, alu->def.num_components - 1);
            int this_side = mul_const_side(mul, c0, c1);

            if (this_side < 0 || (side >= 0 && this_side != side))
               mul = NULL;
            else
               side = this_side;
         }
         if (mul && side >= 0) {
            c->fold[mul->def.index] = side;
            fused = true;
            break;
         }
      }
      if (!fused) {
         for (unsigned i = 0; i < 2; i++) {
            nir_alu_instr *neg = fusable_neg(alu, i, alu->def.num_components);
            if (neg) {
               c->fold[neg->def.index] = FOLD_NEG;
               break;
            }
         }
      }
   }

   nir_foreach_instr_reverse(instr, block) {
      struct use_state st = { c, instr->index };

      if (instr->type == nir_instr_type_alu &&
          c->fold[nir_instr_as_alu(instr)->def.index] >= 0)
         st.idx = c->last_use[nir_instr_as_alu(instr)->def.index];
      else if (instr->type == nir_instr_type_alu &&
          nir_op_is_vec_or_mov(nir_instr_as_alu(instr)->op))
         st.idx = c->last_use[nir_instr_as_alu(instr)->def.index];
      else if (instr->type == nir_instr_type_intrinsic &&
               nir_instr_as_intrinsic(instr)->intrinsic ==
               nir_intrinsic_store_output)
         st.idx = final;
      nir_foreach_src(instr, note_src, &st);
   }

   bool route_fed = c->faa || nvar > M9_VS_FUSED_VARYINGS;
   c->out = route_fed ? &c->p->producer : &c->p->slots;
   if (c->sampled_position) {
      /* Original public sampled-position controls publish two unsampled
       * fields in FIRST, then position and three fields in CONTINUE. Select
       * that partition from live requested roots, never source field numbers. */
      nir_intrinsic_instr *first[2] = {0}, *continuation[4] = { position };
      struct m9_vs_output first_out[2] = {{0}}, continuation_out[4] = {{0}};
      unsigned chosen = 0, remaining = 1;
      bool selected[M9_VS_MAX_VARYINGS] = {0};
      head_dependencies(c, block, final, position, NULL, NULL, 0);
      bool position_sampled = false;
      for (unsigned unit = 0; unit < c->p->texture_count; unit++)
         position_sampled |= c->head_live[c->texture[unit]->def.index] != 0;
      if (!position_sampled)
         return fail(c, "sampled position has no sampled clip dependency");
      for (unsigned j = 0; j < nvar && chosen < 2; j++) {
         head_dependencies(c, block, final, NULL, &varying[j], &out.stream[j], 1);
         bool sampled = false;
         for (unsigned unit = 0; unit < c->p->texture_count; unit++)
            sampled |= c->head_live[c->texture[unit]->def.index] != 0;
         if (!sampled) {
            first[chosen] = varying[j];
            first_out[chosen] = out.stream[j];
            c->p->producer_fields[chosen++] = j + 1;
            selected[j] = true;
         }
      }
      /* Linker-packed vec2/vec3 outputs may occupy only three or four
       * physical streams. Preserve the measured two FIRST + four CONTINUE
       * targets and six-field record. An unused field is explicitly written
       * by write_stream's existing (0,0,0,1) path, never an opaque input or
       * an alias of another field. Five-stream programs stay exact. */
      unsigned padding = nvar + 1;
      while (chosen < 2 && padding < 6)
         c->p->producer_fields[chosen++] = padding++;
      if (chosen != 2)
         return fail(c, "sampled position needs two unsampled requested streams");
      c->p->extra_fields[0] = 0;
      for (unsigned j = 0; j < nvar; j++) {
         if (selected[j])
            continue;
         continuation[remaining] = varying[j];
         continuation_out[remaining] = out.stream[j];
         c->p->extra_fields[remaining++] = j + 1;
      }
      while (remaining < 4 && padding < 6)
         c->p->extra_fields[remaining++] = padding++;
      assert(remaining == 4);
      c->p->sampled_position = true;
      c->unsampled_stage = true;
      if (!compile_head(c, block, final, used, inputs, NULL, first,
                        first_out, 0, 2, false))
         return false;
      c->unsampled_stage = false;
      c->out = &c->p->extra;
      if (!compile_head(c, block, final, used, inputs, NULL, continuation,
                        continuation_out, 0, 4, false))
         return false;
   } else if (!compile_head(c, block, final, used, inputs, position, varying,
                           out.stream, 0, nvar, route_fed))
      return false;
   /* Varyings past the two the fused pass carries are a program of their
    * own, run as the preprocessing draw ahead of it.  Each independently
    * emits the SSA dependencies of its output targets. */
   if (nvar > M9_VS_FUSED_VARYINGS) {
      c->out = &c->p->extra;
      if (!c->sampled_position && !compile_head(c, block, final, used, inputs, NULL,
                        varying + M9_VS_FUSED_VARYINGS,
                        out.stream + M9_VS_FUSED_VARYINGS,
                        M9_VS_FUSED_VARYINGS, nvar - M9_VS_FUSED_VARYINGS, false))
         return false;
      compile_gather(c->p, c->sampled_position ? M9_VS_EXTRA_VARYINGS :
                                               nvar - M9_VS_FUSED_VARYINGS);
      /* The final draw reads the packed position and first two streams
       * through routes 0..2. Preserve route 0's ZW before replacing r02
       * with varying 0's XY. The epilogue consumes the ordinary register
       * convention; it does not rerun the original attribute shader. */
      c->out = &c->p->slots;
      static const unsigned from[] = { 2, 4, 6, 8, 10 };
      static const unsigned to[] = { REG_POS_ZW, REG_VAR_XY, REG_VAR_ZW,
                                    REG_OUT_LO(M9_VS_FUSED_PAIR),
                                    REG_OUT_HI(M9_VS_FUSED_PAIR) };
      for (unsigned i = 0; i < ARRAY_SIZE(from); i++)
         emit(c, w0_make(NONE, NONE, from[i], to[i], 0, 0x1c),
                 w1_make(VS_MOV | 1u << 4, NONE, NONE));
   }
   c->p->outputs = out;
   c->p->varying_count = nvar;
   for (unsigned v = 0; v < nvar; v++) {
      c->p->varying_location[v] = out.stream[v].location;
      c->p->varying_components[v] =
         out.stream[v].coordinate ? (out.stream[v].paired ? 4 : 2) :
         nir_intrinsic_component(varying[v]) +
         varying[v]->src[0].ssa->num_components;
   }
   return true;
}

static uint32_t
fnv1a(uint32_t h, const void *data, size_t size)
{
   const uint8_t *b = data;

   for (size_t i = 0; i < size; i++)
      h = (h ^ b[i]) * 16777619u;
   return h;
}

/* M9X_VS_DUMP=1: one line a compiled program, so two builds can be shown to
 * emit the same words for the same shader and layout; 2 adds the words. */
static void
dump_program(const nir_shader *nir, const struct m9_vs_outputs *outputs,
             const struct m9_vs_program *p)
{
   const uint32_t *w = p->slots.data;
   unsigned n = util_dynarray_num_elements(&p->slots, uint32_t), maxreg = 0;
   uint32_t h = 2166136261u, lh = 2166136261u;

   for (unsigned i = 0; i + 1 < n; i += 2)
      maxreg = MAX2(maxreg, w[i] >> 8 & 0x7f);
   h = fnv1a(h, p->slots.data, p->slots.size);
   h = fnv1a(h, p->extra.data, p->extra.size);
   h = fnv1a(h, p->gather.data, p->gather.size);
   h = fnv1a(h, p->producer.data, p->producer.size);
   h = fnv1a(h, p->consts.data, p->consts.size);
   if (outputs)
      lh = fnv1a(lh, outputs, sizeof(*outputs));
   fprintf(stderr, "m9x vs dump: src %02x%02x%02x%02x layout %08x slots %u+%u+%u "
           "consts %u maxreg %02x words %08x producer %u\n",
           nir->info.source_blake3[0], nir->info.source_blake3[1],
           nir->info.source_blake3[2], nir->info.source_blake3[3],
           outputs ? lh : 0, n / 2,
           (unsigned)util_dynarray_num_elements(&p->extra, uint32_t) / 2,
           (unsigned)util_dynarray_num_elements(&p->gather, uint32_t) / 2,
           (unsigned)util_dynarray_num_elements(&p->consts, struct m9_vs_const),
           maxreg,
           h, (unsigned)p->producer.size / 8);
   if (debug_get_num_option("M9X_VS_DUMP", 0) > 1)
      for (unsigned i = 0; i + 1 < n; i += 2)
         fprintf(stderr, "m9x vs dump: %08x %3u  %08x %08x\n", h, i / 2, w[i],
                 w[i + 1]);
}

/* Component folding exposes the fixed-zero cross-product lanes and unused
 * matrix components of geometry shaders.  Keep ordinary scalar/vector math
 * on its existing path; only a register dot needs this preparation. */
static bool
geometry_dots(nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_alu)
               continue;
            nir_alu_instr *alu = nir_instr_as_alu(instr);
            if ((alu->op == nir_op_fdot2 || alu->op == nir_op_fdot3 ||
                 alu->op == nir_op_fdot4) &&
                !nir_src_is_const(alu->src[0].src) &&
                !nir_src_is_const(alu->src[1].src))
               return true;
         }
      }
   }
   return false;
}

static bool
geometry_scalar(const nir_instr *instr, const void *data)
{
   (void)data;
   const nir_alu_instr *alu = nir_instr_as_alu(instr);
   return alu->def.num_components > 1;
}

/* no_integers lowers public float-to-int casts to ftrunc after st's
 * algebraic pass. Use the same existing floor/ceil/select forms as the FS
 * backend: finite nonnegative values round down, negative ones round up. */
static bool
lower_vertex_trunc(nir_builder *b, nir_alu_instr *alu, void *data)
{
   (void)data;
   if (alu->op != nir_op_ftrunc || alu->def.bit_size != 32)
      return false;
   b->cursor = nir_before_instr(&alu->instr);
   nir_def *x = nir_ssa_for_alu_src(b, alu, 0);
   nir_def_replace(&alu->def,
                  nir_bcsel(b, nir_flt_imm(b, x, 0.0f), nir_fceil(b, x),
                            nir_ffloor(b, x)));
   return true;
}

static void
prepare_geometry(nir_shader *nir)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);

   if (exec_list_length(&impl->body) != 1 || !geometry_dots(nir))
      return;
   /* Use Mesa's own reciprocal/sqrt contraction rules and fast-math guards;
    * restore the shader's shared compiler options before returning. */
   const nir_shader_compiler_options *original = nir->options;
   nir_shader_compiler_options options = *original;
   options.lower_fdiv = true;
   nir->options = &options;
   NIR_PASS(_, nir, nir_lower_alu_to_scalar, geometry_scalar, NULL);
   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, nir, nir_opt_constant_folding);
      NIR_PASS(progress, nir, nir_opt_algebraic);
      NIR_PASS(progress, nir, nir_opt_copy_prop);
      NIR_PASS(progress, nir, nir_opt_cse);
      NIR_PASS(progress, nir, nir_opt_dce);
   } while (progress);
   NIR_PASS(_, nir, nir_opt_vectorize, NULL, NULL);
   /* Repacking enables the two-scalar native instructions; schedule the
    * resulting graph for the existing finite temporary pool. */
   const nir_schedule_options schedule = {.threshold = 36, .fallback = true};
   nir_schedule(nir, &schedule);
   nir->options = original;
}

/* The seed comes from this compile's own typed keys and consumer tracking.
 * Pack only scalar uniforms that no identity-only operand consumes. No new
 * UBO offset, literal payload, coordinate or captured index is introduced. */
static bool
seed_constants(struct ctx *c, const struct const_seed *seed)
{
   uint32_t pending = M9_VS_UNIFORM_ZERO;

   util_dynarray_foreach(&seed->program->consts, struct m9_vs_const, k) {
      unsigned i = k - (struct m9_vs_const *)seed->program->consts.data;

      if (k->kind != M9_VS_CONST_UNIFORM || k->offset[0] != k->offset[1] ||
          k->offset[0] == M9_VS_UNIFORM_ZERO || seed->identity[i])
         continue;
      if (pending == M9_VS_UNIFORM_ZERO) {
         pending = k->offset[0];
      } else {
         if (key_uniform(c, pending, k->offset[0]) < 0)
            return fail(c, "constant seed overflow");
         pending = M9_VS_UNIFORM_ZERO;
      }
   }
   if (pending != M9_VS_UNIFORM_ZERO && key_uniform(c, pending, pending) < 0)
      return fail(c, "constant seed overflow");
   return true;
}

static bool
compile_program(struct nir_shader *nir, const struct m9_vs_outputs *outputs,
                struct m9_vs_program *p, const char **reason,
                bool geometry, const struct const_seed *seed,
                bool *identity, bool faa, bool sampled_position)
{
   nir_function_impl *impl;
   struct ctx c;
   bool ok;

   memset(&c, 0, sizeof(c));
   memset(p, 0, sizeof(*p));
   util_dynarray_init(&p->slots, NULL);
   util_dynarray_init(&p->extra, NULL);
   util_dynarray_init(&p->gather, NULL);
   util_dynarray_init(&p->producer, NULL);
   util_dynarray_init(&p->consts, NULL);
   util_dynarray_init(&c.keys, NULL);
   c.identity_keys = identity;
   c.faa = faa;
   c.sampled_position = sampled_position;
   c.compact = faa || seed != NULL;
   c.nir = nir;
   c.p = p;
   if (nir->info.stage == MESA_SHADER_VERTEX) {
      NIR_PASS(_, nir, nir_shader_alu_pass, lower_vertex_trunc,
               nir_metadata_control_flow, NULL);
      m9_nir_lower_alu_if(nir);
      if (geometry)
         prepare_geometry(nir);
   }
   impl = nir->info.stage == MESA_SHADER_VERTEX ?
          nir_shader_get_entrypoint(nir) : NULL;
   ok = (!seed || seed_constants(&c, seed)) &&
        (impl ? compile_impl(&c, impl, outputs) :
                fail(&c, "not a vertex shader"));
   if (ok) {
      p->faa_preprocess = faa;
      util_dynarray_foreach(&c.keys, struct kkey, k) {
         struct m9_vs_const *out =
            util_dynarray_grow(&p->consts, struct m9_vs_const, 1);

         memset(out, 0, sizeof(*out));
         out->kind = k->kind;
         out->value[0] = k->v[0];
         out->value[1] = k->pair ? k->v[1] : k->v[0];
         out->offset[0] = k->off[0];
         out->offset[1] = k->pair ? k->off[1] : k->off[0];
         out->coord_stream = k->coord_stream;
         out->coord_part = k->coord_part;
      }
   }
   if (ok && debug_get_num_option("M9X_VS_DUMP", 0) > 0)
      dump_program(nir, outputs, p);
   if (reason)
      *reason = ok ? NULL : (c.reason ? c.reason : "unknown");
   util_dynarray_fini(&c.keys);
   free(c.last_use);
   free(c.head_live);
   free(c.fold);
   if (!ok)
      m9_vs_program_fini(p);
   return ok;
}

bool
m9_vs_compile(struct nir_shader *nir, const struct m9_vs_outputs *outputs,
              struct m9_vs_program *p, const char **reason)
{
   return compile_program(nir, outputs, p, reason, true, NULL, NULL, false, false);
}

bool
m9_vs_compile_sampled_position(struct nir_shader *nir,
                               const struct m9_vs_outputs *outputs,
                               struct m9_vs_program *p, const char **reason)
{
   if (!nir || !p) {
      if (reason)
         *reason = "missing sampled-position shader/program";
      return false;
   }
   return compile_program(nir, outputs, p, reason, true, NULL, NULL, false, true);
}

/* This offset exists only on this private compilation clone. Original UBO
 * loads are checked before insertion; the resulting lane is tagged after
 * compilation and never interpreted as an application memory address. */
#define M9_PADDING_SELECTOR_OFFSET 0xfffffff0u

struct gpu_padding_state {
   nir_io_semantics input;
   unsigned stores;
   bool invalid;
};

static bool
gpu_padding_position(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   struct gpu_padding_state *s = data;
   if (intr->intrinsic != nir_intrinsic_store_output ||
       nir_intrinsic_io_semantics(intr).location != VARYING_SLOT_POS)
      return false;
   if (nir_intrinsic_component(intr) || nir_intrinsic_write_mask(intr) != 0xf ||
       intr->src[0].ssa->bit_size != 32 || intr->src[0].ssa->num_components != 4) {
      s->invalid = true;
      return false;
   }
   s->stores++;
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *zero = nir_imm_int(b, 0);
   nir_def *xy = nir_load_input(b, 2, 32, zero, .base = 0, .component = 0,
                                .dest_type = nir_type_float32,
                                .io_semantics = s->input);
   nir_def *flag = nir_load_ubo(b, 1, 32, zero,
                               nir_imm_int(b, M9_PADDING_SELECTOR_OFFSET),
                               .align_mul = 4,
                               .range_base = M9_PADDING_SELECTOR_OFFSET,
                               .range = 4);
   nir_def *pad = nir_vec4(b, nir_channel(b, xy, 0), nir_channel(b, xy, 1),
                          nir_imm_float(b, 1.0f), nir_imm_float(b, 1.0f));
   nir_src_rewrite(&intr->src[0],
                  nir_bcsel(b, nir_fneu(b, flag, nir_imm_float(b, 0.0f)),
                            pad, intr->src[0].ssa));
   return true;
}

bool
m9_vs_compile_gpu_padding(const struct nir_shader *nir,
                          const struct m9_vs_outputs *outputs,
                          bool sampled_position,
                          unsigned max_slots, unsigned max_const_pairs,
                          struct m9_vs_program *p, const char **reason)
{
   const char *why = "GPU padding requires direct float32 source slot0 XY";
   struct gpu_padding_state s = {0};
   bool input = false;
   if (!nir || !p || nir->info.stage != MESA_SHADER_VERTEX || !max_slots ||
       !max_const_pairs || max_const_pairs > M9_VS_MAX_CONST_PAIRS)
      goto refuse;
   if (outputs) {
      if (outputs->count > M9_VS_MAX_VARYINGS)
         goto refuse;
      for (unsigned v = 0; v < outputs->count; v++)
         if (outputs->stream[v].coordinate || outputs->stream[v].paired ||
             outputs->stream[v].coeff_record)
            goto refuse;
   }
   nir_foreach_function_impl(impl, nir) nir_foreach_block(block, impl)
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic == nir_intrinsic_load_ubo ||
             intr->intrinsic == nir_intrinsic_load_ubo_vec4) {
            if (!nir_src_is_const(intr->src[1]))
               goto refuse;
            uint64_t offset = nir_src_as_uint(intr->src[1]);
            if (intr->intrinsic == nir_intrinsic_load_ubo_vec4)
               offset = 16ull * (offset + nir_intrinsic_base(intr)) +
                        4ull * nir_intrinsic_component(intr);
            if (offset + 4ull * intr->def.num_components >
                M9_PADDING_SELECTOR_OFFSET) {
               why = "application uniform overlaps internal padding key";
               goto refuse;
            }
         }
         if (intr->intrinsic == nir_intrinsic_load_input &&
             nir_intrinsic_base(intr) == 0 && !nir_intrinsic_component(intr) &&
             intr->def.bit_size == 32 && intr->def.num_components >= 2 &&
             nir_src_is_const(intr->src[0]) && !nir_src_as_uint(intr->src[0])) {
            nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
            if (input && sem.location != s.input.location)
               goto refuse;
            s.input = sem;
            input = true;
         }
      }
   if (!input)
      goto refuse;
   nir_shader *copy = nir_shader_clone(NULL, nir);
   if (!copy) {
      why = "out of memory";
      goto refuse;
   }
   nir_shader_intrinsics_pass(copy, gpu_padding_position,
                             nir_metadata_control_flow, &s);
   struct m9_vs_program trial = {0};
   bool ok = !s.invalid && s.stores == 1 &&
             compile_program(copy, outputs, &trial, &why, true, NULL, NULL,
                             false, sampled_position);
   ralloc_free(copy);
   if (!ok) {
      m9_vs_program_fini(&trial);
      goto refuse;
   }
   if (m9_vs_slot_count(&trial) > max_slots ||
       (trial.producer.size && m9_vs_producer_slot_count(&trial) +
                              M9_VS_TAIL_SLOTS > max_slots) ||
       m9_vs_extra_slot_count(&trial) > max_slots ||
       m9_vs_const_count(&trial) > max_const_pairs) {
      why = "GPU padding program exceeds unchanged native limits";
      m9_vs_program_fini(&trial);
      goto refuse;
   }
   unsigned attr;
   for (attr = 0; attr < trial.attr_count; attr++)
      if (trial.attrs[attr].location == s.input.location)
         break;
   if (attr == trial.attr_count || trial.attrs[attr].constant ||
       trial.attrs[attr].scalar_count || trial.attrs[attr].driver_location ||
       trial.attrs[attr].components < 2 || trial.attrs[attr].select[0] != 0 ||
       trial.attrs[attr].select[1] != 1) {
      m9_vs_program_fini(&trial);
      goto refuse;
   }
   unsigned tagged = 0;
   util_dynarray_foreach(&trial.consts, struct m9_vs_const, k)
      if (k->kind == M9_VS_CONST_UNIFORM)
         for (unsigned lane = 0; lane < 2; lane++)
            if (k->offset[lane] == M9_PADDING_SELECTOR_OFFSET) {
               k->padding_mask |= 1u << lane;
               /* No application read, even if a later uploader does not
                * yet understand the deliberately unactivated interface. */
               k->offset[lane] = M9_VS_UNIFORM_ZERO;
               tagged++;
            }
   if (!tagged) {
      why = "GPU padding selector was not retained";
      m9_vs_program_fini(&trial);
      goto refuse;
   }
   trial.gpu_padding = true;
   trial.padding_attr = attr;
   *p = trial;
   if (reason)
      *reason = NULL;
   return true;
refuse:
   if (reason)
      *reason = why;
   return false;
}

bool
m9_vs_compile_faa(struct nir_shader *nir, const struct m9_vs_outputs *outputs,
                  struct m9_vs_program *p, const char **reason)
{
   if (!nir || !p) {
      if (reason)
         *reason = "missing FAA shader/program";
      return false;
   }
   return compile_program(nir, outputs, p, reason, true, NULL, NULL, true, false);
}

/* Lowered-I/O counterpart of Mesa nir_lower_clip_halfz: the original public
 * D3D FAA position stage expects z in [0,w], while Gallium GL NIR writes
 * [-w,w]. Preserve X/Y/W and all varyings; perform this affine conversion
 * in the live GPU producer, not by CPU-transforming vertex records. */
struct faa_gl_position_state {
   unsigned stores;
   bool invalid;
};

static bool
faa_gl_position(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   struct faa_gl_position_state *state = data;
   if (intr->intrinsic != nir_intrinsic_store_output ||
       nir_intrinsic_io_semantics(intr).location != VARYING_SLOT_POS)
      return false;
   if (nir_intrinsic_component(intr) || nir_intrinsic_write_mask(intr) != 0xf ||
       intr->src[0].ssa->num_components != 4 || intr->src[0].ssa->bit_size != 32) {
      state->invalid = true;
      return false;
   }
   state->stores++;
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *pos = intr->src[0].ssa;
   nir_def *z = nir_fmul_imm(b, nir_fadd(b, nir_channel(b, pos, 2),
                                       nir_channel(b, pos, 3)), 0.5f);
   nir_def *halfz = nir_vec4(b, nir_channel(b, pos, 0),
                            nir_channel(b, pos, 1), z, nir_channel(b, pos, 3));
   nir_src_rewrite(&intr->src[0], halfz);
   return true;
}

bool
m9_vs_compile_faa_gl(const struct nir_shader *nir,
                     const struct m9_vs_outputs *outputs,
                     struct m9_vs_program *p, const char **reason)
{
   if (!nir || !p || nir->info.stage != MESA_SHADER_VERTEX) {
      if (reason)
         *reason = "missing FAA GL vertex shader/program";
      return false;
   }
   nir_shader *copy = nir_shader_clone(NULL, nir);
   if (!copy) {
      if (reason)
         *reason = "out of memory";
      return false;
   }
   struct faa_gl_position_state state = {0};
   nir_shader_intrinsics_pass(copy, faa_gl_position,
                             nir_metadata_control_flow, &state);
   if (state.invalid || !state.stores) {
      if (reason)
         *reason = "FAA GL requires complete float32 clip position stores";
      ralloc_free(copy);
      return false;
   }
   bool ok = m9_vs_compile_faa(copy, outputs, p, reason);
   ralloc_free(copy);
   return ok;
}

/* The ordinary source can keep independent scalar coefficient chains even
 * when NIR has paired outputs. Vectorizing those chains trades some slots
 * for fewer typed constant pairs. Retry only a constant-span refusal, on a
 * clone: admitted programs and unsuccessful trials keep their exact image. */
static bool
program_slots_fit(const struct m9_vs_program *p, unsigned max_slots)
{
   return m9_vs_slot_count(p) <= max_slots &&
          (!m9_vs_producer_slot_count(p) ||
           m9_vs_producer_slot_count(p) + M9_VS_TAIL_SLOTS <= max_slots) &&
          m9_vs_extra_slot_count(p) <= max_slots;
}

bool
m9_vs_compile_sampled_position_source(struct nir_shader **nir,
                                      const struct m9_vs_outputs *outputs,
                                      struct m9_vs_program *p, const char **reason,
                                      unsigned max_slots, unsigned max_const_pairs)
{
   if (!nir || !*nir || !p)
      return false;
   nir_shader *trial = nir_shader_clone(ralloc_parent(*nir), *nir);
   if (!trial) {
      if (reason) *reason = "out of memory";
      return false;
   }
   if (!m9_vs_compile_sampled_position(trial, outputs, p, reason)) {
      ralloc_free(trial);
      return false;
   }
   if (!program_slots_fit(p, max_slots) || m9_vs_const_count(p) > max_const_pairs) {
      m9_vs_program_fini(p);
      ralloc_free(trial);
      if (reason) *reason = "sampled position exceeds native program limits";
      return false;
   }
   ralloc_free(*nir);
   *nir = trial;
   return true;
}

static bool
compile_bounded(struct nir_shader *nir,
                      const struct m9_vs_outputs *outputs,
                      struct m9_vs_program *p, const char **reason,
                      unsigned max_slots, unsigned max_const_pairs,
                      bool *identity)
{
   nir_shader *trial_nir;
   struct m9_vs_program trial;

   if (!compile_program(nir, outputs, p, reason, true, NULL, identity, false, false))
      return false;
   if (m9_vs_const_count(p) <= max_const_pairs ||
       !program_slots_fit(p, max_slots))
      return true;
   trial_nir = nir_shader_clone(NULL, nir);
   if (!trial_nir)
      return true;
   bool progress = false;
   NIR_PASS(progress, trial_nir, nir_opt_vectorize, NULL, NULL);
   if (progress && m9_vs_compile(trial_nir, outputs, &trial, NULL)) {
      if (program_slots_fit(&trial, max_slots) &&
          m9_vs_const_count(&trial) <= max_const_pairs) {
         m9_vs_program_fini(p);
         *p = trial;
      } else {
         m9_vs_program_fini(&trial);
      }
   }
   ralloc_free(trial_nir);
   return true;
}

bool
m9_vs_compile_bounded(struct nir_shader *nir,
                      const struct m9_vs_outputs *outputs,
                      struct m9_vs_program *p, const char **reason,
                      unsigned max_slots, unsigned max_const_pairs)
{
   return compile_bounded(nir, outputs, p, reason, max_slots, max_const_pairs,
                          NULL);
}

/* Geometry scalarization can expand otherwise supported paired sine/dot
 * arithmetic. Keep a pre-geometry clone only for these sources, then retry
 * a refusal using their existing vector graph and typed scalar keys. The
 * adopted graph must follow its program so subsequent sampler variants do
 * not start again from the unsuccessful scalarized graph. */
bool
m9_vs_compile_bounded_source(struct nir_shader **nir,
                             const struct m9_vs_outputs *outputs,
                             struct m9_vs_program *p, const char **reason,
                             unsigned max_slots, unsigned max_const_pairs)
{
   bool identity[M9_VS_MAX_CONSTS] = {0};
   nir_shader *saved = geometry_dots(*nir) ?
      nir_shader_clone(ralloc_parent(*nir), *nir) : NULL;
   struct m9_vs_program trial;

   if (!compile_bounded(*nir, outputs, p, reason, max_slots, max_const_pairs,
                         identity)) {
      ralloc_free(saved);
      return false;
   }
   if (saved && (!program_slots_fit(p, max_slots) ||
                 m9_vs_const_count(p) > max_const_pairs)) {
      const struct const_seed seed = {.program = p, .identity = identity};

      if (compile_program(saved, outputs, &trial, NULL, false, &seed, NULL, false, false)) {
         if (program_slots_fit(&trial, max_slots) &&
             m9_vs_const_count(&trial) <= max_const_pairs) {
            m9_vs_program_fini(p);
            *p = trial;
            ralloc_free(*nir);
            *nir = saved;
            saved = NULL;
         } else {
            m9_vs_program_fini(&trial);
         }
      }
   }
   ralloc_free(saved);
   return true;
}

void
m9_vs_program_fini(struct m9_vs_program *p)
{
   util_dynarray_fini(&p->slots);
   util_dynarray_fini(&p->extra);
   util_dynarray_fini(&p->gather);
   util_dynarray_fini(&p->producer);
   util_dynarray_fini(&p->consts);
   memset(p, 0, sizeof(*p));
}

int
m9_vs_varying_index(const struct m9_vs_program *p, unsigned location)
{
   for (unsigned v = 0; v < p->varying_count; v++)
      if (p->varying_location[v] == location &&
          !p->outputs.stream[v].coordinate)
         return (int)v;
   return -1;
}

int
m9_vs_stream_index(const struct m9_vs_program *p, unsigned location,
                   unsigned pass, unsigned coord, bool coordinate)
{
   for (unsigned v = 0; v < p->outputs.count; v++) {
      const struct m9_vs_output *o = &p->outputs.stream[v];

      if (o->coordinate != coordinate)
         continue;
      if (coordinate ? o->coord == coord && o->pass == pass :
                       o->location == location)
         return (int)v;
      if (coordinate && o->paired && o->pair_coord == coord &&
          o->pair_pass == pass)
         return (int)v;
   }
   return -1;
}

unsigned
m9_vs_const_count(const struct m9_vs_program *p)
{
   unsigned n = util_dynarray_num_elements(&p->consts, struct m9_vs_const);

   if (p->faa_preprocess)
      return n;
   return n ? MAX2(M9_VS_TAIL_LAST_PAIR + 1u, m9_vs_const_pair(n - 1) + 1u) :
              M9_VS_TAIL_LAST_PAIR + 1u;
}

unsigned
m9_vs_slot_count(const struct m9_vs_program *p)
{
   return util_dynarray_num_elements(&p->slots, uint32_t) / 2 +
          M9_VS_TAIL_SLOTS + 1;
}

unsigned
m9_vs_extra_slot_count(const struct m9_vs_program *p)
{
   unsigned head = util_dynarray_num_elements(&p->extra, uint32_t);

   return head ? MAX2(head / 2 + 1, 4u) : 0;
}

unsigned
m9_vs_gather_slot_count(const struct m9_vs_program *p)
{
   unsigned body = util_dynarray_num_elements(&p->gather, uint32_t);

   return body ? MAX2(body / 2 + 1, 4u) : 0;
}

unsigned
m9_vs_producer_slot_count(const struct m9_vs_program *p)
{
   unsigned body = util_dynarray_num_elements(&p->producer, uint32_t);
   return body ? MAX2(body / 2 + 1, 4u) : 0;
}

/* Keep the existing conservative scratch prefix. The actual vendor GLSL
 * finalizer's 150dc0 metadata calculator additionally couples the two pairs
 * of a complete bound output: when either is occupied, both are reserved.
 * Its complex bump variant and the scratch-r10 control witness this rule.
 * This is metadata closure, not permission to use outputs as temporaries. */
uint16_t
m9_vs_source_mask(const struct m9_vs_program *p, bool primary)
{
   const struct util_dynarray *body = primary ? &p->producer : &p->extra;
   const uint32_t *w = body->data;
   unsigned targets = p->sampled_position ? (primary ? 2 : 4) :
                      primary ? 3 : p->varying_count - M9_VS_FUSED_VARYINGS;
   unsigned pairs = 0;
   for (unsigned i = 0; i < body->size / 8; i++) {
      unsigned d = w[2 * i] >> 8 & 0x7f;
      bool output = false;
      for (unsigned j = 0; j < targets; j++)
         output |= d == REG_OUT_LO(j) || d == REG_OUT_HI(j);
      if (!output) {
         assert(d < 0x20);
         pairs = MAX2(pairs, d / 2 + 1);
      }
   }
   uint16_t mask = (uint16_t)~BITFIELD_MASK(pairs);
   if (p->producer.size) {
      for (unsigned j = 0; j < targets; j++) {
         unsigned low = REG_OUT_LO(j) / 2;
         unsigned high = REG_OUT_HI(j) / 2;
         uint16_t complete = BITFIELD_BIT(low) | BITFIELD_BIT(high);
         if ((mask & complete) != complete)
            mask &= ~complete;
      }
   }
   return mask;
}

/* The ordinary source head shares the captured tail's three bound
 * output groups. Keep its bf00 reservations, add only occupied scratch
 * pairs, then apply 150dc0's coupling only to those complete groups.
 * In particular the unbound r1d/r1f group is not a complete output. */
uint16_t
m9_vs_fused_source_mask(const struct m9_vs_program *p)
{
   const uint32_t *w = p->slots.data;
   uint16_t mask = 0xbf00;
   for (unsigned i = 0; i < p->slots.size / 8; i++) {
      unsigned d = w[2 * i] >> 8 & 0x7f;
      bool output = false;
      for (unsigned j = 0; j < 3; j++)
         output |= d == REG_OUT_LO(j) || d == REG_OUT_HI(j);
      /* The mask has sixteen pairs. The tail's clip-code register r21
       * and source attributes are not part of that availability field. */
      if (!output && d < 0x20)
         mask &= ~BITFIELD_BIT(d / 2);
   }
   for (unsigned j = 0; j < 3; j++) {
      uint16_t complete = BITFIELD_BIT(REG_OUT_LO(j) / 2) |
                          BITFIELD_BIT(REG_OUT_HI(j) / 2);
      if ((mask & complete) != complete)
         mask &= ~complete;
   }
   return mask;
}

/* Frees output pair 1 for a varying by pointing the two tail slots that
 * write the clip position at the tail's own scratch register.  One decoded
 * field each, the destination at W0 bits 14:8; nothing reads either
 * destination again and the scratch is written afresh further down, so the
 * clip code the tail ends with is unchanged.  The slot count and the marker
 * at n - 4 are untouched, and so is index 88's pair mask. */
static void
tail_free_pair(uint32_t *tail)
{
   static const unsigned slot[2] = { M9_VS_TAIL_CLIPPOS_LO,
                                     M9_VS_TAIL_CLIPPOS_HI };

   for (unsigned i = 0; i < 2; i++) {
      uint32_t *w = &tail[2 * slot[i]];

      assert(((w[0] >> 8) & 0x7f) == (i ? REG_OUT_HI(M9_VS_FUSED_PAIR) :
                                          REG_OUT_LO(M9_VS_FUSED_PAIR)));
      w[0] = (w[0] & ~0x7f00u) | M9_VS_TAIL_SCRATCH << 8;
   }
}

unsigned
m9_vs_emit(const struct m9_vs_program *p, uint32_t *out, unsigned max_dwords,
           bool fused_pair)
{
   unsigned head = util_dynarray_num_elements(&p->slots, uint32_t);
   unsigned total = head + 2 * (M9_VS_TAIL_SLOTS + 1);

   if (!out || p->faa_preprocess || total > max_dwords)
      return 0;
   memcpy(out, p->slots.data, head * sizeof(*out));
   memcpy(out + head, &m9_passthrough_c2_program[2 * M9_VS_TAIL_FIRST_SLOT],
          2 * M9_VS_TAIL_SLOTS * sizeof(*out));
   if (fused_pair)
      tail_free_pair(out + head);
   out[head + 2 * M9_VS_TAIL_SLOTS] = STOP_W0;
   out[head + 2 * M9_VS_TAIL_SLOTS + 1] = STOP_W1;
   return total;
}

/* The gather draw: one register-pair move a varying, from the route the
 * record feeds (0x00 + 4k, 0x02 + 4k) to the output pair the target reads
 * (CAPTURED, vs-varyings-4's c3=27 is four such moves and a stop). */
static void
compile_gather(struct m9_vs_program *p, unsigned varyings)
{
   for (unsigned k = 0; k < varyings; k++) {
      util_dynarray_append_typed(&p->gather, uint32_t,
         w0_make(NONE, NONE, M9_VS_ROUTE_LO(k), REG_OUT_LO(k), 0, 0x1c));
      util_dynarray_append_typed(&p->gather, uint32_t,
         w1_make(VS_MOV | 1u << 4, NONE, NONE));
      util_dynarray_append_typed(&p->gather, uint32_t,
         w0_make(NONE, NONE, M9_VS_ROUTE_HI(k), REG_OUT_HI(k), 0, 0x1c));
      util_dynarray_append_typed(&p->gather, uint32_t,
         w1_make(VS_MOV | 1u << 4, NONE, NONE));
   }
}

/* The second head carries no tail: it writes its targets and stops. */
/* Every captured native program marks W1 of slot n - 4, the fragment half's
 * finish_slots() rule (m9_fs_compile.c), and a program shorter than four
 * slots is padded with stops to reach it.  The fused pass inherits the mark
 * from its appended tail, which sits exactly four slots from the end; a
 * tail-free program has to place it itself. */
static unsigned
m9_vs_emit_body(const struct util_dynarray *body, uint32_t *out,
                unsigned max_dwords)
{
   unsigned head = util_dynarray_num_elements(body, uint32_t);
   unsigned slots = head ? MAX2(head / 2 + 1, 4u) : 0;

   if (!slots || !out || 2 * slots > max_dwords)
      return 0;
   memcpy(out, body->data, head * sizeof(*out));
   for (unsigned i = head / 2; i < slots; i++) {
      out[2 * i] = STOP_W0;
      out[2 * i + 1] = STOP_W1;
   }
   out[2 * (slots - 4) + 1] |= 0x40000000u;
   return 2 * slots;
}

unsigned
m9_vs_emit_faa(const struct m9_vs_program *p, uint32_t *out,
               unsigned max_dwords)
{
   if (!p || !p->faa_preprocess || p->slots.size || p->extra.size ||
       p->gather.size)
      return 0;
   return m9_vs_emit_body(&p->producer, out, max_dwords);
}

unsigned
m9_vs_c2_image(const struct m9_vs_program *p, const uint32_t *fs_slots,
               unsigned fs_slot_count, uint32_t *out, unsigned max_dwords,
               bool fused_pair, unsigned *vertex_entry, unsigned *extra_entry,
               unsigned *gather_entry, unsigned *producer_entry)
{
   unsigned vs_dwords = m9_vs_slot_count(p) * 2;
   unsigned extra_dwords = m9_vs_extra_slot_count(p) * 2;
   unsigned gather_dwords = m9_vs_gather_slot_count(p) * 2;
   unsigned producer_dwords = m9_vs_producer_slot_count(p) * 2;
   unsigned total = fs_slot_count * 2 + vs_dwords + extra_dwords +
                    gather_dwords + producer_dwords;

   if (!out || p->faa_preprocess || total > max_dwords ||
       (fs_slot_count && !fs_slots))
      return 0;
   if (fs_slot_count)
      memcpy(out, fs_slots, fs_slot_count * 2 * sizeof(*out));
   if (!m9_vs_emit(p, out + fs_slot_count * 2, vs_dwords, fused_pair))
      return 0;
   if (extra_dwords &&
       !m9_vs_emit_body(&p->extra, out + fs_slot_count * 2 + vs_dwords,
                        extra_dwords))
      return 0;
   if (gather_dwords &&
       !m9_vs_emit_body(&p->gather,
                        out + fs_slot_count * 2 + vs_dwords + extra_dwords,
                        gather_dwords))
      return 0;
   if (producer_dwords &&
       !m9_vs_emit_body(&p->producer,
                        out + fs_slot_count * 2 + vs_dwords + extra_dwords +
                              gather_dwords, producer_dwords))
      return 0;
   if (producer_entry)
      *producer_entry = producer_dwords ?
         fs_slot_count + (vs_dwords + extra_dwords + gather_dwords) / 2 : 0;
   if (vertex_entry)
      *vertex_entry = fs_slot_count;
   if (extra_entry)
      *extra_entry = extra_dwords ? fs_slot_count + vs_dwords / 2 : 0;
   if (gather_entry)
      *gather_entry = gather_dwords ?
                      fs_slot_count + (vs_dwords + extra_dwords) / 2 : 0;
   return total;
}

static float
m9_vs_uniform(const float *uniforms, unsigned floats, uint32_t offset)
{
   if (offset == M9_VS_UNIFORM_ZERO || offset / 4 >= floats)
      return 0.0f;
   return uniforms[offset / 4];
}

bool
m9_vs_padding_upload_build(const struct m9_vs_program *p,
                           const struct m9_vs_padding_bounds *b,
                           unsigned app, unsigned max_vertices,
                           const uint32_t *constants, unsigned pairs,
                           struct m9_vs_padding_upload *out)
{
   if (!p || !b || !out || !constants || !p->gpu_padding || p->faa_preprocess ||
       !app || app % 4 || max_vertices > 65532u ||
       app > max_vertices || M9_VS_PADDING_VERTICES > max_vertices - app ||
       b->width < 2 || b->height < 2 || b->width > 0x7fffu ||
       b->height > 0x7fffu || b->stencil_enabled || b->query_active ||
       b->fragment_depth || b->viewport_scale[0] != b->width * 0.5f ||
       b->viewport_scale[1] != b->height * 0.5f ||
       b->viewport_translate[0] != b->width * 0.5f ||
       b->viewport_translate[1] != b->height * 0.5f ||
       !p->attr_count || p->attr_count > M9_VS_MAX_ATTRIBUTES ||
       p->outputs.count > M9_VS_MAX_VARYINGS ||
       !p->consts.data || p->consts.size % sizeof(struct m9_vs_const) ||
       p->padding_attr >= p->attr_count || !p->record_bytes ||
       p->record_bytes % 16 || p->record_bytes > 16 * M9_VS_MAX_ATTRIBUTES ||
       !pairs || pairs > M9_VS_MAX_CONST_PAIRS || pairs != m9_vs_const_count(p))
      return false;
   unsigned x0 = b->clip_min & 0xffffu, y0 = b->clip_min >> 16;
   unsigned x1 = b->clip_max & 0xffffu, y1 = b->clip_max >> 16;
   if (x0 >= x1 || y0 >= y1 || x1 > b->width || y1 > b->height)
      return false;
   bool interior = x0 >= 1 && y0 >= 1 && x1 < b->width && y1 < b->height;
   bool far_less = b->normalized_less_depth &&
                   isfinite(b->viewport_scale[2]) &&
                   b->viewport_scale[2] >= 0.0f &&
                   b->viewport_scale[2] <= 0.5f &&
                   b->viewport_translate[2] >= b->viewport_scale[2] &&
                   b->viewport_scale[2] + b->viewport_translate[2] == 1.0f;
   if (!interior && !far_less)
      return false;
   /* Match the actual span consumed by the unchanged viewport tail, rather
    * than trusting a separate bounds description of another viewport. */
   const unsigned viewport_words[6] = {
      M9_VS_VIEWPORT_DWORD, M9_VS_VIEWPORT_DWORD + 1,
      M9_VS_DEPTH_SCALE_DWORD, M9_VS_BIAS_DWORD, M9_VS_BIAS_DWORD + 1,
      M9_VS_DEPTH_BIAS_DWORD,
   };
   for (unsigned v = 0; v < 6; v++) {
      unsigned word = viewport_words[v] - M9_VS_CONST_OFFSET / 4;
      if (word >= pairs * 2)
         return false;
      float actual;
      memcpy(&actual, &constants[word], sizeof(actual));
      if (actual != (v < 3 ? b->viewport_scale[v] :
                            b->viewport_translate[v - 3]))
         return false;
   }
   const struct m9_vs_attr *pad = &p->attrs[p->padding_attr];
   if (pad->constant || pad->scalar_count || pad->driver_location ||
       pad->components < 2 || pad->select[0] != 0 || pad->select[1] != 1 ||
       pad->offset > p->record_bytes / 4 ||
       4 > p->record_bytes / 4 - pad->offset)
      return false;
   for (unsigned i = 0; i < p->attr_count; i++) {
      if (p->attrs[i].offset % 4 || p->attrs[i].offset >= p->record_bytes / 4 ||
          p->attrs[i].constant || p->attrs[i].scalar_count)
         return false;
      for (unsigned j = 0; j < i; j++)
         if (p->attrs[i].offset == p->attrs[j].offset)
            return false;
   }
   for (unsigned i = 0; i < p->outputs.count; i++)
      if (p->outputs.stream[i].coordinate || p->outputs.stream[i].paired ||
          p->outputs.stream[i].coeff_record)
         return false;
   unsigned tagged = 0, i = 0;
   util_dynarray_foreach(&p->consts, struct m9_vs_const, k) {
      if (k->padding_mask & ~3u || (k->padding_mask &&
          (k->kind != M9_VS_CONST_UNIFORM || m9_vs_const_pair(i) >= pairs)))
         return false;
      for (unsigned lane = 0; lane < 2; lane++)
         if (k->padding_mask & (1u << lane)) {
            if (k->offset[lane] != M9_VS_UNIFORM_ZERO)
               return false;
            tagged++;
         }
      i++;
   }
   if (!tagged)
      return false;
   struct m9_vs_padding_upload upload = {0};
   upload.record_bytes = p->record_bytes;
   upload.const_pairs = pairs;
   upload.source_vertices = app + M9_VS_PADDING_VERTICES;
   upload.runs[0].count = app;
   upload.runs[1].first = app;
   upload.runs[1].count = M9_VS_PADDING_VERTICES;
   upload.runs[1].const_offset = pairs * 8;
   memcpy(upload.constants, constants, pairs * 8);
   memcpy(upload.constants + pairs * 2, constants, pairs * 8);
   i = 0;
   util_dynarray_foreach(&p->consts, struct m9_vs_const, k) {
      for (unsigned lane = 0; lane < 2; lane++)
         if (k->padding_mask & (1u << lane)) {
            unsigned at = 2 * m9_vs_const_pair(i) + lane;
            upload.constants[at] = 0;
            upload.constants[pairs * 2 + at] = 0x3f800000u;
         }
      i++;
   }
   const unsigned edge[4][4] = {
      {0, b->height - 1, b->width, b->height},
      {0, 0, b->width, 1}, {0, 0, 1, b->height},
      {b->width - 1, 0, b->width, b->height},
   };
   const uint8_t corner[6][2] = {{0,0},{1,0},{0,1},{0,1},{1,0},{1,1}};
   const uint8_t order[12] = {0,1,2,3,4,5,2,1,0,5,4,3};
   unsigned stride = p->record_bytes / 4;
   for (unsigned r = 0; r < 4; r++) {
      for (unsigned v = 0; v < 6; v++) {
         uint32_t *record = upload.records + (r * 6 + v) * stride;
         for (unsigned a = 0; a < p->attr_count; a++)
            record[p->attrs[a].offset + 3] = 0x3f800000u;
         float xy[2] = {
            2.0f * edge[r][corner[v][0] ? 2 : 0] / b->width - 1.0f,
            2.0f * edge[r][corner[v][1] ? 3 : 1] / b->height - 1.0f,
         };
         memcpy(record + pad->offset, xy, sizeof(xy));
      }
      for (unsigned v = 0; v < 12; v++)
         upload.indices[r * 12 + v] = app + r * 6 + order[v];
   }
   upload.indices[M9_VS_PADDING_INDICES] =
      upload.indices[M9_VS_PADDING_INDICES + 1] =
      upload.indices[M9_VS_PADDING_INDICES - 1];
   *out = upload;
   return true;
}

unsigned
m9_vs_constants(const struct m9_vs_program *p, const float *uniforms,
                unsigned uniform_floats, uint32_t *out, unsigned max_dwords)
{
   const struct m9_vs_const *k =
      (const struct m9_vs_const *)p->consts.data;
   unsigned n = util_dynarray_num_elements(&p->consts, struct m9_vs_const);
   unsigned pairs = m9_vs_const_count(p);

   if (!out || 2 * pairs > max_dwords ||
       (p->faa_preprocess && uniform_floats && !uniforms))
      return 0;
   for (unsigned i = 0; i < n; i++) {
      unsigned pair = p->faa_preprocess ? i : m9_vs_const_pair(i);

      for (unsigned lane = 0; lane < 2; lane++) {
         float v = k[i].kind == M9_VS_CONST_UNIFORM ?
                   m9_vs_uniform(uniforms, uniform_floats, k[i].offset[lane]) :
                   k[i].value[lane];

         memcpy(&out[2 * pair + lane], &v, sizeof(v));
      }
   }
   return pairs;
}

unsigned
m9_vs_const_span(const struct m9_vs_program *p, const float *values,
                 unsigned value_floats, uint32_t *out, unsigned max_dwords)
{
   unsigned n = util_dynarray_num_elements(&p->consts, struct m9_vs_const);
   unsigned pairs = m9_vs_const_count(p);

   if (!out || 2 * pairs > max_dwords || value_floats < 2 * n ||
       (n && !values))
      return 0;
   for (unsigned i = 0; i < n; i++)
      memcpy(&out[2 * (p->faa_preprocess ? i : m9_vs_const_pair(i))], &values[2 * i],
             2 * sizeof(*values));
   return pairs;
}

unsigned
m9_vs_decl_words(const struct m9_vs_program *p, uint32_t *out,
                 unsigned max_words)
{
   if (p->texture_count > 2)
      return 0;
   unsigned n = 2 * p->attr_count + p->texture_count + 1;

   if (!out || n > max_words || n > M9_VS_MAX_DECL)
      return 0;
   for (unsigned i = 0; i < p->attr_count; i++) {
      const struct m9_vs_attr *a = &p->attrs[i];

      /* Two words describe one attribute: slots 0 and 1 fill its first
       * register and slots 2 and 3 its second, 6 leaving a slot to the
       * paired word.  Bits 19:16 name the source stream and stay 0 here:
       * every attribute is an element of one interleaved record, which is
       * the shape the vendor's fs-* passes emit (CAPTURED, four attributes
       * at dword offsets 4, 0, 8 and 12 of a 64-byte record). */
      out[2 * i] = a->offset << 24 | (i + 1) << 20 | a->select[0] |
                   a->select[1] << 3 | 6 << 6 | 6 << 9;
      out[2 * i + 1] = a->offset << 24 | (i + 1) << 20 | 6 | 6 << 3 |
                       a->select[2] << 6 | a->select[3] << 9;
   }
   if (p->texture_count) {
      unsigned i;
      for (i = 0; i < p->attr_count; i++)
         if (p->attrs[i].location == p->texture_location)
            break;
      if (i == p->attr_count || p->texture_count > 2)
         return 0;
      /* Texture coordinate set 0 is attribute 5. The four input selectors
       * are source-proven by the genuine public direct-UV producer. Mode
       * 1 uses XY; the runtime's single-level state never reads a W LOD. */
      for (unsigned unit = 0; unit < p->texture_count; unit++)
         out[2 * p->attr_count + unit] =
            p->attrs[i].offset << 24 | (5u + unit) << 20 | 0x688u;
   }
   out[n - 1] = 0x00004b08;
   return n;
}
