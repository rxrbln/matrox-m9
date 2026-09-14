# M9 program constants: static transport boundary

This note follows native shader constants as far as the saved M9 Windows and
Linux binaries allow without assigning an unproved stage name to the final
ports.  Addresses below are virtual addresses in the PE32 images and ELF text
addresses in the Linux image.  `[BIN]` means direct disassembly; `[DER]` means
arithmetic derived from it.

The files checked are:

| image | SHA-256 |
|---|---|
| `MSerX/MxMGLX.dll` | `d4e04dc371809f079651e9938b285878afd42acabcba2eaabef2e65d218e9f86` |
| `MSerX/MxMD.dll` | `b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43` |
| Linux `m9x_drv.so` | `3878475450ffad5cd648cc037cc65c29fcb07b5e86caf41ccf5983ef8e570273` |

## What is joined

The compiler-side source is exact.  As detailed in
`parhelia-m9-native-shader-fragments.md`, a source `c[i]` becomes descriptor
`0x20000000 + 2*i`.  Generic lowerer `MxMD 0x1b5972..0x1b5a6a` recognizes the
top nibble `2`, puts the descriptor in intermediate field `+0x10`, sets format
gate 1, and packs its low seven bits in native `W1[20:14]`.  Consequently
`c0` produces native selector value zero in that gated instruction field
[BIN][DER].

The OpenGL entry side is also exact but ends at a virtual dispatch boundary.
The Windows `glUniform4fv` thunk at `MxMGLX 0x100e92b0` obtains the current GL
context and jumps through context-vtable slot `+0x504`; the adjacent 1fv, 2fv,
and 3fv thunks use `+0x4f8`, `+0x4fc`, and `+0x500`.  The ARB program path is
separate: `glProgramEnvParameter4fvARB` at `0x100eaec0` jumps through slot
`+0x5a4` [BIN].  These public thunks preserve the GL arguments but contain no
constant key or stage-bank conversion.

The final MxMD sparse upload is exact in both vendor builds.

### Windows record and packet

Constructor `MxMD 0xf6240`, at `0xf62f0..0xf631e`, initializes 64 records
starting at program-state `+0xc0`, with stride `0x1c` [BIN]:

| record offset | contents |
|---:|---|
| `+0x00` | key, initialized to `0xffffffff` |
| `+0x04..+0x10` | four inline payload dwords, initialized to zero |
| `+0x14` | current dirty/live flag, initialized to zero |
| `+0x18` | pending dirty flag, initialized to zero |

State promotion `0x13dabd..0x13dad5` begins at state `+0xd4`, copies
`record[+0x18]` to `record[+0x14]`, advances by `0x1c`, and repeats 64 times
[BIN].

Central emitter `0xf6f20`, branch `0xf7312..0xf7480`, handles dirty bit
`0x8000`.  For every live record it computes [BIN][DER]:

```c
line = (key >> 1) + 0x80;
selector = 0x80000000 | ((line & 0x1ff) << 5);
ports = (key & 1) ? 0x67666564 : 0x63626160;
```

The apparent `or 0xfc000000; shl 5` in the 32-bit code is exactly the selector
formula above after 32-bit wraparound.  A changed selector is emitted with
indexed header `0x45454568`.  The four payload words follow under ports
`60,61,62,63` for an even key or `64,65,66,67` for an odd key.  The emitter
then clears current dirty `+0x14` [BIN].

Thus keys `2*n` and `2*n+1` name the lower and upper four-dword halves of one
32-byte selector line at `0x80+n` [DER].  The record array has 64 pending
updates, but the key is a stored 32-bit value rather than the record's array
index.  The static code does not constrain it to 0..63.

These constant values are inline command payload.  This path has no resource
relocation or GPU pointer: the selector's nine-bit line field is an instruction
or constant-store address, not a memory address [BIN].  Native program emitter
`0xf74d0` happens to reuse selector index `68` and payload ports `60..67`; it
also has relocation-backed external-program alternatives through `69` and
entries `6a/6d`.  That is a distinct program upload path and must not be used
to infer address semantics for the sparse constants.

### Linux independent match

Linux central emitter `m9x26001` at `0x4ba3a0` contains the same sparse path
at `0x4ba589..0x4ba838` [BIN].  Its ABI-shifted record base is state `+0xd4`:
payload begins at `+0xd8`, current dirty is `+0xe8`, records advance by
`0x1c`, and the loop count is again `0x40`.  At `0x4ba6d1..0x4ba6de` it loads
the key from payload pointer `-4`, computes `(key >> 1) + 0x80`, and tests key
parity.  At `0x4ba5c0..0x4ba661` it masks the selector to nine bits, shifts it
by five, sets bit 31, emits header `68`, and selects exactly the same
`60..63`/`64..67` port headers.  It copies the four payload dwords and clears
current dirty at `0x4ba6af` [BIN][DER].

This independent match proves record width, capacity, key parity, selector
arithmetic, inline payload semantics, and port numbers.  It does not name the
two halves as vertex and fragment banks.

## What is not joined

No recovered static call/data path connects any of these three endpoints:

```text
GL uniform location/value
        -> hidden MxMGLX context-vtable implementation / native transport
        -> MxMD sparse record key
        -> selector line and even/odd four-dword half
```

Linked-program installation opcodes `0x59` and `0x67` carry native program
blobs, entry information, and varying maps; their proved layouts do not carry
the GL uniform-location to sparse-key association.  MxMGLX helpers
`0x105686e0` and `0x10569370` walk names such as `program_env_%d`,
`program_local_%d`, and `__constant__`, build binding/symbol tables, and
materialize runtime parameters, but their object layouts do not join to the
MxMD `0x1c`-byte records.  The consumer lies behind the private GL/native
interface.

Registry strings `PsBmConstants` and `CeBmConstants` occur beside
`PsBmMicrocode` and `CeBmMicrocode`, with independent configuration fields at
`MxMD` object offsets `+0x140` and `+0x148`.  They prove that the vendor names
two bus-master constant modes.  Static callers do not establish that `Ps` or
`Ce` corresponds to a particular GL shader stage, nor that either names the
even or odd sparse half.  The two parity halves are therefore **not** assigned
stage names here.

For a Mesa implementation, the remaining hard blocker is the mapping from a
stage's logical `c[i]` (or linked uniform location) to the stored sparse key.
`c0 -> key 0`, separate vertex/fragment banks, and any fixed bank stride would
all be guesses.  Once that mapping is captured, the final packet encoder is
complete.

## Smallest decisive differential capture

Capture both the MxMGLX private stream and the final MxMD command buffer so a
GL location can be joined to its sparse key.  Use a fixed minimal linked pair
and four finite values with uncommon bit patterns.

1. Fragment-only case: use `uniform vec4 u; gl_FragColor = u;`, bind once,
   then issue one draw after each `glUniform4fv(u, 1, A/B)` update.  Keep every
   other state and shader byte unchanged.
2. Vertex-only case: move the same declaration and name into the vertex
   program and make it affect one live output; keep the fragment program
   constant.  Repeat A/B.
3. Index stride case: keep two simultaneously live `vec4` uniforms, update
   each independently, and repeat with a two-element uniform array.
4. Toggle `PsBmConstants` alone, then `CeBmConstants` alone.  If a mode removes
   inline values from the final buffer, dump the referenced bus-master buffer
   as well.  This assigns the vendor label from behavior rather than spelling.

For each draw, locate the exact four float dwords after header
`0x63626160` or `0x67666564`, then read the nearest preceding `68` selector:

```c
key_parity = (header == 0x67666564);
line = (selector >> 5) & 0x1ff;
key_mod_1024 = 2 * ((line - 0x80) & 0x1ff) + key_parity;
```

The private-stream record that changes with the same A/B payload supplies the
missing location-to-key join.  Comparing vertex-only and fragment-only cases
assigns stage ownership; the two-uniform and array cases establish register
stride.  One capture matrix therefore resolves all remaining constant-bank
questions without relying on ambient state or inferred `Ps`/`Ce` names.

Run `python3 tools/m9-program-constant-evidence.py` to verify the byte-level
facts used by this note.
