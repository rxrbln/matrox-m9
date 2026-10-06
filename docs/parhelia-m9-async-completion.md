# M9 async completion and presentation contract

2026-10-06; M9128/M9 only. Native reference before this change: Q3 GL1,
1024x768 fullscreen, vsync 0, 387 frames / 6.8 seconds = 56.6 FPS, about
55 jobs/frame, zero uniform splits and about 7.5 ms/frame ioctl time. Those
numbers were measured by the target owner on the earlier uniform-run build;
they are not measurements of this completion change.

## Actual correctness gap

Kernel M9 `async_submit=Y` posts one job and returns its *issued* writeback
target. The next submit drains that job before rewriting the one shared
secondary command image. This is a single pending job, despite two primary
buffer slots. Existing Mesa `m9_context_flush` constructed an already-signaled
fence for every returned point, and `m9_fence_finish` returned that Boolean
without hardware access. Its comment incorrectly asserted that submission
ioctls were synchronous. Therefore a GL flush fence/GLFinish could report
completion while the final posted job still ran. Pixel readback and resource
access ioctls drain separately, so ordinary rendering/readback tests concealed
this gap.

The kernel direct tiled scanout path programs the display address directly.
It relies on the ordinary DRM framebuffer reservation dependency, but user
render jobs currently do not publish a completion fence to `dma_resv`. Correct
Mesa fence waiting alone does not establish that presentation dependency.

## First coherent fix: observed completion timeline

`WAIT_POINT` is a dedicated 32-byte render ioctl, command number `0x0d`:
`point`, `timeout_ns`, `completed_point` (three u64), `flags`, `pad` (two u32).
It never posts a command. `flags`, `pad`, and input `completed_point` must be
zero. A request for an unaligned or future point is rejected. Point zero is
a capability query returning the last observed completion. Timeout zero
polls. A finite wait is bounded by the requested timeout and the driver
watchdog; U64_MAX uses the watchdog bound. Expiration returns `-ETIME` and
leaves pending work and its ownership intact. Transport-lock contention counts
against that same wait budget; a zero-time poll cannot block behind a submit
drain. Device failure returns `-EIO`;
an interrupted wait returns `-ERESTARTSYS`.

The transport now keeps separate submitted/completed 64-bit points using the
existing 32-bit target wrap extension. Async posting advances only submitted
progress. An actual matching hardware writeback advances completed progress
when the existing owner is drained. Synchronous posting advances both after
its real writeback wait. The transport lock prevents a different job replacing
the owner while a wait observes it. A status nibble is masked as in the real
vendor/writeback comparison; observing a later or different raw target does
not invent completion of the pending owner.

Mesa marks a nonzero submitted point unsignaled, probes this ioctl without
posting, and uses it in `fence_finish`. Successful completion is cached with
atomic accesses. Kernel errors, early timeouts and short completion progress
remain unsignaled. On older kernels a zero-time poll returns false; an explicit
positive wait uses the existing synchronous NOOP_FENCE drain. That older ABI
cannot enforce an arbitrary finite caller timeout, so this compatibility path
can run up to its existing watchdog. The new ioctl does enforce its bounded
wait. Empty fences and NULL fences remain immediately complete.

This is a correctness prerequisite, not a demonstrated FPS improvement. The
single shared secondary/BO ownership protocol is unchanged.

## Focused verification

`tools/m9-completion-check.py KERNEL_M9X MESA_M9X` extracts the actual kernel
timeline/wait functions and Mesa fence create/finish functions, compiles them
with controlled writeback, clock, drain and ioctl adapters, then exercises
43 cases under AddressSanitizer and UndefinedBehaviorSanitizer. It checks ABI
layout equality, async unsignaled posting, zero/finite/infinite-bounded waits,
exact writeback matching, delayed completion, lock contention, invalid ioctl
inputs, unplug/disabled-device admission, interrupts/device errors,
64-bit wrap, cached completion, older-kernel fallback, and actual failed-drain
BO/pin quarantine versus real retirement. LeakSanitizer is
disabled because the workspace sandbox uses ptrace; every allocated test
fence is freed explicitly. Actual modified kernel objects compile, Mesa batch
suite and context/screen objects compile. Native validation remains pending
the target owner's next controlled kernel deployment.

## Next required slices, still open

1. Publish real per-job `dma_resv` fences and signal from observed completion
   even when no next submit arrives. KMS/foreign consumers must receive a true
   dependency. Reserve/lock all participating BOs before publication, preserve
   f587 same-posting-lock pin ownership, and wait only foreign hazards that
   cannot be satisfied by the ordered M9 stream.
2. Timed-out async jobs now retain their pending owner, BO references and pins;
   new submits honor a drain error, and fini releases quarantine only after
   clearing PCI bus mastering. This is tested against the actual drain function.
   Synchronous user submission failure after posting still needs the same
   ownership transfer; the next slice must report the true doorbell result from
   user-stream framing and distinguish an unposted failure from active DMA.
3. Provide at least four independent secondary command slots and retirement
   ownership vectors. The current shared 32 KiB secondary cannot be rewritten
   with a previous reader in flight. Primary append capacity alone does not
   solve this.
4. Add retirement-aware Mesa source/index/system/CPU-written arena banks and
   backpressure. Current double buffering permits only one outstanding job;
   posting a third job without bank retirement would overwrite the first.
   Include CPU BE clears, inter-pass scratch, state/resource destruction and
   translation-table changes in this ownership audit.
5. Capture which real full-screen presentation path is active. Direct scanout
   is already enabled; non-addressable tiled windows use a drain/copy path.
   Avoid attributing Q3 ioctl time to a presentation copy without that evidence.

The real Linux vendor ExecuteDma captures in
`parhelia-m9-cold-transition-submit-decode.md` and
`parhelia-m9-requester-drm-contract.md` establish primary ring appends and
switch/ownership sequencing without an ordinary per-submit no-op drain.
They support a queue design, but do not authorize skipping reader retirement
of the clean-room driver's current shared images. No speculative FAA packet,
opaque auxiliary-bank interpretation, or vendor success return is introduced.
