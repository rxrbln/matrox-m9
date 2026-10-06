<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->

# M9 kill state: retained evidence and next proof, 2026-10-06

The conditional-kill lane artifact remains open. This audit uses retained
M9 captures and source, without a new target write or a new hardware result.
Run `python3 tools/m9-kill-state-evidence.py` from this repository to verify
the facts below, including the original corpus manifests and input hashes.

## Vendor instruction and readback are available

The genuine M-Series ICD corpus contains the complete C2 image, executed
submission, shader inputs and RGBA readback of `fs-discard-cond`. Its shader
discards when `gl_TexCoord[0].x < 0.5` and writes constant `v1` otherwise.
Every pixel in columns 0..63 retains the clear and every pixel in columns
64..127 is written: **8192 cleared, 8192 kept, zero coverage errors**. The
written constant differs by at most one byte because the vendor enables
dithering. `fs-discard-all` leaves all 16384 pixels at the clear value.
The comparison `fs-varying` and `fs-color` draws clear no pixels.

The current native failure is already stronger than an instruction-encoding
hypothesis: replay of the unchanged vendor program shows the same artifact
as compiled GLSL kill and fixed-function alpha kill. In up to three lanes
per 2x2 quad the decision of a quad two positions earlier is applied;
`x odd, y even` is unaffected. Non-kill stepped colors and predicate-selected
output are exact. The [fragment-backend note](parhelia-m9-mesa-fragment-backend.md)
and current M9_BRINGUP.md preserve the previous controls that did not move it.

## The `c7` pair is shared by kill and non-kill jobs

The decoder reproduces these exact writes in submission 12 of all four
vendor cases above:

| phase | command offset | index | value |
|---|---:|---:|---:|
| preprocessing range head | `0x00c` | `c7` | `00003fc0` |
| preprocessing range tail | `0x784` | `c7` | `00000000` |
| raster range | — | `c7` | no write |

The conditional-kill raster range then writes `f0=c4008400`, `ef=0`,
`d7=200068c3`, `88=0310fbdc`, `89=fe007ff8`. The `c7` pair is identical
in a kill-all shader and two shaders without kill, so the pair is not an
observed kill-specific enable. It does establish an ordering contract:
the vendor resets `c7` before entering rasterization.

The failed native experiment wrote `c7=00003fc0` in the live raster range
and stopped the engine. It differed in both phase and value from the
vendor's raster state. Do not repeat it, relax the validator around it, or
infer that the vendor's job-head write can safely be moved into a draw.
A future implementation must preserve the complete captured boundary and
retirement dependencies, after its role has been joined to initialization.

## The saved full MMIO snapshots are the wrong phase

Two genuine 8192-byte M9 BAR2 snapshots are retained in the cold-init corpus:
`pre-mode0-151327/mmio-live.bin` and `pre-mode2-151327/mmio-live.bin`.
Both are **before engine arm**. They read `1220=0`, `1224=0`, `131c=0` and
`1334=0`. This does not describe the engine while the correct kill draw runs.

The saved X-accessor traces `xinit-mmio.jsonl` and
`xinit-mmio-part2.jsonl` contain **zero CPU writes in 1000..13ff**.
Those observers instrument the vendor X driver's direct accessor calls;
they do not observe the GPU applying command-stream register writes or all
kernel-side initialization. The requester-live CSV likewise records only
selected requester, command and IRQ state, excluding the needed pixel-pipe
register bank. None closes the missing live vendor-state comparison.

## Bounded next proof

The next capture should retain one correct `fs-discard-cond` render and a
non-kill control under the genuine vendor stack. Keep the context alive,
finish the draw, save readback and complete program/command/resource images,
then take a read-only full BAR2 snapshot before destroying the context. Save
one after engine initialization and one after the non-kill draw as well.
`tools/m9x-mmio-snapshot.c` already maps BAR2 with PROT_READ and writes no
register or IRQ acknowledgement. Record board/subsystem, module/ICD hashes,
boot and capture phase alongside every snapshot.

The root target owner can acquire matching clean-room snapshots with the
same geometry and layouts. A diff names candidate state differences; it
does not by itself prove a cause, because context/resources and command
ordering also differ. First eliminate command-owned format/address fields
and unrelated display/requester values, then trace the responsible vendor
writer and its preconditions. Apply one vendor-proved initialization or
job-boundary correction and verify color plus depth and query effects.

The MSerX emulator can provide complementary ground truth when genuine
miniport initialization and an API-owned shader context are reached: retain
actual MMIO order and full emitter output instead of populating guessed
pixel-pipe defaults. Its software environment does not model hardware quad
kill execution, so an emulated trace cannot prove the artifact is gone.
Do not divert the active FAA capture to an invented kill-register sweep.

No driver or kernel runtime changed in this audit. FAA remains the first
target priority; this capture plan preserves the kill task for a later
bounded comparison without replaying the known hang.
