# M9 multiple render targets: what is decoded, and what routes an output

Two notes in this tree disagreed about MRT.
`docs/parhelia-m9-implementation-backlog.md` recorded it as fully reverse
engineered and needing only `caps->max_render_targets = 1` raised, while
`docs/parhelia-m9-mrt-retirement-re.md` recorded the routing of a colour
export to target 1 as **[OPEN]**.  This note settles that from the evidence
and closes three of the four open pieces.

Labels: **CAPTURED** read from a saved command stream, **BIN** read from a
vendor binary, **DERIVED** a join of those, **MEASURED** rendered and checked
on this hardware, **OPEN** not decoded.  Checked by
`tools/m9-mrt-routing-evidence.py`.

## Result

The retirement note is the correct one.  The *surface* side of MRT is exact
and needs no guessing; the *fragment* side has never been captured, because
the vendor's MRT fragment programs live in the `c2` VRAM allocation and no
capture in this tree dumps one.  `captures/m9x-cold-init-20260915-v1` has
`c2` images only for single-target gltri and glxgears.  So the 2026-09-17
live-GL MRT result proves the hardware renders four targets; it does not
tell a new driver how its own shader names the second one.

| piece | state |
|---|---|
| one address/descriptor pair a target at `b0 + 2s` | CAPTURED, one to four attachments |
| render format class a target, raster `89` | CAPTURED, four cases |
| write mask a target, raster `ab` and `ac` | CAPTURED, four cases (**new, below**) |
| blend a target | BIN, compiled into the target's own program fragment |
| kernel acceptance of `b0/b2/b4/b6` as `COLOR` | already in `m9_index_reloc_kinds` |
| **native destination that names target p** | **DERIVED, below; not yet MEASURED** |

## 1. The four capture cases

`captures/m9x-vendor-openbits-20260917/cap-mrt_{1,2,3,4}`, decoded with
`tools/m9-command-decode.py --execute`.  Each case is one submission holding
the clears, the preprocessing pass and the raster draw.  Everything that
moves between the four raster draws, with the stale words named:

| index | mrt_1 | mrt_2 | mrt_3 | mrt_4 |
|---|---|---|---|---|
| `b0/b1` | `120c9000`/`c8210100` | `12111000` | `12159000` | `121a1000` |
| `b2/b3` | — | `120c9000` | `12111000` | `12159000` |
| `b4/b5` | — | — | `120c9000` | `12111000` |
| `b6/b7` | — | — | — | `120c9000` |
| `89` | `fe007ff8` | `fe007fc0` | `fe007e00` | `fe007000` |
| `ab` | `1aebffff` | `ffffffff` | `ffffffff` | `ffffffff` |
| `ac` | `00002b4d` | `00002b72` | `0000ffff` | `ffffffff` |
| `88` | `0110fff0` | `0110ffc8` | `0110ff30` | `0110ff20` |
| `8c` | `00500600` | `00500700` | `00500800` | `00500900` |
| `8e - 8d` | `0x30` | `0x38` | `0x40` | `0x48` |

Every other raster word is identical across the four.  `b8`/`ba` carry
leftovers of the preceding pass (`mrt_4` has the literal upload word
`ffff931c` in `b8`), which is what a probe with no depth attachment leaves
behind, not state.

### 1a. Index `89`, one three-bit class a target (CAPTURED)

Bits 14:0 are five three-bit fields.  Field `s` holds the render format
class of the target bound at `b0 + 2s`, and a field no target claims reads
`7`.  The fifth field, bits 14:12, reads `7` in all four cases including the
four-target one, so no target ever claims it.  RGBA8 is class `0`, which is
what the existing single-target word `fe207ff8` already carries.

```text
1 target   0x7ff8   0 7 7 7 7
2 targets  0x7fc0   0 0 7 7 7
3 targets  0x7e00   0 0 0 7 7
4 targets  0x7000   0 0 0 0 7
```

`m9_raster_89` in `m9_draw.c` builds this and keeps bits above 14 from the
word the pass already carries.

### 1b. Indices `ab` and `ac`, one write mask a target (CAPTURED, new)

`ab` bits 15:0 are target 0's sixteen-bit colour write mask and bits 31:16
target 1's; `ac` holds targets 2 and 3 the same way.  The four cases fix
this exactly: `mrt_1` writes a mask only in `ab`'s low half and leaves the
rest stale, `mrt_2` fills `ab` and leaves `ac` stale, `mrt_3` fills `ab` and
`ac`'s low half and clears `ac`'s upper, and `mrt_4` fills both.

This joins the Windows construction: `0x1389a0` translates a target's RGBA
write enables to `0x4444` (R), `0x2222` (G), `0x1111` (B) and `0x8888` (A)
and stores the sixteen-bit result at state `+0x358 + 2*target`
(`0x138b78..0x138bd0`) [BIN].  Four halfwords, two registers.  The driver's
existing `m9_color_mask` already builds one target's halfword;
`m9_color_mask_pair` packs two.

## 2. What routes a colour output to its target

This is the piece nothing captured.  The measured single-target output is

```text
W0 = 0xffc00000 | c << 15 | 0x2d << 8 | 4 << 5 | op0    op0 1d -> R,G ; 1e -> B,A
```

with destination selector `0x2d` (MEASURED,
`docs/parhelia-m9-native-fragment-program-re.md`), and `op0` has five bits,
so it cannot itself carry four targets and two halves.

The vendor's per-target program builder does name a target.  MxMD's
per-target routine calls the blend compiler `0x170ff0` with the target index
as its fourth argument, and that compiler, at `0x171fea..0x172072`, looks up
the two output records keyed `0x18` and `0x1a` and writes into each the
register

```text
key 0x18   0xffec + 2 * target
key 0x1a   0xffed + 2 * target
```

[BIN].  Two more sites build the same pair: `0x138ace..0x138b16` in the mask
builder and `0x1747b8..0x1747e7` in the target lowerer, both with the same
`0xffec + 2p` / `0xffed + 2p` and the same `0x18` / `0x1a` keys.

So a target owns **two** virtual output registers, one a half, and they step
by two a target.  The hardware has **one** destination selector a target,
because the measured pair writes `0x2d` for *both* halves and separates them
in `op0`.  Two virtual registers collapsing to one physical destination, and
a step of two virtual registers a target, give a step of one physical
destination a target:

```text
target p  ->  destination selector 0x2d + p,  op0 1d and 1e   (DERIVED)
```

which is `0x2d`, `0x2e`, `0x2f`, `0x30`.  This is DERIVED, not MEASURED.  The
virtual-to-physical map itself is the join
`docs/parhelia-m9-native-ps-compile.md` section 5 records as unresolved, and
this note narrows it rather than closing it.

The competing shape worth naming, so a sweep tests it too, is the routing
class: the output move carries `cls = 4`, the field is three bits, and
`4..7` would seat four targets as well.  Nothing in the binary points at it
-- the per-target work in MxMD touches a register, not a class -- but a
hardware sweep costs the same for both.

### 2a. The probe that would settle it

Bind two RGBA8 attachments whose texels identify their target and their
cell, compile a shader with two colour outputs, and emit the second one's
two slots with the candidate destination.  Read both attachments back.
`0x2d + p` correct and `0x2e` leaving target 0 alone is the positive
result; target 0 carrying both writes says the field is elsewhere.  Sweep,
in order, `d = 0x2d + p`, `cls = 4 + p`, `d = 0x2d + 2p`.  A class-0
destination has stopped this engine before
(`docs/parhelia-m9-native-ps-compile.md`), so each candidate is one boot.

## 3. What the driver now does

`m9_draw.c` binds up to four colour attachments: `job->color` at `b0/b1` and
`job->color_extra[s - 1]` at `b0 + 2s` with its own descriptor and its own
`COLOR` relocation, packs `job->color_class[]` into raster `89` through
`m9_raster_89`, and emits `job->color_mask` and `job->color_mask_hi` into
`ab` and `ac`.  The kernel's `m9_index_reloc_kinds` already accepts `COLOR`
at `b0`, `b2`, `b4` and `b6`, so no kernel change is needed.
`test_mrt_job` in `m9_batch_test.c` pins all of it offline, and the
single-target job stays byte for byte the captured stream.

What is **not** done, and why `caps->max_render_targets` stays `1`:

* the fragment backend compiles one colour output; multiple outputs need the
  scheduler to carry a target index through `struct lop` into `encode_lops`,
  where the destination becomes `OUT + target` instead of `OUT`;
* `m9_context.c` binds one `pipe_surface`, so nothing fills the new job
  fields yet;
* and the destination above is DERIVED.  Advertising four targets before
  section 2a has run would advertise a guess.

## 4. The backlog line that was wrong

`docs/parhelia-m9-implementation-backlog.md` listed MRT as needing only the
cap raised.  What that line described -- descriptors, relocations, masks and
blend, verified live on 2026-09-17 -- is all true of the *vendor* stack and
of the surface side of ours.  It omitted that the vendor's shader-side
routing was never captured and that our own fragment backend has no second
output at all.  The line is corrected there.

<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
