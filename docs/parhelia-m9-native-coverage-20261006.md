<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
# M9128 GPU geometry coverage and presentation evidence, October 6

Current runtime `335d442` (rendering change `5ad6c17`) supports five native
output streams in its compiler but defaults to **two** hardware-delivered
streams. Texture transforms and coordinate sharing are enabled; a separate
coordinate beside another stream and native multi-pass fragment shaders remain
gated. Vendor `vs-varyings-max` transports fourteen varying slots using nine
preprocessing draws and three raster passes. That captured multi-draw grammar
has not yet delivered a third native varying through the modern driver; five
previous chain/descriptor experiments rendered nothing or stalled. Do not
raise the default cap based on the vendor's larger API limit.

## Current Quake3 evidence

`tests/m9-q3-profile-20261006` retains the M9128, 1024x768 fullscreen,
OpenGL1 renderer, uncompressed 32-bit texture, vsync-off diagnostic. One
387-frame timedemo reports **49.5 FPS**, against the immediately preceding
same-board **49.6 FPS** ballpark. It is not a P690 improvement.
Steady sixty-frame intervals average approximately:

| Evidence | Per frame |
| --- | ---: |
| Wall time | 19.8 ms |
| Draw scope, including nested work | 13.4 ms |
| Emit scope, nested in draw | 10.6 ms |
| Submit scope, nested in emit | 8.9 ms |
| Ioctl scope, nested in submit | 6.9 ms |
| Jobs / draws | 54 / 129 |
| Program / depth / mask / clip splits | 53 / 36 / 5 / 3 |
| Uniform / vertex-ceiling splits | 0 / 0 |
| Native draws / draw module per 60 frames | 0 / about 7500 |
| Refused / skipped | 0 / 0 |

The timing scopes overlap: summing them overstates total cost. About 2.8 ms
of draw time lies outside emit, while program changes dominate job boundaries.
Per-object uniform-span batching is therefore not the first performance fix
for this particular GL1 workload. The active fallback reason needs the interval
reason tally; the initial temporary context teardown (`native 0, module 0`)
does not identify gameplay coverage. A short gameplay interval suffices to
capture that tally without repeating the complete timedemo.

## Implemented coverage prerequisite, pending native validation

Mesa folds short uniform-decided `if` branches to `bcsel` ALU before driver
finalization. `m9_nir_find_inlinable_uniforms` inspected remaining if/loop nodes
only, leaving those selects unspecialized. A real scalar NIR fixture with three
vec4 inputs therefore emitted all three even when the uniform selected one.

The shared discovery now follows select conditions that contain only uniforms
and literals. The early native geometry check defers inlinable shaders to the
existing compiled-layout validator, keeping the hardware output cap unchanged.
Offline choices 0/1/2 now emit exactly their one selected input; choice 3 really
reads three and stays distinct. A mixed input/uniform condition still marks
zero inlinable uniforms. Exact required vendor programs remain **23/23**, full
corpus **177/182**, while total emitted slots fall **1660 -> 1655**.

`tools/m9-mesa-glsl.c` adds `varychoice`: four uniform changes in one context,
full readback against independently scaled interpolated colors each time. Its
native acceptance check is three GPU draws, one CPU fallback, and zero wrong
pixels in all variants. This is a coverage fix, with no measured Quake3 gain
until the active reason is captured and the native fixture passes.

## Remaining prerequisites

* Three or more native streams: preserve full vendor preprocessing chain
  state/lifetime and resources, then explain why the existing matching route
  still does not deliver output; another output-register seed guess is not a
  prerequisite proof.
* Per-object uniform batching: establish per-draw constant-span relocation and
  visibility from a complete vendor job. Current GL1 Quake3 has zero such splits.
* Submission/presentation: the existing path already overlaps submission and
  engine work. These intervals cannot independently attribute all ioctl time
  to Present or vblank; vsync is disabled. Root should inspect active fallback
  and shader-driven job boundaries before changing presentation synchronization.
