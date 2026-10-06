# Actual unfinished ordinary FAA output inputs

The signed `normal-faa-output-input-manifest-101f8.json` checkpoint replaces no
baseline template. It follows the real internal `ac1f0` read request against
the retained outer FAA owner triple. It is an explicit diagnostic caller below
the public copy admission gate. The original call executes real allocation,
`abc20` resolve dispatch, `ac0e3/ac139` lifetime bookkeeping and `afbe0` CPU-copy
entry. A passive pre-read observer stops before the first actual live common
buffer load; it supplies neither that load's value nor pixels or completion.

The accompanying source and contract JSON files retain actual vendor
instruction pins, input hashes, MDL ownership/PFNs, prior metadata writers,
CPU-write chronology and the copy/address-builder contract. The producer is
`m9_faa_output_manifest.py`, wrapping the render agent's
`tools/m9-vendor-faa-read-contract.py`. Pass the latter's actual-input arguments
plus `--contract-driver`, `--manifest-out`, `--mm-flags 0x101f8` and
`--continue-to-gpu-input`. The independent checker is:

```
tools/m9emu/check-faa-output-manifest.py MANIFEST --source-capture SOURCE --contract CONTRACT
```

There are 90 actual typed relocation stores, 35 live raw NT MDLs, 34 mapped raw
prefixes and one separately retained counter PFN proxy. Actual guest CPU
writes establish 7268 bytes: primary 5584, secondary 1376, constants 120 and
geometry 188. These are the unfinished output stream's actual bytes, not the
old 92-operand / 5664-byte primary. The unchanged source programs comprise 240
bytes / 30 slots, SHA256
`2efd8e0bb9c16ca8bdc67ddec0990bf995b7ef329d09ae8741b6a76324cb66ec`;
the database comprises 115160 bytes, SHA256
`af36245806515bd7b0e38a986586d33611a84ed76dae845a42ce8776ba89f174`.
Existing CPU maps, exact PTEs and sparse write validity establish those source
bytes. Unwritten tails and all GPU-produced inputs remain unknown.

The genuine temporary resource has allocation kind 1, profile class 2,
logical format 6 and hardware-format field **100 decimal / 0x64**. The actual
layout is 320 by 256, with a 320 by 240 read rectangle and extent 327680. The
actual destination selector is `[GPUVA|1,c8210140,100f0]`. Its 80-page mapped
MDL prefix uses descriptor flags `30`, compact PTE nibble `b`, and produced
companion metadata `[0,80,0]`. No destination pixel byte is source-known. The
native producer must derive the selector, dimensions, extent, allocation and
mapping from its own retained typed resource; these historical values are
bounded diagnostic observations, not portable addresses or parameters.

`system_owner_mapping_policies` records every observed mapped owner. Queue,
geometry and the temporary output use GPUvirtual addresses, tag 1, flags `30`
and PTE nibble `b`. The separate counter proxy uses flags `110`, nibble `a`
and only the actual 159-byte common-buffer subspan has raw backing ownership.
Replacing a GPUvirtual address with physical DMA while retaining tag 1 is
unsupported. The original baseline's tag-2 physical queue/geometry domain
cannot be transplanted into this fresh stream. The capture selects no native
owner or replay domain and invents no contiguous alias.

The original copy reaches `4e2fc`, `mov ecx,[ecx]`, reading common-buffer offset
`20`. The observer records the actual owner, virtual/physical offset,
registers and instruction before executing the read. No `55550`, `4e130` or
`23bc3` event precedes this boundary in the retained copy trace. The exported
primary remains 5584 bytes, and no final retirement capture or successful
return is supplied. A separate explicit queue-finish caller may establish a
private submission boundary, but would not establish the original copy's
return or the public API contract. All owners remain live; the temporary's
final release has not executed.

The public admission gate is separately pinned by
`normal-faa-output-admission-source.json` and
`check-faa-output-admission.py CAPTURE --normal-manifest MANIFEST`. `51e0c`
sets resource `+3fc` only after profile class 4, profile formats `31/32` and
request bit `20000000` are admitted. It calls `ac6a0` on embedded owner
`+3e8`. `a74b0` additionally requires operation `+28` and this `+3fc` flag
before calling `ac1f0` on that embedded owner. The retained normal primary's
profile class is 2 and profile logical-format field is 0; its outer `+420`
FAA graph is a distinct object. The static public callers `92970/92c09` and
`94140/94413` establish operation-field producers, not their runtime admission.
Neither `+3fc` nor any other internal state is changed to admit this case.

A distinct actual registry `101b8` internal-output control returned null from
the ordinary allocation at `ac2a0`. Its failure branch subsequently entered a
64-by-64 helper cleanup resolve and reached a separate missing common-buffer
input at `252b9`. The strict observer supplied no value. No ordinary output
owner, address domain or original API return is proved by that control; its
failure cannot justify mixing baseline addresses into the successful `101f8`
capture. The existing complete normal `101b8` input exporter was regenerated
after factoring its shared read-only observers: the resulting JSON object is
exactly equal to the original retained baseline.
