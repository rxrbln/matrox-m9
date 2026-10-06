# M9 FAA native testing, 2026-10-06

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0-only

Nano is confirmed M9128 PCI `102b:0540` rev1, with1GiB installed VRAM.
Ordinary textured rendering and the deployed driver are validated. FAA
rendering is not yet validated. This note is separate from P690
`102b:2539` evidence.

Latest integrated evidence7806a76d/613dcc84 is a joined real vendor CPU frame:
queue/context initialization, three FAA initialization clears, FAA bind,
public triangle launch, resolve, final completion packet and DMA post. Root's
independent replay passed1768 DMA writes/nine known spans and stopped before
the first GPU completion read at23bc3. No GPU reply or FAA image was supplied.
The remaining native work is the complete trusted state-buffer lifecycle,
including companion HZ, indirect saved state and resolve. Historical probes
below explain earlier gaps; the joined-frame section records the current trace.
Followup review found that the public triangle overwrites the manually bound
FAA companion/HZ with ordinary context state. Root independently confirmed
the B8/BA address changes. Thus the CPU replay PASS does not yet prove that
this draw participates in the FAA graph or issues its raster launch. Correct
context admission/target-state lowering and the count-prefixed secondary
consumer remain required before executing this stream on the GPU.

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
not prove any of them. The captured historical mode sweeps below did not
produce antialiasing.

The initial canonical `m9x` baseline `6780573` already had the diagnostic store
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

The missing contract includes the companion's optional HZ plane and its
`ba/bb` selector12 descriptor, sentinel ownership/save and restore, admission from
the initialized vendor context, resolve and destination retirement. The
ordinary implementation binds `ba/bb` to its ordinary auxiliary depth
resource; replacing only `b8/b9` is not proof of the vendor's complete AA
companion graph. The actual AA companion request uses format5; generic
linked-kind9 recursion requires requestformat9 and does not describe this
companion. BA/BB instead depends on companion+38c/+39c with selector12.
Static recipes should not be equated with a full FAA submission.

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

### Actual ordinary module configuration ownership

`tools/m9-vendor-config-owner.py` and reusable `faaemu/m9_config_owner.py`
execute the real ordinary module initializer `0x61f70`, rather than manually
attaching its two individual constructors. The initializer allocates,
refreshes and publishes raw configuration at `hw+0xfe74`, derived
configuration at `hw+0xfe70`, and per-device configuration at `device+0x5c`.
Its 72 real registry requests produce the expected default FAA842/0 or
explicitly configured843/1 values. Both runs pass.

`--profiles --register` additionally executes actual allocation-profile
publisher `0x4ed00` and actual card registrar `0x27230`. The former publishes
the same real profile owner at `hw+0x1015c` and `device+0x69a8`; the latter
publishes the device through the real registered-card list. This enabled
configuration run also passes. These are selected genuine module initializers
on controlled CPU ABI owners, not a completed hardware startup.

Configuration startup additionally requests miniport property IOCTL `0x23212c`
with selectors5,31,32,22 (input8/output8 bytes, second input DWORD0).
The original helper explicitly returns an unsupported error, exercising
vendor defaults; it accepts a separate query handler. These requests were
initially called feature queries, but actual miniport descriptor mapping below
proves they are registry properties. No hardware-format capability, MMIO
status, command completion or FAA admission is supplied by this helper.

`--process` additionally executes the real module initializer `0x64800`,
which calls `0xbb800` and the actual default-option decoder `0x64530`.
It publishes the default snapshot at `hw+0x10514 -> device+0x14` and
its configuration owner at `hw+0x10518 -> device+0x18`. This closes the
missing per-process owner used by query2 `0x966a0`, without a hand-built
snapshot. The real default AA token changes from1 with842 to2 with843.
Both default and enabled runs pass; the enabled run also includes profiles
and registration. See [the compact owner capture](traces/m9-vendor-config-owner-process-20261006.json).

The remaining shared owner at `device+0x10` is acquired by actual MD
`0x84890` through miniport IOCTL `0x233038` (input20/output20 bytes).
Its reply `+0x0c` supplies an owner declaring size `0x21f068`; MD publishes
it at `0x84928`. Genuine MM dispatch `0x71a7c -> 0x71d8e -> 0x61e82`
returns the miniport owner `+8` here. MM publisher `0x15551` obtains its
mask from actual type14, 0x208-byte configuration record via `0x26d9d` /
`0x28e5f`, copying record `+0x204` to shared owner `+0x21f040`. Query2
uses bit5 of that mask; the ICD itself has not reached a PCI0540 restriction.
The actual record is now decoded from the captured M9128 EEPROM and
published by0x15551, as described below; full query2 still needs its
remaining normal owners and allocations.

### Hardware-format mask includes actual surface creation

Static continuation of the exact rejection producer `0x10044330` shows
its low mask bits are not simply a hardware feature mask returned by query2.
For every actual display owner, bit0 requires an existing primary surface
at `+4`, or successful creation by `0x1003a5e0 -> 0x1000aac0`. That normal
method constructs request type9, changing it to `0x02000009` when the
genuine OGL Multisample option is enabled; the 32-bit format is0x15.
Bit1 similarly requires the depth surface at `+0x0c`, or creation via
`0x1003a6d0` (type0x0a / enabled `0x0200000a`). Bit2 requires the next
surface at `+0x1c`, or creation via `0x1003a900`. The producer clears all
three low bits if any required surface creation fails.

Therefore the full normal FAA admission proof must include genuine surface
allocation after adapter capability acquisition. Returning an enabled mask
or only filling the manager collection would skip this real admission test.
The current fixture has an empty collection and never attempts these
allocations, so its early rejection still proves no M9128 restriction.

`tools/m9-vendor-faa-options.py --surface-boundary` now dynamically executes
the actual primary caller `0x1003a5e0`, its real surface-ID allocator,
`0x1000aac0`, manager dispatch `0x10030c70 -> 0x10030bb0`, and actual
request virtual method `0x1003b270 -> 0x1000b8c0`. It supplies a controlled
display DC and 1024x768 desktop rectangle, without inserting an adapter into
the collection or supplying successful resource allocation. The real request
stops at ExtEscape186a0, input0x48/output0x10, operation0x10, before any
device response. The resource descriptor at input+0x18 contains
`[type,0,1,0x15,1024,768,0,0]`.

[The four-case dynamic capture](traces/m9-vendor-faa-primary-allocation-boundary-20261006.json)
shows type9 when OGL.Multisample=0 and type0x02000009 when it is1. This
allocation type follows the OGL option alone, even when AAMode's enable bit
is off; the later pixel-format enable still requires both options. No
hardware capability, allocation reply, WGL multisampled format or complete
draw is admitted by this controlled caller fixture. It proves the exact
normal resource-request envelope to join to MD's real allocation handler.

Actual MD normal operation0x10 dispatch is
`DrvEscape0x3d7e0 -> 0x972e0 -> 0x921c0 -> 0x7cc50`. The operation table
entry targets `0x97523`. Full dispatch still requires a genuine ICD client
owner (display `+0x70`, request `+8`) and initialized adapter admission;
the controlled display caller above does not supply that owner.

`tools/m9-vendor-faa-allocation-translation.py` feeds the exact ICD-produced
descriptor into real MD leaf `0x7cc50`, with profiles genuinely published
by `0x4ed00`. Actual `0xc4d90 -> 0xc51c0` translation reaches `0xc6220`
before allocation. [The dynamic off/on capture](traces/m9-vendor-faa-primary-md-translation-20261006.json)
shows allocation flags2 versus0x20000002, allocation type7, width1024,
height768 and profile index12 (class2, hardware format0x66, 32 bits).
The surface descriptor flags are0x44 versus0x400044. Thus the request AA
bit0x02000000 survives real conversion as allocator flag0x20000000;
it does not disappear in ordinary translation.

This is a controlled client-wrapper CPU leaf proof, not full DrvEscape or
resource admission. It stops before `0xc6220` and supplies no allocation
reply. Static continuation of the primary allocation first requires actual
device `+0x69bc`, its display/resource owner `+0x54`, and device `+0x69a4`
constructor owner `+0x1074`. These ordinary owner initializers remain to
be joined to genuine hardware startup before a complete FAA sequence claim.

### Exact normal display and client owner publishers

Device `+0x69bc` aliases `hardware+0xfebc+head_index*0x90`, published
by genuine MD `0x84890` after IOCTL233038 and also at `0x2dfea` during
ordinary device initialization. Ordinary module `0x7abc0`, present in the
real startup table at `0x1f2628`, allocates0xf8, executes real `0x7aca0`
and publishes the display/resource owner at head `+0x54` (`0x7ac2a`).
Its initial surface pointers are genuinely zero until real allocation.

Device `+0x69a4` is published by ordinary module `0x1f990`, startup entry
`0x1f2600`. It allocates0x8368 and executes constructor `0x1cb90` before
publishing the completed owner. Exploration with the actual configured
CPU owner originally reached `0x1e1f0 -> 0x12340` and stopped because the
shared miniport owner at device `+0x10` was absent. That reader needs the
current configuration index at shared `+0x21f038`, its record type and
the runtime collection pointer at `+0x21f05c`. The later normal-runtime capture below closes this prerequisite with the
genuine initialized CPU runtime and completes the resource constructor.
Complete device initialization and FAA surface allocation remain separate.

The primary factory at resource owner `+0x1074` now has an exact producer.
It is `owner+0xfe4+0x90`: constructor `0x1cb90` installs the typed factory
block through actual `0x5f550`, then its successful tail `0x1de20` calls
`0x5f740`. The latter publishes resident pointers and passes them through
real typed virtual getters. With an empty engine inventory, the actual
result is owner `+0x1034`, vtable `0x1f40c0`, slot0 `0xb78f0`; its delegate
is owner `+0x1058`, vtable `0x1f3310`, slot0 `0xb6850`.
`tools/m9-vendor-resource-factory.py` executes those genuine CPU leaves and
asserts both factory identities, the constructor/getter visits, and that
device `+0x69a4` remains unpublished. The
[factory capture](traces/m9-vendor-resource-factory-20261006.json) is a
conditional ownership proof; it does not complete or bypass the earlier
shared-runtime requirement in the full resource constructor.

The other primary prerequisite, device `+0x6990`, also has a normal startup
publisher: module `0x273a0` at startup-table `0x1f25a0` calls `0x273c0`,
which assigns it at `0x273ff`. Before that assignment, actual `0x12340`
reads the same genuine device `+0x10` shared runtime owner. Thus both
resource branches depend on initialized runtime configuration;
neither requires an invented factory pointer or an extra FAA registry gate.

Actual M9128 bootstrap now retains the genuine decoded EEPROM record and
miniport object through `tools/m9emu/m9128_bootstrap.py`. On that exact
object, `tools/m9-vendor-runtime-owner.py` executes real MM `0x15551`,
which copies record fields `+0x204/+0x1ac/+0x1b0` into shared
`+0x21f040/+0x21f044/+0x21f048`: the actual values are2/0x1b/1.
Real `0x5de58` allocates the ordinary0x1ee400 CPU runtime owner and
`0x5fa80` publishes its collection at shared `+0x21f05c`. The
[runtime ownership capture](traces/m9-vendor-runtime-owner-20261006.json)
asserts exact copied fields and the genuine resident pointer, with no
manufactured capabilities or populated configuration profile.

Full ordinary runtime initialization is `0x568e7`. The early bounded run
stopped at its host-chipset PCI inventory request. The later complete
same-boot Nano inventory and bounded NT CPU contracts now let this actual
initializer return1, as shown below. The uncaptured transmitter-reset GPIO
in complete FindAdapter startup remains a separate hardware boundary.

The genuine ICD client publisher is `0x1003a2a0`. Actual
`0x1003d480 -> 0x1003b270 -> 0x1003d510` sends operation4, ExtEscape186a0
input0x38/output0x10; only a successful actual reply would publish client
handles into display `+0x70/+0x74` at `0x1003a39b`. The normal MD operation
table selects `0x97461 -> 0x960b0`; that handler uses head `+0x78` and
real constructors `0x873e0 -> 0x87590`, returning the actual client pointer
at output+8. These handles are the dependency used by later operation0x10.

`tools/m9-vendor-faa-options.py --client-boundary` now executes the real
ICD caller after genuine enabled option decoding and captures
[the exact operation4 envelope](traces/m9-vendor-display-client-boundary-20261006.json).
The fixture provides a controlled display DC and ordinary CPU process
pseudo-handle; it stops before any device reply or returned handles, and
does not admit a capability collection. Reserved/high-half stack bytes in
the request remain unspecified and must not be interpreted as device flags.

Both client and primary boundary fixtures now construct the display owner
with actual `0x10039d10`, using the vendor's0x218 allocation size, rather
than treating a zeroed wrapper as its constructor. The ordinary manager
`0x1002d010` allocates it and calls that exact constructor at `0x1002d1c3`;
subsequent `0x1003a180` metadata initialization and collection insertion
require the genuine query2 reply retained in manager `+0x220`. These
fixtures deliberately stop short of that hardware admission: only the DC
and desktop rectangle are controlled CPU caller inputs. All three boundary
captures pass with the real display constructor; no successful device reply
or capability flags are added.

### Real miniport property dispatch closes the four configuration queries

`--miniport-defaults` executes actual MD `0x61f70` and forwards its four
IOCTL `0x23212c` requests into a separate genuine MM emulator. Actual MM
`0x6ca3e` initializes each property from the real descriptor table
`0x55450`; real normal dispatch `0x70eab -> 0x71428 -> 0x5a228 -> 0x6cbbd`
performs registry access. The host explicitly reports missing Windows registry
keys; it supplies no values. Actual MM callback `0x7279d` and default logic
produce these replies, which are copied verbatim back to MD:

| Selector | Real property | Actual default DWORD | Reply second DWORD |
| --- | --- | --- | --- |
| 5 | System.BusMastering | ffffffff | 1 |
| 31 | User.LargeCursor | 0 | 1 |
| 32 | User.ForceHardwareColorCursor | 1 | 1 |
| 22 | System.MmEqualUnit | 0 | 1 |

Both FAA option cases complete the ordinary MD configuration/process owner
initializers; the enabled case also completes profile publication and card
registration. [The dynamic capture](traces/m9-vendor-miniport-property-defaults-20261006.json)
records actual default replies and actual registry keys. These four replies
are now obtained from vendor code rather than replacing them with unsupported
errors. The controlled miniport CPU owners are still not hardware initialized.

Static MM dispatch also resolves MD query2's IOCTL `0x23231c`, selector1:
`0x5d927 -> 0x5dc7b -> 0x5e0eb` reads runtime profile property0x19 and
returns owner `+0x48`, updated when that property's `+0x14` value is4.
This is a runtime policy state, not a direct GPU capability register. The
normal-runtime initialization below now populates its actual collection; this
query still needs a full dynamic join before an admission claim.

The normal option-matrix helper now uses these actual module publications
too: `0x61f70` and `0x64800`, with genuine miniport property dispatch, supply
the configuration queried by actual MD `0xd3e70 -> 0xd4180` and ICD
`0x10048d10`. Its earlier manual raw/derived attachment and zeroed process
snapshot have been removed. [The updated four-case capture](traces/m9-vendor-faa-options-owned-20261006.json)
reproduces enable gates0/0/0/1, 39 fallback formats, samples0 and the same
empty-collection rejection. The default process policy fields in the MD
response now come from the real decoder as well. The active query-owner
wrapper remains a controlled ABI input, and full hardware startup, display
collection population and real surface allocation remain unproved. The
capability-boundary mode still stops at the identical genuine query2 request.

### Exact serial source of the miniport capability record

The sole direct capability-record initializer call is MM `0x2e1c0` in
`0x2e0c5`. It creates a 128-byte type13 source and 0x208-byte type14 result,
copies the PCI device word into result `+4`, then obtains serial source
kind2 through `0x2f70c -> 0x2f9d4`. `0x2f682` reads offsets0..127 and counts
successful reads. `0x31f90` receives that real source and count, accepting
its extended profile only for at least128 bytes and header `0x000825d3`.
The accepted source supplies chip enumeration at `+0x0a` and retained flags;
the actual `0x3265f` fills result `+8/+0x1ac/+0x1b0/+0x204` by chip branch.
These producers now execute with the actual128-byte GPIO EEPROM capture.
The genuine decoded record has header3, chipWORD34, +1ac=1b, +1b0=1,
+204=2. Earlier pre-EEPROM snapshots contained only header/PCI and were
not negative hardware-capability evidence.

An unsuccessful or absent extended source has an actual vendor fallback
for PCI0540 to enumeration0x29. This is a conditional default branch, not
the observed M9128 result: its actual extended source was captured and
accepted. Do not
replace its hardware transaction with a synthetic absent profile. The only
`d3250800` occurrence in the captured M9128 ROM, offset0xb057, is the
immediate operand of a BIOS compare instruction, not a captured profile blob.


### Pool6 backing boundary and corrected companion recipe

The new [bounded resource proof](parhelia-m9-faa-companion-resource-20261006.md)
executes the actual pool6 request through genuine low-level geometry creation,
ending before its absent shared backing owner. It establishes a linear320x1,
1280-byte resource with no admitted physical address, not a successful pool6
allocation. It also corrects the older linked-kind9 companion claim: the actual
AA profile49 is class4/hardware3d/64-bit and its request-format5 refuses that
recursion. BA/BB names optional HZ through selector12.

Conditional real layout/descriptor builders produce B9=7a220140 and extent96020
for320x240/sample2, and B9=7a220400/extent600020 for1024x768. The existing native
diagnostic forces layout1 and reserves width*height*8 without those end guards;
odd/small dimensions also require the vendor's alignment arithmetic. Actual
sample2 admission, physical backing/content and complete command lifecycle
remain necessary before promoting this recipe to a native enabled FAA result.
### Normal miniport runtime initialization completes with actual PCI inputs

The [normal-runtime capture](traces/m9-vendor-runtime-normal-20261006.json)
executes actual `0x568e7` to return1, after real capability publication
`0x15551`. Its inputs retain the actual M9128 EEPROM-derived record and the
complete same-boot Nano PCI device and assigned-bus catalogs. The PCI host
returns captured configuration bytes, explicit absent device vendor IDs, or
absent-bus status; it stops on uncaptured bytes. It supplies no PCI writes.
Actual vendor code issues8250 PCI reads and1190 registry requests. The initially
empty CPU registry retains only the driver's own generated12-byte D00 value.

The resulting collection at202672b4 is published from owner202672b0. Runtime
type/index0 are actual initializer outputs. The sibling's bounded OS MDL host
provides synthetic system-RAM pages and reports the optional Windows Extio
manager absent; those pages are not GPU or VRAM addresses. This establishes
normal CPU runtime construction, including the missing configuration records
used by MD resource constructors. Complete adapter initialization, surface
allocation, draw and FAA resolve remain unproved.

Reproduce with `tools/m9-vendor-runtime-owner.py --normal --pci-inventory
PATH --bootstrap-tools PATH`, using the captured actual PCI inventory and
`tools/m9emu` bootstrap/NT-host dependencies (434e1ed9 or its integration).

### Real MD resource and engine constructors now admit the retained runtime

The [MD runtime-owner capture](traces/m9-vendor-md-runtime-owners-20261006.json)
retains the real MM heap addresses and forwards the MD configuration's four
property requests through that initialized miniport's actual0x70eab dispatch.
The initial MD device is a controlled CPU caller ABI object. Actual MM
0x71d8e handles the normal233038 request, and actual MD0x84890 publishes
its hardware/platform/shared pointers. The platform is the retained real MM
profile (OS500, BAR0 d0000000/10000000). Its256MiB aperture is separate
from installed1GiB VRAM. No startup success, GPU status or caps are supplied.

With that real shared runtime, actual MD0x273a0 returns1 and publishes
resident engine `device+6990`. Actual0x1f990 executes the complete0x1cb90
resource constructor and publishes `device+69a4`, including the real typed
primary factory at owner+1074: wrapper0xb78f0 delegates to0xb6850. This
supersedes the earlier runtime-null stops for these constructors. The
resource module's later platform policy call0x1fca0→0x62170 now completes
with that genuine platform owner. Actual normal policy-owner module0x849a0
constructs and publishes hardware+35fc; it is separate from the memory
manager at+35f8. Executing0x849a0 before the other resource modules lets
all four complete with return1:0x849a0,0x273a0,0x7abc0,0x1f990. Actual
0x7abc0 publishes head+54; full0x1f990 publishes its actual typed factory.
These selected ordinary modules do not complete all adapter startup.


Actual derived config+338 virtual getter returns101f8; actual admission
helper0x477f0 for hardwareformat3d returns1, confirming the companion's
required config masks and format-table flag from vendor code. This admission
is not allocation success. The AA surface constructor, VRAM backing, full
normal pixel-format admission, render and resolve remain to be joined.

### Main and auxiliary stores have exact linear rows and a32-byte guard

`tools/m9-vendor-faa-store-requests.py` executes actual0xac7e0 with a real
vendor-constructed CPU surface and controlled caller geometry, using the
retained normal MM runtime and actual published MD profiles. The
[request capture](traces/m9-vendor-faa-store-requests-20261006.json) stops
before its first0x51870 allocation. Main request flags8002, format8 and
profile12 (class2/hardware66/32bits) are actual constructor outputs.

The main budget is `min(floor(width*height*344*0.38499999046325684)>>3,
32MiB)`. Actual request width is1024; rows are
`max(1,floor((budget+1023)/4096))`. The320x242 case has budget1282019,
remainder4067, and requests313 rows: simply flooring budget/4096 would
incorrectly give312. Tiny1x1 and2x2 requests both preserve one row.

The [layout capture](traces/m9-vendor-faa-store-layout-20261006.json)
executes the real0x19560 scalar prefix against the captured M9128 cleanroom
postinit f04=0448803e, then actual0xac7e0→0x51990→0x45eb0 until layout
completes, before physical backing. Real manager+24/+28 become2/1. This
is an explicit CPU MMIO model of the actual cleanroom baseline, not a
claim about vendor cold state or completed memory-manager initialization.
The resulting store is linear (layoutflag1), with exact requested rows and
extent `1024*rows*4+32`. At1024x768 this is13017120 bytes, and the
32MiB budget cap still requires33554464 bytes including the guard.

Auxiliary static producer0xac8ec..0xac94d uses the same profile/width,
with budget `main_budget>>4`, or fixed2MiB when device+1c bit0 is set.
The capture executes its genuine0x45eb0 layout separately with those
conditional inputs, without fabricating a successful main allocation to
reach the later constructor. At1024x768 the ordinary auxiliary layout is
1024x198/811040 bytes; the fixed branch is1024x512/2097184 bytes.
Both include the32-byte guard. These are conditional builder proofs;
full auxiliary allocation, contents and lifetime remain unproved.

The independent CPU request fixtures explicitly set x87 control word027f:
53-bit precision, round to nearest, exceptions masked. The vendor then saves
and temporarily changes its rounding mode for conversion. This caller FP ABI
is controlled and recorded, not recovered hardware state. Leaving Unicorn's
zero control word active introduced24-bit precision carry between interrupted
queries; it changed some byte budgets by one while preserving these row counts.
Microsoft documents the distinct x86 precision controls in
[the CRT control-word API](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/control87-controlfp-control87-2?view=msvc-170).
The corrected six-case captures are nonempty parsed JSON, including tiny cases,
the rounding boundary and both conditional auxiliary branches. The earlier
empty capture checkpoint is superseded.

Native source7443ff6 now corrects main/ordinary auxiliary extents together,
with source regression d8819db retaining guards after BO page rounding.
Actual-C six-case vendor comparison, ownership ASAN/UBSAN, full batch suite
and DSO build pass. Root independently reran the genuine vendor layout
producer with retained current-board PCI inputs. This source now passes current-M9128 same-context guarded BO growth with
AA raster mode off, then ordinary texture; see
[resource proof](../tests/m9-faa-guards-native-20261006/README.md).
Backing/init/completeFAA command state remain open.

### Two E6 operations and the cached sentinel address are distinct

The fb920 binding operation's actual raw E6 GPU payload is
`[6dc00000,1,1]`; the complete context restore path's actual raw payload is
`[6dc00000,42000000,a1]`. Both have passed the real25680/24870 encoders in
the integrated resource-address/context-fragment-state captures. Their
trailing values are distinct GPU words, not interchangeable recorder metadata.
The shared first opcode does not justify substituting one complete operation
for the other.

Actual AA binder0xaa750 obtains the control resource passed at binder
array+0x0c through its kind5 getter and places that address
at context shadow+74/+78 (aa819..aa82d). It obtains the independent per-head
pool6 resource at hardware+fed8+head*144 and places that address at shadow
+84/+88 (aad07..aad68), also duplicating it into AA state+9c/+a0 for fb920.
The real restore emitter0xfcb60 later copies the current sentinel74/78 into
cached7c/80 (fce05..fce39). The save emitter0xfce50 passes cached7c/80 and
pool6 84/88 into0xfc8f0. These are separate resource references; the store,
sentinel and per-head pool6 must preserve their own ownership and lifetime.
This resolves the static shadow producer split. The caller-array ownership
join to the sentinel allocated by0xac7e0 at its interface+18 still requires
the complete resource graph; no pointer alias is assumed. Full allocated
resource admission remains unproved.

### Actual caller reference publication closes the sentinel array position

`tools/m9-vendor-faa-binder-graph.py` executes real MxMD0xa7200 with explicitly
controlled CPU wrapper/reference inputs. The actual code returns1 and copies
primary, companion, main, sentinel into both participating AAouter arrays at
+8,+c,+10,+14, then marks+18 active. At0xa72d2/+0xa72ed it obtains main and
sentinel from the source AAouter+24/+34: these are exactly the owning
ac7e0 interface+8/+18 because that interface begins at outer+1c. This closes
the caller-array position left conditional above: actualaa750(outer+8)
receives the main resource at array+8 and the sentinel at array+c.

The [bounded reference-copy capture](traces/m9-vendor-faa-binder-graph-20261006.json)
is CPU ABI evidence. Its wrapper pointers and source ownership flag are supplied
caller inputs; it supplies no successful GPU allocator outputs and proves no
normal display admission, backing allocation, or rendered result. The separate
per-head pool6 binding at context84/88 remains a different resource.

### Genuine optional HZ request and initialization boundary

`tools/m9-vendor-faa-hz-request.py` executes real0x54be0 against the actual
configured/runtime graph and controlled companion geometry/profile fields.
The genuine configuration334 getter returns7, admitting bit4 for the 64-bit
companion. The [request capture](traces/m9-vendor-faa-hz-requests-20261006.json)
shows flags2, logical format0xb, profile56(class5/hardwareformat0x78/32bits),
width ceil(companionwidth/8), height ceil(companionheight/4), units/depth1.
320x240 produces40x60;1024x768 produces128x192. No backing reply is supplied.

The ordinary constructor invokes0x54be0 at0x51d68 when profileclass4 and
resourceformat5 match, setting companion+39c first. If the helper fails, actual
0x51d74 clears that flag and continues; this is a vendor optional-resource
fallback, not proof of the present native renderer. On success0x54cd3 invokes
0x55070, which calls0x54e80 then0xb4fe0 to issue a clear using depth0..1 and
mask4. Capturing that real initialization command stream still requires the
returned shared allocator and resource backing; the request proof does not
claim HZ contents, completion or FAA rendering.

### Returned allocator and genuine allocated FAA resources

`tools/m9-vendor-faa-owned-resources.py` uses the sibling reusable
`m9_allocator_fixture.initialize`: real shared allocator0x19420 returns1,
publishing device6984/6988/698c. The actual OS-owned aperture mapping is an
explicit CPU caller input; its normal85d50 refresh still has not returned.
No GPU completion is supplied. Actualac7e0 then allocates the main, auxiliary,
companion and HZ resources before stopping at the first real HZ GPU-clear
0xb4fe0. The [owned-resource capture](traces/m9-vendor-faa-owned-resources-20261006.json)
contains the real objects, backing descriptors, input/return layout stages,
and separate sentinel/pool6 constructors and lifetime checks.

At320x240 the stages are distinct:

| Resource | Actual45eb0 raw return/flags | Stored aligned extent/allocated flags |
|---|---:|---:|
| Main1024x310 |136020/1|136080/3|
| Auxiliary1024x19 |13020/1|13080/3|
| Companion320x240/sample2 |96020/9|97000/b|
| HZ40x60 |2d00/1|3000/3|

Values are hexadecimal. The earlier32-byte geometry guard remains correct:
actual45eb0 returns the raw protected size in EAX, then at464ca..464d9
final-aligns the stored descriptor extent+180 using alignment+174 (128 bytes
for these linear resources,4096 for tiled resources). These stored extents
already exist before backing. Later4dfe0 publishes address15c and marks
allocated layoutbit2. A native allocation rounded to full pages already covers
this padding. Distinguish the raw function return, stored aligned extent and
subsequent allocated-address publication.

An independent actuala3870 sentinel request uses the same flags8202/profile56/
format0/5x1 asac7e0. The genuineac7e0 CPU initialization prefixaca7b..acb06
maps the real allocated wrapper, writes exactly
`a4a3a1a1 28041968 00000000 00000000 00000000`, and unmaps. Its actual backing
extent is80hex with row8. This independent prefix does not return a fake
successful clear to the interruptedac7e0 frame.

Real751f0 constructs the pool6 CPU owner; explicitly controlled owner fields
select the actual hardware/device/head0. Actual76540 allocates the0x500-byte
request with extent580hex, then reuses the identical interface on the second
call. Wrapper reference count increases2→3 (constructor baseline1). The real
kind5 getter returns a direct address pair for pool6, while the flags8202
sentinel returns tag40000000 and a pointer to its real wrapper relocation
record: actuala6740 selects this record when wrapper+a8 is set. These are
separate binding forms and separate resources.

This checkpoint proves actual vendor allocation outputs and sentinel CPU
initialization on modeled OS-owned storage. Pool6 semantic initialization,
HZ/companion GPU clears, enabled FAA draw/resolve and normal display admission
remain open. No rendered FAA fragment is claimed.

### Owned addresses translated by actual command emitters

The owned-resource runner now also executes actualfb920/fce50 and the real
stream relocation routines25680/24870 using the allocated references above.
Stream storage and its reference-recording callback are controlled CPU caller
ABI inputs; the callback supplies no resource allocation or GPU completion.
`tools/check-m9-faa-owned-commands.py` verifies the backing provenance and
encoded address offsets against the generated capture.

Actualfb920 binds pool6 at index24 using kind2b, and main/auxiliary at BC/BD
using kinds9/a. All three payloads preserve the allocated GPU address's low
bit1 tag. It emits E6 payload `6dc00000 00000001 00000001`.

Actualfce50 uses distinct cached references: sentinel at shadow7c/80, pool6 at
84/88. Pool6 restoration produces kind2c/2f payloads `(pool6+284)|1` and
`(pool6+2c4)|1`. The A1/A3/A4 fragment-pointer save uses kind30 and sentinel
offsets8/c/10. The sentinel's input pairs contain tag40000000 plus the offset,
and point to the real wrapper+74 relocation record. Actual25680 reads that
record's cached encoded GPU address and produces `(sentinel+offset)|1`.
The CPU wrapper/backing addresses therefore do not enter the GPU stream.
This context path's E6 payload is `6dc00000 42000000 000000a1`, followed by
the final save E6 `40008000`; it differs from the fb920 binding operation.

The sentinel's second initialization word28041968 is a literal immediate:
pinned instruction0xacae1 bytes `c7 40 04 68 19 04 28` encode
`mov [eax+4],0x28041968`. It is reused by the reset path at0xacb9b. Its
numerical overlap with this model's heap range does not make it an owner
pointer or an address relocation; the native initialization should retain
the literal value. This proof closes command address translation and the
sentinel literal provenance, while HZ clear construction and rendered FAA
remain separate outstanding requirements.

### First-use sentinel restore precedes cached state save

The same owned-resource capture now executes actualfbfc0 fragment construction,
then fce50 before any restore, actualfcb60 with the allocated current sentinel,
and finally fce50 using the cache published by that real restore. Context
construction initializes current74/78, cached7c/80 and pool684/88 to null.
The fixture supplies the
real allocated pool6 pair as an explicit caller binding.

Before the first restore, fce50 emits zero bytes despite its dirtybit8 being
requested: actualfc8f0 rejects a null cached sentinel. First fcb60 uses the
current sentinel and emits20 actual raw words. Its real common-counter fence
packet begins `4545e3e6 1 21` in this capture. The remainder contains E6
`42000000`, eight literal28041968 index44 writes, E6 `3 3`, then index2e/2f
references using kind2c/2e. These encode `sentinel|1` and `sentinel+14`:
kind2e clears the low two address bits, unlike the tagged kind2c path.
No pool6 address appears in this first sentinel-restore command stream.

Only after these commands does actualfcb60 copy current74/78 into cached7c/80
atfce05..fce39. Subsequentfce50 emits the29-word pool6/context save plus
A1/A3/A4 sentinel save sequence already proved above. Thus the previous
capture's preseeded cached sentinel was a command-encoding fixture, not a
first-use policy. Normal context switching atf8fd0 saves the old owner only
if one exists, then restores a new owner's sentinel when dirtybit4 is set;
thefc342 dispatcher similarly processes savebit8 before restorebit4.

This checkpoint executes actual CPU constructors, counter/reference handling
and command production. It supplies no GPU completion or fabricated counter
readback. The commands have not been executed by hardware here, so pool6 GPU
state acquisition semantics and complete enabled draw/resolve remain open.

Constructor attribution correction: the initial first-use fixture called
fce90, the large shader-state constructor. The reproducible runner and capture
now use the actual fragment-control constructorfbfc0, whose ordinary callers
are1edf6/1f226/1f650. This also establishes a null initial pool6 pair, rather
than interpreting unrelated shader default words at84/88 as resource state.
The actualfcb60/fce50 command encoding and cached-sentinel behavior are unchanged.

### Real allocated AA binder joins surface and fragment controls

With `--binder`, the owned-resource runner executes ordinary1a260/1ff90
queue/context publishers and realaa750 with the actual allocated FAA graph.
A separate genuinea3870 request creates the primary resource using explicit
profile12/format0/geometry caller inputs. The binder stops at0xaadcc after
publishing CPU shadows and before queue activation; it neither resumes the
interrupted HZ clear nor supplies successful GPU results.

The actual main+414 and companion+38c links identify the allocated auxiliary
and HZ objects. Aa750 copies the actual sentinel to current74/78, leaves cached
7c/80 null, and copies actual pool6 to84/88. This independently joins the
first-use policy above to genuine binder output, rather than pre-seeding a
cached resource. The runner then executes actualfb920 andfc0e0 with explicit
stream storage and a reference-recording CPU callback only.

At320x240 actualfb920 produces41 raw GPU words: a genuine counter packet,
primary B0/B1, companion B8/B9 (`address|1`, `7a220140`), HZ BA/BB
(`address|1`, `f0010140`), index24/E6/BCBD, and remaining surface state.
The four AD writes hold `9b04`, the actual main stored extent136080 divided
by32, while the AF write is0 in this capture. These are capacity/surface
fields. They must not be conflated with fragment-control shadow20/28.

Actualfc0e0 consumes those separate fragment fields and emits
`45d7eff0 e0000400 20007000 00000000`: the controls target **F0/EF**, with
D7 last. Calling the shadow28/20 values AD/AF would name the wrong registers.
The independent [1024x768 capture](traces/m9-vendor-faa-owned-binder-1024x768-20261006.json)
and the320x240 owned-resource capture both pass the address/provenance checker.
The emitted queued counter values are model-session outputs, not native
constants or fabricated completed counters.

This establishes the allocated CPU binder and actual packet mapping. Queue
activation, HZ initialization execution, kernel-private indirect state
ownership, enabled draw/resolve and hardware FAA rendering remain open.

### Owned FAA output publisher and ordinary compiler order (2026-10-06)

The actual ordinary copy/map path reaches `a74b0→ac1f0` at `a7528`.
When the AA outer owner has companion/main objects, `ac1f0` allocates a
temporary profile14/format6 destination and calls `abc20` at `ac2d4`, then
uses ordinary `afbe0` to copy that output to the requested destination. Its
allocation-failure path calls the same publisher for 64×64 tiles. This is
the concrete caller provenance for the FAA output/resolve candidate;
`abc20` is not an otherwise unreachable emitter.

[Owned resolve capture](traces/m9-vendor-faa-owned-resolve-20261006.json)
executes real `abc20(destination,companion,main,x0,y0,x1,y1)` on returned
vendor allocator objects. It stops at `ac0ac`, after actual shadow publication
and before actual queue dispatch at `ac0e3`. The real main→auxiliary and
companion→HZ links are required. Mode `AAshadow+14` changes from draw `1f`
to output `1d`, `+20=ffff`, and main/auxiliary/pool6 address pairs remain
the genuine owned tagged addresses. Fragment `+28=e0000400` and
`+20=20007000` still target **F0/EF**, and the requested rectangle publishes
fragment `+30=origin`, `+34=width|(height<<16)`.

`tools/faaemu/m9_owned_faa_resolve.py` retains that bounded default and can
execute the same actual call without its boundary once the genuine queue is
available. It never supplies a successful dispatch or GPU completion. The
post-dispatch backing-lifetime updates (`ac101..ac131`) and fragment save bit
(`ac139`) have not executed in this capture. This capture does not prove a
completed FAA resolve, normal pixel-format admission or hardware rendering.

The actual ordinary state compiler `fadf0` emits **`fc0e0` before `fb920`**
(`fae02`, `fae0e`), followed by `f4f50`, `f6f20`, `fb260`. Earlier independent
emitter fixtures intentionally captured each encoder separately; their JSON
field order is not the normal submission order. The complete-frame trace
must run the genuine queue/compiler and preserve this order, including the
first-use sentinel restore before the pool6 producer.

Correction to preliminary caller investigation: `a7390` calls interface+8
virtual+6c, whose actual constructor-published vtable is `1f3c50`; slot
`1f3cbc` contains **`a7440`**, the AA binder refresh. `a7390` then calls
`ac150` to clear AA ownership flags. Neither that flag clear nor this
virtual call is the `ac1f0→abc20` output path.

The pinned ordinary compiler switch provides the remaining trigger location:
`abc20` publishes context `+10=9`; real `f85c0` dispatches modes7..10 through
table `f8fac`, whose mode9 entry is `f8e36`. That branch calls `fadf0`, then
emits `45fcf3ec,0,fragment+30,fragment+34` at `f8e94..f8ed7` before the usual
pending-resource bookkeeping. This branch and write are **static pinned image
provenance**, not executed compiler evidence in the bounded output capture.
Capturing FAA bindings alone omits this rectangle operation; the full-frame
queue trace must also reach the actual trigger and retirement boundary.

## Normal D3D option and CPU frontend prerequisites (2026-10-06)

The ordinary raw descriptor at `b9557` names `DirectX.D3DEnable`; default0
leaves the `78480→77130` callback advertisement disabled. Supplying the real
registry option1 during genuine `61f70` enables callback flags0x0c and the
actual `DrawPrimitives2=beeb0` entry. Enabling that option also causes a real
`23231c`, selector1 runtime-policy query during configuration. Ordinary D3D
module setup subsequently queries selector2. Both execute actual MM
`5d927→5dc7b→5e0eb` on the retained miniport owner and return0, four bytes,
value0 in the current actual runtime. These queries belong to `5d927`, **not**
the registry/property dispatcher `70eab`; no capability reply is substituted.
This is normal option/callback reachability, not a complete FAA frame or GPU
rendering claim.

The real `bec70` context callback uses the registry owners published by
ordinary module `79ed0` (`head+20/+24`). Its `beb40` call creates typed
resource collections through genuine `7a170/c04e0`, then passes them to
`753e0`. The returned queue TaskMgr permits actual `11b9b0` vertex/backend
construction. That construction calls `7b3e0`, whose genuine CPU allocator
ABI is vtable+8 `(size,flags,tag)`; bounded host support now retains those CPU
allocations (observed flags2, tag0x58544d44). The queue itself previously
needed vtable+10 `(size,file,line)` from actual `f829e`.

An initial `f5969` failure in both clear and context construction was a fixture
ordering defect: shader setup `1ff90→1e490→f5d60` consumes `queue+30`.
The genuine required order is `1a260→230f0(return1)→1ff90`; constructing
shader state before activation passed a null TaskMgr. The corrected full
clear/draw/output capture is still the integration gate. No null shader
owner, successful vendor return or GPU retirement is supplied by hand.

### Joined minimal FAA frame, 2026-10-06

`tools/m9-vendor-faa-frame.py` now executes the returned queue and public D3D
context, all three real `ac7e0` initialization clears, `aa750` FAA binding,
public `beeb0` DP2 XYZRHW/diffuse triangle lowering through `17e480`, and
`abc20` output through dispatch and `ac139` lifetime bookkeeping. Actual
`fb920` observes FAA mode `0x1f` for the triangle and `0x1d` for output. The
triangle emits `0x23000001`; the output compiler emits `0x45fcf3ec`.
`docs/traces/m9-vendor-faa-frame-20261006.json` records the joined frame.
`tools/check-m9-faa-frame.py` verifies successful public context/draw callers,
ordered compiler modes, resource links, typed relocations, and exact CPU-written
DMA images against all 32 live OS buffer bounds. Capture starts before queue
construction, includes context initialization, and exports only written bytes.
Every exported byte must equal the final actual CPU write for that stage;
untouched emulator memory is never evidence of an initialized GPU program.

Pool6's actual kind-5 address pair is `{offset=0, tagged_base=GPU_address|1}`,
cached at fragment `+0x84/+0x88`. The real getter and all actual kind-`0x2b`
translations agree. The earlier interpretation `+0x88/+0x8c` was a shifted
scalar read, not an address pair. Kind-`0x2c`/`0x2f` save-state endpoints use
pool6 offsets `0x284`/`0x2c4`; kind-`0x30` writes sentinel slots `8/c/10`.
Sentinel's second initial word `0x28041968` is a pinned literal instruction,
not a CPU heap pointer. Its actual CPU initializer is `[a4a3a1a1,28041968,0,0,0]`.

This proves CPU command production with genuine returned resource/queue owners
and controlled Windows CPU API inputs. It does not prove normal display mapper
admission, GPU execution, retirement, antialiasing, or a Linux screenshot.
Opaque indirect saved-state backing must remain kernel-owned and cannot be
admitted as client-written VRAM. Pool6 initial GPU-written state and additional
shader/constant resource write provenance remain explicit translation work.
The sibling retirement trace executes the same returned frame through the final
counter/flush and stops before the first GPU status read; it supplies no completion.

### Surface admission and triangle coherence correction

The preceding joined CPU-caller trace is **not a coherent FAA rendering frame**.
Although the shared AA shadow keeps `0x1f`, the actual public triangle compiler
replaces the previously bound FAA companion/HZ with ordinary depth state and
selects default MRT outputs. It emits `AD=20`, `AF=0`, `F0=f0000400`,
`EF=00288800`, and a direct `23000001` launch. Those writes cannot establish
an antialiased final color draw. Do not replay that image as an FAA hardware test.

The missing normal graph publication is now dynamically recovered separately:
`tools/m9-vendor-faa-surface-activation.py` executes real `a77c0` on a genuine
sampled color wrapper (`01000002`, profile12/class2/hfmt66/32-bit). It returns1,
sets surface flags `+428 bit8`, `+270=1`, and owner `+420/+3c=1`; real `a787d`
calls `ac7e0` with the graph destination **surface-owner+1c**, and real `a78cc`
executes backing registry insertion. No graph field is hand-published.
`docs/traces/m9-vendor-faa-surface-activation-20261006.json` retains all five
returned resources and exact caller/owner addresses. Primary profile12 is a
valid class2 color profile; its previous use was not by itself the admission bug.

The activated wrapper still has public resource handle `+bc=0`, and public
`753e0` consequently cannot select it through initial `2d740` target binding.
The normal `7a6e0→a3780→885e0` surface association/handle publisher is the next
join. Actual DP2 render-state and viewport requests are supported as explicit
caller inputs in the draw fixture; they do not replace normal resource owners.
A direct GPU-address lookup in BAR0 validity is not sufficient to classify
pool6/sentinel initialization: the GPU virtual address must first be joined to
its actual CPU mapping/physical aperture domain. No empty validity mask is
currently admitted as evidence that the resources are uninitialized.
