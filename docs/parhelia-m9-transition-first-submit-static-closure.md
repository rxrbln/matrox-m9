# M9148 first-submit transition

This note records the static decision used by the clean-room first-triangle
diagnostic.  It supplements `parhelia-m9-pre-submit-engine-sequence.md` and
does not replace the exact address-window or requester recipes there.

## Do not lower the transition to idle MMIO

Linux `m9x421` emits three packet groups through its command builder, but it
publishes them in two distinct submissions:

```text
454545e6 00000002
45454747 000011a4 00001308
454522e6 40000000 (MMIO[1088] | 20000000)
```

The call at `.text+0x1660c` submits the first `e6` packet through vtable slot
`+0x100`.  Only after that call does `m9x421` publish the address windows.  At
the tail it appends the `47` packet, reserves a synchronization value through
slot `+0xe0`, reads `0x1088`, appends the `e6/22` packet, and submits that work
through slot `+0x100` at `.text+0x16a0a`.  Slot `+0x100` resolves to `m9x954`;
with argument one it reaches `m9x1039`, the primary requester doorbell path.
Thus `+0x100` is a real flush boundary, not a builder-only state update.

The slot `+0xe0` call is also observable command content.  It resolves to
`m9x81(-1)`, which calls slot `+0xd8` = `m9x604`.  That routine advances the
common submission target by `0x10`, emits `e3 = target | 1`, and returns the
low-bit-clear target.  `m9x81` immediately emits `43 = target`.  The later
`m9x954` flush advances the target by another `0x10` for its terminal
completion.  Captured streams have this exact pattern:

```text
... e3 = 0000eb40
    43 = 0000eb40
... 22 = 0008000e
... e3 = 0000eb50
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

1. initializing the restore and completion backing;
2. arming the primary requester in exact `m9x109` register order;
3. submitting the initial `e6` packet alone and requiring its completion;
4. publishing or verifying the direct address-window MMIO state;
5. submitting the final `47`, inner completion/dependency, and `e6/22` packet
   groups together, followed by a distinct terminal `e3` completion packet;
6. requiring that completion before sequence 0.

The corresponding clean-room streams are:

```text
454545e6 00000002
4545e3e6 00000003 (target1 | 1)

45454747 000011a4 00001308
4545e3e6 00000003 (target2 | 1)
45454543 (target2)
454522e6 40000000 (MMIO[1088] | 20000000)
4545e3e6 00000003 (target3 | 1)
```

Targets advance by `0x10`.  If the first submission uses `target1=0x10`, the
dependency is exactly `target2=0x20`, while the final fence to poll is
`target3=0x30`.  Reusing `0x20` only as the terminal fence omits the vendor's
inner producer/dependency pair.

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
linked range was followed.  The supported next experiment is the two
independently fenced transition streams above, with the direct windows between
them, followed by the existing sequence-zero submission.  Combining all three
packet groups into one stream removes a statically proven vendor flush boundary
and should not be used as the reference experiment.  The failure is not a
reason to write the packet indices directly through MMIO.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
