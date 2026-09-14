/* SPDX-License-Identifier: MIT */
#include "m9_screen.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "drm-uapi/drm.h"
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
m9_is_format_supported(struct pipe_screen *screen, enum pipe_format format,
                       enum pipe_texture_target target, unsigned samples,
                       unsigned storage_samples, unsigned bind)
{
   struct m9_format_info info;
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
   if (bind & PIPE_BIND_SAMPLER_VIEW)
      required |= M9_FORMAT_TEXTURE;
   if (bind & PIPE_BIND_RENDER_TARGET)
      required |= M9_FORMAT_COLOR;
   if (bind & PIPE_BIND_DEPTH_STENCIL)
      required |= M9_FORMAT_DEPTH;
   if (bind & (PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET |
               PIPE_BIND_SHARED | PIPE_BIND_SHADER_IMAGE))
      return false;
   return required && !(required & ~info.usage);
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
   if (!m9_is_format_supported(pscreen, templ->format, templ->target,
                               templ->nr_samples, templ->nr_storage_samples,
                               templ->bind))
      goto fail;
   if (templ->target == PIPE_BUFFER) {
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
         resource->row_span = (templ->width0 + 7) & ~7u;
         size = (uint64_t)resource->row_span * templ->height0 * format.cpp;
      }
   }
   create.size = size;
   if (ioctl(screen->fd, DRM_IOCTL_PARHELIA_M9_CREATE_BO, &create))
      goto fail;
   resource->map = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        screen->fd, create.mmap_offset);
   if (resource->map == MAP_FAILED) {
      struct drm_gem_close close_bo = { .handle = create.handle };
      resource->map = NULL;
      ioctl(screen->fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
      goto fail;
   }
   resource->base = *templ;
   resource->base.screen = pscreen;
   pipe_reference_init(&resource->base.reference, 1);
   resource->handle = create.handle;
   resource->size = create.size;
   resource->mmap_offset = create.mmap_offset;
   resource->width = templ->width0;
   resource->height = templ->height0;
   resource->depth = templ->depth0;
   resource->cpp = format.cpp;
   resource->format = format.code;
   resource->layout = 1; /* exact ordinary linear descriptor class */
   return &resource->base;
fail:
   FREE(resource);
   return NULL;
}

static void
m9_resource_destroy(struct pipe_screen *pscreen, struct pipe_resource *pres)
{
   struct m9_resource *resource = m9_resource(pres);
   struct drm_gem_close close_bo = { .handle = resource->handle };

   if (resource->map)
      munmap(resource->map, resource->size);
   if (resource->handle)
      ioctl(m9_screen(pscreen)->fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
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
   resource->map = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        screen->fd, create.mmap_offset);
   if (resource->map == MAP_FAILED) {
      int error = errno;
      struct drm_gem_close close_bo = { .handle = create.handle };
      resource->map = NULL;
      ioctl(screen->fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
      return -error;
   }
   resource->handle = create.handle;
   resource->kind = M9_RESOURCE_COMMAND;
   resource->size = create.size;
   resource->mmap_offset = create.mmap_offset;
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
}

static struct pipe_context *
m9_context_create_gated(struct pipe_screen *screen, void *priv, unsigned flags)
{
   (void)screen;
   (void)priv;
   (void)flags;
   /* The kernel currently validates streams and can execute only its owned
    * no-op fence.  Returning NULL prevents a state tracker from treating the
    * recovered packet builders as a safe arbitrary-program submit path. */
   fprintf(stderr, "parhelia: M9 rendering awaits executable-stream UAPI\n");
   return NULL;
}

static void
m9_destroy(struct pipe_screen *pscreen)
{
   close(m9_screen(pscreen)->fd);
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
   screen->chip_id = M9_PCI_ID;
   screen->uapi_version = 1;
   screen->validate_submit = true;
   screen->execute_submit = false;
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
   caps->dmabuf = 0;
   screen->base.destroy = m9_destroy;
   screen->base.context_create = m9_context_create_gated;
   screen->base.get_name = m9_name;
   screen->base.get_vendor = m9_vendor;
   screen->base.get_device_vendor = m9_device_vendor;
   screen->base.is_format_supported = m9_is_format_supported;
   screen->base.resource_create = m9_resource_create;
   screen->base.resource_destroy = m9_resource_destroy;
   return &screen->base;
fail:
   if (screen->fd >= 0)
      close(screen->fd);
   FREE(screen);
   return NULL;
}
