// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/interrupt.h>
#include <linux/iosys-map.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/overflow.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_color_mgmt.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_vram_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "../matrox_common/matrox_kms_common.h"
#include "m9_drv.h"
#include "m9_reg.h"

#define M9_WARM_WIDTH		1024u
#define M9_WARM_HEIGHT		768u
#define M9_WARM_PITCH		4096u
#define M9_WARM_RENDER_BYTES	0x00301000u
#define M9_WARM_CLOCK_KHZ	65000u
/* Scanout surfaces use the tiled 0x21 layout; DC pitch and window descriptor. */
#define M9_SCANOUT_FORMAT	0xcc210400u
#define M9_SCANOUT_EXTENT	0x00010300u
#define M9_SCANOUT_BYTES	(M9_WARM_PITCH * M9_WARM_HEIGHT)

static bool m9_kms;
module_param(m9_kms, bool, 0400);
MODULE_PARM_DESC(m9_kms,
	"Expose guarded KMS only when the tested 1024x768 warm state is present");

static bool m9_kms_scanout;
module_param(m9_kms_scanout, bool, 0400);

static bool m9_kms_direct_scanout = true;
module_param(m9_kms_direct_scanout, bool, 0600);
MODULE_PARM_DESC(m9_kms_direct_scanout,
	"Scan out tiled framebuffers in place instead of copying them");
MODULE_PARM_DESC(m9_kms_scanout,
	"Permit direct BAR0 VRAM scanout address changes (requires m9_kms=1)");

bool m9x_kms_enabled(void)
{
	return m9_kms;
}

bool m9x_kms_scanout_enabled(void)
{
	return m9_kms && m9_kms_scanout;
}

static const struct drm_display_mode m9_warm_mode = {
	.clock = M9_WARM_CLOCK_KHZ,
	.hdisplay = 1024,
	.hsync_start = 1048,
	.hsync_end = 1184,
	.htotal = 1344,
	.vdisplay = 768,
	.vsync_start = 771,
	.vsync_end = 777,
	.vtotal = 806,
	.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

struct m9_warm_snapshot {
	u32 ctrl;
	u32 xstart;
	u32 xend;
	u32 ystart;
	u32 yend;
	u32 start;
	u32 pitch;
	u32 hblank;
	u32 htotal;
	u32 hsync;
	u32 vblank;
	u32 vtotal;
	u32 vsync;
	u32 vsyncend;
	u32 vgaflag;
	u32 vgamisc;
	u32 dvo;
};

static bool m9_warm_snapshot_valid(const struct m9_warm_snapshot *s)
{
	return s->ctrl == 0x33046001 &&
	       s->xstart == 0 && s->xend == 0x03ff0000 &&
	       s->ystart == 0 && s->yend == 0x000002ff &&
	       s->start == 0 && s->pitch == 0xcc210400 &&
	       s->hblank == 0x053f03ff && s->htotal == 0x03ff053f &&
	       s->hsync == 0x049f0417 &&
	       s->vblank == 0x032502ff && s->vtotal == 0x02ff0325 &&
	       s->vsync == 0x03080302 && s->vsyncend == 0x00000324 &&
	       s->vgaflag == M9_VGAFLAG_NATIVE &&
	       s->vgamisc == M9_VGAMISC_NATIVE && s->dvo == 0x0000a800;
}

static int m9_kms_selftest(void)
{
	struct m9_warm_snapshot s = {
		.ctrl = 0x33046001,
		.xend = 0x03ff0000,
		.yend = 0x000002ff,
		.pitch = 0xcc210400,
		.hblank = 0x053f03ff,
		.htotal = 0x03ff053f,
		.hsync = 0x049f0417,
		.vblank = 0x032502ff,
		.vtotal = 0x02ff0325,
		.vsync = 0x03080302,
		.vsyncend = 0x00000324,
		.vgaflag = M9_VGAFLAG_NATIVE,
		.vgamisc = M9_VGAMISC_NATIVE,
		.dvo = 0x0000a800,
	};

	if (!m9_warm_snapshot_valid(&s))
		return -EINVAL;
	s.pitch++;
	if (m9_warm_snapshot_valid(&s))
		return -EINVAL;
	if (matrox_kms_mode_geometry_valid(&m9_warm_mode, 1024, 768,
					     65000, 65000) != MODE_OK)
		return -EINVAL;
	if (M9_WARM_PITCH * M9_WARM_HEIGHT != 0x00300000u ||
	    M9_WARM_RENDER_BYTES != 0x00301000u)
		return -EINVAL;
	return 0;
}

static void m9_capture_warm_snapshot(struct m9x_device *mdev,
				     struct m9_warm_snapshot *s)
{
	s->ctrl = m9x_read(mdev, M9_DC0 + M9_DC_CTRL);
	s->xstart = m9x_read(mdev, M9_DC0 + M9_DC_XSTART);
	s->xend = m9x_read(mdev, M9_DC0 + M9_DC_XEND);
	s->ystart = m9x_read(mdev, M9_DC0 + M9_DC_YSTART);
	s->yend = m9x_read(mdev, M9_DC0 + M9_DC_YEND);
	s->start = m9x_read(mdev, M9_DC0 + M9_DC_START);
	s->pitch = m9x_read(mdev, M9_DC0 + M9_DC_PITCH);
	s->hblank = m9x_read(mdev, M9_DCT0 + M9_DCT_HBLANK);
	s->htotal = m9x_read(mdev, M9_DCT0 + M9_DCT_HTOTAL);
	s->hsync = m9x_read(mdev, M9_DCT0 + M9_DCT_HSYNC);
	s->vblank = m9x_read(mdev, M9_DCT0 + M9_DCT_VBLANK);
	s->vtotal = m9x_read(mdev, M9_DCT0 + M9_DCT_VTOTAL);
	s->vsync = m9x_read(mdev, M9_DCT0 + M9_DCT_VSYNC);
	s->vsyncend = m9x_read(mdev, M9_DCT0 + M9_DCT_VSYNCEND);
	s->vgaflag = m9x_read(mdev, M9_REG_VGAFLAG);
	s->vgamisc = m9x_read(mdev, M9_REG_VGAMISC);
	s->dvo = m9x_read(mdev, M9_REG_DVO0);
}

static u32 m9_kms_read(void *cookie, u32 reg)
{
	return m9x_read(cookie, reg);
}

static void m9_kms_write(void *cookie, u32 reg, u32 value)
{
	m9x_write(cookie, reg, value);
}

static struct matrox_kms_irq m9_kms_irq_desc(struct m9x_device *mdev)
{
	return (struct matrox_kms_irq) {
		.cookie = mdev,
		.read = m9_kms_read,
		.write = m9_kms_write,
		.raw_reg = M9_REG_IRQ_LOW_RAW,
		.ack_reg = M9_REG_IRQ_LOW_ACK,
		.mask_reg = M9_REG_IRQ_LOW_MASK,
		.owned_mask = M9_IRQ_LOW_VBLANK0,
		.vblank_mask = M9_IRQ_LOW_VBLANK0,
	};
}

static int m9_connector_detect(struct drm_connector *connector,
			       struct drm_modeset_acquire_ctx *ctx, bool force)
{
	return connector_status_connected;
}

static int m9_connector_get_modes(struct drm_connector *connector)
{
	struct m9x_device *mdev = to_m9x(connector->dev);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &mdev->warm_mode);
	if (!mode)
		return 0;
	drm_mode_probed_add(connector, mode);
	return 1;
}

static const struct drm_connector_helper_funcs m9_connector_helper_funcs = {
	.get_modes = m9_connector_get_modes,
	.detect_ctx = m9_connector_detect,
};

static enum drm_mode_status
m9_pipe_mode_valid(struct drm_simple_display_pipe *pipe,
		   const struct drm_display_mode *mode)
{
	struct m9x_device *mdev = to_m9x(pipe->crtc.dev);
	enum drm_mode_status status;

	status = matrox_kms_mode_geometry_valid(mode, M9_WARM_WIDTH,
						 M9_WARM_HEIGHT,
						 M9_WARM_CLOCK_KHZ,
						 M9_WARM_CLOCK_KHZ);
	if (status != MODE_OK)
		return status;
	return drm_mode_equal(mode, &mdev->warm_mode) ? MODE_OK : MODE_BAD;
}

static int m9_pipe_check(struct drm_simple_display_pipe *pipe,
			 struct drm_plane_state *plane_state,
			 struct drm_crtc_state *crtc_state)
{
	struct drm_framebuffer *fb = plane_state->fb;
	struct drm_gem_object *gem;
	int ret;

	ret = matrox_kms_plane_geometry_check(plane_state, crtc_state);
	if (ret)
		return ret;
	if (crtc_state->enable &&
	    !drm_mode_equal(&crtc_state->mode, &to_m9x(pipe->crtc.dev)->warm_mode))
		return -EINVAL;
	if (crtc_state->color_mgmt_changed) {
		if (crtc_state->gamma_lut &&
		    drm_color_lut_size(crtc_state->gamma_lut) != 256)
			return -EINVAL;
		crtc_state->planes_changed = true;
	}
	if (!fb)
		return 0;
	if (!m9x_kms_scanout_enabled() ||
	    fb->format->format != DRM_FORMAT_XRGB8888 ||
	    fb->pitches[0] != M9_WARM_PITCH || fb->offsets[0] ||
	    fb->width != M9_WARM_WIDTH || fb->height != M9_WARM_HEIGHT)
		return -EINVAL;
	gem = drm_gem_fb_get_obj(fb, 0);
	if (!m9x_gem_is_vram(gem))
		return -EINVAL;
	return 0;
}

/*
 * Userspace draws linear XRGB8888 into its dumb BO.  Copy those bytes into the
 * kernel-owned tiled scanout surface through host-aperture slot 0 carrying the
 * scanout descriptor, then release that window.
 */
static int m9_scanout_publish(struct m9x_device *mdev,
			      struct drm_framebuffer *fb)
{
	struct drm_gem_object *gem = drm_gem_fb_get_obj(fb, 0);
	struct drm_gem_vram_object *src = drm_gem_vram_of_gem(gem);
	bool tiled = m9x_gem_vram_tiled(gem);
	struct iosys_map map;
	s64 offset, src_offset = 0;
	int ret;

	if (!mdev->scanout || !mdev->bar0_window || !mdev->scanout_bounce)
		return -ENODEV;
	offset = drm_gem_vram_offset(mdev->scanout);
	if (offset < 0)
		return offset;
	if (tiled) {
		src_offset = drm_gem_vram_offset(src);
		if (src_offset < 0)
			return src_offset;
	} else {
		ret = ttm_bo_reserve(&src->bo, true, false, NULL);
		if (ret)
			return ret;
		ret = drm_gem_vram_vmap(src, &map);
		if (!ret) {
			iosys_map_memcpy_from(mdev->scanout_bounce, &map, 0,
					      M9_SCANOUT_BYTES);
			drm_gem_vram_vunmap(src, &map);
		}
		ttm_bo_unreserve(&src->bo);
		if (ret)
			return ret;
	}
	mutex_lock(&mdev->transport.lock);
	/* GPU-rendered frames are tiled: de-tile them through the window. */
	if (tiled)
		m9x_surface_read_locked(mdev, lower_32_bits(src_offset),
					M9_SCANOUT_FORMAT, M9_SCANOUT_EXTENT,
					mdev->scanout_bounce, M9_SCANOUT_BYTES);
	m9x_address_surface_window(mdev, lower_32_bits(offset), M9_SCANOUT_FORMAT,
			       M9_SCANOUT_EXTENT);
	memcpy_toio(mdev->bar0_window, mdev->scanout_bounce, M9_SCANOUT_BYTES);
	/* Complete WC stores before the window is released. */
	wmb();
	m9x_address_surface_release(mdev);
	mutex_unlock(&mdev->transport.lock);
	return 0;
}

static int m9_scanout_address(struct m9x_device *mdev, u32 *address)
{
	u64 limit = min_t(u64, mdev->vram_size - 1, M9_DC_START_MASK);

	if (!mdev->scanout)
		return -ENODEV;
	return matrox_kms_vram_offset(&mdev->scanout->bo.base, 1024, limit,
				      address);
}

/* A tiled framebuffer of the scanout geometry can be fetched where it is,
 * which skips the read and write of the whole surface per frame. */
static int m9_scanout_direct_address(struct m9x_device *mdev,
				     struct drm_framebuffer *fb, u32 *address)
{
	struct drm_gem_object *gem = fb ? drm_gem_fb_get_obj(fb, 0) : NULL;
	u64 limit = min_t(u64, mdev->vram_size - 1, M9_DC_START_MASK);

	if (!m9_kms_direct_scanout || !gem || !m9x_gem_vram_tiled(gem))
		return -ENODEV;
	return matrox_kms_vram_offset(gem, 1024, limit, address);
}

static void m9_program_scanout(struct drm_simple_display_pipe *pipe,
			       struct drm_framebuffer *fb)
{
	struct m9x_device *mdev = to_m9x(pipe->crtc.dev);
	u32 address, pitch;

	if (!m9_scanout_direct_address(mdev, fb, &address)) {
		pitch = (mdev->warm_dc_pitch & ~M9_DC_PITCH_MASK) |
			M9_WARM_WIDTH;
		wmb();
		m9x_write(mdev, M9_DC0 + M9_DC_START,
			  address & M9_DC_START_MASK);
		m9x_write(mdev, M9_DC0 + M9_DC_PITCH, pitch);
		return;
	}
	if (!fb || m9_scanout_publish(mdev, fb) ||
	    m9_scanout_address(mdev, &address))
		return;
	pitch = (mdev->warm_dc_pitch & ~M9_DC_PITCH_MASK) | M9_WARM_WIDTH;
	/* Publish CPU writes before the display controller fetches this BO. */
	wmb();
	m9x_write(mdev, M9_DC0 + M9_DC_START, address & M9_DC_START_MASK);
	m9x_write(mdev, M9_DC0 + M9_DC_PITCH, pitch);
}

static void m9_pipe_load_gamma(struct drm_simple_display_pipe *pipe,
			       const struct drm_property_blob *blob)
{
	struct m9x_device *mdev = to_m9x(pipe->crtc.dev);
	u32 ctrl = m9x_read(mdev, M9_DC0 + M9_DC_CTRL);

	/* The M9 DC LUT is the same ten-bit interface used by Parhelia. */
	m9x_write(mdev, M9_DC0 + M9_DC_CTRL, ctrl & ~M9_DC_LUT_BYPASS);
	matrox_kms_write_lut(mdev, m9_kms_write,
			       M9_DC0 + M9_DC_LUT_INDEX,
			       M9_DC0 + M9_DC_LUT_RED,
			       M9_DC0 + M9_DC_LUT_GREEN,
			       M9_DC0 + M9_DC_LUT_BLUE,
			       M9_DC_LUT_LATCH, false, blob);
}

static void m9_pipe_enable(struct drm_simple_display_pipe *pipe,
			   struct drm_crtc_state *crtc_state,
			   struct drm_plane_state *plane_state)
{
	struct m9x_device *mdev = to_m9x(pipe->crtc.dev);
	int idx;
	u32 ctrl;

	if (!drm_dev_enter(&mdev->drm, &idx))
		return;
	ctrl = m9x_read(mdev, M9_DC0 + M9_DC_CTRL);
	m9x_write(mdev, M9_DC0 + M9_DC_CTRL, ctrl & ~M9_DC_ENABLE);
	m9_program_scanout(pipe, plane_state->fb);
	m9_pipe_load_gamma(pipe, crtc_state->gamma_lut);
	/* Re-enable only the proven warm controller bit after the pinned new BO
	 * is selected.  Preserve the current LUT choice and every opaque bit. */
	ctrl = m9x_read(mdev, M9_DC0 + M9_DC_CTRL);
	ctrl |= mdev->warm_dc_ctrl & M9_DC_ENABLE;
	m9x_write(mdev, M9_DC0 + M9_DC_CTRL, ctrl);
	/* Timing, clocks and the trained DVO/DP path stay untouched. */
	drm_crtc_vblank_on(&pipe->crtc);
	drm_dev_exit(idx);
}

static void m9_pipe_disable_hw(struct drm_simple_display_pipe *pipe)
{
	struct m9x_device *mdev = to_m9x(pipe->crtc.dev);
	u32 ctrl = m9x_read(mdev, M9_DC0 + M9_DC_CTRL);

	/* Stop memory fetch before the old scanout BO is unpinned. */
	m9x_write(mdev, M9_DC0 + M9_DC_CTRL, ctrl & ~M9_DC_ENABLE);
}

static void m9_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct drm_device *drm = pipe->crtc.dev;
	int idx;

	if (!drm_dev_enter(drm, &idx))
		return;
	/* The external DP transmitter and raster timing remain trained/running. */
	matrox_kms_pipe_disable(pipe, m9_pipe_disable_hw);
	drm_dev_exit(idx);
}

static void m9_pipe_update_scanout(struct drm_simple_display_pipe *pipe,
				   struct drm_plane_state *old_state)
{
	/* Every update republishes the linear image into the tiled surface. */
	m9_program_scanout(pipe, pipe->plane.state->fb);
}

static int m9_fb_dirty(struct drm_framebuffer *fb, struct drm_file *file,
		       unsigned int flags, unsigned int color,
		       struct drm_clip_rect *clips, unsigned int num_clips)
{
	struct m9x_device *mdev = to_m9x(fb->dev);
	int ret = 0;

	drm_modeset_lock(&mdev->pipe.plane.mutex, NULL);
	/* Nothing to publish while the framebuffer is scanned out in place. */
	if (mdev->pipe.plane.state && mdev->pipe.plane.state->fb == fb &&
	    mdev->pipe.crtc.state && mdev->pipe.crtc.state->active &&
	    !(m9_kms_direct_scanout &&
	      m9x_gem_vram_tiled(drm_gem_fb_get_obj(fb, 0))))
		ret = m9_scanout_publish(mdev, fb);
	drm_modeset_unlock(&mdev->pipe.plane.mutex);
	return ret;
}

static const struct drm_framebuffer_funcs m9_fb_funcs = {
	.destroy = drm_gem_fb_destroy,
	.create_handle = drm_gem_fb_create_handle,
	.dirty = m9_fb_dirty,
};

static void m9_pipe_update(struct drm_simple_display_pipe *pipe,
			   struct drm_plane_state *old_state)
{
	struct drm_device *drm = pipe->crtc.dev;
	int idx;

	if (!drm_dev_enter(drm, &idx))
		return;
	matrox_kms_pipe_update(pipe, old_state, m9_pipe_update_scanout,
				 m9_pipe_load_gamma);
	drm_dev_exit(idx);
}

static int m9_pipe_enable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct m9x_device *mdev = to_m9x(pipe->crtc.dev);
	struct matrox_kms_irq desc = m9_kms_irq_desc(mdev);
	int idx, ret;

	if (!drm_dev_enter(&mdev->drm, &idx))
		return -ENODEV;
	ret = matrox_kms_enable_vblank(&desc);
	drm_dev_exit(idx);
	return ret;
}

static void m9_pipe_disable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct m9x_device *mdev = to_m9x(pipe->crtc.dev);
	struct matrox_kms_irq desc = m9_kms_irq_desc(mdev);
	int idx;

	if (!drm_dev_enter(&mdev->drm, &idx))
		return;
	matrox_kms_disable_vblank(&desc);
	drm_dev_exit(idx);
}

static const struct drm_simple_display_pipe_funcs m9_pipe_funcs = {
	.mode_valid = m9_pipe_mode_valid,
	.enable = m9_pipe_enable,
	.disable = m9_pipe_disable,
	.check = m9_pipe_check,
	.update = m9_pipe_update,
	.begin_fb_access = matrox_kms_pipe_begin_fb_access,
	.end_fb_access = matrox_kms_pipe_end_fb_access,
	.enable_vblank = m9_pipe_enable_vblank,
	.disable_vblank = m9_pipe_disable_vblank,
};

static const u32 m9_formats[] = {
	DRM_FORMAT_XRGB8888,
};

static struct drm_framebuffer *
m9_fb_create(struct drm_device *drm, struct drm_file *file,
	     const struct drm_format_info *info,
	     const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct drm_gem_object *gem;
	struct drm_framebuffer *fb;

	if (info->format != DRM_FORMAT_XRGB8888 || mode_cmd->offsets[0] ||
	    mode_cmd->width != M9_WARM_WIDTH ||
	    mode_cmd->height != M9_WARM_HEIGHT ||
	    mode_cmd->pitches[0] != M9_WARM_PITCH)
		return ERR_PTR(-EINVAL);
	gem = drm_gem_object_lookup(file, mode_cmd->handles[0]);
	if (!gem)
		return ERR_PTR(-ENOENT);
	if (!m9x_gem_is_vram(gem) || gem->size < M9_WARM_RENDER_BYTES) {
		fb = ERR_PTR(-EINVAL);
		goto out_put;
	}
	fb = drm_gem_fb_create_with_funcs(drm, file, info, mode_cmd,
					  &m9_fb_funcs);
out_put:
	drm_gem_object_put(gem);
	return fb;
}

static const struct drm_mode_config_funcs m9_mode_config_funcs = {
	.fb_create = m9_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static int m9_dumb_create(struct drm_file *file, struct drm_device *drm,
			  struct drm_mode_create_dumb *args)
{
	struct drm_mode_create_dumb alloc_args;
	int ret;

	if (!m9x_kms_scanout_enabled() || !args->width || !args->height ||
	    args->bpp != 32)
		return -EINVAL;
	/*
	 * The framebuffer and plane checks below still admit only the exact warm
	 * 1024x768 scanout.  Generic 32-bit dumb objects must remain available for
	 * small mapped Xorg software-cursor and scratch allocations; none can reach
	 * DC0 without first passing those stricter checks.  Page-align allocations
	 * so the tested front buffer keeps its 4096-byte pitch and DC placement.
	 */
	if (args->width != M9_WARM_WIDTH || args->height != M9_WARM_HEIGHT)
		return drm_gem_vram_fill_create_dumb(file, drm, 1,
						     M9_WARM_PITCH, args);

	/*
	 * The M9148 category-2 constructor adds eight end-pad elements when
	 * aligned geometry leaves no tail slack, then rounds to its 4-KiB byte
	 * quantum.  Preserve the visible 1024x768 geometry and 4096-byte pitch,
	 * but allocate the exact 0x301000-byte renderable backing.
	 */
	alloc_args = *args;
	alloc_args.height = M9_WARM_RENDER_BYTES / M9_WARM_PITCH;
	ret = drm_gem_vram_fill_create_dumb(file, drm, 1, M9_WARM_PITCH,
					    &alloc_args);
	if (ret)
		return ret;
	args->pitch = alloc_args.pitch;
	args->size = alloc_args.size;
	args->handle = alloc_args.handle;
	return 0;
}

int m9x_dumb_create(struct drm_file *file, struct drm_device *drm,
		    struct drm_mode_create_dumb *args)
{
	int idx, ret;

	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;
	ret = m9_dumb_create(file, drm, args);
	drm_dev_exit(idx);
	return ret;
}

irqreturn_t m9x_irq_handler(int irq, void *arg)
{
	struct m9x_device *mdev = arg;
	struct matrox_kms_irq desc = m9_kms_irq_desc(mdev);
	u32 active = matrox_kms_active_irq(&desc);

	if (!active)
		return IRQ_NONE;
	/*
	 * Low IRQ status is W1C.  Do not consume masked sources: a warm live
	 * sample had several of them pending while the low mask was zero.
	 */
	m9x_write(mdev, M9_REG_IRQ_LOW_ACK, active);
	drm_crtc_handle_vblank(&mdev->pipe.crtc);
	return IRQ_HANDLED;
}

static void m9_scanout_fini(struct m9x_device *mdev)
{
	if (mdev->scanout) {
		m9x_vram_unpin(mdev->scanout);
		drm_gem_vram_put(mdev->scanout);
		mdev->scanout = NULL;
	}
	kvfree(mdev->scanout_bounce);
	mdev->scanout_bounce = NULL;
}

static int m9_scanout_init(struct m9x_device *mdev)
{
	struct pci_dev *pci = mdev->pdev;
	int ret;

	mdev->scanout_bounce = kvzalloc(M9_SCANOUT_BYTES, GFP_KERNEL);
	if (!mdev->bar0_window || !mdev->scanout_bounce) {
		ret = -ENOMEM;
		goto fail;
	}
	mdev->scanout = drm_gem_vram_create(&mdev->drm, M9_WARM_RENDER_BYTES, 1);
	if (IS_ERR(mdev->scanout)) {
		ret = PTR_ERR(mdev->scanout);
		mdev->scanout = NULL;
		goto fail;
	}
	ret = m9x_vram_pin(mdev->scanout);
	if (ret) {
		drm_gem_vram_put(mdev->scanout);
		mdev->scanout = NULL;
		goto fail;
	}
	dev_info(&pci->dev, "M9 tiled scanout surface at VRAM %#llx\n",
		 drm_gem_vram_offset(mdev->scanout));
	return 0;
fail:
	m9_scanout_fini(mdev);
	return ret;
}

int m9x_modeset_init(struct m9x_device *mdev)
{
	struct drm_device *drm = &mdev->drm;
	struct m9_warm_snapshot snapshot;
	int ret;

	ret = m9_kms_selftest();
	if (ret)
		return ret;
	m9_capture_warm_snapshot(mdev, &snapshot);
	if (!m9_warm_snapshot_valid(&snapshot))
		return dev_err_probe(&mdev->pdev->dev, -EOPNOTSUPP,
			"untested M9 display state; refusing guarded warm KMS\n");
	mdev->warm_dc_ctrl = snapshot.ctrl;
	mdev->warm_dc_pitch = snapshot.pitch;
	mdev->warm_dc_start = snapshot.start;
	mdev->warm_dvo = snapshot.dvo;
	mdev->warm_mode = m9_warm_mode;
	drm_mode_set_name(&mdev->warm_mode);
	if (m9x_kms_scanout_enabled()) {
		ret = m9_scanout_init(mdev);
		if (ret)
			return ret;
	}

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.funcs = &m9_mode_config_funcs;
	drm->mode_config.min_width = M9_WARM_WIDTH;
	drm->mode_config.max_width = M9_WARM_WIDTH;
	drm->mode_config.min_height = M9_WARM_HEIGHT;
	drm->mode_config.max_height = M9_WARM_HEIGHT;
	drm->mode_config.prefer_shadow = 0;

	ret = drm_simple_display_pipe_init(drm, &mdev->pipe, &m9_pipe_funcs,
					   m9_formats, ARRAY_SIZE(m9_formats),
					   NULL, NULL);
	if (ret)
		return ret;
	ret = drm_connector_init(drm, &mdev->connector,
				 &matrox_kms_connector_funcs,
				 DRM_MODE_CONNECTOR_DisplayPort);
	if (ret)
		return ret;
	drm_connector_helper_add(&mdev->connector, &m9_connector_helper_funcs);
	ret = drm_connector_attach_encoder(&mdev->connector, &mdev->pipe.encoder);
	if (ret)
		return ret;
	ret = drm_mode_crtc_set_gamma_size(&mdev->pipe.crtc, 256);
	if (ret)
		return ret;
	drm_crtc_enable_color_mgmt(&mdev->pipe.crtc, 0, false, 256);
	drm_mode_config_reset(drm);
	mdev->kms_initialized = true;
	return 0;
}

void m9x_modeset_fini(struct m9x_device *mdev)
{
	m9x_write(mdev, M9_REG_IRQ_LOW_MASK, 0);
	m9x_write(mdev, M9_REG_IRQ_HIGH_MASK, 0);
	m9x_write(mdev, M9_REG_IRQ0_MASK, 0);
	if (mdev->irq_installed) {
		free_irq(mdev->pdev->irq, mdev);
		mdev->irq_installed = false;
	}
	/*
	 * Return the controller to its exact warm scanout before TTM releases the
	 * last pinned dumb buffer.  Restoring CTRL also restores firmware's LUT
	 * bypass choice, so no driver-programmed gamma table remains selected.
	 */
	wmb();
	m9x_write(mdev, M9_DC0 + M9_DC_START, mdev->warm_dc_start);
	m9x_write(mdev, M9_DC0 + M9_DC_PITCH, mdev->warm_dc_pitch);
	m9x_write(mdev, M9_DC0 + M9_DC_CTRL, mdev->warm_dc_ctrl);
	m9_scanout_fini(mdev);
	mdev->kms_initialized = false;
}
