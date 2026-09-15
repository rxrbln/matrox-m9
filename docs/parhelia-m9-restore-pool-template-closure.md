# M9 first-render restore-pool template closure

This note gives the byte-exact Linux `m9x655`/`m9x72` restore-pool image used
by the normal M9148 first-render path.  Evidence is the canonical proprietary
Linux core `KernelLibraries.o_shipped`, SHA-256
`bec4490ff72b29887400e988a26e107e80ebb0b11e9d0b437267bec9db5b26e7`.
Run `python3 tools/m9-restore-pool-template-evidence.py`; `--emit-c` prints the
two arrays in kernel-ready form.  The verifier never executes vendor code.

## Allocation and complete initialization

`m9x182+0x5b7` constructs this object with type 1, calls
`m9x1160(object, 0x400)`, and treats failure as fatal before `m9x109` arms the
requester.  `m9x655` computes

```c
size = ALIGN((3 * count + 1) * 0x200, 0x1000);
```

so `count=0x400` requires exactly `0x181000` bytes.  It initializes the
complete allocation as follows, in this order:

1. fill every dword with `0x45454545`;
2. copy the fixed 0xf8-byte template below to byte offset zero;
3. write one 0x50-byte type-1 record at `0x700 + i * 0x600` for every
   `i=0..0x3ff`.

The exact fixed prefix, as little-endian dwords, is:

```text
eeedeff0 f4f3f2ec 47474747 b3b2b1b0 b7b6b5b4 bbbab9b8 45bdbcbe 47474747
abaaa9a8 f6f5aeac a5a7c0af c7c6c5c4 45454547 ada4a3a1 47474747 c28a8988
8e8d8cc3 75797874 78767978 45454547 79787779 47474747 7a797870 83827d7c
81808584 45877f7e 47474747 7a797871 83827d7c 81808584 45457f7e 47474747
7a797872 83827d7c 81808584 45457f7e 45474747 7a797873 83827d7c 81808584
45457f7e 45474747 d1d0ebea 45d4d3ed c1d68b8f 47474747 93929190 97969594
9b9a9998 9f9e9d9c 47474747 6b6a6968 456e6d6c 63626160 67666564 45474747
45f3d7f1 dad9d8d7 dedddcdb 45454547 e8d5a2d2 00000000
```

The exact 0x50-byte static record before its four dynamic substitutions is:

```text
45e3e6e6 624f7fff 00000000 ffbeef04 2f2e3c28 fffffff0 00000000 00000000
00000000 45454543 ffbeef01 45452f2e fffffff0 00000000 453ee6e6 ffbeef03
00000000 08000000 624fbfbf ffbeef07
```

Let `R` be resource field `+0x44`, the pool's GPU address, and let `T` be the
low two bits of resource field `+0x34`.  For record `i`, define
`W = R + 0x200 + i * 0x600`.  `m9x72` replaces precisely:

```text
record + 0x0c = i == 1 ? 0x0c000000 : 0x62000000
record + 0x28 = (W + 0x400) | 1
record + 0x3c = R + 0xf8
record + 0x4c = (W + 0x440) | T
```

No uninitialized holes remain: areas outside the prefix and records retain
the `0x45454545` fill.  The final record begins at `0x180100` and ends at
`0x18014f`, within the `0x181000` allocation.

## Requester arm and the zero branch

With this backing present, `m9x109` writes:

```text
10ec = ((R + 0x200) & ~0x1f) | T
1094 =  (R + 0x200) & ~0x1f
1098 = ((R + 0x17fc00) & ~0x1f) | 6
```

The function also has a literal fallback which writes zero to these three
registers when the restore object/backing is absent.  That fallback is a valid
reduced requester transport state, but it is not the vendor's normal first
triangle state: `m9x182` requires the 1024-entry pool to be allocated and
built before it calls `m9x109`.  A clean-room first-render implementation must
therefore supply the nonzero pool tuple and publish the initialized DMA memory
before arming or ringing the requester.

## Backing-class boundary

`m9x655` allocates through `m9x306` using an ordered capability-dependent list
of internal allocation modes 0, 1, and 2.  The successful resource, rather
than the requested candidate, supplies both values consumed later:

```text
R = resource +0x44
T = resource +0x34 & 3
```

The static path does not force one candidate to succeed on every M9 board, and
these internal modes are not identical to the open wrapper's public memory
enum.  Consequently neither a fixed low tag nor a claim that `R` is always a
raw local address, a system GPUVA, or a requester DMA address is justified by
`m9x655` alone.  The clean-room allocator must choose a supported backing,
retain its resulting command-visible GPU address and access tag as one typed
pair, then use that same pair in every record and in `0x10ec`.  For a live
vendor join, the smallest sufficient observation is the successful restore
resource's `+0x34` and `+0x44` immediately before `m9x109`.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
