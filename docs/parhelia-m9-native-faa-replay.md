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
at (80,180), (240,180), (160,60), then reads through the exact vendor sampled CPU window. Before submission it verifies a positive CPU pattern and clears the background.
It saves actual PPM pixels and checks interior/exterior colors and partial
edge coverage. This is a hardware oracle; it does not rasterize on the CPU.
The scoped sampled-window admission exists only while the diagnostic is on.

The first physical M9128 run (kernel776911a) reached actual retirement
point0x1e0 without timeout and kept SSH healthy on the same boot. The image
oracle FAILED: entirely black, zero colored/partial pixels,8326 bad interior
pixels. Raw former output storage read before another allocation was also
zero. See tests/m9-faa-native-20261006 for hashes, logs and actual pixels.
This is bounded-stream retirement evidence, not working FAA rendering.

Vendor CPU-map tracing subsequently proved selector1 uses F0220140 with
padded height256 and tagged GPU address. Kernel30023d9 admits only that
exact private320x240 view with verified identity mappings. The current image
fixture requires a76800-pixel positive CPU read/write control before clear
and submission. The corrected freshboot probe (kernel30023d9) first refused its pre-submit
CPUcontrol because the identity manager was not yet initialized. A transient
probe using legacy CPUinitialization again retired the FAAstream at0x1e0,
but exact tagged F022readback timed out (rc137, noimage). SSH stayed healthy.
See tests/m9-faa-cpu-map-20261006. Kernel010853b subsequently bootstraps the
private host view before reserving color (built, not deployed here). The
CPUtranslation join and rendered fragments remain unqualified. All four selected E3
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

## Reusable ordering and the remaining M9 mapping distinction

The current Parhelia/P690 implementation provides app-level ordering rules:
drain pending draws before helper state changes, resolve the old FAA owner
before framebuffer rebinding, retain color/depth/samplers and private buffers
through resolve/final retirement, then restore ordinary bindings. Its packet
words and FAA fence opcodes are family-specific and are not M9 packet templates.
The private M9 producer already preserves captured scratch/draw/resolve
chronology and retains owners across real completion under the transport lock.
Those application ownership rules remain necessary when generic M9 FAA is wired.

Actual target read-only inspection now shows GPUprimary1422000 has a correct
GPU leaf and a correct root for its leaf-storage page, but its direct CPU-root
entry is zero. The genuine2cb40 mapper copies the primary leaf into the0360
root child at indexsurfaceGPU>>12 before publishing {GPU|1,F0220140,10100}.
This is not a0f64-table copy. The missing direct CPU-root join is independently
visible in tests/m9-faa-cpu-map-20261006/proof/address-observe.txt. Its exact
flush/copy proof and native implementation are in progress. GPU rendering
qualification still requires actual image evidence after the mapping fix.

## CPU mapping repaired; downstream image still fails

The physical kernel1b4dd67 run on bootfd7798f9 passes all76800 positive
patternpixels with exactF0220140/10100/GPU|1 and completes postretirement
readback without timeout. All117648 knownVRAMsourcebytes verifyexactly.
The FAAstream retires1e0, but output remains black. Before/after private
owner observations and readonly output extraction now prove GPUpreprocess
produces the three correctpositions and greenRGBA varyings. Pool/sentinel,
auxiliary andHZ change; mainfragmentstore remains unchangedzero. The next
question is downstream fragment/raster/FAA output, rather than missing VS
geometry or a broken CPUmapper. Actualbytes/logs/image are retained in
tests/m9-faa-root-join-20261006. Kernel82b4dd2 extends directroot constructor
identities to supportedlocalBARpages using actualvendorinitializer evidence;
its nativeFAAqualification is pending. No appFAA success is claimed.
