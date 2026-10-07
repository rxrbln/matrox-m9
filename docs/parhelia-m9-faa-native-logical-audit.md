# M9128 FAA logical and stale-value audit, 2026-10-07

The actual complete180 stream retires without wedging but produces an all-black
image. Retirement is established; FAA rendering is not. This review examines
native production and the source contract rather than repeating that test.

## Follow-up source review: current native and application paths

Reviewed kernel3b39c98 plus the uncommitted in-place-copy profile, and Mesa
dbb76c8. No new literal-address defect was found in the inspected native
fixup, clear rebuild, borrowed-owner or timeline paths. This is a bounded
review, not a claim that every required vendor transition is implemented.

The review identified these gaps (subsequent fixes are recorded below):

- `m9_faa_program_access` and its callback are compiled only for the complete
  output-write profile. Preparation refuses its option for both in-place
  profiles. The original public draw and c0/120 prefixes are unchanged in
  those profiles, so the source-backed C2 access kick also applies there.
  This is an omitted available ordering control; the older complete-profile
  hardware test already proves that adding this kick alone does not fix the
  black image. CPU-aperture side effects remain a separate contract.
- Mesa `m9_context_destroy` calls `m9_faa_store_report` before flushing its
  draw context and waiting for its last queued point. The report scans GPU
  storage and then clears the screen-wide `store_check` flag. Consequently,
  an early context teardown can record incomplete writes and suppress a
  later report. The userspace store lock prevents handle replacement, but
  does not wait for GPU retirement. A correct reporting fix must also cover
  other contexts sharing the store and the atexit path; moving the call
  below one context's wait alone would not close that contract. This affects
  application diagnostic evidence, not the independently retired private
  hardware observations or a demonstrated rendering failure.
- The raw Mesa probe still calls `m9_faa_ad` with color allocation bytes
  when `M9X_FAA=1` and `M9X_FAA_STORE=0`. Its old comment also claims color
  bytes are the vendor extent. The bound-store path overrides this using
  the retained logical main-store extent; the raw path does not. This is
  reachable diagnostic behavior, not evidence of complete application FAA.
- `m9_faa_surface_prepare` allocates the prepared graph, but production
  context code never assigns `job.faa.graph` or marks the graph initialized.
  The initialized graph, pool integration and resolve/publication must still
  be connected before preparation can become an application FAA path.
- Companion probe formats other than0x3d still use `width*height*8` storage
  and an ordinary depth row-span descriptor. Only0x3d takes the source-backed
  companion layout. The other raw-probe format/extent combinations remain
  unqualified; this is not a demonstrated failure of the private0x3d flow.

Follow-up implementation: kerneld249a3e extends the C2 access control to both
in-place profiles; its new copy-wait hardware run logs the kick atc0 and
retires1a0. Mesa776e5bf fixes shared-store reporting with per-owner last-use
tracking and a retirement wait under the owner lock, covering split submits,
multiple contexts and atexit. Failed submission refuses CPU inspection;
failed wait leaves the report pending. Host checks/full DSO build pass.
Neither correction establishes an FAA image.

A further native ownership defect was found after that run. Successful
pending-output profiles kept only the borrowed color address after the
submit caller dropped its pin/lookup reference and the helper closed its
handle. Complete-output profiles kept an extra identity reference but not
the caller pin. Kernel9dee327 now transfers the existing caller pin/reference
on successful FAA submission and releases it outside the transport lock only
after DMA shutdown and cache restoration. Posted-failure quarantine retains
its existing separate path. Inside-submit observations remain valid; older
post-exit raw physical dumps are byte evidence, not proof of a retained color
BO. Actual release/caller ownership checks, peer review and module build pass;
this fix awaits the next hardware candidate rather than repeating the same
rendering test solely for unchanged command bytes.

The strongest unresolved rendering question remains the missing public
dirty-surface lifecycle before the manually invoked internal resolve. A
correctly relocated command sequence below that boundary does not establish
that the selected operation is a complete public FAA resolve.

The source continuation after170 preserves all previously known CPU bytes;
its extra primary commands append rather than overwrite earlier shader,
geometry or metadata inputs. This rules out that specific upfront-upload
hazard, not GPU-state or MMIO-order differences. The uncommitted copy profile
still needs its own integration checks and hardware receipt; its final1a0 is
not a rendering success. The actual phased requester and program-access
guard fixtures pass ASAN/UBSAN in this review. No GPU test was repeated.

Constants need provenance, not indiscriminate removal. The sentinel filler
`0x28041968` is an actual vendor immediate, not a captured pointer. Fixed
320x240 geometry, descriptor words and output50000 remain confined to the
private source experiment. Live address operands, page masks, clear lengths
and timeline origins must continue to be derived independently.

## Confirmed stale values

The private second raster block has one active color target: indexed89 is
fe207ff8 and AB low16 isffff. Its inactive AB upper16 and AC contain captured
stack residue; AC differs282554d6 versus2824fad6 in the passing ordinary source.
These are packed color masks, not GPU addresses. The existing Windows1389a0 /
181fa0..182549 interpretation and first-triangle stream audit establish their
per-target halfwords. Replaying inactive stack residue is unnecessary and
nonportable. The native producer now normalizes only these three inactive
halfwords toffff, preserving the active mask and every other byte.

The generated header carries source-validated owner/offset metadata. Runtime
checks packet framing, complete extents, one-target89 and active AB before any
write; a different target layout or active mask refuses. The source blob stays
byte-identical. This is a semantic rewrite, separate from address relocation.
It removes a stale value but is not a sufficient explanation of the black image:
the active target mask already enables all four channels.

The older application probe computes AD from color allocation bytes, although
the current vendor graph uses logical fragment-store extent divided by32.
The full prepared graph's m9_faa_store_ad helper already has the correct rule.
Mesa2c01eb9 corrects the bound application caller: main/aux allocations use
their128-rounded logical extent, and AD comes from the live group's dimensions
under its store lock. Missing or undersized owners refuse submission; explicit
diagnostic overrides remain explicit. The current private180 image already
uses the source-derived9b04 and is unaffected by that bug.

## Corrected application lifetime gap

The store lock previously covered growth and handle snapshot separately, then
ended before emission and ioctl. There was no broader submission lock. The batch
retains integer GEM handles, not userspace ownership references; another context
could grow the shared group and close main/aux/companion before kernel lookup.
Companion reads also occurred outside that lock. Mesa dbb76c8 now keeps the group
stable from growth through binding, emission and every split-pass submission,
releasing once on all common success/error paths. Kernel references retain
accepted work after unlocking. FAA-off draws do not acquire this lock. The
screen destructor explicitly closes the companion and clears its diagnostic
atexit pointer; diagnostic map reads use the same store lock. These application
lifetime defects do not explain the isolated private180 black image.

## Deliberately bounded diagnostic constants

The private profile's320x240 geometry, output extent50000, format descriptors
and packet offsets remain deliberately fixed. They describe a guarded source
experiment, not arbitrary application FAA support. Address fixups, page clears
and timelines derive from native owners; retaining a source packet offset is
not permission to retain its captured address. The one-target mask rewrite
likewise refuses a different routing or active mask rather than guessing.

## Reviewed mechanisms that agree

- Page clears are rebuilt from live owner bases and logical clear lengths;
  both suffix shifts run backwards after address/timeline fixups. Captured
  page masks are verified as source evidence, then replaced, not replayed.
- Main raw fragment extent136020 rounds independently to136080 at graph
  allocation. The apparent32-byte versus128-byte discrepancy is not a bug.
  Companion clear length97000 is distinct from backing capacity98000.
- Borrowed color admission checks the pinned BO address and extent; source
  SYSTEM allocations retain separately derived GPU mappings and cache policy.
- Constant-red shader bytes and constants equal the passing ordinary source.
  Green preprocessing colors do not determine this constant shader's output.
- The original180 continuation performs only the frontend314 kick before
  CPU copy. No additional omitted post-wait raster/resolve commands appear.
- Full secondary decoding exposes remaining last-explicit-word differences
  in source-owned addresses, FAA graph/mode/extent, sentinel access and inactive
  masks. These words are not a readback of GPU state: indirect restores can
  also change registers, so this comparison is not a pixel proof.

## Remaining causal gaps

The pool is a saved register image, not independently decoded fragment data.
With phased submission, pool A1 and sentinel+8 are now both0x24a; A3/A4 remain
zero. Main and companion stores also receive writes. This establishes GPU
activity, but neither decodes those records nor proves correct FAA pixels.
The manually invoked ac1f0 output flow is still below the unjoined public
surface read/dirty-flush lifecycle. The association itself is present: actual
101050 inserts its node and both returned interfaces publish the dirty outer
graph before the draw returns. The earlier empty-list observation was only
the first lookup, not the state at output. The public embedded graph at+3fc
and this separate outer graph at+420 must not be conflated. No public flush
executes before the explicit internal output recipe; this is a remaining
contract gap, not proof that adding a flush fixes the black image. See
[the passive lifecycle proof](parhelia-m9-faa-public-draw-ownership.md).
The exercised device-global service/MMIO initialization matches native code
so far; the unclosed cold-device path does not justify speculative toggles.
The independent [global initialization audit](parhelia-m9-faa-global-service-prefix.md)
checks all13 exercised prefix writes and manager-before-prefetch ordering.
The [CPU chronology audit](parhelia-m9-faa-temporal-input-audit.md) proves all
earlier known inputs survive in the final image; it does not prove equivalent
ordering of GPU writes or intervening mapping operations. Kernel8d95847 adds an
optional phased-post diagnostic to isolate collapsing source c0/120/160/180
waits. It retains the same command bytes and actual retirement requirements.
The [fresh-boot same-module comparison](../tests/m9-faa-mask-control-hardware-20261007/README.md)
now demonstrates a real ordering effect: phased submission produces601 main
and1404 companion nonzero words, while a single post leaves both zero. The
final output stays black in both. Preserving waits is the default for this
complete diagnostic; later output-mapping publication and resolve remain open.
The previous nine BAR dumps omit the borrowed sampled primary (owner1), which
has no privately owned vram/cpu field. Kernel98dbe6a adds a bounded read-only
observation of the actual pinned/reserved caller BO before submit and after
retirement. It keeps borrowed ownership separate and never invents a zero
baseline after a mapping failure. The retained-store checker accepts this
optional paired observation only when its address matches the submitted color.
The subsequent fresh-boot d76d07d run observes that surface changing from zero
to1308 nonzero words/2819 bytes, FNV04099694. These are retained sampled-surface
words, not a decoded pixel proof.

Kernel d76d07d adds an opt-in test of the remaining output publication order:
reserve the live output address without staging its binding, then publish its
compact and secondary mappings after actual120 retirement, before the160
suffix. The source first references that output at byte5264, beyond the120
prefix endpoint5068. Host tests exercise the actual helpers, including failure
after a posted prefix; the integrated module builds. The
[hardware run](../tests/m9-faa-late-output-hardware-20261007/README.md)
confirms publication after actual120 but still produces an all-zero output;
main and companion hashes exactly match the earlier phased run. Later output
publication alone is insufficient. DMA allocation still happens early, and the native120
wait is stronger than the original CPU deadline110. The intervening C2 CPU
aperture mapping is another remaining chronology difference.

The [resolve resource-role proof](parhelia-m9-faa-resolve-resource-roles.md)
also rules out a simple reversed explicit destination: the successful vendor
path passes the SYSTEM temporary as destination and companion/main as inputs.
The sampled primary becomes the destination in the separate allocation-failure
branch. No explicit primary-to-output copy is justified by this evidence;
opaque indirect color references remain unproven.

The [complete address-encoding audit](parhelia-m9-faa-complete-address-encoding.md)
matches all52 relocations and31 owner extents against the source plan, and
compares the actual native encoder with shipping vendor instructions in310
cases. Two distinct native placements and packed low-bit controls agree,
including paired C2/C3 reconstruction. No additional stale-address or
discarded-bit defect was found in this admitted profile. This does not prove
that the captured flow includes every required lifecycle transition.

## Validation

The actual native C clear/address/mask path passes128 placement/alignment cases
under ASAN/UBSAN. Mask controls reject altered target routing, active mask,
packet headers and short extent without changing data, and verify every other
secondary byte is unchanged. Source checks and the private module build pass.
No new hardware rendering pass is claimed for mask normalization.
The integrated application AD/ownership fixtures pass ASAN/UBSAN and the full
Mesa DSO builds. Public lifecycle checks reject four false observations; the
global source checks reject four altered initialization variants. A threaded
actual-allocation fixture proves competing replacement remains blocked through
emission and both submission passes, including refusal and partial-pass failure;
the integrated Mesa DSO builds. The phased requester independently passes
ASAN/UBSAN timeout, late-endpoint, PTE, placement and clear-length controls.
