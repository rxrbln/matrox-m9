# M9148 piglit status (m9x Mesa driver)

<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->

Target: nano (VIA Nano VX900, M9148).  Runner: `run-chunk.sh` (python-free,
one test per hardware-lock hold), lists `m9x-basic.txt` (452 core
`spec@!opengl 1.x/2.x`) and `m9x-rest.txt` (5710: GLSL 1.10/1.20, shaders,
extensions, GLX).  Results on nano under `/root/tmp/piglit/results-m9x/`.

## Core set (MEASURED)

| build | pass | fail | crash | timeout | skip |
|---|---|---|---|---|---|
| m9x d2e61c9 (2026-10-02 morning) | 185 | 92 | 7 | 1 | 165 |
| m9x 00f660e (stencil, blend, pixel paths, raster, textures) | 236 | 48 | 0 | 1 | 165 |
| m9x b1298cb (+ fragment compiler) | 262 | 22 | 0 | 1 | 165 |
| m9x c4dd850 (+ render targets, GLX, sRGB/GL 2.1, discard) | 269 | 17 | 0 | 1 | 163 |

Patches: `patches/` carries the driver state as of m9x 6780573, kernel
f587517, plus `mesa-generic-fixes/` (5 commits, format-patch).

Later merges (fog, exact 10-bit uploads, constant coordinates, derivatives,
shadow/projective fetches, variable indexing, native vertex fixes; m9x
5ef40cb + outer mesa 3ceb09e) are measured per agent only.  Non-core, first
1310 tests: 870 pass / 297 fail / 3 crash on d2e61c9 against 1056 / 133 / 0
on c4dd850.  GL 2.1 (shader-lowered sRGB textures) turns ~1890 skipped
glsl-1.20 tests into real results.

## Reasonable skips and failures (with evidence)

* **Accumulation buffers:** Xorg loads Mesa's `dril` stub, which builds GLX
  configs from EGL configs without accum (the client driver has 36 accum
  configs, the server list 0).
* **Multisampling:** the FAA engine writes 0 of 318464 store dwords whatever
  the bindings; the master enable is not recovered.  Needs a vendor capture
  of a multisampled draw.
* **EXT_texture_integer:** GL 3.0 class.
* **glx-make-current, glx-multi-window-single-context:** upstream Mesa GLX
  `releaseDrawable()` (fails on llvmpipe too, expected-fail in Mesa CI), plus
  a piglit glXDestroyWindow bug.  **glx_ext_no_config_context:** libglvnd 1.7.0.
* **glx_arb_sync_control `-divisor` timing:** only under the runner's old
  forced `vblank_mode=0`; the tests need X DPMS off (`xorg-m9x-auto.conf`).
* **streaming-texture-leak:** not a leak; 20 GiB of uploads at ~160 MB/s
  pass in ~125 s, above the 30 s limit.
* **texsubimage-unpack:** stores are exact, but the sampler's 10-bit to
  8-bit result is off by one for 256 of 1024 codes and not monotonic.
* **draw-pixels:** piglit expects GL_RG errors without ARB_texture_rg.
* **fs-dfdx/dfdy-accuracy:** hardware derivatives are per 2x2 quad.

## Open, with what would settle it

* Kill lane artifact (early-z, occlusion-query-discard, pbo, dlist-bitmap):
  vendor MMIO snapshot during fs-discard-cond
  (`parhelia-m9-mesa-fragment-backend.md` section 4a).
* Logic ops: implemented and exact (m9x 6780573).  Core on 5ef40cb+logic:
  271 pass / 15 fail.  Regressions after c4dd850 seen on both 5ef40cb and
  5ef40cb+logic: max-samplers (+ border, "a draw's records did not fit one
  job"), gl-1.0-dlist-bitmap and scissor-bitmap (also seen intermittently on
  c4dd850); owner to be found among the fog/10-bit/fs3/vs merges.
* GL_DITHER is ignored for 8-bit targets (`m9_dither_ef`), see the logic-op doc.
* Polygon stipple: the vendor form is decoded (CAPTURED, g56/polygon_stipple,
  submission 13), not implemented.  It is a two-pass job: pass 0 (c3=0)
  draws the quad into a record surface (b0=10ec1000, b1=ee090002) with a
  4-slot program `fffd191c e28fccdf | e640911c b040015f` reading
  gl_FragCoord (7a) and writing roughly 1 + (x+0.5)/32; pass 1 (c3=4) routes
  that record (74=00ff0110, 78=10ec1000, 79=ee090002) into unit 0
  (70=0004111c, 78=11ec1000, 79=ea010020, 7a=00010020, 7c=00009200,
  7d=0ff80000), samples a 32x32 float texture of +1/-1 in a tiled order, and
  kills through the GLSL kill slot (cls 5) after a 6-slot select tail;
  ef=00000655.  So the vendor stipple is kill-based and will likely show the
  kill lane artifact.  The vendor writes c7 at the head and tail of that
  range; do not reproduce it.  Pass 0's constants are not decoded yet.
* GL_DITHER: ef[7:0] = 0x55 is dither off, 0x00 on (CAPTURED, fragops
  dither cases); `m9_dither_ef()` should take the rasterizer's dither flag.
* Cube faces above 16x16 as render targets: placement not measured.
* Single-pass multi-target blending: destination registers of targets 1..3
  not captured.
* Native vertex temporaries `0x11`/`0x15`/`0x17`/`0x19`/`0x1b`/`0x1f`
  (`parhelia-m9-native-vertex-backend.md`).
* Fragment refusals: six vec4 interpolants per raster pass is a MEASURED
  engine limit (`parhelia-m9-raster-stream-limit-re.md` section 6: registers
  0x40+8i, i=0..5; a seventh reads garbage); the vendor splits into passes
  (vs-varyings-max: 14 varyings as 2/6/6), so the open question is why the
  splitter's interpolant cut does not fire.  The TXB record route is CAPTURED with literal bias +1 and no mip filtering;
  varying bias and mip-selection semantics still require the prepared vendor
  probes ([capture boundary](parhelia-m9-txb-capture-boundary.md)).  Control flow, value splits and the shadow reference clamp
  are not started.
* Regressions after c4dd850: only max-samplers (+ border) is real (passes
  5/5 on c4dd850); bisect candidates a535381, fs3 32eabe7..b885bdd, vs
  4687f1b..5ef40cb, 6780573.  dlist-bitmap and scissor-bitmap fail 5/5 on a
  fresh c4dd850 build too, rastercolor 2/5, so they are not caused by the
  merges; they passed in int4 before a reboot (environment or state, open).
