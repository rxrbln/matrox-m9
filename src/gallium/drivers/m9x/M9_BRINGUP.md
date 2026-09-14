# M9 Gallium bring-up boundary

M9 (PCI `102b:0540`) is a separate 3-D architecture.  It has an independent
`m9x` DRM loader identity, Gallium screen, build dependency, and `m9x_dri.so`
megadriver link.  The separate M9 kernel driver exposes only its M9 ioctls.
No M9 command is described with a classic `P3D_*` register or packet
definition, and no M9 source is built into the Parhelia Gallium libraries.

## Implemented foundation

The following userspace foundation is ready for the first executable
userspace-stream submit.  The independent kernel module has already completed
256 consecutive kernel-owned no-op fences, plus a fresh fence after adopting
the `m9x` DRM identity.  Arbitrary userspace streams remain disabled:

* `m9_internal.h` owns separate screen, context, resource, canonical vertex,
  and batch structures.  M9 command buffers are capped at the kernel's 32 KiB
  limit, with 128 relocations and 64 referenced BOs.
* `m9_batch.c` emits the byte-indexed M9 packet format with stop byte `0x45`.
  Address operands always start as zero and are named by a typed UAPI
  relocation.  BO access is accumulated independently from relocation type.
* Selector-8 linear render/depth descriptors and selector-2 texture
  descriptors use the statically recovered row-span, height/layer, layout,
  flag, and seven-bit format fields.  The captured 320x240 texture vector is
  `f0210140,000100f0`; the corresponding A8R8G8B8 render vector is
  `cc210140`.
* Sampler index `7c` packs the recovered min/mag/mip, wrap, anisotropy, and
  compiler-input tables.  Index `7d` carries signed 10-bit LOD bias and two
  nine-bit bounds.  The two bounds retain neutral names because their final
  orientation has not been captured.
* `m9_mip_layout_init()` builds consecutive ordinary levels and the recovered
  shared 32x16 tail atlas at `(0,0), (16,0), (24,0), (16,8), (24,8)`.
  Only 2-D, uncompressed images are accepted by this first layout helper.
* `m9_repack_vertices()` converts float and normalized-byte Gallium-like
  inputs to one canonical CPU layout: float4 position, float4 color, and
  float2 texture coordinates.  Missing color and texture inputs receive
  OpenGL defaults.  This keeps API vertex formats out of the unknown hardware
  stream descriptors.
* The recovered primitive frontend selectors and API vertex-count arithmetic
  cover points, line list/strip, and triangle list/strip/fan.  Final raster
  control is enabled only for captured triangle-list `0x6403`.  Gears uses
  both `0x6406` and `0x6407`, so strip control remains gated.
* The known gltri linked-program bind emits indices `69,6a,68,6b,6c`.
  Address-directed live snapshots prove index `69` selects drawable backing;
  it uses a 0x1000-aligned RESOURCE_69 relocation with read/write access.  Index `6c`
  uses a distinct RESOURCE_6C relocation to a 0x80-aligned system record.
  The kernel supplies `6c` low bit 0; it does not add the requester system
  tag.  The builder accepts only a program explicitly marked as the captured
  gltri class.  General NIR programs are not claimed.
* The captured triangle raster state emits viewport/scissor, stream controls,
  and `a8` depth bits.  Depth test with no update is `a8 bit0=1, bit1=0`.
  H03 numeric and H07 address-range draws use exclusive ends and only the two
  observed launch classes.
* `m9_batch_prepare_submit()` builds the exact initial DRM UAPI request and
  `m9_batch_validate()` uploads it to a COMMAND BO with
  `PARHELIA_M9_SUBMIT_VALIDATE_ONLY`.  `m9_command_bo_create()` creates the
  required 32 KiB DMA32 command object.

The pipe screen advertises only exact, joined linear format usages.  It does
not advertise MSAA, arrays, cube/3-D targets, dma-buf sharing, shader images,
compute, or GLSL.  Context creation returns `NULL` while the kernel screen
state says executable submit is unavailable.  This is deliberate: the kernel
at commits `b7d0194`, `c611a0e`, and `d70b370` validates arbitrary command
streams but executes only its kernel-owned synchronous no-op fence diagnostic.

## Evidence test

`m9-batch-test` checks:

* UAPI object sizes and submit field placement;
* exact captured color, texture, and depth-control values;
* packet header byte order and `0x45` termination;
* relocation dword, kind, access, and deduplicated BO reference handling;
* a draw END relocation equal to the modeled BO size;
* program binding, primitive counts, mip-tail positions, vertex conversion,
  and validation-only submit construction.

Run it with:

```sh
meson test -C BUILD m9-batch --print-errorlogs
```

## Exact execution blockers

These gaps must remain visible in capability and execution gates:

1. The kernel does not execute a user command stream yet.  It must append and
   own completion, validate full state masks, pin every referenced BO, and
   expose completion before `m9_screen.execute_submit` can become true.
2. The gltri preprocessing pass writes four transient resources which the
   raster pass consumes in the address permutation `[b0,b4,b8,b2]` at
   indices `00..03`.  Their final element formats, strides, and semantic roles
   still need controlled vertex-format captures.  The canonical CPU layout
   therefore stops before hardware descriptor lowering.
3. The native shader storage, linked-record bytes, and its two internal stage
   entry/resource descriptors need a fully decoded grammar and native compiler.
   Index `69` is drawable memory, and the known bind tuple alone is not a
   general shader path.
4. Triangle-strip control is captured from gears, but its correlated `d7`
   field and strip batching boundary need a minimal differential before live
   draw submission.  Other primitive raster-control values are unproved.
5. The direct page-run, GPU rectangle-draw, and CPU fallback clear paths are
   statically recorded, including their surface views and rectangle records.
   They still need translation into the new batch builder and live replay;
   linear BOs can be CPU-cleared during early bring-up.
6. Packed/tiled render storage has exact descriptor rules but still lacks the
   physical in-block permutation needed by a new allocator.  Compressed,
   cube/volume, sampler-compare, query, reset-recovery, and general-shader
   paths remain gated at the narrower boundaries in the readiness ledger.

The Mesa loader maps PCI ID `0x0540` exclusively to `m9x`; the Parhelia map
contains only the P-series IDs.  Keep the driver-local `m9x_drm.h`
synchronized with the kernel UAPI header.
