// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/iosys-map.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/overflow.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_color_mgmt.h>
#include <drm/drm_connector.h>
#include <drm/drm_edid.h>
#include <drm/drm_print.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_vram_helper.h>
#include <drm/drm_plane_helper.h>
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
/* Surface descriptor of the programmed scanout: format cc, ordinary tiled
 * kind 21, the 32-aligned span, and the line count. */
#define M9_SCANOUT_FORMAT(span)	(0xcc210000u | (span))
#define M9_SCANOUT_EXTENT(lines)	(0x00010000u | (lines))
#define M9_SCANOUT_MAX_WIDTH	2048u
#define M9_SCANOUT_MAX_HEIGHT	2048u

static bool m9_kms;
module_param(m9_kms, bool, 0400);
MODULE_PARM_DESC(m9_kms,
	"Expose guarded KMS only when the tested 1024x768 warm state is present");

static bool m9_kms_scanout;
module_param(m9_kms_scanout, bool, 0400);

static bool m9_kms_direct_scanout = true;
/* The firmware console runs with the LUT bypassed; programming a ramp is off
 * until the table is shown to reach the screen. */
/* The recovered upload contract is exact, but earlier hardware testing saw no
 * correct image; that test predates the gamma-table bypass, so it is worth
 * retrying before trusting or discarding this path. */
static bool m9_kms_cursor = true;
module_param(m9_kms_cursor, bool, 0600);
MODULE_PARM_DESC(m9_kms_cursor,
	"Drive the hardware cursor overlay instead of leaving it to software");

static bool m9_kms_modeset;
module_param(m9_kms_modeset, bool, 0400);
MODULE_PARM_DESC(m9_kms_modeset,
	"Program the pixel clock and raster timing instead of keeping the firmware's mode");

/* All four sources are measured (m9_reg.h).  The parameter stays so a board
 * that numbers them differently can be corrected without a rebuild; an entry
 * left out or given as zero takes the measured bit, so no configuration can
 * leave an active head without a vertical blank. */
static const unsigned int m9_vblank_default[M9_MAX_HEADS] = {
	M9_IRQ_LOW_VBLANK0, M9_IRQ_LOW_VBLANK1,
	M9_IRQ_LOW_VBLANK2, M9_IRQ_LOW_VBLANK3,
};
static unsigned int m9_vblank_mask[M9_MAX_HEADS];
static unsigned int m9_vblank_count;
module_param_array_named(vblank_mask, m9_vblank_mask, uint, &m9_vblank_count,
			 0400);
MODULE_PARM_DESC(vblank_mask,
	"Per-head low-interrupt vblank bit; 0 keeps the measured one");

/* The controller's own limits: a surface is at most 2048 in each dimension,
 * and the synthesiser covers this range with the divider search. */
#define M9_CLOCK_MIN_KHZ	20000u
#define M9_CLOCK_MAX_KHZ	200000u

/* Every scanout surface is allocated with a 32-aligned row span. */
static u32 m9_scanout_pitch(u32 width)
{
	return ALIGN(width, 32) * 4;
}

static bool m9_kms_lut;
module_param(m9_kms_lut, bool, 0600);
MODULE_PARM_DESC(m9_kms_lut,
	"Program the scanout gamma table instead of bypassing it");
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

static void m9_capture_warm_snapshot(struct m9_head *head,
				     struct m9_warm_snapshot *s)
{
	s->ctrl = m9x_read(head->mdev, head->dc + M9_DC_CTRL);
	s->xstart = m9x_read(head->mdev, head->dc + M9_DC_XSTART);
	s->xend = m9x_read(head->mdev, head->dc + M9_DC_XEND);
	s->ystart = m9x_read(head->mdev, head->dc + M9_DC_YSTART);
	s->yend = m9x_read(head->mdev, head->dc + M9_DC_YEND);
	s->start = m9x_read(head->mdev, head->dc + M9_DC_START);
	s->pitch = m9x_read(head->mdev, head->dc + M9_DC_PITCH);
	s->hblank = m9x_read(head->mdev, head->dct + M9_DCT_HBLANK);
	s->htotal = m9x_read(head->mdev, head->dct + M9_DCT_HTOTAL);
	s->hsync = m9x_read(head->mdev, head->dct + M9_DCT_HSYNC);
	s->vblank = m9x_read(head->mdev, head->dct + M9_DCT_VBLANK);
	s->vtotal = m9x_read(head->mdev, head->dct + M9_DCT_VTOTAL);
	s->vsync = m9x_read(head->mdev, head->dct + M9_DCT_VSYNC);
	s->vsyncend = m9x_read(head->mdev, head->dct + M9_DCT_VSYNCEND);
	s->vgaflag = m9x_read(head->mdev, M9_REG_VGAFLAG);
	s->vgamisc = m9x_read(head->mdev, M9_REG_VGAMISC);
	s->dvo = m9x_read(head->mdev, head->link);
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
	u32 owned = 0;
	unsigned int i;

	for (i = 0; i < mdev->heads; i++)
		owned |= mdev->head[i].vblank_mask;
	if (!owned)
		owned = M9_IRQ_LOW_VBLANK0;
	return (struct matrox_kms_irq) {
		.cookie = mdev,
		.read = m9_kms_read,
		.write = m9_kms_write,
		.raw_reg = M9_REG_IRQ_LOW_RAW,
		.ack_reg = M9_REG_IRQ_LOW_ACK,
		.mask_reg = M9_REG_IRQ_LOW_MASK,
		.owned_mask = owned,
		.vblank_mask = owned,
	};
}

/* Enabling and disabling act on one head: the descriptor's vblank_mask is
 * what matrox_kms_enable_vblank ORs into the mask register and what disable
 * clears, so the union would let one head's vblank_put mask every other
 * head's source. */
static struct matrox_kms_irq m9_kms_irq_head_desc(struct m9x_device *mdev,
						  const struct m9_head *head)
{
	struct matrox_kms_irq desc = m9_kms_irq_desc(mdev);

	desc.vblank_mask = head->vblank_mask;
	return desc;
}

/*
 * A DDC bus that answers has a display on it.  When it does not, only the
 * head the firmware left scanning out is known to carry one, which keeps a
 * display without DDC usable on the boot head.
 */
static int m9_connector_detect(struct drm_connector *connector,
			       struct drm_modeset_acquire_ctx *ctx, bool force)
{
	struct m9_head *head = container_of(connector, struct m9_head,
					    connector);
	bool present;

	drm_edid_free(head->edid);
	head->edid = m9x_ddc_read_edid(head->mdev, head->ddc_index, connector);
	head->edid_valid = true;
	present = head->edid || (head->warm_dc_ctrl & M9_DC_ENABLE);
	return present ? connector_status_connected :
			 connector_status_disconnected;
}

/* The warm mode the firmware left is always offered: it is the only timing
 * the pipe can currently program, and it must remain available when the
 * display has no EDID. */
static int m9_connector_add_warm_mode(struct drm_connector *connector)
{
	struct m9_head *head = container_of(connector, struct m9_head,
					    connector);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &head->warm_mode);
	if (!mode)
		return 0;
	drm_mode_probed_add(connector, mode);
	return 1;
}

static int m9_connector_get_modes(struct drm_connector *connector)
{
	struct m9_head *head = container_of(connector, struct m9_head,
					    connector);
	int count = 0;

	/* detect_ctx already drove the bus; a forced connector skips it. */
	if (!head->edid_valid) {
		head->edid = m9x_ddc_read_edid(head->mdev, head->ddc_index,
					       connector);
		head->edid_valid = true;
	}
	drm_edid_connector_update(connector, head->edid);
	if (head->edid)
		count = drm_edid_connector_add_modes(connector);
	return count + m9_connector_add_warm_mode(connector);
}

static const struct drm_connector_helper_funcs m9_connector_helper_funcs = {
	.get_modes = m9_connector_get_modes,
	.detect_ctx = m9_connector_detect,
};

static enum drm_mode_status
m9_pipe_mode_valid(struct drm_simple_display_pipe *pipe,
		   const struct drm_display_mode *mode)
{
	struct m9_head *head = m9_head_of_pipe(pipe);
	enum drm_mode_status status;

	status = matrox_kms_mode_geometry_valid(mode, M9_SCANOUT_MAX_WIDTH,
						 M9_SCANOUT_MAX_HEIGHT,
						 M9_CLOCK_MIN_KHZ,
						 M9_CLOCK_MAX_KHZ);
	if (status != MODE_OK)
		return status;
	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		return MODE_NO_INTERLACE;
	/* Without m9_kms_modeset the firmware's timing is all the pipe can
	 * scan out, so nothing else may be advertised. */
	if (!m9_kms_modeset && !drm_mode_equal(mode, &head->warm_mode))
		return MODE_BAD;
	return MODE_OK;
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
	if (crtc_state->enable && !m9_kms_modeset &&
	    !drm_mode_equal(&crtc_state->mode, &m9_head_of_pipe(pipe)->warm_mode))
		return -EINVAL;
	/* A head with no vertical blank cannot complete a flip event: the
	 * commit never retires and the next rmfb blocks flushing it.  Refuse
	 * the flip instead, so the caller is told rather than stalled. */
	if (crtc_state->event && !m9_head_of_pipe(pipe)->vblank_mask)
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
	    fb->format->format != DRM_FORMAT_XRGB8888 || fb->offsets[0] ||
	    fb->pitches[0] != m9_scanout_pitch(fb->width) ||
	    fb->width != crtc_state->mode.hdisplay ||
	    fb->height != crtc_state->mode.vdisplay)
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
/* Bytes of the frame the head currently scans out.  scanout_bytes is only
 * the bounce buffer's capacity, which grows and never shrinks, so it is not
 * the length to copy once a mode has become smaller. */
static size_t m9_scanout_frame_bytes(const struct m9_head *head)
{
	return (size_t)head->scanout_pitch * head->scanout_height;
}

static int m9_scanout_publish(struct m9_head *head,
			      struct drm_framebuffer *fb)
{
	size_t frame;

	struct drm_gem_object *gem = drm_gem_fb_get_obj(fb, 0);
	struct drm_gem_vram_object *src = drm_gem_vram_of_gem(gem);
	bool tiled = m9x_gem_vram_tiled(gem);
	struct iosys_map map;
	s64 offset, src_offset = 0;
	int ret;

	struct m9x_device *mdev = head->mdev;

	frame = m9_scanout_frame_bytes(head);
	if (!head->scanout || !mdev->bar0_window || !head->scanout_bounce ||
	    !frame || frame > head->scanout_bytes ||
	    head->scanout->bo.base.size < frame ||
	    gem->size < frame ||
	    fb->width != head->scanout_width ||
	    fb->height != head->scanout_height ||
	    fb->pitches[0] != head->scanout_pitch)
		return -ENODEV;
	offset = drm_gem_vram_offset(head->scanout);
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
			iosys_map_memcpy_from(head->scanout_bounce, &map, 0,
					      frame);
			drm_gem_vram_vunmap(src, &map);
		}
		ttm_bo_unreserve(&src->bo);
		if (ret)
			return ret;
	}
	mutex_lock(&mdev->transport.lock);
	m9x_transport_drain_locked(mdev);
	/* GPU-rendered frames are tiled: de-tile them through the window. */
	if (tiled)
		m9x_surface_read_locked(mdev, lower_32_bits(src_offset),
					M9_SCANOUT_FORMAT(head->scanout_pitch / 4),
					M9_SCANOUT_EXTENT(head->scanout_height),
					head->scanout_bounce, frame);
	m9x_address_surface_window(mdev, lower_32_bits(offset),
				   M9_SCANOUT_FORMAT(head->scanout_pitch / 4),
				   M9_SCANOUT_EXTENT(head->scanout_height));
	memcpy_toio(mdev->bar0_window, head->scanout_bounce, frame);
	/* Complete WC stores before the window is released. */
	wmb();
	m9x_address_surface_release(mdev);
	mutex_unlock(&mdev->transport.lock);
	return 0;
}

static int m9_scanout_address(struct m9_head *head, u32 *address)
{
	u64 limit = min_t(u64, head->mdev->vram_size - 1, M9_DC_START_MASK);

	if (!head->scanout)
		return -ENODEV;
	return matrox_kms_vram_offset(&head->scanout->bo.base, 1024, limit,
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

/* Format and layout live in the high bits of the pitch word.  A head the
 * firmware never posted has none to inherit, so it takes the measured tiled
 * XRGB8888 word every controller of the four-head run was given. */
static u32 m9_dc_pitch(const struct m9_head *head, u32 pitch_pixels)
{
	u32 high = head->posted ? head->warm_dc_pitch & ~M9_DC_PITCH_MASK :
				  M9_SCANOUT_FORMAT(0);

	return high | (pitch_pixels & M9_DC_PITCH_MASK);
}

static void m9_program_scanout(struct drm_simple_display_pipe *pipe,
			       struct drm_framebuffer *fb)
{
	struct m9_head *head = m9_head_of_pipe(pipe);
	struct m9x_device *mdev = head->mdev;
	u32 address, pitch;
	int ret;

	if (!m9_scanout_direct_address(mdev, fb, &address)) {
		pitch = m9_dc_pitch(head, fb->pitches[0] / 4);
		wmb();
		m9x_write(head->mdev, head->dc + M9_DC_START,
			  address & M9_DC_START_MASK);
		m9x_write(head->mdev, head->dc + M9_DC_PITCH, pitch);
		return;
	}
	if (!fb)
		return;
	/* A failed publish leaves the previous frame on screen; the flip still
	 * retires on the vertical blank, so silence here reads as a working
	 * flip that never changes the picture. */
	ret = m9_scanout_publish(head, fb);
	if (!ret && m9_scanout_address(head, &address))
		ret = -ENXIO;
	if (ret) {
		drm_err_ratelimited(&mdev->drm,
			"head %u: scanout publish failed (%d); the previous frame stays on screen\n",
			head->index, ret);
		return;
	}
	pitch = m9_dc_pitch(head, fb->pitches[0] / 4);
	/* Publish CPU writes before the display controller fetches this BO. */
	wmb();
	m9x_write(head->mdev, head->dc + M9_DC_START, address & M9_DC_START_MASK);
	m9x_write(head->mdev, head->dc + M9_DC_PITCH, pitch);
}

/* Only the upper ten bits of each entry reach the LUT ports, so compare the
 * ramp at that granularity. */
static bool m9_lut_is_identity(const struct drm_property_blob *blob)
{
	const struct drm_color_lut *lut;
	unsigned int i;

	if (!blob)
		return true;
	if (drm_color_lut_size(blob) != 256)
		return false;
	lut = blob->data;
	for (i = 0; i < 256; i++) {
		/* X builds its linear ramp as i << 8, the DRM identity is
		 * i * 257; both are gamma 1.0, so allow the low bits to
		 * differ rather than programming a table for either. */
		u16 want = i << 2;

		if (abs((int)(lut[i].red >> 6) - want) > 4 ||
		    abs((int)(lut[i].green >> 6) - want) > 4 ||
		    abs((int)(lut[i].blue >> 6) - want) > 4)
			return false;
	}
	return true;
}


/* Hardware cursor overlay (parhelia-m9-cursor-gamma-re.md section 1): a
 * 64x64 colour image in VRAM with a 256-byte row, an address and config pair,
 * a shifted-origin position, and a control word that shows or hides it. */
static void m9_cursor_hide(struct m9_head *head)
{
	m9x_write(head->mdev, head->cursor_bank + M9_CURSOR_CONTROL, M9_CURSOR_OFF);
	head->cursor_visible = false;
}

/* A disabled controller keeps no cursor state, so the next update has to
 * program the address and config again before showing the overlay. */
static void m9_cursor_suspend(struct m9_head *head)
{
	m9_cursor_hide(head);
	head->cursor_armed = false;
}

static int m9_cursor_storage(struct m9_head *head)
{
	struct drm_gem_vram_object *bo;
	int ret;

	if (head->cursor)
		return 0;
	bo = drm_gem_vram_create(&head->mdev->drm, M9_CURSOR_BYTES, 0);
	if (IS_ERR(bo))
		return PTR_ERR(bo);
	ret = m9x_vram_pin(bo);
	if (ret)
		goto err_put;
	ret = ttm_bo_reserve(&bo->bo, true, false, NULL);
	if (ret)
		goto err_unpin;
	ret = drm_gem_vram_vmap(bo, &head->cursor_map);
	ttm_bo_unreserve(&bo->bo);
	if (ret)
		goto err_unpin;
	iosys_map_memset(&head->cursor_map, 0, 0, M9_CURSOR_BYTES);
	head->cursor = bo;
	return 0;
err_unpin:
	m9x_vram_unpin(bo);
err_put:
	drm_gem_object_put(&bo->bo.base);
	return ret;
}

/* Copy the plane image into the device row stride, zero filling the rest. */
static int m9_cursor_upload(struct m9_head *head, struct drm_framebuffer *fb)
{
	struct drm_gem_object *gem = drm_gem_fb_get_obj(fb, 0);
	struct drm_gem_vram_object *src = drm_gem_vram_of_gem(gem);
	unsigned int cols = min_t(u32, fb->width, M9_CURSOR_SIZE);
	unsigned int rows = min_t(u32, fb->height, M9_CURSOR_SIZE);
	u32 line[M9_CURSOR_SIZE];
	struct iosys_map smap;
	unsigned int row, i;
	int ret;

	ret = ttm_bo_reserve(&src->bo, true, false, NULL);
	if (ret)
		return ret;
	ret = drm_gem_vram_vmap(src, &smap);
	if (ret) {
		ttm_bo_unreserve(&src->bo);
		return ret;
	}
	for (row = 0; row < M9_CURSOR_SIZE; row++) {
		memset(line, 0, sizeof(line));
		if (row < rows) {
			iosys_map_memcpy_from(line, &smap,
					      (size_t)row * fb->pitches[0],
					      (size_t)cols * 4);
			for (i = 0; i < cols; i++)
				line[i] &= M9_CURSOR_DWORD_MASK;
		}
		iosys_map_memcpy_to(&head->cursor_map,
				    (size_t)row * M9_CURSOR_ROW_BYTES, line,
				    sizeof(line));
	}
	/* Publish the image before the controller is pointed at it. */
	wmb();
	drm_gem_vram_vunmap(src, &smap);
	ttm_bo_unreserve(&src->bo);
	return 0;
}

static int m9_cursor_check(struct drm_plane *plane,
			   struct drm_atomic_commit *state)
{
	struct drm_plane_state *new =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *crtc_state;

	if (!new->fb || !new->crtc)
		return 0;
	if (new->fb->width > M9_CURSOR_SIZE || new->fb->height > M9_CURSOR_SIZE)
		return -EINVAL;
	crtc_state = drm_atomic_get_new_crtc_state(state, new->crtc);
	if (!crtc_state)
		return -EINVAL;
	return drm_atomic_helper_check_plane_state(new, crtc_state,
						   DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING,
						   true, true);
}

static void m9_cursor_atomic_update(struct drm_plane *plane,
				    struct drm_atomic_commit *state)
{
	struct drm_plane_state *new =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_plane_state *old =
		drm_atomic_get_old_plane_state(state, plane);
	struct m9_head *head = container_of(plane, struct m9_head,
					   cursor_plane);
	bool offscreen;
	s64 address;
	u32 px, py;
	int idx;

	if (!drm_dev_enter(plane->dev, &idx))
		return;
	if (!m9_kms_cursor || !new->fb || !new->crtc) {
		m9_cursor_hide(head);
		goto out;
	}
	if (m9_cursor_storage(head))
		goto out;
	if (!old || old->fb != new->fb) {
		/* Keep control zero while the image and address change. */
		m9_cursor_suspend(head);
		if (m9_cursor_upload(head, new->fb))
			goto out;
	}
	if (!head->cursor_armed) {
		m9_cursor_hide(head);
		address = drm_gem_vram_offset(head->cursor);
		if (address < 0 || upper_32_bits(address))
			goto out;
		m9x_write(head->mdev, head->cursor_bank + M9_CURSOR_ADDRESS,
			  lower_32_bits(address));
		m9x_write(head->mdev, head->cursor_bank + M9_CURSOR_CONFIG,
			  M9_CURSOR_CONFIG_COLOR);
		head->cursor_armed = true;
	}
	/* Nothing clips in hardware, so hide a cursor that has left the scanout
	 * rather than let the shifted origin wrap it back on. */
	offscreen = new->crtc_x <= -(int)M9_CURSOR_SIZE ||
		    new->crtc_y <= -(int)M9_CURSOR_SIZE ||
		    new->crtc_x >= (int)head->scanout_width ||
		    new->crtc_y >= (int)head->scanout_height;
	if (offscreen) {
		m9_cursor_hide(head);
		goto out;
	}
	px = (new->crtc_x + M9_CURSOR_SIZE) & M9_CURSOR_POS_MASK;
	py = (new->crtc_y + M9_CURSOR_SIZE) & M9_CURSOR_POS_MASK;
	m9x_write(head->mdev, head->cursor_bank + M9_CURSOR_POSITION, px << 16 | py);
	if (!head->cursor_visible) {
		m9x_write(head->mdev, head->cursor_bank + M9_CURSOR_CONTROL, M9_CURSOR_COLOR);
		head->cursor_visible = true;
	}
out:
	drm_dev_exit(idx);
}

static void m9_cursor_atomic_disable(struct drm_plane *plane,
				     struct drm_atomic_commit *state)
{
	struct m9_head *head = container_of(plane, struct m9_head,
					   cursor_plane);
	int idx;

	if (!drm_dev_enter(plane->dev, &idx))
		return;
	m9_cursor_suspend(head);
	drm_dev_exit(idx);
}

static const struct drm_plane_helper_funcs m9_cursor_helper_funcs = {
	.atomic_check = m9_cursor_check,
	.atomic_update = m9_cursor_atomic_update,
	.atomic_disable = m9_cursor_atomic_disable,
};

static const struct drm_plane_funcs m9_cursor_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static const u32 m9_cursor_formats[] = { DRM_FORMAT_ARGB8888 };

static void m9_cursor_fini(struct m9_head *head)
{
	if (!head->cursor)
		return;
	m9_cursor_suspend(head);
	if (iosys_map_is_set(&head->cursor_map)) {
		if (!ttm_bo_reserve(&head->cursor->bo, true, false, NULL)) {
			drm_gem_vram_vunmap(head->cursor, &head->cursor_map);
			ttm_bo_unreserve(&head->cursor->bo);
		}
		iosys_map_clear(&head->cursor_map);
	}
	m9x_vram_unpin(head->cursor);
	drm_gem_object_put(&head->cursor->bo.base);
	head->cursor = NULL;
}

static void m9_pipe_load_gamma(struct drm_simple_display_pipe *pipe,
			       const struct drm_property_blob *blob)
{
	struct m9_head *head = m9_head_of_pipe(pipe);
	struct m9x_device *mdev = head->mdev;
	u32 ctrl = m9x_read(head->mdev, head->dc + M9_DC_CTRL);

	/* An identity ramp needs no table at all: the firmware console runs
	 * with bit 14 set, and the recovered contract offers that as an
	 * explicit bypass identity.  Only a real ramp turns the table on, so a
	 * table that never took keeps its colours off the screen.  X hands a
	 * linear ramp down at start-up, which is this same identity. */
	if (!m9_kms_lut || m9_lut_is_identity(blob)) {
		m9x_write(head->mdev, head->dc + M9_DC_CTRL, ctrl | M9_DC_LUT_BYPASS);
		return;
	}
	/* The M9 DC LUT is the same ten-bit interface used by Parhelia. */
	m9x_write(head->mdev, head->dc + M9_DC_CTRL, ctrl & ~M9_DC_LUT_BYPASS);
	matrox_kms_write_lut(mdev, m9_kms_write,
			       head->dc + M9_DC_LUT_INDEX,
			       head->dc + M9_DC_LUT_RED,
			       head->dc + M9_DC_LUT_GREEN,
			       head->dc + M9_DC_LUT_BLUE,
			       M9_DC_LUT_LATCH, false, blob);
}

/* Record the geometry the rest of the display path describes surfaces with,
 * and grow the de-tiling bounce buffer to match.  A failed growth only costs
 * the copied path; a tiled framebuffer is still scanned out in place. */
/*
 * The surface a head publishes into is created from the firmware's mode at
 * init, so a larger mode outgrows it and every publish into it fails.  Grow
 * it here, where the caller has already stopped the display controller's
 * fetch, and keep the old one until the new one is pinned.
 */
static void m9_scanout_resize(struct m9_head *head, u32 width, u32 height)
{
	struct m9x_device *mdev = head->mdev;
	struct drm_gem_vram_object *bo;
	u64 bytes;

	if (!m9x_surface_layout_bytes(width, height, 4, &bytes))
		return;
	if (head->scanout && head->scanout->bo.base.size >= PAGE_ALIGN(bytes))
		return;
	bo = drm_gem_vram_create(&mdev->drm, PAGE_ALIGN(bytes), 1);
	if (IS_ERR(bo)) {
		drm_err(&mdev->drm,
			"head %u: no scanout surface for %ux%u: %pe\n",
			head->index, width, height, bo);
		return;
	}
	if (m9x_vram_pin(bo)) {
		drm_gem_vram_put(bo);
		return;
	}
	if (head->scanout) {
		m9x_vram_unpin(head->scanout);
		drm_gem_vram_put(head->scanout);
	}
	head->scanout = bo;
	drm_info(&mdev->drm, "head %u scanout surface at VRAM %#llx for %ux%u\n",
		 head->index, drm_gem_vram_offset(bo), width, height);
}

static void m9_scanout_geometry(struct m9_head *head,
				const struct drm_display_mode *mode)
{
	size_t bytes;

	head->scanout_width = mode->hdisplay;
	head->scanout_height = mode->vdisplay;
	head->scanout_pitch = m9_scanout_pitch(mode->hdisplay);
	m9_scanout_resize(head, mode->hdisplay, mode->vdisplay);
	bytes = (size_t)head->scanout_pitch * head->scanout_height;
	if (bytes <= head->scanout_bytes)
		return;
	kvfree(head->scanout_bounce);
	head->scanout_bounce = kvzalloc(bytes, GFP_KERNEL);
	head->scanout_bytes = head->scanout_bounce ? bytes : 0;
}

/*
 * Drive this head's connector.  The display controller running is not the same
 * thing as a picture: the connector's external transmitter has to be brought
 * up and its pad ungated, and it samples the video it is fed, so a retimed
 * controller needs the bring-up again.  The transmitter reports what it is
 * sending, which is what decides whether to redo it and what gets logged.
 */
static void m9_head_output_enable(struct m9_head *head,
				  const struct drm_display_mode *mode)
{
	struct m9x_device *mdev = head->mdev;
	struct drm_display_mode sent;
	int ret;

	ret = m9x_tx_stream(mdev, head->index, &sent);
	if (!ret && sent.hdisplay == mode->hdisplay &&
	    sent.vdisplay == mode->vdisplay && sent.htotal == mode->htotal &&
	    sent.vtotal == mode->vtotal)
		return;

	ret = m9x_tx_enable(mdev, head->index, mode->clock);
	if (ret) {
		drm_warn(&mdev->drm, "head %u: output bring-up failed (%d)\n",
			 head->index, ret);
		return;
	}
	/* The transmitter needs a moment to lock the feed before it reports. */
	msleep(50);
	ret = m9x_tx_stream(mdev, head->index, &sent);
	if (ret)
		drm_warn(&mdev->drm,
			 "head %u: brought up but sending nothing (%d), pad gate %#x\n",
			 head->index, ret, m9x_read(mdev, M9_REG_PAD_GATE));
	else
		drm_info(&mdev->drm, "head %u: sending %ux%u htotal %u vtotal %u, pad gate %#x\n",
			 head->index, sent.hdisplay, sent.vdisplay,
			 sent.htotal, sent.vtotal,
			 m9x_read(mdev, M9_REG_PAD_GATE));
}

static void m9_pipe_enable(struct drm_simple_display_pipe *pipe,
			   struct drm_crtc_state *crtc_state,
			   struct drm_plane_state *plane_state)
{
	struct m9_head *head = m9_head_of_pipe(pipe);
	struct m9x_device *mdev = head->mdev;
	int idx;
	u32 ctrl;

	if (!drm_dev_enter(&mdev->drm, &idx))
		return;
	ctrl = m9x_read(head->mdev, head->dc + M9_DC_CTRL);
	m9x_write(head->mdev, head->dc + M9_DC_CTRL, ctrl & ~M9_DC_ENABLE);
	m9_scanout_geometry(head, &crtc_state->mode);
	/* A head the firmware never posted owns no link routing or clock, and
	 * one whose timing was never adopted has only a placeholder mode, so
	 * neither may take the "already at this timing" shortcut. */
	if (m9_kms_modeset && (!head->posted || !head->warm_valid ||
	    !drm_mode_equal(&crtc_state->mode, &head->warm_mode)) &&
	    !m9x_mode_program(head, &crtc_state->mode,
			      head->scanout_pitch / 4)) {
		drm_mode_copy(&head->warm_mode, &crtc_state->mode);
		head->warm_valid = true;
	}
	m9_program_scanout(pipe, plane_state->fb);
	m9_pipe_load_gamma(pipe, crtc_state->gamma_lut);
	/* Re-enable only the proven warm controller bit after the pinned new BO
	 * is selected.  Preserve the current LUT choice and every opaque bit.
	 * A head the firmware never posted has no enable bit to preserve, so it
	 * takes the one this driver just programmed. */
	ctrl = m9x_read(head->mdev, head->dc + M9_DC_CTRL);
	ctrl |= head->posted ? (head->warm_dc_ctrl & M9_DC_ENABLE) :
			       M9_DC_ENABLE;
	m9x_write(head->mdev, head->dc + M9_DC_CTRL, ctrl);
	if (m9_kms_modeset)
		m9_head_output_enable(head, &crtc_state->mode);
	if (head->vblank_mask)
		drm_crtc_vblank_on(&pipe->crtc);
	drm_dev_exit(idx);
}

static void m9_pipe_disable_hw(struct drm_simple_display_pipe *pipe)
{
	struct m9_head *head = m9_head_of_pipe(pipe);
	u32 ctrl = m9x_read(head->mdev, head->dc + M9_DC_CTRL);

	/* Stop memory fetch before the old scanout BO is unpinned. */
	m9x_write(head->mdev, head->dc + M9_DC_CTRL, ctrl & ~M9_DC_ENABLE);
}

static void m9_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct drm_device *drm = pipe->crtc.dev;
	int idx;

	if (!drm_dev_enter(drm, &idx))
		return;
	m9_cursor_suspend(m9_head_of_pipe(pipe));
	/* Blank only a transmitter this driver brought up: the raster timing
	 * keeps running either way, and a connector the firmware trained is
	 * left alone until this path has proven it can bring it back. */
	m9x_tx_disable(m9_head_of_pipe(pipe)->mdev,
		       m9_head_of_pipe(pipe)->index);
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
	unsigned int i;
	int ret = 0;

	for (i = 0; i < mdev->heads; i++) {
		struct m9_head *head = &mdev->head[i];

		drm_modeset_lock(&head->pipe.plane.mutex, NULL);
		/* Nothing to publish while the framebuffer is scanned out in
		 * place. */
		if (head->pipe.plane.state &&
		    head->pipe.plane.state->fb == fb &&
		    head->pipe.crtc.state && head->pipe.crtc.state->active &&
		    !(m9_kms_direct_scanout &&
		      m9x_gem_vram_tiled(drm_gem_fb_get_obj(fb, 0))))
			ret = m9_scanout_publish(head, fb);
		drm_modeset_unlock(&head->pipe.plane.mutex);
	}
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
	struct m9_head *head = m9_head_of_pipe(pipe);
	struct m9x_device *mdev = head->mdev;
	struct matrox_kms_irq desc = m9_kms_irq_head_desc(mdev, head);
	int idx, ret;

	/* A head whose vblank source is not measured never reports one. */
	if (!head->vblank_mask)
		return -EINVAL;
	if (!drm_dev_enter(&mdev->drm, &idx))
		return -ENODEV;
	ret = matrox_kms_enable_vblank(&desc);
	drm_dev_exit(idx);
	return ret;
}

static void m9_pipe_disable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct m9_head *head = m9_head_of_pipe(pipe);
	struct m9x_device *mdev = head->mdev;
	struct matrox_kms_irq desc = m9_kms_irq_head_desc(mdev, head);
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

/* The cursor image never reaches DC0 as a scanout, so it needs no dirty
 * publication and no tiled backing - the plane copies it into VRAM. */
static const struct drm_framebuffer_funcs m9_cursor_fb_funcs = {
	.destroy = drm_gem_fb_destroy,
	.create_handle = drm_gem_fb_create_handle,
};

static struct drm_framebuffer *
m9_fb_create(struct drm_device *drm, struct drm_file *file,
	     const struct drm_format_info *info,
	     const struct drm_mode_fb_cmd2 *mode_cmd)
{
	const struct drm_framebuffer_funcs *funcs;
	struct drm_gem_object *gem;
	struct drm_framebuffer *fb;
	u64 need;

	/* Only m9_pipe_check admits a framebuffer to DC0, so this accepts the
	 * cursor image and the small probe buffers Xorg uses to settle on a
	 * depth, and keeps the tail allowance for the warm scanout itself. */
	if (mode_cmd->offsets[0] ||
	    mode_cmd->pitches[0] < (u64)mode_cmd->width * 4)
		return ERR_PTR(-EINVAL);
	if (info->format == DRM_FORMAT_ARGB8888) {
		if (mode_cmd->width > M9_CURSOR_SIZE ||
		    mode_cmd->height > M9_CURSOR_SIZE)
			return ERR_PTR(-EINVAL);
		funcs = &m9_cursor_fb_funcs;
	} else if (info->format == DRM_FORMAT_XRGB8888) {
		funcs = &m9_fb_funcs;
	} else {
		return ERR_PTR(-EINVAL);
	}
	need = (u64)mode_cmd->pitches[0] * mode_cmd->height;
	/* A scanout-shaped image carries the vendor allocation's tail. */
	if (funcs == &m9_fb_funcs &&
	    mode_cmd->pitches[0] == m9_scanout_pitch(mode_cmd->width))
		m9x_surface_layout_bytes(mode_cmd->width, mode_cmd->height, 4,
					 &need);
	gem = drm_gem_object_lookup(file, mode_cmd->handles[0]);
	if (!gem)
		return ERR_PTR(-ENOENT);
	if (!m9x_gem_is_vram(gem) || gem->size < need) {
		fb = ERR_PTR(-EINVAL);
		goto out_put;
	}
	fb = drm_gem_fb_create_with_funcs(drm, file, info, mode_cmd, funcs);
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
	u64 bytes;
	int ret;

	if (!m9x_kms_scanout_enabled() || !args->width || !args->height ||
	    args->bpp != 32)
		return -EINVAL;
	/* Geometry the surface constructor cannot describe still gets a plain
	 * object: nothing reaches DC0 without passing the framebuffer and plane
	 * checks, which insist on the constructor's own span. */
	if (!m9x_surface_layout_bytes(args->width, args->height, 4, &bytes))
		return drm_gem_vram_fill_create_dumb(file, drm, 1,
						     M9_WARM_PITCH, args);

	/*
	 * The M9148 category-2 constructor adds eight end-pad elements when
	 * aligned geometry leaves no tail slack, then rounds to its byte
	 * quantum, so the visible geometry is kept and the height inflated to
	 * cover the real backing.  ALIGN(width, 32) * 4 is ALIGN(width * 4,
	 * 128), and the helper needs a power-of-two alignment.
	 */
	alloc_args = *args;
	alloc_args.height = DIV_ROUND_UP(bytes, m9_scanout_pitch(args->width));
	ret = drm_gem_vram_fill_create_dumb(file, drm, 1, 128, &alloc_args);
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
	unsigned int i;

	if (!active)
		return IRQ_NONE;
	/*
	 * Low IRQ status is W1C.  Do not consume masked sources: a warm live
	 * sample had several of them pending while the low mask was zero.
	 */
	m9x_write(mdev, M9_REG_IRQ_LOW_ACK, active);
	/* A head's bit is only unmasked while DRM has vblank enabled on that
	 * head, so the mask is what ties this to the active scanouts. */
	for (i = 0; i < mdev->heads; i++)
		if (active & mdev->head[i].vblank_mask)
			drm_crtc_handle_vblank(&mdev->head[i].pipe.crtc);
	return IRQ_HANDLED;
}

static void m9_scanout_fini(struct m9_head *head)
{
	if (head->scanout) {
		m9x_vram_unpin(head->scanout);
		drm_gem_vram_put(head->scanout);
		head->scanout = NULL;
	}
	kvfree(head->scanout_bounce);
	head->scanout_bounce = NULL;
}

static int m9_scanout_init(struct m9_head *head)
{
	struct m9x_device *mdev = head->mdev;
	u64 bytes;
	int ret;

	head->scanout_width = head->warm_mode.hdisplay;
	head->scanout_height = head->warm_mode.vdisplay;
	head->scanout_pitch = m9_scanout_pitch(head->scanout_width);
	head->scanout_bytes = (size_t)head->scanout_pitch * head->scanout_height;
	head->scanout_bounce = kvzalloc(head->scanout_bytes, GFP_KERNEL);
	if (!mdev->bar0_window || !head->scanout_bounce) {
		ret = -ENOMEM;
		goto fail;
	}
	/* The controller fetches through the surface constructor's allocation,
	 * and a GEM object is whole pages. */
	if (!m9x_surface_layout_bytes(head->scanout_width, head->scanout_height,
				      4, &bytes)) {
		ret = -EINVAL;
		goto fail;
	}
	head->scanout = drm_gem_vram_create(&mdev->drm, PAGE_ALIGN(bytes), 1);
	if (IS_ERR(head->scanout)) {
		ret = PTR_ERR(head->scanout);
		head->scanout = NULL;
		goto fail;
	}
	ret = m9x_vram_pin(head->scanout);
	if (ret) {
		drm_gem_vram_put(head->scanout);
		head->scanout = NULL;
		goto fail;
	}
	drm_info(&mdev->drm, "head %u scanout surface at VRAM %#llx\n",
		 head->index, drm_gem_vram_offset(head->scanout));
	return 0;
fail:
	m9_scanout_fini(head);
	return ret;
}

/*
 * Per-head register banks, MEASURED with four displays attached and all four
 * controllers driven in one vendor run
 * (xf86-video-parhelia docs/parhelia-m9148-fourhead-outputs.md).  There is no
 * fixed stride: heads 2 and 3 occupy the upper half of heads 0's and 1's
 * timing pages, so their DC block is only 0x80 long, they own no hold pair and
 * borrow their companion's, and each takes one horizontal word in the
 * companion's timing page.  Controllers 2 and 3 are still independent heads,
 * with their own timing block, synthesiser, link and line counter.
 *
 * A head owns no pixel-clock register either; clk_select picks its
 * synthesiser, and the four pixel selects are one per head.
 */
static const struct {
	u32 dc, dct, clksel, link, cursor_bank, watermark, hold, companion_dct;
	u8 clk_select;
	bool half_block;
} m9_head_banks[M9_MAX_HEADS] = {
	{ 0x1800, 0x1900, 0x1c00, 0x1c9c, 0x18c0, 0x1c14, 0x1888, 0, 4, false },
	{ 0x1a00, 0x1b00, 0x1c08, 0x1ca0, 0x1ac0, 0x1c1c, 0x1a88, 0, 1, false },
	{ 0x1980, 0x1e00, 0x1c04, 0x1ca4, 0x18e0, 0x1c18, 0x1888, 0x1900, 2, true },
	{ 0x1b80, 0x1f00, 0x1c0c, 0x1ca8, 0x1ae0, 0x1c20, 0x1a88, 0x1b00, 3, true },
};

static int m9_head_init(struct m9x_device *mdev, struct m9_head *head,
			unsigned int index)
{
	struct drm_device *drm = &mdev->drm;
	struct m9_warm_snapshot snapshot;
	u32 line;
	int ret;

	head->mdev = mdev;
	head->index = index;
	head->dc = m9_head_banks[index].dc;
	head->dct = m9_head_banks[index].dct;
	head->clksel = m9_head_banks[index].clksel;
	head->link = m9_head_banks[index].link;
	head->cursor_bank = m9_head_banks[index].cursor_bank;
	head->watermark = m9_head_banks[index].watermark;
	head->hold = m9_head_banks[index].hold;
	head->companion_dct = m9_head_banks[index].companion_dct;
	head->half_block = m9_head_banks[index].half_block;
	head->clk_select = m9_head_banks[index].clk_select;
	head->fs = M9_FS_REG(head->clk_select);
	head->vblank_mask = index < m9_vblank_count && m9_vblank_mask[index] ?
			    m9_vblank_mask[index] : m9_vblank_default[index];
	head->ddc_index = index;
	head->ddc = m9x_ddc_adapter(mdev, index);

	m9_capture_warm_snapshot(head, &snapshot);
	/* Programming modes means the controller no longer holds the firmware's
	 * state after the first modeset, so adopt whatever it is running. */
	if (!index && !m9_warm_snapshot_valid(&snapshot) && !m9_kms_modeset)
		return dev_err_probe(&mdev->pdev->dev, -EOPNOTSUPP,
			"untested M9 display state; refusing guarded warm KMS\n");
	head->warm_dc_ctrl = snapshot.ctrl;
	head->warm_dc_pitch = snapshot.pitch;
	head->warm_dc_start = snapshot.start;
	head->warm_dvo = snapshot.dvo;
	head->warm_valid = true;
	head->posted = m9_warm_snapshot_valid(&snapshot);
	if (m9_warm_snapshot_valid(&snapshot)) {
		head->warm_mode = m9_warm_mode;
		drm_mode_set_name(&head->warm_mode);
	} else {
		m9x_mode_read_current(head, &head->warm_mode);
	}
	/* A head the firmware left dark reads back as an empty raster, not as
	 * a mode: its registers are zero, so every field decodes to one.  Fall
	 * back to the warm timing until something programs it. */
	if (head->warm_mode.hdisplay < 64 || head->warm_mode.vdisplay < 64 ||
	    head->warm_mode.htotal <= head->warm_mode.hdisplay ||
	    head->warm_mode.vtotal <= head->warm_mode.vdisplay ||
	    head->warm_mode.clock < M9_CLOCK_MIN_KHZ ||
	    head->warm_mode.clock > M9_CLOCK_MAX_KHZ) {
		head->warm_mode = m9_warm_mode;
		drm_mode_set_name(&head->warm_mode);
		head->warm_valid = false;
	}
	line = m9x_read(mdev, head->dct + M9_DCT_LINE);
	usleep_range(1000, 1200);
	drm_info(drm, "head %u: DC %#06x timing %#06x synth %#06x link %#06x "
		 "vblank mask %#x, line %u -> %u, found " DRM_MODE_FMT "\n",
		 index, head->dc, head->dct, head->fs, head->link,
		 head->vblank_mask, line,
		 m9x_read(mdev, head->dct + M9_DCT_LINE),
		 DRM_MODE_ARG(&head->warm_mode));

	if (m9x_kms_scanout_enabled()) {
		ret = m9_scanout_init(head);
		if (ret)
			return ret;
	}
	ret = drm_simple_display_pipe_init(drm, &head->pipe, &m9_pipe_funcs,
					   m9_formats, ARRAY_SIZE(m9_formats),
					   NULL, NULL);
	if (ret)
		return ret;
	/* Each connector owns its own bit-banged bus, so userspace sees which
	 * one carries its EDID. */
	ret = drm_connector_init_with_ddc(drm, &head->connector,
					  &matrox_kms_connector_funcs,
					  DRM_MODE_CONNECTOR_DisplayPort,
					  head->ddc);
	if (ret)
		return ret;
	drm_connector_helper_add(&head->connector, &m9_connector_helper_funcs);
	ret = drm_connector_attach_encoder(&head->connector,
					   &head->pipe.encoder);
	if (ret)
		return ret;
	/* The overlay rides the same controller, so it belongs to this CRTC. */
	ret = drm_universal_plane_init(drm, &head->cursor_plane,
				       drm_crtc_mask(&head->pipe.crtc),
				       &m9_cursor_funcs, m9_cursor_formats,
				       ARRAY_SIZE(m9_cursor_formats), NULL,
				       DRM_PLANE_TYPE_CURSOR, "cursor");
	if (ret)
		return ret;
	drm_plane_helper_add(&head->cursor_plane, &m9_cursor_helper_funcs);
	/* drm_simple_display_pipe_init() builds the CRTC without a cursor
	 * plane, and the legacy cursor ioctl has nothing to target until the
	 * CRTC points at one, so X falls back to its software cursor. */
	head->pipe.crtc.cursor = &head->cursor_plane;
	ret = drm_mode_crtc_set_gamma_size(&head->pipe.crtc, 256);
	if (ret)
		return ret;
	drm_crtc_enable_color_mgmt(&head->pipe.crtc, 0, false, 256);
	return 0;
}

int m9x_modeset_init(struct m9x_device *mdev)
{
	struct drm_device *drm = &mdev->drm;
	unsigned int i;
	int ret;

	ret = m9_kms_selftest();
	if (ret)
		return ret;
	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.funcs = &m9_mode_config_funcs;
	/* The scanout size is pinned by m9_fb_create; the framebuffer bounds
	 * must still admit the small cursor image. */
	drm->mode_config.min_width = 1;
	drm->mode_config.max_width = M9_SCANOUT_MAX_WIDTH;
	drm->mode_config.min_height = 1;
	drm->mode_config.max_height = M9_SCANOUT_MAX_HEIGHT;
	drm->mode_config.prefer_shadow = 0;
	drm->mode_config.cursor_width = M9_CURSOR_SIZE;
	drm->mode_config.cursor_height = M9_CURSOR_SIZE;

	ret = m9x_ddc_init(mdev);
	if (ret)
		return ret;
	/* One head per DDC bus that the board populates, so a connector always
	 * owns the bus its display answers on and connector n drives link n.
	 * Heads beyond the posted one are only brought up when the driver may
	 * program their timing. */
	mdev->heads = min3(mdev->ddc_count, M9_MAX_HEADS,
			   m9_kms_modeset ? M9_MAX_HEADS : 1u);
	if (!mdev->heads)
		mdev->heads = 1;
	for (i = 0; i < mdev->heads; i++) {
		ret = m9_head_init(mdev, &mdev->head[i], i);
		if (ret)
			return ret;
	}
	drm_mode_config_reset(drm);
	mdev->kms_initialized = true;
	return 0;
}

void m9x_modeset_fini(struct m9x_device *mdev)
{
	unsigned int i;

	m9x_write(mdev, M9_REG_IRQ_LOW_MASK, 0);
	m9x_write(mdev, M9_REG_IRQ_HIGH_MASK, 0);
	m9x_write(mdev, M9_REG_IRQ0_MASK, 0);
	if (mdev->irq_installed) {
		free_irq(mdev->pdev->irq, mdev);
		mdev->irq_installed = false;
	}
	/*
	 * Return every controller to the scanout it was found with before TTM
	 * releases the last pinned dumb buffer.  Restoring CTRL also restores
	 * the firmware's LUT bypass choice, so no driver-programmed gamma table
	 * remains selected.
	 */
	wmb();
	for (i = 0; i < mdev->heads; i++) {
		struct m9_head *head = &mdev->head[i];

		m9_cursor_fini(head);
		drm_edid_free(head->edid);
		head->edid = NULL;
		head->edid_valid = false;
		m9x_write(mdev, head->dc + M9_DC_START, head->warm_dc_start);
		m9x_write(mdev, head->dc + M9_DC_PITCH, head->warm_dc_pitch);
		m9x_write(mdev, head->dc + M9_DC_CTRL, head->warm_dc_ctrl);
		m9_scanout_fini(head);
	}
	mdev->kms_initialized = false;
}
