# M9 first-triangle system constant constructor

This note closes the initialized bytes selected by indices `8d/8e` in the
bounded 320x240 fixed-function triangle. It also separates those ranges from
the selector-`6c` record, which carries a second viewport/depth prefix.
`[CAP]` denotes saved vendor capture evidence and `[DER]` a value determined
from that evidence and the specified OpenGL transform.

Reproduce the hashes, decode the selected ranges, and generate the final image
with:

```sh
python3 tools/m9-first-triangle-system-evidence.py
```

The checker reads saved files only. It does not execute vendor code, access a
DRM node, or submit GPU commands.

## Why zero is not a valid image

The captured first triangle binds these end-exclusive system ranges:

```text
preprocess  8d=system+0x10|1  8e=system+0xc0|1
raster      8d=system+0x00|1  8e=system+0x10|1
```

The corresponding vendor allocations contain fixed-function program
constants. They are read-only for the saved submissions: all mapped images
before and after submission are byte-identical. A clean implementation must
therefore initialize bytes `[0,0xc0)` before the draw; an all-zero allocation
sends zero transform and viewport inputs to the selected programs `[CAP]`.

The baseline source is:

```text
captures/m9x-linux-fixed-state-20260914/pairs/case-37/
  draw-a-resource.bin
SHA-256 a6977921a784564b81ae3f41db6300e0316a203ec38ba2a1a0839b2e148586da
first 0xc0 SHA-256
  45262f42ea5b7506b839d97a22f5596aca2f4a98d1296796e34b5df62415ad1f
```

That fixture uses a 256x256 drawable, `glViewport(0,0,256,256)`,
`glDepthRange(0,1)`, and `glOrtho(0,256,0,256,-1,1)` `[CAP]`.

## Append-selected viewport proof

The system allocation is append-only during the controlled state pairs. The
first `0xc0` bytes remain the old A record after state B is compiled, so
examining only the allocation prefix gives a stale result. In extended case
67, setup B writes these selectors:

```text
8d: 40c3a611 40c3a681 40c3a681 40c3a601
8e: 40c3a681 40c3a681 40c3a6e9 40c3a611
6c: 40c3aa01 repeated four times
```

The active B constants therefore begin at system offset `0x600`; the linked
record is independently selected at `0xa00`. The pinned files are:

```text
extended/case-67/setup-b-bm.bin
  548078032b846d750c39f9676f0c980fa4e69444a505c2d378f25102e085f8b7
extended/case-67/setup-b-execute.bin
  a128c11f42d50ddfeddb317313caacf2ee72ca782e908e4320f153240a0fcede
extended/case-67/draw-b-resource.bin
  9ee562e534516030879985889dcde42716103e5db854b36b2096a1327f69b5f5
selected [0x600,0x6c0) SHA-256
  267910a22340dfb6695f6a0faa6ddc44ff787a13144be1b51ae1097069c11dec
```

Case 67 changes only the viewport to `(13,19,171,149)`. Its selected record
contains these values relative to the record:

```text
+0x80  { 85.5, -74.5, 0.5, 1.0 }
+0x90  { 98.5, 162.5, 0.5, 149.0 }
+0xb0  { 13.0, 88.0, 1.0, 0.5 }
```

For drawable height `D`, viewport `(x,y,w,h)`, and depth range `(n,f)`, these
three rows prove the constructor `[CAP][DER]`:

```text
+0x80 = { w/2, -h/2, (f-n)/2, 1 }
+0x90 = { x+w/2, D-y-h/2, (f+n)/2, h }
+0xb0 = { x, D-y-h, 1, 0.5 }
```

Extended case 68 independently changes only `glDepthRange(.25,.75)`. Its
selected record changes `+0x88` from `0.5` to `0.25` while `+0x98` remains
`0.5`, confirming the depth terms. The full resource and selected-record
hashes are:

```text
a21ca0d7816d2d6ff88139ce254ff85d778e9e0d2236ed7471049d1b3a7f6b8d
02a7b776cdcb8099a6c74cff19c1692178235959731935c403ad33177f8bd5fc
```

The projection terms at `+0x24` and `+0x38` are `2/W` and `2/H`. The baseline
contains `2/256` in both positions; the first-triangle fixture specifies
`glOrtho(0,W,0,H,-1,1)`, whose only dimension-dependent matrix coefficients
are those two values. The other captured `-1`, identity, clamp, and guard-band
words remain invariant `[CAP][DER]`.

## Exact 320x240 image

For a 320x240 drawable, full viewport, depth range `(0,1)`, and
`glOrtho(0,320,0,240,-1,1)`, bytes `[0,0xc0)` are the following little-endian
u32 words:

```text
00: 00000000 3f800000 3f800000 3f000000
10: 00000000 bf800000 00000000 3f800000
20: 00000000 3bcccccd 00000000 3f800000
30: 00000000 bf800000 3c088889 00000000
40: 00000000 00000000 bf800000 00000000
50: 00000000 00000000 00000000 00000000
60: 00000000 3f800000 00000000 3f800000
70: 00000000 00000000 3f800000 00000000
80: 43200000 c2f00000 3f000000 3f800000
90: 43200000 42f00000 3f000000 43700000
a0: 00000000 40000000 c67f8000 467f7c00
b0: 00000000 00000000 3f800000 3f000000
```

The exact hashes are:

```text
SHA-256  feafcc5aa2c71a45a09616f3625190b3afe6753ba526f2f822ebd3fbe3dabd46
FNV-1a64 ab2075c3793cfdc5
```

Initialize these bytes once after allocating the system BO, before publishing
its compact-PTE mapping or submitting the first draw. The raster pass reads
the first 16 bytes and preprocessing reads the following 176 bytes. The
selector-`6c` record may share the allocation, but it is a distinct object at
the selected aligned offset and must be constructed separately.

The original 320x240 wrapper did not save the system mapping, so the final
320x240 image is a constrained derivation rather than a direct 320x240 byte
snapshot. Its invariant words are pinned by the 256x256 capture; its viewport
and depth layout is checked against controlled appended records; and its two
remaining changed words are fixed by the exact `glOrtho` matrix used by
`tools/gltri.c`.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
