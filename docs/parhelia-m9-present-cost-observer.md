# Observe the actual fullscreen swap route

The current source offers three materially different Present routes. Their
physical use by current fullscreen Q3 is unobserved. This patch adds an opt-in
route observer to choose the next performance change from the actual route.
It does not enable flipping or change rendering, buffer ownership, waits,
damage, update-region release or idle-fence notification.

`present_execute_copy` first offers the update to the DDX copy callback, then
uses `present_copy_region`/CopyArea when that callback rejects it. The M9 DDX
callback admits only its own tiled DRI3 imports with a backing BO. In that
case it invokes SURFACE_ACCESS READ for each clipped rectangle into the
CPU-mapped destination. That kernel operation drains outstanding GPU work
before CPU access. Glamor DRI3 imports use a separate importer, so enabling
glamor does not by itself prove admission to this CPU-detile callback.

CopyArea can select glamor FBO-to-FBO draw, an intermediate FBO copy, CPU
upload, GPU readback, or CPU fallback. The observer is scoped to the actual
Present copy call; unrelated glyphs, screenshot readbacks and flush-side
work do not count. Successful glamor helpers prove host branch acceptance,
not GPU completion or correct pixels. The DDX handled count includes frames
the existing callback drops, so separate detile/error/drop counts are needed.

PageFlip defaults to FALSE in this local modesetting source. Its optional
configuration and real admission still matter: accepted pageflip requests
and actual flip completion events get separate counters. No old P690 flip
proof or idle MMIO value establishes M9128 fullscreen flip support.

At 1024x768, a full 32-bit frame is 3 MiB. If CPU detiling is physically
selected for every frame, 38.9 FPS corresponds to at least 116.7 MiB/s of
destination stores plus the source reads and synchronization. A full GPU
copy reads and writes at least 6 MiB, or 233.4 MiB/s at that rate, before
tile overhead and shader work. These are conditional traffic calculations,
not measurements or a prediction of FPS gain. Current Q3 and queue-map
glmark results are not a controlled A/B comparison.

## Opt-in trace and one target run

Run the matching Xorg, modesetting and glamoregl artifacts built together.
Set `M9X_PRESENT_PROFILE=60` in the **Xserver environment**, then request one
1024x768 fullscreen Q3 timedemo with the current FAA0/vsync0 settings.
Do not take XGetImage screenshots during the timedemo; collect an image
after it finishes. Preserve normal PageFlip configuration for this observation.

`M9X_PRESENT copy` reports the first copy and cumulative totals every 60
copies, including source/window/screen dimensions, live pageflip/glamor
booleans, DDX handled vs CopyArea fallback, and CPU microseconds in copy and
flush separately. Each route field is `calls/pixels/cpu_microseconds`:

- `detile`: successful SURFACE_ACCESS read; `detile-error`: failed attempt.
- `drop`: existing DDX unusable-destination drop.
- `gl-draw`, `gl-temp`, `cpu-upload`, `gl-readback`: accepted glamor branch.
- `gl-reject`: rejected glamor attempt; `cpu-bail`: fallback invocation.

Only detile includes per-route CPU timing; other routes use total copy time.
Those host times may include waits and enqueue work. They do not measure GPU
execution. Rectangle pixels come from the actual clipped bounds. Cumulative
totals may include different windows; use window/dimensions and an otherwise
idle session to qualify the fullscreen application. `flip=accepted` and
`flip=event` are independent request/event counters; neither substitutes for
an image check. Missing trace lines are not proof that flips occurred.

If CopyArea and `gl-draw` dominate with no readback, the next candidate must
address the GPU copy or prove fullscreen scanout/flip ownership, rather than
optimizing a CPU detile path that is inactive. If detile or gl-readback is
active, its measured host wait/copy cost provides the priority. Reusing the
previous application fence observation can independently reduce already
reached waits; it does not eliminate a required presentation copy.

## Host proof

The signed patch is `xserver-m9x-present-profile.patch`. Base and patched
source hashes, full build artifacts and helper proof are retained in
`tests/m9-present-profile-20261006`. Reproduce without changing shared source:

```sh
python3 tools/m9-present-profile-build.py \
  /home/ai/src/parhelia/xorg-server-parhelia /tmp/m9-present-profile-rebuild
```

The isolated full Xorg, modesetting and glamoregl build passes. The five
observer symbols are exported by Xorg and consumed by its matching modules.
The helper fixture extracts actual patched Present and glamor functions.
ASAN/UBSAN controls cover disabled/invalid/enabled configuration, requeue,
handled/fallback update ownership, copy-only scope, flush and idle ordering,
accepted/event separation and preserved errno around clock observations.
Negative controls remove flush or extend copy scope through flush; both are
rejected. LeakSanitizer alone is disabled because the managed runner is
traced; address and undefined-behavior checks remain enabled. No target run,
GPU completion or visible image is claimed by these CPU fixtures.
