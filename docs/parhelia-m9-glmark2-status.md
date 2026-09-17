# glmark2 on the m9x driver: where the 60 fps goal actually stands

Full default suite, **MEASURED 2026-09-17** at 1920x1200 on the dual-DisplayPort
M9128 (subsystem 102b:22c0, 1 GiB VRAM) in the 16 GB host, one head driven,
stock X server with the image's glamor, Mesa built with LLVM 23.1 JIT
(`draw-use-llvm`), module `cd7d6fd4`, libgallium `6980afbe`.

Measured with `tools/m9x-glmark-sweep.sh` and `tools/m9x-glmark-parse.awk`,
which refuse to count a scene whose draws the driver skipped: such a scene
reports a very high frame rate precisely because nothing was drawn.

```
33 scenes: 25 at or above 60 fps, 8 not
```

**Every scene that renders is already above 60 fps.**  The slowest are
`function medium` 62, `shading cel` 70, `shading phong` 87, `bump high-poly`
89.  The eight remaining scenes are not slow - they do not render, because the
fragment backend refuses their shader and the draws are skipped.

| blocker (the backend's own message) | scenes |
|---|---|
| more texture fetches than one raster pass can bind | effect2d x2, desktop blur |
| texture coordinate is not a varying | terrain, shadow, refract |
| indirectly indexed uniform | bump normals |
| control flow | loop fragment-uniform=true |

So the goal is eight shader features, not a performance problem.  That is worth
stating plainly because the earlier reading was the opposite.

## Six of the eight are one feature

The engine fetches textures **before** the fragment program runs, one per unit,
at the coordinate it interpolates - there is no sample instruction.  Two
consequences meet in the same mechanism:

* more than four fetches cannot be bound in one pass;
* a coordinate **computed in the fragment stage** cannot feed a fetch in the
  same pass, at all.

`shadow.frag` computes `ShadowCoord / ShadowCoord.w` per fragment and fetches
with it.  `light-refract.frag` computes coordinates from `refract()` and binds
three samplers.  Both are therefore the second case, not a parser limitation,
and both need the pass split that `effect2d`, `desktop` blur and `terrain`
need.

**How a pass hands over is settled, and it is not registers.**  CAPTURED, and
reproducible from the saved submissions in seconds: in the two-pass family the
first pass binds `b0 = 10ec1000  b1 = ee190004`, the transient arena rather
than the colour target, and the second pass binds the colour target at `b0`
while binding *that same address back as a texture* through
`74 = 01000310  78 = 10ec1000  79 = ee190204`.  A pass **renders to an
intermediate surface in memory and the next pass samples it**; fragment
registers do not survive a raster draw boundary.  An earlier reading here -
that writing output pair `0x11`/`0x13` and never `0x2d` meant it was *not* a
render to an intermediate surface - was exactly backwards, because
`parhelia-m9-raster-stream-limit-re.md` §3 gives the rule that output pair
`(0x11+4j, 0x13+4j)` goes to the j-th bound target.  The full contract is in
`parhelia-m9-fragment-pass-splitting.md`.

So the two cases are one feature and want one implementation, not two.

The arithmetic is not the obstacle: all seven transcendental chains (exp2,
log2, pow, rcp, rsq, sqrt, normalize) are MEASURED exact against the C library,
0 of 76800 pixels differing, so the projective divide itself is available.

## The other two

Note `terrain` moved: it was refused for "program exceeds the c2 image", which
was the driver's own fixed 8 KiB allocation rather than a hardware limit.  With
the image sized from the compiled program it joins the computed-coordinate
group.  Note also that plain `[effect2d] <default>` is **not** blocked and
renders at 222 fps; only the two kernel variants are.

* `bump normals` multiplies a `uniform mat4` by a vec4 - four dot products
  against uniform rows at constant indices.  "Indirectly indexed uniform"
  suggests a constant-index uniform load being treated as indirect, or NIR
  reaching the backend unfolded.  Contained, not architectural.  UNTESTED.
* `loop fragment-uniform=true` loops to a **uniform** bound, which is real
  control flow - but the bound is known when the draw is submitted, so
  specialising the variant per uniform value and unrolling should reach it.
  UNTESTED.

## Two earlier readings this retires

* `jellyfish` and `ideas` are **117 and 108 fps** here.  On the 481 MB host they
  measured 1 fps, with jellyfish reaching 122 s per frame and triggering the
  OOM killer.  That was host memory, not a driver defect, and the unbounded
  allocation I suspected does not exist.
* `buffer` runs 126-146 fps against 11-51 on the old host; that scene is
  application and CPU bound, as `parhelia-m9-fill-rate.md` already recorded.
