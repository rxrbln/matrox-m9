# Public texture and depth FAA shader joins

Four genuine normal sampled-primary CPU frames now retain actual compiler and
program evidence. The caller supplies public DP2 state; the allocator, handle
publisher, state compilers and command producers run unchanged. No compiler
enum13, private compiled state, shader result or GPU reply is assigned.

| Caller case | Actual program bytes / slots | Preprocess and raster entry byte offsets | Final compiled04 | Merged F0 / EF / D7 |
| --- | --- | --- | --- | --- |
| Diffuse baseline | 240 / 30 | 32, 0 | `62af9` | `c4008600/f8004055/20005ca3` |
| Fixed-function texture | 328 / 41 | 48, 104, 144, 0 | `62af9` | same |
| Depth enabled | 248 / 31 | 32, 0 | `62ae1` | `c4008600/d8004055/20005ca3` |
| Public ps_2_0 texture | 312 / 39 | 32, 88, 128, 0 | `62af9` | same as baseline |

The texture resource is a genuine 64x64 profile12 color allocation. Actual
`7a770` publishes its handle into the context's existing resource table, and
public texture-stage packet25 binds that returned handle. The caller supplies
XYZRHW/diffuse/UV vertices with FVF144 and stride28. The programmable case
uses public CREATEPIXELSHADER54 and SETPIXELSHADER56, with standard ps_2_0
DCL t0, DCL 2D s0, TEXLD and MOV oC0 tokens. The declarations matter: the
initial declaration-free texld request returned CPU success but launched no
draw, so it is excluded from the proof.

The depth case copies the actual FAA companion's profile49 (class4, hardware
format3d, 64 bits) into a genuine sampled 320x240 allocation. Its returned
handle is bound by public SETRENDERTARGET41, with ZENABLE and ZWRITEENABLE1
and ZFUNC4. This finite depth choice does not establish arbitrary Gallium
depth formats or successful depth pixels.

Actual lowerer `181fa0` reads its nested compiler input through argument+20.
The depth case changes the genuine final compiled04 from62af9 to62ae1,
selecting AA mode6 instead of7. At `fae93..faf84`, the actual source fields
produce the final raster state:

* F0 = AA shadow+b4 (`8000`) | program shadow+e0 (`600`) |
  texture shadow+7c (`c4000000`).
* EF = AA shadow+b8 (`e0004055`, or depth `c0004055`) |
  program shadow+e4 (`18000000`) | texture shadow+80 (`0`).
* D7 = texture shadow+84 (`20005ca3`).

The retained program vectors are joined through actual kind13/14 operands,
the existing vendor CPU mapper, all matching CPU-published PTEs, byte offsets
and known CPU-written spans. The resident database's 115160 known bytes keep
SHA256 `af36245806515bd7b0e38a986586d33611a84ed76dae845a42ce8776ba89f174`
in all four cases. Every program has different bytes; the depth program
preserves the first32 FS bytes but changes VS bytes. Texture adds genuine
preprocessing heads and a gather before rasterization. A single captured
240-byte program or fixed two-launch plan is therefore insufficient for
these public shaders.

Run `tools/check-m9-faa-texture-depth.py` against
`docs/traces/m9-vendor-faa-texture-depth-20261006.json`. The compact trace
retains actual public packets, returned resource handles, compiler events,
source merge fields, program vectors/checksums and PTE joins. It omits the
large full-frame chronological CPU-write inventory, preserved in the raw
capture when generating the trace.

Texture contents and depth storage are not populated by a simulated GPU.
This proves CPU admission and program/state production, not rendering or
retirement. The vertices are caller XYZRHW; arbitrary application GPU vertex
transforms, native metadata pairing and texture/depth variants still require
joins before activating prepared Mesa FAA jobs.
