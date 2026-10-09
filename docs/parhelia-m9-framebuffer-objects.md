<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
# M9 framebuffer objects in the m9x Mesa driver

State of GL_ARB_framebuffer_object on the M9128 (nano), 2026-10-09, agent
m9fbo. `MEASURED` = piglit or probe on the card; `OFFLINE` = host build and
unit tests only.

## What is advertised

EXT_framebuffer_object, EXT_framebuffer_blit and EXT_packed_depth_stencil
were always exposed by core Mesa. ARB_framebuffer_object depends on the
Gallium cap `mixed_framebuffer_sizes`, which m9x now sets (Mesa m9x
c2a8740). With it, piglit's `-fbo` runs every core test in an FBO instead
of the window. Not advertised: multisample (no MSAA visuals or storage),
`mixed_colorbuffer_formats` (MRT attachments of different formats are
reported FRAMEBUFFER_UNSUPPORTED), ARB_framebuffer_no_attachments.
Colour-renderable formats stay BGRA8/BGRX8 and the 10-bit ones; GL_RGB565,
GL_RGBA4 and similar render into BGRA8, luminance/alpha/intensity textures
report FRAMEBUFFER_UNSUPPORTED. Depth/stencil attachments are D24S8
(DEPTH_COMPONENT16/24/32 and STENCIL_INDEX* fall back to it); separate depth
and stencil buffers are UNSUPPORTED (Gallium policy).

## Implementation

| commit (m9fbo / main) | change |
|---|---|
| c2a8740 | mixed attachment sizes: clip window = intersection of the attachments, depth/HZ descriptors from the depth attachment's own extent, MRT targets past the first with their own descriptors; depth proxy for levels of mipmapped depth textures |
| c641ffb | colourless fragment shaders (depth/stencil-only draws, u_blitter's empty FS) get a constant output with all colour writes masked; they were dropped ("no color output") |
| f43509b/e0393a3, 5d0913b/3b85f60, 083097d/f503d17 | kill-state drain, query fix, scratch-target drain (see parhelia-m9-kill-state-boundary.md) |
| ab5ee90/045629e | CPU blit: volume mipmaps (z-filtered slices), bilinear depth mipmaps |
| d62996d/b57ec1b | glClear depth stored as the nearest D24 code (was floor) |
| 9923059/0eeca30 | sampler LOD range clamped to the view's level range; base level only without mip filter |
| 649a644/f6c569d | draw buffers of NONE and outputs past the bound run: per-target split passes; discarding multiple-target shaders the same way |

The engine rasterises in top-down surface rows and addresses every colour,
depth and HZ surface through a descriptor of its own row span (depth spans
64-aligned), so attachments of different extents need no common layout; the
viewport and y-flip constants follow colour target 0 (MEASURED with the
mixed-size probe `tools/m9fbo-probes/fbomix.c`: 7 extent pairs, depth and
stencil, exact).

## piglit (MEASURED)

FBO set = spec@ext_framebuffer_object, arb_framebuffer_object,
ext_framebuffer_blit, ext_packed_depth_stencil and every test named *fbo*
(419 tests): 73 PASS before, 163 after c2a8740, 204 + the MRT/drawbuffers
set (12 PASS / 0 FAIL) after the later commits. Core list m9x-basic (452)
with `-fbo` effective: 268 PASS / 17 FAIL on c641ffb, 271 / 14 with the
later work (window-mode reference 272 / 10).

## Open (with cause)

* In-draw conditional-kill lane artifact (dlist-bitmap, scissor-bitmap,
  rastercolor, fbo-scissor-bitmap): late-boot drift, see the kill-state note.
* Cube maps above 16 texels are opt-in (M9X_CUBE_64, awaiting a pixel
  receipt): fbo-cubemap, fbo-incomplete-texture-02/03,
  fbo-generatemipmap-cubemap fail with GL_INVALID_VALUE / incomplete maps.
* gl_FragDepth is stored one code above GL's for ~1/3 of depths
  (depth-clear-precision-check); a code-rounding lowering gets 856/1256
  exact, not all, so it is not merged.
* fbo-alphatest-nocolor(+ff): a fully colour-masked job's kills do not stop
  its depth writes (hypothesis under test).
* fbo-blit-stretch: the engine blit reports the draws done but the
  destination keeps its clear (under test with tools/m9fbo-probes).
* fbo-fragcoord2 (gl_FragCoord with gl_FrontFacing), texwrap of depth
  formats, s3tc mipmap levels that cannot be stored, fbo-pbo-readpixels-small
  (uses DEPTH_COMPONENT32F without requiring ARB_depth_buffer_float).
