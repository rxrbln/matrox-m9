# What the M9128 does with a split fragment shader

`docs/parhelia-m9-fragment-pass-splitting.md` and
`docs/parhelia-m9-fragment-pass-routes.md` decode the vendor's multi-pass
fragment schedule from saved captures.  This note records what happened when
that schedule was emitted at the M9128 for the first time, through the m9x
Gallium driver's own pass splitter.

Labels: **CAPTURED** read from a saved vendor stream, **MEASURED** read back
from the hardware, **OFFLINE** produced by the driver's own compiler without a
device, **DERIVED** a join, **OPEN** not decided.

## 1. The result: the record binds, once the pass says where it sits

**A raster pass may bind the inter-pass record as its render target.  What the
engine needs beyond the record descriptor is index `ef` bits 10:9, which place
the pass inside its multi-pass sequence** (MEASURED).  With that field emitted
the producing pass runs with the captured producer descriptor `b1 = ee190004`,
the captured record `89 = fe007ffc`, the record coverage words and the routes,
a two-pass schedule runs end to end without stopping the engine, and fixture
`tex5` -- five texture fetches the driver's own splitter cuts into two raster
passes -- renders pixel-exact.

The field is uniform across both independent capture sets (CAPTURED, the
raster range only; the preprocessing range carries its own unrelated `ef`):

| schedule | `ef` per raster pass |
|---|---|
| every single-pass draw (`fs-color`, `fs-add`, `fs-abs`) | `00000000` |
| all seven two-pass cases | `00000200` then `00000600` |
| the five-sampler schedule | `00000200`, `00000400`, `00000400`, `00000400`, `00000600` |

so bits 10:9 hold the pass's place: **0 the only pass, 1 the first of several,
2 a middle one, 3 the last** (DERIVED, and the only reading that fits both a
two-pass and a five-pass schedule).  m9x emitted zero there, which told the
engine every pass was a complete draw on its own.

This **refutes** the earlier conclusion of this note, that the record
descriptor's `0x77` format field was what the engine would not take as a
render target.  Two markers tell the engine a pass writes a record -- the
`0x77` descriptor at `b1`, and the render format class `4` that index `89`
carries for target 0 -- and while `ef` left the pass unplaced, **either one
alone stopped the engine** (MEASURED, section 2).  Neither is a blocker; both
are consistent descriptions of a mode the pass had not been placed into.

Index `89` is not a bitfield of enables: `docs/parhelia-m9-mrt-retirement-re.md`
records it as one three-bit render format class per bound target at bit
`3*s`.  The record's class is `4` and an ordinary colour target's is `0`, so
the `fe007ffc`/`fe007ff8` difference the captures show is that class, not a
record-enable bit.  Bit 21, where m9x's own colour value `fe207ff8` differs
from the vendor's `fe007ff8`, plays no part: `fe207ffc` stops the engine
exactly as `fe007ffc` did (MEASURED).

## 2. The ladder

All rows: M9128 at PCI 01:00.0, kernel 7.2.5-t2 at `f930663`, the m9x Gallium
driver with its pass splitter, fixture `tools/m9-mesa-glsl` case `texaff` at
320x240 with one draw, forced into two raster passes with
`M9X_FS_FORCE_SPLIT=2` so that each producing pass binds one texture -- the
shape of the captured five-sampler schedule.  **Every row was measured on a
freshly booted target whose engine had first been proved alive by
`m9-mesa-glsl texture`**, for the reason section 3 gives.

With `ef` left at zero, as the driver emitted it until now:

| configuration | outcome |
|---|---|
| one raster pass through the multi-pass emitter (`M9X_PASS_LOOP=1`) | pixel-exact PASS |
| producing pass alone, colour target at `b0` (`M9X_SPLIT_STAGE=1`) | no stall |
| record object at `b0`, colour descriptor, colour-pass `89`/`a9`/`ae` (stage 7) | **no stall** |
| record object at `b0`, colour descriptor, record `89` = `fe007ffc` (stage 4) | stall |
| the same, with the plain coverage words (`M9X_SPLIT_COVERAGE=0`) | stall |
| the same, with bit 21 kept set (`M9X_RECORD_89=0xfe207ffc`) | stall |
| record descriptor `b1 = ee190004`, colour class in `89` (`M9X_RECORD_89=0xfe207ff8`) | stall |
| record descriptor `b1 = ee190004`, record `89`, the real schedule (stage 3) | stall |

and with `ef` bits 10:9 emitted:

| configuration | outcome |
|---|---|
| producing pass alone, the real schedule (stage 3, `M9X_SPLIT_PASSES=1`) | **no stall** |
| both passes, `texaff` and `vary2tex` forced into two passes | **no stall**, values wrong |
| the whole fragment fixture suite with the schedule off | 97 PASS, 0 FAIL |

Two rows retire earlier readings of this note:

* **the placement hypothesis is ruled out.**  Stage 7 binds the record object
  at `b0` and changes nothing else, and it renders; where the allocator put
  the object relative to the colour target does not matter.  The captures say
  the same: the vendor's record at `0x10ec1000` is the context's *primary
  temporary resource*, 4 MiB from `TempBufferSize` (`docs/parhelia-m9-be-backing-re.md`),
  with the `be` resource at `0x112c1000` exactly 4 MiB above it and the first
  transient at `0x113c1000` (CAPTURED).  Its extent crosses the 16 MiB
  boundary at `0x11000000`, so the object does not sit inside one 16 MiB
  window at all and no window rule of the 0528's shape can be read from it.
* **the old "record object at `b0` with an ordinary colour descriptor runs"
  row was stale.**  It predates the commit that started emitting the captured
  record `89`; re-measured at HEAD that configuration carries class `4` and
  stalls.  The isolation it supported -- that `b1` alone decided -- was
  therefore not single-variable.

## 3. Method, and what a lost measurement looks like

**A stream timeout leaves the device in a state a module reload does not
clear.**  The reload succeeds, but `m9x_requester` finds `0x400`/`0x404`
mismatched, refuses bus mastering with `-EBUSY`, and every later job returns
"Device or resource busy; context lost" without reaching the engine.  A run in
that state renders nothing and logs no timeout, which reads exactly like a
configuration that runs cleanly.  Reboot the target between a stall and the
next measurement, and open every session with a known-good fixture.

The `e6`/`44` inter-draw transition, which this note once put first, is
decoded (`docs/parhelia-m9-fragment-pass-splitting.md` section 6.1), accepted
by `m9_parse_commands` and emitted around every raster draw of a split job.
It is neither the cause nor the cure: a pass loop emitting one pass renders
pixel-exact with it, and a producing pass stopped the engine with it while
`ef` was zero (MEASURED).

## 3a. What the hand-off delivers

A shader that genuinely needs two passes hands its values over correctly:
fixture `tex5`, five texture fetches cut into two raster passes by the
driver's own splitter, renders **0 of 76800 pixels wrong** (MEASURED).  The
schedule is therefore on by default and `tex5` is a listed fixture.

`M9X_FS_FORCE_SPLIT=2` is a different thing and its results do not carry:
forcing a one-fetch shader such as `texaff` or `vary2tex` into two passes
completes without stopping the engine but draws a wrong image, because the cut
that path takes is arbitrary rather than one the partitioner would choose.
Use it to exercise the emitter, not to judge the hand-off.

### 3b. The record's granularity, measured

**The record is a flat array the engine indexes by its own fragment counter
inside a raster range, one slot a shaded fragment** (MEASURED).  It is not
per 2x2 quad and not per shaded sample, and it does not follow the target's
area.

`tools/m9-record-probe.c` renders a five-fetch shader -- which the driver
splits in two -- over a rectangle of a 320x240 target, sampling a 1:1 NEAREST
texture whose texel `(x, y)` carries its own coordinates, and dumps the record
through `M9X_RECORD_DUMP`:

| covered rectangle | fragments | highest record index written |
|---|---:|---:|
| 320x240, the whole target | 76800 | 77431 |
| 160x120, a quarter of it | 19200 | 19495 |

Both rows are the same 320x240 target and the same 81920-slot record, so a
screen-position index is ruled out: a quarter-coverage draw indexed by
`y * width + x` would have reached 38239 and would have left the record
sparse, and instead it filled a dense run ending at 19495.  The count runs
about 1.5% past the covered pixels, which is the quads a shared triangle edge
shades twice.

So the object costs `stride * (fragments a pass shades)` and there is no
granularity to exploit: rounding to whole quads with a measured edge margin
replaces the 64-pixel alignment the driver guessed at, which is about 5%
smaller and no longer accidental.

**What actually refuses the record is the driver's VRAM pool, not its size**
(MEASURED).  `effect2d`'s 3x3 kernel asks for **35 MiB at 1920x1200 with a
stride of 4** and is refused on a card with `0x40000000` bytes -- 1 GiB -- of
VRAM, because `drmm_vram_helper_init` gives TTM only
`min(BAR0, M9_ADDRESS_CPU_BYTES)` = **224 MiB**, the identity-mapped part of
BAR0, which the scanout surfaces, framebuffers, textures and render targets
already occupy.  Sharing one record across the screen instead of one a
context does not help, because a single scene cannot get its first one.
Reaching the rest of the card means allocating VRAM through the surface
window at `[224M, 256M)` rather than the identity view, which is a
VRAM-addressing change and not a record change.  The producers of
every glmark2 shader the splitter cuts hand over four components, so the
four-dword slot is not padding either -- a four-pass shader at 1920x1200
genuinely needs about 106 MiB, and where VRAM cannot hold it the draws are
still skipped with a named reason.

### 3c. A split job carries one raster draw

**Two restated raster draws in one split job stop the engine** (MEASURED,
reproduced twice from a freshly booted target with
`m9-record-probe 320 240 160 120 2`, which draws the same rectangle twice;
the single-draw run of the same probe renders and dumps its record).  The
driver now closes a split job after one draw, which also bounds the record:
its fragment counter then covers a single draw rather than a whole job, so
the size above is an upper bound rather than a hope.  `M9X_SPLIT_DRAWS`
raises the limit for re-measurement.

## 4. What the driver does today

The compiler-side splitter is implemented and gated off.  `m9_fs_compile_passes`
partitions a shader the engine cannot run in one pass by cutting it at a
single value: the producing pass computes that value and hands it on, the
consuming pass reads it as an extra texel, and the cut is chosen to absorb as
many fetches as a pass has units.  The partition is checked by trial
compilation, so a cut that the backend cannot encode is rejected and the next
one tried.

Pass counts it produces for the glmark2 shaders (OFFLINE, the shaders rebuilt
from `src/scene-effect-2d.cpp` and `src/scene-desktop.cpp` and compiled with
`m9-fs-compile-test --glsl`):

| shader | fetches | passes |
|---|---:|---:|
| `effect2d` 3x3 edge kernel | 5 | 2 |
| `effect2d` 5x3 kernel | 15 | 5 |
| `desktop` separable blur, radius 5 | 11 | 4 |

which is the `4 + 3(P-1)` the pass-splitting note predicts.

With the relocation budget bounded by the command image rather than by a
constant, the two scenes it refused now render (MEASURED, 1920x1200, no
stream timeout):

| scene | passes | fps | outcome |
|---|---:|---:|---|
| `effect2d` default | 2 | 222 | renders |
| `desktop` separable blur | 4 | 35 | renders, where it was refused before |

`desktop`'s run still logs one "cannot allocate the inter-pass record", which
section 3b explains: at four passes and a large target the object is over
100 MiB and the honest answer is that the hardware's record costs that.

A glmark2 sweep at 1920x1200 with the schedule off reports 33 scenes, 25 of
them at or above 60 fps (MEASURED).  `terrain`, `shadow` and `refract` are
refused for an unrelated reason, a texture coordinate that is not a varying.

With the sequence field emitted, the six scenes this note listed as blocked
sweep at 1920x1200 as follows, with **no stream timeout anywhere**
(MEASURED):

| scene | passes | fps | outcome |
|---|---:|---:|---|
| `effect2d` default | 2 | 222 | draws, where it was refused before |
| `effect2d` 3x3 Laplacian | 2 | 346 | draws, but see the record size below |
| `effect2d` 5x3 | 5 | -- | refused: the relocation budget |
| `desktop` separable blur | 4 | -- | refused: the relocation budget |
| `terrain`, `shadow`, `refract` | -- | -- | refused: a texture coordinate that is not a varying |

Two limits stand between that and the rest, and neither is the record:

* **the relocation budget of one submission.**  A shader beyond three raster
  passes needs more relocation sites than `M9_MAX_RELOCS` carries, so the
  variant is refused at creation.  That is what now stops `desktop` and
  `effect2d`'s 5x3 kernel.
* **the record's size.**  The driver sizes it for one record per covered
  pixel, which is 37 MiB for a two-pass shader at 1920x1200, and a second
  context asking for one while the first still holds its own failed to
  allocate during the sweep ("cannot allocate the inter-pass record"), so the
  Laplacian scene's draws were skipped for part of its run and its 346 fps is
  not a clean number.  The granularity that would shrink it -- one record per
  fragment, per 2x2 quad or per shaded sample -- is **OPEN**.

The schedule is on by default, `M9X_SPLIT=0` refuses it instead, and the
fragment fixture suite holds at 97 PASS, 0 FAIL with either setting; `tex5`
joins the listed cases and raises the bar to 98 PASS, 0 FAIL (MEASURED).  Nothing in
the Gallium caps advertises the feature: `fs->max_texture_samplers` was already
8 rather than the four-unit binding limit, so no cap claimed or claims that a
five-fetch shader renders.

## 6. A coordinate the fragment stage computes

The engine samples before the program runs, so a coordinate computed in the
fragment stage cannot feed a fetch in the same pass -- which is what refuses
`terrain`, `shadow` and `refract` with "texture coordinate is not a varying".
It can feed one in the **next** pass, through a coordinate route, and
`docs/parhelia-m9-fragment-pass-routes.md` section 4 decodes that from the
captures.  m9x emits one behind `M9X_COORD_ROUTE`; it is **off by default**.

Where it stands (MEASURED, fixture `coordroute`, which samples at the product
of two varyings so that no affine fold can reach it):

| step | outcome |
|---|---|
| the splitter cuts at the computed coordinate | `shadow.frag` becomes two passes, `light-refract.frag` four (OFFLINE) |
| the consuming unit keeps the interpolated selector word `00040118` | **stops the engine** |
| the routed selector word `0004111c` | **no stall**, the schedule runs |
| the producing pass writes the coordinate | **correct**: read back with `M9X_RECORD_DUMP`, the first fragment holds `s = 0.001561` and `t = 0.002081`, exactly `(0.5/320, 0.5/240)` |
| the consuming unit samples there | **wrong place**, 76794 of 76800 pixels differ |

So the schedule, the selector word, the route word and the producer are all
right and what remains is the record's lane layout on the consuming side.
Two readings were tried and both are retired: narrowing a one-pair producer
to the two dwords every captured one-pair producer uses (`ee090002`) **stops
the engine** and also breaks `tex5`, which the four-dword slot renders
pixel-exact; and the consumer's own descriptor already matches `xv-uyvy`'s
`ee090004` byte for byte.

Because it renders rather than stalls, leaving it on would turn three scenes
from a refusal with a named reason into silently wrong pixels, so the
compiler refuses a routed coordinate unless `M9X_COORD_ROUTE` is set and the
fixture is dispatched but not listed.

`terrain` is assessable only at runtime -- the offline harness's synthetic
vertex shader will not link -- and at 1920x1200 it is refused five times over
for the coordinate, and separately needs a D24S8 depth attachment of the
target size (MEASURED).

## 5. Reproduction

```sh
# on the target, inside flock -w 1800 /root/tmp/m9-hw.lock
cd /root/parhelia-m9x-mesa/src && gcc -O2 -o m9-mesa-glsl m9-mesa-glsl.c -lEGL -lGL -lm
export LD_LIBRARY_PATH=/root/parhelia-m9x-mesa/lib:/root/parhelia-m9x-mesa/dri
export LIBGL_DRIVERS_PATH=/root/parhelia-m9x-mesa/dri MESA_LOADER_DRIVER_OVERRIDE=m9x

# the emitter, one pass at a time: every one of these is pixel-exact
M9X_PASS_LOOP=1 ./m9-mesa-glsl tex4

# the producing pass alone, with the record descriptor: runs
M9X_SPLIT=1 M9X_FS_FORCE_SPLIT=2 M9X_SPLIT_PASSES=1 ./m9-mesa-glsl texaff
# both passes: runs, with the wrong values
M9X_SPLIT=1 M9X_FS_FORCE_SPLIT=2 ./m9-mesa-glsl texaff

# the five-fetch fixture
M9X_SPLIT=1 ./m9-mesa-glsl tex5
```

`M9X_SPLIT_STAGE` selects which binding a split job makes: 1 leaves the record
unbound, 2 binds it with the routes idle, 3 is the real schedule, 4 binds the
record object with a colour descriptor, 5 with the captured two-dword producer
descriptor, 6 with the record format over the colour target's row span, and 7
with the colour descriptor and the colour pass's own state words, which leaves
the bound object as the only difference from an ordinary draw.
`M9X_RECORD_88` replaces the derived index-`0x88` low byte of a record pass,
`M9X_RECORD_89` replaces the whole of index `0x89`, and
`M9X_SPLIT_COVERAGE=0` keeps its plain coverage words, so each can be ruled out
on its own.  `M9X_TRANSITION=0` drops the engine transition of section 3 from a
split job, which separates a fault in that block from one in the record;
`M9X_SPLIT_SUBMIT=0` puts every pass of a split shader in one submission, in
one raster range, which is the vendor's own layout.

To reproduce the stall the sequence field cured, build with
`pass->sequence_ef` cleared; the first ladder of section 2 then returns.

<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
