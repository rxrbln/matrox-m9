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
