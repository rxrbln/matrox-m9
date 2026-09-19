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
| ~~**Occlusion query**~~ | - | **implemented and advertised** (`m9_query.c`, `caps->occlusion_query`).  Three things the RE note did not have were needed beyond the validator changes: the counter is off until `a7 = 0x00ffff7f` / `c6 = 0x00ffff09` name it; the counter is **free running and shared by the context**, so a result is a difference and a query may span jobs; and the closing record must be ordered behind the draws or it publishes a partial count (5919, 5578, 5297 on three runs of the same 7000-sample draw).  MEASURED: `occvis` 7000, `occpart` 4130, `occhid` 0 beside a visible companion at 7000, `occspan` 10000 across three jobs.  **UNTESTED**: a range passing more than 2^32 samples (the counter is 32 bits while core Mesa reports 64), multisample, and a query around a split shader, which counts the first raster pass only |
| **Rectangle textures** | **implemented and MEASURED.**  The premise this row carried was wrong in its mechanism: the engine has no unnormalized addressing mode at all.  CAPTURED, the vendor's 13x7 rectangle emits the descriptor words of a 13x7 2-D texture byte for byte and differs only in the three wrap fields of `7c`, while its draw alone carries `1/13` and `1/7` as a program constant, so the reciprocal has to be folded upstream of the sampler.  This driver folds it into `unit_scale`, the per-vertex term that already scales a render target of unaligned width, so no shader constant is needed.  Six listed fixtures, exact at the content edge; `texrectborder` is dispatched but unlisted, because clamp-to-border returns the border with red and blue exchanged and correcting it contradicts a captured vendor word.  See `parhelia-m9-rectangle-textures.md` | closed except clamp-to-border |
| **Separate stencil** | exact face controls and live separate-op packet.  Note the vendor's own API rejects valid LESS/GREATER enums - implement correct core behaviour, do not copy the bug | partial |
| **Fog coordinates, blend equations** | modes and destination-sensitive results captured; static mapping covers operations 1..5 | partial |
| **sRGB write** | RT0 `SRGBWRITEENABLE` emits an exact 25-slot shader suffix; clear has the exact CPU encode curve | the backend implements it behind `m9_fs_blend.srgb`, but `m9_format.c` advertises no sRGB colour format, so the state tracker never binds one and the path is unreachable.  Advertising a format is what remains |
| **Second and further vertex varyings** | output pair `(0x11+4j, 0x13+4j)` feeds the target at `0xb0+2j`, CAPTURED 310/310 passes; count declared in `ef` bits 19/21/23; stride and per-target placement are independent descriptor fields (bits 7:0 and 15:8, in dwords), so several preprocessing draws share one packed record and a route-fed final draw de-interleaves it - see `parhelia-m9-varying-transport-re.md` | one varying (`M9_VS_MAX_VARYINGS 1`); the two-draw chain failed on hardware once, retested after the `ef` chain-role fix |

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
| **Sampling a linear 2-D surface** | MEASURED directly, not inferred: a probe held the image in the texture layout and switched **only** the storage and the kind byte.  Tiled written through the raw map (`01`) and tiled written through the kernel window (`21`) both sample exactly; linear sampled as `00`, `08`, `10`, `18`, `20` all fail at 76800/76800, as do linear bytes carrying a tiled kind.  The load-bearing control is the tiled 16x16 case, which writes through the *same* raw mapping the linear cases use, so the write route is engine-faithful and the layout is the only variable left.  This matches the captures: every sampled linear surface there is 1-D or 1x1, while the linear 2-D surfaces the engine *writes* - hierarchical Z, vertex-output targets - are never sampled.  It is what puts NV12 plane sampling and foreign dma-buf sampling out of reach, independently of format codes |

## Landed since the ledger was written

| feature | state |
|---|---|
| **Engine-side copy and blit** | `resource_copy_region` and `blit` run on the raster back end through `util_blitter`, guarded to destinations bindable as colour target 0 and sampleable sources, with the CPU transfer as fallback.  Two safety properties: a draw the backend cannot express is *dropped* rather than failed, so the skip count is checked and the CPU path redoes the copy; and the engine path stands down inside a live occlusion query, where its draws would be counted.  This is a **correctness** fix as well as a mechanism one - the CPU path ignored the filter and sampled nearest, so `blitlin` and `blitlin:32x128` fail on the pre-change binary at 76776 and 76720 of 76800 pixels |
| **A filtered blit was resampled NEAREST** | `m9_blit_convert` ignored the filter, so every reduction through the CPU fallback was a 1-of-4 decimation rather than a reduction - every CPU-built mipmap level, and any scaled window copy or `glBlitFramebuffer` that falls back.  Silent: aliased output at a good frame rate, which is why it survived.  Found while implementing `generate_mipmap` |
| **TGSI helper shaders reached the native backend** | `util_blitter` builds its shaders with `ureg`, which delivers TGSI, and `m9_create_fs_state` only reached the native backend for NIR - so every blitter draw was dropped with a **null reason**: the path looked available and silently did nothing.  Any Gallium helper shader hit this, not only the blitter |

## Hardware limits established 2026-09-18

| limit | evidence |
|---|---|
| **A multi-pass fragment job whose geometry leaves part of its render target uncovered stops the engine** | MEASURED.  Two passes suffice, the record is not involved (`M9X_SPLIT_STAGE=1` leaves it unbound and it stalls identically), and a large target is not needed (it stalls at 320x240).  A single-pass shader over the same partial rectangle renders 64 draws over, and the same geometry as two triangles rather than a strip stalls identically - so it is the multi-pass schedule over part of a target, not coverage or topology alone.  The boundary is not sharp: 1, 2, 8, 10, 12, 16 and 20 partial draws render, 24 and 32 stall, so no count-based rule can ship.  It is a **back-end** hang: the engine's fetch pointer consumed the whole submission (dword 171 of 171) and the writeback fence never reached its target.  `glmark2`'s `RenderWindowBlur` blurs a window back out over the screen at the window's position, which is why `desktop` hits it inherently.  The driver refuses such a draw by name; the fix is to render into a scratch surface the geometry covers and blit it into place |
| ~~A surface of 64x64 or less whose height is above 16 and not a multiple of 8 samples the wrong texel~~ | **Superseded, and it was never a hardware limit.**  The rule is **odd height**, and the fault is a driver bug in one unit: the CPU window writes the **last row of an odd-height surface into the first row of the second page**.  Exactly two rows are wrong in every failing case; at width 64 row 4 holds row `height-1`, at width 32 row 8 does, and 4 and 8 are `256/row_span` - the rows in one 1 KiB page.  MEASURED producer-versus-consumer: `rtt:64x23` PASSES and `tex:64x23` FAILS on the identical shape with identical descriptor words, so the raster engine and the sampler are both correct.  Not small-family-specific (65x23, 100x61, 16x127 fail); width never enters it.  `13x7` and `16x15` are exact because the 32x16 packed-tail minimum lays them out with 16 rows, and `17x9` is exact because its misplaced last row lands on itself - both predictions of the model, not exceptions.  Fixed by `m9_surface_even_height()` in `cd61a2c` |

## Rules established 2026-09-18

| rule | consequence |
|---|---|
| **A surface's declared width only normalises the texture coordinate; addressing follows the surface geometry** | So a producer and a consumer may state different widths without harm, and **widths need no rounding**.  MEASURED: `rtt:40x32` and `texchain:40x32` both pass with the producer describing the surface by its 32-aligned span (64) and the sampler binding the raw width (40).  This **retracts** an earlier entry here that called those two disagreements defects: they were found by reading the code, and the hardware normalises the difference away.  No patch was landed for them, which was the right call - a plausible patch for a defect that does not exist is worse than none |
| **The border colour registers `7e`/`7f` carry the four channels in the descriptor format's own order** | `c8`/`c9` are R, G, B, A; `cc` is B, G, R, A.  This dissolves an apparent conflict between a captured vendor word (red in `7e[15:8]`, a `c8` surface, and the capture's own rendered frame confirms it) and a driver measurement showing red and blue exchanged (a `cc` surface).  Both are correct.  A previous "surface channel order" hypothesis was **withdrawn on a void experiment rather than disproved**: st offers this driver exactly one colour-renderable order, so the control fixture could not vary what it appeared to |
| **`M9_REG_COMMAND_BASE` (0x400) reads back as the engine's fetch pointer** | It is written once per slot with that slot's page-aligned base, so subtracting the base turns a timeout report into a byte offset into the submitted command image.  This is what "0x400 and 0x404 mismatched after a stall" has always meant.  The driver now names the stalling dword |

## Known defects, cause understood

| defect | consequence |
|---|---|
| Projective fetch whose projector varies across a primitive divides per **vertex** | MEASURED wrong in 75373 of 76800 pixels.  Currently masked by an accidental refusal, so loosening that check without fixing the divide turns a refusal into a silently wrong picture |
| One constant span per job | a per-object transform ends the job; cost the native vertex path 10% on `ideas` and 5% on `pulsar` |
| **One job per draw is the driver's submission model, and it is now the dominant cost** | MEASURED on `refract` after its transfer was removed: 25.13 ms a frame, of which **submit 11.66 (ioctl 10.78)** and **draw 7.87** are 78%, while rendering proper is no longer the constraint.  `m9_prof_jobs` reports **46 jobs a frame, 40 of them the scene's own draws** at roughly 0.23 ms of ioctl each.  A hypothesis that batching the generated mip chain would recover the time is **refuted**: the chain is 6 of those 46 jobs, worth about 1.2 ms of the 8.4 ms that 60 fps needs.  The lever is driver-wide - several draws to a job - and wants its own measurement.  The remaining 7.87 ms is CPU T&L in the draw module, which is the native vertex backend's |
| A filtered **engine** blit whose reduction is not an exact halving loses pixels | MEASURED with plain level-0 textures, so it is not the mip plumbing: `blitlin:75x37` differs in 2452 pixels and `blitcpu:75x37` in 75138, while the exact halvings `74x37`, `76x38` and `150x75` are all exact - and `blitlin:100x40`, ratio 2.5, is **also exact**, so the rule is not simply "non-exact ratio".  Not pinned down; the two failing mip cases are unlisted with the evidence rather than left green.  Every level `refract` samples is an exact halving, which is why its chain is exact |
| `refract` spends 95% of its frame in a CPU transfer | MEASURED, `M9X_PROFILE=4` at 1920x1200: `1065.62 ms/frame: draw 8.05, submit 8.08, ioctl 7.25, transfer 1014.03`.  The scene calls `glGenerateMipmap` on its distance target once a frame; the driver implements no `generate_mipmap` and refused a target whose level is not zero, so the state tracker built every level on the CPU through the de-tiling path.  **Fixed** (`386e0ce`): 1 fps to **39 fps**, transfer 1014.68 to 0.52 ms, attributed on one binary with `M9X_LEVEL_TARGET=0` reproducing the old path.  A base level above zero turned out to be a **LOD clamp** rather than a second allocation, so pinning the view's low bound samples that level alone.  The scene is still **not conformant**: one draw a frame is refused by the partial-coverage gate.  Cost is proportional to that texture's area and nothing else (a 2.13x smaller target gives a 2.14x smaller transfer).  The refusal turned out to be **missing plumbing, not a storage constraint** - a level already is a render surface; what was absent was a byte offset on the colour relocation and a level in the framebuffer state.  The packed tail levels of 16x16 and below genuinely are not surfaces |
| `m9_fs_raster_88()` treats index `88` bits 31:24 as eight per-interpolant flags | They are not.  MEASURED: setting bit 30 for a seventh input made **input 4** read garbage as well, and setting all eight corrupted every input, while forcing the six-input mask `0x3f10dff0` on a seven-input pass left the first six exact.  The `BITFIELD_BIT(24 + k)` loop is correct only because varyings are capped at six, which is now explicit.  Raising that cap without decoding these bits would corrupt inputs the shader never touched |
| `emit_rcp` with a literal operand returns 4x too large | worked around by folding literals at compile time; root cause open |
| ~~D16 depth attachments draw with no depth~~ | **resolved**: Z16 is now claimed for sampling only, so the state tracker falls back to `x8z24_unorm` and a depth attachment always carries the hierarchical surface |
| ~~A split draw that returns to an earlier render-target shape stops the engine~~ | **resolved**: the engine keeps per-target state in the object at index `be`, and a revisited shape read what another shape left there.  One `be` object per render-target shape.  Sharing by shape is what the engine wants - one per colour *resource* still stalls |
| `desktop` and `terrain` stop the engine at scene scale | a second, distinct fault in the split path.  `desktop` is twenty-six split draws alternating 420x420 with 1920x1200 at four raster passes, and renders all of them before stopping (CAPTURED).  Its pass count and its target size each render on their own, at twenty draws (MEASURED: `splitpair:320x240:320x240:20:11` and `splitpair:1920x1200:1920x1200:20:11`), so what is left is the combination, the record being replaced under submitted jobs, and four colour resources on one shape.  `splitdesk` and the tap argument reach all of them.  Keeps `M9X_SPLIT_PRESSURE` off, so `terrain.frag` stays refused |
| ~~No picture when the machine boots without the BIOS VBE POSTing the card~~ | **resolved** (`7c7e80c`).  Two corrections came with it.  The display-controller enable bit was never the mechanism - `m9_program_timing()` already sets it, and the controller was enabled while nothing looked at it.  And this is not an unusual boot: CAPTURED from `captures/m9x-vendor-fourhead-20260917/`, POST always leaves the card on the VGA CRTC (`1c10 = 1`, VGA MISC `0x63`) and the vendor driver leaves `1c10 = 2`, MISC `0x68`.  `vga=0x318` merely hid it, because the BIOS mode set performs the same handoff.  Two registers decide it: `0x1c10` bit 0 selects the VGA raster, and the legacy VGA miscellaneous output register's bits 3:2 are the CRTC clock select, which POST leaves on the fixed **25.175 MHz VGA oscillator** - so with correct geometry the head rastered at **20.00 Hz** and writing `0x0e48` did not move it by a hertz.  MISC `0x68` gives the programmed clock.  MEASURED cold: 1280x720 at 60.00 Hz, line rate 45.0 kHz.  The monitor reporting "out of range" was the decisive datum - the DisplayPort MSA carries geometry only and read correct throughout, so no log would have shown it |
| `m9x_ddc_init()` compacts the PInS DDC map, so a head can read EDID from one socket and light another | this board reports `DDC map 09 0a 00 00 0d 0e 00 00` - populated pairs at positions 0 and **2**.  Compaction packs them into slots 0 and 1 and `head->ddc_index` becomes the head index, while `head->link` still comes from the bank table.  Whenever the map has holes, "DP-2" reads the third selector pair and drives pad 1.  A strong candidate for why outputs 2-4 have always behaved differently from output 1 |
| A module swap leaves the device returning `-EBUSY` for everything, and only a reboot clears it | distinct from a stream timeout: it follows an ordinary reload with no stall, and made one control run read as a failure of the change under test |
| `m9_reloc_uses_vram()` lists `PARHELIA_M9_RELOC_TEXTURE`, and the selftest at `m9_submit.c:1183` asserts it | contradicted by capture: the vendor binds `78 = 40c3a001`, a class-1 system GPUVA with the `|1` tag, 84 times.  The correct rule is the one stream slots `00..09` already use - **the address class is in the tag, not in the slot** - so index `78` should accept a VRAM base untagged or a class-1 GPUVA resolved as `bo->system.start + target_offset \| 1`.  Left unchanged deliberately: relaxing it widens what a submission may reference, and after the linear-2-D result the only use the evidence supports is a linear 1-D texture this driver cannot yet produce.  The **assertion** is the part that will outlive the evidence |
| glmark2 exits with rc=139 moving from the 15-tap `effect2d` scene to `shadow` in one process | client-side, no stream timeout; `shadow` alone is 142 fps.  Sweep one scene per process until understood |
| CPU-uploaded depth textures never reach the sampler | tiled surface, linear transfer |
| `glReadPixels(GL_DEPTH_COMPONENT)` returns the first map's value forever | pre-existing, not understood |
| First modeset above the firmware's mode logs `scanout publish failed (-19)` | the frame after is correct; the ordering is not |

## Display side

| feature | state |
|---|---|
| Extended desktop across heads | **implemented**: `m9_plane_window()` scans out a per-head window of a shared framebuffer, and `mode_config.max_width/height` are raised.  It is still built on `drm_simple_display_pipe` - that turned out not to be the blocker the note assumed; our own three checks were.  **Open:** whether a horizontal start offset addresses a *tiled* surface correctly.  Only 256-pixel and 64-row origins are pure translations, because tiled surfaces are stored in bands of 32 rows and the horizontal term is `x * 4 * 32` |
| Framebuffer console | **implemented**: stock `drm_client_setup_with_fourcc` + `DRM_FBDEV_TTM_DRIVER_OPS`, gated on `m9_kms_scanout`.  Default mode is the raster the card was POSTed with, or the EDID preferred mode on a head the firmware left dark; `m9_kms_mode="DP-1:1280x720"` pins it, below `video=` in precedence.  The buffer is a linear dumb buffer, so every publish re-tiles through the aperture: 395 ms a whole frame at 720p, 15.7 ms a console line.  `m9_fb_dirty()` honours its clip rectangles, without which the blinking cursor alone held a CPU at 100% system time |
| Dumb-buffer page flip cost | MEASURED ~440k pixels/s, a whole-frame de-tiling copy, so 0.19 fps at 1920x1200.  Direct scanout needs a tiled BO, which only the GL path allocates |
| DP++ dual-mode detection | early exit implemented; reading the adapter identifier over DDC would skip training outright |

## Needs capture before it can be implemented at all

Listed so they are not mistaken for ready work: multisample and FAA allocation,
pattern, mask and resolve; anisotropic filtering; PBO; and sRGB **sampling** -
the write path is exact but the sampler decode was never joined.

**S3TC moved out of this list.**  The storage rules are decoded and are
executable assertions in `tools/m9-texture-resource-evidence.py`
(`verify_static_compressed_storage`).  DXT1 is hardware code `0x10` at 4 bits
per texel in storage class 7, DXT2/DXT3 are code `0x15` and DXT4/DXT5 code
`0x17`, both 8 bits per texel in storage class 6; DXT2 and DXT3 are the same
hardware format, and so are DXT4 and DXT5.  Descriptor dimensions are texels
rounded up to whole 4x4 blocks, allocation is
`padded_w * padded_h * depth * bits_per_texel / 8`, the block is the
addressing unit of the class-3 DXT calculators (which take *texel*
coordinates), and blocks upload verbatim - the hardware never decompresses.
`docs/parhelia-m9-s3tc-compressed-textures.md` is the implementation note.
What is still capture-gated is narrower: whether `79[15:0]` holds the logical
or the padded width for a compressed NPOT texture, and BC1 punch-through
behaviour, since DXT1 RGB and RGBA share one code.

**10-bit GigaColor render and sample moved out of this list.**  `A2B10G10R10`
is hardware code `0x68` and `A2R10G10B10` code `0x6a`, both 32 bits per texel
in storage class 2 - allocated and tiled exactly like `B8G8R8A8`, so no new
layout code is needed.  Both take render class 0 at index `89` bits `3*s`, the
same class an 8-bit target takes; the per-target 16-bit write mask in `ab`/`ac`
must be `0xffff`, because that mask is a byte-lane mask and the vendor routes
10-bit channel masking through the fragment output program instead.
`docs/parhelia-m9-gigacolor-10bit.md` is the implementation note, and the
exposure gate is now three hardware results - clear-and-read-back, sample, and
blend rounding - rather than a missing decode.

**10-bit scanout is in scope.**  The M9 display
controller does know the format: `MxMM.sys 0x37b31` lists `0x6a` and `0x68` as
scanout surface codes, `0x368fe` packs the code into bits `[31:25]` of the
display surface descriptor, and `0x3dfd0` builds a ten-bit-per-channel gamma
ramp for code `0x6a`.  But both producers of the mode-record format mask in
`MxMD.dll` (`0x30352` and `0x105f0`, tables `0x30424` and `0x10704`) map a
Windows bit depth to `{1,8,0x10,0x20,0x40}` and reject everything else, so the
10-bit mode arm at `0x302d6` is unreachable, and the `GigaColorRender` /
`GigaColorFullScreen` registry switches that selected it on Parhelia were
deleted from this driver.  What is dead is therefore the **vendor driver's
mode enumeration**, not the silicon: this driver writes that descriptor itself
and enumerates its own modes, so the path is reachable here.  The remaining
unknowns are whether the DisplayPort transmitter needs its bit depth stated in
the MSA and whether the attached panel accepts 30-bit - both answered by
looking at the panel, not by more disassembly.  UNTESTED, under implementation.
An earlier version of this paragraph said "do not add a depth-30 visual", which
mistook the vendor's dead enumeration for a hardware limit.

**Float textures moved out of this list (partly).**  The vendor binds format
code `0x34` (`A32B32G32R32F`) at texture index `78/79` in
`captures/m9x-vendor-glamor-shaders-20260915/gradient-{linear,radial}-18`,
so a 128-bit float texel is a real sampler format, not a table entry.  What is
captured is one linear 1-D lookup with NEAREST filtering from system memory;
tiled placement and filtering of float texels are still uncaptured.  See
`parhelia-m9-texture-upload-and-copy-re.md`.

## Suggested order

Items 1-3 of the previous list - the third vertex attribute, the occlusion
query, and cube and 3D textures - are all done.

1. **The scene-scale split fault.**  The last thing between `desktop` and
   `terrain` and honest frame rates, and the ladder that reaches it is staged
   and runs in one boot.
2. **MRT.**  The surface side is done and pinned offline; what remains is a
   fragment backend that compiles more than one colour output, and measuring
   the destination that names target p rather than trusting the DERIVED
   `0x2d + p`.  Bind and shader must land together - a bound target must be a
   written target, so neither half measures anything alone.
3. **The projective divide defect** - before anything relaxes that refusal,
   because loosening the check without fixing the divide turns a refusal into
   a silently wrong picture.
4. **sRGB write** - the backend is written; advertising a colour format is all
   that stands between it and being reachable.
5. **A second vertex varying**, which needs the second preprocessing draw.
   Three hypotheses are now refuted on hardware: index `0x88`'s pair mask, a
   barrier between the two draws, and `ef` bits 11:9 as the draw's place in
   the chain.  The minimal case fails - `vary3`'s extra draw binds a *single*
   stream target and still stalls the engine.  The one lead left is that every
   captured chain-opening draw writes a **packed intermediate record** that a
   later route-fed draw de-interleaves, where ours writes separate
   `0x68002000` stream views.
   **The one-word declaration is settled and gave no ceiling raise.**  MEASURED:
   the hardware accepts `0688` for a general interpolant exactly, taking six
   inputs from 15 declaration words to 9 - but the ceiling is **six either
   way**, because the interpolation registers `0x40 + 8*i` are handed out in
   ascending attribute order and there are exactly six; `0x70` reads garbage.
   The projected rise to thirteen was wrong: the declaration budget was never
   what bound.  The win is that six interpolants now fit beside all four
   texture units, where the pair form left no words.  A seventh input fails
   **quietly** - every pixel written, no error, wrong values.
6. **The depth pair**: CPU-uploaded depth textures, and
   `glReadPixels(GL_DEPTH_COMPONENT)`.
