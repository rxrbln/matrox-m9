# M9 hierarchical-Z static closure

This note closes the identity, allocation rule, initialization, invalidation,
ownership, synchronization, fallback, and final binding of the optional
`ba/bb` depth auxiliary surface.  The saved Windows M9148 driver calls the
feature `HZFlags`; its only metadata allocator creates an internal
`G16R16`-class plane at one element per 8 by 4 depth pixels, initializes it
through the ordinary clear executor, and selector 12 installs that plane in
the `ba/bb` packet pair.  It is therefore M9 hierarchical-Z metadata rather
than an API stencil plane or an unexplained second depth buffer [BIN][DER].

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
resource consumer described below is from the Windows image.  The Linux 1.15
monolith also contains the common depth-clear rectangle literals at
`0x3a88df/0x3a893c`, but a Linux-side HZ allocation/ownership consumer has not
been independently joined; do not transfer Windows object offsets into that
64-bit image [BIN].

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

## Initialization and the conservative image

Successful allocator tail `0x54cd1..0x54cdf` calls initializer
`0x55070..0x5509e` before returning success.  The initializer constructs the
complete auxiliary rectangle `{0, 0, auxiliary_width, auxiliary_height}` and
passes wrapper `parent+0x38c` to clear builder `0x54e80` [BIN].  The wrapper is
four dwords:

| offset | role |
|---:|---|
| `+0x38c` | auxiliary resource pointer |
| `+0x390` | parent depth resource pointer |
| `+0x394` | auxiliary-needs-refresh marker |
| `+0x398` | non-owning alias marker |

`0x54e80` obtains the parent depth format through wrapper dword 1, converts
the ordered pair `(0.0, 1.0)` through `0x550b0`, targets wrapper dword 0, and
submits the full rectangle through depth/stencil clear leaf `0xb4fe0` [BIN].
All six format identifiers admitted by `HZFlags` take the generic converter
branch.  Its scale constant is exactly `16777215.0` at `0x1f5240`, and the
packed result for `(0.0, 1.0)` is [BIN][DER]:

```text
lo = (truncate(0.0 * 16777215) >> 11) & 0x1ffe
hi =  truncate(1.0 * 16777215)        & 0x00ffe000
metadata_clear_word = (lo | hi) >> 1 = 0x007ff000
```

The common clear executor tries the already closed direct page fill, GPU
rectangle draw, and CPU fallback in that order.  On success `0x54e80` clears
wrapper `+0x394`; on failure it leaves the marker unchanged, so a dirty
transition remains retryable.  The bit-level meaning of `0x007ff000` remains
opaque, but its role is exact: it is the vendor's conservative full-range
metadata image [BIN][DER].

## Depth clears and non-clear invalidation

Depth-clear dispatcher `0xa8320` first clears the ordinary depth attachment
through `0xa48b0`.  When `+0x39c` says HZ exists, tail
`0xa8371..0xa83b5` then invokes companion `0x54f80` with the same depth value,
masks, and rectangle [BIN].  The companion always operates on the complete
auxiliary extent; it does not try to translate an arbitrary depth rectangle
to 8-by-4 metadata cells:

* when the clear rectangle's width or height differs from the stored parent
  dimensions, `0x54fd7..0x55002` calls `0x54e80` and replaces the whole
  auxiliary image with conservative word `0x007ff000`;
* when both extents equal the stored parent dimensions,
  `0x55005..0x55064` converts the supplied clear depth and calls `0x54d00` to
  seed the whole auxiliary image with the format- and mask-aware clear state.

This closes partial-clear safety without assigning meanings to the two
16-bit metadata fields: partial clears discard acceleration information for
the whole image, while full clears can seed useful bounds [BIN][DER].

The resource transition routine `0x55370..0x5553f` provides the same
conservative recovery for writes which bypass coupled depth/HZ rendering.  It
checks `+0x39c`, then, when `+0x394` is set, calls `0x54e80` over the complete
auxiliary allocation.  CPU-visible unlock tail `0x55b40..0x55b67` sets
`+0x394 = 1`; non-clear completion sites `0x4a368..0x4a393` and
`0x4c67a..0x4c6a5`, and depth-copy path `0xc590e..0xc592a`, do likewise before
the next transition [BIN].  A successful conservative clear resets the marker.

Ordinary depth writes need no software metadata rewrite: selector 12 is bound
beside the ordinary depth view, so hardware maintains both while rendering.
Traversal helper `0xa6dc4..0xa6e13` invokes the same resource callback with
the same three ordering/residency arguments first for the parent depth
resource and then, when present, for its `+0x38c` auxiliary.  Auxiliary clears,
ordinary depth clears, and draws therefore share command-stream ordering and
resource tracking; no separate HZ fence or CPU shadow appears in the vendor
path [BIN][DER].

## Allocation lifetime and aliases

Resource constructor `0x50cb0` zeros `+0x38c`, `+0x398`, and `+0x39c`.  Clone path
`0x5164c..0x51689` copies the auxiliary pointer, parent link, and refresh
marker, and sets clone `+0x398 = 1`.  Destructor
`0x51232..0x5125f` releases `+0x38c` only when HZ exists and `+0x398 == 0`,
then clears the auxiliary pointer and presence marker.  Thus the creating
depth resource owns the auxiliary allocation; derived resource objects are
explicit non-owning aliases and must not free it [BIN].

Allocator failure and configuration disable are deliberately non-fatal.
`0x54be0` returns zero when the relevant `HZFlags` bit is clear or auxiliary
creation fails; caller `0x51d6d..0x51d79` clears `+0x39c` and continues normal
depth-resource construction.  Every binding and tracking site checks the
presence marker before following `+0x38c`.  Ordinary depth remains bound at
`b8/b9`, so this is the vendor's exact performance-only fallback [BIN].

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

Hierarchical Z is an optimization, not an OpenGL capability.  A Mesa and DRM
implementation may disable it by omitting the auxiliary allocation and
explicitly emitting null `ba/bb` state.  Ordinary hardware depth testing,
depth writes, and packed stencil continue through `b8/b9` and `a8/a9/aa`.
This follows the vendor's explicit disabled-bit and allocation-failure
behavior [BIN][DER].

Enabling HZ requires all of the following:

1. apply the corresponding `HZFlags` stored-bit gate and 64-byte alignment;
2. allocate format `0x0b` metadata with the exact rounded dimensions;
3. build selector 12 and bind it only while the parent depth resource is live;
4. initialize and conservatively invalidate the complete plane with
   `0x007ff000`, using the established clear executor;
5. on a complete depth clear, seed the complete plane through the
   format/mask-aware `0x54d00` path; on a partial clear, discard the whole
   plane to the conservative image;
6. set the refresh marker after CPU, copy, or other uncoupled writes and clear
   it only after the auxiliary reset succeeds;
7. track parent and auxiliary together and make every alias non-owning;
8. validate the statically recovered protocol on hardware before enabling it
   by default.

The allocation, initialization, invalidation, binding, tracking, and lifetime
schema is statically closed.  The exact meaning of each metadata bit and the
performance effect of the full-clear seed remain unknown; neither is needed
to reproduce the vendor protocol byte for byte.  Dynamic validation is still
required before enabling HZ because a wrong metadata value can reject visible
fragments.  Linux independently proves the feature name/default and shared
clear backend, but its 64-bit resource consumer remains unjoined.  These
remaining boundaries affect optional performance only; they do not block the
direct hardware depth path required for the fixed-function OpenGL subset.
