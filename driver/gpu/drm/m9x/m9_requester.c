// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/jiffies.h>
#include <linux/module.h>

#include <drm/drm_managed.h>

#include "m9_drv.h"
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

bool m9x_enabled(void)
{
	return m9_enable;
}

/* M9 does not use the classic Parhelia 0x384 MMIO write workaround. */
static u32 m9_read(struct m9x_device *mdev, u32 reg)
{
	return readl((u8 __iomem *)mdev->mmio + reg);
}

static void m9_write(struct m9x_device *mdev, u32 reg, u32 value)
{
	writel(value, (u8 __iomem *)mdev->mmio + reg);
}

static const char *m9_fence_get_driver_name(struct dma_fence *fence)
{
	return "m9x";
}

static const char *m9_fence_get_timeline_name(struct dma_fence *fence)
{
	return "m9-requester";
}

static void m9_fence_release(struct dma_fence *fence)
{
	kfree(container_of(fence, struct m9x_fence, base));
}

static const struct dma_fence_ops m9_fence_ops = {
	.get_driver_name = m9_fence_get_driver_name,
	.get_timeline_name = m9_fence_get_timeline_name,
	.release = m9_fence_release,
};

static struct m9x_fence *m9_fence_create(struct m9x_device *mdev,
						  u64 seqno)
{
	struct m9x_fence *fence;

	fence = kzalloc_obj(*fence);
	if (!fence)
		return NULL;
	spin_lock_init(&fence->lock);
	dma_fence_init(&fence->base, &m9_fence_ops, &fence->lock,
		       mdev->transport.fence_context, seqno);
	return fence;
}

static int m9_transport_arm(struct m9x_device *mdev)
{
	u32 command = lower_32_bits(mdev->transport.command_bus);
	u32 writeback0 = lower_32_bits(mdev->transport.fence_bus);
	u32 writeback1 = writeback0 + sizeof(u32);

	if (mdev->transport.transport_armed)
		return 0;
	if (m9_read(mdev, M9_REG_ENGINE_STATUS) == U32_MAX)
		return -ENODEV;
	pci_set_master(mdev->pdev);

	/* Keep all interrupts masked. Render completion is a memory writeback. */
	m9_write(mdev, M9_REG_IRQ0_MASK, 0);
	m9_write(mdev, M9_REG_IRQ1_MASK, 0);
	m9_write(mdev, M9_REG_IRQ2_MASK, 0);
	m9_write(mdev, M9_REG_IRQ0_STATUS, U32_MAX);
	m9_write(mdev, M9_REG_IRQ1_STATUS, U32_MAX);
	m9_write(mdev, M9_REG_IRQ2_STATUS, U32_MAX);

	/*
	 * This is the reduced m9x109 setup whose operands are fully owned here.
	 * The opaque 0x44c/450/454 fields and optional context/window registers
	 * stay firmware-owned until their values have been captured.
	 */
	m9_write(mdev, M9_REG_CONTEXT_BASE, 0x00080000);
	m9_write(mdev, M9_REG_REQUESTER_STATE, 0);
	/* Index e3 writes the address selected by 0x106c.  Keep 0x1068 on an
	 * independent word so the two hardware fence selectors cannot alias. */
	m9_write(mdev, M9_REG_FENCE0_ADDRESS,
		 writeback1 | M9_COMMAND_SYSTEM_TAG);
	m9_write(mdev, M9_REG_FENCE1_ADDRESS,
		 writeback0 | M9_COMMAND_SYSTEM_TAG);
	m9_write(mdev, M9_REG_COMMAND_BASE,
		 command | M9_COMMAND_SYSTEM_TAG);
	m9_read(mdev, M9_REG_COMMAND_BASE);
	mdev->transport.transport_armed = true;
	return 0;
}

static u64 m9_extend_fence(struct m9x_transport *m9, u32 raw)
{
	u32 low = raw & ~0xfu;

	if (low < m9->last_fence)
		m9->fence_epoch++;
	m9->last_fence = low;
	return ((u64)m9->fence_epoch << 32) | low;
}

static u32 m9_next_fence_target(struct m9x_transport *m9)
{
	u32 target = m9->next_fence;

	m9->next_fence += 0x10;
	return target;
}

static bool m9_fence_reached(u32 raw, u32 target)
{
	return (raw & ~0xfu) == target;
}

static int m9_requester_selftest(void)
{
	struct m9x_transport transport = {
		.next_fence = 0xfffffff0,
		.last_fence = 0xfffffff0,
		.fence_epoch = 7,
	};
	u32 target;

	target = m9_next_fence_target(&transport);
	if (target != 0xfffffff0 || transport.next_fence != 0 ||
	    m9_fence_reached(~target, target) ||
	    !m9_fence_reached(target | 1, target))
		return -EINVAL;
	target = m9_next_fence_target(&transport);
	if (target != 0 || transport.next_fence != 0x10 ||
	    m9_fence_reached(~target, target) ||
	    !m9_fence_reached(target | 1, target) ||
	    m9_extend_fence(&transport, target | 1) != (8ULL << 32))
		return -EINVAL;
	return 0;
}

int m9x_submit_noop(struct m9x_device *mdev, u64 *point)
{
	struct m9x_fence *fence;
	unsigned long deadline;
	u32 *command;
	u32 *writeback = mdev->transport.fence_cpu;
	u32 command_offset, command_end, target, raw;
	u64 extended;
	bool progress_kicked = false;
	int ret;

	if (!m9_render_diag)
		return -EACCES;
	mutex_lock(&mdev->transport.lock);
	if (mdev->transport.wedged) {
		ret = -EIO;
		goto out_unlock;
	}
	ret = m9_transport_arm(mdev);
	if (ret)
		goto out_unlock;
	command_offset = mdev->transport.command_offset;
	if (command_offset > PARHELIA_M9_COMMAND_BYTES - 3 * sizeof(u32)) {
		/* The vendor requester advances through a producer interval.  A
		 * rewind needs its separately recovered rollover protocol, so keep
		 * this diagnostic bounded instead of guessing that reset sequence. */
		ret = -ENOSPC;
		goto out_unlock;
	}
	command = (u32 *)((u8 *)mdev->transport.command_cpu + command_offset);
	command_end = lower_32_bits(mdev->transport.command_bus) +
		      command_offset + 3 * sizeof(u32);

	target = m9_next_fence_target(&mdev->transport);
	fence = m9_fence_create(mdev,
		((u64)(mdev->transport.fence_epoch +
			(target < mdev->transport.last_fence)) << 32) | target);
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
	m9_write(mdev, M9_REG_FRONTEND_STATE, 0);
	m9_write(mdev, M9_REG_COMMAND_END, command_end);
	m9_write(mdev, M9_REG_FRONTEND_KICK, 1);

	deadline = jiffies + msecs_to_jiffies(max(m9_fence_timeout_ms, 1u));
	for (;;) {
		dma_rmb();
		raw = READ_ONCE(writeback[0]);
		if (m9_fence_reached(raw, target))
			break;
		if (m9_read(mdev, M9_REG_ENGINE_STATUS) == U32_MAX) {
			ret = -ENODEV;
			goto out_error;
		}
		if (time_after_eq(jiffies, deadline)) {
			dma_rmb();
			dev_err(&mdev->pdev->dev,
				"M9 fence timeout: target=%#x wb=%#x/%#x engine=%#x frontend=%#x command=%#x/%#x fence=%#x/%#x\n",
				target, READ_ONCE(writeback[0]),
				READ_ONCE(writeback[1]),
				m9_read(mdev, M9_REG_ENGINE_STATUS),
				m9_read(mdev, M9_REG_FRONTEND_STATE),
				m9_read(mdev, M9_REG_COMMAND_BASE),
				m9_read(mdev, M9_REG_COMMAND_END),
				m9_read(mdev, M9_REG_FENCE0_ADDRESS),
				m9_read(mdev, M9_REG_FENCE1_ADDRESS));
			ret = -ETIMEDOUT;
			goto out_error;
		}
		/* The recovered timeout contract permits one progress kick.  Repeated
		 * writes can hide the first failing state and are not a reset. */
		if (!progress_kicked) {
			m9_write(mdev, M9_REG_FRONTEND_KICK, 1);
			progress_kicked = true;
		}
		usleep_range(50, 100);
	}

	extended = m9_extend_fence(&mdev->transport, raw);
	mdev->transport.command_offset = command_offset + 3 * sizeof(u32);
	dma_fence_signal(&fence->base);
	dma_fence_put(&fence->base);
	*point = extended;
	ret = 0;
	goto out_unlock;

out_error:
	mdev->transport.wedged = true;
	mdev->transport.generation++;
	dma_fence_set_error(&fence->base, ret);
	dma_fence_signal(&fence->base);
	dma_fence_put(&fence->base);
	/* Leave all coherent allocations reachable until teardown. */
out_unlock:
	mutex_unlock(&mdev->transport.lock);
	return ret;
}

int m9x_init(struct m9x_device *mdev)
{
	struct device *dev = &mdev->pdev->dev;
	int ret;

	ret = m9x_submit_selftest();
	if (ret)
		return dev_err_probe(dev, ret, "M9 submit selftest failed\n");
	ret = m9_requester_selftest();
	if (ret)
		return dev_err_probe(dev, ret, "M9 requester selftest failed\n");
	ret = drmm_mutex_init(&mdev->drm, &mdev->transport.lock);
	if (ret)
		return ret;
	INIT_LIST_HEAD(&mdev->transport.bos);
	mdev->transport.fence_context = dma_fence_context_alloc(1);
	mdev->transport.next_fence = 0x10;
	mdev->transport.requester_cpu = dmam_alloc_coherent(dev, PAGE_SIZE,
						      &mdev->transport.requester_bus,
						      GFP_KERNEL | __GFP_ZERO);
	mdev->transport.command_cpu = dmam_alloc_coherent(dev, PARHELIA_M9_COMMAND_BYTES,
						    &mdev->transport.command_bus,
						    GFP_KERNEL | __GFP_ZERO);
	mdev->transport.fence_cpu = dmam_alloc_coherent(dev, PAGE_SIZE,
						  &mdev->transport.fence_bus,
						  GFP_KERNEL | __GFP_ZERO);
	if (!mdev->transport.requester_cpu || !mdev->transport.command_cpu ||
	    !mdev->transport.fence_cpu)
		return -ENOMEM;
	if (upper_32_bits(mdev->transport.requester_bus) ||
	    upper_32_bits(mdev->transport.command_bus) ||
	    upper_32_bits(mdev->transport.fence_bus))
		return -ERANGE;
	memset32(mdev->transport.command_cpu, 0x45454545,
		 PARHELIA_M9_COMMAND_BYTES / sizeof(u32));
	memset32(mdev->transport.requester_cpu, 0x45454545,
		 PAGE_SIZE / sizeof(u32));
	memset32(mdev->transport.fence_cpu, U32_MAX, PAGE_SIZE / sizeof(u32));
	dma_wmb();
	dev_info(dev,
		 "M9 clean-room transport allocated; hardware submission %s\n",
		 m9_render_diag ? "requires explicit NOOP_FENCE ioctl" : "disabled");
	return 0;
}

void m9x_fini(struct m9x_device *mdev)
{
	mutex_lock(&mdev->transport.lock);
	mdev->transport.wedged = true;
	/* Stop host DMA before devres releases any reachable allocation. */
	pci_clear_master(mdev->pdev);
	mdev->transport.transport_armed = false;
	mutex_unlock(&mdev->transport.lock);
}
