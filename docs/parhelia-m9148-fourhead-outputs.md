# M9148 outputs 0..3: DDC pins and modeset registers, measured

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0-only

This note closes the per-output display contract for the **Matrox M9148 LP**
(`102b:0540`, subsystem `102b:2240`) by snooping the vendor stack on the board
itself, with four displays attached — one per mini-DisplayPort connector.

Evidence tags:

* **MEASURED** — read or observed on this board this session.
* **CAPTURED** — present verbatim in a stored vendor trace or in the board ROM.
* **DERIVED** — inferred from measured/captured facts, not independently shown.
* **UNTESTED** — stated but not exercised.

Primary artifacts: `captures/m9x-vendor-fourhead-20260917/` (this session) and
`captures/m9x-cold-init-20260915-v1/xinit-mmio*.jsonl` (the earlier vendor-X
write trace).

## The board has four outputs, not five

`lspci` reports subsystem `102b:2240` = *M9148 LP PCIe x16*; the vendor X driver
enumerates exactly `mon0..mon3` and the board carries four mini-DP connectors.
**There is no output 4.**  "Outputs 2, 3 and 4" therefore means, at most, the
three connectors after the first: 0-based heads **1, 2 and 3**.  MEASURED.

## Instrumentation, and what does not work

* The vendor kernel module for this board is **`m9x.ko`** (char major 252), not
  `mtx.ko` — `mtx.ko`'s strings are the Parhelia ones and it does not serve
  `0x0540`.  MEASURED.
* **`mmiotrace` is unusable here**: the `2.6.30-dist` kernel is built with
  `# CONFIG_MMIOTRACE is not set`.  Even with it, `docs/parhelia-mmio-capture.md`
  already records that the vendor X driver maps the BARs from userspace, so a
  kernel `ioremap` tracer sees nothing.  MEASURED.
* What does work, and was used: read-only full-register snapshots
  (`tools/m9x-mmio-snapshot.c`), a read-only ordered sampler of the display
  register ranges (`tools/m9-display-watch.c`, new), the stored gdb-observer
  write trace from 2026-09-15, the board ROM, and direct HMC-arbitrated I2C
  reads (`tools/m9-dp-io.c`, new).

## Board PInS, read from this board's ROM

PInS 8.0 lives at ROM `0x7460` (u16 pointer at `0x7ffc`, signature `0x412e`).
CAPTURED:

```
+0x16  "KFQ19421"                     board serial
+0x30  "M9148-E1024LAF"               board name
+0x44  00014444                       1 GiB
+0x50  09 0a 0b 0c 0d 0e 0f 10        DDC GPIO map   -> MMIO 0x1c48
+0x6c  01 02 00 20 21 17 22 23        MISCIO GPIO map-> MMIO 0x1c4c
+0x5c  42001100 42001200 42001300 42001400   connector 0..3 routes
```

The brief's `09 0a 00 00 0d 0e 00 00` for `+0x50` is **wrong for this board**;
all eight physical bits carry a function ID, giving four complete DDC buses.

Connector route byte `0x11 + n` is the direct link code for link index `n`, so
**connector n uses link n**.  CAPTURED.

## DDC: one bit-banged I2C bus per connector on 0x1c48

MMIO `0x1c48`, data bit `n`, output enable bit `n+16`, open drain.  MEASURED and
CAPTURED (four independent bit pairs decode as clean I2C in the vendor trace,
and all four answer live):

| output | clock bit | data bit | OE bits | PInS function IDs |
|---|---|---|---|---|
| 0 | 0 | 1 | 16 / 17 | `09` / `0a` |
| 1 | 2 | 3 | 18 / 19 | `0b` / `0c` |
| 2 | 4 | 5 | 20 / 21 | `0d` / `0e` |
| 3 | 6 | 7 | 22 / 23 | `0f` / `10` |

The clock is the **even** bit of the pair and the data the odd one.  MEASURED.

Proof for the outputs this task is about: reading slave `0xa0` on bit pair
`(4,5)` returns the EDID of the sink the vendor driver calls **mon2**
(VCS, product `0x1145`, monitor name `Connector`), and on bit pair `(6,7)` the
EDID of **mon3** (GSM `0x5bf5`, `LG SDQHD`) — byte-identical to the EDIDs the
vendor X log prints for those outputs, reproduced across two boots.  MEASURED.
Outputs 0 and 1 did not answer `0xa0` in the firmware state because their
transmitter I2C bridges were left disabled; their pin pairs are DERIVED from the
PInS map plus the four identical per-bus transmitter initialisations in the
vendor trace.

Each connector has its **own external DisplayPort transmitter** on its own bus.
On this board the transmitter answers at 8-bit addresses `0x10` (main),
`0x12` and `0x14` — **not** the `0x72`/`0x76` recorded in
`parhelia-m9148-kms-pins-dp-re.md` from the saved binaries.  MEASURED.
The register fingerprint is nevertheless the documented one; the vendor's
per-connector sequence, CAPTURED verbatim from `xinit-mmio-part2.jsonl`, is:

```
S 10 00 / S 11 ..      read main reg 0
S 14 00 / S 15 ..      read page reg 0
S 12 00 / S 13 ..
S 12 7c / S 13 ..      HPD
S 10 74 20 ; S 10 74 a0 ; S 10 0a 10        blank
S 10 00 22 74 00 78 30 72 a9 27 30 30 32
     71 20 2f 82 86 f0 8b 07 02 b6 77 a3    common init
S 10 0a 00 24 01 20 21 76 ff 78 33          main pre
S 14 24 10 / 25 04 / 26 04                  page-3 init
```

### The fifth I2C bus is not a DDC bus

`0x1c4c` bit 1 = clock, bit 0 = data, OE at `+10` (PInS MISCIO IDs `01`/`02`)
carries a single 256-byte device at `0xa0`.  The vendor reads all 256 bytes of
it during driver init; reading it directly returns the **board identification
EEPROM** (`d3 25 08 00 …` then ASCII `KFQ19421`, the PInS serial).  It is not a
monitor EDID.  MEASURED.

The remaining MISCIO functions on this board are the four **transmitter reset**
lines: IDs `0x20`/`0x21`/`0x22`/`0x23` (= `0x20 + connector`) at data bits
3, 4, 6 and 7, output enables 13, 14, 16 and 17.  CAPTURED from PInS and
confirmed by exactly those bits toggling in the vendor trace.

### HMC ownership, corrected

`0x0318` request bits are active low, grant polled in `0x03b0`:

| bus | request bit in `0x0318` | grant bit in `0x03b0` |
|---|---:|---:|
| `0x1c48` (DDC) | 29 | 1 |
| `0x1c4c` (MISCIO) | 28 | 0 |

CAPTURED: every `0x1c48` run in the vendor trace is bracketed by
`0x318 = 0x10000003` / `0x30000003` and every `0x1c4c` run by
`0x60000003` / `0x70000003`.  `docs/parhelia-m9.md` has these two swapped.

## Modeset register map for the four heads

All four heads were programmed by the vendor X server in one run and all four
rasters were then observed advancing.  Bases, MEASURED — each block carries its
own head's mode, identified by matching the programmed timing and pixel clock
against that output's EDID mode:

| head | DC control | DC timing | clock select | watermark | link | cursor |
|---|---|---|---|---|---|---|
| 0 | `0x1800` | `0x1900` | `0x1c00` | `0x1c14` | `0x1c9c` | `0x18c0` |
| 1 | `0x1a00` | `0x1b00` | `0x1c08` | `0x1c1c` | `0x1ca0` | `0x1ac0` |
| 2 | `0x1980` | `0x1e00` | `0x1c04` | `0x1c18` | `0x1ca4` | `0x18e0` |
| 3 | `0x1b80` | `0x1f00` | `0x1c0c` | `0x1c20` | `0x1ca8` | `0x1ae0` |

**The fixed-stride hypothesis does not hold.**  Neither the DC bases
(`0x1800`, `0x1a00`, `0x1980`, `0x1b80`) nor the timing bases (`0x1900`,
`0x1b00`, `0x1e00`, `0x1f00`) nor the clock-select registers are a uniform
stride, and the naive continuation `0x1c00`/`0x1d00` for head 2 would land on
the clock-select and output-routing block.  Heads 2 and 3 occupy the upper half
of heads 0's and 1's timing pages (`0x1980` = `0x1900+0x80`, `0x1b80` =
`0x1b00+0x80`), which is why their DC blocks are only `0x80` long and have no
`+0x88`/`+0x90` hold pair of their own.

Sub-offsets are the same as heads 0 and 1 (`docs/parhelia-m9.md` "Display
controller"), with these measured differences for heads 2 and 3:

* `DC+0x10` is written (same value as `DC+0x0c`) and `DC+0x64 = 0x80010000`;
  neither is written for heads 0 and 1.  MEASURED, meaning not established.
* heads 2 and 3 have no `+0x88`/`+0x90` hold registers.  Instead the vendor
  writes **`0x1888` (DC0 hold) inside the head-2 sequence and `0x1a88`
  (DC1 hold) inside the head-3 sequence**, both `0x00000300`.  So head 2 is
  paired with head 0 and head 3 with head 1 for the hold protocol.  MEASURED.
* `0x190c` is written in the head-2 sequence and `0x1b0c` in the head-3
  sequence, both `(width-1)<<16` of that head's mode.  MEASURED.

Controllers 2 and 3 are **not** secondary planes of heads 0 and 1: each has its
own timing block, its own synthesiser, its own link and its own advancing line
counter.  This contradicts the `[M9WIN]` reading in `docs/parhelia-m9.md`.

### Pixel clocks: selected, not fixed per head

There is no per-head pixel-clock register.  The controller's clock-select word
picks one synthesiser:

```
select   = (clksel >> 1) & 0xf        ; bit 0 = hold
register = 0x0e38 + 4 * select
```

MEASURED, four independent confirmations in one run — the divider word in each
selected register decodes, with the published
`Fout = 27000 * b0 * b1 / b2 / (2*b3)`, to exactly that head's mode clock:

| head | clksel reg | value | select | synthesiser | decoded | mode |
|---|---|---|---|---:|---|---|
| 0 | `0x1c00` | `0x02000008` | 4 | `0x0e48` = `0x0104d6f6` | 154000 kHz | 1920x1200 |
| 1 | `0x1c08` | `0x02000002` | 1 | `0x0e3c` = `0x0102e739` | 261000 kHz | 1792x1344@75 |
| 2 | `0x1c04` | `0x00000004` | 2 | `0x0e40` = `0x0104d6b6` | 148500 kHz | 1920x1080 |
| 3 | `0x1c0c` | `0x00000006` | 3 | `0x0e44` = `0x010375df` | 172800 kHz | 1920x1080 |

`0x0e4c` and `0x0e50` hold `0x000fb370` / `0x000f5b1e` throughout and are
**not** pixel clocks; the brief's "head 1 clock `0x0e4c`" is refuted.  MEASURED.
`0x0e30/34/38` are the system clocks and all three carry the same word.

Bit 25 (`0x02000000`) is set in the clock-select words of heads 0 and 1 and
clear for heads 2 and 3.  MEASURED, meaning not established.

### Links

`0x1c9c + 4*link`, link `n` on connector `n`.  MEASURED final values:

```
0x1c9c = 0x0040a800   source 0x00 = controller 0
0x1ca0 = 0x0010a808   source 0x08 = controller 1
0x1ca4 = 0x0000a801   source 0x01 = controller 2
0x1ca8 = 0x0010a809   source 0x09 = controller 3
```

This confirms the `0x00 / 0x08 / 0x01 / 0x09` controller encoding in bits[4:0]
on live silicon, and that bit 18 (path off, `0x00040000`) is cleared to enable.
The base `0xa800` is the PInS register-patch value `0x0004a800` with bit 18
cleared.

Bit 20 is set for the two modes with negative hsync and clear for the two with
positive hsync — MEASURED, rule holds 4/4.  Bit 22 is set for head 0's negative
vsync but **clear for head 2's, which is also a negative-vsync mode**; the
vsync-inversion rule is therefore **not** established.  Since DisplayPort
carries sync as packets, this may not matter downstream.  UNTESTED.

Companion halves, MEASURED: `0x1c38 = 0x11140114` holds links 0 (low half) and
1 (high half); `0x1c3c = 0x31142114` holds links 2 and 3.  Each half is
`(link << 12) | 0x114` on this board, written inside that link's own sequence.

`0x1cac` walks `0xff -> 0xf0 -> 0x05 -> 0x07 -> 0x0f` as the four links come up,
one bit appearing to gate each pad group; the exact bit assignment is not
established.  MEASURED values, DERIVED meaning.

### Order of the vendor's four-head bring-up

From `captures/m9x-vendor-fourhead-20260917/watch-disp.csv`, MEASURED:

1. tear down head 0's firmware mode: clear `dct0`, `clksel0`, `0x1c10`, DACs,
   `dvo0`, `0x1cac = 0xff`, power down the oscillator and every synthesiser
   (`0x0e24/30/34/38/48/4c/50 = 0x80000000`), clear `dc0`;
2. restore `0x0e4c`/`0x0e50`, `0x1cac = 0xf0`, `0x1c28 = 0x0f021084`;
3. put all four controllers on hold (`0x1c00/0x1c08 = 0x02000001`,
   `0x1c04/0x1c0c = 0x00000001`), `0x1c10 = 2`, `0x1c24 = 0x101`,
   DACs `0x01001400`, all four links `0x00040000` (path off);
4. oscillator `0x70000000`, system clocks `0x0e30/34/38`;
5. then, per head in the order **0, 1, 2, 3**: DC geometry and scanout start,
   that head's synthesiser word, its `clksel` (hold released, select set), its
   watermark, `DC ctrl = 0x33040001`, `DC pitch`, the seven timing writes,
   the `0x1c38`/`0x1c3c` companion half, and finally its link word.

The sampler polls, so a value overwritten between two passes can be missed; the
snapshots are authoritative for final state and the trace for order.

## The PInS must come from the shadow BIOS, not the option ROM BAR

The clean-room kernel driver read `09 0a 00 00 0d 0e 00 00` at PInS `+0x50` and
built two DDC buses, while reading this board's ROM at the same PInS base
`0x7460` gives `09 0a 0b 0c 0d 0e 0f 10`.  The two reads differ in more than
those four bytes: the driver also reported 644 patch-table bytes where the
captured image has 596, so the images themselves are different, not the read.

`captures/m9x-vendor-fourhead-20260917/vbios-shadow.bin` is the copy POST left
at `0xc0000`.  Its expansion-ROM image is `0xf000` bytes, PCIR `102b:0540`,
last-image bit set, and both the image checksum and the PInS's own 0x400-byte
checksum are zero, so the four-bus map is the board's own and internally
consistent.  MEASURED.  (The `55 aa` image at `0xf000` in that dump is the next
option ROM the system BIOS packed into the C segment, PCIR `10ec:8168`, and not
part of the Matrox ROM.)

The vendor X driver does not read the ROM BAR at all.  `m9x_drv.so` calls
`mmap(NULL, 0x20000, …, fd, 0xc0000)` and aborts with *"Cannot map Shadow
BIOS"* / *"M-Series Shadow BIOS detected"* / *"BIOS was not run?"* — it takes
the PInS only from the POSTed RAM copy.  CAPTURED (disassembly at
`0x288f4d`).

So the flash image is generic and the VBIOS specialises the shadow for the
board it finds; the flash PInS carries the two-bus map of the smaller M9
boards.  A driver that reads the ROM BAR silently caps itself at two heads.
DERIVED — the flash image itself was not dumped, and the alternative that some
component patched the RAM copy after POST is not excluded; the driver now logs
which source it used along with the PInS board name and serial, which settles
it in one boot.  If both sources ever disagree on a board, the identification
EEPROM at `0xa0` on the MISCIO bus carries the same serial as PInS `+0x16`
(`KFQ19421` here) and can pick the right image.

## What a driver change needs

`drivers/gpu/drm/m9x` currently brings up head 0 only.  To extend it:

1. replace the single-head register constants with the table above, indexed by
   head, and keep the DC block length at `0x80` for heads 2 and 3 so the hold
   pair of head 0 / head 1 is used for them;
2. drive the pixel clock through the select rule rather than a per-head
   register: choose a free synthesiser, program `0x0e38 + 4*select`, then write
   `clksel = (select << 1)` with bit 0 cleared to release the hold;
3. build the DDC buses from PInS `+0x50`/`+0x54` rather than from the classic
   `+0x1b4` pin table, with the even ID of each pair as clock; take
   `0x0318` bit 29 / `0x03b0` bit 1 for `0x1c48` and bit 28 / bit 0 for
   `0x1c4c`, and correct the swapped assignment in `docs/parhelia-m9.md`;
4. take the transmitter slave address from the board rather than from the
   `0x72`/`0x76` constant — this board answers at `0x10`/`0x12`/`0x14`;
5. per-head vblank IRQ sources for heads 1..3 remain unknown; the per-head line
   counter at `DCT+0x24` is a usable fallback and is confirmed to advance on all
   four heads.  UNTESTED as an IRQ source.

All five were landed in `drivers/gpu/drm/m9x` together with the shadow-BIOS
PInS source above; the per-head vblank sources for heads 1..3 are still unknown
and default to no interrupt.

## All four rasters run under the KMS driver

MEASURED 2026-09-17 on `7.2.4-t2` with `m9x_drm` built from this tree
(`m9_kms=1 m9_kms_modeset=1 m9_kms_scanout=1`), setting 1024x768 on one CRTC at
a time and sampling `DCT+0x24` on all four heads:

```
probe                1924=0000037a 1b24=00000000 1e24=00000000 1f24=00000000
after CRTC 1 enabled 1924=00000422 1b24=0000018f 1e24=00000000 1f24=00000000
after CRTC 2 enabled 1924=0000049c 1b24=000000ea 1e24=00000283 1f24=00000000
after CRTC 3 enabled 1924=0000003f 1b24=00000046 1e24=000001df 1f24=000001e9
```

Each head's counter starts advancing as its CRTC is enabled and keeps advancing
after that CRTC is disabled again, since only `DC+0x00` bit 0 is cleared and the
timing block keeps running.  The heads that "did not raster" in the earlier
report had simply never had a CRTC enabled: the driver programs timing from
`m9_pipe_enable`, and the probe-time line-counter log is taken before that.  No
change to the write order was needed, and none of the global preamble words
(`0x1c28`, `0x1c24` bit 8, the DACs, the `0x1fcc/0x1fd4/0x1fdc` VGA words) is
required for a head to raster.

Still at their firmware values after four heads are up, and still unproven as
picture requirements: `0x1cac = 0xe1` (vendor ends at `0x0f`), `0x1c28 = 0`
(vendor `0x0f021084`), `0x1c80/0x1c84 = 0x03001400` (vendor `0x01001400`).  The
`m9x.pad_gate=` parameter writes `0x1cac` for the first of these.

## EDID: two sinks answer the pins, two need the AUX engine

MEASURED 2026-09-17, four displays attached, transmitter main register 0 read
per bus:

| bus | main reg 0 | direct `0xa0` read | native AUX (DPCD 0) |
|---|---|---|---|
| 0 | `0x00` | address NAK | times out |
| 1 | `0x08` | 13 bytes of zeros, then NAK | times out |
| 2 | `0x20` | full EDID, VCS "Connector" | times out |
| 3 | `0x24` | full EDID, LG SDQHD | `12 14 c4 81 01 00 01 80 …` |

So connectors 2 and 3 carry sinks reachable on the DDC pins and the driver reads
them repeatably; connector 3 additionally answers native AUX.  Connectors 0 and
1 answer neither in the firmware state, although both EDIDs *were* read once
each on 2026-09-17 - connector 1 by the two-head build after the transmitter had
been poked by hand, connector 0 by this build's bridged retry immediately after
a vendor-resolution X session had been running on it.  Neither is reproducible
from a cold transmitter.

The transmitter AUX engine itself is confirmed working exactly as
`parhelia-m9148-kms-pins-dp-re.md` "AUX native transactions" describes
(`0x7d..0x84` on the main slave, completion in `0x76` bit 5): it returns a valid
receiver-capability block on bus 3.  On buses 0, 1 and 2 it never asserts
completion, including after pulsing that connector's MISCIO transmitter-reset
line and applying the vendor's common / main-pre / page-3 tables verbatim
(`tools/m9-dp-io.c` `aux`, `auxedid`, `dump`, `block` subcommands).  Reading
EDID on connectors 0 and 1 therefore needs the full transmitter bring-up -
reset, detect, initialize, train - not a bridge toggle.  UNTESTED as a remedy.

Two driver defects were found and fixed while measuring this:

* the hotplug byte was read from the transmitter's **main** address, where it is
  meaningless; the vendor reads it from the next seven-bit address.  All four
  buses logged `hotplug 0x00`; they now log `0x13 0x07 0x07 0x07`.
* the EDID path only disabled the transmitter's I2C bridge when it found bit 1
  of main register 0 already set, which no connector on this board ever is, so
  the documented lifecycle never ran.  The bridge is now cleared for the read,
  the read is repeated through the bridge when the pins give nothing, and the
  register is restored.

## Lighting several heads at once (MEASURED, 2026-09-17)

All four connectors drive a panel simultaneously.  Measured with all four
attached, each head programmed independently:

| connector | head | mode | pixel clock | link register |
|---|---:|---|---:|---|
| DP-1 | 0 | 1920x1200@60 | 154000 kHz | `0x1c9c = 0x0040a800` |
| DP-2 | 1 | 1024x768@60 | 65000 kHz | `0x1ca0 = 0x0000a808` |
| DP-3 | 2 | 1920x1080@60 | 148500 kHz | `0x1ca4 = 0x0040a801` |
| DP-4 | 3 | 1024x768@60 | 65000 kHz | `0x1ca8 = 0x0000a809` |

`modetest` cannot do this: given several `-s` options it allocates **one**
framebuffer spanning all outputs, and the m9x pipe refuses it.  The pipe is a
`drm_simple_display_pipe`, whose plane always shows the whole framebuffer, and
`m9_kms.c` requires `fb->width == mode.hdisplay && fb->height ==
mode.vdisplay`; `mode_config.max_width` and `max_height` are both 2048, so a
wider screen is rejected before that.  `tools/m9-heads.c` gives each head its
own dumb buffer from one DRM master instead, and paints a pattern that names
the connector and encodes each pixel's coordinates.

The same limit shapes what X can do today.  A `modesetting` screen wider than
one head cannot be scanned out, so the four heads run **cloned**:
`tools/xorg-m9x-clone.conf` pins the virtual size to a mode every panel carries
(1024x768), which keeps all outputs at `+0+0`, and `tools/m9x-clone-x.sh`
starts the server and enables them.  Measured: four monitors in
`xrandr --listmonitors`, `OpenGL renderer string: Matrox M9 (clean-room
bring-up)`, direct rendering, glxgears 663 fps.

An extended desktop needs the pipe replaced by a full `drm_crtc`/`drm_plane`
pair so each head can scan out a window of a larger shared framebuffer at its
own offset - the hardware side is only a per-head `M9_DC_START` of
`base + y * pitch + x * 4` with the shared pitch in `M9_DC_PITCH`, whose mask
holds 14 bits of pixels.  NOT IMPLEMENTED.

DP-3 read `disconnected` on one probe immediately after DP-2 was attached, then
`connected` with valid EDID on the next X start without anything being
replugged.  One transient, not reproduced, cause not established.

## Do all four heads sweep, and does vblank reach the interrupt path?

Two separate questions, answered by two independent paths with
`tools/m9-scanout-probe.c`: the free-running line counter in each head's
timing bank (`DCT + 0x24`) says whether the display controller is sweeping,
and `drmCrtcGetSequence` says whether that head's vertical blank reaches the
kernel's vblank counter.  MEASURED 2026-09-17 with DP-1, DP-3 and DP-4 held by
`tools/m9-heads.c` (DP-2 was disconnected for this run):

| head | line counter | range | wraps | rate | vtotal | source of the timing |
|---:|---|---|---:|---:|---:|---|
| 0 | `0x1924` | 0..1234 | 60/s | 60.00 Hz | 1234 | DP-1 1920x1200, ours |
| 1 | `0x1b24` | 0..805 | 60/s | 60.00 Hz | 805 | firmware's posted 1024x768 |
| 2 | `0x1e24` | 0..1124 | 60/s | 60.00 Hz | 1124 | DP-3 1920x1080, ours |
| 3 | `0x1f24` | 0..805 | 60/s | 60.00 Hz | 805 | DP-4 1024x768, ours |

**All four sweep**, each at exactly the rate and vtotal of its own timing.
Head 1 was not programmed in this run at all: it is still running the
timing the firmware posted, whose `vblank 0x032502ff` is the same word the
warm snapshot in `m9_kms.c` recognises.  Sampled again after the modes were
dropped, **all four still advance at 60 Hz** - releasing the framebuffer does
not stop a timing generator.

Vblank interrupts are a different story:

```
crtc 37 (index 0)  sequence 314 -> 374  +60 in 1.00s  60.00 Hz
crtc 44 (index 1)  no vblank: Invalid argument
crtc 51 (index 2)  no vblank: Invalid argument
crtc 58 (index 3)  no vblank: Invalid argument
```

Only head 0 has one, and it is exact.  Heads 1..3 report none **by
construction**, not by failure: `m9_vblank_mask[]` carries only
`M9_IRQ_LOW_VBLANK0`, so those heads stay out of the interrupt path rather
than being given a guessed bit.

The interrupt status word gives the next step and its hazard.  Sampled
read-only (never acknowledged, so the running driver is undisturbed):

```
irq low raw 0x08e0: ever high 0xfc03ff6f  changing 0x00000000  mask 0x00000000
irq low raw 0x08e0: ever high 0xfc03ff6f  changing 0x00000001  mask 0x00000001
```

The word is W1C and the handler consumes only `raw & mask & owned`, so an
unmasked source stays latched high forever and a masked one never toggles.
**Polling cannot identify the remaining bits** - only bit 0 changes, and only
while the driver has it unmasked.  Twenty-one further bits are latched high
(`0xfc03ff6f` less bit 0) and any of them could be a head's vertical blank, as
could a bit of the high interrupt word reached through
`M9_IRQ_HIGH_LOW_CASCADE`; that the low word carries heads 1..3 at all is an
assumption, not a measurement.

### All four sources, found by frequency (MEASURED 2026-09-17)

Walking the mask was not needed.  The status word latches a source whether or
not it is unmasked, and it is write-1-to-clear, so `tools/m9-irq-scan`
acknowledges whatever is set and times how long each bit takes to come back -
no mask change, no interrupt enabled, no kernel change.  With every head on a
different refresh rate, each source's frequency names its head.  Two runs,
with the rates permuted so that no bit keeps its rate by coincidence:

| bit | run A | run B | head |
|---:|---:|---:|---:|
| 0 | 45.50 | 36.75 | 0 (raced with the driver's own acknowledge) |
| 1 | 60.00 | 60.00 | 1 |
| 2 | 75.00 | 59.75 | 0 |
| 3 | 60.00 | 60.00 | 1 |
| 8 | 50.00 | 24.00 | 2 |
| 9 | 30.00 | 50.00 | 3 |
| 10 | 50.00 | 24.00 | 2 |
| 11 | 30.00 | 50.00 | 3 |
| 12 | 75.00 | 60.00 | 0 |
| 13 | 60.00 | 60.00 | 1 |
| 14 | 50.00 | 24.00 | 2 |
| 15 | 30.00 | 50.00 | 3 |
| 16 | 10.00 | 10.00 | unrelated |
| 17 | 10.00 | 9.75 | unrelated |

Head rates were 75 / 60 / 50 / 30 in run A and 60 / 60 / 24 / 50 in run B;
head 1 stays at 60 in both because DP-2 was disconnected and it runs the
firmware's posted timing.  **Every head raises three interrupts per frame**,
grouped (0,1)+(8,9), (2,3)+(10,11), (12,13)+(14,15) across the head pairs.
Bit 0's measured rate is low in both runs because the running driver
acknowledges it too, which is itself a check that bit 0 is the driver's.

The lowest group is taken as the vblank source, since it contains the
documented head 0 bit: `M9_IRQ_LOW_VBLANK0..3 = BIT(0), BIT(1), BIT(8),
BIT(9)`.  Which of a head's three is the vertical blank edge proper is NOT
established.

### One head was masking the others

Delivery has to be measured by queueing real vblank events.  The DRM sequence
counter is extrapolated from the mode's frame duration while the source is
masked, so it reports a plausible rate for a head that is receiving nothing -
it reported 60.00 Hz for a head whose true delivery was 43 Hz.

With the bits set, delivery was still short on three heads.  The cause was in
the driver: `m9_kms_irq_desc()` filled the descriptor's `vblank_mask` with the
**union** of every head's bit, and that field is exactly what
`matrox_kms_enable_vblank` ORs into the mask register and what disable clears.
One head's `vblank_put` therefore masked every other head's source.  Enable
and disable now act on one head's bit, while the handler keeps the union,
which is what it must consume.

Measured over 2 s with heads at 75 / 60 / 50 / 30 Hz, counting queued events:

| crtc | head | union mask | per-head mask |
|---:|---:|---:|---:|
| 37 | 0 | 75.19 Hz | **75.50 Hz** |
| 44 | 1 | 43.36 Hz | **60.29 Hz** |
| 51 | 2 | 39.17 Hz | **50.20 Hz** |
| 58 | 3 | 24.05 Hz | **30.34 Hz** |

All four now deliver at their own refresh rate.  Head 1 delivers although
DP-2 is disconnected and no mode was set on it: its timing generator is still
running what the firmware posted, and the interrupt follows the generator, not
the CRTC.

## Page flipping on the vertical blank (MEASURED 2026-09-17)

With every head owning a source, `drmModePageFlip` retires on that head's own
vertical blank on every active head.  `tools/m9-heads -p SECS` flips each head
between two buffers and counts completions.

Vblank is tied to the active scanouts by the mask register alone: a head's bit
is unmasked only between `drm_crtc_vblank_on` at pipe enable and `vblank_off`
at pipe disable.  With three connectors attached and one unplugged, delivery
was 60.56 / 60.15 / 60.20 Hz on the three and 0 on the fourth, whose line
counter also read STATIC.

**A dumb-buffer flip is not an address change.**  Direct scanout needs a BO
allocated tiled (`PARHELIA_M9_BO_TILED`), which only the driver-specific create
sets; `m9_dumb_create` never does.  So every dumb flip runs
`m9_scanout_publish`, a whole-frame de-tiling copy into the head's single
scanout surface, and the start register never moves.  The cost is exactly
linear in pixels - one head, four modes, five-second windows:

| mode | pixels | flips/s | pixels/s |
|---|---:|---:|---:|
| 640x480 | 307200 | 1.34 | 412k |
| 800x600 | 480000 | 0.99 | 475k |
| 1024x768 | 786432 | 0.58 | 456k |
| 1920x1200 | 2304000 | 0.19 | 438k |

**~440k pixels/s, about 1.75 MB/s.**  So a dumb-buffer flip cannot reach the
refresh rate at any useful size, and the number to improve is that copy, not
the interrupt.

Two things this measurement caught that event counting alone would have hidden:

* An earlier run reported 361 and 360 flips in six seconds at 1920x1200 and
  1920x1080 - a clean 60 Hz - with the scanout address never moving and no
  copy that could have kept up.  `m9_program_scanout` returns silently when
  `m9_scanout_publish` fails, so the flip completed on the vertical blank and
  presented the previous frame.  A failed publish should be reported.  NOT
  FIXED.
* X here does not page flip at all.  `PageFlip: enabled` in the log is the
  option being accepted, not a flip happening: glxgears measures 527 fps with
  `vblank_mode=0` and 525 with `vblank_mode=1`, on one head and on three, so
  swaps are not synchronised to anything.  The server initialises **DRI2**
  (`(II) modeset(0): [DRI2] DRI driver: m9x`), where glamor swaps by copying;
  Present-based flipping needs DRI3.  `tools/xorg-m9x-dri3.conf` exists and is
  UNTESTED against this.
