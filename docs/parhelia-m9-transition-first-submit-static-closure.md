# M9148 first-submit transition

This note records the static decision used by the clean-room first-triangle
diagnostic.  It supplements `parhelia-m9-pre-submit-engine-sequence.md` and
does not replace the exact address-window or requester recipes there.

## Do not lower the transition to idle MMIO

Linux `m9x421` emits three packet groups through its command builder:

```text
454545e6 00000002
45454747 000011a4 00001308
454522e6 40000000 (MMIO[1088] | 20000000)
```

Although packet indices `e6`, `47`, and `22` correspond arithmetically to
MMIO offsets `1398`, `111c`, and `1088`, the vendor does not perform those
writes through its direct-MMIO interface.  In particular, it reserves a
synchronization token around the `47` pair.  Replacing these packets with
idle CPU writes would discard the observed engine ordering and has no static
support.

## Construction order versus execution order

The `m9x582` construction order is:

```text
m9x1187 -> m9x421       build/publish transition
m9x865                  completion resources
m9x949 -> m9x182
              -> m9x109 arm primary requester
```

This is software construction order.  It does not require an unarmed
requester to execute the packets.  `m9x421` appends the packets through the
command-builder virtual interface; `m9x109` subsequently installs the stable
primary command, fence, and restore addresses.  A clean-room driver which
does not reproduce that retained builder lifetime can preserve the effective
hardware order by:

1. publishing the direct address windows;
2. initializing the restore and completion backing;
3. arming the primary requester in exact `m9x109` register order;
4. submitting the packet groups above as the first primary stream, followed
   by an `e3` completion packet;
5. requiring that completion before sequence 0.

The corresponding fenced stream is:

```text
454545e6 00000002
45454747 000011a4 00001308
454522e6 40000000 (MMIO[1088] | 20000000)
4545e3e6 00000003 (target | 1)
```

The command endpoint remains an untagged, end-exclusive byte address.

## Initial E6 choice

Linux tests one runtime bit and has two encodings:

```text
bit clear: 454545e6 00000002
bit set:   4545e6e6 00000000 00000002
```

Both finish at `e6=2`.  The saved Windows driver selects the analogous
single-write form for the same M9148 default profile because its selector,
`MmFlags` bit 0, is clear in `0x000101f8`.  No saved Linux command image
contains the transient packet.  The clean-room M9148 diagnostic therefore
uses the single-write form as the best cross-driver static specialization;
it should retain the two-write form as a guarded quirk rather than emit both
forms.

## Interpretation of the current failure

Consumption of all sequence-zero primary dwords with no secondary completion
shows that command fetch is alive but does not establish that the `2c/2d`
linked range was followed.  The supported next experiment is the independently
fenced transition stream above, followed by the existing sequence-zero
submission.  It is not a reason to write the packet indices directly through
MMIO.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
