// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/pci.h>

#include <drm/drm_drv.h>
#include <drm/drm_ioctl.h>

#include "m9_drv.h"

#define M9X_MODULE_NAME "m9x_drm"

DEFINE_DRM_GEM_FOPS(m9x_fops);

static void m9x_drm_postclose(struct drm_device *drm, struct drm_file *file)
{
	m9x_postclose(to_m9x(drm), file);
}

static const struct drm_ioctl_desc m9x_ioctls[] = {
	DRM_IOCTL_DEF_DRV(PARHELIA_M9_CREATE_BO, m9x_create_bo_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PARHELIA_M9_SUBMIT, m9x_submit_ioctl,
			  DRM_RENDER_ALLOW),
};

/* Mesa selects the independent M9X Gallium driver from this DRM name. */
static const struct drm_driver m9x_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER,
	.fops = &m9x_fops,
	.postclose = m9x_drm_postclose,
	.ioctls = m9x_ioctls,
	.num_ioctls = ARRAY_SIZE(m9x_ioctls),
	.name = "m9x",
	.desc = "Matrox M9/Phoenix clean-room render driver",
	.major = 1,
	.minor = 0,
};

static bool m9x_firmware_initialized(struct pci_dev *pci)
{
	u16 command = 0;

	pci_read_config_word(pci, PCI_COMMAND, &command);
	return command & PCI_COMMAND_MEMORY;
}

static int m9x_pci_probe(struct pci_dev *pci,
			 const struct pci_device_id *ent)
{
	struct m9x_device *mdev;
	struct drm_device *drm;
	unsigned int mmio_bar;
	int ret;

	if (!m9x_enabled())
		return dev_err_probe(&pci->dev, -ENODEV,
			"clean-room backend disabled; use m9_enable=1 for bring-up\n");
	if (!m9x_firmware_initialized(pci))
		return dev_err_probe(&pci->dev, -ENODEV,
			"device was not POSTed; cold memory/clock bring-up is unavailable\n");

	ret = pcim_enable_device(pci);
	if (ret)
		return ret;
	pci_clear_master(pci);
	ret = dma_set_mask_and_coherent(&pci->dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(&pci->dev, ret,
				     "32-bit DMA is unavailable\n");

	mdev = devm_drm_dev_alloc(&pci->dev, &m9x_drm_driver,
				  struct m9x_device, drm);
	if (IS_ERR(mdev))
		return PTR_ERR(mdev);
	drm = &mdev->drm;
	mdev->pdev = pci;
	mdev->vram_size = pci_resource_len(pci, 0);
	mmio_bar = pci_resource_len(pci, 1) ? 1 : 2;
	if (!(pci_resource_flags(pci, 0) & IORESOURCE_MEM) ||
	    !(pci_resource_flags(pci, mmio_bar) & IORESOURCE_MEM) ||
	    !mdev->vram_size || pci_resource_len(pci, mmio_bar) < 0x2000)
		return dev_err_probe(&pci->dev, -ENODEV,
				     "invalid framebuffer/MMIO BAR layout\n");
	mdev->mmio = pcim_iomap(pci, mmio_bar, 0);
	if (!mdev->mmio)
		return dev_err_probe(&pci->dev, -ENOMEM, "cannot map MMIO BAR\n");

	dev_info(&pci->dev,
		 "M9/Phoenix: %pa-byte framebuffer BAR0, MMIO BAR%u\n",
		 &mdev->vram_size, mmio_bar);
	ret = m9x_init(mdev);
	if (ret)
		return ret;
	ret = drm_dev_register(drm, 0);
	if (ret) {
		m9x_fini(mdev);
		return ret;
	}
	pci_set_drvdata(pci, drm);
	return 0;
}

static void m9x_pci_remove(struct pci_dev *pci)
{
	struct drm_device *drm = pci_get_drvdata(pci);

	drm_dev_unregister(drm);
	m9x_fini(to_m9x(drm));
}

static const struct pci_device_id m9x_pci_table[] = {
	{ PCI_DEVICE(M9X_PCI_VENDOR, M9X_PCI_DEVICE_PHOENIX) },
	{ }
};
MODULE_DEVICE_TABLE(pci, m9x_pci_table);

static struct pci_driver m9x_pci_driver = {
	.name = M9X_MODULE_NAME,
	.id_table = m9x_pci_table,
	.probe = m9x_pci_probe,
	.remove = m9x_pci_remove,
};
module_pci_driver(m9x_pci_driver);

MODULE_AUTHOR("René Rebe <rene@exactco.de>");
MODULE_DESCRIPTION("Matrox M9/Phoenix clean-room DRM driver");
MODULE_LICENSE("GPL");
