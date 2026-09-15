# M9148 kernel service initialization capture

The missing runtime evidence is the interval from initial service construction
through the first primary doorbell. Existing client `ExecuteDma` captures
begin after the X server has constructed global vendor services. They do not
establish the initial register-write chronology.

`tools/m9x-cold-service-kgdb.py` collects this interval on the preserved vendor
Linux installation. It needs a KGDB connection and a **host GDB with Python**.
The currently installed host `/usr/bin/gdb` was checked on 2026-09-15 and is
built `--without-python`; it cannot run this script. The observer has offline
tests, but has not been exercised through KGDB on the target.

This is a kernel service trace. It does not capture the X driver's earlier
board POST, PCI bus-master enable, or direct userspace MMIO accesses. Those
still require a separate X-side observer if full cold-boot parity is needed.

## Exact register writer

The canonical `KernelLibraries.o_shipped` SHA-256 is
`bec4490ff72b29887400e988a26e107e80ebb0b11e9d0b437267bec9db5b26e7`.
The hardware accessor constructor `m9x798` installs `m9x748+0x10`, a vtable
pointer at `.rodata+0x70`. Its effective `+0x78` method is the relocation at
`.rodata+0xe8`, `m9x1019` at `.text+0x55f0`:

```text
48 8b 47 28 55 89 f6 48 89 e5 89 14 30 c9 c3
MMIO base = *(u64 *)(rdi + 0x28)
offset = (u32)esi
value  = (u32)edx
```

The observer stops at function entry, before the write, and records the caller
return address from the stack. Thus a zero register readback cannot hide an
actual write. `m9x1147` is a different vtable slot and must not be substituted.

## Setup

Boot the preserved vendor OS. Load the vendor module and its relocated symbols
into the host debugger before starting X or any operation that creates the
render service. Use the actual final linked module's section addresses; the
section offsets in the proprietary object are not module load offsets.

Supply a new directory on the **debugger host**, and the canonical object:

```gdb
set $m9_cold_output = "/host/captures/m9-service-NEW"
set $m9_cold_image = "/host/parhelia/xf86-video-parhelia/m9xdriver-x86_64-1.4.3-80847-20140908-build_98/kernel/KernelLibraries.o_shipped"
set $m9_cold_vram = 0
source /host/parhelia/xf86-video-parhelia/tools/m9x-cold-service-kgdb.py
continue
```

For compact-table snapshots, replace zero with a **verified kernel virtual
mapping of BAR0 offset zero**, covering at least `[0,0x941000)`. A physical BAR
address, MMIO mapping, userspace mapping, or unverified direct-map offset is
not sufficient. If no such mapping is available, leave zero: event and
allocation collection still works, and every omitted snapshot is explicitly
marked unavailable. The observer does not create mappings or call target
functions.

The complete local object hash and relocation-free live instruction signatures
are checked before any breakpoint is installed. Code or symbol mismatches
abort setup. A runtime vtable check guards the MMIO accessor fields.

## Collected evidence

`events.jsonl` contains a monotonically numbered event stream, debugger host
timestamps, and target thread IDs:

- entry of the anonymous service initializer at `m9x1187+0x40`
  (`.text+0x121d0`), followed by entry/return of `m9x1172`, `m9x53`,
  `m9x109`, `m9x926`, and `m9x115`;
- every `m9x1019` 32-bit write after that initializer entry, including its
  `f04`, `344..354`, `334..340`, and `330` writes preceding `m9x1172`;
- each `m9x887` mapping input, with its 0x48-byte descriptor, 0x80-byte
  allocation resource, GPUVA and extent, plus a post-publication table image;
- each `m9x535` publication-gate input, including allocation mode, the raw
  `System\\MmFlags` wrapper image and its actual getter method address;
- the four `m9x926` allocation returns at offsets `+0xed`, `+0x1b2`,
  `+0x277`, `+0x33b`, including success, allocation mode, request mode,
  complete 0x2b0-byte request, resource, GPUVA and extent;
- optional `[0x801000,0x941000)` table images at phase boundaries;
- a terminal marker immediately before the first `0x404` write.

The allocation data is recorded even if its mapping-publication branch is
skipped. Do not infer PTE writes or `0x31c` triplets merely from the four
allocation sizes: correlate the actual `m9x887` events and register writes.
In particular, the memory-manager property at device `+0x640` is distinct
from the hardware capability word `0x300004`. The usual `MmFlags=0x100e8`
has the `0x8`/`0x40` bits tested by `m9x535`; substituting the hardware word
would incorrectly predict that this publication branch is skipped.
Raw request/resource images preserve fields whose meaning is still uncertain.
The observer does not claim that a GPUVA is a physical backing address.

Every artifact and JSON event is flushed with `fsync`; binary artifacts carry
sizes and SHA-256 values in the event stream. Limits are 2048 events and 64
table images. Errors, bounds, a second service constructor, or an unexpected
accessor disable this observer's breakpoints and mark the capture incomplete.
The first `0x404` also disables them and **leaves the target paused before that
write**. Inspect the artifacts before continuing the vendor operation.

Debugger pauses change timing. This is evidence of CPU ordering, allocation
results and table contents; it is not a performance measurement or proof of
GPU execution. The first doorbell has not executed at the terminal marker.

## Offline checks

```sh
python3 tools/test_m9_cold_service_observer.py -v
```

The tests use the real vendor instruction bytes with a simulated GDB memory
interface. They check rejection of mismatched code and accessor vtables,
preservation of `0x31c` write order, stopping before the first doorbell, and
allocation field/artifact collection. They do not test KGDB availability,
GDB stack unwinding, or the hardware.
