/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_AGGREGATE_UAPI_H
#define M9_AGGREGATE_UAPI_H
#include "drm-uapi/m9x_drm.h"
#ifndef PARHELIA_M9_JOB_BREAK
#define PARHELIA_M9_JOB_BREAK 0x4545452du
#define PARHELIA_M9_AGGREGATE_MAX_JOBS 4u
#define PARHELIA_M9_SUBMIT_AGGREGATE (1u << 6)
#endif
#endif
