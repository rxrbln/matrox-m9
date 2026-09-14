# M9 OpenGL 2.0 conformance evidence audit

This checklist prevents the preserved vendor installation from being retired
after only the ioquake3/OpenGL 1.x path has been captured.  It inventories the
hardware evidence needed for an OpenGL 2.0 and GLSL 1.10 implementation.  It
does not treat a vendor extension string, compiler acceptance, or a static
packer as a successful rendering test.

The bounded static pass in `parhelia-m9-opengl2-static-closure.md` and its
verifier closes compiler/API reachability while retaining the live-result
boundary used here.  It enumerates compiler operations, built-in semantic
records, uniform entrypoints, sampler families, Direct3D state handlers, and
all sixteen logic-op frontend codes.

`parhelia-m9-d3d-caps-audit.md` preserves the independent Windows D3D8/D3D9
capability builders.  D3D9 can publish VS/PS 3.0, predication, 24 levels of
dynamic flow control, 32 temporary registers per stage, 512 pixel-shader
slots, four render targets, vertex texture point sampling, NPOT textures, and
anisotropy 16.  D3D8 clamps the same backend to VS 1.1 and PS 1.4.  These are
valuable upper-bound and compiler-reachability evidence, but remain
declarations until their generated records and visible results are joined.

The evidence states used below are:

* **result**: a controlled draw/readback demonstrated the API result;
* **packet**: the final command, resource, or native-program delta is saved;
* **static**: a hash-pinned Linux or Windows binary establishes the path;
* **API-only**: the old ICD accepted the call, without a decisive result or
  hardware-state delta;
* **open**: the new driver still needs a named capture or implementation test.

## Core state, rasterization, and framebuffer operations

| OpenGL 2.0 area | preserved evidence | remaining work before exposure |
|---|---|---|
| viewport, depth range, scissor | result/packet for nontrivial rectangles and depth range | implementation replay |
| color/depth/stencil clear and masks | packet/static; color mask and complete depth/stencil fields | validate every advertised depth/stencil format and partial clear masks |
| FBO completeness and render-to-texture | result/packet for missing attachment, dimension mismatch, depth-only, stencil-only, color+depth, and producer-to-consumer sampling | packed color+depth+stencil combinations, deletion/rebinding, and new-driver replay |
| all eight depth comparisons; depth write independently disabled | result/packet | implementation replay |
| front/back stencil compare, reference, masks, and eight operations | static/packet; ATI separate op path captured; vendor separate-function call is broken | validate core `glStencil*Separate` behavior in the new implementation |
| culling, winding, smooth/flat shade | packet/result | implementation replay |
| point, line, triangle, strip, fan, quad, quad-strip, polygon topology | packet/result for the ordinary frontend set; D3D caps allow 1,048,575 primitives and indices | implementation replay and degenerate/batch-boundary tests |
| polygon point/line/fill, point/line width, smoothing | packet/result for representative sizes; D3D publishes maximum point size 2048 | width ranges, line stipple, point attenuation, and exact coverage rules |
| polygon offset | enable packet/static; D3D slope and units handlers carry raw floats through `0x104267/0x1042a9` | join OpenGL factor/units to the generated depth equation and visible quantized result |
| alpha test, blending, separate RGB/alpha, min/max/subtract/reverse subtract | enable/result/static factor and equation maps | join generated native state to visible destination-sensitive results |
| all logic operations | static frontend: all sixteen GL enums map to exact ICD internal codes; XOR packet/result | join the other fifteen internal codes to final native state/program and visible results |
| fog, explicit fog coordinate, clip planes, two-sided lighting | API/result plus generated-state snapshots | identify native program/constant deltas after the linked-record grammar is decoded |
| multisample and sample coverage | API-only on an unsampled visual; static `ad/af` emitter | sampled allocation, pattern, mask, resolve, coverage invert, and query scaling |
| occlusion query | result/packet for 16384 visible, 0 hidden, 8192 half-hidden, object reuse, and pre-finish availability zero; static begin/end/result protocol | slot wrap, counter wrap, ordering stress, and implementation replay |
| draw buffers/MRT | static four-target address/mask/compiler paths; masks 0/1 complete and mask 1 writes RT0; mask 2 and mask 15 expose a repeatable vendor teardown/readback wedge after successful finish | recover safe per-target routing/teardown before testing or exposing masks beyond RT0 |

## Textures, samplers, and pixel transfer

| OpenGL 2.0 area | preserved evidence | remaining work before exposure |
|---|---|---|
| 1-D and 2-D integer/normalized formats | static format table, sampler/prototype families and layout; ordinary RGBA8 result/packet | phase-two 1-D and per-format swizzle/type normalization matrix |
| NPOT 2-D | result/packet for a complete 13x7, 6x3, 3x1, 1x1 chain, including exact logical dimensions | implementation replay and incomplete-chain validation |
| mipmaps and generated mipmaps | result/packet for a complete odd NPOT chain, selected levels, tail, signed bias and clamped LOD | base/max-level boundaries, incomplete behavior, and automatic generation |
| nearest/linear and six minification filters | packet | visible LOD/footprint validation |
| S/T wrap modes and border color | packet plus result for repeat and a nontrivial border color; mirror/edge selectors are static/packet | 1-D behavior and exact border-color packing |
| LOD bias, min LOD, max LOD | packet/result for negative bias, both signed saturation limits, min 3, and max 2 | implementation replay |
| cube maps | result/packet for all six faces and orientation checks; static six-face constructor | seams, mip selection, and incomplete cube behavior |
| 3-D textures | result/packet for a 7x5x4 volume, all slices, and R repeat | mip recurrence, edge coordinates, and physical slice layout |
| rectangle textures | result/packet for unnormalized coordinates; level 1, repeat, and mip filter produce the required errors | implementation replay |
| depth textures and shadow compare | result/packet for raw 0.25/0.75 and LEQUAL fail/pass; static formats and eight compare codes | result-channel/filter matrix and FBO producer dependency |
| texture compression | generic extension advertised; static DXT1-5 packers; Linux S3TC absent and direct DXT1/DXT5 uploads return `GL_INVALID_ENUM` | sentinel-filled core-format query; keep DXT disabled unless a returned public format passes upload/sample/readback |
| texture objects, subimages, copies, priorities/residency semantics | ordinary resource lifecycle/static copy path | update-then-sample coherency, overlapping copies, subimage/mip boundaries |
| pixel-store packing/unpacking and readback | basic readback | all core alignment/row-length/skip/type conversions; performance is secondary |
| 10-bit GigaColor | static A2B10G10R10/A2R10G10B10 codes and config gates; Linux `RGB10_A2` upload/sample/readback demonstrably aliases RGBA8 with round-to-nearest 10->8->10 quantization | a Windows-enabled or gated native 10-bit render/sample capture, raw VRAM validation, and separate depth-30 scanout test |
| float, YUV, anisotropic, and sRGB textures | real dormant Windows packers/config paths | remain extension-gated until upload/sample/filter/render result matrices exist |

## Buffers, arrays, and draw submission

| OpenGL 2.0 area | preserved evidence | remaining work before exposure |
|---|---|---|
| client arrays and sixteen generic attributes | static declaration grammar, complete ICD generic-attribute entrypoint vocabulary, four canonical live layouts, and D3D declarations for normalized short/ushort/ubyte and float16 types | visible conversion matrix, stride/offset, disabled current value, and alias rule |
| VBO creation/update/map/unmap | advertised; vendor resource/fence lifecycle and captured VBO draw path | orphaning, partial updates, map failure, deletion while referenced, and context sharing |
| 16/32-bit element buffers and draw ranges | packet/static H06/H07 paths | base/range boundaries, restart absence, mixed index sizes, and zero-count draws |
| immediate mode and display lists | live fixed-function workloads | state capture/replay and resource lifetime in the Mesa implementation |
| multiple contexts/share groups | result/packet for simultaneous A/B/A/B direct contexts | shared texture/VBO/program deletion and concurrent update ordering |
| command completion and errors | coherent fence/static lifetime path | bounded wrap, rollover, OOM, invalid command rejection, hang/device-loss behavior |

## GLSL 1.10 language and programmable pipeline

Compiler acceptance is insufficient here.  Each native family needs its
instruction bytes, stage ownership, entry/count fields, constants/resources,
and a visible result joined in the same fixture.

| GLSL/program area | preserved evidence | remaining work before exposure |
|---|---|---|
| shader create/source/compile/link/validate/use/delete and info logs | live compiler matrix | negative tests, deferred deletion, relink, detach, and cross-context sharing |
| scalar/vector/matrix bool, int, float types; constructors and conversions | static aggregate/type descriptor, integer frontend vocabulary, all 19 uniform update families, and one live program combining arrays, integers, booleans, mat4, VS and FS uniforms | systematic visible-result matrix; lower unrecorded integer/bool combinations conservatively |
| arithmetic, swizzles, write masks, abs/negate, min/max, reciprocal/rsqrt, exp/log, floor | 91-case native lowerer corpus | join generated bytes to the live linked-program record and reject unrecorded native combinations |
| dot products and matrix/vector multiplication | static exact DP3/DP4 expansion; live `mat4 * vec4` result | non-square matrix variants and alias cases in live native storage |
| comparisons, boolean logic, selection | static CMP/SETP plus live ARBfp SLT/SGE and GLSL conditional results | exact native predicate fields and all six relational outcomes |
| constant/uniform conditionals | live result captures for both constant and uniform branches | identify folded versus native branch representation and reconvergence |
| fixed and uniform-bound loops, break/continue/function calls | live fixed- and uniform-loop results plus static frontend paths | native/unrolled policy, targets/backedges, nesting and implementation limits |
| vertex/fragment uniforms and arrays | A/B/A stage-specific resource/result captures; exact limits | matrices, integer/bool uniforms, arrays, partial updates, locations after relink |
| attributes, varyings, built-ins, invariant matching | static 14-entry varying map and exact semantic records; live `gl_FrontFacing` produces distinct front/back colors; `gl_PointCoord` is rejected by the GLSL 1.10 frontend | remaining visible built-in routes, type/size mismatch failures, interpolation slots, and fixed-function point-sprite lowering |
| ordinary, projected, and bias texture lookup | live result captures plus static texture lowerer; explicit fragment LOD/gradient correctly rejected without the extension | exact native mode/resource join |
| vertex explicit-LOD lookup | accepted live result capture under the four-vertex-sampler limit | native instruction and vertex sampler/resource join |
| derivatives | live `dFdx`/`dFdy` results and static DDX/DDY selectors | isolate nonconstant X/Y gradients and finish their native-slot join |
| discard/KIL | live conditional and unconditional discard results plus static KILL lowerer | locate packed conditional KIL slots and validate helper/coverage behavior |
| fragment depth output | live `gl_FragDepth=0.25` color/depth result plus static one-component semantic `0x13` and result-routing vocabulary | native output-route join and later-depth-test ordering replay |
| four fragment color outputs | runtime-sized `gl_FragData[gl_MaxDrawBuffers]` constructor, static target compiler/emitter, and live RT0 result; RT1/all-target attempts finish then wedge during readback/teardown | recover safe targets 1--3 routing and retirement before another result capture |
| vertex position, point size, clip vertex; fragment color/depth built-ins | exact logical semantics `0x0d..0x13`, component counts and stage presence masks; partial fixed-function/point evidence | direct GLSL result fixtures to join every logical semantic to its native packed route |
| compiler resource limits | live advertised limits: 16 attributes, 4 vertex samplers, 16 fragment samplers, 20 combined, 512 uniform components per stage, 60 varying floats, 4 draw buffers; active `vec4[128]` and erroneous limit+1 `vec4[129]` both link; D3D9 publishes 256 VS vec4 constants, 32 VS/PS temporaries, 512 PS slots, and four MRTs | recover actual native allocation limits and enforce a truthful new-driver limit instead of reproducing the vendor link bug |
| program transport and termination | static two-dword slots, inline upload, terminal form; live `0x6c` and full system-resource snapshots | finish record grammar, native storage/entries/counts, endpoint semantics, and relocations |

## Extension and vendor-only retention

The hardware also has paths outside the OpenGL 2.0 core contract.  Preserve
them for later Gallium capabilities without letting them inflate the core
conformance claim:

* native 10-bit GigaColor rendering and its no-page-flip presentation restriction; the Linux GL path's `RGB10_A2` alias to RGBA8 is an explicit negative result, not native 10-bit evidence;
* floating-point texture formats, YUY2/UYVY sampling/video paths, DXT1-5, and
  dormant anisotropy levels 1/4/8/16;
* render-target sRGB conversion, with sampler-side sRGB still unlocated;
* four-target MRT, hardware occlusion counters, and the incomplete FAA/MSAA
  path;
* the D3D9 SM3 compiler surface: predication, long dynamic flow, 32 temporary
  registers per stage, vertex texture fetch, and substantially larger declared
  shader limits than the Linux GLSL 1.10 frontend;
* point sprites, wide/smooth/stippled lines, stereo/layer planes, overlays,
  VPE/DXVA, tear-free/page-flip policy, and independent back buffers;
* conservative serialized context switching, resource fences, hardware copy,
  and the primary requester; no fast-context-switch, fast-trilinear,
  multiple-pipe, or volumetric-shadow M9 path is present in the pinned images.

The aging vendor installation can be retired only after every item in the
remaining-work column has either preserved live evidence, conclusive static
proof that no public path exists, or an explicit decision to keep the
corresponding capability disabled.  New-driver replay tests are still required
after capture; preserved vendor behavior does not prove the new kernel/Mesa
implementation.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
