/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * NIR to native M9148 fragment programs.
 *
 * A native register holds two scalars; a vec4 value occupies one register
 * for its xy lane and one for its zw lane.  The vendor compiler emits every
 * vector operation as a zw slot followed by an xy slot, schedules operations
 * by expression depth, allocates temporaries in a backward scan (scratch
 * registers 11/13, fresh general registers for values read by the last slot,
 * interpolation registers 00/01 and 1a..1f) and numbers constants in backward
 * order of first use.  Every word below is an instruction form captured in
 * captures/m9x-vendor-glsl-programs-20260915 or
 * captures/m9x-vendor-glamor-shaders-20260915; shapes outside these forms
 * are refused.
 */
#include "m9_fs_compile.h"

#include <string.h>

#include "nir.h"
#include "nir_builder.h"
#include "util/u_math.h"

const nir_shader_compiler_options m9_fs_nir_options = {
   .fdot_replicates = false,
   .lower_bitops = true,
   .lower_extract_byte = true,
   .lower_extract_word = true,
   .lower_fdiv = true,
   .lower_fdph = true,
   .lower_fpow = true,
   .lower_fmod = true,
   .lower_sincos = true,
   .lower_uniforms_to_ubo = true,
   .force_indirect_unrolling = nir_var_all,
   .force_indirect_unrolling_sampler = true,
   .max_unroll_iterations = 32,
   .no_integers = true,
   .has_fused_comp_and_csel = true,
   /* Keep st/mesa's IO intrinsics instead of variables; the backend and
    * its corpus test consume load_interpolated_input/store_output. */
   .io_options = nir_io_has_intrinsics | nir_io_mediump_is_32bit,
};

#define STOP_W0 0xffff801cu
#define STOP_W1 0xa01fc00fu
#define MARKER  0x40000000u
#define NONE    0x7fu
#define OUT     0x2du
#define REG_S0  0x11u
#define REG_S1  0x13u
#define REG_I   0x1au

enum node_kind {
   N_VARY, N_TEX, N_UBO, N_LIT, N_MUL, N_ADD, N_SUB, N_MAD,
   N_MIN, N_MAX,        /* against a constant, the captured cls 2 / 3 form */
   N_NEG,               /* move with the W1 negate bit, as fs-neg emits */
   N_FRACT, N_FLOOR, N_CEIL,  /* the move family's rounding classes */
   N_EXP2, N_LOG2,      /* scalar chains, the vendor's fs-exp and fs-log */
   N_RCP, N_RSQ,        /* seed plus two Newton steps, fs-smoothstep, fs-sqrt */
   N_FRAGCOORD,         /* the raster position registers, y flipped */
   N_CMP,               /* register against a constant, giving 1.0 or 0.0 */
   N_SEL,               /* src[0] against zero picks src[1] or src[2] */
   N_SAT,               /* move in the saturating class, as blending uses */
   N_GATHER,            /* a result lane per source: src[lane] * src[2] */
   N_LOAD,              /* constant factor moved into a register */
   N_COMPOSE,           /* vec4(texture.xyz, literal), written in place */
};

struct node {
   enum node_kind kind;
   int src[3];          /* MUL/ADD a,b; SUB b-a as a,b; MAD b*t + c */
   nir_def *def;
   unsigned k;          /* varying slot, or a uniform load's first component */
   unsigned texel;      /* texel register of the xy lane */
   unsigned ubo_slot;
   float lit[4];
   bool neg;            /* ADD with a negated literal */
   bool emitted;        /* already placed by the depth-first schedule */
   int weight;          /* registers its subtree needs, one plus, memoised */
   uint8_t cmp;         /* N_CMP/N_SEL comparison class */
   bool is_cond;        /* holds 1.0 or 0.0, so a select can test it */
   int level;
   int value[2];        /* per lane: 0 xy, 1 zw */
   uint8_t lane[3][2];  /* source lane read for result lane xy, zw */
   uint8_t swz[3][2];   /* 2-bit operand swizzle per result lane, 1 = xy */
};

enum lop_kind {
   L_INTERP, L_MUL_RC, L_MUL_RR, L_ADD_RC, L_ADD_RR, L_SUB_CR, L_MAD,
   L_FRACT, L_FLOOR, L_CEIL, L_CMP, L_SEL, L_CHAIN,
   L_MIN_RC, L_MAX_RC, L_NEG, L_SAT,
   L_MOV_OUT, L_MOVC_OUT, L_MOVC_TMP, L_MOVC_SECOND, L_RAW,
};

struct lop {
   enum lop_kind kind;
   unsigned lane;
   int node;
   int dst;             /* value id, -1 writes the output lane */
   int opval[3];        /* register operands as value ids */
   int opreg[3];        /* fixed register operands (texels), -1 if unused */
   int konst;           /* constant operand key, -1 if unused */
   int kc2;             /* a second constant, in the c field */
   uint8_t opswz[3];    /* register operand swizzles */
   uint8_t kswz;        /* constant operand swizzle */
   uint8_t op0;         /* L_CHAIN destination scalar mask */
   uint8_t cls;         /* L_CHAIN instruction class */
   bool keep_dst;       /* writes part of a value another lop allocated */
   bool kc;             /* L_CHAIN takes its constant index in the c field */
   uint32_t w0, w1;     /* L_RAW, and the L_CHAIN base word */
};

struct value {
   int reg;
   int def, last;
   bool interp, rg_read, general;
};

struct kkey {
   enum m9_fs_const_kind kind;
   float v[2];
   uint32_t off[2];
   bool pair;
};

struct ctx {
   nir_shader *nir;
   struct m9_fs_program *p;
   const struct m9_fs_blend *blend;  /* NULL when unblended */
   struct util_dynarray nodes, lops, values, keys, key_index;
   const char *reason;
   /* Fixed-function alpha test lowered into the shader by st. */
   struct {
      bool enabled;
      unsigned func;        /* kill slot instruction class */
      uint32_t ref_offset;  /* constant buffer byte offset of the reference */
      float ref_value;      /* the reference itself, when it is a literal */
      bool ref_literal;
   } alpha;
   bool has_consts;
   bool custom_consts;  /* a template supplies the whole constant span */
   bool depth_first;    /* schedule operands before breadth, for pressure */
};

#define NODES(c) ((struct node *)(c)->nodes.data)
#define LOPS(c) ((struct lop *)(c)->lops.data)
#define VALUES(c) ((struct value *)(c)->values.data)
#define KEYS(c) ((struct kkey *)(c)->keys.data)
#define NUM(c, a, t) util_dynarray_num_elements(&(c)->a, t)

static const char fail_out_of_temporaries[] = "out of temporaries";

static bool
fail_at(struct ctx *c, const char *reason, int line)
{
   if (getenv("M9_FS_FAILTRACE"))
      fprintf(stderr, "m9x fs: fail at line %d: %s\n", line, reason);
   if (!c->reason)
      c->reason = reason;
   return false;
}
#define fail(c, r) fail_at((c), (r), __LINE__)

static uint32_t
w0_make(unsigned a, unsigned b, unsigned cf, unsigned d, unsigned cls,
        unsigned op0)
{
   return (a & 7u) << 29 | b << 22 | cf << 15 | d << 8 | cls << 5 | op0;
}

static uint32_t
w1_make(uint32_t base, unsigned e, unsigned a)
{
   return base | e << 14 | a >> 3;
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
         return it - KEYS(c);
   }
   util_dynarray_append_typed(&c->keys, struct kkey, *k);
   util_dynarray_append_typed(&c->key_index, int, -1);
   return NUM(c, keys, struct kkey) - 1;
}

static struct kkey
key_literal(float a, float b)
{
   struct kkey k;

   memset(&k, 0, sizeof(k));
   k.kind = M9_FS_CONST_LITERAL;
   k.v[0] = a;
   k.v[1] = b;
   k.pair = a != b;
   return k;
}

static struct kkey
key_uniform(uint32_t off0, uint32_t off1)
{
   struct kkey k;

   memset(&k, 0, sizeof(k));
   k.kind = M9_FS_CONST_UNIFORM;
   k.off[0] = off0;
   k.off[1] = off1;
   k.pair = true;
   return k;
}

/* Constant span index of key in backward first-use order. */
static unsigned
const_ref(struct ctx *c, int key)
{
   struct m9_fs_const *out;
   const struct kkey *k = &KEYS(c)[key];

   int *index = (int *)c->key_index.data;

   if (index[key] >= 0)
      return index[key];
   out = util_dynarray_grow(&c->p->consts, struct m9_fs_const, 1);
   out->kind = k->kind;
   out->value[0] = k->v[0];
   out->value[1] = k->v[1];
   out->offset[0] = k->off[0];
   out->offset[1] = k->off[1];
   index[key] = NUM(c, p->consts, struct m9_fs_const) - 1;
   return index[key];
}

static void
const_tail(struct ctx *c)
{
   static const float tail[2][2] = { { 0.0f, 1.0f }, { 1.0f, 0.5f } };

   for (unsigned i = 0; i < 2; i++) {
      struct m9_fs_const *k = util_dynarray_grow(&c->p->consts,
                                                 struct m9_fs_const, 1);

      memset(k, 0, sizeof(*k));
      k->kind = M9_FS_CONST_LITERAL;
      k->value[0] = tail[i][0];
      k->value[1] = tail[i][1];
   }
}

/* Expression tree */

static int
node_new(struct ctx *c, enum node_kind kind)
{
   struct node *n = util_dynarray_grow(&c->nodes, struct node, 1);

   if (!n)
      return fail(c, "out of memory"), -1;
   memset(n, 0, sizeof(*n));
   n->kind = kind;
   n->src[0] = n->src[1] = n->src[2] = -1;
   n->value[0] = n->value[1] = -1;
   for (unsigned i = 0; i < 3; i++) {
      n->lane[i][1] = 1;
      n->swz[i][0] = n->swz[i][1] = 1;
   }
   return NUM(c, nodes, struct node) - 1;
}

static bool
node_swizzled(const struct node *n, unsigned i)
{
   return n->lane[i][0] != 0 || n->lane[i][1] != 1 || n->swz[i][0] != 1 ||
          n->swz[i][1] != 1;
}

static bool
node_is_const(const struct node *n)
{
   return n->kind == N_UBO || n->kind == N_LIT;
}

static int varying_slot(struct ctx *c, unsigned location);
static int build_vec(struct ctx *c, nir_scalar s[4]);
static int build_def(struct ctx *c, nir_def *def);

/* st scalarises IO, so a load carries the first varying component it reads. */
static unsigned
def_comp_base(nir_def *def)
{
   nir_instr *instr = nir_def_instr(def);

   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);

      if (in->intrinsic == nir_intrinsic_load_interpolated_input ||
          in->intrinsic == nir_intrinsic_load_input)
         return nir_intrinsic_component(in);
   }
   return 0;
}

static int
build_intrinsic(struct ctx *c, nir_intrinsic_instr *intr)
{
   int n;

   switch (intr->intrinsic) {
   case nir_intrinsic_load_interpolated_input:
   case nir_intrinsic_load_input: {
      unsigned location = nir_intrinsic_io_semantics(intr).location;
      int k;

      if (location == VARYING_SLOT_POS) {
         /* The program below matches the vendor's fragcoord case byte for
          * byte and r88 declares it the same way, but on this hardware the
          * position registers still read back constant, so the job side
          * has to publish something the capture does not show.  Refuse
          * until that is found rather than render a wrong position. */
         if (!debug_get_bool_option("M9X_FRAGCOORD", false))
            return fail(c, "gl_FragCoord is not published to the shader"), -1;
         n = node_new(c, N_FRAGCOORD);
         return n < 0 ? -1 : (c->p->fragcoord = true, n);
      }
      if (nir_intrinsic_component(intr) + intr->def.num_components > 4)
         return fail(c, "unsupported fragment input"), -1;
      if (debug_get_bool_option("M9X_DEBUG_FS", false))
         fprintf(stderr, "m9x fs: input location %u component %u comps %u\n",
                 location, nir_intrinsic_component(intr),
                 intr->def.num_components);
      k = varying_slot(c, location);
      if (k < 0)
         return -1;
      /* One interpolation feeds every component read of the same slot. */
      util_dynarray_foreach(&c->nodes, struct node, it)
         if (it->kind == N_VARY && it->k == (unsigned)k)
            return it - NODES(c);
      n = node_new(c, N_VARY);
      if (n < 0)
         return -1;
      NODES(c)[n].k = k;
      return n;
   }
   case nir_intrinsic_load_ubo_vec4:
      /* A narrower load still names the whole vec4 slot; the components
       * above it are simply never read. */
      if (!nir_src_is_const(intr->src[0]) || nir_src_as_uint(intr->src[0]) ||
          !nir_src_is_const(intr->src[1]) ||
          nir_intrinsic_component(intr) + intr->def.num_components > 4 ||
          intr->def.num_components < 1)
         return fail(c, "unsupported uniform access"), -1;
      n = node_new(c, N_UBO);
      if (n < 0)
         return -1;
      NODES(c)[n].ubo_slot = nir_intrinsic_base(intr) +
                             nir_src_as_uint(intr->src[1]);
      NODES(c)[n].k = nir_intrinsic_component(intr);
      return n;
   default:
      return fail(c, "unsupported intrinsic"), -1;
   }
}

static int
build_tex(struct ctx *c, nir_tex_instr *tex)
{
   struct m9_fs_program *p = c->p;
   nir_intrinsic_instr *coord;
   unsigned location;
   int n, ci = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   int pi = nir_tex_instr_src_index(tex, nir_tex_src_projector);
   nir_scalar s[2];

   if (p->texture_count >= M9_FS_MAX_TEXTURES)
      return fail(c, "more textures than the program can bind"), -1;
   if (tex->op != nir_texop_tex || tex->sampler_dim != GLSL_SAMPLER_DIM_2D ||
       tex->is_shadow || tex->is_array || ci < 0 ||
       tex->num_srcs != (pi < 0 ? 1u : 2u))
      return fail(c, "texture sampling needs a computed coordinate pass"), -1;
   /* Linked vec2 varyings may share one slot: xy or zw of the input.  st
    * scalarises IO, so the pair can also be two loads of one location. */
   unsigned coord_comp[2];

   for (unsigned i = 0; i < 2; i++) {
      nir_intrinsic_instr *in;

      s[i] = nir_scalar_chase_movs(nir_get_scalar(tex->src[ci].src.ssa, i));
      if (!nir_scalar_is_intrinsic(s[i]))
         return fail(c, "texture coordinate is not a varying"), -1;
      in = nir_def_as_intrinsic(s[i].def);
      if (in->intrinsic != nir_intrinsic_load_interpolated_input ||
          nir_intrinsic_io_semantics(in).location !=
          nir_intrinsic_io_semantics(nir_def_as_intrinsic(s[0].def)).location)
         return fail(c, "texture coordinate is not a varying"), -1;
      coord_comp[i] = nir_intrinsic_component(in) + s[i].comp;
      if (debug_get_bool_option("M9X_DEBUG_FS", false))
         fprintf(stderr, "m9x fs: coord %u location %u component %u\n", i,
                 nir_intrinsic_io_semantics(in).location, coord_comp[i]);
   }
   if (coord_comp[0] > 2 || coord_comp[1] != coord_comp[0] + 1)
      return fail(c, "texture coordinate is not a varying"), -1;
   coord = nir_def_as_intrinsic(s[0].def);
   /* The fixed-function projector is w of the same varying; the driver
    * divides the coordinate stream per vertex. */
   if (pi >= 0) {
      nir_scalar q = nir_scalar_chase_movs(
         nir_get_scalar(tex->src[pi].src.ssa, 0));

      if (!nir_scalar_is_intrinsic(q) ||
          nir_intrinsic_io_semantics(nir_def_as_intrinsic(q.def)).location !=
          nir_intrinsic_io_semantics(coord).location ||
          nir_intrinsic_component(nir_def_as_intrinsic(q.def)) + q.comp != 3)
         return fail(c, "texture sampling needs a computed coordinate pass"),
                -1;
      p->texcoord_projective[p->texture_count] = true;
   }
   location = nir_intrinsic_io_semantics(coord).location;
   /* An even pair is a captured slot half.  An odd pair is repacked per
    * vertex by the driver, which needs one texture to own the stream. */
   for (unsigned i = 0; i < p->texture_count; i++)
      if (p->texcoords[i] == location &&
          ((coord_comp[0] & 1) || (p->texcoord_components[i] & 1)))
         return fail(c, "texture coordinate is not a varying"), -1;
   p->textures[p->texture_count] = tex->texture_index;
   p->texcoords[p->texture_count] = location;
   p->texcoord_components[p->texture_count] = coord_comp[0];
   n = node_new(c, N_TEX);
   if (n < 0)
      return -1;
   NODES(c)[n].texel = 4 * p->texture_count++;
   return n;
}

/* Operand i of alu; a swizzle that keeps each result lane inside one source
 * lane becomes a source lane plus a 2-bit operand swizzle. */
static int
alu_src_node(struct ctx *c, nir_alu_instr *alu, unsigned i, uint8_t *lane,
             uint8_t *swz)
{
   nir_scalar s[4];
   bool identity = true, same = true;

   lane[0] = 0;
   lane[1] = 1;
   swz[0] = swz[1] = 1;
   unsigned nc = alu->def.num_components;

   for (unsigned k = 0; k < 4; k++) {
      s[k] = nir_scalar_chase_movs(
         nir_get_scalar(alu->src[i].src.ssa,
                        alu->src[i].swizzle[MIN2(k, nc - 1)]));
      same &= s[k].def == s[0].def && !nir_scalar_is_const(s[k]);
      identity &= s[k].comp == k;
   }
   if (same && !identity && s[0].def->num_components == 4 &&
       s[0].comp / 2 == s[1].comp / 2 && s[2].comp / 2 == s[3].comp / 2) {
      /* A swizzle that keeps each result lane inside one source lane is an
       * operand field; one that crosses them goes through the gather. */
      for (unsigned l = 0; l < 2; l++) {
         unsigned c0 = s[2 * l].comp, c1 = s[2 * l + 1].comp;

         lane[l] = c0 / 2;
         swz[l] = (c0 & 1) << 1 | (c1 & 1);
      }
      for (unsigned k = 0; k < 4; k++)
         s[k].comp = k;
   }
   return build_vec(c, s);
}

/* A boolean operand is a comparison result.  The vector paths describe a
 * value by component and a one-bit definition has none, so take its node
 * directly and express the swizzle as a source lane plus operand swizzle. */
static int
bool_src_node(struct ctx *c, nir_alu_instr *alu, unsigned i, uint8_t *lane,
              uint8_t *swz)
{
   unsigned nc = alu->def.num_components;
   nir_scalar s[4];
   int n;

   lane[0] = 0;
   lane[1] = 1;
   swz[0] = swz[1] = 1;
   for (unsigned k = 0; k < 4; k++) {
      s[k] = nir_scalar_chase_movs(
         nir_get_scalar(alu->src[i].src.ssa,
                        alu->src[i].swizzle[MIN2(k, nc - 1)]));
      if (s[k].def != s[0].def)
         return fail(c, "unsupported condition"), -1;
   }
   if (s[0].def->num_components > 1) {
      for (unsigned l = 0; l < 2; l++) {
         unsigned c0 = s[2 * l].comp, c1 = s[2 * l + 1].comp;

         if (c0 / 2 != c1 / 2)
            return fail(c, "swizzle crosses register lanes"), -1;
         lane[l] = c0 / 2;
         swz[l] = (c0 & 1) << 1 | (c1 & 1);
      }
   } else {
      /* A scalar condition holds in every scalar of the first lane. */
      lane[0] = lane[1] = 0;
      swz[0] = swz[1] = 0;
   }
   n = build_def(c, s[0].def);
   if (n >= 0 && !NODES(c)[n].is_cond)
      return fail(c, "unsupported condition"), -1;
   return n;
}

/* A condition used as a value has to arrive unswizzled: either the
 * identity, or the replicating form a scalar condition is given. */
static bool
cond_is_plain(const uint8_t lane[2], const uint8_t swz[2])
{
   return (!lane[0] && lane[1] == 1 && swz[0] == 1 && swz[1] == 1) ||
          (!lane[0] && !lane[1] && !swz[0] && !swz[1]);
}

static void
set_src(struct node *n, unsigned i, int src, const uint8_t *lane,
        const uint8_t *swz)
{
   n->src[i] = src;
   memcpy(n->lane[i], lane, 2);
   memcpy(n->swz[i], swz, 2);
}

/* A constant an operation cannot take directly is moved into a register
 * first, the way the vendor loads a constant multiply factor. */
static int
materialise(struct ctx *c, int n, uint8_t lane[2], uint8_t swz[2])
{
   static const uint8_t lane_id[2] = { 0, 1 }, swz_id[2] = { 1, 1 };
   int load;

   if (n < 0 || !node_is_const(&NODES(c)[n]))
      return n;
   load = node_new(c, N_LOAD);
   if (load < 0)
      return -1;
   set_src(&NODES(c)[load], 0, n, lane, swz);
   memcpy(lane, lane_id, 2);
   memcpy(swz, swz_id, 2);
   return load;
}

/* The add and mix forms carry no operand swizzle field, so a swizzled
 * register operand is copied through a multiply by one, which does. */
static int
deswizzle(struct ctx *c, int n, uint8_t lane[2], uint8_t swz[2])
{
   static const uint8_t lane_id[2] = { 0, 1 }, swz_id[2] = { 1, 1 };
   int one, m;

   if (n < 0 || (!memcmp(lane, lane_id, 2) && !memcmp(swz, swz_id, 2)))
      return n;
   if (node_is_const(&NODES(c)[n]))
      return n;
   one = node_new(c, N_LIT);
   if (one < 0)
      return -1;
   for (unsigned i = 0; i < 4; i++)
      NODES(c)[one].lit[i] = 1.0f;
   m = node_new(c, N_MUL);
   if (m < 0)
      return -1;
   set_src(&NODES(c)[m], 0, n, lane, swz);
   set_src(&NODES(c)[m], 1, one, lane_id, swz_id);
   memcpy(lane, lane_id, 2);
   memcpy(swz, swz_id, 2);
   return m;
}

static int
build_alu(struct ctx *c, nir_alu_instr *alu)
{
   static const uint8_t lane_id[2] = { 0, 1 }, swz_id[2] = { 1, 1 };
   uint8_t lane[3][2], swz[3][2];
   int a, b, t, n, s;

   /* A narrower result computes in every lane and is read back through its
    * own components; the lanes above it are don't-care. */
   if (alu->def.num_components < 1 || alu->def.num_components > 4)
      return fail(c, "non-vec4 arithmetic"), -1;
   /* A constructor is a value, not an operation. */
   if (nir_op_is_vec_or_mov(alu->op)) {
      unsigned nc = alu->def.num_components;
      nir_scalar v[4];

      for (unsigned k = 0; k < 4; k++)
         v[k] = nir_scalar_chase_movs(
            nir_get_scalar(&alu->def, MIN2(k, nc - 1)));
      return build_vec(c, v);
   }
   switch (alu->op) {
   case nir_op_fmul:
   case nir_op_fadd:
      a = alu_src_node(c, alu, 0, lane[0], swz[0]);
      b = a < 0 ? -1 : alu_src_node(c, alu, 1, lane[1], swz[1]);
      if (b < 0)
         return -1;
      if (node_is_const(&NODES(c)[a])) {
         int tmp = a;
         a = b;
         b = tmp;
         memcpy(lane[2], lane[0], 2);
         memcpy(lane[0], lane[1], 2);
         memcpy(lane[1], lane[2], 2);
         memcpy(swz[2], swz[0], 2);
         memcpy(swz[0], swz[1], 2);
         memcpy(swz[1], swz[2], 2);
      }
      /* The vendor moves a constant factor into a register. */
      if (node_is_const(&NODES(c)[a])) {
         a = materialise(c, a, lane[0], swz[0]);
         if (a < 0)
            return -1;
      }
      if (alu->op == nir_op_fadd) {
         a = deswizzle(c, a, lane[0], swz[0]);
         b = deswizzle(c, b, lane[1], swz[1]);
         if (a < 0 || b < 0)
            return -1;
      }
      n = node_new(c, alu->op == nir_op_fmul ? N_MUL : N_ADD);
      if (n < 0)
         return -1;
      set_src(&NODES(c)[n], 0, a, lane[0], swz[0]);
      set_src(&NODES(c)[n], 1, b, lane[1], swz[1]);
      if (alu->op == nir_op_fadd &&
          (node_swizzled(&NODES(c)[n], 0) || node_swizzled(&NODES(c)[n], 1)))
         return fail(c, "swizzled addition operand"), -1;
      if (alu->op == nir_op_fadd && NODES(c)[b].kind == N_LIT) {
         const float *v = NODES(c)[b].lit;

         if (v[0] < 0 && v[0] == v[1] && v[0] == v[2] && v[0] == v[3]) {
            for (unsigned k = 0; k < 4; k++)
               NODES(c)[b].lit[k] = -NODES(c)[b].lit[k];
            NODES(c)[n].neg = true;
         }
      }
      return n;
   case nir_op_fsat: {
      int a = materialise(c, alu_src_node(c, alu, 0, lane[0], swz[0]),
                          lane[0], swz[0]);

      if (a < 0)
         return -1;
      n = node_new(c, N_SAT);
      if (n < 0)
         return -1;
      set_src(&NODES(c)[n], 0, a, lane[0], swz[0]);
      return n;
   }
   case nir_op_fneg:
   case nir_op_ffract:
   case nir_op_ffloor:
   case nir_op_fceil: {
      int a = materialise(c, alu_src_node(c, alu, 0, lane[0], swz[0]),
                          lane[0], swz[0]);
      enum node_kind kind = alu->op == nir_op_fneg ? N_NEG :
                            alu->op == nir_op_ffract ? N_FRACT :
                            alu->op == nir_op_ffloor ? N_FLOOR : N_CEIL;

      if (a < 0)
         return -1;
      n = node_new(c, kind);
      if (n < 0)
         return -1;
      set_src(&NODES(c)[n], 0, a, lane[0], swz[0]);
      return n;
   }
   case nir_op_fmin:
   case nir_op_fmax: {
      int a = alu_src_node(c, alu, 0, lane[0], swz[0]);
      int b = alu_src_node(c, alu, 1, lane[1], swz[1]);

      if (a < 0 || b < 0)
         return -1;
      /* The captured form takes one register and one constant.  Two
       * registers become a + min(b - a, 0) and a + max(b - a, 0), which is
       * exact and built only from forms the corpus shows. */
      if (!node_is_const(&NODES(c)[a]) && !node_is_const(&NODES(c)[b])) {
         int zero = node_new(c, N_LIT), diff, lim;

         if (zero < 0)
            return -1;
         diff = node_new(c, N_SUB);
         if (diff < 0)
            return -1;
         set_src(&NODES(c)[diff], 0, a, lane[0], swz[0]);
         set_src(&NODES(c)[diff], 1, b, lane[1], swz[1]);
         lim = node_new(c, alu->op == nir_op_fmin ? N_MIN : N_MAX);
         if (lim < 0)
            return -1;
         set_src(&NODES(c)[lim], 0, diff, lane_id, swz_id);
         set_src(&NODES(c)[lim], 1, zero, lane_id, swz_id);
         a = deswizzle(c, a, lane[0], swz[0]);
         if (a < 0)
            return -1;
         n = node_new(c, N_ADD);
         if (n < 0)
            return -1;
         set_src(&NODES(c)[n], 0, lim, lane_id, swz_id);
         set_src(&NODES(c)[n], 1, a, lane[0], swz[0]);
         return n;
      }
      if (node_is_const(&NODES(c)[a]) && node_is_const(&NODES(c)[b])) {
         a = materialise(c, a, lane[0], swz[0]);
         if (a < 0)
            return -1;
      }
      if (node_is_const(&NODES(c)[a])) {
         int t = a;
         uint8_t tl[2], ts[2];

         memcpy(tl, lane[0], 2);
         memcpy(ts, swz[0], 2);
         a = b;
         memcpy(lane[0], lane[1], 2);
         memcpy(swz[0], swz[1], 2);
         b = t;
         memcpy(lane[1], tl, 2);
         memcpy(swz[1], ts, 2);
      }
      n = node_new(c, alu->op == nir_op_fmin ? N_MIN : N_MAX);
      if (n < 0)
         return -1;
      set_src(&NODES(c)[n], 0, a, lane[0], swz[0]);
      set_src(&NODES(c)[n], 1, b, lane[1], swz[1]);
      return n;
   }
   case nir_op_fexp2:
   case nir_op_flog2:
   case nir_op_frcp:
   case nir_op_frsq:
   case nir_op_fsqrt: {
      /* The chain is scalar: one seed instruction and its refinement. */
      enum node_kind kind = alu->op == nir_op_fexp2 ? N_EXP2 :
                            alu->op == nir_op_flog2 ? N_LOG2 :
                            alu->op == nir_op_frcp ? N_RCP : N_RSQ;

      if (alu->def.num_components != 1)
         return fail(c, "vector special function"), -1;
      a = materialise(c, alu_src_node(c, alu, 0, lane[0], swz[0]),
                      lane[0], swz[0]);
      if (a < 0)
         return -1;
      n = node_new(c, kind);
      if (n < 0)
         return -1;
      set_src(&NODES(c)[n], 0, a, lane[0], swz[0]);
      if (alu->op != nir_op_fsqrt)
         return n;
      /* sqrt is the argument times its reciprocal square root, with the
       * zero guard the vendor emits, since rsq(0) is an infinity. */
      {
         int zero = node_new(c, N_LIT), cond, mul;

         if (zero < 0)
            return -1;
         cond = node_new(c, N_CMP);
         if (cond < 0)
            return -1;
         set_src(&NODES(c)[cond], 0, a, lane[0], swz[0]);
         set_src(&NODES(c)[cond], 1, zero, lane_id, swz_id);
         NODES(c)[cond].cmp = 4;
         mul = node_new(c, N_MUL);
         if (mul < 0)
            return -1;
         set_src(&NODES(c)[mul], 0, n, lane_id, swz_id);
         set_src(&NODES(c)[mul], 1, a, lane[0], swz[0]);
         n = node_new(c, N_SEL);
         if (n < 0)
            return -1;
         set_src(&NODES(c)[n], 0, cond, lane_id, swz_id);
         set_src(&NODES(c)[n], 1, mul, lane_id, swz_id);
         set_src(&NODES(c)[n], 2, a, lane[0], swz[0]);
         NODES(c)[n].cmp = 4;
      }
      return n;
   }
   case nir_op_fabs: {
      /* No modifier for absolute value is captured, but the compare and
       * select classes express it: negate where the value is below zero. */
      int zero, neg, cond;

      a = materialise(c, alu_src_node(c, alu, 0, lane[0], swz[0]),
                      lane[0], swz[0]);
      if (a < 0)
         return -1;
      zero = node_new(c, N_LIT);
      if (zero < 0)
         return -1;
      cond = node_new(c, N_CMP);
      if (cond < 0)
         return -1;
      set_src(&NODES(c)[cond], 0, a, lane[0], swz[0]);
      set_src(&NODES(c)[cond], 1, zero, lane_id, swz_id);
      NODES(c)[cond].cmp = 0;
      neg = node_new(c, N_NEG);
      if (neg < 0)
         return -1;
      set_src(&NODES(c)[neg], 0, a, lane[0], swz[0]);
      n = node_new(c, N_SEL);
      if (n < 0)
         return -1;
      set_src(&NODES(c)[n], 0, cond, lane_id, swz_id);
      set_src(&NODES(c)[n], 1, a, lane[0], swz[0]);
      set_src(&NODES(c)[n], 2, neg, lane_id, swz_id);
      NODES(c)[n].cmp = 4;
      return n;
   }
   case nir_op_flt:
   case nir_op_fge:
   case nir_op_feq:
   case nir_op_fneu: {
      /* Comparison classes, from fs-step (>=) and fs-ternary (>): 0 <,
       * 1 >=, 2 <=, 3 >, 4 ==, 5 !=.  Bit 0 is the complement. */
      static const uint8_t direct[] = { 0, 1, 4, 5 };
      static const uint8_t flipped[] = { 3, 2, 4, 5 };
      unsigned which = alu->op == nir_op_flt ? 0 : alu->op == nir_op_fge ? 1 :
                       alu->op == nir_op_feq ? 2 : 3;
      bool swap;

      a = alu_src_node(c, alu, 0, lane[0], swz[0]);
      b = a < 0 ? -1 : alu_src_node(c, alu, 1, lane[1], swz[1]);
      if (b < 0)
         return -1;
      /* The compare unit reads one register and one constant pair, so two
       * registers compare their difference against zero instead. */
      if (!node_is_const(&NODES(c)[a]) && !node_is_const(&NODES(c)[b])) {
         int diff = node_new(c, N_SUB), zero;

         if (diff < 0)
            return -1;
         set_src(&NODES(c)[diff], 0, b, lane[1], swz[1]);
         set_src(&NODES(c)[diff], 1, a, lane[0], swz[0]);
         zero = node_new(c, N_LIT);
         if (zero < 0)
            return -1;
         n = node_new(c, N_CMP);
         if (n < 0)
            return -1;
         set_src(&NODES(c)[n], 0, diff, lane_id, swz_id);
         set_src(&NODES(c)[n], 1, zero, lane_id, swz_id);
         NODES(c)[n].cmp = direct[which];
         NODES(c)[n].is_cond = true;
         return n;
      }
      swap = node_is_const(&NODES(c)[a]);
      if (swap == node_is_const(&NODES(c)[b])) {
         a = materialise(c, a, lane[0], swz[0]);
         if (a < 0)
            return -1;
         swap = false;
      }
      if (swap) {
         int t2 = a;
         uint8_t tl[2], ts[2];

         memcpy(tl, lane[0], 2);
         memcpy(ts, swz[0], 2);
         a = b;
         memcpy(lane[0], lane[1], 2);
         memcpy(swz[0], swz[1], 2);
         b = t2;
         memcpy(lane[1], tl, 2);
         memcpy(swz[1], ts, 2);
      }
      n = node_new(c, N_CMP);
      if (n < 0)
         return -1;
      set_src(&NODES(c)[n], 0, a, lane[0], swz[0]);
      set_src(&NODES(c)[n], 1, b, lane[1], swz[1]);
      NODES(c)[n].cmp = swap ? flipped[which] : direct[which];
      NODES(c)[n].is_cond = true;
      return n;
   }
   case nir_op_inot: {
      /* A negated condition is the same compare in the opposite class. */
      int cond = bool_src_node(c, alu, 0, lane[0], swz[0]);

      if (cond < 0)
         return -1;
      if (!cond_is_plain(lane[0], swz[0]))
         return fail(c, "swizzled boolean negate"), -1;
      if (NODES(c)[cond].kind != N_CMP) {
         /* A combined condition negates as one minus itself, which the
          * negate and the add against a literal express between them. */
         int one = node_new(c, N_LIT), neg;

         if (one < 0)
            return -1;
         for (unsigned i = 0; i < 4; i++)
            NODES(c)[one].lit[i] = 1.0f;
         neg = node_new(c, N_NEG);
         if (neg < 0)
            return -1;
         set_src(&NODES(c)[neg], 0, cond, lane_id, swz_id);
         n = node_new(c, N_ADD);
         if (n < 0)
            return -1;
         set_src(&NODES(c)[n], 0, neg, lane_id, swz_id);
         set_src(&NODES(c)[n], 1, one, lane_id, swz_id);
         NODES(c)[n].is_cond = true;
         return n;
      }
      n = node_new(c, N_CMP);
      if (n < 0)
         return -1;
      NODES(c)[n] = NODES(c)[cond];
      NODES(c)[n].def = NULL;
      NODES(c)[n].level = 0;
      NODES(c)[n].value[0] = NODES(c)[n].value[1] = -1;
      NODES(c)[n].cmp ^= 1;
      return n;
   }
   case nir_op_iand:
   case nir_op_ior: {
      /* Conditions hold 1.0 or 0.0, so and is a product and or is the
       * larger of the two, and both stay conditions. */
      int q, pp = bool_src_node(c, alu, 0, lane[0], swz[0]);

      q = pp < 0 ? -1 : bool_src_node(c, alu, 1, lane[1], swz[1]);
      if (q < 0)
         return -1;
      if (!cond_is_plain(lane[0], swz[0]) || !cond_is_plain(lane[1], swz[1]))
         return fail(c, "swizzled boolean operand"), -1;
      if (alu->op == nir_op_iand) {
         n = node_new(c, N_MUL);
         if (n < 0)
            return -1;
         set_src(&NODES(c)[n], 0, pp, lane_id, swz_id);
         set_src(&NODES(c)[n], 1, q, lane_id, swz_id);
      } else {
         int zero = node_new(c, N_LIT), diff, lim;

         if (zero < 0)
            return -1;
         diff = node_new(c, N_SUB);
         if (diff < 0)
            return -1;
         set_src(&NODES(c)[diff], 0, pp, lane_id, swz_id);
         set_src(&NODES(c)[diff], 1, q, lane_id, swz_id);
         lim = node_new(c, N_MAX);
         if (lim < 0)
            return -1;
         set_src(&NODES(c)[lim], 0, diff, lane_id, swz_id);
         set_src(&NODES(c)[lim], 1, zero, lane_id, swz_id);
         n = node_new(c, N_ADD);
         if (n < 0)
            return -1;
         set_src(&NODES(c)[n], 0, lim, lane_id, swz_id);
         set_src(&NODES(c)[n], 1, pp, lane_id, swz_id);
      }
      NODES(c)[n].is_cond = true;
      return n;
   }
   case nir_op_b2f32:
   case nir_op_b2f16: {
      /* The compare unit already produces 1.0 and 0.0. */
      int cond = bool_src_node(c, alu, 0, lane[0], swz[0]);

      if (cond < 0)
         return -1;
      if (!cond_is_plain(lane[0], swz[0]))
         return fail(c, "swizzled boolean conversion"), -1;
      return cond;
   }
   case nir_op_bcsel: {
      int cond = bool_src_node(c, alu, 0, lane[0], swz[0]);

      a = cond < 0 ? -1 : alu_src_node(c, alu, 1, lane[1], swz[1]);
      b = a < 0 ? -1 : alu_src_node(c, alu, 2, lane[2], swz[2]);
      if (b < 0)
         return -1;
      a = materialise(c, a, lane[1], swz[1]);
      b = a < 0 ? -1 : materialise(c, b, lane[2], swz[2]);
      if (b < 0)
         return -1;
      n = node_new(c, N_SEL);
      if (n < 0)
         return -1;
      /* Only the equal class is captured for this opcode (fs-sqrt, fs-exp),
       * so test the condition against zero and take the false value first. */
      set_src(&NODES(c)[n], 0, cond, lane[0], swz[0]);
      set_src(&NODES(c)[n], 1, b, lane[2], swz[2]);
      set_src(&NODES(c)[n], 2, a, lane[1], swz[1]);
      NODES(c)[n].cmp = 4;
      return n;
   }
   case nir_op_ffma:
   case nir_op_ffma_weak: {
      int cc;

      a = alu_src_node(c, alu, 0, lane[0], swz[0]);
      b = a < 0 ? -1 : alu_src_node(c, alu, 1, lane[1], swz[1]);
      cc = b < 0 ? -1 : alu_src_node(c, alu, 2, lane[2], swz[2]);
      if (cc < 0)
         return -1;
      if (node_is_const(&NODES(c)[a])) {
         int t2 = a;
         uint8_t tl[2], ts[2];

         memcpy(tl, lane[0], 2);
         memcpy(ts, swz[0], 2);
         a = b;
         memcpy(lane[0], lane[1], 2);
         memcpy(swz[0], swz[1], 2);
         b = t2;
         memcpy(lane[1], tl, 2);
         memcpy(swz[1], ts, 2);
      }
      cc = materialise(c, cc, lane[2], swz[2]);
      a = cc < 0 ? -1 : materialise(c, a, lane[0], swz[0]);
      if (a < 0)
         return -1;
      /* The captured multiply-add takes one constant factor; two register
       * factors are a multiply and an add. */
      if (node_is_const(&NODES(c)[b])) {
         n = node_new(c, N_MAD);
         if (n < 0)
            return -1;
         set_src(&NODES(c)[n], 0, a, lane[0], swz[0]);
         set_src(&NODES(c)[n], 1, b, lane[1], swz[1]);
         set_src(&NODES(c)[n], 2, cc, lane[2], swz[2]);
         return n;
      }
      s = node_new(c, N_MUL);
      if (s < 0)
         return -1;
      set_src(&NODES(c)[s], 0, a, lane[0], swz[0]);
      set_src(&NODES(c)[s], 1, b, lane[1], swz[1]);
      cc = deswizzle(c, cc, lane[2], swz[2]);
      if (cc < 0)
         return -1;
      n = node_new(c, N_ADD);
      if (n < 0)
         return -1;
      set_src(&NODES(c)[n], 0, s, lane_id, swz_id);
      set_src(&NODES(c)[n], 1, cc, lane[2], swz[2]);
      return n;
   }
   case nir_op_flrp:
      a = alu_src_node(c, alu, 0, lane[0], swz[0]);
      b = a < 0 ? -1 : alu_src_node(c, alu, 1, lane[1], swz[1]);
      t = b < 0 ? -1 : alu_src_node(c, alu, 2, lane[2], swz[2]);
      if (t < 0)
         return -1;
      a = deswizzle(c, a, lane[0], swz[0]);
      b = deswizzle(c, b, lane[1], swz[1]);
      t = deswizzle(c, t, lane[2], swz[2]);
      if (a < 0 || b < 0 || t < 0)
         return -1;
      for (unsigned i = 0; i < 3; i++)
         if (memcmp(lane[i], lane_id, 2) || memcmp(swz[i], swz_id, 2))
            return fail(c, "swizzled mix operand"), -1;
      /* The subtraction takes a uniform on the b side, but nothing else
       * constant: the rest goes into a register first. */
      a = materialise(c, a, lane[0], swz[0]);
      if (a < 0)
         return -1;
      if (NODES(c)[b].kind == N_LIT) {
         b = materialise(c, b, lane[1], swz[1]);
         if (b < 0)
            return -1;
      }
      s = node_new(c, N_SUB);
      if (s < 0)
         return -1;
      NODES(c)[s].src[0] = a;
      NODES(c)[s].src[1] = b;
      /* A constant factor fuses into the multiply-add; a computed one needs
       * the register-register multiply and add. */
      if (node_is_const(&NODES(c)[t])) {
         n = node_new(c, N_MAD);
         if (n < 0)
            return -1;
         NODES(c)[n].src[0] = s;
         NODES(c)[n].src[1] = t;
         NODES(c)[n].src[2] = a;
      } else {
         int m = node_new(c, N_MUL);

         if (m < 0)
            return -1;
         NODES(c)[m].src[0] = s;
         NODES(c)[m].src[1] = t;
         n = node_new(c, N_ADD);
         if (n < 0)
            return -1;
         NODES(c)[n].src[0] = m;
         NODES(c)[n].src[1] = a;
      }
      return n;
   default:
      /* The op name is a static string, so it can be the reason. */
      return fail(c, nir_op_infos[alu->op].name), -1;
   }
}

static int
build_def(struct ctx *c, nir_def *def)
{
   nir_instr *instr = nir_def_instr(def);
   int n = -1;

   util_dynarray_foreach(&c->nodes, struct node, it) {
      if (it->def == def)
         return it - NODES(c);
   }
   switch (instr->type) {
   case nir_instr_type_intrinsic:
      n = build_intrinsic(c, nir_instr_as_intrinsic(instr));
      break;
   case nir_instr_type_tex:
      n = build_tex(c, nir_instr_as_tex(instr));
      break;
   case nir_instr_type_alu:
      n = build_alu(c, nir_instr_as_alu(instr));
      break;
   default:
      return fail(c, "unsupported instruction"), -1;
   }
   if (n >= 0)
      NODES(c)[n].def = def;
   return n;
}

/* One masked term of a gather: result lane l reads component 2*l + t of the
 * vec4, so two terms summed cover any arrangement of four scalars. */
static int
gather_term(struct ctx *c, const int comp_node[4], const unsigned comp_idx[4],
            const bool is_lit[4], unsigned t)
{
   static const uint8_t lane_id[2] = { 0, 1 }, swz_id[2] = { 1, 1 };
   int lane_node[2], n_mask, n;
   uint8_t src_lane[2] = { 0, 1 }, src_swz[2] = { 1, 1 };
   float mask[4] = { 0, 0, 0, 0 };

   for (unsigned l = 0; l < 2; l++) {
      unsigned k = 2 * l + t;

      lane_node[l] = comp_node[k];
      if (is_lit[k] || lane_node[l] < 0)
         continue;
      mask[k] = 1.0f;
      src_lane[l] = comp_idx[k] / 2;
      src_swz[l] = (comp_idx[k] & 1) << 1 | (comp_idx[k] & 1);
   }
   for (unsigned l = 0; l < 2; l++)
      if (lane_node[l] < 0) {
         if (lane_node[1 - l] < 0)
            return -2;          /* nothing of this term is a register */
         lane_node[l] = lane_node[1 - l];
         src_lane[l] = src_lane[1 - l];
         src_swz[l] = src_swz[1 - l];
      }
   /* The multiply needs a register operand, so materialise constants. */
   for (unsigned l = 0; l < 2; l++)
      if (node_is_const(&NODES(c)[lane_node[l]])) {
         uint8_t ll[2] = { src_lane[l], src_lane[l] };
         uint8_t ls[2] = { src_swz[l], src_swz[l] };
         int load = node_new(c, N_LOAD);

         if (load < 0)
            return -1;
         set_src(&NODES(c)[load], 0, lane_node[l], ll, ls);
         if (lane_node[1 - l] == lane_node[l] &&
             src_lane[1 - l] == src_lane[l] && src_swz[1 - l] == src_swz[l]) {
            lane_node[1 - l] = load;
            src_lane[1 - l] = l;
            src_swz[1 - l] = 1;
         }
         lane_node[l] = load;
         src_lane[l] = l;
         src_swz[l] = 1;
      }
   n_mask = node_new(c, N_LIT);
   if (n_mask < 0)
      return -1;
   memcpy(NODES(c)[n_mask].lit, mask, sizeof(mask));
   if (lane_node[0] == lane_node[1]) {
      uint8_t ll[2] = { src_lane[0], src_lane[1] };
      uint8_t ls[2] = { src_swz[0], src_swz[1] };

      n = node_new(c, N_MUL);
      if (n < 0)
         return -1;
      set_src(&NODES(c)[n], 0, lane_node[0], ll, ls);
      set_src(&NODES(c)[n], 1, n_mask, lane_id, swz_id);
   } else {
      uint8_t l0[2] = { src_lane[0], 0 }, s0[2] = { src_swz[0], 1 };
      uint8_t l1[2] = { 0, src_lane[1] }, s1[2] = { 1, src_swz[1] };

      n = node_new(c, N_GATHER);
      if (n < 0)
         return -1;
      set_src(&NODES(c)[n], 0, lane_node[0], l0, s0);
      set_src(&NODES(c)[n], 1, lane_node[1], l1, s1);
      set_src(&NODES(c)[n], 2, n_mask, lane_id, swz_id);
   }
   return n;
}

static int
build_vec(struct ctx *c, nir_scalar s[4])
{
   bool all_const = true;
   int n;

   for (unsigned k = 0; k < 4; k++) {
      s[k] = nir_scalar_chase_movs(s[k]);
      all_const &= nir_scalar_is_const(s[k]);
   }
   if (all_const) {
      n = node_new(c, N_LIT);
      if (n < 0)
         return -1;
      for (unsigned k = 0; k < 4; k++)
         NODES(c)[n].lit[k] = nir_scalar_as_float(s[k]);
      return n;
   }
   /* vec4(texture.xyz, literal): composite rgbx sources */
   if (nir_scalar_is_const(s[3]) && !nir_scalar_is_const(s[0]) &&
       s[0].def->num_components == 4 &&
       nir_def_instr(s[0].def)->type == nir_instr_type_tex &&
       s[1].def == s[0].def && s[2].def == s[0].def &&
       s[0].comp == 0 && s[1].comp == 1 && s[2].comp == 2) {
      float w = nir_scalar_as_float(s[3]);
      int x = build_def(c, s[0].def);

      if (x < 0)
         return -1;
      util_dynarray_foreach(&c->nodes, struct node, it) {
         if (it->kind == N_COMPOSE && it->src[0] == x && it->lit[0] == w)
            return it - NODES(c);
      }
      n = node_new(c, N_COMPOSE);
      if (n < 0)
         return -1;
      NODES(c)[n].src[0] = x;
      NODES(c)[n].lit[0] = w;
      return n;
   }
   /* vec4(fmul(texture, b).xyz, b.w): the fixed-function texenv keeps the
    * incoming alpha, which is fmul(vec4(texture.xyz, 1), b). */
   if (s[0].comp == 0 && s[1].comp == 1 && s[2].comp == 2 && s[3].comp == 3 &&
       s[1].def == s[0].def && s[2].def == s[0].def && s[3].def != s[0].def &&
       nir_scalar_is_alu(s[0]) && nir_scalar_alu_op(s[0]) == nir_op_fmul) {
      nir_alu_instr *mul = nir_def_as_alu(s[0].def);
      nir_def *other = NULL, *colour = NULL;

      for (unsigned i = 0; i < 2; i++) {
         bool plain = true;

         for (unsigned k = 0; k < 4; k++)
            plain &= mul->src[i].swizzle[k] == k;
         if (!plain)
            continue;
         if (mul->src[i].src.ssa == s[3].def)
            other = mul->src[i].src.ssa;
         else
            colour = mul->src[i].src.ssa;
      }
      if (other && colour) {
         static const uint8_t lane_id[2] = { 0, 1 }, swz_id[2] = { 1, 1 };
         int x = build_def(c, colour), rgb = -1, b;

         if (x < 0)
            return -1;
         /* The alpha of the other factor survives, so this side contributes
          * rgb with alpha one: a texel pair, or one already composed. */
         if (NODES(c)[x].kind == N_COMPOSE && NODES(c)[x].lit[0] == 1.0f) {
            rgb = x;
         } else if (NODES(c)[x].kind != N_TEX) {
            return fail(c, "swizzled or composed vector"), -1;
         } else {
            util_dynarray_foreach(&c->nodes, struct node, it) {
               if (it->kind == N_COMPOSE && it->src[0] == x &&
                   it->lit[0] == 1.0f)
                  rgb = it - NODES(c);
            }
         }
         if (rgb < 0) {
            rgb = node_new(c, N_COMPOSE);
            if (rgb < 0)
               return -1;
            NODES(c)[rgb].src[0] = x;
            NODES(c)[rgb].lit[0] = 1.0f;
         }
         b = build_def(c, other);
         if (b < 0)
            return -1;
         n = node_new(c, N_MUL);
         if (n < 0)
            return -1;
         set_src(&NODES(c)[n], 0, rgb, lane_id, swz_id);
         set_src(&NODES(c)[n], 1, b, lane_id, swz_id);
         return n;
      }
   }
   {
      bool identity = true;

      for (unsigned k = 0; k < 4; k++)
         identity &= s[k].def == s[0].def && s[k].comp == k;
      if (identity) {
         if (s[0].def->num_components != 4 || s[0].def->bit_size != 32)
            return fail(c, "non-vec4 value"), -1;
         return build_def(c, s[0].def);
      }
   }
   /* General gather: each result lane reads one source through a swizzle,
    * with literals masked in by a per-lane (1, 0) factor and added back.
    * The two lanes may read different sources, so a vec4 can be assembled
    * from several values; a constant source is moved into a register first.
    * MUL and ADD are captured forms, so this costs slots, not encodings. */
   {
      static const uint8_t lane_id[2] = { 0, 1 }, swz_id[2] = { 1, 1 };
      int comp_node[4], lane_node[2] = { -1, -1 };
      unsigned comp_idx[4];
      uint8_t src_lane[2] = { 0, 1 }, src_swz[2] = { 1, 1 };
      bool is_lit[4], need_add = false;
      float mask[4], addend[4];
      int n_mask, n_val;

      for (unsigned k = 0; k < 4; k++) {
         is_lit[k] = nir_scalar_is_const(s[k]);
         mask[k] = is_lit[k] ? 0.0f : 1.0f;
         addend[k] = is_lit[k] ? nir_scalar_as_float(s[k]) : 0.0f;
         need_add |= is_lit[k];
         comp_node[k] = -1;
         comp_idx[k] = 0;
         if (is_lit[k])
            continue;
         if (s[k].def->bit_size != 32)
            return fail(c, "swizzled or composed vector"), -1;
         comp_node[k] = build_def(c, s[k].def);
         if (comp_node[k] < 0)
            return -1;
         comp_idx[k] = s[k].comp + def_comp_base(s[k].def);
         if (comp_idx[k] > 3)
            return fail(c, "swizzled or composed vector"), -1;
      }
      /* A lane whose scalars sit in different register pairs, or come from
       * different values, is built as two masked terms and summed. */
      for (unsigned l = 0; l < 2; l++) {
         unsigned a = 2 * l, b = a + 1;

         if (comp_node[a] >= 0 && comp_node[b] >= 0 &&
             (comp_node[a] != comp_node[b] ||
              comp_idx[a] / 2 != comp_idx[b] / 2)) {
            int t0 = gather_term(c, comp_node, comp_idx, is_lit, 0);
            int t1 = gather_term(c, comp_node, comp_idx, is_lit, 1);
            int sum;

            if (t0 == -1 || t1 == -1)
               return -1;
            if (t0 == -2 || t1 == -2)
               return fail(c, "swizzled or composed vector"), -1;
            sum = node_new(c, N_ADD);
            if (sum < 0)
               return -1;
            set_src(&NODES(c)[sum], 0, t0, (const uint8_t[]){ 0, 1 },
                    (const uint8_t[]){ 1, 1 });
            set_src(&NODES(c)[sum], 1, t1, (const uint8_t[]){ 0, 1 },
                    (const uint8_t[]){ 1, 1 });
            n_val = sum;
            goto have_value;
         }
      }
      for (unsigned l = 0; l < 2; l++) {
         unsigned a = 2 * l, b = a + 1;

         if (comp_node[a] >= 0 && comp_node[b] >= 0) {
            lane_node[l] = comp_node[a];
            src_lane[l] = comp_idx[a] / 2;
            src_swz[l] = (comp_idx[a] & 1) << 1 | (comp_idx[b] & 1);
         } else if (comp_node[a] >= 0 || comp_node[b] >= 0) {
            unsigned only = comp_node[a] >= 0 ? comp_idx[a] : comp_idx[b];

            lane_node[l] = comp_node[a] >= 0 ? comp_node[a] : comp_node[b];
            src_lane[l] = only / 2;
            src_swz[l] = (only & 1) << 1 | (only & 1);
         }
      }
      /* A lane of pure literals rides on the other lane's register with a
       * zero factor. */
      for (unsigned l = 0; l < 2; l++)
         if (lane_node[l] < 0) {
            if (lane_node[1 - l] < 0)
               return fail(c, "swizzled or composed vector"), -1;
            lane_node[l] = lane_node[1 - l];
            src_lane[l] = src_lane[1 - l];
            src_swz[l] = src_swz[1 - l];
         }
      /* The multiply needs a register operand, so materialise constants. */
      for (unsigned l = 0; l < 2; l++)
         if (node_is_const(&NODES(c)[lane_node[l]])) {
            uint8_t ll[2] = { src_lane[l], src_lane[l] };
            uint8_t ls[2] = { src_swz[l], src_swz[l] };
            int load = node_new(c, N_LOAD);

            if (load < 0)
               return -1;
            set_src(&NODES(c)[load], 0, lane_node[l], ll, ls);
            if (lane_node[1 - l] == lane_node[l] &&
                src_lane[1 - l] == src_lane[l] &&
                src_swz[1 - l] == src_swz[l]) {
               lane_node[1 - l] = load;
               src_lane[1 - l] = l;
               src_swz[1 - l] = 1;
            }
            lane_node[l] = load;
            src_lane[l] = l;
            src_swz[l] = 1;
         }
      n_mask = node_new(c, N_LIT);
      if (n_mask < 0)
         return -1;
      memcpy(NODES(c)[n_mask].lit, mask, sizeof(mask));
      if (lane_node[0] == lane_node[1]) {
         uint8_t ll[2] = { src_lane[0], src_lane[1] };
         uint8_t ls[2] = { src_swz[0], src_swz[1] };

         n_val = node_new(c, N_MUL);
         if (n_val < 0)
            return -1;
         set_src(&NODES(c)[n_val], 0, lane_node[0], ll, ls);
         set_src(&NODES(c)[n_val], 1, n_mask, lane_id, swz_id);
      } else {
         uint8_t l0[2] = { src_lane[0], 0 }, s0[2] = { src_swz[0], 1 };
         uint8_t l1[2] = { 0, src_lane[1] }, s1[2] = { 1, src_swz[1] };

         n_val = node_new(c, N_GATHER);
         if (n_val < 0)
            return -1;
         set_src(&NODES(c)[n_val], 0, lane_node[0], l0, s0);
         set_src(&NODES(c)[n_val], 1, lane_node[1], l1, s1);
         set_src(&NODES(c)[n_val], 2, n_mask, lane_id, swz_id);
      }
have_value:
      if (!need_add)
         return n_val;
      {
         int n_lit = node_new(c, N_LIT), n_add;

         if (n_lit < 0)
            return -1;
         memcpy(NODES(c)[n_lit].lit, addend, sizeof(addend));
         n_add = node_new(c, N_ADD);
         if (n_add < 0)
            return -1;
         set_src(&NODES(c)[n_add], 0, n_val, lane_id, swz_id);
         set_src(&NODES(c)[n_add], 1, n_lit, lane_id, swz_id);
         return n_add;
      }
   }
}

static int
varying_slot(struct ctx *c, unsigned location)
{
   struct m9_fs_program *p = c->p;

   for (unsigned i = 0; i < p->varying_count; i++)
      if (p->varyings[i].location == location)
         return i;
   /* A location may feed both the texture unit and an interpolation: the
    * driver emits one CPU stream per use from the same draw output. */
   /* Three raster streams besides position, c and d. */
   if (p->varying_count + p->texture_count >= 3)
      return fail(c, "too many interpolated inputs"), -1;
   p->varyings[p->varying_count].location = location;
   p->varyings[p->varying_count].components = 0xf;
   return p->varying_count++;
}

/* Scheduling */

static int
node_level(struct ctx *c, int n)
{
   struct node *nd = &NODES(c)[n];
   int level = -1;

   if (nd->level)
      return nd->level - 1;
   switch (nd->kind) {
   case N_VARY: case N_LOAD: case N_COMPOSE:
      level = 0;
      break;
   case N_TEX: case N_UBO: case N_LIT:
      level = -1;
      break;
   default:
      for (unsigned i = 0; i < 3; i++)
         if (nd->src[i] >= 0)
            level = MAX2(level, node_level(c, nd->src[i]));
      level = MAX2(level + 1, 1);
      break;
   }
   NODES(c)[n].level = level + 1;
   return level;
}

static int
value_new(struct ctx *c, bool interp)
{
   struct value v = { .reg = -1, .def = -1, .last = -1, .interp = interp };

   util_dynarray_append_typed(&c->values, struct value, v);
   return NUM(c, values, struct value) - 1;
}

static void
lop_add(struct ctx *c, const struct lop *l)
{
   util_dynarray_append_typed(&c->lops, struct lop, *l);
}

/* Constant key of lane of a uniform or literal node. */
static struct kkey
node_key(const struct node *s, unsigned lane)
{
   if (s->kind == N_UBO)
      return key_uniform((s->ubo_slot * 4 + s->k + 2 * lane) * 4,
                         (s->ubo_slot * 4 + s->k + 2 * lane + 1) * 4);
   return key_literal(s->lit[2 * lane], s->lit[2 * lane + 1]);
}

/* Fill register or constant operand i of a lane operation from source pos of
 * node nd. */
static void
operand(struct ctx *c, struct lop *l, unsigned i, const struct node *nd,
        unsigned pos)
{
   const struct node *s = &NODES(c)[nd->src[pos]];
   unsigned lane = nd->lane[pos][l->lane], swz = nd->swz[pos][l->lane];

   switch (s->kind) {
   case N_TEX:
      l->opreg[i] = s->texel + 2 * lane;
      l->opswz[i] = swz;
      break;
   case N_COMPOSE:
      l->opreg[i] = NODES(c)[s->src[0]].texel + 2 * lane;
      l->opswz[i] = swz;
      break;
   case N_UBO:
   case N_LIT: {
      struct kkey k = node_key(s, lane);

      l->konst = key_add(c, &k);
      l->kswz = swz;
      break;
   }
   default:
      l->opval[i] = s->value[lane];
      l->opswz[i] = swz;
      break;
   }
}

static void
lop_init(struct lop *l, enum lop_kind kind, unsigned lane, int node)
{
   memset(l, 0, sizeof(*l));
   l->kind = kind;
   l->lane = lane;
   l->node = node;
   l->dst = -1;
   for (unsigned i = 0; i < ARRAY_SIZE(l->opval); i++) {
      l->opval[i] = -1;
      l->opreg[i] = -1;
      l->opswz[i] = 1;
   }
   l->kswz = 1;
   l->konst = -1;
   l->kc2 = -1;
}

/* A register operand read through one scalar in both result lanes. */
static bool
replicated_register(struct ctx *c, const struct node *nd)
{
   for (unsigned i = 0; i < 2; i++) {
      if (nd->src[i] >= 0 && !node_is_const(&NODES(c)[nd->src[i]]) &&
          (nd->swz[i][0] == 0 || nd->swz[i][0] == 3))
         return true;
   }
   return false;
}

/* One instruction of a special-function chain: the base word is the
 * vendor's, the registers are ours.  A constant index rides in the c field
 * for the UNIT 1 forms and in e for the rest, and extra is whatever operand
 * select the instruction needs on top of its base. */
static int
chain_lop_into(struct ctx *c, int n, uint32_t w1, unsigned op0, int a, int b,
               int cc, int konst, bool kc, uint32_t extra, unsigned cls,
               int into)
{
   struct lop l;
   int dst = into >= 0 ? into : value_new(c, false);

   if (dst < 0)
      return -1;
   lop_init(&l, L_CHAIN, 0, n);
   l.keep_dst = into >= 0;
   l.w1 = w1 | extra;
   l.op0 = op0;
   l.cls = cls;
   l.opval[0] = a;
   l.opval[1] = b;
   l.opval[2] = cc;
   l.konst = konst;
   l.kc = kc;
   l.dst = dst;
   lop_add(c, &l);
   return dst;
}

static int
chain_lop_cls(struct ctx *c, int n, uint32_t w1, unsigned op0, int a, int b,
              int cc, int konst, bool kc, uint32_t extra, unsigned cls)
{
   return chain_lop_into(c, n, w1, op0, a, b, cc, konst, kc, extra, cls, -1);
}

static int
chain_lop(struct ctx *c, int n, uint32_t w1, unsigned op0, int a, int b,
          int cc, int konst, bool kc, uint32_t extra)
{
   return chain_lop_into(c, n, w1, op0, a, b, cc, konst, kc, extra, 0, -1);
}

static int
chain_const(struct ctx *c, const uint32_t bits[2])
{
   struct kkey k;
   float v[2];

   memcpy(v, bits, sizeof(v));
   k = key_literal(v[0], v[1]);
   k.pair = true;
   return key_add(c, &k);
}

/* The raster engine publishes the fragment position in two registers: 7a
 * holds x and the raw y, 7b holds z.  The vendor floors 7a and then maps y
 * onto the drawable with a multiply-add, which is the same program its
 * fragcoord case emits. */
#define REG_FRAG_XY 0x7au
#define REG_FRAG_Z  0x7bu

static int
frag_lop(struct ctx *c, int n, uint32_t w1, unsigned op0, int reg, int val,
         int konst, int kc2)
{
   struct lop l;
   int dst = value_new(c, false);

   if (dst < 0)
      return -1;
   lop_init(&l, L_CHAIN, 0, n);
   l.w1 = w1;
   l.op0 = op0;
   l.opreg[reg >= 0 ? 2 : 1] = reg;
   l.opval[1] = val;
   l.konst = konst;
   l.kc2 = kc2;
   l.dst = dst;
   lop_add(c, &l);
   return dst;
}

static int chain_const(struct ctx *c, const uint32_t bits[2]);

static bool
emit_fragcoord(struct ctx *c, int n, struct node *nd)
{
   static const uint32_t flip[2] = { 0x3f800000, 0xbf800000 };  /* 1, -1 */
   static const uint32_t one[2] = { 0x3f800000, 0x3f800000 };
   static const uint32_t zero[2] = { 0, 0 };
   struct kkey k;
   int fl, xy, z, kh;

   memset(&k, 0, sizeof(k));
   k.kind = M9_FS_CONST_FRAG_Y;
   k.pair = true;
   kh = key_add(c, &k);
   fl = frag_lop(c, n, 0xa28fccd0, 0x1c, REG_FRAG_XY, -1, -1, -1);
   xy = frag_lop(c, n, 0xb0400150, 0x1c, -1, fl, chain_const(c, flip), kh);
   z = frag_lop(c, n, 0xb0400150, 0x1c, REG_FRAG_Z, -1,
                chain_const(c, one), chain_const(c, zero));
   if (fl < 0 || xy < 0 || z < 0)
      return false;
   nd->value[0] = xy;
   nd->value[1] = z;
   return true;
}

/* The special-function chains.  exp2 and log2 evaluate a minimax polynomial
 * after a seed instruction that splits the argument; rcp and rsq refine a
 * seed with two Newton steps.  The coefficients and the wiring are the
 * vendor's (fs-exp, fs-log, fs-smoothstep, fs-sqrt), so each sequence is
 * the vendor's with our registers.  Every result lands in the first scalar
 * of a register that both lanes then read. */
static bool
emit_chain(struct ctx *c, int n, struct node *nd)
{
   static const uint32_t exp_k[3][2] = {
      { 0x3af71c15, 0x3c130514 }, { 0x3d64b436, 0x3e75ea9e },
      { 0x3f317271, 0x3f800000 },
   };
   static const uint32_t log_k[4][2] = {
      { 0xbc188576, 0x3d553bd2 }, { 0xbe0cd266, 0x3e77ab67 },
      { 0xbeb1d173, 0x3ef51608 }, { 0xbf389e52, 0x3fb8aa10 },
   };
   static const uint32_t rcp_k[2] = { 0x00000000, 0x40000000 };
   static const uint32_t rsq_clamp[2] = { 0x7f800000, 0xff800000 };
   static const uint32_t rsq_k[2] = { 0x00000000, 0x40400000 };
   struct lop probe;
   int src, t0 = -1, t1 = -1, t2 = -1, t3 = -1, r;
   unsigned swz;

   lop_init(&probe, L_CHAIN, 0, n);
   operand(c, &probe, 0, nd, 0);
   if (probe.konst >= 0)
      return fail(c, "constant special function");
   src = probe.opval[0];
   if (src < 0)
      return fail(c, "special function of a texture read");
   swz = probe.opswz[0] == 3 ? 3 : 0;
   switch (nd->kind) {
   case N_EXP2:
      t0 = chain_lop(c, n, 0x9e851d40, 0x1c, src, -1, -1, -1, false, swz << 4);
      t1 = chain_lop(c, n, 0x982082d0, 0x1c, t0, t0, -1,
                     chain_const(c, exp_k[0]), true, 0);
      t2 = chain_lop(c, n, 0xa2600cf0, 0x1e, t0, t1, t1,
                     chain_const(c, exp_k[1]), false, 0);
      t3 = chain_lop(c, n, 0xa2600cf0, 0x1e, t0, t2, t1,
                     chain_const(c, exp_k[2]), false, 0);
      r = chain_lop(c, n, 0xac2fc0c0, 0x1c, t0, t3, -1, -1, false, 0);
      break;
   case N_LOG2: {
      /* The log2 seed reads the first scalar only, so shuffle it there. */
      int p = chain_lop(c, n, 0xa28ffc00, 0x1d, -1, -1, src, -1, false,
                        swz << 4);

      t0 = chain_lop(c, n, 0x96852ec0, 0x1c, p, -1, -1, -1, false, 0);
      t1 = chain_lop(c, n, 0x944041e0, 0x1c, t0, t0, -1,
                     chain_const(c, log_k[0]), true, 0);
      t2 = chain_lop(c, n, 0xa26008a0, 0x1d, t0, t1, t1,
                     chain_const(c, log_k[1]), false, 0);
      t3 = chain_lop(c, n, 0xa26008a0, 0x1d, t0, t1, t2,
                     chain_const(c, log_k[2]), false, 0);
      t2 = chain_lop(c, n, 0xa26008a0, 0x1d, t0, t1, t3,
                     chain_const(c, log_k[3]), false, 0);
      r = chain_lop(c, n, 0xac4fcf00, 0x1c, t0, t2, t0, -1, false, 0);
      if (p < 0)
         return false;
      break;
   }
   case N_RCP:
      t0 = chain_lop(c, n, 0xae2ff000, 0x1c, src, -1, -1, -1, false, swz << 8);
      for (unsigned i = 0; i < 2; i++) {
         /* The residual reads the argument again, so it needs the same
          * scalar select the seed took. */
         t1 = chain_lop_cls(c, n, 0x8c26ac10, 0x1c, src, t0, -1, -1, false,
                            swz << 8, 5);
         t0 = chain_lop(c, n, 0xae600300, 0x1c, t0, t1, t1,
                        chain_const(c, rcp_k), false, 0);
      }
      t2 = t3 = t0;
      r = t0;
      break;
   default: {
      /* The rsq seed reads the first scalar only and clamps through a
       * constant pair of the two infinities. */
      int p = chain_lop(c, n, 0xa28ffc00, 0x1c, -1, -1, src, -1, false,
                        swz << 4);

      t0 = chain_lop_cls(c, n, 0x9e26b090, 0x1c, p, p, -1,
                         chain_const(c, rsq_clamp), true, 0, 5);
      /* The seed writes a pair whose second scalar the refinement needs
       * again next time round, so each step carries it over first and
       * writes only the first scalar itself. */
      for (unsigned i = 0; i < 2; i++) {
         int k = chain_const(c, rsq_k);

         t1 = chain_lop(c, n, 0xac2fcd00, 0x1c, t0, t0, -1, -1, false, 0);
         t2 = chain_lop(c, n, 0xa28fff30, 0x1e, -1, -1, t0, -1, false, 0);
         t3 = chain_lop_into(c, n, 0xa2603f00, 0x1d, t0, t1, t1, k, false,
                             0, 7, t2);
         if (t1 < 0 || t2 < 0 || t3 < 0)
            return false;
         t0 = t2;
      }
      t2 = t3 = t0;
      r = t0;
      if (p < 0)
         return false;
      break;
   }
   }
   if (t0 < 0 || t1 < 0 || t2 < 0 || t3 < 0 || r < 0)
      return false;
   nd->value[0] = nd->value[1] = r;
   return true;
}

static bool
emit_node_lops(struct ctx *c, int n, bool output, bool direct_ba)
{
   struct node *nd = &NODES(c)[n];
   /* The vendor starts with R,G when an operand register is replicated. */
   bool rg_first = nd->kind == N_MUL && replicated_register(c, nd);

   if (nd->kind == N_FRAGCOORD) {
      if (!emit_fragcoord(c, n, nd))
         return false;
      for (unsigned lane = 0; output && lane < 2; lane++) {
         struct lop l;

         lop_init(&l, L_MOV_OUT, lane, n);
         l.opval[0] = nd->value[lane];
         lop_add(c, &l);
      }
      return true;
   }
   if (nd->kind == N_EXP2 || nd->kind == N_LOG2 ||
       nd->kind == N_RCP || nd->kind == N_RSQ) {
      if (!emit_chain(c, n, nd))
         return false;
      for (unsigned lane = 0; output && lane < (direct_ba ? 2u : 1u); lane++) {
         struct lop l;

         lop_init(&l, L_MOV_OUT, lane, n);
         l.opval[0] = nd->value[lane];
         lop_add(c, &l);
      }
      return true;
   }
   if (nd->kind == N_COMPOSE) {
      struct lop l;
      struct kkey k = key_literal(nd->lit[0], nd->lit[0]);

      lop_init(&l, L_MOVC_SECOND, 1, n);
      l.opreg[0] = NODES(c)[nd->src[0]].texel + 2;
      l.konst = key_add(c, &k);
      lop_add(c, &l);
      return true;
   }
   for (int step = 0; step < 2; step++) {
      int lane = rg_first ? step : 1 - step;
      struct lop l;
      const struct node *b = nd->src[1] >= 0 ? &NODES(c)[nd->src[1]] : NULL;
      bool out = output && (lane == 0 || direct_ba);

      switch (nd->kind) {
      case N_VARY:
         lop_init(&l, L_INTERP, lane, n);
         break;
      case N_LOAD:
         lop_init(&l, L_MOVC_TMP, lane, n);
         operand(c, &l, 0, nd, 0);
         break;
      case N_MUL:
         lop_init(&l, node_is_const(b) ? L_MUL_RC : L_MUL_RR, lane, n);
         operand(c, &l, 0, nd, 0);
         operand(c, &l, 1, nd, 1);
         break;
      case N_ADD:
         lop_init(&l, node_is_const(b) ? L_ADD_RC : L_ADD_RR, lane, n);
         operand(c, &l, 0, nd, 0);
         operand(c, &l, 1, nd, 1);
         break;
      case N_SUB:
         /* b - a */
         lop_init(&l, node_is_const(b) ? L_SUB_CR : L_ADD_RR, lane, n);
         if (node_is_const(b)) {
            operand(c, &l, 0, nd, 0);
            operand(c, &l, 1, nd, 1);
         } else {
            operand(c, &l, 0, nd, 1);
            operand(c, &l, 1, nd, 0);
         }
         break;
      case N_GATHER:
         /* Each result lane multiplies its own source by the lane factor. */
         lop_init(&l, L_MUL_RC, lane, n);
         operand(c, &l, 0, nd, lane);
         operand(c, &l, 1, nd, 2);
         break;
      case N_SAT:
         lop_init(&l, L_SAT, lane, n);
         operand(c, &l, 0, nd, 0);
         break;
      case N_NEG:
         lop_init(&l, L_NEG, lane, n);
         operand(c, &l, 0, nd, 0);
         break;
      case N_CMP:
         lop_init(&l, L_CMP, lane, n);
         operand(c, &l, 0, nd, 0);
         operand(c, &l, 1, nd, 1);
         break;
      case N_SEL:
         lop_init(&l, L_SEL, lane, n);
         for (unsigned i = 0; i < 3; i++)
            operand(c, &l, i, nd, i);
         break;
      case N_FRACT:
      case N_FLOOR:
      case N_CEIL:
         lop_init(&l, nd->kind == N_FRACT ? L_FRACT :
                      nd->kind == N_FLOOR ? L_FLOOR : L_CEIL, lane, n);
         operand(c, &l, 0, nd, 0);
         break;
      case N_MIN:
      case N_MAX:
         if (!node_is_const(b))
            return fail(c, "unsupported arithmetic operation");
         lop_init(&l, nd->kind == N_MIN ? L_MIN_RC : L_MAX_RC, lane, n);
         operand(c, &l, 0, nd, 0);
         operand(c, &l, 1, nd, 1);
         break;
      case N_MAD:
         lop_init(&l, L_MAD, lane, n);
         operand(c, &l, 0, nd, 0);
         operand(c, &l, 1, nd, 1);
         operand(c, &l, 1, nd, 2);
         break;
      default:
         return fail(c, "internal: not an operation");
      }
      if (!out) {
         nd->value[lane] = value_new(c, nd->kind == N_VARY);
         l.dst = nd->value[lane];
      }
      lop_add(c, &l);
   }
   return true;
}

/* How many registers a subtree needs alive at once: two subtrees of equal
 * need cost one more than either alone, an uneven pair costs the larger. */
static int
node_weight(struct ctx *c, int n)
{
   struct node *nd = &NODES(c)[n];
   int w = 0;

   if (nd->weight)
      return nd->weight - 1;
   if (nd->kind == N_VARY || nd->kind == N_LOAD || nd->kind == N_COMPOSE ||
       nd->kind == N_TEX || node_is_const(nd)) {
      nd->weight = 2;
      return 1;
   }
   for (unsigned i = 0; i < 3; i++) {
      int s = NODES(c)[n].src[i] >= 0 ? node_weight(c, NODES(c)[n].src[i]) : 0;

      w = s == w ? w + 1 : MAX2(w, s);
   }
   NODES(c)[n].weight = MAX2(w, 1) + 1;
   return MAX2(w, 1);
}

/* Post-order, heaviest operand first: every operand of n is emitted before
 * n itself and nothing else in between, and the subtree that needs the most
 * registers runs while the fewest of its siblings are alive. */
static bool
schedule_operands(struct ctx *c, int n, int root, bool direct_ba)
{
   struct node *nd = &NODES(c)[n];
   unsigned order[3] = { 0, 1, 2 };

   if (nd->kind == N_VARY || nd->kind == N_LOAD || nd->kind == N_COMPOSE ||
       nd->kind == N_TEX || node_is_const(nd) || nd->value[0] >= 0 ||
       nd->emitted)
      return true;
   nd->emitted = true;
   for (unsigned i = 0; i < 3; i++)
      for (unsigned j = i + 1; j < 3; j++) {
         int a = NODES(c)[n].src[order[i]], b = NODES(c)[n].src[order[j]];

         if ((b >= 0 ? node_weight(c, b) : 0) >
             (a >= 0 ? node_weight(c, a) : 0)) {
            unsigned t = order[i];

            order[i] = order[j];
            order[j] = t;
         }
      }
   for (unsigned i = 0; i < 3; i++)
      if (NODES(c)[n].src[order[i]] >= 0 &&
          !schedule_operands(c, NODES(c)[n].src[order[i]], root, direct_ba))
         return false;
   return emit_node_lops(c, n, n == root, direct_ba);
}

static bool
schedule_tree(struct ctx *c, int root)
{
   struct node *r = &NODES(c)[root];
   int max_level = node_level(c, root);
   unsigned count = NUM(c, nodes, struct node);

   if (r->kind == N_TEX || node_is_const(r))
      return fail(c, "internal: leaf root");
   /* Interpolations: highest varying slot first. */
   for (int k = c->p->varying_count - 1; k >= 0; k--) {
      for (unsigned i = 0; i < count; i++) {
         if (NODES(c)[i].kind == N_VARY && NODES(c)[i].k == (unsigned)k &&
             !emit_node_lops(c, i, i == (unsigned)root, true))
            return false;
      }
   }
   for (unsigned i = 0; i < count; i++) {
      enum node_kind kind = NODES(c)[i].kind;

      if ((kind == N_LOAD || kind == N_COMPOSE) &&
          !emit_node_lops(c, i, false, false))
         return false;
   }
   if (c->depth_first) {
      /* A value dies as soon as its last reader runs, so emitting each
       * operand immediately before the operation that reads it keeps far
       * fewer alive at once than sweeping the graph one level at a time. */
      if (!schedule_operands(c, root, root, max_level == 1))
         return false;
   } else {
      for (int level = 1; level <= max_level; level++) {
         for (unsigned i = 0; i < count; i++) {
            struct node *nd = &NODES(c)[i];

            if (nd->kind != N_VARY && nd->kind != N_LOAD &&
                nd->kind != N_COMPOSE && nd->level - 1 == level &&
                !emit_node_lops(c, i, i == (unsigned)root, max_level == 1))
               return false;
         }
      }
   }
   if (r->kind != N_VARY && max_level > 1) {
      struct lop l;

      lop_init(&l, L_MOV_OUT, 1, root);
      l.opval[0] = r->value[1];
      lop_add(c, &l);
   }
   return true;
}

/* Register allocation */

static bool
reg_busy(uint64_t busy, unsigned reg)
{
   return busy & BITFIELD64_BIT(reg);
}

static bool
allocate(struct ctx *c)
{
   unsigned n = NUM(c, lops, struct lop);
   unsigned base = 4 * c->p->texture_count, next_general;
   static const uint8_t interp_extra[] = { 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f };
   uint64_t busy = 0;

   for (unsigned s = 0; s < n; s++) {
      struct lop *l = &LOPS(c)[s];
      bool rg = l->dst < 0 && l->lane == 0 && l->kind != L_INTERP;

      for (unsigned i = 0; i < ARRAY_SIZE(l->opval); i++) {
         if (l->opval[i] < 0)
            continue;
         VALUES(c)[l->opval[i]].last = s;
         VALUES(c)[l->opval[i]].rg_read |= rg;
      }
      if (l->dst >= 0 && !l->keep_dst)
         VALUES(c)[l->dst].def = s;
      else if (l->dst >= 0)
         VALUES(c)[l->dst].last = MAX2(VALUES(c)[l->dst].last, (int)s);
   }
   for (int s = n - 1; s >= 0; s--) {
      struct lop *l = &LOPS(c)[s];

      for (unsigned i = 0; i < ARRAY_SIZE(l->opval); i++) {
         struct value *v;
         int reg = -1;

         if (l->opval[i] < 0)
            continue;
         v = &VALUES(c)[l->opval[i]];
         if (v->reg >= 0 || v->general)
            continue;
         if (v->interp) {
            /* Blending reads the destination from 18..1b. */
            if (v->rg_read && !c->blend && !reg_busy(busy, REG_I))
               reg = REG_I;
            for (unsigned r = base; reg < 0 && r < base + 2; r++)
               if (!reg_busy(busy, r))
                  reg = r;
            for (unsigned r = c->blend ? 2 : 0;
                 reg < 0 && r < ARRAY_SIZE(interp_extra); r++)
               if (!reg_busy(busy, interp_extra[r]))
                  reg = interp_extra[r];
            if (reg < 0)
               return fail(c, "out of interpolation registers");
         } else if (s != (int)n - 1 && !reg_busy(busy, REG_S0)) {
            reg = REG_S0;
         } else if (s != (int)n - 1 && !reg_busy(busy, REG_S1)) {
            reg = REG_S1;
         } else {
            v->general = true;
            continue;
         }
         v->reg = reg;
         busy |= BITFIELD64_BIT(reg);
      }
      if (l->dst >= 0 && VALUES(c)[l->dst].reg >= 0)
         busy &= ~BITFIELD64_BIT(VALUES(c)[l->dst].reg);
   }
   /* Fresh general registers in forward definition order. */
   next_general = base;
   util_dynarray_foreach(&c->values, struct value, v) {
      if (v->interp && v->reg >= 0 && v->reg < 0x10)
         next_general = MAX2(next_general, (unsigned)v->reg + 1);
   }
   {
      /* A register is released once the value in it has had its last read,
       * which is what lets a shader hold more temporaries than the file
       * has registers; the release happens after the destination of the
       * reading instruction is placed, so the two never collide. */
      uint8_t freed[0x20];
      unsigned num_freed = 0;
      uint64_t taken = 0;

      /* 1a..1f are the interpolation spares; the ones no interpolated
       * value took extend the general run rather than sit idle. */
      util_dynarray_foreach(&c->values, struct value, v)
         if (v->reg >= 0)
            taken |= BITFIELD64_BIT(v->reg);

      for (unsigned s = 0; s < n; s++) {
         struct lop *l = &LOPS(c)[s];

         if (l->dst >= 0 && !l->keep_dst && VALUES(c)[l->dst].general) {
            /* 10..13 carry the scratch and interpolation roles, so the
             * general run continues at 14.  Blending reads the destination
             * from 18..1b and takes its interpolation spares from 1c, so it
             * stops at 17.  Fresh registers come first, so a program that
             * fits the file is laid out the way the vendor lays it out. */
            if (next_general == 0x10)
               next_general = 0x14;
            while (next_general > (c->blend ? 0x17u : 0x19u) &&
                   next_general < 0x20u &&
                   (c->blend || reg_busy(taken, next_general)))
               next_general++;
            if (next_general < 0x20u &&
                (next_general <= (c->blend ? 0x17u : 0x19u) ||
                 !reg_busy(taken, next_general)))
               VALUES(c)[l->dst].reg = next_general++;
            else if (num_freed)
               VALUES(c)[l->dst].reg = freed[--num_freed];
            else
               return fail(c, fail_out_of_temporaries);
         }
         util_dynarray_foreach(&c->values, struct value, v)
            if (v->general && v->reg >= 0 && num_freed < ARRAY_SIZE(freed) &&
                (v->last == (int)s || (v->def == (int)s && v->last < 0)))
               freed[num_freed++] = v->reg;
      }
   }
   util_dynarray_foreach(&c->values, struct value, v) {
      if (v->reg < 0 && v->last >= 0)
         return fail(c, "internal: unallocated value");
   }
   /* Both lanes of every node are emitted, but a value no lane reads is an
    * interpolation the raster engine still steps through.  Drop those. */
   {
      struct lop *lops = LOPS(c);
      unsigned keep = 0;

      for (unsigned s = 0; s < n; s++)
         if (lops[s].dst < 0 || VALUES(c)[lops[s].dst].last >= 0)
            lops[keep++] = lops[s];
      c->lops.size = keep * sizeof(struct lop);
      n = keep;
   }
   if (debug_get_bool_option("M9X_DEBUG_FS", false)) {
      static const char *const names[] = {
         "INTERP", "MUL_RC", "MUL_RR", "ADD_RC", "ADD_RR", "SUB_CR", "MAD",
         "FRACT", "FLOOR", "CEIL", "CMP", "SEL", "CHAIN",
         "MIN_RC", "MAX_RC", "NEG", "SAT",
         "MOV_OUT", "MOVC_OUT", "MOVC_TMP", "MOVC_SECOND", "RAW",
      };
      unsigned i = 0;

      util_dynarray_foreach(&c->values, struct value, v)
         fprintf(stderr, "m9x fs: value %u reg %#x interp %d rg_read %d "
                 "general %d def %d last %d\n", i++, v->reg, v->interp,
                 v->rg_read, v->general, v->def, v->last);
      for (unsigned s = 0; s < n; s++) {
         const struct lop *l = &LOPS(c)[s];

         fprintf(stderr, "m9x fs: lop %u %-11s lane %u node %d dst %d "
                 "src %d/%d/%d reg %d/%d/%d swz %u/%u/%u k %d\n", s,
                 l->kind < ARRAY_SIZE(names) ? names[l->kind] : "?",
                 l->lane, l->node, l->dst, l->opval[0], l->opval[1],
                 l->opval[2], l->opreg[0], l->opreg[1], l->opreg[2],
                 l->opswz[0], l->opswz[1], l->opswz[2], l->konst);
      }
   }
   return true;
}

/* Encoding */

static unsigned
opreg(struct ctx *c, const struct lop *l, unsigned i)
{
   return l->opreg[i] >= 0 ? (unsigned)l->opreg[i] :
          (unsigned)VALUES(c)[l->opval[i]].reg;
}

static void
mark_register(struct ctx *c, unsigned reg)
{
   if (reg >= 0x08 && reg < 0x20 && (reg < 0x10 || reg > 0x13))
      c->p->registers_used |= BITFIELD_BIT((reg - 0x08) / 2);
}

static bool
encode_lops(struct ctx *c)
{
   unsigned n = NUM(c, lops, struct lop);

   /* Constants are numbered backward from the last slot. */
   for (int s = n - 1; s >= 0; s--) {
      if (LOPS(c)[s].kc2 >= 0)
         const_ref(c, LOPS(c)[s].kc2);
      if (LOPS(c)[s].konst >= 0)
         const_ref(c, LOPS(c)[s].konst);
   }
   c->has_consts |= NUM(c, p->consts, struct m9_fs_const) > 0;
   for (unsigned s = 0; s < n; s++) {
      const struct lop *l = &LOPS(c)[s];
      const struct node *nd = &NODES(c)[l->node];
      bool out = l->dst < 0;
      unsigned d = out ? OUT : (unsigned)VALUES(c)[l->dst].reg;
      unsigned op0 = out ? (l->lane ? 0x1e : 0x1d) : 0x1c;
      unsigned e = l->konst >= 0 ? const_ref(c, l->konst) : 0;
      bool pair = l->konst >= 0 && KEYS(c)[l->konst].pair;
      /* (v, v) literals read the first scalar only */
      unsigned kbits = pair ? l->kswz : 0;
      unsigned has = c->has_consts ? 0x3f : 0;
      unsigned a;

      mark_register(c, d);
      switch (l->kind) {
      case L_INTERP:
         a = 0x40 + 8 * nd->k + 2 * l->lane;
         /* Interpolating straight into the output uses the 1a/1b group. */
         if (out)
            mark_register(c, REG_I);
         emit(c, w0_make(a, 0x79, 0x44 + 8 * nd->k + l->lane, d, 0, op0),
              w1_make(0xae003c10, has, a));
         break;
      case L_MUL_RC:
         emit(c, w0_make(NONE, opreg(c, l, 0), NONE, d, 0, op0),
              w1_make(0xa0200000 | kbits << 8 | l->opswz[0] << 6, e, NONE));
         break;
      case L_MUL_RR:
         a = opreg(c, l, 0);
         emit(c, w0_make(a, opreg(c, l, 1), NONE, d, 0, op0),
              w1_make(0xac200000 | l->opswz[0] << 8 | l->opswz[1] << 6, has, a));
         break;
      case L_SAT:
         /* Class 4 saturates the result, as the blend lowering relies on. */
         if (l->opswz[0] != 1)
            return fail(c, "swizzled saturate operand");
         emit(c, w0_make(NONE, NONE, opreg(c, l, 0), d, 4, op0),
              w1_make(0xa2803f10, has, NONE));
         break;
      case L_NEG:
         /* The move forms carry no operand swizzle field. */
         if (l->opswz[0] != 1)
            return fail(c, "swizzled negate operand");
         emit(c, w0_make(NONE, NONE, opreg(c, l, 0), d, 0, op0),
              w1_make(0xa2a03f10, has, NONE));
         break;
      case L_MIN_RC:
      case L_MAX_RC:
         a = opreg(c, l, 0);
         emit(c, w0_make(NONE, a, a, d, l->kind == L_MIN_RC ? 2 : 3, op0),
              w1_make(0xa3200d50 | l->opswz[0] << 6, e, NONE));
         break;
      case L_MOVC_TMP:
         emit(c, w0_make(NONE, NONE, NONE, d, 0, 0x1c),
              w1_make(0xa2803300 | kbits << 4, e, NONE));
         break;
      case L_MOVC_SECOND:
         emit(c, w0_make(NONE, NONE, NONE, l->opreg[0], 0, 0x1e),
              w1_make(0xa2803300, e, NONE));
         break;
      case L_ADD_RC:
         a = opreg(c, l, 0);
         emit(c, w0_make(a, NONE, e, d, 0, op0),
              w1_make(0xbc800100 | (pair ? 0x10 : 0) |
                      (nd->neg ? 0xc0 : 0), 0x3f, a));
         break;
      case L_ADD_RR:
         a = opreg(c, l, 0);
         emit(c, w0_make(a, NONE, opreg(c, l, 1), d, 0, op0),
              w1_make(0xac800d10 | (nd->kind == N_SUB ? 0xc0 : 0), has, a));
         break;
      case L_SUB_CR:
         if (!pair)
            return fail(c, "literal minus register");
         emit(c, w0_make(NONE, NONE, opreg(c, l, 0), d, 0, op0),
              w1_make(0xa0800dd0, e, NONE));
         break;
      case L_MAD:
         emit(c, w0_make(NONE, opreg(c, l, 0), opreg(c, l, 1), d, 0, op0),
              w1_make(0xa0400c50 | (pair ? 0x100 : 0), e, NONE));
         break;
      case L_FRACT:
      case L_FLOOR:
      case L_CEIL: {
         /* The move family's rounding classes, byte-identical in fs-fract,
          * fs-floor and fs-ceil apart from this word. */
         uint32_t base = l->kind == L_FRACT ? 0xa2800f00 :
                         l->kind == L_FLOOR ? 0xa2800cc0 : 0xa2800c00;

         emit(c, w0_make(NONE, NONE, opreg(c, l, 0), d, 0, op0),
              w1_make(base | l->opswz[0] << 4, has, NONE));
         break;
      }
      case L_CMP:
         /* The compare unit writes 1.0 or 0.0; its constant operand is a
          * pair index in the c field, as every UNIT 1 instruction takes it. */
         emit(c, w0_make(NONE, opreg(c, l, 0), e, d, nd->cmp, op0),
              w1_make(0x90000000 | kbits << 8 | l->opswz[0] << 6, 0x05, NONE));
         break;
      case L_SEL: {
         unsigned a0 = opreg(c, l, 0);

         emit(c, w0_make(a0, opreg(c, l, 1), opreg(c, l, 2), d, nd->cmp, op0),
              w1_make(0xaf400c00 | l->opswz[0] << 8 | l->opswz[1] << 6 |
                      l->opswz[2] << 4, has, a0));
         break;
      }
      case L_CHAIN: {
         unsigned ra = l->opval[0] >= 0 || l->opreg[0] >= 0 ?
                       opreg(c, l, 0) : NONE;
         unsigned rb = l->opval[1] >= 0 || l->opreg[1] >= 0 ?
                       opreg(c, l, 1) : NONE;
         unsigned rc = l->opval[2] >= 0 || l->opreg[2] >= 0 ?
                       opreg(c, l, 2) : NONE;

         if (l->kc2 >= 0)
            rc = const_ref(c, l->kc2);
         else if (l->konst >= 0 && l->kc)
            rc = e;
         /* The base word already carries every operand select. */
         emit(c, w0_make(ra, rb, rc, d, l->cls, l->op0),
              w1_make(l->w1, l->konst >= 0 && !l->kc ? e : 0, ra));
         break;
      }
      case L_MOV_OUT:
         emit(c, w0_make(NONE, NONE, opreg(c, l, 0), OUT, 0, op0),
              w1_make(0xa2803f10, NONE, NONE));
         break;
      case L_MOVC_OUT:
         emit(c, w0_make(NONE, NONE, NONE, OUT, 0, op0),
              w1_make(0xa2803300 | (pair ? 0x10 : 0), e, NONE));
         break;
      case L_RAW:
         emit(c, l->w0, l->w1);
         break;
      }
   }
   return true;
}

static void
finish_slots(struct ctx *c)
{
   uint32_t *w;
   unsigned n;

   while (m9_fs_slot_count(c->p) < 4)
      emit(c, STOP_W0, STOP_W1);
   n = m9_fs_slot_count(c->p);
   w = (uint32_t *)c->p->slots.data;
   w[2 * (n - 4) + 1] |= MARKER;
}

/* Vendor templates */

static void
raw(struct ctx *c, uint32_t w0, uint32_t w1)
{
   struct lop l;

   lop_init(&l, L_RAW, 0, 0);
   l.w0 = w0;
   l.w1 = w1;
   lop_add(c, &l);
}

static uint32_t
with_c(uint32_t w0, unsigned cf)
{
   return (w0 & ~(0x7fu << 15)) | cf << 15;
}

static uint32_t
with_e(uint32_t w1, unsigned e)
{
   return (w1 & ~(0x7fu << 14)) | e << 14;
}

static unsigned
key_ref(struct ctx *c, const struct kkey *k)
{
   return const_ref(c, key_add(c, k));
}

/* Distinct scalar constants assembled into 00/01 through 11/13. */
static bool
tmpl_scalars(struct ctx *c, const nir_scalar *s)
{
   struct kkey k[4];
   unsigned e[4];

   for (unsigned i = 0; i < 4; i++) {
      if (nir_scalar_is_const(s[i])) {
         float v = nir_scalar_as_float(s[i]);

         k[i] = key_literal(v, v);
      } else if (nir_scalar_is_intrinsic(s[i]) &&
                 nir_def_as_intrinsic(s[i].def)->intrinsic ==
                    nir_intrinsic_load_ubo_vec4 &&
                 s[i].def->num_components == 1) {
         nir_intrinsic_instr *intr = nir_def_as_intrinsic(s[i].def);

         if (!nir_src_is_const(intr->src[0]) || nir_src_as_uint(intr->src[0]) ||
             !nir_src_is_const(intr->src[1]))
            return fail(c, "unsupported uniform access");
         k[i] = key_uniform((nir_intrinsic_base(intr) +
                             nir_src_as_uint(intr->src[1])) * 16 +
                            nir_intrinsic_component(intr) * 4,
                            M9_FS_UNIFORM_ZERO);
      } else {
         return fail(c, "vector constructor");
      }
      for (unsigned j = 0; j < i; j++)
         if (!memcmp(&k[i], &k[j], sizeof(k[i])))
            return fail(c, "repeated constructor constant");
   }
   /* Entries are created y, w, x, z. */
   e[1] = key_ref(c, &k[1]);
   e[3] = key_ref(c, &k[3]);
   e[0] = key_ref(c, &k[0]);
   e[2] = key_ref(c, &k[2]);
   raw(c, 0xffff801e, with_e(0xa280330f, e[1]));
   raw(c, 0xffff931d, with_e(0xa280330f, e[2]));
   raw(c, 0xffff911d, with_e(0xa280330f, e[0]));
   raw(c, 0xffc9811d, 0xa28fff0f);
   raw(c, 0xffff811e, with_e(0xa280330f, e[3]));
   raw(c, 0xffc8801d, 0xa28fff0f);
   raw(c, 0xffc0ad1e, 0xa29fff1f);
   raw(c, 0xffc02d1d, 0xa29fff1f);
   c->p->registers_used = 0;
   return true;
}

static bool
scalar_is_input(nir_scalar s, unsigned location, unsigned comp)
{
   nir_intrinsic_instr *intr;

   if (!nir_scalar_is_intrinsic(s) || s.comp != comp)
      return false;
   intr = nir_def_as_intrinsic(s.def);
   return intr->intrinsic == nir_intrinsic_load_interpolated_input &&
          nir_intrinsic_io_semantics(intr).location == location &&
          !nir_intrinsic_component(intr);
}

/* vec4(gl_FragCoord.xy * s, gl_FragCoord.z or literal, literal) */
static bool
tmpl_fragcoord(struct ctx *c, const nir_scalar *s)
{
   nir_alu_instr *mul;
   nir_scalar x, y, k;
   float scale;
   unsigned es, ew, ef, ey, e1 = 0, e0 = 0, ez = 0;
   bool z_input;

   if (!nir_scalar_is_alu(s[0]) || s[1].def != s[0].def || s[0].comp != 0 ||
       s[1].comp != 1 || nir_scalar_alu_op(s[0]) != nir_op_fmul ||
       !nir_scalar_is_const(s[3]))
      return fail(c, "unsupported gl_FragCoord use");
   mul = nir_def_as_alu(s[0].def);
   x = nir_scalar_chase_movs(nir_scalar_chase_alu_src(s[0], 0));
   y = nir_scalar_chase_movs(nir_scalar_chase_alu_src(s[1], 0));
   k = nir_scalar_chase_movs(nir_scalar_chase_alu_src(s[0], 1));
   if (!scalar_is_input(x, VARYING_SLOT_POS, 0) ||
       !scalar_is_input(y, VARYING_SLOT_POS, 1) || !nir_scalar_is_const(k) ||
       !nir_scalar_is_const(nir_scalar_chase_movs(
          nir_scalar_chase_alu_src(s[1], 1))) ||
       nir_scalar_as_float(nir_scalar_chase_movs(
          nir_scalar_chase_alu_src(s[1], 1))) != nir_scalar_as_float(k))
      return fail(c, "unsupported gl_FragCoord use");
   (void)mul;
   z_input = scalar_is_input(s[2], VARYING_SLOT_POS, 2);
   if (!z_input && !nir_scalar_is_const(s[2]))
      return fail(c, "unsupported gl_FragCoord use");
   scale = nir_scalar_as_float(k);
   /* The template keeps separate entries for equal literals. */
   {
      struct m9_fs_const *out;
      float w = nir_scalar_as_float(s[3]);
      const float lit[][2] = { { scale, scale }, { w, w }, { 1.0f, -1.0f } };

      for (unsigned i = 0; i < 4; i++) {
         out = util_dynarray_grow(&c->p->consts, struct m9_fs_const, 1);
         memset(out, 0, sizeof(*out));
         if (i == 3) {
            out->kind = M9_FS_CONST_FRAG_Y;
         } else {
            out->kind = M9_FS_CONST_LITERAL;
            out->value[0] = lit[i][0];
            out->value[1] = lit[i][1];
         }
      }
      es = 0; ew = 1; ef = 2; ey = 3;
      if (z_input) {
         const float zl[][2] = { { 1.0f, 1.0f }, { 0.0f, 0.0f } };

         for (unsigned i = 0; i < 2; i++) {
            out = util_dynarray_grow(&c->p->consts, struct m9_fs_const, 1);
            memset(out, 0, sizeof(*out));
            out->value[0] = zl[i][0];
            out->value[1] = zl[i][1];
         }
         e1 = 4;
         e0 = 5;
      } else {
         float z = nir_scalar_as_float(s[2]);

         out = util_dynarray_grow(&c->p->consts, struct m9_fs_const, 1);
         memset(out, 0, sizeof(*out));
         out->value[0] = out->value[1] = z;
         ez = 4;
      }
   }
   raw(c, 0xfffd001c, 0xa28fccdf);
   if (z_input)
      raw(c, with_c(0xfec2931d, e0), with_e(0xb041015f, e1));
   else
      raw(c, 0xffff931d, with_e(0xa281330f, ez));
   raw(c, with_c(0xe001911c, ey), with_e(0xb040815f, ef));
   raw(c, 0xffc9811d, 0xa28fff0f);
   raw(c, 0xffff811e, with_e(0xa280730f, ew));
   raw(c, 0xe47fad1d, with_e(0xa020004f, es));
   raw(c, 0xffc0ad1e, 0xa29fff1f);
   c->p->fragcoord = true;
   return true;
}

/* discard; gl_FragColor = varying; */
static bool
tmpl_discard_varying(struct ctx *c, int root)
{
   const struct node *r = &NODES(c)[root];
   unsigned k = r->k, a;

   if (r->kind != N_VARY)
      return fail(c, "unsupported discard");
   raw(c, 0xffff931d, 0xa280330f);
   a = 0x42 + 8 * k;
   raw(c, w0_make(a, 0x79, 0x45 + 8 * k, 0x00, 0, 0x1c),
       w1_make(0xae003c10, 0x3f, a));
   raw(c, 0xe4c091bc, 0xb360000f);
   a = 0x40 + 8 * k;
   raw(c, w0_make(a, 0x79, 0x44 + 8 * k, 0x1b, 0, 0x1c),
       w1_make(0xae003c10, 0x3f, a));
   raw(c, 0x3fc8951e, 0xac8fcc32);
   raw(c, 0xffc02d1e, 0xa28fff1f);
   raw(c, 0xbfffc0bc, 0xafdfc302);
   raw(c, 0xffcdad1d, 0xa29fff1f);
   c->p->registers_used = BITFIELD_BIT((0x1a - 0x08) / 2) |
                          BITFIELD_BIT((0x14 - 0x08) / 2);
   c->p->discard = true;
   return true;
}

/* Compare and select templates (fill-rect-stipple, fill-rect-opaque-stipple) */

static float
float_from_bits(uint32_t bits)
{
   float f;

   memcpy(&f, &bits, sizeof(f));
   return f;
}

static void
const_push(struct ctx *c, const struct kkey *k)
{
   struct m9_fs_const *out = util_dynarray_grow(&c->p->consts,
                                                struct m9_fs_const, 1);

   memset(out, 0, sizeof(*out));
   out->kind = k->kind;
   memcpy(out->value, k->v, sizeof(out->value));
   memcpy(out->offset, k->off, sizeof(out->offset));
}

/* Constant buffer slot of a whole vec4 uniform, or -1. */
static int
ubo_vec4_slot(nir_def *def)
{
   int slot = -1;

   if (def->num_components != 4)
      return -1;
   for (unsigned k = 0; k < 4; k++) {
      nir_scalar s = nir_scalar_chase_movs(nir_get_scalar(def, k));
      nir_intrinsic_instr *intr;
      int this_slot;

      if (!nir_scalar_is_intrinsic(s) || s.comp != k)
         return -1;
      intr = nir_def_as_intrinsic(s.def);
      if (intr->intrinsic != nir_intrinsic_load_ubo_vec4 ||
          !nir_src_is_const(intr->src[0]) || nir_src_as_uint(intr->src[0]) ||
          !nir_src_is_const(intr->src[1]) || nir_intrinsic_component(intr))
         return -1;
      this_slot = nir_intrinsic_base(intr) + nir_src_as_uint(intr->src[1]);
      if (k && this_slot != slot)
         return -1;
      slot = this_slot;
   }
   return slot;
}

/* feq(texture(sampler, varying).w, literal) of the only texture */
static bool
match_alpha_compare(struct ctx *c, nir_scalar cond, float *literal)
{
   nir_scalar t, k;

   cond = nir_scalar_chase_movs(cond);
   if (!nir_scalar_is_alu(cond) || nir_scalar_alu_op(cond) != nir_op_feq)
      return fail(c, "unsupported compare");
   t = nir_scalar_chase_movs(nir_scalar_chase_alu_src(cond, 0));
   k = nir_scalar_chase_movs(nir_scalar_chase_alu_src(cond, 1));
   if (nir_scalar_is_const(t)) {
      nir_scalar tmp = t;

      t = k;
      k = tmp;
   }
   if (!nir_scalar_is_const(k) || t.comp != 3 ||
       nir_def_instr(t.def)->type != nir_instr_type_tex)
      return fail(c, "unsupported compare");
   *literal = nir_scalar_as_float(k);
   if (build_def(c, t.def) < 0)
      return false;
   return c->p->texture_count == 1 || fail(c, "compare with two textures");
}

/* st lowers the alpha test to discard_if(!(alpha OP reference)); the vendor
 * kill slot classes are GEQUAL 0, LESS 1, GREATER 2, LEQUAL 3, NOTEQUAL 4,
 * EQUAL 5 (captures/m9x-linux-gl15-ff-20260915). */
static bool
match_alpha_test(struct ctx *c, nir_intrinsic_instr *kill, nir_def *colour)
{
   static const struct { nir_op op; bool swap; unsigned func; } map[] = {
      { nir_op_flt, false, 1 }, { nir_op_flt, true, 2 },
      { nir_op_fge, false, 0 }, { nir_op_fge, true, 3 },
      { nir_op_feq, false, 5 }, { nir_op_fneu, false, 4 },
   };
   nir_scalar cond = nir_scalar_chase_movs(nir_get_scalar(kill->src[0].ssa, 0));
   nir_scalar out_a = nir_scalar_chase_movs(nir_get_scalar(colour, 3));
   nir_scalar cmp, src[2];
   /* st lowers the fixed-function test to discard_if(!(alpha OP ref)); a
    * GLSL discard states the condition directly, so the kill class is the
    * opposite one, and the six classes pair up as func ^ 1. */
   bool inverted = nir_scalar_is_alu(cond) &&
                   nir_scalar_alu_op(cond) == nir_op_inot;

   cmp = inverted ? nir_scalar_chase_movs(nir_scalar_chase_alu_src(cond, 0)) :
                    cond;
   if (!nir_scalar_is_alu(cmp))
      return false;
   for (unsigned i = 0; i < 2; i++)
      src[i] = nir_scalar_chase_movs(nir_scalar_chase_alu_src(cmp, i));
   for (unsigned i = 0; i < ARRAY_SIZE(map); i++) {
      nir_scalar alpha = src[map[i].swap], ref = src[!map[i].swap];
      nir_intrinsic_instr *intr;

      unsigned func = map[i].func ^ (inverted ? 0 : 1);

      if (map[i].op != nir_scalar_alu_op(cmp) ||
          !nir_scalar_equal(alpha, out_a))
         continue;
      /* A GLSL discard compares against a literal as often as a uniform. */
      if (nir_scalar_is_const(ref)) {
         c->alpha.func = func;
         c->alpha.ref_literal = true;
         c->alpha.ref_value = nir_scalar_as_float(ref);
         return true;
      }
      if (!nir_scalar_is_intrinsic(ref))
         continue;
      intr = nir_def_as_intrinsic(ref.def);
      if (intr->intrinsic != nir_intrinsic_load_ubo_vec4 ||
          !nir_src_is_const(intr->src[0]) || nir_src_as_uint(intr->src[0]) ||
          !nir_src_is_const(intr->src[1]))
         continue;
      c->alpha.func = func;
      c->alpha.ref_literal = false;
      c->alpha.ref_offset = ((nir_intrinsic_base(intr) +
                              nir_src_as_uint(intr->src[1])) * 4 +
                             nir_intrinsic_component(intr) + ref.comp) * 4;
      return true;
   }
   return false;
}

static void
push_uniform_lanes(struct ctx *c, int slot, unsigned lane)
{
   struct kkey k = key_uniform((slot * 4 + 2 * lane) * 4,
                               (slot * 4 + 2 * lane + 1) * 4);

   const_push(c, &k);
}

/* if (texture.w == literal) discard; color = uniform; */
static bool
tmpl_stipple(struct ctx *c, nir_intrinsic_instr *kill, nir_def *color)
{
   static const uint32_t words[] = {
      0xffc284bc, 0x9000d03f, 0xe082059d, 0x900140cf, 0xbfc202bc, 0x8800cc10,
      0xffff821c, 0xa280730f, 0xffff851d, 0xa280730f, 0xffff951c, 0xa280f31f,
      0xe14102bc, 0x5360400f, 0xffff881c, 0xa280331f, 0x5fc1071e, 0xac8fcc30,
      0xffffc0bc, 0xafdfc300, 0xffcaad1d, 0xa29fff1f, 0xffc42d1e, 0xa29fff1f,
   };
   int slot = ubo_vec4_slot(color);
   struct kkey k;
   float lit;

   if (slot < 0)
      return fail(c, "unsupported conditional discard");
   if (!match_alpha_compare(c, nir_get_scalar(kill->src[0].ssa, 0), &lit))
      return false;
   push_uniform_lanes(c, slot, 1);
   k = key_literal(0.0f, 1.0f);
   const_push(c, &k);
   k = key_literal(1.0f, 0.5f);
   const_push(c, &k);
   push_uniform_lanes(c, slot, 0);
   k = key_literal(lit, lit);
   const_push(c, &k);
   k = key_literal(float_from_bits(0x40000001), 1.0f);
   const_push(c, &k);
   for (unsigned i = 0; i < ARRAY_SIZE(words); i += 2)
      raw(c, words[i], words[i + 1]);
   c->p->registers_used = 0x43;
   c->p->discard = true;
   c->custom_consts = true;
   return true;
}

/* color = texture.w == literal ? uniform_a : uniform_b; */
static bool
tmpl_select(struct ctx *c, const nir_scalar *s)
{
   static const uint32_t words[] = {
      0xffc286bc, 0x9000d03f, 0xe082059d, 0x900140cf, 0xbfc302bc, 0x8800cc10,
      0xffff861c, 0xa280f31f, 0xffc1071c, 0x80004c1f, 0xbfc3829c, 0x8800cc10,
      0xffff821c, 0xa280b31f, 0xffc3091c, 0xa28fff1f, 0xffff891c, 0x4280731f,
      0xffc1081c, 0xa28fff1f, 0xffff881c, 0x4280331f, 0xffc4ad1d, 0xa29fff1f,
      0xffc42d1e, 0xa29fff1f,
   };
   nir_alu_instr *alu = nir_def_as_alu(s[0].def);
   int on_true, on_false;
   struct kkey k;
   float lit;

   for (unsigned i = 0; i < 4; i++) {
      if (s[i].def != s[0].def || s[i].comp != i)
         return fail(c, "unsupported select");
   }
   for (unsigned i = 1; i < 3; i++) {
      for (unsigned j = 0; j < 4; j++)
         if (alu->src[i].swizzle[j] != j)
            return fail(c, "unsupported select");
   }
   on_true = ubo_vec4_slot(alu->src[1].src.ssa);
   on_false = ubo_vec4_slot(alu->src[2].src.ssa);
   if (on_true < 0 || on_false < 0)
      return fail(c, "unsupported select");
   if (!match_alpha_compare(c, nir_scalar_chase_alu_src(s[0], 0), &lit))
      return false;
   push_uniform_lanes(c, on_false, 1);
   push_uniform_lanes(c, on_false, 0);
   push_uniform_lanes(c, on_true, 1);
   push_uniform_lanes(c, on_true, 0);
   k = key_literal(lit, lit);
   const_push(c, &k);
   k = key_literal(float_from_bits(0x40000001), 1.0f);
   const_push(c, &k);
   k = key_literal(0.0f, 1.0f);
   const_push(c, &k);
   k = key_literal(1.0f, 0.5f);
   const_push(c, &k);
   for (unsigned i = 0; i < ARRAY_SIZE(words); i += 2)
      raw(c, words[i], words[i + 1]);
   c->p->registers_used = 0x01;
   c->custom_consts = true;
   return true;
}

/* The vendor binds the texture read through a replicated scalar to the first
 * texel pair, otherwise the last sampled one. */
static bool
assign_texels(struct ctx *c)
{
   struct m9_fs_program *p = c->p;
   unsigned order[M9_FS_MAX_TEXTURES], replicated = 0;
   uint8_t textures[M9_FS_MAX_TEXTURES], coords[M9_FS_MAX_TEXTURES];
   uint8_t comps[M9_FS_MAX_TEXTURES], pair_of[M9_FS_MAX_TEXTURES];

   util_dynarray_foreach(&c->nodes, struct node, n) {
      for (unsigned i = 0; i < 3; i++) {
         const struct node *s = n->src[i] >= 0 ? &NODES(c)[n->src[i]] : NULL;

         if (!s || s->kind != N_TEX)
            continue;
         /* A composed alpha overwrites the texel register in place. */
         if (n->kind != N_COMPOSE) {
            util_dynarray_foreach(&c->nodes, struct node, m) {
               if (m->kind == N_COMPOSE && m->src[0] == n->src[i])
                  return fail(c, "texture read with and without composed alpha");
            }
         }
         if (n->swz[i][0] == 0 || n->swz[i][0] == 3)
            replicated |= BITFIELD_BIT(s->texel / 4);
      }
   }
   for (unsigned i = 0; i < p->texture_count; i++)
      order[i] = p->texture_count - 1 - i;
   if (p->texture_count == 2 && util_bitcount(replicated) == 1) {
      order[0] = u_bit_scan(&replicated);
      order[1] = 1 - order[0];
   }
   for (unsigned i = 0; i < p->texture_count; i++) {
      textures[i] = p->textures[order[i]];
      coords[i] = p->texcoords[order[i]];
      comps[i] = p->texcoord_components[order[i]];
      pair_of[order[i]] = i;
   }
   memcpy(p->textures, textures, p->texture_count);
   memcpy(p->texcoords, coords, p->texture_count);
   memcpy(p->texcoord_components, comps, p->texture_count);
   util_dynarray_foreach(&c->nodes, struct node, n) {
      if (n->kind == N_TEX)
         n->texel = 4 * pair_of[n->texel / 4];
   }
   return true;
}

/* Blending */

#define REG_DST 0x18u        /* destination R,G; B,A in 1a */
#define SWZ_ALPHA 3u
#define SWZ_LANE 1u

enum bterm { B_ZERO, B_ONE, B_REG, B_INV };

struct bfactor {
   enum bterm term;
   unsigned reg, swz;
};

static unsigned
slot_a(uint32_t w0, uint32_t w1)
{
   return (w1 & 0xf) << 3 | w0 >> 29;
}

static int
blend_temp(struct ctx *c, uint32_t *used)
{
   /* 10..13 are the scratch roles and 18..1b hold the destination. */
   static const uint8_t pool[] = {
      0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
      0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
      0x14, 0x15, 0x16, 0x17,
   };

   for (unsigned i = 0; i < ARRAY_SIZE(pool); i++) {
      unsigned r = pool[i];

      if (!(*used & BITFIELD_BIT(r))) {
         *used |= BITFIELD_BIT(r);
         mark_register(c, r);
         return r;
      }
   }
   return fail(c, "out of temporaries for blending"), -1;
}

static void
blend_emit(struct ctx *c, uint32_t w0, uint32_t w1)
{
   emit(c, w0, w1);
}

/* temp = K1 - reg.a, replicated (captured OMSA/OMDA factor load) */
static int
blend_inv_alpha(struct ctx *c, uint32_t *used, unsigned reg, unsigned k1)
{
   int t = blend_temp(c, used);

   if (t >= 0)
      blend_emit(c, w0_make(NONE, NONE, reg, t, 0, 0x1c),
                 w1_make(0xa0800cf0, k1, NONE));
   return t;
}

static struct bfactor
blend_factor(unsigned f, unsigned lane, unsigned s, unsigned s_ba)
{
   switch (f) {
   case M9_FS_ONE: return (struct bfactor){ B_ONE };
   case M9_FS_SRC_ALPHA: return (struct bfactor){ B_REG, s_ba, SWZ_ALPHA };
   case M9_FS_INV_SRC_ALPHA: return (struct bfactor){ B_INV, s_ba, SWZ_ALPHA };
   case M9_FS_DST_ALPHA: return (struct bfactor){ B_REG, REG_DST + 2, SWZ_ALPHA };
   case M9_FS_INV_DST_ALPHA: return (struct bfactor){ B_INV, REG_DST + 2, SWZ_ALPHA };
   case M9_FS_SRC_COLOR: return (struct bfactor){ B_REG, s, SWZ_LANE };
   case M9_FS_INV_SRC_COLOR: return (struct bfactor){ B_INV, s, SWZ_LANE };
   case M9_FS_DST_COLOR:
      return (struct bfactor){ B_REG, REG_DST + 2 * lane, SWZ_LANE };
   case M9_FS_INV_DST_COLOR:
      return (struct bfactor){ B_INV, REG_DST + 2 * lane, SWZ_LANE };
   default: return (struct bfactor){ B_ZERO };
   }
}

/* out = S * fs + D * fd for one lane, from instruction forms captured in
 * captures/m9x-vendor-glamor-blend-20260915. */
static bool
blend_lane(struct ctx *c, uint32_t *used, unsigned lane, unsigned s,
           unsigned s_ba, unsigned k0, unsigned k1)
{
   const unsigned op0 = 0x1d + lane, d = REG_DST + 2 * lane;
   struct bfactor fs = blend_factor(c->blend->src, lane, s, s_ba);
   struct bfactor fd = blend_factor(c->blend->dst, lane, s, s_ba);
   int t;

   /* Inverted factors other than the fused forms load 1 - alpha. */
   if (fs.term == B_INV) {
      if (fs.swz != SWZ_ALPHA)
         return fail(c, "ONE_MINUS_SRC_COLOR source factor");
      if ((t = blend_inv_alpha(c, used, fs.reg, k1)) < 0)
         return false;
      fs = (struct bfactor){ B_REG, t, SWZ_ALPHA };
   }
   if (fd.term == B_INV && fs.term == B_REG && fd.reg != s) {
      /* Only the alpha form of the constant-minus-register load is captured. */
      if (fd.swz != SWZ_ALPHA)
         return fail(c, "inverted destination colour factor");
      if ((t = blend_inv_alpha(c, used, fd.reg, k1)) < 0)
         return false;
      fd = (struct bfactor){ B_REG, t, SWZ_ALPHA };
   }

   if (fs.term != B_REG && fd.term == B_ZERO) {
      /* ZERO/ZERO */
      blend_emit(c, w0_make(NONE, s, NONE, OUT, 4, op0),
                 w1_make(0xa0200040, k0, NONE));
   } else if (fs.term == B_ZERO && fd.term == B_ONE) {
      blend_emit(c, w0_make(NONE, d, NONE, OUT, 4, op0),
                 w1_make(0xa0200040, k1, NONE));
   } else if (fs.term == B_ONE && fd.term == B_ONE) {
      blend_emit(c, w0_make(NONE, s, d, OUT, 4, op0),
                 w1_make(0xa0400c50, k1, NONE));
   } else if (fs.term != B_REG && fd.term == B_INV) {
      /* S * K + D * (1 - r) */
      blend_emit(c, w0_make(s, fd.reg, d, OUT, 4, op0),
                 w1_make(0xa4e00040 | fd.swz << 10,
                         fs.term == B_ONE ? k1 : k0, s));
   } else if (fs.term == B_ZERO) {
      blend_emit(c, w0_make(fd.reg, d, NONE, OUT, 4, op0),
                 w1_make(0xac200040 | fd.swz << 8, NONE, fd.reg));
   } else if (fs.term == B_ONE) {
      blend_emit(c, w0_make(fd.reg, d, s, OUT, 4, op0),
                 w1_make(0xac400c50 | fd.swz << 8, NONE, fd.reg));
   } else if (fd.term == B_ZERO) {
      blend_emit(c, w0_make(fs.reg, s, NONE, OUT, 4, op0),
                 w1_make(0xac200040 | fs.swz << 8, NONE, fs.reg));
   } else if (fd.term == B_ONE) {
      blend_emit(c, w0_make(fs.reg, s, d, OUT, 4, op0),
                 w1_make(0xac400c50 | fs.swz << 8, NONE, fs.reg));
   } else if (fd.term == B_INV) {
      /* r.swz * S + D * (1 - S.swz), the inverted operand is S itself */
      blend_emit(c, w0_make(fs.reg, s, d, OUT, 4, op0),
                 w1_make(0xa4a00040 | fd.swz << 10 | fs.swz << 8, NONE,
                         fs.reg));
   } else {
      if ((t = blend_temp(c, used)) < 0)
         return false;
      blend_emit(c, w0_make(fd.reg, d, NONE, t, 0, 0x1c),
                 w1_make(0xac200040 | fd.swz << 8, NONE, fd.reg));
      blend_emit(c, w0_make(fs.reg, s, t, OUT, 4, op0),
                 w1_make(0xac400c50 | fs.swz << 8, NONE, fs.reg));
   }
   return true;
}

/* Registers the encoded program reads or writes, plus the texel pairs.  A
 * reason rejects programs touching 18..1b, which only blending needs free. */
static bool
program_registers(struct ctx *c, uint32_t *used, const char *reason)
{
   const uint32_t *w = (const uint32_t *)c->p->slots.data;
   unsigned n = m9_fs_slot_count(c->p);

   *used = BITFIELD_MASK(4 * c->p->texture_count);
   for (unsigned i = 0; i < n; i++) {
      uint32_t w0 = w[2 * i], w1 = w[2 * i + 1];
      const unsigned field[4] = { slot_a(w0, w1), w0 >> 22 & 0x7f,
                                  w0 >> 15 & 0x7f, w0 >> 8 & 0x7f };

      for (unsigned f = 0; f < 4; f++) {
         if (reason && field[f] >= REG_DST && field[f] < REG_DST + 4)
            return fail(c, reason);
         if (field[f] < 0x20)
            *used |= BITFIELD_BIT(field[f]);
      }
   }
   return true;
}

/* Send both colour lanes to saturated temporaries instead of the output. */
static bool
redirect_output(struct ctx *c, uint32_t *used, int s[2])
{
   uint32_t *w = (uint32_t *)c->p->slots.data;
   unsigned n = m9_fs_slot_count(c->p);

   for (unsigned lane = 0; lane < 2; lane++)
      if ((s[lane] = blend_temp(c, used)) < 0)
         return false;
   for (unsigned i = 0; i < n; i++) {
      uint32_t *w0 = &w[2 * i];
      unsigned op0 = *w0 & 0x1f;

      if ((*w0 >> 8 & 0x7f) != OUT)
         continue;
      if (op0 != 0x1d && op0 != 0x1e)
         return fail(c, "internal: partial colour write");
      *w0 = (*w0 & ~0x7fffu) | s[op0 - 0x1d] << 8 | 4u << 5 | 0x1c;
   }
   return true;
}

/* Redirect the colour writes to saturated temporaries and append the blend
 * against the destination registers 18..1b. */
static bool
blend_append(struct ctx *c, const int have[2])
{
   unsigned consts = NUM(c, p->consts, struct m9_fs_const);
   uint32_t used;
   int s[2] = { have[0], have[1] };

   if (c->custom_consts)
      return fail(c, "blending with a constant template");
   if (!program_registers(c, &used,
                          "blending with a program using registers 18..1b"))
      return false;
   if (s[0] < 0 && !redirect_output(c, &used, s))
      return false;
   /* The blend code runs after every other slot, so once the colour lanes
    * are written the program's general registers are dead and the blend
    * temporaries may reuse them.  The texel pairs stay reserved. */
   used &= ~0x00f0ff00u;
   for (unsigned lane = 0; lane < 2; lane++)
      used |= BITFIELD_BIT(s[lane]);
   /* 88 bit 4 clears whenever the destination is blended. */
   c->p->registers_used |= BITFIELD_BIT(0);
   c->p->blend_dest = c->blend->src != M9_FS_ZERO ||
                      c->blend->dst != M9_FS_ZERO;
   /* The pad pair precedes the (0,1), (1,0.5) tail. */
   return blend_lane(c, &used, 1, s[1], s[1], consts + 1, consts + 2) &&
          blend_lane(c, &used, 0, s[0], s[1], consts + 1, consts + 2);
}

/* Alpha test (captures/m9x-linux-gl15-ff-20260915, cases alpha_*): the
 * comparison value comes from the colour alpha and the (0.5, 255) and (2, 1)
 * helpers; the kill slot carries the compare function in its instruction
 * class and the reference constant pair in its e field. */
static bool
alpha_append(struct ctx *c, int s[2])
{
   uint32_t used;
   unsigned ref;
   struct m9_fs_const *k;
   int t;

   if (c->custom_consts)
      return fail(c, "alpha test with a constant template");
   /* Only blending needs 18..1b free; the kill slot reads no destination. */
   if (!program_registers(c, &used, c->blend ?
                          "alpha test with a program using registers 18..1b" :
                          NULL))
      return false;
   if (!redirect_output(c, &used, s))
      return false;
   used |= BITFIELD_BIT(s[0]) | BITFIELD_BIT(s[1]);
   t = blend_temp(c, &used);
   if (t < 0)
      return false;
   ref = NUM(c, p->consts, struct m9_fs_const);
   k = util_dynarray_grow(&c->p->consts, struct m9_fs_const, 3);
   if (!k)
      return fail(c, "out of memory");
   memset(k, 0, 3 * sizeof(*k));
   /* The kill slot compares in 8.8 fixed point, as M9_FS_CONST_ALPHA_REF
    * scales a uniform reference; a literal is scaled here instead. */
   if (c->alpha.ref_literal) {
      k[0].kind = M9_FS_CONST_LITERAL;
      k[0].value[0] = k[0].value[1] = c->alpha.ref_value * 256.0f;
   } else {
      k[0].kind = M9_FS_CONST_ALPHA_REF;
      k[0].offset[0] = k[0].offset[1] = c->alpha.ref_offset;
   }
   k[1].value[0] = 0.5f;
   k[1].value[1] = 255.0f;
   k[2].value[0] = 2.0f;
   k[2].value[1] = 1.0f;
   emit(c, w0_make(NONE, s[1], NONE, t, 0, 0x1d),
        w1_make(0xa04003c0, ref + 1, NONE));
   emit(c, w0_make(NONE, NONE, t, t, 0, 0x1d), 0xa29fcccf);
   emit(c, w0_make(NONE, t, NONE, 0x40, c->alpha.func, 0x1c),
        w1_make(0xa3c00000, ref, NONE));
   c->has_consts = true;
   return true;
}

/* Move the colour temporaries to the output, as the vendor does after a
 * kill slot when nothing blends. */
static void
output_temps(struct ctx *c, const int s[2])
{
   for (unsigned lane = 0; lane < 2; lane++)
      emit(c, w0_make(NONE, NONE, s[lane], OUT, 0, 0x1d + lane), 0xa29fff1f);
}

/* Output assembly */

/* A template that did not match must leave nothing behind, so that the
 * general compiler starts from an empty program. */
static void
reset_output(struct ctx *c)
{
   c->reason = NULL;
   c->custom_consts = false;
   c->has_consts = false;
   c->p->registers_used = 0;
   c->p->discard = false;
   util_dynarray_clear(&c->keys);
   util_dynarray_clear(&c->key_index);
   util_dynarray_clear(&c->p->consts);
   util_dynarray_clear(&c->p->slots);
   util_dynarray_clear(&c->lops);
}

static bool
compile_output(struct ctx *c, nir_def *value, bool discard)
{
   nir_scalar s[4];
   bool all_const = true, fragcoord = false;
   int root;

   for (unsigned i = 0; i < 4; i++) {
      s[i] = nir_scalar_chase_movs(nir_get_scalar(value, i));
      all_const &= nir_scalar_is_const(s[i]);
      fragcoord |= scalar_is_input(s[i], VARYING_SLOT_POS, i);
   }
   if (!discard && nir_scalar_is_alu(s[0]) &&
       nir_scalar_alu_op(s[0]) == nir_op_fmul &&
       s[0].def->num_components == 2 && tmpl_fragcoord(c, s))
      return true;
   if (!discard && nir_scalar_is_alu(s[0]) &&
       nir_scalar_alu_op(s[0]) == nir_op_bcsel && tmpl_select(c, s))
      return true;
   reset_output(c);
   if (!discard && !fragcoord && (all_const || (s[0].def != s[1].def)) &&
       tmpl_scalars(c, s))
      return true;
   reset_output(c);
   root = build_vec(c, s);
   if (root < 0 || !assign_texels(c))
      return false;
   if (discard)
      return tmpl_discard_varying(c, root);
   switch (NODES(c)[root].kind) {
   case N_TEX: {
      unsigned t = NODES(c)[root].texel;
      struct lop l;

      lop_init(&l, L_MOV_OUT, 0, root);
      l.opreg[0] = t;
      lop_add(c, &l);
      lop_init(&l, L_MOV_OUT, 1, root);
      l.opreg[0] = t + 2;
      lop_add(c, &l);
      return true;
   }
   case N_COMPOSE: {
      unsigned t = NODES(c)[NODES(c)[root].src[0]].texel;
      struct lop l;

      emit_node_lops(c, root, false, false);
      lop_init(&l, L_MOV_OUT, 0, root);
      l.opreg[0] = t;
      lop_add(c, &l);
      lop_init(&l, L_MOV_OUT, 1, root);
      l.opreg[0] = t + 2;
      lop_add(c, &l);
      return true;
   }
   case N_UBO:
   case N_LIT:
      for (int lane = 1; lane >= 0; lane--) {
         struct lop l;
         struct kkey k = node_key(&NODES(c)[root], lane);

         lop_init(&l, L_MOVC_OUT, lane, root);
         l.konst = key_add(c, &k);
         lop_add(c, &l);
      }
      return true;
   default:
      if (schedule_tree(c, root) && allocate(c))
         return true;
      /* A wide expression can outlive the register file laid out one level
       * at a time; scheduling each operand next to its reader is tighter. */
      if (c->depth_first || c->reason != fail_out_of_temporaries)
         return false;
      c->reason = NULL;
      c->depth_first = true;
      c->p->registers_used = 0;
      util_dynarray_clear(&c->lops);
      util_dynarray_clear(&c->values);
      util_dynarray_clear(&c->keys);
      util_dynarray_clear(&c->key_index);
      util_dynarray_clear(&c->p->consts);
      util_dynarray_foreach(&c->nodes, struct node, it) {
         it->value[0] = it->value[1] = -1;
         it->emitted = false;
         it->weight = 0;
      }
      return schedule_tree(c, root) && allocate(c);
   }
}

static bool
compile_impl(struct ctx *c, nir_function_impl *impl)
{
   nir_intrinsic_instr *store = NULL, *kill = NULL;
   bool discard = false, first = true;
   int colour[2] = { -1, -1 };

   if (exec_list_length(&impl->body) != 1)
      return fail(c, "control flow");
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         nir_intrinsic_instr *intr;

         if (instr->type != nir_instr_type_intrinsic)
            continue;
         intr = nir_instr_as_intrinsic(instr);
         switch (intr->intrinsic) {
         case nir_intrinsic_terminate:
            /* Only an unconditional discard ahead of all other code. */
            if (!first || discard)
               return fail(c, "conditional discard");
            discard = true;
            break;
         case nir_intrinsic_terminate_if:
            if (kill)
               return fail(c, "more than one conditional discard");
            kill = intr;
            break;
         case nir_intrinsic_store_output: {
            unsigned location = nir_intrinsic_io_semantics(intr).location;

            if ((location != FRAG_RESULT_COLOR &&
                 location != FRAG_RESULT_DATA0) || store ||
                nir_intrinsic_write_mask(intr) != 0xf ||
                !nir_src_is_const(intr->src[1]) ||
                nir_src_as_uint(intr->src[1]) ||
                intr->src[0].ssa->num_components != 4)
               return fail(c, "unsupported fragment output");
            store = intr;
            break;
         }
         case nir_intrinsic_load_interpolated_input:
         case nir_intrinsic_load_barycentric_pixel:
         case nir_intrinsic_load_barycentric_centroid:
         case nir_intrinsic_load_ubo_vec4:
            break;
         case nir_intrinsic_load_input:
            break;
         default:
            /* intrinsic names are static strings */
            return fail(c, nir_intrinsic_infos[intr->intrinsic].name);
         }
         first &= intr->intrinsic == nir_intrinsic_terminate;
      }
   }
   if (!store)
      return fail(c, "no color output");
   if (kill && match_alpha_test(c, kill, store->src[0].ssa))
      c->alpha.enabled = true;
   else if (kill && (discard || !tmpl_stipple(c, kill, store->src[0].ssa)))
      return fail(c, "unsupported conditional discard");
   if ((!kill || c->alpha.enabled) &&
       !compile_output(c, store->src[0].ssa, discard))
      return false;
   c->has_consts |= discard;
   if (!encode_lops(c))
      return false;
   if (c->alpha.enabled) {
      if (!alpha_append(c, colour))
         return false;
      if (!c->blend)
         output_temps(c, colour);
   }
   if (c->blend && !blend_append(c, colour))
      return false;
   finish_slots(c);
   if (c->blend) {
      struct m9_fs_const *pad = util_dynarray_grow(&c->p->consts,
                                                   struct m9_fs_const, 1);

      memset(pad, 0, sizeof(*pad));
   }
   if (!c->custom_consts)
      const_tail(c);
   return true;
}

/* Uniforms are lowered to load_ubo_vec4, which nir_find_inlinable_uniforms
 * does not track: collect the constant buffer dwords an if condition reads. */
static bool
cond_dwords(nir_scalar s, uint16_t *dw, unsigned *count, unsigned depth)
{
   if (nir_scalar_is_const(s))
      return true;
   if (depth > 16)
      return false;
   if (nir_scalar_is_alu(s)) {
      nir_alu_instr *alu = nir_def_as_alu(s.def);

      if (nir_op_infos[alu->op].output_size && !nir_op_is_vec(alu->op))
         return false;
      for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++)
         if (!cond_dwords(nir_scalar_chase_alu_src(s, i), dw, count, depth + 1))
            return false;
      return true;
   }
   if (nir_scalar_is_intrinsic(s)) {
      nir_intrinsic_instr *intr = nir_def_as_intrinsic(s.def);
      unsigned word;

      if (intr->intrinsic != nir_intrinsic_load_ubo_vec4 ||
          !nir_src_is_const(intr->src[0]) || nir_src_as_uint(intr->src[0]) ||
          !nir_src_is_const(intr->src[1]))
         return false;
      word = (nir_intrinsic_base(intr) + nir_src_as_uint(intr->src[1])) * 4 +
             nir_intrinsic_component(intr) + s.comp;
      for (unsigned i = 0; i < *count; i++)
         if (dw[i] == word)
            return true;
      if (*count == MAX_INLINABLE_UNIFORMS)
         return false;
      dw[(*count)++] = word;
      return true;
   }
   return false;
}

void
m9_fs_find_inlinable_uniforms(nir_shader *nir)
{
   uint16_t dw[MAX_INLINABLE_UNIFORMS] = { 0 };
   unsigned count = 0;

   if (nir->info.stage != MESA_SHADER_FRAGMENT)
      return;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_if *nif = nir_block_get_following_if(block);
         uint16_t trial[MAX_INLINABLE_UNIFORMS];
         unsigned n = count;

         if (!nif)
            continue;
         memcpy(trial, dw, sizeof(dw));
         if (cond_dwords(nir_get_scalar(nif->condition.ssa, 0), trial, &n, 0)) {
            memcpy(dw, trial, sizeof(dw));
            count = n;
         }
      }
   }
   memcpy(nir->info.inlinable_uniform_dw_offsets, dw, sizeof(dw));
   nir->info.num_inlinable_uniforms = count;
}

static bool
inline_ubo_vec4(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   const uint32_t *values = data;
   const struct shader_info *info = &b->shader->info;
   nir_scalar comps[NIR_MAX_VEC_COMPONENTS];
   bool hit = false;
   nir_def *vec;
   unsigned base;

   if (intr->intrinsic != nir_intrinsic_load_ubo_vec4 ||
       !nir_src_is_const(intr->src[0]) || nir_src_as_uint(intr->src[0]) ||
       !nir_src_is_const(intr->src[1]) || intr->def.bit_size != 32)
      return false;
   base = (nir_intrinsic_base(intr) + nir_src_as_uint(intr->src[1])) * 4 +
          nir_intrinsic_component(intr);
   b->cursor = nir_after_instr(&intr->instr);
   for (unsigned c = 0; c < intr->def.num_components; c++) {
      comps[c] = nir_get_scalar(&intr->def, c);
      for (unsigned i = 0; i < info->num_inlinable_uniforms; i++) {
         if (info->inlinable_uniform_dw_offsets[i] != base + c)
            continue;
         comps[c] = nir_get_scalar(nir_build_imm(b, 1, 32,
                                   &(nir_const_value){ .u32 = values[i] }), 0);
         hit = true;
      }
   }
   if (!hit)
      return false;
   vec = nir_vec_scalars(b, comps, intr->def.num_components);
   nir_def_rewrite_uses_after(&intr->def, vec);
   return true;
}

void
m9_fs_inline_uniforms(nir_shader *nir, const uint32_t *values)
{
   bool progress;

   if (!nir->info.num_inlinable_uniforms ||
       !nir_shader_intrinsics_pass(nir, inline_ubo_vec4,
                                   nir_metadata_control_flow, (void *)values))
      return;
   do {
      progress = false;
      NIR_PASS(progress, nir, nir_opt_constant_folding);
      NIR_PASS(progress, nir, nir_opt_dead_cf);
      NIR_PASS(progress, nir, nir_opt_remove_phis);
      NIR_PASS(progress, nir, nir_opt_peephole_select,
               &(nir_opt_peephole_select_options){ .limit = 8 });
      NIR_PASS(progress, nir, nir_opt_copy_prop);
      NIR_PASS(progress, nir, nir_opt_algebraic);
      NIR_PASS(progress, nir, nir_opt_cse);
      NIR_PASS(progress, nir, nir_opt_dce);
   } while (progress);
}

/* The special-function chains compute one scalar each, so a vector one is
 * split rather than refused; the vector paths reassemble the components.
 * The captured dot products take one constant operand, which a shader that
 * dots two computed vectors cannot use, so those are expanded too. */
static bool
is_transcendental(const nir_instr *instr, const void *data)
{
   const nir_alu_instr *alu;

   (void)data;
   if (instr->type != nir_instr_type_alu)
      return false;
   alu = nir_instr_as_alu(instr);
   switch (alu->op) {
   case nir_op_fexp2:
   case nir_op_flog2:
   case nir_op_frcp:
   case nir_op_frsq:
   case nir_op_fsqrt:
   case nir_op_fdot2:
   case nir_op_fdot3:
   case nir_op_fdot4:
      return true;
   default:
      return false;
   }
}

/* Everything but the chains vectorises again afterwards, so the arithmetic
 * around them goes back to whole vectors instead of being reassembled from
 * scalars by the gather. */
static uint8_t
vectorize_around_chains(const nir_instr *instr, const void *data)
{
   return is_transcendental(instr, data) ? 1 : 4;
}

bool
m9_fs_compile(struct nir_shader *nir, const struct m9_fs_blend *blend,
              struct m9_fs_program *program, const char **reason)
{
   /* ONE/ZERO is the unblended program, as the vendor emits it. */
   bool opaque = !blend || (blend->src == M9_FS_ONE && blend->dst == M9_FS_ZERO);
   struct ctx c = { .nir = nir, .p = program, .blend = opaque ? NULL : blend };
   bool ok = false;

   memset(program, 0, sizeof(*program));
   util_dynarray_init(&program->slots, NULL);
   util_dynarray_init(&program->consts, NULL);
   util_dynarray_init(&c.nodes, NULL);
   util_dynarray_init(&c.lops, NULL);
   util_dynarray_init(&c.values, NULL);
   util_dynarray_init(&c.keys, NULL);
   util_dynarray_init(&c.key_index, NULL);

   if (nir->info.stage != MESA_SHADER_FRAGMENT) {
      fail(&c, "not a fragment shader");
   } else {
      /* st hands IO and arithmetic over scalarised; the native program is
       * a two-component machine, so put the vectors back first. */
      NIR_PASS(_, nir, nir_opt_vectorize_io,
               nir_var_shader_in | nir_var_shader_out, false);
      NIR_PASS(_, nir, nir_opt_vectorize, NULL, NULL);
      NIR_PASS(_, nir, nir_lower_alu_to_scalar, is_transcendental, NULL);
      NIR_PASS(_, nir, nir_opt_vectorize, vectorize_around_chains, NULL);
      NIR_PASS(_, nir, nir_opt_copy_prop);
      NIR_PASS(_, nir, nir_opt_dce);
      nir_foreach_function_impl(impl, nir) {
         if (!impl->function->is_entrypoint)
            continue;
         ok = compile_impl(&c, impl);
      }
   }
   util_dynarray_fini(&c.key_index);
   util_dynarray_fini(&c.nodes);
   util_dynarray_fini(&c.lops);
   util_dynarray_fini(&c.values);
   util_dynarray_fini(&c.keys);
   if (!ok) {
      *reason = c.reason ? c.reason : "unsupported shader";
      m9_fs_program_fini(program);
      memset(program, 0, sizeof(*program));
   }
   return ok;
}

void
m9_fs_program_fini(struct m9_fs_program *program)
{
   util_dynarray_fini(&program->slots);
   util_dynarray_fini(&program->consts);
}

unsigned
m9_fs_slot_count(const struct m9_fs_program *program)
{
   return util_dynarray_num_elements(&program->slots, uint32_t) / 2;
}

uint32_t
m9_fs_raster_88(const struct m9_fs_program *program)
{
   uint32_t r88 = 0x00100000 | 0xfff0;
   unsigned low = 0;

   for (unsigned k = 0; k < program->varying_count; k++)
      r88 |= BITFIELD_BIT(24 + k);
   if (program->texture_count > 1)
      r88 |= 0x00200000;
   if (program->fragcoord)
      r88 |= 0x00080000;
   /* Bits 15:4 flag register pairs 08/09 .. 1e/1f the program leaves unused. */
   r88 &= ~((uint32_t)program->registers_used << 4);
   if (program->texture_count > 1) {
      r88 &= ~0x70u;
      low = 0xf;
   } else if (program->texture_count) {
      r88 &= ~0x10u;
      low = 0x3;
   } else if (program->discard || program->fragdepth) {
      r88 &= ~0x30u;
      low = 0xc;
   }
   if (debug_get_bool_option("M9X_DEBUG_FS", false))
      fprintf(stderr, "m9x fs: r88 %08x slots %u consts %u varyings %u "
              "textures %u fragcoord %d\n", r88 | low,
              m9_fs_slot_count(program),
              (unsigned)util_dynarray_num_elements(&program->consts,
                                                   struct m9_fs_const),
              program->varying_count, program->texture_count,
              program->fragcoord);
   return r88 | low;
}
