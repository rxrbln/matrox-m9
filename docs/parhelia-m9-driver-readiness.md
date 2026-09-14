# M9 kernel and Mesa driver readiness

This is the implementation boundary after the static Linux and Windows binary
analysis.  There is enough evidence to write an opt-in kernel and Mesa
bring-up driver, but not enough to promise a safe working first triangle.  This
note distinguishes code that can be written now from behavior that still needs
an M9 capture or hardware experiment.

## Ready to implement

The following is sufficiently specified for an opt-in bring-up driver:

* PCI/BAR discovery, PInS parsing, clocks, display controllers, output routing,
  modesetting, scanout, and the four vblank sources;
* linear VRAM and DMA-visible system allocations, the six internal allocation
  selectors, mode 2's request-controlled `SYS`/`PCI_LOCKED` allocation, mode
  1's optional `SCATTER` backing, requester initialization, command-buffer
  framing, relocation address tags, the `0x404` executable address/doorbell,
  bounded idle waits, requester-local restart, and the exact three-dword memory
  write-back fence record;
* separate M9 packet construction and validation for the recovered H03, H06,
  and H07 records, draw records, render targets, ordinary depth, viewport,
  scissor, clears, and the recovered state indices;
* vertex/index submission, primitive lowering, CPU-transformed vertices,
  blend, alpha-test, depth/stencil, color masks, and the recovered raster
  compiler key;
* ordinary linear 2D, cube, and volume textures, formats, filtering, wrapping,
  LOD, mip allocation, and the packed mip-tail atlas;
* the two-dword native instruction container, native packer fields, program
  upload selector and ports, program-bank limit, entry-point registers, GLSL
  varying linkage, typed stage handles through parsed native descriptors, the
  live Windows and Linux mode-6 MOV forms, the exact shared terminal slot
  `1d 7e fc ff 0f 3f 80 a2`, and enough IR structure to add semantic opcodes
  as they are proved;
* GL locations through stage-native indices, range records, vertex-then-fragment
  FIFO transport, persistent stage constant banks, old-engine limits, and the
  guarded mode-2 bus-master resource path up to its final opaque allocation
  join;
* context ownership, state save/restore order, fence lifetime, and resource
  lifetime rules.

This is enough to start a separate `parhelia_engine_ops` implementation in DRM
and a separate M9 backend in Mesa.  It is not enough to enable PCI ID `0540` by
default or promise a working first triangle without a controlled bring-up.

## First-render gates

These gaps have high leverage and should be resolved before unrestricted 3D
submission:

1. Capture one vendor clear and one triangle, including the command buffer,
   `0x400..0x524`, fence memory, referenced allocations, and affected VRAM.
   There is no saved known-good M9 3D stream in `captures/` today.
2. Validate one real M9 allocation and submission against the recovered three
   separate namespaces: internal allocation mode, public Linux memory type,
   and requester address class/tag.  First-use programming of `0x500`, `0x508`,
   and `0x504` is exact.  The hardware-owned `0x50c/0x510/0x514/0x520/0x524`
   fields still need one wrap capture, and no saved M9 stream confirms the
   resulting GPU addresses on hardware.
3. Close or experimentally validate the minimal vertex/fragment program
   boundary.  Stage ownership, native descriptors and live MOV forms are
   proved, and both vendor compilers produce the same exact terminal slot.  The
   remaining live-path gap is the producer/ownership edge from compiled
   descriptor `+0x128` and native slots `+0x28` into top-state program substate
   `+0xc8`, specifically its resource identity `+0xa0/+0xa8` and `6a/6d`
   controls `+0xb0/+0xd0`.  The separately exported mode-2 constant helper has
   no in-image live GL owner and must stay outside the ordinary stage map until
   runtime evidence identifies an external trigger.
4. Exercise requester timeout and restart.  Neither saved vendor stack exposes
   a reusable global/3D reset sequence or a statically identifiable 3D fault
   interrupt, so the initial driver must fail closed after the proven local
   restart boundary.
5. Run an A/B/A two-context render-and-fence test before exposing concurrent GL
   clients.

## Features that can remain disabled during bring-up

The following do not block a linear, basic OpenGL driver:

* class-3 blocked texture/render-target allocation and its physical in-block
  address permutation;
* render-to-texture cache optimization beyond the proved class-1 dependency
  fence through packet index `0x43`; copy preflight uses class 2 plus a legacy
  channel, and an explicit texture-cache visibility rule is not statically
  identified;
* occlusion queries and their revision-dependent slot details;
* the complete native ALU/texture/control mnemonic table, uncommon predicates,
  and every special lowering path;
* uncommon non-vblank service interrupts and global hang recovery;
* the unresolved hardware cursor layout and optional output features.

The practical next step is implementation plus narrow captures.  More static
analysis can reduce individual uncertainties, but it cannot manufacture the
missing runtime joins: the vendor-generated native program bytes, hardware-owned
requester fields, fault behavior, and class-3 physical address permutation.

## Safe implementation sequence

1. Add M9 probe support behind an explicit module option and keep it separate
   from the Parhelia command backend.
2. Bring up KMS, vblank IRQs, linear scanout, linear BOs, and software cursor.
3. Add a kernel-only requester/fence test, then clear and one-triangle tests with
   a strict M9 packet whitelist.
4. Add a Mesa backend using CPU-transformed vertices and minimal passthrough
   programs; keep unproved opcodes and layouts unsupported.
5. Validate alternating contexts, textures, depth/blend, mipmaps, and multiple
   GL clients before enabling the PCI ID by default.
