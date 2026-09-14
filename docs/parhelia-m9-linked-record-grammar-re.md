# M9 linked render-resource record: closed grammar boundary

This note follows the object selected by final index `6c` from CPU-side
construction to command emission. It records the strongest implementation
contract supported by the saved Linux and Windows binaries and the 17-case
live shader fixture. `tools/m9-linked-record-grammar-evidence.py` pins every
address and live invariant below.

The central result is that `6c` selects an aligned **linked render-resource
record**, not a universal native-program image. Native-looking 8-byte words
occur in some records, but neither operating-system binary exposes a fixed
record-relative code start, code count, VS/FS split, entry selector, or stop
rule along the statically reachable serialization path.

## Exact Linux producer chain

The pinned image is X-server 1.13 `m9x_drv.so`, SHA-256
`3878475450ffad5cd648cc037cc65c29fcb07b5e86caf41ccf5983ef8e570273`.

`m9x12620` at `0x3d23c0` owns four nested state objects. It publishes
`ctx+0x30`, `+0x68`, `+0x94`, and `+0x1fd` through outer pointers `+0x140`,
`+0x148`, `+0x130`, and `+0x138`; then `0x3d2473` calls constructor
`m9x31054` for the `+0x1fd` object. This fixes the identity of the object later
read through `outer+0x138`.

`m9x31054` at `0x3bea10` initializes two packed resource references:

| nested bytes | exact construction | later consumer |
|---|---|---|
| descriptor `+0x2f1..+0x2f4`, object `+0x2f9` | stores at `0x3bf386..0x3bf3da` | promoted to state `+0x98/+0xa0` |
| descriptor `+0x301..+0x304`, object `+0x309` | stores at `0x3bf3f1..0x3bf443` | promoted to state `+0xb8/+0xc0`, then relocated at `6c` |

Each descriptor preserves three high bits already present in the destination
and replaces the remaining 29 bits. Both are initialized from the same global
descriptor/object source at `0x125c9f0`. No stage tag is introduced at either
pair boundary.

`m9x11630` at `0x3ef9c0` is a backend state translator. At
`0x3f06a0..0x3f070e` it copies nested object `+0x309` to backend `+0xf8`,
reassembles descriptor bytes `+0x301..+0x304` into backend `+0xf0`, preserves
the top three bits, and marks dirty bits. This is the exact producer/consumer
join; the routine does not serialize compiler instructions.

`m9x21983` at `0x44e330` reads the compiled pipeline's `outer+0x138` object.
It promotes `+0x2f1/+0x2f9` to a first resource pair and `+0x301/+0x309` to
the second pair at `0x44e36e..0x44e46d`. Cache orchestrator `m9x24062` calls
it at `0x4530db`, then clears the nested dirty heads at `0x4530e8..0x45311b`.
Final `m9x2427` consumes the second pair as the usage-`0x1a`, descriptor-4
relocation placed at index `6c`.

Two lifetime facts prevent treating the nested object as an immutable shader
blob:

* `m9x10394` at `0x3aebc0` copies exactly `0x169` bytes from the object at
  outer `+0x130`. Since that pointer is nested `+0x94`, the copy ends at
  nested `+0x1fd`, exactly where outer `+0x138` begins. The linked object is
  reconstructed or reference-tracked separately.
* `m9x5565` at `0x3afa30` rebuilds a vector at outer `+0x3108`; subtracting
  begin from end and shifting by four proves 16-byte entries. It deduplicates
  and appends objects from shader state, texture units, render resources, and
  other state, with a companion flag. This is a heterogeneous dependency
  list, not a stage-local instruction list.

## Windows Direct3D cross-check

The pinned `MxMD.dll` has SHA-256
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`.
The callable Direct3D serializer `0xfadf0` orders surface state through
`0xfb920`, common state through `0xf4f50`, program/resource state through
`0xf6f20` at call site `0xfae32`, and inputs through `0xfb260`.

`0xf6f20` is therefore a real draw serializer. It calls `0xf74d0` at
`0xf7307` for the `69/6a/6d` group and `0xf78c0` at `0xf7496` for the
`68/6b/6c` group. The external `6c` packet passes usage `0x1a`, descriptor 4
to the relocation callback. It preserves the same separation as Linux but
does not serialize a record header or a native byte count itself.

The fixed-chain builder `0x13c2c0` is independently call-reachable from
`0x110946`. It conditionally runs builders `0x13b7e0`, `0x13b9b0`,
`0x13bb80`, `0x13bda0`, then calls fixed fragment linker `0x13bf80` at
`0x13c503`. This route feeds the alternative inline upload through selector
`68` and ports `60..67`. It cannot define the external linked-record grammar.

This adds one concrete boundary to the broader hidden-feature audit: the D3D
render/sampler/shader paths are call-reachable, but their final external
resource serializer deliberately delegates address construction to a private
relocation callback. No dormant registry switch turns that callback into a
fixed native-program record.

## Live record invariants

In `captures/m9x-linux-gl2-special-20260914`, all 17 final `6c` addresses
clear to a unique, monotonically advancing, `0x80`-aligned offset in the same
system allocation. The selected end-exclusive extents are `0x400`, `0x480`,
`0x500`, or `0x580`, all `0x80` aligned.

Every record begins with the same seven floats:

```text
64.0, -64.0, 0.5, 0.0, 64.0, 64.0, 0.5
```

The eighth dword can vary, so it is not included in that invariant. The seven
floats are geometry/setup content. At relative `+0x100`, more than eight distinct
16-byte prefixes occur. Baseline has
`7f000000 08000000 00000000 1c000000`; `dfdy` has instruction-shaped
`1d00621e 083c00ae 0091ffff 0f0000a0`. Thus `+0x100` is a valid candidate
native span in selected cases and a direct counterexample to a universal code
base in the baseline case.

The allocation snapshots are identical before and after `ExecuteDma`. That
establishes CPU-side construction before submission and excludes GPU-generated
record output for these draws. It does not reveal which private allocator or
virtual builder wrote each subrecord.

## Safe driver contract

A clean driver may rely on these facts:

* the external record relocation is separate from drawable index `69`;
* the record address is `0x80` aligned and retains only the proved low tag;
* record bounds are end-exclusive and must remain inside the BO;
* referenced BOs must remain alive until the submission fence retires;
* state changes may replace the complete record atomically;
* fixed-chain inline uploads are a separate representation.

Arbitrary programmable submission must remain gated. The missing universal
fields are native byte start, byte or slot count, VS/FS ownership, stage entry
selectors, padding and termination, and allocator reuse/wrap. Static tracing
ends at private resource objects and virtual relocation/allocation callbacks;
assigning meanings beyond that boundary would be guesswork.

## Minimal dynamic closure

`tools/m9x-linked-record-gdb.py` installs passive, bounded breakpoints on the
constructor, backend translator, and cache promoter. It logs only CPU object
addresses and the two descriptor/object pairs, continues immediately, and
does not call vendor functions or modify rendering. Run it after the target is
handed off and the DSO is mapped.

The next capture should pair that log with complete system-allocation snapshots
for: one VS with two FS programs, one FS with two VS programs, and one-stage
code lengths around 4, 8, 16, and 32 native slots. A field earns a stage/count
meaning only if its CPU producer, final address, and live byte delta join in
all those cases. A forced allocator wrap is needed separately to close reuse;
do not infer wrap from the current monotonic 17-record fixture.
