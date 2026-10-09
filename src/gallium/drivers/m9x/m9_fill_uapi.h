/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FILL_UAPI_H
#define M9_FILL_UAPI_H

#include "drm-uapi/m9x_drm.h"

/* Transitional copy of the kernel's BO_FILL ioctl (m9x_drm.h). Once Mesa's
 * drm-uapi/m9x_drm.h carries it, that canonical declaration takes priority. */
#ifndef DRM_IOCTL_PARHELIA_M9_BO_FILL
#define DRM_PARHELIA_M9_BO_FILL 0x26
#define PARHELIA_M9_BO_FILL_QUERY (1u << 0)
struct drm_parhelia_m9_bo_fill {
   __u32 handle;
   __u32 flags;
   __u64 offset;
   __u64 size;
   __u32 value;
   __u32 pad;
   __u64 out_point;
};
#define DRM_IOCTL_PARHELIA_M9_BO_FILL \
   DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_BO_FILL, \
            struct drm_parhelia_m9_bo_fill)
#endif

#endif
