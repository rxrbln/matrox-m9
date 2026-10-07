# Original FAA output write post after the qualified160 prefix

This bounded source experiment resumes the retained original `ac1f0` stack,
not a new resolve caller. The actual160 receipt comes from
`tests/m9-faa-preclear-output-hardware-20261007`: common+00 is161/0 and
+20/+40/+60 are10/0. The full source manifest, prior live owners, MDL/PFN
joins, stack and already-consumed CPU source snapshots match before resuming.
The only supplied GPU values are three recorded counter reads:252b9(+00,
requested150),4e2fc(+20,requested0),4e32d(+20,requested0). No pixel or opaque
GPU buffer input is supplied.

The real `abc20` returns1 through `ac139` and `ac2d9`. Original `afbe0`
prepares its retained temporary output: ECX+4 names that resource, while
ECX+38 is a separate copy field. `aff90` enters `2cb40` with actual arguments
(resource,0,1). This is allocationkind1/logicalformat6/hardwareformat64,
320x240,extent50000,descriptor9=[liveGPU|1,c8210140,100f0]. Its flags30,
80-page SYSTEM backing and unknown pixel bytes are preserved.

Original write preparation itself calls25370 twice,24d60,24530 and2461e.
It appends500 source bytes to the earlier5224B stream and posts the complete
5724B primary, whose own last12B are E3/1/181. There is no independent finish
or fabricated footer. Execution stops before the actual `4e1d1` common+00
read requesting180: the160 receipt cannot satisfy it.

The current live kind1 mapper/vtable/arguments and exact mapper/writewait
instructions join the previously qualified
`normal-faa-output-receipt-continuation-public-red.json`. That original
postwait source path has no new command-owner writes and preserves every
command-owner hash. Its real next GPU action is2cba8 MMIO314=1 before kind1
CPU mapping/pixelcopy; later31c writes are actual cleanup/publication.
The existing native private output reader already retains that314 ordering.
This join permits OUTPUT_PENDING=0 in the independent native180 image test;
it does **not** claim this new source call has returned, that its180 wait has
completed, or that FAA pixels are correct.

The complete profile has31 owners,90 typed operands,52 non-null/direct
fixups,27 timelines,six direct endpoint fields and125052 source-known upload
bytes. Both source clear segments retain actual151 and80 logical pages;
native encoders regenerate their variable words from live owners. All prior
consumed LOCAL and SYSTEM known spans are unchanged across the160 boundary,
so this profile preserves their temporal source inputs rather than merging
changed snapshots. Output pixels stay entirely unknown/unuploaded.

`m9_faa_retained_read_guard.py` keys new CPU-known BAR writes by live owner
index and owner-relative offset, so changing a CPUroot mapping cannot carry
knownness to a different owner. Every opaque GPU-owned BAR read is refused,
even when a prior raw receipt exists; interpreting such a receipt would need
consumer ABI and pointer-domain translation. A prior receipt does not qualify
later GPU writes. The continued flow executes zero such reads, and rejects
unknown mapped MDL bytes. No raw pool pointer is transported to the emulator.

Reproduce from the RE worktree:

```sh
sh tests/m9-faa-preclear-output-write-source-20261007/capture.sh
sh tests/m9-faa-preclear-output-write-source-20261007/check.sh
```

The source checker joins all CPU producer writes, PFNs, domains, timelines,
original post and previous receipts. The host proof exercises actual vendor
clear instructions at two placements and all8×8 clear alignments, preserves
all unpatched sparse bytes and unknown output, and rejects seven malformed
source/count/owner/retirement controls. Native C and physical pixels are
separate qualifications. Existing init/post/preclear profiles remain strict.
