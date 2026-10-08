/* SPDX-License-Identifier: MIT */
#ifndef M9_FAA_DIMENSIONS_H
#define M9_FAA_DIMENSIONS_H
#include <stdbool.h>
#include <stdint.h>
/* Whole INIT and allocation producers are source-qualified for these shapes.
 * Rendering at a new shape still requires its own hardware pixel proof. */
static inline bool m9_faa_runtime_dimensions(uint32_t width,uint32_t height)
{return (width==320&&height==240)||(width==1024&&height==768);}
#endif
