<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
# M9 texture coordinates: what the hardware can sample at, and what a
# shader that computes its coordinate needs

The M9 raster engine performs the texture fetch itself, at a coordinate it
interpolates, one per bound unit: the fragment program never issues a read,
it only reads the texel register the engine filled.  That is why the m9x
backend refuses `texture coordinate is not a varying`.  This note separates
the shaders that can be made to work from the ones that cannot, so neither is
chased in the wrong direction.  Written September 17, 2026 from the glmark2
shaders on the target and from the corpus in `m9_fs_corpus.h`.

## What the engine does (CAPTURED)

Each unit has a declaration entry — `0x00500d88`, `0x00600d88`, `0x00700d88`
for the first three — naming the varying it interpolates, and the program
addresses its result as a texel register pair.  Nothing in the 182-shader
corpus samples at anything but an interpolated varying, and there is no
instruction that takes a coordinate operand.  `glamor/xv-planar-3` binds
**three** units (r88 bit 22 set, three declarations), so the driver's
`M9_FS_MAX_TEXTURES` of 2 is a driver limit, not the hardware's; how far above
three it goes is not established.

## The case that can be made to work: an affine coordinate

Interpolation is linear, so for a coordinate that is an affine function of
varyings with per-draw coefficients,

    interp(A * v + b) == A * interp(v) + b

and the driver can compute `A * v + b` per vertex on the CPU and hand it to
the engine as an ordinary varying.  The result is exact, not an approximation:
the engine interpolates a different varying, and samples at precisely the
coordinate the shader asked for.

The vertex stage already runs on the CPU in the Gallium draw module, and the
context already carries driver-synthesised per-vertex varyings (`struct
m9_context::extra`, "float4 CPU varyings per source record"), so the machinery
exists.  What is needed is for the fragment backend to recognise the affine
form, record the transform, and for the draw path to emit the extra varying.

glmark2's **jellyfish** is exactly this shape:

    texture2D(uSampler1, vec2(vWorld.x / 24.0 + uCurrentTime / 20.0,
                              (vWorld.z - vWorld.y) / 48.0 + uCurrentTime / 40.0))

— two varying components combined with uniform coefficients.  It binds two
units, so it needs no new unit either.

**bump-height** is the same shape but needs three units:

    texture2D(HeightMap, TextureCoord)
    texture2D(HeightMap, TextureCoord + vec2(TextureStepX, 0.0))
    texture2D(HeightMap, TextureCoord + vec2(0.0, TextureStepY))

Three fetches of one image at three uniform offsets: three declarations, three
varyings, one image bound three times.  Feasible once the unit count is raised
and the affine fold is in.

## The cases that cannot be made to work

A convolution samples one image at many offsets, and every sample costs a
unit, because a unit is what carries a coordinate.  There is no dependent
read to fall back on.

| scene | fetches of one image | verdict |
|---|---|---|
| effect2d, 3x3 kernel | 9 | out of reach |
| effect2d, 5x5 kernel | 25 | out of reach |
| desktop, blur radius 5 separable | 11 per pass | out of reach |
| terrain | 6 distinct samplers, plus computed coordinates | out of reach |

These are not backend limitations to be worked around; they are what the
raster engine is.  The desktop blur scene still runs and is still worth
optimising — it draws its windows, and only the blur passes are refused — but
its windows will not be blurred on this hardware.

## Implementation sketch

1. In `build_tex`, accept a coordinate whose two components are each
   `fadd(fmul(varying_component, uniform_or_literal), uniform_or_literal)`,
   or any subset of that, and record per unit: the source varying location
   and components, and where the four coefficients come from.
2. Extend `struct m9_fs_program`'s per-unit texcoord description with that
   transform, defaulting to the identity so nothing else changes.
3. In the draw path, when a unit's transform is not the identity, emit the
   transformed coordinate as one of the CPU varyings the context already
   synthesises, and point the unit's declaration at it.
4. Raise `M9_FS_MAX_TEXTURES` to the number of units the declaration form
   supports, which needs the third and any further declaration words
   confirmed against `glamor/xv-planar-3` and the captures.

A fixture should go in first: sample one image twice, once at a varying and
once at that varying times a uniform plus a uniform offset, against a CPU
reference that computes the same two texels.
