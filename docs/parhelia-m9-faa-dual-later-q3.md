# M9 FAA textured path toward Quake3 GL1 (m9faa4, 2026-10-08)

Copyright 2026 René Rebe <rene@exactco.de>

All hardware results are from the M9128 102b:0540 rev01 with the analytic pixel oracle
`tests/m9-faa-dual-later-native-20261008/check-later-pixels.py`. The hardware evidence is
in that directory, under `hardware-*-20261008`.

## Measured on hardware

| Feature | Evidence |
| --- | --- |
| No-clear later frames | `hardware-noclear-fix-20261008` |
| Later batches of 2 or 3 DRAWs | `hardware-batch-20261008` |
| Dual FIRST at 1024x768 | `hardware-1024-20261008` |
| Later frames at 1024x768 | `hardware-1024-later-20261008` |
| Depth-only later clear | `hardware-depth-pairs-20261008` |
| Count4 CLEAR3, any texture pair | `hardware-depth-pairs-20261008` |
| CW/CCW face culling | `hardware-cull-20261008` |
| 128x128 textures (kind 0x21) | `hardware-cull-20261008` |
| FAA gears at 1024 regression (guard field) | `hardware-gears1024-20261008` |
| Mipmapped textures (GL level encoding, six isotropic rates) | `hardware-mip-centre-20261008` |
| Culled 3-DRAW later batches | `hardware-mip-centre-20261008` |
| FAA pixel centre: integer; GL needs -0.5 px | `hardware-mip-centre-20261008` |

Notes on the rows:

- No-clear later frames: the source no-clear span began with the public Unlock's own
  F021->F022 copy, which native code had posted twice.
- Later batches: the FIRST append seam is reused with the later epoch.
- Dual FIRST at 1024x768: the guard band setup field comes from original 13b900.
- Depth-only later clear is what Quake3 issues every frame. It is the count4 CLEAR3
  prefix up to the depth constructor, followed by the no-clear DRAW/Lock form, with the
  Lock at Unlock+0x120.
- Face culling confirms the GL mapping BACK + CCW front -> CW.

## Source facts established (emulated original driver, no GPU completion)

- Culled FIRST changes four 6A selectors (0x28ff0000) and the cull field (CW 3, CCW 2).
  A culled later stage keeps two 12-byte 6A groups, 24 bytes in all
  (`tests/m9-faa-dual-cull-source-20261008`). There is no source for culled appends yet,
  so culled DRAWs go one per transaction.
- Later program switches (`tests/m9-faa-program-switch-source-20261008`). An earlier
  capture showed stage-1 COLOROP changes leaving the later image unchanged. That was an
  emulator artifact: the PDEV CRC table at 0x20cda0 was never built, so every
  program-cache key was 0. With `--pdev-crc-table`, a changed TSS, blend or alpha-test
  state builds and uploads a new program inside the later DRAW. That stage is the
  program's FIRST stage plus the usual sampler reload. A cached program is selected
  without a rebuild.
- Texture shapes (`tests/m9-faa-dual-texture-shape-source-20261008`): 128x128 uses kind
  0x21; request +0x2c is the level count. The D3D level/mip-filter words differ from the
  ordinary GL sampler encoding, so a level-coded hardware probe decides between them.
- A single-texture stage (`--single`) is a distinct 2720-byte stage. It has three target
  lanes, one sampler and a 48-byte first program entry, about 40 structural differences
  from the dual stage. It needs its own constructor.

## Quake3 GL1 inventory (ioq3 renderergl1, DERIVED)

- Lightmapped world: COLOR*T0*T1 with unit 0 MODULATE and the lightmap MODULATE. That is
  the dual MODULATE program.
- Each view starts with a depth-only clear; the colour is cleared only with r_fastsky.
- Depth uses LEQUAL by default and EQUAL for multipass. Culling is mostly on. Texture
  sizes vary, with mipmaps.
- Single-texture MODULATE, blended and alpha-tested stages need FAA fragment programs
  beyond the pinned FS72.

## Remaining, in order

1. Done (m9faa5): mipmaps, culled batches, GL pixel centre on the FAA raster.
2. Single-texture and blended FAA stages (vendor constructors needed).
3. Culled appends; 1024 CLEAR3 later on hardware.
4. First Quake3 GL1 FAA run at 1024 with fallback, then the FAA<->ordinary handoff.
