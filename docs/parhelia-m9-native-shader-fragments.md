# Matrox M9 native shader fragments

This note records the native microcode database recovered statically from the
saved Windows `MxMD.dll` and cross-checked against Linux `m9x_drv.so`.  It is a
companion to `parhelia-m9-3d-static-re.md`.  Addresses and lengths below are
binary evidence.  The backend encoder fixes several operation and operand-field
boundaries.  Compiler lowering also fixes operand direction, result-component
order, and several source-operation anchors, although most native operation
mnemonics remain unknown.

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

## Native instruction encoder

The D3D compiler supplies the missing join between its optimizer and native
code.  Optimizer `0x1958a0` calls backend `0x1d1d40` at `0x195b88`; the latter
eventually presents 0x44-byte instruction objects to a native emitter.  The
instruction constructor is `0x1da2c0`, the whole-program emitter is
`0x1db510`, and the exact one-slot packer is `0x1db840..0x1db9bb` [BIN].  The
packer is the third method in the vtable at `0x1f50bc` and appends one pair of
dwords to an output array limited to `0x100` slots.

The relevant intermediate instruction members are [BIN]:

| member | initial value | proven use |
|---:|---:|---|
| `+0x08` | `0` | base for native `W1`; operation and modifier bits |
| `+0x0c` | `0`, 16-bit | base for native `W0` low half |
| `+0x0e` | `0`, 16-bit | presence/format gates used by the packer |
| `+0x10,+0x14,+0x18,+0x20,+0x24` | `0xf0000000` | explicit source/resource descriptors |
| `+0x1c` | `0xf0000000` | explicit result/destination descriptor |
| `+0x28,+0x2a` | `0`, 16-bit | alternate selectors for two physical fields |
| `+0x2c,+0x2e,+0x30,+0x32,+0x34` | low nibble cleared | per-descriptor component metadata used by analysis/rewrites |
| `+0x38,+0x3c` | `0xf0000000` | inferred/implicit descriptor overrides |

`0xf0000000` is the absent-descriptor sentinel.  Routine `0x1da4a0` walks the
five pairs `(+0x14,+0x2c)`, `(+0x18,+0x2e)`, `(+0x20,+0x30)`,
`(+0x10,+0x32)`, and `(+0x24,+0x34)` while constructing the compiler's
resource/dependency list.  It handles `+0x1c` separately at
`0x1da935..0x1da94f`, storing it in the result-side local rather than adding it
to that source collection.  Routine `0x1daf60` preserves the same separation
while renumbering descriptors.  The arithmetic lowerers provide an independent
data-flow check: the first RCP instruction receives the caller's source in
`+0x14` or the format-dependent alternate `+0x10` and a newly allocated
temporary in `+0x1c`; the final instruction reads intermediate temporaries
through `+0x14/+0x18/+0x20` and places the caller's result descriptor in
`+0x1c`.  This proves the direction shown in the table.  Exact register-class
names remain unresolved.

Let `replace(x, hi, lo, v)` replace inclusive bits `hi:lo` in `x` with the low
`hi-lo+1` bits of `v`.  Ignoring a separate program-property side effect at
`0x1db941..0x1db988`, the packer is exactly [DER]:

```c
W0 = 0xffff8000 | insn_u16[0x0c];
W1 = insn_u32[0x08] | 0x0000000f; /* bits 20:19 are preserved */
g = insn_u16[0x0e];

if (g & 0x20) {
        q = insn_u16[0x14] & 0x7f;
        W0 = replace(W0, 31, 29, q);
        W1 = replace(W1,  3,  0, q >> 3);
}
if (g & 0x40) W0 = replace(W0, 28, 22, insn_u32[0x18]);
if (g & 0x80) W0 = replace(W0, 21, 15, insn_u32[0x20]);
if (g & 0x01) W1 = replace(W1, 20, 14, bank7(insn_u32[0x10]));
if (g & 0x02) W0 = replace(W0, 21, 15, bank7(insn_u32[0x24]));
if (g & 0x04) W0 = replace(W0, 14,  8, insn_u32[0x1c]);
if (g & 0x08) W1 = replace(W1, 20, 14, insn_u16[0x28]);
if (g & 0x10) W0 = replace(W0, 21, 15, insn_u16[0x2a]);
```

Here `bank7(d)` returns `(d + 0x40) & 0x7f` when `d & 0xf0000000` is
`0xb0000000`, and `d & 0x7f` otherwise.  This is direct evidence that at least
two internal descriptor classes share the seven-bit native selector namespace.
The overlapping destinations in the table are real: format gates choose
whether a physical field holds an operation or one of several descriptor
forms.  They must not be implemented as simultaneous independent operands.

### Operation and component fields

Three fields now have compiler-side operation evidence [BIN][DER]:

| native field | encoder/decoder evidence | conclusion |
|---:|---|---|
| `W0[4:0]` | setter `0x1af390`; implicit-resource decoder `0x1db9d0`; explicit tests for `0x1c..0x1e` at `0x1da987` | operation selector with a proven result-component family |
| `W1[25:21]` | setter `0x1dbe50`; expanded-operation decoder `0x1dc220` | second operation selector |
| `W1[18:14]` | setter `0x1dc040`; expanded-operation decoder `0x1dc2d0` | format-dependent third operation selector, sharing `W1[20:14]` with a seven-bit descriptor |

The two `W1` operation setters accept an expanded compiler operation number,
store a five-bit base selector, and place extension choices in separate
modifier bits.  Thus values returned by `0x1dc220` or `0x1dc2d0` can exceed
31 even though the base operation field is five bits.  Native `W0[14:8]` is
also decoded through a sparse implicit-resource table at `0x1dbb40`, but the
packer can replace all seven bits with descriptor `+0x1c`; it remains a
format-dependent selector rather than a safely named fourth operation.

The compiler source operation in its 0x88-byte input record is distinct from
all three native operation fields.  Dispatch `0x1ca2d0` recognizes source
operation values `6`, `7`, `14`, and `15`; these are the D3D `RCP`, `RSQ`,
`EXP`, and `LOG` values and lead to four characteristic multi-instruction
lowerers [BIN][DER]:

| source operation | lowerer | native instruction objects | directly observed operation-setter inputs |
|---|---:|---:|---|
| `RCP` (`6`) | `0x1b0840` | 6 | second `0x11,0x13`; third `0x1a` |
| `RSQ` (`7`) | `0x1b1210` | 7 | second `0x14,0x111,0x113`; third `0x1a` |
| `EXP` (`14`) | `0x1b1ba0` | 6 | second `0x14,0x01`; third `0x14,0x02` |
| `LOG` (`15`) | `0x1b2550` | 9 | second `0x14,0x02`; third `0x14,0x01` |

These are semantic anchors for lowering sequences, not one-to-one native
mnemonics.  In particular, RSQ uses expanded setter values `0x111` and
`0x113`: they retain five-bit bases `0x11` and `0x13` while selecting extension
bits.  Destination masks and descriptor classes also select or omit stages, so
the constant call sites above are deliberately not presented as complete
sequences.

Helper `0x1afd30` establishes the result-component order for the simple
descriptor-class path.  It selects a two-bit pair from the source-level D3D
destination mask in bits `19:16`, using pair zero for `xy` and pair one for
`zw`, and returns a `W0[4:0]` value as follows [DER]:

| active components in selected pair | `W0[4:0]` |
|---|---:|
| neither | `0x00` |
| first (`x` or `z`) | `0x1d` |
| second (`y` or `w`) | `0x1e` |
| both | `0x1c` |

The generic compiler path independently maps descriptor component-class bits
`27:26` to `0x1d`, `0x1e`, and `0x1c` for first, second, and both.  These
selectors therefore express component write modes inside a result-operation
family; they are not safe standalone ALU mnemonic assignments.

The lowerers pass the original destination descriptor through helper
`0x1afee0`, shift its result left by five, and insert it in `W0[7:5]`.  This
proves that `W0[7:5]` is a result-class/routing selector.  Descriptor bit 20
forces class 4; otherwise descriptor bits `27:24` map
`0,1,2,3,4..12,13,14,15` to `0,1,2,3,7,4,5,6`.  Semantic names for those
classes remain unknown.

Two helpers encode independent four-bit component masks into scattered `W1`
bits.  For input bits `m[3:0]`, `0x1af3e0` writes them to
`W1[26,12,8,6]`, while `0x1af430` writes them to `W1[27,13,9,7]`, in
`m[3],m[2],m[1],m[0]` order [DER].  The whole-program emitter invokes both
with `m=0xa`.  Their association with the two `W1` operation lanes is strong;
observed lowerer call sites also pass `2,4,5,6,0xa,0xe` according to format.
No call site found passes the source D3D write mask directly.  The `xyzw`
result order above therefore does not assign component names to these scattered
mask bits; that still awaits a controlled compiler delta.

### Program-context expansion and stream splicing

Whole-program emitter `0x1db510` does more than pack one translated input
instruction.  Its second argument is the output stream and its third argument
is a program context.  After cloning and translating the input, it follows
these mutually exclusive paths [BIN][DER]:

```c
if (program_context_u32[0x60 / 4] != 0) {
        /* Replace the translated input with a complementary pair. */
        append(synthetic_op0_1d_op1_14_mask_a_b_0a);
        append(synthetic_op0_1e_op1_14_mask_a_b_0a);
        return success;
}
if (program_context_u32[0x64 / 4] != 0) {
        append(synthetic_op1_00);
        append(the_same_synthetic_op1_00);
        *(uint32_t *)(output + 0x397) |= 0x20000000;
}
append(translated_input);
```

The order of `0x1d` and `0x1e` in the replacement pair can swap according to
a transformed descriptor bit.  Descriptor class bits are also adjusted by a
format property.  The safe conclusion is a format-dependent result-pair
expansion, not an export mnemonic assignment.  The `+0x64` pair is likewise a
compiler-generated control sequence whose public meaning is not yet known.

The one-slot packer has an independent side effect on the same output property
bit.  Before format gates can replace the overlapping physical field, it tests
the instruction object's internal `W1` high nibble for `8` or `9`, requires
expanded second operation `0`, and requires expanded third operation in
`{0x81,0x82,0x83,0x182,0x183}`.  Only that combination sets output
`+0x397` bit `0x20000000` [BIN].  A raw packed slot cannot always reconstruct
this test because the packer may replace `W1[20:14]` with a descriptor.

A derived backend at vtable `0x1f5104` adds another context-sensitive path.
Emitter `0x1dcaf0` records generated-run boundaries in output bytes
`+0x39b/+0x39c` (using `+0x39d` temporarily).  Stream copier `0x14e900`
recognizes such a run, requires a boundary slot with `W0[14:8]=0x3d`, and
patches the last slot from the first slot: first `W0[14:8]` is copied into last
`W0[21:15]`, and last `W1[5]` records whether first `W0[4:0]` is `0x1e`.
For one following `W0[14:8]=0x21`, `W0[4:0]=0x1d` form, it replaces the same
field in the rest of the special run with `0x7f` [BIN][DER].  Native slot
meaning can therefore depend on compiler-run boundaries and neighboring
slots.  Fixed fragments must not be decoded as isolated source operations.

The low-operation implicit-descriptor table at `0x1dba44` contains these 32
case indices [BIN]:

```text
00 01 02 03 00 01 02 03 00 01 02 03 04 05 04 05
00 01 02 03 00 01 02 03 00 01 02 03 06 06 06 07
```

Cases `0..6` return, respectively, `0x64000000`, `0x68000000`,
`0x64000001`, `0x68000001`, `0x6c000000`, `0x6c000001`, and
`0xdc000000`; case 7 returns the absent sentinel.  A second table at
`0x1dbb14`, indexed by the same `W0[4:0]` selector, maps to a four-bit
operation-derived component form:

```text
0c 04 0c 04 08 0f 08 0f 07 0b 07 0b 02 02 02 02
0c 04 0c 04 08 0f 08 0f 07 0b 07 0b 0e 0e 0e 02
```

The sparse `W0[14:8]` table at `0x1dbbd0` returns a descriptor only for
selectors `1,2,3,4,8,12,16,32,48`, mapping respectively to
`0x64000000`, `0x68000000`, `0x6c000000`, `0x64000001`,
`0x68000001`, `0x6c000001`, `0xd4000000`, `0xd8000000`, and
`0xdc000000`.  All other values return the absent sentinel [BIN].  These
constants expose register/resource classes but do not supply safe class names.

No native disassembler string table was found.  In particular, the readable
`mov`, `mad`, `rcp`, and related switch at `MxMGLX.dll` `0x106ae3b0` decodes
the 16-bit Iris token opcode described in the companion document.  The Linux
ICD has the corresponding formatter strings near `0xecbd6b`.  Neither is a
decoder for these native 64-bit slots, so Iris opcode numbers must not be
copied into the fields above.

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
`0x20`, and `0x10`.  Joined to the backend packer, the first two slots have
operation tuples `(W0[4:0],W1[25:21]) = (0x1d,0x14)` and
`(0x1e,0x14)`.  Their reconstructed split selector
`(W1[3:0] << 3) | W0[31:29]`, `W0[28:22]`, and the untouched
`W1[20:14]` are all `0x7f`, the packer's default seven-bit sentinel.  Slot 1
sets `W0[21:15]=2`, while slot 0 leaves that field zero.  The encoder's
whole-program path also constructs the `0x1d/0x1e` family, and analysis routine
`0x1da987..0x1daa74` treats `0x1c/0x1d/0x1e` paired with second operation
`0x14` as a special family.  This proves the numeric operation selectors and
their pairing.  The `0x1d/0x1e` values are specifically the complementary
first/second result-component forms proved above, rather than evidence for
terminal or export mnemonics.  The purpose of the complete two-live-slot
fragment remains unresolved.

## Predicate and termination boundary

The static databases do not expose a reliable native predicate or `END` bit.
Across the final slot of all 48 fixed blobs, the bitwise intersection is zero
for both dwords: no bit is set in every fragment ending.  Several fragment
tails are all-zero slots, and linker `0x13bf80` appends identical all-zero slots
to round a linked stream to four-slot upload lines.  An all-zero slot is thus
padding/NOP-like and cannot be treated as a mandatory encoded `END`.  Because
the fixed blobs are composable pieces, this negative result alone does not rule
out a terminator in a complete program.  The program-context behavior above
supplies stronger negative evidence against a universal per-slot `END`: the
backend can replace one instruction, prepend two slots, patch fields while
splicing runs, and set an output-program property outside the stream.  The
property is not safely named `END`; its exact execution meaning remains
unknown.

The Iris textual predicate fields in `MxMGLX.dll` belong to the source compiler
container; no static path has yet joined them to a native packed bit.  The exact
remaining boundary is the linked compiler output around the call to
`0xadf00`: vary one source predicate or control-flow/end condition, retain the
descriptor pointer and byte length, output property `+0x397`, and generated-run
bytes `+0x39b/+0x39c`, then diff the unpadded 64-bit stream before `0x13bf80`.
Until that controlled compiler delta exists, predicate and termination fields
should remain unnamed.

The next static/runtime hooks should log `(slot, descriptor pointer, length)` in
`0xadf00` while `0x1454a0` runs, then dump the unpadded linked stream immediately
before `0x13bf80` rounds it to 32 bytes.  Comparing those 64-bit pairs with
controlled Iris source variants will expose compiler-to-native deltas before
MMIO capture is needed.

`tools/m9-shader-diff.py` automates that comparison.  It treats raw files as
two-dword slots, reports the containing four-slot upload line and lane, and
prints XOR masks plus changed bit ranges for each differing instruction.  Its
`--fields` mode also prints the already-proven numeric operation selectors,
result-routing selector, overlapping seven-bit descriptor fields, split
selector, and both scattered component masks before and after each change.  It
labels only the proven `0x1c/0x1d/0x1e` result-component modes and all-zero
slots; it deliberately does not guess native ALU, texture, predicate, or
termination names.
