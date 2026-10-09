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

#include "m9_draw_params.h"
#include "m9_specialise.h"
#include "m9_srgb.h"

#include "pipe/p_defines.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"
#include "util/u_math.h"
#include "util/bitset.h"

const nir_shader_compiler_options m9_fs_nir_options = {
   .fdot_replicates = false,
   .lower_bitops = true,
   .lower_extract_byte = true,
   .lower_extract_word = true,
   .lower_fdiv = true,
   .lower_fdph = true,
   .lower_fpow = true,
   .lower_fmod = true,
   .lower_fsign = true,
   .lower_ftrunc = true,
   .lower_scmp = true,
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
/* The destination a colour output writes.  Target p is derived to own
 * OUT + 4p.  MEASURED on the M9128 by binding four targets, writing one
 * output and reading which surface received it: 0x2d reaches target 0,
 * 0x30 and 0x31 target 1, 0x34 and 0x35 target 2, 0x38 and 0x39 target 3,
 * and 0x32, 0x33 and the rest reach none of them -- two destinations a
 * target, four apart, of which the captured single-target program uses the
 * odd one.  The sweep that measured it is still here: M9X_FS_MRT_BASE and
 * M9X_FS_MRT_STEP move every target, M9X_FS_MRT_DESTp names one outright,
 * M9X_FS_MRT_CLS tests the competing routing class, and
 * M9X_FS_COLOR_TARGET shifts every output by one target so a single-output
 * shader can name a target other than its own.
 * docs/parhelia-m9-mrt-routing.md */
static unsigned
color_dest(unsigned target)
{
   unsigned base = (unsigned)debug_get_num_option("M9X_FS_MRT_BASE", OUT);
   unsigned step = (unsigned)debug_get_num_option("M9X_FS_MRT_STEP", 4);
   unsigned p = (target +
                 (unsigned)debug_get_num_option("M9X_FS_COLOR_TARGET", 0)) & 3;
   char name[20];
   const char *one;

   /* M9X_FS_MRT_DESTp names one target's destination outright, which is how
    * a sweep moves one target while the rest stay where they are known to
    * land. */
   snprintf(name, sizeof(name), "M9X_FS_MRT_DEST%u", p);
   one = getenv(name);
   return (one ? (unsigned)strtoul(one, NULL, 0) : base + step * p) & 0x7fu;
}

/* The output move's instruction class.  Class 4 is what a blended output
 * carries, so the competing candidate seats target p at M9X_FS_MRT_CLS + p
 * and leaves every target on one destination. */
static unsigned
color_cls(unsigned target)
{
   unsigned cls = (unsigned)debug_get_num_option("M9X_FS_MRT_CLS", 0);
   unsigned p = (target +
                 (unsigned)debug_get_num_option("M9X_FS_COLOR_TARGET", 0)) & 3;

   return cls ? (cls + p) & 7u : 0;
}
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
   N_LOAD,              /* constant factor moved into a register */
   N_COMPOSE,           /* vec4(texture.xyz, literal), written in place */
   /* A value whose two lanes name their own source, held on the diagonal
    * src[l]/lane[l][l]/swz[l][l].  It emits nothing: a reader resolves
    * through it to the source its own lane reads. */
   N_PAIR,
   N_FACE,              /* gl_FrontFacing: zero in scalar 0 for a back face */
   N_DDX, N_DDY,        /* screen-space derivative, fs-dfdx / fs-dfdy */
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
   bool broadcast, bcast_known;  /* both scalars of a lane agree */
   bool uniform, uniform_known;  /* all four scalars hold one value */
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
   L_DEPTH, L_KILL,
   L_DERIV,             /* two idle slots, then the derivative; cls the axis */
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
   uint8_t target;      /* colour target this output lop names */
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
   struct util_dynarray gathers;  /* vectors already assembled from scalars */
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
   /* Values earlier raster passes produce, reachable through the record:
    * cut[i] is what pass i handed on, and reading it here binds a route. */
   nir_def *cut[M9_FS_MAX_PASSES];
   unsigned cut_count;
   nir_def *root;       /* value this pass computes, NULL the shader output */
   /* One colour output a target, in target order.  More than one is an MRT
    * shader: every root shares the pass's expression graph and each is
    * moved to its own target's destination. */
   nir_def *target_def[M9_FS_MAX_TARGETS];
   unsigned target_count;
   /* The computed coordinate that stopped this pass, for the splitter to
    * cut at: the pass before hands it over and the unit samples at it. */
   nir_def *coord_blocker;
   bool record_out;     /* write 0x11/0x13 for a later pass, not the colour */
   bool custom_consts;  /* a template supplies the whole constant span */
   bool depth_first;    /* schedule operands before breadth, for pressure */
   bool no_gather_reuse;  /* rebuild each assembled vector, for pressure */
   bool outputs_last;     /* outputs moved last: keep the scratch pair free */
};

struct gather_key {
   nir_scalar s[4];
   int node;
};

#define NODES(c) ((struct node *)(c)->nodes.data)
#define LOPS(c) ((struct lop *)(c)->lops.data)
#define VALUES(c) ((struct value *)(c)->values.data)
#define KEYS(c) ((struct kkey *)(c)->keys.data)
#define NUM(c, a, t) util_dynarray_num_elements(&(c)->a, t)

static const char fail_out_of_temporaries[] = "out of temporaries";
static const char fail_out_of_interp_registers[] =
   "out of interpolation registers";
static const char fail_too_many_textures[] =
   "more texture fetches than one raster pass can bind";
/* The engine samples before the program runs, so a coordinate the fragment
 * stage computes cannot feed a fetch in the same pass.  It can feed one in
 * the next, through a coordinate route, so this failure names a cut rather
 * than an unsupported shader. */
static const char fail_too_many_interp[] = "too many interpolated inputs";
static const char fail_coord_not_varying[] = "texture coordinate is not a varying";

unsigned
m9_fs_coord_route_count(const struct m9_fs_program *program)
{
   unsigned n = 0;

   for (unsigned i = 0; i < program->texture_count; i++)
      n += program->texcoord_source[i] != M9_FS_ROUTE_NONE;
   return n;
}

/* Coordinate streams the pass interpolates, keyed as m9_fs_streams keys
 * them: units that agree in location, first component, projector and
 * transform read one stream, so a pass can bind more units than it has
 * streams left for. */
unsigned
m9_fs_coord_stream_count(const struct m9_fs_program *p)
{
   unsigned n = 0;

   for (unsigned i = 0; i < p->texture_count; i++) {
      bool shared = false;

      if (p->texture_source[i] != M9_FS_ROUTE_NONE ||
          p->texcoord_source[i] != M9_FS_ROUTE_NONE)
         continue;
      for (unsigned k = 0; k < i; k++)
         shared |= p->texture_source[k] == M9_FS_ROUTE_NONE &&
                   p->texcoord_source[k] == M9_FS_ROUTE_NONE &&
                   p->texcoords[k] == p->texcoords[i] &&
                   p->texcoord_components[k] == p->texcoord_components[i] &&
                   p->texcoord_projective[k] == p->texcoord_projective[i] &&
                   m9_fs_coord_affine_same(&p->texcoord_affine[k],
                                           &p->texcoord_affine[i]);
      n += !shared;
   }
   return n;
}

unsigned
m9_fs_sampler_count(const struct m9_fs_program *program)
{
   unsigned n = 0;

   for (unsigned i = 0; i < program->texture_count; i++)
      n += program->texture_source[i] == M9_FS_ROUTE_NONE;
   return n;
}

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

/* Field by field: a key copied by assignment leaves its padding undefined. */
static bool
key_equal(const struct kkey *a, const struct kkey *b)
{
   return a->kind == b->kind && a->pair == b->pair &&
          !memcmp(a->v, b->v, sizeof(a->v)) &&
          !memcmp(a->off, b->off, sizeof(a->off));
}

static int
key_add(struct ctx *c, const struct kkey *k)
{
   util_dynarray_foreach(&c->keys, struct kkey, it) {
      if (key_equal(it, k))
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

/* Whether both scalars of every lane of a node's result hold the same value:
 * an operand read through a replicating select contributes one, and so does
 * one whose own two scalars already agree.  Nothing that names two different
 * scalars does: the raster position holds x and y, and the rsq chain carries
 * its Newton residual in the second scalar of its result. */
static bool
node_broadcast(struct ctx *c, int n)
{
   struct node *nd = &NODES(c)[n];
   bool all = true;

   if (nd->bcast_known)
      return nd->broadcast;
   nd->bcast_known = true;
   nd->broadcast = false;
   switch (nd->kind) {
   case N_LIT:
      nd->broadcast = nd->lit[0] == nd->lit[1] && nd->lit[2] == nd->lit[3];
      break;
   case N_MUL: case N_ADD: case N_SUB: case N_MAD: case N_MIN: case N_MAX:
   case N_NEG: case N_FRACT: case N_FLOOR: case N_CEIL: case N_CMP:
   case N_SEL: case N_SAT: case N_LOAD:
      for (unsigned i = 0; i < 3; i++)
         if (NODES(c)[n].src[i] >= 0) {
            const struct node *m = &NODES(c)[n];

            all &= ((m->swz[i][0] == 0 || m->swz[i][0] == 3) &&
                    (m->swz[i][1] == 0 || m->swz[i][1] == 3)) ||
                   node_broadcast(c, m->src[i]);
         }
      NODES(c)[n].broadcast = all;
      break;
   default:
      break;
   }
   return NODES(c)[n].broadcast;
}

/* Whether every scalar of a node's result holds the same value, so that a
 * vector assembled from that one value is the node itself. */
static bool
node_uniform(struct ctx *c, int n)
{
   struct node *nd = &NODES(c)[n];
   bool all = true;

   if (nd->uniform_known)
      return nd->uniform;
   nd->uniform_known = true;
   nd->uniform = false;
   switch (nd->kind) {
   case N_LIT:
      nd->uniform = nd->lit[0] == nd->lit[1] && nd->lit[1] == nd->lit[2] &&
                    nd->lit[2] == nd->lit[3];
      break;
   case N_MUL: case N_ADD: case N_SUB: case N_MAD: case N_MIN: case N_MAX:
   case N_NEG: case N_FRACT: case N_FLOOR: case N_CEIL: case N_CMP:
   case N_SEL: case N_SAT: case N_LOAD:
      for (unsigned i = 0; i < 3; i++) {
         const struct node *m = &NODES(c)[n];
         int src = m->src[i];

         if (src < 0)
            continue;
         /* Both result lanes have to read the same scalars of the operand,
          * and those scalars have to hold the one value. */
         all &= (m->lane[i][0] == m->lane[i][1] &&
                 m->swz[i][0] == m->swz[i][1] &&
                 (m->swz[i][0] == 0 || m->swz[i][0] == 3 ||
                  node_broadcast(c, src))) ||
                node_uniform(c, src);
      }
      NODES(c)[n].uniform = all;
      break;
   default:
      break;
   }
   return NODES(c)[n].uniform;
}

/* Two scalar selects read one after the other: bit 1 of a select names the
 * source scalar of result scalar 0 and bit 0 that of result scalar 1. */
static unsigned
swz_compose(unsigned outer, unsigned inner)
{
   return ((inner >> (1 - ((outer >> 1) & 1))) & 1) << 1 |
          ((inner >> (1 - (outer & 1))) & 1);
}

static bool
node_is_const(const struct node *n)
{
   return n->kind == N_UBO || n->kind == N_LIT;
}

/* A pair carries a scalar select of its own, which the forms without an
 * operand swizzle field cannot express. */
static bool
pair_swizzled(struct ctx *c, int n)
{
   const struct node *nd;

   if (n < 0 || NODES(c)[n].kind != N_PAIR)
      return false;
   nd = &NODES(c)[n];
   for (unsigned l = 0; l < 2; l++)
      if (nd->swz[l][l] != 1 || pair_swizzled(c, nd->src[l]))
         return true;
   return false;
}

static int varying_slot(struct ctx *c, unsigned location);
static int build_route(struct ctx *c, unsigned pass);
static int build_vec(struct ctx *c, nir_scalar s[4]);
static int build_def(struct ctx *c, nir_def *def);
static void set_src(struct node *n, unsigned i, int src, const uint8_t *lane,
                    const uint8_t *swz);

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
         n = node_new(c, N_FRAGCOORD);
         return n < 0 ? -1 : (c->p->fragcoord = true, n);
      }
      /* Register 7b carries z and the facing under different selects; the
       * vendor compiler reads both from it in one program (emulated). */
      if (location == VARYING_SLOT_FACE) {
         n = node_new(c, N_FACE);
         if (n < 0)
            return -1;
         NODES(c)[n].is_cond = true;
         c->p->face = true;
         return n;
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
   case nir_intrinsic_ddx:
   case nir_intrinsic_ddx_fine:
   case nir_intrinsic_ddx_coarse:
   case nir_intrinsic_ddy:
   case nir_intrinsic_ddy_fine:
   case nir_intrinsic_ddy_coarse: {
      static const uint8_t lane_id[2] = { 0, 1 }, swz_id[2] = { 1, 1 };
      bool y = intr->intrinsic == nir_intrinsic_ddy ||
               intr->intrinsic == nir_intrinsic_ddy_fine ||
               intr->intrinsic == nir_intrinsic_ddy_coarse;
      nir_def *src = intr->src[0].ssa;
      nir_scalar s[4];
      int a;

      for (unsigned k = 0; k < 4; k++)
         s[k] = nir_get_scalar(src, MIN2(k, src->num_components - 1));
      a = build_vec(c, s);
      if (a < 0)
         return -1;
      /* A constant does not change across the quad. */
      if (node_is_const(&NODES(c)[a])) {
         n = node_new(c, N_LIT);
         return n;
      }
      n = node_new(c, y ? N_DDY : N_DDX);
      if (n < 0)
         return -1;
      set_src(&NODES(c)[n], 0, a, lane_id, swz_id);
      return n;
   }
   case nir_intrinsic_load_ubo_vec4:
      /* A narrower load still names the whole vec4 slot; the components
       * above it are simply never read. */
      if (!nir_src_is_const(intr->src[0]) || nir_src_as_uint(intr->src[0]))
         return fail(c, "uniform block other than the first"), -1;
      if (!nir_src_is_const(intr->src[1]))
         return fail(c, "indirectly indexed uniform"), -1;
      if (nir_intrinsic_component(intr) + intr->def.num_components > 4 ||
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

/* A texture coordinate component, read as varying * scale + bias.  The
 * raster engine samples at an interpolated varying and cannot compute a
 * coordinate, but interpolation is linear, so an affine function of a
 * varying folds into a varying the driver computes per vertex and the
 * sample lands exactly where the shader asked. */
struct coord_expr {
   bool varying;
   unsigned location, component;
   struct m9_fs_coord_term scale, bias;
};

static bool
term_is_literal(const struct coord_expr *e)
{
   return !e->varying && e->bias.offset == M9_FS_UNIFORM_ZERO &&
          e->bias.factor == 0.0f && e->scale.literal == 0.0f &&
          e->scale.factor == 0.0f;
}

/* A product of two coefficients, which stays in this form only while at
 * most one of them names a uniform. */
static bool
term_mul(struct m9_fs_coord_term *t, const struct m9_fs_coord_term *o)
{
   if (t->offset == M9_FS_UNIFORM_ZERO) {
      float k = t->literal;

      t->literal = o->literal * k;
      t->factor = o->factor * k;
      /* A coefficient scaled to nothing no longer names the uniform. */
      t->offset = t->factor == 0.0f ? M9_FS_UNIFORM_ZERO : o->offset;
      t->reciprocal = t->offset != M9_FS_UNIFORM_ZERO && o->reciprocal;
      return true;
   }
   if (o->offset != M9_FS_UNIFORM_ZERO)
      return false;
   t->literal *= o->literal;
   t->factor *= o->literal;
   return true;
}

static bool
term_add(struct m9_fs_coord_term *t, const struct m9_fs_coord_term *o)
{
   if (o->offset != M9_FS_UNIFORM_ZERO) {
      /* Two uniform terms would need a wider form than this one. */
      if (t->offset != M9_FS_UNIFORM_ZERO)
         return false;
      t->offset = o->offset;
      t->factor = o->factor;
      t->reciprocal = o->reciprocal;
   }
   t->literal += o->literal;
   return true;
}

static bool
coord_parse(nir_scalar s, struct coord_expr *e)
{
   memset(e, 0, sizeof(*e));
   e->scale.offset = e->bias.offset = M9_FS_UNIFORM_ZERO;
   s = nir_scalar_chase_movs(s);
   if (nir_scalar_is_const(s)) {
      e->bias.literal = nir_scalar_as_float(s);
      return true;
   }
   if (nir_scalar_is_intrinsic(s)) {
      nir_intrinsic_instr *in = nir_def_as_intrinsic(s.def);

      if (in->intrinsic == nir_intrinsic_load_interpolated_input) {
         e->varying = true;
         e->location = nir_intrinsic_io_semantics(in).location;
         e->component = nir_intrinsic_component(in) + s.comp;
         e->scale.literal = 1.0f;
         return true;
      }
      if (in->intrinsic == nir_intrinsic_load_ubo_vec4 &&
          nir_src_is_const(in->src[0]) && !nir_src_as_uint(in->src[0]) &&
          nir_src_is_const(in->src[1])) {
         unsigned slot = nir_intrinsic_base(in) + nir_src_as_uint(in->src[1]);

         e->bias.offset = (slot * 4 + nir_intrinsic_component(in) +
                           s.comp) * 4;
         e->bias.factor = 1.0f;
         return true;
      }
      return false;
   }
   if (!nir_scalar_is_alu(s))
      return false;
   switch (nir_scalar_alu_op(s)) {
   case nir_op_fmul:
   case nir_op_ffma:
   case nir_op_ffma_weak: {
      bool fused = nir_scalar_alu_op(s) != nir_op_fmul;
      struct coord_expr a, b, add;

      if (!coord_parse(nir_scalar_chase_alu_src(s, 0), &a) ||
          !coord_parse(nir_scalar_chase_alu_src(s, 1), &b))
         return false;
      if (b.varying) {
         struct coord_expr t = a;

         a = b;
         b = t;
      }
      /* Neither may be a product of two varyings, and the coefficient side
       * carries its value in its bias, having no varying of its own. */
      if (b.varying)
         return false;
      *e = a;
      if (!term_mul(&e->scale, &b.bias) || !term_mul(&e->bias, &b.bias))
         return false;
      if (!fused)
         return true;
      if (!coord_parse(nir_scalar_chase_alu_src(s, 2), &add) || add.varying)
         return false;
      return term_add(&e->bias, &add.bias);
   }
   case nir_op_frcp: {
      /* One over a uniform: the reciprocal stays on the coefficient, which
       * the driver resolves per draw.  Only a bare uniform qualifies -- a
       * reciprocal of a sum is not a coefficient of this shape. */
      if (!coord_parse(nir_scalar_chase_alu_src(s, 0), e) || e->varying ||
          e->bias.offset == M9_FS_UNIFORM_ZERO || e->bias.reciprocal ||
          e->bias.literal != 0.0f || e->bias.factor != 1.0f)
         return false;
      e->bias.reciprocal = true;
      return true;
   }
   case nir_op_fneg: {
      if (!coord_parse(nir_scalar_chase_alu_src(s, 0), e))
         return false;
      e->scale.literal = -e->scale.literal;
      e->scale.factor = -e->scale.factor;
      e->bias.literal = -e->bias.literal;
      e->bias.factor = -e->bias.factor;
      return true;
   }
   case nir_op_fadd: {
      struct coord_expr a, b;

      if (!coord_parse(nir_scalar_chase_alu_src(s, 0), &a) ||
          !coord_parse(nir_scalar_chase_alu_src(s, 1), &b))
         return false;
      if (b.varying) {
         struct coord_expr t = a;

         a = b;
         b = t;
      }
      if (b.varying)
         return false;
      *e = a;
      return term_add(&e->bias, &b.bias);
   }
   default:
      return false;
   }
}

static bool
coord_is_identity(const struct coord_expr *e)
{
   return e->scale.literal == 1.0f && e->scale.factor == 0.0f &&
          e->bias.literal == 0.0f && e->bias.factor == 0.0f &&
          e->bias.offset == M9_FS_UNIFORM_ZERO;
}

static bool
coord_term_same(const struct m9_fs_coord_term *a,
                const struct m9_fs_coord_term *b)
{
   return a->literal == b->literal && a->factor == b->factor &&
          a->offset == b->offset && a->reciprocal == b->reciprocal;
}

/* Field by field: the struct has padding a memcmp would read. */
bool
m9_fs_coord_affine_same(const struct m9_fs_coord_affine *a,
                        const struct m9_fs_coord_affine *b)
{
   if (a->present != b->present || a->swizzled != b->swizzled)
      return false;
   for (unsigned i = 0; i < ARRAY_SIZE(a->scale); i++) {
      if ((a->swizzled && a->comp[i] != b->comp[i]) ||
          a->scale[i].literal != b->scale[i].literal ||
          a->scale[i].factor != b->scale[i].factor ||
          a->scale[i].offset != b->scale[i].offset ||
          a->scale[i].reciprocal != b->scale[i].reciprocal ||
          a->bias[i].literal != b->bias[i].literal ||
          a->bias[i].factor != b->bias[i].factor ||
          a->bias[i].offset != b->bias[i].offset ||
          a->bias[i].reciprocal != b->bias[i].reciprocal)
         return false;
   }
   return true;
}

/* A value an earlier raster pass handed over through the record.  It
 * arrives in the register pair a texel does, so it takes a texel slot and
 * the rest of the program cannot tell the two apart; what differs is that
 * the emitter binds a route at 0x74 + k instead of a texture at 0x70 + k.
 * docs/parhelia-m9-fragment-pass-routes.md */
static int
build_route(struct ctx *c, unsigned pass)
{
   struct m9_fs_program *p = c->p;
   int n;

   for (unsigned i = 0; i < p->texture_count; i++)
      if (p->texture_source[i] == pass) {
         n = node_new(c, N_TEX);
         if (n >= 0)
            NODES(c)[n].texel = 4 * i;
         return n;
      }
   if (p->texture_count >= M9_FS_MAX_TEXTURES)
      return fail(c, fail_too_many_textures), -1;
   p->texture_source[p->texture_count] = pass;
   n = node_new(c, N_TEX);
   if (n < 0)
      return -1;
   NODES(c)[n].texel = 4 * p->texture_count++;
   return n;
}

/* A rectangle target samples exactly as a plane.  The engine has no
 * unnormalized addressing mode: the vendor's 13x7 rectangle emits the same
 * descriptor words a 13x7 2-D texture does and folds 1/width and 1/height
 * into a shader constant instead (CAPTURED, captures/m9x-linux-texture-
 * resources-20260914/phase2/case-02).  This driver folds the same reciprocal
 * into the coordinate per vertex, in m9_job_textures. */
static bool
sampler_dim_plane(enum glsl_sampler_dim dim)
{
   return dim == GLSL_SAMPLER_DIM_2D || dim == GLSL_SAMPLER_DIM_RECT;
}

/* A fetch whose coordinate an earlier pass produced: the unit binds its own
 * texture and sampler and samples where the record says, which is what lets
 * a coordinate the fragment stage computes reach a fetch the engine issues
 * before the program runs (CAPTURED, the seven two-pass GLSL cases and the
 * five-sampler chain). */
static int
build_coord_route(struct ctx *c, nir_tex_instr *tex, unsigned pass)
{
   struct m9_fs_program *p = c->p;
   int n;

   /* M9X_COORD_ROUTE=0 refuses the route instead, so a scene can be
    * measured with and without it. */
   if (!debug_get_bool_option("M9X_COORD_ROUTE", true))
      return fail(c, fail_coord_not_varying), -1;
   /* Only the plain two-component form is decoded.  A third routed
    * component is a sampler modifier or a resource coordinate and which
    * byte of the route word carries it is settled for neither a projector
    * nor an array, so those keep failing rather than guessing. */
   if (tex->op != nir_texop_tex || tex->is_shadow || tex->is_array ||
       !sampler_dim_plane(tex->sampler_dim) ||
       nir_tex_instr_src_index(tex, nir_tex_src_projector) >= 0 ||
       tex->num_srcs != 1)
      return fail(c, "a routed texture coordinate must be a plain 2-D pair"),
             -1;
   for (unsigned i = 0; i < p->texture_count; i++)
      if (p->texcoord_source[i] == pass &&
          p->textures[i] == tex->texture_index) {
         n = node_new(c, N_TEX);
         if (n >= 0)
            NODES(c)[n].texel = 4 * i;
         return n;
      }
   /* The route word names the slot it feeds in its own low byte and index 3
    * is never captured, so a routed coordinate stays inside 0..2. */
   if (p->texture_count >= M9_FS_MAX_TEXTURES ||
       p->texture_count >= M9_RECORD_MAX_COORD_ROUTE)
      return fail(c, fail_too_many_textures), -1;
   p->textures[p->texture_count] = tex->texture_index;
   p->texture_source[p->texture_count] = M9_FS_ROUTE_NONE;
   p->texcoord_source[p->texture_count] = pass;
   n = node_new(c, N_TEX);
   if (n < 0)
      return -1;
   NODES(c)[n].texel = 4 * p->texture_count++;
   return n;
}

/* Names the coordinate the splitter should cut at, then fails. */
static int
fail_coord(struct ctx *c, nir_def *coord)
{
   if (!c->coord_blocker)
      c->coord_blocker = coord;
   return fail(c, fail_coord_not_varying), -1;
}

static int
build_tex(struct ctx *c, nir_tex_instr *tex)
{
   struct m9_fs_program *p = c->p;
   unsigned location, dims;
   int n, ci = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   int pi = nir_tex_instr_src_index(tex, nir_tex_src_projector);
   int ri = nir_tex_instr_src_index(tex, nir_tex_src_comparator);
   int bi = nir_tex_instr_src_index(tex, nir_tex_src_bias);

   /* A cube and a volume read three coordinate components of one varying,
    * exactly where a shadow fetch reads its reference. */
   bool volume = tex->sampler_dim == GLSL_SAMPLER_DIM_3D ||
                 tex->sampler_dim == GLSL_SAMPLER_DIM_CUBE;
   /* A line is sampled as a plane one texel high; the driver clamps t to
    * that row, so the second coordinate is a constant. */
   bool line = tex->sampler_dim == GLSL_SAMPLER_DIM_1D;

   dims = tex->is_shadow || volume ? 3 : line ? 1 : 2;
   if ((tex->op != nir_texop_tex && tex->op != nir_texop_txb) ||
       (tex->op == nir_texop_txb) != (bi >= 0) ||
       (!sampler_dim_plane(tex->sampler_dim) && !volume && !line) ||
       tex->is_array || ci < 0 || (volume && tex->is_shadow) ||
       tex->is_shadow != (ri >= 0) ||
       tex->num_srcs != 1u + (pi >= 0) + (ri >= 0) + (bi >= 0))
      return fail(c, "texture sampling needs a computed coordinate pass"), -1;
   /* An earlier pass already handed this coordinate over. */
   for (unsigned i = 0; i < c->cut_count; i++)
      if (c->cut[i] == tex->src[ci].src.ssa)
         return build_coord_route(c, tex, i);
   /* Linked vec2 varyings may share one slot: xy or zw of the input.  st
    * scalarises IO, so the pair can also be two loads of one location.
    * A shadow fetch adds the comparison reference as a third component of
    * the same stream. */
   unsigned coord_comp[3];
   struct coord_expr ce[3], qe, be;
   struct m9_fs_coord_affine affine = { 0 };
   int first = -1;
   bool natural;

   if (pi >= 0 && !coord_parse(nir_get_scalar(tex->src[pi].src.ssa, 0), &qe))
      return fail(c, "texture sampling needs a computed coordinate pass"), -1;
   /* Only a bias that is constant over the draw folds into the sampler. */
   if (bi >= 0 && (!coord_parse(nir_get_scalar(tex->src[bi].src.ssa, 0), &be) ||
                   be.varying))
      return fail(c, "texture sampling needs a computed coordinate pass"), -1;
   for (unsigned i = 0; i < dims; i++) {
      nir_scalar src = i < 2 || volume ?
                       nir_get_scalar(tex->src[ci].src.ssa, i) :
                       nir_get_scalar(tex->src[ri].src.ssa, 0);

      /* A line's second coordinate is the constant row set below. */
      if (line && i == 1) {
         memset(&ce[i], 0, sizeof(ce[i]));
         ce[i].scale.offset = ce[i].bias.offset = M9_FS_UNIFORM_ZERO;
         continue;
      }
      if (!coord_parse(src, &ce[i]))
         return fail_coord(c, tex->src[ci].src.ssa);
      if (first < 0 && ce[i].varying)
         first = i;
   }
   /* A component that reads no varying is a per-draw constant: it rides
    * in the transform's bias, at the component its position names. */
   if (first >= 0) {
      location = ce[first].location;
      if (ce[first].component < (unsigned)first)
         return fail_coord(c, tex->src[ci].src.ssa);
   } else {
      location = pi >= 0 && qe.varying ? qe.location : M9_FS_COORD_CONSTANT;
   }
   for (unsigned i = 0; i < dims; i++) {
      if (ce[i].varying && ce[i].location != location)
         return fail_coord(c, tex->src[ci].src.ssa);
      coord_comp[i] = ce[i].varying ? ce[i].component :
                      first >= 0 ? ce[first].component - first + i : i;
      affine.scale[i] = ce[i].scale;
      affine.bias[i] = ce[i].bias;
      affine.present |= !coord_is_identity(&ce[i]);
      if (debug_get_bool_option("M9X_DEBUG_FS", false))
         fprintf(stderr, "m9x fs: coord %u location %u component %u "
                 "scale %g+%g@%#x bias %g+%g@%#x\n", i, ce[i].location,
                 coord_comp[i], ce[i].scale.literal, ce[i].scale.factor,
                 ce[i].scale.offset, ce[i].bias.literal, ce[i].bias.factor,
                 ce[i].bias.offset);
   }
   if (line) {
      coord_comp[1] = coord_comp[0] + 1;
      affine.scale[1].literal = 0.0f;
      affine.scale[1].offset = M9_FS_UNIFORM_ZERO;
      affine.bias[1].literal = 0.5f;
      affine.bias[1].offset = M9_FS_UNIFORM_ZERO;
      affine.present = true;
   }
   if (pi >= 0 && qe.varying && qe.location != location)
      return fail(c, "texture sampling needs a computed coordinate pass"), -1;
   natural = coord_comp[0] <= 2 && coord_comp[1] == coord_comp[0] + 1 &&
             (!(tex->is_shadow || volume) ||
              (!coord_comp[0] && coord_comp[2] == 2)) &&
             (pi < 0 || !qe.varying || qe.component == 3);
   /* Any other order of one varying's components is gathered into the
    * stream per vertex.  A plain pair keeps the coordinate pass it had,
    * and the colour stream has no CPU view to gather in. */
   if (!natural && (tex->is_shadow || volume || line || pi >= 0) &&
       location != VARYING_SLOT_COL0) {
      affine.swizzled = true;
      affine.present = true;
      for (unsigned i = 0; i < dims; i++)
         affine.comp[i] = coord_comp[i];
      affine.comp[3] = pi >= 0 && qe.varying ? qe.component : 3;
      coord_comp[0] = 0;
   } else {
      if (coord_comp[0] > 2 || coord_comp[1] != coord_comp[0] + 1)
         return fail_coord(c, tex->src[ci].src.ssa);
      /* The driver moves a pair down to the xy of its stream; a reference
       * is a third component, so a shadow fetch owns the low half of its
       * slot. */
      if ((tex->is_shadow || volume) && (coord_comp[0] || coord_comp[2] != 2))
         return fail(c, "shadow reference is not the third coordinate"), -1;
      if (pi >= 0 && qe.varying && qe.component != 3)
         return fail(c, "texture sampling needs a computed coordinate pass"),
                -1;
   }
   /* The projector is w of the same varying or a per-draw constant, taken
    * through the transform's fourth term; the driver divides the
    * coordinate stream per vertex. */
   if (pi >= 0) {
      affine.scale[3] = qe.scale;
      affine.bias[3] = qe.bias;
      affine.present |= !qe.varying || !coord_is_identity(&qe);
   }
   /* Fetches that agree in sampler and coordinate read one texel pair: a
    * unit carries a fetch, and two of them would sample the same place. */
   for (unsigned i = 0; i < p->texture_count; i++)
      if (p->texture_source[i] == M9_FS_ROUTE_NONE &&
          p->textures[i] == tex->texture_index && p->texcoords[i] == location &&
          p->texcoord_components[i] == coord_comp[0] &&
          p->texcoord_projective[i] == (pi >= 0) &&
          p->texcoord_shadow[i] == tex->is_shadow &&
          p->texcoord_volume[i] == volume &&
          m9_fs_coord_affine_same(&p->texcoord_affine[i], &affine) &&
          p->texcoord_biased[i] == (bi >= 0) &&
          (bi < 0 || coord_term_same(&p->texcoord_lod_bias[i], &be.bias))) {
         n = node_new(c, N_TEX);
         if (n < 0)
            return -1;
         NODES(c)[n].texel = 4 * i;
         return n;
      }
   if (p->texture_count >= M9_FS_MAX_TEXTURES)
      return fail(c, fail_too_many_textures), -1;
   /* A pair anywhere but the low components is moved down per vertex into
    * the stream's own CPU view, and the built-in colour stream has none. */
   if (coord_comp[0] && location == VARYING_SLOT_COL0)
      return fail_coord(c, tex->src[ci].src.ssa);
   p->texcoord_affine[p->texture_count] = affine;
   p->texcoord_biased[p->texture_count] = bi >= 0;
   if (bi >= 0)
      p->texcoord_lod_bias[p->texture_count] = be.bias;
   p->texcoord_projective[p->texture_count] = pi >= 0;
   p->texcoord_shadow[p->texture_count] = tex->is_shadow;
   p->texcoord_volume[p->texture_count] = volume;
   p->textures[p->texture_count] = tex->texture_index;
   p->texcoords[p->texture_count] = location;
   p->texcoord_components[p->texture_count] = coord_comp[0];
   p->texture_source[p->texture_count] = M9_FS_ROUTE_NONE;
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

/* The scalar select an operand read ends up with once the pairs in the way
 * have been resolved. */
static unsigned
composed_swz(struct ctx *c, int n, unsigned lane, unsigned swz)
{
   const struct node *s = &NODES(c)[n];

   while (s->kind == N_PAIR) {
      unsigned next = s->lane[lane][lane];

      swz = swz_compose(swz, s->swz[lane][lane]);
      s = &NODES(c)[s->src[lane]];
      lane = next;
   }
   return swz;
}

/* The add class carries a scalar select for its a and its c operand, but
 * only these combinations of the two are captured: ac800c00, ac800c30 and
 * ac800d10 for the add, ac800cc0, ac800dd0 and ac800fc0 for the subtract. */
static bool
add_form(unsigned a, unsigned cc, bool sub)
{
   if (a == 1 && cc == 1)
      return true;
   if (!a && !cc)
      return true;
   return sub ? (a == 3 && !cc) : (!a && cc == 3);
}

/* Whether an add of these two operands reads both as they stand.  A
 * constant b rides in the c field, which carries no select of its own. */
static bool
add_reads(struct ctx *c, int a, const uint8_t la[2], const uint8_t sa[2],
          int b, const uint8_t lb[2], const uint8_t sb[2], bool sub)
{
   bool konst = node_is_const(&NODES(c)[b]);

   for (unsigned l = 0; l < 2; l++) {
      unsigned x = node_is_const(&NODES(c)[a]) ? 1 :
                   composed_swz(c, a, la[l], sa[l]);
      unsigned y = konst ? x : composed_swz(c, b, lb[l], sb[l]);

      /* Only the identity select is captured with a constant operand. */
      if (konst && x != 1)
         return false;
      if (!konst && !add_form(x, y, sub))
         return false;
   }
   return true;
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

/* Copy a value so that it reads back unswizzled, which is what an
 * operation that hands its operand straight on needs. */
static int
force_identity(struct ctx *c, int n, uint8_t lane[2], uint8_t swz[2])
{
   static const uint8_t lane_id[2] = { 0, 1 }, swz_id[2] = { 1, 1 };
   int one, m;

   if (n < 0 || (cond_is_plain(lane, swz) && !pair_swizzled(c, n)))
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
   NODES(c)[m].is_cond = NODES(c)[n].is_cond;
   memcpy(lane, lane_id, 2);
   memcpy(swz, swz_id, 2);
   return m;
}

/* The add and mix forms carry no operand swizzle field, so a swizzled
 * register operand is copied through a multiply by one, which does. */
static int
deswizzle(struct ctx *c, int n, uint8_t lane[2], uint8_t swz[2])
{
   static const uint8_t lane_id[2] = { 0, 1 }, swz_id[2] = { 1, 1 };
   int one, m;

   if (n < 0 || (!memcmp(lane, lane_id, 2) &&
                 !memcmp(swz, swz_id, 2) && !pair_swizzled(c, n)))
      return n;
   if (node_is_const(&NODES(c)[n]))
      return n;
   /* A replicating read of a value that is the same in both scalars needs
    * no field at all. */
   if (!memcmp(lane, lane_id, 2) && swz[0] == swz[1] &&
       (swz[0] == 0 || swz[0] == 3) &&
       node_broadcast(c, n)) {
      memcpy(swz, swz_id, 2);
      return n;
   }
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

/* A facing condition is only zero or not; where a value of exactly one or
 * zero is needed, a select between the two literals produces it. */
static int
face_value(struct ctx *c, int n)
{
   static const uint8_t lane_s0[2] = { 0, 0 }, swz_s0[2] = { 0, 0 };
   uint8_t lane[2][2] = { { 0, 1 }, { 0, 1 } }, swz[2][2] = { { 1, 1 }, { 1, 1 } };
   int lit[2], sel;

   if (n < 0 || NODES(c)[n].kind != N_FACE)
      return n;
   for (unsigned i = 0; i < 2; i++) {
      lit[i] = node_new(c, N_LIT);
      if (lit[i] < 0)
         return -1;
      for (unsigned k = 0; k < 4; k++)
         NODES(c)[lit[i]].lit[k] = i ^ NODES(c)[n].neg ? 1.0f : 0.0f;
      lit[i] = materialise(c, lit[i], lane[i], swz[i]);
      if (lit[i] < 0)
         return -1;
   }
   sel = node_new(c, N_SEL);
   if (sel < 0)
      return -1;
   set_src(&NODES(c)[sel], 0, n, lane_s0, swz_s0);
   set_src(&NODES(c)[sel], 1, lit[0], lane[0], swz[0]);
   set_src(&NODES(c)[sel], 2, lit[1], lane[1], swz[1]);
   NODES(c)[sel].cmp = 4;
   NODES(c)[sel].is_cond = true;
   return sel;
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
      if (alu->op == nir_op_fadd &&
          !add_reads(c, a, lane[0], swz[0], b, lane[1], swz[1], false)) {
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

      a = deswizzle(c, a, lane[0], swz[0]);
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

      if (kind == N_NEG)
         a = deswizzle(c, a, lane[0], swz[0]);
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
         int zero, diff, lim;

         if (!add_reads(c, b, lane[1], swz[1], a, lane[0], swz[0], true)) {
            a = deswizzle(c, a, lane[0], swz[0]);
            b = a < 0 ? -1 : deswizzle(c, b, lane[1], swz[1]);
         }
         zero = b < 0 ? -1 : node_new(c, N_LIT);
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
         if (!add_reads(c, lim, lane_id, swz_id, a, lane[0], swz[0], false))
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
      a = deswizzle(c, a, lane[0], swz[0]);
      if (a < 0)
         return -1;
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
      a = deswizzle(c, a, lane[0], swz[0]);
      zero = a < 0 ? -1 : node_new(c, N_LIT);
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
         int diff, zero;

         if (!add_reads(c, a, lane[0], swz[0], b, lane[1], swz[1], true)) {
            a = deswizzle(c, a, lane[0], swz[0]);
            b = a < 0 ? -1 : deswizzle(c, b, lane[1], swz[1]);
         }
         diff = b < 0 ? -1 : node_new(c, N_SUB);
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
      /* The opposite facing selects the other way round. */
      if (NODES(c)[cond].kind == N_FACE) {
         n = node_new(c, N_FACE);
         if (n < 0)
            return -1;
         NODES(c)[n].neg = !NODES(c)[cond].neg;
         NODES(c)[n].is_cond = true;
         return n;
      }
      cond = force_identity(c, cond, lane[0], swz[0]);
      if (cond < 0)
         return -1;
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
      pp = face_value(c, pp);
      q = pp < 0 ? -1 : face_value(c, q);
      if (q < 0)
         return -1;
      pp = force_identity(c, pp, lane[0], swz[0]);
      q = pp < 0 ? -1 : force_identity(c, q, lane[1], swz[1]);
      if (q < 0)
         return -1;
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
      int cond = face_value(c, bool_src_node(c, alu, 0, lane[0], swz[0]));

      if (cond < 0)
         return -1;
      return force_identity(c, cond, lane[0], swz[0]);
   }
   case nir_op_b2b1: {
      /* Without native integers a 32-bit boolean is a uniform holding 1.0
       * or 0.0 (UniformBooleanTrue) or the facing input; b2b1 is its test
       * against zero, which is what a select already makes. */
      nir_def *src = alu->src[0].src.ssa;

      for (unsigned k = 0; k < alu->def.num_components; k++)
         if (alu->src[0].swizzle[k] != k)
            return fail(c, "unsupported condition"), -1;
      if (src->num_components != alu->def.num_components)
         return fail(c, "unsupported condition"), -1;
      n = build_def(c, src);
      if (n < 0)
         return -1;
      if (NODES(c)[n].kind == N_UBO) {
         static const uint8_t lane_id[2] = { 0, 1 }, swz_id[2] = { 1, 1 };

         memcpy(lane[0], lane_id, 2);
         memcpy(swz[0], swz_id, 2);
         n = materialise(c, n, lane[0], swz[0]);
         if (n < 0)
            return -1;
         NODES(c)[n].is_cond = true;
      }
      if (!NODES(c)[n].is_cond)
         return fail(c, "unsupported condition"), -1;
      return n;
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
      if (NODES(c)[cond].kind == N_FACE && NODES(c)[cond].neg) {
         int t2 = a;
         uint8_t tl[2], ts[2];

         memcpy(tl, lane[1], 2);
         memcpy(ts, swz[1], 2);
         a = b;
         memcpy(lane[1], lane[2], 2);
         memcpy(swz[1], swz[2], 2);
         b = t2;
         memcpy(lane[2], tl, 2);
         memcpy(swz[2], ts, 2);
      }
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
      if (!add_reads(c, s, lane_id, swz_id, cc, lane[2], swz[2], false))
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
      /* Constants have no register-copy swizzle to remove. Load a
       * permuted/broadcast constant through its actual component read
       * first; keep identity constants inline for the existing MAD form.
       * This also preserves scalar mix weights assembled as a vec4. */
      {
         int *operand[] = { &a, &b, &t };

         for (unsigned i = 0; i < ARRAY_SIZE(operand); i++) {
            if (node_is_const(&NODES(c)[*operand[i]]) &&
                (memcmp(lane[i], lane_id, 2) ||
                 memcmp(swz[i], swz_id, 2))) {
               *operand[i] = materialise(c, *operand[i], lane[i], swz[i]);
               if (*operand[i] < 0)
                  return -1;
            }
         }
      }
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
   for (unsigned i = 0; i < c->cut_count; i++)
      if (c->cut[i] == def) {
         n = build_route(c, i);
         if (n >= 0)
            NODES(c)[n].def = def;
         return n;
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

/* A constant pair moved into a register, every lane of it holding the same
 * pair, so either lane of the load reads back the same two scalars. */
static int
load_const(struct ctx *c, int n, unsigned lane, unsigned swz)
{
   uint8_t ll[2] = { lane, lane }, ls[2] = { swz, swz };
   int load = node_new(c, N_LOAD);

   if (load < 0)
      return -1;
   set_src(&NODES(c)[load], 0, n, ll, ls);
   return load;
}

/* A constant moved into a register, both scalars holding component idx. */
static int
load_component(struct ctx *c, int n, unsigned idx)
{
   return load_const(c, n, idx / 2, (idx & 1) ? 3 : 0);
}

/* One scalar of a lane as a masked term: the source read through its own
 * scalar select, times a factor that is one in that scalar and zero in the
 * other, so two terms summed hold both scalars. */
static int
lane_term(struct ctx *c, int node, unsigned idx, int factor)
{
   static const uint8_t lane_id[2] = { 0, 1 }, swz_id[2] = { 1, 1 };
   uint8_t lane[2], swz[2];
   int n;

   if (node_is_const(&NODES(c)[node])) {
      node = load_component(c, node, idx);
      if (node < 0)
         return -1;
      idx = 0;
   }
   lane[0] = lane[1] = idx / 2;
   swz[0] = swz[1] = (idx & 1) ? 3 : 0;
   n = node_new(c, N_MUL);
   if (n < 0)
      return -1;
   set_src(&NODES(c)[n], 0, node, lane, swz);
   set_src(&NODES(c)[n], 1, factor, lane_id, swz_id);
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
            /* a computed colour, e.g. a decoded texel: lane by lane */
            goto lanes;
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
lanes:
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
   /* The same vector is assembled from the same scalars over and over in
    * code the front end scalarised, and nothing else keys on it, so the
    * assembled node is remembered here. */
   if (!c->no_gather_reuse)
      util_dynarray_foreach(&c->gathers, struct gather_key, g) {
         bool same = true;

         /* Field by field: nir_scalar is padded, and the padding of a
          * local one holds whatever the stack held. */
         for (unsigned k = 0; k < 4; k++)
            same &= g->s[k].def == s[k].def && g->s[k].comp == s[k].comp;
         if (same)
            return g->node;
      }
   /* Each result lane names its own source.  A lane whose two scalars
    * already sit next to each other in one register costs nothing; one that
    * mixes two values, or a value and a literal, is summed from a masked
    * term each, which are the captured multiply and add.  The lanes of a
    * node only one lane reads are dropped as dead. */
   {
      static const uint8_t lane_id[2] = { 0, 1 }, swz_id[2] = { 1, 1 };
      int comp_node[4], ln[2], unit[2] = { -1, -1 }, n_add = -1;
      unsigned comp_idx[4];
      uint8_t llane[2], lswz[2];
      bool is_lit[4];
      float addend[4];

      for (unsigned k = 0; k < 4; k++) {
         is_lit[k] = nir_scalar_is_const(s[k]);
         addend[k] = is_lit[k] ? nir_scalar_as_float(s[k]) : 0.0f;
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
      for (unsigned l = 0; l < 2; l++) {
         unsigned a = 2 * l, b = a + 1;
         int t[2];

         llane[l] = l;
         lswz[l] = 1;
         if (!is_lit[a] && !is_lit[b] && comp_node[a] == comp_node[b] &&
             comp_idx[a] / 2 == comp_idx[b] / 2) {
            ln[l] = comp_node[a];
            llane[l] = comp_idx[a] / 2;
            lswz[l] = (comp_idx[a] & 1) << 1 | (comp_idx[b] & 1);
            continue;
         }
         if (n_add < 0) {
            n_add = node_new(c, N_LIT);
            if (n_add < 0)
               return -1;
            memcpy(NODES(c)[n_add].lit, addend, sizeof(addend));
         }
         if (is_lit[a] && is_lit[b]) {
            /* The whole lane is a constant pair, so one move holds it. */
            ln[l] = load_const(c, n_add, l, 1);
            if (ln[l] < 0)
               return -1;
            continue;
         }
         /* One term per scalar, the literal one folded into the sum. */
         for (unsigned h = 0; h < 2; h++) {
            unsigned k = a + h;

            if (is_lit[k]) {
               t[h] = n_add;
               continue;
            }
            if (unit[h] < 0) {
               unit[h] = node_new(c, N_LIT);
               if (unit[h] < 0)
                  return -1;
               for (unsigned j = 0; j < 4; j++)
                  NODES(c)[unit[h]].lit[j] = (j & 1) == h ? 1.0f : 0.0f;
            }
            t[h] = lane_term(c, comp_node[k], comp_idx[k], unit[h]);
            if (t[h] < 0)
               return -1;
         }
         ln[l] = node_new(c, N_ADD);
         if (ln[l] < 0)
            return -1;
         /* The add takes its constant on the b side only. */
         set_src(&NODES(c)[ln[l]], 0, t[is_lit[a]], lane_id, swz_id);
         set_src(&NODES(c)[ln[l]], 1, t[!is_lit[a]], lane_id, swz_id);
      }
      /* A vector that re-reads one value the way that value already reads
       * back is that value: no slot assembles it. */
      if (ln[0] == ln[1] &&
          ((!llane[0] && llane[1] == 1 &&
            ((lswz[0] == 1 && lswz[1] == 1) || node_broadcast(c, ln[0]))) ||
           node_uniform(c, ln[0])))
         n = ln[0];
      else {
         /* The two lanes are separate instructions but one operand slot, so
          * a lane naming a constant is moved into a register first. */
         for (unsigned l = 0; l < 2; l++)
            if (node_is_const(&NODES(c)[ln[l]])) {
               ln[l] = load_const(c, ln[l], llane[l], lswz[l]);
               if (ln[l] < 0)
                  return -1;
               llane[l] = l;
               lswz[l] = 1;
            }
         n = node_new(c, N_PAIR);
         if (n < 0)
            return -1;
         for (unsigned l = 0; l < 2; l++) {
            uint8_t lane[2] = { llane[l], llane[l] };
            uint8_t swz[2] = { lswz[l], lswz[l] };

            set_src(&NODES(c)[n], l, ln[l], lane, swz);
         }
      }
      {
         struct gather_key *g = util_dynarray_grow(&c->gathers,
                                                   struct gather_key, 1);

         if (g) {
            memcpy(g->s, s, sizeof(g->s));
            g->node = n;
         }
      }
      return n;
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
   /* The raster pass reserves three of its ten stream slots for position,
    * c and d, and its declaration holds sixteen words, which caps the
    * interpolated inputs at six.  Each texture needs a coordinate stream,
    * so texture_count bounds coord_count here; the exact layout, with
    * coordinates that are shared or folded, is re-checked at variant
    * creation.  The predicate is shared with the emitter so the two
    * cannot disagree. */
   if (!m9_raster_fs_layout_ok(p->varying_count + 1,
                               m9_fs_coord_stream_count(p), p->texture_count))
      return fail(c, fail_too_many_interp), -1;
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
   struct lop *added = util_dynarray_grow(&c->lops, struct lop, 1);

   *added = *l;
   /* These forms have no constant select field and read the pair as it
    * stands, so a selected scalar becomes a constant of its own. */
   if ((l->kind == L_ADD_RC || l->kind == L_SUB_CR || l->kind == L_MIN_RC ||
        l->kind == L_MAX_RC) && l->konst >= 0 && l->kswz != 1 &&
       KEYS(c)[l->konst].pair &&
       (KEYS(c)[l->konst].kind == M9_FS_CONST_UNIFORM ||
        KEYS(c)[l->konst].kind == M9_FS_CONST_LITERAL)) {
      struct kkey k = KEYS(c)[l->konst];
      unsigned s0 = (l->kswz >> 1) & 1, s1 = l->kswz & 1;

      k = k.kind == M9_FS_CONST_UNIFORM ? key_uniform(k.off[s0], k.off[s1]) :
                                          key_literal(k.v[s0], k.v[s1]);
      added->konst = key_add(c, &k);
      added->kswz = 1;
   }
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

   /* A pair emits nothing: the lane being read names the real source. */
   while (s->kind == N_PAIR) {
      unsigned next = s->lane[lane][lane];

      swz = swz_compose(swz, s->swz[lane][lane]);
      s = &NODES(c)[s->src[lane]];
      lane = next;
   }
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
 * holds x and the framebuffer row, 7b holds z.  The vendor floors 7a and
 * then maps y onto the drawable with a multiply-add against (0, height - 1),
 * which its lower-left convention needs; the screen advertises an
 * upper-left origin, so st's wpos transform owns that flip instead. */
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
   static const uint32_t one[2] = { 0x3f800000, 0x3f800000 };
   static const uint32_t zero[2] = { 0, 0 };
   static const uint32_t half[2] = { 0x3f000000, 0x3f000000 };
   int fl, xy, z;

   /* The raster register holds the row and column, and the screen
    * advertises a half-integer pixel centre, so the floored position is
    * biased by half.  st's own wpos lowering does whatever flip the
    * shader asked for, which is why none is applied here. */
   fl = frag_lop(c, n, 0xa28fccd0, 0x1c, REG_FRAG_XY, -1, -1, -1);
   xy = frag_lop(c, n, 0xb0400150, 0x1c, -1, fl,
                 chain_const(c, one), chain_const(c, half));
   z = frag_lop(c, n, 0xb0400150, 0x1c, -1, -1,
                chain_const(c, one), chain_const(c, zero));
   if (fl < 0 || xy < 0 || z < 0)
      return false;
   /* z is the b operand, as in fs-fragcoord; c carries the constant. */
   LOPS(c)[NUM(c, lops, struct lop) - 1].opreg[1] = REG_FRAG_Z;
   nd->value[0] = xy;
   nd->value[1] = z;
   return true;
}

/* gl_FrontFacing, as fs-frontfacing reads it: the multiply-add form against
 * (-0.5, 0.5) leaves scalar 0 zero for a back face and the vendor selects on
 * it (CAPTURED, readback MEASURED).  Every facing node reads the one slot. */
static bool
emit_face(struct ctx *c, int n, struct node *nd)
{
   static const uint32_t k[2] = { 0xbf000000, 0x3f000000 };
   int key, dst;

   util_dynarray_foreach(&c->nodes, struct node, it)
      if (it->kind == N_FACE && it->value[0] >= 0) {
         nd->value[0] = nd->value[1] = it->value[0];
         return true;
      }
   key = chain_const(c, k);
   dst = frag_lop(c, n, 0xb0400030, 0x1d, -1, -1, key, key);
   if (dst < 0)
      return false;
   LOPS(c)[NUM(c, lops, struct lop) - 1].opreg[1] = REG_FRAG_Z;
   nd->value[0] = nd->value[1] = dst;
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
   if (src < 0) {
      /* A split value may arrive in a texel pair instead of an allocated
       * temporary. Seeds read a value; copy its selected scalar through
       * the existing texel-times-one form before the unchanged chain. */
      if (probe.opreg[0] < 0)
         return fail(c, "special function has no register operand");
      struct lop copy = probe;
      struct kkey one = key_literal(1.0f, 1.0f);
      copy.kind = L_MUL_RC;
      copy.konst = key_add(c, &one);
      copy.dst = value_new(c, false);
      copy.kswz = 1;
      lop_add(c, &copy);
      src = copy.dst;
      swz = 0;
   } else {
      swz = probe.opswz[0] == 3 ? 3 : 0;
   }
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
   if (nd->kind == N_FACE) {
      if (!emit_face(c, n, nd))
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
   /* A derivative writes a register of its own, never the output. */
   if (nd->kind == N_DDX || nd->kind == N_DDY) {
      for (int lane = 1; lane >= 0; lane--) {
         struct lop l;

         lop_init(&l, L_DERIV, lane, n);
         l.cls = nd->kind == N_DDY;
         operand(c, &l, 0, nd, 0);
         nd->value[lane] = value_new(c, false);
         l.dst = nd->value[lane];
         lop_add(c, &l);
      }
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
   if (nd->kind == N_PAIR) {
      /* Nothing assembles a pair; only the output needs a move per lane. */
      for (unsigned lane = 0; output && lane < 2; lane++) {
         struct lop l;

         lop_init(&l, L_MOV_OUT, lane, n);
         operand(c, &l, 0, nd, lane);
         if (l.konst >= 0)
            l.kind = L_MOVC_OUT;
         lop_add(c, &l);
      }
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

/* Move one target's root value to that target's colour destination.  A leaf
 * root holds no register of its own: a fetch is read out of its texel pair,
 * a constant straight from the constant span, and a composed vector out of
 * the texel pair whose second scalar the literal already replaced. */
static bool
output_target(struct ctx *c, int root, unsigned target)
{
   const struct node *nd;

   if (root < 0)
      return true;
   nd = &NODES(c)[root];
   for (unsigned lane = 0; lane < 2; lane++) {
      struct lop l;

      lop_init(&l, L_MOV_OUT, lane, root);
      l.target = target;
      switch (nd->kind) {
      case N_TEX:
         l.opreg[0] = nd->texel + 2 * lane;
         break;
      case N_COMPOSE:
         l.opreg[0] = NODES(c)[nd->src[0]].texel + 2 * lane;
         break;
      case N_UBO:
      case N_LIT: {
         struct kkey k = node_key(nd, lane);

         l.kind = L_MOVC_OUT;
         l.konst = key_add(c, &k);
         break;
      }
      case N_PAIR:
         operand(c, &l, 0, nd, lane);
         if (l.konst >= 0)
            l.kind = L_MOVC_OUT;
         break;
      default:
         if (nd->value[lane] < 0)
            return fail(c, "internal: target root holds no value");
         l.opval[0] = nd->value[lane];
         break;
      }
      lop_add(c, &l);
   }
   return true;
}

/* Schedule one expression graph holding one root a colour target.  A single
 * root writes the output as it is computed, which is what the captured
 * programs do; several roots are computed into registers first, so a value
 * two targets share costs one evaluation, and each is then moved to its own
 * target's destination. */
static bool
schedule_forest(struct ctx *c, const int *roots, unsigned targets,
                unsigned outputs)
{
   bool single = targets == 1;
   int root = single ? roots[0] : -1;
   int max_level = 0;
   unsigned count = NUM(c, nodes, struct node);

   for (unsigned p = 0; p < targets; p++)
      if (roots[p] >= 0)
         max_level = MAX2(max_level, node_level(c, roots[p]));
   if (single && (NODES(c)[root].kind == N_TEX ||
                  node_is_const(&NODES(c)[root])))
      return fail(c, "internal: leaf root");
   /* Interpolations: highest varying slot first. */
   for (int k = c->p->varying_count - 1; k >= 0; k--) {
      for (unsigned i = 0; i < count; i++) {
         if (NODES(c)[i].kind == N_VARY && NODES(c)[i].k == (unsigned)k &&
             !emit_node_lops(c, i, (int)i == root, true))
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
      for (unsigned p = 0; p < targets; p++)
         if (roots[p] >= 0 &&
             !schedule_operands(c, roots[p], root, max_level == 1))
            return false;
   } else {
      for (int level = 1; level <= max_level; level++) {
         for (unsigned i = 0; i < count; i++) {
            struct node *nd = &NODES(c)[i];

            if (nd->kind != N_VARY && nd->kind != N_LOAD &&
                nd->kind != N_COMPOSE && nd->level - 1 == level &&
                !emit_node_lops(c, i, (int)i == root, max_level == 1))
               return false;
         }
      }
   }
   if (single) {
      struct node *r = &NODES(c)[root];

      if (r->kind != N_VARY && r->kind != N_PAIR && max_level > 1) {
         struct lop l;

         lop_init(&l, L_MOV_OUT, 1, root);
         l.opval[0] = r->value[1];
         lop_add(c, &l);
      }
      return true;
   }
   for (unsigned p = 0; p < outputs; p++)
      if (!output_target(c, roots[p], p))
         return false;
   return true;
}

static bool
schedule_tree(struct ctx *c, int root)
{
   return schedule_forest(c, &root, 1, 1);
}

/* Register allocation */

static bool
reg_busy(uint64_t busy, unsigned reg)
{
   return busy & BITFIELD64_BIT(reg);
}

/* The register pairs a producing pass may not hold a live value in.
 *
 * Index 88 carries two fields over one range of bits: bits 15:4 flag the
 * register pairs 08/09..1e/1f the engine may reuse, and a record-writing
 * pass sets two more flags one place higher for every unit it binds
 * (m9_fs_raster_88).  From two units up those flags land inside the pair
 * field and mark pairs the program is using as free, which is the fault
 * texhi:4:0 measures for an ordinary pass.  So the allocator keeps those
 * pairs empty rather than the encoder dropping a flag the captures carry.
 */
static uint64_t
record_reserved(const struct ctx *c)
{
   unsigned units = c->p->texture_count;
   uint64_t reserved = 0;

   if (!c->record_out)
      return 0;
   for (unsigned bit = 2 + units; bit <= 3 + units; bit++)
      if (bit >= 4 && bit <= 15)
         reserved |= 3ull << (0x08 + 2 * (bit - 4));
   return reserved;
}

static bool
allocate(struct ctx *c)
{
   unsigned n = NUM(c, lops, struct lop);
   unsigned base = 4 * c->p->texture_count, next_general;
   static const uint8_t interp_extra[] = { 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f };
   const uint64_t reserved = record_reserved(c);
   uint64_t busy = reserved;

   /* A retry re-emits the lops against the values already in the array, so
    * every placement here starts from none: a value an earlier attempt put
    * in a register would otherwise hand that register back to the free list
    * while this attempt still holds a live value in it.
    * M9X_RA_RESET=0 restores the placement that carries over, which is what
    * the interference check below is measured against. */
   if (debug_get_bool_option("M9X_RA_RESET", true))
      util_dynarray_foreach(&c->values, struct value, v) {
         v->reg = -1;
         v->def = -1;
         v->last = -1;
         v->general = false;
         v->rg_read = false;
      }

   /* Both lanes of a node are emitted, but a lane nothing reads is dead, and
    * so is whatever only that lane read.  A backward walk marks what the
    * output still needs; the rest never reaches the register file. */
   {
      struct lop *lops = LOPS(c);
      bool *live = calloc(n ? n : 1, sizeof(bool));
      unsigned keep = 0;

      if (!live)
         return fail(c, "out of memory");
      util_dynarray_foreach(&c->values, struct value, v)
         v->last = -1;
      for (int s = n - 1; s >= 0; s--) {
         if (lops[s].dst >= 0 && VALUES(c)[lops[s].dst].last < 0)
            continue;
         live[s] = true;
         for (unsigned i = 0; i < ARRAY_SIZE(lops[s].opval); i++)
            if (lops[s].opval[i] >= 0)
               VALUES(c)[lops[s].opval[i]].last = s;
         if (lops[s].keep_dst)
            VALUES(c)[lops[s].dst].last = s;
      }
      for (unsigned s = 0; s < n; s++)
         if (live[s])
            lops[keep++] = lops[s];
      free(live);
      c->lops.size = keep * sizeof(struct lop);
      n = keep;
      util_dynarray_foreach(&c->values, struct value, v)
         v->last = -1;
   }
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
            /* Four units put base at 10, the scratch band: an input
             * interpolated there reads a constant (MEASURED, fsel4). */
            for (unsigned r = base; reg < 0 && r < base + 2 && r < 0x10; r++)
               if (!reg_busy(busy, r))
                  reg = r;
            for (unsigned r = c->blend ? 2 : 0;
                 reg < 0 && r < ARRAY_SIZE(interp_extra); r++)
               if (!reg_busy(busy, interp_extra[r]))
                  reg = interp_extra[r];
            if (reg < 0)
               return fail(c, fail_out_of_interp_registers);
         /* The scratch pair is the colour output's own staging, which is
          * why a single-target program may not hold a value there across
          * its last slot.  A program with several colour outputs writes
          * more than one output move, so the pair is unusable throughout:
          * M9X_FS_MRT_SCRATCH=1 puts it back, which is how that was
          * measured.  docs/parhelia-m9-mrt-routing.md */
         } else if (!c->record_out && s != (int)n - 1 && !c->outputs_last &&
                    (c->target_count < 2 ||
                     debug_get_bool_option("M9X_FS_MRT_SCRATCH", false)) &&
                    !reg_busy(busy, REG_S0)) {
            reg = REG_S0;
         } else if (!c->record_out && s != (int)n - 1 && !c->outputs_last &&
                    (c->target_count < 2 ||
                     debug_get_bool_option("M9X_FS_MRT_SCRATCH", false)) &&
                    !reg_busy(busy, REG_S1)) {
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
      taken |= reserved;

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
            while (next_general < 0x20u && reg_busy(reserved, next_general))
               next_general++;
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
            else {
               /* Name what the shader wanted: the pool it draws from and the
                * most values live at once, so a refusal separates a real
                * ceiling from a waste of the file. */
               if (debug_get_bool_option("M9X_DEBUG_SPLIT", false)) {
                  unsigned peak = 0, held = 0;

                  for (unsigned t = 0; t < n; t++) {
                     unsigned live = 0;

                     util_dynarray_foreach(&c->values, struct value, v)
                        live += v->general && v->def <= (int)t &&
                                v->last >= (int)t;
                     peak = MAX2(peak, live);
                  }
                  util_dynarray_foreach(&c->values, struct value, v)
                     held += v->general && v->reg >= 0;
                  fprintf(stderr, "m9x split: out of temporaries at slot %u "
                          "of %u, pool %02x..0f and 14..1f, placed %u, "
                          "peak live %u\n", s, n, base, held, peak);
               }
               return fail(c, fail_out_of_temporaries);
            }
         }
         /* An interpolated input's register is an ordinary one written once
          * by its own slot, so it joins the pool at its last read rather
          * than staying reserved for the length of the program -- but only
          * where the fresh run would hand it out itself: 10..13 carry the
          * scratch roles, which the backward scan places from, and four
          * bound units put base there (MEASURED: the split
          * shared-coordinate-stream case then holds 0x11 as a general
          * temporary and as scratch at once). */
         util_dynarray_foreach(&c->values, struct value, v)
            if ((v->general ||
                 (v->interp && (v->reg < 0x10 || v->reg > 0x13) &&
                  v->reg <= (c->blend ? 0x17 : 0x1f))) &&
                v->reg >= 0 && num_freed < ARRAY_SIZE(freed) &&
                (v->last == (int)s || (v->def == (int)s && v->last < 0)))
               freed[num_freed++] = v->reg;
      }
   }
   util_dynarray_foreach(&c->values, struct value, v) {
      if (v->reg < 0 && v->last >= 0)
         return fail(c, "internal: unallocated value");
   }
   /* Two values may share a register only if one is dead before the other is
    * written.  This cannot fire for register pressure -- the fresh run hands
    * each register out once and the free list only after a last read, and
    * running out returns fail_out_of_temporaries above -- so it fires only
    * when the free-list bookkeeping is wrong, which is a driver defect and
    * not a shape the hardware cannot take.  It is therefore a hard refusal
    * rather than a cut: a pass that breaks it renders every pixel silently
    * wrong, and reporting it as pressure would hide the defect behind extra
    * passes.  M9X_CHECK_RA=0 turns it off to measure what it is catching and
    * M9X_DEBUG_SPLIT names the registers. */
   if (debug_get_bool_option("M9X_CHECK_RA", true)) {
      util_dynarray_foreach(&c->values, struct value, v) {
         if (v->reg < 0 || v->last < 0)
            continue;
         util_dynarray_foreach(&c->values, struct value, w) {
            if (w <= v || w->reg != v->reg || w->last < 0)
               continue;
            if (v->def < w->last && w->def < v->last) {
               if (debug_get_bool_option("M9X_DEBUG_SPLIT", false))
                  fprintf(stderr, "m9x split: register %#x held by [%d,%d] "
                          "and [%d,%d]\n", v->reg, v->def, v->last,
                          w->def, w->last);
               return fail(c, "internal: register held by two values");
            }
         }
      }
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
         "DEPTH", "KILL", "DERIV",
      };
      unsigned i = 0;

      {
         static const char *const kinds[] = {
            "VARY", "TEX", "UBO", "LIT", "MUL", "ADD", "SUB", "MAD",
            "MIN", "MAX", "NEG", "FRACT", "FLOOR", "CEIL", "EXP2", "LOG2",
            "RCP", "RSQ", "FRAGCOORD", "CMP", "SEL", "SAT",
            "LOAD", "COMPOSE", "PAIR", "FACE", "DDX", "DDY",
         };
         unsigned count[ARRAY_SIZE(kinds)] = { 0 };

         util_dynarray_foreach(&c->nodes, struct node, nd)
            if (nd->kind < ARRAY_SIZE(kinds))
               count[nd->kind]++;
         for (unsigned i = 0; i < ARRAY_SIZE(kinds); i++)
            if (count[i])
               fprintf(stderr, "m9x fs: nodes %s %u\n", kinds[i], count[i]);
      }
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
      /* A pass that hands its result to a later one writes the record
       * through 0x11 and 0x13 and never the colour output (CAPTURED, every
       * producing pass of the seven two-pass cases). */
      bool record = out && c->record_out;
      unsigned d = !out ? (unsigned)VALUES(c)[l->dst].reg :
                   record ? (l->lane ? REG_S1 : REG_S0) :
                            color_dest(l->target);
      unsigned op0 = out && !record ? (l->lane ? 0x1e : 0x1d) : 0x1c;
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
         if (out && !record)
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
         /* The operand occupies both the b and the c field, and only the
          * captured identity pair is measured, so a swizzle is refused
          * rather than written into one field of the two. */
         if (l->opswz[0] != 1)
            return fail(c, "swizzled minimum or maximum operand");
         emit(c, w0_make(NONE, a, a, d, l->kind == L_MIN_RC ? 2 : 3, op0),
              w1_make(0xa3200d50, e, NONE));
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
         if (l->opswz[0] != 1)
            return fail(c, "swizzled addition operand");
         emit(c, w0_make(a, NONE, e, d, 0, op0),
              w1_make(0xbc800100 | (pair ? 0x10 : 0) |
                      (nd->neg ? 0xc0 : 0), 0x3f, a));
         break;
      case L_ADD_RR:
         a = opreg(c, l, 0);
         if (!add_form(l->opswz[0], l->opswz[1], nd->kind == N_SUB))
            return fail(c, "swizzled addition operand");
         emit(c, w0_make(a, NONE, opreg(c, l, 1), d, 0, op0),
              w1_make(0xac800c00 | l->opswz[0] << 8 |
                      (nd->kind == N_SUB ? 0xc0 : 0) | l->opswz[1] << 4,
                      has, a));
         break;
      case L_SUB_CR:
         if (!pair)
            return fail(c, "literal minus register");
         if (l->opswz[0] != 1)
            return fail(c, "swizzled addition operand");
         emit(c, w0_make(NONE, NONE, opreg(c, l, 0), d, 0, op0),
              w1_make(0xa0800dd0, e, NONE));
         break;
      case L_MAD:
         /* Operand slots a, b and c carry a swizzle each at bits 9:8, 7:6
          * and 5:4; the constant rides in the unused a field. */
         emit(c, w0_make(NONE, opreg(c, l, 0), opreg(c, l, 1), d, 0, op0),
              w1_make(0xa0400c00 | kbits << 8 | l->opswz[0] << 6 |
                      l->opswz[1] << 4, e, NONE));
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
         /* The move class carries the c operand's scalar select at bits
          * 5:4; all four values appear in the corpus (a2803f00, f10, f20,
          * f30, every one of them a register move).  The destination is d,
          * not the colour output: a producing pass hands its root over in
          * 0x11/0x13, and writing 0x2d instead sent it through the colour
          * path, which packed it as two halves a pixel (MEASURED). */
         emit(c, w0_make(NONE, NONE, opreg(c, l, 0), d,
                         out && !record ? color_cls(l->target) : 0, op0),
              w1_make(0xa2803f00 | l->opswz[0] << 4, NONE, NONE));
         break;
      case L_MOVC_OUT:
         /* The constant's scalar select sits where L_MOVC_TMP carries it. */
         emit(c, w0_make(NONE, NONE, NONE, d,
                         out && !record ? color_cls(l->target) : 0, op0),
              w1_make(0xa2803300 | kbits << 4, e, NONE));
         break;
      case L_RAW:
         emit(c, l->w0, l->w1);
         break;
      case L_DEPTH:
         /* gl_FragDepth, the upper clamp of fs-fragdepth into 0x21. */
         a = opreg(c, l, 0);
         emit(c, w0_make(a, a, a, 0x21, 0, 0x1d),
              w1_make(0xa3200f50, e, a));
         break;
      case L_KILL:
         /* The GLSL kill slot of fs-discard-*: kills where (0 cls v) holds
          * for the scalar the a select picks (MEASURED, all six classes). */
         a = opreg(c, l, 0);
         emit(c, w0_make(a, NONE, NONE, 0x40, l->cls, 0x1c),
              w1_make(0xafdfc000 | l->opswz[0] << 8, 0, a));
         break;
      case L_DERIV:
         /* fs-dfdx, fs-dfdy and fs-fwidth: two idle slots precede every
          * derivative, W1 bit 14 picks y, and the c select repeats a's. */
         a = opreg(c, l, 0);
         emit(c, 0xffff9100u, 0xa00fc00fu);
         emit(c, 0xffff9100u, 0xa00fc00fu);
         emit(c, w0_make(a, NONE, NONE, d, 0, op0),
              w1_make(0x8c020c00u | l->cls << 14 | l->opswz[0] << 8 |
                      l->opswz[0] << 4, 0, a));
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

   /* A depth write is followed by a stop slot, as fs-fragdepth and
    * fs-fragdepth-const end.  A program ending on the write itself left
    * the engine's conditional-kill lanes wrong for the rest of the boot
    * (MEASURED). */
   if (c->p->fragdepth)
      emit(c, STOP_W0, STOP_W1);
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
         if (key_equal(&k[i], &k[j]))
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
   uint8_t source[M9_FS_MAX_TEXTURES], coord_source[M9_FS_MAX_TEXTURES];
   struct m9_fs_coord_affine affine[M9_FS_MAX_TEXTURES];
   bool projective[M9_FS_MAX_TEXTURES], shadow[M9_FS_MAX_TEXTURES];
   bool volume[M9_FS_MAX_TEXTURES];

   util_dynarray_foreach(&c->nodes, struct node, n) {
      for (unsigned i = 0; i < 3; i++) {
         const struct node *s = n->src[i] >= 0 ? &NODES(c)[n->src[i]] : NULL;

         if (!s || s->kind != N_TEX)
            continue;
         /* A composed alpha overwrites the texel register in place, so
          * another read of the same register sees it, not only another
          * read of the same node. */
         if (n->kind != N_COMPOSE) {
            util_dynarray_foreach(&c->nodes, struct node, m) {
               const struct node *t = m->kind == N_COMPOSE && m->src[0] >= 0 ?
                                      &NODES(c)[m->src[0]] : NULL;

               if (t && t->kind == N_TEX && t->texel == s->texel)
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
      /* Everything a unit carries moves with it, or a reordered unit reads
       * another unit's coordinate. */
      affine[i] = p->texcoord_affine[order[i]];
      projective[i] = p->texcoord_projective[order[i]];
      shadow[i] = p->texcoord_shadow[order[i]];
      source[i] = p->texture_source[order[i]];
      /* Everything keyed by slot moves, or a reordered unit reads another
       * unit's route: the coordinate route index has to stay the texel
       * slot, because the route word names the slot it feeds. */
      coord_source[i] = p->texcoord_source[order[i]];
      volume[i] = p->texcoord_volume[order[i]];
      pair_of[order[i]] = i;
   }
   /* A routed coordinate is emitted at 0x74 + slot and index 3 is captured
    * nowhere, so a reorder must not push one there. */
   for (unsigned i = M9_RECORD_MAX_COORD_ROUTE; i < p->texture_count; i++)
      if (coord_source[i] != M9_FS_ROUTE_NONE)
         return fail(c, fail_too_many_textures);
   memcpy(p->textures, textures, p->texture_count);
   memcpy(p->texcoords, coords, p->texture_count);
   memcpy(p->texcoord_components, comps, p->texture_count);
   memcpy(p->texcoord_affine, affine, sizeof(affine));
   memcpy(p->texcoord_projective, projective, sizeof(projective));
   memcpy(p->texcoord_shadow, shadow, sizeof(shadow));
   memcpy(p->texture_source, source, p->texture_count);
   memcpy(p->texcoord_source, coord_source, p->texture_count);
   memcpy(p->texcoord_volume, volume, p->texture_count);
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

enum bterm { B_ZERO, B_ONE, B_REG, B_INV, B_KONST };

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
      blend_emit(c, w0_make(NONE, s, NONE, color_dest(0), 4, op0),
                 w1_make(0xa0200040, k0, NONE));
   } else if (fs.term == B_ZERO && fd.term == B_ONE) {
      blend_emit(c, w0_make(NONE, d, NONE, color_dest(0), 4, op0),
                 w1_make(0xa0200040, k1, NONE));
   } else if (fs.term == B_ONE && fd.term == B_ONE) {
      blend_emit(c, w0_make(NONE, s, d, color_dest(0), 4, op0),
                 w1_make(0xa0400c50, k1, NONE));
   } else if (fs.term != B_REG && fd.term == B_INV) {
      /* S * K + D * (1 - r) */
      blend_emit(c, w0_make(s, fd.reg, d, color_dest(0), 4, op0),
                 w1_make(0xa4e00040 | fd.swz << 10,
                         fs.term == B_ONE ? k1 : k0, s));
   } else if (fs.term == B_ZERO) {
      blend_emit(c, w0_make(fd.reg, d, NONE, color_dest(0), 4, op0),
                 w1_make(0xac200040 | fd.swz << 8, NONE, fd.reg));
   } else if (fs.term == B_ONE) {
      blend_emit(c, w0_make(fd.reg, d, s, color_dest(0), 4, op0),
                 w1_make(0xac400c50 | fd.swz << 8, NONE, fd.reg));
   } else if (fd.term == B_ZERO) {
      blend_emit(c, w0_make(fs.reg, s, NONE, color_dest(0), 4, op0),
                 w1_make(0xac200040 | fs.swz << 8, NONE, fs.reg));
   } else if (fd.term == B_ONE) {
      blend_emit(c, w0_make(fs.reg, s, d, color_dest(0), 4, op0),
                 w1_make(0xac400c50 | fs.swz << 8, NONE, fs.reg));
   } else if (fd.term == B_INV) {
      /* r.swz * S + D * (1 - S.swz), the inverted operand is S itself */
      blend_emit(c, w0_make(fs.reg, s, d, color_dest(0), 4, op0),
                 w1_make(0xa4a00040 | fd.swz << 10 | fs.swz << 8, NONE,
                         fs.reg));
   } else {
      if ((t = blend_temp(c, used)) < 0)
         return false;
      blend_emit(c, w0_make(fd.reg, d, NONE, t, 0, 0x1c),
                 w1_make(0xac200040 | fd.swz << 8, NONE, fd.reg));
      blend_emit(c, w0_make(fs.reg, s, t, color_dest(0), 4, op0),
                 w1_make(0xac400c50 | fs.swz << 8, NONE, fs.reg));
   }
   return true;
}

/* The general blend, built from the multiply (ac200040, a0200040),
 * multiply-add (ac400c50, a0400c50), constant-minus-register (a0800dd0),
 * minimum/maximum (a3200d50) and saturating move forms the backend already
 * emits.  Products land in temporaries; only the last slot of a lane writes
 * the output, in the saturating class. */

struct bdest {
   unsigned reg, op0, cls;
};

struct btail {
   uint32_t *used;
   unsigned base;           /* first constant the tail may add */
   unsigned k0, k1, kz;     /* (0, 1), (1, 0.5) and the (0, 0) pad */
   int sat;                 /* SRC_ALPHA_SATURATE factor, -1 until built */
   unsigned s_ba;
};

static bool
blend_part_opaque(unsigned src, unsigned dst, unsigned func)
{
   return (func == M9_FS_FUNC_ADD || func == M9_FS_FUNC_SUBTRACT) &&
          src == M9_FS_ONE && dst == M9_FS_ZERO;
}

static bool
blend_opaque(const struct m9_fs_blend *b)
{
   return !b->logic && blend_part_opaque(b->src, b->dst, b->func) &&
          (!b->separate ||
           blend_part_opaque(b->alpha_src, b->alpha_dst, b->alpha_func));
}

bool
m9_fs_blend_same(const struct m9_fs_blend *a, const struct m9_fs_blend *b)
{
   return a->src == b->src && a->dst == b->dst && a->srgb == b->srgb &&
          a->func == b->func && a->separate == b->separate &&
          a->logic == b->logic &&
          (!a->logic || a->logic_func == b->logic_func) &&
          (!a->separate || (a->alpha_src == b->alpha_src &&
                            a->alpha_dst == b->alpha_dst &&
                            a->alpha_func == b->alpha_func));
}

/* One ADD factor pair for all four channels, minus the two inverted colour
 * factors the vendor's forms do not cover: blend_lane's domain. */
static bool
blend_classic(const struct m9_fs_blend *b)
{
   bool src_reg = b->src >= M9_FS_SRC_ALPHA;

   return !b->separate && b->func == M9_FS_FUNC_ADD &&
          b->src <= M9_FS_INV_DST_COLOR && b->dst <= M9_FS_INV_DST_COLOR &&
          b->src != M9_FS_INV_SRC_COLOR && b->src != M9_FS_INV_DST_COLOR &&
          !(b->dst == M9_FS_INV_DST_COLOR && src_reg);
}

static bool
blend_part_reads_dst(unsigned src, unsigned dst, unsigned func)
{
   return func == M9_FS_FUNC_MIN || func == M9_FS_FUNC_MAX ||
          src != M9_FS_ZERO || dst != M9_FS_ZERO;
}

/* The constant at or after base holding these values, appended if new. */
static int
blend_const(struct ctx *c, unsigned base, enum m9_fs_const_kind kind,
            float v0, float v1, uint32_t o0, uint32_t o1)
{
   struct m9_fs_const want, *k;
   unsigned n = NUM(c, p->consts, struct m9_fs_const);

   memset(&want, 0, sizeof(want));
   want.kind = kind;
   want.value[0] = v0;
   want.value[1] = v1;
   want.offset[0] = o0;
   want.offset[1] = o1;
   for (unsigned i = base; i < n; i++)
      if (!memcmp((struct m9_fs_const *)c->p->consts.data + i, &want,
                  sizeof(want)))
         return i;
   k = util_dynarray_grow(&c->p->consts, struct m9_fs_const, 1);
   if (!k)
      return fail(c, "out of memory"), -1;
   *k = want;
   return n;
}

static int
blend_literal(struct ctx *c, struct btail *t, float v)
{
   return blend_const(c, t->base, M9_FS_CONST_LITERAL, v, v, 0, 0);
}

/* o = f * x */
static void
bt_prod(struct ctx *c, struct btail *t, struct bfactor f, unsigned x,
        struct bdest o)
{
   switch (f.term) {
   case B_REG:
      emit(c, w0_make(f.reg, x, NONE, o.reg, o.cls, o.op0),
           w1_make(0xac200040 | f.swz << 8, NONE, f.reg));
      break;
   case B_KONST:
      emit(c, w0_make(NONE, x, NONE, o.reg, o.cls, o.op0),
           w1_make(0xa0200040 | f.swz << 8, f.reg, NONE));
      break;
   default:
      emit(c, w0_make(NONE, x, NONE, o.reg, o.cls, o.op0),
           w1_make(0xa0200040, f.term == B_ONE ? t->k1 : t->k0, NONE));
      break;
   }
}

/* o = f * x + y */
static void
bt_mad(struct ctx *c, struct btail *t, struct bfactor f, unsigned x,
       unsigned y, struct bdest o)
{
   switch (f.term) {
   case B_REG:
      emit(c, w0_make(f.reg, x, y, o.reg, o.cls, o.op0),
           w1_make(0xac400c50 | f.swz << 8, NONE, f.reg));
      break;
   case B_KONST:
      emit(c, w0_make(NONE, x, y, o.reg, o.cls, o.op0),
           w1_make(0xa0400c50 | f.swz << 8, f.reg, NONE));
      break;
   case B_ONE:
      emit(c, w0_make(NONE, x, y, o.reg, o.cls, o.op0),
           w1_make(0xa0400c50, t->k1, NONE));
      break;
   default:
      bt_prod(c, t, (struct bfactor){ B_ONE }, y, o);
      break;
   }
}

static struct bdest
bt_temp(int r)
{
   return (struct bdest){ (unsigned)r, 0x1c, 0 };
}

/* temp = (1, 1) - reg, the captured literal-minus-register form */
static int
bt_inv_reg(struct ctx *c, struct btail *t, unsigned reg)
{
   int one = blend_literal(c, t, 1.0f);
   int r = one < 0 ? -1 : blend_temp(c, t->used);

   if (r >= 0)
      emit(c, w0_make(NONE, NONE, reg, r, 0, 0x1c),
           w1_make(0xa0800dd0, one, NONE));
   return r;
}

/* min(As, 1 - Ad) as b + min(a - b, 0), replicated. */
static int
bt_saturate(struct ctx *c, struct btail *t)
{
   int neg = blend_literal(c, t, -1.0f);
   int a, diff, m, f;

   if (t->sat >= 0)
      return t->sat;
   if (neg < 0 || (a = blend_inv_alpha(c, t->used, REG_DST + 2, t->k1)) < 0 ||
       (diff = blend_temp(c, t->used)) < 0 ||
       (m = blend_temp(c, t->used)) < 0 || (f = blend_temp(c, t->used)) < 0)
      return -1;
   emit(c, w0_make(NONE, a, t->s_ba, diff, 0, 0x1c),
        w1_make(0xa0400c00 | 1u << 6 | SWZ_ALPHA << 4, neg, NONE));
   emit(c, w0_make(NONE, diff, diff, m, 2, 0x1c), w1_make(0xa3200d50, t->kz, NONE));
   emit(c, w0_make(NONE, m, a, f, 0, 0x1c), w1_make(0xa0400c50, t->k1, NONE));
   return t->sat = f;
}

static bool
bt_factor(struct ctx *c, struct btail *t, unsigned f, unsigned lane,
          unsigned s, struct bfactor *out)
{
   const unsigned d = REG_DST + 2 * lane;
   int r = 0;

   switch (f) {
   case M9_FS_ZERO: *out = (struct bfactor){ B_ZERO }; return true;
   case M9_FS_ONE: *out = (struct bfactor){ B_ONE }; return true;
   case M9_FS_SRC_ALPHA:
      *out = (struct bfactor){ B_REG, t->s_ba, SWZ_ALPHA };
      return true;
   case M9_FS_DST_ALPHA:
      *out = (struct bfactor){ B_REG, REG_DST + 2, SWZ_ALPHA };
      return true;
   case M9_FS_SRC_COLOR:
      *out = (struct bfactor){ B_REG, s, SWZ_LANE };
      return true;
   case M9_FS_DST_COLOR:
      *out = (struct bfactor){ B_REG, d, SWZ_LANE };
      return true;
   case M9_FS_INV_SRC_ALPHA:
   case M9_FS_INV_DST_ALPHA:
      r = blend_inv_alpha(c, t->used, f == M9_FS_INV_SRC_ALPHA ? t->s_ba :
                          REG_DST + 2, t->k1);
      *out = (struct bfactor){ B_REG, (unsigned)r, SWZ_ALPHA };
      break;
   case M9_FS_INV_SRC_COLOR:
   case M9_FS_INV_DST_COLOR:
      r = bt_inv_reg(c, t, f == M9_FS_INV_SRC_COLOR ? s : d);
      *out = (struct bfactor){ B_REG, (unsigned)r, SWZ_LANE };
      break;
   case M9_FS_CONST_COLOR:
   case M9_FS_INV_CONST_COLOR:
   case M9_FS_CONST_ALPHA:
   case M9_FS_INV_CONST_ALPHA: {
      bool alpha = f == M9_FS_CONST_ALPHA || f == M9_FS_INV_CONST_ALPHA;
      bool inv = f == M9_FS_INV_CONST_COLOR || f == M9_FS_INV_CONST_ALPHA;

      r = blend_const(c, t->base, inv ? M9_FS_CONST_BLEND_COLOR_INV :
                      M9_FS_CONST_BLEND_COLOR, 0.0f, 0.0f,
                      alpha ? 3 : 2 * lane, alpha ? 3 : 2 * lane + 1);
      *out = (struct bfactor){ B_KONST, (unsigned)r, SWZ_LANE };
      break;
   }
   case M9_FS_SRC_ALPHA_SATURATE:
      r = bt_saturate(c, t);
      *out = (struct bfactor){ B_REG, (unsigned)r, SWZ_LANE };
      break;
   default:
      return fail(c, "unknown blend factor");
   }
   return r >= 0;
}

/* One blend equation for a lane, written to o. */
static bool
bt_part(struct ctx *c, struct btail *t, unsigned lane, unsigned s,
        unsigned src, unsigned dst, unsigned func, struct bdest o)
{
   const unsigned d = REG_DST + 2 * lane;
   struct bfactor fs, fd, fp, fn;
   unsigned p, n, tn;
   int r, neg;

   if (func == M9_FS_FUNC_MIN || func == M9_FS_FUNC_MAX) {
      /* D + min(S - D, 0): only the register-constant minimum is captured. */
      int diff, m;

      if ((neg = blend_literal(c, t, -1.0f)) < 0 ||
          (diff = blend_temp(c, t->used)) < 0 ||
          (m = blend_temp(c, t->used)) < 0)
         return false;
      bt_mad(c, t, (struct bfactor){ B_KONST, (unsigned)neg, 0 }, d, s,
             bt_temp(diff));
      emit(c, w0_make(NONE, diff, diff, m, func == M9_FS_FUNC_MIN ? 2 : 3,
                      0x1c), w1_make(0xa3200d50, t->kz, NONE));
      bt_mad(c, t, (struct bfactor){ B_ONE }, m, d, o);
      return true;
   }
   if (!bt_factor(c, t, src, lane, s, &fs) ||
       !bt_factor(c, t, dst, lane, s, &fd))
      return false;
   if (func == M9_FS_FUNC_ADD) {
      if (fd.term == B_ZERO) {
         bt_prod(c, t, fs, s, o);
      } else if (fs.term == B_ZERO) {
         bt_prod(c, t, fd, d, o);
      } else if (fd.term == B_ONE) {
         bt_mad(c, t, fs, s, d, o);
      } else if (fs.term == B_ONE) {
         bt_mad(c, t, fd, d, s, o);
      } else {
         if ((r = blend_temp(c, t->used)) < 0)
            return false;
         bt_prod(c, t, fd, d, bt_temp(r));
         bt_mad(c, t, fs, s, r, o);
      }
      return true;
   }
   /* P - N, the negated term scaled by -1 in a multiply-add */
   if (func == M9_FS_FUNC_SUBTRACT) {
      fp = fs, p = s, fn = fd, n = d;
   } else {
      fp = fd, p = d, fn = fs, n = s;
   }
   if (fn.term == B_ZERO) {
      bt_prod(c, t, fp, p, o);
      return true;
   }
   if ((neg = blend_literal(c, t, -1.0f)) < 0)
      return false;
   tn = n;
   if (fn.term != B_ONE) {
      if ((r = blend_temp(c, t->used)) < 0)
         return false;
      bt_prod(c, t, fn, n, bt_temp(r));
      tn = r;
   }
   fn = (struct bfactor){ B_KONST, (unsigned)neg, 0 };
   if (fp.term == B_ZERO) {
      bt_prod(c, t, fn, tn, o);
   } else if (fp.term == B_ONE) {
      bt_mad(c, t, fn, tn, p, o);
   } else {
      if ((r = blend_temp(c, t->used)) < 0)
         return false;
      bt_prod(c, t, fp, p, bt_temp(r));
      bt_mad(c, t, fn, tn, r, o);
   }
   return true;
}

static unsigned
blend_alpha_factor(unsigned f)
{
   switch (f) {
   case M9_FS_SRC_COLOR: return M9_FS_SRC_ALPHA;
   case M9_FS_INV_SRC_COLOR: return M9_FS_INV_SRC_ALPHA;
   case M9_FS_DST_COLOR: return M9_FS_DST_ALPHA;
   case M9_FS_INV_DST_COLOR: return M9_FS_INV_DST_ALPHA;
   case M9_FS_CONST_COLOR: return M9_FS_CONST_ALPHA;
   case M9_FS_INV_CONST_COLOR: return M9_FS_INV_CONST_ALPHA;
   case M9_FS_SRC_ALPHA_SATURATE: return M9_FS_ONE;
   default: return f;
   }
}

/* B,A lane: the alpha scalar gets its own equation when it differs, written
 * into a temporary under the second-scalar mask and moved out saturated. */
static bool
bt_lane(struct ctx *c, struct btail *t, unsigned lane, unsigned s)
{
   const struct m9_fs_blend *b = c->blend;
   const struct bdest out = { color_dest(0), 0x1d + lane, 4 };
   unsigned as = blend_alpha_factor(b->separate ? b->alpha_src : b->src);
   unsigned ad = blend_alpha_factor(b->separate ? b->alpha_dst : b->dst);
   unsigned af = b->separate ? b->alpha_func : b->func;
   int r;

   if (lane == 0 || (!b->separate && b->src != M9_FS_SRC_ALPHA_SATURATE &&
                     b->dst != M9_FS_SRC_ALPHA_SATURATE))
      return bt_part(c, t, lane, s, b->src, b->dst, b->func, out);
   if ((r = blend_temp(c, t->used)) < 0 ||
       !bt_part(c, t, lane, s, b->src, b->dst, b->func, bt_temp(r)) ||
       !bt_part(c, t, lane, s, as, ad, af,
                (struct bdest){ (unsigned)r, 0x1e, 0 }))
      return false;
   emit(c, w0_make(NONE, NONE, r, out.reg, 4, out.op0),
        w1_make(0xa2803f10, NONE, NONE));
   return true;
}

static bool
blend_general(struct ctx *c, uint32_t used, const int s[2], unsigned base,
              unsigned extra)
{
   struct btail t = {
      .used = &used, .base = base, .kz = base + extra,
      .k0 = base + extra + 1, .k1 = base + extra + 2, .sat = -1,
      .s_ba = (unsigned)s[1],
   };

   return bt_lane(c, &t, 1, s[1]) && bt_lane(c, &t, 0, s[0]);
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

/* The last slot naming each register, so a value the program stops reading
 * before a colour write can carry that lane's saturated copy instead. */
static void
program_last_touch(struct ctx *c, int *last)
{
   const uint32_t *w = (const uint32_t *)c->p->slots.data;
   unsigned n = m9_fs_slot_count(c->p);

   for (unsigned r = 0; r < 0x20; r++)
      last[r] = -1;
   for (unsigned i = 0; i < n; i++) {
      uint32_t w0 = w[2 * i], w1 = w[2 * i + 1];
      const unsigned field[4] = { slot_a(w0, w1), w0 >> 22 & 0x7f,
                                  w0 >> 15 & 0x7f, w0 >> 8 & 0x7f };

      for (unsigned f = 0; f < 4; f++)
         if (field[f] < 0x20)
            last[field[f]] = (int)i;
   }
}

/* A temporary out of the pool, admitting registers the caller knows are dead
 * even though the program named them. */
static int
blend_temp_dead(struct ctx *c, uint32_t *used, uint32_t dead)
{
   uint32_t busy = *used;
   int r;

   *used &= ~dead;
   r = blend_temp(c, used);
   *used |= busy;
   return r;
}

/* Send both colour lanes to saturated temporaries instead of the output. */
static bool
redirect_output(struct ctx *c, uint32_t *used, int s[2])
{
   uint32_t *w = (uint32_t *)c->p->slots.data;
   unsigned n = m9_fs_slot_count(c->p);
   int last[0x20], first[2] = { -1, -1 };
   uint32_t taken = 0;

   program_last_touch(c, last);
   for (unsigned i = 0; i < n; i++) {
      unsigned op0 = w[2 * i] & 0x1f;

      if ((w[2 * i] >> 8 & 0x7f) == color_dest(0) &&
          (op0 == 0x1d || op0 == 0x1e) && first[op0 - 0x1d] < 0)
         first[op0 - 0x1d] = (int)i;
   }
   for (unsigned lane = 0; lane < 2; lane++) {
      uint32_t dead = 0;

      /* The slot that writes the lane reads its own sources first, so a
       * register dead at that slot carries the lane from there on.  Texel
       * pairs are the engine's, not the program's, and stay reserved. */
      if (first[lane] >= 0)
         for (unsigned r = 4 * c->p->texture_count; r < 0x20; r++)
            if (last[r] <= first[lane] && !(taken & BITFIELD_BIT(r)))
               dead |= BITFIELD_BIT(r);
      if ((s[lane] = blend_temp_dead(c, used, dead)) < 0)
         return false;
      taken |= BITFIELD_BIT(s[lane]);
   }
   for (unsigned i = 0; i < n; i++) {
      uint32_t *w0 = &w[2 * i];
      unsigned op0 = *w0 & 0x1f;

      if ((*w0 >> 8 & 0x7f) != color_dest(0))
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
   c->p->blend_dest =
      blend_part_reads_dst(c->blend->src, c->blend->dst, c->blend->func) ||
      (c->blend->separate &&
       blend_part_reads_dst(c->blend->alpha_src, c->blend->alpha_dst,
                            c->blend->alpha_func));
   /* The destination B,A load into 1a; the pair flagged as the program's
    * (left from interpolating into the output) corrupts it at random. */
   if (c->p->blend_dest)
      c->p->registers_used &= ~BITFIELD_BIT((REG_I - 0x08) / 2);
   if (!blend_classic(c->blend)) {
      unsigned slots = m9_fs_slot_count(c->p);
      uint16_t regs = c->p->registers_used;

      /* The constants the tail adds sit in front of the pad and the fixed
       * tail, so a first pass counts them and the second emits for real. */
      if (!blend_general(c, used, s, consts, 0))
         return false;
      c->p->slots.size = 2 * slots * sizeof(uint32_t);
      c->p->registers_used = regs;
      return blend_general(c, used, s, consts,
                           NUM(c, p->consts, struct m9_fs_const) - consts);
   }
   /* The pad pair precedes the (0,1), (1,0.5) tail. */
   return blend_lane(c, &used, 1, s[1], s[1], consts + 1, consts + 2) &&
          blend_lane(c, &used, 0, s[0], s[1], consts + 1, consts + 2);
}

/* Logic op (CAPTURED, captures/m9x-linux-gl15-ff-20260915 fragops cases
 * logic_*, each exact at 144 points): every function but COPY appends this
 * tail.  Vendor slots 0 and 6 move the colour lanes into 00 and 03; the rest
 * converts colour and destination (18/1a) to integers, applies one bitwise
 * slot per lane (35/36) and writes 2d.  The marker is finish_slots' job. */
static const uint32_t logic_tail[39][2] = {
   { 0x1e62009c, 0xae003c18 }, { 0xdfff9b1c, 0xac8201cf },
   { 0xffff9d1c, 0xa280731f }, { 0xbfff941c, 0xac8201cf },
   { 0xffff881c, 0xa280731f }, { 0xffff8a1c, 0xa280331f },
   { 0x5e62839c, 0xae003c18 }, { 0x403f843c, 0xaf024141 },
   { 0x823f891c, 0xaec07140 }, { 0xe27f8a1c, 0xa020815f },
   { 0xffc50b1c, 0xa29fccdf }, { 0xe2ff8c5c, 0xa300c14f },
   { 0xe33f8d1c, 0xa021015f }, { 0xffc68e1c, 0xa29fccdf },
   { 0x9fc70f1c, 0xac9fcdd1 }, { 0xe507951c, 0xa0414d5f },
   { 0xffca961c, 0xa29fccdf }, { 0xffff971c, 0xa280331f },
   { 0xe5bf851c, 0xa021815f }, { 0xe0ff9c3c, 0xaf024142 },
   { 0x877f9e1c, 0xaec07143 }, { 0xe7bf9f1c, 0xa020815f },
   { 0xffcfa01c, 0xa29fccdf }, { 0xe83fa15c, 0xa300c14f },
   { 0xe87f901c, 0xa021015f }, { 0xffc8121c, 0xa29fccdf },
   { 0x3fc9191c, 0xac9fcdd4 }, { 0xe6cc991c, 0xa0414d5f },
   { 0xffcc991c, 0xa29fccdf }, { 0xffff811c, 0xa282b30f },
   { 0xe67f871c, 0xa021815f }, { 0xbfc0851c, 0xac9fcd10 },
   { 0xffc0871c, 0xac9fcd10 }, { 0x1fc0861c, 0xac9fcd13 },
   { 0x5fc0881c, 0xac9fcd13 }, { 0xa1bf851c, 0xaedfcd70 },
   { 0xe23f871c, 0xaedfcd70 }, { 0xbfc0ad1d, 0xac9fcdd0 },
   { 0xffc0ad1e, 0xac9fcdd0 },
};

/* The tail's constants 0..8 in its own order; (0, 1), (1, 0.5) follow. */
static const uint32_t logic_consts[9][2] = {
   { 0x38800006, 0x38800006 }, { 0x3ffff000, 0x3ffff000 },
   { 0x47800000, 0x47800000 }, { 0x477fff00, 0x477fff00 },
   { 0x3b800000, 0x3b800000 }, { 0x40800000, 0x40800000 },
   { 0x37800006, 0x37800006 }, { 0x3ffff088, 0x3ffff088 },
   { 0x44800000, 0x00000000 },
};

/* Slots 35/36 per PIPE_LOGICOP_*: W0 class and W1[15:4]; COPY has no tail. */
static const uint16_t logic_func_word[16][2] = {
   [PIPE_LOGICOP_CLEAR] = { 0, 0xc14 },
   [PIPE_LOGICOP_NOR] = { 6, 0xc14 },
   [PIPE_LOGICOP_AND_INVERTED] = { 0, 0xc17 },
   [PIPE_LOGICOP_COPY_INVERTED] = { 6, 0xc17 },
   [PIPE_LOGICOP_AND_REVERSE] = { 0, 0xcd4 },
   [PIPE_LOGICOP_INVERT] = { 6, 0xcd4 },
   [PIPE_LOGICOP_XOR] = { 0, 0xcd7 },
   [PIPE_LOGICOP_NAND] = { 6, 0xcd7 },
   [PIPE_LOGICOP_AND] = { 0, 0xf14 },
   [PIPE_LOGICOP_EQUIV] = { 6, 0xf14 },
   [PIPE_LOGICOP_NOOP] = { 0, 0xf17 },
   [PIPE_LOGICOP_OR_INVERTED] = { 6, 0xf17 },
   [PIPE_LOGICOP_OR_REVERSE] = { 6, 0xfd4 },
   [PIPE_LOGICOP_OR] = { 0, 0xfd7 },
   [PIPE_LOGICOP_SET] = { 6, 0xfd7 },
};

#define LOGIC_REGS 0x22u      /* the tail's temporaries span 00..21 */
#define LOGIC_FLAT LOGIC_REGS /* the constant standing in for 7d/7e */
#define LOGIC_DITHER_R 0x7du  /* 1024 + an 8x8 ordered dither, R,G and B,A */

static bool
logic_tail_reg(unsigned r)
{
   return r <= LOGIC_FLAT && r != REG_DST && r != REG_DST + 2;
}

/* Replay the tail with the colour lanes in s[], every other temporary
 * renamed by live range around the texel pairs, constants relocated.  The
 * vendor's tail dithers the source by 7d/7e (MEASURED: 1024 plus 0..63 per
 * channel, an 8x8 pattern), which moves exact source bytes by one and keeps
 * XOR from erasing what it drew; a constant replaces both. */
static bool
logic_append(struct ctx *c, const int have[2])
{
   const unsigned func = c->blend->logic_func;
   uint32_t tail[ARRAY_SIZE(logic_tail) + 1][2];
   int first[LOGIC_FLAT + 1], last[LOGIC_FLAT + 1], map[LOGIC_FLAT + 1];
   int busy_until[LOGIC_REGS];
   const unsigned kflat = ARRAY_SIZE(logic_consts);
   unsigned base, n = 0;
   uint32_t used;
   int s[2] = { have[0], have[1] };
   struct m9_fs_const *k;

   if (func >= ARRAY_SIZE(logic_func_word) || func == PIPE_LOGICOP_COPY)
      return fail(c, "internal: logic op without a tail");
   if (c->custom_consts)
      return fail(c, "logic op with a constant template");
   if (!program_registers(c, &used,
                          "logic op with a program using registers 18..1b"))
      return false;
   if (s[0] < 0 && !redirect_output(c, &used, s))
      return false;
   base = NUM(c, p->consts, struct m9_fs_const);
   if (base + kflat + 3 > NONE)
      return fail(c, "logic op: out of constants");
   /* The working tail: vendor slots 0 and 6 are the colour moves the
    * program already made; the flat value loads, in slot 2's form, ahead of
    * every read. */
   tail[n][0] = (logic_tail[2][0] & ~(0x7fu << 8)) | LOGIC_FLAT << 8;
   tail[n][1] = (logic_tail[2][1] & ~(0x7fu << 14)) | kflat << 14;
   n++;
   for (unsigned i = 1; i < ARRAY_SIZE(logic_tail); i++) {
      uint32_t w0 = logic_tail[i][0], w1 = logic_tail[i][1] & ~MARKER;

      if (i == 6)
         continue;
      /* (0, 1) and (1, 0.5) follow the flat value. */
      if ((w1 >> 14 & 0x7f) != NONE && (w1 >> 14 & 0x7f) >= kflat)
         w1 += 1u << 14;
      if (slot_a(w0, w1) == LOGIC_DITHER_R ||
          slot_a(w0, w1) == LOGIC_DITHER_R + 1) {
         w0 = (w0 & 0x1fffffffu) | (LOGIC_FLAT & 7u) << 29;
         w1 = (w1 & ~0xfu) | LOGIC_FLAT >> 3;
      }
      if (i == 35 || i == 36) {
         w0 = (w0 & ~(7u << 5)) | (uint32_t)logic_func_word[func][0] << 5;
         w1 = (w1 & ~0xfff0u) | (uint32_t)logic_func_word[func][1] << 4;
      }
      tail[n][0] = w0;
      tail[n][1] = w1;
      n++;
   }
   for (unsigned r = 0; r <= LOGIC_FLAT; r++)
      first[r] = last[r] = map[r] = -1;
   for (unsigned i = 0; i < n; i++) {
      const uint32_t w0 = tail[i][0], w1 = tail[i][1];
      const unsigned src[3] = { slot_a(w0, w1), w0 >> 22 & 0x7f,
                                w0 >> 15 & 0x7f };
      const unsigned d = w0 >> 8 & 0x7f;

      for (unsigned f = 0; f < 3; f++)
         if (logic_tail_reg(src[f]))
            last[src[f]] = i;
      if (logic_tail_reg(d)) {
         if (first[d] < 0)
            first[d] = i;
         last[d] = MAX2(last[d], (int)i);
      }
   }
   for (unsigned r = 0; r < LOGIC_REGS; r++)
      busy_until[r] = -1;
   /* Texel pairs stay the engine's; the colour lanes live until read. */
   for (unsigned r = 0; r < MIN2(4 * c->p->texture_count, LOGIC_REGS); r++)
      busy_until[r] = n;
   map[0x00] = s[0];
   map[0x03] = s[1];
   busy_until[s[0]] = MAX2(busy_until[s[0]], last[0x00]);
   busy_until[s[1]] = MAX2(busy_until[s[1]], last[0x03]);
   for (unsigned i = 0; i < n; i++) {
      const unsigned d = tail[i][0] >> 8 & 0x7f;
      int pick = -1;

      if (!logic_tail_reg(d) || map[d] >= 0)
         continue;
      /* The vendor's own register when it is free, else the lowest. */
      if (d < LOGIC_REGS && busy_until[d] < first[d])
         pick = d;
      for (unsigned r = 0; pick < 0 && r < LOGIC_REGS; r++)
         if (logic_tail_reg(r) && busy_until[r] < first[d])
            pick = r;
      if (pick < 0)
         return fail(c, "logic op: out of temporaries beside the texel pairs");
      map[d] = pick;
      busy_until[pick] = last[d];
   }
   k = util_dynarray_grow(&c->p->consts, struct m9_fs_const,
                          kflat + 1);
   if (!k)
      return fail(c, "out of memory");
   memset(k, 0, (kflat + 1) * sizeof(*k));
   for (unsigned i = 0; i < ARRAY_SIZE(logic_consts); i++) {
      k[i].kind = M9_FS_CONST_LITERAL;
      memcpy(&k[i].value[0], &logic_consts[i][0], 4);
      memcpy(&k[i].value[1], &logic_consts[i][1], 4);
   }
   /* The middle of the dither range: every source byte exact for all
    * fifteen functions from 1030 to 1072 (MEASURED). */
   k[kflat].kind = M9_FS_CONST_LITERAL;
   k[kflat].value[0] = k[kflat].value[1] = 1056.0f;
   for (unsigned i = 0; i < n; i++) {
      uint32_t w0 = tail[i][0], w1 = tail[i][1];
      unsigned a = slot_a(w0, w1), b = w0 >> 22 & 0x7f, cf = w0 >> 15 & 0x7f;
      unsigned d = w0 >> 8 & 0x7f, e = w1 >> 14 & 0x7f;

      a = logic_tail_reg(a) ? (unsigned)map[a] : a;
      b = logic_tail_reg(b) ? (unsigned)map[b] : b;
      cf = logic_tail_reg(cf) ? (unsigned)map[cf] : cf;
      d = logic_tail_reg(d) ? (unsigned)map[d] : d;
      if (e != NONE)
         e += base;
      emit(c, w0_make(a, b, cf, d, w0 >> 5 & 7, w0 & 0x1f),
           w1_make(w1 & ~(0x7fu << 14 | 0xfu), e, a));
   }
   /* The vendor declares every register pair and r88 bit 19. */
   c->p->registers_used = 0xfff;
   c->p->fragcoord = true;
   c->p->blend_dest = true;
   c->p->logic_op = true;
   c->has_consts = true;
   return true;
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
   /* The kill slot compares the alpha byte against the reference in 8.8
    * fixed point, as M9_FS_CONST_ALPHA_REF scales a uniform reference; a
    * literal is scaled here instead. */
   if (c->alpha.ref_literal) {
      k[0].kind = M9_FS_CONST_LITERAL;
      k[0].value[0] = k[0].value[1] = c->alpha.ref_value * 255.0f;
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
      emit(c, w0_make(NONE, NONE, s[lane], color_dest(0), 0, 0x1d + lane),
           0xa29fff1f);
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
   util_dynarray_clear(&c->gathers);
   util_dynarray_clear(&c->p->consts);
   util_dynarray_clear(&c->p->slots);
   util_dynarray_clear(&c->lops);
}

/* Everything the node graph itself accumulates, so that it can be built
 * again under a different trade-off. */
static void
reset_graph(struct ctx *c)
{
   struct m9_fs_program *p = c->p;

   util_dynarray_clear(&c->nodes);
   util_dynarray_clear(&c->gathers);
   p->texture_count = 0;
   p->varying_count = 0;
   memset(p->texture_source, M9_FS_ROUTE_NONE, sizeof(p->texture_source));
   p->fragcoord = false;
   p->face = false;
   memset(p->texcoord_projective, 0, sizeof(p->texcoord_projective));
}

static bool compile_graph(struct ctx *c, nir_scalar s[4], bool discard);

static bool
compile_output(struct ctx *c, nir_def *value, bool discard)
{
   nir_scalar s[4];
   bool all_const = true, fragcoord = false;
   unsigned comps = value->num_components;

   for (unsigned i = 0; i < 4; i++) {
      /* A record slot always holds four scalars; a narrower value repeats
       * its own components rather than costing new ones. */
      s[i] = nir_scalar_chase_movs(nir_get_scalar(value, i % comps));
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
   for (unsigned attempt = 0; attempt < 2; attempt++) {
      reset_output(c);
      reset_graph(c);
      c->depth_first = false;
      c->no_gather_reuse = attempt > 0;
      if (compile_graph(c, s, discard))
         return true;
      /* Only register pressure is worth another attempt, and only once:
       * the next one assembles every vector afresh, which is longer but
       * keeps nothing alive between its uses. */
      if (c->reason != fail_out_of_temporaries)
         return false;
   }
   return false;
}

static bool
compile_graph(struct ctx *c, nir_scalar s[4], bool discard)
{
   int root = build_vec(c, s);

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

/* Every colour target of an MRT shader shares one expression graph, so a
 * value two of them read is computed once, and each root is moved to its own
 * target's destination.  A target the shader leaves unwritten keeps no root;
 * the driver masks that target's writes off. */
static bool
compile_targets(struct ctx *c)
{
   int roots[M9_FS_MAX_TARGETS];

   for (unsigned attempt = 0; attempt < 2; attempt++) {
      reset_output(c);
      reset_graph(c);
      c->depth_first = attempt > 0;
      c->no_gather_reuse = false;
      for (unsigned p = 0; p < c->target_count; p++) {
         nir_scalar s[4];

         roots[p] = -1;
         if (!c->target_def[p])
            continue;
         for (unsigned i = 0; i < 4; i++)
            s[i] = nir_scalar_chase_movs(nir_get_scalar(c->target_def[p], i));
         roots[p] = build_vec(c, s);
         if (roots[p] < 0)
            return false;
      }
      if (assign_texels(c) &&
          schedule_forest(c, roots, c->target_count, c->target_count) &&
          allocate(c))
         return true;
      if (c->reason != fail_out_of_temporaries)
         return false;
      c->reason = NULL;
   }
   return fail(c, fail_out_of_temporaries);
}

/* The scalar a value holds in scalar 0 of a register lane: a constant is
 * moved into a register first, and a texel or an assembled pair copied
 * through a multiply by one, so the extra slots below read one value. */
static int
scalar_node(struct ctx *c, nir_scalar sc, unsigned *lane, unsigned *swz)
{
   static const uint8_t lane_id[2] = { 0, 1 }, swz_id[2] = { 1, 1 };
   unsigned comp;
   int n;

   if (nir_scalar_is_const(sc)) {
      n = node_new(c, N_LIT);
      if (n < 0)
         return -1;
      for (unsigned i = 0; i < 4; i++)
         NODES(c)[n].lit[i] = nir_scalar_as_float(sc);
      *lane = 0;
      *swz = 0;
      return load_component(c, n, 0);
   }
   n = build_def(c, sc.def);
   if (n < 0)
      return -1;
   comp = sc.comp + def_comp_base(sc.def);
   if (node_is_const(&NODES(c)[n])) {
      *lane = 0;
      *swz = 0;
      return load_component(c, n, comp);
   }
   *lane = comp / 2;
   *swz = comp & 1 ? 3 : 0;
   if (NODES(c)[n].kind == N_TEX || NODES(c)[n].kind == N_COMPOSE ||
       NODES(c)[n].kind == N_PAIR) {
      uint8_t ll[2] = { *lane, *lane }, ls[2] = { *swz, *swz };
      int one = node_new(c, N_LIT), m;

      if (one < 0)
         return -1;
      for (unsigned i = 0; i < 4; i++)
         NODES(c)[one].lit[i] = 1.0f;
      m = node_new(c, N_MUL);
      if (m < 0)
         return -1;
      set_src(&NODES(c)[m], 0, n, ll, ls);
      set_src(&NODES(c)[m], 1, one, lane_id, swz_id);
      *lane = 0;
      *swz = 0;
      return m;
   }
   return n;
}

/* The colour together with gl_FragDepth and any number of discards, which
 * the captured single-output forms do not carry.  All share one graph; the
 * kills come first, then the colour, and the depth is written last, as
 * fs-fragdepth does, with the scratch pair kept free.  A shader that writes
 * no colour gets an undefined (zero) one.  Depth is clamped to [0, 1] and
 * written to destination 0x21 exactly as fs-fragdepth and
 * fs-fragdepth-const do (CAPTURED, readback MEASURED by the vendor).
 *
 * A discard condition holds 1.0 or 0.0, so the fs-discard-* kill slot in
 * class 5 (kill where the scalar is not zero) takes it as it is; an
 * unconditional discard kills on a literal one. */
static bool
compile_extras(struct ctx *c, nir_def *colour,
               const struct util_dynarray *kills, nir_def *depth)
{
   static const uint32_t range[2] = { 0, 0x3f800000 };            /* 0, 1 */
   unsigned nkill = kills ? util_dynarray_num_elements(kills,
                                                       nir_intrinsic_instr *) : 0;
   int *roots = malloc((2 + nkill) * sizeof(int));
   unsigned *klane = malloc((1 + nkill) * 2 * sizeof(unsigned));
   bool ok = false;

   if (!roots || !klane) {
      free(roots);
      free(klane);
      return fail(c, "out of memory");
   }
   c->outputs_last = true;
   for (unsigned attempt = 0; attempt < 2 && !ok; attempt++) {
      int dn = -1;
      unsigned count = 1, dlane = 0, dswz = 0;
      struct lop l;

      reset_output(c);
      reset_graph(c);
      c->depth_first = attempt > 0;
      c->no_gather_reuse = false;
      if (colour) {
         nir_scalar s[4];

         for (unsigned i = 0; i < 4; i++)
            s[i] = nir_scalar_chase_movs(nir_get_scalar(colour, i));
         roots[0] = build_vec(c, s);
      } else {
         /* GL leaves the colour of a shader that writes none undefined. */
         roots[0] = node_new(c, N_LIT);
      }
      if (roots[0] < 0)
         break;
      for (unsigned k = 0; k < nkill; k++) {
         nir_intrinsic_instr *intr =
            *util_dynarray_element(kills, nir_intrinsic_instr *, k);
         int kn;

         if (intr->intrinsic == nir_intrinsic_terminate) {
            kn = node_new(c, N_LIT);
            if (kn < 0)
               break;
            for (unsigned i = 0; i < 4; i++)
               NODES(c)[kn].lit[i] = 1.0f;
            klane[2 * k] = klane[2 * k + 1] = 0;
            kn = load_component(c, kn, 0);
         } else {
            kn = scalar_node(c, nir_scalar_chase_movs(
                                   nir_get_scalar(intr->src[0].ssa, 0)),
                             &klane[2 * k], &klane[2 * k + 1]);
            if (kn >= 0 && NODES(c)[kn].kind == N_FACE) {
               kn = face_value(c, kn);
               klane[2 * k] = klane[2 * k + 1] = 0;
            }
         }
         if (kn < 0)
            break;
         roots[count++] = kn;
      }
      if (count != 1 + nkill)
         break;
      if (depth) {
         dn = scalar_node(c, nir_scalar_chase_movs(nir_get_scalar(depth, 0)),
                          &dlane, &dswz);
         if (dn < 0)
            break;
         roots[count++] = dn;
      }
      if (!assign_texels(c) || !schedule_forest(c, roots, count, 0))
         break;
      for (unsigned k = 0; k < nkill; k++) {
         int kn = roots[1 + k];

         lop_init(&l, L_KILL, 0, kn);
         l.opval[0] = NODES(c)[kn].value[klane[2 * k]];
         l.opswz[0] = klane[2 * k + 1];
         l.cls = 5;
         lop_add(c, &l);
      }
      if (!output_target(c, roots[0], 0))
         break;
      if (depth) {
         /* Scalar 0 reads as the identity select, as the vendor writes it. */
         unsigned sel = dswz == 3 ? 3 : 1;
         int v = NODES(c)[dn].value[dlane], t;

         t = chain_lop_cls(c, dn, 0xa3200c00 | sel << 6 | sel << 4, 0x1d, v,
                           v, v, chain_const(c, range), false, 0, 3);
         if (t < 0)
            break;
         lop_init(&l, L_DEPTH, 0, dn);
         l.opval[0] = t;
         l.konst = chain_const(c, range);
         lop_add(c, &l);
      }
      if (allocate(c)) {
         c->p->fragdepth = depth != NULL;
         c->p->discard = nkill > 0;
         ok = true;
      } else if (c->reason != fail_out_of_temporaries) {
         break;
      }
   }
   free(roots);
   free(klane);
   return ok;
}

static bool compile_impl_kills(struct ctx *c, nir_function_impl *impl,
                               struct util_dynarray *kills);

static bool
compile_impl(struct ctx *c, nir_function_impl *impl)
{
   struct util_dynarray kills;
   bool ok;

   util_dynarray_init(&kills, NULL);
   ok = compile_impl_kills(c, impl, &kills);
   util_dynarray_fini(&kills);
   return ok;
}

/* kills collects every discard, conditional or not, in program order. */
static bool
compile_impl_kills(struct ctx *c, nir_function_impl *impl,
                   struct util_dynarray *kills)
{
   nir_intrinsic_instr *store = NULL, *kill = NULL;
   nir_def *depth = NULL;
   bool discard = false, first = true, general = false;
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
            /* The captured templates take one unconditional discard ahead
             * of all other code; anything else kills in the general form. */
            general |= !first || discard;
            discard = true;
            util_dynarray_append_typed(kills, nir_intrinsic_instr *, intr);
            break;
         case nir_intrinsic_terminate_if:
            general |= kill != NULL;
            kill = intr;
            util_dynarray_append_typed(kills, nir_intrinsic_instr *, intr);
            break;
         case nir_intrinsic_store_output: {
            unsigned location = nir_intrinsic_io_semantics(intr).location;
            unsigned target = location == FRAG_RESULT_COLOR ? 0 :
                              location >= FRAG_RESULT_DATA0 &&
                              location < FRAG_RESULT_DATA0 + M9_FS_MAX_TARGETS ?
                              location - FRAG_RESULT_DATA0 : M9_FS_MAX_TARGETS;

            if (location == FRAG_RESULT_DEPTH && !depth &&
                (nir_intrinsic_write_mask(intr) & 1) &&
                nir_src_is_const(intr->src[1]) &&
                !nir_src_as_uint(intr->src[1])) {
               depth = intr->src[0].ssa;
               break;
            }
            if (target >= M9_FS_MAX_TARGETS || c->target_def[target] ||
                nir_intrinsic_write_mask(intr) != 0xf ||
                !nir_src_is_const(intr->src[1]) ||
                nir_src_as_uint(intr->src[1]) ||
                intr->src[0].ssa->num_components != 4)
               return fail(c, "unsupported fragment output");
            c->target_def[target] = intr->src[0].ssa;
            c->target_count = MAX2(c->target_count, target + 1);
            c->p->target_mask |= 1u << target;
            if (!target)
               store = intr;
            break;
         }
         case nir_intrinsic_load_interpolated_input:
         case nir_intrinsic_load_barycentric_pixel:
         case nir_intrinsic_load_barycentric_centroid:
         case nir_intrinsic_load_ubo_vec4:
            break;
         case nir_intrinsic_load_input:
         case nir_intrinsic_ddx:
         case nir_intrinsic_ddx_fine:
         case nir_intrinsic_ddx_coarse:
         case nir_intrinsic_ddy:
         case nir_intrinsic_ddy_fine:
         case nir_intrinsic_ddy_coarse:
            break;
         default:
            /* intrinsic names are static strings */
            return fail(c, nir_intrinsic_infos[intr->intrinsic].name);
         }
         first &= intr->intrinsic == nir_intrinsic_terminate;
      }
   }
   if (!c->target_count && !c->root && !depth && !kill && !discard)
      return fail(c, "no color output");
   if (depth && (c->root || c->target_count > 1))
      return fail(c, "gl_FragDepth beside several outputs");
   general |= (kill && discard) || ((kill || discard) && (depth || !store));
   c->p->target_count = c->root ? 0 : c->target_count;
   c->p->target_mask = c->root ? 0 : c->p->target_mask;
   if (!c->root && c->target_count > 1) {
      /* Blending, the alpha test and discard are lowered into the one
       * colour output the captured programs carry; none of the three has a
       * per-target form here. */
      if (kill || discard)
         return fail(c, "a multiple-target shader cannot discard");
      if (c->blend)
         return fail(c, "blending with more than one colour target");
      if (!compile_targets(c) || !encode_lops(c))
         return false;
      finish_slots(c);
      if (!c->custom_consts)
         const_tail(c);
      return true;
   }
   if (c->root) {
      /* A producing pass computes one value for the record; the discard
       * belongs to the pass that writes the colour, so a killed fragment
       * still writes nothing the application sees. */
      if (!compile_output(c, c->root, false))
         return false;
      if (!encode_lops(c))
         return false;
      finish_slots(c);
      if (!c->custom_consts)
         const_tail(c);
      return true;
   }
   /* The alpha test, the stipple and the discard-then-varying templates
    * keep their captured forms; every other discard takes the general one. */
   if (!general && kill && match_alpha_test(c, kill, store->src[0].ssa))
      c->alpha.enabled = true;
   else if (!general && kill && !tmpl_stipple(c, kill, store->src[0].ssa)) {
      /* A template miss is no verdict on the general form. */
      c->reason = NULL;
      general = true;
   }
   if (!general && !depth && store && (!kill || c->alpha.enabled) &&
       !compile_output(c, store->src[0].ssa, discard)) {
      if (!discard)
         return false;
      general = true;
   }
   if (general || depth) {
      c->alpha.enabled = false;
      if (!compile_extras(c, store ? store->src[0].ssa : NULL,
                          general ? kills : NULL, depth))
         return false;
      discard = false;
   }
   c->has_consts |= discard;
   if (!encode_lops(c))
      return false;
   if (c->alpha.enabled) {
      if (!alpha_append(c, colour))
         return false;
      if (!c->blend)
         output_temps(c, colour);
   }
   if (c->blend && !(c->blend->logic ? logic_append(c, colour) :
                                       blend_append(c, colour)))
      return false;
   finish_slots(c);
   if (c->blend && !c->blend->logic) {
      struct m9_fs_const *pad = util_dynarray_grow(&c->p->consts,
                                                   struct m9_fs_const, 1);

      memset(pad, 0, sizeof(*pad));
   }
   if (!c->custom_consts)
      const_tail(c);
   return true;
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
   /* Vector equality reduces to per-component compares and and/or. */
   case nir_op_ball_fequal2: case nir_op_ball_fequal3: case nir_op_ball_fequal4:
   case nir_op_ball_iequal2: case nir_op_ball_iequal3: case nir_op_ball_iequal4:
   case nir_op_bany_fnequal2: case nir_op_bany_fnequal3:
   case nir_op_bany_fnequal4:
   case nir_op_bany_inequal2: case nir_op_bany_inequal3:
   case nir_op_bany_inequal4:
      return true;
   default:
      return false;
   }
}

/* Without native integers only booleans reach the integer compares, and a
 * condition here holds 1.0 or 0.0, so they compare as floats.  The integer
 * lowering turns f2i into ftrunc after st's own algebraic pass has run, so
 * truncation is lowered here as well: floor above zero, ceil below. */
static bool
lower_bool_int(nir_builder *b, nir_alu_instr *alu, void *data)
{
   nir_op op;

   (void)data;
   if (alu->op == nir_op_ftrunc) {
      nir_def *x;

      b->cursor = nir_before_instr(&alu->instr);
      x = nir_ssa_for_alu_src(b, alu, 0);
      nir_def_replace(&alu->def,
                      nir_bcsel(b, nir_flt_imm(b, x, 0.0), nir_fceil(b, x),
                                nir_ffloor(b, x)));
      return true;
   }
   switch (alu->op) {
   case nir_op_ieq:
      op = nir_op_feq;
      break;
   case nir_op_ine:
   case nir_op_ixor:
      op = nir_op_fneu;
      break;
   default:
      return false;
   }
   if (alu->def.bit_size != 1 || nir_src_bit_size(alu->src[0].src) != 1)
      return false;
   b->cursor = nir_before_instr(&alu->instr);
   nir_def_replace(&alu->def,
                   nir_build_alu2(b, op,
                                  nir_b2f32(b, nir_ssa_for_alu_src(b, alu, 0)),
                                  nir_b2f32(b, nir_ssa_for_alu_src(b, alu, 1))));
   return true;
}

/* A vector of condition links would read the compares' lanes crosswise,
 * which an operand cannot; a scalar link reads one lane. */
static bool
is_condition_link(const nir_instr *instr)
{
   if (instr->type != nir_instr_type_alu ||
       nir_instr_as_alu(instr)->def.bit_size != 1)
      return false;
   switch (nir_instr_as_alu(instr)->op) {
   case nir_op_iand:
   case nir_op_ior:
   case nir_op_ixor:
   case nir_op_inot:
      return true;
   default:
      return false;
   }
}

static bool
scalar_in_prepare(const nir_instr *instr, const void *data)
{
   return is_condition_link(instr) || is_transcendental(instr, data);
}

/* Everything but the chains and condition links vectorises again
 * afterwards, so the arithmetic around them goes back to whole vectors
 * instead of being reassembled from scalars by the gather. */
static uint8_t
vectorize_around_chains(const nir_instr *instr, const void *data)
{
   return scalar_in_prepare(instr, data) ? 1 : 4;
}

/* st hands IO and arithmetic over scalarised; the native program is a
 * two-component machine, so put the vectors back before anything reads it.
 * The passes of a split shader share one prepared NIR. */
static void
fs_prepare(nir_shader *nir)
{
   bool lowered = false, progress;

   m9_nir_lower_alu_if(nir);
   /* The program has no branches, and a refused program skips the draw:
    * flatten what the bounded if-conversion left, texture fetches and
    * discards included; a discard keeps its arm's condition. */
   do {
      progress = false;
      NIR_PASS(progress, nir, nir_opt_peephole_select,
               &(nir_opt_peephole_select_options){ .limit = ~0u });
      if (!progress)
         break;
      NIR_PASS(_, nir, nir_opt_remove_phis);
      NIR_PASS(_, nir, nir_opt_copy_prop);
      NIR_PASS(_, nir, nir_opt_cse);
      NIR_PASS(_, nir, nir_opt_dce);
      NIR_PASS(_, nir, nir_lower_undef_to_zero, NULL);
   } while (progress);

   /* st lowers uniforms to UBO loads after its last folding pass, so a
    * constant slot index arrives as an iadd of two constants; without this
    * every matrix uniform looks indirectly indexed. */
   NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_opt_vectorize_io,
            nir_var_shader_in | nir_var_shader_out, false);
   NIR_PASS(_, nir, nir_opt_vectorize, NULL, NULL);
   NIR_PASS(_, nir, nir_lower_alu_to_scalar, scalar_in_prepare, NULL);
   NIR_PASS(lowered, nir, nir_shader_alu_pass, lower_bool_int,
            nir_metadata_control_flow, NULL);
   /* A boolean literal compared that way folds to 1.0 or 0.0. */
   if (lowered)
      NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_opt_vectorize, vectorize_around_chains, NULL);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   /* Scalarising a vector special function leaves one chain per component,
    * and the components are often the same argument. */
   NIR_PASS(_, nir, nir_opt_cse);
   NIR_PASS(_, nir, nir_opt_dce);
}

/* sRGB write
 *
 * The vendor encodes a colour clear on the CPU with exactly this curve
 * (docs/parhelia-m9-srgb-format-static-closure.md, from its own constants):
 *
 *   y = x <= 0.0030400000978261232 ? 12.920000076293945 * x
 *                                  : 1.0549999475479126 * pow(x, 0.4166..)
 *                                    - 0.054999999701976776
 *
 * and inserts a 25-slot suffix into the fragment program for draw output
 * zero.  The suffix is not replayed here: it reads and writes registers
 * 0x10/0x12 rather than the colour output 0x2d this backend emits, and three
 * of its five opcodes (op2 0x11, 0x13, 0x18) are not decoded, so it can be
 * neither placed nor evaluated.  The same curve is lowered into the program
 * instead, out of the compare, select, log2, exp2 and multiply-add forms the
 * corpus and the hardware fixtures already cover.
 */

static nir_def *
srgb_encode(nir_builder *b, nir_def *x)
{
   /* The clamp keeps log2 out of the negatives and lands the value in a
    * register, which a special-function chain needs its operand to be. */
   nir_def *c = nir_fmax(b, x, nir_imm_float(b, 0.0f));
   nir_def *hi = nir_fadd_imm(b,
      nir_fmul_imm(b, nir_fexp2(b, nir_fmul_imm(b, nir_flog2(b, c),
                                                M9_SRGB_EXPONENT)),
                   M9_SRGB_SCALE), -M9_SRGB_BIAS);
   nir_def *lo = nir_fmul_imm(b, c, M9_SRGB_LINEAR);

   return nir_bcsel(b, nir_flt(b, nir_imm_float(b, M9_SRGB_THRESHOLD), c),
                    hi, lo);
}

static bool
srgb_store(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   unsigned location;
   nir_def *v;

   (void)data;
   if (intr->intrinsic != nir_intrinsic_store_output)
      return false;
   location = nir_intrinsic_io_semantics(intr).location;
   if ((location != FRAG_RESULT_COLOR && location != FRAG_RESULT_DATA0) ||
       intr->src[0].ssa->num_components != 4)
      return false;
   b->cursor = nir_before_instr(&intr->instr);
   v = intr->src[0].ssa;
   nir_src_rewrite(&intr->src[0],
                   nir_vec4(b, srgb_encode(b, nir_channel(b, v, 0)),
                            srgb_encode(b, nir_channel(b, v, 1)),
                            srgb_encode(b, nir_channel(b, v, 2)),
                            nir_channel(b, v, 3)));
   return true;
}

/* gl_FragColor is written into every bound draw buffer.  st hands the
 * backend IO intrinsics rather than variables, so nir_lower_fragcolor, which
 * rewrites store_deref, finds nothing; the broadcast copies the store. */
static bool
fragcolor_store(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   unsigned targets = *(const unsigned *)data;
   nir_io_semantics sem = nir_intrinsic_io_semantics(intr);

   if (intr->intrinsic != nir_intrinsic_store_output ||
       sem.location != FRAG_RESULT_COLOR)
      return false;
   b->cursor = nir_after_instr(&intr->instr);
   for (unsigned p = 1; p < targets; p++) {
      nir_io_semantics copy = sem;

      copy.location = FRAG_RESULT_DATA0 + p;
      nir_store_output(b, intr->src[0].ssa, intr->src[1].ssa,
                       .write_mask = nir_intrinsic_write_mask(intr),
                       .src_type = nir_intrinsic_src_type(intr),
                       .io_semantics = copy,
                       .base = nir_intrinsic_base(intr) + p);
      b->shader->info.outputs_written |=
         BITFIELD64_BIT(FRAG_RESULT_DATA0 + p);
   }
   sem.location = FRAG_RESULT_DATA0;
   nir_intrinsic_set_io_semantics(intr, sem);
   b->shader->info.outputs_written &= ~BITFIELD64_BIT(FRAG_RESULT_COLOR);
   b->shader->info.outputs_written |= BITFIELD64_BIT(FRAG_RESULT_DATA0);
   return true;
}

void
m9_fs_lower_fragcolor(nir_shader *nir, unsigned targets)
{
   if (targets > 1)
      NIR_PASS(_, nir, nir_shader_intrinsics_pass, fragcolor_store,
               nir_metadata_control_flow, &targets);
}

/* The facing register reads a counter-clockwise primitive as front facing
 * whatever the draw buffer orientation (MEASURED with glsl-fs-frontfacing,
 * window and FBO, native and draw-module vertices), so a clockwise front
 * face tests the opposite way round. */
static bool
flip_face(nir_builder *b, nir_alu_instr *alu, void *data)
{
   nir_scalar s;
   nir_def *flipped;

   (void)data;
   if (alu->op != nir_op_b2b1)
      return false;
   s = nir_scalar_chase_movs(nir_get_scalar(alu->src[0].src.ssa, 0));
   if (!nir_scalar_is_intrinsic(s) ||
       nir_def_as_intrinsic(s.def)->intrinsic != nir_intrinsic_load_input ||
       nir_intrinsic_io_semantics(nir_def_as_intrinsic(s.def)).location !=
       VARYING_SLOT_FACE)
      return false;
   b->cursor = nir_after_instr(&alu->instr);
   flipped = nir_inot(b, &alu->def);
   nir_def_rewrite_uses_after(&alu->def, flipped);
   return true;
}

void
m9_fs_flip_face(nir_shader *nir)
{
   NIR_PASS(_, nir, nir_shader_alu_pass, flip_face, nir_metadata_control_flow,
            NULL);
}

/* Register 7b's z/w is not the fragment's (MEASURED: about 1.0 in FBOs and
 * 0 in the window, and the vendor's own fs-fragcoord reads z = 0), so z and
 * w come from a stream of (z_win * w_clip, w_clip): z = s.x / s.y and
 * w = 1 / s.y. */
static bool
frag_zw(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   unsigned *base = data;
   unsigned first, n;
   nir_def *q, *w, *z, *chan[4];

   if ((intr->intrinsic != nir_intrinsic_load_input &&
        intr->intrinsic != nir_intrinsic_load_interpolated_input) ||
       nir_intrinsic_io_semantics(intr).location != VARYING_SLOT_POS)
      return false;
   first = nir_intrinsic_component(intr);
   n = intr->def.num_components;
   if (first + n <= 2)
      return false;
   b->cursor = nir_after_instr(&intr->instr);
   q = nir_load_input(b, 2, 32, nir_imm_int(b, 0), .base = *base,
                      .component = 0,
                      .io_semantics = (nir_io_semantics){
                         .location = M9_FS_FRAG_ZW_LOCATION,
                         .num_slots = 1 });
   w = nir_frcp(b, nir_channel(b, q, 1));
   z = nir_fmul(b, nir_channel(b, q, 0), w);
   for (unsigned i = 0; i < n; i++)
      chan[i] = first + i == 2 ? z : first + i == 3 ? w :
                nir_channel(b, &intr->def, i);
   nir_def_rewrite_uses_after(&intr->def, nir_vec(b, chan, n));
   return true;
}

bool
m9_fs_lower_frag_zw(nir_shader *nir)
{
   unsigned base = nir->num_inputs;
   bool progress = false;

   NIR_PASS(progress, nir, nir_shader_intrinsics_pass, frag_zw,
            nir_metadata_control_flow, &base);
   if (progress) {
      nir->num_inputs = base + 1;
      nir->info.inputs_read |= BITFIELD64_BIT(M9_FS_FRAG_ZW_LOCATION);
   }
   return progress;
}

void
m9_fs_lower_srgb(nir_shader *nir)
{
   NIR_PASS(_, nir, nir_shader_intrinsics_pass, srgb_store,
            nir_metadata_control_flow, NULL);
}

/* sRGB texel decode
 *
 * No sampler decode is known (the vendor stores D3DSAMP_SRGBTEXTURE but
 * nothing consumes it), so the texel is stored as its UNORM twin and the
 * GL curve is applied after the fetch, from the same compare, select,
 * log2, exp2 and multiply-add forms the encode uses:
 *
 *   c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4)
 */
static nir_def *
srgb_decode(nir_builder *b, nir_def *c)
{
   nir_def *t = nir_fadd_imm(b, nir_fmul_imm(b, c, 1.0 / 1.055),
                             0.055 / 1.055);
   nir_def *hi = nir_fexp2(b, nir_fmul_imm(b, nir_flog2(b, t), 2.4));
   nir_def *lo = nir_fmul_imm(b, c, 1.0 / 12.92);

   return nir_bcsel(b, nir_flt(b, nir_imm_floatN_t(b, 0.04045, 32), c), hi,
                    lo);
}

static bool
srgb_texel(nir_builder *b, nir_tex_instr *tex, void *data)
{
   uint32_t mask = *(const uint32_t *)data;
   nir_def *v, *res;

   if (tex->texture_index >= 32 || !(mask & BITFIELD_BIT(tex->texture_index)) ||
       nir_tex_instr_is_query(tex) || tex->is_shadow ||
       tex->def.num_components != 4 ||
       nir_alu_type_get_base_type(tex->dest_type) != nir_type_float)
      return false;
   b->cursor = nir_after_instr(&tex->instr);
   v = &tex->def;
   /* One vec3 decode: per-channel scalars leave three results that only a
    * composed vector, which the backend refuses, puts back together. */
   res = srgb_decode(b, nir_trim_vector(b, v, 3));
   res = nir_vec4(b, nir_channel(b, res, 0), nir_channel(b, res, 1),
                  nir_channel(b, res, 2), nir_channel(b, v, 3));
   nir_def_rewrite_uses_after(v, res);
   return true;
}

void
m9_fs_lower_srgb_texture(nir_shader *nir, uint32_t mask)
{
   if (mask)
      NIR_PASS(_, nir, nir_shader_tex_pass, srgb_texel,
               nir_metadata_control_flow, &mask);
}

/* GL blends an sRGB target in linear space, which needs the destination
 * decoded before the blend and encoded after it.  Decode is not implemented,
 * so a blended sRGB draw is refused rather than blended in encoded space. */
static bool
fs_srgb(nir_shader *nir, const struct m9_fs_blend *blend, const char **reason)
{
   if (!blend || !blend->srgb)
      return true;
   if (!blend_opaque(blend)) {
      *reason = "blending into an sRGB target (destination decode is not "
                "implemented)";
      return false;
   }
   m9_fs_lower_srgb(nir);
   return true;
}

/* M9X_FS_PROBE=file replaces a compiled single-pass program with a native
 * one for hardware probes; lines "s W0 W1", "k BITS0 BITS1" (literal pair),
 * "u OFF0 OFF1" (uniform pair), "v LOCATION MASK", "r REGS", "d 0|1". */
static void
fs_probe_override(struct m9_fs_program *p)
{
   const char *path = getenv("M9X_FS_PROBE");
   char line[256];
   bool vary = false;
   FILE *f;

   if (!path || !(f = fopen(path, "r")))
      return;
   util_dynarray_clear(&p->slots);
   util_dynarray_clear(&p->consts);
   while (fgets(line, sizeof(line), f)) {
      unsigned a, b;

      if (sscanf(line, "s %x %x", &a, &b) == 2) {
         util_dynarray_append_typed(&p->slots, uint32_t, a);
         util_dynarray_append_typed(&p->slots, uint32_t, b);
      } else if (sscanf(line, "k %x %x", &a, &b) == 2 ||
                 sscanf(line, "u %x %x", &a, &b) == 2) {
         struct m9_fs_const *k = util_dynarray_grow(&p->consts,
                                                    struct m9_fs_const, 1);

         memset(k, 0, sizeof(*k));
         if (line[0] == 'k') {
            k->kind = M9_FS_CONST_LITERAL;
            memcpy(&k->value[0], &a, 4);
            memcpy(&k->value[1], &b, 4);
         } else {
            k->kind = M9_FS_CONST_UNIFORM;
            k->offset[0] = a;
            k->offset[1] = b;
         }
      } else if (sscanf(line, "v %x %x", &a, &b) == 2) {
         if (!vary)
            p->varying_count = 0;
         vary = true;
         if (p->varying_count < M9_FS_MAX_VARYINGS) {
            p->varyings[p->varying_count].location = a;
            p->varyings[p->varying_count++].components = b;
         }
      } else if (sscanf(line, "r %x", &a) == 1) {
         p->registers_used = a;
      } else if (sscanf(line, "d %u", &a) == 1) {
         p->discard = a;
      }
   }
   fclose(f);
   fprintf(stderr, "m9x fs: probe %s: %u slots %u consts %u varyings\n",
           path, m9_fs_slot_count(p),
           (unsigned)util_dynarray_num_elements(&p->consts,
                                                struct m9_fs_const),
           p->varying_count);
}

/* One raster pass: the shader output, or the value root a producing pass
 * hands on, with the values earlier passes produced available as routes. */
static bool
fs_compile_pass(nir_shader *nir, const struct m9_fs_blend *blend,
                struct m9_fs_program *program, nir_def *root, bool record_out,
                nir_def *const *cuts, unsigned cut_count, const char **reason,
                nir_def **coord_blocker)
{
   /* ONE/ZERO is the unblended program, as the vendor emits it. */
   bool opaque = !blend || blend_opaque(blend);
   struct ctx c = { .nir = nir, .p = program, .blend = opaque ? NULL : blend,
                    .root = root, .record_out = record_out,
                    .cut_count = cut_count };
   bool ok = false;

   memset(program, 0, sizeof(*program));
   memset(program->texture_source, M9_FS_ROUTE_NONE,
          sizeof(program->texture_source));
   memset(program->texcoord_source, M9_FS_ROUTE_NONE,
          sizeof(program->texcoord_source));
   program->writes_record = record_out;
   program->record_components = root ? root->num_components : 4;
   for (unsigned i = 0; i < cut_count; i++)
      c.cut[i] = cuts[i];
   util_dynarray_init(&program->slots, NULL);
   util_dynarray_init(&program->consts, NULL);
   util_dynarray_init(&c.nodes, NULL);
   util_dynarray_init(&c.lops, NULL);
   util_dynarray_init(&c.values, NULL);
   util_dynarray_init(&c.keys, NULL);
   util_dynarray_init(&c.key_index, NULL);
   util_dynarray_init(&c.gathers, NULL);

   if (nir->info.stage != MESA_SHADER_FRAGMENT) {
      fail(&c, "not a fragment shader");
   } else {
      nir_foreach_function_impl(impl, nir) {
         if (!impl->function->is_entrypoint)
            continue;
         ok = compile_impl(&c, impl);
      }
   }
   util_dynarray_fini(&c.gathers);
   util_dynarray_fini(&c.key_index);
   util_dynarray_fini(&c.nodes);
   util_dynarray_fini(&c.lops);
   util_dynarray_fini(&c.values);
   util_dynarray_fini(&c.keys);
   if (coord_blocker)
      *coord_blocker = c.coord_blocker;
   if (!ok) {
      *reason = c.reason ? c.reason : "unsupported shader";
      m9_fs_program_fini(program);
      memset(program, 0, sizeof(*program));
   } else if (!record_out && !cut_count) {
      fs_probe_override(program);
   }
   return ok;
}

bool
m9_fs_kills_all(const struct nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir) {
      if (!impl->function->is_entrypoint)
         continue;
      /* Only blocks of the top level run for every fragment. */
      foreach_list_typed(nir_cf_node, node, node, &impl->body) {
         if (node->type != nir_cf_node_block)
            continue;
         nir_foreach_instr(instr, nir_cf_node_as_block(node)) {
            nir_intrinsic_instr *intr;

            if (instr->type != nir_instr_type_intrinsic)
               continue;
            intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_terminate ||
                (intr->intrinsic == nir_intrinsic_terminate_if &&
                 nir_src_is_const(intr->src[0]) &&
                 nir_src_as_bool(intr->src[0])))
               return true;
         }
      }
   }
   return false;
}

bool
m9_fs_compile(struct nir_shader *nir, const struct m9_fs_blend *blend,
              struct m9_fs_program *program, const char **reason)
{
   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      if (!fs_srgb(nir, blend, reason))
         return memset(program, 0, sizeof(*program)), false;
      fs_prepare(nir);
   }
   return fs_compile_pass(nir, blend, program, NULL, false, NULL, 0, reason,
                          NULL);
}

/* Splitting one shader across raster passes
 *
 * A pass holds four texture units, six interpolation registers inside ten
 * stream slots, and sixteen general register pairs.  Whichever of the three
 * a shader runs past is what the cut is chosen for: the producing pass
 * computes one value, hands it over through the inter-pass record, and the
 * consumer reads it back in a texel slot
 * (docs/parhelia-m9-fragment-pass-splitting.md).
 */

/* The resource a pass ran out of, and so what a cut has to shrink. */
enum fs_cut_kind {
   FS_CUT_TEX,      /* texture units */
   FS_CUT_REGS,     /* the register file: values live at once */
   FS_CUT_INTERP,   /* interpolation registers and stream slots */
};

/* What one value reaches: the fetches below it, the varyings it
 * interpolates, and the records of earlier passes it reads instead of
 * recomputing them.  The two bitsets are slices of one pool sized from the
 * shader, so neither the fetch count nor the varying count is capped here. */
struct fs_cone {
   BITSET_WORD *tex;
   BITSET_WORD *vary;
   uint32_t routes;     /* one bit a producing pass, bounded by M9_FS_MAX_PASSES */
   bool done;
};

struct fs_split {
   struct fs_cone *cone;      /* per SSA definition index, plus the accumulator */
   BITSET_WORD *pool;
   unsigned tex_words, vary_words;
   unsigned *fetch;           /* fetch number of a texture definition */
   unsigned *varying;         /* varying number of an interpolated input */
   nir_def *const *cut;
   unsigned cut_count;
   nir_def *extra;            /* a cut under trial, numbered cut_count */
   unsigned defs;
   unsigned visited;          /* definitions the last cone walk expanded */
};

static unsigned
fs_bits(const BITSET_WORD *set, unsigned words)
{
   unsigned n = 0;

   for (unsigned i = 0; i < words; i++)
      n += util_bitcount(set[i]);
   return n;
}

static unsigned
fs_cone_fetches(const struct fs_split *sp, const struct fs_cone *cone)
{
   return fs_bits(cone->tex, sp->tex_words);
}

static unsigned
fs_cone_varyings(const struct fs_split *sp, const struct fs_cone *cone)
{
   return fs_bits(cone->vary, sp->vary_words);
}

/* Texel slots the cone needs: one a fetch and one a record it reads. */
static unsigned
fs_cone_slots(const struct fs_split *sp, const struct fs_cone *cone)
{
   return fs_cone_fetches(sp, cone) + util_bitcount(cone->routes);
}

static const struct fs_cone *fs_cone_of(struct fs_split *sp, nir_def *def);

/* nir_foreach_src carries one pointer, and the walk needs the cone it is
 * filling beside the split state. */
struct fs_cone_walk {
   struct fs_split *sp;
   struct fs_cone *cone;
};

static bool
fs_cone_src(nir_src *src, void *data)
{
   struct fs_cone_walk *w = data;
   const struct fs_cone *sub = fs_cone_of(w->sp, src->ssa);

   for (unsigned i = 0; i < w->sp->tex_words; i++)
      w->cone->tex[i] |= sub->tex[i];
   for (unsigned i = 0; i < w->sp->vary_words; i++)
      w->cone->vary[i] |= sub->vary[i];
   w->cone->routes |= sub->routes;
   return true;
}

static const struct fs_cone *
fs_cone_of(struct fs_split *sp, nir_def *def)
{
   struct fs_cone *cone = &sp->cone[def->index];
   struct fs_cone_walk walk = { .sp = sp, .cone = cone };
   nir_instr *instr;

   if (cone->done)
      return cone;
   cone->done = true;
   sp->visited++;
   for (unsigned i = 0; i < sp->cut_count; i++)
      if (sp->cut[i] == def) {
         cone->routes = BITFIELD_BIT(i);
         return cone;
      }
   if (sp->extra == def) {
      cone->routes = BITFIELD_BIT(sp->cut_count);
      return cone;
   }
   instr = nir_def_instr(def);
   if (instr->type == nir_instr_type_tex)
      BITSET_SET(cone->tex, sp->fetch[def->index]);
   if (sp->varying[def->index] != ~0u)
      BITSET_SET(cone->vary, sp->varying[def->index]);
   nir_foreach_src(instr, fs_cone_src, &walk);
   return cone;
}

static void
fs_cone_reset(struct fs_split *sp, nir_def *extra)
{
   unsigned words = sp->tex_words + sp->vary_words;

   memset(sp->pool, 0, (sp->defs + 1) * words * sizeof(*sp->pool));
   for (unsigned i = 0; i <= sp->defs; i++) {
      sp->cone[i].routes = 0;
      sp->cone[i].done = false;
   }
   sp->extra = extra;
   sp->visited = 0;
}

/* The definitions one value's cone covers: the work a producing pass would
 * take on, and, subtracted from the whole, the work left to the consumer.
 * It is what a cut chosen for register pressure is scored on, because a
 * pass boundary is the only place a live value is spilled. */
static unsigned
fs_cone_defs(struct fs_split *sp, nir_def *def, nir_def *extra)
{
   fs_cone_reset(sp, extra);
   fs_cone_of(sp, def);
   return sp->visited;
}

/* One half of a candidate cut, measured against what a pass can hold.  A
 * record the half reads back may either take a texel slot of its own or
 * steer the coordinate of a fetch that already has one, so the slots it
 * needs lie between max(tex, routes) and tex + routes; which it is only
 * settles once the pass is compiled. */
struct fs_half {
   unsigned tex, vary, routes, defs;
};

static unsigned
fs_half_slots_max(const struct fs_half *h)
{
   return h->tex + h->routes;
}

/* A producing pass has to fit what a pass holds.  The consumer does not:
 * one still over a limit is cut again on the next turn of the loop, and
 * requiring it to fit rejects every cut of a shader that needs more than
 * two passes.  The coordinate streams and the declaration budget are left
 * to the layout gate, which sees the compiled pass rather than a cone. */
static bool
fs_half_fits(const struct fs_half *h, unsigned limit)
{
   return fs_half_slots_max(h) <= limit && h->vary <= M9_RASTER_MAX_INTERP;
}

static void
fs_half_of(struct fs_split *sp, const struct fs_cone *cone, struct fs_half *h)
{
   h->tex = fs_cone_fetches(sp, cone);
   h->vary = fs_cone_varyings(sp, cone);
   h->routes = util_bitcount(cone->routes);
   h->defs = sp->visited;
}

/* The value a producing pass should hand on.
 *
 * Cutting for texel slots takes the value absorbing the most of them that
 * still fits the four a pass binds, leaving the rest of the shader strictly
 * smaller than it was.  A slot is a fetch *or* a record an earlier pass
 * wrote, because the consumer binds both the same way: a shader whose
 * fetches are all placed can still need more slots than a pass has, purely
 * to read its own records back, and the cut that answers that is a gather
 * -- a pass that reads several records and writes one.  Scoring on fetches
 * alone made no gather admissible and left such a shader refused.  A cut
 * costs the consumer a slot of its own, so it has to absorb two to be
 * progress.
 *
 * Cutting for the register file or for the interpolation registers is the
 * other way round: the slots are not what runs out, so a cut that absorbs
 * no fetch at all is admissible -- the work it moves is what the pass could
 * not hold.  Such a cut is scored on the records it leaves the consumer
 * reading, because a record that stays live costs its four dwords of stride
 * for every fragment of every pass, and only then on the widest half. */
static nir_def *
fs_choose_cut(struct fs_split *sp, nir_function_impl *impl, nir_def *out,
              unsigned limit, nir_def *const *rejected, unsigned rejects,
              enum fs_cut_kind kind, bool forced)
{
   nir_def *best = NULL;
   struct fs_half whole;
   unsigned best_tex = 0, best_rest = ~0u, best_wide = ~0u;

   fs_cone_reset(sp, NULL);
   fs_half_of(sp, fs_cone_of(sp, out), &whole);
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         nir_def *def = nir_instr_def(instr);
         struct fs_half prod, cons;
         bool skip = false;
         unsigned wide, taken, left;

         if (!def || def == out || def->bit_size != 32 ||
             def->num_components > 4)
            continue;
         for (unsigned i = 0; i < sp->cut_count; i++)
            skip |= sp->cut[i] == def;
         for (unsigned i = 0; i < rejects; i++)
            skip |= rejected[i] == def;
         if (skip)
            continue;

         fs_cone_reset(sp, NULL);
         fs_half_of(sp, fs_cone_of(sp, def), &prod);
         if (!fs_half_fits(&prod, limit))
            continue;
         taken = fs_half_slots_max(&prod);
         fs_cone_reset(sp, def);
         fs_half_of(sp, fs_cone_of(sp, out), &cons);
         left = fs_half_slots_max(&cons);
         /* Both halves must be strictly smaller than the whole in the
          * resource that ran out, or the cut moves nothing and the next
          * pass meets the same ceiling. */
         switch (kind) {
         case FS_CUT_TEX:
            /* Forcing exercises the captured shape on a shader that needs no
             * split, where one absorbed fetch is the whole point. */
            if (forced ? (!prod.tex || cons.tex >= whole.tex)
                       : (taken < 2 || left >= fs_half_slots_max(&whole)))
               continue;
            break;
         case FS_CUT_REGS:
            if (cons.tex > whole.tex || prod.defs >= whole.defs ||
                cons.defs >= whole.defs)
               continue;
            break;
         case FS_CUT_INTERP:
            if (cons.tex > whole.tex ||
                (prod.vary >= whole.vary && cons.vary >= whole.vary))
               continue;
            break;
         }
         wide = kind == FS_CUT_INTERP ? MAX2(prod.vary, cons.vary)
                                      : MAX2(prod.defs, cons.defs);
         if (debug_get_bool_option("M9X_DEBUG_SPLIT", false))
            fprintf(stderr, "m9x split: candidate %%%u kind %u comps %u "
                    "tex %u/%u vary %u/%u slots %u/%u defs %u/%u of %u\n",
                    def->index, kind, def->num_components, prod.tex, cons.tex,
                    prod.vary, cons.vary, taken, left, prod.defs, cons.defs,
                    whole.defs);
         /* M9X_SPLIT_RANK=1 scores a register-file cut on the widest half
          * instead, which schedules the same shader with more records live
          * at once; it is how the record-slot ceiling is measured against a
          * fixture that passes either way otherwise. */
         /* Interpolation cuts must shrink the widest live varying set
          * before saving record slots. Slot-first ranking repeatedly moved
          * arithmetic while leaving seven inputs in the consumer. */
         bool better = kind == FS_CUT_TEX ?
            (taken > best_tex || (taken == best_tex && left < best_rest)) :
            kind == FS_CUT_INTERP ?
            (wide < best_wide || (wide == best_wide && left < best_rest)) :
            (debug_get_bool_option("M9X_SPLIT_RANK", false) ?
             (wide < best_wide || (wide == best_wide && taken > best_tex)) :
             (left < best_rest || (left == best_rest && wide < best_wide)));

         if (better) {
            best = def;
            best_tex = taken;
            best_rest = left;
            best_wide = wide;
         }
      }
   }
   if (debug_get_bool_option("M9X_DEBUG_SPLIT", false))
      fprintf(stderr, "m9x split: kind %u whole tex %u routes %u vary %u "
              "defs %u -> %s\n", kind, whole.tex, whole.routes, whole.vary,
              whole.defs, best ? "cut" : "no candidate");
   return best;
}

/* Which resource a refusal names, and so whether a cut can answer it. */
static bool
fs_cut_kind_of(const char *reason, const nir_def *blocker,
               enum fs_cut_kind *kind)
{
   if (reason == fail_too_many_textures)
      return *kind = FS_CUT_TEX, true;
   /* Interpolation values occupy a restricted part of the register file.
    * Their lifetime can exhaust it even with fewer than six inputs, so
    * split their work just as for general temporary pressure. */
   if (reason == fail_out_of_temporaries ||
       reason == fail_out_of_interp_registers)
      return *kind = FS_CUT_REGS, true;
   if (reason == fail_too_many_interp)
      return *kind = FS_CUT_INTERP, true;
   /* A computed coordinate is a cut, not a refusal: the pass before hands
    * it over and the consumer's unit samples at the record. */
   if (reason == fail_coord_not_varying && blocker)
      return *kind = FS_CUT_TEX, true;
   return false;
}

/* Numbers the fetches and the interpolated inputs the cone walk tracks, and
 * refuses what the pass loop cannot schedule. */
static bool
fs_split_index(struct fs_split *sp, nir_function_impl *impl, nir_def **out,
               const char **reason)
{
   unsigned fetches = 0, varyings = 0;
   uint8_t *location = NULL;

   for (unsigned i = 0; i < sp->defs; i++)
      sp->varying[i] = ~0u;
   location = calloc(sp->defs ? sp->defs : 1, sizeof(*location));
   if (!location)
      return *reason = "out of memory", false;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         nir_intrinsic_instr *intr;
         unsigned loc;

         if (instr->type == nir_instr_type_tex) {
            sp->fetch[nir_instr_as_tex(instr)->def.index] = fetches++;
            continue;
         }
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic == nir_intrinsic_store_output) {
            /* The cut machinery follows the cone of one output.  A shader
             * writing several colour targets is refused rather than split
             * along the last of them. */
            if (*out) {
               free(location);
               return *reason = "a multiple-target shader does not split "
                                "across raster passes", false;
            }
            *out = intr->src[0].ssa;
            continue;
         }
         if (intr->intrinsic != nir_intrinsic_load_input &&
             intr->intrinsic != nir_intrinsic_load_interpolated_input)
            continue;
         loc = nir_intrinsic_io_semantics(intr).location;
         sp->varying[intr->def.index] = varyings;
         for (unsigned i = 0; i < varyings; i++)
            if (location[i] == loc)
               sp->varying[intr->def.index] = i;
         if (sp->varying[intr->def.index] == varyings)
            location[varyings++] = loc;
      }
   }
   free(location);
   sp->tex_words = BITSET_WORDS(MAX2(fetches, 1));
   sp->vary_words = BITSET_WORDS(MAX2(varyings, 1));
   return true;
}

/* A boolean operation whose result feeds another boolean operation: the
 * links of a chain such as failed = failed || a != b. */
static bool
fs_bool_link(const nir_alu_instr *alu)
{
   if (alu->def.bit_size != 1 ||
       (alu->op != nir_op_ior && alu->op != nir_op_iand &&
        alu->op != nir_op_ixor))
      return false;
   nir_foreach_use(src, &alu->def) {
      nir_instr *user = nir_src_use_instr(src);

      if (user->type == nir_instr_type_alu &&
          nir_instr_as_alu(user)->def.bit_size == 1)
         return true;
   }
   return false;
}

static bool
fs_bool_chain(nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir)
      nir_foreach_block(block, impl)
         nir_foreach_instr(instr, block)
            if (instr->type == nir_instr_type_alu &&
                fs_bool_link(nir_instr_as_alu(instr)))
               return true;
   return false;
}

/* Hands a chain link on as b2f32 and rebuilds the boolean from it, so the
 * splitter finds a 32-bit value at every link. */
static bool
fs_bool_materialize(nir_builder *b, nir_alu_instr *alu, void *data)
{
   (void)data;
   if (!fs_bool_link(alu))
      return false;
   b->cursor = nir_after_instr(&alu->instr);
   nir_def *f = nir_b2f32(b, &alu->def);
   nir_def *back = nir_fneu_imm(b, f, 0.0);
   nir_def_rewrite_uses_after(&alu->def, back);
   return true;
}

static bool
fs_split_passes(struct nir_shader *nir, const struct m9_fs_blend *blend,
                struct m9_fs_passes *passes, const char **reason,
                unsigned force, enum fs_cut_kind kind, nir_def *blocker);

bool
m9_fs_compile_passes(struct nir_shader *nir, const struct m9_fs_blend *blend,
                     struct m9_fs_passes *passes, const char **reason)
{
   nir_def *blocker = NULL;
   enum fs_cut_kind kind = FS_CUT_TEX;
   bool ok;

   *reason = NULL;
   memset(passes, 0, sizeof(*passes));
   if (nir->info.stage != MESA_SHADER_FRAGMENT) {
      *reason = "not a fragment shader";
      return false;
   }
   /* M9X_FS_FORCE_SPLIT=N splits a shader the engine would run in one pass
    * and caps each producing pass at N units, which is how a schedule of
    * the captured shape is exercised against the hardware. */
   unsigned force = debug_get_num_option("M9X_FS_FORCE_SPLIT", 0);

   if (!fs_srgb(nir, blend, reason))
      return false;
   fs_prepare(nir);
   if (!force &&
       fs_compile_pass(nir, blend, &passes->pass[0], NULL, false, NULL, 0,
                       reason, &blocker)) {
      passes->count = 1;
      return true;
   }
   if (!force && !fs_cut_kind_of(*reason, blocker, &kind))
      return false;
   ok = fs_split_passes(nir, blend, passes, reason, force, kind, blocker);
   /* A boolean chain has no 32-bit value to cut at: retry once with each
    * boolean operation's result also held as a float. */
   if (!ok && !force && fs_bool_chain(nir)) {
      nir_shader *clone = nir_shader_clone(NULL, nir);
      const char *why = NULL;

      NIR_PASS(_, clone, nir_shader_alu_pass, fs_bool_materialize,
               nir_metadata_control_flow, NULL);
      if (fs_split_passes(clone, blend, passes, &why, false, kind, NULL)) {
         *reason = NULL;
         ok = true;
      }
      ralloc_free(clone);
   }
   return ok;
}

/* The pass loop of m9_fs_compile_passes once a single pass has failed. */
static bool
fs_split_passes(struct nir_shader *nir, const struct m9_fs_blend *blend,
                struct m9_fs_passes *passes, const char **reason,
                unsigned force, enum fs_cut_kind kind, nir_def *blocker)
{
   nir_function_impl *impl = NULL;
   nir_def *out = NULL, *cut[M9_FS_MAX_PASSES], *rejected[M9_FS_MAX_PASSES];
   struct fs_split sp = { .cut = cut };
   unsigned rejects = 0, words;
   bool ok = false;

   memset(passes, 0, sizeof(*passes));

   impl = nir_shader_get_entrypoint(nir);
   if (exec_list_length(&impl->body) != 1) {
      *reason = "control flow";
      return false;
   }
   nir_index_ssa_defs(impl);
   sp.defs = impl->ssa_alloc;
   sp.cone = calloc(sp.defs + 1, sizeof(*sp.cone));
   sp.fetch = calloc(sp.defs ? sp.defs : 1, sizeof(*sp.fetch));
   sp.varying = calloc(sp.defs ? sp.defs : 1, sizeof(*sp.varying));
   if (!sp.cone || !sp.fetch || !sp.varying) {
      *reason = "out of memory";
      goto done;
   }
   if (!fs_split_index(&sp, impl, &out, reason))
      goto done;
   words = sp.tex_words + sp.vary_words;
   sp.pool = calloc((sp.defs + 1) * words, sizeof(*sp.pool));
   if (!sp.pool) {
      *reason = "out of memory";
      goto done;
   }
   for (unsigned i = 0; i <= sp.defs; i++) {
      sp.cone[i].tex = sp.pool + i * words;
      sp.cone[i].vary = sp.cone[i].tex + sp.tex_words;
   }
   if (!out) {
      *reason = "no color output";
      goto done;
   }
   for (passes->count = 0; passes->count + 1 < M9_FS_MAX_PASSES; ) {
      unsigned i = passes->count;
      nir_def *v = NULL;
      bool taken = false;

      /* A coordinate the engine cannot interpolate is cut where it is
       * computed; anything else is cut where the resource that ran out
       * says. */
      if (blocker) {
         bool skip = false;

         for (unsigned k = 0; k < sp.cut_count; k++)
            skip |= cut[k] == blocker;
         for (unsigned k = 0; k < rejects; k++)
            skip |= rejected[k] == blocker;
         if (!skip && blocker != out)
            v = blocker;
      }
      taken = v != NULL;
      if (!v)
         v = fs_choose_cut(&sp, impl, out,
                           force ? force : M9_FS_MAX_TEXTURES, rejected,
                           rejects, kind, force != 0);
      if (!v) {
         if (i && fs_compile_pass(nir, blend, &passes->pass[i], NULL, false,
                                  cut, sp.cut_count, reason, NULL)) {
            passes->count++;
            ok = true;
            goto done;
         }
         *reason = "no value splits this shader into raster passes";
         goto done;
      }
      if (!fs_compile_pass(nir, NULL, &passes->pass[i], v, true, cut,
                           sp.cut_count, reason, &blocker)) {
         /* A producer that cannot compute the coordinate either is cut at
          * the coordinate *it* stopped on, one link deeper in the chain. */
         if (*reason == fail_coord_not_varying && blocker && blocker != v)
            continue;
         if (rejects >= ARRAY_SIZE(rejected))
            goto done;
         rejected[rejects++] = v;
         if (taken)
            blocker = NULL;
         continue;
      }
      cut[sp.cut_count++] = v;
      passes->count++;
      rejects = 0;
      blocker = NULL;
      if (fs_compile_pass(nir, blend, &passes->pass[passes->count], NULL,
                          false, cut, sp.cut_count, reason, &blocker) &&
          (!force ||
           m9_fs_sampler_count(&passes->pass[passes->count]) <= force ||
           passes->count + 1 >= M9_FS_MAX_PASSES - 1)) {
         passes->count++;
         ok = true;
         goto done;
      }
      if (!force && !fs_cut_kind_of(*reason, blocker, &kind))
         goto done;
   }
   if (!ok && !*reason)
      *reason = "more raster passes than the driver emits";
done:
   free(sp.cone);
   free(sp.fetch);
   free(sp.varying);
   free(sp.pool);
   if (!ok)
      m9_fs_passes_fini(passes);
   return ok;
}

void
m9_fs_passes_fini(struct m9_fs_passes *passes)
{
   for (unsigned i = 0; i < M9_FS_MAX_PASSES; i++)
      m9_fs_program_fini(&passes->pass[i]);
   memset(passes, 0, sizeof(*passes));
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
   /* Bits 23:20 count the texture units in unary: one unit reads 0x1, two
    * 0x3, three 0x7 and four 0xf, the last in the glamor gradient cases.
    * A program with no texture still sets the first. */
   uint32_t r88 = BITFIELD_MASK(MAX2(program->texture_count, 1u)) << 20 |
                  0xfff0;
   unsigned low = 0;

   /* One bit an interpolated input, and only six of them: setting bit 30
    * for a seventh made input 4 read garbage (MEASURED). */
   for (unsigned k = 0; k < program->varying_count; k++)
      r88 |= BITFIELD_BIT(24 + k);
   if (program->fragcoord)
      r88 |= 0x00080000;
   /* Bits 15:4 flag register pairs 08/09 .. 1e/1f the program leaves unused. */
   r88 &= ~((uint32_t)program->registers_used << 4);
   /* A texel pair flagged unused is one the engine reuses, and a pass that
    * binds four units then reads nothing at register 0e: fixture texhi:4:0
    * renders half its pixels wrong with bit 7 set and pixel-exact with it
    * cleared (MEASURED, and index 88 swept a bit at a time).  So every
    * texel pair the bound units occupy is marked live; the ~0x70 below is
    * the same rule for the pairs a shorter pass reaches.
    * M9X_TEXEL_HIGH=0 restores the encoding that renders wrong. */
   if (debug_get_bool_option("M9X_TEXEL_HIGH", true))
      for (unsigned r = 8; r < 4 * program->texture_count; r += 2)
         r88 &= ~BITFIELD_BIT(4 + (r - 8) / 2);
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
   /* A pass that writes the record instead of the colour sets two more
    * flags, one place higher for every unit it binds.  Both captured
    * producers follow it: 0110fffc where the producer samples nothing
    * (the seven two-pass cases, 0xfff0 | 0x0c) and 0110dffb where it
    * samples one (the five-sampler schedule, 0xffe3 | 0x18).  Two points
    * fix the rule; beyond one unit it is UNTESTED.  From two units up the
    * flags land inside the register-pair field above, which record_reserved
    * keeps empty rather than dropping a flag the captures carry. */
   if (program->writes_record) {
      /* M9X_RECORD_88 replaces the derived low nibbles, so the value can
       * be swept against the hardware. */
      int override = debug_get_num_option("M9X_RECORD_88", -1);

      low = override >= 0 ? (unsigned)override & 0xff :
                            low | 0x0cu << program->texture_count;
      if (override >= 0)
         r88 = (r88 & ~0xffu) | low;
   }
   /* M9X_R88_AND / M9X_R88_OR sweep index 88 against the hardware, which is
    * how a field no capture pins is measured rather than guessed. */
   r88 = (r88 | low) & (uint32_t)debug_get_num_option("M9X_R88_AND", 0xffffffff);
   r88 |= (uint32_t)debug_get_num_option("M9X_R88_OR", 0);
   low = 0;
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
