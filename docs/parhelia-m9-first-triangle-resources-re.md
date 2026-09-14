# M9 first-triangle context and resource closure

This note reconstructs the saved resource prerequisites around the already
decoded two-pass M9148 triangle.  It is limited to offline captures and the
saved Linux and Windows vendor binaries.  No packet in this work was submitted
to hardware.  Reproduce every numeric claim with:

```sh
python3 tools/m9-first-triangle-resources-evidence.py --self-test
```

The verifier pins the complete input files and the instruction-aligned binary
slices used below.  The raw fixed-function archive has SHA-256
`49859ba73319efd6d967e2a873aa6508b1de7eccfeb026508cd10bf35317d2c2`.

## The 80-slot context image

Startup sequences 0, 8, 10, and 14 in the settled `gltri` capture contain the
same 80 two-dword native slots.  The payload is exactly `0x280` bytes and has
SHA-256:

```text
78434eea53365d762f7eda163d5981ab765d754d1bed9e5377ed231e30bc34fb
```

Every slot is emitted in its own three-write indexed packet.  For slot `i`:

```text
8f = 80000000 + (i / 8) * 40
data ports = 90 + 2*(i % 8), 91 + 2*(i % 8)
```

The 80 `{selector,port,lo,hi}` records have SHA-256
`4fa0c712c862b15ad0f5195d7926f3de00600fef0c834ba96ce3372d58ac1593`.
The four byte-identical uploads prove that this is invariant context state,
not an application shader selected by `6c`.  Sequence 0 also records the full
surrounding transition and ordinary-state envelope.  The exact image and its
packet grammar are therefore closed; the smallest subset of the surrounding
envelope that is safe after a cold reset remains a hardware-isolation
question.  A first kernel-owned diagnostic should reproduce the complete
sequence-0 setup envelope instead of uploading the `0x280` bytes in isolation.

## Canonical vertex allocation and accessed bytes

Allocation result 40 in the preserved fixed-function archive describes the
canonical vertex arena:

| field | value |
| --- | ---: |
| GPU base at allocation result `+0x208` | `40f52000` |
| byte extent at `+0x218` | `00192700` |
| dimensions at `+0x198/+0x19c` | `3fc0 x 65` |

The fixed-function lighting triangle binds `00=40f52101`, `10=18`, and H03
`[0,4)`.  Removing the low address tag gives the exact accessed byte interval:

```text
arena + 0x100 .. arena + 0x160     (four records * 0x18 bytes)
```

The original float2 `gltri` draw binds `00=40f52001`, `10=14`, H03 `[0,4)`,
so its interval is `arena + 0 .. arena + 0x50`.  Index `10` is a per-element
record span, not the total source byte count.

For the original flat green `gltri`, the declaration under repeated index `23`
is exactly:

```text
00100dac 00100236 02200d88 02200ab6 00004b08 0 0 0
```

The same `0x14` span/declaration exists in the controlled M9148 topology
corpus, whose `SHA256SUMS` manifest hashes to
`fc30144d2f472b6f07213908daf4d7fc7de62a7927fb54060c4b257c47acb48d`.
Two independent M0528 CPU-ring captures expose the corresponding payload:
flat hashes to `b3f3086104deec1ec9aa1c3819109eb8abbd20e2376542a5706d4a75ff5cba0b`
and Gouraud to
`7bfcfd077ba4d8eae30821df6155d0f73420ba794efe45dd4fcdb2887b0f8fcd`.
Their source records are exactly five little-endian
float32 values in `{x,y,r,g,b}` order.  Current color is copied into every
record; it is not carried only in state.  The default-green bounded source is:

```text
20,20,0,1,0   300,20,0,1,0   160,220,0,1,0
```

M9148 preprocessing reads four records although raster launches `[0,3)`.  A
clean-room producer must initialize that entire read window.  Duplicating the
third record into record four gives this exact 80-byte diagnostic image:

```text
SHA-256 58bd76a39629399e0da5241acea797223c6b68c8eaf83dec7161a6e095845f3a
```

The fourth record is deterministic safe padding, not a claim about bytes left
there by the vendor allocator.  It cannot affect the three-element raster
launch.  This closes source construction for the flat first triangle and gives
the clean-room rule for the captured float2/current-RGB layout.  The other
captured canonical layouts remain usable through their documented repackers;
arbitrary direct VBO layouts remain outside this diagnostic.

## C2/C3 common resource

The same float3 draw uses `c2=11f61000,c3=4` in preprocessing and
`c2=11f61000,c3=0` in raster.  Allocation result 52 identifies that backing:

```text
GPU base    11f61000
extent      00001080 bytes
dimensions  00000400 x 1
```

Linux emitter `m9x29970` at `0x4ae4ed..0x4ae55b` emits header `c2,c3`, takes
the resource pair at state `+0x148`, and registers two views through the same
relocation callback:

```text
packet slot 2, usage 13
packet slot 3, usage 14
```

Its null-resource branch at `0x4ae62f..0x4ae68c` emits the adjacent `e6/e3`
transition and literal `c2=4,c3=0` form.  Consequently `c2/c3` are a common
resource-bank transition with two resource usages; index identity alone does
not say whether a given value is an address or literal.  The narrow hardware
read/write direction of usages `0x13/0x14` is still unavailable, so retain the
0x1080-byte object for the complete job and declare it read/write in the first
diagnostic.

The raw archive also settles when initialization can occur.  Allocation 52 is
followed by submits 53 and 54, each exactly five writes:

```text
1b=05927002  e6=3  e6=2  e3=<fence|1>  43=<fence>
```

The real two-pass draw is submit 55.  There is therefore no GPU command that
initializes C2 between allocation and its first preprocessing use.  Any vendor
initial bytes come from a CPU aperture store or allocator policy, neither of
which the wrapper saved.  A guarded diagnostic should zero all `0x1080` bytes
and treat C2 as a job-local read/write object.  This is a deterministic probe
policy, not proof that zero matches the missing vendor image; exact initial C2
bytes remain an evidence gap if the probe does not complete correctly.

## BE auxiliary surface resource

Linux surface emitter `m9x12791` tests dirty bit `0x800`, emits header `be`,
passes the resource pair at surface shadow `+0xd0`, and invokes its relocation
callback with usage `8` (`0x4b08de..0x4b0939`).  This proves raster `be` is a
relocated auxiliary member of the bound surface state.  Preprocess `be=3` is
the literal alternate form.

The saved allocation records are client allocations and do not contain the
server-owned raster `be=112c1000` object, so its exact allocation extent remains
absent.  The M0528 MMIO trace with SHA-256
`8fa1b88deda8cedd8a747c17669e8ad6ace7c408d754a0687e4165321b9aabd6`
adds one useful cross-family bound.  A
single `rep stosq` run zeros every aperture qword from `d1120000` through
`d112fff8`.  Local command addresses use 16-byte units, placing
`be=112c1000` at aperture byte `d112c100`; at least `0x3f00` physical bytes at
and after BE are therefore zero initialized in that vendor stack.  This does
not prove an M9148 allocation size.  A bounded M9148 diagnostic may allocate a
separate zeroed read/write BO of at least `0x3f00` bytes and guard its end; any
guard access means the saved evidence did not establish a sufficient extent.

## AB, AC, and AE are packed surface state

Linux `m9x12791` at `0x4b0b77..0x4b0bc9` copies surface-shadow dwords
`+0x20,+0x24,+0x30` directly to indices `ab,ac,ae` when dirty bit `0x200` is
set.  These are ordinary indexed values and have no relocation callback.

Windows `MxMD.dll` closes their producer.  The complete relevant function
prefix is VA `0x181fa0..0x182455`; the AB/AC block is
`0x1824b6..0x182549`.  Let each four-byte device descriptor be `d`.  The loop
performs:

```text
source = d & 0xffff
if source != 0xfffe:
    slot = ((((d >> 16) & 0x3f) - 0x10) >> 2)
    halfword[slot] = surface_halfword_table[source]  # object +0x1a0
AB = halfword[1] << 16 | halfword[0]
AC = halfword[3] << 16 | halfword[2]
```

The prologue allocates `0x28` stack bytes but never initializes the four local
halfwords before the descriptor-directed stores.  This is visible in the
captures: all twenty independently executed topology cases have
`AB[15:0]=ffff`, while all twenty upper 48-bit values differ.  Those inactive
slots are vendor stack residue, not surface layout.  For the bounded one-RT
triangle, initialize all four halfwords to `ffff` and overwrite every active
descriptor destination using the formula above.  The resulting deterministic
single-RT value is `AB=ffffffff,AC=ffffffff`, which the vendor already emits in
the preprocessing pass.  Arbitrary MRT still requires construction of each
active entry in the per-surface halfword table.

At `0x1822e1..0x18230a`, AE is copied from object `+0x1b8`, optionally with bit
28 set for one device mode.  AB/AC/AE remain ordinary state and must never be
relocated.

## Four preprocessing transients

Both the original float2 triangle and the independent float3 fixed-function
triangle use the same bases:

```text
113c1000  113d7800  113ee000  11404800
```

All three adjacent differences are exactly `0x16800`.  Preprocessing writes
them through `[b0,b2,b4,b8]`; raster reads them in the exact permutation
`[00,03,01,02]`, equivalent to the canonical join `[b0,b4,b8,b2] ->
[00,01,02,03]`.  Their descriptors are also invariant in the saved ordinary
cases.

The capture proves the address lattice and the full-job read/write lifetime.
It does not prove that each base is a distinct allocation rather than a
suballocation of one server arena, and no between-pass contents snapshot was
saved.  For a guarded diagnostic, allocate four independent objects with at
least `0x16800` writable bytes each, preserve their alignment requirements,
and place an inaccessible or sentinel guard after that conservative writable
extent.  This reproduces at least the spacing available to the first three
vendor views; it is a diagnostic allocation policy, not a claim that
`0x16800` is the hardware's minimum or exact logical stream size.  Treat any
guard access as a failed bound and stop.

## Linked-record cold-context rule

The GLSL A/B/A fixture saves the system allocation at `40c3a000`, allocation
extent `0x20080`, and `0x21000`-byte snapshots.  Linked records A and B begin
at offsets `0x300` and `0x880`, giving adjacent `0x580`-byte slots.  The
verifier scans every aligned dword in both slots of sequences 15, 19, and 23.
After clearing the documented low tag bits, no value points into either:

```text
captured local aperture       10000000..1fffffff
captured system allocation    40c3a000..40c5a07f
```

Thus the captured programmable A/B slots have an empty embedded GPU-BO pointer
relocation map.  This result does **not** permit copying arbitrary opaque slots.

The fixed-function draw selects `6c=40c3b601`, hence the `0x580`-byte slot at
system offset `0x1600`.  Its SHA-256 is
`356476b633ed3efcf8f23f9ef39bb9cbf30c2654a47a79ec9111b5b5b2081335`.
It embeds nine 64-bit process addresses at slot offsets `80,98,c8,f8,198,1b0,
1c8,1e0,1f8`; their values are `00002b9481be8970` or
`00002b948257fed8`, matching the capture process' `0x2b94...` address space.
These are not GPU BO pointers, but they are necessarily stale after a cold
process/context transition.

Therefore raw fixed-slot replay is rejected.  A cold diagnostic must bind only
a newly serialized record owned by the current context.  The statically
documented selector-`68` fixed-chain path is a possible replacement after its
selection relation to this captured two-pass draw is validated; it cannot yet
be substituted merely because its image format is known.  A verifier that
checks only GPU apertures is insufficient.  General programmable rendering
still needs the linked-pair byte serializer.

## Updated first-triangle boundary

Closed from saved evidence:

* exact 80-slot bytes, selector/port grammar, and equality across four uploads;
* canonical vertex arena extent, exact source windows, and an exact 80-byte
  flat-green source/repacker contract;
* C2 backing extent, paired static usages, and absence of a GPU initializer;
* BE ownership plus a cross-family zeroed lower bound of `0x3f00` bytes;
* exact AB/AC descriptor formula and deterministic one-RT initialization;
* exact transient address lattice, join, and conservative guarded probe size;
* rejection of raw fixed linked-record replay due to embedded host addresses.

Still required before the first validated triangle executes:

* exact M9148 BE allocation extent; the diagnostic must retain its end guard;
* direct proof of M9148 BE initialization if the M0528-derived zero policy is
  insufficient;
* exact initial C2 bytes if deterministic zero initialization does not work;
* between-pass transient content semantics, if the conservative `0x16800`
  guard test fails;
* a current-context linked-pair serializer, or validation that the inline
  fixed-chain path selects the required two-pass program.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
