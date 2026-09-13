# Matrox M9 native shader fragments

This note records the native microcode database recovered statically from the
saved Windows `MxMD.dll` and cross-checked against Linux `m9x_drv.so`.  It is a
companion to `parhelia-m9-3d-static-re.md`.  Addresses and lengths below are
binary evidence; semantic shader-operation names remain unknown.

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
