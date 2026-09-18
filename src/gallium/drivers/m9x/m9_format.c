/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include "m9_format.h"

#include "m9_draw_params.h"

bool
m9_format_info(enum pipe_format format, struct m9_format_info *out)
{
   struct m9_format_info info;

   switch (format) {
   case PIPE_FORMAT_B8G8R8A8_UNORM:
      info = (struct m9_format_info){ 0x66, 4,
         M9_FORMAT_TEXTURE | M9_FORMAT_COLOR };
      break;
   case PIPE_FORMAT_B8G8R8X8_UNORM:
      info = (struct m9_format_info){ 0x66, 4,
         M9_FORMAT_TEXTURE | M9_FORMAT_COLOR };
      break;
   case PIPE_FORMAT_R8G8B8A8_UNORM:
      info = (struct m9_format_info){ 0x64, 4, M9_FORMAT_TEXTURE };
      break;
   case PIPE_FORMAT_B5G6R5_UNORM:
      info = (struct m9_format_info){ 0x4e, 2, M9_FORMAT_TEXTURE };
      break;
   case PIPE_FORMAT_B5G5R5A1_UNORM:
      info = (struct m9_format_info){ 0x4a, 2, M9_FORMAT_TEXTURE };
      break;
   case PIPE_FORMAT_B4G4R4A4_UNORM:
      info = (struct m9_format_info){ 0x46, 2, M9_FORMAT_TEXTURE };
      break;
   case PIPE_FORMAT_L8_UNORM:
      info = (struct m9_format_info){ 0x30, 1, M9_FORMAT_TEXTURE };
      break;
   case PIPE_FORMAT_R8G8B8X8_UNORM:
      info = (struct m9_format_info){ 0x64, 4, M9_FORMAT_TEXTURE };
      break;
   case PIPE_FORMAT_A8_UNORM:
      info = (struct m9_format_info){ 0x33, 1, M9_FORMAT_TEXTURE };
      break;
   case PIPE_FORMAT_L8A8_UNORM:
      info = (struct m9_format_info){ 0x51, 2, M9_FORMAT_TEXTURE };
      break;
   /* 10-bit GigaColor: storage class 2 at 32 bits per texel, allocated and
    * tiled exactly like B8G8R8A8, and the same code in the sampler and the
    * target role (docs/parhelia-m9-gigacolor-10bit.md).  R10G10B10A2 puts
    * red at bit 0, which is D3D A2B10G10R10. */
   case PIPE_FORMAT_R10G10B10A2_UNORM:
      info = (struct m9_format_info){ M9_COLOR_FORMAT_A2B10G10R10, 4,
         M9_FORMAT_TEXTURE | M9_FORMAT_COLOR };
      break;
   case PIPE_FORMAT_B10G10R10A2_UNORM:
      info = (struct m9_format_info){ M9_COLOR_FORMAT_A2R10G10B10, 4,
         M9_FORMAT_TEXTURE | M9_FORMAT_COLOR };
      break;
   case PIPE_FORMAT_I8_UNORM:
      info = (struct m9_format_info){ 0x51, 1, M9_FORMAT_TEXTURE };
      break;
   /* Depth surfaces sample as well as store: the descriptor carries the
    * same code in either role (docs/parhelia-m9-openbits-gl-capture.md).
    * Z16 samples but does not store: the depth unit needs the hierarchical
    * surface m9_resource_attach_hz builds, which is decoded for D24S8 only,
    * so a Z16 attachment silently drew without depth (MEASURED, glmark2
    * terrain).  Claiming only what works lets st fall back to D24S8. */
   case PIPE_FORMAT_Z16_UNORM:
      info = (struct m9_format_info){ 0x5c, 2, M9_FORMAT_TEXTURE };
      break;
   case PIPE_FORMAT_S8_UINT_Z24_UNORM:
   case PIPE_FORMAT_X8Z24_UNORM:
      /* D24S8 element: depth in bits 31:8, stencil in the low byte. */
      info = (struct m9_format_info){ 0x7d, 4,
         M9_FORMAT_TEXTURE | M9_FORMAT_DEPTH };
      break;
   default:
      return false;
   }
   if (out)
      *out = info;
   return true;
}
