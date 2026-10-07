# M9128 retained COLOR controls after public Unlock

These are original `102b:0540` vendor CPU controls, not GPU completion or
pixel proofs. The retained public-VS context was resumed only after the
matching real same-boot Unlock `0x1b0` receipt. Its independent queue-finish
CPU stack was discarded under the established source-capture policy. Every
new Lock query remains unread (`0x280`, or `0x290` after CLEAR). No captured
address is a native parameter: the owner numbers below describe source roles;
native commands must use current retained owners and recompute all offsets,
lengths, links and timeline fields.

The repeat baseline reuses its 328-byte program. Its new primary range is
`[8672,12304)`, secondary `[2688,5236)`, and owner 28 geometry `[512,608)`.
It posts END at 8732, 12292 twice and 12304. Controlled changes are:

| genuine second-call input | exact source consequence |
|---|---|
| SETVSCONST48 CB0 before DRAW | Reupload owner 28 constants `[512,664)` (152 bytes), move geometry to `[768,864)`; nine secondary owner-28 address fields change at 3020, 3064, 3068, 3692, 3696, 4184, 4188, 4912, 4916. Program and END shape stay fixed. This is a new constant allocation and *all* stage range bindings must follow it. |
| two triangle-list primitives, six 32-byte vertices | Geometry grows 96→192 bytes; secondary padded counts at 3436/3932/4372 change 4→8 and raster count at 5232 changes 3→6. No new primary packet. |
| depth enable/write, LESS (`RS7=1, RS14=1, RS23=4`) | Secondary 4748 changes `0038038c→0038038e`. This qualifies only the state encoding, not hardware depth pixels. |
| back-face cull (`RS22=2`) | Adds a 24-byte secondary state packet and rebases subsequent relative links and three primary pointers. It requires stream rebuilding. |
| flat shading (`RS9=1`) | Secondary 4484 changes `00000203→00004203`. The source packet does not establish GL provoking-vertex semantics or pixel correctness. |
| full black CLEAR42 before DRAW | Primary grows 128 bytes; ENDs become 8800, 8860, 12420 twice, 12432, and source timeline advances by `0x10`. No display latch or clear pixel claim follows. |

An original one-COLOR public shader (`ps_2_0 dcl v0; mov oC0,v0`) compiles
FS64 + fused VS232: a three-slot shader head followed by 25 position/clip
slots and STOP. A controlled ADD-temporary/MUL-position version still uses
two vectors, FS64 + VS264, with a seven-slot head and the same 26-slot tail.
Its interface is clip POSITION in raw registers r00/r01 and COLOR in
r02/r13; the tail reads r00/r01 and routes r02 into output pair 0, while
the head has written r13. Mesa's ordinary fused `compile_head` uses this
register interface. The original compact COPY constant image has two
utility pairs, two CB0 pairs, then 13 viewport/clip pairs (17 total). The
source VS C3 span excludes the two utility pairs and carries 15 pairs;
the arithmetic chain carries 17. The source scheduler and some tail operand
roles differ from Mesa's ordinary passthrough tail: in particular original
fused slots reference CB0 where ordinary slots name the ordinary tail's
`(-1,0)`/zero pairs. Both the W1 E selector and, for some opcodes, W0 CF
can name constant pairs. A uniform selector rebase is invalid. One possible
bounded native experiment is Mesa's already GPU-tested ordinary fused
head/tail and its own sparse constants through pair 21, inserted into the
source two-vector command grammar with a live 22-pair C3 span. It is not
vendor byte equality and needs pixel proof.

The four-vector vendor control has **two** real VS outputs (COLOR and an
unused-by-FS TEXCOORD): FS64 + carry56 + gather40 + position200. It is not
an exact packet authority for a one-real-output 416-byte four-vector
program. In source raster packets, the fused one-COLOR first input pair
references retained owner 23 at relative offsets `0` and `2158592`, whereas
the two-output four-vector packet first pair uses `873472` and `2620416`.
The native one-stream gather/consumer must bind the same live produced route;
it must not inherit the two-output first pair unchanged.

The raster AC word is not a geometry extent. Source descriptor count is one;
descriptor `04d10000` maps input index 0 to output slot 0. Source
`182500` reads `ffff` from the input's `+1a0` table and writes the active
mask slot. `182535..182545` copies four stack halfwords to shadow `+20/+24`;
`fbee8` copies shadow `+24` to the emitted AC word. The resulting inactive
slot residue differs between one-COLOR three-vertex `28260306`, two-output
three-vertex `28268eb6`, and two-output six-vertex `28268f16`, while the
active AB slot remains `ffff`. Source data do not prove that replacing
inactive slots is harmless on hardware.

The source callback at `24060` selects a retired pool slot and emits a
16-byte control packet while advancing the queue epoch by `0x10`. It does
not justify rewinding primary or secondary cursors. Native repeated draw
remains bounded by retained owner capacity until pool ownership and
retirement are implemented. A direct CREATEVS handle-2 experiment returned
`D3DERR_INVALIDCALL` even before SETVS; it does not establish a general
program-change rejection because the caller's handle registry was absent.

Source captures are compressed under
`captures/m9-faa-graph-public-vs-20261007/next-state-controls/`, with the
M9128 mask/fused program control in
`captures/m9-faa-gl-one-color-20261007/mask-and-fused-controls.json`.
Run `python3 tests/m9-faa-public-vs-next-controls-20261007/check.py` and
`python3 tests/m9-faa-public-vs-next-controls-20261007/check-mask-fused.py`.
The reproducible controllers are
`tools/m9emu/m9_faa_public_vs_next_controls.py`,
`tools/m9emu/m9_faa_graph_color_mask_initial.py` and
`tools/m9emu/m9_faa_graph_color_one_stream_chain.py` under the established
vendor emulator environment.
