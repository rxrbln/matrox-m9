# M9128 fullscreen async presentation admission

The actual fullscreen Q3 route uses a GPU CopyArea over all 786432 pixels
per frame. The retained 1024x768, FAA0, interval0 observation is in
`tests/m9-present-fullscreen-20261007`: 387 frames in 10.9 seconds, 35.5 FPS,
with no CPU detile, upload, readback or temporary surface. This is not an
A/B comparison with another profile. Copy elimination remains a meaningful
performance candidate; the host copy/flush scopes do not measure its GPU cost.

## Physically repaired format publication

Canonical RE commit `67e70597` retains the read-only KMS observation at
[the original archive](../tests/m9-faa-status-hardware-20261007/kms-before.json).
On M9128 `102b:0540` rev1 / `102b:22c0`, boot
`d3f40c04-11a2-4807-8cf9-f5f99c3d03e2`, kernel `516990f`, module SHA256
`3bad5ac5941d37e3236e30a336604cf9c2db3aa71e72e7c416a3de5febbfd18c`,
both primary planes expose all five formats for TILED, TILED_SMALL and LINEAR.
The raw blobs independently decode to three `1f` rows. Cursor planes expose
only ARGB8888 / LINEAR. ADD_FB2 modifiers is 1; legacy and atomic async flip
caps both remain zero. This closes blob publication, not update/event/pixel
or performance qualification. The archived getter warning occurred once per
head. Kernel candidate `f663187` replaces the forbidden atomic property
getter with `drm_object_immutable_property_get_value`, preserving masks and
blob ownership; its actual-helper fixture covers 14 controls and four
negative mutations. Its next normal-load warning check is still pending.

## Interval0 cannot use the current flip path

Mesa `loader_dri3_helper.c` sends XCB Present ASYNC when the actual swap
interval is zero or negative. Xserver `present_get_target_msc` then requests
the current MSC; synchronous presentation requests the next MSC.
`present_vblank_init` permits that current-MSC flip only with
`PresentCapabilityAsync`. Modesetting sets the capability only after a
successful `DRM_CAP_ASYNC_PAGE_FLIP` query returns 1. Physically that query
returns zero. Correct IN_FORMATS and an opt-in PageFlip setting therefore
cannot eliminate the current interval0 fullscreen copy by themselves.
No interval override or unsupported capability advertisement is justified.

The separate remaining gates include explicit PageFlip admission (its
current default is FALSE), VT ownership, no DRI2 flip or rotation, a hardware
cursor, an unredirected screen-sized window at the correct position,
scanout-capable imported backing, and every active CRTC accepting its actual
format/modifier. The present active scene has not physically qualified all
these gates or a KMS flip.

## Original display-start producer and the missing contract

`tools/m9-present-async-source-check.py` pins the saved M9 Linux driver image
SHA256 and its ELF address/file join. The original diagnostic table joins
`PhxSetDisplayStart` code `0x5004` to `m9x15225` at `2f2830`. That routine builds
parameters through `2f2730`, obtains display parameters through `2f24f0`,
and calls the actual write leaf `m9x28811` at `2ef4b0`.

Sixteen original-instruction controls cover two display-bank classes, two
routes and interlace on/off. They execute that leaf against synthetic CPU
owner/parameter records and retain each original write-service call at
`2b8a20` in a write-only semantic sink. No register backing, read-back shadow,
status response or physical write is supplied. The source orders START,
optional second-field START, bounds and final configuration writes. It has
no read or event in this bounded leaf. This proves programming grammar;
it does not prove when scanout changes or when the old framebuffer can be
released. The full original caller's scheduling and completion remain open.

The native simple-pipe `.update` calls `m9_program_scanout`, then arms a
vblank event through `matrox_kms_arm_event`. Its direct path derives START
from live pinned tiled backing and writes START and pitch. Neither
`.async_check` nor `.async_update` is implemented. The generic simple CRTC
uses `drm_atomic_helper_page_flip`. A CPU-returned MMIO write and a later
vblank notification do not establish an immediate latch or an async event.

Before implementing async support, the missing hardware contract is: the
source-qualified update/latch sequence for the actual active display bank;
which completion proves the new backing is being fetched; and when the old
owner may be released while scanout continues. Neither an idle register
value nor the bounded vendor write leaf supplies those guarantees. Async
caps stay false. A synchronous flip/image/event test is a separate possible
qualification and does not improve interval0 by itself.

## Reproduce the bounded proof

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9-present-async-source-check.py \
  --out /tmp/m9-present-async-source.json
```

The check executes all sixteen original controls, joins actual native/Mesa/
Xserver source gates, verifies archived identity and hashes, and independently
decodes raw physical IN_FORMATS blobs. The original leaf is CPU-only;
the physical observation proves metadata only. No target operation is run.
