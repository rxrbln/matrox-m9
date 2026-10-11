/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef M9_FORMAT_H
#define M9_FORMAT_H

#include <stdbool.h>
#include <stdint.h>
#include "util/format/u_format.h"

enum m9_format_usage {
   M9_FORMAT_TEXTURE = 1u << 0,
   M9_FORMAT_COLOR = 1u << 1,
   M9_FORMAT_DEPTH = 1u << 2,
};

struct m9_format_info {
   uint8_t code;
   uint8_t cpp;
   uint8_t usage;
};

bool m9_format_info(enum pipe_format format, struct m9_format_info *out);
enum pipe_format m9_format_storage(enum pipe_format format);

#endif
