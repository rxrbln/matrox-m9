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

The independently normalized float3 triangle binds `00=40f52101`,
`10=18`, and H03 `[0,4)`.  Removing the low address tag gives the exact
accessed byte interval:

```text
arena + 0x100 .. arena + 0x160     (four records * 0x18 bytes)
```

The original float2 `gltri` draw binds `00=40f52001`, `10=14`, H03 `[0,4)`,
so its interval is `arena + 0 .. arena + 0x50`.  Index `10` is a per-element
record span, not the total source byte count.

The allocation metadata closes BO sizing and bounds validation.  It does not
close the payload contents: the tracing wrapper saved command and system-map
images but did not snapshot this CPU-filled arena.  The application vertices
alone are insufficient because the vendor repacks current color and position
into its canonical records.  A clean-room first-triangle fixture still needs
either one source-arena snapshot or a statically complete canonical repacker;
substituting the current Mesa `struct m9_vertex` remains unsupported.

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

## BE auxiliary surface resource

Linux surface emitter `m9x12791` tests dirty bit `0x800`, emits header `be`,
passes the resource pair at surface shadow `+0xd0`, and invokes its relocation
callback with usage `8` (`0x4b08de..0x4b0939`).  This proves raster `be` is a
relocated auxiliary member of the bound surface state.  Preprocess `be=3` is
the literal alternate form.

The saved allocation records are client allocations and do not contain the
server-owned raster `be=112c1000` object.  Its exact bytes and allocation
extent therefore remain absent.  Keep it in a separate read/write BO for the
bounded diagnostic; do not alias it with C2 or infer a size from its address.

## AB, AC, and AE are packed surface state

Linux `m9x12791` at `0x4b0b77..0x4b0bc9` copies surface-shadow dwords
`+0x20,+0x24,+0x30` directly to indices `ab,ac,ae` when dirty bit `0x200` is
set.  These are ordinary indexed values and have no relocation callback.

Windows `MxMD.dll` closes their producer shape.  At
`0x1824d3..0x182548`, it walks a device table of records whose low 16 bits
index a per-surface halfword table at object `+0x1a0`; bits 16..21 choose one
of four local halfword positions.  It packs those four derived halfwords into
surface-shadow `+0x20..+0x26`, hence AB/AC.  At `0x1822e1..0x18230a`, AE is
copied from object `+0x1b8`, optionally with bit 28 set for one device mode.

This explains why AB/AC changes with the process/surface while AE commonly
remains `22444442`.  AB/AC are compiler-derived packed surface fields, not GPU
addresses.  A new driver must derive them from its surface layout table; it
must not relocate or blindly reuse the sequence-12 values.  The device-table
field meanings and a clean-room formula that produces arbitrary AB/AC values
remain open.

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

## Linked-record pointer relocation map

The GLSL A/B/A fixture saves the system allocation at `40c3a000`, allocation
extent `0x20080`, and `0x21000`-byte snapshots.  Linked records A and B begin
at offsets `0x300` and `0x880`, giving adjacent `0x580`-byte slots.  The
verifier scans every aligned dword in both slots of sequences 15, 19, and 23.
After clearing the documented low tag bits, no value points into either:

```text
captured local aperture       10000000..1fffffff
captured system allocation    40c3a000..40c5a07f
```

Thus the captured baseline A/B record slots have an **empty embedded GPU-BO
pointer relocation map**.  Values such as `40000000` are ordinary IEEE
constants and lie outside the captured system allocation.  The external
`6c=system_base+record_offset|1` selector is the only BO relocation needed to
address one of these opaque saved slots.

This closes the previously suspected embedded-BO-pointer gap for the captured
slots.  It does not prove that an opaque record can be copied across cold
contexts: native control targets, generation values, and compiler-internal
references may still be context dependent even though they are not BO
addresses.  General programmable rendering still needs the linked-pair
serializer.  A fixed saved-record diagnostic may proceed only after its
complete context freshness contract is established.

## Updated first-triangle boundary

Closed from saved evidence:

* exact 80-slot bytes, selector/port grammar, and equality across four uploads;
* canonical vertex arena extent and exact source windows for float2 and float3;
* C2 backing extent and its paired static resource usages;
* BE ownership as a relocated auxiliary surface resource;
* AB/AC/AE ownership and packed-halfword producer shape;
* exact transient address lattice, join, and conservative guarded probe size;
* empty embedded GPU-BO relocation map for the captured linked A/B slots.

Still required before the first validated triangle executes:

* exact canonical source record bytes or a fully closed repacker;
* exact BE contents/extent and the initial C2 contents;
* between-pass transient content semantics, if the conservative `0x16800`
  guard test fails;
* a clean-room AB/AC derivation for the chosen render-target layout;
* the cold-context freshness/transition contract for an opaque linked record.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
