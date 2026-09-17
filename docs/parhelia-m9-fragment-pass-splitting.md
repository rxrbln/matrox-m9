# M9 fragment multi-pass scheduling: the split contract

How the M9148 fragment pipeline splits one GLSL fragment shader across several
raster draws, and what a clean-room scheduler must emit for each.  Everything
below is read out of the saved vendor captures in this tree; the decode
commands are given so every line can be re-derived.

Labels:

* **CAPTURED** — read from a saved vendor command stream or program image.
* **MEASURED** — a vendor readback checked against a CPU reference.
* **DERIVED** — a conclusion joined from several captured facts; the join is
  given.
* **OPEN** — not decoded here.

## 1. The result: passes hand values over through memory, not registers

**The carrier between raster pass `N` and pass `N+1` is an intermediate render
surface.**  The producing pass binds that surface as render target `b0` instead
of the application colour target and writes it through the output register
pairs `0x11`/`0x13`; the consuming pass binds the same address back through the
descriptor-only route index `0x74 + k` with `0x78` the base and `0x79` the
consumer descriptor.  Fragment registers do not survive a draw boundary
(CAPTURED, section 2).

This **refutes** the reading that the two-pass computed-coordinate family
(`fs-tex2d-bias` and its six siblings) is *not* a render to an intermediate
surface because its first program never writes the colour output `0x2d`.
Writing `0x11`/`0x13` and not writing `0x2d` is exactly what a render to an
intermediate record looks like on this engine: `docs/parhelia-m9-raster-stream-limit-re.md`
section 3 states the rule for the raster half — output register pair
`(0x11 + 4j, 0x13 + 4j)` goes to the `j`-th bound target — and the capture
shows the bound target (CAPTURED):

```text
$ python3 tools/m9-command-decode.py --execute \
      captures/m9x-vendor-glsl-programs-20260915/fs-tex2d-bias/m9x-exec-8350-000015-in.bin \
      captures/m9x-vendor-glsl-programs-20260915/fs-tex2d-bias/m9x-exec-8350-000015-bm2-in.bin

range 1 (the raster range), first draw:
  b0=10ec1000  b1=ee190004          <- intermediate surface, not the colour target
  88=0110fffc  8c=00500100  c3=00
range 1, second draw:
  b0=11f09000  b1=cc210080          <- the application colour target
  74=01000310  78=10ec1000  79=ee190204   <- the same surface bound back
  88=0010ffe3  8c=00500200  c3=04
```

The consequence for a scheduler is that a computed coordinate and a shader
needing more than four texture fetches are **the same feature**: both need an
intermediate surface, a producing pass that writes `0x11`/`0x13`, and a route
binding in the consumer.  They differ only in the route word and in whether the
consumer reads the record as a coordinate or as a value.

## 2. Evidence that registers do not persist

Three independent captured facts, none of which a surviving register file can
explain:

1. The producing pass binds an intermediate address as `b0` and the consuming
   pass binds that same address through `0x78` (CAPTURED, the decode above, and
   identically in all seven two-pass cases — section 4).
2. `docs/parhelia-m9-five-sampler-schedule-re.md` records four intermediate
   render passes followed by sampled reads of the same address, and states the
   negative directly: "no register-only interpretation can explain four RT0
   writes followed by sampled reads of the same address".
3. `docs/parhelia-m9-raster-stream-limit-re.md` section 3 gives the renaming
   rule: a pass writes `0x11 + 4j` / `0x13 + 4j` to target `j`, and a later
   pass that binds slot `s` as route `k` reads it as the register pair
   `(0x00 + 4k, 0x02 + 4k)` — the same registers a texel arrives in.  A value
   that survived in a register would not be renamed.

## 3. Per-pass state, from the captures

Shared by every two-pass case (CAPTURED; `fs-tex2d-swizzle` differs only where
noted):

| register | producing pass | consuming pass |
|---|---|---|
| `c3` | `0x00` | first slot after the producing program |
| `88` | `0110fffc` (`0110effc` for `fs-tex2d-swizzle`) | `0010ffe3` |
| `8c` | `00500000 \| nconst << 8` | `00500000 \| nconst << 8` |
| `8d`/`8e` | the pass's own constant span | the next span, starting where the first ends |
| `b0`/`b1` | intermediate surface + producer descriptor | colour target + its descriptor |
| `a8` | `0038038a` (depth write set) | `00380388` (depth write cleared) |
| `a9` | `88ffff00` | `08ffff00` |
| `ae` | `32444442` | `22444442` |
| `ab`/`ac` | `ffffffff` / `ffffffff` | the application colour mask |
| `0x74` | `00000100` (idle) | the route word, section 4 |
| `0x70`/`78`/`79`/`7a`/`7c` | the 1x1 placeholder texture, `7c=00000200` | the application texture, `7c=02409200` or `12409200` |
| index-23 declaration | one interpolated input `00100d88` plus the fixed trio `00010688 00c231b6 00d30688` | the fixed trio alone, `00000688 00c131b6 00d20688` |
| streams | four slots, strides `00100010` / `00100006` | three slots, strides `00060010` / `00000010` |

Both passes share **one** `c2` image, **one** `6c` linked record and one
preprocessing range; the two constant spans are consecutive inside the same
arena.  The depth and auxiliary surfaces stay bound across both passes.

Note the two facts a scheduler is most likely to get wrong:

* the producing pass still binds a texture — the opaque-black 1x1 placeholder —
  because index `88` bit 20 is set and setting it without a bound unit-0
  texture stops the engine (MEASURED, `docs/parhelia-m9-raster-stream-limit-re.md`
  section 4);
* the consuming pass of every captured coordinate case declares **no**
  interpolated inputs at all.  Whether a consuming pass may declare
  interpolated inputs *and* a route at the same time is **OPEN** — no captured
  case does both, and glmark2 `shadow` needs it (its second pass still reads
  normals and colours).

## 4. Producer descriptor, consumer descriptor and route word

The intermediate record is addressed by a `0xee` descriptor whose low byte is
the record stride in dwords and whose second byte is a start offset in dwords.
The route word at `0x74 + k` has `0x10` as its low byte and three component
selector bytes above it, `0xff` meaning "absent".

All seven captured cases (CAPTURED; the coordinate each program builds is read
from the program image with `tools/m9-glsl-program-extract.py`):

| case | what pass 1 writes | producer `b1` | route `0x74` | consumer `0x79` |
|---|---|---|---|---|
| `fs-tex2d-swizzle` | `0x11 = (t, s)` | `ee090002` | `00ff0110` | `ee090002` |
| `fs-texrect` | `0x11 = (s', t')` | `ee090002` | `00ff0110` | `ee090002` |
| `fs-tex2d-bias` | `0x11 = (·, lod)`, `0x13 = (s, t)` | `ee190004` | `01000310` | `ee190204` |
| `fs-tex2d-proj3` | `0x11 = (q, ·)`, `0x13 = (sq, tq)` | `ee190004` | `01000310` | `ee190204` |
| `fs-shadow2d` | `0x11 = (ref, ·)`, `0x13 = (s, t)` | `ee190004` | `01000310` | `ee110204` |
| `fs-tex3d` | `0x11 = (r, ·)`, `0x13 = (s, t)` | `ee190004` | `01000310` | `ee110204` |
| `fs-texcube` | `0x11 = (c0, c1)`, `0x13 = (c2, ·)` | `ee190004` | `03020110` | `ee110004` |

Reading the table (DERIVED, and consistent with all seven):

* the **producer** descriptor sizes the record: `0002` = two dwords (one
  register pair, `0x11` only), `0004` = four dwords (`0x11` and `0x13`).  Bit
  20 of the descriptor (`ee09` vs `ee19`) moves with that size.
* the **consumer** descriptor's second byte is where the unit's `(s, t)` pair
  starts inside the record: `02` for the four-dword cases, whose `(s, t)` sits
  in `0x13`, and `00` for the two-dword cases and for the cube, whose first
  two components sit in `0x11`.
* the **route word** supplies the remaining components.  `00ff0110` is "take
  the record's own two scalars", used both for a two-component coordinate and,
  in `docs/parhelia-m9-five-sampler-schedule-re.md`, for a value handed between
  passes (`00ff0110`, `00ff0910`, `00ff0510`, `00ff0d10` there, the second byte
  following the record's slot offset).  `01000310` and `03020110` add a third
  component for the projector, the LOD bias, the shadow reference, the 3-D `r`
  and the cube's major axis.

The exact field boundaries inside the route word are **OPEN**; the five-sampler
note declines to name them, and nothing here needs them.  A scheduler should
emit the captured triple `(producer descriptor, route word, consumer
descriptor)` as a unit for the shape it is reproducing.

## 5. Pass budget

* Four independent fetches is the ceiling for one pass, in the four physical
  slots `0x70..0x73` (CAPTURED, `docs/parhelia-m9-openbits-gl-capture.md`
  section 2, table rows `indep_2` .. `indep_4`).
* A continuation pass carries at most three application textures, because one
  slot holds the previous pass's record.  Measured pass counts: 5 -> 2,
  6 -> 2, 8 -> 3 (MEASURED, same table).  `ceil(N / 3)` above four is the
  doc's extrapolation from those three points.
* A fully dependent chain costs one pass per sampler, because each fetch needs
  the previous result as its coordinate (MEASURED, rows `samplers_4`,
  `samplers_5`, `samplers_9`).
* Six interpolated inputs per pass, bounded by the four-bit declaration word
  count at index `0x22` (CAPTURED/MEASURED,
  `docs/parhelia-m9-raster-stream-limit-re.md`).  With texture coordinates the
  bound is `2 * varyings + coords + 3 <= 16`.

### A correction to the tap claim

`docs/parhelia-m9-openbits-gl-capture.md` section 2 asserts that "many taps of
one sampler stay in one pass, which `taps_49` confirms", and concludes that
glmark2 `effect2d` "needs none at all".  **That pass count is not supported by
anything in this tree.**  What is supported is only correctness:
`result-matrix.txt` records `taps_4/9/16/25/49 PASS checked=64 mismatches=0`
(MEASURED), and `tools/m9x-openbits-probe.c` builds those shaders as
`a += texture2D(s, uv + vec2(dx, dy) / TS)` — a distinct coordinate per tap.
No `cap-taps_*` submission was saved, so the number of raster passes the vendor
used for them was never captured.

Each tap is an independent fetch at a distinct coordinate, so by the engine's
own four-slot rule it consumes a slot, and a 49-tap shader is an `N = 49`
independent-fetch case, not a free one.  Until a `taps_*` submission is
captured, treat the pass count for many taps as `ceil(N / 3)` like any other
independent fetch set, and treat the "needs none at all" sentence as an
unsupported inference.

The same correction applies to the comment on `M9_FS_MAX_TEXTURES` in
`m9_fs_compile.h`, which cites corpus section 6 for "repeated taps of one
sampler take a unit each".  The *unit* claim is right; what section 6 does not
say, and what the openbits note wrongly adds, is that many taps avoid the
split.

## 6. What a scheduler has to emit

For a shader that needs `P` passes, in submission order:

1. One `c2` image holding the `P` fragment programs back to back, then the
   vertex program.  Pass `i` enters at the cumulative slot offset of the
   programs before it; the preprocessing draw still enters at the vertex
   program.  One `6c` linked record serves all of them.
2. One constant arena whose `P` spans are consecutive: pass `i` emits
   `8d` = span start, `8e` = span end, `8c = 0x00500000 | count << 8`.
3. For each pass `i < P - 1`: `b0` = an intermediate record surface with the
   producer descriptor of section 4, `a8` with the depth-write bit set on the
   first pass only, `88 = 0110fffc`, the 1x1 placeholder texture in slot 0, and
   a program that writes `0x11` (and `0x13` when the record is four dwords)
   and never `0x2d`.
4. For the final pass: `b0` = the application colour target, `a8` with the
   depth-write bit cleared, `88 = 0010ffe3`, the route words at `0x74 + k` with
   `0x78`/`0x79` for each record it consumes, then the application textures in
   the remaining full slots.
5. A range break between passes, and the index-`43` dependency advanced for
   each (the five-sampler capture advances it by `0x20` per pass).

Correctness gates that the captures do **not** settle, restated from
`docs/parhelia-m9-five-sampler-schedule-re.md` because they bound what may be
scheduled: blending must read the application destination and run exactly once,
on the final pass; depth and stencil updates must happen at most once; a
`discard` in an early pass must suppress every later write; derivatives must be
computed before a value crosses a record boundary; an occlusion query must
count application fragments once.  A shader whose compiler cannot prove these
must be refused, not split.

## 7. Reproduction

```sh
# the two-pass raster range of any of the seven cases
c=captures/m9x-vendor-glsl-programs-20260915/fs-tex2d-bias
python3 tools/m9-command-decode.py --execute \
        $c/m9x-exec-8350-000015-in.bin $c/m9x-exec-8350-000015-bm2-in.bin

# the programs and per-draw state of a case
python3 tools/m9-glsl-program-extract.py $c
```

<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
