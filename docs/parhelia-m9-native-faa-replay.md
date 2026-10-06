# Private native M9128 FAA replay

The Linux producer now owns the actual normal FAA frame's allocations and
source bytes. It is a bounded diagnostic behind `m9_faa_diag=1`, selected
through the existing FIRST_TRIANGLE ioctl. It does not enable application
MSAA or permit arbitrary client-owned indirect streams. Normal application
submission refuses while the private context diagnostic is enabled.

`tools/m9-native-faa-template.py` converts the pinned normal capture into
30 unique backing owners (40 resource views), 53 typed address fixups,
26 timeline fields and an input blob containing only 125,008 actual known bytes. Aliases share
their backing owner; constants and geometry retain the physical DMA domain.
The primary is 5,676 raw bytes, while the two counted secondary images
retain their original 604/772-byte structure and physical selector2.
Address-looking instruction/texture-state words are not numeric relocations.

The generated kernel header contains compile-time ownership tables and the
blob size/fingerprint. The kernel loads `m9x/faa-normal-known-inputs.bin`,
allocates page-rounded pinned VRAM/coherent DMA32, copies only valid spans,
and joins the sampled primary to the caller's reserved 320x240 BO.
Opaque stores/HZ/pool and uninitialized allocation tails receive no inferred
CPU initialization. Program/database/metadata sources are unchanged when a
genuine preceding vendor allocation moves their GPU addresses.

The private producer verifies native local identity leaves and their root
leaves before retaining the captured tagged-address encodings. It joins
the requester's command/common-counter/default-restore ownership to native
allocations. Default restore is separate from FAA pool6; the captured
selector1 restore binding is checked against native identity mappings.
Transport drain precedes counter-register modifications under the same
lock. A posted failure retains private owners and quarantines the caller's
color/fence until bus mastering stops. A successful final GPU writeback,
rather than command posting, produces the completion point.

`tools/check-m9-native-faa-template.py` proves captured identity replay,
distinct native rebasing, raw/counted packet preservation, tracker ordering,
untouched non-fixup bytes, absence of opaque tail initialization, and refusal
of unaligned/wrapping owners, damaged source blobs and ambiguous aliases.

`tools/m9-native-faa-image.c` initializes only the application's sampled
primary background using SURFACE_ACCESS, submits the captured green triangle
at (80,180), (240,180), (160,60), then reads through the same sampled window.
It saves actual PPM pixels and checks interior/exterior colors and partial
edge coverage. This is a hardware oracle; it does not rasterize on the CPU.
The scoped sampled-window admission exists only while the diagnostic is on.

Hardware FAA retirement and image proof are pending. All four selected E3
timeline snapshots, twenty E3 completion tokens and two waits now translate
through the same genuine timeline bias. The independent fetch audit rejects
plans that omit any of these actual producer-written slots. The private
pool save/restore GPU effect and shader fetch behavior require the hardware
run; CPU capture alone does not prove rendered fragments or completion.

Reproduce the producer and audit from the RE repository:

```sh
python3 tools/m9-native-faa-template.py \
  captures/m9128-device-inputs-20261006/normal-faa-input-bytes.json \
  --external captures/m9128-device-inputs-20261006/normal-faa-native-addresses.json \
  --plan captures/m9128-device-inputs-20261006/native-faa-plan.json \
  --blob captures/m9128-device-inputs-20261006/native-faa-known-inputs.bin \
  --kernel-header /path/to/kernel/drivers/gpu/drm/m9x/m9_faa_native_data.h
python3 tools/check-m9-native-faa-template.py
```
