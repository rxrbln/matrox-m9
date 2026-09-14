/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef __M9X_DRM_DRV_H__
#define __M9X_DRM_DRV_H__

#include <linux/dma-fence.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/sizes.h>

#include <drm/drm_device.h>
#include <drm/drm_gem_dma_helper.h>

#include "m9x_drm.h"

struct drm_file;

#define M9X_PCI_VENDOR			0x102b
#define M9X_PCI_DEVICE_PHOENIX		0x0540

#define PARHELIA_M9_COMMAND_BYTES	SZ_32K
#define PARHELIA_M9_MAX_RELOCS		128u
#define PARHELIA_M9_MAX_BOS		128u

struct m9x_bo {
	struct drm_gem_dma_object dma;
	struct list_head link;
	struct drm_file *file;
	u32 flags;
};

struct m9x_fence {
	struct dma_fence base;
	spinlock_t lock;
};

struct m9x_transport {
	struct mutex lock;
	struct list_head bos;
	void *requester_cpu;
	dma_addr_t requester_bus;
	void *command_cpu;
	dma_addr_t command_bus;
	void *fence_cpu;
	dma_addr_t fence_bus;
	u64 fence_context;
	u32 next_fence;
	u32 last_fence;
	u32 fence_epoch;
	u32 generation;
	u32 command_offset;
	bool transport_armed;
	bool wedged;
};

struct m9x_device {
	struct drm_device drm;
	struct pci_dev *pdev;
	void __iomem *mmio;
	resource_size_t vram_size;
	struct m9x_transport transport;
};

static inline struct m9x_device *to_m9x(struct drm_device *drm)
{
	return container_of(drm, struct m9x_device, drm);
}

bool m9x_enabled(void);
int m9x_init(struct m9x_device *mdev);
void m9x_fini(struct m9x_device *mdev);
void m9x_postclose(struct m9x_device *mdev, struct drm_file *file);
int m9x_create_bo_ioctl(struct drm_device *drm, void *data,
			struct drm_file *file);
int m9x_submit_ioctl(struct drm_device *drm, void *data,
		     struct drm_file *file);
int m9x_submit_noop(struct m9x_device *mdev, u64 *point);
int m9x_submit_selftest(void);
struct m9x_bo *m9x_bo_lookup(struct m9x_device *mdev,
			     struct drm_file *file, u32 handle);

#endif
