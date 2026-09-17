# What the M9 driver still has to implement

This separates three things that the readiness ledger deliberately mixes:
what the **vendor hardware** does, what we have **reverse engineered**, and
what **this driver actually implements**.  Only the third column decides
whether a feature works today.

Checked against the driver source on 2026-09-17.

## Reverse engineered and verified, not implemented

These need no further capture.  The evidence is in the tree and in most cases
was confirmed against a live vendor GL context.

| feature | evidence | driver today |
|---|---|---|
| **MRT, four render targets** | exact descriptors, relocations, masks and blend; one to four attachments verified on live GL 2026-09-17 with 64-256 cell checks and zero mismatches, surviving framebuffer destroy and rebuild | `caps->max_render_targets = 1` |
| **Occlusion query** | exact fixed selector pair, modulo-32 result, begin/end retirement, live 16384/8192/0 visible/partial/hidden with asynchronous availability and reuse | not implemented |
| **Cube textures** | all six face and orientation tests pass, exact descriptor and per-mip per-face base formula | not advertised |
| **3D textures** | 7x5x4 slices, R repeat, exact volume descriptor and full-depth mip recurrence | not advertised |
| **Rectangle textures** | unnormalized sampling passes; level 1, repeat and mip filters raise the required errors | not advertised |
| **Separate stencil** | exact face controls and live separate-op packet.  Note the vendor's own API rejects valid LESS/GREATER enums - implement correct core behaviour, do not copy the bug | partial |
| **Fog coordinates, blend equations** | modes and destination-sensitive results captured; static mapping covers operations 1..5 | partial |
| **sRGB write** | RT0 `SRGBWRITEENABLE` emits an exact 25-slot shader suffix; clear has the exact CPU encode curve | not implemented |
| **Third vertex attribute** | MEASURED 2026-09-17: `88[27:24]` is the unary count of declared source attributes; `m9_draw.c` hardcodes two.  100% correct readback once declared | `M9_VS_HW_ATTRIBUTES 2` |
| **Second and further vertex varyings** | output pair `(0x11+4j, 0x13+4j)` feeds the target at `0xb0+2j`, CAPTURED 310/310 passes; target 3 at `b6` is bound by nobody; count declared in `ef` bits 19/21/23 | one varying |

## Reverse engineered, implementation in progress

| feature | state |
|---|---|
| Framebuffer multipass fragment | raster emitter, descriptor and route model committed and verified; compiler-side splitter in progress.  Unblocks `effect2d` x2, `desktop`, `terrain`, `shadow`, `refract` |

## Known defects, cause understood

| defect | consequence |
|---|---|
| Projective fetch whose projector varies across a primitive divides per **vertex** | MEASURED wrong in 75373 of 76800 pixels.  Currently masked by an accidental refusal, so loosening that check without fixing the divide turns a refusal into a silently wrong picture |
| One constant span per job | a per-object transform ends the job; cost the native vertex path 10% on `ideas` and 5% on `pulsar` |
| `emit_rcp` with a literal operand returns 4x too large | worked around by folding literals at compile time; root cause open |
| D16 depth attachments draw with no depth | only D24S8 gets a depth descriptor |
| CPU-uploaded depth textures never reach the sampler | tiled surface, linear transfer |
| `glReadPixels(GL_DEPTH_COMPONENT)` returns the first map's value forever | pre-existing, not understood |
| First modeset above the firmware's mode logs `scanout publish failed (-19)` | the frame after is correct; the ordering is not |

## Display side

| feature | state |
|---|---|
| Extended desktop across heads | register analysis done - pitch is 14 bits **in pixels** so 7680 fits with room, scanout start is 128-byte aligned giving 32-pixel horizontal granularity, and `mode_config.max_width` 2048 is a driver constant not a hardware limit.  Needs `drm_simple_display_pipe` replaced by a real CRTC and plane.  **Open:** whether a horizontal start offset addresses a *tiled* surface correctly |
| Dumb-buffer page flip cost | MEASURED ~440k pixels/s, a whole-frame de-tiling copy, so 0.19 fps at 1920x1200.  Direct scanout needs a tiled BO, which only the GL path allocates |
| DP++ dual-mode detection | early exit implemented; reading the adapter identifier over DDC would skip training outright |

## Needs capture before it can be implemented at all

Listed so they are not mistaken for ready work: multisample and FAA allocation,
pattern, mask and resolve; native 10-bit GigaColor render and sample; S3TC;
float textures; anisotropic filtering; PBO; and sRGB **sampling** - the write
path is exact but the sampler decode was never joined.

## Suggested order

1. **MRT** - fully verified on live hardware, the descriptor work is decoded,
   and the driver currently advertises one target while the hardware does four.
2. **Third vertex attribute** - four lines in `m9_draw.c`, already measured.
3. **Occlusion query** - self-contained, exact.
4. **Cube and 3D textures** - captured, and they widen GL 2.1 conformance.
5. **The projective divide defect** - before anything relaxes that refusal.
