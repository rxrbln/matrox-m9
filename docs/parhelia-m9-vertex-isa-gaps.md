<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
# M9 vertex instruction set: what is missing, what is only missing from the
# compiler, and what the hardware does not have

`docs/parhelia-m9-native-vertex-isa.md` records the M9 (`102b:0540`) vertex
program the driver runs; `docs/parhelia-m9-native-vertex-backend.md` records
what `m9_vs_compile.c` compiles into it.  This note answers the separate
question of what the backend still **refuses**, and for each refusal says
whether an instruction exists, whether the compiler simply never learned a
lowering the fragment backend already has, or whether the hardware lacks the
operation outright.

It was opened because ioquake3's opengl2 renderer is almost entirely on the
CPU vertex path: of its fifteen vertex programs, eight are refused on `fsin`,
two on more than four vertex attributes, and five compile natively (MEASURED
by the caller of this work, not re-derived here).

Labels: **STATIC** is read from a named address of a named binary, with the
bytes; **CAPTURED** is read from a saved vendor command stream; **DERIVED** is
a join of those, with the join given; **MEASURED** is a hardware result;
**OPEN** names what is not settled and the one experiment that would settle it.

Run `python3 tools/m9-vertex-isa-gap-evidence.py` (47 checks) to re-derive every STATIC,
CAPTURED and DERIVED claim below.  It ends with negative controls that must
find a planted fault, so a pass cannot be a stale one.  The vendor package is
not in the repository, so pass `--dll` when `xddm32_*/MSerX/MxMD.dll` lives
elsewhere; without it the static half reports itself unverified rather than
quietly passing.

## Result, in one table

| refused | verdict | where it stands |
|---|---|---|
| `fsin`, `fcos` | **no instruction exists**; the vendor lowers it | implemented behind `M9X_VS_SINCOS`, verified offline, never run on the engine |
| `flog2`, and so `fpow` | an instruction exists on the fragment unit; not seen on the vertex unit | OPEN, one hardware probe (§4) |
| `iand`, `ior`, `inot`, `b2f16` | **no integer ALU exists**; they are float lowerings | implemented, verified offline, no new instruction word |
| `flrp` | a float lowering, and the driver's own NIR options already lower it | nothing to do (§5) |
| register-register dot | **an instruction exists and already runs**, `op2 = 0x0d` | DERIVED, not yet used by either backend (§6) |

Two further findings fell out and are recorded because they change what the
existing notes say: several forms the backend note calls "untested on the
vertex unit" are in fact CAPTURED there (§6), and the offline gate compiles
NIR that differs from what the driver hands the backend (§7).

## 1. There is no sine and no cosine (STATIC, CAPTURED, DERIVED)

### 1.1 The vendor's compiler keeps sine coefficients, not a sine opcode

`MSerX/MxMD.dll`, SHA-256
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`, carries one
constant pool for the shared shader compiler.  Addresses below are file offsets
with the image virtual address beside them; every section of this PE sits
`0x10000` above its file offset, image base included.  **STATIC**:

| file offset | image VA | words | what they are |
|---|---|---|---|
| `0x1faed8` | `0x20aed8` | `3af71c15 3c130514 3d64b436 3e75ea9e 3f317271 3f800000` | the exp2 polynomial |
| `0x1faef8` | `0x20aef8` | `bc188576 3d553bd2 be0cd266 3e77ab67 beb1d173 3ef51608 bf389e52 3fb8aa10` | the log2 polynomial |
| `0x1faf18` | `0x20af18` | `b5c08e8a 35902424 39881741 b8ca0212 bcaaa5d9 3b882268 3effffc7 bdaa9f95` | **the sine polynomial** |
| `0x1faf38` | `0x20af38` | `00000000 3f7ffea6` | its linear term |
| `0x1faf48` | `0x20af48` | `40490fdb 40c90fdb 3e22f983 3f000000` | pi, 2pi, 1/2pi, 0.5 |

The first two rows are exactly the coefficients
`docs/parhelia-m9-native-fragment-arithmetic.md` measured on hardware for the
exp2 and log2 chains.  The sine rows begin at the byte the log2 row ends on
(**STATIC**: `0x1faef8 + 8 words = 0x1faf18`), so the sine coefficients are in
the same pool, reached the same way.  Each row is handed out by a constant-id
dispatcher at `0x180e20 .. 0x18142c`, one arm per pair, which loads the pool
address into `eax`: `0x181419` for exp2, `0x181400` for log2, `0x1813ce` for
the sine polynomial, `0x180f69` for its linear term and `0x180f8d` for the
reduction row (**STATIC**, each address is the `mov $ADDR,%eax` of that arm).
Each of the four distinctive sine coefficients occurs exactly once in the whole
image, so the pool is the only place the compiler keeps them.

A polynomial the compiler must upload is not an instruction the silicon runs.

### 1.2 The vendor's own program confirms it (CAPTURED)

`captures/m9x-vendor-glsl-programs-20260915/fs-sin` is `gl_FragColor =
sin(v1)`, a four-component sine, and its whole fragment program is **21
slots**.  Decoded, the 21 are, in order (CAPTURED, and asserted by the tool):

```text
 0  1   op2=0x10   interpolate v1 into r00 and r01
 2  3   op2=0x02   t = v * (1/2pi) + 0.5                  pairs 8 and 9
 4  5   op2=0x14   t = fract(t)                           the measured fract class
 6  7   op2=0x02   x = t * 2pi + (-pi)                    pairs 6 and 7
 8  9   op2=0x01   y = x * x
10..15  op2=0x02   three Horner steps over y              pairs 4, 5, 3, 2
16 17   op2=0x0c cls 7   S = y * t3 + k1                  pair 1
18 19   op2=0x0c cls 0   result = 2 * x * S - x           pair 0
20      op2=0x14   move the second lane to the output
```

The eleven constant pairs the case binds are the pool's own rows plus pi, 2pi
and 1/2pi, in the pool's order (CAPTURED, asserted pair by pair).  Each pair
holds **two different coefficients**; the Horner chain's constant selects all
read the pair's **second** float, and the first floats are a second, unused
half whose values are exactly `0.5 * sinc(x/2)` — the cosine partner of a
shared sine-and-cosine table.

Nothing in those 21 slots could be a sine instruction: the argument is reduced
with a `fract` against uploaded constants and the answer is a polynomial in
`x * x`.

### 1.3 The arithmetic, folded (DERIVED)

Reading the chain with the second float of each pair gives, with `y = x * x`
and `x` the reduced argument in `[-pi, pi)`:

```text
t3 = ((1.07393544e-06 * y - 9.63249622e-05) * y + 0.00415449217) * y
     - 0.0833121911
S  = y * t3 + 0.999979377            op2 = 0x0c class 7
sin(x) = 2 * x * S - x               op2 = 0x0c class 0
```

which folds to `sin(x) = x * P(y)` with

```text
P = 2.14787088e-06 y^4 - 1.92649924e-04 y^3 + 8.30898434e-03 y^2
    - 0.166624382 y + 0.999958754
```

**DERIVED, and the join is numeric**: evaluated against the C library over
200001 points of `[-8pi, 8pi]`, including the reduction, the worst absolute
error is **6.925e-05**.  A minimax fit landing that close is not a coincidence,
and the negative controls show a one per cent change in a single coefficient
takes the error to 5.2e-02 and the wrong reduction phase to 1.4e+00.

What this does **not** settle: the exact semantics of `op2 = 0x0c`.  Class 0
must compute `2ab - a + k` for the program to be a sine, and class 7 must
compute `ab + k`; both readings are forced by the requirement that the result
be `sin`, not read off a second instance, because `op2 = 0x0c` occurs **only in
fs-sin** in the whole corpus — 4 slots, 0 of them in any vertex program
(CAPTURED).  It is **OPEN** whether class 0's doubling is a property of the
class or of the opcode.  Nothing here depends on the answer: the folded
polynomial needs no such instruction.

### 1.4 What this means for the vertex unit

No captured vertex program contains a sine, and `op2 = 0x0c` appears in no
vertex program.  But none of the folded form's steps needs it: a multiply-add
against two constants, a `fract`, a multiply-add, a square and a Horner chain
are all forms the vertex backend already emits.  **So `fsin` and `fcos` are a
lowering, not an encoding to find, and the recommendation is to lower them.**

`m9_vs_compile.c` now does, behind `M9X_VS_SINCOS`, **default off**:
`emit_sincos()` builds the vendor's reduction and the folded polynomial from
`emit_mad`, `emit_unary(VS_FRACT)`, `emit_mul` and `emit_add`.  `cos(x)` is
`sin(x + pi/2)`, which the phase of the reduction carries (0.75 in place of
0.5), so it costs the same.  A scalar sine is 12 slots for both scalars of a
register.  Offline (`m9-vs-compile-test`): `sincos` (a deformVertexes-shaped
scalar sine and cosine) compiles to 65 slots and `sincos-vec4` to 84, both
verified against the NIR reference — which evaluates the true `sinf` — within
the polynomial's own 1.5e-4 bound, and `sincos-default-off` is still refused
with the reason `fsin` when the knob is unset.

**The knob stays off until the engine has run it.**  Nothing about the
sequence is hardware evidence: every instruction form in it is one the backend
already emits and has run, but the sequence has not.

A second, cheaper route exists and is worth knowing about: Mesa's own
`nir_shader_compiler_options::lower_sincos` expands `fsin` and `fcos` into
`ffract`, `fmul`, `fsub`, `fabs` and `ffma`, all of which the backend already
accepts, so setting that one flag in `m9_vs_nir_options` would unblock
ioquake3 with no backend code at all.  Its approximation is about sixteen
times coarser (worst error near 1.1e-3 against 6.9e-5), which is why the
vendor's own polynomial was implemented instead.

## 2. There is no integer ALU (STATIC, and a lowering)

Three independent statements, none of them new evidence but none of them
previously joined:

* `docs/parhelia-m9-d3d-sm23-audit.md` records that the D3D compiler's integer
  and Boolean constant files feed comparisons, address and control
  expressions, and conversions only: "There is no D3D SM3 general integer ALU
  here."
* The screen declares no integer support — `no_integers = true` in both
  `m9_vs_nir_options` and the fragment backend's options — so Mesa stores even
  an integer uniform as a float.
* The fragment backend has always built `iand`, `ior` and `inot` out of float
  arithmetic: a condition holds 1.0 or 0.0, so and is a product, or is the
  larger of the two and not is one minus.

So the vertex backend's missing `iand`/`ior`/`inot` were never an encoding
gap; they were a lowering the fragment backend had and the vertex backend did
not.  `m9_vs_compile.c` now has them, **on by default because they emit no
instruction word the backend did not already emit** — a product, a maximum and
a subtract.  Only a one-bit definition is accepted; a 32-bit bitwise operation
still fails with `integer bitwise operation`, because nothing here implements
one.  `b2f16` joins `b2f32`.  Offline: `bool-ops` verifies a conjunction, a
negation and a disjunction against the NIR reference.

Shifts and integer compares were looked for and are not there: no captured
program of either stage contains an opcode that is not accounted for by the
float forms this note and the two ISA notes list.

## 3. `op2` census, by stage (CAPTURED)

The whole vendor corpus, 112 cases, with the shared 25-slot epilogue stripped
from every vertex program so the head's opcodes stand alone: 1248 fragment
slots and 4813 vertex slots.  The opcodes that appear in **only one** stage:

| op2 | stage | cases | what it is |
|---|---|---|---|
| `0x0c` | fragment only, 4 slots | `fs-sin` | the sine's combining step (§1.3) |
| `0x03` | fragment only, 3 slots | `fs-cross` | the cross product's step, arithmetic **OPEN** |
| `0x16` | fragment only, 1 slot | `fs-loop-break` | control flow |
| `0x1b` | fragment only, 13 slots | `fs-pow`, `fs-ternary`, `fs-discard-*` | the register/constant select |
| `0x1e` | fragment only, 7 slots | `fs-discard-*`, `ff-alpha-test` | the kill |
| `0x02` with two registers and a constant addend | fragment only, 4 slots | `fs-sin` | see §1.4 |
| `0x14` `hi=9` `W1[13:12]=2` | fragment only, 8 slots | `fs-log`, `fs-pow` | **the log2 seed** |

and the ones the vertex unit is now known to run, which §6 revisits.

## 4. `flog2` and `fpow`: OPEN, and the probe that settles them

The fragment unit's log2 is a seed instruction followed by a polynomial: the
seed splits the exponent from the mantissa, which nothing in the float-only
instruction set can do otherwise, so log2 cannot be lowered the way sine can.
`exp2`'s seed and `log2`'s seed are the **same opcode and the same `hi`
nibble** — `op2 = 0x14`, `hi = 9` — differing in `W1[13:12]`, 1 against 2.

* exp2's seed **is** CAPTURED on the vertex unit: 2 slots, in `ff-fog-exp` and
  `ff-fog-exp2`, and the backend emits it.
* log2's seed is CAPTURED **only** in fragment programs: 8 slots, `fs-log` and
  `fs-pow`, 0 vertex.

That the two seeds are neighbouring classes of one opcode makes it likely the
vertex unit has both, but likely is not evidence, and the backend note already
records that the fragment log2 chain cannot be validated offline — its
polynomial slots write the first scalar while their chained operand's swizzle
reads the second, which the simulator's model does not reproduce.  So `flog2`
and, through `exp2(y * log2(x))`, `fpow` stay **OPEN**.

**The probe.**  Extend `tools/m9-user-vertex-probe.c` with a variant that
writes the fragment note's log2 chain — `mov 0xa28ffc00`, seed `0x96852ec0`,
polynomial `0x944041e0`, three steps `0xa26008a0`, `mad 0xac4fcf00`, with the
four captured coefficient pairs — into the vertex program's head over a value
carried in an attribute, and compares the resulting geometry against a CPU
`log2f`.  One draw answers it.  Expect to have to reboot: the probe writes an
instruction class no vertex program has been seen to carry, and probing
unknown vertex encodings has locked this engine before (`0x5e`, `0x66`).  Take
both `/tmp/m9-hw.lock` and `/root/tmp/m9-hw.lock`, reload with
`KO=/root/tmp/dxtmip-m9x_drm.ko` named explicitly, and re-prove the engine
with a known-good fixture afterwards.

## 5. `flrp` needs nothing

`mix(a, b, t)` is `a + t * (b - a)`, three instructions the backend already
emits, and the fragment backend lowers it exactly so.  The vertex backend
never sees it: `m9_vs_nir_options` sets `lower_flrp32` and `lower_flrp64`, so
Mesa expands it before the backend is called.  Nothing to implement.  The
offline test does **not** set those options, which is one item of §7.

## 6. What the vertex unit is now known to run

`docs/parhelia-m9-native-vertex-backend.md` section 5 lists forms "taken from
the fragment unit's measured forms ... but not captured in a vertex program",
and calls them untested on the vertex unit.  The census says four of them are
CAPTURED there after all (slot counts are vertex heads, epilogue stripped):

| form | vertex evidence |
|---|---|
| maximum, `op2 = 0x19` class 3 | 2 slots, `vs-builtin-light`, `vs-lighting` |
| negate, `op2 = 0x15` | 2 slots, `ff-fog-exp`, `ff-fog-exp2` |
| select, `op2 = 0x1a` class 4 | 6 slots, `vs-builtin-light`, `vs-lighting`, `vs-math` |
| the reciprocal-square-root chain, seed `op2 = 0x11` class 5 and step `op2 = 0x13` class 7 | 5 and 10 slots, `vs-builtin-light`, `vs-lighting`, `vs-math` |
| multiply-add over three registers, `op2 = 0x02`, `e = 0x3f` | 5 slots, the same three cases |

That closes open item 4 of the ISA note for `rsq`: the vertex unit has the
reciprocal-square-root chain, byte for byte the fragment unit's, in the
vendor's own `normalize()` programs.  **Minimum** (`op2 = 0x19` class 2) is
still fragment-only, which is why the backend's two-register minimum lowering
stays as it is.

### A register-register dot product exists: `op2 = 0x0d` (DERIVED)

Both backends expand a dot of two *computed* vectors into multiplies and adds,
because the captured dot forms `0x0e` and `0x10` take a constant operand per
half.  There is a third form that does not.

`op2 = 0x0d`, destination mask `0x1d` (first scalar only), with the operand
registers in `a` and `b`, `c` encoded equal to `a`, and a constant pair index
in `e` whose scalar select sits at `W1[5:4]`:

```text
dst.s0 = a.s0 * b.s0 + a.s1 * b.s1 + k
```

**DERIVED from four independent programs**, each of which needs exactly three
or four products and spends one `0x0d` plus one ordinary scalar multiply-add:

* `fs-length`, `length(v1.xyz)`: `0x0d` over one interpolated pair, then
  `op2 = 0x02` adding the square of the third component;
* `fs-normalize`, `normalize(v1.xyz)`: the same shape;
* `fs-distance`, `distance(v1, u)`: `0x0d` over one difference pair, then
  `op2 = 0x10` over the other difference pair with the first result as a
  register addend — a second, register-addend form of the same idea, whose
  single join makes it weaker evidence;
* `vs-math` and `vs-lighting`, `normalize()` in a **vertex** program:
  `0x0d` over the attribute's first register pair, then `op2 = 0x02` for the
  third component.

It is not merely available on the vertex unit, it already runs there on every
native draw: **slot 36 of the epilogue `m9_vs_emit()` copies verbatim is
`c3c70a1d eda40c01`**, an `op2 = 0x0d` with `a = r0e`, `b = r0f`, `c = r0e`
and constant pair 16 — the clip code's dot over the two compare results.  The
ISA note calls that slot "a DP3"; it is a two-component dot plus a constant,
and that note is corrected.  The census finds `0x0d` in 100 vertex head slots
across 98 cases and in 17 epilogues.

Using it would take a four-component register-register dot from seven
instructions to two.  Nothing here implements that yet, and it is not on the
path to ioquake3, so it is recorded rather than done.  What stays **OPEN** is
whether the `c` field is read at all: every captured instance has `c == a`, so
a form with `c != a` is untested and must not be emitted.

## 7. The offline gate does not compile what the driver compiles

`m9_vs_compile_test.cpp` builds its own `nir_shader_compiler_options` rather
than using the driver's `m9_vs_nir_options` from `m9_screen.c`, and the two
disagree on four options that change which opcodes reach the backend:

| option | driver | offline test | effect |
|---|---|---|---|
| `lower_sincos` | unset | **set** | the test could never see `fsin` at all |
| `lower_flrp32`/`64` | set | unset | the test can see `flrp`, the driver cannot |
| `lower_fsat` | set | unset | the backend's `fsat` path is dead in the driver |
| `lower_scmp` | set | unset | different comparison shapes |
| `float_mul_add32` | unset | `has_ffma` | the test gets `ffma`, the driver gets `fmul` plus `fadd` and relies on the backend's own fusion |

The first row is why a sine could pass through this gate unnoticed for as long
as it did: **the gate that is supposed to say what the backend accepts could
not, by construction, ever have presented it with a sine.**  This work worked
around it — the three new cases set `lower_sincos` per case — rather than
fixing the divergence, because aligning all five options changes what every
existing case compiles and belongs in its own change.  The fix is for the test
to include the driver's option struct instead of restating it.

## 8. Reproduction

```sh
# every STATIC, CAPTURED and DERIVED claim above, with its negative controls
python3 tools/m9-vertex-isa-gap-evidence.py \
    --dll xddm32_210_00_105_se_u_nowhql/MSerX/MxMD.dll

# the backend, offline
/home/ai/tmp/m9x-ninja-llvm.sh src/gallium/drivers/m9x/m9-vs-compile-test
tools/m9-offline-gates.sh objs-m9x-llvm        # read the exit code, not the last line

# the three new cases on their own
M9_VS_PROGRAM=1 objs-m9x-llvm/src/gallium/drivers/m9x/m9-vs-compile-test
```
