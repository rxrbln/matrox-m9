# M9128 display-route scheduler: bounded original-source check

The physical M9128 `102b:0540` fullscreen interval-0 path currently copies
each presented frame. Removing that copy would matter, but enabling async flip
requires a qualified latch and old-scanout-owner retirement contract. The
original `PhxSetDisplayStart` leaf and complete
`H2OCeSetDisplayStartAddress` API establish write grammar only.

`tools/m9-display-route-scheduler-check.py` executes a separate original
route-transition routine, `m9x11412` at `0x2b63d0`, in the pinned M9 Linux
driver image (SHA256 `3878475450ffad5cd648cc037cc65c29fcb07b5e86caf41ccf5983ef8e570273`).
The CPU descriptor and three 28-byte route records are synthetic owned data.
The current route ID is 0; the requested ID is 1. When row 1 has required
status 3, the original code enters `m9x25201` at `0x2b5650`, which calls
`m9x17687` with zero. On the M9128 branch, `m9x8988` at `0x2f80a0` calls the
actual read service at `0x2b8ad0`, requesting offset `0x318` with a stack
output. The emulator **stops at that service**. The setter `m9x7332` at
`0x2ed6d0`, the `m9x6104` family dispatcher, and the write service have not
been reached. No read value is invented. Changing only row 1 status to 2
returns a refusal before a read, setter or write.

The source instruction after the read takes the high byte of the returned
32-bit value, clears bit 6 when the argument is zero (sets it otherwise), and
writes the resulting word through the original write service. This is an
actual read-modify-write dependency; a write-only trace of the display-start
setter cannot stand in for the full transition. The direct-call inventory
includes the route setter's calls into the same `m9x6104` dispatcher used by
the complete display-start API, but this bounded run intentionally ends
before that point.

This routine has not been traced from the Linux Present callback or proven
to be the active KMS update path. Its `0x318` read is **not** a measured
scanout latch, completion or vblank acknowledgment. The next source experiment
is to identify the actual original caller for route transitions, then trace
the complete M9128 transition with read values acquired from a controlled
source environment and observe which event permits release of the old scanout
owner. Any eventual native update must derive start addresses from live pinned
backing. Async capabilities remain disabled until latch, event and lifetime
are qualified on the target under the established lock.

Run the CPU-only proof with:

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9-display-route-scheduler-check.py \
  --out /tmp/m9-display-route-scheduler.json
```
