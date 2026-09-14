# M9 kernel and Mesa driver readiness

The saved M9148 vendor stack now supplies a hardware reference for the first
Linux driver, rather than only a static design. Captures cover command
submission, a clear and triangle, all ordinary GL 1.x topologies, canonical
vertex repacking, program-pair selection, stage constants, fixed state, broad
texture targets and formats, FBO/query behavior, special GLSL results, and two
simultaneous direct-GLX clients. The remaining work divides into implementation
validation and a smaller set of hard native-record, blocked-layout, MRT,
multisample, GigaColor, sRGB, and recovery boundaries.

Confidence labels in the tables are:

* **closed**: sufficient static and/or captured evidence exists to implement;
* **captured**: the vendor behavior is preserved but the new driver still has
  to reproduce it;
* **validation**: the implementation rule is known and needs a hardware test;
* **capture**: one named live differential is still required;
* **unexposed**: the saved Linux ICD does not advertise the feature.

## First useful driver boundary

The following pieces no longer depend on another vendor capture:

* primary DMA32 command transport through exclusive interval `0x400/0x404`,
  typed relocations, the expanded writeback footer, 32-bit fence rollover, and
  fail-closed timeout handling;
* linear scanout and rendering allocations, color/depth descriptors, viewport,
  scissor, rectangle clear, and the two-pass preprocessing/raster draw;
* points, lines, line strips/loops, triangles, strips/fans, quads, quad strips,
  polygons, and H03/H06/H07 indexed/nonindexed launches;
* CPU repacking into four captured canonical vertex layouts and the exact
  generated-stream producer/consumer joins;
* depth test with writes independently enabled or disabled, one- and two-sided
  stencil packing, alpha test, blending, color masks, cull/fill/shade compiler
  inputs, and the fixed-state differential corpus;
* RGB565, RGBA4, RGB5A1, RGB8, RGBA8, L8, A8, LA8, I8, D16 and D32 textures,
  odd 1-D/2-D dimensions, full mip chains and packed tails, sampler state,
  four live sampler selectors, subimage/copy coherency, and the class-1
  render-to-sample dependency through index `43`.  The exact 40-bit PTE
  grammar and all class-3 calculator bodies/dispatches have deterministic
  vectors.  The mapped index-`78` image is repeated PTE-shaped metadata rather
  than linear texels; its table-root and physical-page ownership remain
  unjoined;
* separate index-`69` drawable/external and `6c` linked resources, atomic vertex plus
  fragment A/B/A selection, live stage-constant ranges, and the invariant
  nature of the 80-slot `8f/90..9f` store;
* context lifetime rules and a successful two-process test in which both
  direct contexts alternated distinct programs/colors and read back the
  expected result without state leakage.

These facts are enough for a guarded clear/triangle Gallium path and for the
fixed-function subset needed by ioquake3. They do not make arbitrary GLSL
safe: the `6c` resource is a heterogeneous linked record, and its universal
native start/count, stage split, entry selectors, and stop rule remain open.

The first DRM implementation should continue to omit the dormant auxiliary
`0x500..0x524` requester. Ordinary GL uses the primary `0x400/0x404` path;
the five hardware-owned auxiliary words and a forced-wrap test affect only a
later streaming path. Render completion is the coherent memory fence, not an
IRQ. The documented IRQ handler should enable the four vblank sources and
leave unproved service/fault sources masked.

## OpenGL 2.0 compatibility fixed function

`parhelia-m9-fixed-function-static-closure.md` closes the complete software
state before native-program selection: world/view/projection and eight texture
matrices; front/back materials; light, color-material, normalize/rescale and
local-viewer inputs; six clip planes; all texgen modes; fog and point fields;
the GL texenv/combine source/operand/scale shadow; 28 D3D texture-stage records
and all D3DTOP operations 1 through 26.  GL should expose eight lights even
though D3D publishes 12 and the internal record list holds 16.

Direct depth/stencil/raster state uses captured packets.  Transform, lighting,
texgen, fog, points, texenv and some fragment state use generated programs.
Selection, feedback and evaluator semantics belong in Mesa/Draw; resident
display-list meshes are an optional cache.  Fast pixel paths exist with exact
fallback conditions and shader pack/unpack built-ins, but their conversion,
zoom and overlap result matrix is still live work.  The five bounded fixtures
at the end of the static-closure document are the remaining compatibility
capture set.

## OpenGL 2.0 shader coverage

The live compiler is OpenGL 2.0 / GLSL 1.10. Its advertised limits are 16
vertex attributes, 4 vertex samplers, 16 fragment samplers, 20 combined
samplers, 512 uniform components in each stage, 60 varying floats, and 4 draw
buffers.  Static emission proves four full sampler slots and four generic
descriptor/route operands at `74..77`; D3D separately declares four
point-filtered vertex samplers.  A live shader with five dependent fragment
textures produces the exact result through four scratch render-target passes
and one final pass in one submission, proving compiler scheduling beyond the
physical slot count.  The table distinguishes compiler acceptance from a
native encoding or an implemented scheduling limit.

| GLSL/ARB family | evidence now | driver status | smallest remaining check |
|---|---|---|---|
| MOV, ADD, SUB, MUL, MAD, DP3/4, MIN/MAX, RCP/RSQ, EXP2/LOG2, FLOOR | executable hash-pinned lowerer fixtures plus live compile/link/draw | **closed** for canonical forms | expand only when a new modifier/result class is needed |
| absolute/negate, swizzle, write masks, scalar predicates | 91-case native lowering matrix and live draw | **closed** for recorded forms | reject unrecorded combinations |
| matrices M4x4/M4x3/M3x4/M3x3/M3x2 | exact static expansion to 4/3 DP4 and 4/3/2 DP3 rows, including alias MOV | **closed** | one `mat4*vec4` regression is sufficient |
| KIL/discard | dedicated pairwise comparison lowerer plus live conditional and unconditional discard results/resources | typed behavior **captured**, packed form **capture** | identify the conditional KIL native slots inside the heterogeneous linked record |
| DDX/DDY | backend selectors 91/92 plus isolated live `dFdx` and `dFdy` results/resources | **captured**, native-slot join open | finish operation-specific instruction ownership in the linked record |
| TEXLD ordinary 2D | static lowerer, live single-level and mipmapped texture draws | **closed** for captured 2D forms | none for initial path |
| TEXLDB / projected TEXLD | exact one-hot lowerer modes plus live bias and projected result/resources | **captured** | name the final native mode field |
| TEXLDL / TEXLDD | vertex explicit LOD has a live result/resource; fragment explicit LOD and gradient are rejected because the required extension is absent | vertex **captured**; fragment **unexposed** | join the vertex native instruction and resource selector |
| cube texture | all six faces and orientation probes pass; descriptor/resource dimension is captured | **captured** | mip/seam/incomplete cases and physical face layout |
| shadow comparison | exact sampler enable/eight compare codes plus live raw 0.25/0.75 and LEQUAL fail/pass results | **captured** for the tested D24/nearest route | filtering, result-channel variants, and FBO producer dependency |
| vertex texture fetch | four-sampler compiler limit plus live vertex `texture2DLod` result/resource | **captured** | native instruction and vertex sampler join |
| SLT/SGE and comparisons | CMP/SETP frontend plus live ARBfp SLT/SGE zero/one results | **captured** for the recorded pair | exact native predicate fields and all relational outcomes |
| constant/uniform branches | live constant and uniform conditional results/resources | **captured** | distinguish folding, native branch, and reconvergence |
| fixed/dynamic loops, BREAK/CALL | live fixed and uniform-bound loop results plus static BREAK/CALL frontend | loops **captured**; BREAK/CALL native form open | targets/backedges, nesting, BREAK/CALL results and implementation limits |
| uniforms/constants | vertex and fragment A/B/A sentinels identify live-component ranges in the 0x21000 resource | **closed** for external range path | mode-2 inline constant bank remains optional |
| program entry/resource ownership | `69` is live drawable/resource backing; `6c` selects a tagged 0x80-aligned heterogeneous linked record through an exact constructor/translator/cache chain | external bind identity **closed** | universal native start/count, VS/FS split, entry selectors, allocator wrap and lifetime |
| termination | count-delimited compiler stream, endpoint selector `0x7e`, and complete terminal slot are exact | **validation** | determine endpoint-only versus full-terminal stop on generated programs |

`tools/m9-gl2-compiler-control-evidence.py` pins the MxMD and MxMGLX hashes,
the complete 55-entry compiler dispatch, special lowerer call targets, matrix
call counts, derivative marker selector, and extension-token inventory. Its
JSON output is the compact machine-readable static matrix.

## Vendor extension and hardware-feature coverage

This table uses the exact live inventory in
`captures/m9x-linux-gl2-context-20260914/glxinfo-l.txt`. A name embedded in
`MxMGLX.dll` is only binary vocabulary: it is
not marked advertised unless the running M9148 ICD returned it. This matters
for S3TC and texture-float, whose names and hardware formats exist but which
the live Linux ICD did not advertise.

| feature / public path | live M9 ICD | hardware and capture evidence | driver boundary |
|---|---|---|---|
| `GL_EXT_framebuffer_object` | advertised | missing/dimension errors, depth-only, stencil-only, RGBA8+depth, render-to-sample dependency, and readback pass; distinct color+depth+stencil is rejected | **captured** for tested linear attachments; packed depth-stencil and lifecycle replay remain |
| `GL_ARB_draw_buffers` / MRT | advertised, max 4 | four address/descriptor pairs and output compilers are static; RT0-only works, but RT1/all-target routes wedge during later readback/teardown | **unsafe beyond RT0** until routing and resource retirement are recovered |
| `GL_ARB_occlusion_query` | advertised, 32-bit counter | exact packet plus live 16384/8192/0 visible/partial/hidden counts, pre-finish availability, and reuse | **captured** for ordinary single-sample use; wrap and MSAA scaling remain |
| `GL_ARB_multisample` / FAA | advertised; current GLX visuals report zero sample buffers | Windows selects type 8/eight quality levels; `ad[20:0]` is allocation bytes/32, `af` sources and GL query post-scaling are exact, and zero mask suppresses color | **capture** allocation growth/layout, sample positions/pattern, nonzero masks, resolve, present and physical counter unit |
| 10-bit GigaColor (`A2B10G10R10`, `A2R10G10B10`) | no dedicated GL extension observed | exact codes `0x68/0x6a`, masks, sampler/RT descriptors and rounded native clear packers; Linux `GL_RGB10_A2` explicitly downgrades to `GL_RGBA8` | Linux route **not native**; capture gated 10-bit render/blend/sample/backing/readback and matching visual before exposure |
| framebuffer/texture sRGB | not advertised | Windows state has `SRGBWRITEENABLE` and an sRGB clear/surface flag | **unexposed/capture**: linear-vs-sRGB RT and texture A/B; keep CRT gamma LUT separate |
| NPOT 2D | advertised | complete 13x7/6x3/3x1/1x1 chain, base/max selection, and defined incomplete-texture behavior are captured | **captured**; automatic generation remains |
| rectangle textures | ARB/EXT/NV names advertised | unnormalized sampling passes; level 1, repeat, and mip filters produce required errors | **captured** |
| cube textures | ARB/EXT advertised | all six face/orientation tests pass with exact descriptor/resource dimension | **captured**; seams, mips, and physical face layout remain |
| 3D textures | `GL_EXT_texture3D` advertised | 7x5x4 slices and R repeat pass; exact volume descriptor captured | **captured**; volume mips and physical slice layout remain |
| depth texture and shadow | depth/shadow extensions advertised | raw 0.25/0.75 plus LEQUAL fail/pass results and exact descriptor/control delta | **captured** for D24 nearest; filtering/FBO dependency remain |
| S3TC/DXT1-5 | not advertised by live ICD | hardware codes/layouts are static; direct DXT1/DXT5 tokens reject, and the core compressed-format query reports four but leaves its output array untouched | **unexposed** pending a valid public token and result capture |
| VBO | advertised | indexed H06/H07 and CPU canonical repacking are captured | **closed** with CPU repack; server-side zero-copy layout is optional |
| PBO | not advertised and no `GL_ARB_pixel_buffer_object` token in pinned ICD | no M9-specific asynchronous pixel-transfer join | **unexposed**; Gallium staging copies may still back core transfers |
| separate/two-sided stencil | `GL_ATI_separate_stencil` advertised; `GL_EXT_stencil_two_side` absent | exact face controls and live separate-op packet; the vendor separate-function call rejects valid LESS/GREATER enums | implement correct core behavior from final state; do not copy the vendor API bug |
| point sprites | `GL_ARB_point_sprite` advertised | point topology/expansion and real GL_POINTS result captured; `gl_PointCoord` is rejected by the GLSL frontend | fixed-function coord replacement/attenuation and exact coverage remain |
| fog coordinates | `GL_EXT_fog_coord` advertised | fog modes and explicit fog-coordinate result/state are captured | **captured** for representative modes; implementation replay remains |
| blend add/subtract/reverse/min/max | minmax/subtract/separate extensions advertised | separate factors/equations and representative destination-sensitive results are captured; static mapping covers operations 1..5 | complete visible equation/factor matrix before broad exposure |
| anisotropic filtering | not advertised by the live ICD (`max=0`) | exact hardware anisotropy classes 1..16 exist in the Windows sampler packer | **unexposed/capture** before enabling |
| float textures | not advertised | R16F/RG16F/RGBA16F and 32-bit format codes exist | **unexposed/capture** format upload/sample and filter restrictions |

## Exact remaining blocker ledger

| remaining gap | consequence | smallest closing experiment |
|---|---|---|
| universal native program grammar inside heterogeneous `6c` records | arbitrary newly compiled GLSL cannot execute safely | use the passive constructor/translator/cache hook to join native start/count, VS/FS ownership, entry selectors, stop rule and allocator lifetime to one-stage A/B programs |
| special shader packed forms: KIL, derivatives, texture controls, predicates, branches and loops | visible behavior is captured but a new compiler cannot emit every form yet | join each preserved isolated draw to the universal native-record grammar and pin operation-specific slots |
| four-target MRT output routing and retirement | draw buffers beyond RT0 remain unsafe | trace the RT1/readback/teardown resource path statically or with a passive hook before any more live multi-target stress |
| multisample/FAA allocation, pattern, mask and resolve | no sample buffers and no FAA cap | acquire a vendor multisample visual/surface, then clear/edge/mask/query differentials |
| native 10-bit render and sample round trip | Linux GL downgrades RGB10_A2 to RGBA8, so GigaColor formats cannot be advertised | enable the gated Windows path and capture A2B10/A2R10 clear, draw, sample, raw storage and presentation with sentinel channels |
| sRGB write/sample conversion | framebuffer sRGB remains off; output LUT semantics stay separate | identical linear/sRGB RT and texture fixtures, compare pre-LUT BO contents |
| query wrap and multisample scaling | ordinary visible/hidden/partial counts and reuse are closed | slot wrap, 32-bit counter wrap, ordering stress, and a sampled-surface comparison |
| auxiliary requester wrap and five hardware-owned words | optional streaming path remains absent | one forced auxiliary wrap with four timestamped descriptor snapshots |
| transparent 3D reset and fault IRQ | timeout recovery remains fail-closed | controlled bad address/opcode tests with complete pre-ack snapshots |
| class-3 physical layout and backing join | all 35 dispatch/body routes are pinned and 32 tiled routes have 352 golden coordinate vectors; maintainable formulas exist for the four 32-bit routes | transcribe the remaining element/DXT families, then capture one page-table root plus decoded coordinate-coded physical pages, including cube faces and volume slices |

The vendor capture gates are now separate from new-driver validation. The new
kernel/Mesa stack still has to replay a no-op fence, clear, triangle, mipmapped
texture, FBO chain, and two-client workload before default enablement. Those
tests validate implementation; they are no longer requests for missing vendor
behavior.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
