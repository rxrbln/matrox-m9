# M9 multisampling (FAA): static closure, captures, and what the silicon did

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

This note closes the multisampling investigation for the M-series (M9/P690)
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

A second candidate exists [STATIC]: `MxMD.dll` contains exactly one packet
name string, `RESET_FRAG_PTR` at `0x1f4924`, emitted by `0xfab30` as
`e6 = 3`, `e6 = 3`, then `a1 = 0`, `a3 = 0`, `a4 = 0`; a sibling at `0x764e6`
writes `a1 = 0x28041968`.  Index `0xad` shares a static index-group table
entry with `0xa1/0xa3/0xa4` (`0x65514 c7 05 d0 f5 20 00 a1 a3 a4 ad`).  The
emitter has no recoverable caller - it is reached only through a vtable slot
- so its role is [OPEN].  Note that `0xa0..0xa6` are outside this project's
kernel command whitelist, so testing it needs a kernel change.

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
`a1/a3/a4` sentinel block is written [STATIC].  None of that exists in this
driver, and the indices that would address it are outside the kernel's command
whitelist, so the next step is an allocation and a binding, not another sweep.

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
   (section 4b), not an enlargement of the colour allocation.
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
6. **Physical fragment order and sample positions.**  **OPEN, and now
   bounded.**  Hard static negative (section 8), and on hardware the unit
   reserves one lane of every 4x2 pixel block at a fixed screen-space phase
   (x a multiple of four, y even) while producing no blending [MEASURED].
   That is the shape of the fragment storage, not of the sample pattern; the
   sample positions themselves stay open until the fragment store is
   allocated and bound, and the probe that reads them off a staircase is
   built and calibrated.
7. **The resolve, and the fence before the destination is reused.**
   **OPEN**, and now for a concrete reason: nothing is accumulated to
   resolve.  With the unit enabled the engine reserves a lane and writes
   nothing into it [MEASURED], because the separate multisample buffer group
   of section 4b is never allocated.  Statically, no call edge from
   multisample surface state to a resolve packet was recovered: the AD/AF
   emission is gated only by a surface-shadow dirty bit, never by a
   multisample condition, and no emitter selects a different index because a
   surface is multisampled [STATIC, whole-`.text` header scan of both the
   Windows and the Linux image].  The one packet-name string in either image
   is `RESET_FRAG_PTR`, issued after every blit rather than at a resolve.
8. **What an occlusion query counts.**  **CLOSED as far as the hardware can
   be asked today, negative.**  With multisampling off the M9 counter counts
   pixels exactly [MEASURED]; the vendor's GL layer multiplies by
   `GL_SAMPLES`, so the vendor believes the same, and the M-series Linux
   driver does it too at `0x6bb034..0x6bb04e` [STATIC, two independent
   images].  With the unit enabled the counter is still at pixel rate -
   42400 against 42040 written pixels, 21170 against 21210 - and is not two
   or four times anything [MEASURED, `faaoccnz`, section 7].  The question
   is reopened only if a bound fragment store changes it.
9. **A depth format chosen for the internal 0x10/0x11 GigaColor
   resources.**  **CLOSED, negative, confirmed.**  No such branch exists.
   Depth does participate in multisampling, but as a *presence* test: the
   depth companion at `[edi+0x18]` selects between the 6/7 and 4/5 rows of
   the section 4 table.  It selects no depth *format*.

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
tested rather than assumed.

Everything is off by default and **no capability changed**:
`m9_is_format_supported` still refuses every multisampled format and
`pipe_screen` still reports no multisample support.  That stays true until a
measurement earns the change, which on the evidence above means until the
enable of section 4 is found and `faaedge` reports partial levels on the
M9128.

`M9X_FAA_AD` and `M9X_FAA_AF` force raw words, because a sweep that cannot
reach a value the formula does not produce cannot test the formula.

`M9X_FAA_EF_MODE` writes the AA mode field into the raster range.  Modes 6
and 7 are the ones the silicon obeys.

**Three settings stop the engine on this hardware**: `M9X_FAA_ALPHA_TO_COVERAGE`
with or without a mode, any mode value written into the preprocessing range,
and mode 6 or 7 beside a depth attachment.  A stopped engine is not cleared
by a module reload - the machine has to be rebooted, the module reloaded with
`KO=` named explicitly, `/dev/dri` checked, and a known-good fixture rerun
before any later measurement can be trusted.

### The next step, stated precisely

Not another sweep.  Allocate the buffer group of section 4b - about 16.55
bytes a pixel, a second buffer a sixteenth of that, and a same-size surface -
and find the command index that binds it.  The candidate register block is
`0xa1/0xa3/0xa4`, which share a static index-group table entry with `0xad`
(`MxMD.dll 0x65514 c7 05 d0 f5 20 00 a1 a3 a4 ad`) and which the vendor
writes as `a1 = 0x28041968, a1 = 0, a3 = 0, a4 = 0` once at context creation
and as `a1 = a3 = a4 = 0` after every blit, under the only packet-name string
in the image, `RESET_FRAG_PTR`.  Those indices are outside this project's
kernel command whitelist, so that step needs a kernel change, and it has
deliberately not been made on the evidence so far.
