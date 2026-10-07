# First public draw after retained graph INIT

This separate source control reconstructs the sealed graph INIT exactly, joins
only its real M9128 completion receipt, then calls public DrawPrimitives2 on the
same graph and context. CREATEPS54, SETCONST57 and SETPS56 are real public
commands: pixel shader `MOV oC0,c0`, c0=(1,0,0,1). The original triangle remains
suspended at its first new 252b9/common00 wait for 0x100. No draw receipt or
pixels have been supplied. This is not the earlier diagnostic180 flow.

The qualified INIT receipt is the confirmed-retirement archive under
`tests/m9-faa-graph-init-hardware-20261007/`: INIT/independent WAIT_POINT both
returned b0; the actual four counters are b1/0,10/0,10/0,10/0. The controller
pins the exact INIT plan and archived file hashes. It rejects all new deadlines
not covered by that receipt. GPU-owned BAR and mapped MDL bytes require a
source CPU write; newly constructed local shader owners join this guard before
CPU mapping. No raw hardware buffer is imported.

The actual first draw sequence is:

1. Retain all six borrowed graph owners, existing private context/pool and
   initialized virtual-primary transport. The independent INIT finalizer is
   not resumed through its resource destruction after retirement.
2. Source PTE publication precedes program access. Real 2cb40 takes the newly
   allocated program resource, arguments (resource,0,1). 2cba8 writes 314=1;
   the CPU-root publication writes 31c=f,0,c. The program has two exact alias
   pages, allocation extent1080 and240 source-written bytes. All60 DWORD stores
   at1e5383 occur after map and before release2c878/31c=0 and preprocessing.
3. Public draw launches preprocessing at17e480. Constants/declaration source
   storage is sparse136B at0 plus188B at256. The secondary grows to648B;
   the independently emitted finish extends it to664B. C2/C3 both reference
   the same live program owner at+32. Neither a +0 program entry nor raster
   completion is inferred.
4. Actual source posts END at primary source offset4944 twice, at2461e then
   b55af. Source CPU-known primary extends to4956 at the original100 wait,
   but those last12 bytes are not included in either observed END. Do not
   synchronously wait for100 after the4944 post: the source wait is unread.
5. A separate genuine23b80 finish emits another12 primary bytes and posts4968,
   with independently produced target110. Its23bc3 status read is stopped
   before execution. This instrumented finish is explicitly distinct from the
   suspended original100 caller, and must not be treated as a returned draw.

The derived plan has31 owners,20 address fixups,19 timeline fields and123604
source-known bytes. Primary27 contains4968B; program26 adds240B; sparse
constant/geometry28 adds324B; secondary29 adds664B. The old INIT primary26
maps to post-INIT primary27, and startup27 maps to startup30. Every earlier
owner maps by retained backing identity, and every original fixup/timeline
slot is preserved. The first4880 primary source bytes are exact, including
INIT's b1 footer. All previously CPU-written spans remain unchanged; the only
changed prior resource descriptor word is its observed last-use value0→d0
at backing+30, with no allocation/layout change.

A native continuation must borrow the existing binding and append, rather
than prepare/submit this whole plan as another INIT. Keep origin10; do not
reseed counters or rebase existing/new tokens from next_fence c0. Program
upload/mapping occurs after actual INIT retirement. The owner map and
`post_init_upload_spans` identify new uploads; never upload old sentinel or
context images again. Retain all new owners after any post, including timeout
or partial-publication failure.

The companion clear is placement-dependent. If retained native INIT consumed
4884B while source INIT consumed4880B, its live delta is+4. Append88B at the
retained runtime primary_bytes; source END4944/4968 become live4948/4972.
For any supported placement derive the delta from the retained object, then
translate only later primary offsets/END values. Preserve the already posted
prefix. The actual native C fixture checks this rule at two placements and
eight alignments; the source checker preserves original fixups/trackers under
the owner-index mapping. This CPU encoder proof is not a real draw receipt.

Generated artifacts are in `captures/m9-faa-graph-post-init-20261007/`:
`draw.json`, `manifest.json`, `addresses.json`, `native-plan.json`,
`native-blobs.bin`, `m9_faa_graph_post_init_data.h`. Captured addresses and CPU
aliases in these forensic records are not executable native inputs. The plan's
fixups and owner map must derive every executable address from retained owners.

Reproduce host proof from the repository root:

```sh
PYTHONPATH=tools/faaemu:tools/m9emu /home/ai/tmp/faaemu/venv/bin/python \
 tests/m9-faa-graph-post-init-20261007/check.py
M9_FAA_NATIVE_SOURCE=/absolute/path/to/m9_faa_native.c \
 sh tests/m9-faa-graph-post-init-20261007/check-native.sh
```

The first command also refuses a forged receipt, opaque GPU input, false draw
return, altered INIT prefix, invented final deadline, changed program writer,
wrong direct address domain and reordered program release. Original INIT
plan/blob/header regenerate byte-exactly. The pending110 receipt is the next
hardware qualification; original100 continuation and later raster/Lock/resolve
still require their genuine caller and input contracts. No application image,
public presentation or generic shader support is established by this prefix.
