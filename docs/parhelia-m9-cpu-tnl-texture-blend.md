# M9 CPU T&L texturing and blending: raster encodings and hardware status

This note collects what is known for texturing and blending on the M9148
(`102b:0540`) userspace two-pass draw used by the m9x CPU T&L driver.  Labels:
**MEASURED** = reproduced through `DRM_IOCTL_PARHELIA_M9_SUBMIT` on the M9148,
**CAPTURED** = vendor ICD command capture, **STATIC** = saved vendor binaries.
Read `parhelia-m9-cpu-tnl-raster-state.md` and
`parhelia-m9-cpu-tnl-draw-parameters.md` first.

Probe tool: `tools/m9-user-texture.c` (build next to `m9-user-triangle-data.h`,
`gcc -O2 -Wall -I<uapi>`).  All state is set with `key=value` arguments
(texture unit block, raster `22/88/8c/f0/10/11`, raster declaration `rdecl=`,
a CPU-written transient view, raster stream 0 redirection, c2 raster-entry
slots).  The texture BO carries a per-dword storage-index code.

## 1. The c2 raster entry is fixed setup, not the combiner (CAPTURED/STATIC)

The raster pass binds the same c2 image with `c3=0`.  Slots 0..3 are
`5e62ad9e ee003c18`, `1e622d9d ae003c18` and two `ffff801c a01fc00f`
terminators in the flat and Gouraud gltri program and, byte for byte, in the
lit vendor glxgears program (`gt-gears-150539/vram/c2-12033000.bin`).  The
per-state part of c2 is the preprocessing section (entries `c3=4`, and
`4/0x14/0x19` for the three textured stages).  Offline execution of the
vendor TEXLD lowerer emits the same three slots for every coordinate and
sampler descriptor, so no native slot carries the unit or coordinate
routing.  Texture combine is therefore selected by raster state words and the
raster declaration, not by writing a fragment program into c2.

## 2. System constants and linked record are texture independent (CAPTURED)

Textured draw `texture-resources/phase3/case-20` (256x256 target, 4x4 RGBA8,
GL_REPLACE) uses raster `8d/8e = [0,0x10)`, `6c = +0x400`.  Its `0x21000`
snapshot has the same constant layout as the untextured prefix; only the
viewport-size words differ (`256 -> 0x43000000`), and the linked record is
`{W/2,-H/2,.5,0,W/2,H/2,.5,0}`.  No texture-dependent value lives there.

## 3. Captured textured raster templates

| word | untextured gltri | REPLACE, case-20 | MODULATE gltri 64x64 | MODULATE + blend, q3 |
|---|---|---|---|---|
| `22` | `00006403` flat | `00002306` smooth quads | `00006503` flat | `00002506` smooth quads |
| `88` | `0100dff0` | `0010ffe3` | `0110dfe3` | `0110ff03` |
| `8c` | `00500200` | `00500200` | `00500200` | `00500300` |
| `f0` | `c4008400` | `c4008400` | `c4008400` | `c5008200` |
| streams | 4 (`00..03`) | 4 (`00..03`) | 5 (`00..04`) | 5 |
| `10/11/12` | `00100010/00100006/-` | `00100010/00100006/-` | `00100010/00060010/00000010` | same as gltri |
| raster `23` | `80100d88 801006b6 00010688 00c231b6 00d30688` | `00500688 00010688 00c231b6 00d30688` | `80100d88 801006b6 00510688 00020688 00c331b6 00d40688` | as gltri, bit 31 clear |

Unit block (all textured cases): `74..77=100`, `70=00040118`,
`78=<texture>, 79=c8010000|width, 7a=00010000|height`, `7c=00009200`
(nearest; `00009292` = linear min/mag + nearest mip), `7d=0ff80000`,
`87=0`, `82..85=0`, `80=ffffffff, 81=7e=7f=0`, `71..73=0`.

Decoded consequences (CAPTURED, one sample each; not yet MEASURED):

* Raster declaration attribute `5` is texture coordinate 0; attribute `1`
  (the bit-31 link pair) is the color varying.  REPLACE drops the color pair
  entirely and places the texcoord on stream 0.
* `22` bits `9:8`: `1` in both MODULATE captures, `3` in REPLACE.
* `88`: bit 24 is set whenever the color attribute is declared; the low
  byte is `f0` untextured and `e3` textured without blend, `03` with blend;
  bit 13 is set in REPLACE and in the blended draw.
* `10/11/12` are consistent with one 16-bit record stride per raster stream
  (low half stream `2k`, high half stream `2k+1`): the stream carrying the
  one-component attribute `c` (vendor `b8` view) is `6`, all float4 views are
  `0x10`, in all three layouts.

## 4. Hardware results (MEASURED, September 15, 2026)

Baseline (flat white quad x[40,296) y[40,232) on the untextured template):
49152 pixels `ffffffff`, exact.

| change on the untextured template | result |
|---|---|
| unit block with `7e..85/87` | `EPERM`: indices outside the kernel allow-list |
| unit block `70/78/79/7a/7c/7d` + `22|=0x100` + `88=0110dfe3` | retired, no pixel written |
| same + CPU texcoord view on stream 2 (`rdecl` attr 5) | retired, no pixel written |
| `22|=0x100` only | retired, no pixel written |
| `88=0110dfe3` only (no texture bound) | **job timeout; the engine stops** with command base `0x4820fb6` != end `0x4820fc0`.  `rmmod/insmod` then fails bus-master enable (`requester is not quiescent`) and every submit returns `EBUSY` |

Consequences:

* `22` bit 8 gates fragment output on texture state; with it set and no valid
  texture/stream combination nothing is written.
* `88` low byte `e3` (texture-combine form) without a bound texture hangs the
  engine.  A driver must never emit the textured `88` form without a
  complete unit block, and the probe tool must not be run with `r88` alone.
* The hang is not recoverable by module reload; recovery needs a machine
  reboot (the requester interval never retires).

Hardware testing stopped at this point.  Nothing in sections 1..3 beyond the
baseline is MEASURED yet.

## 5. Next measurements (once the target is rebooted)

1. REPLACE on four streams, exactly the case-20 form: `22=0x2300|topo`,
   `88=0010ffe3`, `rdecl=00500688,00010688,00c231b6,00d30688,0,0,0,0`
   `linkbit=0`, raster stream 0 redirected to a CPU-written texcoord view
   (`rs0view=4 tcstream=4`), unit block without `7e..87`.  Verify every
   pixel against the storage-index code to name the texel fetched (linear or
   tiled storage, descriptor bit 21, 8x4 tile order).
2. MODULATE on four streams by sharing one record: stream 0 stride `0x20`
   (`r10=00100020`), color at dword 0, texcoord at dword 4
   (`rdecl=80100d88,801006b6,04500688,00010688,00c231b6,00d30688,0,0`,
   `22=0x6500|topo`, `88=0110dfe3`, `tccolor=1 tcoff=16 tcstride=32`).
3. Linear filter (`7c=00009292`), wrap/clamp (`7c[27:22]`, table in
   `parhelia-m9-texture-state.md`), then blend: `f0=c5008200`,
   `8c=00500300`, `88` blend form, destination-sensitive fixture with
   alpha ramps; factor encoding is still unlocated (identical direct words
   for all factor pairs in the vendor captures).

## 6. Kernel requests

* Allow indices `7e..87` (border `7e..81`, bump `82..85`, reserved `87`)
  with plain values; the vendor emits them in every textured draw.
* Five-stream raster (MODULATE vendor form): allow index `04` with the
  `STREAM` relocation kind (as `00..03`) and index `12`.
* A recovery path for a stopped engine (non-retired requester interval)
  that does not require a reboot, or reject `88` textured forms without a
  `78` texture relocation in the same pass until the hang is understood.

<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
