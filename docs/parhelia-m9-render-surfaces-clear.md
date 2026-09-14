# M9 color/depth surfaces, clear, viewport, and scissor

This note closes the static frontend-to-packet joins for color render targets,
the depth attachment, and viewport/scissor state in the saved M-Series Windows
driver.  It also identifies the Direct3D clear ABI and its dispatch chain.  It
keeps the resource-specific clear methods separate because their final packet
implementations have not yet been resolved statically.

Confidence labels match `parhelia-m9-3d-static-re.md`: **[BIN]** is directly
visible in the binary, **[DER]** is deterministic data-flow derived from those
instructions, and **[INF]** still needs a differential capture.  Addresses are
section-relative virtual addresses in `MSerX/MxMD.dll`; an indexed state write
uses MMIO offset `0x1000 + index * 4`.

## Color render-target join

The draw lowerer at `0x181fa0` receives a state object whose `+0x24` member
selects the batch surface shadow (`object + 0x3e8`) and whose `+0x20` member is
the nested compiled resource state [BIN].  The fragment-output attachment list
has a byte count at `[object+0x28]+0x361` and four-byte declarations beginning
at `+0x362`.  For every declaration [BIN]:

* bits `15:0` select a compiled resource entry;
* `((declaration >> 16) & 0x3f) - 0x10`, divided by four, selects color slot
  `0..3`;
* declaration value `0xfffe` is the unbound sentinel.

The copy at `0x182096..0x1820f3` is exact [BIN]:

| compiled resource field | surface-shadow destination | final index | role |
|---:|---:|---:|---|
| `+0x1bc + 8*r` | `+0x38 + 8*s` | `b0,b2,b4,b6` for `s=0..3` | tagged address word |
| `+0x1c0 + 8*r` | `+0x3c + 8*s` | relocation paired with the preceding index | relocation backing |
| `+0x1fc + 4*r` | `+0x58 + 4*s` | `b1,b3,b5,b7` for `s=0..3` | surface descriptor |

Here `r` is the declaration's resource index and `s` is its output slot.  The
address copy preserves the high three tag bits and replaces the low 29 bits;
the exact XOR-mask sequence is at `0x1820a1..0x1820d9`.  A private override at
`0x1820f6..0x18211c` can replace address bits `28:16` while retaining those
high tag bits.  The lowerer grows the live attachment count at surface shadow
`+0x34` to at least `s+1`, sets dirty bit `1 << s`, and emitter `0xfb920`
relocates each address using selector `s+2` before issuing the pair [BIN].

The final hardware map is therefore [BIN][DER]:

| slot | indices | MMIO offsets |
|---:|---|---|
| 0 | `b0,b1` | `0x12c0,0x12c4` |
| 1 | `b2,b3` | `0x12c8,0x12cc` |
| 2 | `b4,b5` | `0x12d0,0x12d4` |
| 3 | `b6,b7` | `0x12d8,0x12dc` |

The descriptor's seven-bit format code is exactly bits `31:25`.  At
`0x182120..0x18215c`, the lowerer computes `(descriptor >> 25) - 0x30`, bounds
it by `0x4a`, and feeds that value plus descriptor bits `20:19` to `0x181830`.
The helper's three-bit result is packed at `3*s` into per-batch common-state
shadow `+0x14`, emitted by `0xf4f50` as final index `89` (MMIO `0x1224`).  An
out-of-range format produces value seven [BIN].  This independently agrees
with the format-code family recovered for texture descriptor index `78`,
including `A8R8G8B8=0x66`, `X8R8G8B8/R8G8B8=0x64`, `R5G6B5=0x4e`,
`A2B10G10R10=0x68`, and `A2R10G10B10=0x6a` [DER].

Static data flow does not yet split the color descriptor's remaining bits into
pitch, height, tiling, and allocation-layout fields.  The open driver may use
the proven address/relocation and format locations, but must not invent those
lower fields from the texture descriptor merely because the formats agree.

## Depth/stencil attachment join

The lowerer tests `[object+0x28]+0x372` for a depth/stencil attachment at
`0x1821f4..0x182201`.  When present, it installs two relocation-backed
descriptor pairs [BIN]:

| nested compiled fields | surface-shadow fields | final indices | MMIO offsets | dirty bit / relocation selector |
|---|---|---|---|---|
| `+0x21c,+0x220,+0x224` | `+0x68,+0x6c,+0x70` | address/descriptor `b8,b9` | `0x12e0,0x12e4` | `0x10` / `6` |
| `+0x228,+0x22c,+0x230` | `+0x74,+0x78,+0x7c` | address/descriptor `ba,bb` | `0x12e8,0x12ec` | `0x20` / `7` |

The address fields use the same low-29-bit address/high-three-bit tag
preservation as the color targets.  The adjacent words `+0x220` and `+0x22c`
are relocation backing, while `+0x224` and `+0x230` are emitted verbatim as
descriptors.  The first install is at `0x182201..0x182291`; the repeated update
path is at `0x182549..0x182620` [BIN].  If a depth attachment is declared but
its compiled binding must be reset, the code substitutes the global resource
pair at `0x20d51c/0x20d520` while retaining the compiled descriptors [BIN].

The fixed depth/stencil controls are separate.  `0x181900` derives final
`a8,a9,aa`; their complete bit layout is documented in
`parhelia-m9-3d-static-re.md`.  Depth surface format codes occupy the same
seven-bit family used by the color and texture paths: the recovered table
includes `D16=0x5c`, `D15S1=0x5d`, `D32=0x7c`, and the D24 family at `0x7d`
[BIN][DER].  The two descriptors above remain distinct even when both refer to
one API depth/stencil attachment.

Static code proves that `b8/b9` and `ba/bb` are the two backing planes of the
active depth attachment.  It does not prove which is the ordinary Z plane and
which is auxiliary metadata, nor the pitch/tiling bits within either
descriptor.  Those names require a capture that varies depth format, pitch,
tiling, and hierarchical-Z allocation independently.

## Viewport and scissor through final packets

Compiler `0x138c10` reads two rectangles [BIN]:

| rectangle | validator inputs | construction |
|---|---|---|
| viewport | `+0x108,+0x10c,+0x110,+0x114` | `x,y,x+width,y+height` |
| scissor | `+0x124,+0x128,+0x12c,+0x130` | `x,y,x+width,y+height` |

Helper `0x138cd0` tests validator `+0x8c bit 2`, already joined to
`SCISSORTESTENABLE`.  When enabled, helper `0xf1790` intersects the scissor and
viewport rectangles; when disabled, the viewport passes through.  An optional
subsurface/origin mode adds `+0x118/+0x11c`.  The helper then clamps negative
coordinates to zero and clips against the width/height reported by each of up
to eight attachments referenced from validator `+0x1cc` [BIN].

The result is packed as unsigned 16-bit coordinates with exclusive right and
bottom endpoints [BIN][DER]:

| validator output | nested offset | surface-shadow offset | final index / MMIO | value |
|---:|---:|---:|---|---|
| `+0x410` | `+0x258` | `+0xa4` | `f5 / 0x13d4` | `left | top << 16` |
| `+0x414` | `+0x25c` | `+0xa8` | `f6 / 0x13d8` | `right | bottom << 16` |
| `+0x418` | `+0x260` | `+0xac` | `c4 / 0x1310` | internal transformed left/top pair |
| `+0x41c` | `+0x264` | `+0xb0` | `c5 / 0x1314` | internal transformed right/bottom pair |

`0x138cb6` sets nested dirty bit `0x04000000`.  Lowerer `0x181fa0` tests it at
`0x18283e`, copies the four words at `0x182846..0x18286a`, and sets surface
dirty bit `0x400` at `0x182870`.  Emitter `0xfb920` writes header
`c5c4f6f5` and those four values [BIN].  In the ordinary zero-origin path,
`c4/c5` duplicate `f5/f6`; their different transformed values in the private
origin mode should be preserved until that mode is identified.

There is no minus-one adjustment: `0x138c10` adds width and height directly.
Consequently a Mesa scissor `(x,y,w,h)` must send `(x,y,x+w,y+h)`, after the
API-to-framebuffer Y conversion appropriate to the current framebuffer.

## Clear dispatch and payload

The DP2 dispatch table is initialized at `0xdd8e0`; table entry 42 at
`0x20ca48` selects `0xdf9d0`, the `D3DDP2OP_CLEAR` parser [BIN].  Its payload
contract is [BIN]:

| byte offset | type | meaning |
|---:|---|---|
| `+0x00` | `u32` | clear flags, low four bits consumed |
| `+0x04` | `u32` | packed color |
| `+0x08` | `float` | depth |
| `+0x0c` | `u32` | stencil |
| `+0x10` | array | 16-byte clear rectangles |

The rectangle count is at parser record `+0x14`, and the parser advances by
`0x10 + count*0x10`.  It calls vtable slot `+0x04` on `[parser+0x24]` [BIN].
The draw/context vtable at `0x1f4abc` joins that slot to clear routine
`0x13de20`; the neighboring entries are `0x13ca20`, draw `0x13e790`, and
indexed draw `0x13e7e0` [BIN].

`0x13de20` dispatches flags exactly as follows [BIN]:

| flag | D3D meaning | action |
|---:|---|---|
| `0x1` | `D3DCLEAR_TARGET` | call color clear `0x13e0f0` |
| `0x2` | `D3DCLEAR_ZBUFFER` | participate in depth/stencil clear `0x13e4c0` |
| `0x4` | `D3DCLEAR_STENCIL` | participate in depth/stencil clear `0x13e4c0` |
| `0x8` | private rectangle preprocessing | call `0x13def0` |

Color clear `0x13e0f0` walks up to eight color attachments from the surface set
at `[arg+8]+0xf8` and `+0x18+4*slot`.  It clamps each rectangle with attachment
width/height methods `+0x24/+0x28`, obtains a color mask from `+0x130/+0x100`
or defaults to `0xf`, and calls attachment method `+0x8c` at
`0x13e436..0x13e470` with the rectangle and converted color metadata [BIN].
Attachment kinds returned by method `+0x3c` select conversion method `+0x7c`
for kind 2, `+0x88` for kind 3, and `+0x80` for kind 5.  The first render
target may apply sRGB conversion when surface-set `+0xf4 bit 0x800` is set
[BIN].

Depth/stencil clear `0x13e4c0` takes the attachment at surface-set `+0x3c`,
uses method `+0x58` to form format-specific depth/stencil metadata from flags,
depth, and stencil, then calls attachment method `+0x64` for every clipped
rectangle at `0x13e73e..0x13e74a` [BIN].

These are resource virtual calls, not raw indexed command writes.  The common
resource constructor `0xa32a0` installs interface vtable `0x1f3ba0` at resource
subobject `+0x18`; that is the exact subobject passed to the render-target
binder at `0x2d6d0..0x2d721` [BIN].  The vtable therefore closes the next clear
dispatch level [BIN][DER]:

| attachment vtable slot | concrete function | observed action |
|---:|---:|---|
| `+0x64` | `0xa6c10` | construct depth/stencil clear state and call `0xb4fe0` |
| `+0x8c` | `0xa7b70` | color clear/lazy-clear selection, falling through to `0xa7ee0` |

Color method `0xa7b70` recognizes a whole-surface rectangle when width times
height exceeds `0x280000` pixels.  That path flushes incompatible prior state,
then caches the four converted clear words at subobject `+0x318..+0x324`, the
rectangle at `+0x328..+0x334`, the color mask at `+0x338`, and sets pending bit
`+0x34c bit 0` [BIN].  Other clears flush pending state through `0xa7ce0` and
call `0xa7ee0`.  `0xa7ee0` routes ordinary matching-size clears through
`0xa48b0`; a size-mismatch branch constructs a conversion operation through
`0xb38d0`, `0xb4690`, and `0xab620` [BIN].

This vtable join proves that the vendor batches some full color clears lazily
and that the clear path is deeper than one indexed packet.  It still does not
prove a hardware clear opcode or final blit/fill packet: the command-producing
leaves below `0xb4fe0` and `0xa48b0` remain unflattened.  A driver implementation
must not turn the DP2 payload into a guessed command.

## Driver-ready fields and remaining blockers

The following can be implemented from static evidence [BIN][DER]:

* four color attachment packet pairs `b0/b1` through `b6/b7`, their relocation
  selectors, dirty bits, slot declaration mapping, and descriptor format bits;
* two depth-attachment packet pairs `b8/b9` and `ba/bb`, their source fields,
  relocation selectors, and dirty bits;
* effective viewport/scissor calculation, exclusive endpoints, dirty-state
  propagation, and final `f5/f6/c4/c5` packet indices;
* clear API parsing, flag separation, rectangle clipping, attachment iteration,
  color-mask selection, and depth/stencil metadata dispatch.

The remaining hardware blockers are narrowly defined:

1. Color and depth descriptor pitch, height, tiling, and allocation-layout
   bitfields below the proven format field.
2. The exact ordinary-Z versus auxiliary-plane meaning of depth pairs
   `b8/b9` and `ba/bb`.
3. Concrete command emission below color leaf `0xa48b0` and depth/stencil leaf
   `0xb4fe0`, including any required cache transitions.  The attachment
   vtable itself and the lazy full-color-clear decision are now exact.
4. The private transformed meaning of `c4/c5` when the nonzero-origin path is
   active.  Ordinary framebuffer viewport/scissor operation does not depend on
   this gap because the zero-origin path duplicates `f5/f6`.

Items 1--3 still prevent a general hardware render-target/depth/clear
implementation from being called complete.  They can be closed by flattening
the two remaining clear leaves and descriptor builders, or by controlled
captures that vary one field at a time; no additional shader-compiler reverse
engineering is needed for these surface-state questions.
