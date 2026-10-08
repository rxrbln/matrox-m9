# M9128 generalized GL FAA upload boundary


## October 8: full glxgears FIRST retires; pixels and frame reuse still fail

DRMff3696f removes the false address check on vendor transition controls and
integrates initialized mixed-layout/flat/cull composition. On the actual
M9128, all12 glxgears FIRST jobs now execute: INITb0 -> Lock8e0 -> Unlock900.
The captured320x240 window is entirely black, so this is not a rendering pass.
SECOND frame preparation returns EFAULT before submission; the target stays
healthy at900. [Hardware evidence](../tests/m9-faa-glxgears-first-hardware-20261008/first-retired-black/README.md).

Next work separates the F021 readback from application presentation and
checks post-render mapping metadata during frame reuse. Strict initial
mapping publication and live mapping identity must not be conflated. The
actual-submit CPU regression (DRM7de4237) proves three ordinary SYSTEM
placements are independent of the control literals while six real FAA-owner
PTE corruptions still refuse before any upload or END.

Texture binding words are now derived from retained live owners in DRM6c9b985,
with CPU source/placement/refusal checks. Full texture submission and command
pool rotation remain unfinished. No valid FAA glxgears screenshot, continuous
run, Quake3 FAA result or FAA FPS measurement yet.

## October 8: glxgears mixed batch prepares; control words misclassified as addresses block submission

The latest target run accepts all21 glxgears draws and prepares12 native FAA
jobs (91,392 input bytes). Mesa7cebd8e and DRM8c0f6c1 resolve the clear/layout
and mixed declaration3/5 preparation blockers. Submission returns ESTALE before
code upload or any geometry END; INITb0 remains healthy through cleanup.
[Exact hardware evidence](../tests/m9-faa-glxgears-first-hardware-20261008/mixed-layout-submit-refusal/README.md).

The actual ordinary SYSTEM allocation is at GPUVA0x40047000, while submission
incorrectly requires fixed command controls0x40004000 and0x40008000 to fall
inside that allocation. The first such admission necessarily rejects the
frame. The original M9128 DLL writes both values as instruction immediates;
existing transition/query decoding also treats them as controls. Rebasing
these words from the ordinary SYSTEM owner would be unsupported. The fix
removes their false address classification while retaining checks for actual
address-bearing fields and live owners. Hardware qualification is pending.

Initialized mixed-layout/flat/cull support and command-pool rotation remain
under integration. Texture fragment code is CPU-qualified; native texture
binding remains gated. There is no valid FAA glxgears screenshot, continuous
run, Quake3 FAA result or FAA FPS measurement yet.

## October 8: mixed flat/smooth FAA now passes hardware

The next single hardware run passes flat368 →smooth400 →flat368 with the
native deduplicated instruction page: INITb0/Lock340/F021/Unlock360, healthy
delayed and cleanup snapshots. The independent pixel oracle passes23,492
interiors,7,697 overlaps,zero errors and777 partial pixels; the raw image is
identical to the previous all-flat pass. [Exact mixed-program evidence](../tests/m9-faa-first-batch-hardware-20261008/mixed-pass/README.md).
These constant-color inputs do not qualify varying-color interpolation.

Mesa36d9fc8 now tracks authenticated initialized-depth state and explicit
FRAME_INITIALIZEDbit2, preserving flags0's old +1a0 contract. Its full20-object
build and client checks pass. Pool primary/secondary managers share one
actual-completion timeline (DRMb4b6df0/e321d0e); native allocation/publication
hooks remain under implementation. The80-byte BATCH ABI now names a separate
maintenance_point output (db01829), so real pool maintenance cannot silently
rewrite the previous application Unlock history. No repeated-frame/glxgears
hardware pass yet.

## October 8: repaired flat/CW multi-draw FAA passes hardware

The34-owner correction now passes the exact three-draw test that previously
lost the machine: INITb0 →Lock340/read/Unlock360, all14/17 ENDs retired,
generation0/queue0/wedged0 through cleanup. Independent pixels pass23,492
interiors,7,697 depth-overlap checks,zero errors and777 partial-coverage
pixels. [Hardware receipts and actual image](../tests/m9-faa-first-batch-hardware-20261008/ownerfix-pass/README.md).
Frozen5dcaa82 contains the owner fix and WC barrier; canonical equivalents
are6170da1 and2717bf4. The preceding failure was a reproduced kernel stack
overwrite before batch submission. Its test stub had hidden the capacity
mismatch; the test now calls the actual production fill.

This is one same-program flat/CW frame, not continuous glxgears. Mixed shader
native/Mesa code is integrated but awaits hardware. Initialized later-frame
integration and observed-retirement command-pool rotation remain active.

## October 8: batch failure traced to CPU stack overwrite

A zero-delay phase trace with the WC-publication fix reaches INITb0 and
successful first-frame preparation, then loses the host before batch
composition or any batch END. Source inspection found a concrete kernel
stack overwrite: the batch allocated31 owner descriptors but the production
fill writes34, including depth attachments. DRM `6170da1` fixes the capacity;
the updated test uses the real fill and reproduces the old overflow under
ASan. Its earlier31-role stub had masked this bug. The WC barrier correction
`2717bf4` is retained independently. The corrected hardware test is pending.
[Host-streamed phase evidence](../tests/m9-faa-first-batch-hardware-20261008/wc-trace-failure/README.md).
Do not characterize this observed failure as a proved GPU-command wedge.

Initialized later-frame source controls are integrated as RE `42361ee2`:
clear3/depth7 reuses bound attachments and reaches unread Lock370 from
native Unlock220; no-clear reaches Lock2f0. The clear prefix includes a
separate existing-sentinel operation. Native integration and pool rotation
remain open; glxgears FAA is not yet verified.

## October 8: combined flat/CW batch fails after successful INIT

The frozen native candidate `d2a5c03` (root `b1e5ba3`) now pairs flat32
programs with their full fragment metadata and common cull state. Mesa
`970d205` forwards those profiles. Host checks and full builds pass, but the
physical M9128 test **fails**: INIT retires at b0, then three flat368/CW draws
with distinct CB colors and overlapping depth enter FRAME_BATCH; the target
becomes unreachable before the expected Lock340 or any pixel readback.
The exact failing GPU END is not yet known. [Candidate hashes, inputs,
runner and host-streamed failure](../tests/m9-faa-first-batch-hardware-20261008/README.md).
The earlier single smooth six-vertex FIRST pass remains valid and narrower.

An initial unobserved failure was repeated once with durable staging and
host-streamed phase markers; the repeat establishes the batch transaction
as the boundary. No optimization or new feature was reverted. Reviews have
not found a concrete C3 relocation or input-lifetime defect. A complete
original flat/CW N1/N2/N3 comparison now passes against frozen actual C
(`d8d50832`), including whole primary/secondary, owner placement and all
fourteen ENDs. It does not qualify the live one-output hardware adaptation.
The recovered target is on boot4640313a with no candidate module loaded;
opt-in submission phase tracing is being added to locate the failing END.

Separate progress remains host-only: native immutable code-slot/C2 lowering
`e68f6be`, executable counted-pool ownership `2b3e755`, retired CPU-frame
reclamation `9282eee`, and flat-interface classification compatibility
`1d977db`. Mixed-program native execution is now integrated as `89e9b86` with an
owned preloaded instruction page and per-job C2/flat metadata; its host
mixed400/368/832/1024 producer and failure tests pass. It has no new GPU
qualification. Repeated initialized-depth frames and sustained pool wiring
remain open. Glxgears FAA and later Quake3
GL1, glmark2, piglet and Quake3 GL2 objectives are not complete.

## October 8: native frame batches and actual flat shader frontend

DRM `2520b48` integrates FIRST FRAME_BATCH execution for 1..32 same-program
draws. Every draw retains separate CB/header/raw inputs; the final Lock is
INIT+0x150+(N-1)*0xa0 with fourteen END posts. Source N2/N3 comparisons,
actual preparation N1..32, and actual submission/failure host checks pass.
Hardware qualification of multiple draws is pending. Initially admitted
batches use full black/depth-one clear, LESS/write, smooth no-cull state.

Mesa `e0083b7` sends queued application frames to that typed batch API with
one final presentation and authenticated count-derived receipt. `f675b1a`
fixes a concrete flat-shading bug: the live frontend previously emitted the
smooth64 shader for flat state. It now emits the original flat32 ISA and
utility ordering, including legacy fixed-function interpolation. Smooth
uploads remain unchanged; GPU transformation and lighting remain in the
carry shader. The full twenty-object Mesa build and live/fixed-function
host checks pass. Native flat metadata pairing and hardware pixels remain
pending; shader byte length alone is not sufficient admission.

The original FIRST source proves the corresponding compiler usage mask,
EF contribution and varying interpolation metadata. Its earlier retained
flat control had reused a cached compiled shader, so the raster-bit-only
inference was incomplete. See [FIRST raster/metadata proof](parhelia-m9-faa-first-raster-controls.md).
Continuous glxgears still needs flat/cull batches, real program switching,
initialized-depth later frames, and observed-retirement-based pool reuse.
[Counted pool source proof](../tests/m9-faa-pool-counted-source-20261008/README.md)
now distinguishes pending CPU references, submitted pool points and actual
GPU completion; it does not authorize recycling from submission alone.

## October 8: fresh FIRST clear/depth also passes physical M9128

DRM `c1962dc` (sealed candidate `43d80b0`) now runs directly from authentic
INITb0 through full sampled color/depth reset and a live single-COLOR GPU
program with six vertices: Lock200/readF021/Unlock220. All14 initial ENDs
and17 after Unlock retire; delayed/cleanup snapshots remain queue0/wedged0.
The image passes12,302 colored/638 partial pixels and9,936 interior checks
with zero pixel errors. Its entire raw readback is byte-identical to the
previous passing retained full-clear image. [Exact hardware evidence and
screenshot](../tests/m9-faa-first-depth-hardware-20261008/README.md).

Mesa `617e812` owns all draw snapshots until flush/readback, target change,
or a later clear, instead of presenting each draw separately. `63a0835`
authenticates FIRST+150 and retained+1a0 depth receipts with exact metadata
and an independent wait. The full20-object driver build and focused host
checks pass. Actual directional/positional fixed-function uploads also pass
the fresh native encoder on CPU; this is not GPU lighting qualification.

Glxgears FAA remains open: FIRST flat/cull, complete native multiple-draw
composition for live application shaders, initialized-depth later frames,
and retirement-aware command-pool reuse. The corrected pure appender
`af5137c` compares the whole composed primary to vendor output; its earlier
footer-only version was incomplete and was never posted. At that earlier checkpoint native batch
execution remained gated; the new host-tested integration is described above. Pool RE proves primary rotation through an unused
slot and a separate pool-counter reuse dependency; Unlock alone does not
authorize rewinding active pools. See [pool contract](parhelia-m9-faa-pool-reuse-contract.md).
No glxgears/Quake3 screenshot, FPS, or sustained rendering claim yet.

## October 8: full sampled clear/depth FRAME passes physical M9128

DRM `cf43aa2` completes live first3 Lock190/read/Unlock1b0, then full black
color/depth-one clear plus LESS/write and changed live6 Lock350/read/Unlock370.
The single hardware run and cleanup stay generation0/queue0/wedged0.
The next image passes all checked interior/exterior/channel tests; **exactly
six pixels differ from the previous failing image, removing all six stale
old-color edge contributions**. It contains12,302 colored/638 partial pixels
and9,936 checked interiors. [Hardware receipts, raw data and screenshot](../tests/m9-faa-depth-frame-hardware-20261008/README.md).

The new COLOR attachment prefix derives live page masks and its distinct
source prologue; the draw suffix derives live shader/CB/geometry and the
actual retained queue epoch. Flat/CW/CCW packet construction matches six
complete vendor source controls, but those raster variants have no new
hardware qualification. Real Mesa directional and positional fixed-function
lighting uploads also pass the native depth encoder's host tests.

At this earlier retained-frame checkpoint, fresh FIRST, multiple draws
and sustained pool reuse were still open. FIRST is now superseded by the
hardware pass above; the remaining application gaps are listed there. Original vendor FIRST now passes the
finite25270 dependency-query branch without a fabricated completion; its
normal Lock path is being integrated. Separate source DP2 callers reuse bound
attachments within a frame without clearing, so application submission must
preserve that frame lifetime. The older six-stale-pixel results below are
historical and superseded by this full-clear hardware test.

## October 7: genuine retained flow executes; same six stale edges remain

DRM `6c3fdd4` replaces the reused first-draw secondary with the actual vendor
retained command structure (primary3624/secondary2536 for two declaration
groups), preserving live resource and shader derivation. Independent source
comparisons pass128 placement/state/count/declaration cases. One complete
hardware batch retires first3 → clear → next6 → both Lock/read/Unlock cycles
and cleanup with generation0/queue0/wedged0. Both images are byte-identical
to the prior build: first image passes; second still has six stale old-color
edge samples. The correction is retained; full FAA sample reset is open.
[Exact hardware evidence](../tests/m9-faa-retained-flow-hardware-20261007/README.md).

Mesa `0dba5b2` preserves the retained graph across same-layout back/front
retargets and requires a real clear for a new destination. `dcc14df`/`5e6f59c`
add flat COLOR frontend/topology and correct FIRST/LAST provoking vertices.
`9fa80ea` uses the authenticated160-byte FRAME ABI for both first and repeated
application draws. `14f1fbe` combines Mesa fixed-function lighting's split RGB
and alpha stores on an owned NIR clone: original Mesa directional/positional
producer controls pass4,096 independent GPU transform/lighting components,
within existing instruction/constant limits. These are host proofs; actual
glxgears has not passed. Full clear, depth and native flat/cull state are still
gated pending their complete source-derived implementation and hardware test.
DRM `a918988`/`376cee8` prepare bound attachments from authentic INIT and encode
live HZ/depth clear bodies; the complete bound clear/draw sequence is unfinished.

The subsequent clear implementation is now committed: DRM `6df69cd` emits
complete sampled b5e70 (3516/3520 bytes with live page masks), and `48fe227`
emits bound attachment/HZ/depth setup (1160/1164 bytes and five ENDs).
Both pass complete source comparison, varying placements/alignments and
atomic-refusal host checks. This code has **not** been hardware-tested yet;
the stale-edge hardware result above predates these full clear producers.
The apparent pointer0x40888889 is proved to be an immutable constructor float,
not an allocation address; [source evidence](../tests/m9-faa-gl-one-color-20261007/sampled-clear/README.md).

Mesa `363c0f3` also admits the original fixed-function fragment COLOR0 legacy
interpolation, resolving its smooth/flat mode from live Gallium state.
`2a861dd` fixes FAA clear dispatch: journal the request and stop before ordinary
clear GPU submission, preserving retained FAA command state. The production
wrapper test passes. Complete native FRAME sequencing, one-stream depth draw,
cull/flat packets, and sustained command-pool reuse remain unfinished; no
working glxgears or ioquake3 FAA run is claimed. Further hardware testing will
combine these ready changes instead of rebooting for each small edit.

Mesa `ea4be47` preserves pending color/depth values across separate clear
calls and merges component masks. `c5caa09` retains the exact authenticated
Lock increment for subsequent Unlock/NEXT validation. `802ab27` forwards a
bounded full black/depth-one reset plus live depth/cull/flat state, consuming
its clear journal only after successful presentation. Production-function
host tests and the complete20-object Gallium build pass; the FRAME client
still refuses unimplemented native profiles before submission.

DRM `0ab1f57` integrates the bounded depth DRAW/Lock suffix and `a4df024` the
source-derived flat/CW/CCW packet encoder. `a31eb3b` integrates the fenced
13-END depth submission lifecycle, **with public activation still gated**.
The retained suffix is not yet proved valid for a fresh INIT first draw, and
its fixed source shader profile still needs the live COLOR adapter. The
source epoch difference is explained by retained queue state, not shader
identity: the control finalizer changes the next queue epoch. Runtime commands
must derive that epoch from the retained queue; neither historical delta is
a portable replacement. [Queue/source evidence](../captures/m9-faa-graph-public-vs-20261007/next-state-controls/BOUND-REPEAT.md).
No new hardware result supersedes the six stale edge pixels above.

## October 7: six-vertex FAA draw executes; frame reset still fails

DRM `102a969` corrects real launch counts instead of overwriting packet lengths.
The M9128 test completes first3 then changed6 vertices through Lock/read/Unlock,
release and file close without a wedge. Both new triangles render, with9,936
checked interior pixels and zero interior/exterior errors, but six edge pixels
retain old color. This is batch execution progress, not full frame correctness.
Mesa `73b22bb` integrates the authenticated repeated-draw client; `46d4f02`
adds the genuine six-vertex shader fixture. Mesa `b871c33` now wires opt-in
application draws through live upload/read/Unlock/presentation, and `cb1a062`
adds smooth quads/quad strips without CPU transforms. Pending clear, depth,
culling and flat shading remain explicitly refused. DRM `1fc1972` retains the
three distinct bound color/HZ/depth allocations; their clear/draw activation
is still unfinished. Focused host checks and complete builds pass, but this
new application/depth code has no hardware qualification.
[Exact batch evidence and images](../tests/m9-faa-live-color-six-20261007/README.md).

The original two-output vendor COLOR4 profile (360-byte program, gather40)
also passes an authentic Lock190/read/Unlock1b0 cycle and the pixel oracle,
producing the exact same first image. Its real receipt now permits the
matching post-Unlock vendor continuation; repeated clearing is still open.
[Original-profile receipt and image](../tests/m9-faa-original-color4-hardware-20261007/README.md).

## October7: repeated live COLOR completes, frame reset has three stale edge pixels

Native ade4713 and the full-cycle helper complete first Lock190/Unlock1b0,
black TARGET clear, changed live CB0/geometry and second Lock290/Unlock2b0,
release and file close with a healthy empty queue. Frame2 has9,801 colored
pixels and zero checked interior/exterior errors, but three edge pixels
retain old-frame color contributions. Complete frame-reset correctness is
therefore still unproved. The single-frame GL COLOR pixel success below
remains valid. Mesa7f90fcc also adds an unactivated presentation helper that
copies F021 rows, authenticates Unlock, then writes the live destination.
[Exact repeated-frame receipt, images and failing pixel oracle](../tests/m9-faa-live-color-next-20261007/README.md).

## October 7 latest: live GL COLOR triangle now passes hardware

The combined Mesa5c7fc89 output-arity fix and DRMfa9f2f6 raster/gather
connection fix render the live GL triangle correctly:9,801 colored pixels,
440 partial edges, zero checked interior/exterior/channel errors. Full
Lock190/readback/Unlock1b0/release/file-close passes with an empty healthy
queue. GPU observations confirm the caller color and GL half-depth transform.
Application repeated draws and presentation are still being implemented;
this is not a full glxgears/ioquake3 qualification.
[Original image, inputs and hardware proof](../tests/m9-faa-live-color-20261007/README.md).
The earlier black-image results below are retained as historical evidence.

## October 7 resumed: live COLOR hardware result

FAA work resumed at the user's request, with parallel application, native and
vendor-source agents. The first live COLOR four-stage upload and the build
with corrected packed-record readers both retire through Lock190, F021 read,
Unlock1b0 and cleanup, helper exit0; generation0/queue0/wedged0 remain healthy.
Both images are entirely black. Full application FAA remains unfinished.
Canonical DRM1fd825f contains the reader fix and current-frame authentication;
the unchanged live GL upload comes from Mesa fc5cfc1. Further work now targets
the remaining producer/consumer joins and application lifecycle.
[Exact two-run evidence](../tests/m9-faa-live-color-20261007/README.md).
Earlier wind-down notes below are historical and superseded by this resume.

## October 7 wind-down checkpoint

Work is paused at the user's request. Canonical Mesa9128470 includes the
source-derived COLOR0 FS64 frontend, an actual lowered-NIR COLOR pass-through
classifier (vector/scalar stores and smooth pixel interpolation), live GPU
carry/gather/position uploads, and the variable136-byte COLOR_LOCK client.
The client authenticates two-vector INIT+a0 and four-vector INIT+e0 final
Lock receipts, then independently waits; COLOR Unlock authenticates Lock+20.
Owned storage capacity and all program/range/record/declaration metadata are
checked. The complete19-object DSO build and focused host controls pass.
`native_admission` stays false; no new COLOR hardware run was performed.

The one-real-COLOR four-stage upload is independently generated and remains
experimental. Its representative fixture is FS64/carry120/gather32/position200,
with168live constant bytes, three32-byte raw records and five semantic VS
declaration words. No nonexistent TEXCOORD output is added. The genuine vendor
four-stage control has **two real outputs** and now has a complete typed
DRAW/Lock plan; it does not itself prove the experimental one-output flow.

The original one-color fused VS changes21of29scheduled slots when a live ADD
is introduced. The ordinary Mesa tail also differs. A captured fixed suffix
must not be spliced into arbitrary MVP/lighting. All GL position arithmetic
in the current carry builder, including half-Z conversion, stays on the GPU.

See [the wind-down handoff](parhelia-m9-wind-down-20261007.md) for final code,
hardware evidence and remaining application/presentation work. The sections
below describe the earlier fixed-upload and original source boundaries.

The generalized Mesa `m9_faa_live_gl_build` added in `385b2c2` is an owned
CPU snapshot. It compiles a retained GL vertex NIR on a clone, keeps the
GL-to-D3D half-Z operation in the GPU carry program, resolves live CB0 keys,
and copies program, constants, declaration, and input records. Its
`native_admission` result is deliberately false. No generalized draw or
glxgears hardware receipt follows from a successful snapshot.

The original M9128 public-VS controls and the qualified first live draw
establish these field relationships for their two-complete-stream profile:

| Field | Live derivation | Evidence boundary |
| --- | --- | --- |
| Four C2 program entries | Cumulative finalized vector slots times eight, in fragment/carry/gather/position order; C2 selects `entry_bytes[i] / 8` | The original profile uses 4/7/5/25 slots and entries 0/4/11/16. The generalized linker computes the cumulative byte offsets. |
| C3 constant range | Retained constant owner base plus the vector's dense begin/end; pair count is `(end-begin)/8`, encoded as `0x00500000 | (pairs << 8)` | The first live FS used `[16,32)`, two pairs, whereas the captured source used `[0,32)`, four pairs. The kernel correctly changed all three fields for the live case. |
| Vertex record | Source-map writer uses the compiled live input layout and bound Gallium formats; `record_bytes` and vertex count determine the snapshot byte extent | The qualified public command uses exactly two FLOAT4 inputs, a 32-byte record and three vertices. The generalized snapshot may produce another stride/count, but its GPU command lengths are not yet qualified. |
| Input declaration | `m9_vs_decl_words` generates two words per live attribute, optional texture words, and the terminal word | The qualified command has exactly five declaration words. Its packet offsets and lengths cannot be reused for another word count. |

The older kernel public-VS ioctl takes the fixed 796-byte
layout/program/constants/geometry image, requires a 328-byte four-vector
program and five declaration words, and checks the admitted program's SHA.
Its command offsets, counts, fixed geometry `+0x100` placement, and later
relative branches belong to that bounded image. A generalized native
producer needs a sized owned upload, packet length/declaration rebuilding,
live owner offsets/capacity checks and source-proven raster/fragment routing;
changing only the C2/C3 fields is insufficient.

The one-color GL1 case has a distinct original source shape. The generalized
Mesa carry compiler and builder now support **one or two real complete vec4
linkage streams**. Their four-vector output is distinct from the original
one-color fused two-vector profile. The ordinary Mesa pass-through color
layout requests only `COL0`.
We therefore ran the original M9128 public compiler with a genuine
`ps_2_0 dcl v0; mov oC0,v0`, POSITION and COLOR VS outputs, and otherwise
unchanged viewport, declaration, vertex bytes and stride. It produced two
vectors: FS64 + VS232 = 296 bytes. The original public DRAW returned EAX1,
with a 624-byte first secondary block, one 4968-byte source END, and no
deferred marker/relative rewrite. Its next original `252b9` input desired
`0x100` and remained unread in the capture.

The paired control adds a real TEXCOORD VS output which that same FS does not
read. Its FS64 bytes are identical, but it produces four vectors:
FS64 + carry56 + gather40 + position200 = 360 bytes. The first secondary
block is 736 bytes, two deferred markers and one relative rewrite appear,
the independent source END is 4992, and the original unread query is `0xf0`.
This proves an unused second output is accepted by the original CPU compiler
for this input, but it changes the whole command topology. It is not a
portable zero-output alias or a reason to pass nonexistent linkage from
glxgears. The source-supported one-color route should get its own bounded
two-vector native producer; the experimental one-real-stream four-stage
producer must independently pass hardware rendering.

Continuing only source nonblocking `25270` dependencies with the historical
actual INIT `0xb1` as an explicitly labeled branch control returns the
one-color public DRAW and constructs the normal Lock suffix. The source END
sequence is DRAW 4968, Lock 8520 twice and final 8532. The true positive
CPU wait at `4e1d1` asks for `0x150`; it is unread. The archived `+20`
zero-floor control applies to the two intervening maintenance reads. This
does not claim an actual post-DRAW status, Lock return, pixel image or a
glxgears shader. The source images and refusal checker are in
`captures/m9-faa-gl-one-color-20261007` and
`tests/m9-faa-gl-one-color-20261007`.

Sources: Mesa `m9_faa_live.c`, `m9_vs_compile.c`, `m9_faa_program.c`,
`m9_vs_decl_words`, and `tests/faa-live-public-upload-20261007/GENERALIZED.md`;
M9128 `parhelia-m9-faa-public-vs-preprocess-interface.md`,
`tests/m9-faa-graph-public-vs-20261007`, and the actual first live-color
receipt in `tests/m9-faa-public-vs-unlock-20261007`. Apply
`gpu-capture-replay-policy.md` to any future producer.
