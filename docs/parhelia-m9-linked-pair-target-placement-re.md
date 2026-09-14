# M9 linked-pair target placement: static boundary

This note records the newly closed part of control-flow target placement and a
negative classification in the saved Linux translator. It does not infer a
selector-`6c` byte layout. Addresses are virtual addresses. `[BIN]` marks
direct binary evidence and `[DER]` a conclusion from it.

The pinned inputs are:

* Linux `m9x_drv.so`, SHA-256
  `3878475450ffad5cd648cc037cc65c29fcb07b5e86caf41ccf5983ef8e570273`;
* Windows `MSerX/MxMD.dll`, SHA-256
  `b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`.

`tools/m9-linked-pair-target-evidence.py` checks both identities, every byte
range and every direct call below without loading vendor code.

## The Linux 0x6d8 object is a per-target translation frame

Linux `m9x25697` at `0x3f6460` allocates
`target_count * (0xdb * 8) + 8` bytes at `0x3f64e2`, constructs objects at a
`0x6d8` stride at `0x3f6520`, and initializes each object through `m9x4836`
at `0x3f6682` [BIN].

The call argument and store sequences give this exact pointer map [BIN]:

| frame field | input |
|---:|---|
| `+0x30` | pipeline outer `+0x140` |
| `+0x38` | pipeline outer `+0x130` |
| `+0x40` | pipeline outer `+0x138`, the linked state |
| `+0x48` | per-target backend object |
| `+0x50` | another per-target input object |

`m9x919` selects `owner+0x30 + target_index*0x6d8` at `0x3f6224` and calls
`m9x5672` at `0x3f6231`. That helper calls `m9x11630` at `0x3f1443`.
`m9x11630` reads frame `+0x48`, selects backend `+0x728` at
`0x3ef9fc..0x3efa0d`, and reads the linked-state source from frame `+0x40` at
`0x3efa7f` [BIN].

The 0x6d8 object is therefore a host-side per-target translation frame joining
the linked state to a backend destination [DER]. It is not evidence for a
0x6d8-byte GPU-visible selector-`6c` record, a VS/FS split, or an instruction
offset. This removes a tempting false serializer candidate [DER].

## REP and LOOP carry a source anchor

The Windows REP lowerer at `0x1a7970` computes node `+0x24` at
`0x1a7a56..0x1a7a66` as [BIN]:

```text
anchor = ([token_argument + 0x2c]->dword_04 >> 1) & 0xffff
```

The LOOP lowerer at `0x1a7f20` performs the identical transform at
`0x1a8006..0x1a8016`, using token argument `+0x30` [BIN]. The exact semantic
unit of the source value before the shift remains unnamed; the recovered
value is called the **source anchor** here.

When compiler context `+0x270` is zero, both lowerers call scanner `0x1a6f70`
with the anchor. The scanner walks the current graph and returns the number of
type-1 and type-2 nodes whose `+0x24` equals that anchor. The result becomes
the new node's `+0x28` [BIN]. When context `+0x270` is nonzero, the lowerers
store `0xffffffff` at node `+0x28` instead [BIN]. Thus `+0x28` is an exact
same-anchor multiplicity or a disabled-scan sentinel, not an arbitrary
predecessor or native PC [DER]. The scanner body at `0x1a6f70..0x1a700a` has
SHA-256 `78c0da0ba8629c28f95644ff9dfdd5ae563ae6b8b41c2462389b770d8486f79a`.

Both lowerers then pass the same four cdecl arguments to builder `0x1ba2c0`
[BIN]:

```text
builder(compiler_context, anchor, context_flag, same_anchor_multiplicity)
```

## The builder preserves the anchor in typed descriptors

Builder `0x1ba2c0` snapshots `compiler_context+0x59c` and increments that
serial at `0x1ba2c8..0x1ba2d9` [BIN]. Let `A` be the 16-bit source anchor and
`S` the old serial. The descriptor fields written before object publication
are [BIN]:

| multiplicity | fixup object | descriptor fields |
|---:|---:|---|
| zero | 0 | `+0x14 = 0x94000000+A`; `+0x1c = 0xc4000000+A` |
| nonzero | 0 | `+0x14 = 0x94000000+A`; `+0x1c = 0xb4000000+S` |
| nonzero | 1 | `+0x14 = +0x1c = 0xc4000000+A` |
| nonzero | 2 | `+0x14 = +0x1c = 0xb4000000+S` |
| nonzero | 3 | `+0x14 = +0x1c = 0xc4000000+A`; `+0x18 = 0xb4000000+S` |

The zero-multiplicity sequence is at `0x1ba4fa..0x1ba520`; the nonzero
sequences are at `0x1ba565..0x1ba6f5` [BIN]. These are 0x20-byte compiler
fixup objects. They must not be decoded as packed two-dword native
instructions [DER].

This proves that the source anchor survives lowering as the low 16 bits of
typed `0x94` and `0xc4` descriptor families. Only the `0xb4` family uses the
new compiler serial [DER]. A driver implementation must not substitute the
serial for the branch target.

## Late mapping replacement and modifier preservation

The later transform at `0x1cef80` walks source descriptors and a table of
0x1c-byte mapping entries. At `0x1cf00b..0x1cf03c`, an entry matches when
[BIN]:

```text
(entry.key & 0xffff) == (source & 0xffff)
((entry.key ^ source) & 0xf0000000) == 0
```

On a match it reads `entry+0x04` as the replacement and performs the exact
merge at `0x1cf040..0x1cf055` [BIN]:

```text
resolved = replacement ^ ((replacement ^ source) & 0x0c000000)
```

It writes the result back to the same ordinal through helper `0x1a2b10` at
`0x1cf061` [BIN]. Therefore the replacement supplies every bit except 27:26;
those two bits remain from the source descriptor [DER]. This is the recovered
symbolic-anchor substitution boundary. Static evidence in this note does not
yet identify the producer meaning of every replacement value, so it does not
rename the replacement as a byte PC [DER].

## What remains open

The semantic chain is now exact through source-anchor extraction,
same-anchor multiplicity, typed fixup construction, mapping match, modifier
preservation and in-place replacement. The remaining selector-`6c` work is:

1. identify how each 0x1c-byte mapping entry's replacement is assigned from
   final native placement;
2. locate the byte serializer that joins the completed VS/FS native images and
   referenced resources into the heterogeneous linked record;
3. prove the final GPU-visible location and unit for resolved control targets.

Existing allocation, cache, COW publication and lifetime results are unchanged
and are intentionally not repeated here.

## Reproduction

From the repository root:

```sh
python3 tools/m9-linked-pair-target-evidence.py
```

The verifier uses only Python's standard library and performs static reads.
