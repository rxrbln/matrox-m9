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
* Polygon stipple: vendor form captured (`raster/batch.tgz`
  g56/polygon_stipple; 79=ea010020, 70=0004111c, an extra draw at c3=4 not
  decoded).
* Cube faces above 16x16 as render targets: placement not measured.
* Single-pass multi-target blending: destination registers of targets 1..3
  not captured.
* Native vertex temporaries `0x11`/`0x15`/`0x17`/`0x19`/`0x1b`/`0x1f`
  (`parhelia-m9-native-vertex-backend.md`).
* Fragment refusals: interpolation registers, control flow, value splits.
