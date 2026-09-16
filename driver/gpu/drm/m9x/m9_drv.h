/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2026 René Rebe <rene@exactco.de> */
#ifndef __M9X_DRM_DRV_H__
#define __M9X_DRM_DRV_H__

#include <linux/dma-fence.h>
#include <linux/list.h>
#include <linux/i2c-algo-bit.h>
#include <linux/io.h>
#include <linux/llist.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/sizes.h>

#include <drm/drm_device.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_vram_helper.h>
#include <drm/drm_mm.h>
#include <drm/drm_modes.h>
#include <drm/drm_simple_kms_helper.h>

#include "m9x_drm.h"

struct drm_file;
struct drm_mode_create_dumb;
struct iosys_map;

#define M9X_PCI_VENDOR			0x102b
#define M9X_PCI_DEVICE_PHOENIX		0x0540

#define PARHELIA_M9_COMMAND_BYTES	SZ_32K
#define PARHELIA_M9_COMMAND_SLOTS	2u
#define PARHELIA_M9_RESTORE_BYTES	0x00181000u
#define PARHELIA_M9_MAX_RELOCS		128u
#define PARHELIA_M9_MAX_BOS		128u
/* Local GPU address of VRAM offset 0; diagnostic, VRAM offsets by default. */
extern uint m9x_local_gpu_base;
#define M9_VRAM_GPU_BASE		((u64)m9x_local_gpu_base)
#define M9_VRAM_GPU_SIZE		0x10000000ull
/* BAR0 [0,224M) is the identity VRAM view, [224M,256M) the surface window. */
#define M9_ADDRESS_CPU_BYTES		0x0e000000u
#define M9_ADDRESS_SURFACE_BYTES	0x02000000u

struct m9x_bo {
	struct drm_gem_dma_object dma;
	/* Class-1 engine address of a non-command BO. */
	struct drm_mm_node system;
	/* Page-array backing when no contiguous DMA32 range was available. */
	struct page **pages;
	struct sg_table sgt;
	u32 flags;
};

struct m9x_vram_bo {
	struct drm_gem_vram_object vram;
	bool tiled;
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
	u32 restore_base;
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
	/*
	 * A posted job the CPU has not waited for.  The objects it reads stay
	 * referenced and pinned until it retires, which is what lets the next
	 * job be built while this one runs.
	 */
	bool async_pending;
	u32 async_target;
	struct drm_gem_object **async_bos;
	bool *async_pinned;
	u32 async_count;
	/* Releasing an object takes this same lock, so retired objects go to a
	 * worker rather than being freed under it. */
	struct llist_head async_retired;
	struct work_struct async_release;
	struct m9x_fence *quarantined_fence;
	struct drm_gem_vram_object *quarantined_color;
	struct m9x_address_space *address;
	struct m9x_triangle_diag *triangle;
};

/* PInS 8.0 pairs the eight DDC function IDs into four selectors. */
#define M9_DDC_MAX_BUSES	4u

struct m9_ddc_bus {
	struct m9x_device *mdev;
	struct i2c_adapter adapter;
	struct i2c_algo_bit_data bit;
	u8 scl, sda;
};

/* The M9 has four display controllers; the upper pair are secondary planes
 * composited into the lower pair, so only two are independent heads. */
#define M9_MAX_HEADS	2u

/* One independent display controller with its own register banks, connector
 * and DDC bus. */
struct m9_head {
	struct m9x_device *mdev;
	unsigned int index;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	struct drm_plane cursor_plane;
	struct i2c_adapter *ddc;
	bool ddc_probed;
	/* Register banks of this controller. */
	u32 dc, dct, clksel, fs, link, cursor_bank;
	u32 clk_select;
	u32 vblank_mask;
	/* The timing the controller was found running, and its opaque bits.
	 * warm_valid is false for a head the firmware left dark, whose
	 * registers decode to an empty raster rather than a mode. */
	struct drm_display_mode warm_mode;
	bool warm_valid;
	/* True only for the controller the firmware actually posted; any other
	 * head owns no link routing or clock until the driver programs it. */
	bool posted;
	u32 warm_dc_ctrl;
	u32 warm_dc_pitch;
	u32 warm_dc_start;
	u32 warm_dvo;
	/* Geometry the controller is currently programmed for. */
	u32 scanout_width, scanout_height, scanout_pitch;
	size_t scanout_bytes;
	struct drm_gem_vram_object *scanout;
	u8 *scanout_bounce;
	struct drm_gem_vram_object *cursor;
	struct iosys_map cursor_map;
	bool cursor_visible;
	bool cursor_armed;   /* address and config survive in the controller */
};

static inline struct m9_head *m9_head_of_pipe(struct drm_simple_display_pipe *p)
{
	return container_of(p, struct m9_head, pipe);
}

struct m9x_device {
	struct drm_device drm;
	struct pci_dev *pdev;
	void __iomem *mmio;
	resource_size_t vram_size;
	u64 vram_total;
	struct m9_head head[M9_MAX_HEADS];
	unsigned int heads;
	bool kms_initialized;
	bool irq_installed;
	/* BAR0 [M9_ADDRESS_CPU_BYTES, +M9_ADDRESS_SURFACE_BYTES), slot 1 view. */
	/* Native DDC, shared with the board controller. */
	struct mutex ddc_lock;
	unsigned int ddc_owned;
	struct m9_ddc_bus ddc[M9_DDC_MAX_BUSES];
	unsigned int ddc_count;
	/* Board register patches from PInS, keyed to the pixel clock. */
	const u8 *pins_patch;
	size_t pins_patch_len;
	void __iomem *bar0_window;
	struct m9x_transport transport;
};

static inline struct m9x_device *to_m9x(struct drm_device *drm)
{
	return container_of(drm, struct m9x_device, drm);
}

void m9x_mode_read_current(struct m9_head *head,
			   struct drm_display_mode *mode);
int m9x_mode_program(struct m9_head *head,
		     const struct drm_display_mode *mode, u32 pitch_pixels);
bool m9x_surface_layout_bytes(u32 width, u32 height, u32 cpp, u64 *size);
int m9x_ddc_init(struct m9x_device *mdev);
void m9x_pins_patch(struct m9x_device *mdev, u16 reg, u32 dot_khz);
struct i2c_adapter *m9x_ddc_adapter(struct m9x_device *mdev,
				    unsigned int index);
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
/* Objects a retired job was reading, released off the transport lock. */
struct m9_retired {
	struct llist_node node;
	struct drm_gem_object **bos;
	bool *pinned;
	u32 count;
};

void m9x_transport_release_retired(struct work_struct *work);
int m9x_transport_drain_locked(struct m9x_device *mdev);
int m9x_submit_kernel_stream_locked(struct m9x_device *mdev,
				    const u32 *commands, u32 dwords,
				    u32 target, u64 *point, bool *doorbell_posted,
				    bool async);
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
int m9x_address_map_system(struct m9x_device *mdev, struct drm_mm_node *node,
			   dma_addr_t dma, u32 bytes);
int m9x_address_map_system_sgt(struct m9x_device *mdev,
			       struct drm_mm_node *node, struct sg_table *sgt,
			       u32 bytes);
void m9x_address_unmap_system(struct m9x_device *mdev, struct drm_mm_node *node);
int m9x_address_restore_region(struct m9x_device *mdev,
			       struct iosys_map *map, u32 *base);
int m9x_address_publish_restore(struct m9x_device *mdev);
int m9x_address_read_pte(struct m9x_device *mdev, u32 gpuva, u32 *value);
int m9x_address_read_root_pte(struct m9x_device *mdev, u32 gpuva, u32 *value);
u32 m9x_address_pte_word(dma_addr_t dma);
void m9x_address_surface_window(struct m9x_device *mdev, u32 surface,
				u32 format, u32 extent);
void m9x_address_surface_release(struct m9x_device *mdev);
int m9x_submit_triangle_diag(struct m9x_device *mdev, u32 color,
			     struct drm_gem_vram_object *color_bo, u64 *point,
			     bool *color_active);
int m9x_triangle_prepare_diag(struct m9x_device *mdev);
int m9x_engine_ready_locked(struct m9x_device *mdev);
int m9x_submit_user_stream_locked(struct m9x_device *mdev, const u32 *records,
				  u32 dwords, const u32 *range_ends, u32 ranges,
				  u32 target, u64 *point, bool async);
int m9x_triangle_selftest(void);
void m9x_triangle_fini(struct m9x_device *mdev);
int m9x_submit_selftest(void);
struct m9x_bo *m9x_bo_lookup(struct m9x_device *mdev,
			     struct drm_file *file, u32 handle);
bool m9x_gem_is_dma(struct drm_gem_object *gem);
bool m9x_bo_is_paged(const struct m9x_bo *bo);
void m9x_bo_sync_for_device(struct m9x_bo *bo);
bool m9x_gem_is_vram(struct drm_gem_object *gem);
bool m9x_gem_vram_tiled(struct drm_gem_object *gem);
int m9x_surface_access_ioctl(struct drm_device *drm, void *data,
			     struct drm_file *file);
void m9x_surface_read_locked(struct m9x_device *mdev, u32 offset, u32 d0,
			     u32 d1, u8 *dst, size_t bytes);
int m9x_surface_selftest(void);
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
