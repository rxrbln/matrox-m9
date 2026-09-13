# Matrox M9 ("Phoenix") — bus-master DMA infrastructure

> **Resolved privilege boundary:** the saved vendor kernel code writes requester
> base/control at `0x400` and submits the final address through `0x404`.
> M9148 userspace tests could not latch those registers because this requester
> programming belongs in the kernel.  See `parhelia-m9-3d-static-re.md` for the
> exact formulas and `parhelia-m9-parhelia-3d-comparison.md` for the remaining
> implementation boundary.

The register-level map of the M9 command-DMA path, reversed from
`m9xdriver…/xserver/1_13_0/m9x_drv.so` and its
`kernel/KernelLibraries.o_shipped` requester core (initialization function
`m9x109` at `0x2d150` and submit function `m9x1039` at `0x2d750`).  This is what
a working EXA/XAA and GL submitter must program. Read together with
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

## DMA context init — kernel `m9x109` [DRV]

Programmed once to arm the DMA context (ordered as in the binary):

| reg | value | meaning (inferred) |
|---|---|---|
| `0x400` | `([tagctx+0x50] & ~3) \| [BM+0x34]` | requester base/control |
| `0x109c` | `0x80000` | context base A |
| `0x448` | `0` | requester state |
| `0x1068` | `[tagctx+0x18] \| [tagctx+0x00]` | write-back **fence/tag address 1** |
| `0x106c` | `[tagctx+0x40] \| [tagctx+0x28]` | write-back fence/tag address 2 |
| `0x10ec` | optional `(([BM+0x44] + 0x200) & ~0x1f) \| ([BM+0x34] & 3)` | aligned context address with memory tag |
| `0x1094` | optional `([BM+0x44] + 0x200) & ~0x1f` | aligned context address |
| `0x1098` | `(base + 3 * field * 0x200 - 0x400) & ~0x1f \| 6` | computed context limit/control |
| `0x44c` | `[object+0x6c]` | DMA control field |
| `0x450` | `[object+0x70]` | DMA control field |
| `0x454` | `[object+0x74]` | DMA control/alignment field |

The kernel function does not write `0x1070`.  Earlier tentative lists which
included it were based on an incomplete Windows-side trace.

Plus, from the engine-init block already in `parhelia_m9_exa.c`
(`m9x` save/restore + IRQ setup): IRQ enable/status `0x8e4`/`0x8e8`/`0x8f8`/`0x8fc`,
surface windows `0x1320–0x1334`, aperture `0xf64/0xf68/0xf74/0xf78/0xf7c`,
context block `0x330=0x1000, 0x338=0x40, 0x33c=0x80, 0x340=0xc0, 0x354=0x03020100`.

## The DMA ring descriptor — MMIO `0x500–0x524` [DRV]

Kernel `m9x399` at `0x2b390` saves the requester descriptor.  It optionally
quiesces through `m9x1016`, reads `0x508`, writes back `old | 2`, waits, then
snapshots these eight dwords exactly:

| MMIO | object shadow |
|---:|---:|
| `0x500` | `+0x30` |
| `0x504` | `+0x34` |
| `0x508` | `+0x38` |
| `0x50c` | `+0x3c` |
| `0x510` | `+0x40` |
| `0x514` | `+0x44` |
| `0x520` | `+0x48` |
| `0x524` | `+0x4c` |

It sets saved flag `+0x2c`.  Restore function `m9x714` at `0x2b5d0` writes
`0x520,0x524` first; if saved `0x50c != 0x510`, it writes
`0x50c,0x514,0x510`; it then writes `0x500,0x508,0x504` and clears the saved
flag.  For an unsaved object it delegates to initializer `m9x759` at `0x2b290`,
which writes translated base/tag/address-space bits to `0x500`, zero to `0x508`,
and translated current/head to `0x504` [DRV].

Quiescence helper `m9x1016` at `0x28960` publishes the requester pointer through
`m9x620` to `0x504`, then polls `0x508` bit 0.  Once set, it accepts idle only
after two consecutive `0x500/0x50c` pairs are stable and a final `0x508` read
still has bit 0 set.  Its alternate branch combines the bit-0 poll with callback
`m9x966`.  This supports bit 0 as idle/stable and bit 1 as hold/freeze, although
those semantic names remain medium confidence.

### Hardware context-restore templates

Linux `m9x46` at `0x361d0` independently copies the exact `0xf8`-byte full 3D
index template into per-context memory and stores length `0xf8` at object `+0x20`
[DRV].  `m9x655` at `0x36bc0` allocates
`round_up((3*n + 1) * 0x200, 0x1000)`, fills it with `0x45454545`, and selects
builder `m9x1069` at `0x36620` or `m9x72` at `0x368e0` by object type.  They
build 0x50-byte records at `base + 0x700`, stride `0x600`, replacing ten
`0xffbeefXX` placeholders with addresses or control values.

Their fixed header sequences are:

```text
m9x1069: 454545e6 45454444 2f2e3c28 45452f2e 45e62f2e 454545e6
m9x72:   454545e6 45e3e6e6 2f2e3c28 45454543 45452f2e 453ee6e6
```

One substituted control chooses `0x0c000000` or `0x62000000`.  These are
hardware context-restore records with exact packet bytes; the ten relocation
roles still need names.

## Doorbell and front-end control [DRV][HW]

| reg | write | meaning |
|---|---|---|
| `0x404` | final write address | primary requester submit doorbell — kernel `m9x1039` |
| `0x314` | `1` | kick / restart DMA front end — `m9x9938` |
| `0x31c` | `5` / `2` / `0` | park / resume / clear — `m9x27114` |

`0x314`/`0x31c` are real controls, but they are not the primary submission
doorbell.  Kernel `m9x1039` pads the command list with `0x45454545`, stores its
final address in tag context `+0x50`, conditionally clears `0x31c`, then writes
the final address to `0x404`.

DDX `m9x27152` at `0x305570` is only the command-15 clear/park operation
(`0x31c <- 0` in its direct fallback), while `m9x12870` at `0x3055d0` is the
command-14 restart/kick (`0x314 <- 1`).  Kernel `m9x22` at `0x15f10` uses
`0x31c` states `5 -> 0 -> 2` around aperture/window restore, emits matching
`cb/cc/cd` and optional `c8/c9/ca` region bounds with `e6` transitions, fences,
then restores `0x1320..0x1334`.  This is requester parking and address-window
restore, not a full engine reset.

## Completion sync [DRV]

`WGLContextManager::EqualizeDMATag()` — a memory write-back fence: the engine
writes a tag to the address in `0x1068`/`0x106c`; the driver polls that memory
word, **not** the `0x8f0` idle bits. Tag values are made non-zero so a cleared
word never reads complete.

Linux `m9x721` at `0xee30` gives the exact first-domain wait.  It optionally
drains the current requester through `m9x679`, reads the hardware fence dword at
object `+0x30`, and compares values after masking low tag bits with `& ~0xf`.
It extends 32-bit wrap using `last32` at `+0x48` and epoch at `+0x4c`, then waits
until the combined value reaches the target.  While pending it writes
`0x314 <- 1` when object `+0x40` is zero, and aborts if `0x8f0` reads
`0xffffffff`.  `m9x390` at `0xef00` implements the nonblocking form.

`m9x946` at `0xef90` implements the analogous second tag domain with pointer
`+0x08`, last32 `+0x20`, epoch `+0x24`, and kick-control `+0x18`.  `m9x679` at
`0xed20` obtains the newest submitted fence through requester vtable slots
`+0xd8/+0xe0/+0x100`, then uses the same tag poll and `0x314` kick.  Completion
in the saved stack is therefore memory-tag polling; no recovered IRQ bit can yet
be assigned as DMA completion.

The Windows miniport independently confirms this completion model [DRV].
`MxMM.sys` function `0x224c8` snapshots its CPU producer pointer and writes the
requester base/control value to MMIO `0x400`.  Function `0x224f3` clears
`0x31c`, computes `producer_end - producer_start + gpu_base`, and posts that
address to `0x404`.  Its synchronous helper at `0x22533` advances a sequence
target by `0x10`, emits a 12-byte completion record, submits it, and then loops
until the dword addressed by object `+0x30` reaches the target.  Every iteration
writes `1` to `0x314`.  It has no timeout.  No part of this Windows wait consumes
an interrupt-completion counter, so the Linux and Windows implementations agree
that the requester fence is a GPU memory write-back and `0x314` is a progress
kick rather than a completion acknowledgement.

### Vendor waits that must not be copied literally [DRV]

The saved implementations contain four distinct unbounded waits:

| wait | vendor condition | disconnect handling |
|---|---|---|
| pending submission drain | tag-context `+0x54 == 0` in Linux `m9x1039` | none in this loop |
| front-end state | `(read(0x8f0) & 0x00030000) == 0` in `m9x1039` | the following state loop does not special-case `0xffffffff` |
| requester quiescence | `0x508` bit 0 set, two equal `0x500/0x50c` samples, then bit 0 still set in `m9x1016` | its alternate branch exits when `0x8f0 == 0xffffffff` |
| fence completion | extended memory tag reaches the target in `m9x721`/`m9x946` and Windows `0x22533` | Linux tag waits abort on `0x8f0 == 0xffffffff`; Windows does not |

These conditions observe different blocks and must retain separate diagnostics
and deadlines.  Reaching one does not prove the others are healthy.  In
particular, `0x8f0 & 0x30000` is not the fence, and a stable descriptor does not
prove that the GPU wrote the requested tag.

## Address-window initialization [DRV]

Linux `m9x421` at `0x16410` programs the following exact windows before it
submits matching indexed state:

| MMIO | value |
|---:|---|
| `0x360` | `base & 0xfffff00c` |
| `0x364` | computed limit/base control |
| `0xf64` | `base` |
| `0xf68` | `base + size - 0x1000` |
| `0xf74`,`0xf78` | `base` |
| `0xf7c` | `limit` |
| `0x1320`,`0x1324` | `base` |
| `0x1328` | `limit` |
| `0x132c`,`0x1330` | `base` |
| `0x1334` | `limit` |

Its indexed mirrors are `45cdcccb` (`cb=base, cc=base, cd=limit`) and
`45cac9c8` (`c8=base, c9=base, ca=limit`).  Later transitions write index `47`
with `0x11a4` then `0x1308`, followed by `e6 <- 0x40000000` and index `22 <-
read(0x1088) | 0x20000000`, with submission/waits around the groups.

The separate service initializer beginning at object-file text `0x121d0`
allocates 0x50 bytes, derives two fields from `0xf04`, clears `0xf04` bits
28:29, zeros `0x344..0x350`, and writes `0x354=0x03020100`, `0x334=0`,
`0x338=0x40`, `0x33c=0x80`, `0x340=0xc0`, and `0x330=0x1000`.  It initializes
`0xf0c` with `0x8000`, then replaces bits `9:6` from a device property.  These
are exact values; the address-translation/cache field names remain provisional.

## Submit sequence (assembled) [DRV]/[RE?]

1. **Arm** the context once — enable `PCI_COMMAND_MASTER`, allocate requester
   resources, then program the kernel `m9x109` register set above.  Linux
   `m9x28121 -> m9x30894` proves the bus-master prerequisite; its remaining
   work is a userspace bridge/display-services lifecycle, not 3D reset MMIO.
2. **Build** a BmBuffer: indexed register packets
   (`idx0|idx1<<8|idx2<<16|idx3<<24`, data dwords, `0x45` = stop) — the format
   already in `parhelia_m9_ring.c`, payload = the 2D op from `parhelia-m9-accel.md`.
3. **Post**: configure requester base/control at `0x400`, build the proven
   `0x2c/0x2d` start/end pair in the command stream, pad to the configured
   alignment, then write the final command address to `0x404`.
4. **Sync**: poll the write-back tag at `[0x1068]`.

## Safe kernel bring-up and bounded recovery [DRV]/[RE?]

The statically defensible bring-up order for a future kernel backend is:

1. Map the BAR and reject a disconnected device (`0x8f0 == 0xffffffff`).
2. Mask the interrupt banks and acknowledge stale status before registering and
   enabling individual sources.  Initially enable only the four proven vblank
   sources.
3. Enable `PCI_COMMAND_MASTER`, then allocate and pin requester, context, and
   fence memory subject to the still-unresolved M9 address-tag constraints.
4. Program the address windows and `m9x109` context registers, initialize the
   `0x500..0x524` descriptor, and seed fence memory so zero cannot look complete.
5. Submit one bounded no-op/completion record.  Permit rendering only after its
   memory tag advances and all relevant MMIO reads remain connected.

Steps 3--5 remain behind an experimental opt-in until address domains and the
completion record are captured.  They are an implementation order derived from
the vendor call graph, not evidence for an undiscovered global reset sequence.

Every kernel wait needs an absolute deadline and checks for device removal,
signals where sleeping is allowed, and a reset-generation change.  On timeout,
the driver should first stop accepting submissions and serialize recovery, then
snapshot at least `0x8f0`, raw/enabled IRQ state, `0x400/0x404/0x448`, the full
`0x500..0x524` descriptor, `0x31c`, fence addresses/contents, and software
producer, target, and wrap-epoch values.  Capturing these before acknowledgement
or reprogramming preserves the evidence needed to distinguish a bad relocation,
requester stall, engine hang, and disconnect.

A requester-local restart is defensible only after the stable/quiescent
handshake succeeds.  One bounded attempt may then use the known park/hold path,
restore `0x520/0x524`, optionally `0x50c/0x514/0x510`, and finally
`0x500/0x508/0x504` in the exact `m9x714` order, restore the address windows,
kick `0x314`, and require a new no-op fence.  The driver must attach a generation
to each job and fence, re-seed write-back memory on restart, and report every
outstanding fence from the old generation as an error; otherwise a stale tag can
falsely complete a new job.

If quiescence cannot be reached or that one requester restart fails, fail the
engine closed while preserving scanout when possible.  The recovered `0x31c`
states and descriptor bit 1 park requester traffic; they do not reset a wedged
3D engine.  Static analysis has found neither a complete engine/global reset nor
proof that PCI function reset is safe on these display devices.  A production
driver must not improvise either operation or repeatedly rewrite a live
descriptor.

## What is still uncertain (blocks a first-try implementation) [RE?]

- **Engine-specific reset/arm sequence** — `m9x30894` enables PCI bus mastering
  and constructs board/display services, but contains no direct 3D reset MMIO.
  The requester core is decoded separately; any additional engine reset must
  be found in its initialization callers or established by bounded testing.
- **Requester address domains** — the origin of each base component, the exact
  VRAM/system-memory tags, and all relocation constraints.
- **Descriptor field semantics** — save/restore order and idle handshake are
  exact, but base/head/tail/address-tag meanings and wrap handling are not all
  joined to `0x400/0x404` submission.
- **Recovery** — status/mask/W1C banks are decoded, but no high-bank source has
  a defensible DMA-completion/fault name and no full 3D/global reset was found.
  `0x31c` state changes and the `0x500..0x524` save/restore handshake provide a
  bounded requester-local recovery candidate, not a global reset.

These are why a fully-drawing DMA path cannot be written blind without risking a
card lockup on each wrong guess. The safe way forward is a kernel-only diagnostic
which programs and reads back the known context registers without triggering
execution, then submits a bounded no-op/fence list.  This validates address tags
and descriptor ownership without exposing requester MMIO to userspace.
