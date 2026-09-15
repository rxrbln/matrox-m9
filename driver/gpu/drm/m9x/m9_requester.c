// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/iosys-map.h>
#include <linux/jiffies.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>

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

#define M9_RESTORE_RECORD_BASE		0x700u
#define M9_RESTORE_RECORD_STRIDE	0x600u
#define M9_RESTORE_RECORD_COUNT		0x400u
#define M9_RESTORE_WORK_OFFSET		0x200u
#define M9_RESTORE_FILL			0x45454545u

/* Live vendor local restore-pool prefix (BAR0 image at the pool base). */
static const u32 m9_restore_prefix[62] = {
	0x45454747, 0xeeedeff0, 0xf4f3f2ec, 0x47474747,
	0xb3b2b1b0, 0xb7b6b5b4, 0xbbbab9b8, 0x45bdbcbe,
	0x47474747, 0xabaaa9a8, 0xf6f5aeac, 0xa5a7c0af,
	0xc7c6c5c4, 0x45454547, 0xada4a3a1, 0x47474747,
	0xc28a8988, 0x8e8d8cc3, 0x75797874, 0x78767978,
	0x45454547, 0x79787779, 0x47474747, 0x7a797870,
	0x83827d7c, 0x81808584, 0x45877f7e, 0x47474747,
	0x7a797871, 0x83827d7c, 0x81808584, 0x45457f7e,
	0x47474747, 0x7a797872, 0x83827d7c, 0x81808584,
	0x45457f7e, 0x47474747, 0x7a797873, 0x83827d7c,
	0x81808584, 0x45457f7e, 0x45474747, 0xd1d0ebea,
	0x45d4d3ed, 0xc1d68b8f, 0x47474747, 0x93929190,
	0x97969594, 0x9b9a9998, 0x9f9e9d9c, 0x47474747,
	0x6b6a6968, 0x456e6d6c, 0x63626160, 0x67666564,
	0x45474747, 0x45f3d7f1, 0xdad9d8d7, 0xdedddcdb,
	0x45454547, 0xe8d5a2d2,
};

/* Live vendor m9x72 record; zero words are patched per record. */
static const u32 m9_restore_record[20] = {
	0x454545e6, 0x624f7fff, 0x45e3e6e6, 0x00000000,
	0x00000002, 0xfffffff0, 0x2f2e3c28, 0x00000000,
	0x00000000, 0x00000000, 0x00000000, 0x45454543,
	0xfffffff0, 0x45452f2e, 0x00000000, 0x00000000,
	0x453ee6e6, 0x08000000, 0x624fbfbf, 0x00000000,
};

static void m9_restore_fill(u32 *pool, u32 base)
{
	unsigned int i;

	memset32(pool, M9_RESTORE_FILL, PARHELIA_M9_RESTORE_BYTES / sizeof(*pool));
	memcpy(pool, m9_restore_prefix, sizeof(m9_restore_prefix));
	for (i = 0; i < M9_RESTORE_RECORD_COUNT; i++) {
		u32 *record = pool + (M9_RESTORE_RECORD_BASE +
				      i * M9_RESTORE_RECORD_STRIDE) / sizeof(u32);
		u32 work = base + M9_RESTORE_WORK_OFFSET +
			   i * M9_RESTORE_RECORD_STRIDE;

		memcpy(record, m9_restore_record, sizeof(m9_restore_record));
		record[3] = i == 1 ? 0x0c000000 : 0x62000000;
		record[9] = work;
		record[10] = work + 0x401;
		record[14] = base;
		record[15] = base + 0xf8;
		record[19] = work + 0x440;
	}
}

int m9x_restore_bind(struct m9x_device *mdev)
{
	struct iosys_map map;
	u32 *pool;
	u32 base;
	int ret;

	ret = m9x_address_restore_region(mdev, &map, &base);
	if (ret)
		return ret;
	if (base > U32_MAX - PARHELIA_M9_RESTORE_BYTES)
		return -ERANGE;
	pool = kvmalloc(PARHELIA_M9_RESTORE_BYTES, GFP_KERNEL);
	if (!pool)
		return -ENOMEM;
	m9_restore_fill(pool, base);
	iosys_map_memcpy_to(&map, 0, pool, PARHELIA_M9_RESTORE_BYTES);
	kvfree(pool);
	/* The BAR/WC pool image must be visible before its publication. */
	wmb();
	mdev->transport.restore_base = base;
	return 0;
}

static int m9_restore_selftest(void)
{
	u32 *pool;
	int ret = -EINVAL;

	pool = kvmalloc(PARHELIA_M9_RESTORE_BYTES, GFP_KERNEL);
	if (!pool)
		return -ENOMEM;
	m9_restore_fill(pool, 0x00ac1000);
	/* Words of the live vendor pool at 0xac1000 on the 1 GiB M9148. */
	if (pool[0] != 0x45454747 || pool[61] != 0xe8d5a2d2 ||
	    pool[62] != M9_RESTORE_FILL ||
	    memcmp(pool + 0x1c0, (const u32[]) {
		0x454545e6, 0x624f7fff, 0x45e3e6e6, 0x62000000, 0x00000002,
		0xfffffff0, 0x2f2e3c28, 0, 0, 0x00ac1200, 0x00ac1601,
		0x45454543, 0xfffffff0, 0x45452f2e, 0x00ac1000, 0x00ac10f8,
		0x453ee6e6, 0x08000000, 0x624fbfbf, 0x00ac1640 },
		20 * sizeof(u32)) ||
	    pool[0x340 + 3] != 0x0c000000 || pool[0x340 + 9] != 0x00ac1800 ||
	    pool[0x340 + 19] != 0x00ac1c40 ||
	    pool[(M9_RESTORE_RECORD_BASE + 1023 * M9_RESTORE_RECORD_STRIDE) /
		 sizeof(u32) + 20] != M9_RESTORE_FILL)
		goto out;
	ret = 0;
out:
	kvfree(pool);
	return ret;
}

bool m9x_enabled(void)
{
	return m9_enable;
}

static dma_addr_t m9_command_slot_bus(const struct m9x_transport *transport)
{
	return transport->command_bus +
	       transport->command_slot * PARHELIA_M9_COMMAND_BYTES;
}

static dma_addr_t m9_command_current_bus(const struct m9x_transport *transport)
{
	return m9_command_slot_bus(transport) + transport->command_offset;
}

static void *m9_command_slot_cpu(const struct m9x_transport *transport)
{
	return (u8 *)transport->command_cpu +
	       transport->command_slot * PARHELIA_M9_COMMAND_BYTES;
}

static void m9_advance_command_slot(struct m9x_transport *transport)
{
	transport->command_slot = (transport->command_slot + 1) %
				  PARHELIA_M9_COMMAND_SLOTS;
	transport->command_offset = 0;
}

static bool m9_primary_controls_valid(struct m9x_device *mdev)
{
	return m9x_read(mdev, M9_REG_REQUESTER_CTL0) ==
			M9_REQUESTER_CTL0_FALLBACK &&
	       m9x_read(mdev, M9_REG_REQUESTER_CTL1) ==
			M9_REQUESTER_CTL1_FALLBACK &&
	       m9x_read(mdev, M9_REG_REQUESTER_CTL2) ==
			M9_REQUESTER_CTL2_FALLBACK;
}

int m9x_transport_enable_bus_master_locked(struct m9x_device *mdev)
{
	struct m9x_transport *transport = &mdev->transport;
	u32 base, end;

	lockdep_assert_held(&transport->lock);
	if (transport->bus_master_enabled)
		return 0;
	if (m9x_read(mdev, M9_REG_ENGINE_STATUS) == U32_MAX)
		return -ENODEV;
	if (!m9_primary_controls_valid(mdev))
		return -EOPNOTSUPP;

	/*
	 * The vendor enables PCI mastering before service and compact-translation
	 * construction.  Refuse a reload whose old requester interval has not
	 * retired: 0x400 is hardware-advanced and retains its low address-class
	 * tag, while 0x404 is the untagged end-exclusive producer.
	 */
	base = m9x_read(mdev, M9_REG_COMMAND_BASE);
	end = m9x_read(mdev, M9_REG_COMMAND_END);
	if ((base & ~3u) != end) {
		dev_err(&mdev->pdev->dev,
			"M9 requester is not quiescent before bus-master enable: %#x/%#x\n",
			base, end);
		return -EBUSY;
	}
	pci_set_master(mdev->pdev);
	transport->bus_master_enabled = true;
	return 0;
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

static u64 m9_extend_fence(struct m9x_transport *m9, u32 raw);
static bool m9_fence_reached(u32 raw, u32 target);

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

struct m9x_fence *m9x_fence_create_target_locked(struct m9x_device *mdev,
						 u32 target)
{
	struct m9x_transport *transport = &mdev->transport;
	u32 epoch;

	lockdep_assert_held(&transport->lock);
	epoch = transport->fence_epoch + (target < transport->last_fence);
	return m9_fence_create(mdev, ((u64)epoch << 32) | target);
}

int m9x_transport_arm_locked(struct m9x_device *mdev)
{
	struct m9x_transport *transport = &mdev->transport;
	u32 command = lower_32_bits(m9_command_current_bus(transport));
	u32 writeback0 = lower_32_bits(mdev->transport.fence_bus);
	u32 writeback1 = writeback0 + sizeof(u32);
	u32 restore = transport->restore_ready ? transport->restore_base : 0;
	u32 restore_base = restore ? (restore + 0x200) & ~0x1fu : 0;
	u32 restore_limit = restore ?
		((restore + 0x17fc00) & ~0x1fu) | 6 : 0;
	int ret;

	lockdep_assert_held(&mdev->transport.lock);
	if (mdev->transport.transport_armed)
		return 0;
	ret = m9x_transport_enable_bus_master_locked(mdev);
	if (ret)
		return ret;
	if (m9x_read(mdev, M9_REG_ENGINE_STATUS) == U32_MAX)
		return -ENODEV;
	if (!m9_primary_controls_valid(mdev)) {
		dev_err(&mdev->pdev->dev,
			"M9 primary requester controls differ from tested M9148 values: %#x/%#x/%#x\n",
			m9x_read(mdev, M9_REG_REQUESTER_CTL0),
			m9x_read(mdev, M9_REG_REQUESTER_CTL1),
			m9x_read(mdev, M9_REG_REQUESTER_CTL2));
		return -EOPNOTSUPP;
	}
	if (transport->restore_ready &&
	    (!restore || restore > U32_MAX - PARHELIA_M9_RESTORE_BYTES))
		return -ERANGE;
	/*
	 * Byte-for-byte m9x109 register order. The live vendor restore pool is
	 * local (mode 3), so its 0x10ec tag is zero.
	 */
	m9x_write(mdev, M9_REG_COMMAND_BASE,
		  command | M9_COMMAND_SYSTEM_TAG);
	m9x_write(mdev, M9_REG_CONTEXT_BASE, 0x00080000);
	m9x_write(mdev, M9_REG_REQUESTER_STATE, 0);
	/* Index e3 writes the address selected by 0x106c.  Keep 0x1068 on an
	 * independent word so the two hardware fence selectors cannot alias. */
	m9x_write(mdev, M9_REG_FENCE0_ADDRESS,
		 writeback1 | M9_COMMAND_SYSTEM_TAG);
	m9x_write(mdev, M9_REG_FENCE1_ADDRESS,
		 writeback0 | M9_COMMAND_SYSTEM_TAG);
	m9x_write(mdev, M9_REG_RESTORE_TAGGED, restore_base);
	m9x_write(mdev, M9_REG_RESTORE_BASE, restore_base);
	m9x_write(mdev, M9_REG_RESTORE_LIMIT, restore_limit);
	m9x_write(mdev, M9_REG_REQUESTER_CTL0, M9_REQUESTER_CTL0_FALLBACK);
	m9x_write(mdev, M9_REG_REQUESTER_CTL1, M9_REQUESTER_CTL1_FALLBACK);
	m9x_write(mdev, M9_REG_REQUESTER_CTL2, M9_REQUESTER_CTL2_FALLBACK);
	m9x_read(mdev, M9_REG_COMMAND_BASE);
	dev_info(&mdev->pdev->dev,
		 "M9 requester armed: command=%#x programmed fences=%#x/%#x restore=%#x/%#x/%#x controls=%#x/%#x/%#x\n",
		 m9x_read(mdev, M9_REG_COMMAND_BASE),
		 writeback1 | M9_COMMAND_SYSTEM_TAG,
		 writeback0 | M9_COMMAND_SYSTEM_TAG,
		 restore_base, restore_base, restore_limit,
		 m9x_read(mdev, M9_REG_REQUESTER_CTL0),
		 m9x_read(mdev, M9_REG_REQUESTER_CTL1),
		 m9x_read(mdev, M9_REG_REQUESTER_CTL2));
	mdev->transport.transport_armed = true;
	return 0;
}

int m9x_transport_rearm_locked(struct m9x_device *mdev)
{
	lockdep_assert_held(&mdev->transport.lock);
	mdev->transport.transport_armed = false;
	return m9x_transport_arm_locked(mdev);
}

int m9x_submit_kernel_stream_locked(struct m9x_device *mdev,
				    const u32 *commands, u32 dwords,
				    u32 target, u64 *point, bool *doorbell_posted)
{
	struct m9x_transport *transport = &mdev->transport;
	unsigned long deadline;
	dma_addr_t start_bus;
	u32 *writeback = transport->fence_cpu;
	u32 command_offset, end, raw;
	u64 extended;
	int ret;

	lockdep_assert_held(&transport->lock);
	if (doorbell_posted)
		*doorbell_posted = false;
	if (!commands || !dwords ||
	    dwords > PARHELIA_M9_COMMAND_BYTES / sizeof(u32))
		return -EINVAL;
	if (transport->wedged)
		return -EIO;
	ret = m9x_transport_arm_locked(mdev);
	if (ret)
		return ret;

	command_offset = transport->command_offset;
	if (command_offset > PARHELIA_M9_COMMAND_BYTES - dwords * sizeof(u32)) {
		/* The prior interval is synchronous and retired.  Only an actual
		 * primary-buffer switch rebases the vendor requester's 0x400. */
		m9_advance_command_slot(transport);
		command_offset = 0;
		m9x_write(mdev, M9_REG_COMMAND_BASE,
			  lower_32_bits(m9_command_slot_bus(transport)) |
			  M9_COMMAND_SYSTEM_TAG);
		m9x_read(mdev, M9_REG_COMMAND_BASE);
	}
	start_bus = m9_command_slot_bus(transport) + command_offset;
	if (upper_32_bits(start_bus) ||
	    check_add_overflow(lower_32_bits(start_bus),
			       dwords * (u32)sizeof(u32), &end))
		return -ERANGE;
	memcpy((u8 *)m9_command_slot_cpu(transport) + command_offset, commands,
	       dwords * sizeof(u32));
	dma_wmb();

	m9x_write(mdev, M9_REG_COMMAND_END, end);
	m9x_read(mdev, M9_REG_COMMAND_END);
	if (doorbell_posted)
		*doorbell_posted = true;

	deadline = jiffies + msecs_to_jiffies(max(m9_fence_timeout_ms, 1u));
	for (;;) {
		raw = READ_ONCE(writeback[0]);
		if (m9_fence_reached(raw, target)) {
			dma_rmb();
			break;
		}
		if (m9x_read(mdev, M9_REG_ENGINE_STATUS) == U32_MAX) {
			transport->wedged = true;
			transport->generation++;
			return -ENODEV;
		}
		if (time_after_eq(jiffies, deadline)) {
			dev_err(&mdev->pdev->dev,
				"M9 kernel stream timeout: target=%#x wb=%#x/%#x engine=%#x frontend=%#x kick=%#x command=%#x/%#x\n",
				target, READ_ONCE(writeback[0]),
				READ_ONCE(writeback[1]),
				m9x_read(mdev, M9_REG_ENGINE_STATUS),
				m9x_read(mdev, M9_REG_FRONTEND_STATE),
				m9x_read(mdev, M9_REG_FRONTEND_KICK),
				m9x_read(mdev, M9_REG_COMMAND_BASE),
				m9x_read(mdev, M9_REG_COMMAND_END));
			dev_err(&mdev->pdev->dev,
				"M9 timeout windows: 360/364=%#x/%#x f64/f68=%#x/%#x f74/f78/f7c=%#x/%#x/%#x 132c/1330/1334=%#x/%#x/%#x\n",
				m9x_read(mdev, M9_REG_TRANSLATION_BASE),
				m9x_read(mdev, M9_REG_TRANSLATION_LIMIT),
				m9x_read(mdev, M9_REG_PTE_BASE),
				m9x_read(mdev, M9_REG_PTE_LIMIT),
				m9x_read(mdev, M9_REG_LOCAL_BASE0),
				m9x_read(mdev, M9_REG_LOCAL_BASE1),
				m9x_read(mdev, M9_REG_LOCAL_LIMIT),
				m9x_read(mdev, M9_REG_SURFACE_BASE2),
				m9x_read(mdev, M9_REG_SURFACE_BASE3),
				m9x_read(mdev, M9_REG_SURFACE_LIMIT1));
			dev_err(&mdev->pdev->dev,
				"M9 timeout requester: 448=%#x 1068/106c=%#x/%#x 10ec/1094/1098=%#x/%#x/%#x\n",
				m9x_read(mdev, M9_REG_REQUESTER_STATE),
				m9x_read(mdev, M9_REG_FENCE0_ADDRESS),
				m9x_read(mdev, M9_REG_FENCE1_ADDRESS),
				m9x_read(mdev, M9_REG_RESTORE_TAGGED),
				m9x_read(mdev, M9_REG_RESTORE_BASE),
				m9x_read(mdev, M9_REG_RESTORE_LIMIT));
			transport->wedged = true;
			transport->generation++;
			return -ETIMEDOUT;
		}
		usleep_range(50, 100);
	}

	extended = m9_extend_fence(transport, raw);
	transport->command_offset = command_offset + dwords * sizeof(u32);
	if (point)
		*point = extended;
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
		.command_bus = 0x00100000,
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
	if (M9_REQUESTER_CTL0_FALLBACK != 0x00280800 ||
	    M9_REQUESTER_CTL1_FALLBACK != 0x1f3f2707 ||
	    M9_REQUESTER_CTL2_FALLBACK != 0x00070703)
		return -EINVAL;
	if (m9_command_slot_bus(&transport) != 0x00100000)
		return -EINVAL;
	transport.command_offset = PARHELIA_M9_COMMAND_BYTES - 4;
	m9_advance_command_slot(&transport);
	if (transport.command_slot != 1 || transport.command_offset ||
	    m9_command_slot_bus(&transport) !=
	    0x00100000 + PARHELIA_M9_COMMAND_BYTES)
		return -EINVAL;
	m9_advance_command_slot(&transport);
	if (transport.command_slot || transport.command_offset ||
	    m9_command_slot_bus(&transport) != 0x00100000)
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
	int ret;

	if (!m9_render_diag)
		return -EACCES;
	mutex_lock(&mdev->transport.lock);
	if (mdev->transport.wedged) {
		ret = -EIO;
		goto out_unlock;
	}
	ret = m9x_transport_enable_bus_master_locked(mdev);
	if (ret)
		goto out_unlock;
	ret = m9x_transport_arm_locked(mdev);
	if (ret)
		goto out_unlock;
	command_offset = mdev->transport.command_offset;
	if (command_offset > PARHELIA_M9_COMMAND_BYTES - 4 * sizeof(u32)) {
		/*
		 * Every submission is synchronous here, so the prior slot has
		 * retired before its successor becomes visible.  Rebase 0x400 to
		 * the alternate kernel-owned slot instead of rewinding a published
		 * producer interval in place.
		 */
		m9_advance_command_slot(&mdev->transport);
		command_offset = 0;
		m9x_write(mdev, M9_REG_COMMAND_BASE,
			 lower_32_bits(m9_command_slot_bus(&mdev->transport)) |
			 M9_COMMAND_SYSTEM_TAG);
		m9x_read(mdev, M9_REG_COMMAND_BASE);
	}
	command = (u32 *)((u8 *)m9_command_slot_cpu(&mdev->transport) +
			  command_offset);
	command_end = lower_32_bits(m9_command_slot_bus(&mdev->transport)) +
		      command_offset + 3 * sizeof(u32);

	target = m9_next_fence_target(&mdev->transport);
	fence = m9x_fence_create_target_locked(mdev, target);
	if (!fence) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	command[0] = M9_PACKET_FENCE_HEADER;
	command[1] = 3;
	command[2] = target | 1;
	command[3] = 0x45454545;
	dma_wmb();

	/*
	 * A class-2 requester skips the vendor's class-0 0x31c clear.  This also
	 * preserves an active compact-translation state across submissions.
	 */
	m9x_write(mdev, M9_REG_COMMAND_END, command_end);
	m9x_read(mdev, M9_REG_COMMAND_END);

	deadline = jiffies + msecs_to_jiffies(max(m9_fence_timeout_ms, 1u));
	for (;;) {
		raw = READ_ONCE(writeback[0]);
		if (m9_fence_reached(raw, target)) {
			dma_rmb();
			break;
		}
		if (m9x_read(mdev, M9_REG_ENGINE_STATUS) == U32_MAX) {
			ret = -ENODEV;
			goto out_error;
		}
		if (time_after_eq(jiffies, deadline)) {
			dma_rmb();
			dev_err(&mdev->pdev->dev,
				"M9 fence timeout: target=%#x wb=%#x/%#x engine=%#x frontend=%#x command=%#x/%#x fence=%#x/%#x\n",
				target, READ_ONCE(writeback[0]),
				READ_ONCE(writeback[1]),
				m9x_read(mdev, M9_REG_ENGINE_STATUS),
				m9x_read(mdev, M9_REG_FRONTEND_STATE),
				m9x_read(mdev, M9_REG_COMMAND_BASE),
				m9x_read(mdev, M9_REG_COMMAND_END),
				m9x_read(mdev, M9_REG_FENCE0_ADDRESS),
				m9x_read(mdev, M9_REG_FENCE1_ADDRESS));
			ret = -ETIMEDOUT;
			goto out_error;
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
	size_t command_bytes = PARHELIA_M9_COMMAND_BYTES *
			       PARHELIA_M9_COMMAND_SLOTS;
	int ret;

	ret = m9x_submit_selftest();
	if (ret)
		return dev_err_probe(dev, ret, "M9 submit selftest failed\n");
	ret = m9x_triangle_selftest();
	if (ret)
		return dev_err_probe(dev, ret, "M9 triangle selftest failed\n");
	ret = m9_requester_selftest();
	if (ret)
		return dev_err_probe(dev, ret, "M9 requester selftest failed\n");
	ret = m9_restore_selftest();
	if (ret)
		return dev_err_probe(dev, ret, "M9 restore selftest failed\n");
	ret = drmm_mutex_init(&mdev->drm, &mdev->transport.lock);
	if (ret)
		return ret;
	mdev->transport.fence_context = dma_fence_context_alloc(1);
	/* m9x865 seeds both tracker domains at 0x10.  m9x708/m9x604
	 * increment their tracker before emitting the first 0x20 token. */
	mdev->transport.next_fence = 0x20;
	mdev->transport.requester_cpu = dmam_alloc_coherent(dev, PAGE_SIZE,
						      &mdev->transport.requester_bus,
						      GFP_KERNEL | __GFP_ZERO);
	mdev->transport.command_cpu = dmam_alloc_coherent(dev, command_bytes,
						    &mdev->transport.command_bus,
						    GFP_KERNEL | __GFP_ZERO);
	mdev->transport.fence_cpu =
		dmam_alloc_coherent(dev, PAGE_SIZE, &mdev->transport.fence_bus,
				    GFP_KERNEL | __GFP_ZERO);
	if (!mdev->transport.requester_cpu || !mdev->transport.command_cpu ||
	    !mdev->transport.fence_cpu)
		return -ENOMEM;
	if (upper_32_bits(mdev->transport.requester_bus) ||
	    upper_32_bits(mdev->transport.command_bus) ||
	    mdev->transport.command_bus > U32_MAX - command_bytes ||
	    upper_32_bits(mdev->transport.fence_bus))
		return -ERANGE;
	memset32(mdev->transport.command_cpu, 0x45454545,
		 command_bytes / sizeof(u32));
	memset32(mdev->transport.requester_cpu, 0x45454545,
		 PAGE_SIZE / sizeof(u32));
	memset32(mdev->transport.fence_cpu, U32_MAX, PAGE_SIZE / sizeof(u32));
	WRITE_ONCE(((u32 *)mdev->transport.fence_cpu)[0], 0x10);
	WRITE_ONCE(((u32 *)mdev->transport.fence_cpu)[1], 0x10);
	dev_info(dev,
		 "M9 clean-room transport allocated; hardware submission %s\n",
		 m9_render_diag ? "requires explicit NOOP_FENCE ioctl" : "disabled");
	return 0;
}

void m9x_fini(struct m9x_device *mdev)
{
	struct drm_gem_vram_object *color;
	struct m9x_fence *fence;

	mutex_lock(&mdev->transport.lock);
	mdev->transport.wedged = true;
	/* Stop host DMA before devres releases any reachable allocation. */
	pci_clear_master(mdev->pdev);
	mdev->transport.bus_master_enabled = false;
	mdev->transport.transport_armed = false;
	fence = mdev->transport.quarantined_fence;
	mdev->transport.quarantined_fence = NULL;
	color = mdev->transport.quarantined_color;
	mdev->transport.quarantined_color = NULL;
	mutex_unlock(&mdev->transport.lock);
	m9x_triangle_fini(mdev);
	if (fence) {
		dma_fence_set_error(&fence->base, -EIO);
		dma_fence_signal(&fence->base);
		dma_fence_put(&fence->base);
	}
	if (color) {
		m9x_vram_unpin(color);
		drm_gem_vram_put(color);
	}
}
