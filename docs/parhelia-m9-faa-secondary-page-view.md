# M9128 ordinary and sampled allocations in the secondary page view

The genuine Windows allocator with `MmFlags=101b8` publishes both its compact
four-byte page view and its optional sixteen-byte page view for ordinary and
sampled primary surfaces. The latter has physical base `501000`, size
`400000`, CPU alias `60501000`, and initialization publication through
`1320/1324/1328`. This per-allocation observation complements the independent
initializer proof; it does not establish a hardware consumer for either view.

`59651` loads the manager's selector from `+40`; `59654` tests the selected
extent at `+2c+8*selector`. When it is nonzero, `59664` calls `5b750` after the
compact update at `5964c`. The gate does not inspect render-target layout kind.
The actual allocation descriptor is eleven words: eight zeroes, the first GPU
page, page count eighty, and zero. Ordinary `{e81001,cc210140,100f0}` and sampled
`{ed1001,cc220140,100f0}` each own `50000` bytes and use this same descriptor
shape. The real selected child supplies `+1c=60501000`, `+20=501000`, and
`+14=400000`.

For each page, actual guest stores at `5bad1/5bae6/5baed` produce words
`{page_address|8,0,0}` at offsets `0/4/8` of a sixteen-byte record. The fourth
word is retained: it is zero in the captured initialized owner and this loop
does not write it. The destination is exactly
`60501000 + 16*(GPU_address>>12)`. Both surfaces produce eighty records and
240 DWORD stores. Neither descriptor flags nor page records encode a difference
between layout kinds one and two.

Actual calls to the packed flush decoder `59af0` retain tokens
`0,3000018f,10000005,20000028`; guest MMIO stores at `59b82` emit `31c=f,0,1`
after compact publication, then `5` before the secondary mirror and `0,2`
after it. These are the Windows allocation sequence, independently captured;
they must not silently replace the different Linux compact-only sequence.

The Linux `100e8` manager has zero companion extent and skips this second
view. That is a concrete vendor-profile difference. The CPU trace cannot show
whether tiled raster target accesses consume it, whether compact C2/HZ accesses
use a different table unit, or whether adding it fixes the retired black FAA
image. The observed layout-independent records rule out deriving a
kind-two-only metadata encoding from this loop. Hardware selection and the
default-restore pointer tags remain separate proof obligations.

Validate the retained genuine observation with:

```sh
python3 tools/check-m9-faa-secondary-pages.py
```

The checker verifies every record, actual source descriptor and child, all 240
stores per surface, the retained fourth word, ordered flush calls/MMIO, and
negative corrupt-address/value/metadata controls. It supplies no GPU reply,
completion, texture bytes, or pixel output.

<!-- SPDX-License-Identifier: GPL-2.0-only -->
