# M9128 cube and volume textures in the m9x driver

<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->

Cube maps and 3-D textures in the clean-room Gallium driver: what the
hardware does, what the driver therefore advertises, and what it refuses.

Labels: **CAPTURED** = the vendor dumps behind
`parhelia-m9-texture-storage.md`, **MEASURED** = the M9128 target through
this driver (`tools/m9-mesa-glsl`), **DERIVED** = neither, stated as such.

## 1. Storage

Both targets reuse the tail placement of `m9_texture.c` unchanged -- the
`m9x659` mode-2 aperture calculator with the captured state -- and differ
only in how the layer reaches it:

* **Cube (CAPTURED 4x4, MEASURED to 16x16).**  Face `f` is a whole 32x16
  32-bit allocation at `level_offset + f * face_bytes`, faces ordered +X,
  -X, +Y, -Y, +Z, -Z.  For a chain inside the tail `face_bytes` is the
  0x800-byte atlas, so the six faces occupy 0x3000 bytes and every level of
  a face shares its face's atlas.
* **Volume (CAPTURED 4x4x4).**  The slice is an input of the calculator, not
  an allocation stride: `A9` carries `z`, and the page index is
  `z + (y >> 3)`.  One level allocates `0x800 + (depth - 1) * 0x400`.

Gallium addresses both layers as the transfer box's `z`, so
`m9_texture_store`/`m9_texture_fetch` read the box layer as the face on a
cube and as the slice on a volume.

## 2. Where the model and the hardware agree (MEASURED)

`tools/m9-mesa-glsl cube:N` renders one band per face and
`volume:WxHxD` one strip per slice, each texel coordinate-coded in all
three channels, every pixel checked.  A texel's address does not depend on
the level's extent, so a shape that passes at 16x16 covers every smaller
extent at that depth.

**Cube: every extent passes.**  `cube:2`, `cube:4`, `cube:8` and `cube:16`
render all six faces exactly, 0 of 76800 pixels differing.

**Volume: the model holds only inside a height/depth domain.**  The grid
`volume:16xHxD`, H in {4, 8, 16} and D in 1..8:

| depth | height 4 | height 8 | height 16 |
|---:|---|---|---|
| 1 | PASS | PASS | PASS |
| 2 | PASS | PASS | FAIL |
| 3 | PASS | PASS | FAIL |
| 4 | PASS | **FAIL** | FAIL |
| 5 | PASS | PASS | FAIL |
| 6 | PASS | PASS | FAIL |
| 7 | PASS | PASS | FAIL |
| 8 | PASS | **FAIL** | FAIL |

Depth 16 and 32 fail at every height.  `16x5x2` passes and `8x5x4` fails
one texel row of five, so the boundary is the height itself, not a power of
two.

The failures are not noise.  At `16x8x4` exactly the rows with bit 2 of the
texel `y` set -- half the pixels -- read the slice two pages away: the
hardware's page bit 1 is `z1 ^ y2` there, while the captured state has
plain `z1`.  At `16x16x2` no texel reads correctly.  Both are the vendor's
"different calculator state" for taller volumes, which no capture covers.

So the driver accepts a volume level only for

* height <= 4 at any depth up to 8,
* height <= 8 at depth 2, 3, 5, 6 and 7,
* height <= 16 at depth 1,

and refuses everything else (`m9_volume_agrees` in `m9_texture.c`).

## 3. Placement is verified, not assumed

`m9_texture_layout_init` now walks every texel of every tail level and face
and checks that each lands on an address of its own inside the allocation.
A level that would overwrite another is left unplaced, and so is every
level after it.  This replaces the previous hand-written "3-D level 0 with
height <= 8" rule and catches, for instance, the 8x8x8 four-level chain,
whose level 3 at tail origin (16,8) collides with level 1's second slice.

A plane falls back to the kernel texture window for unplaced levels.  A
cube face and a volume slice have no such fallback -- the window addresses
a plane and would write every face over face 0 -- so
`m9_texture_layout_complete` refuses a layered resource unless every level
is placed and the base level is inside the domain of section 2.

Levels above the base are written to addresses this check proved disjoint
from the base, and nothing samples them: a tail chain clamps the sampler's
last level to the base, exactly as it already did for planes.  Mipmapped
cubes and volumes are therefore created and sampled at the base level, and
their higher levels are stored but unverified.

## 4. Descriptor words

Unchanged from `parhelia-m9-texture-storage.md` section 4; the driver's
`m9_texture_unit_init` already produced them and needed no change:

* `70`: `form` 3 for a cube, 2 for a volume.
* `79`: kind `09` cube, `11` volume.
* `7a`: `depth` is the volume's depth, 1 for a cube.
* `7c`: the R wrap comes from the sampler for both.

`m9_texture_bind_kind` picks the kind from the storage, so a cube and a
volume name themselves and only a plane is still classified small (kind
`01`) or ordinary tiled (kind `21`) by its extent.

## 5. What the driver advertises

`m9_is_format_supported` accepts `PIPE_TEXTURE_CUBE` and `PIPE_TEXTURE_3D`
for `PIPE_BIND_SAMPLER_VIEW` only: there is no captured render, depth or
scanout placement for a face or a slice.  `max_texture_cube_levels` and
`max_texture_3d_levels` are both 5, the 16-texel tail extent, which GL
reports as `GL_MAX_CUBE_MAP_TEXTURE_SIZE` and `GL_MAX_3D_TEXTURE_SIZE` 16
and which turns on `OES_texture_3D`.  A volume inside 16x16x16 but outside
section 2 -- 16x16x16 itself, for one -- is refused at resource creation
rather than placed wrongly; the refusal reaches the application as an
out-of-memory error on `glTexImage3D`.

## 6. Correction to parhelia-m9-driver-readiness.md

The readiness table says of 3-D textures: "7x5x4 slices and R repeat pass;
exact volume descriptor, full-depth mip recurrence, and the calculator-Z
convention", marked **captured**.  The first half is a vendor-ICD result;
the second half overstates what the captures pin down for a clean-room
driver.  Every captured volume dump is 4x4x4, and MEASURED here, 7x5x4 --
height 5 at depth 4 -- is one of the shapes whose rows above texel row 3
read the wrong slice.  The narrower statement in
`parhelia-m9-texture-storage.md` ("heights above 8 need a different state,
not captured") is closer but still too generous: at depth 4 and 8 the
boundary is height 4, not 8.

The cube row is confirmed rather than corrected: all six faces and
orientations render exactly, at every extent up to the tail's 16x16.

## 7. Open

* The calculator state a taller or deeper volume uses.  Recovering it needs
  a vendor capture of a volume larger than 4x4x4, or the state field that
  `request+0x90` carries.
* Mip levels of a cube or a volume: stored, never sampled, unverified.  A
  sampler that selects a level above the base needs the tail's own contract
  first, which is the same gap planes have.
* A face or a slice above the 16x16 tail.  The kernel texture window would
  have to take a face and a slice, which its plane-shaped descriptor cannot
  express today.
