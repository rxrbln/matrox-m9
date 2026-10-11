/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_WAIT_UAPI_H
#define M9_WAIT_UAPI_H

#ifndef PARHELIA_M9_SUBMIT_QUEUED
#define PARHELIA_M9_SUBMIT_QUEUED (1u << 3)
#endif

/* Transitional copy of the dedicated kernel completion ioctl. Once Mesa's
 * drm-uapi/m9x_drm.h carries it, that canonical declaration takes priority. */
#ifndef DRM_IOCTL_PARHELIA_M9_WAIT_POINT
#define DRM_PARHELIA_M9_WAIT_POINT 0x0d
struct drm_parhelia_m9_wait_point {
   __u64 point;
   __u64 timeout_ns;
   __u64 completed_point;
   __u32 flags;
   __u32 pad;
};
#define DRM_IOCTL_PARHELIA_M9_WAIT_POINT \
   DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_WAIT_POINT, \
             struct drm_parhelia_m9_wait_point)
#endif

#endif
