<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
# M9 raster streams: how many interpolated inputs one pass really carries

The clean-room driver states its interpolation limit once, in
`m9_raster_fs_layout_ok()`: six stream slots, three of them reserved for the
rasterizer position, the clip code and the clip position, and an eight-word
declaration.  Three interpolated inputs are left, which is what refuses
glmark2's `jellyfish`, `bump bump-render=height` and `ideas`.

This note establishes what the engine actually carries, decodes the vendor's
multi-pass contract for the shaders that need more, and records the hardware
run that settles it.

Labels: **MEASURED** is an M9148 (`102b:0540`) result read back from the colour
surface and compared against a CPU reference for every pixel, through
`tools/m9-user-raster-streams.c` on September 17, 2026; **CAPTURED** is read
from the vendor command streams in `captures/m9x-vendor-glsl-programs-20260915`;
**DERIVED** is inferred from those; **UNTESTED** is named where it is.

## Result

**Six interpolated inputs per raster pass, not three — and six is the real
ceiling.**  MEASURED: a single raster pass with six four-component inputs, nine
stream slots (`00..08`), five packed stride words (`10..14`) and a
fifteen-word declaration renders all three probe triangles with 100 % of
25518 interior pixels matching the CPU reference within one count, for each of
the six inputs in turn, with no input aliasing another.

Three was a limit of the driver's own constants and of the kernel validator's
index whitelist.  Nothing about it was a hardware property.

The ceiling above six is not a stream-slot count: it is the declaration-word
count field of index `22`, four bits wide at 11:8, so at most sixteen words.
An interpolated input costs two declaration words and the fixed trio
(position, clip code, clip position) costs three, so `2 * 6 + 3 = 15` fits and
`2 * 7 + 3 = 17` does not.  This is exactly where the vendor's own compiler
stops: `vs-varyings-max`, a shader with fourteen varyings, is split into
**three raster passes** carrying 2, 6 and 6 inputs (CAPTURED).

## 1. What a vendor raster pass binds (CAPTURED)

Every raster pass names its streams as indexed slots `00..`, their strides
packed two per word from index `10..`, and its attribute layout as index `23`
words counted by index `22` bits 11:8.

| case | inputs | slots used | stride words | decl words (`22`) |
|---|---:|---|---|---:|
| driver's first-triangle stream | 1 | `00..03` | `10, 11` | 5 (`0x6403`) |
| `pr-tex-mul-color` | 1 + 1 texture | `00..04` | `10..12` | 6 (`0x2503`) |
| `vs-varyings-2` | 2 | `00..04` | `10..12` | 7 (`0x2603`) |
| `vs-varyings-4` | 4 | `00..06` | `10..13` | 11 (`0x2a03`) |
| `vs-varyings-max` pass 2 and 3 | 6 | `00..08` | `10..14` | 15 (`0x2e03`) |

`vs-varyings-4`'s raster pass, verbatim from the capture:

```text
00=113e8000 01=113db000 10=00100010
02=113ce000 03=113c1000 11=00100010
04=11402000 05=1140f000 12=00060010
06=113f5000 07=00000000 13=00000010
22=00002a03
23=00100d88 23=001006b6   attr 1  stream 0
23=00210d88 23=002106b6   attr 2  stream 1
23=00320d88 23=003206b6   attr 3  stream 2
23=00430d88 23=004306b6   attr 4  stream 3
23=00040688             attr 0  stream 4   rasterizer position
23=00c531b6             attr 12 stream 5   clip code, stride 6
23=00d60688             attr 13 stream 6   clip position
```

So the driver's `M9_RASTER_MAX_STREAMS = 6` and `M9_RASTER_MAX_DECL = 8` are
both under-counts of what the vendor emits, by four slots and seven words.

### The attribute numbers are a fixed functional space

CAPTURED across the corpus: attribute 0 is the rasterizer position, 5..8 are
the four texture coordinates (one declaration word each, form `0688`), 12 is
the clip code (`31b6`, a six-byte stream) and 13 is the clip position.  The
interpolated inputs take 1..4 and then **9, 10** — `vs-varyings-max`'s
six-input passes declare attributes `1 2 3 4 9 10`, skipping the texture
coordinate block.  MEASURED here for all six.

The fragment register group is **not** the attribute number: it is the index of
the input among the declared four-component inputs, in declaration order.
`vs-varyings-max`'s six-input fragment program interpolates from
`0x40 + 8k` for `k = 0..5` while declaring attributes 1,2,3,4,9,10 (CAPTURED),
and the probe confirms the same dense numbering (MEASURED).  This is what
`m9_fs_compile.c` already assumes, and why `M9_FS_MAX_VARYINGS` is 6.

### Why the driver's `m9_raster_fs_init_textured()` would have collided

It numbers input `k` as attribute `k + 1`, so a fifth input would have claimed
attribute 5 — texture unit 0's coordinate.  The correct map is `k + 1` for
`k < 4` and `k + 5` after it.

## 2. The kernel validator was the hard stop

`m9_index_allowed()` accepted stream slots `0x00..0x05` and strides
`0x10..0x12` only, and `m9_index_reloc_kinds()` described the same range, so
a job naming slot `0x06` was refused with `-EPERM` before it reached the
engine.  MEASURED baseline, unmodified module: `n=1..3` draw, `n=4..6`
return `Operation not permitted`.

Widening both to `0x00..0x09` and `0x10..0x14` — the captured maximum — is the
whole kernel change.

## 3. The multi-pass contract, for the record (CAPTURED)

The driver does not need this (it runs transform and lighting on the CPU and
writes its varyings straight into stream views), but it is what the vendor uses
when one pass cannot hold a shader, and it was undecoded until now.

**A pass writes at most four vector targets plus the clip code.**  Output
register pair `(0x11 + 4j, 0x13 + 4j)` goes to the `j`-th bound target,
`j = 0..3` being `b0, b2, b4, b6`; register `0x21` goes to `b8`.
`vs-varyings-4`'s second preprocessing pass writes `r11/r13, r15/r17, r19/r1b,
r1d/r1f` with `b0,b2,b4,b6` bound.

**Intermediate results are one interleaved arena.**  The target descriptor
`b1/b3/b5/b7` is `0xee19 | slot * 4 << 8 | total_slots * 4`: each vertex record
is `total_slots * 16` bytes and slot *s* sits at `s * 16`.  `vs-varyings-4`
uses five slots (`ee190014 ee190414 ee190814 ee190c14 ee191014`),
`vs-varyings-2` three (`ee19000c ee19040c ee19080c`), `vs-varyings-max`
fifteen (`...3c`).

**A later pass binds up to four of those slots as sources and sees them as
low-numbered registers.**  Binding `k` is index `0x74 + k` with `0x78` the
arena base and `0x79` the descriptor of slot *s*; the `0x74 + k` word is
`0x0c | (4s + 1) << 8 | (4s + 2) << 16 | (4s + 3) << 24`.  The program then
reads binding `k` as the register pair `(0x00 + 4k, 0x02 + 4k)`.
`vs-varyings-4`'s third pass binds slots 1..4 and its nine slots are exactly

```text
r11 = mov r00   r13 = mov r02   r15 = mov r04   r17 = mov r06
r19 = mov r08   r1b = mov r0a   r1d = mov r0c   r1f = mov r0e   stop
```

a pure gather of four inputs into four targets.  A later pass carries no
source record declaration: its index `22` is `0x...000e` and its only index
`23` word is the `00004b08` terminator.

**The raster half splits the same way.**  `vs-varyings-max` issues three raster
draws: the first two write `b0` into a two-slot arena (`ee190408`, then
`ee190008`) and the third writes the colour surface, each later pass binding
the previous one's slot through `0x74`/`0x78`/`0x79` exactly as above.  The
fragment programs are three separate entries at `c3` = 0x00, 0x09 and 0x22.

## 4. The hardware measurement

`tools/m9-user-raster-streams.c` rebuilds the verified two-pass stream packet
by packet, replaces the raster pass's stream, stride and declaration packets
with `N` interpolated inputs, and binds each input to its own CPU-written view
in a system-memory buffer — the address class the driver already uses for
varyings the draw module produces.  The fragment program is the two-slot
interpolation the driver's own backend emits, returning input `k`:

```text
slot 0   a = 0x42 + 8k   b = 0x79   c = 0x45 + 8k   d = 0x2d   op0 = 0x1e
slot 1   a = 0x40 + 8k   b = 0x79   c = 0x44 + 8k   d = 0x2d   op0 = 0x1d
slot 2,3 stop
```

Each view carries a value that identifies both the view and the vertex, so a
mis-wired slot reads as a different input rather than as noise, and the probe
counts those aliases separately.  Every pixel at least one pixel inside a
triangle is compared against the barycentric mix of that triangle's three
corner values.

```text
staged module /root/tmp/re-m9x_drm.ko, 2026-09-17
n=1 read=0 PASS inside 25518 match 25518 (100.0%) worst 1 written 26838 outside 0
...
n=6 read=5 PASS inside 25518 match 25518 (100.0%) worst 1 written 26718 outside 0
raster-streams: 21 PASS 0 FAIL
```

All 21 (`n`, `k`) combinations for `n = 1..6` pass, no run reported an alias,
and no run wrote a pixel outside a predicted triangle.

Two things the probe had to get right, and both cost a debugging round:

* **The four-slot marker.**  `finish_slots()` pads a fragment program to at
  least four slots and sets bit 30 of the `W1` of slot `n - 4`.  Without it the
  program returns a constant `(0, 1, 0, 1)` instead of the interpolation —
  it does not fault, it silently produces the wrong colour.
* **The placeholder texture.**  Index `88` bit 20 is the unary texture-unit
  count and the driver sets it even with no texture; setting it without binding
  the opaque-black 1x1 unit-0 texture **stops the engine** (`-ETIMEDOUT`, then
  `-EIO` on every later submit, recovered by a reboot).

## 5. What the driver has to change

Nothing in the hardware; six constants, one loop bound, one attribute map and
one kernel whitelist.

1. `m9_submit.c`: `m9_index_allowed()` and `m9_index_reloc_kinds()` from
   `0x00..0x05` / `0x10..0x12` to `0x00..0x09` / `0x10..0x14`.  Done.
2. `m9_draw_params.h`: `M9_RASTER_MAX_STREAMS` 6 -> 10,
   `M9_RASTER_MAX_DECL` 8 -> 16, `M9_TRANSIENT_VIEWS` 7 -> 11 (four views for
   the preprocessing pass plus the seven CPU views the stream limit allows).
3. `m9_draw_params.c`: `m9_raster_fs_layout_ok()` keeps its shape — the two
   inequalities are already the right ones, only their bounds were wrong — and
   gains `varying_count <= M9_FS_MAX_VARYINGS`.  Input `k` is declared as
   attribute `k < 4 ? k + 1 : k + 5`.
4. `m9_draw.c`: the stream loop `for (pair = 0; pair < 3 && ...)` becomes
   `pair < (fs->stream_count + 1) / 2`; it already emits the two-value packet
   for an odd stream count, which the probe confirms the engine accepts.
5. `m9_fs_compile.c`: `varying_slot()`'s `varying_count + texture_count >= 3`
   becomes the same predicate the raster layout uses, so the compiler and the
   emitter agree in one place.
6. `m9_context.c`: the transient-view gate follows `M9_TRANSIENT_VIEWS`, and
   the local `views[]`/`flat[]` arrays are already `M9_FS_MAX_VARYINGS` wide.

What stays refused: seven or more interpolated inputs in one pass, which the
declaration-count field cannot express, and which the vendor answers with the
multi-pass split of section 3.  With four texture coordinates the declaration
limit binds first: `2 * varying + coord + 3 <= 16`.

## Reproduction

```sh
# host: the captured passes, their programs and their constant spans
python3 tools/m9-vertex-program-extract.py \
    captures/m9x-vendor-glsl-programs-20260915/vs-varyings-4
python3 tools/m9-glsl-program-extract.py \
    captures/m9x-vendor-glsl-programs-20260915/vs-varyings-max

# target, next to m9-user-triangle-data.h, staged module, inside the lock
gcc -O2 -Wall -I/root/parhelia-cleanroom/include -o m9-user-raster-streams \
    m9-user-raster-streams.c -lm
flock -w 1800 /root/tmp/m9-hw.lock ./m9-user-raster-streams 1 6
```

The probe takes `first` and `last` input counts, and reads
`M9RS_DUMP`, `M9RS_STREAM`, `M9RS_VIEW0_TRANSIENT`, `M9RS_NO_TEXTURE`,
`M9RS_CLS` and `M9RS_R88` for bisecting against the captured configuration.
