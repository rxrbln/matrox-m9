# M9 FAA: vendor forms Quake3 GL1 needs (m9emu-q3, 2026-10-08)

Copyright 2026 René Rebe <rene@exactco.de>

All vendor facts below are DERIVED from the original M9128 `MxMD.dll` executed under
Unicorn (m9emu). No GPU was used and no pixels or completions are claimed. The FAA
graph INIT receipt is the archived hardware one. The dual FIRST/Unlock receipt is
m9faa4's `proof-3a663ef9`. Evidence and regeneration scripts are in
`tests/m9-faa-q3-vendor-forms-20261008/`:

- `mip-tail/`: 28 stage-0 chain shapes with `table.txt` and `../tools/chainmodel.py`.
- `later/`: 66 later-frame captures, with decoded diffs in `later/diff/*.txt`,
  per-DRAW segments in `later/segments.txt`, and the FS comparison against Mesa in
  `later/fs-programs-vs-mesa.txt`.
- `first/`: FIRST batches of 3/4/6/8 DRAWs plus mixed cases, with diffs in
  `first/diff/`.

Captured addresses in these files are source identities only (capture policy). A
native producer derives every address, size and count from its live owners.

The kernel/Mesa status was audited on the main trees: DRM `ae1b62c`, Mesa `ad75910`.
K = `drivers/gpu/drm/m9x/` and M = `src/gallium/drivers/m9x/`.

## Baseline structure

A plain dual-MODULATE later DRAW is four secondary segments:

| segment | bytes |
| --- | --- |
| setup | 788 (920 when appended) |
| pass A | 528 |
| pass B | 440 |
| colour pass | 980 |

Each further DRAW adds 60 primary bytes and one stage, and moves the Lock by +0xa0.

## Feature table (Quake3 frequency order)

| # | Feature | Vendor delta vs dual later DRAW | Our status | Fix location |
| --- | --- | --- | --- | --- |
| 1 | Mip chain reaching the 32x16 tail atlas (all Q3 mipmapped textures) | One allocation per chain (request flag 0x10, levels at +0x20, constructor 0x52660). The levels above the tail follow the ordinary recurrence. The first level with w<=16 && h<=16 starts a 0x800-byte (BGRA8) 32x16 atlas at the running offset, and all smaller levels share it. Chain end = tail offset + 0x800 (28/28 shapes). Descriptor: `79` as single level, `7a=(levels-1)<<28 \| 1<<16 \| h`, `7c` mip bits (Q3 LINEAR_MIPMAP_NEAREST `0x9292`, mip LINEAR `0x9312`), `7d=0x0ff80000` | Refused. K `m9_faa_graph_texture_bind.c:71-73` (`levels>1 && last level <=16x16` -EINVAL), and `chain_extent()` :29-53 has no tail. M `m9_context.c:9799-9807` refuses the same chains. The 7a/7c encoding is already right | K `chain_extent()`: at the first level with w<=16 && h<=16 return max(end, offset+0x800); drop the refusal. M `m9_context_faa_texture_snapshot()`: drop the tail refusal |
| 2 | GL_CLAMP / CLAMP_TO_EDGE | `7c \|= 1<<22` (U) and `1<<25` (V); nothing else changes, per stage and per DRAW | Encoded (K `texture_bind.c:131-138`; M maps CLAMP_TO_EDGE, `m9_context.c:9842`). ioq3 uses GL_CLAMP_TO_EDGE when the extension exists. GL_CLAMP with LINEAR lowers to CLAMP_TO_BORDER and is refused (M :9814) | none for Q3 with the extension |
| 3 | Per-DRAW texture change (many textures per frame) | That DRAW's `78/79/7a/7c` change. The vendor also re-uploads its 200-byte program-constant block (byte-identical for every shape) and points 00/8d/8e at the copy | Later: per DRAW, a fresh constants slot per DRAW (K `m9_faa_native.c:10892-10938`, `texture_dual_link.c:51-73`). FIRST: Mesa splits on a texture change (M `m9_faa_graph_dual.c:246-251`); the K FIRST append requires equal texture words (`dual_first_append.c:125-131`) | Optional: FIRST append with per-DRAW texture words (vendor `first/qa`: only sampler words differ) |
| 4 | Single texture / REPLACE / no vertex colour | Programs: single MODULATE FS48; REPLACE FS32. FVF without DIFFUSE (0x102) is a different 1552-byte stage with a 280-byte VS | Mesa: white stride-0 COLOR0 into FS48/FS72 (`m9_faa_application.c:125-148`, :626-651). This is functionally equal (x*1.0), not the vendor form. The kernel has only COLOR decl 7/9 programs | none required. Native XYZ\|TEX programs need new VS/decl constructors |
| 5 | Blend families | The fragment program carries the blend tail. Mesa `m9_faa_texture_fragment_build` is byte-equal (program, C3 pairs, f0, 88) for 32 vendor forms: dual/single x {ONE/ONE, SRC_ALPHA/INV_SRC_ALPHA, DST_COLOR/ZERO, ZERO/SRC_COLOR, ONE/INV_SRC_ALPHA, SRC_ALPHA/ONE, DST_COLOR/SRC_COLOR}, DST_COLOR/ONE, INV_DST_COLOR/{ZERO,ONE}, and alpha-modulate variants. The vendor builds blend ONE/ZERO as plain FS72 | Missing: (ZERO, INV_SRC_COLOR) and (INV_SRC_ALPHA, SRC_ALPHA). Captured vendor programs: `later/blend-zero-invsrccolor`, `later/blend-invsrcalpha-srcalpha` (96 B). Families are gated by `M9X_FAA_FAMILIES` (default off, M `m9_faa_application.c:691`) | M `factor_tail` (`m9_faa_fragment_state.c:357`) and K `m9_faa_graph_texture_fragment.c:215-250`: add both tails from the captures. Add INV_SRC_COLOR to the uAPI (`m9x_drm_fragment.h:28-34`) and `texture_factor()` (`m9_faa_live_effects.c:179`). Enable families by default after a card run |
| 6 | Alpha test GT0 / LT128 / GE128 | In the program, byte-equal in Mesa for dual and single, with and without blend (GE128 + alpha blend: dual 120 B, single 104 B) | Encoded, behind the same families gate | none (gate) |
| 7 | Depth func / write per DRAW | Only A8 changes: LEQUAL+W `0x38038f`, EQUAL+W `0x380387`, LEQUAL `0x38038d`, EQUAL `0x380385`; no insertion. In-batch changes need no extra packet | A8 encoding is exact (K `m9_faa_depth_state.h:13-20`). In-batch changes are refused (K `m9_faa_graph_dual_batch_input.c:126-129` -EOPNOTSUPP; M `m9_faa_graph_dual.c:161-162`, :298-300 splits) | K batch input: allow per-DRAW depth flags (write A8 per DRAW stage). M `dual_same_state`: stop splitting on depth |
| 8 | Depth test disabled (2-D HUD) | Heavy: the DRAW adds a 4224-byte primary transition (f0=e0000400, depth/HZ surface rebinding, bc/bd, 1x1 targets), an in-frame GPU wait on a token the frame already published (synthetic here), A8 `0x38038e`, EF bit 29 and a rebuilt program (`later/depth-off`) | Mesa substitutes enable+ALWAYS+no-write, A8 `0x38039d` (M `m9_faa_graph_dual.c:42-43`). This is functionally equal for colour and avoids the transition | keep the substitute. The vendor form needs a hardware check of its wait |
| 9 | Cull NONE/CW/CCW changes inside a frame | Colour-pass EF bits 25:24 (CW 3, CCW 2). Every 6A selector is 0x28ff0000 while culled and 0x29340000 otherwise. The extra 12-byte 6A group in passes A and B (+24 B) appears **only in a DRAW whose raster state differs from the previous DRAW** in the context (cull or depth bias), carrying the new selector, including culled->none (0x29340000). It does not depend on the program: built and cached switches carry the FIRST-form 6A groups anyway | K encodes it. In the v3/program-cache path it keys `cull_state = e->cull != raster` on that program's last use (`m9_faa_graph_later_stage.c:294`). The v1 path keys on the culled bit (`dual_later_encode.c:164`). M splits on any cull change (`m9_faa_graph_dual.c:301-303`) | K: key the insertion on the previous DRAW's raster state (cull + bias) in the transaction/queue, not per program. M: drop the cull split once the kernel does (vendor `later/n3-cull-mixed`, `n4-cull-cw-none`) |
| 10 | Polygon offset (decals) | d0/d1 packet `4545d1d0 units/(2^24-1) slope` in the colour pass, EF bit 26, +24 B 6A pair on change. d0/d1 are re-emitted in every later DRAW once set, with EF bit 26 gating them | Missing: no FAA d0/d1 and no EF bit 26 in K. M refuses `offset_tri` (`m9_context.c:9880`) | K colour-pass encoder: emit d0/d1 and EF bit 26 from a per-DRAW bias in the uAPI (reuse `m9_rast_depth_bias` arithmetic). M: pass units/factor |
| 11 | More than 3 DRAWs per FIRST / later batch | FIRST 3/4/6/8 and later 8 DRAWs use the same append: +2868 B secondary, +60 B primary, +0xa0 Lock per DRAW. There is no source limit at 3 | FIRST max 3 (K `m9_faa_graph_dual_batch_input.h:7`, `dual_first_append.c:203-205`; M `m9_faa_graph_dual.c:224`). Later max 64 | K/M FIRST limit: generalise the append to N (vendor `first/n4,n6,n8`) |
| 12 | Program switches in later frames | Frame 1 builds (FIRST-form stage + sampler reload). Frames 2-3 select cached programs (`differs, select`; cached form = plain + 19/18) but still upload the per-program constant block (200/208/224 B) per DRAW (`later/q3seq`) | K has PLAIN/CACHED/BUILD forms (`m9_faa_graph_later_stage.c:268-316`). M splits when a same-kind program differs (`m9_faa_graph_dual.c:312-325`) | M: keep same-kind switches in one later batch (the kernel already has the forms) |

## Notes

- **D3D enums.** The vendor sampler uses D3D9 values: MIN/MAG POINT 1, LINEAR 2;
  MIP NONE 0, POINT 1, LINEAR 2 (`7c` 0x0, 0x80, 0x100). The kernel uAPI uses
  mip 1=none, 2=point, 3=linear. The bits agree, except that the kernel's
  none/single-level emits 0x80, as the vendor does for its default POINT state.
  With min POINT, mag LINEAR and mip POINT the vendor D3D emits `0x9290`, without
  the GL-captured bit 10 that the kernel sets for that combination; Q3 does not use
  it.
- **Request +0x2c.** It is extra height rows (`0x51bac` adds it to `+0x1c`), not a
  level count. This corrects `tests/m9-faa-dual-texture-shape-source-20261008`.
- **Mixed FIRST batches.** cull/depth (`first/qb`), program switches (`first/qc`) and
  mip chains (`first/qa`) capture cleanly. An alpha-test switch inside a FIRST batch
  stops the harness's pinned relocation-payload check (kinds 23/24 C3 pointer
  rewritten after relocation). That is a harness limit, not a vendor fact. Alpha
  tests in later frames are captured.
- **New harness options.**
  - `m9_faa_graph_first_depth_dual_xyz_control.py`: `--tex-chain`, `--tex-height`,
    `--stage1-chain`, `--draw-rs`.
  - `m9_faa_graph_dual_later.py`: `--later-tex W:H:LEVELS`, `--draw-bind`,
    `--draw-fvf`, `--draw2-bind`, `--draw2-fvf`, `--frames` up to 4, `--first-arg`,
    and `--synthetic-in-frame-wait` (CPU structure only, recorded in
    `synthetic_in_frame_waits`).
  - `m9_faa_stream_diff.py` prefers the counted framing.
- **Unknown or not captured.**
  - Hardware behaviour of every row (no card use).
  - GL_CLAMP with LINEAR (CLAMP_TO_BORDER) under FAA.
  - Texgen. ioq3 GL1 computes environment/sky coordinates on the CPU, so no VS
    texgen variant is needed.
  - NPOT. ioq3 GL1 resamples every image to a power of two (`R_CreateImage` /
    `Upload32`), so FAA needs only POT chains.
