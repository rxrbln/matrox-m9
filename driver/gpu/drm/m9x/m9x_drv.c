// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/dma-mapping.h>
#include <linux/capability.h>
#include <linux/aperture.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_fbdev_ttm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_ttm_helper.h>
#include <drm/drm_gem_vram_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_vblank.h>

#include "m9_drv.h"
#include "m9_reg.h"

#define M9X_MODULE_NAME "m9x_drm"

DEFINE_DRM_GEM_FOPS(m9x_fops);

uint m9x_local_gpu_base;
module_param_named(m9_local_gpu_base, m9x_local_gpu_base, uint, 0400);
MODULE_PARM_DESC(m9_local_gpu_base,
	"Diagnostic GPU address of VRAM offset 0 for local render resources");

static const struct drm_ioctl_desc m9x_ioctls[] = {
	DRM_IOCTL_DEF_DRV(PARHELIA_M9_WAIT_POINT, m9x_wait_point_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PARHELIA_M9_CREATE_BO, m9x_create_bo_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PARHELIA_M9_SUBMIT, m9x_submit_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PARHELIA_M9_GET_BO_INFO, m9x_get_bo_info_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PARHELIA_M9_SURFACE_ACCESS, m9x_surface_access_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PARHELIA_M9_FAA_GRAPH_PREPARE,
			  m9x_faa_graph_prepare_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PARHELIA_M9_FAA_GRAPH_RELEASE,
			  m9x_faa_graph_release_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PARHELIA_M9_FAA_GRAPH_INIT,
			  m9x_faa_graph_init_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PARHELIA_M9_FAA_GRAPH_NEXT_DRAW,
			  m9x_faa_graph_next_draw_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PARHELIA_M9_FAA_GRAPH_NEXT_FINISH,
			  m9x_faa_graph_next_finish_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PARHELIA_M9_FAA_GRAPH_NEXT_LOCK,
			  m9x_faa_graph_next_lock_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PARHELIA_M9_FAA_GRAPH_UNLOCK,
			  m9x_faa_graph_unlock_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_NEXT,
			  m9x_faa_graph_public_vs_next_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_LOCK,
			  m9x_faa_graph_public_vs_lock_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_DRAW,
			  m9x_faa_graph_public_vs_draw_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_HEAD,
			  m9x_faa_graph_public_vs_head_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PARHELIA_M9_FAA_GRAPH_PUBLIC_VS_FOLLOW,
			  m9x_faa_graph_public_vs_follow_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PARHELIA_M9_FAA_GRAPH_PUBLIC_VS,
			  m9x_faa_graph_public_vs_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PARHELIA_M9_FAA_GRAPH_DRAW_RED,
			  m9x_faa_graph_draw_red_ioctl, DRM_RENDER_ALLOW),
};

struct m9_transport_snapshot {
	u64 sampled_ns, submitted, completed, async_point;
	u32 raw[4], generation, next, last, epoch;
	u32 queue_head, queue_count, queue_peak, async_target;
	u32 command_slot, command_offset;
	bool armed, wedged, async_pending, bus_master;
};

/* Sample owned CPU memory even after the watchdog quarantines the engine.
 * Do not observe/extend completion or release a job here. Successful probe
 * owns fence_cpu as a PAGE_SIZE dmam allocation before debugfs registration;
 * unplug waits for this device critical section before fini/devres release.
 * trylock refuses a busy transport instead of adding another wait. */
static int m9_transport_snapshot_read(struct m9x_device *mdev,
				     struct m9_transport_snapshot *out)
{
	struct m9x_transport *t = &mdev->transport;
	int idx, ret = 0;

	if (!drm_dev_enter(&mdev->drm, &idx))
		return -ENODEV;
	if (!mutex_trylock(&t->lock)) {
		ret = -EBUSY;
		goto exit;
	}
	/* Fini sets async_stop under this lock before releasing any owner.
	 * No pointer is exported or dereferenced after that boundary. */
	if (t->async_stop || !t->fence_cpu) {
		ret = -ENODEV;
		goto unlock;
	}
	BUILD_BUG_ON(sizeof(out->raw) > PAGE_SIZE);
	for (unsigned int i = 0; i < ARRAY_SIZE(out->raw); i++)
		out->raw[i] = READ_ONCE(((u32 *)t->fence_cpu)[i]);
	out->sampled_ns = ktime_get_ns();
	out->submitted = t->submitted_point;
	out->completed = t->completed_point;
	out->async_point = t->async_point;
	out->generation = t->generation;
	out->next = t->next_fence;
	out->last = t->last_fence;
	out->epoch = t->fence_epoch;
	out->queue_head = t->queue_head;
	out->queue_count = t->queue_count;
	out->queue_peak = t->queue_peak;
	out->async_target = t->async_target;
	out->command_slot = t->command_slot;
	out->command_offset = t->command_offset;
	out->armed = t->transport_armed;
	out->wedged = t->wedged;
	out->async_pending = t->async_pending;
	out->bus_master = t->bus_master_enabled;
unlock:
	mutex_unlock(&t->lock);
exit:
	drm_dev_exit(idx);
	return ret;
}

static int m9_transport_snapshot_show(struct seq_file *m, void *unused)
{
	struct drm_debugfs_entry *entry = m->private;
	struct m9_transport_snapshot s;
	int ret = m9_transport_snapshot_read(to_m9x(entry->dev), &s);

	if (ret)
		return ret;
	seq_printf(m, "sampled_ns=%llu raw00=%08x raw04=%08x raw08=%08x raw0c=%08x\n",
		   s.sampled_ns, s.raw[0], s.raw[1], s.raw[2], s.raw[3]);
	seq_printf(m, "submitted=%#llx completed=%#llx next=%#x last=%#x epoch=%#x generation=%u\n",
		   s.submitted, s.completed, s.next, s.last, s.epoch, s.generation);
	seq_printf(m, "queue_head=%u queue_count=%u queue_peak=%u async_pending=%u async_target=%#x async_point=%#llx\n",
		   s.queue_head, s.queue_count, s.queue_peak, s.async_pending,
		   s.async_target, s.async_point);
	seq_printf(m, "armed=%u wedged=%u bus_master=%u command_slot=%u command_offset=%u\n",
		   s.armed, s.wedged, s.bus_master, s.command_slot, s.command_offset);
	return 0;
}

static int m9_faa_dma_snapshot_show(struct seq_file *m, void *unused)
{
	struct drm_debugfs_entry *entry = m->private;
	void *data;
	size_t bytes;
	int ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	ret = m9x_faa_dma_snapshot(to_m9x(entry->dev), &data, &bytes);
	if (ret)
		return ret;
	/* seq_file grows its buffer and retries on overflow. The one-shot
	 * private owners remain immutable after the successful final wait. */
	seq_write(m, data, bytes);
	kvfree(data);
	return 0;
}

static void m9x_debugfs_init(struct drm_minor *minor)
{
	drm_vram_mm_debugfs_init(minor);
	drm_debugfs_add_file(minor->dev, "m9_transport_snapshot",
			     m9_transport_snapshot_show, NULL);
	drm_debugfs_add_file(minor->dev, "m9_faa_dma_snapshot",
			     m9_faa_dma_snapshot_show, NULL);
}

/* Mesa selects the independent M9X Gallium driver from this DRM name. */
#define M9X_DRM_DRIVER_COMMON						\
	.fops = &m9x_fops,						\
	.open = m9x_faa_graph_file_open,				\
	.postclose = m9x_faa_graph_file_close,			\
	.ioctls = m9x_ioctls,						\
	.num_ioctls = ARRAY_SIZE(m9x_ioctls),				\
	.gem_create_object = m9x_gem_create_object,			\
	.debugfs_init = m9x_debugfs_init,				\
	.name = "m9x",							\
	.desc = "Matrox M9/Phoenix clean-room driver",			\
	.major = 1,							\
	.minor = 0

static const struct drm_driver m9x_render_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER,
	M9X_DRM_DRIVER_COMMON,
};

static const struct drm_driver m9x_kms_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER | DRIVER_MODESET |
			   DRIVER_ATOMIC,
	.dumb_create = m9x_dumb_create,
	.dumb_map_offset = drm_gem_ttm_dumb_map_offset,
	DRM_FBDEV_TTM_DRIVER_OPS,
	M9X_DRM_DRIVER_COMMON,
};

static bool m9x_firmware_initialized(struct pci_dev *pci)
{
	u16 command = 0;

	pci_read_config_word(pci, PCI_COMMAND, &command);
	return command & PCI_COMMAND_MEMORY;
}

/*
 * m9x320 -> m9x798: the u16 at ROM 0x7ffc locates PInS (".A").  Each of the
 * four low nibbles of PInS+0x44 in 1..6 adds 32 MiB << (n - 1).  The sum only
 * replaces the default when it is not smaller.
 */
static u64 m9x_pins_memory_size(struct pci_dev *pci, u64 fallback)
{
	void __iomem *rom;
	size_t size;
	u64 total = 0;
	unsigned int i;
	u32 word;
	u16 pins;

	rom = pci_map_rom(pci, &size);
	if (!rom)
		return fallback;
	if (size > 0x7ffe) {
		pins = readw(rom + 0x7ffc);
		if (size >= 0x48 && pins <= size - 0x48 &&
		    readw(rom + pins) == 0x412e) {
			word = readl(rom + pins + 0x44);
			for (i = 0; i < 4; i++) {
				u32 n = (word >> (i * 4)) & 0xf;

				if (n >= 1 && n <= 6)
					total += (u64)SZ_32M << (n - 1);
			}
		}
	}
	pci_unmap_rom(pci, rom);
	return total >= fallback ? total : fallback;
}

static int m9x_pci_probe(struct pci_dev *pci,
			 const struct pci_device_id *ent)
{
	struct m9x_device *mdev;
	struct drm_device *drm;
	const struct drm_driver *driver;
	unsigned int mmio_bar;
	bool kms = m9x_kms_enabled();
	int ret;

	if (!m9x_enabled())
		return dev_err_probe(&pci->dev, -ENODEV,
			"clean-room backend disabled; use m9_enable=1 for bring-up\n");
	if (!m9x_firmware_initialized(pci))
		return dev_err_probe(&pci->dev, -ENODEV,
			"device was not POSTed; cold memory/clock bring-up is unavailable\n");

	/* The render address manager owns and clears low VRAM even without KMS. */
	ret = aperture_remove_conflicting_pci_devices(pci, M9X_MODULE_NAME);
	if (ret)
		return ret;
	ret = pcim_enable_device(pci);
	if (ret)
		return ret;
	pci_clear_master(pci);
	ret = dma_set_mask_and_coherent(&pci->dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(&pci->dev, ret,
				     "32-bit DMA is unavailable\n");

	driver = kms ? &m9x_kms_driver : &m9x_render_driver;
	mdev = devm_drm_dev_alloc(&pci->dev, driver,
				  struct m9x_device, drm);
	if (IS_ERR(mdev))
		return PTR_ERR(mdev);
	drm = &mdev->drm;
	mdev->pdev = pci;
	mdev->bar0_bytes = pci_resource_len(pci, 0);
	mmio_bar = pci_resource_len(pci, 1) ? 1 : 2;
	if (!(pci_resource_flags(pci, 0) & IORESOURCE_MEM) ||
	    !(pci_resource_flags(pci, mmio_bar) & IORESOURCE_MEM) ||
	    !mdev->bar0_bytes || pci_resource_len(pci, mmio_bar) < 0x2000)
		return dev_err_probe(&pci->dev, -ENODEV,
				     "invalid framebuffer/MMIO BAR layout\n");
	mdev->mmio = pcim_iomap(pci, mmio_bar, 0);
	if (!mdev->mmio)
		return dev_err_probe(&pci->dev, -ENOMEM, "cannot map MMIO BAR\n");

	mdev->vram_total = m9x_pins_memory_size(pci, mdev->bar0_bytes);
	/* The CPU sees whichever is smallest: the aperture's identity window,
	 * the aperture, or the memory that is actually there. */
	mdev->vram_visible = min3((u64)mdev->bar0_bytes,
				  (u64)M9_ADDRESS_CPU_BYTES, mdev->vram_total);
	dev_info(&pci->dev,
		 "M9/Phoenix: %pa-byte framebuffer BAR0, %#llx-byte VRAM (%#llx CPU-visible), MMIO BAR%u\n",
		 &mdev->bar0_bytes, mdev->vram_total, mdev->vram_visible,
		 mmio_bar);
	/*
	 * The render requester also needs the pinned low-VRAM translation root,
	 * including when KMS exposure is disabled.
	 */
	/* TTM only sees the slot 0 identity range; the BAR top is the window. */
	ret = drmm_vram_helper_init(drm, pci_resource_start(pci, 0),
				    mdev->vram_visible);
	if (ret)
		return ret;
	ret = m9x_dma_reserve_init(mdev);
	if (ret)
		return ret;
	ret = m9x_init(mdev);
	if (ret)
		return ret;
	ret = m9x_address_init(mdev);
	if (ret)
		goto err_fini;
	/* The slot 1 window the engine-memory probe needs exists only once the
	 * address space owns BAR0. */
	ret = m9x_engine_vram_init(mdev);
	if (ret)
		goto err_fini;
	ret = m9x_restore_bind(mdev);
	if (ret)
		goto err_fini;
	if (kms) {
		ret = m9x_modeset_init(mdev);
		if (ret)
			goto err_fini;
		ret = drm_vblank_init(drm, mdev->heads);
		if (ret)
			goto err_modeset;
		/* Mask every source without consuming the pending masked low bits
		 * observed in the warm vendor state. */
		m9x_write(mdev, M9_REG_IRQ0_MASK, 0);
		m9x_write(mdev, M9_REG_IRQ_LOW_MASK, 0);
		m9x_write(mdev, M9_REG_IRQ_HIGH_MASK, 0);
		ret = request_irq(pci->irq, m9x_irq_handler, IRQF_SHARED,
				  M9X_MODULE_NAME, mdev);
		if (ret) {
			ret = dev_err_probe(&pci->dev, ret,
					    "cannot request IRQ %u\n", pci->irq);
			goto err_modeset;
		}
		mdev->irq_installed = true;
	}
	ret = drm_dev_register(drm, 0);
	if (ret)
		goto err_modeset;
	pci_set_drvdata(pci, drm);
	/* The console draws into a dumb buffer, which only the scanout path
	 * can publish, so it follows that gate rather than KMS alone. */
	if (m9x_kms_scanout_enabled())
		drm_client_setup_with_fourcc(drm, DRM_FORMAT_XRGB8888);
	return 0;

err_modeset:
	if (kms)
		m9x_modeset_fini(mdev);
err_fini:
	m9x_fini(mdev);
	m9x_address_fini(mdev);
	return ret;
}

static void m9x_pci_remove(struct pci_dev *pci)
{
	struct drm_device *drm = pci_get_drvdata(pci);

	drm_dev_unplug(drm);
	if (m9x_kms_enabled()) {
		drm_atomic_helper_shutdown(drm);
		m9x_modeset_fini(to_m9x(drm));
	}
	m9x_fini(to_m9x(drm));
	m9x_address_fini(to_m9x(drm));
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
MODULE_DESCRIPTION("Matrox M9/Phoenix clean-room DRM/KMS driver");
MODULE_LICENSE("GPL");
