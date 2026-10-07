# M9128 generalized GL FAA upload boundary

The generalized Mesa `m9_faa_live_gl_build` added in `385b2c2` is an owned
CPU snapshot. It compiles a retained GL vertex NIR on a clone, keeps the
GL-to-D3D half-Z operation in the GPU carry program, resolves live CB0 keys,
and copies program, constants, declaration, and input records. Its
`native_admission` result is deliberately false. No generalized draw or
glxgears hardware receipt follows from a successful snapshot.

The original M9128 public-VS controls and the qualified first live draw
establish these field relationships for their two-complete-stream profile:

| Field | Live derivation | Evidence boundary |
| --- | --- | --- |
| Four C2 program entries | Cumulative finalized vector slots times eight, in fragment/carry/gather/position order; C2 selects `entry_bytes[i] / 8` | The original profile uses 4/7/5/25 slots and entries 0/4/11/16. The generalized linker computes the cumulative byte offsets. |
| C3 constant range | Retained constant owner base plus the vector's dense begin/end; pair count is `(end-begin)/8`, encoded as `0x00500000 | (pairs << 8)` | The first live FS used `[16,32)`, two pairs, whereas the captured source used `[0,32)`, four pairs. The kernel correctly changed all three fields for the live case. |
| Vertex record | Source-map writer uses the compiled live input layout and bound Gallium formats; `record_bytes` and vertex count determine the snapshot byte extent | The qualified public command uses exactly two FLOAT4 inputs, a 32-byte record and three vertices. The generalized snapshot may produce another stride/count, but its GPU command lengths are not yet qualified. |
| Input declaration | `m9_vs_decl_words` generates two words per live attribute, optional texture words, and the terminal word | The qualified command has exactly five declaration words. Its packet offsets and lengths cannot be reused for another word count. |

The current kernel public-VS ioctl takes the fixed 796-byte
layout/program/constants/geometry image, requires a 328-byte four-vector
program and five declaration words, and checks the admitted program's SHA.
Its command offsets, counts, fixed geometry `+0x100` placement, and later
relative branches belong to that bounded image. A generalized native
producer needs a sized owned upload, packet length/declaration rebuilding,
live owner offsets/capacity checks and source-proven raster/fragment routing;
changing only the C2/C3 fields is insufficient.

The one-color GL1 case remains a separate admission gap. The FAA compiler's
whole-program branch requires **two real complete vec4 output stores** and
the generalized builder requires two distinct explicit linkage streams.
The ordinary Mesa pass-through color layout requests only `COL0`. The
qualified public VS writes COLOR and TEXCOORD, but its FS is constant-color,
so that run proves neither stream is read; it does not prove a fabricated
second output is inert for a color-reading FS. Original `14eba0` tests show
how shorter vectors are finalized, not a whole one-stream FAA draw. No actual
glxgears lowered NIR or M9128 one-color public draw is archived here.

The smallest source experiment is a genuine public M9128 VS with POSITION and
COLOR outputs, paired with an FS that reads COLOR, compared against a control
that also writes an independently varying but FS-unused second vec4. Record
the original compiler output, declaration and final draw packet lengths and
pixel/retirement behavior separately. Only if that proves an unused second
stream harmless may Mesa add it from a live, explicit source and the native
producer encode it; no captured address, raw record, or command byte is a
portable default.

Sources: Mesa `m9_faa_live.c`, `m9_vs_compile.c`, `m9_faa_program.c`,
`m9_vs_decl_words`, and `tests/faa-live-public-upload-20261007/GENERALIZED.md`;
M9128 `parhelia-m9-faa-public-vs-preprocess-interface.md`,
`tests/m9-faa-graph-public-vs-20261007`, and the actual first live-color
receipt in `tests/m9-faa-public-vs-unlock-20261007`. Apply
`gpu-capture-replay-policy.md` to any future producer.
