# M9/Phoenix versus Parhelia 3D command comparison

This note records what can safely be reused when adding Matrox M9/Phoenix
(`102b:0540`, M9120/M9138/M9140/M9148) support to the DRM and Mesa drivers, and
what must be reverse engineered as a separate engine.  It is based on the saved
Linux M9 driver, the Windows MSerX driver, the existing M9 notes, and the current
Parhelia command-stream work in this tree.

Confidence labels used here:

* **[BIN]**: visible in a saved vendor binary or its static disassembly.
* **[HW]**: previously measured on M9148 hardware and recorded in the M9 notes.
* **[INF]**: architectural inference which still needs a command capture or a
  hardware test.

## Result

M9 and Parhelia retain the same *idea* of an indexed register command stream,
but they are not command compatible.  The common packet-header shape is useful
for sharing a small packet-builder library.  The terminator rules, DMA requester,
chaining commands, completion protocol, register meanings, draw launch, surface
descriptors, and both shader engines differ.  A Parhelia command buffer cannot
be replayed on M9, and M9 support must have a separate engine backend and a
separate Mesa state/compiler backend.

The DRM infrastructure can still be shared: PCI/GEM/VRAM management, scheduler,
syncobj plumbing, IRQ/hangcheck framework, KMS framework, and a common validated
submit ioctl.  The chip-specific command parser, relocation table, engine arm,
reset, fence emitter, and state encoder belong behind M9-specific operations.

## Sources present in this tree

### Linux M9 package

`m9xdriver-x86_64-1.4.3-80847-20140908-build_98/` is extracted and contains:

* `xserver/1_13_0/m9x_drv.so`: 64-bit, stripped, about 17 MiB.  It contains the
  Phoenix display/EXA code and the Linux OpenGL implementation.  Diagnostic
  strings identify `SendCommandStreamUseDmaList`, `EqualizeDMATag`,
  `PBoard::InitBoard`, GLSL compiler code, and the Phoenix hardware paths.
* `kernel/KernelLibraries.o_shipped`: 64-bit relocatable object, about 500 KiB.
  It is not stripped and has 1,258 symbol-table entries, but private names were
  renamed to `m9xNNN`.  Relocations against the open kernel wrapper and detailed
  diagnostic strings remain.  This is the best static source for recovering the
  kernel-side requester, ioctl, DMA allocation, interrupt, and enable sequence.
* `kernel/src`, `kernel/common`, and `kernel/include`: source for the Linux
  wrapper, memory allocation, mapping, context, public ioctl, and IRQ glue.  The
  proprietary hardware core is the shipped object above.
* `xserver/*/libglx.so`: matching vendor GLX modules for several Xorg versions.

### Windows MSerX package

`xddm32_210_00_105_se_u_nowhql/MSerX/` contains:

* `MxMGLX.dll`: 32-bit M-Series OpenGL ICD, about 9 MiB.  This is the primary
  source for GL state lowering, vertex delivery, texture state, and shader
  compilation.
* `MxMD.dll`: 32-bit D3D/GDI display driver, about 2 MiB.  It is useful for
  register emission, surface descriptors, DMA command construction, D3D state,
  and shader upload.
* `MxMM.sys`: 32-bit miniport, about 458 KiB.  It owns low-level hardware and DMA
  setup on Windows.
* `MxMDI.dll` and the INF/auxiliary binaries.  `MSerX.inf` binds the Phoenix
  subsystem variants to PCI device `0540`.

The detailed pre-capture command and shader findings are in
`parhelia-m9-3d-static-re.md`.  The four earlier M9 notes are `parhelia-m9.md`,
`parhelia-m9-accel.md`, `parhelia-m9-dma.md`, and
`parhelia-m9-3d-submission.md`.

## Command-stream comparison

| Property | Parhelia/P650/P690 | M9/Phoenix | Reuse |
|---|---|---|---|
| Register packet header | Four 8-bit indices in one dword, LSB first, followed by values | Same four-index header shape and value order | Share a parameterised packet builder |
| Register index address | `0x1000 + index * 4` | The same relationship is supported by several index/MMIO correspondences [BIN], although CPU reads of the engine window return zero on M9148 [HW] | Share address/index helpers only |
| Short packet framing | `0x20` consumes a pad value; `0x21` terminates a tag.  Header-bearing vendor records add an outer count, so `0x21` does not by itself delimit the complete record | `0x45` stops index scanning and fills unused header bytes | Separate parser and validator |
| Command chaining | `0x28/0x29` are SECADDRESS/SECEND; the primary ring calls validated secondary buffers | M9 notes identify `0x2c/0x2d` as sub-buffer start/end and a BmBuffer/DMA-list submission path | Separate control-index policy and relocations |
| Direct register execution | Direct writes to the indexed window can execute through Parhelia's MMIO/emulated-DMA path | Direct writes stage/read as zero and did not render on M9148; the vendor uses DMA lists [HW][BIN] | No shared PIO fallback |
| Primary DMA | `0x404` start, `0x408` end/trigger, `0x400` write-back, with system-memory address tags | Saved Linux kernel code programs DMA base/control at `0x400` and rings `0x404` with the final write address. `0x314`/`0x31c` are additional front-end kick/state controls; userspace cannot replace the kernel-owned requester path with direct MMIO [BIN][HW] | Separate requester backend |
| Completion | In-stream `0x1090/0x1094` write-back, `0x12bc` DWGSYNC, `0x41c`, plus Parhelia idle state | Fence/tag addresses at `0x1068/0x106c` and a memory write-back polled by both Linux and Windows; `0x314` is a progress kick and `0x8f0 & 0x30000` is only an engine-state observation. No completion IRQ is required [BIN] | Share scheduler/fence API, not packets |
| Engine arm | Recovered Parhelia register sequence plus WARP microcode load | PCI bus-master enable and requester/context initialization are decoded. Descriptor park/save/restore is requester-local; a full engine/global reset is still unknown [BIN] | Separate init/reset |
| 2D launch | MGA-lineage surface slots, `DWGCTL` at index `0xc0`, geometry through `DSTXY/LEN` | Unified raster backend: control at index `0xf0`, geometry around `0xf2..0xfc`; copy binds a texture source | No payload compatibility |
| 3D launch | WARP triangle setup and `DWGSYNC`; software decomposes other GL primitives | Context draw vtable methods are joined to concrete three-dword command records.  H03 carries numeric start/exclusive-end ranges; H07 carries relocation-backed 32-bit ranges; H06 carries relocation-backed packed-16 ranges.  Topology and base-vertex address adjustment are lowered before launch.  Header `2b,2a,e6` is independently proven to be a 2D inline-data path [BIN][DER] | Separate draw emitter |

The first row is the strongest similarity.  It likely reflects a retained Matrox
front-end convention.  It does not imply that an index has the same meaning on
both chips.  Examples already prove collisions:

* Parhelia index `0xb9` (`0x12e4`) is alpha-test state.  M9 material uses the
  same address in its surface/texture area.
* Parhelia index `0xf0` (`0x13c0`) is not its main drawing control; M9 uses it
  for the raster/ROP control word.
* Parhelia render-target descriptors are at indices `0x8a/0x8b`; the current M9
  2D notes place surface descriptors at `0xb6..0xb9`, and even their source/dest
  roles were revised during hardware testing.

Therefore the Parhelia register whitelist and state shadow must never be used
for an M9 context.

## 3D pipeline comparison

| Block | Parhelia/P690 | M9/Phoenix |
|---|---|---|
| Vertex processing | WARP microcode engine, 12-byte instruction format, attribute-plane latch submission, canned fixed-function programs | A distinct programmable vertex engine.  The M-Series ICD contains hardware-T&L and hardware-GLSL paths; no Parhelia WARP END signatures occur in the MSerX binaries [BIN] |
| Fragment processing | Small DX8 ps.1.x-like combiner with two temporaries and two constants; Parhelia `GL_MTX_fragment_shader` lowering is now decoded | A separate programmable pixel-shader compiler.  MSerX exposes ARB fragment programs and GLSL 1.10 and has predicate registers `p0..p5`.  Static analysis recovers the compiler IR vocabulary, two-dword native instruction, four-instruction upload lines, exact native packer, three operation-selector fields, five source/resource descriptors, one result descriptor, result routing, `xy`/`zw` result-component modes, and component-mask scatter.  Native ALU/texture mnemonics and predicate/end fields still need differential recovery [BIN][DER] |
| Texture path | Four documented Parhelia pipes, sampler state and formats around `0x11c0..0x120c` | Four M9 texture-unit selectors (`0x70..0x73`) are recovered.  Index `7c` filter/aniso/wrap packing, `7d` LOD packing, `7e..81` border color, `82..85` bump matrix, compare-function mapping, common color/luminance/vector/float/depth/DXT formats, and the `78/79` descriptor extents are decoded.  The combined texture-operation/resource-class decision tree, 2D/cube/volume dimension join, visible class-3 block geometry, and ordinary volume-mip offset recurrence are exact.  The live D3D selector-2 view has no independent channel-swizzle field; physical class-3 and packed mip-tail permutations remain unresolved [BIN][DER] |
| Fixed function | Mesa currently lowers fixed-function GL to Parhelia-specific state and WARP/combiner programs | The vendor ICD lowers fixed function, ARB programs, and GLSL to M9 vertex/pixel programs; reuse the Mesa state tracker, then write M9-specific lowering |
| API evidence | Current open driver targets OpenGL 1.x behavior on Parhelia | M-Series ICD strings advertise GLSL, ARB vertex/fragment programs, FBO, VBO, occlusion query, draw buffers, NPOT, depth textures, cube/3D textures, S3TC, anisotropy, and multisample.  The live compiler caller supplies exact limits of 16 vertex attributes, 4 vertex samplers, 16 fragment samplers, 20 combined samplers, 512 vertex and fragment uniform components, 60 varying floats, and 4 draw buffers.  Strings prove driver paths, not that every advertised path works on every board |

The M9 shader command set is substantially more capable and structurally
different.  Porting the Parhelia fragment combiner encoder or WARP assembler
would be misleading.  NIR-to-M9 shader lowering and M9 program upload need a new
backend.  The existing Parhelia Gallium fixed-function handling, resource
lifetime code, draw decomposition, and software fallbacks remain useful at a
higher level.

## Existing prototype status and conflicting notes

The current `src/parhelia_m9_ring.c` and `src/parhelia_m9_exa.c` are experiments,
not a working specification:

* `parhelia_m9_ring.c` treats `0x400/0x404` as a writable VRAM ring start and
  producer doorbell.  Later M9148 tests found these writes did not provide a
  working submit path and pointed at the DMA-adapter descriptor/state instead.
* `parhelia_m9_exa.c` still offers direct-MMIO register emission, while the later
  `parhelia-m9-3d-submission.md` hardware result says the engine is DMA-list-only.
* `parhelia-m9-accel.md` contains successive, mutually exclusive corrections for
  whether `0xb6/0xb7` or `0xb8/0xb9` is the destination.  The last section says
  `0xb6/0xb7`; earlier tables and the C implementation still say `0xb8/0xb9`.
* `parhelia-m9-dma.md` records a vendor write to `0x400`, while hardware tests
  found `0x400/0x404` read-only from the experimental userspace path.  Static
  Linux kernel recovery resolves the role: privileged requester code programs
  `0x400` and rings `0x404`; direct DDX writes are not a substitute.

Do not copy these prototypes into DRM.  Use them as a list of experiments and
known register candidates, then replace them after a vendor command capture or
a complete static recovery.

## What is missing

No saved M9/Phoenix command-buffer capture was found under `captures/`.  This is
the most important missing artifact.  In particular, the tree lacks a known-good
M9 stream for a clear and one triangle, with its command-buffer physical/GPU
address, DMA descriptor image, register trace, fence word, and resulting VRAM
contents.

The following technical pieces are also not yet recovered well enough to write
a safe DRM submitter and a working Mesa driver:

1. The complete engine/global reset sequence.  PCI bus-master enable, requester
   initialization, descriptor quiescence, and requester-local save/restore are
   decoded, but the latter must not be treated as a 3D-engine reset.
2. The remaining `0x500..0x524` rollover fields and public memory-domain names.
   Static recovery now establishes the initializer formulas, the internal
   four-class address-tag table `{2,3,0,3}`, tagged base/transition addresses,
   a raw live `0x504` pointer, save/restore order and a vendor bounded-idle
   entry point.  `0x50c/0x510` are address-like above a low tag nibble;
   `0x514` is their companion and `0x520/0x524` are always restored first.  A
   controlled rollover trace is still required to name their producer/consumer
   roles and final relationship to the `0x404` submit limit.
3. The remaining M9 context-switch lifecycle.  The vendor's exact `0xf8`-byte
   full-state index template and its per-context restore-record builders are
   recovered, including texture and shader-store indices.  Query/fence state,
   restore submission ownership, and alternating-context completion still need
   validation.
4. Render-surface layout and concrete clear packets.  Four color pairs are
   joined to `b0..b7`, the two depth attachment pairs to `b8..bb`, descriptor
   format to bits `31:25`, and viewport/scissor to `f5/f6/c4/c5`.  The remaining
   descriptor pitch/tiling bits, ordinary-Z versus auxiliary-plane names, and
   final command generation below the joined color/depth clear leaves
   `0xa48b0`/`0xb4fe0` require further static recovery or capture.  Draw launch
   is statically joined to H03/H06/H07 records.
5. The final M9-index join for culling, polygon/shade mode, and multisampling,
   plus the final upload selector/port association for generated fragment
   programs.  Blend, alpha test, and format-dependent colour masks are now
   joined through their compiler routines to the final 256-byte native-program
   cache; depth/stencil is joined through per-batch shadows to final indices
   `a8/a9/aa`.  See `parhelia-m9-blend-raster-lowering.md`.  Query semantics
   remain less complete.
6. Texture cache flushes and the physical address permutations within class-3
   blocks and packed mip tails.  Descriptor formats, filter/wrap/LOD fields,
   2D/cube/volume selection, visible class-3 geometry, and ordinary volume-mip
   offsets are statically recovered.
7. Native vertex and pixel shader mnemonic joins, predicate encoding,
   constant-record meanings, physical varying routing, and compiler quirks.
   Logical GLSL stage linkage is recovered as an exact-name/type/size join into
   a 14-entry remap table.  The two-dword
   instruction, exact native packer and physical fields, source/result
   direction, result routing and component-pair modes, upload selector/ports,
   384-instruction/96-line bank boundary, compiler IR opcodes and modifiers,
   predicate names, and several resource limits are recovered in
   `parhelia-m9-3d-static-re.md`.
8. IRQ source bits for fault reporting and non-vblank service events.  Linux and
   Windows both use memory write-back fences rather than a completion IRQ.  The
   three status/W1C and enable-mask register pairs, four vblank source mappings,
   Windows event routes, and mask-before-ack order are recovered; the remaining
   source semantics are not.

There is also no M9 support in the current `linux-7.2` Parhelia DRM directory or
the current Mesa Parhelia driver.  PCI device `0540` is intentionally absent, so
probing an M9 cannot accidentally run Parhelia engine commands.

## Implementation order

1. Continue the `KernelLibraries.o_shipped` and `MxMM.sys` reset call graphs.
   Requester programming, memory-tag fences, IRQ banks, vblank routing, and a
   bounded requester-local restart contract are now recovered.  Do not promote
   that local restart to a global reset without another vendor join or capture.
2. Capture a vendor clear and triangle on M9.  Save the DMA buffers, descriptor
   bank `0x400..0x524`, engine/IRQ registers, fence memory, and the affected VRAM
   pages.  A capture resolves the contradictory ring and surface notes quickly.
3. Add M9 as a separate `parhelia_engine_ops` implementation in DRM, initially
   behind an explicit module option.  Validate and relocate M9 packet indices
   with an M9 whitelist.  Keep Parhelia and M9 contexts/state shadows separate.
4. Prove kernel-only fill, clear, and triangle self-tests, including memory-tag
   fence completion, independent IRQ source tests, timeout, requester restart,
   and two alternating contexts.
5. Add an M9 Mesa screen/backend which initially uses CPU-transformed vertices
   and minimal passthrough shaders.  Then recover textures, depth/blend, and the
   full shader ISA incrementally.

This order reuses the modern driver framework without assuming command-level
compatibility that the evidence does not support.
