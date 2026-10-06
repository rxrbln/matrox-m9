# M9 normal FAA raster-state lowering

The coherent surface-owned normal CPU frame is replayed with passive raster
shadow snapshots at actual retail `fc0e0`. Its dirty bit0 gates the first state
packet. The actual lowerer writes header `45d7eff0` at `fc1ed`, then takes F0
from shadow+28 (`fc1f7` load / `fc1fa` store), EF from shadow+20 (`fc200` load /
`fc203` store), and D7 from shadow+24 (`fc209` load / `fc20c` store).

`tools/check-m9-faa-raster-state.py` first applies the complete normal graph
coherence audit, then matches every such packet to its chronological stage's
actual dirty shadow. It checks exact producer PCs, four contiguous DWORDs in
one live DMA owner, and values. The underlying frame checker separately proves
all exported bytes against the last actual CPU writes, including count records.

| Actual minimal stage | F0 | EF | D7 |
| --- | --- | --- | --- |
| Queue initialization | 00000000 | 00000000 | 00000000 |
| FAA graph initialization | 2e00040c | 000000aa | 00000000 |
| Public context initialization | 2618040c | 000000aa | 00000000 |
| Geometry helper quad | f0000400 | 00288800 | 000054ae |
| Surface-owned FAA triangle | c4008600 | f8004055 | 20005ca3 |
| FAA resolve | e0000400 | 20007000 | 00000000 |

The triangle uses FVF44 XYZRHW+diffuse, Z off, cull none, lighting off, MSAA on;
320x240 format66 sampled primary, and the complete genuine graph. The above
values are exact lowerer outputs for this minimal variant. They do not prove a
universal FAA F0/EF mask or generic textured/depth-tested rendering variant.

The frame artifact is `docs/traces/m9-vendor-faa-normal-frame-20261006.json` and
strict positive receipt `docs/traces/m9-vendor-faa-raster-state-20261006.json`.
The CPU replay completes actual draw/resolve dispatch. It supplies no GPU
retirement or rendered result. Kernel-private pool ownership, complete shader
and source resources, and the actual saved-state lifecycle remain necessary
for safe executable translation.
