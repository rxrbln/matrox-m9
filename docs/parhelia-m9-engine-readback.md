# M9 engine readback, 2-D blit context and engine upload

<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->

Agent m9glperf, 2026-10-09. Target nano (M9128, VIA Nano X2), kernel 7.2.5-t2.

## Why

Every CPU read of a VRAM image (tiled colour targets, depth, texture levels:
`transfer_map` READ, `glReadPixels`, `glGetTexImage`, the CPU blit fallbacks)
goes through the kernel `SURFACE_ACCESS` window: `memcpy_fromio` over the
write-combined BAR0 window, one uncached word per bus round trip. MEASURED on
nano: **9.2 MB/s** over a piglit readback subset (42.8 MB in 4.66 s).
Streaming loads (`drm_memcpy_from_wc`, MOVNTDQA) did not change it
measurably; that attempt (57a1582) was discarded.

## Vendor evidence (MxMD.dll under Unicorn, DERIVED from emulated vendor code)

* `DdLock` on a render target reads through the BAR aperture, as our window
  path does (no engine copy).
* The video->system `DdBlt` (GetRenderTargetData, D3D8 CopyRects, DirectDraw
  Blt to system memory) is an **engine copy** (`afbe0` direction 1 ->
  `b1290`): source = the tiled VRAM view at texture unit 0
  (`78 = GPU|1`, `79 = 0x78<<25 | kind 21 | span`, `7a = 1<<16 | height`),
  destination = colour target 3 (`b6 = bus|2` into one of two 2 MiB
  contiguous coherent rings, `b7 = 0x78<<25 | pitch`), `f0 = 280c1c0c`,
  32-row chunks `b6/f2/fc` each followed by `e6=1/e3=token|1`, then a CPU
  `memcpy` from the cached ring. No PTE publication for the rings.
* The system->video upload is CPU by default (`DdFlags = 0x8007`); with
  `DdFlags` bit 6 it is a **textured rectangle**: the data is CPU-copied into
  a linear system shadow (GPU-VA via compact PTEs, `78 = VA|1`, kind 00,
  format 0x78), the destination is colour target 0 (`b0 = VRAM|1`,
  `b1` kind 21 or 01), `f0 = 080c040c`, `fc = (-h)<<16 | (-w)`, `f2`/`f3`
  source/destination origins, one launch per level, no chunking.
* Both run in the vendor's 2-D blit context (below), emitted once per context
  and as a delta when warm.

Scripts and captures: `/home/ai/tmp/agents/m9glperf-emu` (`bin/rt-bltsys.py`,
`bin/rt-upload.py`, `blt2-decoded.txt`, `up/*-decoded.txt`).

## The 2-D blit context, classified (DRM `m9x/m9_engine_2d_state.c`)

The captured 812-word block is emitted packet by packet from named encoding
constants and one table of program words; its length is computed from the
packets (`m9x_engine_2d_state_words()`), and the encoder test still matches
the capture word for word.

| packets | roles | class |
|---|---|---|
| `f0=0 ef=180000aa d7=0`, `ee=ffff0000` | copy control off, raster control (ROP copy 0xaa), 2-D selects | encoding constants |
| `ec f2 f3 f4 = 0`, `22 = 0` | 2-D origins/extent cleared; the copy sets them | derived per copy |
| `a8..aa = 0`, `ab ac = ffffffff`, `ae = 22444442` | depth/stencil off, all channels written, blend pass-through | encoding constants |
| `f5=0 f6=7fff7fff c4=0 c5=40004000` | unbounded clip window, guard band | encoding constants (clip = full range) |
| `88=00103303 89=fe207ff8 8a=0 c1=0`, `c2=4 c3=0` | register-pair layout and store entry of the 2-D program | tied to the program |
| `e6=04000001 e3=token` (x2) | bank switches | token derived from the live timeline |
| 170 x `8f=0x80000000+0x40*block, 90+2k/91+2k` | the 2-D fragment program, 340 words in 21.25 blocks | captured code (`m9_engine_2d_program[]`) |
| `8c..8f = 0`, `8f=c0000680 90..93 = 0, 1.0f, 4.2667f, 0` | program constants | captured code constants |
| `e6, 8f=80000600, 90..92 = 0, 0, ffffffff`, `f0=c0000000` | constant bank the copy reads | encoding constants |
| `74..77 = 0`, units 70..73 with `78..7a 7c 7d 87 80 81 7e 7f = 0`, `f0 = 0` | every texture unit unbound | encoding constants |

No address, size or extent is captured in the block. The program overwrites
on-chip program memory 0..0x547 and bank 0x680; ordinary Mesa jobs bind their
programs through `c2` relocations and never use that store, and a module load
re-runs sequence0. The readback restores what Mesa does not restate
(`f2 b6 b7`, the 0x600 bank) to the sequence0 values.

## MEASURED on nano (2026-10-09, DRM 479438e, plan E, no stall)

| check | result |
|---|---|
| `m9-mesa-glsl` uniform, mode 2 (copy without the 2-D state) | 76800 of 76800 pixels wrong, no stall |
| uniform/varying/mul/texture/rtt, mode 1 | 0 of 76800 differ, all five |
| piglit readback subset (30 tests), mode 1, `surface_engine_min_bytes=0` | 27 PASS, 2 SKIP, 0 FAIL |
| bytes moved in that run | engine 156.5 MB in 0.75 s (~209 MB/s, thousands of small reads); window 42.8 MB in 4.66 s (9.2 MB/s) |
| glmark2 terrain on the pre-0603cd3 DSO (per-frame readback) | transfer 1760 ms/frame (mode 0) -> 97.6 ms/frame (mode 1) |
| ordinary rendering after mode 1 | correct (later fixtures, piglit, terrain) |

Mode 2 is kept only as a diagnostic. The default stays 0 until a full piglit
core run with `surface_engine_read=1`.

## Next

* Default mode 1 after the core run.
* Engine zero fill of the back-end object (the CPU memset through BAR costs
  refract 110, terrain 204, desktop blur 6-9 ms a frame) as a rectangle in the
  same 2-D context, posted as a queued kernel stream.
* Engine texture upload on the same plumbing (`streaming-texture-leak`: 5000
  1024x1024 uploads at ~150 MB/s through the window).
