# M9128 generalized GL FAA upload boundary

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
