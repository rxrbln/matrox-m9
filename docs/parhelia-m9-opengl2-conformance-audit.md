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
| all eight depth comparisons; depth write independently disabled | result/packet | implementation replay |
| front/back stencil compare, reference, masks, and eight operations | static/packet; ATI separate op path captured; vendor separate-function call is broken | validate core `glStencil*Separate` behavior in the new implementation |
| culling, winding, smooth/flat shade | packet/result | implementation replay |
| point, line, triangle, strip, fan, quad, quad-strip, polygon topology | packet/result for the ordinary frontend set | implementation replay and degenerate/batch-boundary tests |
| polygon point/line/fill, point/line width, smoothing | packet/result for representative sizes | width ranges, line stipple, point attenuation, and exact coverage rules |
| polygon offset | enable packet/static; D3D slope and units handlers carry raw floats through `0x104267/0x1042a9` | join OpenGL factor/units to the generated depth equation and visible quantized result |
| alpha test, blending, separate RGB/alpha, min/max/subtract/reverse subtract | enable/result/static factor and equation maps | join generated native state to visible destination-sensitive results |
| all logic operations | static frontend: all sixteen GL enums map to exact ICD internal codes; XOR packet/result | join the other fifteen internal codes to final native state/program and visible results |
| fog, explicit fog coordinate, clip planes, two-sided lighting | API/result plus generated-state snapshots | identify native program/constant deltas after the linked-record grammar is decoded |
| multisample and sample coverage | API-only on an unsampled visual; static `ad/af` emitter | sampled allocation, pattern, mask, resolve, coverage invert, and query scaling |
| occlusion query | static begin/end/result protocol | visible/hidden counts, availability, slot reuse/wrap, counter wrap, and ordering |
| draw buffers/MRT | static four-target address, mask, and compiler paths | one shader writing four distinct attachments, readback of each, masks and completeness failures |

## Textures, samplers, and pixel transfer

| OpenGL 2.0 area | preserved evidence | remaining work before exposure |
|---|---|---|
| 1-D and 2-D integer/normalized formats | static format table, sampler/prototype families and layout; ordinary RGBA8 result/packet | per-format swizzle/type normalization matrix |
| NPOT 2-D | advertised/static arbitrary dimensions | odd-size coordinate and full-mip result capture |
| mipmaps and generated mipmaps | ordinary full chain and tail packet/result | odd dimensions and base/max-level boundary cases |
| nearest/linear and six minification filters | packet | visible LOD/footprint validation |
| S/T wrap modes and border color | packet for repeat/mirror/edge/border selection | border color result; 1-D behavior |
| LOD bias, min LOD, max LOD | packet for representative positive values | negative and clamped boundary matrix |
| cube maps | API/result and static six-face constructor | face orientation, seams, mip selection, and incomplete cube behavior |
| 3-D textures | advertised/static volume constructor | slice addressing, R wrap, mip recurrence, and edge coordinates |
| rectangle textures | advertised/static resource family | unnormalized coordinate result and forbidden wrap/mipmap behavior |
| depth textures and shadow compare | static formats and eight compare codes; one accepted GLSL shadow draw | compare-disabled raw depth, pass/fail pairs, result channel, filtering, and FBO producer dependency |
| texture compression | generic extension advertised; static DXT1-5 packers, Linux S3TC token absent | query numeric core compressed formats; round-trip returned formats; DXT stays gated if not returned |
| texture objects, subimages, copies, priorities/residency semantics | ordinary resource lifecycle/static copy path | update-then-sample coherency, overlapping copies, subimage/mip boundaries |
| pixel-store packing/unpacking and readback | basic readback | all core alignment/row-length/skip/type conversions; performance is secondary |
| 10-bit GigaColor | static A2B10G10R10/A2R10G10B10 codes and config gates | sentinel upload, render, sample, readback, and separate depth-30 scanout test |
| float, YUV, anisotropic, and sRGB textures | real dormant Windows packers/config paths | remain extension-gated until upload/sample/filter/render result matrices exist |

## Buffers, arrays, and draw submission

| OpenGL 2.0 area | preserved evidence | remaining work before exposure |
|---|---|---|
| client arrays and sixteen generic attributes | static declaration grammar, complete ICD generic-attribute entrypoint vocabulary, and four canonical live layouts | visible conversion matrix, stride/offset, disabled current value, and alias rule |
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
| scalar/vector/matrix bool, int, float types; constructors and conversions | static aggregate/type descriptor, integer frontend vocabulary, all 19 uniform update families, and representative live draws | systematic visible-result matrix; lower integer/bool forms conservatively until native results are recorded |
| arithmetic, swizzles, write masks, abs/negate, min/max, reciprocal/rsqrt, exp/log, floor | 91-case native lowerer corpus | join generated bytes to the live linked-program record and reject unrecorded native combinations |
| dot products and matrix/vector multiplication | static exact DP3/DP4 expansion; live mat4 draw | non-square matrix variants and alias cases in live native storage |
| comparisons, boolean logic, selection | static CMP/SETP and ARB SLT/SGE result draws | exact native predicate fields and all six relational outcomes |
| constant/uniform conditionals | live result captures | identify folded versus native branch representation and reconvergence |
| fixed and uniform-bound loops, break/continue/function calls | live result captures and static frontend | native/unrolled policy, targets/backedges, nesting and implementation limits |
| vertex/fragment uniforms and arrays | A/B/A stage-specific resource/result captures; exact limits | matrices, integer/bool uniforms, arrays, partial updates, locations after relink |
| attributes, varyings, built-ins, invariant matching | static 14-entry varying map; exact semantics for FrontFacing/FragCoord/Position/PointSize/ClipVertex/fragment outputs; live linked programs | visible built-in routes, type/size mismatch failures and interpolation slots; `gl_PointCoord` is absent from both pinned compiler images and needs a live acceptance probe |
| ordinary, projected, and bias texture lookup | compiler/result captures and static texture lowerer | exact native mode/resource join |
| vertex explicit-LOD lookup | compiler/result capture | native instruction and vertex sampler/resource join |
| derivatives | accepted result captures and static DDX/DDY selectors | isolate nonconstant X/Y gradients and recover their actual native slots |
| discard/KIL | accepted conditional/all result captures and static KILL lowerer | locate packed conditional KIL slots and validate helper/coverage behavior |
| fragment depth output | static one-component result semantic `0x13`, constructor/presence path and result-routing vocabulary | shallow/deep sentinel draw against later depth test, with native output route |
| four fragment color outputs | runtime-sized `gl_FragData[gl_MaxDrawBuffers]` constructor, static target compiler and MRT emitter | distinct four-attachment result capture |
| vertex position, point size, clip vertex; fragment color/depth built-ins | exact logical semantics `0x0d..0x13`, component counts and stage presence masks; partial fixed-function/point evidence | direct GLSL result fixtures to join every logical semantic to its native packed route |
| compiler resource limits | live limits: 16 attributes, 4 vertex samplers, 16 fragment samplers, 20 combined, 512 uniform components per stage, 60 varying floats, 4 draw buffers | compile/link boundary tests at limit and limit+1; do not infer hardware maxima from allocation sizes |
| program transport and termination | static two-dword slots, inline upload, terminal form; live `0x6c` and full system-resource snapshots | finish record grammar, native storage/entries/counts, endpoint semantics, and relocations |

## Extension and vendor-only retention

The hardware also has paths outside the OpenGL 2.0 core contract.  Preserve
them for later Gallium capabilities without letting them inflate the core
conformance claim:

* 10-bit GigaColor rendering and its no-page-flip presentation restriction;
* floating-point texture formats, YUY2/UYVY sampling/video paths, DXT1-5, and
  dormant anisotropy levels 1/4/8/16;
* render-target sRGB conversion, with sampler-side sRGB still unlocated;
* four-target MRT, hardware occlusion counters, and the incomplete FAA/MSAA
  path;
* point sprites, wide/smooth/stippled lines, stereo/layer planes, overlays,
  VPE/DXVA, tear-free/page-flip policy, and independent back buffers;
* conservative serialized context switching, resource fences, hardware copy,
  and the primary requester; no fast-context-switch, fast-trilinear,
  multiple-pipe, or volumetric-shadow M9 path is present in the pinned images.

The aging vendor installation can be retired only after the rows marked open
have either a preserved live capture, a conclusive static proof that no public
path exists, or an explicit decision to keep the corresponding capability
disabled.  New-driver replay tests are still required after capture; preserved
vendor behavior does not prove the new kernel/Mesa implementation.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
