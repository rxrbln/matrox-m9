# M9 bounded queued-submit design checkpoint

2026-10-06; design, not executable queue support. The committed completion and
implicit-fence work retains one in-flight job. Native qualification of that
slice precedes a queue deployment. Current Q3 baseline is 56.6 FPS at
1024x768/vsync 0, around 55 jobs/frame and 7.5 ms/frame ioctl time; no queue FPS
improvement has been measured.

## Evidence and smallest safe queue

The retained genuine Linux vendor ExecuteDma captures show primary BmBuffer
appends and endpoint doorbells with ownership/ring switching. Normal submission
does not require a CPU writeback drain after each draw. Existing class-1
secondary framing and completion records are native-proved, so a four-job queue
can reuse those packet forms while giving each job an independent command
image. No opaque auxiliary `0x500` bank or uncaptured ring switch encoding is
needed for the first implementation.

Kernel user-submit currently overwrites one 32 KiB secondary, then appends
primary `2c/2d` range references. Two 32 KiB primary slots do not make that
secondary independent. Mesa alternates two sets of four CPU-written arenas
before filling a job. Removing the next-submit drain would let job three's
CPU producer overwrite job one's input. A new kernel must retain compatibility
with older Mesa: queuing needs an explicit new submit opt-in, admitted by a
validate-only probe, rather than silently changing flags-zero execution.

The first bounded implementation needs four independently allocated/mapped
32 KiB user secondary banks and a four-entry FIFO. Each FIFO entry owns its
target/extended point, primary slot/range, secondary slot, true implicit fence,
BO/pin vectors and preallocated retirement node. The existing diagnostic
secondary keeps its original exact 32 KiB extent and captured initializer.
User banks must use real compact-PTE GPUVA, not raw DMA backing addresses.
The diagnostic's 44-PTE bookkeeping limit must not be expanded by conflating
its source/system/secondary set with the new user-bank mappings.

Allocate/map every bank before publishing work. Append while a bank and primary
space are available. Queue-full backpressure waits for the oldest entry and
reuses only its retired bank. Every existing operation that requires engine
quiescence (diagnostics, translation edits, shared CPU clears, primary buffer
switch, synchronous NOOP, final teardown) drains the whole FIFO. The first
version can retain a full drain at primary-slot rollover; a vendor ring-switch
packet optimization can follow separately.

## Completion with more than one target

The current exact-equality waiter is correct for one owner, but can miss an
older queued target once a later job overwrites the common writeback. FIFO
retirement needs an observed monotonic high-water mark with explicitly bounded
32-bit wrap arithmetic. Each job's existing final completion/footer remains
unchanged. Dependency tokens are bounded by `PARHELIA_M9_DEPENDENCY_LIMIT`
`0x400`; four jobs' maximum token span stays far below the signed half-range.
The implementation must reject any span violating that bound and distinguish
the status nibble from the target. WAIT_POINT must retire all actually observed
FIFO entries up to its point; it cannot merely wait for the newest submitted
target or treat a returned point as completion.

Required offline proof includes skipped intermediate writeback observations,
status bits, wrap at `0xfffffff0`, an unfinished head, queue-full backpressure,
bank reuse only after observation, foreign dependency errors, unposted errors,
last-job automatic completion, timeout quarantine and primary-slot rollover.
Native proof then checks queued geometry/matrix A-B-A, a longer multi-job input,
depth, ordinary texture, and a single short 1024x768 Q3 regression/profile run.

## CPU writer ownership audit

| Object | Current owner/use | Queue requirement |
| --- | --- | --- |
| source/system/index/cpu_streams | CPU-filled, two arena sets per context | Five sets: four posted readers plus one CPU builder; each set has a last submitted point and is observed retired before CPU write/growth |
| userspace command BO | Kernel copies validated image before return | No GPU bank required for this copied staging image; keep context serialization |
| native VS transient views | GPU preprocess writes, raster reads in ordered job | May remain shared with proven final completion/order; kernel per-job refs/pins retain lifetime |
| per-screen inter-pass record | GPU-only split-pass scratch | Keep ordered access and existing intra-job dependencies; do not CPU clear in flight |
| per-target BE | CPU memset, currently two target slots; default context BE can also be CPU-cleared | More retirement-aware slots or wait for the bound object's last reader before memset; two-slot shape rotation cannot be assumed safe with four pending jobs |
| shader/fragment/program BOs | Normally immutable GPU-read code | Keep per-job refs/pins/fences; any patch/upload requires retirement before CPU modification |
| query result slots | GPU writes and CPU pending-sentinel initialization | Track slot/page reuse; pending sentinel writes cannot overwrite a queued query's result |
| render/texture/depth/HZ surfaces | GPU access, occasional CPU upload/window access | Ordered own fences plus foreign hazards; CPU/window paths must drain/wait all relevant readers before writing |
| FAA groups/store/aux/pool | Shared context state plus initial/growth CPU writes | Keep FAA command/lifetime proof separate; all CPU changes need complete ownership, not a blanket queued exception |

Mesa arena rotation must select an actually retired bank before writing one
byte. If none is free it waits the oldest required point through WAIT_POINT;
this is bounded backpressure, not a guessed delay. Destruction or growth of an
old arena must also respect its point. GPU-only local scratch can stay shared
because jobs execute in the same ordered primary stream, while CPU-modified
objects need explicit retirement. The existing implicit fences make foreign
consumers and the normal KMS prepare_fb dependency possible, but do not prevent
an application's own mmap producer from overwriting an in-flight input.

Queue capability, command slots, ownership FIFO and Mesa banks should land as
one coherent opt-in feature. Keep the current single-job path available for
older kernels/clients and diagnostics. Do not turn queue support on solely
because a Boolean reports async posting.
