.. SPDX-License-Identifier: GPL-2.0-only

M9/Phoenix clean-room DRM/KMS bring-up
======================================

The M9 backend is the independent ``m9x_drm.ko`` module.  Device ``102b:0540``
is absent from ``parhelia.ko`` and is the sole PCI ID claimed by this driver.
The module does not link against or call the Parhelia module's host-aperture,
WARP, packet validator, submit, reset, register or output code.  The two
independent modules compile the same neutral ``matrox_common`` atomic-event,
mode/plane validation, LUT, VRAM-offset and IRQ-mask helpers.  Its DRM driver
name is ``m9x`` and Mesa selects the independent M9X Gallium driver and
``m9x_dri.so`` from that name.

Safety gates
------------

``m9_enable=1`` is required before the PCI driver binds M9.  With that gate
alone the driver maps the BARs, enforces a coherent 32-bit DMA mask, allocates
kernel-owned requester/command/fence memory, and exposes DMA32 GEM plus a
validate-only command ioctl.  It leaves PCI bus mastering disabled and does
not write requester registers until the separately gated diagnostic runs.

``m9_render_diag=1`` additionally permits ``M9_SUBMIT(NOOP_FENCE)``.  It
builds the known
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

``m9_triangle_diag=1`` permits
``M9_SUBMIT(FIRST_TRIANGLE)`` to run a staged, literal diagnostic.  The
ioctl accepts one read/write VRAM color BO, but no command object, userspace
command address or relocation.
It lazily allocates and retains coherent DMA32 storage for the exact 80-byte
flat-green source, the captured ``0x20080``-byte system payload and the exact
32-KiB class-1 BmBuffer.  The first ``0xfa0`` bytes hold the framed sequence-0
image; all bytes through ``0x8000`` are deterministic zeroes and all eight
pages receive compact PTEs.  It allocates C2, the exact 1-MiB BE context, one
zeroed ``0x5a000``-byte transient arena exposed as four adjacent
``0x16800``-byte views, the vendor's ``0x50000``-byte category-2
depth backing and ``0x2d00``-byte category-3 HZ backing directly in local
VRAM.  The color BO must likewise provide the captured ``0x50000``-byte
backing for its 320x240 visible rectangle.  Resources without an
exact vendor extent are followed by at least one page of a resource-specific
sentinel.  The source is initialized to four ``{x,y,r,g,b}`` records,
including the proved duplicate prefetch record; BE and the system payload are
zeroed deterministically.  The ``0x1080``-byte C2 object begins at its
page-aligned allocation base; page rounding gives the captured ``0x2000``
reservation stride.  The first
triangle uploads the exact vendor P00 and S28 fixed-fragment chains through
selector 68 in both rendering passes, binds their inline starts with
``69=4, 6a=0x00990000``, and then performs the vendor's selector reset before
each linked-record binding.  It does not manufacture external index-69
storage from the opaque address-directed BAR snapshot.
``m9_triangle_gouraud=1`` selects the equally
bounded captured red/green/blue source records; flat green remains the default
that matches the exact M9148 command fixture.

The preparation code encodes all 22 typed address sites in the exact
597/592/8-dword preprocess/raster/completion layout and all eight uses of six
consecutive transition targets.  It derives every address from those retained
kernel allocations, adds the established low address tags, and reserves the
largest observed ``0x580``-byte linked slot at system offset ``0x300``.  It
constructs the bounded selector-6c image semantically: Gouraud has a zero
linked payload, while the flat/case-B image uses the six references proved
across thirteen fresh vendor processes.  The fixed-function draw is published
only after its setup sequence retires.  Source, system and secondary storage receive
separate ``0x4...`` GPU virtual intervals.  Their pages use the proved
four-byte compact PTE packing and ``0xa`` flags in the reserved low-VRAM
manager image.  The load-time selftest pins each
raw template with an independent byte-wise FNV-1a value, checks every captured
address/target site, then confirms that rebuilding replaces all of those sites
and reproduces the three exact vendor ranges.

Cold execution follows the recovered vendor construction order.  It runs the
complete ``m9x421`` transition once, publishes and activates the compact PTE
mirror, runs the complete transition a second time, and then installs the
nonzero 1024-entry restore pool through the exact ``m9x109`` requester writes.
Each transition preserves its two flush boundaries and three consecutive
completion targets.  A direct no-op must retire before the driver submits the
complete captured sequence-0 context image.  Once that image retires, the
driver replaces the shared BmBuffer through ``0x17a0`` with the exact
three-range draw image and submits its nine-dword primary publication.

The color target remains pinned and TTM-reserved across the synchronous draw.
After observing the final writeback token, a DMA read barrier orders all later
guard and color reads after GPU completion.  A timeout wedges the transport
and quarantines the still-pinned color GEM until teardown, because the local
raster engine may still reference it even when host completion is uncertain.
Guard corruption is likewise terminal for that transport generation.

The guarded M9148 path writes primary requester controls
``0x44c/0x450/0x454`` as ``00280800/1f3f2707/00070703`` in their exact
``m9x109`` position after the restore tuple.  These values come from the
independently recovered target property tuple and match three warm readbacks.
A different board/property tuple must derive its own packed controls.

The primary stream uses two kernel-owned 32-KiB command slots.  Once a slot's
last synchronous fence has retired, the producer advances to the alternate
slot, rebases ``0x400`` to its tagged start, and begins again at offset zero.
It never rewinds the currently published interval in place.  The auxiliary
``0x500..0x524`` requester page is allocated but not published.
The captured vendor GL stream leaves that descriptor dormant and forced-wrap
behavior has not passed its hardware gate.  The five hardware-owned words are
therefore neither cleared nor assigned invented meanings.

``m9_kms=1`` switches the device to the modesetting-capable DRM driver only
when an exact, already-running M9148 display snapshot is present.  The initial
gate accepts only the measured DC0 1024x768 raster (65 MHz, 1344x806 totals),
XRGB8888 pitch, native/VGA flags and DVO0 state.  It exposes that one fixed
DisplayPort connector mode and never writes the timing, PLL, DVO routing or
external transmitter.  A mismatch fails probe rather than attempting cold
display setup or DP link training.

``m9_kms_scanout=1`` is a second gate for changing DC0 START/PITCH.  Dumb
buffers are XRGB8888 1024x768 objects allocated by the DRM VRAM/TTM
manager in linear BAR0.  Their ``0x301000`` backing includes the vendor
category-2 end pad beyond the ``0x300000`` visible pixels.  They are pinned
before the atomic commit and scanned out directly.  Command/resource objects
remain the independent DMA32 GEM class and
cannot become framebuffers.  There is no shadow buffer or present-time
resolve.  An ordinary CRTC disable clears only DC0 CTRL's enable bit before the
old BO is unpinned; enable selects the already-pinned replacement and reasserts
that saved warm enable bit without touching timing or DVO training.  Module
teardown restores the saved warm START, PITCH and CTRL before the VRAM manager
is destroyed; restoring CTRL also returns to the firmware LUT bypass state.

The guarded pipe shares Parhelia's atomic event, gamma-LUT and vblank-mask
lifecycle helpers.  M9 retains its own registers and uses the measured DC0 low
IRQ bit ``0x1``.  Its shared-IRQ handler computes ``raw & mask & owned``, W1C
acknowledges only that active source and leaves the normal masked pending low
bits alone.  The Parhelia handler applies the same ownership rule while also
retaining its engine-completion source.  M9 gamma streams index zero followed
by 256 ten-bit RGB entries as recovered for M9; Parhelia keeps its separately
tested explicit-index-per-entry sequence.

The driver deliberately does not install an automatic fbdev client.  The first
live scanout-gated load called ``drm_client_setup_with_fourcc()`` without a
driver ``fbdev_probe`` callback.  The helper rejected initialization, then its
partially registered restore client dereferenced a null fbdev modeset when the
first ``modetest`` file closed.  The GPU remained bus-master disabled and the
target stayed reachable, but the leaked client held the test module until a
warm reboot.  Explicit KMS owners (first ``modetest``, then Xorg) now create and
own the direct VRAM framebuffer lifecycle; this also avoids introducing a
deferred shadow fbcon path into the initial direct-scanout test.

Address layout
--------------

The manager tables are sized from the real VRAM size, not from the 256-MiB
BAR0.  Like the vendor ``m9x320 -> m9x798``, the driver reads the u16 PInS
pointer at ROM ``0x7ffc``, checks signature ``0x412e`` and sums
``32 MiB << (n - 1)`` for each low nibble ``n`` in 1..6 of PInS+0x44.  The
M9148-E1024LAF reports ``00014444``, i.e. 1 GiB.  With ``P = 4 * (VRAM pages +
0x8000)`` the layout is primary ``[0x801000, 0x801000 + P)``, translation
window ``0x80000`` bytes, tail ``P`` bytes, then the 0x181000-byte local
restore pool and its 4-MiB reserve, followed by the four mode-0 pools.  On
1 GiB this reproduces the live vendor windows ``1334/f7c=920000``,
``360/364=921000/9a0000``, ``f64/f68=9a1000/ac0000``, restore ``0xac1000`` and
pools ``0x10ec1000/0x112c1000/0x113c1000/0x11421000``.  Class-1 GPU
addresses from ``0x40000000`` index the primary child at
``0x801000 + 4 * (A >> 12)``.

The restore pool is the vendor's local mode-3 image: a ``45454747`` prefix,
1024 records at ``0x700 + i * 0x600`` with untagged local pointers, and
``10ec = 1094 = base + 0x200``.

BAR0 is a CPU window translated by the 20-slot host-aperture descriptor bank
at MMIO ``0x000..0x13f``.  The service construction (``m9x115``) invalidates
every slot, after which the low 32 MiB of BAR0 alias to the last accessed
page.  The driver publishes the linear descriptor vendor X uses for its VRAM
test (``0004=0``, ``0008=f0000000``, ``000c=00010001``, ``0000=a0000000``)
before the first CPU VRAM access and after each invalidation.  BAR0 above
32 MiB is not covered by that window.

GPU addresses below the PInS memory size are VRAM offsets, so a BO at BAR
offset ``X`` is addressed as ``X`` and receives an identity leaf.  The vendor's
private pools stay above the BAR at ``0x10000000 + base``.  Windows
``360/364`` survive warm reboot; ``f64..f7c`` are reset.

First-triangle status
---------------------

Probe runs the vendor ``m9x24643`` VRAM check once (``0x0f28`` status, dword
index and ``index << 16`` over the first ``0x1ffff`` aperture dwords) and
fails on the first mismatch.  The four service pools are zeroed in
BAR-visible VRAM.  The first triangle binds index 69 to a local VRAM copy of
the normal fixed-fragment database, loaded as firmware
``m9x/rsfragptrs-normal.bin`` (``6a=0x25010000``, Linux vendor bm-12 framing);
``m9_triangle_inline_program=1`` selects the Windows inline P00/S28 upload.
Index ``c2`` binds the vendor's compiled 40-instruction vertex program, and
the ``8d/8e`` constants and ``6c`` linked record use the bytes captured from
the vendor draw.

On the M9148 the flat 320x240 triangle renders: its 28000 green pixels land
at the same storage positions as in the vendor color target, and the
preprocessing transient views match the vendor records.  The Gouraud triangle
(raster ``22=0x2403``) matches the vendor colors at every pixel.  Render
targets are stored as an opaque tiling of 8x4 pixel blocks; a host-aperture
descriptor carrying the surface's own format (slot ``d1`` = surface address,
``d2=cc210140``, ``d3=000100f0`` for this target) makes the hardware present
linear top-down rows, where the triangle covers exactly its 28000 pixels.
CPU readback, scanout copies and userspace transfers of render targets must
use such a window rather than the raw BAR0 BO mapping.

``m9_triangle_present=1`` follows the draw with the vendor texture-rectangle
present stream, copying the color target into a kernel-owned 1024x768
layout-``0x21`` surface; its descriptor window shows exactly the triangle in
the top-left 320x240.

``m9_user_submit=1`` executes validated userspace streams.  The kernel frames
the count-free packets into counted records, submits them through the
class-1 BmBuffer followed by the vendor completion range, and waits for its
fence with every referenced VRAM BO pinned.

Every non-command DMA32 BO receives a class-1 engine address from a
``drm_mm`` range above ``0x40000000``; its compact leaves are published at
creation and cleared on free.  Complete two-pass draws use the relocation
kinds ``SYSTEM`` (vertex sources, ``8d/8e`` constant spans, tag 1),
``PROGRAM`` (``c2``), ``STREAM`` (preprocessing outputs, raster inputs,
``be``), ``RESOURCE_69`` and ``RESOURCE_6C``.  ``e3``/``43`` values are
0x10-aligned dependency offsets rebased onto the kernel job target, and a
``0x4545452c`` packet with value zero ends an execution range.
``tools/m9-user-triangle`` replays the first triangle entirely from userspace
this way: flat gives the vendor 28000 green pixels, Gouraud 28000 shaded
pixels.

Tiled KMS scanout
-----------------

With ``m9_kms=1 m9_kms_scanout=1`` the display controller never fetches a
userspace dumb BO.  Probe pins a kernel scanout BO; each plane update and
dirty-fb call copies the linear 1024x768 XRGB8888 image through slot 0
retargeted to that BO (``d2=cc210400``, ``d3=00010300``), restores the
identity window and points ``DC_START`` at the scanout BO.  modetest's SMPTE
pattern reads back row-exact through the same descriptor.  Xorg modesetting
with ``AccelMethod none`` and ``ShadowFB on`` scans out through the dirty-fb
copy; every root-window pixel matches the scanout surface.

UAPI boundary
-------------

``M9_CREATE_BO`` creates an immutable DMA32 object by default and returns a GEM
handle and DRM mmap cookie.  Command objects are explicitly marked with
``PARHELIA_M9_BO_COMMAND``.  The additive ``PARHELIA_M9_BO_VRAM`` flag instead
creates a page-aligned object pinned in local video memory for its handle
lifetime; it is mutually exclusive with ``COMMAND``.  No bus or local address
is returned.

``M9_GET_BO_INFO`` returns the memory class, size, and DRM mmap cookie for an
imported handle.  It is render-node allowed so Mesa can map a driver VRAM BO
without calling the primary-node-only ``MODE_MAP_DUMB`` ioctl.

``M9_SUBMIT(VALIDATE_ONLY)`` copies a command slice from such an object.  It
parses M9 four-index packets, padding and H03/H06/H07 draw triples.  The
validator currently permits only indices present in the controlled triangle
capture, rejects kernel-owned fence/dependency indices, requires a semantic
relocation for every known address operand, checks all BO ranges and alignment,
patches a private copy, and discards it.  Color, depth, texture and resource-69
operands accept this driver's pinned local-VRAM objects, including direct dumb
scanout buffers and ``BO_VRAM`` allocations, and resolve through the local
0x10000000 address window; command, vertex/index and other private resource
operands remain DMA32.  H06/H07 start and exclusive-end
relocations must name the same BO in increasing order; H06 records its odd
terminal halfword with ``PARHELIA_M9_RELOC_PACKED16_ODD`` instead of smuggling
bit 1 through an address.  The kernel derives access from each relocation
kind, unions it per BO, and rejects under- or over-declared BO access.  Color
and depth attachments are conservatively read/write until blend and write-mask
state belong to a kernel-selected template.  Relocations name GEM handles and
offsets; userspace cannot provide MMIO, requester, fence, DMA or VRAM bus
addresses.  Private linked-program ports ``c8..cd`` remain kernel-owned, while
captured ordinary state ``af`` is accepted.

The captured bind tuple has two distinct operands.  ``RESOURCE_69`` describes
the external binding for the per-context immutable fixed-fragment database.
The saved descriptor reports a 0x1b080-byte, 0x1000-aligned allocation, but its
address-directed BAR snapshot is an opaque alias rather than a logical
resource dump.  External address translation remains unresolved; the bounded
triangle therefore uses the vendor's exact inline alternative.  ``RESOURCE_6C`` relocates
the separate, 0x80-aligned system-resource record selected at index ``0x6c``
and adds its hardware-owned low tag bit.  The captured values are
``0x11e42000`` and ``0x40c3a000 + 0x300 | 1`` respectively.  The kernel must
not infer native shader storage from the latter address until the record
grammar is completely decoded.

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

The fixed warm KMS gate has passed its first M9148 hardware review.  With the
module staged only below ``/tmp`` and loaded with
``m9_enable=1 m9_kms=1 m9_kms_scanout=1``, ``modetest -M m9x -c`` enumerated
one connected ``DP-1`` and exactly one preferred 1024x768 mode at 60.00 Hz.
The connector, CRTC and primary XR24 linear plane were usable by an explicit
DRM client.  A direct XR24 modeset completed without a kernel warning, and the
same modeset with the vblank test produced five interrupts on the M9 IRQ.
After the clients closed, ``rmmod m9x_drm`` succeeded and restored
``CTRL=33046001``, ``START=0``, ``PITCH=cc210400``,
``HBLANK=053f03ff``, ``VBLANK=032502ff`` and ``DVO0=0000a800`` exactly; the
PCI function was left unbound with bus mastering disabled.  The corrected
client-owned build also completed a subsequent no-op fence at point ``0x10``
while KMS and the vblank IRQ handler were installed.

Arbitrary timings, EDID/DDC, cold PLL setup, output route changes, external DP
retraining, cursor planes and additional CRTCs remain disabled until their
M9-specific sequences are independently proven.
