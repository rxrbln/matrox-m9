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

The conclusion is that **binding the record needs something this tree has not
decoded yet**, most plausibly part of the engine transition the vendor emits
between its passes and the m9x kernel refuses (section 3).

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

The first row matters as much as the rest: it says the multi-pass emitter
itself is right.  A pass loop that emits one pass produces a job the hardware
renders exactly, for an interpolated-varying shader, a four-texture shader and
a shader with two varyings and a texture alike.

## 3. What this leaves open in the pass-splitting note

`docs/parhelia-m9-fragment-pass-splitting.md` section 6 states that the vendor
separates its passes with an `e6`/`44` block the m9x kernel does not accept,
and that "a clean-room driver therefore separates its passes with a range
break, which is a stronger barrier".  **That substitution is still untested**:
every configuration with two raster passes also bound the record, so the stall
in section 2 explains them all without saying anything about the barrier.

What the measurements do say is that the `e6` block is not obviously optional.
It is the one part of the captured schedule a clean-room driver cannot
reproduce, and the one part of the state a producing pass needs that this tree
cannot yet emit.  Decoding it -- the values are
`e6=38000000 40004000 0c000000` with two writes of `44=40004000` as the
epilogue, and `e6=604fff9f 2c000000 49d000a9 08000000 3` as the prologue, with
the third prologue word varying per pass (`49d000ac`, `49d000a3`, `49d00095`
in the five-sampler capture) -- is the next step, together with letting the
kernel's `m9_parse_commands` accept them.

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
on its own.

<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
