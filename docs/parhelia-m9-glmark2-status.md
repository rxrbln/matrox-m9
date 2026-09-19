# glmark2 on the m9x driver: where the 60 fps goal actually stands

**MEASURED 2026-09-18** at 1920x1200 on the dual-DisplayPort M9128 (subsystem
102b:22c0, 1 GiB VRAM) in the 16 GB host, one head driven, stock X server with
the image's glamor, Mesa built with LLVM JIT (`draw-use-llvm`).

Measured with `tools/m9x-glmark-sweep.sh` and `tools/m9x-glmark-parse.awk`.
The parser refuses to count a scene whose draws the driver skipped, whose job
lost its context, or whose stream timed out - each of those reports a very high
frame rate precisely because nothing was drawn.  It annotates rather than
invalidates a scene that fell back off a hardware path, because such a scene
does draw.  A fault raised *after* the last scene's result is charged to the
run, since submission is asynchronous and the error surfaces later than the job
that caused it.

Sweep one scene per process: glmark2 segfaults (rc=139, client-side, no stream
timeout) moving from the 15-tap `effect2d` scene to `shadow` within one
process, and `shadow` run alone is 142 fps.

Every run asserts `GL_RENDERER` is `Matrox M9`: a Mesa build configured without
`--cross-file cross-t2-llvm.ini` links against the host libdrm and X, EGL falls
back to llvmpipe silently, and every fixture still passes against the CPU
reference.

## Standing 2026-09-18: all thirty-three run, twenty-nine above 60 fps

MEASURED at 1920x1200, one process per benchmark, 0 stream timeouts, fixture
suite 194 cases 194 PASS.  `desktop` draws its blur where it used to stall
and take the seventeen benchmarks behind it down with it.

Four are below 60, each for a different reason:

| benchmark | fps | why |
|---|---:|---|
| `effect2d` 15-tap | 31-33 | fragment execution.  The record-bandwidth reading is **refuted**: halving its stride from 16 dwords to 8 -- and its record from 141 MiB to 70 -- left it at the same 33 fps (MEASURED 2026-09-19, one binary, `M9X_RECORD_LIVENESS` the only variable).  `parhelia-m9-fill-rate.md` had it right |
| `desktop` blur | 38 | GPU execution: 26.77 ms a frame, submit 23.13 of it, ioctl 22.84, over 28 jobs and 24 draws with none refused or skipped.  At 0.82 ms a job this is work, not the per-job overhead that dominates `refract` at 0.23 |
| `terrain` | 1 | INVALID - draws refused by the **fragment backend**, not by the coverage rule: five distinct samplers against four units, and the register file.  MEASURED 2026-09-19 |
| ~~`refract`~~ | 38-40 | **conformant** - it was never the geometry class.  A record carries x and y multiplied by w with w in slot 3, so recovering a screen position **divides**; the extent test multiplied, which agrees only where w is 1 - every fixture it had been written against.  The bunny's computed extent landed at `6694,1583..10158,5042` on a 1920x1200 target, outside it, so the draw was refused for covering none of it.  With the divide, skipped draws go 1 to 0 a frame and CPU varyings 0.4 to 101.6 KiB.  `conv3subw` is the fixture that fails without it.  It still reports SETUP-FAILED inside the full run while passing alone, which is a separate unreleased-resource question |

**Open: which `refract` measurement is current.**  A second measurement on
2026-09-19 (`M9X_PROFILE=4`, 1920x1200) reported `refract` at 19.89 ms a frame
with **`skipped 1` and 0.3 KiB of CPU varyings**, 400 native against 2801 draw
module -- the *before* numbers of the row above, from a build made in a
different tree.  Nothing was reverted in `m9x` that would restore them, so the
likely cause is that build not carrying `2f752ee`.  Until a fresh rebuild of
current `m9x` HEAD is rerun on the card, treat 0 skipped / 101.6 KiB as the
figure to reproduce and this as unreconciled, not as two valid results.

**The depth attachment is not what refuses them, and carrying the
destination's depth box into the scratch surface would restore nothing**
(MEASURED, `M9X_SKIP_REPORT` over the whole default list): with the geometry
asked about first, *no* draw anywhere in the thirty-three is refused for
depth.  A scratch surface is the size of the box the geometry spans, so it
only helps a draw that **fills** that box -- a rectangle does, a mesh does
not.  `refract`'s missing draw is its bunny under a four-pass shader and
`terrain`'s is its mesh under seven, and no target of any size can be covered
by either, so the multi-pass schedule cannot carry them at all.  A single-pass
compilation would sidestep the coverage requirement, and **neither shader can
be compiled in one pass on this hardware** -- see below; see also
`parhelia-m9-fragment-pass-hardware.md` 3bc.

### Where their passes come from

OFFLINE, `M9_FS_SPLIT=1 m9-fs-compile-test --glsl` on glmark2's own shaders:

```text
light-refract.frag passes 4
    pass 0 units 0 samplers 0 varyings 3 record 1/2  routes coord
    pass 1 units 1 samplers 1 varyings 2 record 1/2  routes -1 coord 0
    pass 2 units 2 samplers 2 varyings 2 record 1/2  routes -1 -1 coord 1 0
    pass 3 units 1 samplers 1 varyings 2 record 0/0  routes -1 coord 2
```

**`refract` is a dependent texture chain, and nothing else.**  Its three
fetches would fit one pass -- four units are available and it uses four across
all four passes -- and every pass compiles, so neither the unit count nor the
register file is the constraint.  Each fetch's coordinate is computed from the
*previous fetch's result*: `DistanceMap` positions the read of `NormalMap`,
which positions the read of `ImageMap`.  A raster pass can only take a
coordinate that is affine in a varying, computed per vertex, or one handed to
it through the inter-pass record, so a fully dependent chain costs one pass per
sampler (already recorded in `parhelia-m9-fragment-pass-splitting.md` 5,
MEASURED from the vendor capture).  **No compiler can make this one pass**; it
would need the engine to sample with a coordinate computed inside the pass,
which it cannot do.

**`terrain` is both, plus a chain.**  It is refused before any splitting for
**out of temporaries** -- the register file, not the unit count -- and it binds
five distinct samplers against four units; with `M9X_SPLIT_PRESSURE=1` it
compiles into seven passes, three of which carry no sampler at all and exist
only to route values forward.  Its normal map feeds the lighting, so it has a
dependent link as well.

So aiming backend work at *fewer passes* will not close either scene.  The only
route that would is making a multi-pass job over geometry that does not cover
its target legal, which is the hardware limit in 3bb.  One idea nobody has
tried: a split job carries exactly one raster draw (3c), so the covering
geometry would have to be appended to that same draw -- extra triangles that
cover the target and are rejected before they write, if rasterised coverage
rather than written pixels is what the back end counts.  **UNTESTED.**

The rest, for the record: texture 284/283/284, build 271/271, shading
188/121/75/70, bump 89/179/154, effect2d 3x3 79, pulsar 188, desktop shadow
124, buffer 147/127/130, ideas 102, jellyfish 119, shadow 104, conditionals
198/123/197, function 146/63, loop 144/144/300.

## The suite is thirty-three benchmarks, not sixteen scenes

MEASURED 2026-09-18.  Every sweep before this one ran the sixteen scene
*names*; glmark2's default run is **thirty-three parameterised
benchmarks**, and variants reach paths a bare scene never does.  A driver
that passes the sixteen can still fail the run, so "12 of 15 above 60"
overstated where this driver stands.

First partial baseline at 1920x1200, one process per benchmark:

| benchmark | fps | |
|---|---:|---|
| `build` x2 | 267, 271 | ok |
| `texture` nearest / linear / mipmap | 285, 284, 284 | ok, but see below |
| `shading` gouraud / blinn-phong-inf / phong / cel | 188, 120, 75, 70 | ok |
| `bump` high-poly / normals / height | 88, 180, 157 | ok |
| `effect2d` 3x3 | 79 | ok |
| `effect2d` 15-tap | 31 | BELOW |
| `pulsar` | 200 | ok |
| `desktop` blur | 38 | BELOW, and it renders its blur |

Three findings from it:

* **`desktop`'s stall used to kill the seventeen benchmarks after it.**  The
  scratch surface closed that (`parhelia-m9-fragment-pass-hardware.md` 3bc):
  the whole list of thirty-three now runs in one go with no stream timeout,
  29 of them at or above 60 fps, and `desktop` draws its blur at 38 fps.  Its
  default string is `desktop:windows=4:effect=blur:blur-radius=5:passes=1:separable=true`,
  which is what "all glmark" has to mean.
* **`bump:bump-render=normals` runs at 180 fps** where the ledger recorded it
  as refused for an indirectly indexed uniform.  It was fixed at some point
  and nobody noticed, because only the bare `bump` scene was ever swept.
  There may be more stale entries of that kind.
* **`texture:texture-filter=mipmap` reports 284 fps and is almost certainly
  wrong**: `m9_blit_convert` resampled a filtered blit NEAREST, so every
  CPU-built level is a decimation rather than a reduction.  A fast, incorrect
  result in the "ok" column - which is why a frame-rate sweep cannot answer a
  conformance question on its own.

## The scenes that were blocked

All but one now render.  They were blocked on one feature - the fragment
multipass path - and on three defects found while implementing it.

| scene | rate | state |
|---|---|---|
| `effect2d` default (1 tap, no split) | 222-231 fps | renders |
| `effect2d` 3x3 kernel (2 passes) | 86-89 fps | renders |
| `effect2d` 15-tap (5 passes) | 25-33 fps | renders, with a 141 MiB record |
| `shadow` | 143-147 fps | renders |
| `refract` (4 passes) | 1 fps at 1920x1200, 2 at 640x480 | renders; 95% of the frame is the CPU mipmap of its distance target, not fill |
| `terrain` | 21 fps at 1920x1200, INVALID | `terrain.frag` still refused for register pressure; `M9X_SPLIT_PRESSURE=1` compiles it into seven passes over **mesh** geometry (576 records a draw), which fills no box a scratch surface could be given, so those draws are refused rather than drawn and it stays off |
| `desktop` separable blur (4 passes) | 38 fps at 1920x1200 | draws its blur: the partial pass renders on a scratch surface of the pixels it covers and is copied into place (3bc).  The frame is the engine's -- 26.77 ms of which 22.84 is the submit ioctl, against 2.73 draw and 2.66 emit |

The rest of the suite was already above 60 fps and is unchanged; the slowest
were `function medium` 62, `shading cel` 70, `shading phong` 87.

## Three of these cannot reach 60 fps, and that is the fill rate

`parhelia-m9-fill-rate.md` measures one full-screen fragment operation at
**1.157 ms** per 1920x1200 screen, linear across `alu0..alu16`.  A 60 fps frame
therefore holds **14.4** such operations.

| scene | full-screen operations | ceiling |
|---|---:|---:|
| `effect2d` 15-tap | ~19 | ~46 fps |
| `terrain` tilt-shift pair | ~18 | ~50 fps |
| `desktop` 11-tap separable blur over composited windows | ~23 | ~43 fps |

These are not failures and no scheduling reaches 60 fps on them at this size.
Correct rendering at an honest rate is the deliverable.

### `refract` is not fill-bound, and that reading is withdrawn

`refract` was recorded here as fill-bound.  It is not, and the arithmetic said
so before the measurement did: four raster passes into a distance target the
driver clamps to 2048x1280 is about sixteen full-screen operations, 18.5 ms,
some 54 fps, against a measured 1 fps.

The driver's own phase profile names the missing second (MEASURED,
`M9X_PROFILE=4` at 1920x1200):

```text
m9x: 4 frames 1065.62 ms/frame: draw 8.05 emit 0.11 submit 8.08 varying 0.00
     records 0.45 ioctl 7.25 target 0.01 clear 0.00 transfer 1014.03
     | jobs 40 draws 40 ...
```

**95.2% of the frame is a CPU transfer**, and the GPU half of it is 8 ms.  Ten
draws a frame is the whole of the scene's submission cost.

The transfer is the mipmap chain of the distance target.  `scene-refract.cpp`
calls `glGenerateMipmap` on its 2048x1280 colour render target once a frame
(CAPTURED, `RefractPrivate::draw`), the driver implements no
`generate_mipmap`, and `m9_update_target` refuses a target whose level is not
zero, so the state tracker falls back to mapping the texture and building
every level on the CPU -- through the de-tiling transfer path.

The size scaling confirms it (MEASURED): at a 640x480 canvas the distance
target is 1280x960, 2.13x smaller in area than the 2048x1280 the 1920x1200
canvas clamps to, and the transfer falls from 1014 ms to 473 ms, a ratio of
2.14.  The cost is proportional to that texture's area and to nothing else.

So the ceiling for `refract` is not the fill rate; it is a per-frame CPU
readback of a render target.  Rendering the mipmap chain on the engine -- or
allowing a non-zero level as a render target so `util_gen_mipmap` can -- is
what this scene needs.  **UNTESTED**, not implemented.

## What was actually wrong, in the order it was found

Each was closed from the working side - take a passing fixture, add one
property at a time until it fails - which beat every attempt to reason from the
command stream.

1. **Index `ef` bits 10:9** carry a raster pass's place in a multi-pass
   sequence (0 only, 1 first, 2 middle, 3 last).  The driver emitted zero,
   telling the engine every pass was a complete draw, so the inter-pass record
   never bound.  CAPTURED: single-pass draws `00000000`; every two-pass case
   `00000200` then `00000600`; the five-sampler schedule
   `00000200 00000400 00000400 00000400 00000600`.
2. **A producing pass's root went to the colour output `0x2d`** instead of the
   destination `encode_lops` computes, so it was packed into the render
   target's pixel format.  A shader whose root arrives through arithmetic
   worked all along; one whose root arrives through a plain move did not.
3. **Depth must be evaluated in the first raster pass only.**  The passes of a
   split shader are one fragment pipeline in sequence and those behind inherit
   the coverage.  Restating the test in a continuation pass stops the engine -
   which is why every *windowed* scene with a splitting shader stalled, since a
   window carries a depth attachment whether the scene tests against it or not.
4. **Index `88` bits 15:4 flag register pairs the program leaves unused, and a
   pair flagged unused is one the engine reuses.**  A four-unit pass reads a
   texel out of `0x0e`, which the backend was declaring free.  Reproduced with
   single-pass fixtures, no split involved: `texhi:4:0` wrong in 37822 of
   76800 pixels, every other unit exact.
5. **Three unjustified constants**, each replaced by the real bound rather than
   raised: a 128-relocation cap (the real bound is the command image's word
   count), a fixed batch relocation array, and a 32 MiB BO size cap applied to
   every memory class.  `M9_VRAM_GPU_SIZE` was deleted outright - it matched
   BAR0's length rather than any hardware limit, and four clamps compared
   engine addresses against the aperture instead of the memory.

6. **The engine keeps per-target state in the object bound at index `be`.**
   A split draw returning to an earlier render-target shape read the state a
   target of another shape had left there, and the engine stopped.  Fixed with
   one `be` object per render-target shape, allocated beside the depth and HZ
   pair.

   The decisive evidence was CAPTURED rather than reasoned: dumping the stream
   over `splitpair:84x84:320x240:3` shows the stalling third draw's two
   submissions are **byte for byte identical** to the first draw's - command
   dwords, relocations and BO list all equal.  Nothing the driver emitted
   separated the draw that renders from the draw that stops the engine, which
   is what turned the search towards state the engine carries between jobs.
   Against the rendering `88x88` case the only differing dword in the entire
   stream is the clip/scissor word.  Dumping `be` after each job then showed
   the engine writing a mask over the first 16 KiB whose extent is
   proportional to the target's area - 306 dwords for 84x84, 334 for 88x88,
   3149 for 320x240, always contiguous from offset 0.

   Two negatives worth keeping: one `be` per *colour resource* stalls where one
   per *shape* passes, so the engine wants sharing by shape rather than
   per-surface isolation; and clearing `be` every job passes at 3 draws and
   stalls at 20, which is exactly why the probes use 20 draws - a rare event
   must not be able to hide.

## What still stalled, and what it was

**Closed 2026-09-19.**  The scene-scale fault was the driver clearing the
engine's back-end state object with a CPU `memset` while a job that bound it
was still executing: a job posted asynchronously retires only at the *next*
submission (CAPTURED from the kernel), and the scratch path's seed copy is a
draw into the very surface whose object is cleared next.  A race, which is
why the count a context survived was never sharp and why every reproduction
attempt gave a different number.  A target shape now keeps two back-end
objects and a clear swaps to the spare
(`parhelia-m9-fragment-pass-hardware.md` 3bf).

MEASURED with `M9X_SPLIT_BE=3`, one binary, `M9X_BE_SWAP` the only variable:
`conv3rep` fails 76800 of 76800 pixels and stops the engine without the swap,
and passes three runs over with it.

`desktop` renders its blur at **38 fps** with nothing refused or skipped
(MEASURED 2026-09-19, one process, no stream timeout), so the row above that
said it stops the engine is retired.  `terrain` does not stop the engine
either -- it is **refused by the fragment backend**, which is a different
problem and is stated below.

### What the scene asks for, measured rather than assumed

`M9X_DEBUG_SPLIT` over `glmark2 -b desktop --size 1920x1200` names every split
draw the scene issues (CAPTURED, 2026-09-18):

```text
m9x record: 420x420   stride 12 ->   8 MiB, new object
m9x record: 1920x1200 stride 12 -> 106 MiB, new object
m9x record: 420x420   stride 12 wants   8 MiB, kept 106 MiB
m9x record: 1920x1200 stride 12 wants 106 MiB, kept 106 MiB
... twenty-two more, strictly alternating ...
m9x: two-pass job failed (Input/output error); context lost
```

So the scene is **twenty-six split draws alternating two target shapes**,
420x420 and 1920x1200, with an eleven-tap separable blur the backend cuts into
**four raster passes** - the stride the driver logs is `4 * (passes - 1)`.  It
is not the first split draw that stops the engine, and it is not the first
frame: twenty-six of them render first.

Two things this settles without another run.  The record is **not** refused any
more - 106 MiB is allocated and kept, so the thirteen refusals of the older log
are gone.  And the scene keeps exactly **two** live target shapes, so the
number of simultaneously live shapes is not a property of `desktop` at all.

### The two properties named as untested are both retired

Against a clean checkout of this tree built and deployed under its own prefix,
`GL_RENDERER` asserted `Matrox M9` on every run (MEASURED, 2026-09-18, twenty
draws a case):

| case | what it adds | result |
| --- | --- | --- |
| `splitpair:320x240:320x240:20:5` | two passes, one shape | renders |
| `splitpair:320x240:320x240:20:7` | two passes | renders |
| `splitpair:320x240:320x240:20:9` | three passes, stride 8 | renders |
| `splitpair:320x240:320x240:20:11` | **four passes**, stride 12 | renders |
| `splitpair:1920x1200:1920x1200:20:1` | **1920x1200**, no split | renders |
| `splitpair:1920x1200:1920x1200:20:5` | 1920x1200, two passes, 35 MiB record | renders |
| `splitpair:1920x1200:1920x1200:20:11` | 1920x1200, four passes, 106 MiB record | renders |

A split draw into a target far larger than 320x240 renders, the scene's own
pass count renders, and the two together render.  Each on its own is therefore
**not** what stops the scene.

The `be` object is not the size limit either.  Its mask extent is linear in the
target's area - 306 dwords at 84x84, 334 at 88x88, 3149 at 320x240, which fits
`area / 24.495 + 20` to within three dwords (DERIVED from the dumps).  That is
367 KiB at 1920x1200 against a 1 MiB `M9_BE_BYTES`, and the object would only
overrun above about 6.4 Mpx - a real latent limit, but not this one.

### What is left, and the fixture that reaches it

What the scene has that no case above has is the **combination**: alternating
shapes *and* four passes, at the scene's own sizes, with four colour resources
sharing the window shape and a plain fill between the split draws.  One further
property comes with the alternation - the scene's first split draw is the
*small* one, so the shared inter-pass record is allocated at 8 MiB and then
**replaced** at 106 MiB under jobs already submitted.

`tools/m9-mesa-glsl.c` now reaches every one of these, and the ladder to walk
in one boot is, in this order so that each control is reached before the step
expected to stall:

```text
splitpair:160x120:320x240:20:5     alternating shapes, two passes
splitpair:160x120:320x240:20:11    alternating shapes, four passes
splitpair:1920x1200:420x420:20:11  the scene's shapes, record allocated once
splitpair:420x420:1920x1200:20:11  the scene's shapes, record replaced
splitdesk:2:1:11:320x240           a plain fill between the split draws
splitdesk:2:4:11:320x240           four colour resources on one shape
splitdesk:3:4:11:1920x1200         the scene
```

`splitdesk:frames:windows:taps:WxH` is the desktop frame at any scale; at
`1920x1200` its windows are 420x420, the size the scene itself uses.

## Readings this retires

* `effect2d` at 640x480 rendering clean does **not** refute the 3x3 kernel
  stalling there: the default kernel is one tap and never splits.  Two agents
  reported those as contradictory results for the same scene.
* `refract` was reported as stalling.  It does not - it had simply never been
  measured, and completes with no timeout, no skipped draws and no lost
  context.
* `desktop`'s 37 fps was never the 4-pass path: its log showed the 106 MiB
  record refused thirteen times, so that number was the fallback.
* Fixing the `be` per-shape stall was expected to close `desktop` and
  `terrain`.  It did not - both still stop the engine, which is how the second
  scene-scale fault was found.
* `desktop` was recorded as faulting on the **first frame**.  It does not: the
  capture above shows twenty-six split draws rendering first.
* The number of simultaneously live render-target shapes was recorded as an
  untested scene-scale property.  It is not a scene property: `desktop` keeps
  two shapes, which `splitpair` already covers.
* A split draw into a target far larger than 320x240 was the other untested
  property.  It renders, at two and at four passes.
* `jellyfish` and `ideas` are 117 and 108 fps here; their 1 fps on the 481 MB
  host was host memory, not a driver defect.
