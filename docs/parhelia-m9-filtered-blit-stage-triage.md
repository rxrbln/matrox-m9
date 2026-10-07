# M9128 odd-height filtered-blit stage triage

The archived GL fixture does not yet isolate a filtered-sampler defect.
`blitlin:75x37` differs in 2,452 final pixels and `blitlin:100x37` in
3,288, but `blitnear:100x37` also differs in 3,792. The nearest control has
no bilinear interpolation. Both `blitlin:100x40` and `blitcpu:100x40`
pass, while `blitcpu:75x37` and `blitcpu:100x37` each differ in 75,138
pixels. Those observations distinguish an odd-height or transfer boundary
from an interpolation-only explanation; they do not identify its producer.
The counts come from `tools/m9-mesa-glsl.c` and
`docs/parhelia-m9-implementation-backlog.md`; no raw intermediate images
were archived with them.

The original M9128 `MxMD.dll` resource-copy path at
`0xb21b0 -> 0x56a10/0x56c60 -> 0x1b8d0 -> 0x1b650` takes logical
rectangles and resource descriptors (see
`parhelia-m9-texture-upload-and-copy-re.md` and
`parhelia-m9-texture-coherency-re.md`). Its source-qualified structure does
not establish a scaled, filtered sampler rule for Mesa's `util_blitter`
draw. The archived size ratios alone cannot justify changing native
coordinates, filter state, or a dimension cap. In particular, a rounded
fractional-weight model can explain some non-halving differences, but not
the failing nearest control.

An opt-in fixture capture now retains three distinct observations: source
FBO readback before the blit, destination FBO readback after the blit, and
the existing final 320x240 sampled output. On a future locked M9128 run,
use one process per case and a unique prefix:

```sh
M9_GLSL_BLIT_STAGES=/tmp/blit-100x37-near \
M9_GLSL_RAW=/tmp/blit-100x37-near.final.rgba \
  m9-mesa-glsl blitnear:100x37
python3 tools/m9-blit-stage-analyze.py \
  --prefix /tmp/blit-100x37-near --src-size 100 --dst-size 37 \
  --filter nearest --final /tmp/blit-100x37-near.final.rgba
```

The stage files are `PREFIX.src.NxN.rgba` and
`PREFIX.dst.NxN.rgba`, raw GL RGBA8 with the bottom row first. Run
`blitlin:75x37`, `blitlin:74x37`, `blitlin:100x40`, and matching
`blitcpu` cases with their own prefixes. The analyzer reports per-row
source, destination, and final mismatches. Its host-only self-test checks
four exact size/filter constructions, a single damaged row and a truncated
input.

FBO `glReadPixels` itself uses a CPU transfer. Thus a bad destination
stage file alone may be a readback fault even when the GPU-resident image
is correct. A clean destination file with bad final sampling implicates
the later sampled draw or its descriptor. A bad source file prevents
attributing destination differences to the blit. Compare the final sampled
image and both engine and CPU routes before changing either producer.
The capture is read-only and adds no GPU command replay or captured address.
