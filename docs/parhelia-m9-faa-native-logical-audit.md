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
The application caller is being corrected separately; the current private180
image already uses the source-derived9b04 and is unaffected by that bug.

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
surface admission/association/dirty-flush lifecycle. The genuine draw marks
its outer surface dirty; real public flush association must be traced rather
than assigning list/class/dirty fields or assuming an internal helper is valid.
The exercised device-global service/MMIO initialization matches native code
so far; the unclosed cold-device path does not justify speculative toggles.

## Validation

The actual native C clear/address/mask path passes128 placement/alignment cases
under ASAN/UBSAN. Mask controls reject altered target routing, active mask,
packet headers and short extent without changing data, and verify every other
secondary byte is unchanged. Source checks and the private module build pass.
No new hardware rendering pass is claimed for mask normalization.
