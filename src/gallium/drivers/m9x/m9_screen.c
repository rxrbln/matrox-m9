/* SPDX-License-Identifier: MIT */
#include "m9_screen.h"
#include "m9x_public.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xf86drm.h>
#include "drm-uapi/drm.h"
#include "drm-uapi/drm_fourcc.h"
#include "drm-uapi/drm_mode.h"
#include "m9_batch.h"
#include "m9_format.h"
#include "m9_internal.h"
#include "util/u_memory.h"
#include "util/u_inlines.h"
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

static bool
m9_presentation_template(const struct pipe_resource *templ)
{
   const unsigned presentation = PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET |
                                 PIPE_BIND_SHARED;

   return templ->target == PIPE_TEXTURE_2D && templ->width0 == 1024 &&
          templ->height0 == 768 && templ->depth0 == 1 &&
          templ->array_size == 1 && !templ->last_level &&
          templ->nr_samples <= 1 && templ->nr_storage_samples <= 1 &&
          templ->format == PIPE_FORMAT_B8G8R8X8_UNORM &&
          (templ->bind & (PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET)) &&
          !(templ->bind & ~(presentation | PIPE_BIND_RENDER_TARGET |
                            PIPE_BIND_LINEAR));
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
                               PIPE_BIND_LINEAR;
   unsigned required = 0;

   (void)screen;
   if (samples > 1 || storage_samples > 1)
      return false;
   if (target == PIPE_BUFFER)
      return bind && !(bind & ~(PIPE_BIND_VERTEX_BUFFER |
                                PIPE_BIND_INDEX_BUFFER |
                                PIPE_BIND_CONSTANT_BUFFER));
   if (target != PIPE_TEXTURE_1D && target != PIPE_TEXTURE_2D &&
       target != PIPE_TEXTURE_RECT)
      return false;
   if (!m9_format_info(format, &info))
      return false;
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
   if (bind & (PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET |
               PIPE_BIND_SHARED))
      return target == PIPE_TEXTURE_2D &&
             (format == PIPE_FORMAT_B8G8R8X8_UNORM ||
              format == PIPE_FORMAT_B8G8R8A8_UNORM) &&
             !(bind & ~(PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET |
                        PIPE_BIND_SHARED | PIPE_BIND_RENDER_TARGET |
                        PIPE_BIND_BLENDABLE | PIPE_BIND_SAMPLER_VIEW |
                        PIPE_BIND_LINEAR));
   /* bind==0 and LINEAR-only images are CPU staging resources. */
   return !(required & ~info.usage);
}

static int
m9_resource_map_dumb(struct m9_screen *screen, struct m9_resource *resource)
{
   struct drm_mode_map_dumb map = { .handle = resource->handle };

   if (ioctl(screen->fd, DRM_IOCTL_MODE_MAP_DUMB, &map))
      return -errno;
   resource->mmap_offset = map.offset;
   resource->map = mmap(NULL, resource->size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, screen->fd, map.offset);
   if (resource->map == MAP_FAILED) {
      resource->map = NULL;
      return -errno;
   }
   resource->map_writable = true;
   return 0;
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
      struct drm_gem_close close_bo = { .handle = handle };

      ioctl(screen->fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
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
   entry->owns_handle = resource->handle_owned || resource->dumb_owned;
   entry->destroy_dumb = resource->dumb_owned;
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
         if (entry->owns_handle && entry->destroy_dumb) {
            struct drm_mode_destroy_dumb destroy = {
               .handle = entry->handle,
            };

            ioctl(screen->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
         } else if (entry->owns_handle) {
            struct drm_gem_close close_bo = { .handle = entry->handle };

            ioctl(screen->fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
         }
         FREE(entry);
      }
      break;
   }
   simple_mtx_unlock(&screen->import_lock);
}

static int
m9_resource_create_dumb(struct m9_screen *screen,
                        struct m9_resource *resource)
{
   struct drm_mode_create_dumb create = {
      .width = 1024,
      .height = 768,
      .bpp = 32,
   };
   int ret;

   if (ioctl(screen->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create))
      return -errno;
   if (create.pitch != 4096 || create.size < 4096ull * 768) {
      struct drm_mode_destroy_dumb destroy = { .handle = create.handle };
      ioctl(screen->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
      return -EINVAL;
   }
   resource->handle = create.handle;
   resource->size = create.size;
   resource->row_span = create.pitch / 4;
   resource->domain = M9_BO_VRAM;
   resource->dumb_owned = true;
   ret = m9_resource_map_dumb(screen, resource);
   if (ret) {
      struct drm_mode_destroy_dumb destroy = { .handle = create.handle };
      ioctl(screen->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
      resource->handle = 0;
      resource->dumb_owned = false;
   }
   return ret;
}

static struct pipe_resource *
m9_resource_create(struct pipe_screen *pscreen,
                   const struct pipe_resource *templ)
{
   struct m9_screen *screen = m9_screen(pscreen);
   struct m9_resource *resource = CALLOC_STRUCT(m9_resource);
   struct drm_parhelia_m9_create_bo create = {0};
   struct m9_format_info format = {0};
   uint64_t size;

   if (!resource)
      return NULL;
   resource->map_fd = -1;
   if (!m9_is_format_supported(pscreen, templ->format, templ->target,
                               templ->nr_samples, templ->nr_storage_samples,
                               templ->bind))
      goto fail;
   if (m9_presentation_template(templ)) {
      resource->kind = M9_RESOURCE_LINEAR_COLOR;
      format.cpp = 4;
      if (m9_resource_create_dumb(screen, resource))
         goto fail;
      size = resource->size;
   } else if (templ->target == PIPE_BUFFER) {
      size = templ->width0;
      resource->kind = M9_RESOURCE_LINEAR_BUFFER;
      format.cpp = 1;
   } else {
      if (!m9_format_info(templ->format, &format) || templ->depth0 != 1 ||
          templ->array_size != 1 || templ->last_level >= M9_MAX_MIP_LEVELS ||
          templ->width0 > 2048 || templ->height0 > 2048)
         goto fail;
      resource->kind = templ->bind & PIPE_BIND_DEPTH_STENCIL ?
         M9_RESOURCE_LINEAR_DEPTH : templ->bind & PIPE_BIND_RENDER_TARGET ?
         M9_RESOURCE_LINEAR_COLOR : M9_RESOURCE_LINEAR_TEXTURE;
      if (resource->kind != M9_RESOURCE_LINEAR_TEXTURE && templ->last_level)
         goto fail;
      if (resource->kind == M9_RESOURCE_LINEAR_TEXTURE) {
         if (m9_mip_layout_init(&resource->mip, templ->width0,
                                templ->height0, 1, templ->last_level + 1,
                                format.cpp))
            goto fail;
         resource->row_span = resource->mip.level[0].row_span;
         size = resource->mip.size;
      } else {
         resource->row_span = m9_resource_row_span(templ->width0);
         size = (uint64_t)resource->row_span *
                m9_render_surface_height(templ->height0) * format.cpp;
      }
   }
   if (!resource->handle) {
      create.size = size;
      create.flags = m9_resource_bo_flags(templ);
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
      resource->domain = create.flags & PARHELIA_M9_BO_VRAM ?
                         M9_BO_VRAM : M9_BO_DMA32;
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
   return &resource->base;
fail:
   if (resource->map)
      munmap(resource->map, resource->size);
   if (resource->dumb_owned && resource->handle) {
      struct drm_mode_destroy_dumb destroy = { .handle = resource->handle };
      ioctl(screen->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
   }
   FREE(resource);
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
       whandle->offset || !m9_format_info(templ->format, &format) ||
       !format.cpp || whandle->stride % format.cpp ||
       whandle->stride / format.cpp < m9_resource_row_span(templ->width0) ||
       whandle->stride / format.cpp > UINT16_MAX ||
       (whandle->modifier != DRM_FORMAT_MOD_INVALID &&
        whandle->modifier != DRM_FORMAT_MOD_LINEAR))
      return NULL;
   required = (uint64_t)whandle->stride *
              ((templ->bind & (PIPE_BIND_RENDER_TARGET |
                               PIPE_BIND_DEPTH_STENCIL)) ?
               m9_render_surface_height(templ->height0) : templ->height0);
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
      resource->handle_owned = true;
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
   resource->domain = M9_BO_VRAM;
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
      ret = m9_resource_map_dumb(screen, resource);
   }
   if (ret)
      goto fail;
   resource->base = *templ;
   resource->base.screen = pscreen;
   pipe_reference_init(&resource->base.reference, 1);
   resource->kind = templ->bind & PIPE_BIND_DEPTH_STENCIL ?
      M9_RESOURCE_LINEAR_DEPTH :
      (templ->bind & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_SCANOUT |
                      PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SHARED)) ?
      M9_RESOURCE_LINEAR_COLOR : M9_RESOURCE_LINEAR_TEXTURE;
   resource->width = templ->width0;
   resource->height = templ->height0;
   resource->depth = 1;
   resource->cpp = format.cpp;
   resource->format = format.code;
   resource->layout = 1;
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
   if (!whandle || !resource->handle || resource->domain == M9_BO_DMA32)
      return false;
   whandle->stride = resource->row_span * resource->cpp;
   whandle->offset = 0;
   whandle->modifier = DRM_FORMAT_MOD_LINEAR;
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

   if (resource->map)
      munmap(resource->map, resource->size);
   if (resource->map_fd >= 0)
      close(resource->map_fd);
   if (resource->prime_import) {
      m9_prime_import_put(m9_screen(pscreen), resource->prime_import);
   } else if (resource->dumb_owned && resource->handle) {
      struct drm_mode_destroy_dumb destroy = { .handle = resource->handle };
      ioctl(m9_screen(pscreen)->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
   } else if (resource->handle_owned && resource->handle) {
      ioctl(m9_screen(pscreen)->fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
   }
   FREE(resource);
}

int
m9_command_bo_create(struct m9_screen *screen, struct m9_resource *resource)
{
   struct drm_parhelia_m9_create_bo create = {
      .size = 32768,
      .flags = PARHELIA_M9_BO_COMMAND,
   };

   if (!screen || !resource)
      return -EINVAL;
   if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_M9_CREATE_BO, &create))
      return -errno;
   memset(resource, 0, sizeof(*resource));
   resource->map_fd = -1;
   resource->map = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        screen->fd, create.mmap_offset);
   if (resource->map == MAP_FAILED) {
      int error = errno;
      struct drm_gem_close close_bo = { .handle = create.handle };
      resource->map = NULL;
      ioctl(screen->fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
      return -error;
   }
   resource->map_writable = true;
   resource->handle = create.handle;
   resource->kind = M9_RESOURCE_COMMAND;
   resource->size = create.size;
   resource->mmap_offset = create.mmap_offset;
   resource->domain = M9_BO_DMA32;
   resource->handle_owned = true;
   return 0;
}

void
m9_command_bo_destroy(struct m9_screen *screen, struct m9_resource *resource)
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

struct pipe_fence_handle *
m9_fence_create(uint64_t point, bool signaled)
{
   struct m9_fence *fence = CALLOC_STRUCT(m9_fence);

   if (!fence)
      return NULL;
   pipe_reference_init(&fence->reference, 1);
   fence->point = point;
   fence->signaled = signaled;
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

   (void)screen;
   (void)ctx;
   (void)timeout;
   /* Both currently defined M9 fence-producing ioctls complete
    * synchronously.  A future asynchronous UAPI needs an explicit wait
    * ioctl before execute_submit can be enabled. */
   return !m9f || m9f->signaled;
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
   return !(bind & PIPE_BIND_LINEAR) || resource->layout == 1;
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

static void
m9_destroy(struct pipe_screen *pscreen)
{
   struct m9_screen *screen = m9_screen(pscreen);

   while (screen->imports) {
      struct m9_prime_import *entry = screen->imports;

      screen->imports = entry->next;
      if (entry->owns_handle && entry->destroy_dumb) {
         struct drm_mode_destroy_dumb destroy = {
            .handle = entry->handle,
         };

         ioctl(screen->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
      } else if (entry->owns_handle) {
         struct drm_gem_close close_bo = { .handle = entry->handle };

         ioctl(screen->fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
      }
      FREE(entry);
   }
   simple_mtx_destroy(&screen->import_lock);
   close(screen->fd);
   FREE(pscreen);
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
   screen->chip_id = M9_PCI_ID;
   screen->uapi_version = 1;
   screen->validate_submit = true;
   screen->execute_submit = false;
   screen->linked_program_ready = false;
   screen->base.get_screen_fd = m9_fd;
   u_init_pipe_screen_caps(&screen->base, 1);
   caps = (struct pipe_caps *)&screen->base.caps;
   caps->max_render_targets = 1;
   caps->max_texture_2d_size = 2048;
   caps->max_texture_array_layers = 0;
   caps->max_viewports = 1;
   caps->max_vertex_buffers = 16;
   caps->max_vertex_attrib_stride = 2048;
   caps->constant_buffer_offset_alignment = 16;
   caps->min_map_buffer_alignment = 64;
   caps->vendor_id = 0x102b;
   caps->device_id = M9_PCI_ID;
   caps->endianness = PIPE_ENDIAN_NATIVE;
   caps->glsl_feature_level = 0;
   caps->glsl_feature_level_compatibility = 0;
   caps->compute = false;
   caps->dmabuf = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT;
   screen->base.destroy = m9_destroy;
   screen->base.context_create = m9_context_create;
   screen->base.get_name = m9_name;
   screen->base.get_vendor = m9_vendor;
   screen->base.get_device_vendor = m9_device_vendor;
   screen->base.is_format_supported = m9_is_format_supported;
   screen->base.resource_create = m9_resource_create;
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
