# M9 FAA private graph admission and execution gate

Status on 2026-10-06: the retained vendor frame joins real CPU allocation,
clear, binding, public DP2 triangle and resolve instructions, but does not yet
prove that the triangle renders through the same FAA graph. No executable
Linux indirect-pool admission follows from this trace.

## Concrete surface membership gap

The SHA256-pinned MSerX `MxMD.dll` activation entry `a77c0` accepts the actual
surface wrapper in EAX and no stack arguments. It reads the surface at
`wrapper+c0`, then the device at `surface+238`. The real caller `7d030` invokes
this entry at `7d098` when its CPU request bit `02` is set. This is a normal
surface activation caller, separate from a fragment-mode register write.

The activation entry performs these actual instructions:

| PC | Action |
| --- | --- |
| `a77e7` | Set `surface+428` bit `08`. |
| `a77fb` | Set `surface+270` to one. |
| `a780d` | Require the selected allocation profile's class to equal two. |
| `a7817` | Load the surface's metadata owner from `surface+420`. |
| `a7827` | Set metadata `+3c` admission flag to one. |
| `a7848` | Look up the actual backing object in the device registry. |
| `a785e` | Join an existing registry owner when one already exists. |
| `a787d` | Call `ac7e0` with EAX=`metadata+1c`, EDI=device and args `(surface,0)`. |
| `a7886` | Clear metadata `+3c` if allocation/initialization fails. |
| `a78cc` | Publish the initialized owner through the genuine registry insertion. |

The other actual `ac7e0` callers at `51ef2` and `b6a19` also initialize
`[surface+420]+1c`; they do not construct a detached scratch graph.

The frame helper retained as `b954cf25` instead allocates a separate 32-byte
graph, calls `ac7e0` directly, manually calls `aa750`, then executes a normal
public DP2 request. This omits surface membership and the normal admission
publisher. A plausible explanation for its later resource overwrite is that
the public context sees an ordinary surface owner. The corrected emulator
must execute `a77c0` or its actual normal caller and recover the graph from the
returned surface owner. Assigning the missing flags or graph fields in Python
would not prove the vendor publisher.

## Actual triangle overwrites in the retained frame

The frame's binding selects the genuine graph companion and its HZ. Its
triangle then writes different bindings before the operation:

| Register | FAA bind | Triangle | Meaning |
| --- | --- | --- | --- |
| `B8` | `01a51001` | `012a8801` | Companion address changes. |
| `B9` | `7a220140` | `b8002000` | Companion descriptor changes. |
| `BA` | `01ae9001` | `00000000` | Graph HZ address is removed. |
| `BB` | `f0010140` | `f0010040` | HZ descriptor changes. |

The fragment mode remains `1f`; checking mode alone misses all four changes.
The frame's main/auxiliary BC/BD and pool6 address can still be retained while
the depth/companion graph has already changed. The initial bind's association
with the exported graph is independently checked by
`tools/m9-faa-surface-admission-check.py`. Its `--require-coherent` option
requires the companion, HZ and their descriptors, main, auxiliary and pool6
bindings to survive the final pre-triangle lowering. The existing retained
frame fails this gate, as expected. Success would remain a CPU binding proof,
not a hardware image or a completion claim.

The triangle system-DMA image uses a distinct type2, count-prefixed software
representation. This audit reads those records as semantic register writes;
it does not copy them into the Linux count-free command stream. The actual
vendor submitting consumer and its packing rules must remain explicit.

## Smallest coherent Linux execution gate

The first gate is a private trusted diagnostic frame. It must cover
initialization/clear, retained bindings, GPU draw, resolve and an actual final
completion. The generic client FAA_POOL relocation remains non-executable;
generic `2e/2f` indirect execution and `1a` register stores remain refused.
The existing nine-word binding block is only one step in that full pipeline.

A private owner can reuse the existing kernel diagnostic allocation and
transport patterns. Allocate and pin its VRAM graph before entering client BO
WW reservation. Keep pool6 and the sentinel unexposed: no GEM handle, mmap,
SURFACE_ACCESS, PRIME export or client-provided physical address. Private BOs
can be accessed only by fixed kernel templates patched with checked offsets,
address tags and extents. Do not reuse the ordinary system restore pool as
this FAA context resource.

The exact pool6 allocation in the joined capture is `0x580` bytes although
the ordinary cache request is `0x500` bytes. Its actual fragment pair is
`+84/+88 = [0,GPU address|1]`. The sentinel pair is `+7c/+80` and uses an
indirect CPU record; its relocation resolves to the separate private VRAM
sentinel. The short-lived shifted `+88/+8c` exporter interpretation was
corrected and must not enter an implementation.

Actual sentinel initialization supplies five DWORDs:
`a4a3a1a1,28041968,0,0,0`. The first two are CPU-written program/header bytes;
the last three are initialized snapshot slots. The genuine context producer
stores A1, A3 and A4 to sentinel offsets `08/0c/10`, then restores the private
pool program range at `284..2c4` with the captured low tags. The sentinel's
first-use restore precedes the FAA graph bind. These are distinct programs
and lifetimes; their contents cannot be inferred from unrelated clear bytes.

Before executable construction, join an actual CPU write-validity mask for
pool6, or prove the exact GPU producer which initializes every later fetched
program byte and its ordering before the first restore. Untouched zero-filled
emulator backing is unknown. Mapping validity must follow the actual CPU BAR
alias; a raw GPU offset is insufficient if mapping introduces an offset.
Do not fill unknown program slots with zero or a guessed nop.

The owner must retain every referenced program/source operand plus color,
main, auxiliary, companion, optional companion HZ, pool6 and sentinel until
the completion **after resolve**. Intermediate triangle/E3 tokens are not a
release point for the graph. Subsequent context consumers need the graph's
same lifetime even if the first triangle has completed. No CPU rewrite or
replacement of the indirect records is allowed while those consumers remain
posted. A posted timeout quarantines all graph resources until observed
retirement or PCI DMA stop, using the current transport/fence contract.

Keep FAA queueing disabled during the first image proof. On teardown, stop
DMA before releasing unresolved resources, signal retained reservation fences
before the final TTM put, and release BOs outside the transport lock. These
are the ownership rules already proved for ordinary submissions; FAA cannot
bypass them merely because the command templates are trusted.

Only after the private frame produces an FAA image and retires correctly
should the reusable helpers gain a file-bound graph API. No placeholder graph
token, app ioctl or writable opaque-program BO is introduced in this round.
