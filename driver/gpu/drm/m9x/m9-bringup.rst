.. SPDX-License-Identifier: GPL-2.0-only

M9/Phoenix clean-room render bring-up
=====================================

The M9 backend is isolated from the classic Parhelia engine.  Device
``102b:0540`` selects a render-only ``drm_driver`` and never calls the
Parhelia KMS, host-aperture, WARP, packet validator, submit or reset code.

Safety gates
------------

``m9_enable=1`` is required before the PCI driver binds M9.  With that gate
alone the driver maps the BARs, enforces a coherent 32-bit DMA mask, allocates
kernel-owned requester/command/fence memory, and exposes DMA32 GEM plus a
validate-only command ioctl.  It does not enable PCI bus mastering or write
requester registers.

``m9_render_diag=1`` additionally permits ``M9_SUBMIT(NOOP_FENCE)``.  This is
the sole hardware submission accepted by this revision.  It builds the known
``{ 4545e3e6, 3, target|1 }`` completion packet in kernel-owned DMA memory,
programs only the statically closed command and fence addresses, rings the
``0x404`` endpoint, and waits with an absolute deadline.  A disconnect or
timeout wedges render submission.  The coherent allocations remain reachable
until teardown disables PCI bus mastering.

The auxiliary ``0x500..0x524`` requester page is allocated but not published.
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
patches a private copy, and discards it.  Relocations name GEM handles and
offsets; userspace cannot provide MMIO, requester, fence, DMA or VRAM bus
addresses.

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

The following tests are required in order:

1. Load from a temporary path with both gates set and run one no-op fence.
   Record ``0x400/0x404``, ``0x1068/0x106c``, the writeback word, ``0x8f0``
   and all ``0x500..0x524`` words before and after.
2. Repeat enough no-op fences to cross a 32-bit tag wrap and verify the
   software epoch extension.
3. Capture the three unknown ``0x44c/0x450/0x454`` controls and all address
   windows before enabling a copied user command image.
4. Add exact value masks for every whitelisted state index, reserve and fence
   every referenced BO, and run the two-pass captured linear triangle.
5. Force one auxiliary requester wrap and validate quiescence, save/restore
   order and address class before publishing ``0x500`` from the new driver.
6. Add scheduler entities and syncobj timeline output, then pass an A/B/A
   multi-context state-isolation test before concurrent clients are enabled.

KMS and vblank/IRQ support are separate M9 work.  The present M9 driver has no
display feature and keeps all IRQ sources masked during the render diagnostic.
