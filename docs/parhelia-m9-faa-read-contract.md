# M9128 FAA output destination and CPU read boundary

The joined frame originally supplied the sampled primary directly to the
explicit `abc20` output helper. Hardware retirement of that stream does not
prove that this destination contains ordinary pixels. The vendor has a
separate allocated-output contract. The retained
[101f8 helper capture](../captures/m9128-device-inputs-20261006/normal-faa-read-contract-101f8.json)
executes actual `ac1f0` after the genuine normal triangle and **before** any
manual sampled-primary resolve.

The explicit CPU caller supplies the actual retained outer owner plus four
bytes: its next three fields identify primary, companion and main. It supplies
source/destination rectangles derived from the admitted 320×240 geometry,
a 1280-byte CPU destination pitch and an owned CPU output buffer. This is a
bounded internal helper request. It does not mutate resource admission flags.

Actual `ac1f0` constructs a flags1/kind6 allocation request through `51870`.
Its profile is read from the live device `+69a8` plus `818` (profile14).
The returned resource is **system-backed**, allocation kind1, logical format6,
profile class2, hardware format100. Its layout has kind1, requested320×240,
padded320×256 and extent50000. The actual selector9 descriptor is the returned
GPUVA tagged1, `c8210140`, `100f0`. These are source results for this bounded
shape and format, not permission to replay a captured GPUVA or substitute a
local allocation with another descriptor.

At `ac2d4`, actual `abc20` receives that returned destination, retained
companion and main, and the rectangle. Actual dispatch `ac0e3` and backing
lifetime bookkeeping `ac139` execute. The final AA destination shadow is the
new system owner and its actual descriptor; fragment F0/EF/D7 remain
`e0000400/20007000/0`. The destination stays owned because execution stops at
actual `afbe0` entry, before CPU copy, waits, pixel access or release. No GPU
completion or counter reply is provided.

`a74b0→ac1f0` is a conditional public copy branch. The caller boolean and
resource `+3fc` must admit it. Source investigation shows the ordinary sampled
primary does not meet that separate resource-profile/request admission; this
capture therefore does **not** claim to have entered through that public
branch. It proves the allocator/output helper contract needed for a private
native diagnostic. Ordinary wrapper Lock is also distinct: actual
`a8a60→a66c0→a8d60→55850` sees primary `+3d0=1`, copies through `57d90/48c60`
to its retained `+3c8` staging resource, then maps the staging resource and
waits. The earlier direct `2cb40(sampled primary)` host mapper proof did not
cover this chain.

A native implementation must retain a separate system-backed destination,
reserve a live class1 GPUVA and publish all of its PTE/companion metadata
before submission. Draw still uses the sampled primary. Resolve uses the
actual admitted ordinary destination descriptor and owner. The native stream
and indirect sources must be compared against this actual output capture,
including typed operands, timeline, clear page runs and command lengths.
Reading linear DMA bytes as pixels is not yet proved: the subsequent actual
`afbe0` CPU copy/translation contract remains to be traced up to its first
missing GPU input. Correct hardware retirement and rendered pixels remain
separate qualifications.

`tools/m9-vendor-faa-read-contract.py` runs the source helper against retained
OS/MDL owners with strict stops at the first wait or CPU copy. It requires the
full-frame MDL bridge and scatter observer as explicit tool paths.
`tools/check-faa-read-contract.py` verifies the retained owner relationships,
allocation request, returned format/layout, exact stage sequence and boundary.
