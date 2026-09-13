# Matrox M9/Phoenix 3D static reverse engineering

This note is the pre-capture specification recovered from the saved Matrox
M-Series Windows and Linux drivers.  It covers indexed command construction,
the 3D state banks, shader program upload, the vendor compiler interface, and
the compiler's Iris instruction vocabulary.  It deliberately separates
compiler IR from the native eight-dword hardware instruction record.

Confidence labels are:

* **[BIN]**: directly visible in a saved binary or its disassembly.
* **[DER]**: a deterministic result derived from binary instructions.
* **[INF]**: a likely meaning which still needs a differential capture or an
  M9148 test.

Addresses below are preferred-image virtual addresses for `MxMGLX.dll` and
section-relative virtual addresses for `MxMD.dll`.  They are useful for finding
the code again; they are not M9 MMIO addresses.  Named Linux functions use the
virtual `.text` addresses from their respective ELF binary.

## Binary identity

The principal inputs are:

| file | SHA-256 | role |
|---|---|---|
| `MSerX/MxMGLX.dll` | `d4e04dc371809f079651e9938b285878afd42acabcba2eaabef2e65d218e9f86` | OpenGL ICD, GLSL/ARB compiler and GL lowering |
| `MSerX/MxMD.dll` | `b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43` | D3D/GDI driver, packet and native-program emission |
| Linux `xserver/1_13_0/m9x_drv.so` | `3878475450ffad5cd648cc037cc65c29fcb07b5e86caf41ccf5983ef8e570273` | Linux GL/X implementation and submission path |
| Linux `KernelLibraries.o_shipped` | `bec4490ff72b29887400e988a26e107e80ebb0b11e9d0b437267bec9db5b26e7` | proprietary kernel hardware core |

The Windows build paths call this family `phoenix`.  The compiler source names
left in the ICD include `PsCompile.cpp`, `PsTokenStream.h`, `PsIris.cpp`,
`PsIris.h`, `PsTranslate.cpp`, `PsTrace.cpp`, `PsIrisTrace.cpp`, and
`PsTranslateBuiltins.cpp`.  Its public version string is `1.10 Matrox GLSL
compiler` [BIN].

## Indexed command packets

An ordinary state packet begins with a dword containing four byte-sized register
indices in little-endian execution order.  `0x45` ends a short header and fills
the remaining bytes [BIN].  An index maps to the engine window as
`0x1000 + index * 4` [BIN].  Examples are:

| header dword | writes | engine offsets |
|---:|---|---|
| `0x45454569` | `69` | `0x11a4` |
| `0x45456a69` | `69, 6a` | `0x11a4, 0x11a8` |
| `0x457a7978` | `78, 79, 7a` | `0x11e0, 0x11e4, 0x11e8` |
| `0x63626160` | `60, 61, 62, 63` | `0x1180..0x118c` |

The last example has no terminator because all four slots are live.  Command
buffer bookkeeping places a count/control dword before the header in the paths
examined.  The exact outer record semantics belong to the requester/DMA ABI and
must not be inferred from `0x45` alone.

Frequently used banks therefore occupy these engine offsets [DER]:

| indices | engine offsets | recovered role |
|---|---|---|
| `60..67` | `0x1180..0x119c` | eight program/vector data ports |
| `68..6e` | `0x11a0..0x11b8` | program selector, address, and control bank |
| `70..73` | `0x11c0..0x11cc` | four texture-unit selectors |
| `78..87` | `0x11e0..0x121c` | shared texture descriptor fields |
| `90..9f` | `0x1240..0x127c` | 16-entry common state bank |
| `a8..be` | `0x12a0..0x12f8` | render/depth surface bank, with unused indices between proven groups |
| `c0..c7` | `0x1300..0x131c` | surface and adjacent state |
| `e3,e6` | `0x138c,0x1398` | command allocation/resource result and frequent transition command |

[`tools/m9-packet-scan.py`](../tools/m9-packet-scan.py) finds candidate
`0x45`-terminated header constants in an x86 binary or existing Intel-syntax
`objdump` output.  It intentionally omits full four-index headers because an
arbitrary immediate cannot be classified safely without data-flow context.
Terminated matches also need contextual confirmation because unrelated x86
immediates can accidentally have the same byte pattern.

As a cross-build check, the scanner finds 355 sites and 143 distinct terminated
header immediates in `MxMD.dll`, and 180 sites/82 immediates in Linux
`xserver/1_13_0/m9x_drv.so`.  Seventy-five exact header immediates occur in both
the independently compiled 32-bit Windows and 64-bit Linux drivers [DER].  The
shared set includes the texture, surface, program-port, `18/19`, `1a/42`,
`2c/2d`, `2e/2f`, `e3`, `e6`, and main raster groups described below.  This
substantially lowers the chance that those constants are x86 data coincidences.

## The driver's context-index template

`MxMD.dll` function `0x65430` constructs a 248-byte template at runtime and
copies it into each object.  This is the densest static inventory of 3D-visible
indices in the binary [BIN].  The following rows retain its dword boundaries;
`--` denotes a `0x45` stop byte.  Repeated `47` bytes are preserved because
their command meaning is not yet proven.

```text
47 47 -- -- | f0 ef ed ee | ec f2 f3 f4 | 47 47 47 47
b0 b1 b2 b3 | b4 b5 b6 b7 | b8 b9 ba bb | be bc bd --
47 47 47 47 | a8 a9 aa ab | ac ae f5 f6 | af c0 a7 a5
c4 c5 c6 c7 | 47 -- -- -- | a1 a3 a4 ad | 47 47 47 47
88 89 8a c2 | c3 8c 8d 8e | 74 78 79 75 | 78 79 76 78
47 -- -- -- | 79 77 78 79 | 47 47 47 47

70 78 79 7a | 7c 7d 82 83 | 84 85 80 81 | 7e 7f 87 --
47 47 47 47 | 71 78 79 7a | 7c 7d 82 83 | 84 85 80 81
7e 7f -- -- | 47 47 47 47 | 72 78 79 7a | 7c 7d 82 83
84 85 80 81 | 7e 7f -- -- | 47 47 47 47 | 73 78 79 7a
7c 7d 82 83 | 84 85 80 81 | 7e 7f -- --

47 47 47 -- | ea eb d0 d1 | ed d3 d4 -- | 8f 8b d6 c1
47 47 47 47 | 90 91 92 93 | 94 95 96 97 | 98 99 9a 9b
9c 9d 9e 9f | 47 47 47 47 | 68 69 6a 6b | 6c 6d 6e --
60 61 62 63 | 64 65 66 67 | 47 47 47 -- | f1 d7 f3 --
d7 d8 d9 da | db dc dd de | 47 -- -- -- | d2 a2 d5 e8
```

The four nearly identical blocks selected by `70`, `71`, `72`, and `73` are
strong evidence for four texture units in this particular state path [DER].
Indices `78..87` form their shared descriptor payload.  The contiguous
`90..9f` bank contains 16 state values, while `68..6e` plus `60..67` is the
program/parameter upload bank [DER].  Names for the other groups remain
provisional.

## State emitters and register clusters

The following `MxMD.dll` functions build real packet payloads.  This table is a
map for later annotation, not a final register-name header.

| function | observed indices | static conclusion |
|---:|---|---|
| `0xf4350` | `f0`, dynamic `70 + unit`, `78..7a`, `7c,7d,87`, `7e..85`, `c0` | texture/sampler state, four selectable units [BIN][DER] |
| `0xf4f50` | `ef`, `e6,8f`, `90..92`, dynamic `00`, `70`, `f0` | common 3D setup and state-bank selection [BIN] |
| `0xf5730`, `0xf59b0` | `c2,c3,8f`, `e6,e3` | resource/cache transition path [BIN][INF] |
| `0xf6f20` | `ec,f4,ea,eb`, `30`, `28`, repeated `23`, `d0,d1`, then program helpers | central dirty-state emitter [BIN] |
| `0xf74d0` | `69,6a,6d,e3,68,60..67` | native program address/control and inline upload [BIN] |
| `0xf78c0` | `68,6b,6c,60..67` | auxiliary program/constant records [BIN] |
| `0xf7c60` | `68,60..67` | another complete auxiliary record [BIN] |
| `0xfb920` | `b8..be`, `24`, `bc,bd`, `af`, `a8..ae`, `f1` | render/depth surface group [BIN][INF] |
| `0xfc0e0` | `f0,ef,d7`, `ee`, `22`, `a7,c6` | late raster state group [BIN] |
| `0xfc3a0` | `a5,d3` | paired state [BIN] |
| `0xfc620`, `0xfcb60` | `44,2e,2f`, with `e6` in the latter | stream or launch candidate [BIN][INF] |
| `0xfc8f0` | `1a,42`, sometimes followed by `e6` | stream or launch candidate [BIN][INF] |
| `0xfb260` | `19,18` | paired input/geometry state candidate [BIN][INF] |

Index `e6` occurs in many state transitions and draw builders.  Its frequency
supports a barrier/flush/synchronization role, but the value encoding is not yet
known.  Indices `2c/2d` occur as a pair in many render paths and agree with the
earlier sub-buffer start/end finding.  Neither should be admitted to a kernel
command whitelist until its address and length constraints are understood.

Top-level commit function `0xfadf0` emits these blocks in the fixed order
`0xfc0e0` (late state), `0xfb920` (surfaces), `0xf4f50` (common banks),
`0xf6f20` (program/vector state), then `0xfb260` (paired input state), and
finally clears the context dirty word at `+0xc` [BIN].  The open driver should
preserve this ordering until captures prove which dependencies can be relaxed.

### Texture shadow layout

The texture emitter at `0xf4350` exposes its complete per-unit dirty-state
layout [BIN].  The object at `edi` uses `+0xc` as the unit number and its first
dword as a dirty mask.  It clears that mask after emission.  The individual
groups are:

| dirty bit | indices | source fields |
|---:|---|---|
| common | dynamic `70 + unit` | `+0x10` |
| `0x02` | `78,79,7a` | `+0x4c`, `+0x50`, plus relocated resource `+0x44/+0x48` |
| `0x04` | `7c,7d,87` | `+0x14`, `+0x18`, `+0x54` |
| `0x08` | `82,83,84,85` | `+0x2c`, `+0x30`, `+0x34`, `+0x38` |
| `0x10` | `80,81,7e,7f` | `+0x24`, `+0x28`, `+0x1c`, `+0x20` |
| `0x20` | `c0` plus relocation | zero, then resource `+0x3c/+0x40` |

The emitter also rewrites bits `14:13` of the current `f0` value with the unit
number before sending it.  The relocation callback receives selectors
`unit + 0x0b` for the first resource and `unit + 0x0f` for the second [BIN].
This gives the Mesa state encoder an exact shadow grouping and dirty-bit model
even though the filtering, wrap, format, pitch, and address bit fields inside
the values still need semantic names.

### Common state-bank shadow layout

The common emitter at `0xf4f50` maps another group of dirty bits and shadow
fields exactly [BIN]:

| dirty bit | indices | source fields or action |
|---:|---|---|
| `0x01` | `88,89,8a,c1` | `+0x10`, `+0x14`, `+0x18`, `+0x1c` |
| `0x02` | helper `0xf5730` | resource/cache transition group |
| `0x04` | helper `0xf59b0` | resource/cache transition group |
| `0x10` | `90,91,92` | `+0x3a8`, `+0x3ac`, `+0x3b0` |
| `0x20` | `94,95,96,97` | `+0x3b4`, `+0x3b8`, `+0x3bc`, `+0x3c0` |
| `0x40` | `98,99,9a,9b` | `+0x3c4`, `+0x3c8`, `+0x3cc`, `+0x3d0` |
| `0x100` | `9c,9d,9e,9f` | `+0x3dc`, `+0x3e0`, `+0x3d4`, `+0x3d8` |

The `0x10..0x100` groups therefore form four independently dirty slices of
the 16-entry `90..9f` bank [DER].  The nonsequential source order in the last
slice is intentional and must be preserved.  The surrounding path also emits
the fixed setup pair `e6 <- 0x04000001`, `8f <- 0x80000600`, dynamically
constructs indexed resource packets, and refreshes selectors `70` and `f0`.
Static evidence does not yet assign GL blend, viewport, raster, or linkage
names to the four slices, so the driver should retain neutral bank names until
the differential capture matrix labels them.

The late-state emitter at `0xfc0e0` supplies another compact exact map [BIN]:

| dirty bit in `+0x0` | indices | source fields |
|---:|---|---|
| `0x01` | `f0,ef,d7` | `+0x28`, `+0x20`, `+0x24` |
| `0x02` | `ee` | `+0x1c` |
| `0x04` | `ec,f2,f3,f4` | `+0x18`, `+0x2c`, `+0x30`, `+0x34` |
| `0x10` | `22` | `+0x38` |
| `0x20` | `a7,c6` | either `+0x3c/+0x40` or `+0x60/+0x64`, selected by `+0x68` |

Its secondary mask at `+0xc` bit `0x08` calls the `1a/42` emitter at
`0xfc8f0` with fields `+0x7c..+0x88`; bit `0x04` calls the `44/2e/2f/e6`
emitter at `0xfcb60`.  The function derives `+0x68` from two live object
pointers and fields in `+0x28`, so the `a7/c6` pair is conditionally selected
state rather than an independent dirty block [BIN].

### Direct3D semantic anchors

`MxMD.dll` function `0x103370` is the Direct3D render-state dispatcher.  It
takes the numeric `D3DRS` ID in `edx` and value in `eax`; IDs `2..219` pass
through the byte dispatch table at `0x104bf4` and handler jump table at
`0x104990` [BIN].  Static handler data flow gives exact frontend bit packing
for the following states.  Here `p2514` means the object pointer at context
`+0x2514`, and similarly for `p2518` and `p2528`.

| D3DRS ID | handler | frontend shadow update | dirty update |
|---|---:|---|---|
| `ZENABLE` 7 | `0x103857` | `p2514+0xc[6:5] = (v&1)<<5`; `p2528+4 bit2 = v&1`; `p2518+0x1a8 bit9 = v&1` | `p2514+4 \|= 0x100000`; `p2528+0 \|= 0x10`; `p2518+0 \|= 0x40000` |
| `ZWRITEENABLE` 14 | `0x1038cb` | `p2514+0xc bit1 = v&1` | `p2514+4 \|= 0x100000` |
| `ZFUNC` 23 | `0x1038db` | `p2514+0xc[10:7] = (v&0xf)<<7` | same `0x100000` |
| `SLOPESCALEDEPTHBIAS` 175 | `0x104267` | raw `p2528+0x20`; nonzero in `p2514+0xc bit22` | `p2528+0 \|= 0x1000`; `p2514+8 \|= 0x40` |
| `DEPTHBIAS` 195 | `0x1042a9` | raw `p2528+0x24`; nonzero in `p2514+0xc bit21` | same two masks |

Stencil packing in `p2514` is [BIN]:

| D3DRS ID | handler | field | dirty update |
|---|---:|---|---|
| `STENCILENABLE` 52 | `0x103903` | `+0xc bit3` | `+4 \|= 0x200000` |
| `STENCILFAIL` 53 | `0x10392d` | `+0x10[3:0]` | same |
| `STENCILZFAIL` 54 | `0x103952` | `+0x10[7:4]` | same |
| `STENCILPASS` 55 | `0x103968` | `+0x10[11:8]` | same |
| `STENCILFUNC` 56 | `0x10397b` | `+0x10[15:12]` | same |
| `STENCILREF` 57 | `0x103a2d` | raw `+0x6c` | same |
| `STENCILMASK` 58 | `0x103a38` | raw `+0x70` | same |
| `STENCILWRITEMASK` 59 | `0x103a43` | raw `+0x74` | same |
| `TWOSIDEDSTENCILMODE` 185 | `0x10398e` | `+0xc bit4` | same |
| `CCW_STENCILFAIL` 186 | `0x1039b4` | `+0x10[19:16]` | same |
| `CCW_STENCILZFAIL` 187 | `0x1039c7` | `+0x10[23:20]` | same |
| `CCW_STENCILPASS` 188 | `0x1039da` | `+0x10[27:24]` | same |
| `CCW_STENCILFUNC` 189 | `0x103a02` | `+0x10[31:28]` | same |

Blend and alpha packing in `p2514` is [BIN]:

| D3DRS ID | handler | field | dirty update |
|---|---:|---|---|
| `ALPHATESTENABLE` 15 | `0x103815` | `+0xf4 bit4` | `+4 \|= 0x2000` |
| `ALPHAREF` 24 | `0x1036c8` | low byte of `+0xec` | same |
| `ALPHAFUNC` 25 | `0x103829` | `+0xf4[10:7]` | same |
| `ALPHABLENDENABLE` 27 | `0x10367c` | low byte of `+0xf8` | `+4 \|= 0x4000` |
| `SRCBLEND` 19 | `0x1036ed` | `+0xf8[24:20]` | same |
| `DESTBLEND` 20 | `0x10371c` | `+0xfc[4:0]` | same |
| `BLENDOP` 171 | `0x10375d` | `+0xf8[11:9]` | same |
| `SEPARATEALPHABLENDENABLE` 206 | `0x1036af` | `+0xf8 bit8` | same |
| `SRCBLENDALPHA` 207 | `0x103706` | `+0xf8[19:15]` | same |
| `DESTBLENDALPHA` 208 | `0x103747` | `+0xf8[29:25]` | same |
| `BLENDOPALPHA` 209 | `0x103773` | `+0xf8[14:12]` | same |
| `BLENDFACTOR` 193 | `0x1037a1` | raw `+0xd0`, serial at `+0xcc` | `+4 \|= 0x8000` |

Raster and output anchors are [BIN]:

| D3DRS ID | handler | shadow update | dirty update |
|---|---:|---|---|
| `CULLMODE` 22 | `0x104172` | `p2528+4[1:0] = v&3` | `p2528+0 \|= 0x4` |
| `SCISSORTESTENABLE` 174 | `0x103a9b` | `p2514+0xc bit2` | `p2514+4 \|= 0x400000` |
| `MULTISAMPLEANTIALIAS` 161 | `0x10360b` | `p2514+0xf4 bit0` | `p2514+4 \|= 0x1000` |
| `MULTISAMPLEMASK` 162 | `0x103635` | raw `p2514+0xe8` | same |
| `COLORWRITEENABLE` 168 | `0x103454` | raw `p2514+0x100` | composite dirty path at `0x103517` |
| `COLORWRITEENABLE1` 190 | `0x103465` | raw `+0x104` | `+4 \|= 0x8`, then common dirty path |
| `COLORWRITEENABLE2` 191 | `0x103480` | raw `+0x108` | `+4 \|= 0x10`, then common dirty path |
| `COLORWRITEENABLE3` 192 | `0x103498` | raw `+0x10c` | `+4 \|= 0x20`, then common dirty path |

`FILLMODE` 8 dispatches to `0x10405d` and `SHADEMODE` 9 to `0x1040c9`, but
both update several coupled objects and need a longer join before their final
packing can be stated safely.  These D3D objects are also distinct from the
lower command-shadow objects emitted by `0xf4f50`, `0xfb920`, and `0xfc0e0`.
The static pass has not joined every frontend field to a final M9 index, so the
semantic names above must not yet be attached directly to those packet
registers [INF].

### Render and depth surface shadow layout

The surface emitter at `0xfb920` provides a second exact shadow-state map
[BIN].  The object at `edi` has command dirty bits at `+0x0`, resource dirty
bits at `+0x8`, a live attachment count at `+0x34`, attachment resource pairs
at `+0x38`, and one state dword per attachment at `+0x58`.  Both attachment
arrays advance with the loop index.  The emitted indices and fields are:

| dirty bit in `+0x0` | indices | source fields or relocation |
|---:|---|---|
| bits `0..3`, selected by attachment number | `b0,b1`; `b2,b3`; `b4,b5`; `b6,b7` | state `+0x58 + 4*n`; resource pair `+0x38 + 8*n`; relocation selector `n + 2` |
| `0x10` | `b8,b9` | `b9 <- +0x70`; resource `+0x68/+0x6c`, selector `6` |
| `0x20` | `ba,bb` | `bb <- +0x7c`; resource `+0x74/+0x78`, selector `7` |
| `0x40` | `24`, `e6,e6,e6`, `bc,bd` | resources `+0x9c/+0xa0`, `+0x80/+0x84`, `+0x94/+0x98`, selectors `0x2b`, `9`, `0xa`; fixed `e6` values `0x6dc00000,1,1` |
| `0x80` | `ad,ad,ad,ad`, then `af` | `ad <- +0x28` four times; `af <- +0x2c` |
| `0x100` | `a8,a9,aa` | `+0x14`, `+0x18`, `+0x1c` |
| `0x200` | `ab,ac,ae` | `+0x20`, `+0x24`, `+0x30` |
| `0x400` | `f5,f6,c4,c5` | `+0xa4`, `+0xa8`, `+0xac`, `+0xb0` |
| `0x800` | `be` | resource `+0x8c/+0x90`, selector `8` |
| `0x1000` | `f1` | `+0xbc` |

The first four pairs are conclusively four indexed attachment slots because
the driver derives their headers from byte offsets `0x2c0/0x2c4`, increments
the offsets by eight for each slot, and bounds the loop with `+0x34` [DER].
They are the render-target descriptor bank in this surface object [INF].  The
`b8/b9`, `ba/bb`, `bc/bd`, and `be` resource paths are further surface or
depth/stencil attachments, but their individual semantic names remain for
differential capture.  Keeping these unknown names prevents a guessed depth
base/pitch assignment from entering the kernel ABI.

Before emitting commands, the same function walks every dirty resource pair
and calls the command-buffer resource tracker.  This proves that addresses in
these packet groups are relocation-backed and must not be accepted as raw
userspace GPU addresses [BIN].

### Program and vector state shadow layout

The central emitter at `0xf6f20` uses the first dword of its state object as a
dirty mask.  Its directly decoded groups are [BIN]:

| dirty bit | action |
|---:|---|
| `0x01` | write `28 <- 0`, then emit each four-dword vector through a `23,23,23,23` header; vector storage begins at `+0x18` |
| `0x04` | emit up to four variable-length blocks selected by metadata at `+0xb90`; inline data begins at `+0x870` with a `0x64`-byte stride |
| `0x08` | `ec <- +0x8c`, `f4 <- +0x88`, `ea <- +0x90`, `eb <- +0x94` |
| `0x10` | `d0 <- +0x98`, `d1 <- +0x9c` |
| `0x40` | `30 <- +0x78` |
| `0x80`, `0x100`, `0x200` | call the native program/resource emitter at `0xf74d0` |
| `0x3c00` | call auxiliary upload emitter `0xf78c0` |
| `0x4000` | call auxiliary upload emitter `0xf7c60` |
| `0x8000` | walk 64 possible four-dword upload records beginning at `+0xc0`, with a `0x1c`-byte stride |

Each live `0x8000` record has a key at record `+0x0`, four payload dwords at
`+0x4..+0x10`, and a dirty/live field at `+0x14`.  The driver computes [DER]:

```c
selector = 0x80000000 | ((((key >> 1) + 0x80) & 0x1ff) << 5);
ports = (key & 1) ? indices_64_through_67 : indices_60_through_63;
```

It emits the selector through index `68` when it changes, sends the four
payload words through the selected half of the eight data ports, then clears
record `+0x14`.  This is an exact sparse vector/constant upload mechanism
[BIN][DER].  Stage and register-class names still require a controlled shader
diff, but the address calculation, data width, capacity, and dirty tracking can
already be implemented.

### Fixed index-44 resource records

Functions `0xfc620` and `0xfcb60` each emit exactly eight writes to index `44`
(engine offset `0x1110`), followed by relocation-backed indices `2e/2f`
[BIN].  The two forms are:

| function | state resource | eight index-`44` values | other fixed writes |
|---:|---|---|---|
| `0xfc620` | object `+0x58` | `0x24081913` repeated eight times | `e6 <- 0x42000000` |
| `0xfcb60` | object `+0x74` | `0x28041968` repeated eight times | `e6 <- 0x42000000`, then two `e6 <- 3` writes |

Both paths compare the resource's low 29 address bits and upper-three flag
bits separately, track the resource before emitting, and call relocation
helpers with selectors `0x2c` and `0x2e`.  This is a fixed-width eight-dword
FIFO/resource record, but static evidence does not yet prove whether it is a
vertex stream descriptor, a launch record, or another draw resource.  Capture
should identify it by toggling the secondary dirty bit that selects each form.

## Userspace requester submission

Linux function `m9x22842` at `0x303150` is identified by its
`BrdgDirectRequester::PostDOSBuffer` diagnostic and constructs the proven
`2c/2d` submission packet [BIN].  Its input has a bus-master object at `+0x0`
and byte size at `+0x8`.  Given bus-master base field `BM+0x34` and current
write offset `BM+0x44`, it emits [DER]:

```c
start = BM_34 | (BM_44 + 8);
end   = (BM_44 + 8 + size - 8) | 2;
emit(0x2c, start);
emit(0x2d, end);
```

Requester vtable slot `+0xd0` flushes and returns the fence; slot `+0x100`
submits with argument `1`.  Chained-buffer function `m9x17181` at `0x303c90`
uses the same pair.  Its optional post-link sequence is exactly [BIN]:

```text
e6e6e6e6 63000008 2c000000 62000020
ac000002 45e6e6e6 62804020 e04f7fff
b0000000
```

The sequence is likely ordering/cache work [INF]; its byte values and position
in the chain are established and should be retained as an opaque transition
until captures give each `e6` operation a semantic name.

Function `0xf9c70` in `MxMD.dll` exposes a direct inline primitive
path [BIN].  It emits the full header `2b,2a,e6`, writes
`2b <- 0x00ffffff`, selects an inline-layout word of `0x45fcf2f3` or
`0xfcf2f379` through `2a`, and writes
`e6 <- 0x20000000 | (vertex_count - 1)`.  It then appends three or four packed
dwords per vertex [DER].  This is the strongest static draw-launch anchor and
should be the first path compared against a one-triangle capture.

## Kernel DMA ABI and hardware doorbell

The proprietary Linux `KernelLibraries.o_shipped` provides the matching static
kernel side.  Its ioctl dispatcher `m9x1185` at `0x4e80` masks the command with
`0xff` and exposes these relevant calls [BIN]:

| command | saved name/operation | structure size | core vtable slot |
|---:|---|---:|---:|
| `1` | `GetBmBufferAddr` | `0x38` | `+0x98` |
| `12` | `ExecuteDma` | `0x840` | `+0x80` |
| `15` | `SetCommandBufferSize` | `0x10` | `+0xf8` |
| `16` | `DrawOnSyncBuffer`, selector `1..4` | `0x18` | `+0xe0/+0xe8/+0xf0` |
| `18` | `ReleaseBmBuffer` | `0x30` | `+0xa0` |

DMA initialization function `m9x109` at `0x2d150` writes the following device
offsets through the vendor MMIO method [BIN]:

| offset | value construction |
|---:|---|
| `0x400` | `(tag_context[0x50] & ~3) \| BM[0x34]` |
| `0x109c` | `0x00080000` |
| `0x448` | zero |
| `0x1068` | `tag_context[0x18] \| tag_context[0x00]` |
| `0x106c` | `tag_context[0x40] \| tag_context[0x28]` |
| `0x10ec` | optional `((BM[0x44] + 0x200) & ~0x1f) \| (BM[0x34] & 3)` |
| `0x1094` | optional `(BM[0x44] + 0x200) & ~0x1f` |
| `0x1098` | `(base + 3 * field * 0x200 - 0x400) & ~0x1f \| 6` |
| `0x44c,0x450,0x454` | object `+0x6c,+0x70,+0x74` |

No write to `0x1070` occurs in this Linux initialization function; any earlier
tentative list containing it must not become part of the open-driver sequence.

Submission function `m9x1039` at `0x2d750` pads to the configured command
alignment with `0x45454545`, stores the final write address in tag-context
`+0x50`, and rings the engine by writing that address to MMIO `0x404` [BIN].
If `BM+0x34` is zero it first writes zero to `0x31c`.  With object flag `+0x80`
bit 1 set it waits for tag-context `+0x54` to clear, then polls `0x8f0` until
`value & 0x00030000` is zero.  Flag bit 2 adds callbacks around submission and
an idle-wait helper.  The returned fence is stored at object `+0x1a8`.

This strongly establishes `0x400` as DMA base/control and `0x404` as the submit
doorbell for this binary [DER].  The vendor's unbounded waits must become
bounded DRM waits with reset/error recovery; copying them literally would make
a malformed stream capable of hanging the kernel.

## Native shader upload transport

Function `0xf74d0` gives an exact inline program upload [BIN]:

1. A program resource descriptor lives at state offsets `+0xa0/+0xa4`.  The
   command-buffer relocation callback is invoked with usage/type `0x19`.
2. Index `69` receives the relocated address, `6a` receives state `+0xa8`, and
   `6d` receives state `+0xbc`.
3. For an inline program, `+0xbb0` is the byte length and `+0xbb4` is the CPU
   pointer.  Index `e3` receives a command-buffer allocation/result value.
4. The driver computes `instruction_count = byte_length >> 5`.  It reserves
   `15 * instruction_count + 10` dwords in the command buffer.
5. For each instruction, index `68` receives a selector.  It starts at
   `0x80000000`; selector bits `13:5` advance by `0x20` for each 32-byte record.
6. Indices `60,61,62,63` receive record dwords 0 through 3.  Indices
   `64,65,66,67` receive record dwords 4 through 7.  The source pointer advances
   by exactly 32 bytes.

Equivalent pseudocode is:

```c
for (unsigned byte_off = 0; byte_off < byte_length; byte_off += 32) {
        emit(0x68, 0x80000000 | (byte_off & 0x3fe0));
        emit4(0x60, 0x61, 0x62, 0x63, code[0], code[1], code[2], code[3]);
        emit4(0x64, 0x65, 0x66, 0x67, code[4], code[5], code[6], code[7]);
        code += 8;
}
```

This proves a native hardware-visible instruction record is 32 bytes/eight
dwords and that `68` is the indexed-store selector [DER].  It does not yet
identify fields inside those eight dwords.

Linux function `m9x11728` at `0x4b9340` independently implements the same
transport [BIN].  In that build the native byte length and CPU pointer are at
state `+0xbd8/+0xbe0`.  It again computes `length >> 5`, reserves `15*N+10`
dwords, emits selector `68` followed by ports `60..67`, and advances the source
by `0x20`.  Its selector update preserves bits selected by `0xc01f` and inserts
`(instruction_number & 0x1ff) << 5`, independently fixing the selector's
instruction-number field at bits `13:5` [DER].

The Linux external-program form binds index `69` from resource
`+0xa0/+0xa8` with usage `0x19` and descriptor `2`, writes `6a <- +0xb0`, and
`6d <- +0xd0`.  It also exposes a short control form selected by state
`+0xbcc` [BIN]:

```c
if (state_bcc) {
        emit(0x6d, low16(state_bd4));
        emit(0x69, 4);
        emit(0x6a, low16(state_bd0));
} else {
        emit(0x6d, low16(state_bd4) | (low16(state_bd0) << 16));
}
```

The meaning of this short form remains unknown, but its encoding can be
preserved and isolated in a typed helper rather than reconstructed later from
a trace.

The same function can bind a relocated program resource rather than uploading
inline words.  Registry names `PsBmMicrocode`, `PsBmConstants`,
`CeBmMicrocode`, and `CeBmConstants` select bus-master paths independently for
two program classes [BIN].  This explains why native code may be absent from an
ordinary MMIO-only trace.  Capture work must exercise both inline and BM modes.

### Program linking and limits

`MxMD.dll` function `0x13bf80` links microcode fragments from two source lists
into the buffer later consumed at state `+0xbb0/+0xbb4` [BIN].  It:

* accumulates fragment byte counts;
* moves to a second source/bank when adding a fragment reaches `0xc00` bytes;
* rounds the final allocation up with `(size + 0x1f) & ~0x1f`;
* copies both lists into the allocation;
* stores the aligned length at `+0xbb0` and pointer at `+0xbb4`.

The `0xc00` boundary is 3,072 bytes, or 96 native 32-byte records [DER].  It is
a bank/assembly limit visible in this path, not yet proof that every shader
stage exposes 96 user instructions.

Several address fields preserve bits `31:29` while replacing bits `28:0`, using
the masks `0xe0000000` and `0x1fffffff` [BIN].  The upper three bits therefore
carry address-space or resource flags; relocations must preserve them.

### Auxiliary upload records

Windows function `0xf78c0` uses selectors in the `0x1300` region.  Linux twin
`m9x2427` at `0x4b99d0` resolves its apparent half-record loop into four
complete 32-byte records [BIN][DER]:

| selector | eight state dwords in port order `60..67` |
|---:|---|
| `0x80001300` | `+0x7d4,+0x7d8,+0x7dc,+0x858,+0x7e4,+0x7e8,+0x7ec,+0x854` |
| `0x80001320` | `+0x7f4..+0x810` |
| `0x80001340` | `+0x814..+0x830` |
| `0x80001360` | `+0x834..+0x850` |

The surrounding resource-bind packet writes `68 <- 0`, `6b <- +0xc8`, and
relocates resource `+0xb8/+0xc0` into `6c` with usage `0x1a`, descriptor `4`.
Dirty bit `0x08` controls the `1320/1340/1360` group.

Windows function `0xf7c60` and Linux twin `m9x14617` at `0x4b8fa0` use selector
`0x80001500` and all eight data ports.  The Linux state map is [BIN]:

| port | source |
|---:|---|
| `60` | `+0x884` |
| `61` | truncated float `+0x85c` in low 16 bits, truncated float `+0x860` in high 16 bits |
| `62` | relocation `+0x888/+0x890`, usage `0x15`, descriptor `5` |
| `63` | same resource, usage `0x16`, descriptor `2` |
| `64` | `+0x878` |
| `65` | `+0x87c` |
| `66` | relocation `+0x868/+0x870`, usage `0x13`, descriptor `5` |
| `67` | same resource, usage `0x14`, descriptor `2` |

These are likely constant/control records adjacent to the native program store
[INF].  Captures should initially call them `upload-1300`, `upload-1320`,
`upload-1340`, `upload-1360`, and `upload-1500`, not vertex or fragment
constants, until stage-specific diffs establish the names.

## Matrox GLSL compiler interface

The `MxMGLX.dll` compiler object's vtable is at `0x10804014`.  Constructors
`0x1066e4a0` and `0x1066e640` install it.  Relevant slots are [BIN]:

| slot | function | result |
|---:|---:|---|
| `+0x14` | `0x1066e7a0` | main compile pipeline |
| `+0x1c` | `0x1066f500` | return output pointer and byte count |
| `+0x20` | `0x1066f540` | metadata at object `+0x484` |
| `+0x24` | `0x1066f550` | metadata at object `+0x4a4` |
| `+0x28` | `0x1066f560` | metadata at object `+0x494` |
| `+0x2c` | `0x1066f570` | metadata at object `+0x4b4` |
| `+0x30` | `0x1066f580` | metadata at object `+0x4c4` |

The output getter returns the vector beginning at object `+0x14`; its byte count
is the dword-vector length multiplied by four.  With a debug flag enabled,
function `0x10672c00` writes that exact vector to `iris.o` [BIN].  The compile
pipeline calls translation, allocation/linkage, and serialization passes at
`0x106717e0`, `0x10671930`, `0x10671b10`, and gated serializer `0x106726a0`
before the dump.

The Iris stream is variable length and contains metadata/sentinels as well as
instructions.  It is not the fixed eight-dword native record copied by
`MxMD.dll`.  A Mesa compiler can reuse the recovered operation semantics, but
must still implement a separate Iris/native lowering and native bit packer.

### Serialized Iris object

The serializer gate is compiler `+0x460` bit 2, tested at `0x1066f33d`; the
constructor at `0x1066e4a0` initializes that field to `4`, so normal compilation
calls serializer `0x106726a0` [BIN].  Its header is [DER]:

```c
header = 0xfffc0100 |
         ((~(*(uint32_t *)(compiler + 0x478) << 14)) & 0x10000);
```

Header bit 16 is therefore the inverse of compiler `+0x478` bit 2.  Primary
records are emitted in the following fixed list order while the corresponding
list is nonempty [BIN]:

| list below `compiler+0x464` | tag | payload |
|---:|---:|---|
| `+0x40` | `0x002f` | six dwords/24 bytes from node `+0x10` |
| `+0x198` | `0x8008` | six dwords/24 bytes from node `+0x18` |
| `+0x220` | `0x8009` | six dwords/24 bytes from node `+0x18` |
| `+0x2a8` | `0x800a` | six dwords/24 bytes from node `+0x18` |
| `+0x110` | `0x001d` | six dwords/24 bytes from node `+0x18` |

Each primary record is one tag dword plus six payload dwords: 28 bytes, not a
native 32-byte instruction.  For tag `0x2f`, node `+0x14` bit 1 controls a
separate descriptor canonicalization.  When clear, the derived descriptor
field is [DER]:

```c
n = (payload_dword1 >> 7) & 7;
m = ((1u << n) - 1) & 0xf;
descriptor_dword2 = (old_descriptor_dword2 & 0x07803fff) |
                    (m << 14) | 0x07800010;
```

After the primary lists, the serializer appends another variable dword vector
and `0xffff`, then metadata from `0x106731c0`.  That metadata begins with
`0xfff9`, contains variable `0xfffa` symbol records with counts and 60-byte
subrecords, and ends with a second `0xffff` [BIN].  Getter `0x1066f500` returns
this dword vector and its exact byte length; debug writer `0x10672c00` opens
literal filename `iris.o` in `wb` mode.

Functions `0x105686e0` and `0x10569370` operate after program installation but
do not perform native packing [BIN].  The first walks `program_env_%d`,
`program_local_%d`, and `__constant__` symbols and maintains 20-byte/16-byte
binding tables plus 40-byte symbol nodes.  The second allocates `count*0x18`
and materializes runtime matrix/vector parameters.  Neither uses a 32-byte
stride.  The `GL_PROGRAM_FORMAT_ASCII_ARB` path at `0x10568220` feeds its
translated program back through compiler frontend `0x105658a0`.

No Iris header or tag constants occur in `MxMD.dll`, and `MxMGLX.dll` does not
import `MxMD.dll` [BIN].  The final Iris-to-native lowering therefore lies
behind another opaque interface or code path [INF]; the traced serializer
cannot be treated as the hardware ISA packer.

## Iris instruction token layout

The trace formatter at `0x106ae3b0` provides these token facts [BIN]:

* the opcode is the low 16 bits of the dword at token offset `+4`;
* bits `27:24` of that dword encode instruction modifiers:
  `0` none, `1` `_sat`, `2` `_pp`, `4` `_centroid`;
* bits `31:28` encode result scale:
  `0` none, `1` `_x2`, `2` `_x4`, `3` `_x8`, `13` `_d8`, `14` `_d4`,
  `15` `_d2`;
* the operand formatter sees component masks/swizzles, source negation and
  absolute forms, immediates, array/range indices, and predicate state;
* predicate names include `p0` through `p5` and negated forms;
* control operations include `PushConditionalStack`,
  `MaskConditionalStack`, and `PopConditionalStack`.

The instruction-prefix formatter at `0x106ae220` further identifies token
offset `+0x8` bit 0 as the printed conditional marker and bit 1 as predicate
enable.  Predicate-enabled instructions take the predicate number from token
`+0x18`, a polarity/modifier nibble from `+0x16c`, component enables from bits
`16:13` of `+0x44`, and four packed two-bit component selectors from bits
`24:17` of `+0xc` [BIN].  Modifier values `0`, `1`, and `13` print as no prefix,
`-`, and `!`, respectively.  The resulting debug form is a predicate register
plus an `x/y/z/w` mask.  This layout is directly usable by an Iris dump parser,
while its eventual native encoding remains separate.

The operand formatter at `0x106afa90` and its caller at `0x106af220` expose a
substantial part of the Iris operand descriptor [BIN].  Operand `n` begins at
token `+0x7c + 0x38*n`; a related per-operand nibble is stored at token
`+0x15c + 4*n`.  In the descriptor's first dword:

| bits | compiler-IR meaning |
|---:|---|
| `4:0` | register-file code; known printed files include `r`, `v`, `c`, `a`, `t`, `s`, `cb`, `aL`, `l`, `p`, and special face registers |
| `10:6` | operand/addressing form; formatter has specialized paths for values `3`, `7`, and `15` |
| `12:11` | `x/y/z/w` component used by relative-address syntax |
| `16:13` | four component-enable bits, from `x` at bit 13 through `w` at bit 16 |
| `24:17` | four packed two-bit `x/y/z/w` selectors |
| `27:25` | type class: `1` scalar, `2` vector, `3` matrix, `4` struct, `5` array, `6` label |
| `30:28` | array element type when the outer class is array, using the same scalar through label numbering |

Descriptor dword `+0x4` bit 0 selects the formatter's immediate marker and bit
1 selects its `$` marker.  Fields at `+0x8/+0xc`, `+0x14/+0x18`, and
`+0x20/+0x24` supply scalar or range bounds in the printed addressing forms
[BIN].  These are compiler-container fields, not native M9 instruction bits,
but they are enough to build a compatible Iris parser and differential dumper.

The instruction vocabulary recovered from the formatter is:

| opcode | Iris operation | opcode | Iris operation |
|---:|---|---:|---|
| `0` | NOP | `1` | MOV |
| `2` | ADD | `3` | SUB |
| `4` | MAD | `5` | MUL |
| `6` | RCP | `7` | RSQ |
| `8` | DP3 | `9` | DP4 |
| `10` | MIN | `11` | MAX |
| `12` | EXP | `13` | LOG |
| `16` | LRP | `17` | FRC |
| `18` | M4x4 | `19` | M4x3 |
| `20` | M3x4 | `21` | M3x3 |
| `22` | M3x2 | `23` | CALL |
| `30` | POW | `31` | SGN |
| `32` | ABS | `33` | NRM |
| `34` | SINCOS | `41` | BREAK family |
| `45` | KILL | `46` | TEXLD |
| `48` | CMP | `49` | DSX |
| `50` | DSY | `51` | TEXLDD |
| `52` | SETP | `53` | TEXLDL |
| `54` | TEXLDB | `55` | TEXLDP |
| `0x101` | FTOI | `0x102` | FTOU |
| `0x103` | IADD | `0x104` | integer comparisons |
| `0x106` | IMAD | `0x107` | IMAX |
| `0x108` | IMIN | `0x109` | IMUL |
| `0x10a` | INEG | `0x10b` | ISHL |
| `0x10c` | ISHR | `0x10d` | ITOF |
| `0x10e` | NOT | `0x10f` | OR |
| `0x110` | UDIV | `0x111` | UMOD |
| `0x112` | unsigned comparisons | `0x114` | UMUL |
| `0x115` | UMAD | `0x116` | UMAX |
| `0x117` | UMIN | `0x118` | USHR |
| `0x119` | UTOF | `0x11a` | XOR |
| `0x11b` | ICMP | `0x11d` | SIN |
| `0x11e` | COS | `0x8001` | CEIL |
| `0x8002` | TRUNC | `0x8003` | FLOOR |
| `0x8004` | DP2 | `0x8005` | M2x2 |

The comparison families represented at `0x104`, `0x112`, and `0x11b` retain
the condition names `GT`, `EQ`, `GE`, `LT`, `NE`, and `LE`.  Their signed forms
print as `ISGT/ISEQ/ISGE/ISLT/ISNE/ISLE`, unsigned forms as
`USGT/USEQ/USGE/USLT/USNE/USLE`, and conditional-select forms as `ICMP GT`
through `ICMP LE` [BIN].

Base opcodes `14..15`, `24..29`, `35..40`, `42..44`, and `47` take the
formatter's unsupported/reserved path.  The extended integer vocabulary is
compiler-front-end evidence.  It does not prove that M9 executes those
operations natively; GLSL 1.10 lowering can expand them into simpler native
records.

The ARB assembly parser separately recognizes `KIL`, `TXB`, `TXP`, `TEX`,
`SWZ`, `MAD`, `LRP`, `CMP`, `XPD`, `SUB`, `SLT`, `SGE`, `MUL`, `MIN`, `MAX`,
`DST`, `DPH`, `DP4`, `DP3`, `ADD`, `POW`, `SCS`, `SIN`, `RSQ`, `RCP`, `LG2`,
`EX2`, `COS`, `MOV`, `LIT`, `FRC`, `FLR`, and `ABS` [BIN].

## Linker resource limits

The GLSL linker/resource allocator at `0x1055d710` applies these exact checks
[BIN]:

| stage/resource | limit |
|---|---:|
| vertex samplers | 4 |
| vertex attributes | 15 |
| vertex output registers | 15 |
| fragment samplers | 16 |
| fragment input registers | 16 |
| vertex allocation metric | `0x7c00` |
| fragment allocation metric | `0x7c00` |

The two `0x7c00` comparisons are internal allocation offsets/metrics.  They
must not be exposed as 31,744 constants.  The ICD has built-ins for
`gl_MultiTexCoord0..7`; those eight input names do not override the separate
four-vertex-sampler check.

Linux function `m9x20565` at `0x6bbb40` repeats the same checks and provides
diagnostic strings that identify the first three vertex fields as samplers,
attributes, and output registers, and the fragment fields as samplers and
input registers [BIN].  It calls the `0x7c00` fields constant-allocation units,
but their hardware granularity remains unknown.  The independent 64-bit build
therefore corroborates both the caps and the warning against treating
`0x7c00` as a public vector count.

The `GL_MTX_fragment_shader` validator at `0x1012c180` also proves a legacy
fragment path with two special temporaries, texture dependency rules, paired
color/alpha requirements for DP3/DP4, result-only saturate/scale modifiers,
argument-only replicate/convert modifiers, unique invariant accounting, and a
mandatory final output write [BIN].  That validator describes the extension
language.  Its validation rules should be used as frontend constraints, not
copied blindly into native ISA validation.

## Native ISA facts and remaining unknown fields

Static analysis has established these native facts:

* instruction record: eight dwords, 32-byte alignment [BIN];
* inline instruction-store selector: index `68`, enable bit 31, address in bits
  `13:5` [BIN][DER];
* data ports: indices `60..67`, one dword each [BIN];
* linked bank boundary: `0xc00` bytes, 96 records [BIN][DER];
* program resources can be uploaded inline or fetched by bus mastering [BIN];
* GPU resource addresses preserve three flag bits above a 29-bit address [BIN];
* selectors `0x1300,0x1320,0x1340,0x1360,0x1500` address complete auxiliary
  eight-dword records [BIN];
* index `44` is an eight-write record port in two relocation-backed paths
  [BIN][DER];
* `2b,2a,e6` launches the recovered inline primitive path, with count-minus-one
  in the low bits of the `e6` value [BIN][DER];
* the serialized Iris object uses 28-byte primary records and variable metadata,
  conclusively separating it from native 32-byte records [BIN][DER].

The static binaries do not yet give an unambiguous native mapping for ALU
opcode, destination, source selectors, swizzles, predicates, texture operation,
or end-of-program bits.  Prebuilt fragments are linked and copied verbatim, so
there is no single obvious switch from a high-level opcode to one eight-dword
literal in `MxMD.dll`.  Those fields require controlled compiler outputs,
capturing the linked `+0xbb4` buffer, or differential DMA/MMIO capture.

## Driver implementation consequences

M9 needs an engine and compiler backend distinct from Parhelia.  Code that can
be implemented before capture is:

1. a typed M9 packet builder with `0x45` short-header termination;
2. register-bank definitions for the proven texture selectors and program
   upload ports, retaining unknown names where semantics are incomplete;
3. a shader-program object model with eight-dword records, 32-byte alignment,
   a 96-record bank boundary, and relocations which preserve address flag bits;
4. Mesa compiler IR definitions for the recovered Iris operations and
   modifiers, separate from a still-incomplete native packer;
5. resource checks for four vertex samplers, 15 attributes/outputs, 16 fragment
   samplers/inputs, and six predicate names;
6. command validation which rejects native program selectors outside the
   proven store and checks all bus-master resources and lengths;
7. requester/DMA scaffolding for the proven `2c/2d` start/end pair, `0x404`
   doorbell, fences, bounded idle waits, and reset/error recovery;
8. a vendor-Iris parser/dumper for exact compiler comparisons, while keeping
   the unresolved native packer behind an explicit unsupported boundary.

The first capture matrix should compile one-operation vertex and fragment
programs with identity operands, then vary one destination, source, swizzle,
write mask, negate/absolute modifier, predicate, saturation/scale, and texture
unit at a time.  Save both the compiler output and the linked 32-byte records.
For submission, capture inline microcode/constants with all four BM knobs off,
then repeat with each knob enabled.  This will connect the Iris vocabulary to
native bit fields without conflating compiler containers, linked programs, and
DMA packets.
