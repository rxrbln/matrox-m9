/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include "m9_format.h"

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
   case PIPE_FORMAT_Z16_UNORM:
      info = (struct m9_format_info){ 0x5c, 2, M9_FORMAT_DEPTH };
      break;
   case PIPE_FORMAT_S8_UINT_Z24_UNORM:
   case PIPE_FORMAT_X8Z24_UNORM:
      /* D24S8 element: depth in bits 31:8, stencil in the low byte. */
      info = (struct m9_format_info){ 0x7d, 4, M9_FORMAT_DEPTH };
      break;
   default:
      return false;
   }
   if (out)
      *out = info;
   return true;
}
