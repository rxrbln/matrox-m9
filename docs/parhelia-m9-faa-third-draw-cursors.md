# M9128 retained graph: third DRAW cursor boundary

The [compact source trace](../captures/m9-faa-third-frame-20261007/source-prefix.json)
continues the original **M9128** `MSerX/MxMD.dll` (SHA-256
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`)
after the second normal Lock returns under the actual 0x240 receipt. It admits
the separate actual second Unlock 0x260 receipt (boot
`0472bb3e-5c0f-487f-a2ce-522b4aac0567`), calls the genuine second public
Unlock, then calls a third public DRAW on the same
retained context with changed XY geometry and green diffuse input. The
fragment program remains constant red. The independent Unlock finish stops
before an unread GPU counter desired 0x260. The third DRAW itself stops at
the original `0x252b9` read desired 0x2a0. Neither counter nor pixels were
supplied by the emulator. This trace qualifies CPU-produced prefix structure
only. The full local source trace SHA-256 is
`d71a4115182eb0534cc637729dd0e401c5a3486fe7299b72a382faaf8ed949ac`.

The source writes are contiguous in three retained owners:

| Role | Prior cursor | Third cursor | Bytes now known | Next source cursor |
| --- | ---: | ---: | ---: | ---: |
| primary command owner 27 | 12344 | 12344 | 12 | 12356 |
| secondary command owner 29 | 2748 | 2748 | 652 | 3400 |
| constants/geometry owner 28 | 544 | 544 | 60 | 624 |

The preceding second DRAW source used secondary `[1372,2040)` (668 bytes)
and geometry `[464,524)`; its queued geometry allocator advanced by 80 bytes
to 544. The third source uses the same first 652 template bytes at its new
secondary cursor, after deriving 14 typed relocation slots and four timeline
tokens from live retained owners and the new Unlock epoch. Source kind 27
references geometry at 544, an 80-byte advance; other referenced owner
offsets are unchanged. The first 12 primary bytes form an indirect packet
whose endpoints reference secondary `[2748,3356)`. The source has written no
third-DRAW END by the unread 0x2a0 boundary. The omitted 16-byte secondary
tail and second primary indirect packet of the prior complete DRAW must not
be treated as already produced or posted in this third DRAW.

The original DLL's `0x17e480` writes the indirect tuple to the current
primary pointer and adds 12 to it. At `0x176eaf` it advances the geometry
cursor by `4 * queue[0xdc]`; the observed value is 80. The command queue's
`0x24a20` path checks `write_cursor + 0x30 <= guard` and calls a rotation
callback when it fails. In this retained source state, secondary write cursor
is base+3400, indirect endpoint base+3356, and guard base+0x3fe00 for a
0x40000-byte owner. These facts justify a **bounded no-wrap path** with live
capacity checks. The source callback's rotation and reuse synchronization
are not yet qualified; a native continuous-frame producer must refuse when
the next packet would cross the guard until those behaviors are derived.

The [checker](../tests/m9-faa-next-frame-20261007/check-third-draw-prefix.py)
compares every fixed source word with the earlier typed second-DRAW template,
verifies the pinned 0x260 receipt, live fields and cursor changes, and rejects
ten mutations.
For native use, every owner address, tagged endpoint, geometry input, extent,
and timeline word must be regenerated from retained live state as required by
[the capture policy](gpu-capture-replay-policy.md). The source trace's guest
addresses are evidence only.

Run:

```sh
python3 tests/m9-faa-next-frame-20261007/check-third-draw-prefix.py \
  --source captures/m9-faa-third-frame-20261007/source-prefix.json
```
