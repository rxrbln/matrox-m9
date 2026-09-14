// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/jiffies.h>
#include <linux/module.h>

#include <drm/drm_managed.h>

#include "parhelia_drv.h"
#include "m9_reg.h"

static bool m9_enable;
module_param(m9_enable, bool, 0400);
MODULE_PARM_DESC(m9_enable,
	"Bind PCI 102b:0540 to the guarded clean-room M9 backend");

static bool m9_render_diag;
module_param(m9_render_diag, bool, 0400);
MODULE_PARM_DESC(m9_render_diag,
	"Permit the kernel-owned M9 no-op writeback-fence hardware diagnostic");

static uint m9_fence_timeout_ms = 100;
module_param(m9_fence_timeout_ms, uint, 0600);
MODULE_PARM_DESC(m9_fence_timeout_ms,
	"Absolute timeout for the guarded M9 writeback-fence diagnostic");

bool parhelia_is_m9(const struct parhelia_device *pdev)
{
	return pdev->chip->engine_family == PARHELIA_ENGINE_M9;
}

bool parhelia_m9_enabled(void)
{
	return m9_enable;
}

bool parhelia_m9_render_diagnostic(void)
{
	return m9_render_diag;
}

/* M9 does not use the classic Parhelia 0x384 MMIO write workaround. */
static u32 m9_read(struct parhelia_device *pdev, u32 reg)
{
	return readl((u8 __iomem *)pdev->mmio + reg);
}

static void m9_write(struct parhelia_device *pdev, u32 reg, u32 value)
{
	writel(value, (u8 __iomem *)pdev->mmio + reg);
}

static const char *m9_fence_get_driver_name(struct dma_fence *fence)
{
	return "parhelia-m9";
}

static const char *m9_fence_get_timeline_name(struct dma_fence *fence)
{
	return "m9-requester";
}

static void m9_fence_release(struct dma_fence *fence)
{
	kfree(container_of(fence, struct parhelia_m9_fence, base));
}

static const struct dma_fence_ops m9_fence_ops = {
	.get_driver_name = m9_fence_get_driver_name,
	.get_timeline_name = m9_fence_get_timeline_name,
	.release = m9_fence_release,
};

static struct parhelia_m9_fence *m9_fence_create(struct parhelia_device *pdev,
						  u64 seqno)
{
	struct parhelia_m9_fence *fence;

	fence = kzalloc_obj(*fence);
	if (!fence)
		return NULL;
	spin_lock_init(&fence->lock);
	dma_fence_init(&fence->base, &m9_fence_ops, &fence->lock,
		       pdev->m9.fence_context, seqno);
	return fence;
}

static int m9_transport_arm(struct parhelia_device *pdev)
{
	u32 command = lower_32_bits(pdev->m9.command_bus);
	u32 fence0 = lower_32_bits(pdev->m9.fence_bus);
	u32 fence1 = fence0 + sizeof(u32);

	if (pdev->m9.transport_armed)
		return 0;
	if (m9_read(pdev, M9_REG_ENGINE_STATUS) == U32_MAX)
		return -ENODEV;

	/* Keep all interrupts masked. Render completion is a memory writeback. */
	m9_write(pdev, M9_REG_IRQ0_MASK, 0);
	m9_write(pdev, M9_REG_IRQ1_MASK, 0);
	m9_write(pdev, M9_REG_IRQ2_MASK, 0);
	m9_write(pdev, M9_REG_IRQ0_STATUS, U32_MAX);
	m9_write(pdev, M9_REG_IRQ1_STATUS, U32_MAX);
	m9_write(pdev, M9_REG_IRQ2_STATUS, U32_MAX);

	/*
	 * This is the reduced m9x109 setup whose operands are fully owned here.
	 * The opaque 0x44c/450/454 fields and optional context/window registers
	 * stay firmware-owned until their values have been captured.
	 */
	m9_write(pdev, M9_REG_CONTEXT_BASE, 0x00080000);
	m9_write(pdev, M9_REG_REQUESTER_STATE, 0);
	m9_write(pdev, M9_REG_FENCE0_ADDRESS,
		 fence0 | M9_COMMAND_SYSTEM_TAG);
	m9_write(pdev, M9_REG_FENCE1_ADDRESS,
		 fence1 | M9_COMMAND_SYSTEM_TAG);
	m9_write(pdev, M9_REG_COMMAND_BASE,
		 command | M9_COMMAND_SYSTEM_TAG);
	m9_read(pdev, M9_REG_COMMAND_BASE);
	pci_set_master(pdev->pdev);
	pdev->m9.transport_armed = true;
	return 0;
}

static u64 m9_extend_fence(struct parhelia_m9 *m9, u32 raw)
{
	u32 low = raw & ~0xfu;

	if (low < m9->last_fence)
		m9->fence_epoch++;
	m9->last_fence = low;
	return ((u64)m9->fence_epoch << 32) | low;
}

int parhelia_m9_submit_noop(struct parhelia_device *pdev, u64 *point)
{
	struct parhelia_m9_fence *fence;
	unsigned long deadline;
	u32 *command = pdev->m9.command_cpu;
	u32 *writeback = pdev->m9.fence_cpu;
	u32 target, raw;
	u64 extended;
	int ret;

	if (!m9_render_diag)
		return -EACCES;
	mutex_lock(&pdev->m9.lock);
	if (pdev->m9.wedged) {
		ret = -EIO;
		goto out_unlock;
	}
	ret = m9_transport_arm(pdev);
	if (ret)
		goto out_unlock;

	target = pdev->m9.next_fence;
	pdev->m9.next_fence += 0x10;
	if (!pdev->m9.next_fence)
		pdev->m9.next_fence = 0x10;
	fence = m9_fence_create(pdev,
		((u64)(pdev->m9.fence_epoch +
			(target < pdev->m9.last_fence)) << 32) | target);
	if (!fence) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	WRITE_ONCE(writeback[0], ~target);
	command[0] = M9_PACKET_FENCE_HEADER;
	command[1] = 3;
	command[2] = target | 1;
	command[3] = 0x45454545;
	dma_wmb();

	/*
	 * Windows clears the front-end state immediately before ringing 0x404.
	 * The endpoint is a byte address, never a size or a userspace pointer.
	 */
	m9_write(pdev, M9_REG_FRONTEND_STATE, 0);
	m9_write(pdev, M9_REG_COMMAND_END,
		 lower_32_bits(pdev->m9.command_bus) + 3 * sizeof(u32));
	m9_write(pdev, M9_REG_FRONTEND_KICK, 1);

	deadline = jiffies + msecs_to_jiffies(max(m9_fence_timeout_ms, 1u));
	for (;;) {
		dma_rmb();
		raw = READ_ONCE(writeback[0]);
		if ((raw & ~0xfu) == target)
			break;
		if (m9_read(pdev, M9_REG_ENGINE_STATUS) == U32_MAX) {
			ret = -ENODEV;
			goto out_error;
		}
		if (time_after_eq(jiffies, deadline)) {
			ret = -ETIMEDOUT;
			goto out_error;
		}
		m9_write(pdev, M9_REG_FRONTEND_KICK, 1);
		usleep_range(50, 100);
	}

	extended = m9_extend_fence(&pdev->m9, raw);
	dma_fence_signal(&fence->base);
	dma_fence_put(&fence->base);
	*point = extended;
	ret = 0;
	goto out_unlock;

out_error:
	pdev->m9.wedged = true;
	pdev->m9.generation++;
	dma_fence_set_error(&fence->base, ret);
	dma_fence_signal(&fence->base);
	dma_fence_put(&fence->base);
	/* Leave all coherent allocations reachable until teardown. */
out_unlock:
	mutex_unlock(&pdev->m9.lock);
	return ret;
}

int parhelia_m9_init(struct parhelia_device *pdev)
{
	struct device *dev = &pdev->pdev->dev;
	int ret;

	ret = parhelia_m9_submit_selftest();
	if (ret)
		return dev_err_probe(dev, ret, "M9 submit selftest failed\n");
	ret = drmm_mutex_init(&pdev->drm, &pdev->m9.lock);
	if (ret)
		return ret;
	INIT_LIST_HEAD(&pdev->m9.bos);
	pdev->m9.fence_context = dma_fence_context_alloc(1);
	pdev->m9.next_fence = 0x10;
	pdev->m9.requester_cpu = dmam_alloc_coherent(dev, PAGE_SIZE,
						      &pdev->m9.requester_bus,
						      GFP_KERNEL | __GFP_ZERO);
	pdev->m9.command_cpu = dmam_alloc_coherent(dev, PARHELIA_M9_COMMAND_BYTES,
						    &pdev->m9.command_bus,
						    GFP_KERNEL | __GFP_ZERO);
	pdev->m9.fence_cpu = dmam_alloc_coherent(dev, PAGE_SIZE,
						  &pdev->m9.fence_bus,
						  GFP_KERNEL | __GFP_ZERO);
	if (!pdev->m9.requester_cpu || !pdev->m9.command_cpu ||
	    !pdev->m9.fence_cpu)
		return -ENOMEM;
	if (upper_32_bits(pdev->m9.requester_bus) ||
	    upper_32_bits(pdev->m9.command_bus) ||
	    upper_32_bits(pdev->m9.fence_bus))
		return -ERANGE;
	memset32(pdev->m9.command_cpu, 0x45454545,
		 PARHELIA_M9_COMMAND_BYTES / sizeof(u32));
	memset32(pdev->m9.requester_cpu, 0x45454545,
		 PAGE_SIZE / sizeof(u32));
	memset32(pdev->m9.fence_cpu, U32_MAX, PAGE_SIZE / sizeof(u32));
	dma_wmb();
	dev_info(dev,
		 "M9 clean-room transport allocated; hardware submission %s\n",
		 m9_render_diag ? "requires explicit NOOP_FENCE ioctl" : "disabled");
	return 0;
}

void parhelia_m9_fini(struct parhelia_device *pdev)
{
	mutex_lock(&pdev->m9.lock);
	pdev->m9.wedged = true;
	/* Stop host DMA before devres releases any reachable allocation. */
	pci_clear_master(pdev->pdev);
	pdev->m9.transport_armed = false;
	mutex_unlock(&pdev->m9.lock);
}
