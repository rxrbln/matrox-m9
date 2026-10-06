<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->

# M9 open topics and evidence, 2026-10-06

This ledger reconciles the independent M9 (`102b:0540`, `m9x`) userspace
source at **6780573**, the October 2 piglit results, and the feature-specific
RE notes. It is a work queue, not a new hardware result. The user has installed
an M9 in nano for deployment and FAA testing; results from that new installation
must record the actual adapter/subsystem identity, boot, kernel and Mesa hashes.
Older M9128 or M9148 measurements remain labelled with their original board.
P690, Parhelia and LX packet meanings do not establish an M9 command.

The [October 2 piglit status](parhelia-m9-piglit-status.md) supersedes the
September 18 backlog where they disagree. The glmark2 standing is still the
September 20 **M9128 at 1920x1200** result, not a current nano benchmark. Root
owns the target and its established hardware locks. Background investigators
work on local evidence and isolated branches and submit bounded tests to that
owner; historical scripts' different lock names do not authorize concurrent
access.

## Priority and focused agent scopes

FAA is first. With four concurrent agent slots including root, retain the
target owner and FAA investigators, then rotate the remaining topic agents.
Each row is a separate scope; an inventory is not evidence that every row has
an agent running simultaneously.

| order | scope | actual remaining gap | next useful proof |
|---|---|---|---|
| 0 | deployment and baseline | New nano installation has not been qualified by this ledger | Verify `102b:0540`, loader `m9x`, firmware, module/DSO provenance, renderer, a plain textured/depth fixture and post-test engine health at 1024x768 |
| 1 | FAA vendor emulation | Real M9 emitters were executed; complete API-created FAA resource/draw/resolve was not | Complete miniport initialization from genuine device responses, then capture FAA-off/on allocation, complete packets, program images and retirement |
| 1 | FAA implementation and native probe | Existing diagnostic state does not prove multisampling: previously zero of 318464 store dwords changed | Compare the vendor-owned resource/enable/reset/resolve graph, test one edge scene, read back store writes and resolved edge coverage; follow with depth and texture controls |
| 2 | max-samplers regression | October 2 `max-samplers` and border variants exceed the owned job-record budget on later merges but pass on c4dd850 | Reproduce one failing shader, trace `m9_job_max_vertices`, producer run length and pass count against the earlier build; fix the responsible budget/splitting layer |
| 2 | kill/early-z state | Three quad lanes use an earlier quad's kill outcome with our instruction and an unchanged vendor program | Obtain genuine vendor engine MMIO while `fs-discard-cond` runs and compare initialization plus job-head controls |
| 3 | interpolation splitting | A raster pass has six interpolants; vendor schedules fourteen as 2/6/6; current splitter contains an interpolation cut but piglit still reports refusals | Feed the precise failing piglit NIR through the offline compiler, expose the first refusal, add the smallest bounded native record-routing proof |
| 3 | per-fragment TXB | Uniform/draw-constant bias works; varying bias is explicitly refused | Implement the captured two-pass coordinate-plus-LOD record form and prove mip selection with a varying bias image |
| 3 | fragment control flow and values | General control flow, some value splits and shadow-reference clamping remain refused or unstarted | Map one existing vendor control-flow program into the native pass graph; prove branch-dependent color/depth/kill semantics before generalizing |
| 3 | native vertex ISA | Vertex logarithm/power remain refused; six odd temporary registers are excluded after native failures | Capture a genuine vertex log2 seed and complete chain; separately trace output/register lifetime to determine whether any excluded register can safely be recovered |
| 4 | complex split scenes | glmark2 terrain remains incorrect/slow in the recorded standing; partial split coverage is implemented, not a blanket missing feature | Capture one bounded terrain/split case against a reference with skip accounting and identify the first bad intermediate rather than reporting its FPS as conformant |
| 4 | texture layout and filtered copies | Cube faces larger than 16 are not placed; non-halving filtered engine copies have known failures | Trace the vendor cube address/layout calculator above the packed tail; use existing `blitlin:75x37`, `blitlin:74x37` and `blitlin:100x40` controls for the copy issue |
| 4 | MRT blending and combined operations | Four unblended targets work; multiple-target discard, blending and split shaders are refused | Capture destination-read registers and program image for RT1..3 blend, and verify each target plus retained BO membership |
| 5 | performance and GPU geometry | Per-object uniform spans still end jobs; native cull bits have not removed hidden faces; complex shaders remain expensive | Profile one 1024x768 scene, compare complete rendering and native/module counts; pursue vendor-proved preprocessing constant spans and the missing cull condition |
| 5 | presentation and async submission | Existing presentation/submit features need current-board measurement; this ledger does not establish an extra async ABI gap | Check current kernel/DDX implementation and one fullscreen/windowed swap trace before changing the framebuffer or retirement model |
| 6 | 10-bit masks and conversion | Ten-bit formats work, but partial masks are explicitly refused; older converting-readback failure needs current retest | Implement captured fragment-channel mask lowering with untouched-destination proof, then test converting readback and linear filtering |
| 6 | 10-bit display verification | M9128 KMS/GBM/EGL 30-bit path passed; panel precision and transmitter depth remain unverified | Repeat on the installed board and inspect the physical output; keep raw storage precision distinct from panel output precision |
| 7 | query and kernel lifetime/recovery | Query counter wrap, split-pass sample accounting and fault/recovery boundaries require targeted qualification | Audit the current kernel first, then submit a bounded wrap/lifetime test to the target owner; no unsupported arbitrary requester state |
| 8 | pixel conformance | Eight-bit GL_DITHER ignored; polygon stipple capture decoded but not implemented; minor rounding/state tests remain | Schedule after FAA and major rendering/performance gaps; use the captured state plus explicit pixel references |

## What is already implemented

These are controls for deployment, not new open feature implementations:

* Native vertex and fragment programs, textures, derivatives, variable
  indexing, shadow/depth sampling, and framebuffer pass splitting exist.
  Native VS defaults on, with CPU draw fallback for refused shaders. Native
  program length is bounded at 188 slots and its constant span at 63 pairs.
* Native VS transports up to five varyings (`m9_vs_compile.h`), rather than
  the one varying in the old backlog. Native FS has six interpolation
  registers, four fetch units and up to sixteen raster passes. API sampler
  declarations advertise eight so unused variants can link.
* `FS_CUT_INTERP` is implemented in `m9_fs_compile.c`; the open item is the
  observed shader that still fails to enter or complete the cut. Do not raise
  the six-register hardware limit.
* Four MRT destinations were measured pixel-exact, at pairs `0x2c+4p` and
  `0x2d+4p`. The backend explicitly refuses MRT blend/discard/pass splitting;
  this does not reopen the basic four-target routing decode.
* Occlusion queries are implemented and advertised. Logic operations were
  implemented at 6780573. Stencil, fog, sRGB texture/write support and GL 2.1
  exposure supersede the earlier partial or unreachable claims.
* Fragment `nir_op_flog2` is implemented. Native **vertex** logarithm and
  power remain refused; the two stages must not be conflated.
* The six bad native VS temporary registers (`11`, `15`, `17`, `19`, `1b`,
  `1f`) were removed from allocation. Their mechanism is open RE with
  possible performance benefit, not an unmitigated allocator defect.
* Partial mesh split draws have the padded path by default
  (`M9X_SPLIT_PAD=true`). Earlier blanket refusals are historical. Correct
  scheduling and coverage still need proof for any newly admitted shape.
* Cube sampling in the captured tail works up to 16-pixel faces; volumes are
  placed up to the advertised 256 extent. This is not a blanket missing cube
  or 3-D texture implementation.
* Ten-bit render, nearest sample and clear, plus the
  `SRC_ALPHA/ONE_MINUS_SRC_ALPHA` blend pair, were measured on M9128. The
  actual KMS/GBM/EGL 30-bit chain and packed codes were also measured. Partial
  channel masks, filtering precision, other blend factors, converting
  readback and panel/transmitter verification remain separate questions.

## FAA evidence boundary

The separate worktree `wip/m9-faa-emulator` contains the current M9-specific
execution evidence through commit **b080d1498f7f66ed2047bf4371e2cfd0a843b5d4**.
It uses the genuine MSerX `MxMD.dll`, `MxMGLX.dll` and `MxMM.sys` for
`102b:0540`. Its actual `0xfb920` AD/AF emitter and `0xfab30`
`RESET_FRAG_PTR` emitter ran under the established PE/Unicorn emulator with
controlled ABI owners. These are real instructions, but the owners are not
the result of a fully initialized multisampled API surface.

The latest preserved bootstrap boundary is serial-flash identification
opcode `0x15`; the genuine manufacturer/status response is missing. The
earlier FindAdapter 87 was an emulator chip-select polarity error and does
not establish an unsupported device. Full FindAdapter/HwInitialize, FAA
admission, allocation, draw, resolve and physical store retirement have not
been proved. The partial trace must not be called a complete FAA capture.

Current diagnostic source separates `ad`/`af`, the `ef[31:29]` AA mode,
`f0` control bits, `bc`/`bd` store bindings, `b8` companion and reset state.
The old hardware probe's zero changed store words is a failure to establish
FAA enable, not proof that M9 has no FAA engine. MSAA remains unadvertised.
`af` bit 11 alone previously stopped the engine. Index `c7` emitted mid-job
also stopped it; do not transplant that isolated value from a vendor job
head into a live raster range.

An acceptable native FAA result needs all of: real M9 renderer, visible
baseline, changed fragment-store contents attributable to the draw,
resolved edge coverage different from single sample, correct textured and
depth-tested output, and full retirement without timeout. A completed process
or altered command word alone is insufficient.

## Existing bounded proof tools

The source tree supplies `m9-batch-test`, `m9-vs-compile-test`,
`m9-fs-compile-test` and the `--srgb` compiler test. Compiler tests prove
construction/reference behavior; several earlier native VS defects were
shared by the simulator, so they do not replace target readback.

The RE tree supplies `tools/m9-mesa-glsl.c` and `tools/m9-mesa-vs.c`, with
texture, depth, split, MRT and GigaColor controls. For larger source changes,
`tools/m9-mesa-glsl-suite.sh` asserts `GL_RENDERER Matrox M9` and stops after
a kernel stream timeout. Use the established root-held locks around it and
submit only the relevant cases first. The current target baseline should
be brief; do not rerun old full suites to manufacture a new FPS claim.

`tools/m9x-glmark-default-suite-fresh-x.sh` defines the thirty-three benchmark
list; `M9X_SKIP_REPORT` and `M9X_PROFILE` expose refused draws and costs.
Use one short 1024x768 run for a new candidate and record vsync explicitly.
Retain correct features and optimizations even when one sample is unchanged.

The fresh nano baseline, FAA result and final deployed hashes belong in a
separate dated test record linked here when the target owner finishes them.

## Detailed owners of the remaining evidence

* [Piglit failures and capture requests](parhelia-m9-piglit-status.md)
* [FAA static and native evidence](parhelia-m9-faa-re.md)
* [Fragment backend and kill artifact](parhelia-m9-mesa-fragment-backend.md)
* [Native vertex backend and corrected allocator](parhelia-m9-native-vertex-backend.md)
* [Raster interpolation limit](parhelia-m9-raster-stream-limit-re.md)
* [Native control-flow decode](parhelia-m9-native-control-flow-re.md)
* [Cube and volume placement](parhelia-m9-cube-volume-textures.md)
* [MRT routing, superseding older retirement blockers](parhelia-m9-mrt-routing.md)
* [glmark2 standings, costs and incomplete terrain](parhelia-m9-glmark2-status.md)
* [Ten-bit results and remaining questions, section 9](parhelia-m9-gigacolor-10bit.md)
* [Buffer sharing and linear-sampling boundary](parhelia-m9-buffer-sharing.md)

The September 18 implementation backlog and outstanding-features note are
historical records. Their one-varying, absent MRT/query and unreachable sRGB
rows must not be used as the current task queue.
