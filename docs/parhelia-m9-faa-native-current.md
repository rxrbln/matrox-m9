# M9 FAA native testing, 2026-10-06

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0-only

Nano now has an M9 adapter. The first test must identify PCI `102b:0540`,
record the loaded module/runtime identities and establish ordinary textured
rendering before writing multisample state. This note is separate from P690
`102b:2539` evidence.

`tools/m9-faa-smoke.sh` runs the current instrument at 320x240, its fixed
measurement size. Application performance checks remain at 1024x768. Run it
under the target owner's common lock and both historical M9 locks:

```sh
flock /tmp/parhelia-hw.lock flock /tmp/m9-hw.lock \
  flock /root/tmp/m9-hw.lock env M9X_MESA=/root/parhelia-m9x-mesa \
  M9X_OUT=/root/tmp/m9-faa-smoke sh /root/parhelia-m9x-tools/m9-faa-smoke.sh
```

The smoke runs `texture`, `faarect`, `faaedge` and `faapos` with FAA off,
then the three FAA instruments with only `M9X_FAA=1`, followed by ordinary
texture rendering again. Each child gets a clean environment: inherited
mode overrides, alpha-to-coverage, arbitrary bind indices and the software
multisample calibration option cannot contaminate the run. New kernel
timeouts, a nonzero child exit or a different renderer stop the sequence.
The paired phase proves only current AD/AF packet plumbing. True FAA needs
nonzero partial edge levels, a full 48000-pixel integer-edge rectangle,
valid sample staircases and GPU retirement; a process exit of zero does
not prove any of them. Hardware results are pending until logs are captured.

Current canonical `m9x` commit `6780573` already has the diagnostic store
allocator, the `bc/bd` fragment-store bindings, optional `b8/b9` companion
binding and `RESET_FRAG_PTR`. Kernel `m9x/m9_submit.c` requires explicit
`faa_probe_indices` permission for `bc/bd` and `a1/a3/a4`; ordinary AD/AF
and the raster `ef` word need no probe whitelist. No shipping MSAA caps are
advertised. Module dependencies are `i2c-algo-bit`, `drm_client_lib`,
`drm_dma_helper`, `drm_ttm_helper` and `drm_vram_helper` on the current target.

Previous M9128 measurements already tried mode6 plus the genuine-sized
store, pointer reset and every 128 companion format codes. The store had
zero changed dwords and the diagonal had zero partial levels. Modes6/7
reserved approximately one eighth of the pixels; some descriptor classes
restored those pixels without antialiasing. Repeating those sweeps is not
an implementation step. Alpha-to-coverage, mode bits in preprocessing, and
mode6/7 with ordinary D24S8 are established hangs.

The missing contract includes the companion's linked kind9 resource and
`ba/bb` descriptor, sentinel ownership/save and restore, admission from
the initialized vendor context, resolve and destination retirement. The
ordinary implementation binds `ba/bb` to its ordinary auxiliary depth
resource; replacing only `b8/b9` is not proof of the vendor's complete AA
companion graph. Static recipes should not be equated with a full FAA
submission.

Actual MSerX emulation has executed the AD/AF and reset emitters, but with
controlled host owners. The isolated M9 emulator worktree tracks progress
through genuine miniport initialization. A complete enabled vendor lifecycle
capture is still required to close the missing contract. The older main
FAA note's absence of this new bootstrap progress is historical, not a
statement that no M9 emulation was run.

## Additional real store-binding operation found during this round

Static MSerX `MxMD.dll` emitter `0xfb920` contains a larger conditional
store-binding operation than the existing native `bc/bd` pair. Dirty bit
`0x40` at `0xfbca4` tests a backing-address pair in shadow `+0x9c/+0xa0`
against the global null pair `0x20d51c/0x20d520`. If different, it emits:

1. Index `0x24`, using genuine relocation kind `0x2b` on that address pair
   (`0xfbd09` header `0x45454524`; callback at `0xfbd29`).
2. Three `e6` words: `0x6dc00000`, `1`, `1`
   (`0xfbd4b` header `0x45e6e6e6`, values at `0xfbd55/0xfbd5f/0xfbd65`).
3. The familiar `bc/bd` bindings (`0xfbd8e` header `0x4545bdbc`).

This is conditional vendor code, not an enabled FAA API capture. The native
path emits neither index24 nor this e6 triple. The current kernel also
rejects index24 unless explicitly listed as a probe index, and rejects
`0x6dc00000` and `1` as e6 controls. Whitelisting the index alone does not
admit the operation.

The AA binder `0xaa750` does fill the additional address pair. At `0xaad07`
it selects a device-owned resource at `hw + 0xfed8 + slot*144`, then calls
that resource's virtual method `+0x5c` with descriptor kind5 and destination
shadow `+0x9c` (`0xaad13..0xaad1c`). `hw` is `[device+8]`, and `slot` is
`[device+0x24]`, with `device` read from the AA surface `+0x238`. Thus this
is a separate per-slot owned resource; it is not a justified alias of the
main fragment store or the companion.

Helper `0x756f0` returns the 144-byte slot entry at `hw+0xfebc`, placing
that resource at entry `+0x1c`. Allocator `0x76540` reuses it only on its
pool6 path; when absent it calls `0xa3870` with an allocation request and
publishes the result to entry `+0x1c` at `0x7661a`. Its request uses flags
`0x8002`, request-format `0x1a`, width derived from `(bytes+3)>>2`, and
rows derived from `(width+1023)>>10`. The request-format and descriptor-kind
namespaces must not be conflated with texture format-table IDs.

Required next proof is the resource's genuine address method, allocation
size/domain and initialized owner lifetime, followed by the operation's
dispatch/retirement semantics. Replaying a guessed index24 address would
not answer those questions. The emulator agent is executing the complete
conditional emitter with recorded callback arguments separately.
