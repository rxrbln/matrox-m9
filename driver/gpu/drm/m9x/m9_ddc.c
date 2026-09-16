// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
/*
 * M9 native DDC: bit-banged I2C on MMIO 0x1c48, with the pin pairs taken from
 * the PInS 8.0 function map and the register owned through the board
 * controller's request/grant handshake
 * (xf86-video-parhelia docs/parhelia-m9148-kms-pins-dp-re.md).
 */
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

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
#define M9_PINS_DDC_MAP		0x0050	/* 8 function IDs, physical bit order */
#define M9_PINS_DDC_MAP_BYTES	8

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

static bool m9_pins_ddc_map(struct m9x_device *mdev, u8 *map)
{
	struct pci_dev *pdev = mdev->pdev;
	void __iomem *rom;
	size_t size, base;
	bool ok = false;
	u8 *copy;

	rom = pci_map_rom(pdev, &size);
	if (!rom)
		return false;
	copy = kmalloc(size, GFP_KERNEL);
	if (copy)
		memcpy_fromio(copy, rom, size);
	pci_unmap_rom(pdev, rom);
	if (!copy)
		return false;
	if (size < M9_PINS_POINTER + sizeof(u16) || copy[0] != 0x55 ||
	    copy[1] != 0xaa)
		goto out;
	base = get_unaligned_le16(copy + M9_PINS_POINTER);
	if (base + M9_PINS_DDC_MAP + M9_PINS_DDC_MAP_BYTES > size ||
	    get_unaligned_le16(copy + base) != M9_PINS_SIGNATURE ||
	    get_unaligned_le16(copy + base + M9_PINS_VERSION) <
	    M9_PINS_MIN_VERSION)
		goto out;
	memcpy(map, copy + base + M9_PINS_DDC_MAP, M9_PINS_DDC_MAP_BYTES);
	drm_info(&mdev->drm, "PInS %#06x at %#zx: DDC map %8ph\n",
		 get_unaligned_le16(copy + base + M9_PINS_VERSION), base, map);
	ok = true;
out:
	kfree(copy);
	return ok;
}

static void m9_ddc_bus_release(struct drm_device *drm, void *res)
{
	i2c_del_adapter(&((struct m9_ddc_bus *)res)->adapter);
}

static int m9_ddc_bus_init(struct m9x_device *mdev, struct m9_ddc_bus *bus,
			   unsigned int index, u8 scl, u8 sda)
{
	int ret;

	bus->mdev = mdev;
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
		 "M9 DDC %u (GPIO %u/%u)", index, scl, sda);
	i2c_set_adapdata(&bus->adapter, bus);

	ret = i2c_bit_add_bus(&bus->adapter);
	if (ret)
		return ret;
	return drmm_add_action_or_reset(&mdev->drm, m9_ddc_bus_release, bus);
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
	if (!m9_pins_ddc_map(mdev, map)) {
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
	 * at most four of them; this one uses the 09..10 range. */
	for (pair = 0; pair < M9_PINS_DDC_MAP_BYTES; pair++) {
		int scl = m9_pins_ddc_bit(map, 2 * pair + 1);
		int sda = m9_pins_ddc_bit(map, 2 * pair + 2);

		if (scl < 0 || sda < 0 || mdev->ddc_count >= M9_DDC_MAX_BUSES)
			continue;
		ret = m9_ddc_bus_init(mdev, &mdev->ddc[mdev->ddc_count],
				      mdev->ddc_count, scl, sda);
		if (ret)
			return ret;
		mdev->ddc_count++;
	}
	drm_info(&mdev->drm, "%u DDC buses\n", mdev->ddc_count);
	return 0;
}

/* Connector n uses DDC bus n: the PInS map orders the pairs, and with two
 * displays attached the first two buses are the two lit connectors. */
struct i2c_adapter *m9x_ddc_adapter(struct m9x_device *mdev, unsigned int index)
{
	if (index >= mdev->ddc_count)
		return NULL;
	return &mdev->ddc[index].adapter;
}
