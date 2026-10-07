# M9128 fresh FIRST depth and raster controls

The six controls in `tests/m9-faa-first-raster-source-20261008` use unchanged
original source from authenticated INIT through the first bound CLEAR3,
LESS/write, six-vertex public DRAW and genuine normal Lock. All DRAW calls
return1/HRESULT0. The original blocking Lock200 read remains unexecuted;
no rendering, opaque GPU input or new completion is supplied.

The whole primary stream is identical across cull1/2/3 and smooth/flat:
13164 bytes with fourteen genuine ENDs. Secondary is2712 bytes, published in
ranges0..1840 and1840..2712. Existing FIRST6A payloads are at offsets596,
1232,1792,2676. Cull1 yields29340000; cull2 and3 yield28ff0000. FIRST therefore
needs no24-byte insertion, unlike a retained flow missing that state packet.
The final writer for each payload isf760c. Relative links and primary indirect
range sites remain unchanged.

| Source state | EF at1920 | Raster control at1936 |
| --- | --- | --- |
| smooth, cull1 | d8024055 | 00000403 |
| smooth, cull2/CW | db024055 | 00000403 |
| smooth, cull3/CCW | da024055 | 00000403 |
| flat, cull1 | d8004055 | 00004403 |
| flat, cull2/CW | db004055 | 00004403 |
| flat, cull3/CCW | da004055 | 00004403 |

The control4000 bit and CW/CCW03000000/02000000 direction bits are source
state differences. The EF20000 difference accompanies a different compiler
result; it is not independently qualified as a universal FLAT bit.

Smooth emits FS64/carry56/gather40/position200. Flat emits FS32 with the same
carry/gather/position instruction bytes. Its four LE64 slots are:

```
ffe2ad9e e29fff1f
ffe22d9d a29fff1f
ffff801c a01fc00f
ffff801c a01fc00f
```

The136-byte known constant image has smooth utility floats `(1,.5,0,1)`
and flat `(0,1,1,.5)` at bytes0..16. Bytes16..136 match exactly, including
live CB0 COLOR and viewport/clip constants. The program entry index operands
at secondary352/980/1592 move8/15/20 to4/11/16 as the FS shrinks; these must
be derived from the assembled live program, not fixed captured offsets.

Additional flat differences, with actual source producers:

| Secondary offset | Smooth → flat | Actual source read/write |
| --- | --- | --- |
| 1920 | clears00020000 | fc200 reads fragment shadow+20; fc203 publishes EF |
| 2276 | 01189ff0 → 0110fff0 | f5053 reads state shadow+10; f5056 publishes |
| 2616 | 00100d88 → 80100d88 | f70f4 reads varying entry; f70fa publishes |
| 2620 | 001006b6 → 801006b6 | f70fd reads paired varying entry; f7102 publishes |

These are paired shader-state fields. Their upstream allocator/interpolation
producer contract remains a separate live-lowering task; this differential
does not license modifying them solely because Gallium flatshade is true on
an unchanged smoothFS64. The AC word at2224 also differs, but its actualfbee8
writer carries inactive stack residue, not a shader field to derive from flat.

The checker matches every remaining secondary DWORD to the smooth baseline,
so no additional insertion, hidden state difference or changed instruction
vector is silently discarded. Captured addresses are source identity evidence
only. Hardware flat/provoking/cull pixel proof and application shader admission
remain separate from this complete source transition.
