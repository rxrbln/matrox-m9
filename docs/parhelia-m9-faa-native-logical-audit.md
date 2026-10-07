# M9128 FAA logical and stale-value audit, 2026-10-07

The actual complete180 stream retires without wedging but produces an all-black
image. Retirement is established; FAA rendering is not. This review examines
native production and the source contract rather than repeating that test.

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

## Confirmed application lifetime gap, fix pending

The store lock currently covers growth and handle snapshot separately, then
ends before emission and ioctl. There is no broader submission lock. The batch
retains integer GEM handles, not userspace ownership references; another context
can grow the shared group and close main/aux/companion before kernel lookup.
Companion reads also occur outside that lock. The required follow-up must keep
the group stable through every split-pass submission and all error exits;
kernel references then retain accepted work. The screen destructor also lacks
an explicit companion close (closing the final DRM file eventually releases
its handles). These are application lifetime defects, not an explanation for
the isolated private180 black image. The follow-up is not yet integrated.

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

The changed pool is a saved register image, not edge/fragment evidence. Its
A1/A3/A4 fields match the still-zero saved sentinel fields. The selected
source raster controls and dispatch exist, but fragment generation is unproven.
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

## Validation

The actual native C clear/address/mask path passes128 placement/alignment cases
under ASAN/UBSAN. Mask controls reject altered target routing, active mask,
packet headers and short extent without changing data, and verify every other
secondary byte is unchanged. Source checks and the private module build pass.
No new hardware rendering pass is claimed for mask normalization.
The integrated application AD/ownership fixtures pass ASAN/UBSAN and the full
Mesa DSO builds. Public lifecycle checks reject four false observations; the
global source checks reject four altered initialization variants. The confirmed
shared-store race above remains open; these passing tests do not cover concurrent
replacement between snapshot and submission.
