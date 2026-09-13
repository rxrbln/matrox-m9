# Matrox M9 native shader fragments

This note records the native microcode database recovered statically from the
saved Windows `MxMD.dll` and cross-checked against Linux `m9x_drv.so`.  It is a
companion to `parhelia-m9-3d-static-re.md`.  Addresses and lengths below are
binary evidence; semantic shader-operation names remain unknown.

Caller analysis identifies this table as the fixed-function vertex-program and
vertex-declaration conversion generator.  It is not a pixel-shader library.

## Proven representation

* A native instruction slot is 8 bytes/two little-endian dwords.
* One selector-`68`, ports-`60..67` upload line is 32 bytes/eight dwords and
  carries four native slots.
* Linker `0x13bf80` pads the final stream to 32 bytes, so its last one to three
  slots can be padding.
* The `0xc00` bank boundary is 3,072 bytes: 384 native slots or 96 upload lines.
* All fixed-fragment lengths are divisible by eight.  The standalone fragment
  at VA `0x1ecdd8` is exactly one instruction:

```text
{ 0xfffc0fff, 0x8bffffc0 }
```

Windows function `0x1454a0` builds 57 vector slots, numbered `0x00..0x38`, at
stride `0x10`.  Helper `0xadf00` appends `{read_only_data_pointer, byte_length}`
descriptors.  Function `0x120910` flattens the entire `+0x000..+0x380` table.
Linux 1.15 exports the equivalent initializer as `m9x30489` at `0x4462d0`.

The Windows fixed data covers 48 unique contiguous blobs at
`0x1e91f8..0x1ef780`, totaling `0x6588` bytes or 3,249 instruction slots.  The
SHA-256 of that contiguous block is
`0191f28356ce9d99de6c859ecb54e54ac66e9b8dd2a5823aff75f757b4095883`.

## Slot manifest

Entries are `Windows_VA/byte_length`, with hexadecimal lengths.  `A` and `B`
mark mutually exclusive prefixes; `optional` preserves branches whose condition
has not been named safely.

```text
00: 1e91f8/a8 1e9358/240 1eabf0/128 1eca48/b8
01: 1ecf20/58 1ecd58/80 1ecdd8/8 1e91f8/a8 1e9358/240 1eabf0/128 1eca48/b8
02: 1ece08/d8 1ecbb8/100 1e91f8/a8 1e9358/240 1eabf0/128 1eccb8/a0 1eca48/b8
03: 1ece08/d8 1ecbb8/100 1ecf20/58 1ecd58/80 1ecdd8/8 1e91f8/a8 1e9358/240 1eabf0/128 1eccb8/a0 1eca48/b8
04: 1e91f8/a8 1e9598/440 1eabf0/128 1eca48/b8
05: 1e91f8/a8 1ea3b0/568 1eabf0/128 1eca48/b8
06: 1ecf20/58 1ecd58/80 1ecdd8/8 1e91f8/a8 1ea3b0/568 1eabf0/128 1eca48/b8
07: 1ece08/d8 1ecbb8/100 1e91f8/a8 1ea3b0/568 1eabf0/128 1eccb8/a0 1eca48/b8
08: 1ece08/d8 1ecbb8/100 1ecf20/58 1ecd58/80 1ecdd8/8 1e91f8/a8 1ea3b0/568 1eabf0/128 1eccb8/a0 1eca48/b8
09: 1e91f8/a8 1e9358/240 1ead18/208 1ecb00/b8
0a: 1ece08/d8 1ecbb8/100 1e91f8/a8 1e9358/240 1ead18/208 1eccb8/a0 1ecb00/b8
0b: A=(1e92a0/b8 1e99d8/458) B=(1e91f8/a8 1e9598/440), then 1ead18/208 1ecb00/b8
0c: 1e91f8/a8 1e92a0/b8 1e9e30/580 1ead18/208 1ecb00/b8
0d: 1ece08/d8 1ecbb8/100 1e91f8/a8 1e92a0/b8 1e9e30/580 1ead18/208 1eccb8/a0 1ecb00/b8
0e: 1e91f8/a8 1e9358/240 1eaf20/310 1ecb00/b8
0f: 1ecf20/58 1e91f8/a8 1e9358/240 1eaf20/310 1ecb00/b8
10: 1ece08/d8 1ecbb8/100 1e91f8/a8 1e9358/240 1eaf20/310 1eccb8/a0 1ecb00/b8
11: 1ece08/d8 1ecbb8/100 1ecf20/58 1e91f8/a8 1e9358/240 1eaf20/310 1eccb8/a0 1ecb00/b8
12: A=(1e92a0/b8 1e99d8/458) B=(1e91f8/a8 1e9598/440), then 1eaf20/310 1ecb00/b8
13: 1e91f8/a8 1e92a0/b8 1e9e30/580 1eaf20/310 1ecb00/b8
14: 1ecf20/58 1e91f8/a8 1e92a0/b8 1e9e30/580 1eaf20/310 1ecb00/b8
15: 1ece08/d8 1ecbb8/100 1e91f8/a8 1e92a0/b8 1e9e30/580 1eaf20/310 1eccb8/a0 1ecb00/b8
16: 1ece08/d8 1ecbb8/100 1ecf20/58 1e91f8/a8 1e92a0/b8 1e9e30/580 1eaf20/310 1eccb8/a0 1ecb00/b8
17: 1e91f8/a8 1e9358/240 1eb230/340 1ecb00/b8
18: 1ecf20/58 1e91f8/a8 1e9358/240 1eb230/340 1ecb00/b8
19: A=(1e92a0/b8 1e99d8/458) B=(1e91f8/a8 1e9598/440), then 1eb230/340 1ecb00/b8
1a: 1e91f8/a8 1e92a0/b8 1e9e30/580 1eb230/340 1ecb00/b8
1b: 1ecf20/58 1e91f8/a8 1e92a0/b8 1e9e30/580 1eb230/340 1ecb00/b8
1c: 1eb570/78
1d: if owner state +0xc == 0: 1ed400/9c0 1e91f8/a8 1ea918/2d8 1ebb08/118 1eca48/b8; otherwise 1eb570/78 plus dynamic descriptor
1e: 1e91f8/a8 1e9358/240 1eb5e8/270 1eca48/b8
1f: optional 1ed3e0/20 1ed400/9c0; branch body uses 1e91f8/a8 1e9598/440 and 1ebc20/248 or 1eb5e8/270; suffix 1eca48/b8
20: 1e91f8/a8 1ea3b0/568 1eb5e8/270 1eca48/b8
21: 1e91f8/a8 1e9358/240 1eb858/2b0 1eca48/b8
22: optional 1ed3e0/20 1ed400/9c0; branch body uses 1e91f8/a8 1e9598/440 and 1ebe68/290 or 1eb858/2b0; suffix 1eca48/b8
23: 1e91f8/a8 1ea3b0/568 1eb858/2b0 1eca48/b8
24: 1ece08/d8 1ecee0/20
25: 1ecf78/58 1ece08/d8 1ecee0/20
26: 1ecd58/80 1ecde0/28
27: 1ecf78/58 1ecd58/80 1ecde0/28
28: 1ecf00/20
29: 1ec0f8/78 1ec170/1b8 1ec990/b8 1eca48/b8
2a: 1ecfd0/60 1ec0f8/78 1ec170/1b8 1ec990/b8 1eca48/b8
2b: 1ec0f8/78 1ec170/1b8 1ec328/280 1ec990/b8 1eca48/b8
2c: 1ec0f8/78 1ec170/1b8 1ec5a8/3e8 1ec990/b8 1eca48/b8
2d: 1ecfd0/60 1ec0f8/78 1ec170/1b8 1ec5a8/3e8 1ec990/b8 1eca48/b8
2e: 1ed030/3b0 1e91f8/a8 1ea918/2d8 1ebb08/118 1eca48/b8
2f: 1eddc0/630
30: 1eed60/40 1eddc0/630
31: 1e92a0/b8 1e9358/240 1ee3f0/8e0 1ecb00/b8
32: 1e92a0/b8; conditional 1e99d8/458; then 1ee3f0/8e0 1ecb00/b8
33: 1e92a0/b8 1e9e30/580 1ee3f0/8e0 1ecb00/b8
34: 1e92a0/b8 [optional 1eecd0/58] 1e9e30/580 [optional 1eed28/38] 1ee3f0/8e0 1ecb00/b8
35: 1eeda0/38
36: 1eedd8/968
37: 1ef740/20
38: 1ef760/20
```

For slot `0x0b`, branch A is selected exactly by:

```c
owner = *(obj + 0x414);
state = *(owner + 8);
use_a = (*(state + 0x0c) != 0) && (*(state + 0x40) == 0);
```

This branch is at Windows `0x145db1..0x145e8b`.  Slots `0x12` and `0x19`
strongly share that A/B prefix construction.  Conditions for `0x1f`, `0x22`,
`0x32`, and `0x34` are only partially decoded.

## Fixed-function vertex selector

The selector and two-fragment linker are independently present in both vendor
drivers [BIN]:

| role | Windows `MxMD.dll` | Linux `m9x_drv.so` |
|---|---:|---:|
| main selector | `0x13b2b0`, wrapper `0x13b260` | `m9x1091` `0x44a9f0`, wrapper `m9x11255` `0x44add0` |
| companion selector | `0x13b570` | `m9x27688` `0x44ae10` |
| two-fragment linker | `0x13bf80` | corresponding linked-program path |

The Windows pipeline calls main selector at `0x13c3a7`, companion selector at
`0x13c3ec`, and linker at `0x13c503`.  The Linux order is `m9x11255` at
`0x44c5ae`, then `m9x27688` at `0x44c5e3`.

The encoded declaration uses `t = D & 0x0f` and `m = D & 0x30`.  Its low-nibble
families match `D3DDECLTYPE` exactly [DER]:

| declaration types | enum values | selector family |
|---|---|---|
| `UBYTE4,SHORT2,SHORT4,UBYTE4N,SHORT2N,SHORT4N,USHORT4N,UDEC3` | `5,6,7,8,9,10,12,13` | packed-input conversion |
| `FLOAT3,FLOAT4,D3DCOLOR,USHORT2N,DEC3N` | `2,3,4,11,14` | float/color/special conversion |
| `FLOAT1,FLOAT2,FLOAT16_2`, default | `0,1,15` | generic conversion |

`FLOAT16_4` needs higher format bits because its enum value 16 aliases low
nibble zero.  Helper `0x13c8d0` recognizes packed types
`5,6,7,9,10,12,13`, while type 8 is forced into the same family.

The main selector records `{P,S,M}` and the companion optionally replaces `S`
with `Q`.  Linker `0x13bf80` concatenates `P + (Q when active, otherwise S)`.
Roles overlap between slots:

| role | slot IDs, hexadecimal |
|---|---|
| `P`, main conversion/fixed-function body | `00,01,04-06,09-0f,12-14,17-24,29-2f,31-35` |
| `Q`, conditional companion/continuation | `02,03,07,08,0a,0b,0d,0e,10,11,15,16,2a,2d,2f,30,35,36` |
| `S`, default/alternate continuation | `24-29` |
| `M`, class marker | `37,38` |

The main selector writes `cumulative_offset[P] >> 3` and
`cumulative_offset[S] >> 3` to Windows pass fields `+0xa8/+0xaa` or Linux
`+0xb0/+0xb2`.  The companion writes `cumulative_offset[Q] >> 3` at Windows
`+0xbc` or Linux `+0xd0`.  The trace tuple has
Windows stride `0x10` and Linux stride `0x18`; the cumulative-offset table is
at Windows library `+0x418` or Linux `+0x7b0`.  Pass strides are `0xc04` and
`0xc34`, respectively.

Let `Fn` denote bit `n` of selector flags `F`.  The exact base `P` selection is:

| predicate | base `P` |
|---|---:|
| `F7 && !F4` | `31` |
| `F7 && F4 && !F8` | `32` |
| `F7 && F4 && F8 && (m==0x10 || (D&0x6000)==0x2000)` | `34` |
| remaining `F7` | `33` |
| packed, `!F3`, `!F4 && !F9` | `00` |
| packed, `!F3`, later zero-`F8/F9` case | `04` |
| remaining packed `!F3` | `05` |
| packed, `F3,!F6,m==0x20,!F5,!F4,!F9` | `09` |
| same feature branch, later zero-`F8/F9` case | `0b` |
| remaining same branch | `0c` |
| packed, `F3,!F6,m==0x20,F5,!F4,!F9` | `0e` |
| same feature branch, later zero-`F8/F9` case | `12` |
| remaining same branch | `13` |
| packed, `F3,!F6,m!=0x20,!F5` | `1c + F4` |
| packed, `F3,!F6,m!=0x20,F5,!F4` | `1e` |
| packed, `F3,!F6,m!=0x20,F5,F4` | `1f + F8` |
| packed, `F3,F6,m==0x20,!F4,!F9` | `17` |
| same feature branch, later zero-`F8/F9` case | `19` |
| remaining same branch | `1a` |
| packed, `F3,F6,m!=0x20,!F4` | `21` |
| packed, `F3,F6,m!=0x20,F4` | `22 + F8` |
| float/color, `!F4 && !F9` | `29` |
| float/color, later zero-`F8/F9` case | `2b` |
| remaining float/color | `2c` |
| generic declaration fallback | `2e` |

Flag `F10` normally increments both `P` and `S`.  The base continuation is
`S=24` for `F6`, `S=28` for `!F6 && mode==1 && !global_inhibit`, and `S=26`
otherwise.  Windows uniquely rejects special slot `28` with `F10`, falls back
to `26`, then increments to `27`; Linux preserves the special choice and uses
`29`.  This is a real cross-version behavior difference [BIN].

The companion override is:

| predicate | `Q` |
|---|---:|
| `F7`, special format condition | `30` |
| other `F7` | `2f` |
| `!F7,F11,!F3,!F4,!F9` | `02 + F10` |
| `!F7,F11,!F3`, remaining | `07 + F10` |
| `!F7,F11,F3,!F5,!F4,!F9` | `0a + F10` |
| same branch, remaining | `0d + F10` |
| `!F7,F11,F3,F5,!F4,!F9` | `10 + F10` |
| same branch, remaining | `15 + F10` |
| `!F7,!F11,F10,t in {FLOAT3,FLOAT4},!F9` | `2a` |
| same with `F9` | `2d` |
| pass zero, derived fallback A/B | `35` / `36` |
| otherwise | retain `S` |

For the `FLOAT3/FLOAT4` choice between `2a` and `2d`, Windows tests cached `F9`
at `0x13b6b9..0x13b6c9`, while Linux tests a nested global-state bit at
`0x44b030..0x44b042`.  They may be synchronized earlier, but the physical
sources differ and an open implementation should derive this choice from its
own explicit state rather than imitate either object layout.

Marker `M` is `38` for `F7` or for packed input with
`F3 && (F5 || F6)`; otherwise it is `37`.  No direct lookup of `M` through the
cumulative-offset table was found, so its hardware use remains unknown.

Proven flag construction includes `F2=packed_numeric`,
`F7=F2&&(D&0x1000)`, and `F3=F2&&((D&0x30)!=0x30)`.  `F4` reflects nonzero
context-derived state mask `&0x7e`; `F8` is helper `0x12bed0(0,1)`; `F9`
mirrors context `+0x2504` bit 4 and accompanies installation of a 4x4 identity
matrix; `F10` mirrors the next context bit; and `F11` accompanies allocation of
floating-point constants.  Their final D3D semantic names remain unresolved.

A prior sign-test interpretation as `F bit31` is wrong.  Windows and Linux both
test the sign of the low byte, proving `F bit7`.

## Dynamically built fragment

Windows `0x147d30` and Linux `m9x25489` at `0x445ea0` allocate four slots; the
resource copy path at Windows `0x120a30` uses `count * 8`.  The exact program is:

```text
{ 0xffc0291d, 0xe29fff1f }
{ 0xffc1291e, 0xa29fff1f }
{ 0x00000000, 0x00000000 }
{ 0x00000000, 0x00000000 }
```

The builder independently manipulates dword 0 low values `0x1d/0x1e`, dword 0
bit 16, dword 0 bits `22:15`, and dword 1 fields selected by masks
`0xfe9fffff`, `0xf7ffff7f`, and `0xfbffffbf`, plus bits `0x800`, `0x400`,
`0x20`, and `0x10`.  Those are proven physical fields, but their opcode,
operand, modifier, or flow-control meanings remain unassigned.

The next static/runtime hooks should log `(slot, descriptor pointer, length)` in
`0xadf00` while `0x1454a0` runs, then dump the unpadded linked stream immediately
before `0x13bf80` rounds it to 32 bytes.  Comparing those 64-bit pairs with
controlled Iris source variants will expose compiler-to-native deltas before
MMIO capture is needed.

`tools/m9-shader-diff.py` automates that comparison.  It treats raw files as
two-dword slots, reports the containing four-slot upload line and lane, and
prints XOR masks plus changed bit ranges for each differing instruction.
