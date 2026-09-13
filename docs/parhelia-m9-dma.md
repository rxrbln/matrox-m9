# Matrox M9 ("Phoenix") — bus-master DMA infrastructure

> **Unresolved conflict:** static vendor analysis shows a write to `0x400`,
> while M9148 testing found `0x400/0x404` did not latch or submit and instead
> implicated the descriptor bank at `0x500..0x524`.  Do not implement the ring
> from this note alone.  See `parhelia-m9-parhelia-3d-comparison.md` for the
> current boundary between confirmed facts and missing evidence.

The register-level map of the M9 command-DMA path, reversed from
`m9xdriver…/xserver/1_13_0/m9x_drv.so` (the DMA context init `m9x6295`, the
descriptor save/restore, and the submit worker `m9x26691`). This is what a
working EXA/XAA (and later GL) submitter must program. Read together with
`parhelia-m9-3d-submission.md` (the architecture: engine is DMA-FIFO-only, direct
MMIO cannot draw) and `parhelia-m9-accel.md` (the 2D op encoding = the packet
payload).

Confidence: `[DRV]` from the binary, `[HW]` verified on the M9148, `[RE?]`
inferred / not yet certain.

## Two meanings of the literal `0x404` — do not conflate [DRV]

A pitfall that produced earlier bad leads: `0x404` (and `0x400`, `0x401`) appears
both as a real MMIO register **and** as a BmBuffer command *tag*.

- Via the register accessor (`call *0x78(%rax)` on the object at `[dev+0x20]`,
  slot `0x78`=write32 / `0x88`=read32): a genuine MMIO write.
- Via the command-record builders (`m9x16709`@0x2bce10, `m9x26870`@0x2d1050):
  `0x404` is a *command opcode* queued into the DMA command list, value 1/2/0x14
  its parameter. **Not** an MMIO write.

Only accessor-`*0x78` sites are hardware register writes; everything below was
extracted with that filter.

## DMA context init — `m9x6295` [DRV]

Programmed once to arm the DMA context (ordered as in the binary):

| reg | value | meaning (inferred) |
|---|---|---|
| `0x400` | `[ctx+0x50]` | **PRIMADDRESS** — DMA command-list base (a bus/VRAM address) |
| `0x109c` | `0x80000` | context base A |
| `0x448` | `0x80000` | context base B |
| `0x1068` | `[ctx+0x18]` | write-back **fence/tag address 1** |
| `0x106c` | `[ctx+0x40]` | write-back fence/tag address 2 |
| `0x10ec` | (ctx) | context |
| `0x1094` | `[obj+0x44]` | context |
| `0x1098` | `base + 3·[obj+0x18] − 0x400` | computed context limit |
| `0x44c` | `[ctx+0x6c]` | DMA control field |
| `0x450` | `[ctx+0x70]` | DMA control field |
| `0x454` | `[ctx+0x74]` | DMA control field |

Plus, from the engine-init block already in `parhelia_m9_exa.c`
(`m9x` save/restore + IRQ setup): IRQ enable/status `0x8e4`/`0x8e8`/`0x8f8`/`0x8fc`,
surface windows `0x1320–0x1334`, aperture `0xf64/0xf68/0xf74/0xf78/0xf7c`,
context block `0x330=0x1000, 0x338=0x40, 0x33c=0x80, 0x340=0xc0, 0x354=0x03020100`.

## The DMA ring descriptor — MMIO `0x500–0x524` [DRV]

Eight dwords, shadowed from a driver struct `[ctx+0x30 … +0x4c]` and copied to
`0x500–0x524` (save/restore function). Layout (base/limit/head/tail/control) not
yet field-by-field certain `[RE?]`; the shadow at `[ctx+0x30]` is where the ring
geometry lives.

## Doorbells / front-end control [DRV][HW]

| reg | write | meaning |
|---|---|---|
| `0x314` | `1` | kick / restart DMA front-end (doorbell) — `m9x9938` |
| `0x31c` | `5` / `2` / `0` | park / resume / clear — `m9x27114` |

`0x314`/`0x31c` match `parhelia_m9_ring.c`'s `M9_2D_KICK`/`M9_2D_PARK` — those
offsets are correct.

## Completion sync [DRV]

`WGLContextManager::EqualizeDMATag()` — a memory write-back fence: the engine
writes a tag to the address in `0x1068`/`0x106c`; the driver polls that memory
word, **not** the `0x8f0` idle bits. Tag values are made non-zero so a cleared
word never reads complete.

## Submit sequence (assembled) [DRV]/[RE?]

1. **Arm** the context once — the `m9x6295` register set above, after the
   `EnableHardware`/`InitBoard` step (still a virtual call, `m9x28002` slot
   `*0x30`, not fully decoded — the one hard gap).
2. **Build** a BmBuffer: indexed register packets
   (`idx0|idx1<<8|idx2<<16|idx3<<24`, data dwords, `0x45` = stop) — the format
   already in `parhelia_m9_ring.c`, payload = the 2D op from `parhelia-m9-accel.md`.
3. **Post**: set PRIMADDRESS `0x400` = buffer base and program the end/length
   (PRIMEND path), then ring the `0x314` doorbell.
4. **Sync**: poll the write-back tag at `[0x1068]`.

## What is still uncertain (blocks a first-try implementation) [RE?]

- **EnableHardware arm register writes** — behind a C++ virtual call; the concrete
  subclass method was not cleanly decoded from the stripped binary.
- **PRIMADDRESS address encoding** — VRAM-local offset vs PCI bus address, and the
  low-bit memory-space tag.
- **Exact fetch trigger** — writing PRIMEND vs the `0x314` doorbell vs both, and
  the `0x500` descriptor's head/tail update.

These are why a fully-drawing DMA path cannot be written blind without risking a
card lockup on each wrong guess. The safe way forward (implemented as a
diagnostic) is to program the known context registers and **read them back**
without triggering execution, to learn empirically which latch and whether
`0x400` behaves as a DMA pointer once the context is set — the decisive datum the
earlier bare `0x400` probe lacked.
