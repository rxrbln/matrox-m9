# M9 multisampling (FAA): static closure, captures, and what the silicon did

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

This note closes the multisampling investigation for M9 (PCI `102b:0540`, M9128/M9148, `MSerX` images)
as far as it can be closed without booting Windows, and records exactly what
the M9128 did when the recovered state was written to it.

It supersedes the multisampling parts of
`parhelia-m9-gigacolor-faa-static-closure.md` where the two disagree; that
note's GigaColor half stands.  `parhelia-antialiasing.md` describes the
**older Parhelia-512** fragment buffer and is kept separate on purpose: its
register meanings are not imported here, and section 5 below says which of
its constructs exist on the M-series and which do not.

Every statement carries a label:

* **STATIC** - read out of a saved vendor binary; binary, address and bytes
  are given.
* **CAPTURED** - counted over saved vendor command streams by
  `tools/m9-faa-capture-evidence.py`.
* **MEASURED** - run on the M9128 in this driver, with the readback that
  shows it.
* **DERIVED** - a deterministic consequence of labelled facts above it.
* **OPEN** - not established, with the experiment that would establish it.

Images, with the address model verified before anything was read from them:

| image | base | file offset |
|---|---|---|
| `xddm32_210_00_105_se_u_nowhql/MSerX/MxMD.dll` | `0x10000` | VA - 0x10000 |
| `xddm32_210_00_105_se_u_nowhql/MSerX/MxMGLX.dll` | `0x10000000` | VA - 0x10000000 |
| `xddm32_210_00_105_se_u_nowhql/MSerX/MxMM.sys` | `0x10000` | VA - 0x10000 |

Run the two deterministic verifiers from the repository root:

```sh
python3 tools/m9-gigacolor-faa-static-evidence.py
python3 tools/m9-faa-capture-evidence.py
```

## 1. The shape of the feature

The M-series multisample state the vendor's Direct3D user-mode driver builds
is **two dwords and nothing else**.  Compiler `0x139240` writes them to
validator `+0x368` and `+0x36c`; lowerer `0x181fa0` copies them into the
surface shadow at `+0x28`/`+0x2c` (`0x1826a5`, `0x1826ae`); emitter `0xfb920`
writes them into the command stream [STATIC]:

```
000fbe09  c7 40 04 ad ad ad ad   header 0xadadadad   ; four values, index ad
000fbe13..000fbe31               four writes of [edi+0x28]
000fbe37  c7 40 18 af 45 45 45   header 0x454545af   ; one value, index af
000fbe41  8b 5f 2c / 89 58 1c    one write of [edi+0x2c]
```

`0x45` is the pad index of this command grammar, so those headers are four
writes to index `ad` and one to index `af`.  The byte patterns
`c74004adadadad` and `c74018af454545` occur exactly once in `MxMD.dll` and in
no other saved binary [STATIC].  This note calls the two dwords **AD** and
**AF** after those indices.

Neither index takes a relocation in this project's kernel validator, so no
address reaches the unit through them [STATIC, `m9_submit.c`
`m9_index_reloc_kinds`].

## 2. AD - the colour allocation extent, and nothing else

### 2.1 Construction [STATIC]

Before the backed surface is known, `0x1392e4..0x139304` seeds AD:

```
001392e4  8b b7 68 03 00 00            mov esi, [edi+0x368]
001392ea  81 a7 bc 01 00 00 8f 7f ff ff  and dword ptr [edi+0x1bc], 0xffff7f8f
001392f4  81 e6 20 00 e0 ff            and esi, 0xffe00020
001392fa  83 ce 20                     or  esi, 0x20
00139304  89 b7 68 03 00 00            mov [edi+0x368], esi
```

On a backed surface AD is then **zeroed first** and rebuilt:

```
001395aa  c7 87 68 03 00 00 00 00 00 00   mov dword ptr [edi+0x368], 0
001395b4  8b 01 / 8b 50 60 / ff d2        call surface vtable +0x60 (0xa6d00)
001395bf  c1 e8 05                        shr eax, 5
001395c2  3d ff ff 0f 00 / 72 05 / b8 ff ff 0f 00   eax = min(len>>5, 0xfffff)
001395ce  8b 8f 68 03 00 00 / 33 c8 / 81 e1 ff ff 1f 00 / 31 8f 68 03 00 00
```

so the final AD on a backed surface is exactly `min(allocation_bytes >> 5,
0xfffff)`, with every upper bit clear - the seed does not survive.  The
existing closure note's `AD = (AD & ~0x1fffff) | min(...)` is numerically
right only because the preserved field happens to be zero there [DERIVED].

When the length getter returns zero there are exactly two reachable forms,
both `0x20` from the nested initializer at `0x12e589` [STATIC].

### 2.2 There is no sample-count multiplier [STATIC, clean negative]

Resource-layout constructor `0x45eb0` computes the allocation size at
`0x46470..0x464d9` from aligned width, aligned height, depth/array count,
bits per pixel, a guard band and an alignment round-up.  There is no further
multiplicand and no second term.  The total at `+0x180` is copied unchanged
into the allocation request (`0x5c386 8b 91 80 01 00 00`, `0x5c391 89 57 1c`).
All 27 callers of `0x45eb0` were listed; none reaches it through a
multisample type or quality field.

CAPTURED confirms it from the other side: over 1206 decoded vendor passes no
captured surface descriptor carries a colour span an allocation factor would
enlarge, and the one submission whose span is twice its clip width is an
ordinary 256-wide surface drawn into a sub-window, the same descriptor
appearing 273 times with a full 256x256 clip.

### 2.3 AD is inert on the M9128 [MEASURED]

Fixture `faarect` in `tools/m9-mesa-glsl.c`, one rectangle on integer pixel
edges into a 320x240 RGBA8 target, interior pixel count read back:

| AD written | interior pixels |
|---|---|
| off form `0x20` | 48000 |
| `0x00`, `0x10`, `0x20`, `0x30`, `0x40`, `0x50`, `0x60`, `0x70` | 48000 each |
| `0x01`, `0x1fffff` | 48000 each |
| single bit 21..30 set beside the true extent | 48000 each |

Twenty distinct AD values, including the true allocation extent
(`0x2800` for this target, traced by the driver), all render identically and
none stops the engine.  **AD carries no enable and no observable behaviour on
its own.**

This closes gate 1 of the earlier note as a negative: `allocation_byte_length`
implies neither a sample-count multiplier nor a physical fragment
permutation.

## 3. AF - a sample-coverage word, fully named

### 3.1 The construction, corrected [STATIC]

`0x139310..0x139359`, gated on nested `+0x1b0 != 0` and validator `+0x174`
bit 14:

```
00139310  8b 87 8c 00 00 00      mov eax, [edi+0x8c]          ; v8c
00139316  d9 87 70 01 00 00      fld  dword ptr [edi+0x170]   ; v170
0013931c  dc 05 c8 82 20 00      fadd qword ptr [0x2082c8]    ; 1.5 * 2^44
00139322  8b f0 / c1 ee 02 / 23 f2     esi = (v8c >> 2) & 0x20000
00139332  25 00 00 04 00 / 0b f0       esi |= v8c & 0x40000
0013933c  c1 ee 03                     shr esi, 3
0013933f  81 e1 00 00 01 00 / 0b f1    esi |= v174 & 0x10000
00139347  c1 ee 05                     shr esi, 5
0013934a  81 e2 00 f0 ff ff / 0b f2    esi |= old_AF & 0xfffff000
00139352  25 ff 01 00 00 / 0b f0       esi |= bias & 0x1ff
00139359  89 b7 6c 03 00 00            mov [edi+0x36c], esi
```

The two shifts give

```c
AF = (old_AF & 0xfffff000)
   | (round(v170 * 256.0f) & 0x1ff)   /* AF[8:0]  */
   | ((v8c  >> 19 & 1) << 9)          /* AF[9]  */
   | ((v8c  >> 18 & 1) << 10)         /* AF[10] */
   | ((v174 >> 16 & 1) << 11);        /* AF[11] */
```

The magic constant at `0x2082c8` is `26388279066624.0 = 1.5 * 2^44`, so the
double's ulp there is `1/256` and the low dword after the add is
`round(x * 256)`; the reset value `1.0f` (stored at `0x12dfbf`) gives `0x100`
[DERIVED].

**The earlier closure note is wrong by three bit positions** - it places the
three flags at 11/12/13 and says the old AF bits survive.  It missed
`0013933c c1 ee 03`.  Old AF bits 11:0 are all discarded.

### 3.2 What each field is [STATIC, joined across two binaries]

The float and the three flags arrive through the reserved DP2 opcode-5
escape, which carries 8-byte `{id, value}` records to auxiliary-state
dispatcher `0x104ce0` (ids 1..31, target table `0x1052b8`).  The three ids
that matter:

| id | handler | destination |
|---:|---|---|
| 23 | `0x10510c` | value bit0 -> frontend `+0xf4` b14; bit1 -> `+0xf4` b16; bit2 -> `+0x0c` b18; bit3 -> `+0xf4` b15 |
| 24 | `0x1051d8` | value bit0 -> frontend `+0x0c` b19 |
| 25 | `0x1051b9` | value as float -> frontend `+0xf0` |

`MxMGLX.dll` names all three.  Its opcode-5 sender is `0x100a5070`
(`6a 08 / 6a 05` - payload eight bytes, opcode five), and the multisample
flush at `0x1009ee81..0x1009ef3e` emits exactly this triple from the GL
multisample attribute group:

```
1009eede..1009eefd   packed = (ALPHA_TO_ONE<<3)|(SAMPLE_COVERAGE<<2)
                            |(ALPHA_TO_COVERAGE<<1)|MULTISAMPLE_enable
1009ef02  6a 17      push 0x17   ; id 23  <- that bitfield
1009ef14  d9 41 10   fld [ms+0x10]
1009ef20  6a 19      push 0x19   ; id 25  <- GL_SAMPLE_COVERAGE_VALUE, raw f32
1009ef30  0f b6 51 14 movzx edx,[ms+0x14]
1009ef37  6a 18      push 0x18   ; id 24  <- GL_SAMPLE_COVERAGE_INVERT
```

`glSampleCoverage` is `0x100a9e50`, which clamps to `[0,1]`, stores the
invert byte at `+0x14` and the value float at `+0x10` of that object; the
object's constructor (`0x1008d6e0`, and the GL context copy at
`0x100afb66..0x100afbb2`) sets value `1.0f` and invert `GL_FALSE`.

Joining the two tables gives AF in full [DERIVED]:

| AF field | GL state |
|---|---|
| `[8:0]` | `GL_SAMPLE_COVERAGE_VALUE` x 256, 1.8 fixed point; reset 1.0 -> `0x100` |
| `9` | `GL_SAMPLE_COVERAGE_INVERT` |
| `10` | `GL_SAMPLE_COVERAGE` enable |
| `11` | `GL_SAMPLE_ALPHA_TO_COVERAGE` |
| `[31:12]` | not written by the construction |

This closes gate 3: auxiliary state 25 is the **sample coverage value**.  It
is not a sample count, not a coverage inversion and not a sample-pattern
selector - the earlier note was right to refuse all three.

Two GL multisample bits deliberately do **not** reach AF: the master
`GL_MULTISAMPLE` enable (auxiliary 23 bit 0 -> frontend `+0xf4` bit 14) and
`GL_SAMPLE_ALPHA_TO_ONE` (bit 3 -> `+0xf4` bit 15).  They only feed the
compiler's decision to build the pair at all.  **The master enable therefore
reaches the hardware through some other dword** [DERIVED] - see section 4.

### 3.3 What the M9128 did [MEASURED]

Every bit of AF was written on its own beside a valid AD, and `faarect` read
back:

| AF written | result |
|---|---|
| `0x000`, `0x080`, `0x100`, `0x1ff` (coverage 0, 0.5, 1.0, 1.99) | 48000 interior, unchanged |
| `0x200` (invert), `0x300`, `0x400` (coverage enable), `0x480`, `0x500`, `0x600`, `0x700` | 48000 interior, unchanged |
| **`0x800` (alpha to coverage)** | **0 interior, engine stream timeout** |
| `0x1000`, and single bits 13,14,15,16,17,18,19,20,24,28,31 | 48000 interior, unchanged |

The `0x800` result was reproduced twice, each time needing a reboot; after
each reboot the known-good `texture` fixture was rerun and passed before any
further measurement was trusted.

So **exactly one bit of AF is live silicon, and it is the top bit of the
field the vendor constructs.**  That is the strongest single result of this
pass: the unit exists and responds, and what is missing is its enable, not
the AF construction.  Writing alpha-to-coverage while the unit is disabled is
an invalid combination that stops the engine.

## 4. The enable, and the dword this driver does not write

The earlier note attributed a three-bit field at bits 6:4 to AD.  That is a
misattribution [STATIC]: `0x139521..0x139594` writes `[edi+0x1bc]`, which is
nested compiled state `+0x04`, a different hardware dword.  AD is
`[edi+0x368]` = nested `+0x1b0`.  The seed block's
`and dword ptr [edi+0x1bc], 0xffff7f8f` clears bits 6:4 and bit 15 of that
other word every compile, and the branches then set them:

| AA enable A | depth companion `+0x18` | flag B | allocation length | `+0x1bc` bits 6:4 |
|---|---|---|---|---|
| 0 | - | 0 | - | 1 |
| 0 | - | 1 | nonzero | 7 |
| 0 | - | 1 | zero | 3 |
| 1 | nonzero | 0 | - | 7 |
| 1 | nonzero | 1 | nonzero | 5 |
| 1 | nonzero | 1 | zero | 3 |
| 1 | zero | 0 | - | 6 |
| 1 | zero | 1 | nonzero | 4 |
| 1 | zero | 1 | zero | 3 |

`A` is the AA-enable decision at `0x139250..0x1392dd`: set by AAMode bit 0,
by D3D render state 161 `MULTISAMPLEANTIALIAS`, by validator `+0x174` bit 14
(the master `GL_MULTISAMPLE` enable of section 3.2), or by two conditions on
`+0x174` bits 12 and 13; and cleared again when `+0x8c` bit 17 (auxiliary
state 9) is set beside a surface capability bit.

**This is the first field whose value depends on the AA enable and which is
not AD or AF.**  Which command index carries nested `+0x04` is [OPEN]; it is
the single most useful thing left to recover, because the M9128 says the unit
is there and only its enable is missing.  The experiment that would close it:
find the lowerer and emitter of nested `+0x00`/`+0x04` the way `0x1826a5` and
`0xfbe09` were found for `+0x1b0`/`+0x1b4`, then write that index with the
field values above and rerun `faaedge`.

`a1`, `a3` and `a4` are the fragment-pointer registers themselves, and they
never carry an address [STATIC, section 4c].  `MxMD.dll` contains exactly one
packet name string, `RESET_FRAG_PTR` at `0x1f4924`, emitted by `0xfab30` as
`e6 = 3`, `e6 = 3`, then `a1 = 0`, `a3 = 0`, `a4 = 0`; a sibling at `0x764e6`
writes `a1 = 0x28041968`, which is a don't-care filler constant and not an
address (see section 4c).  Index `0xad` shares a static index-group table
entry with `0xa1/0xa3/0xa4` (`0x65514 c7 05 d0 f5 20 00 a1 a3 a4 ad`).  The
emitter is slot 7 of vtable `0x1f48e8`, installed by the constructor at
`0xf7eb0`, and `MxMD.dll` contains no direct call to it [STATIC]; the
M-series Linux driver has the same function at `0x889e50`, reached only from
a dispatch table at `0x121ecf0`.  `0xa0..0xa6` are outside this project's
kernel command whitelist, so testing it needed a kernel change; that change
was made, bounded, and the answer is a clean negative (section 4c).

**The captures give that lead no support at all** [CAPTURED, item 0 of
`tools/m9-faa-capture-evidence.py --stream`]: over 1206 decoded vendor
submissions the *only* index in `a0..a6` ever written is `a5`, eight times,
always zero, and always in the same place - inside the four occlusion-query
captures, paired with `d3` in a single `4545d3a5` packet behind a dependency
marker, which is the query sequence and not this feature.  `a1`, `a3` and
`a4` appear in **no** captured stream.  That is consistent with the vendor's
shipping configuration having multisampling off (section 6), and it means a
whitelist change for those indices rests on the static evidence alone, with
no captured packet to check an emitted one against.

## 4b. The enable is index ef bits 31:29, and it is live [MEASURED]

The lowering of that field is now exact [STATIC].  Lowerer `0x181fa0` shifts
nested `+0x04` left by 25 into surface shadow `+0xb8`:

```
0018233e  c1e219                shl edx, 0x19
00182358  8911                  mov [ecx], edx        ; shadow+0xb8 [31:29] = nested04 [6:4]
00182367  c1e20f                shl edx, 0xf          ; shadow+0xb4 bit 15 = nested04 bit 0
001823ad  8986b4000000          mov [esi+0xb4], eax   ; shadow+0xb4 bit 24 = (nested04 & 6) != 0
```

`0xfae60` folds `+0xb8` into combined state `+0x20` (`0x faef6`/`0xfaefc`) and
`+0xb4` into `+0x28`, and emitter `0xfc0e0` writes them:

```
000fc1ed  c74004f0efd745        mov dword ptr [eax+4], 0x45d7eff0
000fc1f7  payload 1 = combined+0x28      ; index f0
000fc200  payload 2 = combined+0x20      ; index ef
000fc209  payload 3 = combined+0x24      ; index d7
```

`0x45d7eff0` is indices `f0`, `ef`, `d7` with the `0x45` pad - the packet this
driver already emits in both its ranges.  **So the AA mode field is index `ef`
bits 31:29.**

And the gate matters as much as the field [STATIC]:

```
00182637  c1e804 / 83e007       eax = nested04 bits 6:4
0018263d  je 0x18275e                     ; field == 0 -> do not lower ad/af
00182646  je 0x18275e                     ; field == 3 -> do not lower ad/af
001826a5  ad -> shadow+0x28
001826ae  af -> shadow+0x2c
```

The vendor emits the `ad`/`af` pair **only** when the mode field is neither 0
nor 3.  This driver left `ef[31:29]` at zero, so the pair the engine was being
given in sections 2.3 and 3.3 is one the vendor would never have sent - which
is exactly why `ad` was inert and why `af` bit 11 was a protocol violation
rather than a feature.

### What the M9128 does with it [MEASURED]

The field is live, and where it is written matters:

| where | value | result |
|---|---|---|
| preprocessing range | 1 | engine stream timeout |
| raster range | 1, 4 | engine stream timeout |
| raster range | **7** | **renders, and renders differently** |

Writing it in the preprocessing range stops the engine at the very start of
the submission, which is consistent with the field belonging to the surface
shadow and so to the raster range alone; the driver now edits it only there.

With the field written in the raster range as 7 - the value the vendor's own
compiler produces with multisampling **off** and a backed target - the M9128
renders, and drops pixels deterministically:

| probe | multisampling off | `ef[31:29] = 7` |
|---|---|---|
| `faarect`, 240x200 on integer edges | 48000 interior | **42040** interior |
| `faaedge`, the 240x200 diagonal triangle | 24000 interior | **21170** interior |
| `faapos`, `faaposy` staircases | 18 steps at 255, then 0 | identical, unchanged |
| partial levels, either shape | 0 | **0** |

42040 is 240 x 200 x 7/8 within 40 pixels, and the loss scales with area
between the two shapes (5960 and 2830, a ratio of 2.11 for shapes of area
ratio 2).  The staircases are unchanged, so nothing has moved sub-pixel.  It
is therefore a **deterministic drop of roughly one pixel in eight, with no
blending** [MEASURED].

### Which pixels, exactly [MEASURED]

The probe reports the positions of the pixels that were not written.  They
are not scattered:

```
faarect, 240x200 at (8,8), ef[31:29] = 7
  missing 5960 of 48000; row counts 180..240
  distinct row counts    180:60  181:40  240:100
  missing x mod 32  795 0 0 0  795 0 0 0  794 0 0 0  795 0 0 0  695 0 0 0 ...
  missing y mod 32  417 0 417 0  417 0 417 0  358 0 358 0  357 0 358 0 ...
  skipped pixel values  ff000000:5960   (clear ff000000)
```

Every missing pixel has **x a multiple of four and y even**, in absolute
screen coordinates; half the rows are untouched at 240 and the other half
carry 180 or 181.  The same phase appears inside the diagonal triangle - a
120x100 region wholly inside it is missing 1500 of 12000 at exactly the same
offsets - so the pattern is fixed in screen space and is not a property of
the primitive.

The skipped pixels hold the clear value byte for byte, so nothing was written
there: **the engine reserves one lane of every 4x2 pixel block and writes
nothing into it.**  That is what a fragment unit does when it has been
switched on and has nowhere to put fragments.

### Which mode values are live [MEASURED]

| `ef[31:29]` | what the vendor's compiler means by it | M9128 |
|---:|---|---|
| 0 | not an AA state; the vendor refuses to lower ad/af | (this driver's previous behaviour) |
| 1, 2, 3 | AA off, or an unbacked target | renders unchanged, 48000 |
| 4, 5 | AA on, master multisample enable clear | renders unchanged, 48000 |
| **6** | AA on, master enable set, no depth companion | **drops the 4x2 lane** |
| **7** | AA on, master enable set, depth companion | **drops the 4x2 lane** |

The two values the silicon obeys are exactly the two the vendor's compiler
produces when the master `GL_MULTISAMPLE` enable is set (the truth table of
section 4 takes its `B = 0` rows only when validator `+0x174` bit 14 is set),
which is an independent check that the field has been identified correctly.

### What the unit still refuses [MEASURED]

Two combinations stop the engine even with the mode set, and both name the
storage that is missing:

| combination | result |
|---|---|
| mode 6 or 7 with `af` bit 11, alpha to coverage | stream timeout |
| mode 6 with a D24S8 depth/stencil attachment | stream timeout, nothing rendered |
| **mode 7 with a D24S8 depth/stencil attachment** | **stream timeout, nothing rendered** |

The depth refusal is not a property of the mode *value*.  Mode 7 is the value
the vendor's compiler produces when a depth companion exists, and it stops
the engine beside an ordinary depth attachment exactly as mode 6 does
[MEASURED, `faarectd`, one reboot].  So what the unit wants is the companion
surface itself, not a different mode selector.

The depth case is the more telling of the two: the mode value itself is
chosen by whether a *depth companion* surface exists (`[edi+0x18]` in section
4), and the vendor OpenGL driver has a function named
`CreateDepthAA64Buffer` for making one.  Binding an ordinary depth attachment
while the unit is on, with no such companion, stops the engine.  A stopped
engine is not cleared by a module reload; each of these measurements cost a
reboot and was retaken only after `m9-mesa-glsl texture` passed again.

The drop is independent of everything else the pair carries: the same 42040
comes back for coverage values 0, 32, 64, 128, 192, 255 and 256 of 256, with
the invert bit, with the sample-coverage bit, and with `ad` forced to zero
(eleven runs, one sweep, guarded by a known-good fixture at the start).  It
is caused by the mode field **alone**: with `ad` and `af` forced back to
their off forms, `0x20` and `0`, and only `ef[31:29] = 6` written, the same
42040 and the same 5960 skipped pixels come back [MEASURED].  So the mode
field is the enable and the pair is state the enabled unit consumes, not the
other way round.

So `ef[31:29]` is the first field of this feature that the M9128 demonstrably
obeys, and the coverage word still has no effect - which is what one expects
if the unit is enabled but has no storage to put fragments in.  The vendor
allocates that storage separately: `MxMD.dll 0xb69ce..0xb6a19` calls the
allocator `0xac7e0` when the AAMode-bit-0 path has set flag `+0x428` bit 3 and
the surface's sample field `[surface+0x270]` is nonzero, and `0xac7e0` sizes
its main buffer as

```
000ac837  0faf8844020000  imul ecx, dword ptr [eax+0x244]   ; width * height
000ac83e  69c958010000    imul ecx, ecx, 0x158              ; * 344
000ac854  dc0df8551f00    fmul qword ptr [0x1f55f8]         ; * 0.38499999046325684
000ac872  c1e803          shr eax, 3
000ac875  3d00000002      cmp eax, 0x2000000                ; capped at 32 MiB
```

about 16.55 bytes a pixel, plus a second buffer of a sixteenth that size, plus
a surface of the same width and height, plus the resource into which the
`a1/a3/a4` sentinel block is written [STATIC].  Sections 4c and 4d allocate
all of that, bind it where the vendor binds it, and report what the M9128
then did.

## 4c. The store, its four objects and where they are bound

### The allocator, fully decomposed [STATIC]

`0xac7e0` makes **four** objects, through allocation-request structs at
`[ebp-0x3c]` whose fields are `+0x00` flags, `+0x04` pool, `+0x08` format,
`+0x18` width, `+0x1c` height:

| # | result field | flags | format | shape |
|---|---|---|---|---|
| 1 | `[esi+0x08]` main fragment store | `0x8002` | 8 | `0x400` per row, rows from the byte count rounded to 1 KiB |
| 2 | `[esi+0x10]` second buffer | `0x8002` | 8 | the same, on `size >> 4` - or a flat 2 MiB when device flag `[dev+0x1c]` bit 0 is set (`0xac8f2`) |
| 3 | `[esi+0x04]` AA companion surface | `0x1000002` | 5 | the source surface's own width and height |
| 4 | `[esi+0x18]` sentinel resource | `0x8202` | 0 | 5 dwords by 1 |

The second buffer is hung off the first (`0xac974` writes it into
`[main+0x414]`, `0xac968` sets `[main+0x418] = 1`).  Allocation flag bit 24 of
the companion sets `surface+0x234 = 2` and `surface+0x3d0 = 1` through the
capability check at `0x477f0`, which is a sample-count field [DERIVED]; flag
bit 1 makes the allocator build a linked companion of descriptor kind 9
cross-linked at `+0x380/+0x384/+0x388` [STATIC].

The Linux vendor driver names the object: its surviving assert strings carry
`virtual Pnx::RsFragPtrsBuffer* LinuxDevice::CreateFragPtrsBuffer(MUINT32,
Pnx::RsResourceType::Pool)` at `0xe91600`, a sibling of
`LinuxDevice::CreateZBuffer`.  **The multisample fragment store is
`RsFragPtrsBuffer`, a fragment-pointer buffer** [STATIC].

### The indices that bind it [STATIC]

Emitter `0xfb920` - the same function that writes the `ad`/`af` pair - builds
the surface packets from a hardware shadow block at `[unit+0xb4]`
(`0x1e97b` fixes it at `devstate+0xc98+0x3768`), which `0xaa750` fills from
the four objects above:

```
000fbbbf  c7 40 04 b8 b9 45 45   header 0x4545b9b8
          reloc kind 6, pair [edi+0x68]   -> b8 = AA companion surface address
          [edi+0x70]                      -> b9 = its descriptor word
000fbc18  c7 40 04 ba bb 45 45   header 0x4545bbba   (kind 7, [edi+0x74])
000fbc74  c7 40 04 be 45 45 45   header 0x454545be   (kind 8, [edi+0x8c])
000fbd8e  c7 40 04 bc bd 45 45   header 0x4545bdbc
000fbd96  6a 09 / 8d 87 80...    reloc kind 9,    pair [edi+0x80] -> bc = store
000fbdc4  6a 0a / 8d 8f 94...    reloc kind 0x0a, pair [edi+0x94] -> bd = second buffer
```

`0xaa986` files the main store under descriptor kind `0x0e` into shadow
`+0x80/+0x84/+0x88`, `0xaa9fa` the second buffer under the same kind into
`+0x94/+0x98`, and `0xaab42` the companion under kind `0x0d` into
`+0x68/+0x6c/+0x70`.  The M-series Linux driver emits both packets with the
same indices and the same relocation kinds (`0x4b09a3` and `0x4b077f`).

So the binding is:

| index | carries |
|---|---|
| `b8` / `b9` | the AA companion surface and its descriptor - **the slot this driver binds the ordinary depth surface in** |
| `ba` / `bb` | the linked kind-9 companion and its descriptor |
| `bc` | the fragment store's address |
| `bd` | the second, sixteenth-size buffer's address |
| `a1`, `a3`, `a4` | the fragment-pointer registers themselves |

That last row is the correction to section 4's second candidate.  `0xfc8f0`
emits three records of header `0x4545421a` whose index-`0x42` payloads are
`0xa1000009`, `0xa3000009` and `0xa4000009` beside a relocated address at
index `0x1a`, at offsets `+8`, `+0xc` and `+0x10` of the 20-byte sentinel
resource - a save and restore block for those three registers [STATIC,
`0xfca1f`, `0xfcaa2`, `0xfcb0c`; the same code is in the Linux driver at
`0x498c16`].  `0x28041968` is the vendor's don't-care filler payload, a date,
used the same way at `0xfcd2c` beside `0x26041981` and `0x07111983`
[STATIC]; **no index in `a0..a6` ever receives an address in any vendor
image.**

### What the M9128 did with all of it [MEASURED]

Everything below was run with the enable set, `ef[31:29] = 6`, the store
allocated at the vendor's own size and the fragment probe reporting how many
of its dwords the engine changed.  Every dword of the store is filled with
its own index before the draw, so a write anywhere in it is identifiable, and
a known-good fixture runs before and after every case.

| what was bound | faarect interior | partial levels | store dwords written |
|---|---:|---:|---:|
| nothing (the enable alone) | 42040 of 48000 | 0 | - |
| `RESET_FRAG_PTR`: `e6 = 3`, `e6 = 3`, `a1 = a3 = a4 = 0` | 42040 | 0 | - |
| the store's address at `a0`, `a1`, `a2`, `a3`, `a4`, `a5`, `a6`, one at a time | 42040 each | 0 | **0 of 318464** each |
| the store at `bc` and the second buffer at `bd` | 42040 | 0 | **0 of 318464** |
| the same, with a companion surface at `b8`/`b9`, every one of the 128 descriptor format codes | 42040 for 120 of them, **48000** for `0x38`..`0x3f` | 0 | **0 of 318464** each |

No case was refused by the kernel and none stopped the engine.

The one thing in that table that is not flat is the last row, and it is worth
stating precisely because it is easy to misread as success.  **Descriptor
format codes `0x38`..`0x3f` at `b8` make the reserved lane come back**: with
the enable set to 6 the rectangle renders all 48000 of its pixels again and
the diagonal triangle 24120.  But `faaedge` still reports **zero** partial
levels, `faapos` and `faaposy` still print the single-sampled staircase (18
steps at 255, then 0), and the store is still untouched, with or without it
bound.  So that format class does not make the unit antialias; it makes the
unit stop reserving, which is what one expects of a surface class the depth
and fragment stage treats as absent.  It is a fact about the lane, not about
multisampling: **what is bound at `b8` decides whether the lane is reserved,
and no value of it has yet made the unit accumulate anything.**  So the two
addresses `0xfbd8e` names are **necessary but not sufficient**: the unit
accepts them, keeps reserving its lane, and accumulates nothing.  What is
still missing is whatever tells it the surfaces are multisampled - the
allocation-flag bit 24 of the companion reaches `surface+0x234`, which the
descriptor builder `0x467e0` does **not** read, so it must leave the driver
through some other word [DERIVED].

A blind search for that word was run and came back empty.  The store's
address was written, one index at a time, at every index outside the kernel
whitelist in the 3D state neighbourhood - `6d`, `6e`, `6f`, `8b`, `bf`, `c0`,
`c7`..`d6`, `d8`..`e2`, `e4`, `e5`, `e7`..`e9`, `ed`, `ee`, `f2`, `f3`, `f7`
- and **not one produced a single write into the store** [MEASURED].  Three
of them stop the engine (`f8`, `f9`, `fa`) and six suppress the draw entirely
without stopping it (`c8`..`cd`); the rest are inert.

### The capture that would close it

One vendor stream captured with multisampling actually enabled.  On Windows
that is two registry values (section 6); on the M-series Linux driver the
`FragSize` option exists but the allocator does not, so Windows is the only
side that can produce it.  In the saved DMA buffer, the header dword
`0x4545bdbc` is followed by the store and second-buffer addresses and the
header `0x4545b9b8` by the companion surface and its descriptor; every other
word of that submission is then the answer to what this note still calls
[OPEN].  Without it the remaining fields are a search over the whole command
space, and the search above shows that a single address at a single index is
not enough to find them.

## 5. What the M-series does not have

These are clean negatives with their search extent, and they matter because
`parhelia-antialiasing.md` documents all of them for the older Parhelia-512
and it would be easy to carry them across.

| Parhelia-512 construct | M-series |
|---|---|
| fragment-buffer size formula `FragSize * max(w*h/786432, sqrt(w*w+h*h)/1280) * 1 MiB` | **absent.** Its three float constants appear only in `MtxPar/MTXPARD.dll` (`0x19ca10`, `0x19ca0c`, `0x19c9e8`); zero occurrences in `MxMD.dll`, `MxMGLX.dll`, `MxMDI.dll`, `MxMM.sys`, `MxMKDI.sys`.  `MxMM.sys` contains no `fsqrt` in 130363 disassembled instructions and imports no float runtime [STATIC] |
| 2 MiB retry step, 6 MiB floor, 8/16/32 MiB clamps | **absent** [STATIC, operand-anchored search over the whole miniport and UMD] |
| `DWGCTL 0x1300 = 0x20a / 0x00a` | **absent.** CAPTURED: index `c0` is written 558 times in the M9 streams, always zero, never `0x20a` or `0x00a`.  On Parhelia-512 these are the Z-unit tile-tracking commands anyway, corrected in `parhelia-antialiasing.md` section 9a.0 |
| `ThresholdAA` at MMIO `0x12f8` holding `0x007fe000` | **does not carry.** CAPTURED: on Parhelia-512 index `be` holds exactly `02800000` (1722) and `007fe000` (801) across 52 section dumps; on the M9 the same index holds a VRAM backing address, 0 of 2633 writes carry either word, and **no M9 register anywhere is written with `0x007fe000`**.  This corrects `parhelia-gigacolor-faa-hardware-status.md` |
| PLNWT `0x1324` handling | **absent** from `MxMM.sys` [STATIC] |
| any FAA string, allocator or command in the miniport | **absent.** Regex over all 468992 bytes of `MxMM.sys`, ASCII and UTF-16, for `FAA|Frag|AA|multisample|sample|coverage|resolve|antialias|AAMode`: zero hits [STATIC] |

`MxMD.dll` has no nonzero `MULTISAMPLEMASK` consumer either.  Frontend
`+0xe8` has two writers (the render-state 162 handler at `0x10363b` and the
initializer at `0x12de96`) and **no readers**; validator `+0x168` has exactly
one consumer in the whole image, the zero-mask test at `0x138be2`.  The
16-bit word at `+0x358 + 2*target` that it clears is the per-target **colour
write mask**, four identical ARGB nibbles built at `0x138b78..0x138bc8`
(`0x4444` red, `0x2222` green, `0x1111` blue, `0x8888` alpha), not an AA
output mask - a second correction to the earlier note.  So gate 5 is closed
as far as this driver goes: arbitrary per-sample coverage does not exist in
it, and inversion is the AF bit of section 3.2.

## 6. The vendor's own switch

`MxMGLX.dll` does advertise multisample pixel formats, conditionally
[STATIC].  `0x10046f20` publishes `WGL_SAMPLE_BUFFERS_ARB` as
`(pf->0x74 != 0) ? 1 : 0` and `WGL_SAMPLES_ARB` as `(pf->0x74 != 0) ? 8 : 0`
(`0x100470f6 8b 4a 74 f7 d9 1b c9 83 e1 08`), and `GL_SAMPLES` is written as
the literal 8 at `0x100041c0`.  `pf->0x74` is set at `0x10043f32` from a
global filled by driver escape `0x80040610` at `0x10048d10`, which `MxMD`
answers at `0xd4180` from two configuration values:

```
g_msEnable = (OGL\Multisample != 0) ? (3D\AAMode & 1) : 0
```

`OGL\Multisample` is a third configuration name the earlier note does not
carry - string at `0x1e74c0`, read at `0xb9f49`, default **0**.  With
`AAMode` defaulting to `0x80000842` (bit 0 clear) the shipping driver reports
zero sample buffers, and turning multisampling on in the vendor stack is a
two-registry-value change.  Neither name appears in any shipped `.inf`,
`.ini`, `.cfg` or `.msi`; they are code defaults only [STATIC, greps over all
20 INFs and both MSIs].

The saved M-series **Linux** driver
(`m9xdriver-x86_64-1.4.3-80847-20140908-build_98/xserver/*/m9x_drv.so`) is
the same codebase - it carries the C++ signature strings, including
`GLboolean WGLMonitor::CreateDepthAA64Buffer(GLhandle, GLContext*, ULONG,
ULONG)` - but its option-name table holds `FragSize` and **neither
`Multisample` nor `AAMode`** (whole-word string search).  So the same switch
is not reachable through `xorg.conf` on this hardware [STATIC].

Three diagnostics the earlier notes lean on are **dead strings**: the
`CreateDepthAA64Buffer failed`, `ARB_MULTISAMPLE not supported for OGL
independent buffer` and gigacolor page-flipping messages have **zero**
4-byte references anywhere in the 9465856 bytes of `MxMGLX.dll`, and so do
the `__FILE__` strings of the two source files they belong to; the retail
build compiled the trace macro out [STATIC].  They establish that the source
had those code paths, not that this binary runs them.

## 7. The occlusion counter

[MEASURED] With multisampling off, the M9 counter counts **pixels exactly**:
fixture `faaocc` reports 48000 for a rectangle whose readback shows 48000
covered pixels, and 24000 for a triangle whose readback shows 24000.

[STATIC] The vendor agrees.  `MxMGLX 0x100b96f0` sums the four counter slots
through `0x10097240` and then, when the multisample-available byte at
`[ctx+0x258]+0x20` is 1, multiplies the sum by `GL_SAMPLES` at
`[ctx+0x258]+0x08`:

```
100b97ff  80 7e 20 01      cmp byte [esi+0x20], 1
100b9805  8b 46 08         mov eax, [esi+8]
100b9808  0f af 01         imul eax, [result]
```

A driver that multiplies a hardware count by the sample count in order to
report samples believes the hardware counts **pixels**.  `MxMD`'s Direct3D
type-9 result applies no such scaling (`0x1199ef`).

### With the unit on [MEASURED]

`faaocc` attaches depth and so cannot run with the unit on.  `faaoccnz` is
the same measurement with no depth attachment, which is the only way to ask
the question at all.  Both arms are deterministic over three runs:

| probe | mode | counter | pixels the readback shows written |
|---|---|---:|---:|
| rectangle 240x200 | off | 48000 | 48000 |
| diagonal triangle | off | 24000 | 24000 |
| rectangle 240x200 | `ef[31:29] = 6` | **42400** | 42040 |
| diagonal triangle | `ef[31:29] = 6` | **21170** | 21210 |

The counter is **not** multiplied by a sample count: it stays within 0.9% of
the pixels the engine actually wrote, and is nowhere near two or four times
either that or the unclipped 48000.  So with the unit enabled but no
fragment store bound, the counter still counts at pixel rate.  This is
consistent with section 7's vendor evidence - the vendor's GL layer
multiplies the hardware count by `GL_SAMPLES` precisely because the hardware
counts pixels.

It does not yet answer what the counter would do with fragments actually
accumulating, because none are: the small residuals (+360 on the rectangle,
-40 on the triangle) are the reserved lane being counted or not at block
edges, and they are the size of a rounding effect, not of a factor.  The
measurement is repeated once the store of section 4b is bound.

## 8. Sample positions

[STATIC, clean negative] No sub-pixel sample-position table exists in any
M-series binary.  Every non-code section of `MxMGLX.dll`, `MxMD.dll`,
`MxMM.sys` and `MxMDI.dll` was read as little-endian f32 looking for runs of
eight or more values in `[-1,1]` on the 1/16 grid: 9378, 324, 322 and 1160
runs respectively, and **none** with three or more distinct grid magnitudes.
A stricter 1/32 pass found one run, a boolean table.  No double-precision
variant.  There is no quality-level table and no pattern selector.

[MEASURED, method validated] The probe that recovers them exists and is known
to work.  `faapos` and `faaposy` sweep an edge across one pixel in
thirty-seconds and print the coverage staircase; on a software renderer with
a four-sample attachment (`M9_GLSL_MS=4`) they print four plateaus whose step
boundaries are that renderer's sample coordinates, `faaedge` reports exactly
three partial levels, and `faaocc` reports a count/pixels ratio of exactly
4.0.  On the M9128 with the pair written and no enable, all three report the
single-sampled answer.  So gate 6 is [OPEN] only because the enable is, and
the instrument for it is built and calibrated.

## 9. Answers to the nine gates

1. **Does the allocation length imply a sample multiplier or a fragment
   permutation?**  **CLOSED, negative, and the real storage is now found.**
   The colour layout constructor multiplies only width, height, depth and
   bits per pixel [STATIC]; no captured descriptor is enlarged [CAPTURED];
   twenty AD values including the true extent render identically [MEASURED].
   Multisample storage is a **separate buffer group** allocated by
   `MxMD.dll 0xac7e0` at about 16.55 bytes a pixel and capped at 32 MiB
   (sections 4b and 4c), not an enlargement of the colour allocation.  The
   vendor's own name for it is `Pnx::RsFragPtrsBuffer` [STATIC].
2. **AD bits 6:4.**  **CLOSED, and the field is on hardware.**  They are not
   AD bits at all; they are bits 6:4 of nested compiled state `+0x04`, which
   the lowerer shifts into surface shadow `+0xb8` bits 31:29 and the emitter
   sends as **index `ef` bits 31:29** [STATIC, section 4b].  AD's only
   fallback form is `0x20`.  MEASURED: values 6 and 7 change what the M9128
   renders and 1 to 5 do not.
3. **Reserved opcode 5 / auxiliary ID 25.**  **CLOSED.**  It is
   `GL_SAMPLE_COVERAGE_VALUE`, named from the vendor ICD's own
   `glSampleCoverage` sink [STATIC]; AF`[8:0]` is that value in 1.8 fixed
   point.
4. **The other AF source bits and the exact AF construction.**  **CLOSED.**
   AF`[9]` is coverage invert, AF`[10]` the coverage enable, AF`[11]`
   alpha-to-coverage; all three come from auxiliary states 23 and 24, not
   from any D3D render state (221-handler exhaustive check) [STATIC].  The
   earlier reconstruction was three bits out.  [MEASURED] only bit 11 is
   live on the M9128.
5. **`MULTISAMPLEMASK`.**  **CLOSED for this driver, negative.**  No reader
   of frontend `+0xe8`, one consumer of validator `+0x168` - the zero-mask
   suppression - and the word it clears is the colour write mask [STATIC].
   Arbitrary per-sample coverage is not implemented by the vendor; inversion
   is AF bit 9.
6. **Physical fragment order and sample positions.**  **OPEN, and bounded
   from both sides.**  Hard static negative (section 8); on hardware the unit
   reserves one lane of every 4x2 pixel block at a fixed screen-space phase
   (x a multiple of four, y even) and produces no blending - `faapos` and
   `faaposy` print the single-sampled staircase, 18 steps at 255 then 0, with
   the enable set [MEASURED].  Nothing accumulates, so there is nothing to
   read positions off; the probe that would read them is built and calibrated
   against a four-sample software attachment.
7. **The resolve, and the fence before the destination is reused.**
   **OPEN.**  Nothing is accumulated to resolve.  The store is now allocated
   at the vendor's size and bound at the two indices the vendor's emitter
   names, `bc` and `bd`, with the companion surface at `b8`/`b9`; the engine
   accepts all of it and writes **zero** store dwords (section 4c)
   [MEASURED].  Statically, no call edge from multisample surface state to a
   resolve packet was recovered [STATIC, whole-`.text` header scan of both
   the Windows and the Linux image].
8. **What an occlusion query counts.**  **CLOSED as far as the hardware can
   be asked today, negative.**  With multisampling off the M9 counter counts
   pixels exactly [MEASURED]; the vendor's GL layer multiplies by
   `GL_SAMPLES`, so the vendor believes the same, and the M-series Linux
   driver does it too at `0x6bb034..0x6bb04e` [STATIC, two independent
   images].  With the unit enabled the counter is still at pixel rate -
   42400 against 42040 written pixels, 21170 against 21210 - and is not two
   or four times anything [MEASURED, `faaoccnz`, section 7].
9. **A depth format chosen for the internal 0x10/0x11 GigaColor
   resources.**  **CLOSED, negative, confirmed.**  No such branch exists.
   Depth does participate in multisampling, but as a *presence* test: the
   depth companion at `[edi+0x18]` selects between the 6/7 and 4/5 rows of
   the section 4 table.  It selects no depth *format*.  MEASURED: mode 7,
   the value that means "a depth companion exists", stops the engine beside
   an ordinary depth attachment exactly as mode 6 does, so what the unit
   wants is the companion surface of section 4c and not a mode selector.

## 9b. A driver fault this cost, and the guard against it [MEASURED]

The first run of this pass reproduced none of section 4b: with
`M9X_FAA_EF_MODE=6` the M9128 rendered **nothing at all** - 0 of 48000
interior pixels, 0 of 12000 inside the triangle - and did not stop the
engine, which reads exactly like a unit refusing to draw.

The cause was in this driver.  `m9_context.c` filled the and/or pair of
index `ae` and left those of `ef` and `f0` at zero, so an active job wrote
both of those indices as literal 0 and cleared the cull, depth and
render-format bits the pass had built.  The unit test could not see it
because it hand-filled the three and-masks itself.

The three per-index helpers are now one `m9_faa_edits()` that fills every
edited index at once, and the unit test asks that function rather than
writing masks of its own.  With that fixed, `ef[31:29] = 6` reproduces
section 4b exactly: 42040 of 48000 and 21170 of 24000, 5960 and 1500 pixels
skipped at x a multiple of four and y even.

The lesson generalises past this feature: **a state that edits several
command indices must fill them from one object**, or a later index is
silently written as zero and the hardware is blamed for it.

## 10. What this driver implements

`m9_faa.c`/`m9_faa.h` build both dwords exactly as sections 2 and 3 describe,
dispatched on the chip family, and `m9_draw.c` emits them where the captures
put them - in the preprocessing range, 819 of 819, and in no raster range
[CAPTURED].  `M9X_FAA_RASTER` restates them in the raster range so that can be
tested rather than assumed.  `m9_faa_edits()` fills every index this state
edits in one call, for the reason section 9b gives.

`m9_faa_fragment_bytes()` is the vendor's own size arithmetic in 64 bits:
`min(trunc(w * h * 344 * 0.38499999046325684) >> 3, 32 MiB)`, with the second
buffer a sixteenth of it.  That is 16.555 bytes a pixel, and it is already at
the cap by 1920x1200.

Everything is off by default and **no capability changed**:
`m9_is_format_supported` still refuses every multisampled format and
`pipe_screen` still reports no multisample support.  **The hardware has not
earned the change.**  With the unit enabled it reserves one lane of every 4x2
pixel block and writes nothing into it, `faaedge` reports zero partial levels,
the sub-pixel staircases of `faapos` and `faaposy` are bit-identical to the
single-sampled ones, and the fragment store stays untouched with every
binding the vendor's own emitter names.  A driver that advertised
`GL_SAMPLES = 2` here would lose an eighth of every primitive's pixels and
antialias nothing.  The caps change belongs in the same commit as the
measurement that shows `faaedge` reporting partial levels, and not before.

### The probes, and what each one is for

| variable | what it does |
|---|---|
| `M9X_FAA` | the master switch for this path; nothing below has any effect without it |
| `M9X_FAA_EF_MODE` | the AA mode field, index `ef` bits 31:29, in the raster range.  6 and 7 are the values the silicon obeys |
| `M9X_FAA_AD`, `M9X_FAA_AF` | force raw words, because a sweep that cannot reach a value the formula does not produce cannot test the formula |
| `M9X_FAA_COVERAGE`, `_INVERT`, `_SAMPLE_COVERAGE`, `_ALPHA_TO_COVERAGE` | the `af` fields of section 3 |
| `M9X_FAA_STORE` | bind the fragment store at `bc` and the second buffer at `bd`, as the vendor's emitter does |
| `M9X_FAA_COMPANION=<format>` | replace the depth attachment at `b8`/`b9` with a companion surface of the target's shape, described with that format code |
| `M9X_FAA_BIND_RESET` | emit `RESET_FRAG_PTR` exactly as the vendor does |
| `M9X_FAA_BIND_INDEX`, `M9X_FAA_BIND_DESC` | write the store's address at one arbitrary index, for the search of section 4c |
| `M9X_FAA_STORE_CHECK` | keep the store in the CPU-mapped window, fill every dword with its own index, and report at teardown how many the engine changed |

The kernel side is `faa_probe_indices`, a module parameter listing the command
indices the probe may write.  **It is empty by default**, so the shipping
validator is byte for byte what it was, and no index has earned a permanent
entry in `m9_index_allowed()`: `bc` and `bd` are the two the vendor uses, and
on this hardware they are accepted and inert.  `PARHELIA_M9_RELOC_FRAGMENT` is
the relocation kind a probed index may carry; it needs a read/write VRAM
object and page alignment.

**Settings that stop the engine on this hardware** [MEASURED]:
`M9X_FAA_ALPHA_TO_COVERAGE` with or without a mode; any mode value written
into the preprocessing range; and mode 6 **or 7** beside an ordinary D24S8
depth attachment.  A stopped engine is not cleared by a module reload - the
machine has to be rebooted, the module reloaded with `KO=` named explicitly,
`/dev/dri` checked, and a known-good fixture rerun before any later
measurement can be trusted.

### The next step, stated precisely

Not another sweep: section 4c ran the sweep, over `a0..a6` and over every
unallocated index in the 3D state neighbourhood, and every one of them is
inert or destructive, never productive.  What is needed is **one vendor
submission captured with multisampling actually on**.  Turning it on is two
registry values on the Windows side (section 6); the M-series Linux driver
carries the `FragSize` option but not the allocator, so it cannot produce the
capture.  In that submission, `0x4545bdbc` and `0x4545b9b8` name the two
packets this driver already emits, and every other word of it is the state
this note still calls [OPEN]: what marks a surface as multisampled, what the
resolve is, and where the sample positions come from.

## 2026-10-06 independent M9 emulator milestone

`tools/m9emu/faa-trace.py` loads the actual `MSerX/MxMD.dll` and
`MxMM.sys` in separate PE32 instances; it hashes those images plus
`MxMGLX.dll`. Its declared PCI identity guard accepts only102b:0540. This
is not a captured PCI/PCIR bootstrap and does not emulate M9 silicon. It
imports the generic PE32/Windows host services, never the Parhelia/P690
hardware model, miniport adapter or initialized snapshots. P6902539/MtxPser
and LX identities are rejected rather than treated as M9.

**EXECUTED VENDOR CODE, CONTROLLED HOST ABI:** `MxMD.dll` entry10510
returns1 and registers35 DDI callbacks. `MxMM.sys` DriverEntry55e05 returns0
and calls VideoPortInitialize with actual FindAdapter55ebd and
HwInitialize56304. FindAdapter currently stops honestly at24ba1 requesting
`MmAllocateMappingAddress` through an unimplemented system-routine lookup.
No unsupported callback is forced to succeed. Subsequent adapter hardware,
registry, resource ownership and D3D initialization remain open.

The complete actual surface emitterfb920 executes with a controlled
shadow/command-buffer owner: AD2800 is written four times, followed by
AF100 once, then its dirty bit80 clears. This is dynamic execution of the
existing static emitter contract, not an API-created multisample surface.
The complete reset emitterfab30 additionally produces exactly
`[6,4545e6e6,3,3,45a4a3a1,0,0,0]`, with its actual RESET_FRAG_PTR begin/end
label callbacks observed. The controlled callback sinks only record labels;
they do not supply GPU status or claim a valid reset dispatch. Neither
execution establishes sample allocation, FAA admission, resolve order or
physical retirement. No M9 hardware was accessed.

Run with the established Unicorn environment:

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/faa-trace.py \
  --pci-id 102b:0540 --out captures/m9-faa-emulator-20261006/trace.json
python3 tools/m9emu/check-trace.py
```

Retained hashes and actual output are in the M9-named capture directory.
Next: implement the genuine OS reserved-mapping service contract, then
observe FindAdapter's actual M9 PCI/resource requests before attempting
D3D multisample allocation/binding. Missing M9 register or firmware inputs
must remain explicit boundaries; P690 register behavior must not fill them.

The optional `--os-mapping` follow-on models bounded virtual-address reservation
and ownership, independently of any M9 bus address. It resolves the reserved
MDL mapping function names to explicit stop-only endpoints: invoking either
without a real MDL/PFN model stops instead of returning fake mapping success.
With bounded host pool allocation, FindAdapter advances to the concrete
`KeInitializeMutex` import at1066d; no PCI/MMIO operation has been modeled or
executed. `os-mapping-trace.json` retains this separate boundary. The default
baseline capture still stops at the original mapping-routine query, allowing
its proof to be rerun without the optional adapter. This is host-service setup,
not initialized M9 hardware or recovered multisample allocation admission.

### NT synchronization and captured M9148 device-input progress

The independent MSerX harness now implements single-thread recursive mutexes,
notification/synchronization events and bounded mapping-address reservations.
Unsignaled event waits stop rather than fabricate GPU completion. The executable
`tools/m9emu/check-nt-host.py` checks recursion, release ownership, event reset,
zero-timeout and refusal to invent completion.

`--os-mapping --device-inputs` supplies captured M9148 PCI 102b:0540 configuration
from `m9x-cold-init-20260915-v1` and validates the genuine 128 KiB ROM shadow from
`m9x-vendor-fourhead-20260917` (matching PCIR identity, image checksum). These are
separate captures, not a single Windows startup snapshot. Assigned BAR sizes and
legacy VGA access grants model OS resource ownership; no register values or
engine completion are supplied. The miniport reaches actual VideoPortMapMemory
at caller 0x6c9cf, requesting BAR2 0xfebfe000/8192 bytes, address-space flags 4.
This supersedes the earlier routine-import stop, but FindAdapter/HwInitialize
are not yet complete. `synchronization-device-trace.json` retains exact image and
input hashes plus the request. Next required work is an independent M9 register
mapping model with captured read provenance; a P690 register model is unsuitable.
Controlled AD/AF and RESET_FRAG_PTR command producers still pass. This progress
is command generation and bootstrap evidence, not FAA admission or retirement.

The next bounded run supplies only the virtual BAR2 mapping (OS address glue)
and intercepts **every** device read/write before it can consume invented data.
`bar2-first-access-trace.json` records the first genuine access: 32-bit read at
BAR2+0x304, executing MxMM.sys PC 0x2a3c5. No value is supplied and initialization
stops there. This identifies a real M9 device-input boundary, rather than a
missing VideoPortMapMemory implementation. Existing Linux M9148 register
snapshots require explicit cold-state/register provenance before replay; their
values cannot be presumed valid Windows initialization or completion state.

### Captured initial serial control and real bootstrap rejection

Both retained M9148 `pre-mode*/mmio-live.bin` snapshots read BAR2+0x304 as zero.
The harness now supplies that captured initial control and separately models
only the serial READ protocol grounded in MxMM.sys: 0x448ef sends opcode03 and
24-bit address; 0x449ca writes MOSI bit8, 0x44a93/0x44ac1 toggle clock bit4,
0x44960/0x4499c select/deselect bit0, and 0x44a3a samples MISO bit16 MSB-first.
Unknown opcodes and reads beyond the captured ROM stop. The independent
`check-serial-rom.py` proves bit order and bounds. No engine status is modeled.

`captured-bar2-bootstrap-trace.json` records FindAdapter returning status87,
not success. The actual decision chain 0x5d68f has flags2; helper0x589b0 returns1,
0x116c1 returns0, 0x11b73 returns1, then 0x11484 returns16 at0x5d6e6. This causes
rejection; HwInitialize is correctly skipped. No serial ROM transaction has
executed in this run yet. The next concrete investigation is the 0x11484
bootstrap requirement, rather than overriding its result or admission gate.

### Serial identification boundary and control correction

Further source tracing corrects the earlier serial model: MxMM 0x44960 sets
control bit0 to **select** the flash, and 0x4499c clears it to deselect. The
previous opposite polarity supplied no flash data and produced status87; that
rejection is a harness artifact, not an unsupported M9148 revision. At0x303d5
that zero-filled firmware result reached the PInS-version check at0x1157a.
The corrected bit-order test now uses this actual select polarity.

The real FindAdapter path first calls flash identification 0x4472b. It emits
opcode0x15, samples two bytes at0x44766/0x44775 and checks first byte0x1f.
Alternative paths send opcode0xab with three zero bytes (expect0x10), then test
other identities. The harness stops at the first actual opcode0x15 rather than
inventing its response. `serial-identity-boundary-trace.json` retains this
executed boundary. The captured ROM image proves firmware contents, not flash
manufacturer/status responses; `board-eeprom-miscio.txt` is a separate slave
0xa0 EEPROM and cannot supply SPI identity. Required next genuine input is the
M9148 serial-flash identification response, or a retained vendor-process flash
object populated by these calls. No M9 target operation has been attempted.

### Genuine serial identity recovered from retained vendor-X traffic

The previous identity boundary is now crossed without a new hardware operation.
The pinned genuine M9148 `xinit-mmio.jsonl` already records MISO bit16 in
vendor GPIO read-modify-write arguments at rising clock edges. The independent
decoder `tools/m9emu/serial-inputs.py` verifies the trace hash, vendor-image hash,
and phase/bit ordering against **15 separate known ROM reads, 1123 bytes**,
including `PCIR`, PCI identity, the `0x7ffc` PInS pointer and the complete
1024-byte PInS record. Two chained-image probe reads do not match the retained
ROM shadow and are explicitly excluded from that verification. These echoes
are derived physical inputs, not directly logged read return values.

The first captured transaction (seq14) sends `15`, returning `ff ff`; the next
(seq76) sends `ab 00 00 00`, returning **`bf 49`**. Genuine MxMM recognizes
that alternative at `0x44858/0x44861` and builds the serial descriptor with
manufacturer `bf`, granularity1 and size`0x20000`. The two-byte response is
bounded; an unknown opcode or additional uncaptured identity byte still stops.
These inputs are from a historical M9148 boot, not the current nano boot, and
do not establish a current board's flash identity if its model differs.

With optional `--serial-inputs`, the actual FindAdapter executes flash identity
and ROM READs at `0`, `0x7ffc`, `0x7460`, `0x7464`, `0x7462`, `0x7460`.
It passes firmware discovery and next stops at its real **BAR2+0x318 read**
(MxMM PC`0x2a3c5`). `captured-identity-bootstrap-trace.json` retains exact
provenance, transactions and stop. The larger bounded instruction budget only
allows the bit-banged PInS copy to finish; it supplies no GPU completion or
unknown register value. FindAdapter/HwInitialize and a genuine API-created
FAA allocation/draw/resolve remain incomplete.

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/faa-trace.py \
  --pci-id 102b:0540 --os-mapping --device-inputs --serial-inputs \
  --out captures/m9-faa-emulator-20261006/captured-identity-bootstrap-trace.json
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/check-serial-rom.py
python3 tools/m9emu/check-captured-identity.py
```

Next inspect the genuine `0x318/0x3b0` ownership protocol. Captured baseline
values alone cannot stand in for every subsequent grant/acknowledgement state.

### Entire surface emitter: conditional index24 / E6 / store binding

`tools/m9emu/surface-packets.py` now executes the full genuine `fb920` function
with explicit controlled address/descriptor markers and a relocation sink.
The **shadow is not produced by an initialized API allocation**: this proves
the function's conditional packet order and relocation contract, not FAA
admission or the validity of marker descriptors. Owner-allocation dirty bits
remain zero; no allocator/reference callback is forced to succeed.

With surface dirty mask`0x8f0` and shadow pair`+0x9c/+0xa0` different from
the driver's global null pair`20d51c/20d520`, genuine execution emits:

1. B8/B9 (relocation6), BA/BB (7), BE (8).
2. **index24**, relocation kind`0x2b` from shadow`+0x9c`.
3. **E6 three times**, payloads`0x6dc00000`, `1`, `1`.
4. BC/BD, relocation kinds9/10 from shadow`+0x80/+0x94`.
5. AD four times and AF once.

The precise middle **software capture-envelope** words are (the leading
1/3/2 counts are not GPU command dwords; raw mode is proved below):

```text
00000001 45454524 <controlled address marker>
00000003 45e6e6e6 6dc00000 00000001 00000001
00000002 4545bdbc <controlled store marker> <controlled second marker>
```

When shadow`+0x9c/+0xa0` equals the genuine global null pair, the function
**skips the complete index24 / E6 / BCBD block** and leaves dirty bit`0x40`
set, while emitting the other dirty state. Thus the store pair is gated by
an additional bound object; emitting BC/BD alone does not reproduce this
vendor producer. The clear case retires all controlled dirty bits.

Current native M9 FAA code lacks index24 and this E6 triple; its kernel
validator also rejects these E6 payloads. **Do not broaden the whitelist or
insert a guessed index24 address.** The exact owner/resource producer and
real relocation semantics must be established first. Independent static
tracing locates shadow`+0x9c` producer in binder`aa750` at`aad13`, fetching
a device-owned per-slot resource and invoking its descriptor method`+0x5c`
with kind5; this is not an alias of BC fragment storage or B8 companion.

The conditional outputs and both relocation lists are retained in
`full-surface-packets.json`; `check-surface-packets.py` proves both branches.
No hardware operation or physical GPU completion occurred in these runs.

### Current M9128 ownership capture and bounded host replay

The root hardware owner ran `tools/m9emu/capture-hmc.c` with idle GPU/display
clients and the established target lock on the current **M9128 LP PCIe x16,
102b:0540 rev1, 102b:22c0**, boot`815d4dcc`. The cleanroom driver had already
initialized the board. The captured request changes only miscIO request bit28:

```text
318 saved70000003 -> requested60000003 -> restored70000003
3b0 before0000ffff -> acquired0000ffff -> after0000ffff
```

The helper guards PCI identity and existing ownership, completes all fallible
setup before requesting ownership, blocks asynchronous signals across the
request/read/restore operations, flushes posted writes with reads and prints
only after restoration. It sends no serial data, flash write or erase command.
Its source builds with `-std=c11 -Wall -Wextra -Werror`.

Optional `--hmc-inputs` replays only this captured bit28 request/restore and
grant state; unknown request bits, DDC requests and writes to the grant
register remain explicit stops. This modern cleanroom postinit snapshot is
**distinct from the historical M9148 vendor-cold PCI/ROM/SPI captures** used
by bootstrap; the harness records this mixed-input provenance explicitly.
It supplies no engine completion or GPIO input value.

Genuine MxMM FindAdapter now executes the `0x341bb` ownership acquisition,
clears request28 and observes the captured grant, then stops on its actual
**BAR2+0x1c4c GPIO read**. `captured-hmc-bootstrap-trace.json`, the raw
`m9128-ownership-request.json` and board identity retain the evidence.
`check-hmc-inputs.py` proves the bounded state transitions and refusal to
invent other operations. Full FindAdapter/HwInitialize remain open.

Next bootstrap input is the actual miscIO I2C/board-EEPROM protocol; a whole
live-register snapshot must not stand in for dynamic slave ACK/data. For FAA,
the newly exposed device-owned index24 resource and conditional E6/BCBD
producer remain the highest leverage packet investigation.

### Real index24 resource getter and descriptor producer

`tools/m9emu/resource-address.py` now executes the complete real interface
constructor `a32a0`, real descriptor getter `a6740` (vtable `1f3a90`, slot
`+5c`), descriptor builder `467e0` with its helper `47350`, and surface emitter
`fb920`. Inputs are explicit controlled resource/layout fixtures; this does
**not** initialize the device's pool-6 allocation or establish its lifetime,
contents, domain, size, Windows API ownership or hardware completion.

The low-level resource's `+268` contains a **pointer to a separate interface**,
not an inline interface. `a3870` loads this pointer at `a38b8`. Descriptor-kind
5 branches at `a6776` to descriptor selector **2**, not format 5. The getter
uses layout `[interface+c0]+3c`; `467e0` creates triple `layout+20/+24/+28`
and marks validity bit `400` at `layout+11c`. A second genuine getter call
uses this cache and never reenters the builder.

`47350` at `47370..47383` only admits the layout address `+15c` when layout
bit1 is set. Layout `+144` is separate metadata, **not the mapped address**.
The fixture uses format `78`, dimensions `256 x 1 x 1`, stride256, address
`06000000` and zero layout mode. Actual builder output is
`{06000000, f0000100, 00010001}`. None of those fixture values establishes
the real pool-6 allocation's descriptor format or layout.

The two real getter branches produce distinct relocation contracts:

| Input condition | Getter pair at shadow `+9c/+a0` | Additional state |
| --- | --- | --- |
| `interface+a4 == 0` and `interface+a8 == 0` | `{0, address}` | direct mapped address |
| either field nonzero | `{40000000, &interface+74}` | `+74` backing-owner pointer from resource`+1e4`; `+78` address |

Passing either pair into real `fb920` emits index24 via relocation kind`2b`,
then E6`6dc00000,1,1`, then BC/BD. The controlled relocation sink records the
direct address or follows the explicit indirect pair; it does not assign a
physical address to a real backing owner. Clearing layout bit1 makes the real
getter produce `{0,0}`; `fb920` then omits the entire block and retains dirty40.
`resource-address.json` preserves all three cases. `check-resource-address.py`
checks actual constructor/getter entries, descriptor cache, both pair forms,
exact packet order and the address-admission negative control.

Static pool-6 request correction: `76540` initially writes request`+8=1a` at
`7656e`, but **overwrites it with zero at `765e3` before calling `a3870`**.
The final request format is therefore zero, not `1a`. Pool6 flags are `8002`;
width is `ceil(bytes/4)` and height is `ceil(width/1024)`, with other dimensions
1. `a3870` at `a38e2..a38fa` derives interface`+a4/+a8` from request bits8/9,
both zero for `8002`; this selects the mapped getter branch if that request
successfully reaches a valid low-level resource. Pool-6 allocation and its
initial data are still open and must be captured before native index24 use.

`76540` has no direct `E8` callers: it is normal owner vtable `1f3850` slot
`44`. Initializer `11be20` calls that slot at `11c0be` with pool6 and byte
count `500` (1280), stores the result at owner`+78`, and passes it to `109310`.
That setter binds context`+2514` subshadow`+50` and sets its dirty bit8. This
is ordinary context initialization, not proof of an FAA-only resource.

`tools/m9emu/pool6-request.py` executes the genuine vtable method with those
arguments and an explicit hardware/cache owner, stopping **before** low-level
`a3870`. The actual final request has flags`8002`, format0, width320 at`+18`,
height1 at`+1c` and dimensions1 at`+20/+24`. The pointer at`+4` derives from
hardware`+1015c + 2060`; its selected profile entry is now constructed below,
while the physical address domain remains unproven.
Request`+c/+28` are not initialized by this producer and are omitted from
the reported initialized fields rather than treating host stack zeros as
vendor data. There is no fake successful allocation callback.

A second case uses a separately constructed cached interface at real slot
`hw+fed8+slot*144`. Two genuine method calls perform no allocation and return
interface`+58`, incrementing interface`+5c` from1 to3. This proves the cached
reference increment contract; bounded context release is also executed below,
while final hardware retirement remains unproven. Captured
`pool6-request.json` and `check-pool6-request.py` preserve both cases. Actual
allocation domain, initialized data, final hardware release and the enabled surface
resource graph remain the next requirements.

### Software capture envelopes versus raw GPU commands

The emitter's stream modes must not be conflated. At `fbb9e..fbbb3`, real
`fb920` tests stream`+10 == 1` and stream`+14 != 0`, then writes a count2
before B8/B9. Otherwise `fbbb5` subtracts4 from the cursor before the same
header/payload stores. The latter consumes the reserved count slot and
directly generates raw indexed GPU words. The index24/E6/BCBD and AD/AF
blocks use the same mode branch. The returned cursor is already exactly
after the last payload: **do not subtract4 again when exporting it**.

`surface-packets.py` now independently executes both modes of the actual
emitter; `full-surface-packets.json` keeps old envelope `cases` separately
from raw `gpu_cases`. `resource-address.py` also calls the actual emitter
again in raw mode after each real getter case and records `gpu_words` and
`gpu_relocations`. These words are not produced by Python count stripping.
Both checkers parse raw output with the existing strict
`m9-command-decode.py` indexed-packet grammar. The dirty40 block has exactly
three GPU packets and nine dwords:

```text
45454524 <one 32-bit relocated address>
45e6e6e6 6dc00000 00000001 00000001
4545bdbc <32-bit store address> <32-bit second-buffer address>
```

The first payload is relocation kind`2b`, flags1; the E6 header contains
three index-E6 writes, not a leading GPU count3. Enveloped fixtures are
intermediate software records and must never be submitted directly through
the cleanroom raw command ABI. This establishes the actual emitter's raw
grammar; a complete initialized vendor submit/export lifecycle remains open.

### Genuine relocation resolver and kind2b payload width

The new `resource-address.json` `genuine_relocation_cases` execute the real
command-stream relocation method `25680` and relocation-kind encoder `24870`.
Real stream vtable `1f2c60` slot`54` points to `25680`; the fixtures retain
controlled stream/resource/layout ownership, but replace the controlled sink
with that actual method. Passive instruction/write hooks only record arguments
and vendor payload stores. Direct and indirect getter cases produce the same
nine raw dwords, with actual `2b/9/10` relocation writes.

`25680` resolves the pair's top-three-bit address class: class0 uses the second
word directly; class1 loads resource`+1c4`; class2 loads reference`+4`. Class2
therefore consumes exactly the `{40000000,&interface+74}` pair produced by the
real getter. It separates the base address's low2 bits, sign-extends the pair's
low29-bit offset and adds that offset to the aligned base. Kind`2b` selects
jumptable byte9 at `24993`, reaching `24930`: the encoder restores the low2
bits and writes **one complete 32-bit payload**. There is no kind2b-specific
address truncation. This closes encoding and indirection, not the allocation's
physical domain, contents, lifetime or normal enabled FAA admission.

### Pool6 profile construction and bounded context release

The normal paired module startup table has profile initializer `4ed00` at
`1f25f8`. It allocates `31bc` bytes, calls `4ede0` and publishes the returned
profile collection at hardware`+1015c`. Constructor `4ede0` initializes86
entries, each `94` bytes, by calling actual `4fe00`. Pool6 offset`2060` is
therefore entry56 decimal. The fixture now executes real owner constructor
`751f0` and actual `4fe00` for that selected entry, rather than substituting
profile data. This produces class5, hardware format`78`, bits-per-element32,
metadata pointer`1e6ca0`, flags`+c=0` and sentinel`+2c=ffffffff`. These are
actual selected-entry outputs; the complete collection constructor still
requires configured-owner methods for other entries. Physical allocation and
the profile's memory-domain consequences remain open.

After two actual cached acquisitions, `pool6-request.py` now executes actual
context binder `109310` and complete context destructor `11c190` on two
controlled context owners. Each binder stores the returned subinterface at
context`+2514` subshadow`+50`, sets dirtybit8 and returns1. Destructor
`11c4a9..11c4dd` decrements subinterface`+4` (interface`+5c`), clears owner`+78`
and returns1. The two releases take refs3 to2 to1 while preserving the
device-slot cache. This proves ordinary context reference lifetime; it never
reaches the last-reference virtual teardown or establishes device shutdown,
GPU completion, allocation contents or enabled FAA surface admission.

### Separate actual M9128 PCI and legacy-ROM inputs

`captures/m9128-device-inputs-20261006/` preserves root's sole-owner, idle
read-only capture from the installed M9128 LP PCIe x16, rev1, subsystem
`102b:22c0`, boot`815d4dcc`. The4096-byte PCI config SHA is
`b45f1f8d86bd041d6824d6bf4dea11fbfa9113d3de5a199d64aee32a9ec9c7e0`.
The131072-byte legacy ROM shadow SHA is
`34064c0553954252f0d45fb78150a30c5773ea8205a1bfb8995d9711ff874cf4`.
Sysfs ROM reading failed with EINVAL without changing ROM-enable state;
`/dev/mem` legacy shadow reading succeeded. The shadow has55aa, PCIR at60
withVID/DID102b/0540, declared61440-byte image checksum0, board string
`128-E1024LAF` and serial`KFP35758`. This establishes the shadow's board
identity, not independent dynamic SPI transaction responses.

`faa-trace.py --os-mapping --m9128-inputs` uses this actual PCI/ROM/resource
set through `m9128-inputs.py`, independently of the older M9148 capture.
Actual resources put BAR2 at`fdffe000`, versus historical`febfe000`.
The genuine MSerX miniport FindAdapter reaches that actual BAR2 mapping;
the model now uses the separately captured BAR2`+304=0` baseline to clock
the genuine first SPI header, then deliberately stops at opcode15's uncaptured
identity response. It cannot borrow
M9148 initial MMIO, opcode15/AB identity responses or later completion
state. Current actual idle GPIO values are baselines, not an EEPROM/SPI
protocol trace. `check-m9128-inputs.py` verifies the board hashes, actual
mapping and this precise refusal boundary. The scalar baseline's SHA is
`7361bda8958499b6d0447316723db92767916c44e26f41982dec1dda2601a620`;
it is not a substituted whole MMIO snapshot. Physical SPI identities and
board-EEPROM slave responses remain required to continue actual-board
bootstrap.

The full actual profile publisher `4ed00` was subsequently executed with
the separately initialized real config owner `61f70`, returning1 and
publishing all86 entries at hardware`+1015c` and device`+69a8`. Its earlier
100k-instruction stop at`50af0` was a fixture instruction limit during
mask counting, not a missing capability rejection. The complete constructor
works with a1M-instruction bound. Ordinary card-list registration `27230`
also returns1, links device`+69c0` and publishes global`20d1cc`; the normal
device-init loop at`2e03a..2e056` calls it from paired module table`1f2638`.
These CPU ownership steps do not substitute for initialized physical
capability owners or prove M9128 FAA admission.

### Bounded actual-board SPI identity capture helper

`tools/m9emu/capture-spi.c` is prepared for root's review and sole-owner,
locked idle-target execution; root subsequently ran it successfully, as
documented below. The helper
requires PCI102b0540 rev1 subsystem102b22c0 and inactive CS/clock/MOSI/1000
controls. It changes only BAR2`+304`, issuing read-only flash commands15,
AB000000 (only if command15's first byte is not1f, matching vendor fallback)
and03000000 with an8-byte known-ROM read. The read protocol includes the
genuine1000 control transitions; it is not a simplified three-signal SPI loop.

Actual MSerX miniport `4472b` emits15 at`44757`, reads two bytes, then
conditionally emitsAB/three dummy bytes at`447d6..447f5` and reads two bytes.
Actual ROM producer `448ef` sends03/address24 and reads its requested count.
Their shared helpers are `44960` (clock-low, set1000, CSassert), `44aef`
(clear1000), `449ca` (eight MSB-first MOSI/clock cycles), `44a3a` (eight
clock cycles with MISO16 samples), `44b20` (set1000) and `4499c` (CSrelease).
`spi-protocol.py` independently executes those real functions with a clearly
controlled zero-MISO negative fixture. Its retained `spi-protocol.json` has
196 identity/fallback writes and230 ROM-read writes. The capture helper's
426 emitted words are checked word-for-word against that genuine execution.
These zero response bytes are **not** captured M9128 flash data.

The helper checks a1-second monotonic deadline before every write. All fallible
setup precedes writes, asynchronous signals are blocked during transactions,
and every transaction error reaches unconditional saved-control restoration
before output/unmapping. Input-only MISO bit16 is excluded from the restoration
comparison. `check-capture-spi.py` uses ordinary temporary files, never PCI:
it verifies exact vendor writes, restoration on known-ROM mismatch and forced
deadline failure within each of the three transactions, and no-write refusal
for active controls, wrong identity and already expired deadlines. Actual
target bytes must match the retained board's ROM header before identity
responses are admitted to the bootstrap model.

### Actual shared resource owner creation before VRAM inputs

Ordinary module initializer `19420` publishes device`+6988` from
hardware`+35f8` shared owner`+4`. If that shared owner is missing, it calls
actual `19560` with EAX=device. `shared-resource-owner.py` now executes real
`61f70`, `4ed00`, `27230` then `19560`, using root's separately captured idle
M9128 cleanroom postinit BAR2`+f04=0448803e` baseline (SHA
`f7ed34bdf7855e12d7e68220661209b098cca91ddb84524799f5d9949a511611`).
The captured`+3a8=00010a05` is retained but has not been read by this execution.
This is conditional execution from current cleanroom state, not a capture of
the vendor's cold hardware initialization or initialized capability owners.

Real `19560` allocates/zeros a2c-byte shared owner, publishes it at
hardware`+35f8`, then reads f04 and records fields`+24=2` and`+28=1` from its
actual bit extraction. It writes f04 unchanged, then exact ordered MMIO:
344=0,348=0,34c=0,350=0,354=03020100,334=0,338=40,33c=80,340=c0,330=1000.
Those writes are actual vendor instructions in emulator memory, never target
writes. `check-shared-resource-owner.py` verifies the exact order and values.
At `196e1` execution stops before using unproven platform`+98/+a0` and
hardware`+10` VRAM descriptor inputs. That boundary precedes real25a70/44b90
backing-allocator ownership and its device`+6988` publication. Profile
construction, address encoding and ordinary context refcounts are proved;
physical allocation domain, resource contents and enabled surface admission
remain open.

### Actual M9128 SPI responses now captured and admitted

Root ran the reviewed bounded capture under the established locks with
X/GPU clients stopped. Actual M9128 returned command15=`ffff`, AB=`bf49`
and ROM8=`55aa78eb7b17ba02`, exactly the retained board shadow's first8
bytes. The capture completed426 writes, saved/restored304=0, timeout0,
return0 and no new error on the same boot815d4dcc. Retained
`spi-identity.json` SHA is
`707744e68f7374669c42a33322d488be31591936e4451198810f84cbc4db7014`.
These replies happen to match M9148, but are independently measured on this
installed M9128 and no longer borrowed from another board.

`M9128Inputs` now validates the physical capture against board identity,
restoration and known-ROM8, admits those identity bytes, and reuses the
bounded ownership capture from this same M9128 boot. Real FindAdapter
continues through ROM header/PInS accesses at0,7ffc,7460,7464,7462,7460,
then ownership31870000003→60000003/grant3b0ffff, and stops precisely at
uncaptured GPIO1c4c. Full ROM payloads still come from the actual board's
legacy shadow; physical SPI verification covers its first8 bytes, not every
payload byte. No EEPROM ACK/data or engine completion is supplied.

At this boundary actual miniport owner`+4 +1ec` already points to a genuine
indexed configuration instance. Executing real `26d9d` finds its single
type14/208-byte record. Its initial header holds1/0540 at offsets0/4, but
fields8/1ac/1b0/204 are still zero before GPIO-dependent initialization.
`bootstrap.json` preserves its pointers and exact208 bytes under
`partial_capability_record`. This is an allocated, incompletely initialized
record; zero`+204` must **not** be used as evidence of unsupported M9128 FAA
or fed to normal ICD admission as initialized capability data.
