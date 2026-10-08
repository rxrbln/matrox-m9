// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2026 René Rebe <rene@exactco.de> */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/iosys-map.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>

#include <drm/drm_drv.h>
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

/* base is the allocation's first member. The core default release performs
 * kfree_rcu; omitting custom release/wait also lets signaled fences detach
 * driver ops before a dma_resv-held fence outlives this module. */
static const struct dma_fence_ops m9_fence_ops = {
	.get_driver_name = m9_fence_get_driver_name,
	.get_timeline_name = m9_fence_get_timeline_name,
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
	if (transport->faa_virtual_primary) {
		dev_warn_ratelimited(&mdev->pdev->dev,
			"M9 ordinary requester arm refused: errno=%d faa_virtual=%u faa_source=%u armed=%u generation=%u submitted=%#llx completed=%#llx\n",
			-EACCES, transport->faa_virtual_primary,
			transport->faa_source_context, transport->transport_armed,
			transport->generation,
			(unsigned long long)transport->submitted_point,
			(unsigned long long)transport->completed_point);
		return -EACCES;
	}
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

/* Writeback polls before the wait sleeps; about a hundred microseconds. */
static unsigned int m9_fence_spins = 20000;
module_param_named(fence_spins, m9_fence_spins, uint, 0644);
MODULE_PARM_DESC(fence_spins, "writeback polls before the fence wait sleeps");
#define M9_FENCE_SPIN_LIMIT m9_fence_spins

/*
 * Wait for the writeback to reach a target.  The word lands in system memory,
 * so poll it directly for about a hundred microseconds before sleeping: a job
 * that retires in tens of microseconds used to wait out a whole
 * usleep_range() interval, and the register read that checks for a dead
 * engine cost a bus round trip on every pass.
 */
/*
 * Where the engine stopped, in the command image we submitted.
 *
 * 0x400 is written once per slot with the slot's own page-aligned base, but
 * reads back as the fetch pointer -- which is why a stalled device shows
 * 0x400 and 0x404 mismatched.  Subtracting the slot base turns it into a byte
 * offset into the dwords this driver wrote, so a timeout names the packet the
 * engine died on rather than leaving it to hand arithmetic on two addresses.
 * The surrounding dwords are printed straight, for tools/m9-command-decode.py.
 */
static void m9_report_stall_point(struct m9x_device *mdev,
				  const struct m9x_transport *transport)
{
	u32 head = m9x_read(mdev, M9_REG_COMMAND_BASE) & ~(u32)M9_REQUESTER_DMA_TAG;
	u32 base = lower_32_bits(m9_command_slot_bus(transport));
	const u32 *slot = m9_command_slot_cpu(transport);
	unsigned int i, first, last, off;

	if (head < base || head - base >= PARHELIA_M9_COMMAND_BYTES) {
		dev_err(&mdev->pdev->dev,
			"M9 stall point: fetch %#x outside slot %#x+%u\n",
			head, base, (unsigned int)PARHELIA_M9_COMMAND_BYTES);
		return;
	}
	off = (head - base) / sizeof(u32);
	first = off > 8 ? off - 8 : 0;
	last = min(off + 4, (unsigned int)(transport->command_offset /
					   sizeof(u32)));
	dev_err(&mdev->pdev->dev,
		"M9 stall point: dword %u of %u submitted (byte %#x of slot)\n",
		off, (unsigned int)(transport->command_offset / sizeof(u32)),
		head - base);
	for (i = first; i < last; i++)
		dev_err(&mdev->pdev->dev, "M9 stall  %c[%4u] %08x\n",
			i == off ? '>' : ' ', i, slot[i]);
}

static int m9_fence_wait_locked(struct m9x_device *mdev, u32 target)
{
	struct m9x_transport *transport = &mdev->transport;
	u32 *writeback = transport->fence_cpu;
	unsigned long deadline;
	u32 raw;

	lockdep_assert_held(&transport->lock);
	deadline = jiffies + msecs_to_jiffies(max(m9_fence_timeout_ms, 1u));
	/*
	 * The writeback word lands in system memory, so poll it directly for a
	 * while before sleeping: a job that retires in tens of microseconds
	 * used to wait out a whole usleep_range() interval, and the register
	 * read that checked for a dead engine cost a bus round trip on every
	 * pass.  Both now belong to the slow path.
	 */
	for (unsigned int spins = 0;;) {
		raw = READ_ONCE(writeback[0]);
		if (m9_fence_reached(raw, target)) {
			dma_rmb();
			break;
		}
		if (++spins < M9_FENCE_SPIN_LIMIT) {
			cpu_relax();
			continue;
		}
		spins = 0;
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
			m9_report_stall_point(mdev, transport);
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
		usleep_range(10, 30);
	}
	return 0;
}

/*
 * Retire a posted job: wait for it, then release the objects it was reading.
 * Every path that touches the shared command image, the engine state or an
 * object the engine may still be reading goes through here first.
 */
/* Four outstanding jobs occupy far less than half of the 32-bit timeline.
 * Extend an observed token relative to the last genuinely completed point;
 * reject stale/future words instead of mistaking a wrapped token for progress.
 * Intermediate E3 tokens may retire an older final point, never themselves
 * become completion points. */
static void m9_queue_observe_locked(struct m9x_device *mdev)
{
	struct m9x_transport *t = &mdev->transport;
	u64 span, observed;
	u32 delta;

	lockdep_assert_held(&t->lock);
	if (!t->queue_count || t->wedged)
		return;
	span = t->submitted_point - t->completed_point;
	delta = (READ_ONCE(((u32 *)t->fence_cpu)[0]) & ~0xfu) -
		(u32)t->completed_point;
	if (span >= (1ULL << 31) || delta > span)
		return;
	observed = t->completed_point + delta;
	dma_rmb();
	while (t->queue_count) {
		struct m9_queued_job *job = &t->queue[t->queue_head];
		struct m9_retired *retired = job->retire;

		if (job->point > observed)
			break;
		t->completed_point = job->point;
		dma_fence_signal(&job->fence->base);
		dma_fence_put(&job->fence->base);
		retired->bos = job->bos;
		retired->pinned = job->pinned;
		retired->count = job->count;
		llist_add(&retired->node, &t->async_retired);
		schedule_work(&t->async_release);
		memset(job, 0, sizeof(*job));
		t->queue_head = (t->queue_head + 1) % M9_QUEUE_SLOTS;
		t->queue_count--;
	}
}

static int m9_queue_wait_head_locked(struct m9x_device *mdev)
{
	struct m9x_transport *t = &mdev->transport;
	u64 owner, start = ktime_get_ns();

	lockdep_assert_held(&t->lock);
	if (t->wedged)
		return -EIO;
	if (!t->queue_count)
		return 0;
	owner = t->queue[t->queue_head].point;
	for (unsigned int spins = 0;;) {
		m9_queue_observe_locked(mdev);
		if (!t->queue_count || t->queue[t->queue_head].point != owner)
			return 0;
		if (++spins < M9_FENCE_SPIN_LIMIT) {
			cpu_relax();
			continue;
		}
		spins = 0;
		if (ktime_get_ns() - start >=
		    (u64)max(m9_fence_timeout_ms, 1u) * NSEC_PER_MSEC) {
			t->wedged = true;
			t->generation++;
			dev_err(&mdev->pdev->dev,
				"M9 queued completion timeout: target=%#x writeback=%#x\n",
				t->queue[t->queue_head].target,
				READ_ONCE(((u32 *)t->fence_cpu)[0]));
			return -ETIMEDOUT;
		}
		usleep_range(10, 30);
	}
}

/* Legacy ownership drains separately so opted-in submits can leave the FIFO
 * running, while diagnostics/translation edits still drain every reader. */
static int m9_drain_legacy_locked(struct m9x_device *mdev)
{
	struct m9x_transport *transport = &mdev->transport;
	struct m9_retired *retired;
	int ret;

	lockdep_assert_held(&transport->lock);
	if (!transport->async_pending)
		return 0;
	if (transport->wedged)
		return -EIO;
	ret = m9_fence_wait_locked(mdev, transport->async_target);
	/* A timeout does not stop DMA. Keep the outstanding refs and pins
	 * quarantined until fini disables bus mastering, or this owner really
	 * retires. Never schedule object release merely because a wait failed. */
	if (ret)
		return ret;
	transport->async_pending = false;
	transport->completed_point = transport->async_point;
	if (transport->async_fence) {
		dma_fence_signal(&transport->async_fence->base);
		dma_fence_put(&transport->async_fence->base);
		transport->async_fence = NULL;
	}
	if (!transport->async_bos && !transport->async_retire)
		return ret;
	/*
	 * The job has retired, so its objects are free, but unpinning and
	 * putting one takes this lock again: hand them to the worker.
	 */
	retired = transport->async_retire;
	/* Kernel-only old owners can have no preallocated retirement record. */
	if (!retired)
		retired = kmalloc(sizeof(*retired), GFP_KERNEL);
	if (retired) {
		retired->bos = transport->async_bos;
		retired->pinned = transport->async_pinned;
		retired->count = transport->async_count;
		llist_add(&retired->node, &transport->async_retired);
		schedule_work(&transport->async_release);
	}
	transport->async_bos = NULL;
	transport->async_pinned = NULL;
	transport->async_count = 0;
	transport->async_retire = NULL;
	return ret;
}

int m9x_transport_drain_locked(struct m9x_device *mdev)
{
	struct m9x_transport *t = &mdev->transport;
	int ret;

	lockdep_assert_held(&t->lock);
	while (t->queue_count) {
		ret = m9_queue_wait_head_locked(mdev);
		if (ret)
			return ret;
	}
	return m9_drain_legacy_locked(mdev);
}

int m9x_transport_queue_room_locked(struct m9x_device *mdev, u32 *slot)
{
	struct m9x_transport *t = &mdev->transport;
	u32 used = 0;
	int ret;

	lockdep_assert_held(&t->lock);
	ret = m9_drain_legacy_locked(mdev);
	if (ret)
		return ret;
	if (t->wedged)
		return -EIO;
	m9_queue_observe_locked(mdev);
	if (t->queue_count == M9_QUEUE_SLOTS) {
		ret = m9_queue_wait_head_locked(mdev);
		if (ret)
			return ret;
	}
	for (u32 i = 0; i < t->queue_count; i++)
		used |= 1u << t->queue[(t->queue_head + i) % M9_QUEUE_SLOTS].secondary_slot;
	for (*slot = 0; *slot < M9_QUEUE_SLOTS; (*slot)++)
		if (!(used & (1u << *slot)))
			return 0;
	return -EBUSY;
}

void m9x_transport_queue_commit_locked(struct m9x_device *mdev,
				      const struct m9_queued_job *job)
{
	struct m9x_transport *t = &mdev->transport;
	u32 tail = (t->queue_head + t->queue_count) % M9_QUEUE_SLOTS;

	lockdep_assert_held(&t->lock);
	t->queue[tail] = *job;
	t->queue_count++;
	if (t->queue_count > t->queue_peak) {
		t->queue_peak = t->queue_count;
		dev_info(&mdev->pdev->dev, "M9 queued submit high-water: %u jobs\n",
			 t->queue_peak);
	}
	if (!t->wedged)
		schedule_work(&t->async_complete);
}

/* Signal the last user job even when there is no following submit/readback.
 * Observe the real writeback without holding transport while sleeping. This
 * worker takes no BO reservation: releasing refs/pins stays in async_release. */
static void m9x_transport_complete(struct work_struct *work)
{
	struct m9x_transport *t = container_of(work, struct m9x_transport,
						async_complete);
	struct m9x_device *mdev = container_of(t, struct m9x_device, transport);
	u64 owner = 0, start = 0;
	int idx;

	if (!drm_dev_enter(&mdev->drm, &idx))
		return;
	for (;;) {
		bool done;

		mutex_lock(&t->lock);
		done = t->async_stop || (!t->async_pending && !t->queue_count) || t->wedged;
		if (!done) {
			u64 pending;

			m9_queue_observe_locked(mdev);
			pending = t->queue_count ? t->queue[t->queue_head].point :
				t->async_pending ? t->async_point : 0;
			if (owner != pending) {
				owner = pending;
				start = ktime_get_ns();
			}
			if (!pending) {
				done = true;
			} else if (t->async_pending &&
				   m9_fence_reached(READ_ONCE(((u32 *)t->fence_cpu)[0]),
					    t->async_target)) {
				dma_rmb();
				m9x_transport_drain_locked(mdev);
				done = true;
			} else if (ktime_get_ns() - start >=
				   (u64)max(m9_fence_timeout_ms, 1u) * NSEC_PER_MSEC) {
				/* Failed progress is not completion. Retain fence/BO
				 * quarantine until fini stops host DMA. */
				t->wedged = true;
				t->generation++;
				dev_err(&mdev->pdev->dev,
					"M9 async completion timeout: target=%#x writeback=%#x\n",
					t->queue_count ? t->queue[t->queue_head].target : t->async_target,
					READ_ONCE(((u32 *)t->fence_cpu)[0]));
				done = true;
			}
		}
		mutex_unlock(&t->lock);
		if (done)
			break;
		usleep_range(10, 30);
	}
	drm_dev_exit(idx);
}

void m9x_transport_release_retired(struct work_struct *work)
{
	struct m9x_transport *transport =
		container_of(work, struct m9x_transport, async_release);
	struct llist_node *nodes = llist_del_all(&transport->async_retired);
	struct m9_retired *retired, *next;

	llist_for_each_entry_safe(retired, next, nodes, node) {
		u32 i;

		for (i = 0; i < retired->count; i++) {
			if (!retired->bos[i])
				continue;
			if (retired->pinned && retired->pinned[i])
				m9x_gem_vram_unpin(retired->bos[i]);
			drm_gem_object_put(retired->bos[i]);
		}
		kfree(retired->bos);
		kfree(retired->pinned);
		kfree(retired);
	}
}

static void m9_transport_publish_end(struct m9x_device *mdev, u32 end)
{
	m9x_write(mdev, M9_REG_COMMAND_END, end);
	m9x_read(mdev, M9_REG_COMMAND_END);
}

/* Where an ordinary job's primary words go and which END publishes them. */
struct m9_primary_place {
	u8 *cpu;
	u32 offset, end;
};

/* The current DMA32 staging slot; only a job that does not fit switches the
 * slot, after the prior interval retired, and rebases the requester's 0x400.
 */
static int m9_primary_place_staging_locked(struct m9x_device *mdev, u32 bytes,
					   struct m9_primary_place *place)
{
	struct m9x_transport *transport = &mdev->transport;
	dma_addr_t start_bus;
	u32 command_offset;
	int ret;

	ret = m9x_transport_arm_locked(mdev);
	if (ret)
		return ret;

	command_offset = transport->command_offset;
	if (command_offset > PARHELIA_M9_COMMAND_BYTES - bytes) {
		ret = m9x_transport_drain_locked(mdev);
		if (ret)
			return ret;
		m9_advance_command_slot(transport);
		command_offset = 0;
		m9x_write(mdev, M9_REG_COMMAND_BASE,
			  lower_32_bits(m9_command_slot_bus(transport)) |
			  M9_COMMAND_SYSTEM_TAG);
		m9x_read(mdev, M9_REG_COMMAND_BASE);
	}
	start_bus = m9_command_slot_bus(transport) + command_offset;
	if (upper_32_bits(start_bus) ||
	    check_add_overflow(lower_32_bits(start_bus), bytes, &place->end))
		return -ERANGE;
	place->cpu = (u8 *)m9_command_slot_cpu(transport) + command_offset;
	place->offset = command_offset;
	return 0;
}

static int m9_primary_place_locked(struct m9x_device *mdev, u32 bytes,
				   struct m9_primary_place *place)
{
	return m9_primary_place_staging_locked(mdev, bytes, place);
}

static void m9_primary_place_commit_locked(struct m9x_device *mdev,
					   const struct m9_primary_place *place,
					   u32 bytes)
{
	mdev->transport.command_offset = place->offset + bytes;
}

static int m9_submit_kernel_stream_common(struct m9x_device *mdev,
				    const u32 *commands, u32 dwords,
				    u32 target, u64 *point, bool *doorbell_posted,
				    bool async, bool queued)
{
	struct m9x_transport *transport = &mdev->transport;
	struct m9_primary_place place;
	u32 *writeback = transport->fence_cpu;
	u32 raw;
	u64 extended, queued_point = 0;
	int ret;

	lockdep_assert_held(&transport->lock);
	if (doorbell_posted)
		*doorbell_posted = false;
	/* The command image and the engine state are shared, so a job posted
	 * earlier has to retire before this one is built. */
	ret = queued ? m9_drain_legacy_locked(mdev) :
		m9x_transport_drain_locked(mdev);
	if (ret)
		return ret;
	if (!commands || !dwords ||
	    dwords > PARHELIA_M9_COMMAND_BYTES / sizeof(u32))
		return -EINVAL;
	if (transport->wedged)
		return -EIO;
	if (queued) {
		queued_point = ((u64)(transport->fence_epoch +
				 (target < transport->last_fence)) << 32) | target;
		if (transport->queue_count >= M9_QUEUE_SLOTS)
			return -EBUSY;
		if ((target & 0xf) || queued_point <= transport->submitted_point ||
		    queued_point - transport->completed_point >= (1ULL << 31))
			return -ERANGE;
	}
	ret = m9_primary_place_locked(mdev, dwords * sizeof(u32), &place);
	if (ret)
		return ret;
	memcpy(place.cpu, commands, dwords * sizeof(u32));
	dma_wmb();

	m9_transport_publish_end(mdev, place.end);
	if (doorbell_posted)
		*doorbell_posted = true;
	m9_primary_place_commit_locked(mdev, &place, dwords * sizeof(u32));
	if (async) {
		/* The next drain retires it; the point is the target it
		 * reaches. */
		if (!queued) {
			transport->async_pending = true;
			transport->async_target = target;
		}
		raw = target;
	} else {
		ret = m9_fence_wait_locked(mdev, target);
		if (ret)
			return ret;
		raw = READ_ONCE(writeback[0]);
	}
	/* A queued issue is not completion, even when the FIFO was empty. */
	if (queued) {
		transport->fence_epoch = queued_point >> 32;
		transport->last_fence = target;
		extended = queued_point;
		transport->submitted_point = extended;
	} else {
		extended = m9_extend_fence(transport, raw);
	}
	if (async && !queued)
		transport->async_point = extended;
	if (point)
		*point = extended;
	return 0;
}

int m9x_submit_kernel_stream_locked(struct m9x_device *mdev,
				    const u32 *commands, u32 dwords, u32 target,
				    u64 *point, bool *doorbell_posted, bool async)
{
	return m9_submit_kernel_stream_common(mdev, commands, dwords, target,
					     point, doorbell_posted, async, false);
}

/* Each of @pages pages from @base must map @dma + offset with @valid.
 * On failure @bad_page and, for -ESTALE, @bad_pte name the first mismatch.
 */
int m9x_system_pages_mapped(struct m9x_device *mdev, u32 base, dma_addr_t dma,
			    u32 pages, u32 valid, u32 *bad_page, u32 *bad_pte)
{
	u32 page, pte;
	int ret;

	for (page = 0; page < pages; page++) {
		ret = m9x_address_read_pte(mdev, base + page * PAGE_SIZE, &pte);
		if (!ret && pte != (m9x_address_pte_word(dma + page * PAGE_SIZE) |
				    valid)) {
			ret = -ESTALE;
			if (bad_pte)
				*bad_pte = pte;
		}
		if (ret) {
			if (bad_page)
				*bad_page = page;
			return ret;
		}
	}
	return 0;
}

static int m9_transport_finish_wait_locked(struct m9x_device *mdev, u32 target,
					   u64 *point)
{
	struct m9x_transport *t = &mdev->transport;
	int ret;

	ret = m9_fence_wait_locked(mdev, target);
	if (ret)
		return ret;
	*point = m9_extend_fence(t, READ_ONCE(((u32 *)t->fence_cpu)[0]));
	return 0;
}

/* A failure after a posted doorbell wedges the transport and quarantines
 * @fence until DMA stop; otherwise @fence (with @ret) is signalled and put.
 */
void m9x_transport_fence_finish_locked(struct m9x_device *mdev,
				       struct m9x_fence *fence, int ret,
				       bool posted)
{
	struct m9x_transport *t = &mdev->transport;

	if (fence && ret)
		dma_fence_set_error(&fence->base, ret);
	if (ret && posted) {
		t->wedged = true;
		t->generation++;
		WARN_ON(t->quarantined_fence);
		t->quarantined_fence = fence;
		return;
	}
	if (fence) {
		dma_fence_signal(&fence->base);
		dma_fence_put(&fence->base);
	}
}

/* Actual MxMD 23a09 selects the retained flags30/B primary GPUVA|1;
 * 2461e posts its untagged end-exclusive GPUVA. This private synchronous
 * request consumes the already published owner, without a physical clone.
 * The caller retains that allocation and mapping through real retirement
 * or, on failure after publication, through bus-master teardown. */
int m9x_submit_faa_virtual_primary_locked(struct m9x_device *mdev,
				 const struct drm_mm_node *node, dma_addr_t dma,
				 u32 bytes, u32 target, u64 *point,
				 bool *doorbell_posted)
{
	struct m9x_transport *t = &mdev->transport;
	u32 base, end;
	int ret;

	lockdep_assert_held(&t->lock);
	if (doorbell_posted)
		*doorbell_posted = false;
	if ((!m9x_faa_diag_enabled() && !m9x_faa_graph_active(mdev)) ||
	    !t->faa || t->faa_virtual_primary ||
	    !t->transport_armed || !t->fence_cpu || !point || !node ||
	    !drm_mm_node_allocated(node) || !bytes || !IS_ALIGNED(bytes, 4) ||
	    bytes > PARHELIA_M9_COMMAND_BYTES || bytes > node->size ||
	    !IS_ALIGNED(node->start, PAGE_SIZE) ||
	    !IS_ALIGNED(node->size, PAGE_SIZE) || node->size > 0x50000 ||
	    node->start < 0x40000000 || node->start >= 0x41000000 ||
	    node->size > 0x41000000 - node->start ||
	    upper_32_bits(dma) || !IS_ALIGNED(dma, PAGE_SIZE) ||
	    (u64)dma + node->size > (u64)U32_MAX + 1 || !target || (target & 15))
		return -EINVAL;
	if (t->wedged)
		return -EIO;
	ret = m9x_transport_drain_locked(mdev);
	if (ret)
		return ret;
	if (m9_fence_reached(READ_ONCE(((u32 *)t->fence_cpu)[0]), target))
		return -EAGAIN;
	base = node->start;
	if (check_add_overflow(base, bytes, &end))
		return -ERANGE;
	/* Reject an uncommitted or differently cached mapping before touching
	 * requester controls. The allocated node bounds the consumed prefix. */
	ret = m9x_system_pages_mapped(mdev, base, dma,
				      DIV_ROUND_UP(bytes, PAGE_SIZE), 1u,
				      NULL, NULL);
	if (ret)
		return ret;
	/* The flags30 mapping is committed before engine activation. Drain
	 * WC command/data stores before MMIO publication: dma_wmb() is only
	 * a compiler barrier on x86; writel() does not emit an SFENCE. */
	wmb();
	t->faa_virtual_primary = true;
	m9x_write(mdev, M9_REG_COMMAND_BASE, base | 1u);
	m9x_read(mdev, M9_REG_COMMAND_BASE);
	m9_transport_publish_end(mdev, end);
	if (doorbell_posted)
		*doorbell_posted = true;
	dev_info(&mdev->pdev->dev,
		 "M9 FAA virtual primary: base=%#x end=%#x bytes=%u policy=b\n",
		 base | 1u, end, bytes);
	return m9_transport_finish_wait_locked(mdev, target, point);
}

/* A bounded diagnostic preserves actual source retirement boundaries while
 * extending the same retained primary. It does not replay captured MMIO or
 * claim equivalence of intervening mapping operations. Check every endpoint
 * and consumed page before the first doorbell; failure retains posted owners. */
int m9x_submit_faa_virtual_phases_locked(struct m9x_device *mdev,
				const struct drm_mm_node *node, dma_addr_t dma,
				const struct m9x_faa_submit_phase *phases,
				u32 count, u64 *point, bool *doorbell_posted,
				int (*retired)(struct m9x_device *, u32, u64, void *),
				void *retired_data)
{
	u32 i, end;
	int ret;

	lockdep_assert_held(&mdev->transport.lock);
	if (doorbell_posted)
		*doorbell_posted = false;
	/* Each strictly increasing, DWORD-aligned endpoint consumes at least
	 * one command word. Bound the count by transport capacity, not by how
	 * many source waits a particular diagnostic has reached so far. */
	if (!node || !phases || count < 2 ||
	    count > PARHELIA_M9_COMMAND_BYTES / sizeof(u32) || !point ||
	    !drm_mm_node_allocated(node) || node->start > U32_MAX ||
	    upper_32_bits(dma) || !IS_ALIGNED(dma, PAGE_SIZE))
		return -EINVAL;
	for (i = 0; i < count; i++) {
		if (!phases[i].bytes || !IS_ALIGNED(phases[i].bytes, 4) ||
		    phases[i].bytes > node->size ||
		    phases[i].bytes > PARHELIA_M9_COMMAND_BYTES ||
		    !phases[i].target || (phases[i].target & 15) ||
		    check_add_overflow((u32)node->start, phases[i].bytes, &end) ||
		    (i && (phases[i].bytes <= phases[i - 1].bytes ||
			   phases[i].target <= phases[i - 1].target)))
			return -EINVAL;
	}
	if ((u64)dma + node->size > (u64)U32_MAX + 1)
		return -EINVAL;
	ret = m9x_system_pages_mapped(mdev, (u32)node->start, dma,
				      DIV_ROUND_UP(phases[count - 1].bytes,
						   PAGE_SIZE), 1u, NULL, NULL);
	if (ret)
		return ret;
	ret = m9x_submit_faa_virtual_primary_locked(mdev, node, dma,
			phases[0].bytes, phases[0].target, point, doorbell_posted);
	if (ret)
		return ret;
	if (retired) {
		ret = retired(mdev, 0, *point, retired_data);
		if (ret)
			return ret;
	}
	for (i = 1; i < count; i++) {
		/* The prior actual wait returned; neither CPU seeds nor a later
		 * phase's desired counter are treated as GPU completion. */
		wmb();
		m9_transport_publish_end(mdev, (u32)node->start + phases[i].bytes);
		ret = m9_transport_finish_wait_locked(mdev, phases[i].target, point);
		if (ret)
			return ret;
		if (retired) {
			ret = retired(mdev, i, *point, retired_data);
			if (ret)
				return ret;
		}
		dev_info(&mdev->pdev->dev,
			 "M9 FAA source phase retired: phase=%u bytes=%u target=%#x actual=%#llx\n",
			 i, phases[i].bytes, phases[i].target, *point);
	}
	return 0;
}

/* Continue the already-owned flags30/B primary after a genuine CPU caller
 * boundary. The first issue installed COMMAND_BASE and retired old_bytes;
 * repeating that issue would reset the requester instead of extending it.
 * The caller owns the retained graph, color fence and transport lock. */
int m9x_validate_faa_virtual_resume_locked(struct m9x_device *mdev,
				 const struct drm_mm_node *node, dma_addr_t dma,
				 u32 old_bytes, u32 new_bytes, u32 target)
{
	struct m9x_transport *t = &mdev->transport;
	u32 end;

	lockdep_assert_held(&t->lock);
	if (!m9x_faa_diag_enabled() || !t->faa || !t->faa_virtual_primary ||
	    !t->transport_armed || !t->fence_cpu || !node ||
	    !drm_mm_node_allocated(node) || node->start > U32_MAX ||
	    !IS_ALIGNED(node->start, PAGE_SIZE) ||
	    !IS_ALIGNED(dma, PAGE_SIZE) || upper_32_bits(dma) ||
	    !old_bytes || old_bytes >= new_bytes || !IS_ALIGNED(old_bytes, 4) ||
	    !IS_ALIGNED(new_bytes, 4) || new_bytes > PARHELIA_M9_COMMAND_BYTES ||
	    new_bytes > node->size || node->size > 0x50000 ||
	    !target || !IS_ALIGNED(target, 16) ||
	    check_add_overflow((u32)node->start, new_bytes, &end) ||
	    (u64)dma + node->size > (u64)U32_MAX + 1)
		return -EINVAL;
	if (t->wedged)
		return -EIO;
	if (t->async_pending || t->queue_count ||
	    m9_fence_reached(READ_ONCE(((u32 *)t->fence_cpu)[0]), target))
		return -EAGAIN;
	/* The suffix can cross a page boundary after the Lock return. Demand
	 * that the same coherent command owner still maps every consumed page. */
	return m9x_system_pages_mapped(mdev, (u32)node->start, dma,
				       DIV_ROUND_UP(new_bytes, PAGE_SIZE), 1u,
				       NULL, NULL);
}

int m9x_extend_faa_virtual_primary_locked(struct m9x_device *mdev,
				 const struct drm_mm_node *node, dma_addr_t dma,
				 u32 old_bytes, u32 new_bytes, u32 target,
				 u64 *point, bool *doorbell_posted)
{
	u32 end;
	int ret;

	lockdep_assert_held(&mdev->transport.lock);
	if (doorbell_posted)
		*doorbell_posted = false;
	if (!point)
		return -EINVAL;
	ret = m9x_validate_faa_virtual_resume_locked(mdev, node, dma,
						 old_bytes, new_bytes, target);
	if (ret)
		return ret;
	end = (u32)node->start + new_bytes;
	/* The primary and consumed data/secondary owners can be WC. Order
	 * all earlier CPU stores before exposing this range to the GPU. */
	wmb();
	m9_transport_publish_end(mdev, end);
	if (doorbell_posted)
		*doorbell_posted = true;
	return m9_transport_finish_wait_locked(mdev, target, point);
}

/* A retained graph can post the same END twice before any new fence write.
 * This pure shape check leaves the stage ordering to the native producer:
 * equality is admitted only for an explicitly repeated endpoint, and a
 * target is never confused with the CPU's previous INIT observation. */
static int m9_faa_graph_end_shape(const struct drm_mm_node *node,
				 dma_addr_t dma, u32 old_bytes, u32 new_bytes,
				 bool repeat, u32 target)
{
	u32 end;

	if (!node || !drm_mm_node_allocated(node) || node->start > U32_MAX ||
	    !IS_ALIGNED(node->start, PAGE_SIZE) ||
	    !IS_ALIGNED(node->size, PAGE_SIZE) ||
	    node->size > 0x50000 || upper_32_bits(dma) ||
	    !IS_ALIGNED(dma, PAGE_SIZE) ||
	    (u64)dma + node->size > (u64)U32_MAX + 1 ||
	    !IS_ALIGNED(old_bytes | new_bytes, 4) ||
	    new_bytes > node->size ||
	    /* Retained FAA pools use their authenticated owner extent; the
	     * ordinary requester's 32 KiB staging-slot limit does not apply. */
	    (repeat ? new_bytes != old_bytes : new_bytes <= old_bytes) ||
	    (target && (!IS_ALIGNED(target, 16) || target > U32_MAX - 0x10)) ||
	    check_add_overflow((u32)node->start, new_bytes, &end))
		return -EINVAL;
	return 0;
}

/* Continue the same flags30/B COMMAND_BASE namespace.  This function never
 * writes COMMAND_BASE, seeds a counter, or treats an END post as completion.
 * The caller holds TRANSPORT and the retained graph's owner reservations;
 * after any posted failure those owners must remain quarantined.  Only the
 * native graph producer may call this helper, after its identity gate. */
int m9x_post_faa_graph_end_locked(struct m9x_device *mdev,
				 const struct drm_mm_node *node, dma_addr_t dma,
				 u32 old_bytes, u32 new_bytes, bool repeat,
				 u32 target, u64 *point, bool *doorbell_posted)
{
	struct m9x_transport *t = &mdev->transport;
	u32 page, pte, end;
	int ret;

	lockdep_assert_held(&t->lock);
	if (doorbell_posted)
		*doorbell_posted = false;
	if (point)
		*point = 0;
	if (!point || !doorbell_posted)
		return -EINVAL;
	if (!m9x_faa_graph_profile_enabled())
		return -EOPNOTSUPP;
	if (!m9x_faa_graph_active(mdev) || m9x_faa_graph_preparing(mdev) ||
	    !t->faa_virtual_primary || !t->faa_source_context ||
	    !t->transport_armed || !t->bus_master_enabled || !t->fence_cpu ||
	    t->async_stop || t->async_pending || t->queue_count)
		return -EBUSY;
	if (t->wedged)
		return -EIO;
	if (!old_bytes && !m9x_faa_graph_empty_primary_locked(mdev, node, dma))
		return -ESTALE;
	ret = m9_faa_graph_end_shape(node, dma, old_bytes, new_bytes,
				     repeat, target);
	if (ret) {
		dev_info(&mdev->pdev->dev,
			 "M9 FAA END refused: shape old=%u new=%u capacity=%llu repeat=%u target=%#x error=%d\n",
			 old_bytes, new_bytes, (unsigned long long)node->size, repeat, target, ret);
		return ret;
	}
	if (target && (target < t->next_fence ||
	    m9_fence_reached(READ_ONCE(((u32 *)t->fence_cpu)[0]), target)))
		return -EAGAIN;
	/* A new suffix can cross a mapped page.  Verify every consumed page
	 * still names the same coherent primary before the first doorbell. */
	ret = m9x_system_pages_mapped(mdev, (u32)node->start, dma,
				      DIV_ROUND_UP(new_bytes, PAGE_SIZE), 1u,
				      &page, &pte);
	if (ret == -ESTALE) {
		dev_info(&mdev->pdev->dev,
			 "M9 FAA END refused: PTE base=%#llx page=%u actual=%#x expected=%#x\n",
			 (unsigned long long)node->start, page, pte,
			 m9x_address_pte_word(dma + page * PAGE_SIZE) | 1u);
		return ret;
	}
	if (ret) {
		dev_info(&mdev->pdev->dev,
			 "M9 FAA END refused: PTE read base=%#llx page=%u error=%d\n",
			 (unsigned long long)node->start, page, ret);
		return ret;
	}
	end = (u32)node->start + new_bytes;
	/* The primary and consumed data/secondary owners can be WC. Order
	 * all earlier CPU stores before exposing this range to the GPU. */
	wmb();
	m9x_write(mdev, M9_REG_COMMAND_END, end);
	/* Source repeats +64 END without an intervening MMIO read. */
	if (target) {
		u32 readback = m9x_read(mdev, M9_REG_COMMAND_END);

		dev_info(&mdev->pdev->dev,
			 "M9 FAA END post: old=%#x new=%#x readback=%#x target=%#x repeat=%u\n",
			 (u32)node->start + old_bytes, end, readback, target, repeat);
	}
	*doorbell_posted = true;
	if (!target)
		return 0;
	ret = m9_transport_finish_wait_locked(mdev, target, point);
	if (ret)
		return ret;
	t->next_fence = target + 0x10;
	return 0;
}

int m9x_submit_kernel_queue_locked(struct m9x_device *mdev,
				   const u32 *commands, u32 dwords, u32 target,
				   u64 *point, bool *doorbell_posted)
{
	return m9_submit_kernel_stream_common(mdev, commands, dwords, target,
					     point, doorbell_posted, true, true);
}

static u64 m9_extend_fence(struct m9x_transport *m9, u32 raw)
{
	u32 low = raw & ~0xfu;

	if (low < m9->last_fence)
		m9->fence_epoch++;
	m9->last_fence = low;
	m9->submitted_point = ((u64)m9->fence_epoch << 32) | low;
	if (!m9->async_pending && !m9->queue_count)
		m9->completed_point = m9->submitted_point;
	return m9->submitted_point;
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

/* Completion is derived from an observed writeback, never the token merely
 * returned at posting. The transport lock preserves the single pending
 * owner while a wait observes it; no command/requester register is written. */
static int m9_wait_point_locked(struct m9x_device *mdev,
				struct drm_parhelia_m9_wait_point *args)
{
	struct m9x_transport *t = &mdev->transport;
	u64 start = ktime_get_ns();
	u64 limit = min_t(u64, args->timeout_ns,
			 (u64)max(m9_fence_timeout_ms, 1u) * NSEC_PER_MSEC);
	int ret;

	lockdep_assert_held(&t->lock);
	if ((args->point & 0xf) || args->point > t->submitted_point)
		return -EINVAL;
	for (;;) {
		m9_queue_observe_locked(mdev);
		args->completed_point = t->completed_point;
		if (args->point <= t->completed_point)
			return 0;
		if (t->wedged)
			return -EIO;
		if (t->async_pending &&
		    m9_fence_reached(READ_ONCE(((u32 *)t->fence_cpu)[0]),
				     t->async_target)) {
			dma_rmb();
			ret = m9x_transport_drain_locked(mdev);
			if (ret)
				return ret;
			continue;
		}
		if (!limit || ktime_get_ns() - start >= limit)
			return -ETIME;
		if (signal_pending(current))
			return -ERESTARTSYS;
		usleep_range(10, 30);
	}
}

int m9x_wait_point_ioctl(struct drm_device *drm, void *data,
			struct drm_file *file)
{
	struct drm_parhelia_m9_wait_point *args = data;
	struct drm_parhelia_m9_wait_point wait = *args;
	struct m9x_device *mdev = to_m9x(drm);
	u64 start, elapsed, limit;
	int idx, ret;

	if (!m9x_enabled())
		return -ENODEV;
	if (args->flags || args->pad || args->completed_point)
		return -EINVAL;
	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;
	start = ktime_get_ns();
	limit = min_t(u64, args->timeout_ns,
			 (u64)max(m9_fence_timeout_ms, 1u) * NSEC_PER_MSEC);
	/* A zero-time poll must not block behind another submit's drain.
	 * Account contention against the same finite/watchdog wait budget. */
	while (!mutex_trylock(&mdev->transport.lock)) {
		if (!limit || ktime_get_ns() - start >= limit) {
			ret = -ETIME;
			goto out_exit;
		}
		if (signal_pending(current)) {
			ret = -ERESTARTSYS;
			goto out_exit;
		}
		usleep_range(10, 30);
	}
	elapsed = ktime_get_ns() - start;
	wait.timeout_ns = elapsed < limit ? limit - elapsed : 0;
	ret = m9_wait_point_locked(mdev, &wait);
	args->completed_point = wait.completed_point;
	mutex_unlock(&mdev->transport.lock);
out_exit:
	drm_dev_exit(idx);
	return ret;
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
	/* A posted job is retired first: its target is matched exactly, and
	 * this fence would move the writeback past it. */
	m9x_transport_drain_locked(mdev);
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
	ret = m9x_faa_graph_selftest();
	if (ret)
		return dev_err_probe(dev, ret, "M9 FAA graph selftest failed\n");
	ret = m9x_faa_graph_continuation_selftest();
	if (ret)
		return dev_err_probe(dev, ret,
				     "M9 FAA continuation selftest failed\n");
	ret = m9x_triangle_selftest();
	if (ret)
		return dev_err_probe(dev, ret, "M9 triangle selftest failed\n");
	ret = m9x_surface_selftest();
	if (ret)
		return dev_err_probe(dev, ret, "M9 surface selftest failed\n");
	ret = m9_requester_selftest();
	if (ret)
		return dev_err_probe(dev, ret, "M9 requester selftest failed\n");
	ret = m9_restore_selftest();
	if (ret)
		return dev_err_probe(dev, ret, "M9 restore selftest failed\n");
	ret = drmm_mutex_init(&mdev->drm, &mdev->transport.lock);
	if (ret)
		return ret;
	atomic64_set(&mdev->faa_graph_next, 0);
	init_llist_head(&mdev->transport.async_retired);
	INIT_WORK(&mdev->transport.async_release,
		  m9x_transport_release_retired);
	INIT_WORK(&mdev->transport.async_complete, m9x_transport_complete);
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
	struct m9_queued_job queued[M9_QUEUE_SLOTS];
	struct drm_gem_vram_object *color;
	struct m9x_fence *fence;
	struct m9x_fence *user_fence;
	struct m9_retired *retired;
	struct drm_gem_object **bos;
	bool *pinned;
	u32 count, i, queued_count;

	mutex_lock(&mdev->transport.lock);
	mdev->transport.async_stop = true;
	mutex_unlock(&mdev->transport.lock);
	cancel_work_sync(&mdev->transport.async_complete);
	mutex_lock(&mdev->transport.lock);
	/* Retire a posted job and let go of what it was reading before the
	 * engine is stopped. */
	m9x_transport_drain_locked(mdev);
	mdev->transport.wedged = true;
	/* Stop host DMA before devres releases any reachable allocation. */
	pci_clear_master(mdev->pdev);
	mdev->transport.bus_master_enabled = false;
	mdev->transport.transport_armed = false;
	/* Failed user jobs retain their own arrays, not the diagnostic color
	 * quarantine. Host DMA is stopped before either ownership is released. */
	bos = mdev->transport.async_bos;
	pinned = mdev->transport.async_pinned;
	count = mdev->transport.async_count;
	retired = mdev->transport.async_retire;
	user_fence = mdev->transport.async_fence;
	mdev->transport.async_bos = NULL;
	mdev->transport.async_pinned = NULL;
	mdev->transport.async_count = 0;
	mdev->transport.async_pending = false;
	mdev->transport.async_retire = NULL;
	mdev->transport.async_fence = NULL;
	queued_count = mdev->transport.queue_count;
	for (i = 0; i < queued_count; i++) {
		u32 slot = (mdev->transport.queue_head + i) % M9_QUEUE_SLOTS;

		queued[i] = mdev->transport.queue[slot];
		memset(&mdev->transport.queue[slot], 0, sizeof(queued[i]));
	}
	mdev->transport.queue_count = 0;
	fence = mdev->transport.quarantined_fence;
	mdev->transport.quarantined_fence = NULL;
	color = mdev->transport.quarantined_color;
	mdev->transport.quarantined_color = NULL;
	mutex_unlock(&mdev->transport.lock);
	/* A last VRAM put can wait its reservation fences (TTM's allocation
	 * failure fallback). DMA is already stopped, so publish the failure
	 * before releasing an object that carries this very fence. */
	if (user_fence) {
		dma_fence_set_error(&user_fence->base, -EIO);
		dma_fence_signal(&user_fence->base);
		dma_fence_put(&user_fence->base);
	}
	/* Several jobs can share a reservation; signal all of their failures
 * before any last BO put can enter TTM's reservation wait fallback. */
	for (i = 0; i < queued_count; i++) {
		dma_fence_set_error(&queued[i].fence->base, -EIO);
		dma_fence_signal(&queued[i].fence->base);
		dma_fence_put(&queued[i].fence->base);
	}
	for (i = 0; i < queued_count; i++) {
		struct m9_queued_job *job = &queued[i];

		for (u32 j = 0; j < job->count; j++) {
			if (job->pinned[j])
				m9x_gem_vram_unpin(job->bos[j]);
			drm_gem_object_put(job->bos[j]);
		}
		kfree(job->bos);
		kfree(job->pinned);
		kfree(job->retire);
	}
	for (i = 0; bos && i < count; i++) {
		if (pinned && pinned[i])
			m9x_gem_vram_unpin(bos[i]);
		drm_gem_object_put(bos[i]);
	}
	kfree(bos);
	kfree(pinned);
	kfree(retired);
	m9x_faa_fini(mdev);
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
	flush_work(&mdev->transport.async_release);
}
