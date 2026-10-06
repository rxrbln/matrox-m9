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
`0x8002`, final request-format0 (the earlier `0x1a` write at `0x7656e`
is overwritten at `0x765e3` before allocation), width derived from
`(bytes+3)>>2`, and
rows derived from `(width+1023)>>10`. The request-format and descriptor-kind
namespaces must not be conflated with texture format-table IDs.

Required next proof is the resource's genuine address method, allocation
size/domain and initialized owner lifetime, followed by the operation's
dispatch/retirement semantics. Replaying a guessed index24 address would
not answer those questions. The emulator agent is executing the complete
conditional emitter with recorded callback arguments separately.

The real interface getter is also identified. Resource-interface constructor
`0xa32a0` installs vtable `0x1f3a90`, whose slot `+0x5c` is `0xa6740`.
Its descriptor-kind5 dispatch entry `0xa68fc+5*4` targets `0xa6776`, which
selects layout selector2. It invokes descriptor builder `0x467e0` on
`[interface+0xc0]+0x3c` if the selector's valid bit is absent, then reads
that selector's address at layout `+0x20`. For an ordinary directly backed
resource (`interface+0xa4/+0xa8` zero), it returns an address pair
`{0, address}`. The indirect branch returns `{0x40000000, &interface+0x74}`,
with that reference carrying the allocation object and address offset
(`0xa6811..0xa682b`). A relocation recorder must resolve that branch through
the genuine allocation owner; treating its host pointer as a GPU address
would fabricate a binding. This identifies the getter contract statically,
but not the per-slot resource's purpose, initialized lifetime or physical
extent.

## Is vendor FAA reachable through normal options?

The saved retail MSerX binaries contain a normal configuration-to-pixel-format
path, beyond the debug strings and controlled emitters. This is static
reachability evidence, not proof of an initialized M9128 completing FAA:

* The MxMD configuration constructor reads `OGL\Multisample` with default0
  at `0xb9f49`. The real `DrvEscape` query `0x80040610` handler `0xd4180`
  returns that option at output `+0x20` (configuration wrapper `+0x2ec`,
  virtual getter at `0xd429a`). The same response carries `3D\AAMode & 1`
  at output `+4` via the actual getter `0x64760`. Default AAMode
  `0x80000842` has that bit clear.
* The ICD's actual pixel-format enumeration function `0x10043900` calls
  configuration query `0x10048d10` at `0x1004393c`. The query combines the
  two response values into global `0x108aa200` at `0x10048d78..0x10048d8e`.
* The pixel-format builder requires color depth greater than24 at
  `0x10043f18`. If that combined global is nonzero, `0x10043f32` marks
  the pixel format multisampled at `pf+0x74`. The normal WGL attribute
  getter reports eight samples for that format at `0x100470f6..0x10047100`.
* The real MxMD AA allocator `0xac7e0` has direct call sites `0x51ef2`,
  `0xa787d` and `0xb6a19`. Surface construction at the first site tests
  a request flag `0x20000000` or retained multisample flag, positive sample
  count `surface+0x270`, resource class2 at `[surface+0x23c]+4` and absent
  existing AA storage before invoking the allocator.

Therefore configuring both options and choosing a multisample 32-bit WGL
pixel format is a real retail-code route to investigate. Merely setting
`GL_MULTISAMPLE` on an ordinary single-sample pixel format is not that route.
The registry root and an exposed PowerDesk UI control have not been verified
in this round. No initialized normal-API allocation, enabled submission or
native M9 antialiased image has yet been established by this static chain.


### Dynamic normal-option proof (2026-10-06)

`tools/m9-vendor-faa-options.py` now executes the actual MSerX binaries,
verifying both SHA256s before running. Its CPU host is isolated in
`tools/faaemu/m9_options_host.py`. The committed compact result is
[the four-case matrix](traces/m9-vendor-faa-options-20261006.json); the full
output additionally records every pixel format and published sample value.

The complete raw configuration constructor `0xb8ea0` builds 97 descriptors;
the complete derived-owner constructor `0x5fa70` succeeds. Their actual
DWORD getters issue IOCTL `0x232104` with keys `3D.AAMode` and
`OGL.Multisample`. The fixture supplies bounded Windows registry responses,
including explicit missing-property errors that exercise real defaults.
The actual descriptor and derived-wrapper code, MD dispatch
`0xd3e70 -> 0xd4180`, and ICD query `0x10048d10` all execute. Neither the
query response nor ICD AA globals are supplied by a host callback.

| AAMode | OGL Multisample | Actual ICD FAA enable gate |
| --- | --- | --- |
| missing (default80000842) | missing (default0) | 0 |
| 80000842 | 1 | 0 |
| 80000843 | 0 | 0 |
| 80000843 | 1 | 1 |

The genuine ICD CRT attach `0x1072ada8`, device-manager constructor
`0x1002ae30`, complete ordinary pixel-format enumeration `0x10043900` and
WGL attribute construction `0x10046f20` also finish. However, this manager
has not acquired genuine adapter capabilities. Its hardware-format branch
`0x10043f18/0x10043f2a/0x10043f32` is never reached. Enumeration returns
39 baseline/fallback formats, all actually publishing WGL sample buffers0
and samples0 in all four cases. Three appended formats have `pf+0x74=1`
when OGL Multisample is enabled, while still publishing samples0. Therefore
that flag alone must not be treated as proof of multisample advertisement.

This closes dynamic option parsing and the real configuration query, not
normal enabled FAA pixel-format admission. Host services cover ordinary CPU
allocation/TLS/charset and a 1024x768, 32-bit GDI display. The MD query owner
and per-process default snapshot are controlled ABI fixtures; no initialized
GPU, created AA surface, submission, or retirement is represented. Genuine
ICD capability acquisition and the specific hardware-candidate rejection
must be traced next to distinguish an uninitialized fixture from an actual
M9128 capability restriction. Do not infer retail M9 FAA support merely from
the enabled ICD option gate, and do not infer permanent M9128 rejection from
this incomplete owner.


### Exact fallback rejection and first normal capability request

The option helper now records the actual capability source and rejection.
In each case `0x10044330` leaves the mask at builder stack `+0x0c` zero;
six hardware candidates execute `test cl,1` at `0x10043d8f` and branch
away at `0x10043d92`. They never reach the AA gate. The producer traverses
the ICD manager's display/capability collection `+0x1b8/+0x1dc`; the
constructor-only fixture has not populated that collection. This is
incomplete capability acquisition, not a demonstrated PCI0540 restriction.

Run `tools/m9-vendor-faa-options.py --capability-boundary` to continue the
actual normal acquisition function `0x1002d4b0` with a bounded CPU display
inventory matching Nano's M9128 (PCI102b:0540rev1). This supplies ordinary
EnumDisplayDevices/GDI monitor metadata, not hardware capabilities. The
actual code reaches its first adapter query and deliberately stops before
any reply. The committed request is
[the capability boundary capture](traces/m9-vendor-faa-capability-boundary-20261006.json):

* Escape `0x186a0` (100000), input32 bytes, output244 (`0xf4`) bytes.
* Input hex:
  `020000000100000000000000000000000000000000d9dadb0000000000000000`.
* `MxMD` ordinary `DrvEscape` at `0x3d7e0` routes this to `0x972e0`.
  Request operation2 dispatches to `0x966a0`. Its actual response producer
  reads derived configuration `hw+0xfe70`, the registered card-owner list
  `0x20d1cc`, per-process options and an actual miniport IOCTL `0x23231c`.
  Those owners must be initialized before returning a response.
* The actual card registration method `0x27230` prepends its argument
  through `device+0x69c0` to `0x20d1cc`. It is an entry in the ordinary
  paired module initialization table (`0x1f2638`), invoked by the startup
  loop `0x2e03a..0x2e056`; it is not a standalone proof of initialized
  adapter capabilities or a virtual-method table.

No hardware capability reply is fabricated. The first missing reply is now
an exact normal API request with an identified retail-code producer. Native
Linux Nano cannot directly supply a Windows vendor DrvEscape result; the
emulated display-device initialization and miniport must provide its genuine
ownership and query inputs. The current capture does not establish that
M9128 admits or permanently rejects normal FAA pixel formats.
