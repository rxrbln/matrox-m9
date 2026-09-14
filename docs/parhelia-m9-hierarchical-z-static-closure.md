# M9 hierarchical-Z static closure

This note closes the identity, allocation rule, and final binding of the
optional `ba/bb` depth auxiliary surface.  The saved Windows M9148 driver calls
the feature `HZFlags`; its only metadata allocator creates an internal
`G16R16`-class plane at one element per 8 by 4 depth pixels, and selector 12
installs that plane in the `ba/bb` packet pair.  It is therefore M9
hierarchical-Z metadata rather than an API stencil plane or an unexplained
second depth buffer [BIN][DER].

The reproducible evidence command is:

```sh
python3 tools/m9-hierarchical-z-evidence.py
```

`--json` reports both PE preferred virtual addresses and RVAs.  All claims
below are pinned to:

* Windows `MxMD.dll`, SHA-256
  `b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`;
* Linux Xorg/GL monolith `xserver/1_15_0/m9x_drv.so`, SHA-256
  `15ff4ad6e450c2d5a27c3d114011014e8aaf97851d107a070f31baa26eafc7e5`.

Addresses in the Windows discussion are preferred VAs for the image base
`0x10000`; subtract `0x10000` for the RVA.  The verifier prints both forms.

## Configuration and runtime join

Windows constructor `0xb9297..0xb92be` creates `3D\HZFlags` with default 7
and stores its configuration wrapper at object `+0x48`.  Runtime construction
`0x60397..0x603d6` wraps that value again and stores the result at the settings
owner's `+0x334`.  Vtable entry `0x1f343c` points to getter `0x62310`, which
returns wrapper `+0x10` [BIN].

The saved Linux stack independently contains the same setting.  Code
`0x32f40d..0x32f444` passes the `HZFlags` string at `0xe9dea5`, section `3D`,
default 7, and stores the wrapper at its configuration object `+0x98` [BIN].
This establishes cross-stack provenance for the name and default.  The exact
resource consumer described below is from the Windows image.

The value is a three-bit depth-class mask:

| stored depth bits | required `HZFlags` bit | value |
|---:|---:|---:|
| 16 | bit 0 | `0x1` |
| 32 | bit 1 | `0x2` |
| 64 | bit 2 | `0x4` |

Default 7 enables all three cases.  These are internal stored-bit classes;
they are not three separately exposed OpenGL formats.

## Depth layout and metadata allocation

Layout constructor `0x461ea..0x46278` first requires resource-layout flag
`+0x14c bit 5`.  It reads the `HZFlags` wrapper through settings `+0x334`, then
matches [BIN]:

* bit 0 against format identifiers `0x5c` and `0x5d`, independently joined to
  `D16` and `D15S1`;
* bit 1 against `0x7c` and `0x7d`, independently joined to `D32` and the D24
  family;
* bit 2 against identifiers `0x3d` and `0x3e`.  This last comparison is kept
  as an internal format pair because its caller-side representation is not
  independently named.

On a match the constructor stores `(row_span + 0x3f) & ~0x3f` at layout
`+0x16c`, selecting 64-byte alignment.  This is a layout prerequisite tied to
the same mask; it does not itself allocate the auxiliary plane [BIN][DER].

Resource creator `0x51d3a..0x51d79` invokes allocator
`0x54be0..0x54ce9` only for format-class record `+4 == 4` and resource mode
`+4 == 5`.  It supplies the slot at resource `+0x38c`, clears the success
marker at `+0x39c`, and retains that marker only if creation succeeds [BIN].

Allocator `0x54be0` repeats the format-class test, maps stored-bit count
`resource+0x254` through the table above, and returns without allocating when
the corresponding bit is clear.  For an accepted depth resource it creates
internal format ID `0x0b`, independently joined by the format table to
`G16R16`, with [BIN][DER]:

```text
metadata_width  = (depth_width  + 7) >> 3
metadata_height = (depth_height + 3) >> 2
```

Thus each 32-bit metadata element covers an 8 by 4 block, or 32 depth pixels.
Examples are `320x240 -> 40x60`, `321x241 -> 41x61`, and
`1920x1080 -> 240x270`.  The format identity and coverage do not reveal the
meaning of the two 16-bit fields; a new driver must treat the contents as
opaque hardware metadata.

## Final `ba/bb` binding

Resource/state binder `0xaaa7d..0xaab1b` checks the depth resource's `+0x39c`
success marker.  When present it follows `+0x38c` to the auxiliary resource
and its layout at `+0x3c`.  If selector-12 cache-valid bit `+0x11c bit 20` is
clear, it calls view builder `0x467e0` with selector 12.  The resulting cached
address, relocation backing, and descriptor at layout
`+0x98/+0x9c/+0xa0` are copied to the surface shadow
`+0x74/+0x78/+0x7c` [BIN].

The already closed surface lowerer then maps compiled fields
`+0x228/+0x22c/+0x230` to those shadow fields, sets dirty bit `0x20`, and
emitter `0xfb920` emits address/descriptor indices `ba/bb` with relocation
selector 7.  Ordinary depth stays in the separate `b8/b9` pair; stencil is
packed in the depth format and controlled by `a8/a9/aa` [BIN][DER].

This complete chain is:

```text
3D\HZFlags
  -> stored-depth-bit eligibility and 64-byte layout alignment
  -> opaque format-0x0b metadata at ceil(width/8) x ceil(height/4)
  -> auxiliary resource +0x38c / success +0x39c
  -> selector-12 view
  -> surface shadow +0x74/+0x78/+0x7c
  -> final ba/bb
```

## New-driver contract

Hierarchical Z is an optimization, not an OpenGL capability.  The first Mesa
and DRM implementation may disable it by omitting the auxiliary allocation and
emitting the established null `ba/bb` state.  Ordinary hardware depth testing,
depth writes, and packed stencil continue through `b8/b9` and `a8/a9/aa`.
This path follows the vendor's explicit disabled-bit behavior and avoids
guessing metadata contents [BIN][DER].

Enabling HZ requires all of the following:

1. apply the corresponding `HZFlags` stored-bit gate and 64-byte alignment;
2. allocate format `0x0b` metadata with the exact rounded dimensions;
3. build selector 12 and bind it only while the parent depth resource is live;
4. serialize metadata use with clears, depth writes, copies, resolves, and
   depth sampling;
5. validate initial contents, partial-clear invalidation, parent/auxiliary
   lifetime, and read-after-write behavior on hardware.

The allocation and bind schema is closed.  Static code has not yet assigned a
meaning to each metadata bit or proved its initialization/invalidation
protocol.  No new driver should enable HZ until those maintenance operations
are captured or reproduced by a bounded differential test.  This remaining
boundary affects performance only; it does not block the direct hardware
depth path required for the fixed-function OpenGL subset.
