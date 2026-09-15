# M9 auxiliary-window inputs and initial E6 packet

This closes the arithmetic and write order for M9148 `m9x421` MMIO
`0x360/0x364` and `0xf64/0xf68`.  It also identifies the exact selector for
the initial one- versus two-value E6 packet.  Values from the saved warm
register dump are stale state and are not initialization constants.

Evidence tags below mean binary disassembly (`BIN`), open wrapper source
(`SRC`), saved capture (`CAP`), and algebraic substitution (`DER`).

## Descriptor inputs

Let:

* `V` be local framebuffer bytes and `P = V >> 10`.
* `A` be the actual successfully allocated non-local aperture bytes, shifted
  right by ten.

The Windows descriptor builder at `MxMD.dll` RVA `0xcfb40` reads `V` from
`hw+0xb0`.  Its default M9148 flags are `0x101f8`, and its maximum-aperture
configuration getter returns zero.  The relevant descriptor fields are
therefore `[BIN]`:

```
C18 = 0x401000             C1c = P
C24 = 4*P
C28 = 0x401000 + 5*P       C2c = A
C30 = 0x401000 + 5*P + A   C34 = 0x40000
C38 = 0x441000 + 5*P + A   C3c = P
```

In the open Linux wrapper, `dev->framebuffer.size` is
`pci_resource_len(pci_dev, 0)` and is passed to the shipped core as
`uiFbLen`.  The canonical shipped `m9x798` copies that field into the device
memory object.  Thus clean-room Linux obtains `V` from PCI BAR0 length
`[SRC][BIN]`.  The saved target BAR0 is `0xd0000000-0xdfffffff`, so
`V=0x10000000` and `P=0x40000` `[CAP][DER]`.

Windows obtains `A` through miniport IOCTL `0x230458`.  `MxMD.dll` RVA
`0x85d50` stores output dword 3 into `hw+0xa0`.  The handler in `MxMM.sys`
RVA `0x702cf` selects and clamps a candidate using system-memory, registry,
and device policy, floors it to 4 MiB, attempts the allocation/mapping, and
reduces it by 32 MiB above 128 MiB or 4 MiB otherwise until successful.
Output dword 3 is the accepted byte count `[BIN]`.  Consequently `A` cannot
be inferred from PCI BARs.  A Linux driver should allocate its own non-local
aperture and feed the actual accepted size into this construction.

## Exact MMIO construction

The manager constructor at `MxMD.dll` RVA `0x58cc0` transfers `C28/C2c` to
the `0x360` range, `C30/C34` to its auxiliary-size object, and `C38/C3c` to
the `0xf64` range.  `m9x421` writes `[BIN][DER]`:

```
if A != 0:
    MMIO[0x360] = (0x401000 + 5*P) & 0xfffff00c
    MMIO[0x364] = 0x440000 + 5*P + A
MMIO[0xf64] = 0x441000 + 5*P + A
MMIO[0xf68] = 0x440000 + 6*P + A
```

Write order is conditional `0x360`, `0x364`, then unconditional `0xf64`,
`0xf68`, followed by `0xf74/0xf78/0xf7c`.  The identity
`MMIO[0x364] = MMIO[0xf64] - 0x1000` provides a useful assertion.

For the target's 256 MiB BAR0 this specializes to:

```
if A != 0:  0x360 = 0x00541000; 0x364 = 0x00580000 + A
always:     0xf64 = 0x00581000 + A; 0xf68 = 0x005c0000 + A
```

Here `A` remains in the hardware's KiB-scaled address domain.  For example,
an accepted 128 MiB allocation gives `A=0x20000` and ordered values
`0x541000, 0x5a0000, 0x5a1000, 0x5e0000`.  This example must not be treated
as evidence that the target accepted 128 MiB.

The smallest vendor-assisted observation, if comparison is required, is to
record the four output dwords of IOCTL `0x230458` before its paired cleanup,
or read `hw+0xa0`/descriptor `C+0x2c` before `m9x421`.  Issuing that Windows
IOCTL is not a read-only probe: it allocates and maps memory.

## Initial E6 packet

The canonical Linux `m9x421` tests exactly
`(*(builder+0x48)+0x28) & 1` `[BIN]`:

```
bit clear:  454545e6 00000002
bit set:    4545e6e6 00000000 00000002
```

Neither saved register capture contains this transient packet or the object
bit, so the Linux choice is a one-bit live boundary.  The smallest safe
observation is the byte at manager build time, or the command packet before
publication.  A clean-room bring-up should expose this as an explicit
observed quirk until captured rather than infer it from unrelated flags.

Windows tests `device+0x1051c` bit 0, which is `MmFlags` bit 0.  The saved
default constructor value `0x101f8` clears it, selecting `[BIN]`:

```
default:    454545e6 00000004
alternate:  4545e6e6 00000000 00000004
```

The payload differs between Linux and Windows (`2` versus `4`), so the
Windows packet must not be copied into the Linux initialization stream.

## Reproduction

Run:

```
python3 tools/m9-aux-window-evidence.py
```

The verifier pins the complete canonical vendor files, relevant code slices,
source-level BAR0 plumbing, capture columns, formula results, write order,
and both E6 variants.  It deliberately defaults to
`/tmp/m9vendor-original/kernel/KernelLibraries.o_shipped`; the working-tree
copy is not trusted because it may be modified during bring-up.
