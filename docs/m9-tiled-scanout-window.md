# M9: scanning out a window of a shared framebuffer

Copyright 2026 René Rebe <rene@exactco.de>

An extended desktop is several heads showing different windows of one shared
framebuffer.  A head's display controller offers exactly two knobs for that:
`M9_DC_START`, the address its fetch begins at, and `M9_DC_PITCH`, the span it
advances per line.  So a window at (x, y) is only possible if the window's first
pixel has an address of its own, and the rest of the window follows from that
address with the unchanged span.

For a linear surface that address is `y * pitch + x * 4`.  Scanout surfaces are
not linear: they carry the tiled `0xcc21....` descriptor.  This is what the
tiling does to a window, measured rather than assumed.

## What was measured, and how

`tools/m9-tile-window.c` writes a pattern in which every pixel carries its own
coordinates under a per-run salt into a tiled VRAM object, through the
host-aperture surface window, and then reads the object back two ways:

* through BAR0 below `0x0e000000`, which is the identity view of video memory,
  so the address every single pixel is really stored at is recorded - the
  storage order itself, with no model in between;
* through the surface window again, but with the window's base moved to the
  address of a candidate window origin, which is the same de-tiling the display
  controller performs when it is given a start address.

Measured on the M9128 at 01:00.0, kernel 7.2.5-t2, for surfaces of span 2048,
1280 and 1024, every pixel located, none missing and none stored twice.

## The storage order

Within a band the order is span-independent - spans 1024, 1280 and 2048 give
byte-identical addresses for the pixels below:

| pixel   | byte address |
| ------- | ------------ |
| (0,0)   | `0x0`        |
| (1..7,0)| `0x4`..`0x1c`|
| (8,0)   | `0x80`       |
| (0,1)   | `0x20`       |
| (0,4)   | `0x300`      |
| (32,0)  | `0x1e00`     |
| (64,0)  | `0x2400`     |
| (128,0) | `0x4c00`     |
| (256,0) | `0x8000`     |

The large-scale structure is a band of 32 rows:

* `addr(0, 64) = span * 256`, that is exactly two bands of `span * 32` pixels,
  for every span measured (`0x80000` at span 2048, `0x50000` at 1280,
  `0x40000` at 1024);
* `addr(0, 32) = span * 128 + 0x800`: a single band does *not* start where a
  linear layout would put it, so the two bands of a pair are stored in
  different internal orders;
* `addr(x, 0) = x * 128` for x a multiple of 256, that is a whole band's worth
  of that pixel column - a column step advances 32 rows of storage, not 4 bytes.

Inside a 256-pixel, 64-row cell the order is swizzled (see the table above:
`addr(32,0)` is `0x1e00`, not `0x1000`), and that swizzle is what decides which
window origins work.

## The rule

A window origin is usable when the whole stored order is the same picture
translated by the origin's own address, that is when

    addr(x0 + x, y0 + y) == addr(x0, y0) + addr(x, y)

for every pixel of the window.  Tested exhaustively over a 2048x1024 surface:

| origin    | translation |
| --------- | ----------- |
| (32,0)    | scrambled   |
| (64,0)    | scrambled   |
| (128,0)   | scrambled   |
| (256,0)   | exact       |
| (512,0)   | exact       |
| (1024,0)  | exact       |
| (0,1)     | scrambled   |
| (0,8)     | scrambled   |
| (0,16)    | scrambled   |
| (0,32)    | scrambled   |
| (0,64)    | exact       |
| (0,128)   | exact       |
| (1024,32) | scrambled   |
| (1024,64) | exact       |
| (1024,128)| exact       |

So, MEASURED:

* a window origin must be a multiple of **256 pixels** horizontally and of
  **64 rows** vertically;
* its address is **`y0 * pitch + x0 * 4 * 32`** - the horizontal term is *not*
  `x0 * 4`, because one pixel column costs a whole 32-row band;
* that address is a multiple of 128, so it always satisfies the
  `M9_DC_START_MASK` quantum.

Reading the surface through the aperture at exactly that address returns the
window and nothing else: 0 of 1048576 compared pixels wrong for (256,64) at span
2048 and 1024, and for (1024,128) at span 2048.  That read is the hardware's own
de-tiler working from a moved base, which is what the display controller does.

## What the driver does with it

`m9_kms.c` dispatches per head:

* a **tiled** framebuffer whose window origin satisfies the rule is fetched in
  place, `M9_DC_START = surface + y0 * pitch + x0 * 128`, pitch unchanged;
* any other window - a tiled one at an origin the tiling cannot address, or a
  linear one, which is what a dumb desktop buffer is - is published into that
  head's own surface by the copy path, which reads the window's rows out of the
  framebuffer (de-tiling them through the aperture when the source is tiled) and
  writes them into the head's surface at the head's own span.  The copy path
  places no alignment constraint on the origin at all.

`tools/m9-window.c` checks both paths on live hardware, by reading back the
bytes the controller fetches rather than by trusting the commit:

    dumb framebuffer 3840x1400 pitch 15360
      window +0+0     2304000 of 2304000 pixels present, 0 missing, 0 from outside
      window +1920+0  2304000 of 2304000 pixels present, 0 missing, 0 from outside
      window +1920+200 2304000 of 2304000 pixels present, 0 missing, 0 from outside
    tiled framebuffer 2048x896, window 1024x768
      window +1024+0   DC start = surface + 0x20000, de-tiled: 0 of 786432 wrong
      window +1024+64  DC start = surface + 0xa0000, de-tiled: 0 of 786432 wrong

## Limits

* The mode a head can drive is still at most 2048x2048; only the framebuffer
  extent grew, to four heads' worth in each direction.
* A **tiled** framebuffer is bounded by the vendor surface contract
  (`m9x_surface_layout_bytes`), which describes spans up to 2048 only, so a
  desktop wider than 2048 has to be a linear framebuffer and takes the copy
  path.  A GL-rendered shared desktop wider than that would need the tiled
  layout for larger spans to be established first.
* Common widths are not all a multiple of 256: 1024, 1280, 1536 and 2048 are,
  1366, 1600 and 1920 are not.  A head to the right of a 1920-wide head is
  therefore always a copy-path window, never an in-place fetch.

## Through the X stack, with one panel attached

`mode_config.max_width`/`max_height` now advertise the desktop extent, and X
sees it: `xrandr` reports `maximum 8192 x 8192` where it previously reported
2048x2048, and `xrandr --fb 3840x1200` resizes the screen on a single
1920x1200 head without the server or the driver complaining.

A single output is normalised back to `+0+0` by the `xrandr` client, so the way
to drive a non-zero window with one panel is a panning area wider than the
mode.  With `--panning 3840x1200` and the pointer warped to the far right edge,
`tools/m9-pan-check.c` reads the head's surface back out of video memory:

    pointer x=0:    0 white of 2304000 pixels
    pointer x=3839: 255 white of 2304000 pixels
    pointer x=0:    0 white of 2304000 pixels

The window follows the panning origin - content that exists only in the right
half of the framebuffer reaches the head - and returns when the pointer does.
The black root window made the difference small (the 255 pixels are the
software pointer glyph); the content proof is the KMS-level test above.

The glamor path is a separate matter: an X screen wider than 2048 fails at
`ScreenInit`, because the screen pixmap is then a GL surface wider than the
vendor surface contract describes.  An extended desktop wider than 2048 works
today on the shadow path (`AccelMethod "none"`), whose framebuffer is a linear
dumb buffer.
