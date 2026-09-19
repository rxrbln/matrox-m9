# glmark2 on the m9x driver: where the 60 fps goal actually stands

## Standing 2026-09-19: the three scenes that were not drawing everything

MEASURED at 1920x1200 on the M9128, one X server and one process a benchmark,
`M9X_SKIP_REPORT=1`, **0 stream timeouts**, fixture suite **228 of 228 PASS**
before the sweep.  Each of the three had been reporting a frame rate for a
picture it did not draw.

| benchmark | before: fps, draws skipped a frame | after: fps, skipped | what it was |
|---|---|---|---|
| `shadow` | 145, **1447** | 111, **0** | a depth-only framebuffer derived no raster parameters |
| `jellyfish` | 125, **1245** | 77, **0** | the blend path took only registers the program never named |
| `loop` uniform=true | 345, **3446** | 159, **0** | the loop bound arrived as the float encoding of 5 |

The frame rates fall because the work is now being done.  `loop` at
159 fps sits beside the 167 fps of `loop:fragment-uniform=false`, which
is the same picture with the bound written as a literal, so the specialised
variant is doing the same five iterations.

### The pictures, not only the tallies

CAPTURED off scanout at 1920x1200, `xwd` of the root window 6 s into each run,
one X server a scene:

| scene | distinct colours | mean r,g,b |
|---|---:|---|
| `shadow` | 805 | 22.5, 22.5, 22.5 |
| `jellyfish` | 232951 | 68.8, 105.1, 199.9 |
| `loop` uniform=true | 1786 | 44.5, 44.5, 44.5 |

`shadow` is the one worth reading closely, because a frame rate and a skip
tally cannot tell an empty shadow map from a full one: the fragment shader
samples the map and multiplies the lit colour by **0.5** where the surface is
farther from the light than the stored depth, and with an empty map
(`light_distance` = 1.0) that branch is never taken.  The capture's luminance
histogram has exactly that pair -- **76 over 26.7% of pixels and 38 over
3.0%**, one being half the other, with 68% black background.  So the map
carries real depth and the shadow is drawn.

`jellyfish` is the blended scene, 232951 distinct colours over a blue
distribution centred near 160-165.  `loop` is the `fract` noise pattern, 256
luminance levels thinly spread over the grid with the background black.

Three fixtures were added with these fixes, because none of the 228 already
in the suite covered any of the three shapes: `depthonly` renders a shadow
map into a framebuffer with no colour attachment (the existing `depthtex`
always attached a colour texture as well), `blendheavy` blends a shader whose
body names the whole register pool, and `loopuni:0`, `:5` and `:24` draw a
loop over a uniform bound.  Each was checked against the build before its fix
as a negative control.


## Superseded: standing 2026-09-19 (merged): the whole thirty-three on one build

The two sweeps below it were measured on **different builds that were never
tested together** -- the `terrain` sweep without `refract`'s record-ceiling
fix (`22c0352`), the `refract` sweep without the resource splitter.  Both are
merged, and this is the hardware sweep of the union.  It is the only standing
to read as current; both of those are superseded.

MEASURED at 1920x1200 on the M9128, **one X server and one process a
benchmark** (`tools/m9x-glmark-default-suite-fresh-x.sh`),
`M9X_SKIP_REPORT=1`, all thirty-three completed, **0 stream timeouts**, with a
known-good fixture passing before and after each sweep.  Two columns: the
merged tree as it was, and the same tree carrying the index-88 fix that
closes `coordconv:4` (`docs/parhelia-m9-fragment-pass-splitting.md` 8.5).
Fixture suite: **224 of 224 PASS** on the merged tree and **228 of 228** with
the fix, which lists `coordconv:4`, `:5`, `:6` and `:4d` as well.

| benchmark | merged, fps | with the fix, fps | draws skipped a frame |
|---|---:|---:|---|
| `build` vbo=false / vbo=true | 273, 295 | 273, 294 | none |
| `texture` nearest / linear / mipmap | 284, 283, 283 | 284, 283, 283 | none |
| `shading` gouraud / blinn-phong-inf / phong / cel | 243, 149, 88, 83 | 243, 150, 88, 83 | none |
| `bump` high-poly / normals / height | 105, 182, 164 | 105, 182, 164 | none |
| `effect2d` 3x3 / 15-tap | 84, 33 | 84, 33 | none |
| `pulsar` | 220 | 220 | none |
| `desktop` blur / shadow | 37, 124 | 37, 124 | none |
| `buffer` map / subdata / interleaved | 153, 153, 154 | 153, 153, 154 | none |
| `ideas` | 110 | 110 | none |
| `jellyfish` | 125 | 125 | **1245**, out of temporaries for blending |
| **`terrain`** | **1** | **1** | **none** |
| `shadow` | 145 | 145 | **1447**, a depth-only framebuffer has no raster parameters |
| **`refract`** | **5** | **5** | none |
| `conditionals` 0/0, 0/5, 5/0 | 229, 142, 228 | 229, 142, 228 | none |
| `function` low / medium | 169, 72 | 168, 72 | none |
| `loop` no-loop / uniform=false / uniform=true | 167, 167, 345 | 167, 167, 345 | **3446** on uniform=true, control flow |

**Nothing regressed in the union.**  Every row of both sweeps matches both
earlier standings within run-to-run noise; `terrain` still skips nothing and
`refract` is still 5 fps.  The three scenes that skip draws skip them for the
fragment-backend reasons they did before, none of which is the split path.

`refract` is still **correct**: CAPTURED off scanout at 1920x1200, the
refracting bunny over black, 174602 distinct colours on the merged tree and
190375 with the fix -- the same range as the 181662 the llvmpipe reference
gave in the superseded standing below, which was not re-measured here
because llvmpipe drew no frame in ten seconds at either size.

`terrain` is **not**, and the fix moves it without closing it.  CAPTURED off
scanout at 1920x1200 and 800x500 with a known-good fixture passing after:
the distinct-colour count over the root window goes from **1297 to 104000**
and the frame from a flat near-white wash to a lit landscape with relief and
detail, but the scene is blue-dominant where the reference is brown and
green, and the near field carries a regular striped block pattern fixed in
screen pixels.  So the frame rate of 1 fps is still not a result for it, and
what remains has **no fixture yet**
(`docs/parhelia-m9-fragment-pass-splitting.md` 8.4).

## Superseded: standing 2026-09-19 (second), the resource splitter alone

The merged standing above replaces this; it was measured without `refract`'s
record-ceiling fix.  `terrain`'s picture is no longer what this says.

MEASURED at 1920x1200 on the M9128, `tools/m9x-glmark-default-suite-fresh-x.sh`,
`M9X_SKIP_REPORT=1`, 0 stream timeouts.

| scene | before | after |
|---|---|---|
| `terrain` | 1 fps, **24 draws a frame refused** by the fragment backend | 1 fps, **0 draws skipped**, mesh silhouette and sky on the monitor, body wrong |

What changed is the fragment splitter: it now partitions a shader by the
resources a raster pass holds -- texel slots, the register file and the six
interpolation registers -- rather than by the fetch count alone, and three
defects in front of that are fixed (the coordinate's varying location was
read from the wrong intrinsic, two units could not share a varying location
at an odd component, and the layout gate counted a coordinate stream a
sampler).  `docs/parhelia-m9-fragment-pass-splitting.md` section 8.

**`terrain` is not conformant and the frame rate is not a result.**  The
terrain body comes back with R and G exactly zero.  The pattern is fixed in
screen pixels -- the same block size at 1920x1200 and at 480x300 (MEASURED,
both captured) -- so it is the inter-pass record, not texture sampling.

The defect is isolated to a fixture that did not exist before:
**`coordconv:4` fails, 54092 of 76800 pixels, against a CPU reference**, while
`coordconv:2` and `coordconv:3` pass exactly.  It is glmark2's tilt-shift
blur in miniature -- N taps around a base coordinate the fragment stage
computes, one producing pass a tap -- and `terrain` presents through the
nine-tap form of it.  It is not the record slot count, not the pass count and
not the choice of cuts; each of those is ruled out with one variable in
section 8.5 of the splitting note.

**This very likely also explains `refract`'s black frame**, which is the same
family: four passes, three record slots, every coordinate computed.

### The whole list, re-run (MEASURED 2026-09-19, one X server a benchmark)

`tools/m9x-glmark-default-suite-fresh-x.sh`, 1920x1200, `M9X_SKIP_REPORT=1`,
**0 stream timeouts**, all thirty-three completed.

| benchmark | fps | draws skipped |
|---|---:|---|
| `build` vbo=false / vbo=true | 273, 296 | none |
| `texture` nearest / linear / mipmap | 284, 283, 283 | none |
| `shading` gouraud / blinn-phong-inf / phong / cel | 243, 149, 88, 83 | none |
| `bump` high-poly / normals / height | 105, 182, 164 | none |
| `effect2d` 3x3 / 15-tap | 84, 33 | none |
| `pulsar` | 221 | none |
| `desktop` blur / shadow | 37, 124 | none |
| `buffer` map / subdata / interleaved | 153, 153, 154 | none |
| `ideas` | 110 | none |
| `jellyfish` | 125 | 1244, out of temporaries for blending |
| **`terrain`** | **1** | **none** (was 24) |
| `shadow` | 145 | 1447, no valid raster parameters |
| `refract` | 39 | none |
| `conditionals` 0/0, 5/0, 0/5 | 229, 142, 228 | none |
| `function` low / medium | 168, 72 | none |
| `loop` no-loop / uniform=false / uniform=true | 167, 166, 345 | 3445 on uniform=true, control flow |

Every row matches the previous standing within run-to-run noise.  `terrain`
is the only change, and the three scenes that still skip draws skip them for
the reasons they did before.

## Superseded: standing 2026-09-19 (first)

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

## Superseded: standing 2026-09-19 (later), the record ceiling alone

The merged standing above replaces this; it was measured without the resource
splitter, which is why `terrain` still skips 24 draws here.

MEASURED at 1920x1200, **one X server and one process a benchmark**
(`tools/m9x-glmark-default-suite-fresh-x.sh`), `M9X_SKIP_REPORT=1`, the whole
thirty-three in one sweep, **0 stream timeouts**, no
`glCheckFramebufferStatus` failure and no lost context anywhere in the run.
Fixture suite alongside it: **221 cases, 221 PASS, 0 FAIL, 0 non-M9** against
the module carrying the 10-bit surface-window commits, so the four `giga10`
failures of the sweep below are closed as well.
The only row that moved is `refract`; every other benchmark is within a frame
a second of the sweep below, which is the control this is read against.

| benchmark | fps | draws skipped a frame |
|---|---:|---|
| `build` vbo=false / vbo=true | 270, 294 | none |
| `texture` nearest / linear / mipmap | 284, 283, 283 | none |
| `shading` gouraud / blinn-phong-inf / phong / cel | 242, 149, 88, 83 | none |
| `bump` high-poly / normals / height | 105, 182, 164 | none |
| `effect2d` 3x3 | 84 | none |
| **`effect2d` 15-tap** | **33** | none |
| `pulsar` | 220 | none |
| **`desktop` blur** | **37** | none |
| `desktop` shadow | 124 | none |
| `buffer` map / subdata / interleaved | 153, 153, 153 | none |
| `ideas` | 111 | none |
| `jellyfish` | 125 | **1244**, out of temporaries for blending |
| **`terrain`** | **1** | **24**, five samplers against four units and the register file |
| `shadow` | 145 | **1447**, a depth-only framebuffer has no raster parameters |
| **`refract`** | **5** | none - **and the picture is right** |
| `conditionals` 0/0, 0/5, 5/0 | 229, 142, 228 | none |
| `function` low / medium | 168, 72 | none |
| `loop` no-loop / uniform=false / uniform=true | 166, 166, 344 | **3437** on uniform=true, control flow |

Twenty-nine of the thirty-three are at or above 60 fps, as before; `refract`
was already below it at 39 and is further below now.  That is a real cost and
it is named in the backlog, but it buys the first correct frame this scene has
ever produced on the card.

### What `refract` was doing, and what it does now

**The black frame was never the split path.**  A split shader preprocesses one
copy of the geometry per raster pass, so `m9_job_reserve` refuses a run of more
than `ceiling/passes` source records - 2856 of the 11520 ceiling for a
four-pass shader.  Every producer sized its runs by the **whole** ceiling, and
on the refusal simply returned.  The draw module handed refract's main pass
over in runs of 11520, **every one of them was refused**, and the frame carried
the tail alone.

MEASURED with `M9X_SPLIT_TRACE=2`, which names every job and not only the split
ones - one frame of `refract`, in order:

```text
m9x job 33: 2048x1280 passes 0 draws 1 records 4      the distance target's clear
m9x job 34..51: 2048x1280 passes 1 draws 1 records 11520   the depth pass, 18 jobs
m9x job 52: 2048x1280 passes 1 draws 1 records 1640        its tail
m9x job 53..58: 1024x640 .. 32x20 passes 1 records 4       the generated mip chain
m9x job 59: 1920x1200 passes 4 draws 1 records 156         the main pass, ENTIRE
```

The depth pass draws all 208998 corners of `bunny.obj` (69666 triangles, one
record a corner); the main pass, over the same mesh, carried **156**.  52
triangles of 69666, which is a black screen.

This **predates the padded split path**.  The control was run: the 2026-09-18
deployment at `/root/parhelia-m9x-mesa`, built before `a47dcf5`, `4cd7d1d` and
`15dbebf`, renders `refract` at **1 distinct colour** in 2.3 M pixels while
reporting `draws skipped` for coverage.  So this is a scene that had **never
rendered**, not a regression from those three; the padded path removed the
coverage refusal and exposed the record loss underneath it.

`m9_job_max_vertices()` (`98be636`) is that ceiling in one place - the pass
count and the padding a partial draw needs - and `m9_job_reserve`, the draw
module's two append paths and the three native vertex paths all use it.  A run
that still does not fit is **tallied as a skipped draw**, so this class of loss
cannot be silent again.

CAPTURED off scanout with the fix, `xwd` of the root window at 5 s and again at
8 s of a 1920x1200 run, the known-good fixture rendering after it in the same
session:

| | distinct colours | mean r,g,b |
|---|---:|---|
| 2026-09-18 control, 5 s | 1 | 0, 0, 0 |
| HEAD before the fix, 5 s / 8 s | 15 / 4 | 0, 0, 0 |
| **with the fix, 5 s** | **223965** | 21.6, 18.1, 26.0 |
| with the fix, 8 s | 194363 | 17.8, 15.8, 22.9 |
| llvmpipe reference, same size | 181662 | - |

The picture is the refracting bunny over black, and it matches the software
reference in structure and colour; the distinct-colour ratio against it is
1.23.  Reproduced on the final binary (`22c0352`) after a fresh module load:
227978 at 5 s, 194812 at 8 s, fixture after it, 0 stream timeouts.

### The cost, and where it comes from

MEASURED, `M9X_PROFILE=20` at 1920x1200 with the fix:

```text
m9x: 20 frames 212.10 ms/frame (job vertices 11520): draw 194.11 emit 184.21
     submit 186.90 varying 16.21 records 4.04 ioctl 81.24 | jobs 130 draws 130
     records 418084 varying KiB 118982.6 refused 0 skipped 0
```

130 jobs a frame against 28 while it was dropping the geometry, 81 ms of submit
ioctl at 0.62 ms a job, and 116 MiB of CPU varying gather - the varyings are
repeated once per raster pass behind the first set, so a four-pass job pays for
its geometry four times.

The job count is forced by the hardware rule, not by a choice: a split job
carries exactly one raster draw (3c), and its records are capped at
`ceiling/passes`.  The only lever inside that rule is a larger record ceiling,
and the 16-bit index space caps `records * passes` at 65536, so 16384 records a
job and about 13 jobs for this mesh.  `M9X_JOB_VERTICES=65532` does **not**
reach it on this target: MEASURED, 0 jobs, 0 draws, nothing drawn and a stream
timeout, which is the 481 MB of system RAM rather than the rule.

## Superseded: standing 2026-09-19, before `refract`'s geometry was delivered
### All thirty-three run, twenty-nine above 60 fps, four still skip draws

MEASURED at 1920x1200, **one X server and one process a benchmark**
(`tools/m9x-glmark-default-suite-fresh-x.sh`, and see why below), the padded
split path on by default, `M9X_SKIP_REPORT=1`, 0 stream timeouts, fixture
suite 211 cases 207 PASS.  The four failures are the `giga10` cases and belong
to the **module**, not to this build: MEASURED, the same Mesa binary against a
kernel carrying the 10-bit surface-window commits passes `giga10tex`,
`giga10rt` and `giga10blend` exactly and leaves only `giga10clear` reporting a
channel difference.

| benchmark | fps | draws skipped a frame |
|---|---:|---|
| `build` vbo=false / vbo=true | 273, 294 | none |
| `texture` nearest / linear / mipmap | 284, 283, 283 | none |
| `shading` gouraud / blinn-phong-inf / phong / cel | 243, 149, 88, 83 | none |
| `bump` high-poly / normals / height | 105, 182, 164 | none |
| `effect2d` 3x3 | 84 | none |
| **`effect2d` 15-tap** | **33** | none |
| `pulsar` | 220 | none |
| **`desktop` blur** | **37** | none |
| `desktop` shadow | 124 | none |
| `buffer` map / subdata / interleaved | 153, 154, 154 | none |
| `ideas` | 111 | none |
| `jellyfish` | 125 | **1244**, out of temporaries for blending |
| **`terrain`** | **1** | **24**, five samplers against four units and the register file -- **0 as of the second standing above** |
| `shadow` | 145 | **1446**, a depth-only framebuffer has no raster parameters |
| **`refract`** | **39** | none |
| `conditionals` 0/0, 0/5, 5/0 | 229, 142, 228 | none |
| `function` low / medium | 168, 72 | none |
| `loop` no-loop / uniform=false / uniform=true | 166, 167, 345 | **3444** on uniform=true, control flow |

Twenty-nine of the thirty-three are at or above 60 fps and twenty-nine draw
every draw they are given.  The four that skip draws are `jellyfish`,
`terrain`, `shadow` and `loop:fragment-uniform=true`, each for a **fragment
backend** reason rather than anything in the split path.

**A frame rate with skipped draws is not a result.**  Two of these were only
found because `M9X_SKIP_REPORT` was on: `shadow` reported 295 fps while
skipping 2942 draws a frame and `loop:fragment-uniform=true` reports 345 fps
while skipping 3444.  Both look like the fastest scenes in the list.

### What changed on 2026-09-19

| scene | before | after | why |
|---|---|---|---|
| `shadow` | 295 fps, 2942 draws a frame skipped | 145 fps, none for coverage | the padded split path carries a multi-pass draw whose geometry does not fill the box it spans.  CAPTURED on scanout at the same point of the run: the same 1786 distinct colours at **twice the mean luminance**, 11.2 to 22.2, which is the pass it was dropping |
| `refract` | 41 fps, 403 draws a frame skipped | 39 fps, none | the same |
| `desktop` | stops the engine inside the full list, taking the seventeen benchmarks behind it down | 37 fps, whole list completes | the back-end object is swapped rather than cleared under a live job |
| the list | ends at benchmark 16 | all 33 complete, 0 stream timeouts | the same |

The `desktop` row is not a reconstruction: the **control was run**.  Against
the build of 2026-09-18 on the same target, same harness and same module,
`desktop` at position 16 reports `two-pass job failed (Input/output error);
context lost` and 6536 fps, and benchmarks 17 to 33 all exit rc=1.

### Two readings this retires

* **`refract` is not conformant** even now.  It skips no draws and reports
  39 fps, and the frame on the monitor is **black** - 16 distinct colours in
  2.3 million pixels, mean 0, CAPTURED at 5 s and again at 8 s, with the same
  capture showing `pulsar` at 88075 distinct colours and `texture` at 15096.
  So "0 skipped draws" is not conformance, and the ledger's "refract is
  conformant" is withdrawn.  *(Closed below: the geometry was being dropped
  by a producer that sized its runs by the whole record ceiling rather than
  by a split job's share of it.)*
* **`shadow` was never 143-147 fps for a complete picture.**  That number is
  what it reaches now, drawing the pass it used to skip; the 295 fps it
  reported before was the scene without it.

## Superseded: standing 2026-09-18

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
**Closed 2026-09-19**: a fresh rebuild of `m9x` HEAD reproduced 0 skipped and
101.6 KiB exactly, so that reading stands and the 0.3 KiB one was the older
build.  Both, however, describe a frame that drew 156 of 208998 records; with
the ceiling respected the figure is 118982 KiB (see the standing section).

**The depth attachment is not what refuses them, and carrying the
destination's depth box into the scratch surface would restore nothing**
(MEASURED, `M9X_SKIP_REPORT` over the whole default list): with the geometry
asked about first, *no* draw anywhere in the thirty-three is refused for
depth.  A scratch surface is the size of the box the geometry spans, so it
only helps a draw that **fills** that box -- a rectangle does, a mesh does
not.  `refract`'s missing draw is its bunny under a four-pass shader and
`terrain`'s is its mesh under seven, and no target of any size can be covered
by either, so the multi-pass schedule cannot carry them at all.  *(Superseded
for `refract`: the padded path does carry it - the geometry reaches the
target's extent through four thin rectangles while the clip window keeps the
pixels it covers - and what was left of the scene after that was a producer
dropping the runs that did not fit a split job's record ceiling.  See the
standing section at the top.)*  A single-pass
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

## One X server for the whole list is not an honest sweep

**MEASURED 2026-09-19.**  The X server keeps **three full-screen buffers of
every GL client that has exited**, and releases them only when X itself does.
Read straight off the VRAM manager (`/sys/kernel/debug/dri/*/vram-mm`) with
one X session and five identical `build` clients in a row:

```text
before X            used 11754  free 45590
X up, no client     used 17572  free 39772
after client 1      used 24413  free 32931
after client 2      used 31253  free 26091
after client 3      used 38093  free 19251
after client 4      used 44933  free 12411
after client 5      used 51773  free  5571
after X exit        used 11754  free 45590
```

6840 pages a client, exactly three 1920x1200x4 surfaces, and every one of them
comes back when X goes.  So it is the **server**, not the kernel and not a
client leak: a client's DRI3 buffer chain outlives it.

The consequence is a measurement trap, not only a defect.  By the sixteenth
benchmark of the default list there is no room for a 1920x1200 render target,
`desktop`'s blur targets are refused with `glCheckFramebufferStatus` =
`GL_FRAMEBUFFER_UNSUPPORTED`, and glmark2 reports **96 fps for a picture it
did not draw** - with no skipped draw, no stream timeout and no lost context,
so the parser cannot catch it.  Reproduced on demand: fifteen `build` clients
and then `desktop` gives 96 fps; `desktop` first in the same session gives 38.
It does not depend on what the fifteen clients were.

`tools/m9x-glmark-default-suite-fresh-x.sh` starts **one X server per
benchmark** for this reason, and is what the numbers here are measured with.
`tools/m9x-glmark-default-suite.sh` runs inside one session and is only safe
for a short list.

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
  context.  It was, however, drawing 156 of its 208998 records until
  `98be636`, so "completes with no skipped draws" was true and still meant a
  black screen.
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
