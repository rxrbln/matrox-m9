# M9128 command pool reuse after an authenticated Unlock

Continuous FAA currently appends to retained primary/secondary pools. Their
capacity guard is correct, but exhaustion prevents sustained application
rendering. The original M9128 queue provides a rotation protocol, with a
separate CPU reference and two full64-bit retirement conditions. A completed
Unlock is necessary for prior draw inputs; it does not remove the active
queue's CPU reference or authorize resetting a cursor.

Original vtable1f2c60+a4 selects24060. Its primary path(q+10==0) scans slot
objects from q+a0..a4. Resource mapping/admission occurs at240c0..240e5. For
an already mapped ordinary slot(+50==0),240eb..24129 compares queue+120/+124
against backing+38/+3c. Then2412b..24174 compares queue+110/+114 against the
maximum of backing fence groups0(+28/+2c) and1(+30/+34). The backing getter
66970 selects this maximum using both words. Queue+78 getter25230 returns
cached+110/+114; this is not an MMIO-derived completion claim. Finally24176
requires slot+48==0. Other initialization/mapping states follow separate
branches and must not be silently equated to this already-mapped path.

After selection, original241cc calls slot vtable+4 (2c7e0), which increments
CPUrefs+48. It increments slot+54, advances platform's64-bit pool timeline
(+20/+24) by16, and writes this16-byte link at the OLD primary cursor:

| Word | Actual source expression |
| --- | --- |
|0|453e421a, source opcode at241f2|
|1|platform counter address+18 OR2|
|2|new logical timeline low OR1|
|3|selected slot GPU base+2c OR its selector+20|

At24231..24238 the old slot's fence-interface vtable+8 (2c740) forwards the
new64-bit point into its backing via66950 (+38/+3c). It retains the old CPU
toGPU translation in queue+40, decrements the old active slot's CPUrefs,
installs the selected slot as q+bc, sets q+4/+8 to the live selected CPU base,
and derives guard as selectedCPUbase+capacity-4*queueReservedWords. It updates
saved mode0 owners/cursors atq+c0..d0. The required original flush+7c(24d60)
at242ec follows. That flush closes/stamps pending owner ranges and emits/posts
its genuine completion protocol. The link alone is not a complete rotation.

The nonzero q+10 path is different:24319 temporarily retains the old slot,
24b10 closes the prior counted range into its parent, then24360..244ac scans a
size-qualified retired slot using the same64-bit conditions and refs. After
selection,244c1 calls24a20 to preserve/install the nested command context;
its source-generated relative/indirect ranges remain live. This is relevant
to secondary/code/geometry pools: resetting primary, secondary and upload
cursors together omits this mode stack and pending-owner closure.

The [source controls](../tests/m9-faa-pool-reuse-source-20261008/README)
execute original unchanged comparison and transition subregions on explicitly
synthetic CPU ABI owners.288 comparisons and3 transitions pass, including
32-bit wrap and an active-reference refusal. They pin original instructions,
vtable targets, reference-count helpers, full64 backing methods and primary
publication code. They supply no hardware counter, change no production code
and do not assert a complete original source caller or native rotation pixels.

A concrete native implementation should retain at least two primary owners
with independent last-use points and CPU/pending-reference state. At an
actual authenticated idle Unlock, select another capacity-compatible pool,
verify every queued/posted owner fence, construct the source link using its
live GPU address and generation-scoped full64 point, close pending ranges,
and publish the ordered END. Retain old BOs through the new rotation fence;
a failed posted transition must follow existing quarantine. Keep source pool
points separate from Lock/Unlock deltas until the complete flush/timeline
chain is lowered. U64 overflow must refuse; the source arithmetic alone does
not establish a safe new epoch or generation.

The exact next source boundary to close is the full24060→24d60→END chain on
a genuinely constructed retained queue, with all mapping/view publications
and nested24a20/24b10 owner fields exported. The known subregion checks remove
an ambiguity about retirement and active ownership; they do not permit a
blind rewind or fabricated complete rotation. Secondary/code reuse needs its
own complete producer and live owner joins. Hardware qualification should
force more than one pool switch and then return correct FAA pixels while the
same graph token/generation remains retained.

## Complete constructed-queue transition and completion distinction

The [complete rotation proof](../tests/m9-faa-pool-rotation-source-20261008/README)
now executes original24060 through its real return on a byte-equal reconstructed
INIT queue, with the authentic INIT receipt and opaque-read guard. The explicit
allocator call selects unused mapped pool4 after rejecting active pools0..3.
Four CPU stores append the16-byte link to the old primary; old/new refs change
1→0/0→1. Original24d60→24530→2461e posts the selected live GPU base as END.
It emits no new E3 epoch and performs no new allocation or mapping MMIO.

This closes the primary flush missing from the earlier subregion proof. It also
clarifies its terminology: queue+120/+124 are CPU-published pool points, not
observed hardware completion. Queue+110/+114 are cached source use/publication
state; neither full64 comparison substitutes for the preceding resource busy
queries. Resource55550→4e130 reads actual common lane0; resource555c0→4e250
reads actual pool lane+20. Original4e203/4e32d update their software wrap caches
and compare full64 needed points. The earlier synthetic fixture's variable
names describe supplied CPU test fields, not newly sampled GPU values.

The next original allocator call reaches4e32d needing pool20 and is stopped
before the load. The prior authentic pool10 does not establish this new point.
A global Lock/Unlock completion cannot simply be renamed a pool completion.
A later native ordered E3 point could establish retirement of a preceding pool
link only with explicit command-order and generation proof.

Native observation uses the already retained PAGE_SIZE coherent
`transport.fence_cpu` owner and its DMA `fence_bus`; the source pool link's
physical-tag2 destination derives from `fence_bus+0x20`. The existing FAA
counter_node is a class1 mapping of the same allocation, not a separate
completion owner. Read-only debugfs can sample DWORD8/9 as raw20/raw24 under the
existing device/transport lifetime guard. Raw24 is not asserted to be a GPU
high-word producer: source high64 tracking uses software wrap state. Such a
snapshot must never extend either completion domain or release an owner.

The next bounded planner may construct the proven four link words from live
old/new retained owners, aligned physical pool-counter address and checked
full64 pool origin. It must preserve old owner references through true observed
retirement and posted failure quarantine. This does not authorize secondary,
constant or code rewinds: their original nonzero-mode24b10/24a20 range and
reference protocol remains distinct.
