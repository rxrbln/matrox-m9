/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef __M9X_DRM_DRV_H__
#define __M9X_DRM_DRV_H__

#include <linux/dma-fence.h>
#include <linux/list.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/sizes.h>

#include <drm/drm_device.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_vram_helper.h>
#include <drm/drm_modes.h>
#include <drm/drm_simple_kms_helper.h>

#include "m9x_drm.h"

struct drm_file;
struct drm_mode_create_dumb;

#define M9X_PCI_VENDOR			0x102b
#define M9X_PCI_DEVICE_PHOENIX		0x0540

#define PARHELIA_M9_COMMAND_BYTES	SZ_32K
#define PARHELIA_M9_COMMAND_SLOTS	2u
#define PARHELIA_M9_RESTORE_BYTES	0x00181000u
#define PARHELIA_M9_RESTORE_GPUVA	0x41000000u
#define PARHELIA_M9_MAX_RELOCS		128u
#define PARHELIA_M9_MAX_BOS		128u
#define M9_VRAM_GPU_BASE		0x10000000ull
#define M9_VRAM_GPU_SIZE		0x10000000ull

struct m9x_bo {
	struct drm_gem_dma_object dma;
	u32 flags;
};

struct m9x_vram_bo {
	struct drm_gem_vram_object vram;
};

struct m9x_fence {
	struct dma_fence base;
	spinlock_t lock;
};

struct m9x_triangle_diag;
struct m9x_address_space;

struct m9x_transport {
	struct mutex lock;
	void *requester_cpu;
	dma_addr_t requester_bus;
	void *command_cpu;
	dma_addr_t command_bus;
	void *fence_cpu;
	dma_addr_t fence_bus;
	void *restore_cpu;
	dma_addr_t restore_bus;
	u64 fence_context;
	u32 next_fence;
	u32 last_fence;
	u32 fence_epoch;
	u32 generation;
	u32 command_slot;
	u32 command_offset;
	bool transport_armed;
	bool bus_master_enabled;
	bool engine_transitioned;
	bool restore_ready;
	bool wedged;
	struct m9x_fence *quarantined_fence;
	struct drm_gem_vram_object *quarantined_color;
	struct m9x_address_space *address;
	struct m9x_triangle_diag *triangle;
};

struct m9x_device {
	struct drm_device drm;
	struct pci_dev *pdev;
	void __iomem *mmio;
	resource_size_t vram_size;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	struct drm_display_mode warm_mode;
	u32 warm_dc_ctrl;
	u32 warm_dc_pitch;
	u32 warm_dc_start;
	u32 warm_dvo;
	bool kms_initialized;
	bool irq_installed;
	struct m9x_transport transport;
};

static inline struct m9x_device *to_m9x(struct drm_device *drm)
{
	return container_of(drm, struct m9x_device, drm);
}

bool m9x_enabled(void);
bool m9x_kms_enabled(void);
bool m9x_kms_scanout_enabled(void);
int m9x_init(struct m9x_device *mdev);
void m9x_fini(struct m9x_device *mdev);
int m9x_create_bo_ioctl(struct drm_device *drm, void *data,
			struct drm_file *file);
int m9x_get_bo_info_ioctl(struct drm_device *drm, void *data,
			  struct drm_file *file);
int m9x_submit_ioctl(struct drm_device *drm, void *data,
		     struct drm_file *file);
int m9x_submit_noop(struct m9x_device *mdev, u64 *point);
int m9x_transport_arm_locked(struct m9x_device *mdev);
int m9x_transport_enable_bus_master_locked(struct m9x_device *mdev);
int m9x_transport_rearm_locked(struct m9x_device *mdev);
int m9x_restore_bind(struct m9x_device *mdev);
int m9x_submit_kernel_stream_locked(struct m9x_device *mdev,
				    const u32 *commands, u32 dwords,
				    u32 target, u64 *point, bool *doorbell_posted);
struct m9x_fence *m9x_fence_create_target_locked(struct m9x_device *mdev,
						 u32 target);
int m9x_address_init(struct m9x_device *mdev);
void m9x_address_fini(struct m9x_device *mdev);
int m9x_address_bind_dma(struct m9x_device *mdev, u32 gpuva,
			 dma_addr_t dma, u32 bytes);
int m9x_address_prepare(struct m9x_device *mdev);
int m9x_address_publish(struct m9x_device *mdev);
int m9x_address_construct_manager(struct m9x_device *mdev);
int m9x_address_commit_bindings(struct m9x_device *mdev);
int m9x_address_commit_gpuva(struct m9x_device *mdev, u32 gpuva);
int m9x_address_finalize_service(struct m9x_device *mdev);
int m9x_address_read_pte(struct m9x_device *mdev, u32 gpuva, u32 *value);
int m9x_address_read_root_pte(struct m9x_device *mdev, u32 gpuva, u32 *value);
u32 m9x_address_pte_word(dma_addr_t dma);
int m9x_submit_triangle_diag(struct m9x_device *mdev, u32 color,
			     struct drm_gem_vram_object *color_bo, u64 *point,
			     bool *color_active);
int m9x_triangle_prepare_diag(struct m9x_device *mdev);
int m9x_triangle_selftest(void);
void m9x_triangle_fini(struct m9x_device *mdev);
int m9x_submit_selftest(void);
struct m9x_bo *m9x_bo_lookup(struct m9x_device *mdev,
			     struct drm_file *file, u32 handle);
bool m9x_gem_is_dma(struct drm_gem_object *gem);
bool m9x_gem_is_vram(struct drm_gem_object *gem);
struct drm_gem_object *m9x_gem_create_object(struct drm_device *drm,
					      size_t size);
int m9x_vram_pin(struct drm_gem_vram_object *bo);
void m9x_vram_unpin(struct drm_gem_vram_object *bo);
int m9x_modeset_init(struct m9x_device *mdev);
void m9x_modeset_fini(struct m9x_device *mdev);
irqreturn_t m9x_irq_handler(int irq, void *arg);
int m9x_dumb_create(struct drm_file *file, struct drm_device *drm,
		    struct drm_mode_create_dumb *args);

static inline u32 m9x_read(struct m9x_device *mdev, u32 reg)
{
	return readl((u8 __iomem *)mdev->mmio + reg);
}

static inline void m9x_write(struct m9x_device *mdev, u32 reg, u32 value)
{
	writel(value, (u8 __iomem *)mdev->mmio + reg);
}

#endif
