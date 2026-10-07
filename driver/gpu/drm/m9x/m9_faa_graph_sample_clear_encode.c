/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <crypto/sha2.h>
#include "m9_faa_graph_sample_clear_encode.h"
#include "m9_faa_graph_depth_encode.h"

static const u8 digest[32] = {
 0x60,0x64,0x4a,0x29,0x61,0x12,0x6d,0xcf,
 0x19,0x14,0xf2,0xaf,0x16,0x30,0xc7,0x84,
 0x3b,0x19,0xd2,0x96,0x4d,0xe8,0x48,0xbb,
 0x95,0x50,0x86,0x3e,0x4d,0x3b,0xba,0x27,
};
static void put(u8 *p, u32 v)
{
 p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24;
}

int m9x_faa_sample_clear_encode(const u8 *blob, size_t bytes,
 const struct m9x_faa_graph_draw_owner owner[M9_FAA_DRAW_SOURCE_OWNERS],
 const struct m9x_faa_depth_views *views, u64 base_point,
 u8 *primary, size_t capacity, u32 *length)
{
 struct m9x_faa_depth_stage_image stages;
 const struct m9x_faa_depth_view *hz;
 u8 hash[32];
 u32 first, end, page, count, at, i;
 int ret;

 if (!blob || bytes != M9_FAA_SAMPLE_CLEAR_TEMPLATE_BYTES || !owner ||
     !views || !primary || !length || (base_point & 15) ||
     (u32)base_point < 0xb0 || (u32)base_point > 0xffffff80u ||
     base_point > ~(u64)0 - 0x70)
  return -EINVAL;
 sha256(blob, bytes, hash);
 if (memcmp(hash, digest, sizeof(hash)))
  return -EINVAL;
 ret = m9x_faa_graph_draw_owners_validate(owner);
 if (ret) return ret;
 /* Reuse the exact live bound-color/depth/HZ profile validation. The
  * caller must have installed these same profiles in the preceding stages. */
 ret = m9x_faa_depth_stages_encode(views, 0x3f800000, &stages);
 if (ret) return ret;
 if ((owner[1].base & 4095) || owner[1].bytes < 0x50000 ||
     (u64)owner[1].base + 0x50000 > 0x40000000u)
  return -ERANGE;
 for (i = M9_FAA_DEPTH_HZ; i < M9_FAA_DEPTH_OWNERS; i++) {
  const struct m9x_faa_depth_view *v = &views->owner[i];
  if ((u64)owner[1].base + owner[1].bytes > v->base &&
      (u64)v->base + v->capacity_bytes > owner[1].base)
   return -ERANGE;
 }
 hz = &views->owner[M9_FAA_DEPTH_HZ];
 first = owner[1].base >> 12;
 end = first + 80; /* admitted sampled profile: 320x240, 0x50000 logical */
 count = ((end + 7) >> 3) - (first >> 3);
 at = M9_FAA_SAMPLE_CLEAR_TEMPLATE_BYTES;
 if (capacity < at + count * 4 + 20)
  return -ERANGE;

 /* Remaining bytes are fixed commands/program for the admitted full black
  * clear. Float 0x40888889 is NOT a source owner31 address: constructor
  * fcec8/fced0 loads image constant 1f54f8 into object+55c; f47b0/f5bee
  * copies that unclamped numeric pair into this internal program's state. */
 memcpy(primary, blob, bytes);
 put(primary + 12, ((u32)base_point + 0x50) | 1);
 put(primary + 48, (hz->allocation_height << 16) | hz->storage_width);
 put(primary + 68, ((u32)base_point + 0x60) | 1);
 put(primary + 76, hz->base | 1);
 put(primary + 80, 0xf0000000 | hz->storage_width);
 /* Native submit separately authenticates ownership/PTEs of this fixed
  * SYSTEM virtual page. It is not a captured physical address. */
 put(primary + 168, 0x40004000);
 put(primary + 172, 0x40004000);
 put(primary + 204, (u32)base_point + 0x60);
 put(primary + 2968, (u32)base_point + 0x60);
 put(primary + 3104, hz->base | 1);
 put(primary + 3452, 0x20000000 | (count - 1));
 page = first;
 while (page < end) {
  u32 group = page & ~7u;
  u32 last = group + 8 < end ? group + 8 : end;
  u32 mask = ((1u << (last - page)) - 1) << (page & 7);
  put(primary + at, ((group | 0xd00000u) << 5) | mask);
  at += 4;
  page = last;
 }
 put(primary + at, 0x4545e3e6);
 put(primary + at + 4, 1);
 put(primary + at + 8, ((u32)base_point + 0x70) | 1);
 put(primary + at + 12, 0x454545e6);
 put(primary + at + 16, 1);
 *length = at + 20;
 return 0;
}
