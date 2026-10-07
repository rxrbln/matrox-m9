/* SPDX-License-Identifier: MIT */
#ifndef M9_SRGB_H
#define M9_SRGB_H

#include <math.h>

/* Original M9128 colour-clear curve; shared with the NIR RT0 lowering. */
#define M9_SRGB_THRESHOLD  0.0030400000978261232f
#define M9_SRGB_LINEAR     12.920000076293945f
#define M9_SRGB_SCALE      1.0549999475479126f
#define M9_SRGB_BIAS       0.054999999701976776f
#define M9_SRGB_EXPONENT   0.4166666567325592f

static inline float
m9_srgb_encode_clear(float x)
{
   x = fmaxf(x, 0.0f);
   return x > M9_SRGB_THRESHOLD ?
          M9_SRGB_SCALE * powf(x, M9_SRGB_EXPONENT) - M9_SRGB_BIAS :
          M9_SRGB_LINEAR * x;
}

#endif
