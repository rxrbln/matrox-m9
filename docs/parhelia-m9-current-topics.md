<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->

# M9 open topics and evidence, 2026-10-06

## Latest checkpoint: three active tracks and corrected GL2 rendering

The user requested three background tracks: FAA command/output flow,
GL2 operations, and performance. Render owns FAA (including the completed
SYSTEM output admission handoff), GL2 owns packed-record bounds and missing
operations, and the former inputs investigator now owns performance. Root
alone integrates and runs serialized hardware tests using the established
locks. Keep source-proven improvements even when a short benchmark is flat.

Mesa c7b8a6e corrects native packed publication. Both complete1024x768
textured/depth palettes now match786432pixels each,4608vertices, six GPU
user draws and zero user CPU fallback, with healthy retirement. Strict CPU
record decoding and unusual batch-size bounds are now implemented with full
retained-owner checks. A fresh integrated307ff0a/4f2012e hardware test exits0,
passes all221184actual record components and both independent786432pixel
palettes, with healthy retirement. See
[strict record/pixel proof](../tests/m9-gl2-record-complete-hardware-20261006/README.md).
This is the measured six-field fixture scope; default varyingcap2 is
unchanged. Actual glmark2 buffer wireframe now also
completes with34native draws/0module/0skipped, noGPUtimeout and an empty
queue. Its single1024x768/vblank0/FAAoff run reports about34FPS; screenshot
and independent wireframe image fidelity are still unqualified. Bump-height
now retires170GPU user draws with healthy completion, but the actual captured
image shows stretched triangles. Its85FPS printout is invalid as a rendering
benchmark. Narrower packed-layout diagnosis, radial multipass and optional
Q3 lighting remain open. See
[bump image failure](../tests/m9-gl2-bump-packed-hardware-20261006/README.md).
Latest basic Q3GL2 checkpoint remains38.9FPS; the historical GL1 numbers
below describe a different renderer/profile. See
[wireframe evidence](../tests/m9-gl2-wireframe-packed-hardware-20261006/README.md)
and [current GL2 status](parhelia-m9-glmark2-status.md).

FAA genuine red public shader constants cause a different actual main-store
result from green, but final output remains allzero/black. This is not working
FAA. Complete source/address/lifetime audits have not yet found the final
output defect; resolve/store translation remains open. Kernelc70c607 contains
the isolated red diagnostic, while healthy normal target boot3cd3b20a still
uses observer73c2a45 with user submissions enabled and private FAA disabled.
See [current FAA evidence](parhelia-m9-faa-native-current.md) and
[SYSTEM output admission](parhelia-m9-faa-output-system-admission.md).

The older ledger below retains its dated measurements and source checkpoints;
use this checkpoint and linked topic notes for the current work queue.

This ledger reconciles the independent M9 (`102b:0540`, `m9x`) userspace
source at **05c25ed** and kernel **8b4e339** (native combined controls at
a9c5668/4b84cbc; last timedemo bd24336), the October 2 piglit results, and the feature-specific
RE notes. It is a work queue, not a new hardware result. The user has installed
an M9 in nano for deployment and FAA testing; results from that new installation
must record the actual adapter/subsystem identity, boot, kernel and Mesa hashes.
Older M9128 or M9148 measurements remain labelled with their original board.
P690, Parhelia and LX packet meanings do not establish an M9 command.

Current nano deployment is now qualified on M9128 102b:0540/22c0,
kernel7.2.5-t2, boot815d4dcc. See the
[deployment proof](../tests/m9-native-deployment-20261006/README.md) and
[integrated sampler/FAA-resource proof](../tests/m9-faa-sampler-integration-20261006/README.md).
Ordinary texture/depth, queries, four MRT targets and accelerated GLX pass.
One1024x768/vsync-off texture scene reports717FPS; this is not a whole-suite
performance result. One fullscreen Quake3 GL1 timedemo reports49.6FPS on
d1f2b12, a new-board baseline. The sampler base and border regressions pass.
Driver5ad6c17 additionally passes the actual advertised varying-limit test,
after compiler splitting and padding overlap fixes. See the
[interpolation proof](../tests/m9-interpolation-native-20261006/README.md).
FAA mode remains unproved and unadvertised. Historical5ad6c17 profile49.5FPS
had zero native vertex draws. Current32fa687 packs independent identity
texture pairs into one stream and preserves interpolated color in the second;
actual texture/affine/depth controls pass. One1024x768/vsync0 Quake3 run is
51.4FPS, about99.2% GPU vertex draws and zero skips; see
[latest native proof](../tests/m9-packed-coord-native-20261006/README.md).
Native constant batching now passes textured A/B/A and49-matrix pixel
references, including depth, with actual3/49 constant runs per job. One new
1024x768/vsync0 run reports56.6FPS,55jobs/frame and zero uniform splits;
see [batching proof](../tests/m9-uniform-runs-native-20261006/README.md).
ea97652 enables the tested path by default. Remaining CPU draw is triangle
polygon offset (factor=-1,units=-2), about0.8% of calls/0.03ms per frame.
Native D24 polygon offset is now implemented in0b101e9/c74a519, with typed
kernel admission in7da488d. Source408e866 probes that admission without GPU
execution and retains CPU fallback on older kernels. The DSO build and batch
suite pass. Kernel4b84cbc with Mesa a9c5668 now passes native texture,
depth-tested offset and depth-disabled offset controls, each0/76800 wrong;
see [combined control proof](../tests/m9-completion-native-20261006/README.md).
The99.2% and56.6FPS measurements above predate this feature.
FAA companion allocation now matches28 genuine vendor geometry/descriptor
cases (fb4e710). Main and auxiliary stores now use genuine linear rows and
independent32-byte guards, even at the32MiB budget cap (7443ff6/d8819db).
Actual-C helpers match six independently captured vendor cases; ownership
sanitizers, complete batch suite and DSO build pass. The corrected resources now pass native1920x1200→2048x1536→1920x1200
same-context growth with AA mode off, strict readbacks and texture controls;
see [guarded-resource proof](../tests/m9-faa-guards-native-20261006/README.md).
They have no FAA-on rendering proof.

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

There are currently three background agents: `m9_faa_emulator` traces the
shared allocator and platform/OS contracts; `m9_faa_native` traces the FAA
resource graph, initialization, binding and lifetime; `m9_open_topics` works
on asynchronous completion and presentation. Root integrates their results
and owns physical tests. Remaining shader, texture and conformance traces
are queued. Some have partial vendor evidence, but they are not all closed.

| order | scope | actual remaining gap | next useful proof |
|---|---|---|---|
| 0 | deployment and baseline | Qualified on current M9128; broader scenes remain | Retain the pinned deployment and post-test health proof when testing each new candidate |
| 1 | FAA vendor emulation | Genuine normal target-owner publication independently passes with recorded DC inputs. Normal surface-owned frame now reaches resolve and its second launcher retains the FAA graph; strict descriptor/order audit is pending. Earlier manually bound frame remains incoherent. Exact counted-secondary export and CPU sentinel alias publication are proven | Finish strict normal-frame audit using actual selector13 companion/selector12 HZ descriptors and both ordered launchers; retain actual pool initialization, resource membership and retirement before executable translation |
| 1 | FAA implementation and native probe | Existing diagnostic state does not prove multisampling: previously zero of 318464 store dwords changed | Compare the vendor-owned resource/enable/reset/resolve graph, test one edge scene, read back store writes and resolved edge coverage; follow with depth and texture controls |
| 2 | max-samplers regression | Base and border cases closed: stale CPU record layout after fragment specialization | Both native cases pass32seen/zero skips after d8fa015, without capacity changes |
| 2 | kill/early-z state | Three quad lanes use an earlier quad's kill outcome with our instruction and an unchanged vendor program | Obtain genuine vendor engine MMIO while `fs-discard-cond` runs and compare initialization plus job-head controls |
| 3 | interpolation splitting | Advertised varying-limit case closed; broader scene schedules remain | a4995c2 classifies allocator pressure;5ad6c17 excludes overlapping padding. Actual glsl-max-varyings passes69seen/zero skips. Fourteen-input stress passes offline only; native VS still has a five-input limit |
| 3 | per-fragment TXB | Uniform/draw-constant bias works; saved two-pass route uses literal +1 without mip filtering; varying bias remains refused | Run the prepared LOD-sensitive vendor probes before implementing packed coordinates/bias and captured selector mode 3; see [capture boundary](parhelia-m9-txb-capture-boundary.md) |
| 3 | fragment control flow and values | General control flow, some value splits and shadow-reference clamping remain refused or unstarted | Map one existing vendor control-flow program into the native pass graph; prove branch-dependent color/depth/kill semantics before generalizing |
| 3 | native vertex ISA | Vertex logarithm/power remain refused; six odd temporary registers are excluded after native failures | Capture a genuine vertex log2 seed and complete chain; separately trace output/register lifetime to determine whether any excluded register can safely be recovered |
| 4 | complex split scenes | glmark2 terrain remains incorrect/slow in the recorded standing; partial split coverage is implemented, not a blanket missing feature | Capture one bounded terrain/split case against a reference with skip accounting and identify the first bad intermediate rather than reporting its FPS as conformant |
| 4 | texture layout and filtered copies | Cube faces larger than 16 are not placed; non-halving filtered engine copies have known failures | Trace the vendor cube address/layout calculator above the packed tail; use existing `blitlin:75x37`, `blitlin:74x37` and `blitlin:100x40` controls for the copy issue |
| 4 | MRT blending and combined operations | Four unblended targets work; blending has per-target replay, but single-pass destination reads for RT1..3 and combined stencil/discard/split cases remain open | Capture destination-read registers and program image for RT1..3 blend, and verify each target plus retained BO membership |
| 5 | performance and GPU geometry | Current 05c25ed / kernel8b4e339 at1024x768/vsync0:56.3FPS, all steady gameplay transforms native;56jobs/frame,zero uniform splits. Earlier56.6FPS used older sources, so performance is flat at single-run precision | Reduce remaining submission costs; retain packing, batching, GPU polygon offset and queued ownership improvements |
| 5 | presentation and async submission | Four-job opt-in queue passes native matrix/depth and shared-FS vertex-program A/B/A controls. Q3 reaches four actual jobs in flight; post-game texture and WAIT pass on the same boot. Copy presentation passes; opt-in flip switches DC_START but output enable reports -67 | Retain [native queue proof](../tests/m9-queue-native-20261006/README.md); qualify physical flip output separately. No repeat timedemo needed |
| 6 | 10-bit masks and conversion | Unblended single-target partial masks use a constant-color blend fallback; already-blended and multiple-target partial masks remain refused, and converting readback needs current retest | Prove the existing unblended mask fallback, implement captured mask lowering for combined cases, then test converting readback and linear filtering |
| 6 | 10-bit display verification | M9128 KMS/GBM/EGL 30-bit path passed; panel precision and transmitter depth remain unverified | Repeat on the installed board and inspect the physical output; keep raw storage precision distinct from panel output precision |
| 7 | query and kernel lifetime/recovery | Query counter wrap, split-pass sample accounting and fault/recovery boundaries require targeted qualification | Audit the current kernel first, then submit a bounded wrap/lifetime test to the target owner; no unsupported arbitrary requester state |
| 8 | pixel conformance | Eight-bit GL_DITHER ignored; polygon stipple capture decoded but not implemented; minor rounding/state tests remain | Schedule after FAA and major rendering/performance gaps; use the captured state plus explicit pixel references |

## What is already implemented

These are controls for deployment, not new open feature implementations:

* Native vertex and fragment programs, textures, derivatives, variable
  indexing, shadow/depth sampling, and framebuffer pass splitting exist.
  Native VS defaults on, with CPU draw fallback for refused shaders. Native
  program length is bounded at 188 slots and its constant span at 63 pairs.
* Native VS compiles up to five varyings (`m9_vs_compile.h`); the default
  fused hardware path uses two output streams. Three or more require an
  additional preprocessing draw that remains unqualified on this board. Native FS has six interpolation
  registers, four fetch units and up to sixteen raster passes. API sampler
  declarations advertise eight so unused variants can link.
* Interpolation splitting is implemented in `m9_fs_compile.c`. Register
  pressure now enters `FS_CUT_REGS`; the advertised-limit piglit case passes
  after padding overlap repair. Broader complex scenes still need qualification;
  the six-register hardware limit is unchanged.
* Four MRT destinations were measured pixel-exact, at pairs `0x2c+4p` and
  `0x2d+4p`. The fragment compiler refuses single-program MRT
  blend/discard/pass splitting. `m9_mrt_blend_split()` replays blended draws
  one target at a time, with equal-depth/no-write and query pause on later
  targets; it refuses a live stencil test. Single-pass destination-read
  decoding remains open, without reopening basic four-target routing.
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
  actual KMS/GBM/EGL 30-bit chain and packed codes were also measured.
  `m9_draw_vbo()` implements an unblended single-target partial mask as
  `src*m + dst*(1-m)` with an exact 0/1 constant mask. Combined blend/MRT
  masks remain refused, while filtering precision, other blend factors,
  converting readback and panel/transmitter verification are separate
  questions. The older GigaColor note predates that fallback.

## Latest FAA command and resource evidence

The implementation gate is one complete vendor FAA frame: allocation,
initialization, normal state compilation, draw, resolve and retirement.
Canonical7806a76d/613dcc84 now provide the joined CPU frame through final
DMA submission and the first genuine GPU retirement-read boundary. Root
independently replayed both:1768 live-owned DMA writes, nine known byte spans,
actual public context/triangle success, FAA mode1f draw and mode1d resolve.
Every exported DMA byte matches an actual vendor CPU write within its owner.
The final real completion packet requests130 hex and posts01002780; execution
stops before status load23bc3 from the actual common-buffer physical02007000.
No GPU completion or subsequent release is supplied. Kernel-owned indirect
state initialization and complete operand/backing provenance remain executable
translation requirements. This is CPU evidence, not a rendered FAA image.
Subsequent review found the public DP2 triangle replaces the FAA companion/HZ
after manual aa750 binding. Root confirmed B8 changes01a51001→012a8801 and
BA01ae9001→0, with ordinary descriptors. Mode1f alone is insufficient.
The joined sequence is therefore not yet a semantically coherent FAA frame;
23000001 alone does not prove the63000001 raster launch. Native agent owns
the actual FAA context/target-state join; emulator owns the count-prefixed
secondary consumer contract. The strict checker must additionally assert FAA
attachment identities and raster execution before porting.
Canonical5661c4f4 now activates the real vendor queue with32 owned system-DMA
allocations through genuine MM handlers and bounded synthetic OS RAM, using
the genuine FAA-admitting MmFlags101b8 option. Root independently replayed
this successfully. Default101f8 has a separate mapping contract still open;
its class1 predicate alone was incorrectly treated as terminal rejection.
Canonical58ac2169 fixes the capture's initialization order: queue activation
precedes resource/shader enable, so the shader uses the live TaskMgr+0xc.
The genuine PDEV CPU alias publisher supplies the existing timeline owner.
These close the earlier f5969 and missing device+58 CPU boundaries.
Root independently replayed actual ac7e0 returning1, all three initialization
clears,900 system-DMA CPU writes and sentinel initialization; no GPU execution
or completion was supplied. Main/auxiliary/companion/HZ extents at320x240 are
136080/13080/97000/3000 hex. Captured packets must not be concatenated to
infer normal ordering. No FAA fragments or resolved target image have been
demonstrated.

Kernel1f1ff0d admits the exact nine-word FAA binding grammar for validation,
with typed pool ownership, tagged VRAM addresses and extent checks. Actual
pool execution is refused until kernel-owned initialization and lifetime are
implemented. The actual-parser check passes the captured record and rejects
202 malformed or unsafe cases; this is CPU validation evidence only.

Actual128-byte M9128 capability EEPROM now executes genuine31f90/3265f and
populates type14. Both SPI payloads and EEPROM responses are from this board;
full adapter initialization is still incomplete. Retained live MM owners and
actual25-device PCI/assigned-bus inventory advance the normal runtime profile
constructor. Actual configured companion477f0(format3d) admits sample2; this
is a constructor prerequisite, not a native FAA rendering result.

The genuine conditional shared allocator now returns1 and publishes its
resource owners using the actual OS-owned aperture mapping. Its128-byte
synchronization counter comes from a real vendor CPU producer backed by
bounded synthetic OS system RAM, not a GPU response. Integratedb0b416c8
passes independent shared-allocator and common-buffer ownership checks.
Normal85d50 mapper publication remains incomplete. See the
[allocator evidence boundary](parhelia-m9-faa-re.md).

[Companion recipe proof](parhelia-m9-faa-companion-resource-20261006.md) corrects
the old linked-format9 BA/BB interpretation: AA uses64-bit format3d/layout2
and its own optional HZ plane. The1024x768 preallocation extent is600020;
sourcefb4e710 now includes that guard and the actual sample-layout2 descriptor.
Ordinary context pool6 is a distinct1280-byte resource; its allocated graph
is captured, while its GPU initialization and executable ownership remain
open. The genuine allocation stores an aligned1408-byte extent.

Actual fce50/fc8f0/24f20 command execution now captures full contextrestore
thenA1/A3/A4save:29GPUwords,6genuine relocations, strict negative controls.
It is distinct from the existing24/E6/BCBD surface binding and pointer reset.
Native FAA still requires complete physical owners/content/lifetime, enabled
draws, store writes, resolved edge coverage, depth and texture controls.
Two FAA agents own normal allocation and platform/OS backing joins; the
performance agent continues independently. Root owns all target tests.

## FAA evidence boundary

The separate worktree `wip/m9-faa-emulator` contains M9-specific execution
evidence now integrated into this RE tree through **b0b416c8**.
It uses the genuine MSerX `MxMD.dll`, `MxMGLX.dll` and `MxMM.sys` for
`102b:0540`. Its actual `0xfb920` AD/AF emitter and `0xfab30`
`RESET_FRAG_PTR` emitter ran under the established PE/Unicorn emulator with
controlled ABI owners. These are real instructions, but the owners are not
the result of a fully initialized multisampled API surface.

Genuine SPI identity responses were recovered from retained vendor-X trace
echoes and checked against1123ROM bytes. The actual miniport now passes flash
identity and PInS parsing. A bounded ownership model grounded in nano's
request/grant/restore capture progresses to GPIO1c4c. The earlier FindAdapter87
was an emulator chip-select error. Full FindAdapter/HwInitialize, configured
FAA allocation, draw, resolve and physical store retirement remain unproved.

The genuine surface emitter additionally executes conditional index24,
E6=[6dc00000,1,1], thenBC/BD when the device-owned per-slot resource exists;
the null owner skips that whole block. These packets are missing from the
Linux FAA diagnostic, but the resource producer/physical type must be joined
before replay. Normal shipping Windows configuration and API routes exist:
OGL\\Multisample nonzero plus3D\\AAMode bit0 enables32-bit WGL pixel formats
advertising eight samples. That establishes static reachability, not a fully
initialized API draw. Dead debug strings are separate weaker evidence.
The dynamic configuration/query chain now executes both constructors and
real driver escape, with actual ICD enable matrix0/0/0/1. Pixel-format
capability initialization is incomplete: the harness rejects ordinary
hardware formats and its appended fallback formats actually advertise zero
samples. The flag alone must not be called dynamic AA advertisement; the
agent is tracing that capability producer and rejection condition.

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
