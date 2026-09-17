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
 * attribute registers, computes the clip position into r00/r01 and one
 * varying into r02/r13.  m9_vs_emit() appends the vendor tail -- the
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
 * Logarithm and power are still refused.  No captured vertex program
 * contains the log2 seed, and the fragment unit's own log2 chain cannot be
 * validated offline: its three polynomial slots write the first scalar and
 * read the chained operand's second, so each reads a scalar its predecessor
 * left unwritten.  Guessing it would compile silently wrong geometry.
 */
#include "m9_vs_compile.h"

#include "m9_passthrough.h"

#define NONE       0x7fu
#define STOP_W0    0xffff801cu
#define STOP_W1    0xa01fc00fu

#include <string.h>

#include "nir.h"
#include "util/u_math.h"
#include "util/u_memory.h"

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
   bool pair;
};

struct ctx {
   nir_shader *nir;
   struct m9_vs_program *p;
   struct util_dynarray keys;
   struct held holds[M9_VS_REGS][2];
   unsigned *last_use;      /* per definition, the instruction that ends it */
   int8_t *fold;            /* per definition: the product's constant side */
   unsigned def_count;
   unsigned index;          /* instruction being emitted */
   /* Lanes whose result the output registers take directly. */
   struct out_pin {
      unsigned def, lane, reg;
   } pins[4];
   unsigned pin_count;
   unsigned temp_seq;
   bool temp_live;
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

static void
emit(struct ctx *c, uint32_t w0, uint32_t w1)
{
   util_dynarray_append_typed(&c->p->slots, uint32_t, w0);
   util_dynarray_append_typed(&c->p->slots, uint32_t, w1);
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

/* A constant pair holding the two scalars, and the operand swizzle that
 * reads them; a single-valued pair is read with the first scalar. */
static bool
const_pair(struct ctx *c, const struct held *a, const struct held *b,
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

static bool
held_is_const(const struct held *h)
{
   return h->kind == HELD_LIT || h->kind == HELD_UNIFORM;
}

/* Registers */

static bool
reg_in_pool(unsigned r)
{
   return (r >= 0x04u && r <= 0x12u) || (r >= 0x14u && r <= 0x21u);
}

static bool
held_live(struct ctx *c, const struct held *h)
{
   switch (h->kind) {
   case HELD_DEF:
      return h->a < c->def_count && c->last_use[h->a] >= c->index;
   case HELD_TEMP:
      return c->temp_live;
   default:
      return false;
   }
}

static int
reg_alloc(struct ctx *c)
{
   for (unsigned r = 0; r < M9_VS_REGS; r++) {
      if (!reg_in_pool(r) || held_live(c, &c->holds[r][0]) ||
          held_live(c, &c->holds[r][1]))
         continue;
      memset(c->holds[r], 0, sizeof(c->holds[r]));
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
      if (!const_pair(c, h, h, &key, &swz))
         return fail(c, "constant operand");
      e = const_index(c, key);
      if (e < 0)
         return fail(c, "constant span overflow");
      emit_move_const(c, dst, op0, e, 0);
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

   if (find_pair(c, a, b, identity_only, reg, swz))
      return true;
   dst = reg_alloc(c);
   if (dst < 0)
      return fail(c, "out of temporaries");
   if (!write_scalar(c, dst, 0, a) || !write_scalar(c, dst, 1, b))
      return false;
   *reg = dst;
   *swz = 1;
   return true;
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
   pair = m9_vs_const_pair(key);
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
       const_pair(c, b0, b1, &key, &kswz)) {
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

   if (!const_pair(c, k0, k1, &key, &kswz))
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

/* a + b, or a - b with negate: the subtract form computes the c operand
 * minus the a operand, so the two swap fields. */
static bool
emit_add(struct ctx *c, unsigned dst, const struct held *a0,
         const struct held *a1, const struct held *b0, const struct held *b1,
         bool negate)
{
   const struct held *f0 = negate ? b0 : a0, *f1 = negate ? b1 : a1;
   const struct held *g0 = negate ? a0 : b0, *g1 = negate ? a1 : b1;
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
       !const_pair(c, b0, b1, &key, &kswz))
      return fail(c, "compare against a register");
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
   return true;
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
 * writes the first scalar and the completion adds it. */
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
   int key, e, t;
   struct held h;

   if (!const_pair(c, &b[0], &b[1], &key, &kswz))
      return false;
   e = const_index(c, key);
   if (e < 0)
      return fail(c, "constant span overflow");
   if (!operand(c, &a[0], &a[1], true, &ra, &sa))
      return false;
   if (n <= 2) {
      emit(c, w0_make(NONE, ra, REG_DOT_C, dst, 0, 0x1d),
           w1_make(VS_DOT_P, e, NONE));
      return true;
   }
   h = held_temp(c);
   t = reg_alloc_pinned(c, &h);
   if (t < 0)
      return fail(c, "out of temporaries");
   emit(c, w0_make(NONE, ra, REG_DOT_C, t, 0, 0x1d), w1_make(VS_DOT_P, e, NONE));
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
   for (unsigned lane = 0; lane < lanes; lane++) {
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
            side = mul ? c->fold[mul->def.index] : -1;
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
         a0 = src_held(alu, 0, c0); a1 = src_held(alu, 0, c1);
         b0 = src_held(alu, 1, c0); b1 = src_held(alu, 1, c1);
         if (!emit_add(c, dst, &a0, &a1, &b0, &b1, false))
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
      case nir_op_fcsel: {
         const struct held *sel[3][2];

         a0 = src_held(alu, 0, c0); a1 = src_held(alu, 0, c1);
         b0 = src_held(alu, 1, c0); b1 = src_held(alu, 1, c1);
         x0 = src_held(alu, 2, c0); x1 = src_held(alu, 2, c1);
         /* Class 4 picks the second operand where the condition is zero. */
         sel[0][0] = &a0; sel[0][1] = &a1;
         sel[1][0] = &x0; sel[1][1] = &x1;
         sel[2][0] = &b0; sel[2][1] = &b1;
         if (!emit_sel(c, dst, 4, sel))
            return false;
         break;
      }
      case nir_op_b2f32:
      case nir_op_mov:
         a0 = src_held(alu, 0, c0); a1 = src_held(alu, 0, c1);
         if (!emit_unary(c, dst, VS_MOV, 0, &a0, &a1, false))
            return false;
         break;
      case nir_op_frcp:
         a0 = src_held(alu, 0, 0);
         if (!emit_rcp(c, dst, &a0))
            return false;
         pair = false;
         break;
      case nir_op_frsq:
         a0 = src_held(alu, 0, 0);
         if (!emit_rsq(c, dst, &a0))
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

         a0 = src_held(alu, 0, 0);
         rr = reg_alloc_pinned(c, &hr);
         rc = reg_alloc_pinned(c, &hc);
         rm = reg_alloc_pinned(c, &hm);
         if (rr < 0 || rc < 0 || rm < 0)
            return fail(c, "out of temporaries");
         if (!emit_rsq(c, rr, &a0))
            return false;
         memset(&c->holds[rr][1], 0, sizeof(c->holds[rr][1]));
         if (!emit_cmp(c, rc, 4, &a0, &a0, &zero, &zero) ||
             !emit_mul(c, rm, &hr, &hr, &a0, &a0))
            return false;
         sel[0][0] = sel[0][1] = &hc;
         sel[1][0] = sel[1][1] = &hm;
         sel[2][0] = sel[2][1] = &a0;
         if (!emit_sel(c, dst, 4, sel))
            return false;
         break;
      }
      case nir_op_fexp2: {
         struct held h0 = held_temp(c), h1;
         int r0, r1;

         a0 = src_held(alu, 0, c0); a1 = src_held(alu, 0, c1);
         /* The chain writes both scalars with one result, so a lane whose
          * two components differ runs it twice and gathers the pair. */
         if (held_same(&a0, &a1)) {
            if (!emit_exp2(c, dst, &a0))
               return false;
            break;
         }
         r0 = reg_alloc_pinned(c, &h0);
         if (r0 < 0)
            return fail(c, "out of temporaries");
         if (!emit_exp2(c, r0, &a0))
            return false;
         h1 = held_temp(c);
         r1 = reg_alloc_pinned(c, &h1);
         if (r1 < 0)
            return fail(c, "out of temporaries");
         if (!emit_exp2(c, r1, &a1) ||
             !emit_unary(c, dst, VS_MOV, 0, &h0, &h1, true))
            return false;
         break;
      }
      case nir_op_fdiv: {
         struct held hr = held_temp(c);
         int rr;

         a0 = src_held(alu, 0, c0); a1 = src_held(alu, 0, c1);
         b0 = src_held(alu, 1, 0);
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
   unsigned components;
};

static bool
note_input(struct ctx *c, struct input_use *used, unsigned *count,
           nir_intrinsic_instr *intr)
{
   unsigned location = nir_intrinsic_io_semantics(intr).location;
   unsigned comps = nir_intrinsic_component(intr) + intr->def.num_components;

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
   if (*count >= M9_VS_MAX_ATTRIBUTES)
      return fail(c, "more than four vertex attributes");
   used[*count].location = location;
   used[(*count)++].components = comps;
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

   for (unsigned i = 0; i < count; i++) {
      unsigned reg = REG_ATTR0 + 8 * i;

      p->attrs[i].location = used[i].location;
      p->attrs[i].offset = dword;
      for (unsigned j = 0; j < 4; j++) {
         struct held h;

         memset(&h, 0, sizeof(h));
         if (j < used[i].components) {
            p->attrs[i].select[j] = j;
            h.kind = HELD_INPUT;
            h.a = used[i].location;
            h.b = j;
         } else {
            p->attrs[i].select[j] = j == 3 ? 5 : 4;
            h.kind = HELD_LIT;
            h.lit = j == 3 ? 1.0f : 0.0f;
         }
         hold_def(c, reg + j / 2, j % 2, &h);
      }
      dword += used[i].components;
   }
   p->attr_count = count;
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
       const_pair(c, h0, h1, &key, &swz) &&
       (e = const_index(c, key)) >= 0) {
      emit_move_const(c, reg, 0x1c, e, swz);
   } else if (find_pair(c, h0, h1, false, &src, &swz)) {
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
   return write_pair(c, reg_xy, &h[0], &h[1]) &&
          write_pair(c, reg_zw, &h[2], &h[3]);
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

static bool
compile_impl(struct ctx *c, nir_function_impl *impl)
{
   struct input_use used[M9_VS_MAX_ATTRIBUTES];
   nir_intrinsic_instr *position = NULL, *varying = NULL;
   static const float pos_default[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
   static const float var_default[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
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
         } else if (varying &&
                    nir_intrinsic_io_semantics(varying).location != location) {
            return fail(c, "more than one varying");
         } else {
            varying = intr;
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

   nir_foreach_instr(instr, block) {
      nir_alu_instr *alu;

      if (instr->type != nir_instr_type_alu)
         continue;
      alu = nir_instr_as_alu(instr);
      if (alu->op != nir_op_fadd)
         continue;
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
            break;
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

   pin_output(c, position, 0, REG_POS_XY);
   pin_output(c, position, 2, REG_POS_ZW);
   pin_output(c, varying, 0, REG_VAR_XY);
   pin_output(c, varying, 2, REG_VAR_ZW);
   declare_attributes(c, used, inputs);
   nir_foreach_instr(instr, block) {
      c->index = instr->index;
      c->temp_live = true;
      if (instr->type == nir_instr_type_alu &&
          !build_alu(c, nir_instr_as_alu(instr)))
         return false;
      c->temp_live = false;
   }
   c->index = final;
   c->temp_live = true;
   if (!write_output(c, position, REG_POS_XY, REG_POS_ZW, pos_default))
      return false;
   if (!write_output(c, varying, REG_VAR_XY, REG_VAR_ZW, var_default))
      return false;
   c->temp_live = false;
   if (varying) {
      c->p->has_varying = true;
      c->p->varying_location = nir_intrinsic_io_semantics(varying).location;
      c->p->varying_components =
         nir_intrinsic_component(varying) + varying->src[0].ssa->num_components;
   }
   return true;
}

bool
m9_vs_compile(struct nir_shader *nir, struct m9_vs_program *p,
              const char **reason)
{
   nir_function_impl *impl;
   struct ctx c;
   bool ok;

   memset(&c, 0, sizeof(c));
   memset(p, 0, sizeof(*p));
   util_dynarray_init(&p->slots, NULL);
   util_dynarray_init(&p->consts, NULL);
   util_dynarray_init(&c.keys, NULL);
   c.nir = nir;
   c.p = p;
   impl = nir->info.stage == MESA_SHADER_VERTEX ?
          nir_shader_get_entrypoint(nir) : NULL;
   ok = impl ? compile_impl(&c, impl) : fail(&c, "not a vertex shader");
   if (ok) {
      util_dynarray_foreach(&c.keys, struct kkey, k) {
         struct m9_vs_const *out =
            util_dynarray_grow(&p->consts, struct m9_vs_const, 1);

         memset(out, 0, sizeof(*out));
         out->kind = k->kind;
         out->value[0] = k->v[0];
         out->value[1] = k->pair ? k->v[1] : k->v[0];
         out->offset[0] = k->off[0];
         out->offset[1] = k->pair ? k->off[1] : k->off[0];
      }
   }
   if (reason)
      *reason = ok ? NULL : (c.reason ? c.reason : "unknown");
   util_dynarray_fini(&c.keys);
   free(c.last_use);
   free(c.fold);
   if (!ok)
      m9_vs_program_fini(p);
   return ok;
}

void
m9_vs_program_fini(struct m9_vs_program *p)
{
   util_dynarray_fini(&p->slots);
   util_dynarray_fini(&p->consts);
   memset(p, 0, sizeof(*p));
}

unsigned
m9_vs_const_count(const struct m9_vs_program *p)
{
   unsigned n = util_dynarray_num_elements(&p->consts, struct m9_vs_const);

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
m9_vs_emit(const struct m9_vs_program *p, uint32_t *out, unsigned max_dwords)
{
   unsigned head = util_dynarray_num_elements(&p->slots, uint32_t);
   unsigned total = head + 2 * (M9_VS_TAIL_SLOTS + 1);

   if (!out || total > max_dwords)
      return 0;
   memcpy(out, p->slots.data, head * sizeof(*out));
   memcpy(out + head, &m9_passthrough_c2_program[2 * M9_VS_TAIL_FIRST_SLOT],
          2 * M9_VS_TAIL_SLOTS * sizeof(*out));
   out[head + 2 * M9_VS_TAIL_SLOTS] = STOP_W0;
   out[head + 2 * M9_VS_TAIL_SLOTS + 1] = STOP_W1;
   return total;
}

unsigned
m9_vs_c2_image(const struct m9_vs_program *p, const uint32_t *fs_slots,
               unsigned fs_slot_count, uint32_t *out, unsigned max_dwords,
               unsigned *vertex_entry)
{
   unsigned vs_dwords = m9_vs_slot_count(p) * 2;
   unsigned total = fs_slot_count * 2 + vs_dwords;

   if (!out || total > max_dwords || (fs_slot_count && !fs_slots))
      return 0;
   memcpy(out, fs_slots, fs_slot_count * 2 * sizeof(*out));
   if (!m9_vs_emit(p, out + fs_slot_count * 2, vs_dwords))
      return 0;
   if (vertex_entry)
      *vertex_entry = fs_slot_count;
   return total;
}

static float
m9_vs_uniform(const float *uniforms, unsigned floats, uint32_t offset)
{
   if (offset == M9_VS_UNIFORM_ZERO || offset / 4 >= floats)
      return 0.0f;
   return uniforms[offset / 4];
}

unsigned
m9_vs_constants(const struct m9_vs_program *p, const float *uniforms,
                unsigned uniform_floats, uint32_t *out, unsigned max_dwords)
{
   const struct m9_vs_const *k =
      (const struct m9_vs_const *)p->consts.data;
   unsigned n = util_dynarray_num_elements(&p->consts, struct m9_vs_const);
   unsigned pairs = m9_vs_const_count(p);

   if (!out || 2 * pairs > max_dwords)
      return 0;
   for (unsigned i = 0; i < n; i++) {
      unsigned pair = m9_vs_const_pair(i);

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
m9_vs_decl_words(const struct m9_vs_program *p, uint32_t *out,
                 unsigned max_words)
{
   unsigned n = 2 * p->attr_count + 1;

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
   out[2 * p->attr_count] = 0x00004b08;
   return n;
}
