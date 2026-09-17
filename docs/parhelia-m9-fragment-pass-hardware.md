# What the M9128 does with a split fragment shader

`docs/parhelia-m9-fragment-pass-splitting.md` and
`docs/parhelia-m9-fragment-pass-routes.md` decode the vendor's multi-pass
fragment schedule from saved captures.  This note records what happened when
that schedule was emitted at the M9128 for the first time, through the m9x
Gallium driver's own pass splitter.

Labels: **MEASURED** read back from the hardware, **OFFLINE** produced by the
driver's own compiler without a device, **DERIVED** a join, **OPEN** not
decided.

## 1. The result

**A raster pass that binds the inter-pass record as its render target stalls
the engine, and the descriptor at index `b1` is the only thing that has to
change for it to stall** (MEASURED).  Everything else a producing pass does
runs: a fragment program that writes the output register pairs `0x11`/`0x13`
and never the colour output `0x2d`, the record object itself bound at `b0`,
the record pass's coverage words, and its index-`0x88`.

The isolation is single-variable.  The same job, the same program, the same
object at `b0`, differing only in `b1`:

| `b1` | outcome |
|---|---|
| the colour target's own descriptor, `cc210140` | runs, no stall (MEASURED) |
| `ee190004`, the producer descriptor of all seven captured two-pass cases | `M9 kernel stream timeout` (MEASURED) |
| `ee190140`, the same record format over the colour target's row span | `M9 kernel stream timeout` (MEASURED) |

So it is the descriptor's format field -- `0x77` in bits 31:25, the code
`docs/parhelia-m9-five-sampler-schedule-re.md` records as "an internal code
not present among the ordinary public render formats" -- that the engine will
not take as a render target in this state, not the record's address, its row
span, its stride, or the program that fills it.

Two further candidates were tested against the hardware and are ruled out
(MEASURED):

* **index `0x89` bit 2.**  Every captured pass that writes a record carries
  `89 = fe007ffc` and every pass that writes the colour carries `fe007ff8`,
  so bit 2 looked like the record enable.  Emitting the captured value
  verbatim changes nothing: the pass still stalls.  The driver emits it
  anyway, because it is what the captures hold.
* **index `0xc0`.**  It is outside the kernel's `m9_index_allowed` (which
  runs `0xc1 ... 0xc5`), so a job carrying it is refused with `-EPERM`; with
  the kernel's allow-list widened by one case to accept it, the job reaches
  the engine and stalls exactly as before.  The kernel change was reverted,
  having bought nothing.  (The reason it was tried is wrong, and worth
  correcting: `c0 = 0` is **not** particular to a record pass.  Re-decoding
  the whole GLSL capture set counts 82 writes of it behind an ordinary
  `cc210080` colour target against eight behind a record descriptor, and the
  two raster draws of every two-pass case both carry it -- CAPTURED.)
* **the `e6`/`44` inter-draw transition.**  Decoded in
  `docs/parhelia-m9-fragment-pass-splitting.md` section 6.1, accepted by a
  widened `m9_parse_commands` and emitted around every raster draw of a
  split job.  A one-pass shader driven through the multi-pass loop with the
  block renders pixel-exact on all four fixtures, so the engine takes it; a
  producing pass with the block and `b1 = ee190004` stops the engine exactly
  as it did without it (MEASURED, section 2).

The conclusion is that **binding the record needs something this tree has not
decoded yet**, and it is not the engine transition: that was the last part of
the captured schedule a clean-room driver could not reproduce, and
reproducing it changes nothing (section 3).

## 2. The ladder

All rows: M9128 at PCI 01:00.0, kernel 7.2.5-t2, the m9x Gallium driver with
its pass splitter, fixture `tools/m9-mesa-glsl` case `texaff` at 320x240 with
one draw, forced into two raster passes with `M9X_FS_FORCE_SPLIT=2` so that
each producing pass binds one texture -- the shape of the captured
five-sampler schedule.  "stall" is `M9 kernel stream timeout` in the kernel
log, after which the device needs a reboot.

| configuration | outcome |
|---|---|
| one raster pass driven through the multi-pass emitter (`M9X_PASS_LOOP=1`): `texture`, `texaff`, `tex4`, `vary2tex` | pixel-exact PASS |
| producing pass alone, colour target at `b0`, program writes `0x11`/`0x13` and never `0x2d` | no stall |
| producing pass alone, record object at `b0` with an ordinary colour descriptor | no stall |
| producing pass alone, record object at `b0` with `b1 = ee190004` | stall |
| the same, with the plain index-`0x88` and the plain coverage words | stall |
| the same, with `b1 = ee190140` | stall |
| the same, with index `0x89` = the captured `fe007ffc` | stall |
| the same, with index `0xc0` cleared and the kernel widened to accept it | stall |
| the same, with the vendor's `e6`/`44` transition around the draw | stall |
| one pass through the multi-pass emitter **with** that transition: `texture`, `texaff`, `tex4`, `vary2tex` | pixel-exact PASS |
| both passes in one submission, one raster range, with that transition | stall |

The first row matters as much as the rest: it says the multi-pass emitter
itself is right.  A pass loop that emits one pass produces a job the hardware
renders exactly, for an interpolated-varying shader, a four-texture shader and
a shader with two varyings and a texture alike.

A note on method, because two measurement sessions were lost to it: **a
stream timeout leaves the device in a state a module reload does not clear.**
The reload succeeds, but `m9x_requester` finds `0x400`/`0x404` mismatched,
refuses bus mastering with `-EBUSY`, and every later job returns "Device or
resource busy; context lost" without reaching the engine.  A run in that
state renders nothing and logs no timeout, which reads exactly like a
configuration that runs cleanly.  Reboot the target between a stall and the
next measurement, and check that a known-good fixture renders before
believing a negative.

## 3. The engine transition, and what it does not explain

The block is now decoded (`docs/parhelia-m9-fragment-pass-splitting.md`
section 6.1), accepted by `m9_parse_commands`, and emitted around every
raster draw of a split job.  Two measurements settle it:

* one pass through the multi-pass emitter **with** the block renders
  pixel-exact on `texture`, `texaff`, `tex4` and `vary2tex` -- so the kernel
  passes it and the engine takes it (MEASURED);
* the producing pass alone, on a freshly booted target, with the block and
  with `b1 = ee190004`, stops the engine with the same
  `M9 kernel stream timeout` as before (MEASURED).

**So the transition is ruled out.**  It was the last part of the captured
schedule this tree could not reproduce, which is why it was the first
hypothesis; it is not the answer.

What is left untried, in the order the evidence supports:

1. **A capture of the vendor rendering a five-fetch shader on this M9128.**
   Every two-pass capture in the tree is from a different board at a
   different resolution, so the comparison carries two unknowns at once.  A
   same-device, same-resolution capture would let the producing pass be
   diffed word for word instead of reasoned about.
2. **Where the record is allowed to live.**  In every captured case the
   record is the only object below the 16 MiB boundary the rest of the job
   sits above -- record `0x10ec1000`, colour `0x11f09000`, depth
   `0x11f21000`, aux `0x11f39000`, streams and programs `0x113x/0x11f4`
   (CAPTURED).  The 0528 in this same family has a rule of that shape
   already measured -- its depth surface has to sit in the 16 MiB engine
   window above its colour target, or the two buffers mix -- so a placement
   rule on the M9's record is not far-fetched.  The m9x allocator places the
   record wherever it lands, which for the fixture is the same window as the
   colour target.  The regularity may equally be an accident of the vendor's
   allocator; it has never been tested.
3. **Indices `82..85`.**  The vendor writes `82 = 83 = 84 = 85 = 0` behind
   the texture block of 89 of the captured GLSL cases, record passes and
   ordinary colour draws alike (CAPTURED); m9x emits them only for the
   placeholder unit, so a record pass that binds a real texture leaves them
   at whatever the context held.  Cheap to test, weak prior: the group is
   not particular to a record pass, and m9x's own textured single-pass draws
   omit it and render.
4. **The record's own extent.**  `ee190004` carries a four-dword record
   stride and no row span, so how the engine turns a fragment position into
   a record address is undecided.  The consumer samples the record at the
   same position, so the addressing must be positional, which means a pitch
   comes from somewhere this tree has not found -- neither `b1`, nor `be`,
   nor the clip window has been shown to supply it.

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

A glmark2 sweep at 1920x1200 with the schedule off reports 33 scenes, 25 of
them at or above 60 fps (MEASURED).  The three the splitter would reach --
both `effect2d` kernels and `desktop`'s separable blur -- report a frame rate
but are counted invalid, because their draws are skipped: the shaders are
refused rather than drawn.  `terrain`, `shadow` and `refract` are refused for
an unrelated reason, a texture coordinate that is not a varying.

Because a record pass stalls the engine, the schedule is behind `M9X_SPLIT=1`
and a shader that needs it is refused with a reason rather than drawn, so the
fragment fixture suite is unchanged at 90 PASS, 0 FAIL (MEASURED).  Nothing in
the Gallium caps advertises the feature: `fs->max_texture_samplers` was already
8 rather than the four-unit binding limit, so no cap claimed or claims that a
five-fetch shader renders.

## 5. Reproduction

```sh
# on the target, inside flock -w 1800 /root/tmp/m9-hw.lock
cd /root/parhelia-m9x-mesa/src && gcc -O2 -o m9-mesa-glsl m9-mesa-glsl.c -lEGL -lGL -lm
export LD_LIBRARY_PATH=/root/parhelia-m9x-mesa/lib:/root/parhelia-m9x-mesa/dri
export LIBGL_DRIVERS_PATH=/root/parhelia-m9x-mesa/dri MESA_LOADER_DRIVER_OVERRIDE=m9x

# the emitter, one pass at a time: every one of these is pixel-exact
M9X_PASS_LOOP=1 ./m9-mesa-glsl tex4

# the producing pass alone, with the record descriptor: stalls
M9X_SPLIT=1 M9X_FS_FORCE_SPLIT=2 M9X_SPLIT_PASSES=1 ./m9-mesa-glsl texaff
# the same with an ordinary colour descriptor on the same object: runs
M9X_SPLIT=1 M9X_FS_FORCE_SPLIT=2 M9X_SPLIT_PASSES=1 M9X_SPLIT_STAGE=4 \
	./m9-mesa-glsl texaff

# the five-fetch fixture, dispatched but not listed while the record stalls
M9X_SPLIT=1 ./m9-mesa-glsl tex5
```

`M9X_SPLIT_STAGE` selects which binding a split job makes: 1 leaves the record
unbound, 2 binds it with the routes idle, 3 is the real schedule, 4 binds the
record object with a colour descriptor, 5 with the captured two-dword producer
descriptor, 6 with the record format over the colour target's row span.
`M9X_RECORD_88` replaces the derived index-`0x88` low byte of a record pass and
`M9X_SPLIT_COVERAGE=0` keeps its plain coverage words, so each can be ruled out
on its own.  `M9X_TRANSITION=0` drops the engine transition of section 3 from a
split job, which separates a fault in that block from one in the record;
`M9X_SPLIT_SUBMIT=0` puts every pass of a split shader in one submission, in
one raster range, which is the vendor's own layout.

```sh
# the block on its own, with one pass: renders
M9X_PASS_LOOP=1 ./m9-mesa-glsl texaff
# the block in front of a record pass: still stalls
M9X_SPLIT=1 M9X_FS_FORCE_SPLIT=2 M9X_SPLIT_PASSES=1 M9X_TRANSITION=1 \
	./m9-mesa-glsl texaff
```

<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
