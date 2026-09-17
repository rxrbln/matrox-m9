// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * Native timing and pixel clock for the M9's four display controllers.  The
 * divider search, the register encodings and the vid-reset handshake are the
 * ones recovered for the X driver in xf86-video-parhelia src/parhelia_mode.c
 * and src/parhelia_pll.c; the per-head banks, the clock-select rule and the
 * half-block controllers are measured in
 * xf86-video-parhelia docs/parhelia-m9148-fourhead-outputs.md.
 */
#include <linux/delay.h>
#include <linux/module.h>

#include <drm/drm_modes.h>
#include <drm/drm_print.h>

#include "m9_drv.h"
#include "m9_reg.h"

/*
 * 0x1cac does not accept writes on this board: MEASURED, every value read back
 * as 0x00000001, including rewriting its own POST value, and only a reboot
 * restores it.  It reads as status - high nibble a per-pad enable request, low
 * nibble per-pad up, DERIVED - so this cannot enable a head and is kept only
 * to try a value on a board that behaves differently.
 */
static int m9_pad_gate = -1;
module_param_named(pad_gate, m9_pad_gate, int, 0600);
MODULE_PARM_DESC(pad_gate,
	"Value written to 0x1cac after a link is programmed; -1 leaves it alone.  Measured to be ignored on the M9148");

struct m9_pll {
	u8 b0, b1, b2, b3;
	u32 out_khz;
};

static u32 m9_pll_out(u32 b0, u32 b1, u32 b2, u32 b3)
{
	return (u32)((u64)M9_FS_REF_KHZ * b0 * b1 / b2 / (2 * b3));
}

/* Take the divider set with the smallest error the field widths allow.  The
 * vendor's own 1920x1200 word is one of the exact solutions this finds. */
static bool m9_pll_solve(u32 target_khz, struct m9_pll *out)
{
	u32 best = U32_MAX, b0, b1, b2, b3;

	if (!target_khz)
		return false;
	for (b0 = 1; b0 <= M9_FS_B0_MAX; b0++) {
		u32 vco1 = M9_FS_REF_KHZ * b0;

		if (vco1 < M9_VCO_MIN_KHZ || vco1 > M9_VCO_MAX_KHZ)
			continue;
		for (b2 = 1; b2 <= M9_FS_B2_MAX; b2++) {
			u32 tap = vco1 / b2;

			if (tap < M9_FS_OUT_MIN_KHZ || tap > M9_FS_OUT_MAX_KHZ)
				continue;
			for (b1 = 1; b1 <= M9_FS_B1_MAX; b1++) {
				u32 vco2 = (u32)((u64)vco1 * b1 / b2);

				if (vco2 < M9_VCO_MIN_KHZ ||
				    vco2 > M9_VCO_MAX_KHZ)
					continue;
				for (b3 = 1; b3 <= M9_FS_B3_MAX; b3++) {
					u32 f = vco2 / (2 * b3);
					u32 delta = f > target_khz ?
						f - target_khz : target_khz - f;

					if (delta >= best)
						continue;
					best = delta;
					*out = (struct m9_pll){ b0, b1, b2,
								b3, f };
				}
			}
		}
	}
	return best != U32_MAX;
}

static u32 m9_pll_encode(const struct m9_pll *s)
{
	return ((u32)(s->b0 - 1) & 0x3f) | (((u32)(s->b1 - 1) & 0x1f) << 6) |
	       (((u32)(s->b2 - 1) & 0x1f) << 11) |
	       (((u32)(s->b3 - 1) & 0x0f) << 16);
}

/* The synthesiser a controller is running from, by the measured select rule.
 * Selects outside the pixel range address a system or board clock. */
u32 m9x_mode_active_fs(struct m9_head *head)
{
	u32 sel = (m9x_read(head->mdev, head->clksel) & M9_CLK_SEL_MASK) >>
		  M9_CLK_SEL_SHIFT;

	if (sel < M9_FS_PIXEL_FIRST || sel > M9_FS_PIXEL_LAST)
		return 0;
	return M9_FS_REG(sel);
}

static int m9_program_clock(struct m9_head *head, u32 dot_khz)
{
	struct m9_pll pll;
	unsigned int i;
	u32 word, value;

	if (head->clk_select < M9_FS_PIXEL_FIRST ||
	    head->clk_select > M9_FS_PIXEL_LAST) {
		drm_err(&head->mdev->drm, "head %u has no pixel synthesiser\n",
			head->index);
		return -EINVAL;
	}
	if (!m9_pll_solve(dot_khz, &pll)) {
		drm_err(&head->mdev->drm, "no PLL divider for %u kHz\n", dot_khz);
		return -EINVAL;
	}
	word = m9_pll_encode(&pll);

	/* The oscillator is only disturbed when it is not already up. */
	value = m9x_read(head->mdev, M9_FS_OSC);
	if (((value >> 24) & 0xc0) != 0x40) {
		m9x_write(head->mdev, M9_FS_OSC, value & ~BIT(31));
		usleep_range(1000, 1200);
		m9x_write(head->mdev, M9_FS_OSC, value | BIT(30));
		usleep_range(1000, 1200);
	}

	m9x_write(head->mdev, head->fs, word | M9_FS_LOAD | M9_FS_INIT);
	m9x_write(head->mdev, head->fs, word);
	udelay(10);
	for (i = 0; i < 100; i++) {
		if (m9x_read(head->mdev, head->fs) & M9_FS_LOCK)
			break;
		udelay(10);
	}
	drm_info(&head->mdev->drm,
		 "head %u pixel clock %u kHz -> %u kHz (%u,%u,%u,%u) word %#010x%s\n",
		 head->index, dot_khz, pll.out_khz, pll.b0, pll.b1, pll.b2, pll.b3, word,
		 i == 100 ? " (no lock)" : "");

	value = m9x_read(head->mdev, head->clksel);
	if (!(value & M9_CLK_HOLD) &&
	    (value & M9_CLK_SEL_MASK) >> M9_CLK_SEL_SHIFT == head->clk_select)
		return 0;
	m9x_write(head->mdev, head->clksel, value | M9_CLK_HOLD);
	udelay(10);
	value = m9x_read(head->mdev, head->clksel) & ~0x000038e0u;
	m9x_write(head->mdev, head->clksel, value);
	value = (value & ~M9_CLK_SEL_MASK) |
		(head->clk_select << M9_CLK_SEL_SHIFT);
	m9x_write(head->mdev, head->clksel, value);
	m9x_write(head->mdev, head->clksel, value & ~M9_CLK_HOLD);
	return 0;
}

/* True while another controller is fetching, which the global vid reset would
 * interrupt.  The vendor's four-head bring-up issues no strobe at all, so a
 * head that joins a running set does without one.  DERIVED. */
static bool m9_other_head_enabled(struct m9_head *head)
{
	struct m9x_device *mdev = head->mdev;
	unsigned int i;

	for (i = 0; i < mdev->heads; i++)
		if (i != head->index &&
		    m9x_read(mdev, mdev->head[i].dc + M9_DC_CTRL) &
		    M9_DC_ENABLE)
			return true;
	return false;
}

/* A global strobe with this controller's timing-block reset held across it. */
static void m9_apply_vid_reset(struct m9_head *head)
{
	bool held = false;
	u32 value;

	if (m9_other_head_enabled(head))
		return;
	value = m9x_read(head->mdev, head->clksel);
	if (value & M9_CLK_HOLD) {
		m9x_write(head->mdev, head->clksel, (value & 1) | 0x2e);
		m9x_write(head->mdev, head->clksel, 0x2e);
		held = true;
	}
	value = m9x_read(head->mdev, head->dct + M9_DCT_CTRL);
	m9x_write(head->mdev, head->dct + M9_DCT_CTRL, value | M9_DCT_RESET);

	m9x_write(head->mdev, M9_REG_SYNCARM,
		  m9x_read(head->mdev, M9_REG_SYNCARM) & 0x0000000eu);
	value = m9x_read(head->mdev, M9_REG_VGAFLAG);
	m9x_write(head->mdev, M9_REG_VGAFLAG, value & ~M9_VIDRST_STROBE);
	m9x_write(head->mdev, M9_REG_VGAFLAG, value | M9_VIDRST_STROBE);
	udelay(10);
	m9x_write(head->mdev, M9_REG_VGAFLAG, value & ~M9_VIDRST_STROBE);
	m9x_write(head->mdev, M9_REG_SYNCARM, m9x_read(head->mdev, M9_REG_SYNCARM) | 1);

	value = m9x_read(head->mdev, head->dct + M9_DCT_CTRL);
	m9x_write(head->mdev, head->dct + M9_DCT_CTRL, value & ~M9_DCT_RESET);
	if (held)
		m9x_write(head->mdev, head->clksel,
			  m9x_read(head->mdev, head->clksel) | M9_CLK_HOLD);
}

/* Timing and fetch geometry, bracketed by the controller's hold bits so the
 * head never scans out a half-programmed set. */
static void m9_program_timing(struct m9_head *head,
			      const struct drm_display_mode *mode,
			      u32 pitch_pixels)
{
	u32 hd = mode->hdisplay, ht = mode->htotal;
	u32 hss = mode->hsync_start, hse = mode->hsync_end;
	u32 vd = mode->vdisplay, vt = mode->vtotal;
	u32 vss = mode->vsync_start, vse = mode->vsync_end;
	u32 value;

	value = m9x_read(head->mdev, head->hold);
	/* Copied: the vendor sets these two bits in the companion's hold word
	 * when a half-block controller comes up. */
	if (head->half_block)
		value |= M9_DC_HOLD_COMPANION;
	m9x_write(head->mdev, head->hold, value | M9_DC_HOLD_SET);
	if (!head->half_block) {
		value = m9x_read(head->mdev, head->dc + M9_DC_HOLD2);
		m9x_write(head->mdev, head->dc + M9_DC_HOLD2,
			  value & 0x00ffffffu);
	}

	m9x_write(head->mdev, head->dct + M9_DCT_HBLANK,
		  ((hd - 1) & M9_DCT_H_MASK) |
		  (((ht - 1) & M9_DCT_H_MASK) << 16));
	m9x_write(head->mdev, head->dct + M9_DCT_HTOTAL,
		  ((ht - 1) & M9_DCT_H_MASK) |
		  (((hd - 1) & M9_DCT_H_MASK) << 16));
	m9x_write(head->mdev, head->dct + M9_DCT_HSYNC,
		  ((hss - 1) & M9_DCT_H_MASK) |
		  (((hse - 1) & M9_DCT_H_MASK) << 16));
	m9x_write(head->mdev, head->dct + M9_DCT_VBLANK,
		  ((vd - 1) & M9_DCT_V_MASK) |
		  (((vt - 1) & M9_DCT_V_MASK) << 16));
	m9x_write(head->mdev, head->dct + M9_DCT_VTOTAL,
		  ((vt - 1) & M9_DCT_V_MASK) |
		  (((vd - 1) & M9_DCT_V_MASK) << 16));
	m9x_write(head->mdev, head->dct + M9_DCT_VSYNC,
		  ((vss - 1) & M9_DCT_V_MASK) |
		  (((vse - 1) & M9_DCT_V_MASK) << 16));
	/* Raw vsync end, no -1: the vendor's own 1920x1200 and 1024x768 states
	 * both hold exactly that, which the firmware's value does not. */
	m9x_write(head->mdev, head->dct + M9_DCT_VSYNCEND, vse & M9_DCT_V_MASK);

	m9x_write(head->mdev, head->dc + M9_DC_MISC, 0);
	m9x_write(head->mdev, head->dc + M9_DC_XSTART, 0);
	m9x_write(head->mdev, head->dc + M9_DC_XEND, (hd - 1) << 16);
	m9x_write(head->mdev, head->dc + M9_DC_YSTART, 0);
	m9x_write(head->mdev, head->dc + M9_DC_YEND, vd - 1);
	/* Copied: written for the half-block controllers alone, meaning not
	 * established. */
	if (head->half_block) {
		m9x_write(head->mdev, head->dc + M9_DC_XEND2, (hd - 1) << 16);
		m9x_write(head->mdev, head->dc + M9_DC_OPAQUE,
			  M9_DC_OPAQUE_VALUE);
		m9x_write(head->mdev, head->companion_dct + M9_DCT_HDISPLAY2,
			  (hd - 1) << 16);
	}
	m9x_write(head->mdev, head->dc + M9_DC_PITCH,
		  (pitch_pixels & M9_DC_PITCH_MASK) | (3u << 16) |
		  (M9_DC_FORMAT_XRGB8888 << M9_DC_PITCH_FORMAT_SHIFT));

	value = m9x_read(head->mdev, head->dc + M9_DC_CTRL);
	value &= ~(M9_DC_FORMAT_MASK | M9_DC_CTRL_INTERLACE);
	value |= (M9_DC_FORMAT_XRGB8888 << M9_DC_FORMAT_SHIFT) | M9_DC_ENABLE;
	m9x_write(head->mdev, head->dc + M9_DC_CTRL, value);

	value = m9x_read(head->mdev, head->hold);
	m9x_write(head->mdev, head->hold, value & ~M9_DC_HOLD_CLR);
	if (!head->half_block) {
		value = m9x_read(head->mdev, head->dc + M9_DC_HOLD2);
		m9x_write(head->mdev, head->dc + M9_DC_HOLD2,
			  value | 0xff000000u);
	}
}

/*
 * Route this controller to its digital link.  Connector n is wired to link n
 * and the board's own setup word for it comes from the PInS patch table, keyed
 * to the pixel clock - the values the firmware left are only right for the mode
 * it posted.  Clock source codes are not the controller number: controllers
 * 0..3 encode as 00, 08, 01, 09 (MEASURED, all four in one run).  The companion
 * format register holds two links in its two halves, so only this link's half
 * is patched.
 */
static void m9_program_link(struct m9_head *head,
			    const struct drm_display_mode *mode)
{
	static const u32 source[M9_MAX_HEADS] = { 0x00, 0x08, 0x01, 0x09 };
	struct m9x_device *mdev = head->mdev;
	unsigned int link = head->index;
	u32 value;

	m9x_pins_patch(mdev, M9_LINK_FORMAT_REG(link), mode->clock,
		       M9_LINK_FORMAT_MASK(link));
	m9x_pins_patch(mdev, head->link, mode->clock, ~0u);

	value = m9x_read(mdev, head->link);
	value &= ~(M9_DVO_SRC_MASK | M9_DVO_HSYNC_NEG | M9_DVO_VSYNC_NEG |
		   M9_DVO_PATH_OFF);
	value |= source[head->index];
	/* Bit 20 tracked the hsync polarity on all four measured modes; bit 22
	 * did not track the vsync polarity, so its meaning is unestablished and
	 * this only reproduces what the posted head was found with. */
	if (mode->flags & DRM_MODE_FLAG_NHSYNC)
		value |= M9_DVO_HSYNC_NEG;
	if (mode->flags & DRM_MODE_FLAG_NVSYNC)
		value |= M9_DVO_VSYNC_NEG;
	m9x_write(mdev, head->link, value);
	if (m9_pad_gate >= 0)
		m9x_write(mdev, M9_REG_PAD_GATE, m9_pad_gate);
	drm_info(&mdev->drm, "head %u link %#06x = %#010x, format %#06x = %#010x\n",
		 head->index, head->link, m9x_read(mdev, head->link),
		 M9_LINK_FORMAT_REG(link),
		 m9x_read(mdev, M9_LINK_FORMAT_REG(link)));
}

int m9x_mode_program(struct m9_head *head,
		     const struct drm_display_mode *mode, u32 pitch_pixels)
{
	int ret;

	ret = m9_program_clock(head, mode->clock);
	if (ret)
		return ret;
	/* One watermark word served all four heads and all four modes of the
	 * measured run; the firmware leaves it zero on every head. */
	m9x_write(head->mdev, head->watermark, M9_WATERMARK_VALUE);
	m9_program_timing(head, mode, pitch_pixels);
	m9_program_link(head, mode);
	/* The serializers latch their source from a controller that is already
	 * running, so the reset comes after the timing. */
	m9_apply_vid_reset(head);
	drm_info(&head->mdev->drm, "head %u programmed " DRM_MODE_FMT " pitch %u pixels\n",
		 head->index, DRM_MODE_ARG(mode), pitch_pixels);
	return 0;
}

/* Reconstruct the timing the controller is running from its own registers, so
 * a driver that programs modes itself can adopt whatever state it left behind
 * instead of demanding the firmware's. */
void m9x_mode_read_current(struct m9_head *head,
			   struct drm_display_mode *mode)
{
	u32 hblank = m9x_read(head->mdev, head->dct + M9_DCT_HBLANK);
	u32 hsync = m9x_read(head->mdev, head->dct + M9_DCT_HSYNC);
	u32 vblank = m9x_read(head->mdev, head->dct + M9_DCT_VBLANK);
	u32 vsync = m9x_read(head->mdev, head->dct + M9_DCT_VSYNC);
	u32 fs = m9x_mode_active_fs(head);
	u32 word = fs ? m9x_read(head->mdev, fs) : 0;
	u32 b0 = (word & 0x3f) + 1, b1 = ((word >> 6) & 0x1f) + 1;
	u32 b2 = ((word >> 11) & 0x1f) + 1, b3 = ((word >> 16) & 0x0f) + 1;

	memset(mode, 0, sizeof(*mode));
	mode->hdisplay = (hblank & M9_DCT_H_MASK) + 1;
	mode->htotal = ((hblank >> 16) & M9_DCT_H_MASK) + 1;
	mode->hsync_start = (hsync & M9_DCT_H_MASK) + 1;
	mode->hsync_end = ((hsync >> 16) & M9_DCT_H_MASK) + 1;
	mode->vdisplay = (vblank & M9_DCT_V_MASK) + 1;
	mode->vtotal = ((vblank >> 16) & M9_DCT_V_MASK) + 1;
	mode->vsync_start = (vsync & M9_DCT_V_MASK) + 1;
	mode->vsync_end = ((vsync >> 16) & M9_DCT_V_MASK) + 1;
	/* A controller the firmware left dark selects no pixel synthesiser, and
	 * a zero clock is what makes the caller reject the timing. */
	mode->clock = fs ? m9_pll_out(b0, b1, b2, b3) : 0;
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	mode->flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC;
	drm_mode_set_name(mode);
}
