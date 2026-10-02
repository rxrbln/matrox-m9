# M9 Gallium bring-up boundary

M9 (PCI `102b:0540`) is a separate 3-D architecture.  It has an independent
`m9x` DRM loader identity, Gallium screen, build dependency, and `m9x_dri.so`
megadriver link.  The separate M9 kernel driver exposes only its M9 ioctls.
No M9 command is described with a classic `P3D_*` register or packet
definition, and no M9 source is built into the Parhelia Gallium libraries.

## CPU transform rendering path (working)

Contexts are created when the kernel executes user streams (probed by
ioctl behaviour: an empty flags-zero SUBMIT fails with `EINVAL`, and a
validate-only `STREAM` relocation passes) and the fixed-fragment database
`m9x/rsfragptrs-normal.bin` is found (`M9X_FRAGMENT_DB`, the firmware_class
path, `/lib/firmware/updates`, `/lib/firmware`).  The earlier user-submit and
linked-program blockers below do not apply to this path.

* The Gallium draw module runs vertex programs (NIR to TGSI exec, GLSL 1.20
  caps, GL 1.5 compatibility is what st exposes).  A vbuf render receives
  window-space primitives; strips, fans, quads, quad strips and polygons are
  native, line loops and first-vertex flat shading are converted to lists.
  Points are expanded to triangles by draw.
* `m9_draw.c` emits one two-pass job per pass state: the captured
  pass-through program over `xyz + rgba` float records (y submitted bottom-up,
  `z = 1 - 2 z_win`), one covering preprocess draw padded to four records,
  and raster draws restating `22/6a` and `f0/ef/d7/f4`.
* `m9_draw_params.c` holds every size and state word, measured on the
  M9148 (xf86-video-parhelia docs `parhelia-m9-cpu-tnl-raster-state.md`,
  `parhelia-m9-cpu-tnl-draw-parameters.md`): descriptors
  `cc21/fa21/f001 | align(W,32)`, clip `H<<16|W`, the 48-word system prefix
  and linked record formulas, index 10/23 declarations, depth `a8`+`ef`,
  cull `ef`, scissor `f5/f6/c4/c5`, color mask `ab`, topology and per-draw
  shading (flat-form links, `22` bit 14).  Its unit tests reproduce the
  verified 320x240 stream dword for dword.
* Color render targets are `PARHELIA_M9_BO_TILED`; transfers read and write
  rows through `DRM_IOCTL_PARHELIA_M9_SURFACE_ACCESS`.  Full clears fill the
  BO (tiling only permutes pixels); scissored or masked clears draw a quad.
  D24S8 depth images own a hierarchical-Z BO initialized to `0x007ff000`.
* `tools/m9-mesa-triangle` (xf86-video-parhelia) passes on hardware at
  320x240: reference triangle 28000 pixels at the right positions, flat and
  Gouraud, native topologies, culling, scissored clear, color mask, a
  7200-vertex draw split over two jobs, and the LESS depth scene.

Gaps: texturing, blending, stencil, alpha test and non-pass-through fragment
shaders (draws are skipped with a warning), perspective-correct color
interpolation (records carry w = 1), render sizes other than 320x240 are
covered by the measured formulas but not yet run through Mesa, surface access
excludes targets with both sides <= 64, and depth readback is raw tiled data.

Stencil (2026-10-02) is the fixed-function depth unit's: `a8` bit 5 enables
it, bit 6 the second face, function/sfail/zfail/zpass at bits 9:7..18:16 for
clockwise faces and 21:19..30:28 for counter-clockwise ones, `a9`/`aa` the
ref/value-mask/write-mask bytes of those faces, `ef` 0x7000 even without the
depth test (CAPTURED, vendor GL fixed-state and gl15 fragops corpora, whose
readbacks pass).  `m9_stencil_words` packs it; clears replace stencil
through the same quad as depth.  The alpha test is lowered by st into the
fragment program (`M9_FS_CONST_ALPHA_REF`).

## Implemented foundation

The following userspace foundation is ready for the first executable
userspace-stream submit.  The independent kernel module has already completed
6,000 consecutive kernel-owned no-op fences across both directions of its
two-slot primary command rebase.  Arbitrary userspace streams remain disabled:

* `m9_internal.h` owns separate screen, context, resource, canonical vertex,
  and batch structures.  M9 command buffers are capped at the kernel's 32 KiB
  limit, with 128 relocations and 64 referenced BOs.
* `m9_batch.c` emits the byte-indexed M9 packet format with stop byte `0x45`.
  Address operands always start as zero and are named by a typed UAPI
  relocation.  BO access is accumulated independently from relocation type;
  render/depth targets are conservatively read/write for kernel lifetime
  validation.
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
  Static emitter ownership and live snapshots prove index `69` selects the
  immutable external fixed-fragment database; it uses a 0x1000-aligned
  RESOURCE_69 relocation with read-only access. The render target is bound
  independently through selector-8 `b0/b1`. Index `6c`
  uses a distinct RESOURCE_6C relocation to a 0x80-aligned system record.
  The kernel supplies `6c` low bit 0; it does not add the requester system
  tag.  The builder accepts only a program explicitly marked as the captured
  gltri class.  General NIR programs are not claimed.
* The captured triangle raster state emits viewport/scissor, stream controls,
  and `a8` depth bits.  Depth test with no update is `a8 bit0=1, bit1=0`.
  H03 numeric, H07 32-bit-index, and H06 packed-16-index draws use exclusive
  ends and only the two observed launch classes.  H06's odd terminal halfword
  is an explicit relocation flag; both H06 and H07 use one BO for the paired
  start/end relocations and reject a 32-bit packet-span overflow.
* `m9_batch_prepare_submit()` builds the exact initial DRM UAPI request and
  `m9_batch_validate()` uploads it to a COMMAND BO with
  `PARHELIA_M9_SUBMIT_VALIDATE_ONLY`.  `m9_command_bo_create()` creates the
  required 32 KiB DMA32 command object.
* The lower execution path is now built rather than implied.  It prepares the
  same request with flags zero, uploads the sealed command image, requires a
  nonzero returned fence point, and refuses the operation unless the screen's
  executable-submit gate is set.  The diagnostic no-op-fence request has a
  separate exact helper.  Pipe fences model the current synchronous UAPI; an
  asynchronous kernel ABI must add a wait operation before it can be enabled.
  `m9_submit_first_triangle()` supplies one kernel-validated VRAM color target
  to the otherwise kernel-owned diagnostic.  The current kernel prepares its
  complete guarded job and returns `-EOPNOTSUPP` at the fresh selector-6c
  serializer gate.
* Context allocation, its per-context command BO, batch isolation, referenced
  framebuffer/texture state, flush, teardown, and direct CPU buffer/texture
  transfers are implemented behind
  two independent gates: executable submit and a safe linked-program
  constructor.  Mip transfer offsets include the recovered 32x16 tail atlas.
  A failed submit marks its context lost; later flushes discard pending work
  without issuing another global-engine job.
  A state tracker still receives `NULL` while either gate is closed.
* Ordinary render targets, depth buffers, and sampled images use
  `CREATE_BO(VRAM)`, matching the vendor's placement-4 local-memory resources,
  ordinary layout 1. Render/depth allocation reproduces the vendor's
  small-versus-ordinary category selection, horizontal and vertical quanta,
  eight-element end padding, and final byte-quantum rounding. Gallium buffers,
  explicit staging images, and command BOs remain DMA32 upload resources.
  Framebuffer state accepts only a complete local-VRAM layout-1 image; it
  rejects DMA32 and placement-unknown imports before command construction.
* Presentation BOs use the render-node-safe `CREATE_BO(VRAM)` path with the
  guarded KMS contract's exact 1024x768 XRGB8888 shape, 4096-byte pitch,
  vendor-exact `0x301000` renderable backing, and direct BAR0 mapping.
  `GET_BO_INFO` returns the mmap cookie needed to map imported local-VRAM
  handles without primary-node-only modesetting ioctls.
  Resource creation rejects every other scanout or display-target template,
  including ARGB8888, before allocating a generic VRAM image.
  KMS, flink, and PRIME winsys handles have symmetric import/export paths.
  `flush_frontbuffer` publishes writes to that same BO and performs no shadow
  copy or resolve.  DMA32 resources and VRAM image resources retain distinct
  domains and destruction rules.

The pipe screen advertises only exact, joined linear format usages and
same-driver dma-buf import/export. It does not advertise MSAA, arrays,
cube/3-D targets, shader images, compute, or GLSL. Context creation returns
`NULL` while the kernel screen
state says executable submit is unavailable or the linked-program constructor
is unsafe.  This is deliberate: the kernel
at commits `b7d0194`, `c611a0e`, `d70b370`, and `f459a8b` validates arbitrary
command streams but executes only its kernel-owned synchronous no-op fence
diagnostic.  Fence targets now preserve the natural
`0xfffffff0 -> 0 -> 0x10` rollover and polling issues at most one extra
progress kick.  Kernel commits `6c27e17` and `9219f41` add derived BO access,
same-BO H06/H07 range validation, and retired two-slot command rotation.  The
6,000-submit hardware run crossed from slot 0 to 1 at submit 2,731 and back to
slot 0 at submit 5,461, ending at fence point `0x17700`.
Kernel commit `e8bd2f3` adds the no-input first-triangle preparation path.  It
does not execute: after allocating, initializing and relocating the complete
known draw it fails before MMIO at the fresh-record serializer boundary.

## Evidence test

`m9-batch-test` checks:

* UAPI object sizes and submit field placement;
* exact captured color, texture, and depth-control values;
* packet header byte order and `0x45` termination;
* relocation dword, kind, access, and deduplicated BO reference handling;
* H07 and even/odd H06 exclusive-end construction, including a draw END
  relocation equal to the modeled BO size;
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
   The Mesa flags-zero execution request is ready, but the current ioctl
   returns `-EOPNOTSUPP` by design.
2. The gltri preprocessing pass writes four transient resources which the
   raster pass consumes in the address permutation `[b0,b4,b8,b2]` at
   indices `00..03`.  Four canonical preprocessing layouts, their routing and
   repeatable `0x16800` address lattice are captured.  The invariant 80-slot
   native image, canonical vertex arena extent `0x192700`, C2 extent `0x1080`,
   BE auxiliary-surface ownership, its exact 1-MiB allocation extent, and
   AB/AC/AE producer shape are also exact.
   The bounded flat-green source is four 20-byte `{x,y,r,g,b}` records (the
   fourth duplicates record 2), and the one-target surface state is
   `AB=AC=0xffffffff`.  C2 has no intervening GPU initializer and the guarded
   diagnostic policy is a zeroed job-local `0x1080` object.  This Mesa skeleton
   general Mesa path still lacks the preprocessing builder, local-VRAM
   allocator, and a fresh linked record.  The kernel-owned diagnostic now
   contains the exact 118/113/8-dword templates, all 24 typed address sites,
   six transition targets, and guarded operand initialization.
3. Selector `6c` atomically binds one immutable linked VS/FS pair.  Private
   0x3158-byte allocation, ordered cache reuse, COW validation/publication and
   fence lifetime are exact.  Symbolic graph descriptors now map through the
   physical converter to finite `94/b4/c4` selector namespaces and exact
   W0/W1 bit fields.  A new compiler still needs arbitrary CFG scheduling and
   the heterogeneous external `6c` record constructor.  The captured A/B
   programmable records contain no embedded GPU-BO pointers, but the selected
   fixed-function slot contains nine process-specific host pointers and cannot
   be replayed in a new context. Index `69` is the external fixed-fragment
   database, and the known bind tuple alone is not a general shader path.
   This is now the final Mesa context-creation gate after kernel execution.
4. Triangle-strip control is captured from gears, but its correlated `d7`
   field and strip batching boundary need a minimal differential before live
   draw submission.  Other primitive raster-control values are unproved.
5. The direct page-run, GPU rectangle-draw, and CPU fallback clear paths are
   statically recorded, including their surface views and rectangle records.
   They still need translation into the new batch builder and live replay;
   linear BOs can be CPU-cleared during early bring-up.
6. All class-3 calculator routes and all fourteen non-32-bit aperture formulas
   are exact.  Their live index-`78` table-root/page ownership and physical-page
   validation remain open.  Hierarchical-Z format `0x0b`, its
   `ceil(width/8) x ceil(height/4)` extent and selector-12 `ba/bb` bind are
   exact.  Static analysis also closes initialization/partial invalidation to
   `0x007ff000`, full-depth-clear seeding, uncoupled-write dirty refresh,
   creator/clone ownership, and null-auxiliary fallback.  Keep HZ disabled
   until live rejection-correctness validates that protocol.  Compressed,
   cube/volume, sampler sRGB, query, reset-recovery, and
   general-shader paths remain gated at the narrower boundaries in the
   readiness ledger.
7. The additive `PARHELIA_M9_BO_VRAM` create flag and typed VRAM relocation
   validation must remain synchronized with the kernel UAPI.  Mesa now uses
   that domain for raster images and rejects DMA32 framebuffer attachments;
   executable userspace draws remain gated until the parameterized job path
   has complete BO reservations and retirement fences.

The Mesa loader maps PCI ID `0x0540` exclusively to `m9x`; the Parhelia map
contains only the P-series IDs.  Keep `include/drm-uapi/m9x_drm.h`
synchronized with the kernel UAPI header.
