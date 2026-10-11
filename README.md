# m9x: Matrox M9 (M9128/M9148, PCI 102b:0540) open-source driver

Copyright 2026 René Rebe <rene@exactco.de>

A clean-room Linux DRM kernel driver and a Mesa Gallium driver for the
Matrox M9 family, developed by reverse engineering. This repository holds
only the driver sources, with their full development history, the driver
documentation, and the few patches that the surrounding Linux and Mesa
trees need.

## Layout

| Path | Contents | Goes to |
|---|---|---|
| `driver/gpu/drm/m9x/` | Linux DRM/KMS driver, module `m9x_drm` | `drivers/gpu/drm/m9x/` in a Linux 7.2 tree |
| `src/gallium/drivers/m9x/` | Mesa Gallium driver, `m9x_dri` | `src/gallium/drivers/m9x/` in a Mesa 26.2.2 tree |
| `docs/` | M9 reverse-engineering and design notes; `gpu-capture-replay-policy.md` | — |
| `linux-*.patch` | Changes to the Linux tree outside the driver directory | applied with `-p1` |
| `mesa-*.patch` | Changes to the Mesa tree outside the driver directory | applied with `-p1`, in name order |

The history before each driver got its own directory is included under the
new paths: kernel commits made as `drivers/gpu/drm/parhelia/m9*`, and Mesa
commits made as `src/gallium/drivers/parhelia/m9*`.

## Patches

The patches are made against the T2 SDE package trees (Linux 7.2 and Mesa
26.2.2 with the T2 package patches applied). They apply in name order.

- `linux-01-m9x-integration.patch`: Kconfig and Makefile hooks, plus
  `drivers/gpu/drm/matrox_common/matrox_kms_common.h`, which `m9_kms.c`
  includes.
- `mesa-01-m9x-integration.patch`: registers the driver in meson, the pipe
  loader, and the DRI and DRIL targets, and adds `include/drm-uapi/m9x_drm.h`.
- `mesa-02` … `mesa-14`: generic Gallium, NIR and Mesa fixes the M9 driver
  relies on (lowered-IO aaline/aapoint/stipple lowering, fog, polygon
  offset, wide points, no-integer gallivm lowering and others). Each keeps
  its original commit message.

## Building

Kernel: copy `driver/gpu/drm/m9x` to `drivers/gpu/drm/m9x`, apply the
`linux-*.patch` files, and enable `CONFIG_DRM_M9X=m`. The driver binds only
with `m9_enable=1`. The other features have module parameters of their
own; see `driver/gpu/drm/m9x/Kconfig` and `m9-bringup.rst`.

Mesa: copy `src/gallium/drivers/m9x` to `src/gallium/drivers/m9x`, apply the
`mesa-*.patch` files, and configure with `-Dgallium-drivers=m9x` (plus any
other drivers you need).

## Licences

- Kernel driver: GPL-2.0-only. The UAPI header `m9x_drm.h` is MIT, and
  `m9x_drm_fragment.h` is `GPL-2.0-only WITH Linux-syscall-note`.
- Mesa driver: MIT.
- Encoders shared between the kernel and Mesa drivers: `GPL-2.0-only OR MIT`.
