# M9 first-flat selector-6c evidence boundary

This note records exactly what is known about the six arena references used by
the cold fixed-function triangle. It prevents the strong repeated-capture
inference from being reported as a saved image of the original draw. `[BIN]`
means pinned vendor binary evidence, `[CAP]` saved capture evidence, and `[DER]`
a conclusion constrained by those facts.

## Original 320x240 flat draw

The original direct, untextured, `GL_FLAT` `gltri` capture has no selector-6c
write in ExecuteDma sequences 0 through 11. Sequence 12 is its first 6c bind
and writes the same tuple twice [CAP]:

```text
68=00000000 69=11e42000 6a=25010000 6b=00001030 6c=40c3a301
```

The tagged selector therefore chooses the 0x80-aligned record at system offset
`0x300`. The capture directory contains command and ExecuteDma images only. It
contains no system resource/map image, so the bytes selected at `+0x300` were
not captured [CAP].

## What the controlled flat capture proves

Controlled case 37 runs one Gouraud baseline and then changes only to
`GL_FLAT`. Its flat B draw binds the same `68`, `69`, `6a`, and `6b` values and
selects its newly allocated record at `+0xa00`. Its defined native payload,
record `+0x20..+0x7f`, is exactly [CAP]:

```text
+04/+0c = 007ff880
+2c/+34 = 007ff8a8
+54/+5c = 007ff8d0
all other payload bytes = 0
```

The payload SHA-256 is
`3d0acf403423e8ffe97feae0de18632577a66fc6937c7a4b60b673175aca4f3c`.
Thirteen independent fresh processes, cases 18--28, 34, and 37, have that
exact 0x60-byte payload. These cases make their first transition from a zero
linked payload to a state needing the shared linked-fragment references. The
first address is `0x00800000 - 0x780`; subsequent addresses advance by
`0x28` [CAP][DER].

## Why `+0x300` versus `+0xa00` is irrelevant

Linux `m9x25711` obtains the pool host image, then copies 0x40 or 0x60 bytes
directly from `pipeline+0xe3c` to host-image `+0x20`. The later type-5 pool
operation allocates an outer backing slot and copies the host image byte for
byte. Neither backing allocation nor the copy rewrites internal payload words
[BIN]. Thus changing the selected outer slot from `+0x300` to `+0xa00` cannot
change the six values already present in `pipeline+0xe3c` [BIN][DER].

This closes a prior ambiguity about the record slot. It does not close the
compiler-arena cursor. Other pinned captures contain the same six field
positions beginning at `0x00801880` and `0x0083a420`, so the values are not
universal literals [CAP]. Their numeric value depends on compiler allocation
history.

## Exact status of the kernel constructor

For the cold diagnostic, `0x007ff880/0x007ff8a8/0x007ff8d0` is the uniquely
supported **bounded inference**: the old draw is the first selector-6c bind,
has the same flat linked controls, and every fresh process whose first
nonzero-linked transition is captured uses that schedule [CAP][DER]. The
current kernel constructor matches all available evidence. No kernel patch is
made by this closeout.

It is not yet a fully proven byte image for the original `+0x300` record. The
remaining missing edge is the upstream writer that converts compiler arena
state into `pipeline+0xe3c`. Static analysis has reached the direct copy out of
that field but has not recovered the aliased writer or its cursor inputs [BIN].

Either of these observations closes the last uncertainty:

1. Run the original 320x240 untextured flat program in a fresh vendor-driver
   process while saving the system mapping at its first sequence-12 submit;
   preserve at least map bytes `+0x300..+0x37f` plus command and ExecuteDma
   images from the same submit.
2. Stop before `m9x25711` instruction `0x44c0ad`, set a write watchpoint on
   that context's `pipeline+0xe3c..+0xe9b`, and record the writing routine,
   arena object/cursor before allocation, and resulting 0x60 bytes. The process
   must be fresh and the flat draw must be its first 6c program.

The first method is sufficient to validate the cold diagnostic. The second
also recovers the general allocator-dependent constructor needed after
arbitrary program history.

## Reproduction

```sh
python3 tools/m9-flat-first-linked-boundary-evidence.py
python3 -m py_compile tools/m9-flat-first-linked-boundary-evidence.py
```

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
