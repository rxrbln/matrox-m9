<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
# The inter-pass record's traffic, and why it bounds a multi-pass scene

DERIVED 2026-09-18 from the driver's own stride rule and a MEASURED frame
rate.  Not yet confirmed against a memory-bandwidth measurement, which is
the one thing that would settle it - see the open question at the end.

## The arithmetic

The record carries **four 32-bit floats a fragment per hand-over**, and the
driver allocates one slot per hand-over: `stride = 4 * (passes - 1)` dwords.
Every hand-over is written once and read once, so at 1920x1200:

| passes | stride | traffic a frame | at 60 fps | record |
|---:|---:|---:|---:|---:|
| 2 | 4 dw | 74 MB | 4.4 GB/s | 35 MiB |
| 4 | 12 dw | 221 MB | 13.3 GB/s | 105 MiB |
| 5 | 16 dw | 295 MB | 17.7 GB/s | 141 MiB |

`effect2d`'s 15-tap kernel is the five-pass row and MEASURED **31 fps**,
which is 9.1 GB/s of record traffic alone - before a texture read or a
colour write.  Reaching 60 fps would need 17.7 GB/s from the record alone.

That also retires the fill-rate explanation for this scene: the model gives
19 full-screen operations at 1.157 ms, so 45 fps, and the scene runs at 31.
The model was not measuring what it appeared to.  (The same model was
already refuted outright on `refract`, which turned out to be 95% CPU
transfer.)

## Why the stride can be smaller

A slot per hand-over is only necessary if a pass can read an output older
than its immediate predecessor.  When the pass graph is a **chain** - pass
N writes, pass N+1 reads, and nothing reads further back - two slots
suffice, used alternately, and the stride is 8 dwords whatever the pass
count:

| passes | by liveness | at 60 fps | record |
|---:|---:|---:|---:|
| 4 | 8 dw, 147 MB | 8.8 GB/s | 70 MiB |
| 5 | 8 dw, 147 MB | 8.8 GB/s | 70 MiB |

For two passes the present rule is already minimal and must be kept: a
ping-pong would double it.  So the rule is **allocate slots by liveness**,
which degenerates to `min(4 * (passes - 1), 8)` for a chain and gives each
value its own slot only where one is genuinely live across another.

This halves the traffic for the cases that need it and halves the record
with it - 141 MiB to 70 MiB for the five-pass case, 105 to 70 for the
four-pass one.  Both of those sizes have been refused on this card before
now, so the saving is not only bandwidth.

**The correctness condition is the part to check first.** The splitter is
known to produce chains where every middle pass consumes and produces a
routed coordinate, which is the good case; a schedule where a later pass
reads an earlier pass's output directly is not, and would need its own
slot.  Liveness answers that per shader rather than by assumption.

## Open

Whether 8.8 GB/s is itself reachable is unknown: this board's memory
bandwidth has not been measured.  Until it is, the honest statement is that
the five-pass scene is bandwidth-bound rather than fill-bound, that the
present stride rule costs twice what a chain needs, and that whether 60 fps
is reachable after halving it is an open question rather than a promise.
