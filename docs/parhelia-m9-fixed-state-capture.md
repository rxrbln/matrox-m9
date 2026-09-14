# M9 fixed-function and sampler state

This note records controlled direct-rendering differentials from the saved M9
Linux vendor ICD on the M9148 target.  Every A/B pair used one GLX context and
changed one GL state between identical textured triangles.  The X server stayed
alive, every fixture returned `GL_NO_ERROR`, and the post-run renderer remained
`Matrox ICD for M-Series` with direct rendering enabled.

Evidence is in `captures/m9x-linux-fixed-state-20260914`; run:

```sh
python3 tools/m9-fixed-state-evidence.py --self-test
```

## Sampler word `7c`

The baseline 2-D sampler is clamp-to-edge S/T/R, linear magnification, and
linear mipmap-linear minification.  Its word is `02409312`.

| GL change | `7c` |
|---|---:|
| S repeat | `02009312` |
| S mirrored repeat | `03009312` |
| S clamp-to-border | `03c09312` |
| T repeat | `00409312` |
| T mirrored repeat | `08409312` |
| R repeat on `GL_TEXTURE_2D` | `02409312` |
| min nearest | `02409210` |
| min linear, no mip selection | `02409212` |
| nearest-mipmap-nearest | `02409690` |
| linear-mipmap-nearest | `02409292` |
| nearest-mipmap-linear | `02409710` |
| mag nearest | `02409302` |

Bits 24:22 are the S wrap code and bits 27:25 are T: repeat `0`, edge `1`,
mirrored repeat `4`, border `7`.  The unchanged R case is expected for a 2-D
resource and does not assign the R field; a 3-D texture is still required.
The complete low sampler values above are safer initial implementation
constants than an inferred subfield split.

Word `7d` is `0ff80000` at baseline.  LOD bias 1.25 produces `0ff80028`, which
is consistent with 5 fractional bits.  Minimum LOD 2 produces `0ff90000`, and
maximum LOD 2 produces `02000000`.  Each nondefault LOD case also changes the
last `70` word from `00040118` to `000c0118`.  These are exact encodings; the
signed saturation boundaries still need a negative/boundary matrix.

The ICD reports no `GL_EXT_texture_filter_anisotropic`, and the two guarded
anisotropy fixtures therefore issue no extension call.  M9 software should not
advertise anisotropy from this evidence.

## Alpha and blending

Enabling blending changes the last setup words from `88=0110dfe3,
f0=c4008400` to `88=0110ff03,f0=c5008200`.  Four requested factor/equation
combinations all produce those same direct words:

* source alpha / one-minus-source-alpha, add;
* one / one, add;
* destination color / zero, add;
* source alpha / one, subtract.

The factor and equation selection is therefore indirect or program-derived;
this capture proves the enable path but does not assign those fields.  A
destination-sensitive correctness fixture plus program-heap capture is needed
before implementing the full blend matrix.

Alpha reference 0.5 is materialized twice as float `128.0` (`43000000`) at
resource offsets `600/604`; reference 0.25 becomes float `64.0` (`42800000`).
Greater and greater-or-equal do not differ in the direct setup stream.  This
assigns the reference conversion but leaves the comparison opcode in generated
program state.

Color-mask RGB/no-alpha changes the low half of the last `ab` word from `ffff`
to `7777`; red/alpha changes it to `cccc`.  The high half contains a
process-specific address fragment and must be relocated rather than copied.

## Depth and stencil

Baseline setup ends with `a8=0038038a`, `a9=08ffff00`, and `ef=00000000`.

| State | `a8` | `ef` |
|---|---:|---:|
| depth less, write enabled | `0038038b` | `0000f000` |
| depth lequal, write disabled | `0038038d` | `00007000` |
| depth always, write disabled | `0038039d` | `00007000` |

This directly separates depth enable/function/write behavior.  The remaining
five compare functions need the follow-up equivalence-class matrix.

| Stencil case | `a8` | `a9` |
|---|---:|---:|
| always, ref 3, mask ff, keep/keep/replace, write ff | `003903aa` | `08ffff03` |
| equal, ref 5, mask 7f, keep/incr/incr, write 3f | `003ec0aa` | `083f7f05` |
| notequal, ref 12, mask 5a, invert/decr/zero, write a5 | `003beb2a` | `08a55a12` |

`a9` is exactly `0x08WWCCRR` for these cases: `WW` is the write mask, `CC` the
compare mask, and `RR` the reference.  Word `a8` carries enable, compare, and
the three operations, but three mixed samples are insufficient to split every
field.  Capture one operation at a time and separate front/back state before
constructing arbitrary `a8` values.

## Cull and shading

Cull back with CCW front faces produces `ef=03000000`; cull front/CCW and cull
back/CW both produce `02000000`, as expected for the same effective triangle
rejection after reversing the front-face convention.  Baseline culling off is
zero.

Flat shading changes the last `22` word from `00002503` to `00006503` and sets
bit 31 in the two `23` words `00100d88` and `001006b6`.  This is sufficient to
implement the captured smooth/flat switch.  Line and point polygon modes were
only present in the independent-process set and had no selected draw-state
differential; they remain open until a same-context pair is captured.

## Driver rules

Emit sampler and fragment/raster state in the B setup position before the draw,
then retain it as context-owned hardware state.  Do not treat `1b`, the high
half of `ab`, or the resource-tail host pointers as state: they vary between
otherwise identical processes.  The command validator must allow the proven
indices while relocating all address-bearing words.

The next fixture should close the eight depth/alpha/stencil compare codes,
single-op stencil fields, separate front/back stencil, blend factors and
equations with visible destination dependence, logic ops, scissor/viewport/
depth range, polygon offset, widths/smoothing/point sprites, fog/clip planes,
two-sided lighting, and multisample state when a sampled FBConfig exists.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
