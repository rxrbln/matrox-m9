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

The OpenGL entry side and its two linked-program banks are now exact.  The
Windows `glUniform4fv` thunk at `MxMGLX 0x100e92b0` obtains the current GL
context and jumps through context-vtable slot `+0x504`.  The concrete table at
`0x107f4a70` resolves that slot to `0x100b9f70`, which loads the native context
from `+0x238` and calls `0x1054f3b0`.  The adjacent 1fv, 2fv, and 3fv thunks use
`+0x4f8`, `+0x4fc`, and `+0x500` [BIN].

`0x10555cd0` proves that a non-negative GL uniform location is the index of a
64-byte metadata record in linked-program array `+0x617c..+0x6180`.  Common
updater `0x10559510` reads two independent native indices from metadata
`+0x24` and `+0x28`; `0xffffffff` means that the uniform is absent from that
stage.  For each array element, it compares and, only when changed, copies up
to one vec4 into [BIN][DER]:

| metadata field | value array | dirty vector |
|---:|---:|---:|
| `+0x24` | linked program `+0x410 + 16*index` | `+0x3f0` |
| `+0x28` | linked program `+0x2fd0 + 16*index` | `+0x400` |

The stage names are joined independently.  Selector `0x1054f020` makes
context `+0x54` the active vertex program when presence byte `+0x61ac` is set,
and `+0x58` the active fragment program when `+0x61ad` is set.  On a program
change, `0x105660e0` clears old `+0x54`'s vector `+0x3f0` and old `+0x58`'s
vector `+0x400`.  Therefore metadata `+0x24`, value base `+0x410`, and dirty
vector `+0x3f0` are the **vertex** bank; metadata `+0x28`, value base `+0x2fd0`,
and dirty vector `+0x400` are the **fragment** bank [BIN].

Activation helper `0x105564d0` uses 16-bit high-water counts at `+0x3e4`
(vertex) and `+0x3e8` (fragment), copies every live vec4 from the corresponding
array, and appends it to the corresponding dirty vector.  Subsequent uniform
updates compare old bytes first and append only changed ranges at
`0x1055972d..0x10559800`.  Activation therefore promotes the complete live
range, while ordinary updates promote only changed values.  The public GLSL
limit is 512 uniform components per stage, or 128 vec4 registers; this is an
API/compiler limit, distinct from the 64-entry final sparse-update queue
[BIN][DER].

The ARB program path rejoins the same updater.  `glProgramEnvParameter4fvARB`
at `0x100eaec0` dispatches through `+0x5a4`; concrete entry `0x100ba5c0` calls
`0x10567100`.  That function maps `GL_VERTEX_PROGRAM_ARB` (`0x8620`) to handle
`+0x18` and 20-byte env-record array `+0x44`, and
`GL_FRAGMENT_PROGRAM_ARB` (`0x8804`) to handle `+0x1c` and array `+0x58`.
It writes the vec4 at `20*index`, increments both the record and bank versions,
and calls dependency walker `0x1056b610`.  Dependent linked uniforms reach
`0x10569170`, which validates a one-element vec4 and calls common updater
`0x10559d90` [BIN].  This proves stage ownership before native transport, but
the dependency node does not expose the final sparse key.

The saved 32-bit Linux `libGL.so.2.0.0` independently exposes `glUniform4fv`
at `0x76380` and `glProgramEnvParameter4fvARB` at `0x79b10`.  They preserve the
same arguments and dispatch through Linux context-table slots `+0x894` and
`+0x720`, respectively.  The Linux table layout differs from Windows, so equal
slot numbers cannot be assumed; neither public thunk contains a sparse key
[BIN].

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

## Remaining unjoined boundary

The newly recovered path connects GL locations and ARB env records to explicit
vertex/fragment native indices and dirty vectors:

```text
GL location -> 64-byte metadata -> vertex index +0x24 / fragment index +0x28
            -> changed vec4 queued in vertex +0x3f0 / fragment +0x400
            -> private MxMGLX-to-MxMD transport
            -> MxMD sparse record key
        -> selector line and even/odd four-dword half
```

The last arrow remains unjoined.  Dirty-vector elements are 16-byte ownership
records for vec4 staging allocations; their index and value are not an MxMD
sparse key.  Linked-program installation opcodes `0x59` and `0x67` carry
native program blobs, entry information, and varying maps, but their proved
layouts do not carry the native-index-to-sparse-key association.  MxMGLX helpers
`0x105686e0` and `0x10569370` walk names such as `program_env_%d`,
`program_local_%d`, and `__constant__`, build binding/symbol tables, and
materialize runtime parameters, but their object layouts do not join to the
MxMD `0x1c`-byte records.  The conversion lies behind the private native
transport used when the queued staging objects are submitted.

Registry strings `PsBmConstants` and `CeBmConstants` occur beside
`PsBmMicrocode` and `CeBmMicrocode`, with independent configuration fields at
`MxMD` object offsets `+0x140` and `+0x148`.  They prove that the vendor names
two bus-master constant modes.  Static callers still do not establish whether
`Ps` or `Ce` selects the vertex or fragment dirty vector, nor whether either
names the even or odd sparse half.  The two parity halves are therefore **not**
assigned stage names here.

For a Mesa implementation, the remaining hard blocker is narrower: map each
stage-native index to the stored sparse key.  Separate vertex/fragment software
banks are proved, but `vertex index 0 -> key 0`, the parity-to-stage mapping,
and any fixed bank stride remain guesses.  Once that mapping is captured, the
final packet encoder is complete.

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
