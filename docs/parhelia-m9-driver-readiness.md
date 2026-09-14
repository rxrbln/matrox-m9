# M9 kernel and Mesa driver readiness

The saved M9148 vendor stack now supplies a hardware reference for the first
Linux driver, rather than only a static design. Captures cover command
submission, a clear and triangle, all ordinary GL 1.x topologies, canonical
vertex repacking, program-pair selection, stage constants, ordinary sampler
and mip state, an FBO render-to-sample chain, and two simultaneous direct-GLX
clients. The remaining work divides cleanly into implementation validation and
optional GL2/extension paths whose final native form still needs a small
capture.

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
* ordinary linear RGBA8 textures, the full mip chain and packed mip tail,
  sampler wrap/filter/LOD packing, four live sampler selectors, and the
  class-1 render-to-sample dependency through index `43`;
* separate `69` program heap and `6c` linked-pair resources, atomic vertex plus
  fragment A/B/A selection, live stage-constant ranges, and the invariant
  nature of the 80-slot `8f/90..9f` store;
* context lifetime rules and a successful two-process test in which both
  direct contexts alternated distinct programs/colors and read back the
  expected result without state leakage.

These facts are enough for a guarded clear/triangle Gallium path and for the
fixed-function subset needed by ioquake3. They do not make arbitrary GLSL
safe: native instruction packing for several special operations and the
internal format of the vendor linked-program resources remain capture gates.

The first DRM implementation should continue to omit the dormant auxiliary
`0x500..0x524` requester. Ordinary GL uses the primary `0x400/0x404` path;
the five hardware-owned auxiliary words and a forced-wrap test affect only a
later streaming path. Render completion is the coherent memory fence, not an
IRQ. The documented IRQ handler should enable the four vblank sources and
leave unproved service/fault sources masked.

## OpenGL 2.0 shader coverage

The live compiler is OpenGL 2.0 / GLSL 1.10. Its exact limits are 16 vertex
attributes, 4 vertex samplers, 16 fragment samplers, 20 combined samplers,
512 uniform components in each stage, 60 varying floats, and 4 draw buffers.
The table distinguishes compiler acceptance from a native encoding.

| GLSL/ARB family | evidence now | driver status | smallest remaining check |
|---|---|---|---|
| MOV, ADD, SUB, MUL, MAD, DP3/4, MIN/MAX, RCP/RSQ, EXP2/LOG2, FLOOR | executable hash-pinned lowerer fixtures plus live compile/link/draw | **closed** for canonical forms | expand only when a new modifier/result class is needed |
| absolute/negate, swizzle, write masks, scalar predicates | 91-case native lowering matrix and live draw | **closed** for recorded forms | reject unrecorded combinations |
| matrices M4x4/M4x3/M3x4/M3x3/M3x2 | exact static expansion to 4/3 DP4 and 4/3/2 DP3 rows, including alias MOV | **closed** | one `mat4*vec4` regression is sufficient |
| KIL/discard | dedicated pairwise comparison lowerer; conditional discard compiles, links, and draws | typed lowering **closed**, packed form **capture** | unconditional versus conditional linked-program delta |
| DDX/DDY | backend selectors 91/92, common lowerer, expanded markers `0x88/0x89` | **capture** | one isolated `dFdx`, one isolated `dFdy` |
| TEXLD ordinary 2D | static lowerer, live single-level and mipmapped texture draws | **closed** for captured 2D forms | none for initial path |
| TEXLDB / projected TEXLD | exact one-hot lowerer modes; bias and projection compile/link/draw | **captured** | linked resource/program delta to name the final mode field |
| TEXLDL / TEXLDD | exact one-hot lowerer routes, no decisive live resource join | **capture** | isolated explicit-LOD and explicit-gradient draws |
| cube texture | compiler accepts and draws; cube resource dimension mapping is static | **captured** | preserve descriptor/resource snapshot in an isolated cube case |
| shadow comparison | exact sampler enable and eight compare codes | **capture** | one `sampler2DShadow` pass/fail pair and result-channel join |
| vertex texture fetch | four-sampler compiler limit and stage-neutral texture lowerer | **capture** | vertex `texture2DLod` with resource and generated-stream snapshots |
| SLT/SGE and comparisons | frontend acceptance plus CMP/SETP family | **capture** | SLT/SGE A/B to assign nonzero six-way predicate controls |
| constant/uniform branches | conditional-stack frontend is exact | **capture** | constant `if` versus uniform `if` to distinguish folding and native branch |
| fixed/dynamic loops, BREAK/CALL | frontend loop, BREAK, and CALL paths are exact | **capture** | fixed-count versus uniform-bound loop with linked-program bytes |
| uniforms/constants | vertex and fragment A/B/A sentinels identify live-component ranges in the 0x21000 resource | **closed** for external range path | mode-2 inline constant bank remains optional |
| program entry/resource ownership | `69` heap, `6a/6d` entries, separate aligned `6c` linked pair, tag bit 0, and whole-pair A/B/A selection | external bind **closed** | internal native storage and the two stage entries inside the linked resource |
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
| `GL_EXT_framebuffer_object` | advertised | RGBA8 FBO completeness, render, dependency, sample, and readback all pass | **closed** for linear color render-to-sample; capture depth/stencil FBO transition |
| `GL_ARB_draw_buffers` / MRT | advertised, max 4 | four address/descriptor pairs `b0..b7`, four output masks and per-target fragment compilation are static | **capture** one four-color-attachment draw/readback before enabling MRT |
| `GL_ARB_occlusion_query` | advertised, 32-bit counter | exact begin/end snapshot packet, modulo result and completion contract | **validation** behind an experimental cap: visible count, alignment, slot reuse, MSAA scaling |
| `GL_ARB_multisample` / FAA | advertised; current GLX visuals report zero sample buffers | enable reaches four `ad` writes plus `af`; zero mask suppresses color | **capture** a real multisample visual/surface, sample pattern/resolve, and nonzero partial mask |
| 10-bit GigaColor (`A2B10G10R10`, `A2R10G10B10`) | no dedicated GL extension observed | exact hardware format codes `0x68/0x6a` and surface/texture descriptor paths | **capture** 10-bit RT clear/draw/readback and texture sample; expose only matching visual/FBO formats |
| framebuffer/texture sRGB | not advertised | Windows state has `SRGBWRITEENABLE` and an sRGB clear/surface flag | **unexposed/capture**: linear-vs-sRGB RT and texture A/B; keep CRT gamma LUT separate |
| NPOT 2D | advertised | descriptors/layout accept arbitrary dimensions; no isolated live NPOT draw preserved | **validation** with coordinate-coded odd dimensions and mip tail |
| rectangle textures | ARB/EXT/NV names advertised | compiler/resource family exists | **capture** rectangle coordinates, wrap restrictions, and descriptor delta |
| cube textures | ARB/EXT advertised | live GLSL cube compile/link/draw plus exact six-face constructor and dimension kind | **captured**, enable after isolated face/orientation result test |
| 3D textures | `GL_EXT_texture3D` advertised | exact volume constructor, depth-halving recurrence and dimension kind | **capture** slice-addressing and mip sampling |
| depth texture and shadow | depth/shadow extensions advertised | depth formats and exact sampler compare packing | **capture** sampled depth format plus compare/no-compare A/B |
| S3TC/DXT1-5 | not advertised by live ICD | binary contains extension name; hardware codes and DXT mip/block offsets are exact | implement format support, then **capture** DXT1 and DXT5 before exposing |
| VBO | advertised | indexed H06/H07 and CPU canonical repacking are captured | **closed** with CPU repack; server-side zero-copy layout is optional |
| PBO | not advertised and no `GL_ARB_pixel_buffer_object` token in pinned ICD | no M9-specific asynchronous pixel-transfer join | **unexposed**; Gallium staging copies may still back core transfers |
| separate/two-sided stencil | `GL_ATI_separate_stencil` advertised; `GL_EXT_stencil_two_side` absent | exact two face compare/op quartets and two ref/mask words `a8..aa` | **validation** with opposite front/back outcomes |
| point sprites | `GL_ARB_point_sprite` advertised | point topology/expansion and five generated streams captured | **capture** coord replacement, size attenuation and raster result |
| fog coordinates | `GL_EXT_fog_coord` advertised | compiler vocabulary/path exists; no isolated M9 final-state join | **capture** depth fog versus explicit fog-coordinate A/B |
| blend add/subtract/reverse/min/max | minmax/subtract/separate extensions advertised | factor dispatch, operations 1..5 and per-target native compilation static; ADD/SUB fixed-state draws captured | **captured** for ADD/SUB; capture MIN/MAX/REV_SUB result before broad exposure |
| anisotropic filtering | not advertised by the live ICD (`max=0`) | exact hardware anisotropy classes 1..16 exist in the Windows sampler packer | **unexposed/capture** before enabling |
| float textures | not advertised | R16F/RG16F/RGBA16F and 32-bit format codes exist | **unexposed/capture** format upload/sample and filter restrictions |

## Exact remaining blocker ledger

| remaining gap | consequence | smallest closing experiment |
|---|---|---|
| internal native instruction storage and vertex/fragment entry fields inside the `69`/`6c` resources | arbitrary newly compiled GLSL cannot execute | capture compiler-owned program heap and linked record for one-stage A/B programs, without copying raw addresses |
| special shader packed forms: DDX/DDY, TEXLDL/TEXLDD, shadow, vertex texture, comparisons, branches and loops | corresponding GL2 forms must fail compilation | the isolated probes listed in the GL2 table, recording compile/link/use/draw, `69/6a/6b/6c`, full 0x21000 resource and program heap |
| four-target MRT output routing | draw-buffers cap remains off | one shader writing four unique colors into four RGBA8 attachments, then read each target |
| multisample/FAA allocation, pattern, mask and resolve | no sample buffers and no FAA cap | acquire a vendor multisample visual/surface, then clear/edge/mask/query differentials |
| 10-bit render and sample round trip | GigaColor formats cannot be advertised safely | A2B10 and A2R10 clear/draw/read/sample with sentinel channel values |
| sRGB write/sample conversion | framebuffer sRGB remains off; output LUT semantics stay separate | identical linear/sRGB RT and texture fixtures, compare pre-LUT BO contents |
| query result details | occlusion query remains experimental | known visible sample count, snapshot alignment/second word, slot wrap, counter wrap, MSAA |
| auxiliary requester wrap and five hardware-owned words | optional streaming path remains absent | one forced auxiliary wrap with four timestamped descriptor snapshots |
| transparent 3D reset and fault IRQ | timeout recovery remains fail-closed | controlled bad address/opcode tests with complete pre-ack snapshots |
| class-3 physical in-block permutation | tiled/blocked resources remain unavailable | coordinate-coded upload/readback for one element size per layout family |

The vendor capture gates are now separate from new-driver validation. The new
kernel/Mesa stack still has to replay a no-op fence, clear, triangle, mipmapped
texture, FBO chain, and two-client workload before default enablement. Those
tests validate implementation; they are no longer requests for missing vendor
behavior.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
