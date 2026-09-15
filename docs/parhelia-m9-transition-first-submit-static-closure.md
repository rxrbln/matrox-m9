# M9148 first-submit transition

> **Superseded (2026-09-15):** the packet half of `m9x421` is gated on
> `B+0xc8 != 0` (`.text+0x16ab0`), a refcount that `m9x182` increments only
> after both cold transitions.  Both cold calls are MMIO-only; no
> `e6/47/e3/43/22` packet precedes sequence 0 on the M9148, and the
> post-publication packet path in `m9x887` is disabled by `MmFlags` bit 24.
> The streams below are therefore not a vendor transition; kernel commit
> `b788cc4` removes them.  See
> `parhelia-m9-cold-transition-submit-decode.md` section 1.

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

## Cold construction runs the transition twice

The cold `m9x582` constructor reaches `m9x421` twice.  The first call is
nested in initializer-table entry `m9x1042`: `m9x1042` calls the service
initializer at `.text+0x121d0`, which calls `m9x92`, then `m9x53`, and
`m9x53` calls `m9x421(..., publish=1)` at `.text+0x187aa`.  That complete
transition returns before `m9x53` calls the compact-table publisher `m9x328`
at `.text+0x1891b`.

After `m9x1042` and the three remaining initializer-table entries return,
`m9x582` calls `m9x1187` at `.text+0x2efd0`.  `m9x1187` calls
`m9x421(..., publish=1)` again at `.text+0x121bc`.  The exact cold logical
order is therefore:

```text
complete m9x421 transition #1
compact PTE CPU mirror and 31c invalidation
complete m9x421 transition #2
completion/restore allocation and primary requester setup
```

The first `m9x421` read of `0x1088` has no preceding software writer in this
path.  The immutable M9148 warm-unbound hardware sample also reads
`0x1088=0`.  Consequently the first terminal index-22 operand on the tested
M9148 is `0x00000000 | 0x20000000 = 0x20000000`.  The transition itself then
writes that value to index 22.  No code between the two calls changes index
22, so the second call also emits `0x20000000`, whether its CPU read observes
the first transition already executed or still observes zero.

Zero is therefore the expected first read on this M9148, not evidence of a
missing context-state initializer.  The implementation should nevertheless
retain the vendor read-modify-write operation rather than hard-code zero,
because it intentionally preserves any firmware or product-specific low
bits.

## Construction order versus execution order

The tail of the `m9x582` construction order is:

```text
m9x1187 -> m9x421       build/publish transition #2
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
3. submitting one complete `m9x421` transition and requiring its completion;
4. publishing the compact PTE mirror and its exact invalidation sequence;
5. submitting the second complete `m9x421` transition and requiring its
   completion;
6. requiring that completion before sequence 0.

Each complete transition retains the internal split below: initial `e6`, a
flush boundary, direct window publication, then the final `47`, dependency,
and `e6/22` group.  The PTE publication belongs between two such complete
transitions; it does not belong inside either one.

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
