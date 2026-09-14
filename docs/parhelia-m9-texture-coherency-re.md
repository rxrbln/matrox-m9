# Matrox M9 texture-write coherency: static evidence boundary

This note isolates the static evidence for an M9 resource write followed by
texture sampling.  It deliberately does not rename common resource/address
records as texture-cache flushes.  Addresses below are image virtual addresses
in the saved 32-bit Windows `MxMD.dll` unless stated otherwise.

The checked artifacts are:

| artifact | SHA-256 |
|---|---|
| `xddm32_210_00_105_se_u_nowhql/MSerX/MxMD.dll` | `b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43` |
| `xddm32_210_00_105_se_u_nowhql/MSerX/MxMGLX.dll` | `d4e04dc371809f079651e9938b285878afd42acabcba2eaabef2e65d218e9f86` |
| `m9xdriver-x86_64-1.4.3-80847-20140908-build_98/kernel/KernelLibraries.o_shipped` | `bec4490ff72b29887400e988a26e107e80ebb0b11e9d0b437267bec9db5b26e7` |

Run `python3 tools/m9-texture-coherency-evidence.py` from the repository root
to check the hashes, packet words, Linux cross-check, and the complete direct
`CALL rel32` census for the named Windows routines.

## Exact texture descriptor path

`0xf4350` is the final four-unit texture/sampler emitter.  Its only direct call
is at `0xf55e0`, inside the common emitter's unit loop.  The emitted groups are:

| dirty group | indices | relevant source |
|---:|---|---|
| common | dynamic `70 + unit` | backend `+0x10` |
| `0x02` | `78,79,7a` | backend `+0x4c,+0x50`; relocation from `+0x44/+0x48` |
| `0x04` | `7c,7d,87` | sampler/format fields |
| `0x08` | `82,83,84,85` | sampler/format fields |
| `0x10` | `80,81,7e,7f` | sampler/LOD fields |
| `0x20` | `c0` | relocation from `+0x3c/+0x40` |

The comparator at `0xf41f0` marks groups by comparing descriptor and sampler
fields.  `0xf4350` clears the backend dirty mask at `0xf4615`.  Neither routine
reads a content-generation counter or receives a signal from the upload
routines identified below.  `0xf4350` does not emit indices `e6`, `e3`, `ef`,
`c2`, `c3`, or `8f`.

This proves that rebinding or changing a backing address causes texture
descriptor state to be emitted.  It does not prove that rewriting content at
the same address makes this descriptor comparator dirty.

## Exact resource-copy/upload path

The resource-copy entry `0xb21b0` has four direct callers (`0x55f5d`,
`0x56074`, `0xa5197`, and `0xa57d4`).  It selects among copy helpers including:

```text
0xb283b -> 0x56c60 -> 0x1b8d0
0xb2c29 -> 0x56eb0
0xb2ce4 -> 0x56a10 -> 0x1b8d0
```

`0x56a10` and `0x56c60` adjust packed-mip-tail coordinates before reaching
`0x1b8d0`.  That routine constructs a GPU transfer/blit task and calls the core
emitter `0x1b650` at `0x1b9f5`.  Other copy shapes go through `0x571d0`,
`0x574f0`, and `0x57600`.  At `0xb2e20` or `0xb2e32`, `0xb21b0` stores the
calculated byte offset in destination-resource field `+0x3e0`; the stored
value is an offset, not a Boolean dirty flag.

The direct-call census finds no edge from `0xb21b0`, `0x56a10`, `0x56c60`,
`0x56eb0`, `0x1b8d0`, or `0x1b650` to `0xf4350`, `0xf4f50`, `0xf5730`,
`0xf59b0`, `0xfc620`, `0xfcb60`, or `0xfc8f0`.  This is a precise negative
result for direct calls.  Virtual dispatch and higher-level task sequencing
remain outside that result.

`MxMGLX.dll` contains diagnostics for
`TextureObjectMgr::GenericUpdateTextureImage()` and
`TextureObjectMgr::SynchronizeTexture()`.  The strings confirm that its GL
layer has explicit update and synchronization concepts.  Static string
references have not yielded a native M9 packet producer, so they do not join a
GL texture update to any packet below.

## What the common `e6/e3/c2/c3/8f/ef` sites prove

The only direct caller of common-state emitter `0xf4f50` is `0xfae25`.  On its
normal successful path:

1. Common dirty bit `0x02` calls `0xf5730` at `0xf5197` and is then cleared.
2. Common dirty bit `0x04` calls `0xf59b0` at `0xf51a8` and is then cleared.
3. After those conditional branches, `0xf5227..0xf5242` emits the fixed indexed
   pair `e6 = 0x04000001`, `8f = 0x80000600`.

There is no `e3` in that fixed pair.  The `ef = 0` write at
`0xf4f95..0xf4fa6` occurs only when common object field `+0x3e4` was nonzero;
the path first calls whole-common-shadow initializer `0xf4a80`.  This is a
context/common-state reset condition, with no static join to resource content
writes.

### `0xf5730`: common dirty group `0x02`

This routine compares record fields `+0xc4/+0xc8` with globals
`0x20d51c/0x20d520`.

* When the address/domain differs, it optionally registers the resource when
  record flag `+0x08 & 0x02` is set, then emits the `c2,c3,8f` tag and applies
  relocation selectors `0x13` and `0x14` to the `+0xc4` address record.
* When it matches, it emits `e6 = 0x04000001`, obtains `e3` dynamically from
  the command-buffer callback at vtable offset `+0x70`, emits `c2 = 4,c3 = 0`,
  and follows with `+0xec` entries from the array at `+0xf0`.

Its only direct caller is the common dirty-bit path above.  No upload helper or
texture descriptor object reaches it through a direct call.  The packet is an
exact resource-bank transition, but the static evidence does not identify it
as a texture resource or a texture-cache invalidate.

### `0xf59b0`: common dirty group `0x04`

This routine optionally registers the two address records `+0xd0/+0xd4` and
`+0xd8/+0xdc` when `+0x08 & 0x04` is set.  It emits `8c,8d,8e,8f` from
`+0xcc` and zero payloads.  One variable-bank branch emits
`e6 = 0x04000001`, a dynamic `e3` from the same callback class, and entries
controlled by record counts `+0xf8` and `+0x2fc`.

Its only direct caller is the common dirty-bit path above.  Like `0xf5730`, it
has no direct static join to the texture upload or sampler emitter.  Calling it
a texture-cache transition would therefore exceed the evidence.

## Late address-domain transitions are a separate family

The complete direct callers of the late helpers are:

```text
0xfc620 <- 0xfc3c9
0xfcb60 <- 0xf9080, 0xfc378
0xfc8f0 <- 0xfc365, 0xfce79
```

`0xfc620` and `0xfcb60` operate on late-state address records at `+0x58` and
`+0x74`.  They compare low address bits and upper domain bits against
`0x20d51c/0x20d520`, register relocations, and use selectors `0x2c/0x2e` for
indices `2e/2f`.  Both routines scan task resources at emitter fields
`+0x5e4/+0x5e8`, select a maximum completion value, and wait/join it through
virtual callbacks before emitting the transition.  Their fixed packet bodies
are:

| routine | fixed body |
|---|---|
| `0xfc620` | `e6 = 0x42000000`; eight index-`44` values of `0x24081913` total, with the eighth bundled in the final `44,2e,2f` packet |
| `0xfcb60` | `e6 = 0x42000000`; eight index-`44` values of `0x28041968`; two later `e6 = 3`; then `2e,2f` |

`0xfc8f0` emits address-pair transitions through indices `1a,42`, with observed
payload forms `0xa1000009`, `0xa3000009`, and `0xa4000009`; one branch ends in
`e6 = 0x40008000`.  It uses relocation selector `0x30`.

These facts support the narrower description **address-window/domain
synchronization with resource completion ordering**.  The late-state caller
family is disjoint from the direct upload and final texture-state call families.
Static analysis therefore does not establish any of these helpers as the
missing texture-write cache operation.  They may still be necessary when a
resource changes address domain.

## Linux cross-check prevents a false `e6` conclusion

The saved Linux `KernelLibraries.o_shipped` contains the little-endian literal
`0x04000001` nine times.  A resolved non-texture occurrence is in global
function `m9x193` at `.text+0x2c2e9`: record mode 2 stores `0x04000001` and
then writes computed resource-address words.  This proves that the value is
also a resource/requester record and is not unique to texture sampling.

Nearby `m9x1011` at `.text+0x2bf33` places `0x6dc00000`, `0x42000000`, and
`0x000000a1` behind an `e6,e6` tag in another address transition.  Index `e6`
is a polymorphic command/state port.  Neither the index nor
`0x04000001` alone names a flush.

## Defensible driver sequence before a differential capture

The static code supports the following conservative sequence for an update
followed by sampling:

1. Submit the copy/upload or render-write task and retain its resource.
2. Order the later sampling task after that producer's completion fence.
3. Re-emit the `78,79,7a` texture address/descriptor group when the backing,
   offset, layout, or address domain changes.  Re-emitting on every content
   generation is a conservative experiment, not a recovered vendor rule.
4. Preserve the normal common-state `e6 = 0x04000001,
   8f = 0x80000600` pair while testing, because the Windows driver emits it on
   the successful common 3D path.  Do not describe it as a texture invalidate.
5. Use the `fc620/fcb60/fc8f0` family only for the address-domain transitions
   its object and relocation flow requires.  Injecting it after every texture
   write has no static justification.

This sequence gives task ordering and descriptor visibility without inventing
a cache-packet meaning.  It does not yet prove that same-address texture
content becomes visible with only the ordinary common-state prefix.

## Remaining boundary and required capture

Static analysis has not recovered an exact dedicated texture-cache invalidate,
nor proved that one exists.  The unresolved boundary is indirect task/resource
dispatch between the GL update methods, the GPU copy task, and the next 3D task.
A differential command/MMIO capture must compare at least:

* sample without an intervening write;
* GPU blit/upload to the same backing followed by sample;
* CPU lock/update to the same backing followed by sample;
* render-to-texture followed by sample;
* the same cases with unchanged and changed backing address/domain.

For each pair, capture the complete producer tail, task submission boundary,
consumer prefix, relocations, and waits.  Diff the occurrences of
`e6/e3/c2/c3/8f/ef` while holding all sampler and descriptor fields constant.
That experiment can distinguish an ordinary per-task prefix, an
address-domain transition, descriptor rebinding, and a true content-coherency
operation.  Until then, assigning cache semantics to any one of these packet
words is unsupported.
