# Matrox M9 blocked texture layout: static evidence boundary

This note traces the last software-visible address calculations around the
numeric class-3 texture layout. It distinguishes the CPU-visible staging
layout from the physical destination layout. Addresses are image virtual
addresses in the saved 32-bit Windows `MxMD.dll`.

The checked binary is
`xddm32_210_00_105_se_u_nowhql/MSerX/MxMD.dll`, SHA-256
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`.
Run `python3 tools/m9-blocked-layout-evidence.py` from the repository root to
verify the byte sequences, tables, and direct-call joins below.

## Class 3 is accessed through linear staging

The resource-to-descriptor join already establishes that resource source enum
`+0x1e8 == 1` becomes descriptor storage/layout class `+0x13c == 3`. The
ordinary 2D constructor at `0x51c39..0x51c50` tests that same source enum and
sets staging flag `+0x2b8 = 1`. When `+0x234` is zero it also sets `+0x2c4`,
selecting allocation of a temporary staging buffer on the first map.

The constructor computes ordinary linear byte geometry [BIN][DER]:

```text
one resource branch:
    staging_pitch = align(width, 4) * bits_per_element / 8
    staging_size  = align(height, 4) * staging_pitch

other resource branch:
    staging_pitch = align(width * bits_per_element / 8, 2)
    staging_size  = height * staging_pitch
```

The first formula is at `0x51c5e..0x51c89`, the second at
`0x51c91..0x51cb5`, and both store the pitch at `+0x2b0`. Per-level and
volume constructors repeat the same selection at `0x52ddc..0x52e4a`,
`0x539e4..0x53a4d`, and `0x54534..0x5459c`. This is construction evidence
for a linear CPU representation, rather than evidence for the physical
class-3 permutation.

The common row-pitch helper `0x4d1f0` closes the consumer join. It returns
`+0x290` for staging mode `+0x2a4`, `+0x2b0` for class-3 staging mode
`+0x2b8`, and only then falls back to resource-layout pitch formulas.

## The CPU fallback does not swizzle

The clear fallback `0xb5a80` chooses map helper `0x561b0` when `+0x2b8` is
set (`0xb5ba8..0xb5bba`). With `+0x2c4` set, `0x561b0` allocates the
page-aligned temporary buffer and returns its CPU pointer. Unlock chooses
`0x56430` at `0x55a77..0x55a87`; its temporary-buffer branch submits the
staging record at `0x5644e..0x56460` before freeing it.

After mapping, the fallback obtains `element_bytes = +0x1a0 >> 3` at
`0xb5b41..0xb5b4f` and constructs the first address at
`0xb5c33..0xb5c4e` [BIN][DER]:

```text
address = mapped_base + x * element_bytes + y * staging_pitch
```

The inner loop advances one logical element and the outer loop adds the
staging pitch (`0xb5d62..0xb5d78`). The table at `0xb5e38/0xb5e50` merely
dispatches element widths 1, 2, 4, 8, and 16 bytes to five masked writers.
Unsupported widths select the no-write/increment case. There is no coordinate
permutation or CPU swizzle table in this complete fallback.

## The GPU upload keeps logical coordinates

The normal resource-copy path reaches `0x56a10` or `0x56c60`, then transfer
builder `0x1b8d0`, and core emitter `0x1b650` [BIN][DER]. In the representative
`0x56a10` path:

* `0x56a3b..0x56a63` copies the four caller rectangle coordinates unchanged;
* `0x56a71..0x56a88` adds only the separately recovered packed-mip-tail X/Y
  origin when that special case is active;
* `0x56af0` clips the logical rectangle, and `0x56b66` calls `0x1b8d0`.

The transfer builder obtains source and destination descriptor/address
records through `0x1b420` at `0x1b8e9` and `0x1b8f4`. It copies the four
logical rectangle dwords again at `0x1b9b3..0x1b9d6`, then calls `0x1b650` at
`0x1b9f5`. The core emitter packs logical endpoints and emits the resource
descriptor state. It contains no CPU calculation of a destination byte offset
from X and Y.

This producer/consumer trace proves that the user-mode driver delegates the
linear-to-blocked conversion to the GPU transfer operation. A whole-image scan
additionally finds none of the standard `0x55555555`, `0xaaaaaaaa`, or
`0x33333333` Morton expansion masks in `MxMD.dll`; that absence is only
corroborating evidence, because a different implementation need not use those
masks.

## Exact static boundary and smallest capture

Static evidence now defines the logical staging address, staging pitch,
rectangle, descriptor, block-width/allocation fields, and the operation that
converts between the two resources. None of the inspected software computes
the physical byte position of a texel inside a class-3 block. The hardware
consumes the descriptor and logical coordinates, so the physical permutation
cannot be derived uniquely from these binaries.

The smallest decisive capture for an address-independent in-block mapping is
one class-3 allocation containing one complete block:

1. Use an uncompressed 32-bit format and the exact block width and allocation
   quantum reported by the recovered constructor (`+0x16c` and `+0x170`).
2. Lock through the vendor path and fill each logical texel with a unique
   32-bit code such as `0x5a000000 | (y << 8) | x`.
3. Unlock, wait for the upload fence, and record the emitted destination
   address plus descriptor words (`idx78/idx79`).
4. Dump exactly one physical destination block from VRAM as 32-bit words.

Each code directly maps one logical `(x,y)` to one physical word offset. If
the permutation may include block-address-dependent XORs, use one 2-by-2 block
allocation in the same capture, encode block X/Y in the upper code bits, and
compare all four blocks. That is the smallest extension which distinguishes a
fixed in-block permutation from horizontal or vertical block-dependent address
folding. An MMIO command trace without the destination VRAM dump is
insufficient: the traced command deliberately contains logical coordinates.
