.. SPDX-License-Identifier: GPL-2.0-only

M9/Phoenix clean-room render bring-up
=====================================

The M9 backend is the independent ``m9x_drm.ko`` module.  Device ``102b:0540``
is absent from ``parhelia.ko`` and is the sole PCI ID claimed by this
render-only driver.  The module does not link against or call the Parhelia KMS,
host-aperture, WARP, packet validator, submit or reset code.  Its DRM driver
name is ``m9x`` and Mesa selects the independent M9X Gallium driver and
``m9x_dri.so`` from that name.

Safety gates
------------

``m9_enable=1`` is required before the PCI driver binds M9.  With that gate
alone the driver maps the BARs, enforces a coherent 32-bit DMA mask, allocates
kernel-owned requester/command/fence memory, and exposes DMA32 GEM plus a
validate-only command ioctl.  It leaves PCI bus mastering disabled and does
not write requester registers until the separately gated diagnostic runs.

``m9_render_diag=1`` additionally permits ``M9_SUBMIT(NOOP_FENCE)``.  This is
the sole hardware submission accepted by this revision.  It builds the known
``{ 4545e3e6, 3, target|1 }`` completion packet in kernel-owned DMA memory,
programs only the statically closed command and fence addresses, rings the
``0x404`` endpoint, and waits with an absolute deadline.  A disconnect or
timeout wedges render submission.  The coherent allocations remain reachable
until teardown disables PCI bus mastering.

The ``e3`` packet operand writes through the address bound at MMIO ``0x106c``.
That register points at writeback word 0, which the diagnostic polls;
``0x1068`` points independently at word 1.  The driver enables PCI bus
mastering before publishing either address.  On timeout it logs both
writeback words plus engine, front-end, command and fence-address registers.

Before enabling bus mastering, the guarded M9148 path now requires warm
primary partition controls ``0x44c/0x450/0x454`` to equal the independently
recovered and live-verified fallback image
``00280800/1f3f2707/00070703``.  It preserves those values rather than writing
them.  A different board/property tuple fails closed until its seven inputs or
packed controls have their own evidence.

The primary stream uses two kernel-owned 32-KiB command slots.  Once a slot's
last synchronous fence has retired, the producer advances to the alternate
slot, rebases ``0x400`` to its tagged start, and begins again at offset zero.
It never rewinds the currently published interval in place.  The auxiliary
``0x500..0x524`` requester page is allocated but not published.
The captured vendor GL stream leaves that descriptor dormant and forced-wrap
behavior has not passed its hardware gate.  The five hardware-owned words are
therefore neither cleared nor assigned invented meanings.

UAPI boundary
-------------

``M9_CREATE_BO`` creates an immutable DMA32 object and returns a GEM handle and
DRM mmap cookie.  No bus address is returned.  Command objects are explicitly
marked with ``PARHELIA_M9_BO_COMMAND``.

``M9_SUBMIT(VALIDATE_ONLY)`` copies a command slice from such an object.  It
parses M9 four-index packets, padding and H03/H06/H07 draw triples.  The
validator currently permits only indices present in the controlled triangle
capture, rejects kernel-owned fence/dependency indices, requires a semantic
relocation for every known address operand, checks all BO ranges and alignment,
patches a private copy, and discards it.  H06/H07 start and exclusive-end
relocations must name the same BO in increasing order; H06 records its odd
terminal halfword with ``PARHELIA_M9_RELOC_PACKED16_ODD`` instead of smuggling
bit 1 through an address.  The kernel derives access from each relocation
kind, unions it per BO, and rejects under- or over-declared BO access.  Color
and depth attachments are conservatively read/write until blend and write-mask
state belong to a kernel-selected template.  Relocations name GEM handles and
offsets; userspace cannot provide MMIO, requester, fence, DMA or VRAM bus
addresses.  Private linked-program ports ``c8..cd`` remain kernel-owned, while
captured ordinary state ``af`` is accepted.

The captured bind tuple has two distinct operands.  ``RESOURCE_69`` relocates
index ``0x69``; address-directed snapshots prove that its allocation contains
drawable pixels rather than native program bytes.  Its captured allocation
requires 0x1000 alignment.  ``RESOURCE_6C`` relocates
the separate, 0x80-aligned system-resource record selected at index ``0x6c``
and adds its hardware-owned low tag bit.  The captured values are
``0x11e42000`` and ``0x40c3a000 + 0x300 | 1`` respectively.  The kernel must
not infer native shader storage from either address until the record grammar
is completely decoded.

Hardware gates before general execution
---------------------------------------

The relocated standalone module has completed 256 consecutive kernel-owned
no-op fences on the M9148, with monotonically advancing 12-byte command
intervals and completion points through ``0x1000``.  After the independent
DRM name change, a fresh module completed another fence at ``0x10`` and
unloaded cleanly.  Commit ``f459a8b`` completed a further 256 fences through
``0x1000``.  The later typed-access/H06/H07 validator build probed successfully
with execution disabled, ran its load-time selftests, and unloaded with the PCI
device unbound and bus mastering disabled.  These results close basic
primary-requester execution but
do not authorize arbitrary userspace streams.

The two-slot primary rebase build (module SHA-256
``de5cf70322daae380c4668a1c678704f7c2dc784c85785bce8c98525a5b7eea2``)
then completed 6,000 consecutive fences on the M9148.  Submit 2,731 crossed
from slot 0 to slot 1; submit 5,461 crossed back to slot 0; the final point was
``0x17700``.  The module unloaded cleanly with no new warning or timeout and
left the device unbound with PCI bus mastering disabled.  This independently
closes both primary slot-switch directions.

The following tests are required in order:

1. Record all remaining address windows before enabling a copied user command
   image.  The tested M9148's warm ``0x44c/0x450/0x454`` controls now have a
   fail-closed equality gate; other boards still require their own property
   tuple or packed-control evidence.
2. Add exact value masks for every whitelisted state index, reserve and fence
   every referenced BO, and run the two-pass captured linear triangle.
3. Force one auxiliary requester wrap and validate quiescence, save/restore
   order and address class before publishing ``0x500`` from the new driver.
4. Add scheduler entities and syncobj timeline output, then pass an A/B/A
   multi-context state-isolation test before concurrent clients are enabled.

KMS and vblank/IRQ support are separate M9 work.  The present M9 driver has no
display feature and keeps all IRQ sources masked during the render diagnostic.
