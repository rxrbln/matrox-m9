<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->

# M9 native vertex backend: what it compiles, and on what evidence

`src/gallium/drivers/m9x/m9_vs_compile.c` turns a NIR vertex shader into the
head of a native preprocessing program.  The instruction set it emits is the
one `docs/parhelia-m9-native-vertex-isa.md` decodes; this document records the
compiler's own contracts, what each form rests on, and what is not covered.

## Result

Twelve vertex shaders, glmark2's `bump-poly.vert` among them, compile and
verify offline (OFFLINE, `m9-vs-compile-test`).  `mvp * position` with a
colour varying compiles to **36 slots, the same length the vendor compiler
emits for it**, with the transform in the vendor's own eight instructions.

## 1. What the compiler produces, and what it appends

The compiled head does three things: it reads the declared attribute
registers, it computes the clip position into `r00`/`r01`, and it computes one
varying into `r02` (x,y) and `r13` (z,w).  `m9_vs_emit()` then appends slots
14..38 of `m9_passthrough_c2_program` **verbatim** and the stop pair.

That tail is the reciprocal of `w`, the perspective divide, the viewport scale
and bias, the depth range and the clip code.  It is copied rather than
re-derived for two reasons the reverse engineering settled: it is the same 25
slots in every captured single-pass program, and one of its instructions
(the three-source `op2 = 0x00` form at W1 `0x8c01cd10`) is undecoded.

Its interface to the head is exactly:

| register | holds |
|---|---|
| `r00` | clip x, clip y |
| `r01` | clip z, clip w |
| `r02` | varying x, y -- the tail moves it to the output register `0x11` |
| `r13` | varying z, w -- an output register the head writes itself, as the vendor does |

`r03` is reserved: the partial dot names it in its `c` field and nothing in
the vendor program writes it, so the compiler does not either.  Everything the
tail writes -- `r04`..`r0f`, `r19`, `r1b`..`r1d`, `r21` -- is free for the head,
which runs first.  The temporary pool is `r04`..`r12` and `r14`..`r21`, 29
registers.

## 2. Constants

The tail names constant pairs 9..21 by absolute index, so user constants take
pairs 0..8 and then 22 upward: `m9_vs_const_pair()` is that map, and
`m9_vs_const_count()` reports the span the pass needs, never fewer than the 22
pairs the tail occupies.  The driver keeps filling pairs 9..21 from
`m9_draw_params_init()`, which already computes exactly those words.

A constant pair is either two literals or two constant-buffer components; a
uniform paired with a literal zero keeps the pair addressable through the
`M9_VS_UNIFORM_ZERO` offset, which is what a three-component dot against a
uniform vector needs.

## 3. The value model

A native register holds two scalars and every instruction carries a
destination scalar mask, so a vector value occupies one register per component
pair -- the same model the fragment backend uses, because the two units share
the instruction encoding field for field.

Each register scalar records *what it holds*: a component of a declared
attribute, a component of a NIR definition, a literal, a constant-buffer
component, or an intermediate of the instruction being emitted.  An operand is
then a register whose two scalars are the two source components in some order,
found by scanning; the two-bit operand swizzle names the order (bit 1 is the
source scalar of result scalar 0).  When no register carries them, up to two
moves gather them into a fresh one.

Registers are reclaimed by liveness: a definition's last use is the
instruction index computed in a backward pass, and a register is free once no
scalar it holds is still live.  A definition folded into a multiply-add, and
one that only feeds a vector or move, extends its sources' liveness to its own
consumer instead.

The attribute declaration pads a short input with the GL default `(0, 0, 0, 1)`,
which makes `vec4(attribute.xyz, 1.0)` a register pair the program already
has -- no instruction, and the four-row transform reads it directly.

## 4. Two things the backend does that Mesa does not

**Multiply-add folding.**  Mesa has no pass that fuses `fadd(fmul(a, b), c)`
into `ffma`; drivers are expected to do it.  The hardware's multiply-add takes
its multiplier from a constant pair, which is exactly what a matrix column is,
so the backend folds a product whose result feeds nothing else and whose
constant side is the same operand on every lane.  `mat4 * vec4` -- which GLSL
gives as a linear combination of columns, not as dot products -- therefore
compiles to one multiply and three multiply-adds per lane, the vendor's eight.

**Output pinning.**  When both scalars of an output register come from one
lane that a single instruction writes whole, that instruction writes the
output register itself and the two moves disappear.  A lane written by a dot
product, a reciprocal or a reciprocal square root is excluded: those forms
write the first scalar only.

## 5. Instruction forms and their provenance

Reconstructed byte for byte from `m9_passthrough_c2_program`, so **CAPTURED in
the vertex unit**:

| form | op2 | slot |
|---|---|---|
| move register | 0x14 | s16 |
| move constant | 0x14 | s29 |
| multiply register by constant | 0x01 | s30 |
| multiply register by register | 0x01 | s23 |
| multiply-add, both addend and multiplier constant | 0x02 | s26 |
| partial dot against a constant pair | 0x10 | s5 |
| dot completion | 0x0e | s7, s12 |
| compare against a constant pair | 0x00, UNIT 1 | s14 |
| reciprocal seed, residual, Newton step | 0x11, 0x01 cls 5, 0x13 | s15, s17, s18 |

Taken from the fragment unit's measured forms, which section 5 of the ISA
document says the vertex unit shares, but **not captured in a vertex program**:
multiply-add with a register addend, add and subtract of two registers,
minimum and maximum, negate, saturate, fract/floor/ceil, select, and the
reciprocal-square-root chain.  These are UNTESTED on the vertex unit and are
what a hardware run has to confirm first.

Absolute value is a compare, a negate and a select, and square root is the
argument times its reciprocal square root with the vendor's zero guard --
both the shapes the fragment backend already uses.

## 6. Limits

* **Three attributes.**  The preprocessing packet has room for eight index-`23`
  words and two describe one attribute, so three fit and four do not.  This is
  a limit of the command stream, not of the hardware.
* **One varying.**  The driver's four-target configuration carries one varying
  vector; the register-to-target rule for any other target set is UNDECODED.
* **One basic block.**  Control flow is refused.
* **No exponential, logarithm or power.**  The fragment unit has those chains,
  but the vertex unit's own were not decoded, so they are refused rather than
  guessed at.
* A shader outside any of this fails with a reason and the draw module runs it,
  which is what the driver does today.

The point-size store the compatibility linker adds to every shader is accepted
and ignored when it writes the constant 1.0, and refused otherwise: the
preprocessing pass has no point-size output.

## 7. How it is verified

`m9-vs-compile-test` compiles each case with Mesa's GLSL compiler, lowers it
the way st/mesa hands a vertex shader to the driver, and then compares two
evaluations of the same random inputs:

1. the NIR the backend consumed, interpreted with Mesa's own constant folder;
2. the emitted program, run through a simulator that decodes the instruction
   words the backend wrote -- the attribute declaration included, so the
   selectors are checked as well.

The simulator treats the reciprocal and reciprocal-square-root seeds as exact.
Newton refinement from an exact seed is a fixed point, so the chains still have
to carry their operands through the right registers to agree; what is not
checked is the hardware's approximation itself.

```sh
ninja -C objs-m9x-llvm src/gallium/drivers/m9x/m9-vs-compile-test
./objs-m9x-llvm/src/gallium/drivers/m9x/m9-vs-compile-test
# M9_VS_DUMP=1 prints the NIR, M9_VS_PROGRAM=1 the emitted slots,
# M9_VS_TRACE=1 every register the simulator writes
```

## 8. What integration still needs

The backend is complete and tested offline; the driver still runs every vertex
shader in the draw module.  Binding it needs, in order:

1. a screen path that compiles the shader at `create_vs_state` time and keeps
   the draw-module shader as the fallback;
2. source records built from the raw vertex attributes and the index-`23`
   declaration from `m9_vs_decl_words()`, instead of the CPU-transformed
   position and colour the driver writes today;
3. the constant span written from `m9_vs_const()` entries resolved against
   constant buffer 0, with pairs 9..21 left as `m9_draw_params_init()` sets
   them;
4. the raster pass reading the varying from transient view 0 -- the vertex
   program's own `b0` target -- instead of the CPU-written views 4..6.

Step 4 is where the one-varying limit bites: a shader with two varyings has to
keep running in the draw module until the multi-target output contract is
decoded.
