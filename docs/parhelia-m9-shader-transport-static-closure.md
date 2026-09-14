# M9 native shader transport: static closure and implementation boundary

This note reconciles the linked-record, stage-install, entry, termination, and
native-control-flow investigations into one driver-facing result. It uses only
the saved binaries. `[BIN]` is a direct binary observation and `[DER]` is the
narrow conclusion supported by it.

The pinned inputs are Windows `MxMD.dll`, SHA-256
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`,
and Linux 1.4.3 build 98 `xserver/1_13_0/m9x_drv.so`, SHA-256
`3878475450ffad5cd648cc037cc65c29fcb07b5e86caf41ccf5983ef8e570273`.

## Driver-facing result

There is no statically supported universal `6c` record field containing a
native-image address, slot count, vertex start, fragment start, or terminal PC.
The reachable `6c` objects are heterogeneous linked render-resource records,
and captured record-relative native-looking regions move between workloads.
The external bind writes a resource through `69`, controls through `6a/6d`,
and the linked record through `6c`; it emits no code count or end [BIN].

The fixed-chain inline form is a clean, bounded transport for a driver-owned
native image [DER]. It uploads 8-byte slots in 32-byte lines through selector
`68` and ports `60..67`, writes `69=4`, and supplies two 16-bit slot starts in
`6a` or splits the companion start to `6d`. The Windows builder records image
pointer/length at `+0xbb4/+0xbb0` and starts at `+0xba8/+0xbac`; Linux uses
`+0xbe0/+0xbd8` and `+0xbd0/+0xbd4` [BIN]. The upload is padded to four slots,
and the compiler-side slot vector rejects growth beyond 256 slots [BIN].

This is sufficient to implement native-image upload, bounds checking, padding,
and entry emission without reverse engineering a universal external record.
It is not yet sufficient to label the two starts as universal VS and FS
entries. The recovered inline producer is the fixed vertex/declaration chain;
its second start is a selected companion chain, and the inactive-companion form
packs both starts into `6a`. One stage-swap capture remains necessary before a
new driver assigns arbitrary vertex and fragment programs to these controls.

## Typed control resolution closes before serialization

Windows control builders allocate 0x20-byte objects with vtable `0x1f50e0`.
The live virtual sequence is [BIN]:

| vtable slot | VA | role |
|---:|---:|---|
| `+0x00` | `0x1dc790` | type-2/type-3 compatibility dispatch |
| `+0x04` | `0x1dc7d0` | clone, resolve, then serialize |
| `+0x08` | `0x1dc8b0` | direct native-slot serializer |

`0x1dc7d0` copies all fields into a temporary object, constructs the local
resolver data passed by its caller, invokes `0x1dc490` for type 2 or `0x1dc690`
for type 3, then calls `0x1dc8b0`. Thus a
`0xb4000000 + ctx[0x59c]` value is an intermediate
typed descriptor, not a native branch displacement [DER].

For type 3, `0x1dc690` walks eligible descriptors in object fields `+0x14` and
`+0x18` in encounter order. Eligibility is `high_nibble != 0xf` and
`descriptor & 0x0c000000 != 0`. Each eligible value is replaced by dword 1 of
the next 12-byte entry in a resolved table and enables serializer gate `2` or
`4`. An eligible `+0x1c` is replaced from the corresponding transformed local
value and enables gate `0x10` [BIN]. Lookup `0x1a2e60` uses six inline entries
at resolver `+0x6c`, then a dynamic tail at `+0xb8`; an out-of-range lookup
returns `0xf0000000` [BIN]. The serializer performs no arithmetic on the low
serial value. Physical destination selection has already become a resolved
descriptor before native packing [DER].

This closes the typed-fixup *mechanism* but does not recover the upstream rule
that builds the ordered resolved table from graph targets. Forward branch,
backedge, CALL, BREAK, and reconvergence target placement still need either
the linker pass decoded to that table's producer or a compiler hook dumping
the table before `0x1dc7d0`.

## Exact serializer footprint

Direct serializer `0x1dc8b0` packs fields under the gate word at object `+0x10`
[BIN]:

| gate | source | native insertion |
|---:|---:|---:|
| `0x02` | `+0x14` | second output dword bits 20:15 |
| `0x04` | `+0x18` | first output dword low 6 bits |
| `0x10` | `+0x1c` | second output dword bits 13:9 |
| `0x08` / `0x41` | `+0x0c` | first output descriptor/control field |

It dispatches on `(object[+8] >> 22) & 31`. Classes 19 and 20 subtract four
from the first output dword; classes 19, 20, 21, 22, and 25 append the packed
slot plus three literal `{0,0}` slots and set output property bit 29. Classes
23 and 24 append only the packed slot. Every append checks `count + 1 <= 256`
[BIN]. The helper at `0x14c6a0` merely copies one two-dword slot and increments
the count, so those three slots are padding/footprint, not hidden target
patches [DER].

A clean compiler may reproduce the resolved native fields and footprints. It
must not expose the Windows intermediate `0xb4000000 + serial` marker as a
kernel ABI because this marker belongs to the compiler's pre-resolution object
graph rather than the native transport [DER].

## Terminal rule

Both vendor compiler paths converge on terminal slot [BIN]:

```text
W0 = fffc7e1d
W1 = a2803f0f
```

It moves the stage-relative output selector `0x78` to destination selector
`0x7e`. Native transport sends neither a slot count nor an end PC. Static code
cannot distinguish whether selector `0x7e` itself terminates execution or the
whole final MOV form does. A driver can safely append the exact vendor terminal
slot to compiler output; claiming a general `END` encoding still requires the
single terminal mutation experiment described in
`parhelia-m9-shader-termination-boundary.md`.

## Association, allocation, and lifetime

GL stage identity is exact before the final bind. Private opcode `0x59` carries
flag 0 for fragment and 1 for vertex into distinct Linux backend objects at
device `+0x2560/+0x2550`; opcode `0x67` associates the varying map with the
fragment install result [BIN]. The compiler/cache vtable and native descriptor
copies terminate in separate top-state `+0xd8` resource/link state. Final
program emission uses top-state `+0xc8`. Constructor, vtable, copy, relocation,
and serializer audits find no edge joining the typed current handles to the
external program fields or assigning universal VS/FS names to `6a/6d` [BIN].

Inline upload copies the CPU native-image bytes into the ordered command stream
before the bind. The CPU compilation buffer can be released after command
construction, while the command BO and its restore/context state must remain
valid until the job fence retires [DER]. External `69/6c` resources and every
referenced backing object must stay pinned through that fence. Windows
`0xf9b70` attaches one submission value to both TaskMgr resource vectors before
submission; Linux `m9x954` attaches the returned submission value to referenced
backings before dropping their in-flight references [BIN]. Restore slots are
reused only after their availability/fence checks [BIN].

Program RAM must be treated as context state. The recovered Linux dirty-state
promotion and ordered A/B context switch re-emits program state through the
same queue, but no binary proves that inline program RAM survives reset or
another context's upload. Re-upload on context switch, reset generation, and
engine recovery is therefore the implementation-safe policy [DER].

## Remaining live gates

Only three hardware-facing questions remain for this transport [DER]:

1. Run one VS-only and one FS-only program swap while forcing inline transport;
   record `6a/6d` and the two inline starts to assign universal stage ownership
   or prove that a separate mode selector controls it.
2. Dump the ordered resolved table immediately before `0x1dc7d0` for one
   forward IF/ELSE, one backward loop, and one CALL/RET. This reveals the
   graph-target-to-native-descriptor placement rule without decoding the
   heterogeneous external record.
3. Mutate the terminal destination and final operation independently to decide
   selector-stop versus full-terminal-form semantics.

Until those are captured, use inline upload only for vendor-identical bounded
images whose two entry roles are known by construction, append the exact
terminal slot, keep all command/resources fenced, and leave arbitrary native
control flow behind a development flag.

Run `python3 tools/m9-shader-transport-closure-evidence.py` from the repository
root. The checker uses Python's standard library, executes no vendor code, and
pins the hashes, vtable, typed resolver, direct serializer, footprint table,
zero-slot append helper, and Linux inline bind.
