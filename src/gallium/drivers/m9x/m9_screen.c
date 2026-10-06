/* SPDX-License-Identifier: MIT */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include "m9_screen.h"
#include "m9_fs_compile.h"
#include "m9_vs_compile.h"
#include "m9_specialise.h"
#include "m9x_public.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <xf86drm.h>
#include "drm-uapi/drm.h"
#include "drm-uapi/drm_fourcc.h"
#include "drm-uapi/drm_mode.h"
#include "draw/draw_context.h"
#include "m9_batch.h"
#include "m9_wait_uapi.h"
#include "m9_format.h"
#include "m9_internal.h"
#include "m9_vs_texture.h"
#include "nir.h"
#include "util/format/u_format.h"
#include "util/u_memory.h"
#include "util/u_inlines.h"
#include "util/u_atomic.h"
#include "util/u_screen.h"

static inline struct m9_screen *
m9_screen(struct pipe_screen *screen)
{
   return (struct m9_screen *)screen;
}

static inline struct m9_resource *
m9_resource(struct pipe_resource *resource)
{
   return (struct m9_resource *)resource;
}

int
m9_bo_create(struct m9_screen *screen, uint64_t size, uint32_t flags,
             unsigned kind, struct m9_resource *resource)
{
   struct drm_parhelia_m9_create_bo create = {
      .size = size,
      .flags = flags,
   };

   if (!screen || !resource || !size)
      return -EINVAL;
   if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_M9_CREATE_BO, &create))
      return -errno;
   memset(resource, 0, sizeof(*resource));
   resource->map_fd = -1;
   /* Engine-only storage lives above the host aperture and carries no mmap
    * cookie; it is never touched from here. */
   if (!(flags & PARHELIA_M9_BO_ENGINE_ONLY)) {
      resource->map = mmap(NULL, create.size, PROT_READ | PROT_WRITE,
                           MAP_SHARED, screen->fd, create.mmap_offset);
      if (resource->map == MAP_FAILED) {
         int error = errno;
         struct drm_gem_close close_bo = { .handle = create.handle };

         resource->map = NULL;
         ioctl(screen->fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
         return -error;
      }
      resource->map_writable = true;
   }
   resource->handle = create.handle;
   resource->kind = kind;
   resource->size = create.size;
   resource->mmap_offset = create.mmap_offset;
   resource->domain = flags & (PARHELIA_M9_BO_VRAM | PARHELIA_M9_BO_TILED) ?
                      M9_BO_VRAM : M9_BO_DMA32;
   resource->tiled = flags & PARHELIA_M9_BO_TILED;
   resource->handle_owned = true;
   return 0;
}

/*
 * Storage the engine alone reads and writes.  It is placed in the video memory
 * no host aperture maps, of which there is far more than the CPU-visible
 * window holds; a kernel without that placement still serves it from the
 * window.
 */
int
m9_bo_create_engine_only(struct m9_screen *screen, uint64_t size,
                         unsigned kind, struct m9_resource *resource)
{
   /* M9X_ENGINE_VRAM=0 keeps the object in the mapped window, which separates
    * a fault in the placement from one in what uses the object. */
   const char *enable = getenv("M9X_ENGINE_VRAM");
   int ret = -EINVAL;

   if (!enable || strcmp(enable, "0"))
      ret = m9_bo_create(screen, size,
                         PARHELIA_M9_BO_VRAM | PARHELIA_M9_BO_ENGINE_ONLY,
                         kind, resource);
   if (ret)
      ret = m9_bo_create(screen, size, PARHELIA_M9_BO_VRAM, kind, resource);
   return ret;
}

void
m9_bo_destroy(struct m9_screen *screen, struct m9_resource *resource)
{
   struct drm_gem_close close_bo;

   if (!screen || !resource)
      return;
   if (resource->map)
      munmap(resource->map, resource->size);
   close_bo.handle = resource->handle;
   if (close_bo.handle)
      ioctl(screen->fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
   memset(resource, 0, sizeof(*resource));
   resource->map_fd = -1;
}

static int
m9_resource_attach_hz(struct m9_screen *screen, struct m9_resource *resource)
{
   struct m9_resource *hz = CALLOC_STRUCT(m9_resource);
   uint32_t *words;
   int ret;

   if (!hz)
      return -ENOMEM;
   ret = m9_bo_create(screen, m9_hz_bytes(resource->width, resource->height),
                      PARHELIA_M9_BO_VRAM, M9_RESOURCE_LINEAR_DEPTH, hz);
   if (ret) {
      FREE(hz);
      return ret;
   }
   hz->width = resource->width;
   hz->height = resource->height;
   hz->cpp = 4;
   words = hz->map;
   for (uint64_t i = 0; i < hz->size / 4; i++)
      words[i] = M9_HZ_CLEAR;
   resource->hz = hz;
   return 0;
}

static void
m9_resource_release_hz(struct m9_screen *screen, struct m9_resource *resource)
{
   if (!resource->hz)
      return;
   m9_bo_destroy(screen, resource->hz);
   FREE(resource->hz);
   resource->hz = NULL;
}

static const char *m9_name(struct pipe_screen *screen)
{
   (void)screen;
   return "Matrox M9 (clean-room bring-up)";
}

static const char *m9_vendor(struct pipe_screen *screen)
{
   (void)screen;
   return "Mesa";
}

static const char *m9_device_vendor(struct pipe_screen *screen)
{
   (void)screen;
   return "Matrox";
}

static int m9_fd(struct pipe_screen *screen)
{
   return m9_screen(screen)->fd;
}

/* Formats the display controller scans out: the 8-bit BGRX/BGRA family and
 * the two 10-bit GigaColor orders, which reach the controller through the
 * same descriptor field (docs/parhelia-m9-gigacolor-10bit.md 5.1). */
static bool
m9_presentation_format(enum pipe_format format)
{
   return format == PIPE_FORMAT_B8G8R8X8_UNORM ||
          format == PIPE_FORMAT_B8G8R8A8_UNORM ||
          format == PIPE_FORMAT_B10G10R10X2_UNORM ||
          format == PIPE_FORMAT_B10G10R10A2_UNORM ||
          format == PIPE_FORMAT_R10G10B10X2_UNORM ||
          format == PIPE_FORMAT_R10G10B10A2_UNORM;
}

static bool
m9_presentation_template(const struct pipe_resource *templ)
{
   const unsigned presentation = PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET |
                                 PIPE_BIND_SHARED;

   /* DRI3 back buffers are SCANOUT|SHARED at window size; KMS checks the
    * framebuffer size itself. */
   return templ->target == PIPE_TEXTURE_2D && templ->width0 &&
          templ->width0 <= 2048 && templ->height0 &&
          templ->height0 <= 2048 && templ->depth0 == 1 &&
          templ->array_size == 1 && !templ->last_level &&
          templ->nr_samples <= 1 && templ->nr_storage_samples <= 1 &&
          m9_presentation_format(templ->format) &&
          (templ->bind & (PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET)) &&
          !(templ->bind & ~(presentation | PIPE_BIND_RENDER_TARGET |
                            PIPE_BIND_BLENDABLE | PIPE_BIND_SAMPLER_VIEW |
                            PIPE_BIND_LINEAR |
                            PIPE_BIND_USE_FRONT_RENDERING));
}

static bool
m9_is_format_supported(struct pipe_screen *screen, enum pipe_format format,
                       enum pipe_texture_target target, unsigned samples,
                       unsigned storage_samples, unsigned bind)
{
   struct m9_format_info info;
   const unsigned image_bind = PIPE_BIND_DEPTH_STENCIL |
                               PIPE_BIND_RENDER_TARGET |
                               PIPE_BIND_BLENDABLE |
                               PIPE_BIND_SAMPLER_VIEW |
                               PIPE_BIND_DISPLAY_TARGET |
                               PIPE_BIND_SCANOUT | PIPE_BIND_SHARED |
                               PIPE_BIND_LINEAR |
                               PIPE_BIND_USE_FRONT_RENDERING;
   unsigned required = 0;

   (void)screen;
   if (samples > 1 || storage_samples > 1)
      return false;
   if (target == PIPE_BUFFER)
      /* VERTEX_STATE: display-list (vbo_save) vertex/index storage. */
      return bind && !(bind & ~(PIPE_BIND_VERTEX_BUFFER |
                                PIPE_BIND_INDEX_BUFFER |
                                PIPE_BIND_CONSTANT_BUFFER |
                                PIPE_BIND_VERTEX_STATE));
   switch (target) {
   case PIPE_TEXTURE_1D:
   case PIPE_TEXTURE_2D:
   case PIPE_TEXTURE_RECT:
      break;
   case PIPE_TEXTURE_CUBE:
   case PIPE_TEXTURE_3D:
      /* A cube face and a volume slice are stored only by the tiled texture
       * calculator: there is no captured render, depth or scanout placement
       * for either, and the kernel texture window addresses a plane.  st
       * asks for RENDER_TARGET speculatively, and without retrying, for the
       * formats GL requires (GL_RGBA with a packed 4444/5551/8888 type), so
       * accept the bind; m9_update_target refuses a face or slice as a
       * target. */
      if (bind & ~(PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_LINEAR |
                   PIPE_BIND_RENDER_TARGET))
         return false;
      if (!m9_texture_format(format, NULL) ||
          util_format_is_depth_or_stencil(format))
         return false;
      break;
   default:
      return false;
   }
   /* S3TC: a sampler source only.  The vendor's own operation table gives
    * the DXT rows no render-target, mipmap-generation or display bit
    * (docs/parhelia-m9-s3tc-compressed-textures.md 9), and nothing here
    * writes a compressed surface with the raster engine.  The sRGB variants
    * are the same blocks, decoded by the fragment program. */
   if (m9_texture_compressed(format, NULL, NULL))
      return target == PIPE_TEXTURE_2D && samples <= 1 &&
             !(bind & ~(PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_LINEAR));
   if (!m9_format_info(format, &info))
      return false;
   /* M9X_GIGA_ORDER=0x68|0x6a hides the other 10-bit code, so a fixture can
    * reach the channel order the state tracker does not prefer.  Both are
    * the same storage; only the descriptor code differs. */
   if ((info.code == M9_COLOR_FORMAT_A2B10G10R10 ||
        info.code == M9_COLOR_FORMAT_A2R10G10B10)) {
      static int giga_order = -1;

      if (giga_order < 0)
         giga_order = (int)debug_get_num_option("M9X_GIGA_ORDER", 0);
      if (giga_order && info.code != giga_order)
         return false;
   }
   if (bind & ~image_bind)
      return false;
   if ((bind & PIPE_BIND_BLENDABLE) && !(bind & PIPE_BIND_RENDER_TARGET))
      return false;
   if ((bind & PIPE_BIND_DEPTH_STENCIL) &&
       (bind & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_BLENDABLE)))
      return false;
   if (bind & PIPE_BIND_SAMPLER_VIEW)
      required |= M9_FORMAT_TEXTURE;
   if (bind & PIPE_BIND_RENDER_TARGET)
      required |= M9_FORMAT_COLOR;
   if (bind & PIPE_BIND_DEPTH_STENCIL)
      required |= M9_FORMAT_DEPTH;
   if (bind & PIPE_BIND_SHADER_IMAGE)
      return false;
   if (bind & (PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET))
      return target == PIPE_TEXTURE_2D && m9_presentation_format(format) &&
             !(bind & ~(PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET |
                        PIPE_BIND_SHARED | PIPE_BIND_RENDER_TARGET |
                        PIPE_BIND_BLENDABLE | PIPE_BIND_SAMPLER_VIEW |
                        PIPE_BIND_LINEAR |
                        PIPE_BIND_USE_FRONT_RENDERING));
   /* bind==0 and LINEAR-only images are CPU staging resources. */
   return !(required & ~info.usage);
}

static int
m9_prime_import_get(struct m9_screen *screen, int fd,
                    struct m9_resource *resource)
{
   struct m9_prime_import *entry;
   uint32_t handle;
   int ret = 0;

   simple_mtx_lock(&screen->import_lock);
   if (drmPrimeFDToHandle(screen->fd, fd, &handle)) {
      ret = -errno;
      goto out_unlock;
   }
   for (entry = screen->imports; entry; entry = entry->next) {
      if (entry->handle == handle) {
         if (entry->references == UINT32_MAX) {
            ret = -EOVERFLOW;
            goto out_unlock;
         }
         entry->references++;
         resource->handle = handle;
         resource->prime_import = entry;
         goto out_unlock;
      }
   }
   entry = CALLOC_STRUCT(m9_prime_import);
   if (!entry) {
      ret = -ENOMEM;
      goto out_unlock;
   }
   entry->handle = handle;
   entry->references = 1;
   entry->owns_handle = true;
   entry->next = screen->imports;
   screen->imports = entry;
   resource->handle = handle;
   resource->prime_import = entry;
out_unlock:
   simple_mtx_unlock(&screen->import_lock);
   return ret;
}

static int
m9_prime_import_get_cached(struct m9_screen *screen, uint32_t handle,
                           struct m9_resource *resource)
{
   struct m9_prime_import *entry;
   int ret = -ENOENT;

   simple_mtx_lock(&screen->import_lock);
   for (entry = screen->imports; entry; entry = entry->next) {
      if (entry->handle != handle)
         continue;
      if (entry->references == UINT32_MAX) {
         ret = -EOVERFLOW;
         break;
      }
      entry->references++;
      resource->handle = handle;
      resource->prime_import = entry;
      ret = 0;
      break;
   }
   simple_mtx_unlock(&screen->import_lock);
   return ret;
}

static int
m9_prime_track_export(struct m9_screen *screen, struct m9_resource *resource)
{
   struct m9_prime_import *entry;

   simple_mtx_lock(&screen->import_lock);
   if (resource->prime_import) {
      simple_mtx_unlock(&screen->import_lock);
      return 0;
   }
   for (entry = screen->imports; entry; entry = entry->next) {
      if (entry->handle == resource->handle) {
         if (entry->references == UINT32_MAX) {
            simple_mtx_unlock(&screen->import_lock);
            return -EOVERFLOW;
         }
         entry->references++;
         resource->prime_import = entry;
         simple_mtx_unlock(&screen->import_lock);
         return 0;
      }
   }
   entry = CALLOC_STRUCT(m9_prime_import);
   if (!entry) {
      simple_mtx_unlock(&screen->import_lock);
      return -ENOMEM;
   }
   entry->handle = resource->handle;
   entry->references = 1;
   entry->owns_handle = resource->handle_owned;
   entry->next = screen->imports;
   screen->imports = entry;
   resource->prime_import = entry;
   simple_mtx_unlock(&screen->import_lock);
   return 0;
}

static void
m9_prime_import_put(struct m9_screen *screen, struct m9_prime_import *entry)
{
   struct m9_prime_import **link;

   simple_mtx_lock(&screen->import_lock);
   for (link = &screen->imports; *link; link = &(*link)->next) {
      if (*link != entry)
         continue;
      if (--entry->references == 0) {
         *link = entry->next;
         if (entry->owns_handle) {
            struct drm_gem_close close_bo = { .handle = entry->handle };
            ioctl(screen->fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
         }
         if (getenv("M9X_DEBUG_RESOURCE"))
            fprintf(stderr, "m9x: shared handle %u released%s\n",
                    entry->handle, entry->owns_handle ? ", closed" : "");
         FREE(entry);
      }
      break;
   }
   simple_mtx_unlock(&screen->import_lock);
}

static struct pipe_resource *
m9_resource_create(struct pipe_screen *pscreen,
                   const struct pipe_resource *templ)
{
   struct m9_screen *screen = m9_screen(pscreen);
   struct m9_resource *resource = CALLOC_STRUCT(m9_resource);
   struct drm_parhelia_m9_create_bo create = {0};
   struct m9_format_info format = {0};
   struct m9_render_layout render_layout;
   struct m9_texture_format texture;
   uint64_t size;

   if (!resource)
      return NULL;
   resource->map_fd = -1;
   /* Renderbuffers always request SAMPLER_VIEW; depth images are never
    * sampled on this path, so do not advertise depth textures for them.
    * A buffer is CPU memory: st binds a pixel pack/unpack buffer as
    * RENDER_TARGET | SAMPLER_VIEW for GPU paths this driver never takes. */
   if (templ->target != PIPE_BUFFER &&
       !m9_is_format_supported(pscreen, templ->format, templ->target,
                               templ->nr_samples, templ->nr_storage_samples,
                               templ->bind & PIPE_BIND_DEPTH_STENCIL ?
                               templ->bind & ~PIPE_BIND_SAMPLER_VIEW :
                               templ->bind))
      goto fail;
   if ((templ->bind & (PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET)) &&
       !m9_presentation_template(templ))
      goto fail;
   if (m9_presentation_template(templ)) {
      resource->kind = M9_RESOURCE_LINEAR_COLOR;
      if (!m9_format_info(templ->format, &format) ||
          !m9_render_surface_layout(templ->width0, templ->height0,
                                    format.cpp, &render_layout))
         goto fail;
      resource->row_span = render_layout.row_span;
      size = render_layout.size;
   } else if (templ->target == PIPE_BUFFER) {
      size = templ->width0;
      resource->kind = M9_RESOURCE_LINEAR_BUFFER;
      format.cpp = 1;
   } else {
      /* The storage family follows from the target, not from a plane path
       * with the layered targets bolted on: a cube keeps six face
       * allocations of one chain, a volume feeds its slice to the address
       * calculator, and a plane has neither. */
      enum m9_texture_kind kind = m9_texture_kind_for_target(templ->target);
      /* A line is stored and sampled as a plane one texel high, which keeps
       * it on the measured plane storage, uploads and mip chain. */
      if (kind == M9_TEXTURE_KIND_1D)
         kind = M9_TEXTURE_KIND_2D;
      bool layered = kind == M9_TEXTURE_KIND_CUBE ||
                     kind == M9_TEXTURE_KIND_3D;
      uint32_t depth = kind == M9_TEXTURE_KIND_3D ? templ->depth0 : 1;
      unsigned layers = kind == M9_TEXTURE_KIND_CUBE ? 6 : 1;

      /* A compressed image is stored by the surface window in whole
       * blocks; it has no cpp and takes none of the layouts below. */
      if (m9_texture_compressed(templ->format, &texture.code,
                                &resource->block_bytes)) {
         if (kind != M9_TEXTURE_KIND_2D ||
             m9_resource_bo_flags(templ) != PARHELIA_M9_BO_VRAM ||
             m9_texture_layout_compressed(&resource->texture, templ->width0,
                                          templ->height0,
                                          templ->last_level + 1,
                                          resource->block_bytes))
            goto fail;
         resource->kind = M9_RESOURCE_LINEAR_TEXTURE;
         resource->texture_storage = templ->format;
         resource->texture_code = texture.code;
         resource->row_span = (templ->width0 + 31) & ~31u;
         size = resource->texture.size;
         goto allocate;
      }
      if (!m9_format_info(templ->format, &format) || !kind ||
          (kind != M9_TEXTURE_KIND_3D && templ->depth0 != 1) ||
          templ->array_size != layers ||
          templ->last_level >= M9_MAX_MIP_LEVELS ||
          templ->width0 > 2048 || templ->height0 > 2048)
         goto fail;
      /* Mesa asks for mipmapped textures that are also render targets.  The
       * render surface holds one level, so such an image is stored as a
       * texture; m9_update_target refuses it as a target, which is the bind
       * Mesa only asks for speculatively. */
      resource->kind = templ->bind & PIPE_BIND_DEPTH_STENCIL ?
         M9_RESOURCE_LINEAR_DEPTH :
         (templ->bind & PIPE_BIND_RENDER_TARGET) && !templ->last_level &&
         !layered ? M9_RESOURCE_LINEAR_COLOR : M9_RESOURCE_LINEAR_TEXTURE;
      /* A mipmapped depth image is a sampler source only, like a
       * mipmapped colour one: store the chain as a texture. */
      if (resource->kind == M9_RESOURCE_LINEAR_DEPTH && templ->last_level)
         resource->kind = M9_RESOURCE_LINEAR_TEXTURE;
      if (resource->kind != M9_RESOURCE_LINEAR_TEXTURE && templ->last_level)
         goto fail;
      /* A face and a slice are reachable only through the tiled calculator:
       * neither the linear mip storage nor the plane-shaped kernel texture
       * window can address one. */
      if (layered && (resource->kind != M9_RESOURCE_LINEAR_TEXTURE ||
                      m9_resource_bo_flags(templ) != PARHELIA_M9_BO_VRAM ||
                      !m9_texture_format(templ->format, &texture)))
         goto fail;
      if (resource->kind == M9_RESOURCE_LINEAR_TEXTURE &&
          m9_resource_bo_flags(templ) == PARHELIA_M9_BO_VRAM &&
          kind != M9_TEXTURE_KIND_1D &&
          m9_texture_format(templ->format, &texture)) {
         /* Sampled images use the vendor's tiled texture storage. */
         if (m9_texture_layout_init(&resource->texture, kind,
                                    templ->width0, templ->height0, depth,
                                    templ->last_level + 1))
            goto fail;
         /* A plane falls back to the kernel texture window for the levels
          * the calculator cannot place; a layered chain has no fallback, so
          * refuse it rather than write a face or a slice to a wrong
          * address. */
         if (layered && !m9_texture_layout_complete(&resource->texture))
            goto fail;
         resource->texture_storage = texture.storage;
         resource->texture_code = texture.code;
         resource->row_span = m9_resource_row_span(templ->width0);
         size = resource->texture.size;
      } else if (resource->kind == M9_RESOURCE_LINEAR_TEXTURE) {
         if (m9_mip_layout_init(&resource->mip, templ->width0,
                                templ->height0, 1, templ->last_level + 1,
                                format.cpp))
            goto fail;
         resource->row_span = resource->mip.level[0].row_span;
         size = resource->mip.size;
      } else {
         if (!(resource->kind == M9_RESOURCE_LINEAR_DEPTH && format.cpp == 4 ?
               m9_depth_surface_layout(templ->width0, templ->height0,
                                       &render_layout) :
               m9_render_surface_layout(templ->width0, templ->height0,
                                        format.cpp, &render_layout)))
            goto fail;
         resource->row_span = render_layout.row_span;
         size = render_layout.size;
      }
   }
allocate:
   if (templ->target == PIPE_BUFFER) {
      /* Only the CPU draw module reads Gallium buffers; no kernel BO, so no
       * contiguous DMA32 allocation (vbo_save asks for 1 MiB). */
      resource->map = align_malloc(size, 64);
      if (!resource->map)
         goto fail;
      resource->map_writable = true;
      resource->size = size;
      resource->domain = M9_BO_DMA32;
   } else if (!resource->handle) {
      create.size = size;
      create.flags = m9_resource_bo_flags(templ);
      /* GPU color targets carry the kernel's immutable tiled layout tag.
       * A 10-bit target is the same class 2 storage, so it is tagged too. */
      if (resource->kind == M9_RESOURCE_LINEAR_COLOR &&
          (format.code == M9_COLOR_FORMAT_A8R8G8B8 ||
           format.code == M9_COLOR_FORMAT_A2B10G10R10 ||
           format.code == M9_COLOR_FORMAT_A2R10G10B10))
         create.flags = PARHELIA_M9_BO_TILED;
      if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_M9_CREATE_BO, &create))
         goto fail;
      resource->map = mmap(NULL, create.size, PROT_READ | PROT_WRITE,
                           MAP_SHARED, screen->fd, create.mmap_offset);
      if (resource->map == MAP_FAILED) {
         struct drm_gem_close close_bo = { .handle = create.handle };
         resource->map = NULL;
         ioctl(screen->fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
         goto fail;
      }
      resource->map_writable = true;
      resource->handle = create.handle;
      resource->size = create.size;
      resource->mmap_offset = create.mmap_offset;
      resource->domain = create.flags &
                         (PARHELIA_M9_BO_VRAM | PARHELIA_M9_BO_TILED) ?
                         M9_BO_VRAM : M9_BO_DMA32;
      resource->tiled = create.flags & PARHELIA_M9_BO_TILED;
      resource->handle_owned = true;
   }
   resource->base = *templ;
   resource->base.screen = pscreen;
   pipe_reference_init(&resource->base.reference, 1);
   resource->width = templ->width0;
   resource->height = templ->height0;
   resource->depth = templ->depth0;
   resource->cpp = format.cpp;
   resource->format = format.code;
   resource->layout = 1; /* exact ordinary linear descriptor class */
   if (resource->kind == M9_RESOURCE_LINEAR_DEPTH &&
       format.code == M9_DEPTH_FORMAT_D24S8 &&
       m9_resource_attach_hz(screen, resource))
      goto fail;
   if (templ->target != PIPE_BUFFER && getenv("M9X_DEBUG_RESOURCE"))
      fprintf(stderr, "m9x: resource %ux%u levels %u kind %u tex %u "
              "format %s bind %#x\n", templ->width0, templ->height0,
              templ->last_level + 1, resource->kind, resource->texture.levels,
              util_format_short_name(templ->format), templ->bind);
   return &resource->base;
fail:
   if (templ->target != PIPE_BUFFER && getenv("M9X_DEBUG_RESOURCE"))
      fprintf(stderr, "m9x: resource REFUSED %ux%u levels %u format %s "
              "bind %#x usage %u\n", templ->width0, templ->height0,
              templ->last_level + 1, util_format_short_name(templ->format),
              templ->bind, templ->usage);
   if (templ->target != PIPE_BUFFER && getenv("M9X_ABORT_REFUSED"))
      abort();
   m9_resource_release_hz(screen, resource);
   if (resource->map && !resource->handle)
      align_free(resource->map);
   else if (resource->map)
      munmap(resource->map, resource->size);
   if (resource->handle_owned && resource->handle) {
      struct drm_gem_close close_bo = { .handle = resource->handle };
      ioctl(screen->fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
   }
   FREE(resource);
   return NULL;
}

static void m9_resource_destroy(struct pipe_screen *pscreen,
                                struct pipe_resource *pres);

/*
 * The modifier describing how a resource is stored.  Only colour targets are
 * ever exported (m9_resource_get_handle), and only those the kernel tagged
 * BO_TILED are in the render calculator's storage; the small family below
 * 64x64 is a different address function and so a different modifier.
 */
static uint64_t
m9_storage_modifier(bool tiled, uint32_t width, uint32_t height)
{
   if (!tiled)
      return DRM_FORMAT_MOD_LINEAR;
   return width <= 64 && height <= 64 ?
          DRM_FORMAT_MOD_MATROX_M9_TILED_SMALL :
          DRM_FORMAT_MOD_MATROX_M9_TILED;
}

static uint64_t
m9_resource_modifier(const struct m9_resource *resource)
{
   return m9_storage_modifier(resource->tiled, resource->width,
                              resource->height);
}

/* Modifiers this screen can hand out and take back, most preferred first. */
static const uint64_t m9_modifiers[] = {
   DRM_FORMAT_MOD_MATROX_M9_TILED,
   DRM_FORMAT_MOD_MATROX_M9_TILED_SMALL,
   DRM_FORMAT_MOD_LINEAR,
};

static bool
m9_modifier_supported(uint64_t modifier)
{
   for (unsigned i = 0; i < ARRAY_SIZE(m9_modifiers); i++)
      if (m9_modifiers[i] == modifier)
         return true;
   return false;
}

static void
m9_query_dmabuf_modifiers(struct pipe_screen *pscreen, enum pipe_format format,
                          int max, uint64_t *modifiers,
                          unsigned int *external_only, int *count)
{
   unsigned n = 0;

   /* Only the presentation family, 8 and 10 bit, is ever stored tiled, so
    * every other format can be shared linear alone. */
   for (unsigned i = 0; i < ARRAY_SIZE(m9_modifiers); i++) {
      if (m9_modifiers[i] != DRM_FORMAT_MOD_LINEAR &&
          !m9_presentation_format(format))
         continue;
      if (modifiers && (int)n < max)
         modifiers[n] = m9_modifiers[i];
      if (external_only && (int)n < max)
         external_only[n] = 0;
      n++;
   }
   *count = modifiers ? MIN2((int)n, max) : (int)n;
}

static bool
m9_is_dmabuf_modifier_supported(struct pipe_screen *pscreen, uint64_t modifier,
                                enum pipe_format format, bool *external_only)
{
   uint64_t list[ARRAY_SIZE(m9_modifiers)];
   int count = 0;

   m9_query_dmabuf_modifiers(pscreen, format, ARRAY_SIZE(list), list, NULL,
                             &count);
   if (external_only)
      *external_only = false;
   for (int i = 0; i < count; i++)
      if (list[i] == modifier)
         return true;
   return false;
}

/*
 * A resource asked for by modifier.  The storage a template selects is fixed
 * (m9_resource_create), so this only accepts a list that contains the
 * modifier that template will actually produce rather than laying anything
 * out differently.
 */
static struct pipe_resource *
m9_resource_create_with_modifiers(struct pipe_screen *pscreen,
                                  const struct pipe_resource *templ,
                                  const uint64_t *modifiers, int count)
{
   struct pipe_resource *pres = m9_resource_create(pscreen, templ);
   uint64_t have;

   if (!pres)
      return NULL;
   have = m9_resource_modifier(m9_resource(pres));
   for (int i = 0; i < count; i++)
      if (modifiers[i] == have || modifiers[i] == DRM_FORMAT_MOD_INVALID)
         return pres;
   m9_resource_destroy(pscreen, pres);
   return NULL;
}

static struct pipe_resource *
m9_resource_from_handle(struct pipe_screen *pscreen,
                        const struct pipe_resource *templ,
                        struct winsys_handle *whandle, unsigned usage)
{
   struct m9_screen *screen = m9_screen(pscreen);
   struct m9_resource *resource;
   struct m9_format_info format;
   struct m9_render_layout render_layout;
   struct drm_parhelia_m9_get_bo_info info = {0};
   uint64_t required;
   int ret = 0;

   if (!whandle || !m9_is_format_supported(pscreen, templ->format,
                                            templ->target,
                                            templ->nr_samples,
                                            templ->nr_storage_samples,
                                            templ->bind) ||
       templ->target != PIPE_TEXTURE_2D || !templ->width0 ||
       !templ->height0 || templ->width0 > 2048 || templ->height0 > 2048 ||
       templ->depth0 != 1 || templ->array_size != 1 || templ->last_level ||
       ((templ->bind & (PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET)) &&
        !m9_presentation_template(templ)) ||
       whandle->offset || !m9_format_info(templ->format, &format) ||
       !format.cpp || whandle->stride % format.cpp ||
       whandle->stride / format.cpp > UINT16_MAX ||
       (whandle->modifier != DRM_FORMAT_MOD_INVALID &&
        !m9_modifier_supported(whandle->modifier)))
      return NULL;
   /* A tiled buffer is a render-calculator surface whatever it is bound as:
    * its stride is the 32-aligned row span and its size the whole padded
    * allocation, not stride times height. */
   if ((templ->bind & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_DEPTH_STENCIL)) ||
       (whandle->modifier != DRM_FORMAT_MOD_INVALID &&
        whandle->modifier != DRM_FORMAT_MOD_LINEAR)) {
      if (!m9_render_surface_layout(templ->width0, templ->height0,
                                    format.cpp, &render_layout) ||
          whandle->stride / format.cpp != render_layout.row_span)
         return NULL;
      required = render_layout.size;
   } else {
      if (whandle->stride / format.cpp <
          m9_resource_row_span(templ->width0))
         return NULL;
      required = (uint64_t)whandle->stride * templ->height0;
   }
   resource = CALLOC_STRUCT(m9_resource);
   if (!resource)
      return NULL;
   resource->map_fd = -1;
   resource->row_span = whandle->stride / format.cpp;

   switch (whandle->type) {
   case WINSYS_HANDLE_TYPE_KMS:
      ret = m9_prime_import_get_cached(screen, whandle->handle, resource);
      if (ret == -ENOENT) {
         resource->handle = whandle->handle;
         ret = 0;
      }
      break;
   case WINSYS_HANDLE_TYPE_SHARED: {
      struct drm_gem_open open = { .name = whandle->handle };
      if (ioctl(screen->fd, DRM_IOCTL_GEM_OPEN, &open)) {
         ret = -errno;
         break;
      }
      resource->handle = open.handle;
      /* Duplicated screen fds share one DRM file/handle namespace.  Keep
       * imported handles until that file closes so another screen cannot
       * invalidate the same numeric handle. */
      resource->handle_owned = false;
      break;
   }
   case WINSYS_HANDLE_TYPE_FD:
      ret = m9_prime_import_get(screen, whandle->handle, resource);
      if (ret)
         break;
      resource->map_fd = fcntl(whandle->handle, F_DUPFD_CLOEXEC, 3);
      if (resource->map_fd < 0) {
         ret = -errno;
         break;
      }
      break;
   default:
      ret = -EINVAL;
      break;
   }
   if (ret)
      goto fail;
   info.handle = resource->handle;
   if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_M9_GET_BO_INFO, &info)) {
      ret = -errno;
      goto fail;
   }
   if (info.memory_class != PARHELIA_M9_MEMORY_CLASS_VRAM ||
       info.size < required) {
      ret = -EINVAL;
      goto fail;
   }
   resource->size = info.size;
   resource->mmap_offset = info.mmap_offset;
   resource->domain = M9_BO_VRAM;
   resource->tiled = info.layout == PARHELIA_M9_LAYOUT_TILED;
   if (getenv("M9X_DEBUG_RESOURCE"))
      fprintf(stderr, "m9x: import handle %u type %u %ux%u %s %llu bytes\n",
              resource->handle, whandle->type, templ->width0, templ->height0,
              util_format_short_name(templ->format),
              (unsigned long long)info.size);
   /* The kernel owns the truth about the storage.  An explicit modifier has
    * to agree with it: a tiled buffer taken in as linear would be read as
    * rows and come back band structured, which is a scrambled image and not
    * an error the importer could see.  MOD_INVALID is the legacy path that
    * negotiated nothing, so there the kernel's answer stands alone. */
   if (whandle->modifier != DRM_FORMAT_MOD_INVALID &&
       whandle->modifier != m9_storage_modifier(resource->tiled,
                                                templ->width0,
                                                templ->height0)) {
      ret = -EINVAL;
      goto fail;
   }
   /* Whatever the importer asked to bind it as, tiled storage can only be
    * addressed by the surface contract it was laid out with. */
   if (resource->tiled &&
       (!m9_render_surface_layout(templ->width0, templ->height0, format.cpp,
                                  &render_layout) ||
        resource->row_span != render_layout.row_span ||
        info.size < render_layout.size)) {
      ret = -EINVAL;
      goto fail;
   }
   if (resource->map_fd >= 0) {
      int prot = PROT_READ;

      resource->map_writable =
         usage & (PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE |
                  PIPE_HANDLE_USAGE_SHADER_WRITE) ||
         templ->bind & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_DEPTH_STENCIL);
      if (resource->map_writable)
         prot |= PROT_WRITE;
      resource->map = mmap(NULL, resource->size, prot,
                           MAP_SHARED, resource->map_fd, 0);
      if (resource->map == MAP_FAILED) {
         resource->map = NULL;
         ret = -errno;
      }
   } else {
      resource->map_writable = true;
      resource->map = mmap(NULL, resource->size, PROT_READ | PROT_WRITE,
                           MAP_SHARED, screen->fd, resource->mmap_offset);
      if (resource->map == MAP_FAILED) {
         resource->map = NULL;
         ret = -errno;
      }
   }
   if (ret)
      goto fail;
   resource->base = *templ;
   resource->base.screen = pscreen;
   pipe_reference_init(&resource->base.reference, 1);
   resource->kind = templ->bind & PIPE_BIND_DEPTH_STENCIL ?
      M9_RESOURCE_LINEAR_DEPTH :
      resource->tiled ||
      (templ->bind & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_SCANOUT |
                      PIPE_BIND_DISPLAY_TARGET)) ?
      M9_RESOURCE_LINEAR_COLOR : M9_RESOURCE_LINEAR_TEXTURE;
   resource->width = templ->width0;
   resource->height = templ->height0;
   resource->depth = 1;
   resource->cpp = format.cpp;
   resource->format = format.code;
   resource->layout = 1;
   if (resource->kind == M9_RESOURCE_LINEAR_DEPTH &&
       format.code == M9_DEPTH_FORMAT_D24S8 &&
       (ret = m9_resource_attach_hz(screen, resource)))
      goto fail;
   if (resource->kind == M9_RESOURCE_LINEAR_TEXTURE) {
      resource->mip.level[0].width = resource->width;
      resource->mip.level[0].height = resource->height;
      resource->mip.level[0].depth = 1;
      resource->mip.level[0].row_span = resource->row_span;
      resource->mip.size = required;
      resource->mip.levels = 1;
      resource->mip.cpp = resource->cpp;
   }
   return &resource->base;
fail:
   m9_resource_release_hz(screen, resource);
   if (resource->map)
      munmap(resource->map, resource->size);
   if (resource->map_fd >= 0)
      close(resource->map_fd);
   if (resource->prime_import) {
      m9_prime_import_put(screen, resource->prime_import);
   } else if (resource->handle_owned && resource->handle) {
      struct drm_gem_close close_bo = { .handle = resource->handle };
      ioctl(screen->fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
   }
   FREE(resource);
   return NULL;
}

static bool
m9_resource_get_handle(struct pipe_screen *pscreen, struct pipe_context *ctx,
                       struct pipe_resource *pres,
                       struct winsys_handle *whandle, unsigned usage)
{
   struct m9_resource *resource = m9_resource(pres);

   (void)ctx;
   (void)usage;
   /* A texture chain has no one stride and no one modifier to export. */
   if (!whandle || !resource->handle || resource->domain == M9_BO_DMA32 ||
       resource->texture.levels)
      return false;
   whandle->stride = resource->row_span * resource->cpp;
   whandle->offset = 0;
   whandle->modifier = m9_resource_modifier(resource);
   switch (whandle->type) {
   case WINSYS_HANDLE_TYPE_KMS:
      if (m9_prime_track_export(m9_screen(pscreen), resource))
         return false;
      whandle->handle = resource->handle;
      return true;
   case WINSYS_HANDLE_TYPE_SHARED: {
      struct drm_gem_flink flink = { .handle = resource->handle };
      if (ioctl(m9_screen(pscreen)->fd, DRM_IOCTL_GEM_FLINK, &flink))
         return false;
      whandle->handle = flink.name;
      return true;
   }
   case WINSYS_HANDLE_TYPE_FD: {
      int fd;
      if (m9_prime_track_export(m9_screen(pscreen), resource))
         return false;
      if (drmPrimeHandleToFD(m9_screen(pscreen)->fd, resource->handle,
                             DRM_CLOEXEC | DRM_RDWR, &fd))
         return false;
      whandle->handle = fd;
      return true;
   }
   default:
      return false;
   }
}

static void
m9_resource_destroy(struct pipe_screen *pscreen, struct pipe_resource *pres)
{
   struct m9_resource *resource = m9_resource(pres);
   struct drm_gem_close close_bo = { .handle = resource->handle };

   m9_resource_release_hz(m9_screen(pscreen), resource);
   /* Heap-backed Gallium buffers carry no kernel handle. */
   if (resource->map && !resource->handle)
      align_free(resource->map);
   else if (resource->map)
      munmap(resource->map, resource->size);
   if (resource->map_fd >= 0)
      close(resource->map_fd);
   if (resource->prime_import) {
      m9_prime_import_put(m9_screen(pscreen), resource->prime_import);
   } else if (resource->handle_owned && resource->handle) {
      ioctl(m9_screen(pscreen)->fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
   }
   FREE(resource);
}

int
m9_command_bo_create(struct m9_screen *screen, struct m9_resource *resource)
{
   return m9_bo_create(screen, M9_MAX_COMMAND_DWORDS * sizeof(uint32_t),
                       PARHELIA_M9_BO_COMMAND, M9_RESOURCE_COMMAND, resource);
}

void
m9_command_bo_destroy(struct m9_screen *screen, struct m9_resource *resource)
{
   m9_bo_destroy(screen, resource);
}

struct pipe_fence_handle *
m9_fence_create(uint64_t point, bool signaled)
{
   struct m9_fence *fence = CALLOC_STRUCT(m9_fence);

   if (!fence)
      return NULL;
   pipe_reference_init(&fence->reference, 1);
   fence->point = point;
   p_atomic_set(&fence->signaled, signaled || !point);
   return (struct pipe_fence_handle *)fence;
}

static void
m9_fence_reference(struct pipe_screen *screen,
                   struct pipe_fence_handle **ptr,
                   struct pipe_fence_handle *fence)
{
   struct m9_fence *old = (struct m9_fence *)*ptr;
   struct m9_fence *new_fence = (struct m9_fence *)fence;

   (void)screen;
   if (pipe_reference(old ? &old->reference : NULL,
                      new_fence ? &new_fence->reference : NULL))
      FREE(old);
   *ptr = fence;
}

static bool
m9_fence_finish(struct pipe_screen *screen, struct pipe_context *ctx,
                struct pipe_fence_handle *fence, uint64_t timeout)
{
   struct m9_fence *m9f = (struct m9_fence *)fence;

   struct m9_screen *m9s = m9_screen(screen);

   (void)ctx;
   if (!m9f || p_atomic_read(&m9f->signaled))
      return true;
   if (m9s->wait_point) {
      struct drm_parhelia_m9_wait_point wait = {
         .point = m9f->point, .timeout_ns = timeout,
      };
      if (ioctl(m9s->fd, DRM_IOCTL_PARHELIA_M9_WAIT_POINT, &wait) ||
          wait.completed_point < m9f->point)
         return false;
   } else {
      uint64_t completed;

      /* Older async kernels have no read-only completion query. A zero
       * timeout must not pretend completion or post a blocking command;
       * an explicit wait falls back to the existing synchronous drain. */
      if (!timeout || m9_submit_noop_fence(m9s, &completed) ||
          completed < m9f->point)
         return false;
   }
   p_atomic_set(&m9f->signaled, true);
   return true;
}

static bool
m9_check_resource_capability(struct pipe_screen *pscreen,
                             struct pipe_resource *pres, unsigned bind)
{
   struct m9_resource *resource = m9_resource(pres);

   (void)pscreen;
   if (bind & ~(PIPE_BIND_SCANOUT | PIPE_BIND_LINEAR))
      return false;
   if (bind & PIPE_BIND_SCANOUT)
      if (resource->domain != M9_BO_VRAM ||
          resource->kind != M9_RESOURCE_LINEAR_COLOR ||
          resource->layout != 1 || resource->row_span != 1024 ||
          resource->width != 1024 || resource->height != 768 ||
          resource->depth != 1 || resource->cpp != 4 ||
          pres->target != PIPE_TEXTURE_2D || pres->last_level ||
          pres->array_size != 1 ||
          pres->format != PIPE_FORMAT_B8G8R8X8_UNORM)
         return false;
   return !(bind & PIPE_BIND_LINEAR) ||
          (resource->layout == 1 && !resource->tiled);
}

static void
m9_flush_frontbuffer(struct pipe_screen *screen, struct pipe_context *ctx,
                     struct pipe_resource *pres, unsigned level,
                     unsigned layer, void *winsys_drawable_handle,
                     unsigned nboxes, struct pipe_box *subbox)
{
   struct m9_resource *resource = m9_resource(pres);

   (void)screen;
   (void)ctx;
   (void)level;
   (void)layer;
   (void)winsys_drawable_handle;
   (void)nboxes;
   (void)subbox;
   /* Scanout is the same linear VRAM BO.  Publish direct CPU writes; there
    * is no shadow, resolve, or Present-side copy in this driver. */
   if (resource->domain != M9_BO_DMA32)
      __sync_synchronize();
}

/* The uniforms selecting control flow are marked for inlining per value set,
 * which is what makes the state tracker send their values.  The fragment NIR
 * is also brought here to the float-only form the screen declares; the vertex
 * shader is not, because the draw module runs this very shader and gallivm
 * does that lowering for itself -- the native backend lowers its own clone. */
static void
m9_finalize_nir(struct pipe_screen *pscreen, struct nir_shader *nir,
                bool optimize)
{
   (void)pscreen;
   (void)optimize;
   if (nir->info.stage == MESA_SHADER_FRAGMENT)
      m9_nir_lower_integers(nir);
   if (nir->info.stage == MESA_SHADER_FRAGMENT ||
       nir->info.stage == MESA_SHADER_VERTEX)
      m9_nir_find_inlinable_uniforms(nir);
}

static void
m9_destroy(struct pipe_screen *pscreen)
{
   struct m9_screen *screen = m9_screen(pscreen);

   while (screen->imports) {
      struct m9_prime_import *entry = screen->imports;
      struct drm_gem_close close_bo = { .handle = entry->handle };

      screen->imports = entry->next;
      if (entry->owns_handle)
         ioctl(screen->fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
      FREE(entry);
   }
   m9_bo_destroy(screen, &screen->record);
   m9_faa_store_report(screen);
   m9_bo_destroy(screen, &screen->faa_store);
   m9_bo_destroy(screen, &screen->faa_store_aux);
   simple_mtx_destroy(&screen->faa_store_lock);
   simple_mtx_destroy(&screen->record_lock);
   simple_mtx_destroy(&screen->import_lock);
   FREE(screen->fragment_db);
   close(screen->fd);
   FREE(pscreen);
}

/*
 * What the engine put in the multisample fragment store.  Every dword was
 * filled with its own index, so a write is identifiable wherever it landed.
 * Printed once, from whichever teardown path runs first.
 */
static struct m9_screen *m9_faa_check_screen;

void
m9_faa_store_report(struct m9_screen *screen)
{
   const uint32_t *words;
   uint64_t count, changed = 0, shown = 0;

   if (!screen || !screen->faa.store_check || !screen->faa_store.map)
      return;
   words = screen->faa_store.map;
   count = screen->faa_store.size / 4;
   for (uint64_t k = 0; k < count; k++)
      if (words[k] != m9_faa_store_pattern(k)) {
         changed++;
         if (shown < 8) {
            fprintf(stderr, "m9x: faa store dword %llu %08x -> %08x\n",
                    (unsigned long long)k, m9_faa_store_pattern(k), words[k]);
            shown++;
         }
      }
   fprintf(stderr, "m9x: faa store %llu of %llu dwords changed\n",
           (unsigned long long)changed, (unsigned long long)count);
   screen->faa.store_check = false;
}

static void
m9_faa_store_atexit(void)
{
   m9_faa_store_report(m9_faa_check_screen);
}



static void
m9_init_shader_caps(struct m9_screen *screen)
{
   struct pipe_shader_caps *vs =
      (struct pipe_shader_caps *)&screen->base.shader_caps[MESA_SHADER_VERTEX];
   struct pipe_shader_caps *fs =
      (struct pipe_shader_caps *)&screen->base.shader_caps[MESA_SHADER_FRAGMENT];

   draw_init_shader_caps(vs);
   vs->supported_irs = 1 << PIPE_SHADER_IR_NIR;
   /* st requires matching integer support across stages. */
   vs->integers = false;
   vs->int16 = false;
   vs->fp16 = false;
   vs->fp16_derivatives = false;
   vs->fp16_const_buffers = false;
   vs->glsl_16bit_load_dst = false;
   vs->indirect_temp_addr = false;
   /* Vertex programs run on the CPU in the draw module, whose own sampler
    * reads a linear copy of the image (m9_vs_texture.c). */
   vs->max_texture_samplers = vs->max_sampler_views = M9_VS_MAX_SAMPLERS;
   vs->max_shader_buffers = vs->max_shader_images = 0;

   fs->supported_irs = 1 << PIPE_SHADER_IR_NIR;
   fs->max_instructions = fs->max_alu_instructions = 64;
   fs->max_tex_instructions = fs->max_tex_indirections = 8;
   fs->max_inputs = 8;
   fs->max_outputs = 1;
   fs->max_const_buffer0_size = 32 * 4 * sizeof(float);
   fs->max_const_buffers = 1;
   fs->max_temps = 32;
   /* A program binds at most M9_FS_MAX_TEXTURES texel pairs, but shaders
    * declare more samplers than any one variant uses and the unused ones are
    * eliminated before compiling; advertising only the binding limit stops
    * such shaders linking at all (ioquake3's opengl2 renderer). */
   fs->max_texture_samplers = fs->max_sampler_views = 8;
}

static bool
m9_probe_user_submit(struct m9_screen *screen)
{
   struct drm_parhelia_m9_submit empty = {0};
   struct m9_resource command, stream;
   struct m9_batch *batch;
   bool ok = false;

   /* An empty executable request fails argument validation (EINVAL) only
    * when the kernel executes user streams; otherwise it is refused. */
   if (!ioctl(screen->fd, DRM_IOCTL_PARHELIA_M9_SUBMIT, &empty) ||
       errno != EINVAL)
      return false;
   /* The two-pass STREAM relocation kind must pass validation. */
   batch = CALLOC_STRUCT(m9_batch);
   if (!batch)
      return false;
   if (!m9_command_bo_create(screen, &command)) {
      if (!m9_bo_create(screen, 4096, PARHELIA_M9_BO_VRAM,
                        M9_RESOURCE_LINEAR_BUFFER, &stream)) {
         const struct m9_packet_value be = {
            .index = 0xbe, .handle = stream.handle,
            .kind = PARHELIA_M9_RELOC_STREAM,
         };

         m9_batch_init(batch);
         ok = !m9_batch_emit_packet(batch, &be, 1) &&
              !m9_batch_validate(screen, &command, batch, 0);
         if (ok) {
            const struct m9_packet_value bias[] = {
               { .index = 0xd0, .value = 0 },
               { .index = 0xd1, .value = 0 },
            };

            /* Older kernels execute STREAM but do not admit depth bias.
             * Probe only their typed parser; no GPU commands execute. */
            m9_batch_init(batch);
            screen->native_depth_bias =
               !m9_batch_emit_packet(batch, bias, ARRAY_SIZE(bias)) &&
               !m9_batch_validate(screen, &command, batch, 0);
         }
         m9_bo_destroy(screen, &stream);
      }
      m9_command_bo_destroy(screen, &command);
   }
   FREE(batch);
   return ok;
}

static bool
m9_read_file(const char *path, uint8_t **data, size_t *size)
{
   int fd = open(path, O_RDONLY | O_CLOEXEC);
   uint8_t *buffer = NULL;
   struct stat st;
   size_t done = 0;

   if (fd < 0)
      return false;
   if (!fstat(fd, &st) && st.st_size > 0 && st.st_size <= (64 << 20))
      buffer = MALLOC(st.st_size);
   while (buffer && done < (size_t)st.st_size) {
      ssize_t n = read(fd, buffer + done, st.st_size - done);

      if (n <= 0)
         break;
      done += n;
   }
   close(fd);
   if (!buffer || done != (size_t)st.st_size) {
      FREE(buffer);
      return false;
   }
   *data = buffer;
   *size = done;
   return true;
}

static void
m9_load_fragment_db(struct m9_screen *screen)
{
   static const char name[] = "m9x/rsfragptrs-normal.bin";
   const char *dirs[] = { NULL, "/lib/firmware/updates", "/lib/firmware" };
   const char *env = getenv("M9X_FRAGMENT_DB");
   char custom[PATH_MAX] = "", path[PATH_MAX];
   FILE *param;

   if (env) {
      if (!m9_read_file(env, &screen->fragment_db, &screen->fragment_db_size))
         fprintf(stderr, "m9x: cannot read M9X_FRAGMENT_DB %s\n", env);
      return;
   }
   /* Search the firmware directories in the kernel loader's order. */
   param = fopen("/sys/module/firmware_class/parameters/path", "r");
   if (param) {
      if (fgets(custom, sizeof(custom), param))
         custom[strcspn(custom, "\n")] = 0;
      fclose(param);
   }
   if (custom[0])
      dirs[0] = custom;
   for (unsigned i = 0; i < ARRAY_SIZE(dirs); i++) {
      if (!dirs[i])
         continue;
      snprintf(path, sizeof(path), "%s/%s", dirs[i], name);
      if (m9_read_file(path, &screen->fragment_db, &screen->fragment_db_size))
         return;
   }
}

struct pipe_screen *
m9_screen_create(int fd, const struct pipe_screen_config *config)
{
   struct m9_screen *screen = CALLOC_STRUCT(m9_screen);
   struct pipe_caps *caps;

   (void)config;
   if (!screen)
      return NULL;
   screen->fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
   if (screen->fd < 0)
      goto fail;
   simple_mtx_init(&screen->import_lock, mtx_plain);
   simple_mtx_init(&screen->record_lock, mtx_plain);
   simple_mtx_init(&screen->faa_store_lock, mtx_plain);
   screen->chip_id = M9_PCI_ID;
   m9_faa_init(&screen->faa, (uint16_t)screen->chip_id);
   /* The report has to survive a client that exits without tearing the
    * screen down, which is what the probe fixture does. */
   if (screen->faa.store_check) {
      m9_faa_check_screen = screen;
      atexit(m9_faa_store_atexit);
   }
   screen->uapi_version = 1;
   screen->validate_submit = true;
   screen->execute_submit = m9_probe_user_submit(screen);
   struct drm_parhelia_m9_wait_point completion_probe = {0};
   screen->wait_point = !ioctl(screen->fd, DRM_IOCTL_PARHELIA_M9_WAIT_POINT,
                               &completion_probe);
   m9_load_fragment_db(screen);
   screen->base.get_screen_fd = m9_fd;
   u_init_pipe_screen_caps(&screen->base, 1);
   screen->base.nir_options[MESA_SHADER_VERTEX] = &m9_vs_nir_options;
   screen->base.nir_options[MESA_SHADER_FRAGMENT] = &m9_fs_nir_options;
   m9_init_shader_caps(screen);
   caps = (struct pipe_caps *)&screen->base.caps;
   /* Four colour targets: b0 + 2s a target with its own descriptor, class
    * and write mask, and one native destination a target in the fragment
    * program.  M9X_MAX_RENDER_TARGETS holds it back to one, which is how a
    * run separates an MRT fault from anything else.
    * docs/parhelia-m9-mrt-routing.md */
   caps->max_render_targets =
      CLAMP((unsigned)debug_get_num_option("M9X_MAX_RENDER_TARGETS",
                                           M9_MAX_COLOR),
            1u, M9_MAX_COLOR);
   caps->max_texture_2d_size = 2048;
   /* A cube face is placed only inside the captured 32x16 tail, so 16 is
    * the largest face (MEASURED).  A volume is placed by the vendor address
    * calculators at any extent (MEASURED to 256x4x2, 128x64x3, 64x64x64);
    * 256 keeps the largest one at 64 MiB of VRAM. */
   caps->max_texture_cube_levels = 5;
   caps->max_texture_3d_levels = 9;
   caps->max_texture_array_layers = 0;
   /* 7d[9:0] is a signed bias with five fraction bits (CAPTURED,
    * docs/parhelia-m9-gl15-ff-capture.md), which m9_texture_unit_init
    * clamps to -16 .. 15.97. */
   caps->max_texture_lod_bias = 15.0f;
   caps->max_viewports = 1;
   caps->max_vertex_buffers = 16;
   caps->max_vertex_attrib_stride = 2048;
   caps->constant_buffer_offset_alignment = 16;
   caps->min_map_buffer_alignment = 64;
   caps->vendor_id = 0x102b;
   caps->device_id = M9_PCI_ID;
   caps->endianness = PIPE_ENDIAN_NATIVE;
   /* Vertex programs run in the draw module; fragment shading is limited to
    * the interpolated primary color (m9_context.c). */
   caps->glsl_feature_level = 120;
   caps->glsl_feature_level_compatibility = 120;
   caps->essl_feature_level = 100;
   caps->npot_textures = true;
   /* Rectangle targets: unnormalized coordinates reach the driver, which
    * folds the reciprocal of the sampled span into them per vertex
    * (m9_job_textures).  The engine itself normalizes over the descriptor
    * extent and has no unnormalized mode. */
   caps->texrect = true;
   /* No fixed-function alpha test: st lowers it into the fragment shader. */
   caps->alpha_test = false;
   /* ES 2.0 for glamor; factors lower into the fragment program, and
    * equations other than ADD skip the draw with a warning. */
   caps->blend_equation_separate = true;
   caps->user_vertex_buffers = true;
   caps->vertex_color_clamped = true;
   caps->vertex_color_unclamped = true;
   caps->fs_coord_origin_upper_left = true;
   caps->fs_coord_pixel_center_half_integer = true;
   caps->clear_scissored = true;
   caps->clear_masked = true;
   caps->shareable_shaders = false;
   caps->max_varyings = 8;
   caps->min_point_size = caps->min_point_size_aa = 1.0f;
   caps->min_line_width = caps->min_line_width_aa = 1.0f;
   caps->max_point_size = caps->max_point_size_aa = 255.0f;
   caps->max_line_width = caps->max_line_width_aa = 255.0f;
   caps->point_size_granularity = caps->line_width_granularity = 0.1f;
   /* The draw module names a sprite coordinate by its TEXCOORD semantic, the
    * one it scans fragment inputs with; GENERIC bits never matched one. */
   caps->tgsi_texcoord = true;
   caps->compute = false;
   /* Run finalize_nir at link time, so the inlinable uniforms the fragment
    * backend marks reach gl_program::info and st sends their values; without
    * it set_inlinable_constants is never called and every specialised variant
    * compiles against zeros. */
   caps->call_finalize_nir_in_linker = true;
   /* The engine publishes a samples-passed counter around a range of draws
    * (m9_query.c); the count is exact and the result is read from memory,
    * so nothing about it stalls the engine. */
   caps->occlusion_query = true;
   caps->dmabuf = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT;
   screen->base.destroy = m9_destroy;
   screen->base.finalize_nir = m9_finalize_nir;
   screen->base.context_create = m9_context_create;
   screen->base.get_name = m9_name;
   screen->base.get_vendor = m9_vendor;
   screen->base.get_device_vendor = m9_device_vendor;
   screen->base.is_format_supported = m9_is_format_supported;
   screen->base.resource_create = m9_resource_create;
   screen->base.resource_create_with_modifiers =
      m9_resource_create_with_modifiers;
   screen->base.query_dmabuf_modifiers = m9_query_dmabuf_modifiers;
   screen->base.is_dmabuf_modifier_supported =
      m9_is_dmabuf_modifier_supported;
   screen->base.resource_from_handle = m9_resource_from_handle;
   screen->base.resource_get_handle = m9_resource_get_handle;
   screen->base.check_resource_capability = m9_check_resource_capability;
   screen->base.resource_destroy = m9_resource_destroy;
   screen->base.flush_frontbuffer = m9_flush_frontbuffer;
   screen->base.fence_reference = m9_fence_reference;
   screen->base.fence_finish = m9_fence_finish;
   return &screen->base;
fail:
   if (screen->fd >= 0)
      close(screen->fd);
   FREE(screen);
   return NULL;
}
