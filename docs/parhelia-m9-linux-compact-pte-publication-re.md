# Linux M9148 compact page-table root and publication

This note closes the table object used by the normal Linux mode-1 scatter
mapping. It also reconciles the primary compact-table origin at `C18` with
the later `f64` range at `C38`, across which the higher compact slots land,
and proves that the visible `0x40xxxxxx` allocation address is used directly.
Recheck the arithmetic and pinned instruction sequences with:

```sh
python3 tools/m9-linux-compact-pte-evidence.py
```

Evidence below is from the shipped relocatable Linux core
`KernelLibraries.o_shipped`, SHA-256
`bec4490ff72b29887400e988a26e107e80ebb0b11e9d0b437267bec9db5b26e7`.

## Exact Linux target layout

Linux uses `MmFlags=0x100e8`. For 256 MiB actual VRAM (`V`) and a 256 MiB
BAR0 (`B`), `m9x858` constructs:

```text
P=(V>>10)+20000=60000       A=B>>10=40000
C00=00000000 C04=00800000 C08=00800000 C0c=00001000
C14=00400000 C18=00801000 C1c=00060000 C24=00000000
C28=00861000 C2c=00040000 C30=008a1000 C34=00040000
C38=008e1000 C3c=00060000 C10=00941000 C54=00d41000
C40=00010000 C4c=00000001
```

The Linux `P` contains the extra `0x20000` because flag bit 6 is set. `A` is
the BAR0 size because flag bit 5 is set. The companion extent `C24` is zero
because bit 4 is clear. These definitions differ from the Windows descriptor,
where `P=V>>10`, `C24=4*P`, and `A` is the successful nonlocal allocation
size. Windows formulas must not be substituted into this Linux layout.

`m9x565` sums `C04+C0c+C14+C1c+C24+C2c+C34+C3c`, giving one low root of
`0xd41000` bytes at GPU origin `C00=0`. It reserves `[0,0x800000)` and
`[0x800000,0x801000)` before returning its allocator to `m9x53`. With
`C4c=1`, `m9x53` makes these child objects:

| manager group | descriptor | child GPU interval | count |
|---|---|---|---:|
| primary `+0x20`, selector `+0x30=0` | `C18/C1c` | `[801000,861000)` | 1 |
| companion `+0x48`, selector `+0x58=0` | `C24=0` | absent | 0 |
| range `+0x70`, selector `+0x80=0` | `C28/C2c` | `[861000,8a1000)` | 1 |
| auxiliary `+0x98`, selector `+0xa8=0` | `C30/C34` | `[8a1000,8e1000)` | 1 |
| tail `+0xc0`, selector `+0xd0=0` | `C38/C3c` | `[8e1000,941000)` | 1 |

`m9x313` at `.text+0x117b0` proves that each child's `+0x48` is the root GPU
origin plus its allocated offset. The normal selected primary object's exact
`+0x48` is therefore `0x801000`, and its `+0x30` extent is `0x60000`.
The final `0x400000` bytes in the `0xd41000` root follow `0x941000`; they are
part of the root extent but not one of these five selected manager children.

## Allocation identity and zero page bias

The allocation object returned to userspace is the same object mapped by
`m9x887`:

1. `m9x1030` obtains the object in `r14` at
   `.text+0x1d0ac..0x1d0c4` and passes that same `r14` as `rdx` to `m9x887`
   at `.text+0x1d2f4..0x1d317`.
2. It later passes the same `r14` to `m9x1215` at
   `.text+0x1d3c8..0x1d3ce`.
3. `m9x1215` stores the object at request `+0x228`, reads object `+0x48`,
   and stores that value at request `+0x1cc` at
   `.text+0x130d2..0x13100`.
4. `m9x805` stores the request pointer at output `+0` and reads that same
   request object's `+0x48` into output `+8` at
   `.text+0xd344..0xd358`. `m9x925` supplies its ioctl blob `+0x200` as the
   output pointer at `.text+0x4b01..0x4b70`.

Thus a captured allocation result such as ioctl output `+8 = 0x40c3a000`
is exactly the resource `+0x48` later read by `m9x887`; it is not an alias
that must be converted to an internal `0x10xxxxxx` address.

The mapping descriptor is the stack object at `rbp-0x90` in `m9x1030`.
Descriptor `+0x20` is initialized by
`.text+0x1d14d: movl $0,-0x70(%rbp)`. There is no later write before the call
to `m9x887`; the next access is the post-call archive copy at
`.text+0x1d35d..0x1d361`. `m9x887` fills descriptor `+0x34` and `+0x38`
before calling `m9x328`, but does not change `+0x20`. Therefore:

```c
page_index = allocation_resource_gpu_address >> 12;
```

## Exact compact-table destination

`m9x887` selects manager `+0x20+8*(manager+0x30)` and unconditionally passes
that object to `m9x328` at `.text+0x1757d..0x1758d`. Before the call,
`.text+0x1744d..0x17454` reads the mode-1 allocation child's `+0x48`, shifts it
right by 12, and stores it in mapping descriptor `+0x34`. The identity chain
above proves that this is the command-visible system GPUVA. The normal
mode-1 descriptor adds no bias; in particular there is no `-0x30000` page
correction and no `0x40000000` to `0x10000000` normalization.

`m9x328` adds descriptor `+0x20` to that page number at
`.text+0x129d..0x12a7`, multiplies by four at `0x1548..0x1555`, and adds the
selected table object's `+0x48` at `0x156d..0x1591`:

```c
slot = 0x00801000 + 4 * (system_gpuva >> 12);
```

Consequently:

```text
GPUVA 40000000 -> slot 901000
GPUVA 40c3a000 -> slot 9040e8
GPUVA 40f52000 -> slot 904d48
GPUVA 4ffff000 -> slot 940ffc
```

This indexing intentionally reaches beyond the selected child's nominal
`0x60000` extent into later typed ranges of the same low root. The `f64/f68`
child `[0x8e1000,0x941000)` corresponds exactly to compact indices for GPUVA
`0x38000000..0x4fffffff`; therefore the captured `0x40000000..0x4fffffff`
system domain occupies `[0x901000,0x941000)`. Changing the origin to `C38`
would double-count the page index and is incorrect.

The separate `m9x953` interval beginning at
`manager+0xdc << 12 = 0x10000000` belongs to another allocator. No
instruction in the object-allocation, ioctl-output, `m9x887`, or `m9x328`
chain substitutes it for resource `+0x48`.

## Publication into the wrapper context

At the start of `m9x421`, the selected manager children are copied into the
object reached through `[service+0x10]`.  This object is not a low-address
BAR mapping or a hardware page-table root.  The open wrapper calls
`connect(dev->core_data, dev, &dev->ctx, ...)`; the exported `connect` entry
reaches `m9x320`, whose `.text+0x4c71` stores its third argument at core
object `+0x10`.  `m9x92` then stores that same core object at owner `+0x740`,
which is the service pointer used by `m9x421`.  Consequently these are
wrapper-private `m9x_context` fields.  For the Linux target they receive:

```text
raw+130 = 00801000     raw+134 = 00060000
raw+138/+13c omitted because the companion size is zero
raw+140 = 008e1000     raw+144 = 00060000
raw+148 = 00861000     raw+14c = 00080000
```

The last size is the selected range size plus the selected auxiliary size,
`C2c+C34`. This metadata publication happens before the command/MMIO window
transition, but a clean-room driver must not reproduce it by writing the BAR
root at offsets `0x130..0x14c`.

For direct Linux transport, `m9x421` writes the primary tuple and then, when
its publish argument is one, the remaining windows in this order:

```text
132c=00801000 1330=00801000 1334=00860000
360 =00861000 364 =008e0000
f64 =008e1000 f68 =00940000
f74 =00801000 f78 =00801000 f7c =00860000
```

The `f64` range is the tail child. It is not the compact-table base.

## PTE upload and invalidation

For an ordinary mode-1 scatter mapping, the compact PTE dword is:

```c
pte = (dma & 0xfffff000) | ((dma >> 28) & 0x00000ff0) | 0x0a;
```

`m9x328` stages entries with `m9x789`, uploads them through `m9x694` using
destination mode 2 and the low slot address, writes `MMIO[0x314]=1` when the
copy helper reports success, and mirrors the dwords through the table child's
BAR0 CPU pointer `+0x40`. The vendor therefore performs both publications;
the binary contains no path that proves a CPU mirror alone becomes visible
after invalidation. The normal compact-bind post-list is
`0x3000001f`; least-significant nibble first, it produces the exact
`MMIO[0x31c]` sequence `0xf,0x1,0x0`. Initial manager construction has the
separate final packed list `0x2000001c`, producing `0xc,0x1`.

This selection follows directly from the call arguments. `m9x887` passes
`ecx=0` to `m9x328`. That selector makes `m9x328` use manager `+0xe0` as its
empty pre-list and manager `+0xf8` as its post-list. `m9x53` constructs
`+0xf8=0x3000001f`. The `0x20000028` list at manager `+0x100`, which produces
`0,2`, is decoded later by `m9x887` only after building the optional 16-byte
companion table. The Linux `C24=0` branch skips that whole path, so `0,2`
must not replace `f,1,0` for a normal compact bind.

The outer service-owner order is also fixed.  The anonymous initializer at
`.text+0x121d0` writes the service-lane registers and calls `m9x1172` at
`0x12512`.  `m9x1172` writes `0x30c=0`, updates `0x3a8[17:16]`, and publishes
the service table.  The initializer then calls `m9x92`, which constructs the
manager by calling `m9x53` at `0x1e6be`.  The first `0x30c=0` is therefore a
real service-construction write; it does not replace the later
`0x30c=0x1000`.

Cold manager construction has an exact, different order.  `m9x53` first calls
`m9x421(..., publish=1)`, which completes the initial `E6`, all direct window
writes, and the terminal `454522e6,40000000,(1088|20000000)` submission.
It then calls `m9x328(..., selector=0)`.  For every staging chunk, `m9x328`
executes the auxiliary copy and synchronous wait, then writes `0x314=1`;
after all chunks it executes the empty `+0xe0` pre-list, writes the CPU BAR
mirror, and writes `0x31c=f,1,0`.  Back in `m9x53`, the next relevant write is
`0x30c=0x1000`.  After the remaining internal table initializers, `m9x53`
ends with `0x31c=c,1`.  Thus moving the terminal `m9x421` transition after the
compact invalidation reverses the vendor's cold order.

The corresponding `m9x694` arguments are:

```text
src      = staging GPU byte address returned by m9x789
dst      = table_resource+0x48 + 4*page_index
src_mode = mode returned by m9x789
dst_mode = 2
count    = compact PTE dword count
pre      = manager+0xe0 (empty for selector zero)
post     = manager+0xf8 (3000001f)
arg9/10  = 0,0
```

It emits 16-byte records into the auxiliary builder at `builder+0x18`, calls
`m9x620(builder,1)` to publish that stream through `MMIO[0x504]`, and calls
`m9x1016(builder,1)` to wait for `0x508.bit0` and stable `0x500/0x50c`.
For the concrete aligned example `src=0x12340000`, `dst=0x00901090`, modes
`2,2`, and one dword, the hash-pinned binary emits:

```text
0a000078 12340002 04000000 00901082
00000008 00000000 ffffffff 00000000
00000004 00000000 ffffffff 00000000
00000078 00000000 ffffffff 00000000
```

The last three records are the `f,1,0` post-list translated for the auxiliary
requester. These records cannot be submitted through the primary
`0x400/0x404` requester without separate hardware evidence.

The first transfer record depends on the transient staging allocation's
address and alignment, so a byte-identical live upload still requires the
runtime `m9x789` source. The destination formula, request modes, post records,
submission boundary, synchronous wait, BAR mirror, and invalidations are
fully fixed by the binary.

## Initial E6 selector

`m9x421` initializes the payload to 2, then tests exactly
`(*(builder+0x48)+0x28)&1` at `.text+0x16ade`:

```text
bit clear: 454545e6 00000002
bit set:   4545e6e6 00000000 00000002
```

Both forms leave the same final `e6=2` state. The saved captures do not include
this transient packet or the tested object byte, so static evidence does not
select the branch for the target. Keep this choice parameterized until it is
observed; do not emit both complete variants in succession. To clone the vendor
byte stream exactly, the minimal safe diagnostic is a breakpoint or kprobe at
relocated module `.text+0x16ade`, read `rbx`, then read
`(*(rbx+0x48)+0x28)&1`; alternatively capture the command bytes immediately
before the vtable submit at `.text+0x1660c`.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
