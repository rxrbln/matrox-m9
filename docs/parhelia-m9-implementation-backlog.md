# What the M9 driver still has to implement

This separates three things that the readiness ledger deliberately mixes:
what the **vendor hardware** does, what we have **reverse engineered**, and
what **this driver actually implements**.  Only the third column decides
whether a feature works today.

Checked against the driver source on 2026-09-18.

## Reverse engineered and verified, not implemented

These need no further capture.  The evidence is in the tree and in most cases
was confirmed against a live vendor GL context.

| feature | evidence | driver today |
|---|---|---|
| **Occlusion query** | exact fixed selector pair, modulo-32 result, begin/end retirement, live 16384/8192/0 visible/partial/hidden with asynchronous availability and reuse | implementation in progress (`m9_query.c`) |
| **Rectangle textures** | unnormalized sampling passes; level 1, repeat and mip filters raise the required errors | `PIPE_TEXTURE_RECT` is accepted by `m9_is_format_supported`; no fixture covers it, so UNTESTED |
| **Separate stencil** | exact face controls and live separate-op packet.  Note the vendor's own API rejects valid LESS/GREATER enums - implement correct core behaviour, do not copy the bug | partial |
| **Fog coordinates, blend equations** | modes and destination-sensitive results captured; static mapping covers operations 1..5 | partial |
| **sRGB write** | RT0 `SRGBWRITEENABLE` emits an exact 25-slot shader suffix; clear has the exact CPU encode curve | the backend implements it behind `m9_fs_blend.srgb`, but `m9_format.c` advertises no sRGB colour format, so the state tracker never binds one and the path is unreachable.  Advertising a format is what remains |
| **Second and further vertex varyings** | output pair `(0x11+4j, 0x13+4j)` feeds the target at `0xb0+2j`, CAPTURED 310/310 passes; target 3 at `b6` is bound by nobody; count declared in `ef` bits 19/21/23 | one varying (`M9_VS_MAX_VARYINGS 1`) |

## Reverse engineered, implementation in progress

| feature | state |
|---|---|
| Framebuffer multipass fragment | **implemented and on by default.**  The missing state was index `ef` bits 10:9, a pass's place in the sequence (0 only, 1 first, 2 middle, 3 last).  The record is four 32-bit floats a fragment, indexed by the engine's fragment counter.  Depth is evaluated in the **first** raster pass only.  Scene results in `parhelia-m9-glmark2-status.md`; what still stalls is a split draw returning to an earlier render-target shape |
| MRT, four render targets | surface side implemented and pinned offline: four `b0 + 2s` pairs with `COLOR` relocations, one render format class a target in `89`, one write mask a target across `ab` and `ac`.  What remains is a fragment backend that compiles more than one colour output, and the native destination that names target p, which is DERIVED as `0x2d + p` and not yet measured.  `caps->max_render_targets` stays 1 until it is.  `docs/parhelia-m9-mrt-routing.md` |

## Established as unreachable on this hardware

Recorded so they are not attempted again.

| feature | why |
|---|---|
| **R8, RG8** | MEASURED: the M9 narrow family is luminance/alpha only (`0x30` L8, `0x31` A4L4, `0x33` A8, `0x50` L16, `0x51` A8L8); the firmware's 52-record format table holds no R8 or R8G8, and there is no sampler swizzle field - the format code *is* the swizzle.  Added experimentally with storage expanded into the RGBA8 family, an imported R8 plane samples nothing: "texture is not stored in a captured sampler layout".  `GL_RED` additionally needs R8 as a render target, for which no code exists.  Advertising it would make EGL accept an NV12 or R8 import and return black |
| **Sampling any linear imported buffer** | MEASURED as part of the above: only the driver's own tiled texture storage and tiled BGRA render surfaces sample.  This is what puts NV12 plane sampling out of reach regardless of format codes |

## Known defects, cause understood

| defect | consequence |
|---|---|
| Projective fetch whose projector varies across a primitive divides per **vertex** | MEASURED wrong in 75373 of 76800 pixels.  Currently masked by an accidental refusal, so loosening that check without fixing the divide turns a refusal into a silently wrong picture |
| One constant span per job | a per-object transform ends the job; cost the native vertex path 10% on `ideas` and 5% on `pulsar` |
| `emit_rcp` with a literal operand returns 4x too large | worked around by folding literals at compile time; root cause open |
| ~~D16 depth attachments draw with no depth~~ | **resolved**: Z16 is now claimed for sampling only, so the state tracker falls back to `x8z24_unorm` and a depth attachment always carries the hierarchical surface |
| ~~A split draw that returns to an earlier render-target shape stops the engine~~ | **resolved**: the engine keeps per-target state in the object at index `be`, and a revisited shape read what another shape left there.  One `be` object per render-target shape.  Sharing by shape is what the engine wants - one per colour *resource* still stalls |
| `desktop` and `terrain` stop the engine at scene scale | a second, distinct fault in the split path that no fixture reaches: every `splitpair` and `deskblur` case passes after the `be` fix, while `glmark2 -b desktop --size 1920x1200` faults on the first frame.  Untested properties are a split draw into a target far larger than 320x240, and the number of simultaneously live shapes.  Keeps `M9X_SPLIT_PRESSURE` off, so `terrain.frag` stays refused |
| glmark2 exits with rc=139 moving from the 15-tap `effect2d` scene to `shadow` in one process | client-side, no stream timeout; `shadow` alone is 142 fps.  Sweep one scene per process until understood |
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

1. **Third vertex attribute** - four lines in `m9_draw.c`, already measured.
2. **Occlusion query** - self-contained, exact.
3. **Cube and 3D textures** - captured, and they widen GL 2.1 conformance.
4. **The projective divide defect** - before anything relaxes that refusal.
