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
   .fdot_replicates = true,
   .lower_bitops = true,
   .lower_extract_byte = true,
   .lower_extract_word = true,
   .lower_fdiv = true,
   .lower_fdph = true,
   .lower_fmod = true,
   .lower_sincos = true,
   .lower_uniforms_to_ubo = true,
   .force_indirect_unrolling = nir_var_all,
   .force_indirect_unrolling_sampler = true,
   .max_unroll_iterations = 32,
   .no_integers = true,
   .has_fused_comp_and_csel = true,
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
   N_LOAD,              /* constant factor moved into a register */
   N_COMPOSE,           /* vec4(texture.xyz, literal), written in place */
};

struct node {
   enum node_kind kind;
   int src[3];          /* MUL/ADD a,b; SUB b-a as a,b; MAD b*t + c */
   nir_def *def;
   unsigned k;          /* varying slot */
   unsigned texel;      /* texel register of the xy lane */
   unsigned ubo_slot;
   float lit[4];
   bool neg;            /* ADD with a negated literal */
   int level;
   int value[2];        /* per lane: 0 xy, 1 zw */
   uint8_t lane[3][2];  /* source lane read for result lane xy, zw */
   uint8_t swz[3][2];   /* 2-bit operand swizzle per result lane, 1 = xy */
};

enum lop_kind {
   L_INTERP, L_MUL_RC, L_MUL_RR, L_ADD_RC, L_ADD_RR, L_SUB_CR, L_MAD,
   L_MOV_OUT, L_MOVC_OUT, L_MOVC_TMP, L_MOVC_SECOND, L_RAW,
};

struct lop {
   enum lop_kind kind;
   unsigned lane;
   int node;
   int dst;             /* value id, -1 writes the output lane */
   int opval[2];        /* register operands as value ids */
   int opreg[2];        /* fixed register operands (texels), -1 if unused */
   int konst;           /* constant operand key, -1 if unused */
   uint8_t opswz[2];    /* register operand swizzles */
   uint8_t kswz;        /* constant operand swizzle */
   uint32_t w0, w1;     /* L_RAW */
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
   struct util_dynarray nodes, lops, values, keys, key_index;
   const char *reason;
   bool has_consts;
   bool custom_consts;  /* a template supplies the whole constant span */
};

#define NODES(c) ((struct node *)(c)->nodes.data)
#define LOPS(c) ((struct lop *)(c)->lops.data)
#define VALUES(c) ((struct value *)(c)->values.data)
#define KEYS(c) ((struct kkey *)(c)->keys.data)
#define NUM(c, a, t) util_dynarray_num_elements(&(c)->a, t)

static bool
fail(struct ctx *c, const char *reason)
{
   if (!c->reason)
      c->reason = reason;
   return false;
}

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

static int
build_intrinsic(struct ctx *c, nir_intrinsic_instr *intr)
{
   int n;

   switch (intr->intrinsic) {
   case nir_intrinsic_load_interpolated_input:
   case nir_intrinsic_load_input: {
      unsigned location = nir_intrinsic_io_semantics(intr).location;
      int k;

      if (location == VARYING_SLOT_POS || intr->def.num_components != 4 ||
          nir_intrinsic_component(intr))
         return fail(c, "unsupported fragment input"), -1;
      k = varying_slot(c, location);
      if (k < 0)
         return -1;
      n = node_new(c, N_VARY);
      if (n < 0)
         return -1;
      NODES(c)[n].k = k;
      return n;
   }
   case nir_intrinsic_load_ubo_vec4:
      if (!nir_src_is_const(intr->src[0]) || nir_src_as_uint(intr->src[0]) ||
          !nir_src_is_const(intr->src[1]) || nir_intrinsic_component(intr) ||
          intr->def.num_components != 4)
         return fail(c, "unsupported uniform access"), -1;
      n = node_new(c, N_UBO);
      if (n < 0)
         return -1;
      NODES(c)[n].ubo_slot = nir_intrinsic_base(intr) +
                             nir_src_as_uint(intr->src[1]);
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
   nir_scalar s[2];

   if (tex->op != nir_texop_tex || tex->sampler_dim != GLSL_SAMPLER_DIM_2D ||
       tex->is_shadow || tex->is_array || tex->num_srcs != 1 || ci < 0 ||
       p->texture_count >= M9_FS_MAX_TEXTURES)
      return fail(c, "texture sampling needs a computed coordinate pass"), -1;
   /* Linked vec2 varyings may share one slot: xy or zw of the input. */
   for (unsigned i = 0; i < 2; i++) {
      s[i] = nir_scalar_chase_movs(nir_get_scalar(tex->src[ci].src.ssa, i));
      if (s[i].comp != s[0].comp + i || (s[0].comp & 1) ||
          s[i].def != s[0].def || !nir_scalar_is_intrinsic(s[i]))
         return fail(c, "texture coordinate is not a varying"), -1;
   }
   coord = nir_def_as_intrinsic(s[0].def);
   if (coord->intrinsic != nir_intrinsic_load_interpolated_input)
      return fail(c, "texture coordinate is not a varying"), -1;
   location = nir_intrinsic_io_semantics(coord).location;
   for (unsigned i = 0; i < p->varying_count; i++)
      if (p->varyings[i].location == location)
         return fail(c, "texture coordinate also used as a varying"), -1;
   p->textures[p->texture_count] = tex->texture_index;
   p->texcoords[p->texture_count] = location;
   p->texcoord_components[p->texture_count] = s[0].comp;
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
   for (unsigned k = 0; k < 4; k++) {
      s[k] = nir_scalar_chase_movs(nir_get_scalar(alu->src[i].src.ssa,
                                                  alu->src[i].swizzle[k]));
      same &= s[k].def == s[0].def && !nir_scalar_is_const(s[k]);
      identity &= s[k].comp == k;
   }
   if (same && !identity && s[0].def->num_components == 4) {
      for (unsigned l = 0; l < 2; l++) {
         unsigned c0 = s[2 * l].comp, c1 = s[2 * l + 1].comp;

         if (c0 / 2 != c1 / 2)
            return fail(c, "swizzle crosses register lanes"), -1;
         lane[l] = c0 / 2;
         swz[l] = (c0 & 1) << 1 | (c1 & 1);
      }
      for (unsigned k = 0; k < 4; k++)
         s[k].comp = k;
   }
   return build_vec(c, s);
}

static void
set_src(struct node *n, unsigned i, int src, const uint8_t *lane,
        const uint8_t *swz)
{
   n->src[i] = src;
   memcpy(n->lane[i], lane, 2);
   memcpy(n->swz[i], swz, 2);
}

static int
build_alu(struct ctx *c, nir_alu_instr *alu)
{
   static const uint8_t lane_id[2] = { 0, 1 }, swz_id[2] = { 1, 1 };
   uint8_t lane[3][2], swz[3][2];
   int a, b, t, n, s;

   if (alu->def.num_components != 4)
      return fail(c, "non-vec4 arithmetic"), -1;
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
      if (node_is_const(&NODES(c)[a])) {
         int load;

         if (alu->op != nir_op_fmul)
            return fail(c, "constant expression"), -1;
         /* The vendor moves a constant factor into a register. */
         load = node_new(c, N_LOAD);
         if (load < 0)
            return -1;
         set_src(&NODES(c)[load], 0, a, lane[0], swz[0]);
         a = load;
         memcpy(lane[0], lane_id, 2);
         memcpy(swz[0], swz_id, 2);
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
   case nir_op_flrp:
      a = alu_src_node(c, alu, 0, lane[0], swz[0]);
      b = a < 0 ? -1 : alu_src_node(c, alu, 1, lane[1], swz[1]);
      t = b < 0 ? -1 : alu_src_node(c, alu, 2, lane[2], swz[2]);
      if (t < 0)
         return -1;
      for (unsigned i = 0; i < 3; i++)
         if (memcmp(lane[i], lane_id, 2) || memcmp(swz[i], swz_id, 2))
            return fail(c, "swizzled mix operand"), -1;
      if (node_is_const(&NODES(c)[a]) || !node_is_const(&NODES(c)[t]) ||
          NODES(c)[b].kind == N_LIT)
         return fail(c, "unsupported mix operands"), -1;
      s = node_new(c, N_SUB);
      if (s < 0)
         return -1;
      NODES(c)[s].src[0] = a;
      NODES(c)[s].src[1] = b;
      n = node_new(c, N_MAD);
      if (n < 0)
         return -1;
      NODES(c)[n].src[0] = s;
      NODES(c)[n].src[1] = t;
      NODES(c)[n].src[2] = a;
      return n;
   default:
      return fail(c, "unsupported arithmetic operation"), -1;
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
   for (unsigned k = 0; k < 4; k++) {
      if (s[k].def != s[0].def || s[k].comp != k)
         return fail(c, "swizzled or composed vector"), -1;
   }
   if (s[0].def->num_components != 4 || s[0].def->bit_size != 32)
      return fail(c, "non-vec4 value"), -1;
   return build_def(c, s[0].def);
}

static int
varying_slot(struct ctx *c, unsigned location)
{
   struct m9_fs_program *p = c->p;

   for (unsigned i = 0; i < p->varying_count; i++)
      if (p->varyings[i].location == location)
         return i;
   for (unsigned i = 0; i < p->texture_count; i++)
      if (p->texcoords[i] == location)
         return fail(c, "texture coordinate also used as a varying"), -1;
   /* The kernel accepts two raster streams besides position. */
   if (p->varying_count + p->texture_count >= 2)
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
      return key_uniform((s->ubo_slot * 4 + 2 * lane) * 4,
                         (s->ubo_slot * 4 + 2 * lane + 1) * 4);
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
   l->opval[0] = l->opval[1] = -1;
   l->opreg[0] = l->opreg[1] = -1;
   l->opswz[0] = l->opswz[1] = 1;
   l->kswz = 1;
   l->konst = -1;
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

static bool
emit_node_lops(struct ctx *c, int n, bool output, bool direct_ba)
{
   struct node *nd = &NODES(c)[n];
   /* The vendor starts with R,G when an operand register is replicated. */
   bool rg_first = nd->kind == N_MUL && replicated_register(c, nd);

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
   for (int level = 1; level <= max_level; level++) {
      for (unsigned i = 0; i < count; i++) {
         struct node *nd = &NODES(c)[i];

         if (nd->kind != N_VARY && nd->kind != N_LOAD &&
             nd->kind != N_COMPOSE && nd->level - 1 == level &&
             !emit_node_lops(c, i, i == (unsigned)root, max_level == 1))
            return false;
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

      for (unsigned i = 0; i < 2; i++) {
         if (l->opval[i] < 0)
            continue;
         VALUES(c)[l->opval[i]].last = s;
         VALUES(c)[l->opval[i]].rg_read |= rg;
      }
      if (l->dst >= 0)
         VALUES(c)[l->dst].def = s;
   }
   for (int s = n - 1; s >= 0; s--) {
      struct lop *l = &LOPS(c)[s];

      for (unsigned i = 0; i < 2; i++) {
         struct value *v;
         int reg = -1;

         if (l->opval[i] < 0)
            continue;
         v = &VALUES(c)[l->opval[i]];
         if (v->reg >= 0 || v->general)
            continue;
         if (v->interp) {
            if (v->rg_read && !reg_busy(busy, REG_I))
               reg = REG_I;
            for (unsigned r = base; reg < 0 && r < base + 2; r++)
               if (!reg_busy(busy, r))
                  reg = r;
            for (unsigned r = 0; reg < 0 && r < ARRAY_SIZE(interp_extra); r++)
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
   for (unsigned s = 0; s < n; s++) {
      struct lop *l = &LOPS(c)[s];

      if (l->dst >= 0 && VALUES(c)[l->dst].general) {
         if (next_general > 0x0f)
            return fail(c, "out of temporaries");
         VALUES(c)[l->dst].reg = next_general++;
      }
   }
   util_dynarray_foreach(&c->values, struct value, v) {
      if (v->reg < 0 && v->last >= 0)
         return fail(c, "internal: unallocated value");
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

/* Output assembly */

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
       nir_scalar_alu_op(s[0]) == nir_op_fmul && s[0].def->num_components == 2)
      return tmpl_fragcoord(c, s);
   if (!discard && nir_scalar_is_alu(s[0]) &&
       nir_scalar_alu_op(s[0]) == nir_op_bcsel)
      return tmpl_select(c, s);
   if (fragcoord)
      return fail(c, "unsupported gl_FragCoord use");
   if (!discard && (all_const || (s[0].def != s[1].def)) &&
       tmpl_scalars(c, s))
      return true;
   c->reason = NULL;
   c->p->registers_used = 0;
   util_dynarray_clear(&c->keys);
   util_dynarray_clear(&c->key_index);
   util_dynarray_clear(&c->p->consts);
   util_dynarray_clear(&c->lops);
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
      return schedule_tree(c, root) && allocate(c);
   }
}

static bool
compile_impl(struct ctx *c, nir_function_impl *impl)
{
   nir_intrinsic_instr *store = NULL, *kill = NULL;
   bool discard = false, first = true;

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
            return fail(c, "unsupported intrinsic");
         }
         first &= intr->intrinsic == nir_intrinsic_terminate;
      }
   }
   if (!store)
      return fail(c, "no color output");
   if (kill && (discard || !tmpl_stipple(c, kill, store->src[0].ssa)))
      return fail(c, "unsupported conditional discard");
   if (!kill && !compile_output(c, store->src[0].ssa, discard))
      return false;
   c->has_consts |= discard;
   if (!encode_lops(c))
      return false;
   finish_slots(c);
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

bool
m9_fs_compile(struct nir_shader *nir, struct m9_fs_program *program,
              const char **reason)
{
   struct ctx c = { .nir = nir, .p = program };
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
   return r88 | low;
}
