// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * M9 native DDC: bit-banged I2C on MMIO 0x1c48, with the pin pairs taken from
 * the PInS 8.0 function map and the register owned through the board
 * controller's request/grant handshake
 * (xf86-video-parhelia docs/parhelia-m9148-kms-pins-dp-re.md).
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>

#include <drm/drm_edid.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>

#include "m9_drv.h"
#include "m9_reg.h"

#define M9_PINS_SIGNATURE	0x412e
#define M9_PINS_POINTER		0x7ffc
#define M9_PINS_LENGTH		0x0002
#define M9_PINS_VERSION		0x0004
#define M9_PINS_MIN_VERSION	0x0800
#define M9_PINS_SERIAL		0x0016
#define M9_PINS_SERIAL_BYTES	9
#define M9_PINS_NAME		0x0030
#define M9_PINS_NAME_BYTES	15
#define M9_PINS_DDC_MAP		0x0050	/* 8 function IDs, physical bit order */
#define M9_PINS_DDC_MAP_BYTES	8
#define M9_PINS_PATCH_START	0x000e	/* -> register-patch table */

/*
 * The option ROM in flash and the copy POST left in RAM do not carry the same
 * PInS: the flash image is generic and the VBIOS specialises the shadow for
 * the board it finds.  Reading the ROM BAR on an M9148 yields the two-bus DDC
 * map of the smaller boards and caps the driver at two heads.  The vendor X
 * driver maps exactly this window from /dev/mem and refuses to run without it
 * ("Cannot map Shadow BIOS" / "BIOS was not run?"), so the shadow is the
 * authority and the BAR only a fallback.  CAPTURED.
 */
#define M9_SHADOW_BIOS_BASE	0x000c0000u
#define M9_SHADOW_BIOS_SIZE	0x00020000u
#define M9_ROM_PCIR_POINTER	0x0018

/*
 * The HMC shares both GPIO registers with the driver.  Acquisition is
 * active low: clear the request bit, then wait for the grant.
 */
static int m9_hmc_acquire(struct m9x_device *mdev)
{
	unsigned int i;

	guard(mutex)(&mdev->ddc_lock);
	if (mdev->ddc_owned++)
		return 0;
	m9x_write(mdev, M9_HMC_REQUEST,
		  m9x_read(mdev, M9_HMC_REQUEST) & ~M9_HMC_DDC_REQUEST);
	for (i = 0; i < M9_HMC_GRANT_TRIES; i++) {
		if (m9x_read(mdev, M9_HMC_GRANT) & M9_HMC_DDC_GRANT)
			return 0;
		usleep_range(1000, 1200);
	}
	m9x_write(mdev, M9_HMC_REQUEST,
		  m9x_read(mdev, M9_HMC_REQUEST) | M9_HMC_DDC_REQUEST);
	mdev->ddc_owned--;
	return -ETIMEDOUT;
}

static void m9_hmc_release(struct m9x_device *mdev)
{
	guard(mutex)(&mdev->ddc_lock);
	if (WARN_ON(!mdev->ddc_owned) || --mdev->ddc_owned)
		return;
	m9x_write(mdev, M9_HMC_REQUEST,
		  m9x_read(mdev, M9_HMC_REQUEST) | M9_HMC_DDC_REQUEST);
}

/* Open drain: release the line by clearing its output enable, drive it low by
 * enabling the output while the data latch holds zero. */
static void m9_ddc_line(struct m9_ddc_bus *bus, u8 pin, int high)
{
	u32 value = m9x_read(bus->mdev, M9_DDC) &
		    ~(BIT(pin) | BIT(pin + M9_DDC_OE_SHIFT));

	if (!high)
		value |= BIT(pin + M9_DDC_OE_SHIFT);
	m9x_write(bus->mdev, M9_DDC, value);
}

static int m9_ddc_sense(struct m9_ddc_bus *bus, u8 pin)
{
	return !!(m9x_read(bus->mdev, M9_DDC) & BIT(pin));
}

static void m9_ddc_setsda(void *data, int state)
{
	m9_ddc_line(data, ((struct m9_ddc_bus *)data)->sda, state);
}

static void m9_ddc_setscl(void *data, int state)
{
	m9_ddc_line(data, ((struct m9_ddc_bus *)data)->scl, state);
}

static int m9_ddc_getsda(void *data)
{
	return m9_ddc_sense(data, ((struct m9_ddc_bus *)data)->sda);
}

static int m9_ddc_getscl(void *data)
{
	return m9_ddc_sense(data, ((struct m9_ddc_bus *)data)->scl);
}

static int m9_ddc_pre_xfer(struct i2c_adapter *adapter)
{
	struct m9_ddc_bus *bus = i2c_get_adapdata(adapter);

	return m9_hmc_acquire(bus->mdev);
}

static void m9_ddc_post_xfer(struct i2c_adapter *adapter)
{
	struct m9_ddc_bus *bus = i2c_get_adapdata(adapter);

	m9_hmc_release(bus->mdev);
}

/* PInS 8.0 keeps eight DDC function IDs in physical-bit order; the abstract
 * DDC selectors are the consecutive pairs (01,02) (03,04) ... (0f,10). */
static int m9_pins_ddc_bit(const u8 *map, u8 id)
{
	unsigned int bit;

	for (bit = 0; bit < M9_PINS_DDC_MAP_BYTES; bit++)
		if (map[bit] == id)
			return bit;
	return -ENOENT;
}

/*
 * PInS 8.0 register patches: a list of {register, flags, clock selector} with
 * an optional data word, mask and above-the-threshold alternative.  The board
 * keys its link setup to the pixel clock, so the values the firmware left for
 * its own mode are wrong for any other one.  @keep narrows the write further,
 * so one head can patch its own half of a register shared with another.
 */
void m9x_pins_patch(struct m9x_device *mdev, u16 reg, u32 dot_khz, u32 keep)
{
	const u8 *patch = mdev->pins_patch;
	size_t i = 0;

	while (patch && i + 4 <= mdev->pins_patch_len) {
		const u8 *e = patch + i;
		u16 entry = get_unaligned_le16(e);
		u8 flags = e[2], clksel = e[3];
		u32 data = 0, mask = ~0u, alt = 0;
		size_t n = 4;

		if ((!entry && !flags && !clksel) || (entry == 0xffff &&
						      flags == 0xff))
			return;
		if (flags & 1) { data = get_unaligned_le32(e + n); n += 4; }
		if (flags & 2) { mask = get_unaligned_le32(e + n); n += 4; }
		if (flags & 4) { alt = get_unaligned_le32(e + n); n += 4; }
		if (i + n > mdev->pins_patch_len)
			return;
		if (entry == reg) {
			u32 value = data;

			if ((flags & 4) &&
			    ((clksel == 1 && dot_khz > 100000) ||
			     (clksel == 2 && dot_khz > 200000)))
				value = alt;
			mask &= keep;
			m9x_write(mdev, reg,
				  (m9x_read(mdev, reg) & ~mask) |
				  (value & mask));
		}
		i += n;
	}
}

/* An image only describes this board when its PCI data structure names this
 * device: the shadow window holds whichever card the firmware booted. */
static bool m9_rom_matches(struct m9x_device *mdev, const u8 *rom, size_t size)
{
	size_t pcir;

	if (size < 0x20 || rom[0] != 0x55 || rom[1] != 0xaa)
		return false;
	pcir = get_unaligned_le16(rom + M9_ROM_PCIR_POINTER);
	if (pcir + 8 > size || memcmp(rom + pcir, "PCIR", 4))
		return false;
	return get_unaligned_le16(rom + pcir + 4) == mdev->pdev->vendor &&
	       get_unaligned_le16(rom + pcir + 6) == mdev->pdev->device;
}

static bool m9_pins_locate(const u8 *rom, size_t size, size_t *out_base,
			   size_t *out_len)
{
	size_t base, len;

	if (size < M9_PINS_POINTER + sizeof(u16))
		return false;
	base = get_unaligned_le16(rom + M9_PINS_POINTER);
	if (base + M9_PINS_DDC_MAP + M9_PINS_DDC_MAP_BYTES > size ||
	    get_unaligned_le16(rom + base) != M9_PINS_SIGNATURE ||
	    get_unaligned_le16(rom + base + M9_PINS_VERSION) <
	    M9_PINS_MIN_VERSION)
		return false;
	len = get_unaligned_le16(rom + base + M9_PINS_LENGTH);
	if (len < M9_PINS_DDC_MAP + M9_PINS_DDC_MAP_BYTES || base + len > size)
		return false;
	*out_base = base;
	*out_len = len;
	return true;
}

/* The board's own PInS carries a byte checksum over its whole length; a
 * mismatch is reported rather than fatal, since only one board is measured. */
static u8 m9_pins_checksum(const u8 *pins, size_t len)
{
	u8 sum = 0;
	size_t i;

	for (i = 0; i < len; i++)
		sum += pins[i];
	return sum;
}

static u8 *m9_rom_shadow(size_t *size)
{
	void *map;
	u8 *copy;

	map = memremap(M9_SHADOW_BIOS_BASE, M9_SHADOW_BIOS_SIZE, MEMREMAP_WB);
	if (!map)
		return NULL;
	copy = kvmalloc(M9_SHADOW_BIOS_SIZE, GFP_KERNEL);
	if (copy)
		memcpy(copy, map, M9_SHADOW_BIOS_SIZE);
	memunmap(map);
	*size = copy ? M9_SHADOW_BIOS_SIZE : 0;
	return copy;
}

static u8 *m9_rom_bar(struct m9x_device *mdev, size_t *size)
{
	struct pci_dev *pdev = mdev->pdev;
	void __iomem *rom;
	size_t len;
	u8 *copy;

	rom = pci_map_rom(pdev, &len);
	if (!rom)
		return NULL;
	copy = kvmalloc(len, GFP_KERNEL);
	if (copy)
		memcpy_fromio(copy, rom, len);
	pci_unmap_rom(pdev, rom);
	*size = copy ? len : 0;
	return copy;
}

/* Printable ASCII only: the PInS pads its strings with 0xff. */
static void m9_pins_string(char *dst, size_t dst_len, const u8 *src)
{
	size_t i;

	for (i = 0; i + 1 < dst_len; i++)
		dst[i] = src[i] >= 0x20 && src[i] < 0x7f ? src[i] : ' ';
	dst[i] = '\0';
	while (i-- && dst[i] == ' ')
		dst[i] = '\0';
}

static bool m9_pins_read(struct m9x_device *mdev, u8 *map)
{
	char serial[M9_PINS_SERIAL_BYTES + 1], name[M9_PINS_NAME_BYTES + 1];
	size_t base = 0, len = 0, size = 0;
	size_t patch, tail;
	unsigned int source;
	u8 *rom = NULL;

	for (source = 0; source < 2; source++) {
		rom = source ? m9_rom_bar(mdev, &size) : m9_rom_shadow(&size);
		if (!rom)
			continue;
		if (m9_rom_matches(mdev, rom, size) &&
		    m9_pins_locate(rom, size, &base, &len))
			break;
		kvfree(rom);
		rom = NULL;
	}
	if (!rom)
		return false;

	memcpy(map, rom + base + M9_PINS_DDC_MAP, M9_PINS_DDC_MAP_BYTES);
	m9_pins_string(serial, sizeof(serial), rom + base + M9_PINS_SERIAL);
	m9_pins_string(name, sizeof(name), rom + base + M9_PINS_NAME);
	/* Retain the register-patch table: the image is freed here but the link
	 * setup is needed at every modeset.  It runs to the end of the PInS. */
	patch = get_unaligned_le16(rom + base + M9_PINS_PATCH_START);
	if (patch && patch < len) {
		tail = len - patch;
		mdev->pins_patch = devm_kmemdup(&mdev->pdev->dev,
						rom + base + patch, tail,
						GFP_KERNEL);
		mdev->pins_patch_len = mdev->pins_patch ? tail : 0;
	}
	drm_info(&mdev->drm,
		 "PInS %#06x at %#zx from %s: board \"%s\" serial \"%s\", DDC map %8ph, %zu patch bytes%s\n",
		 get_unaligned_le16(rom + base + M9_PINS_VERSION), base,
		 source ? "the option ROM BAR" : "the shadow BIOS", name,
		 serial, map, mdev->pins_patch_len,
		 m9_pins_checksum(rom + base, len) ? " (bad checksum)" : "");
	kvfree(rom);
	return true;
}

static void m9_ddc_bus_release(struct drm_device *drm, void *res)
{
	i2c_del_adapter(&((struct m9_ddc_bus *)res)->adapter);
}

static int m9_ddc_bus_init(struct m9x_device *mdev, struct m9_ddc_bus *bus,
			   unsigned int index, u8 selector, u8 scl, u8 sda)
{
	int ret;

	bus->mdev = mdev;
	bus->selector = selector;
	bus->scl = scl;
	bus->sda = sda;
	bus->bit.data = bus;
	bus->bit.setsda = m9_ddc_setsda;
	bus->bit.setscl = m9_ddc_setscl;
	bus->bit.getsda = m9_ddc_getsda;
	bus->bit.getscl = m9_ddc_getscl;
	bus->bit.pre_xfer = m9_ddc_pre_xfer;
	bus->bit.post_xfer = m9_ddc_post_xfer;
	bus->bit.udelay = 10;
	bus->bit.timeout = usecs_to_jiffies(100000);

	bus->adapter.owner = THIS_MODULE;
	bus->adapter.algo_data = &bus->bit;
	bus->adapter.dev.parent = mdev->drm.dev;
	snprintf(bus->adapter.name, sizeof(bus->adapter.name),
		 "M9 DDC %u (selector %u, GPIO %u/%u)", index, selector, scl,
		 sda);
	i2c_set_adapdata(&bus->adapter, bus);

	ret = i2c_bit_add_bus(&bus->adapter);
	if (ret)
		return ret;
	return drmm_add_action_or_reset(&mdev->drm, m9_ddc_bus_release, bus);
}

/*
 * Each connector carries its own external DisplayPort transmitter on its DDC
 * pins.  The address is the board's, not a constant: the saved binaries record
 * 8-bit 0x72/0x76 while the M9148-E1024LAF answers at 0x10/0x12/0x14, so the
 * candidates are probed and whichever answers is kept.  MEASURED.
 */
static const u8 m9_tx_candidates[] = { 0x10 >> 1, 0x72 >> 1 };

static int m9_tx_xfer(struct m9_ddc_bus *bus, u8 addr, u8 reg, u8 *val,
		      bool write)
{
	u8 out[2] = { reg, write && val ? *val : 0 };
	struct i2c_msg msg[2] = {
		{ .addr = addr, .len = write ? 2 : 1, .buf = out },
		{ .addr = addr, .flags = I2C_M_RD, .len = 1, .buf = val },
	};
	int n = write ? 1 : 2;

	if (!addr)
		return -ENXIO;
	return i2c_transfer(&bus->adapter, msg, n) == n ? 0 : -ENXIO;
}

static int m9_tx_read(struct m9_ddc_bus *bus, u8 reg, u8 *val)
{
	return m9_tx_xfer(bus, bus->tx_addr, reg, val, false);
}

static int m9_tx_write(struct m9_ddc_bus *bus, u8 reg, u8 val)
{
	return m9_tx_xfer(bus, bus->tx_addr, reg, &val, true);
}

static void m9_tx_probe(struct m9_ddc_bus *bus)
{
	unsigned int i;
	u8 val;

	for (i = 0; i < ARRAY_SIZE(m9_tx_candidates); i++) {
		if (m9_tx_xfer(bus, m9_tx_candidates[i], M9_DP_TX_MAIN, &val,
			       false))
			continue;
		bus->tx_addr = m9_tx_candidates[i];
		if (m9_tx_read(bus, M9_DP_TX_HPD, &val))
			val = 0;
		drm_info(&bus->mdev->drm,
			 "DDC %u: transmitter at %#04x, hotplug %#04x\n",
			 bus->selector, bus->tx_addr << 1, val);
		return;
	}
	drm_info(&bus->mdev->drm, "DDC %u: no transmitter answered\n",
		 bus->selector);
}

int m9x_ddc_init(struct m9x_device *mdev)
{
	u8 map[M9_PINS_DDC_MAP_BYTES];
	unsigned int pair;
	int ret;

	mdev->ddc_count = 0;
	ret = drmm_mutex_init(&mdev->drm, &mdev->ddc_lock);
	if (ret)
		return ret;
	if (!m9_pins_read(mdev, map)) {
		drm_info(&mdev->drm, "no PInS DDC map; EDID unavailable\n");
		return 0;
	}
	/* Release every pin this map claims before the first transaction, in
	 * case a previous owner left one asserted. */
	if (!m9_hmc_acquire(mdev)) {
		m9x_write(mdev, M9_DDC, m9x_read(mdev, M9_DDC) &
			  ~GENMASK(M9_DDC_OE_SHIFT + M9_PINS_DDC_MAP_BYTES - 1,
				   M9_DDC_OE_SHIFT));
		m9_hmc_release(mdev);
	}
	/* Eight selectors share the eight physical bits, so a board populates
	 * at most four of them; the M9148 uses the 09..10 range, one bus per
	 * mini-DisplayPort connector, the even bit of each pair the clock. */
	for (pair = 0; pair < M9_PINS_DDC_MAP_BYTES; pair++) {
		int scl = m9_pins_ddc_bit(map, 2 * pair + 1);
		int sda = m9_pins_ddc_bit(map, 2 * pair + 2);

		if (scl < 0 || sda < 0 || mdev->ddc_count >= M9_DDC_MAX_BUSES)
			continue;
		ret = m9_ddc_bus_init(mdev, &mdev->ddc[mdev->ddc_count],
				      mdev->ddc_count, pair, scl, sda);
		if (ret)
			return ret;
		mdev->ddc_count++;
	}
	for (pair = 0; pair < mdev->ddc_count; pair++)
		m9_tx_probe(&mdev->ddc[pair]);
	drm_info(&mdev->drm, "%u DDC buses\n", mdev->ddc_count);
	return 0;
}

/* Connector n uses DDC bus n: the PInS map orders the selectors and the board
 * routes connector n to link n. */
struct i2c_adapter *m9x_ddc_adapter(struct m9x_device *mdev, unsigned int index)
{
	if (index >= mdev->ddc_count)
		return NULL;
	return &mdev->ddc[index].adapter;
}

/*
 * The connector's transmitter bridges these same pins onto the DisplayPort AUX
 * channel, and a sink only answers the host's own bit-banging while that bridge
 * is off.  Ownership is held across the whole sequence so the board controller
 * cannot take the bus between the bridge write and the EDID read.
 */
const struct drm_edid *m9x_ddc_read_edid(struct m9x_device *mdev,
					 unsigned int index,
					 struct drm_connector *connector)
{
	const struct drm_edid *edid;
	struct m9_ddc_bus *bus;
	bool bridged;
	u8 reg0;

	if (index >= mdev->ddc_count)
		return NULL;
	bus = &mdev->ddc[index];
	if (m9_hmc_acquire(mdev))
		return NULL;
	bridged = bus->tx_addr && !m9_tx_read(bus, M9_DP_TX_MAIN, &reg0) &&
		  (reg0 & M9_DP_TX_BRIDGE);
	if (bridged)
		m9_tx_write(bus, M9_DP_TX_MAIN, reg0 & ~M9_DP_TX_BRIDGE);
	edid = drm_edid_read_ddc(connector, &bus->adapter);
	if (bridged)
		m9_tx_write(bus, M9_DP_TX_MAIN, reg0);
	m9_hmc_release(mdev);
	return edid;
}
