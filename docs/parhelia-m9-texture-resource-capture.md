# M9 live texture and resource matrix

This experiment exercises the saved Linux M9 vendor ICD on the M9148 LP
(`102b:0540`) with direct OpenGL 2.0 rendering.  It joins visible sampling,
OpenGL errors, vendor allocations, and the exact indexed state emitted for
each target.  The phase-one evidence is under
`captures/m9x-linux-texture-resources-20260914`; its self-test is
`tools/m9-texture-resource-evidence.py --self-test`.

## Visible and API results

| case | operation | live result |
|---:|---|---|
| 0 | compressed-format inventory | count reports 4; initial zero-filled output remained ambiguous |
| 1 | 13x7 NPOT, complete 13x7/6x3/3x1/1x1 chain | all four selected levels sampled correctly |
| 2 | 13x7 rectangle, unnormalized coordinates | samples correctly; level 1 is `GL_INVALID_VALUE`; repeat and mip filter are `GL_INVALID_ENUM` |
| 3 | six 8x8 cube faces with S/T gradient | every face and orientation check passes |
| 4 | 7x5x4 3-D texture | all slices and R-repeat selections pass |
| 5 | 8x8 depth texture | raw 0.25/0.75 and LEQUAL fail/pass outcomes pass |
| 6 | clamp-to-border with `(0.125,0.25,0.75,1)` | inside and outside samples pass |
| 7 | LOD bias `-2,+100,-100`, min 3, max 2 | all selected mip colors pass |
| 8 | `GL_RGB10_A2` packed upload | upload/readback calls accepted and sampling passes; packed readback differs |
| 9/10 | direct DXT1/DXT5 tokens | both rejected with `GL_INVALID_ENUM`, consistent with absent S3TC extension |

Framebuffer FNV-1a checksums are asserted by the verifier.  The vendor reports
`GL_ARB_texture_compression` but no `GL_EXT_texture_compression_s3tc`.  The
first inventory run reported count four but used a zero-initialized result
array, then observed four zeros.  That is not yet evidence that zero is a
returned format token: a sentinel-filled follow-up must determine whether the
driver writes the array at all.

## Target and descriptor words

The six values below are the last writes to indices `70,78,79,7a,7c,7d` in
the selected draw.  Index `78` is the relocated texture base.  Indices `79`
and `7a` are its descriptor dimensions/format fields; `7c` contains sampler
wrap/filter state and `7d` contains LOD controls.

| target/use | `70` | `78` | `79` | `7a` | `7c` | `7d` |
|---|---:|---:|---:|---:|---:|---:|
| NPOT level 0, 13x7 | `000c0118` | `11f81000` | `c801000d` | `00010007` | `00009200` | `00000000` |
| NPOT level 1, 6x3 | `000c0118` | `11f91000` | `c8010006` | `00010003` | `00009200` | `00000000` |
| NPOT level 2, 3x1 | `000c0118` | `11f91000` | `c8010003` | `00010001` | `00009200` | `00000000` |
| NPOT level 3, 1x1 | `000c0118` | `11f91000` | `c8010001` | `00010001` | `00009200` | `00000000` |
| rectangle 13x7 | `00040118` | `11f81000` | `c801000d` | `00010007` | `12409200` | `0ff80000` |
| cube 8x8 | `00040318` | `11f81000` | `c8090008` | `00010008` | `02409200` | `0ff80000` |
| volume 7x5x4 | `00040218` | `11f81000` | `c8110007` | `00040005` | `02409200` | `0ff80000` |
| depth raw 8x8 | `00040118` | `11f81000` | `fa010008` | `00010008` | `00009200` | `0ff80000` |
| depth compare | `14040218` | `11f81000` | `fa010008` | `00010008` | `00009200` | `0ff80000` |

The target dimension changes the combined operation/resource word at `70` and
the layer/depth field at `7a`: cube uses `79=c809...`, while the 3-D texture
uses `79=c811...` and places four in `7a[31:16]`.  Enabling depth compare changes
`70` from `00040118` to `14040218` without changing the depth resource
descriptor.  These are implementation-ready live joins for target selection
and shadow compare.

Border mode emits `7c=0fc09200` plus
`7e=40002000, 7f=ff00bf00, 80=ffffffff, 81=00000000`.  The requested normalized
RGBA border was `(0.125,0.25,0.75,1)`; the four packed words must be copied as
the measured hardware representation until their conversion is independently
decoded.

For a 64x64 complete mip chain, index `7d` was:

| request | `7d` |
|---|---:|
| LOD bias -2 | `0ff803c0` |
| LOD bias +100, saturated | `0ff801ff` |
| LOD bias -100, saturated | `0ff80200` |
| min LOD 3, max LOD 4 | `04018000` |
| max LOD 2 | `02000000` |

This validates the static signed ten-bit bias and two nine-bit bound fields in
`parhelia-m9-texture-state.md` on live hardware.

## Allocation classes

Every ordinary resource used allocation type `0x10`, placement 4.  Rectangle
alone used type `0x11`, placement 4.  Small 2-D, rectangle, depth, border, and
RGB10_A2 requests received an `0x800` byte allocation aligned to `0x80`.
The 7x5x4 volume received `0x2000`.  Cube and 64x64 mip-chain resources each
received `0x2c0000` aligned to `0x16000`.  These unexpectedly large allocations
are vendor layout policy and should not be copied into a new allocator without
mapped-content evidence.

The odd NPOT updates show a further implementation boundary.  Level zero used
base `0x11f81000`; each later `glTexImage2D` call obtained type `0x10`, base
`0x11f91000`, size `0x800`, and the selected descriptor dimensions shrank for
the requested base level.  The capture does not yet show whether those repeated
responses refer to one reused backing or replacement allocations.

## RGB10_A2 precision boundary

The exact standardized API upload words, in texel order, were:

```text
021fa011 456ee44e 88be288b cc0d6cc8
0f5cb105 52abf542 95fb397f d94a7dbc
1c99c1f9 5fe90636 a3384a73 e6878eb0
29d6d2ed 6d26172a b0755b67 f3c49fa4
```

Their byte FNV-1a is `3dde8c3c`.  `glGetTexImage` returned successfully but
produced hash `155aedce`, and the rendered sample matched the requested color.
The emitted sample descriptor is exactly the same as the ordinary RGBA8 4x4
case: `70=00040118, 79=c8010004, 7a=00010004, 7c=00009200`; its allocation is
also the same type, size, and alignment.  This proves that this vendor OpenGL
path aliases the sample resource to its ordinary 8-bit hardware format.  It
does not by itself name the internal byte/channel order.  The packed
`glGetTexImage` values are an API conversion result and cannot be used as a
raw-storage dump.  The focused follow-up preserves both those returned dwords
and the VRAM bytes so the conversion and hardware order can be described
separately.

## Remaining capture gates

The next bounded pass uses sentinel-filled compressed-format output, preserves
the exact RGB10 packed readback, snapshots the complete 0x21000-byte linked
system resource for selected draws, and captures each texture allocation from
VRAM.  Until that pass is complete, the mip/cube/volume physical layouts and
the compressed-inventory bug boundary remain open.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
