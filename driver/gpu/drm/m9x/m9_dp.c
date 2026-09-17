// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * DisplayPort mode and link training.
 *
 * The transmitter owns the training state machine: there is no per-phase
 * clock-recovery or channel-equalisation loop here and no voltage-swing or
 * pre-emphasis stepping, because the vendor X driver and the VBIOS both hand
 * the chip a rate and a lane count, toggle one bit and wait.  Both
 * implementations are decoded in xf86-video-parhelia
 * docs/parhelia-m9148-displayport-transmitter-re.md; where they differ this
 * file follows the X driver, which derives the rate from the pixel clock
 * instead of forcing 1.62 Gbps whenever the sink offers four lanes.
 */
#include <linux/delay.h>
#include <linux/jiffies.h>

#include <drm/drm_print.h>

#include "m9_dp.h"
#include "m9_drv.h"
#include "m9_reg.h"

/* DPCD addresses reached over AUX when the mirror does not answer. */
#define M9_DPCD_MAX_LINK_RATE	0x00001
#define M9_DPCD_LANE0_1_STATUS	0x00202
#define M9_DPCD_LINK_BW_SET	0x00100
#define M9_DPCD_SET_POWER	0x00600

#define M9_DPCD_LANE_MASK	0x1fu
#define M9_DPCD_LANE_OK		0x07u	/* CR | EQ | symbol lock, one lane  */
#define M9_DPCD_LANE_PAIR_OK	0x77u	/* both lanes of a status byte      */
#define M9_DPCD_ALIGN_DONE	BIT(0)
#define M9_DPCD_POWER_D0	0x01u
#define M9_DPCD_POWER_D3	0x02u

#define M9_DP_AUX_TIMEOUT_MS	200u
#define M9_DP_TRAIN_TIMEOUT_MS	500u
#define M9_DP_SETTLE_MS		200u
#define M9_DP_TRAIN_TRIES	3u
#define M9_DP_MODE_TRIES	3u
/* A sink with no AUX channel never reports capabilities, so retrying the whole
 * mode init three times only costs time; one retry covers a transient. */
#define M9_DP_CAPS_TRIES	2u
#define M9_DP_PLL_RETRIES	5u
#define M9_DP_PLL_SETTLE_US	1200u
#define M9_DP_PLL_RESET		BIT(7)

/*
 * Output gate bits in M9_DP_TX_GATE.  Bit 4 blanks the stream and leaves AUX
 * and the trained link alone; bit 5 kills both and needs a full re-train
 * afterwards, so nothing here ever sets it.  MEASURED, and consistent with the
 * vendor blank, which is (v & ~0x20) | 0x10.
 */
#define M9_DP_GATE_BLANK	BIT(4)
#define M9_DP_GATE_LINK_OFF	BIT(5)

/*
 * Largest pixel clock in kHz each link carries, by rate then by lane count.
 * The vendor's own table, indexed (rate == 1.62 Gbps ? 0 : 4) + lanes - 1.
 */
static const u32 m9_dp_max_khz[2][4] = {
	{  45000,  85000, 125000, 165000 },	/* 1.62 Gbps */
	{  85000, 165000, 248000, 340000 },	/* 2.7 Gbps  */
};

struct m9_dp_regval { u8 reg, val; };

/* DisplayPort mode, main page, in emission order. */
static const struct m9_dp_regval m9_dp_mode_main[] = {
	{ 0x2c, 0x00 }, { 0x2d, 0x00 }, { 0x2e, 0x0c }, { 0x76, 0xff },
	{ 0x78, 0x33 }, { 0x75, 0xf8 }, { 0x75, 0x00 }, { 0x87, 0x70 },
	{ 0x5f, 0x0c }, { 0x71, 0x20 }, { 0x5e, 0x04 }, { 0x87, 0x74 },
};

/* DisplayPort mode, page 3.  The VBIOS writes this half first; the X driver
 * writes it last.  Order between the two pages has no observed effect. */
static const struct m9_dp_regval m9_dp_mode_page3[] = {
	{ 0x00, 0x18 }, { 0x03, 0x06 }, { 0x24, 0x10 }, { 0x25, 0x04 },
	{ 0x26, 0x04 },
};

static int m9_dp_rd(struct m9x_device *mdev, unsigned int i, u8 page, u8 reg,
		    u8 *val)
{
	return m9x_tx_reg(mdev, i, page, reg, val, false);
}

static int m9_dp_wr(struct m9x_device *mdev, unsigned int i, u8 page, u8 reg,
		    u8 val)
{
	return m9x_tx_reg(mdev, i, page, reg, &val, true);
}

static int m9_dp_rmw(struct m9x_device *mdev, unsigned int i, u8 page, u8 reg,
		     u8 clear, u8 set)
{
	int ret;
	u8 v;

	ret = m9_dp_rd(mdev, i, page, reg, &v);
	if (ret)
		return ret;
	return m9_dp_wr(mdev, i, page, reg, (v & ~clear) | set);
}

static int m9_dp_table(struct m9x_device *mdev, unsigned int i, u8 page,
		       const struct m9_dp_regval *tab, size_t n)
{
	size_t k;
	int ret;

	for (k = 0; k < n; k++) {
		ret = m9_dp_wr(mdev, i, page, tab[k].reg, tab[k].val);
		if (ret)
			return ret;
	}
	return 0;
}

/* Poll the write-1-to-clear status register for any bit of @mask. */
static int m9_dp_wait(struct m9x_device *mdev, unsigned int i, u8 mask,
		      unsigned int ms)
{
	unsigned long end = jiffies + msecs_to_jiffies(ms);
	u8 v;

	do {
		if (m9_dp_rd(mdev, i, 0, M9_DP_TX_STATUS, &v))
			return -ENXIO;
		if (v & mask)
			return 0;
		cond_resched();
	} while (time_before(jiffies, end));
	return -ETIMEDOUT;
}

/*
 * One native AUX transaction, at most 16 bytes.  @cmd is already in the high
 * nibble.  The engine only answers while the output gate is open, so every
 * caller runs after the gate has been set to M9_DP_TX_GATE_ON.
 */
static int m9_dp_aux(struct m9x_device *mdev, unsigned int i, u32 addr, u8 cmd,
		     u8 *buf, u8 len)
{
	unsigned int k;
	u8 reply;
	int ret;

	if (addr > 0x7ffff || !len || len > M9_DP_AUX_MAX)
		return -EINVAL;
	ret = m9_dp_wr(mdev, i, 0, M9_DP_AUX_ADDR0, addr & 0xff);
	ret = ret ?: m9_dp_wr(mdev, i, 0, M9_DP_AUX_ADDR1, (addr >> 8) & 0xff);
	ret = ret ?: m9_dp_wr(mdev, i, 0, M9_DP_AUX_CMD,
			      cmd | ((addr >> 16) & 0x0f));
	ret = ret ?: m9_dp_wr(mdev, i, 0, M9_DP_AUX_LEN, len - 1);
	if (ret)
		return ret;
	if (cmd == M9_DP_AUX_CMD_WRITE)
		for (k = 0; k < len; k++) {
			ret = m9_dp_wr(mdev, i, 0, M9_DP_AUX_WDATA, buf[k]);
			if (ret)
				return ret;
		}
	ret = m9_dp_wr(mdev, i, 0, M9_DP_TX_STATUS, M9_DP_IRQ_AUX_DONE);
	ret = ret ?: m9_dp_wr(mdev, i, 0, M9_DP_AUX_GO, 0x01);
	ret = ret ?: m9_dp_wait(mdev, i, M9_DP_IRQ_AUX_DONE,
				M9_DP_AUX_TIMEOUT_MS);
	if (ret)
		return ret;
	ret = m9_dp_rd(mdev, i, 0, M9_DP_AUX_REPLY, &reply);
	m9_dp_wr(mdev, i, 0, M9_DP_TX_STATUS, M9_DP_IRQ_AUX_DONE);
	if (ret)
		return ret;
	if (reply & M9_DP_AUX_REPLY_CODE)
		return -EIO;
	if (cmd != M9_DP_AUX_CMD_READ)
		return 0;
	if ((reply & M9_DP_AUX_REPLY_COUNT) != len)
		return -EIO;
	for (k = 0; k < len; k++) {
		ret = m9_dp_rd(mdev, i, 0, M9_DP_AUX_RDATA, &buf[k]);
		if (ret)
			return ret;
	}
	return 0;
}

static int m9_dp_dpcd_read(struct m9x_device *mdev, unsigned int i, u32 addr,
			   u8 *buf, u8 len)
{
	return m9_dp_aux(mdev, i, addr, M9_DP_AUX_CMD_READ, buf, len);
}

static int m9_dp_dpcd_write(struct m9x_device *mdev, unsigned int i, u32 addr,
			    u8 val)
{
	return m9_dp_aux(mdev, i, addr, M9_DP_AUX_CMD_WRITE, &val, 1);
}

/*
 * Wait for the transmitter's I2C bridge to settle, resetting the PHY PLL if it
 * reports a lost lock.  The vendor runs this between the mode tables and the
 * first training attempt.
 */
static void m9_dp_settle(struct m9x_device *mdev, unsigned int i)
{
	unsigned long end = jiffies + msecs_to_jiffies(M9_DP_SETTLE_MS);
	unsigned int resets = 0;
	u8 v, pll;

	do {
		if (m9_dp_rd(mdev, i, 0, M9_DP_TX_STATUS, &v))
			break;
		if ((v & M9_DP_IRQ_AUX_ERROR) && resets++ < M9_DP_PLL_RETRIES) {
			if (!m9_dp_rd(mdev, i, 0, M9_DP_TX_FAULT, &pll)) {
				m9_dp_wr(mdev, i, 0, M9_DP_TX_FAULT,
					 pll | M9_DP_PLL_RESET);
				m9_dp_wr(mdev, i, 0, M9_DP_TX_FAULT,
					 pll & ~M9_DP_PLL_RESET);
			}
			m9_dp_wr(mdev, i, 0, M9_DP_TX_STATUS,
				 M9_DP_IRQ_AUX_ERROR);
			usleep_range(M9_DP_PLL_SETTLE_US,
				     M9_DP_PLL_SETTLE_US + 200);
			end = jiffies + msecs_to_jiffies(M9_DP_SETTLE_MS);
			continue;
		}
		if (v & M9_DP_IRQ_BRIDGE)
			break;
		cond_resched();
	} while (time_before(jiffies, end));
	m9_dp_wr(mdev, i, 0, M9_DP_TX_STATUS, M9_DP_IRQ_BRIDGE);
}

/*
 * Arm the capability mirror.  Both vendor implementations open link training
 * with exactly these two accesses and nothing else before reading 0x51/0x52,
 * and the mirror reads zero on a board that has not had them.
 */
static void m9_dp_arm_mirror(struct m9x_device *mdev, unsigned int i)
{
	u8 v;

	m9_dp_wr(mdev, i, 0, M9_DP_ARM_B, 0xe4);
	if (!m9_dp_rd(mdev, i, 0, M9_DP_ARM_A, &v))
		m9_dp_wr(mdev, i, 0, M9_DP_ARM_A, v & 0xf0);
}

/* Receiver capability: the mirror when it answers, otherwise the sink itself. */
static int m9_dp_caps(struct m9x_device *mdev, unsigned int i, u8 *rate,
		      u8 *lanes)
{
	u8 mirror_rate = 0, mirror_lanes = 0, cap[2];
	int ret;

	m9_dp_rd(mdev, i, 0, M9_DP_CAP_MAX_RATE, &mirror_rate);
	m9_dp_rd(mdev, i, 0, M9_DP_CAP_MAX_LANES, &mirror_lanes);
	if (mirror_lanes & M9_DPCD_LANE_MASK) {
		*rate = mirror_rate;
		*lanes = mirror_lanes;
		return 0;
	}
	ret = m9_dp_dpcd_read(mdev, i, M9_DPCD_MAX_LINK_RATE, cap,
			      sizeof(cap));
	if (ret) {
		drm_dbg_kms(&mdev->drm,
			    "head %u: no receiver capability, mirror %#04x/%#04x, AUX %d\n",
			    i, mirror_rate, mirror_lanes, ret);
		return ret;
	}
	*rate = cap[0];
	*lanes = cap[1];
	return 0;
}

/*
 * Link rate codes are monotonic, so a plain comparison covers a sink that
 * advertises more than the transmitter is driven at.  The sink on this board
 * reports 0x14 (5.4 Gbps) and neither vendor implementation ever writes a rate
 * above 2.7 Gbps, so that is the cap.  Masking the code with 0x0f, as the
 * VBIOS does, turns 0x14 into 4 and silently picks 1.62 Gbps.
 */
static u8 m9_dp_pick_rate(u8 max_rate, u8 lanes, u32 pixel_khz)
{
	unsigned int idx = clamp_t(unsigned int, lanes, 1, 4) - 1;

	if (max_rate < M9_DP_RATE_HBR)
		return M9_DP_RATE_RBR;
	return pixel_khz > m9_dp_max_khz[0][idx] ? M9_DP_RATE_HBR :
						   M9_DP_RATE_RBR;
}

/* The sink's own view of the link, over AUX; 0x63..0x65 mirror the same bytes. */
static bool m9_dp_link_ok(struct m9x_device *mdev, unsigned int i, u8 lanes)
{
	u8 st[3];

	if (m9_dp_dpcd_read(mdev, i, M9_DPCD_LANE0_1_STATUS, st, sizeof(st)))
		return false;
	if (!(st[2] & M9_DPCD_ALIGN_DONE))
		return false;
	if (lanes >= 2) {
		if ((st[0] & M9_DPCD_LANE_PAIR_OK) != M9_DPCD_LANE_PAIR_OK)
			return false;
	} else if ((st[0] & M9_DPCD_LANE_OK) != M9_DPCD_LANE_OK) {
		return false;
	}
	if (lanes > 2 &&
	    (st[1] & M9_DPCD_LANE_PAIR_OK) != M9_DPCD_LANE_PAIR_OK)
		return false;
	return true;
}

/* One pass of the transmitter's own training sequence. */
static int m9_dp_train_once(struct m9x_device *mdev, unsigned int i, u8 rate,
			    u8 lanes)
{
	u8 v, bw;
	int ret;

	/* The lane byte goes out as the sink reported it, which is what both
	 * vendor implementations do: bit 7 is enhanced framing and the chip is
	 * expected to ignore the rest.  If training fails with "master" on a
	 * sink reporting bit 6 (TPS3), masking to 0x9f is the first thing to
	 * try. */
	ret = m9_dp_wr(mdev, i, 0, M9_DP_LINK_BW, rate);
	ret = ret ?: m9_dp_wr(mdev, i, 0, M9_DP_LANE_COUNT, lanes);
	ret = ret ?: m9_dp_wr(mdev, i, 0, M9_DP_PATTERN, 0x00);
	ret = ret ?: m9_dp_wr(mdev, i, 0, M9_DP_TX_STATUS, M9_DP_IRQ_TRAINED);
	ret = ret ?: m9_dp_rd(mdev, i, 0, M9_DP_TRAIN, &v);
	if (ret)
		return ret;
	m9_dp_wr(mdev, i, 0, M9_DP_TRAIN, v & ~M9_DP_TRAIN_START);
	ret = m9_dp_wr(mdev, i, 0, M9_DP_TRAIN, v | M9_DP_TRAIN_START);
	if (ret)
		return ret;

	ret = m9_dp_wait(mdev, i, M9_DP_IRQ_TRAINED, M9_DP_TRAIN_TIMEOUT_MS);
	if (ret) {
		drm_dbg_kms(&mdev->drm, "head %u: link training timeout\n", i);
		return ret;
	}
	m9_dp_wr(mdev, i, 0, M9_DP_STATUS_SEL, M9_DP_STATUS_SEL_TRAIN);
	ret = m9_dp_rd(mdev, i, 0, M9_DP_STATUS, &v);
	m9_dp_wr(mdev, i, 0, M9_DP_TX_STATUS, M9_DP_IRQ_TRAINED);
	if (ret)
		return ret;
	/* The VBIOS accepts bit 6 alone, the X driver the whole field; a zero
	 * readback means the status mirror did not answer and is not a
	 * failure report. */
	if (v && (v & M9_DP_STATUS_MASK) != M9_DP_STATUS_TRAINED) {
		drm_dbg_kms(&mdev->drm,
			    "head %u: link training failed - master %#04x\n",
			    i, v);
		return -EIO;
	}
	if (!m9_dp_link_ok(mdev, i, lanes & M9_DPCD_LANE_MASK)) {
		drm_dbg_kms(&mdev->drm,
			    "head %u: link training failed - slave\n", i);
		return -EIO;
	}
	ret = m9_dp_dpcd_read(mdev, i, M9_DPCD_LINK_BW_SET, &bw, 1);
	if (ret || bw != rate) {
		drm_dbg_kms(&mdev->drm,
			    "head %u: link training failed - bandwidth (%d, %#04x)\n",
			    i, ret, bw);
		return -EIO;
	}
	return 0;
}

/* Take the sink out of D3 so AUX and training have a partner. */
static void m9_dp_power_up_sink(struct m9x_device *mdev, unsigned int i)
{
	u8 power;

	if (m9_dp_dpcd_read(mdev, i, M9_DPCD_SET_POWER, &power, 1))
		return;
	if (power != M9_DPCD_POWER_D3)
		return;
	m9_dp_dpcd_write(mdev, i, M9_DPCD_SET_POWER, M9_DPCD_POWER_D0);
	drm_dbg_kms(&mdev->drm, "head %u: sink was in D3\n", i);
}

/*
 * Put one connector's transmitter into DisplayPort mode and train its link.
 * Runs after the common bring-up in m9_ddc.c has initialised the core and
 * opened the output gate, and before the pad gate is cleared: the AUX engine
 * and the capability mirror are both dead while the gate is closed.
 */
int m9x_dp_train(struct m9x_device *mdev, unsigned int index, u32 pixel_khz)
{
	unsigned int attempt, try;
	u8 max_rate, lanes, rate;
	int ret;

	/* The status register only latches sources enabled here, and the
	 * firmware leaves this value on the connector it trained. */
	m9_dp_wr(mdev, index, 0, M9_DP_TX_EVENT, M9_DP_EVENT_ENABLE);

	for (attempt = 0; attempt < M9_DP_MODE_TRIES; attempt++) {
		ret = m9_dp_table(mdev, index, 0, m9_dp_mode_main,
				  ARRAY_SIZE(m9_dp_mode_main));
		ret = ret ?: m9_dp_rmw(mdev, index, 0, M9_DP_TX_MAIN, BIT(3),
				       0);
		ret = ret ?: m9_dp_table(mdev, index, 2, m9_dp_mode_page3,
					 ARRAY_SIZE(m9_dp_mode_page3));
		ret = ret ?: m9_dp_wr(mdev, index, 0, M9_DP_TX_GATE,
				      M9_DP_TX_GATE_ON);
		if (ret)
			return ret;

		m9_dp_settle(mdev, index);
		m9_dp_rmw(mdev, index, 2, M9_DP_TX_PAGE3_MARK, 0, BIT(0));
		m9_dp_rmw(mdev, index, 2, M9_DP_TX_PAGE3_MARK, BIT(0), 0);

		m9_dp_arm_mirror(mdev, index);
		m9_dp_power_up_sink(mdev, index);
		ret = m9_dp_caps(mdev, index, &max_rate, &lanes);
		if (ret || !(lanes & M9_DPCD_LANE_MASK)) {
			/*
			 * A dual-mode consumer - a passive adapter or DP++
			 * dongle - answers EDID through the transmitter's I2C
			 * bridge and has no AUX channel at all, so no number
			 * of mode attempts will produce a capability block.
			 * MEASURED.  Give a transient one retry, then stop:
			 * the caller drives the pad unconditioned, which is
			 * how such a sink is meant to work.
			 */
			if (attempt + 1 >= M9_DP_CAPS_TRIES)
				return -ENOTCONN;
			ret = ret ?: -ENODEV;
			continue;
		}
		rate = m9_dp_pick_rate(max_rate, lanes & M9_DPCD_LANE_MASK,
				       pixel_khz);
		/* Zero for a sink the transmitter reaches directly. */
		m9_dp_wr(mdev, index, 0, M9_DP_DONGLE, 0x00);

		for (try = 0; try < M9_DP_TRAIN_TRIES; try++) {
			ret = m9_dp_train_once(mdev, index, rate, lanes);
			if (!ret)
				break;
		}
		if (!ret) {
			drm_dbg_kms(&mdev->drm,
				    "head %u: DP link up, %u lanes at %s\n",
				    index, lanes & M9_DPCD_LANE_MASK,
				    rate == M9_DP_RATE_HBR ? "2.7 Gbps" :
							     "1.62 Gbps");
			return 0;
		}
	}
	drm_info(&mdev->drm, "head %u: DP link training failed (%d)\n", index,
		 ret);
	return ret;
}
