// SPDX-License-Identifier: GPL-2.0
/*
 * cpu_comm_hw.c — Hardware abstraction for CPU_COMM
 *
 * Register access via io_accessor (MMIO read/write with whitelist),
 * hardware spinlocks via sunxi hwspinlock controller,
 * shared registers mapped to MIPS control MMIO (0x03061024/28),
 * software spinlocks in shared memory.
 *
 * RE source: HAL_SX6/Kernel_Driver/cpu_comm/
 *   - cpu_comm_core.c (io_accessor, known_regs)
 *   - tridsharereg.c (getShareRegbyID)
 *   - tridspinlock.c (hwspinlock, sw spinlock)
 */

#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/hwspinlock.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/interrupt.h>
#include "cpu_comm.h"

/* ── Globals ───────────────────────────────────────────────── */

void *hwlocks[HW_SPINLOCK_COUNT * 2];	/* [0..13]=lock handles, [14..27]=counters */

/*
 * Known register addresses — the only registers io_accessor will touch.
 * From IDA: known_regs @ 0x16350 in cpu_comm_dev.ko
 */
static const u32 known_regs[] = {
	0x03061024,	/* MIPS ShareReg14 — SharedMemAddr */
	0x03061028,	/* MIPS ShareReg13 — SharedMemSize */
	0x03003000,	/* Msgbox channel register */
	0x03003010,	/* Msgbox channel register */
};

static void __iomem *mips_ctrl_base;	/* ioremap of 0x03061000 */
static void __iomem *msgbox_ctrl_base;	/* ioremap of 0x03003000 */

/*
 * ── H713 Msgbox Register Layout ─────────────────────────────
 *
 * The H713 uses a custom Allwinner msgbox with per-user regions
 * (0x400 bytes each). ARM = User 0 @ 0x03003000.
 *
 * Register offsets within ARM user region (verified from stock
 * vmlinux IDA RE + live hardware register dump):
 *
 *   +0x010: MSG_DATA   — FIFO read/write (MIPS→ARM messages)
 *   +0x024: IRQ_EN     — RX IRQ enable (bit pattern 0x55 = all RX)
 *   +0x034: IRQ_EN2    — TX IRQ enable (bit pattern 0xAA = all TX)
 *   +0x050: IRQ_STATUS — RX pending (write-1-to-clear)
 *   +0x060: FIFO_STAT  — Number of messages in FIFO
 *
 * TX to MIPS: write message to msgbox_ctrl_base + 0x20
 * (ARM→MIPS FIFO data register, confirmed working).
 *
 * GIC IRQ: SPI 21 (verified: permanently asserted when FIFO
 * has unread messages; SPI 46 was wrong — never asserts).
 */
/*
 * Verified from stock vmlinux IDA (sunxi_msgbox_irq):
 *   RX: sx_base(mdev, ARM=0) + port*4 + offset
 *   TX: sx_base(mdev, MIPS=1) + port*4 + offset
 * With port=0, ARM_base=0x03003000, MIPS_base=0x03003400:
 */
/*
 * H713 msgbox layout (Stock-RE from sunxi_msgbox_amp.c):
 *
 *   sx_base(mdev, i)  = user bank i  (User0=0x3003000, User1/MIPS=0x3003400,
 *                                     User2=0x3003800)
 *   adj(l, r)         = (l < r) ? r-1 : r   // peer-block idx within bank
 *   USER_STRIDE       = 256
 *
 *   TX (ARM writes to MIPS bank):
 *     reg = sx_base(R=MIPS) + USER_STRIDE * adj(L=ARM, R=MIPS) + offset
 *         = 0x3003400 + 256*0 + offset
 *     port 1 MSG_DATA = 0x3003400 + 0 + 0x70 + 4 = 0x3003474
 *
 *   RX (ARM reads from MIPS bank):
 *     ra  = (R >= L) ? L : L - 1            = 0
 *     reg = sx_base(R=MIPS) + USER_STRIDE * ra + offset
 *         = 0x3003400 + 0 + offset
 *     port 1 FIFO_count = 0x3003400 + 0 + 0x60 + 4 = 0x3003464
 *     port 1 MSG_DATA   = 0x3003400 + 0 + 0x70 + 4 = 0x3003474
 *     IRQ_EN            = 0x3003400 + 0 + 0x20     = 0x3003420
 *     IRQ_STAT/CLR      = 0x3003400 + 0 + 0x24     = 0x3003424
 *
 * Both TX and RX use the same port-1 register address (0x3003474) — the
 * H713 msgbox's per-port FIFO is a single push/pop register in the
 * remote's bank; write = enqueue-to-remote, read = pop-from-remote.
 *
 * Historical bug: we used base[L=ARM]+0x100+offset = 0x3003120/0x3003164
 * — that is either an unused mirror or a different peer-block. The IRQ
 * never asserted because the HW enable bit lives in the MIPS bank.
 *
 * All offsets below are relative to msgbox_ctrl_base (ioremap of 0x3003000,
 * size 0x1000 → covers User0..User2 + headroom).
 */
/*
 * HY310-fix 2026-04-18: RX offsets on ARM bank (User0) sub-block 1.
 * Empirically verified write-protection via devmem: ARM can only write
 * User0 IRQ registers (0x020/0x120). Writes to User1/User2 banks silently
 * dropped. MIPS-→ARM messages arrive at User0 sub-block 1 (per adj(ARM,MIPS)
 * formula in stock IDA + HANDOFF-D verified test: MIPS writes to 0x03003174).
 * TX at 0x474 (User1 sub-block 0) kept — known-good (MIPS FIFO count visibly
 * incremented 0→5→0 per HANDOFF-16).
 */
/*
 * HY310-fix 2026-04-18: RX on User0 sub-block 0 (not sub-block 1).
 * Verified via disasm of display.bin sub_8B121870 (MIPS TX function):
 *   fp = (remote<<10) + (adj(local,remote)<<8) + port*4
 *   adj(v0,v1) = v0 - (v0>=v1 ? 1 : 0)
 * For MIPS(1)→ARM(0) port 1: remote=0, adj(1,0)=0, port=1
 *   fp = 0 + 0 + 4 = 4
 *   data_addr = 0x03003070 + 4 = 0x03003074 (User0 sub-block 0 port 1)
 */
/*
 * 2026-04-21: RX offsets corrected from User0 (+0x000) to User1 (+0x100).
 * Live verification showed MIPS writes ACK to +0x174 (User1 Port1 data),
 * and User1 IRQ_STATUS (+0x124) BIT(2) fires. ARM was listening on User0
 * (+0x074) — hence IRQs 353/354/355 stayed at 0 count forever.
 * MIPS endpoint struct @ 0x8B2543F4 computes write offset via
 *   sub_8B121870: v14 = 0x03003070 + 4*a1[2] + (a1[1]<<10) + (adj<<8)
 * With endpoint {2,0,1,1}: v14 = 0x03003070 + 4 + 0 + 0x100 = 0x03003174.
 */
#define H713_MSGBOX_RX_FIFO	0x164	/* User1 sub-block 0 Port 1 FIFO count */
#define H713_MSGBOX_RX_DATA	0x174	/* User1 sub-block 0 Port 1 FIFO data  */
#define H713_MSGBOX_RX_IRQ_EN	0x120	/* User1 sub-block 0 IRQ enable        */
#define H713_MSGBOX_RX_IRQ_CLR	0x124	/* User1 sub-block 0 IRQ status/W1C    */
#define H713_MSGBOX_TX_DATA	0x874	/* User2 sub-block 0 Port 1 FIFO data (MIPS reads here per IDA sub_8B12156C) */
#define H713_MSGBOX_TX_FIFO	0x864	/* User2 sub-block 0 Port 1 FIFO count */

/*
 * Port 1 RX IRQ bit. Stock-RE formula: RX_IRQ_BIT(port) = BIT(2*port).
 * We listen on Port 1 (MIPS writes ACKs to User0 Port1 @ +0x174), so
 * the matching IRQ-enable/status bit is BIT(2), not BIT(0).
 * Historical bug: was BIT(0) → IRQ was enabled for Port 0 which MIPS
 * never writes to → the GIC line never asserted → we thought IRQ didn't
 * work and switched to polling in 2026-04-16.
 */
#define H713_MSGBOX_RX_IRQ_BIT	BIT(2)	/* RX IRQ bit for Port 1 */

/*
 * H713 msgbox exposes three GIC SPI lines (one per user-bank). MIPS can
 * assert any of them depending on which bank it writes to. We register a
 * handler on every line the DTS provides and share one FIFO drain loop.
 */
#define MSGBOX_MAX_IRQS	3

struct msgbox_irq_slot {
	int  num;		/* Linux IRQ number (−1 if unused) */
	bool registered;	/* request_irq succeeded for this slot */
};

static struct msgbox_irq_slot msgbox_irqs[MSGBOX_MAX_IRQS];
static int  msgbox_irqs_used;	/* number of slots consumed (success or fail) */

static void (*msgbox_rx_callback)(int type, int direction);

/* Fallback polling — only armed when every request_irq() call failed. */
static struct delayed_work msgbox_poll_work;
static bool msgbox_poll_active;
static bool msgbox_poll_initialized;	/* INIT_DELAYED_WORK done */

/* IRQ statistics (readable via printk for verification). */
static atomic_t msgbox_irq_count;
static atomic_t msgbox_irq_msgs_drained;

static int msgbox_registered_count(void)
{
	int i, n = 0;

	for (i = 0; i < MSGBOX_MAX_IRQS; i++)
		if (msgbox_irqs[i].registered)
			n++;
	return n;
}

/*
 * Hard IRQ handler.
 *
 * H713 msgbox is level-triggered and stays asserted as long as the
 * Port 1 FIFO has data. To avoid the 100k IRQ-storm kill:
 *   1. Mask the RX IRQ at the source (clear RX_IRQ_EN BIT(2)) first,
 *      so re-arming only happens after we fully drain.
 *   2. Drain every pending message from the FIFO.
 *   3. Write-1-clear the IRQ status bit.
 *   4. Re-enable the IRQ bit for future messages.
 *
 * Step 1+4 ensure the GIC sees at most one IRQ per drain-cycle, not
 * one per message. The callback is called in IRQ context — cpu_comm's
 * msg_cb is atomic-safe (bitop on a u32 pending mask + schedule_work).
 */
static irqreturn_t cpu_comm_msgbox_irq_handler(int irq, void *dev_id)
{
	u32 en, raw;
	int drained = 0;
	int type, direction;

	atomic_inc(&msgbox_irq_count);

	if (!msgbox_ctrl_base)
		return IRQ_NONE;

	/* 1) Mask RX IRQ bit so we don't re-fire while draining. */
	en = readl(msgbox_ctrl_base + H713_MSGBOX_RX_IRQ_EN);
	writel(en & ~H713_MSGBOX_RX_IRQ_BIT,
	       msgbox_ctrl_base + H713_MSGBOX_RX_IRQ_EN);

	/* 2) Drain FIFO. */
	while (readl(msgbox_ctrl_base + H713_MSGBOX_RX_FIFO) & 0xF) {
		raw = readl(msgbox_ctrl_base + H713_MSGBOX_RX_DATA);
		/* 2026-04-21: Fixed bit layout. TX packs
		 *   raw = (cpu << 16) | (type & 0xFFFF)   — type in LOW 16.
		 * RX was parsing type from HIGH 16 → every MIPS CALL_ACK
		 * (raw=0x02) was decoded as type=0 (CALL) + dir=2.
		 * MIPS writes only the type as u32 (no cpu encoding), so
		 * high 16 == 0 and we infer sender=MIPS (direction=2) since
		 * only MIPS fires the User1 RX IRQ. */
		type = raw & 0xFFFF;
		direction = (raw >> 16) & 0xFFFF;
		if (direction == 0)
			direction = 2;
		if (msgbox_rx_callback)
			msgbox_rx_callback(type, direction);
		if (++drained > 64)
			break;   /* safety cap */
	}
	atomic_add(drained, &msgbox_irq_msgs_drained);

	/* 3) W1C the RX IRQ status bit. */
	writel(H713_MSGBOX_RX_IRQ_BIT,
	       msgbox_ctrl_base + H713_MSGBOX_RX_IRQ_CLR);

	/* 4) Re-arm the IRQ. */
	writel(en | H713_MSGBOX_RX_IRQ_BIT,
	       msgbox_ctrl_base + H713_MSGBOX_RX_IRQ_EN);

	return drained ? IRQ_HANDLED : IRQ_NONE;
}

/*
 * Legacy polling fallback. Only activated if CPU_COMM_FORCE_POLL is
 * set at module-init time (compile-time symbol) or if request_irq
 * ultimately fails.
 */
static void cpu_comm_msgbox_poll_fn(struct work_struct *work)
{
	u32 raw;
	int type, direction;
	int count = 0;

	if (!msgbox_poll_active || !msgbox_ctrl_base)
		return;

	while (readl(msgbox_ctrl_base + H713_MSGBOX_RX_FIFO) & 0xF) {
		raw = readl(msgbox_ctrl_base + H713_MSGBOX_RX_DATA);
		/* 2026-04-21: Fixed bit layout. TX packs
		 *   raw = (cpu << 16) | (type & 0xFFFF)   — type in LOW 16.
		 * RX was parsing type from HIGH 16 → every MIPS CALL_ACK
		 * (raw=0x02) was decoded as type=0 (CALL) + dir=2.
		 * MIPS writes only the type as u32 (no cpu encoding), so
		 * high 16 == 0 and we infer sender=MIPS (direction=2) since
		 * only MIPS fires the User1 RX IRQ. */
		type = raw & 0xFFFF;
		direction = (raw >> 16) & 0xFFFF;
		if (direction == 0)
			direction = 2;
		if (msgbox_rx_callback)
			msgbox_rx_callback(type, direction);
		if (++count > 32)
			break;
	}

	if (msgbox_poll_active)
		schedule_delayed_work(&msgbox_poll_work, msecs_to_jiffies(1));
}

int cpu_comm_msgbox_request_irq(int irq, void *callback)
{
	int slot = msgbox_irqs_used;
	u32 raw, en;
	int ret;

	if (slot >= MSGBOX_MAX_IRQS) {
		pr_err("cpu_comm: too many msgbox IRQs (max=%d)\n",
		       MSGBOX_MAX_IRQS);
		return -EINVAL;
	}

	/*
	 * First call owns the one-time shared setup: callback, stats counters,
	 * stale-FIFO drain, and RX_IRQ status clear. Subsequent calls only
	 * add a GIC line to the handler pool.
	 */
	if (slot == 0) {
		msgbox_rx_callback = callback;
		atomic_set(&msgbox_irq_count, 0);
		atomic_set(&msgbox_irq_msgs_drained, 0);

		while (readl(msgbox_ctrl_base + H713_MSGBOX_RX_FIFO) & 0xF) {
			raw = readl(msgbox_ctrl_base + H713_MSGBOX_RX_DATA);
			pr_info("cpu_comm: drained stale msgbox message: 0x%08x\n",
				raw);
		}

		writel(H713_MSGBOX_RX_IRQ_BIT,
		       msgbox_ctrl_base + H713_MSGBOX_RX_IRQ_CLR);
	}

	msgbox_irqs[slot].num = irq;
	msgbox_irqs[slot].registered = false;
	msgbox_irqs_used++;

	/* IRQF_SHARED: msgbox_amp also listens on this IRQ for ARISC RX traffic.
	 * Each handler must return IRQ_NONE for events it doesn't own.
	 *
	 * NOTE: TRIGGER_HIGH dropped — DT property already specifies the level
	 * mode, and IRQF_SHARED requires all sharers to use IDENTICAL flags.
	 * msgbox_amp claims with bare IRQF_SHARED, so we match. */
	ret = request_irq(irq, cpu_comm_msgbox_irq_handler,
			  IRQF_SHARED, "hy310-cpu-comm",
			  &msgbox_irqs[slot]);
	if (ret) {
		pr_err("cpu_comm: request_irq(%d) failed: %d (slot %d)\n",
		       irq, ret, slot);
		return ret;
	}

	msgbox_irqs[slot].registered = true;

	/* Enable the shared RX IRQ bit on the first success only. */
	if (msgbox_registered_count() == 1) {
		en = readl(msgbox_ctrl_base + H713_MSGBOX_RX_IRQ_EN);
		writel(en | H713_MSGBOX_RX_IRQ_BIT,
		       msgbox_ctrl_base + H713_MSGBOX_RX_IRQ_EN);
	}

	pr_info("cpu_comm: msgbox RX IRQ %d registered (slot %d, Port 1, BIT(2)); "
		"enable-reg: 0x%08x\n",
		irq, slot,
		readl(msgbox_ctrl_base + H713_MSGBOX_RX_IRQ_EN));
	return 0;
}

/*
 * Arm the polling fallback. Caller (module init) should invoke this only
 * when zero msgbox IRQs were registered, so the RX path still works. Safe
 * to call twice — subsequent calls are no-ops.
 */
int cpu_comm_msgbox_start_polling(void *callback)
{
	if (msgbox_poll_active)
		return 0;

	if (!msgbox_poll_initialized) {
		INIT_DELAYED_WORK(&msgbox_poll_work, cpu_comm_msgbox_poll_fn);
		msgbox_poll_initialized = true;
	}
	if (!msgbox_rx_callback)
		msgbox_rx_callback = callback;

	msgbox_poll_active = true;
	schedule_delayed_work(&msgbox_poll_work, msecs_to_jiffies(10));
	pr_warn("cpu_comm: msgbox RX polling started (no IRQ available)\n");
	return 0;
}

void cpu_comm_msgbox_free_irq(void)
{
	int i, freed = 0;

	if (msgbox_ctrl_base && msgbox_registered_count() > 0) {
		u32 en = readl(msgbox_ctrl_base + H713_MSGBOX_RX_IRQ_EN);

		writel(en & ~H713_MSGBOX_RX_IRQ_BIT,
		       msgbox_ctrl_base + H713_MSGBOX_RX_IRQ_EN);
	}

	for (i = 0; i < MSGBOX_MAX_IRQS; i++) {
		if (!msgbox_irqs[i].registered)
			continue;
		free_irq(msgbox_irqs[i].num, &msgbox_irqs[i]);
		msgbox_irqs[i].registered = false;
		freed++;
	}
	msgbox_irqs_used = 0;

	if (freed)
		pr_info("cpu_comm: msgbox RX IRQs freed (%d lines, count=%d, drained=%d msgs)\n",
			freed,
			atomic_read(&msgbox_irq_count),
			atomic_read(&msgbox_irq_msgs_drained));

	if (msgbox_poll_active) {
		msgbox_poll_active = false;
		cancel_delayed_work_sync(&msgbox_poll_work);
		pr_info("cpu_comm: msgbox RX poll stopped (fallback)\n");
	}
	msgbox_rx_callback = NULL;
}

/* Legacy stubs — no longer needed but keep exports for now */
void cpu_comm_msgbox_probe_start(void) { }
void cpu_comm_msgbox_probe_stop(void) { }

/* ── Register access ───────────────────────────────────────── */

static int is_known_reg(u32 addr)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(known_regs); i++) {
		if (known_regs[i] == addr)
			return 1;
	}
	return 0;
}

/*
 * Map a physical register address to its ioremapped virtual address.
 * We maintain two mappings: MIPS control (0x03061xxx) and msgbox (0x03004xxx).
 */
static void __iomem *reg_to_iomem(u32 phys_addr)
{
	if (phys_addr >= 0x03061000 && phys_addr < 0x03062000) {
		if (!mips_ctrl_base)
			return NULL;
		return mips_ctrl_base + (phys_addr - 0x03061000);
	}
	if (phys_addr >= 0x03003000 && phys_addr < 0x03004000) {
		if (!msgbox_ctrl_base)
			return NULL;
		return msgbox_ctrl_base + (phys_addr - 0x03003000);
	}
	return NULL;
}

int comm_ReadRegWord(u32 reg_addr)
{
	void __iomem *vaddr;

	if (!is_known_reg(reg_addr)) {
		pr_err("cpu_comm: read from unknown reg 0x%08x\n", reg_addr);
		WARN_ON(1);
		return 0;
	}

	vaddr = reg_to_iomem(reg_addr);
	if (!vaddr) {
		pr_err("cpu_comm: reg 0x%08x not mapped\n", reg_addr);
		return 0;
	}

	return readl(vaddr);
}

void comm_WriteRegWord(u32 reg_addr, u32 value)
{
	void __iomem *vaddr;

	if (!is_known_reg(reg_addr)) {
		pr_err("cpu_comm: write to unknown reg 0x%08x\n", reg_addr);
		WARN_ON(1);
		return;
	}

	vaddr = reg_to_iomem(reg_addr);
	if (!vaddr) {
		pr_err("cpu_comm: reg 0x%08x not mapped\n", reg_addr);
		return;
	}

	writel(value, vaddr);
}

/* ── Shared Registers (MIPS MMIO) ─────────────────────────── */

/*
 * Shared registers are MIPS control MMIO registers used to exchange
 * configuration between ARM and MIPS at init time.
 *
 * Reg 14 → 0x03061024 → SharedMemAddr (physical)
 * Reg 13 → 0x03061028 → SharedMemSize
 */
int getShareRegbyID(int reg_id)
{
	switch (reg_id) {
	case SHARE_REG_ADDR:
		return 0x03061024;
	case SHARE_REG_SIZE:
		return 0x03061028;
	default:
		pr_err("cpu_comm: unknown share reg %d\n", reg_id);
		return 0;
	}
}

int comm_ReadShareReg(int reg_id)
{
	return comm_ReadRegWord(getShareRegbyID(reg_id));
}

void comm_WriteShareReg(int reg_id, u32 value)
{
	comm_WriteRegWord(getShareRegbyID(reg_id), value);
}

/* ── Hardware Spinlocks ────────────────────────────────────── */

/*
 * The H713 has a hardware spinlock controller (sunxi-hwspinlock).
 * cpu_comm uses 14 locks. The hardware spinlock ID = logical_id + 8.
 * These locks synchronize shared-memory access between ARM and MIPS.
 */

static int getRegbySpinID(int id)
{
	if (id < 0 || id > 13) {
		pr_err("cpu_comm: invalid spinlock ID %d\n", id);
		return -1;
	}
	return id + 8;	/* hardware spinlock offset */
}

int comm_InitHwSpinLock(void)
{
	int i, j, hw_id;

	for (i = 0; i < HW_SPINLOCK_COUNT; i++) {
		if (!hwlocks[i]) {
			hw_id = getRegbySpinID(i);
			hwlocks[i] = hwspin_lock_request_specific(hw_id);
			if (!hwlocks[i]) {
				pr_err("cpu_comm: failed to get hwspinlock %d (hw=%d)\n",
				       i, hw_id);
				for (j = 0; j < i; j++) {
					if (hwlocks[j]) {
						hwspin_lock_free(hwlocks[j]);
						hwlocks[j] = NULL;
					}
				}
				return -EBUSY;
			}
		}
	}

	/* HY310-TEST 2026-04-20: force-clear ALL 32 HW spinlock hardware
	 * registers at 0x03005000+4*N. Reason for this test:
	 *
	 *   MIPS hangs in AddInRoutine -> comm_SpinLock(2) for SetMirrorMode
	 *   even though the SharedMem-side SW lock 2 owner shows FREE and our
	 *   reads of all 16 HW spinlock registers showed 0x00000000. We don't
	 *   100% trust those readings (Allwinner sun50i hwspinlock semantics
	 *   are not fully verified in our context — 0 might still mean "taken"
	 *   from a state we cannot otherwise observe). MIPS's enterCritical
	 *   uses __hwspin_lock without our 50ms timeout fallback, so any held
	 *   HW lock would hang MIPS forever before it ever reaches the SW
	 *   lock check.
	 *
	 *   Writing 0 to each HW spinlock register releases it (sun6i-style
	 *   semantics). This is a force-clear independent of whether the
	 *   kernel hwspinlock framework currently considers them "owned".
	 *
	 *   If MIPS no longer hangs after this test patch is active, HW
	 *   spinlock leftover state was the cause; we then need a permanent
	 *   solution that doesn't fight the kernel framework. If MIPS still
	 *   hangs, the blocker is elsewhere (residual SharedMem corruption,
	 *   semaphore-init order, etc.). */
	/* DISABLED 2026-05-03 (session 20260503-X): force-clear of all 32 HW
	 * spinlocks may release locks MIPS firmware legitimately holds, causing
	 * MIPS to spawn worker threads that hammer hwlocks[0] forever. Stock
	 * vendor driver does not do this. Re-enable if MIPS hangs in
	 * AddInRoutine again. */
#if 0
	{
		void __iomem *hwsl_base = ioremap(0x03005000, 0x100);
		int n;

		if (hwsl_base) {
			for (n = 0; n < 32; n++)
				writel(0, hwsl_base + 4 * n);
			pr_warn("cpu_comm: HY310-TEST: force-cleared 32 HW spinlocks @ 0x03005000\n");
			iounmap(hwsl_base);
		} else {
			pr_err("cpu_comm: HY310-TEST: ioremap(0x03005000) failed, HW spinlocks NOT cleared\n");
		}
	}
#else
	pr_info("cpu_comm: HY310-TEST force-clear skipped (test 20260503-X)\n");
#endif
	return 0;
}

/*
 * spinlockhwReg — Acquire hardware spinlock (busy-wait)
 */
int spinlockhwReg(int lock_id)
{
	int ret;

	if (!hwlocks[lock_id]) {
		pr_err("cpu_comm: spinlockhwReg(%d): hwlocks[%d] is NULL!\n",
		       lock_id, lock_id);
		return -EINVAL;
	}

	ret = __hwspin_lock_timeout(hwlocks[lock_id], 100, HWLOCK_RAW, NULL);
	if (ret) {
		pr_warn("cpu_comm: spinlockhwReg(%d): timeout (ret=%d) hwlock=%px\n",
			lock_id, ret, hwlocks[lock_id]);
		return ret;
	}

	/* Increment lock counter (stored at hwlocks[lock_id + 14]) */
	((u32 *)hwlocks)[lock_id + HW_SPINLOCK_COUNT]++;
	return 0;
}

/*
 * tryspinlockhwReg — Try to acquire hardware spinlock (non-blocking)
 * Returns 1 on success, 0 on failure.
 */
int tryspinlockhwReg(int lock_id)
{
	int ret;

	ret = __hwspin_trylock(hwlocks[lock_id], HWLOCK_RAW, NULL);
	if (ret == 0) {
		((u32 *)hwlocks)[lock_id + HW_SPINLOCK_COUNT]++;
		return 1; /* acquired */
	}
	return 0; /* failed */
}

/*
 * spinUnlockhwReg — Release hardware spinlock
 */
void spinUnlockhwReg(int lock_id)
{
	__hwspin_unlock(hwlocks[lock_id], HWLOCK_RAW, NULL);
}

/* ── Software Spinlocks (in shared memory) ─────────────────── */

/*
 * 2-Level spinlock system from IDA:
 *
 * Level 1: HW spinlock — held BRIEFLY for atomic SharedMem update.
 *          Mapping: SW lock N → HW lock N+2 (hwlocks[N+2])
 *
 * Level 2: SharedMem owner field — held for the ENTIRE critical section.
 *          12 entries at ShMemAddrBase + 0, each 12 bytes:
 *            +0: type (u8) — lock type, initialized to 2
 *            +1: owner_cpu (u8) — 2=free, 0=ARM, 1=MIPS
 *            +2: mutex_flag (u8) — 0 for normal, set for nested
 *            +3: (u8) — initialized to 2
 *            +4: ref_count (u32) — nesting depth
 *            +8: thread_id (u32) — 255=unassigned, else current->tgid
 *
 * enterCritical: acquires HW lock → sets owner in SharedMem → releases HW lock
 * leaveCritical: sets owner back to 2 (free) in SharedMem (no HW lock needed)
 * spinLock: uses enterCritical/leaveCritical + ref_count management
 */

/*
 * SharedMem spinlock entry layout (12 bytes, from IDA comm_InitSpinLock):
 *   [0]: type (init: 2)
 *   [1]: status (init: 2)
 *   [2]: mutex_flag (init: 0)
 *   [3]: owner_cpu (SPINLOCK_FREE=2, ARM=0, MIPS=1)
 *   [4..7]: ref_count (u32, init: 0)
 *   [8..11]: thread_id (u32, init: 255=NONE)
 */
#define SPINLOCK_FREE		2	/* owner_cpu value for "free" */
#define SPINLOCK_HWID_NONE	255	/* unassigned hw lock id */
#define SPINLOCK_MAX_ID		11	/* max SW lock id */
#define SPINLOCK_HW_OFFSET	2	/* SW lock N → hwlocks[N+2] */
#define SPINLOCK_ENTRY_SIZE	12	/* bytes per entry in SharedMem */
#define SPINLOCK_OFF_TYPE	0
#define SPINLOCK_OFF_STATUS	1
#define SPINLOCK_OFF_MUTEX	2
#define SPINLOCK_OFF_OWNER	3	/* THE critical offset — was wrongly 1 */

static u8 *getSpinLockMemArea(void)
{
	if (WARN_ON(!ShMemAddrBase))
		return NULL;
	return (u8 *)(unsigned long)ShMemAddrBase;
}

/*
 * comm_InitSpinLock — Initialize 12 spinlock entries in shared memory.
 * From IDA @ 0x92e4.
 */
int comm_InitSpinLock(int dummy)
{
	u8 *base;
	int i;
	int ret;

	base = getSpinLockMemArea();
	if (!base)
		return -1;

	ret = comm_InitHwSpinLock();
	if (ret)
		return ret;

	/* Release all HW locks 1..13 (clear stale state from previous boot) */
	for (i = 1; i < HW_SPINLOCK_COUNT; i++)
		spinUnlockhwReg(i);

	/* Initialize 12 SW spinlock entries in shared memory.
	 * HY310 2026-04-22: atomic dword writes + explicit memory barrier.
	 * Reason: byte-wise writes can be reordered/batched by compiler+cpu,
	 * so MIPS could see a partially-initialized lock entry. Observed at
	 * runtime: lock 2 stuck at byte[0]=0x01 (previously held by MIPS),
	 * locks 5/6 with byte[3]=0x00 (incomplete init pattern). MIPS enterCritical
	 * reads byte[0] and hangs if it is not 0x02, so any lingering stale byte
	 * blocks the whole registration path. Writing as a single 32-bit store
	 * makes the init atomic; dmb(ish) ensures visibility to MIPS before we
	 * release HW lock 0 (which signals MIPS to proceed). */
	for (i = 0; i < 12; i++) {
		u32 *entry = (u32 *)(base + SPINLOCK_ENTRY_SIZE * i);

		entry[0] = 0x02000202;	/* type=2, status=2, mutex=0, owner=2 */
		entry[1] = 0;		/* ref_count */
		entry[2] = SPINLOCK_HWID_NONE; /* thread_id = 255 */
	}
	dmb(ish);

	return 0;
}

/*
 * enterCritical — Atomically claim ownership via HW lock + SharedMem.
 * From IDA @ 0x97c4.
 *
 * 1. Spin until SharedMem entry shows "free" (owner==2)
 * 2. Acquire HW lock (lock_id + 2)
 * 3. Verify still free → set owner = our CPU
 * 4. Release HW lock
 *
 * After this, the calling CPU "owns" the lock in SharedMem.
 */
static void enterCritical(int lock_id)
{
	u8 *base = getSpinLockMemArea();
	u8 *entry = base + SPINLOCK_ENTRY_SIZE * lock_id;
	int hw_id = lock_id + SPINLOCK_HW_OFFSET;
	int spins = 0;
	u8 cur_cpu;

	pr_debug("cpu_comm: enterCritical(%d) hw_id=%d entry=[%02x %02x %02x %02x]\n",
		 lock_id, hw_id, entry[0], entry[1], entry[2], entry[3]);

	/*
	 * Use hwspin_lock_timeout instead of trylock loop.
	 * MIPS may already hold HW locks — trylock would spin forever.
	 * Timeout of 50ms: if we can't get the HW lock, force-proceed
	 * because ARM needs to initialize regardless of MIPS state.
	 */
	{
		int ret = __hwspin_lock_timeout(hwlocks[hw_id], 50, HWLOCK_RAW, NULL);

		if (ret) {
			/* HW lock timeout — MIPS probably holds it */
			pr_warn("cpu_comm: enterCritical(%d): HW lock %d timeout "
				"(ret=%d), force-claiming SharedMem entry\n",
				lock_id, hw_id, ret);
			/* Force-claim without HW lock protection */
			cur_cpu = (u8)getCurCPUID(0);
			entry[SPINLOCK_OFF_OWNER] = cur_cpu;
			dmb(ish);
			return;
		}

		/* Got HW lock — claim SharedMem entry */
		cur_cpu = (u8)getCurCPUID(0);
		entry[SPINLOCK_OFF_OWNER] = cur_cpu;
		dmb(ish);

		/* Release HW lock — we now own via SharedMem */
		__hwspin_unlock(hwlocks[hw_id], HWLOCK_RAW, NULL);

		pr_debug("cpu_comm: enterCritical(%d) acquired OK\n", lock_id);
	}
}

/*
 * leaveCritical — Release ownership in SharedMem.
 * From IDA @ 0x9914.
 * Just sets owner back to 2 (free). No HW lock needed.
 */
static void leaveCritical(int lock_id)
{
	u8 *base = getSpinLockMemArea();
	u8 *entry = base + SPINLOCK_ENTRY_SIZE * lock_id;

	entry[SPINLOCK_OFF_OWNER] = SPINLOCK_FREE;	/* set owner = free */
}

/*
 * spinLock — Full lock with ref_count tracking.
 * From IDA @ 0x9a68.
 *
 * @lock_id: 0..11
 * @mode: 0=try (non-blocking), 1=mutex (recursive), 2=exclusive (blocking)
 *
 * Returns: 0=failed (mode 0 only), 1=recursive re-lock, 2=new lock acquired
 */
static int spinLock(int lock_id, int mode)
{
	u8 *base = getSpinLockMemArea();
	u8 *entry = base + SPINLOCK_ENTRY_SIZE * lock_id;
	u8 cur_cpu;
	unsigned int retries = 0;
	const unsigned int max_retries = 10000; /* ~10s worst case with schedule() */

	if (lock_id > SPINLOCK_MAX_ID) {
		pr_err("cpu_comm: spinLock: invalid lock_id %d\n", lock_id);
		return -EINVAL;
	}
	if (*(int *)(entry + 4) < 0) { /* ref_count sanity */
		pr_err("cpu_comm: spinLock: negative ref_count on lock %d\n", lock_id);
		return -EIO;
	}

	while (1) {
		if (++retries > max_retries) {
			pr_err("cpu_comm: spinLock(%d, mode=%d): TIMEOUT after %u retries "
			       "(owner=%u thread_id=0x%x refcnt=%d) — returning -EBUSY\n",
			       lock_id, mode, retries,
			       entry[SPINLOCK_OFF_OWNER],
			       *(u32 *)(entry + 8),
			       *(int *)(entry + 4));
			return -EBUSY;
		}
		enterCritical(lock_id);
		cur_cpu = (u8)getCurCPUID(0);

		if (entry[SPINLOCK_OFF_OWNER] == cur_cpu) {
			/*
			 * We own this lock (entry[3] set by enterCritical).
			 * Two sub-cases:
			 *   a) First acquisition: thread_id == NONE (255)
			 *      → enterCritical claimed it but thread_id not set yet
			 *   b) Recursive lock: thread_id == our tgid
			 */
			if (*(u32 *)(entry + 8) == SPINLOCK_HWID_NONE) {
				/* First acquisition — set thread_id + ref_count */
				*(u32 *)(entry + 8) = (u32)current->tgid;
				(*(u32 *)(entry + 4))++;
				leaveCritical(lock_id);
				return 2;	/* new lock acquired */
			}
			if (!entry[2] && /* mutex_flag clear */
			    *(u32 *)(entry + 8) == (u32)current->tgid) {
				/* Same thread — recursive lock */
				(*(u32 *)(entry + 4))++;
				leaveCritical(lock_id);
				return 1;	/* recursive */
			}
			/* Different thread holds it — release and retry */
			leaveCritical(lock_id);
			if (!mode)
				return 0;	/* non-blocking: fail */
			schedule();
			continue;
		}

		if (entry[SPINLOCK_OFF_OWNER] == SPINLOCK_FREE) {
			/* Lock is free but enterCritical didn't claim it
			 * (shouldn't happen — enterCritical always claims).
			 * Handle defensively: claim it now.
			 */
			entry[SPINLOCK_OFF_OWNER] = cur_cpu;
			*(u32 *)(entry + 8) = (u32)current->tgid;
			(*(u32 *)(entry + 4))++;
			dmb(ish);
			leaveCritical(lock_id);
			return 2;	/* new lock acquired */
		}

		/* Someone else owns it */
		leaveCritical(lock_id);
		if (!mode)
			return 0;	/* non-blocking: fail */
		if (mode >= 2)
			schedule();	/* blocking: yield and retry */
	}
}

/*
 * comm_SpinLock — Acquire lock (exclusive, blocking).
 * From IDA @ 0x9e2c: calls spinLock(id, 2).
 */
void comm_SpinLock(int lock_id)
{
	pr_debug("cpu_comm: comm_SpinLock(%d) enter\n", lock_id);
	spinLock(lock_id, 2);
	pr_debug("cpu_comm: comm_SpinLock(%d) acquired\n", lock_id);
}

/*
 * comm_SpinUnLock — Release lock with ref_count management.
 * From IDA @ 0x9e74.
 */
void comm_SpinUnLock(int lock_id, int dummy)
{
	u8 *base = getSpinLockMemArea();
	u8 *entry = base + SPINLOCK_ENTRY_SIZE * lock_id;

	if (lock_id > SPINLOCK_MAX_ID) {
		pr_err("cpu_comm: SpinUnLock: invalid lock_id %d\n", lock_id);
		return;
	}

	enterCritical(lock_id);

	if (*(int *)(entry + 4) <= 0) {
		/* ref_count already 0 — double unlock */
		leaveCritical(lock_id);
		return;
	}

	if (entry[SPINLOCK_OFF_OWNER] == SPINLOCK_FREE) {
		pr_warn("cpu_comm: SpinUnLock(%d): unlocking a free lock, skipping\n", lock_id);
		leaveCritical(lock_id);
		return;
	}
	if (*(u32 *)(entry + 8) == SPINLOCK_HWID_NONE) {
		pr_warn("cpu_comm: SpinUnLock(%d): no thread_id, skipping\n", lock_id);
		leaveCritical(lock_id);
		return;
	}

	/* Decrement ref_count */
	if (--(*(u32 *)(entry + 4)) == 0) {
		/* Last unlock — release fully */
		entry[SPINLOCK_OFF_OWNER] = SPINLOCK_FREE;
		*(u32 *)(entry + 8) = SPINLOCK_HWID_NONE;
	}

	leaveCritical(lock_id);
}

/*
 * comm_TrySpinLock — Non-blocking lock attempt.
 * Returns 1 on success, 0 on failure.
 */
int comm_TrySpinLock(int lock_id)
{
	return spinLock(lock_id, 0) ? 1 : 0;
}

/*
 * comm_SpinLockMutex — Recursive-safe lock (mutex mode).
 * From IDA @ 0x9e48: calls spinLock(id, 1).
 */
void comm_SpinLockMutex(int lock_id)
{
	spinLock(lock_id, 1);
}

void comm_SpinLocksetType(int lock_id, int type)
{
	u8 *base = getSpinLockMemArea();

	if (!base)
		return;
	base[SPINLOCK_ENTRY_SIZE * lock_id] = type;
}

/*
 * comm_ReqestSpinLock — Allocate a free software spinlock from the pool.
 * Scans slots 5..11 for a free one (owner == SPINLOCK_FREE).
 * Returns slot ID (5..11) or 255 if none available.
 */
int comm_ReqestSpinLock(void)
{
	int id;
	u8 *base, *entry;

	spinlockhwReg(1);

	base = getSpinLockMemArea();
	if (!base) {
		spinUnlockhwReg(1);
		return 255;
	}

	for (id = HW_SPINLOCK_APP_START; id <= HW_SPINLOCK_APP_END; id++) {
		entry = base + SPINLOCK_ENTRY_SIZE * id;
		if (entry[SPINLOCK_OFF_OWNER] == SPINLOCK_FREE) {
			if (*(u32 *)(entry + 4) != 0) {
				pr_warn("cpu_comm: AssignSpinLock(%d): free lock has non-zero ref_count\n", id);
				continue;
			}
			if (entry[0] != SPINLOCK_FREE) {
				pr_warn("cpu_comm: AssignSpinLock(%d): inconsistent mutex state\n", id);
				continue;
			}
			if (*(u32 *)(entry + 8) != SPINLOCK_HWID_NONE) {
				pr_warn("cpu_comm: AssignSpinLock(%d): free lock has thread_id set\n", id);
				continue;
			}
			entry[SPINLOCK_OFF_MUTEX] = 0;
			entry[SPINLOCK_OFF_OWNER] = getCurCPUID(0);
			spinUnlockhwReg(1);
			return id;
		}
	}

	spinUnlockhwReg(1);
	return 255;
}

/*
 * comm_ReleaseSpinLock — Return a spinlock to the pool.
 */
int comm_ReleaseSpinLock(int lock_id)
{
	u8 *base, *entry;

	if (lock_id < HW_SPINLOCK_APP_START || lock_id > HW_SPINLOCK_APP_END) {
		pr_err("cpu_comm: release invalid spinlock %d\n", lock_id);
		return -EINVAL;
	}

	base = getSpinLockMemArea();
	entry = base + SPINLOCK_ENTRY_SIZE * lock_id;

	spinlockhwReg(1);

	if (*(u32 *)(entry + 4) != 0 || *(u32 *)(entry + 8) != SPINLOCK_HWID_NONE) {
		pr_err("cpu_comm: spinlock %d still in use\n", lock_id);
		spinUnlockhwReg(1);
		return -1;
	}

	entry[SPINLOCK_OFF_OWNER] = SPINLOCK_FREE;
	spinUnlockhwReg(1);
	return 0;
}

/* ── Interrupt management ──────────────────────────────────── */

/*
 * getInterruptRegChannel — Get interrupt register set for a CPU pair
 * cpu: 0=ARM, 1=MIPS. channel: 0 or 1.
 * Returns pointer to interrupt register struct (5 DWORDs).
 * Stock returns 0 (simplified — actual registers are in the built-in
 * sunxi_cpu_comm RPMSG layer which handles the msgbox hardware).
 */
int getInterruptRegChannel(u32 cpu, u32 channel)
{
	if (cpu > 1 || channel > 1) {
		pr_err("cpu_comm: invalid IRQ channel cpu=%u ch=%u\n",
		       cpu, channel);
		return -EINVAL;
	}
	return 0; /* stub — actual interrupt routing is in sunxi_cpu_comm */
}

/*
 * ResetCommInterrupt — Clear interrupt registers for all channels
 */
void ResetCommInterrupt(int dummy)
{
	/* In the stock module, this writes 0 to four interrupt registers
	 * per channel. Since our interrupt handling goes through the
	 * built-in sunxi_cpu_comm RPMSG layer, this is a no-op.
	 * The RPMSG layer manages msgbox interrupts directly.
	 */
}

/*
 * comm_SpinLockCleanUpInCPUReset — Release spinlocks held by reset CPU.
 *
 * From IDA @ 0xa008. Walks all spinlock slots and releases any that
 * were held by the specified CPU. This prevents deadlocks when a CPU
 * resets while holding spinlocks.
 */
/*
 * comm_SpinLockCleanUpInCPUReset — Release locks held by reset CPU.
 * From IDA @ 0xa008. Walks SharedMem entries and force-releases any
 * owned by the specified CPU.
 */
void comm_SpinLockCleanUpInCPUReset(u32 cpu_id)
{
	u8 *base;
	int i;

	pr_info("cpu_comm: SpinLockCleanUpInCPUReset(%u)\n", cpu_id);

	base = getSpinLockMemArea();
	if (!base)
		return;

	for (i = 0; i <= SPINLOCK_MAX_ID; i++) {
		u8 *entry = base + SPINLOCK_ENTRY_SIZE * i;

		if (entry[SPINLOCK_OFF_OWNER] == (u8)cpu_id) {
			/* This lock was held by the resetting CPU — force release */
			pr_info("cpu_comm: force-releasing lock %d (held by CPU %u)\n",
				i, cpu_id);
			entry[SPINLOCK_OFF_OWNER] = SPINLOCK_FREE;
			*(u32 *)(entry + 4) = 0;		/* ref_count = 0 */
			*(u32 *)(entry + 8) = SPINLOCK_HWID_NONE; /* thread_id = none */
		}
	}
}

/*
 * sunxi_cpu_comm_send_intr_to_mips — Trigger msgbox interrupt to MIPS.
 *
 * The stock module writes directly to the msgbox MMIO registers.
 * On our mainline kernel, we write to the msgbox channel 2 register
 * at 0x03003000 + offset. The write value encodes the message type.
 *
 * @cpu: message type in stock HAL path (0=CALL,1=RETURN,2=CALL_ACK,3=RETURN_ACK)
 * @type: interrupt route/type (stock uses constant 2)
 * @value: payload value (unused in current path)
 */
void sunxi_cpu_comm_send_intr_to_mips(u32 cpu, u32 type, u32 value)
{
	u32 raw;

	if (!msgbox_ctrl_base) {
		pr_err("cpu_comm: msgbox not mapped, can't send intr!\n");
		return;
	}

	/*
	 * Stock HAL semantics recovered from IDA:
	 *   sunxi_cpu_comm_send_intr_to_mips(msg_type, intr_type, payload)
	 * Message format:
	 *   raw[31:16] = msg_type (0..3)
	 *   raw[15:0]  = direction/intr_type (stock: 2)
	 *
	 * HY310-fix 2026-04-18: previous code did `raw = cpu & 0x3` which
	 * dropped intr_type — for dir=0 that wrote plain 0x00000000 to the
	 * FIFO and MIPS's IRQ handler saw (type=0, direction=0) which it
	 * treats as no-op. Must encode both halves as stock does.
	 */
	raw = ((cpu & 0x3) << 16) | (type & 0xFFFF);

	/* TX goes to MIPS base (0x400 offset) + 0x70 */
	writel(raw, msgbox_ctrl_base + H713_MSGBOX_TX_DATA);
	pr_info_ratelimited("cpu_comm: msgbox tx raw=0x%08x\n", raw);
}

/*
 * ──────────────────────────────────────────────────────────────
 * ARISC direct-TX path (pfad 3 — session-N 2026-04-24)
 *
 * The stock ARISC-rpm client (sunxi,cpus-msgbox channel) sends packets
 * starting with a 0xA5 marker + 8-byte header + payload. We don't want
 * to refactor cpu_comm into an rpmsg client, so we expose a direct
 * msgbox-write helper that a tiny consumer module uses to drive HPD
 * commands to ARISC.
 *
 * ARISC is CPU-index 1, port 1 per stock DTS (rpmsg_amp_remote-1=<1>,
 * rpmsg_read_channel-1=<1>, rpmsg_write_channel-1=<1>).
 *
 * ARM-side TX address (H713 msgbox-amp semantics):
 *   base[remote=1] + USER_STRIDE * tw + MSG_DATA + 4 * tx_port
 *   = 0x03003400 + 256 * 0 + 0x70 + 4 = 0x03003474
 *
 * Because the full ARISC packet is 8..140 bytes, we stream it as
 * sequential 4-byte writes to the same FIFO register (HW auto-pushes
 * each word into the msgbox ring).
 * ──────────────────────────────────────────────────────────── */
/*
 * ARISC msgbox port confirmed via firmware RE (session-N extract+disasm of
 * bootloader_a.bin SCP image, 2026-04-24):
 *
 *   ARISC polls FIFO_STAT at 0x0300346c (user1 sub0 port3 count)
 *   ARISC reads MSG_DATA at 0x0300347c (user1 sub0 port3 data)
 *   ARISC clears IRQ_STAT bit 6 at 0x03003424 (writes 0x40 = RX_IRQ_BIT(3))
 *
 * So ARM-TX must target **port 3**, not port 1 as the stock DTS naming
 * "rpmsg_read_channel-1 = 1" suggests. "Channel 1" in stock DTS turns out
 * to map to HW port 3 on the ARM↔ARISC pair. ARM → ARISC register
 * addresses (base[remote=1=ARISC] + adj(L=0,R=1)*256 + off + 4*port):
 *   TX FIFO:   0x3003400 + 0 + 0x70 + 4*3 = 0x0300347c  → offset 0x47c
 *   TX IRQ EN: 0x3003400 + 0 + 0x30       = 0x03003430  → offset 0x430
 *   TX IRQ bit: 1 << (2*port+1) = 1 << 7  = 0x80        → BIT(7)
 */
#define H713_MSGBOX_ARISC_TX_DATA	0x47c	/* user1 sub0 port3 FIFO data */
#define H713_MSGBOX_ARISC_TX_IRQ_EN	0x430	/* user1 sub0 TX IRQ enable */
#define H713_MSGBOX_ARISC_TX_BIT	BIT(7)	/* TX_IRQ_BIT(port=3) = 1<<(2*3+1) */

static DEFINE_MUTEX(arisc_tx_lock);
static u8 arisc_tx_seq;

int cpu_comm_send_to_arisc(u8 type, u8 length, const u8 *pdata)
{
	u8 buf[140];		/* 8 header + up to 132 payload, matches stock */
	u32 *words;
	int total, nwords, i;

	if (!msgbox_ctrl_base)
		return -ENODEV;
	if (length > 132)
		return -EINVAL;
	if (length && !pdata)
		return -EINVAL;

	memset(buf, 0, sizeof(buf));
	buf[0] = 0xA5;
	/* buf[1] = seq — filled under lock */
	buf[2] = type;
	buf[3] = length;
	/* bytes 4..7 = pad (zero) */
	if (length)
		memcpy(buf + 8, pdata, length);

	total  = 8 + length;
	nwords = (total + 3) / 4;
	words  = (u32 *)buf;

	mutex_lock(&arisc_tx_lock);
	buf[1] = ++arisc_tx_seq;

	/* Push all words into the ARISC FIFO first. */
	for (i = 0; i < nwords; i++) {
		dsb(st);
		writel(words[i], msgbox_ctrl_base + H713_MSGBOX_ARISC_TX_DATA);
	}

	/*
	 * TX doorbell: pulse TX_IRQ_EN bit(3) to signal ARISC that new
	 * data is in the FIFO. Stock msgbox-amp's sx_ept_send does the
	 * same pulse pattern (set → udelay(10) → clear). H713 TX_IRQ_EN
	 * is write-only; no readl between set and clear.
	 */
	dsb(sy);
	writel(H713_MSGBOX_ARISC_TX_BIT,
	       msgbox_ctrl_base + H713_MSGBOX_ARISC_TX_IRQ_EN);
	dsb(sy);
	udelay(10);
	writel(0, msgbox_ctrl_base + H713_MSGBOX_ARISC_TX_IRQ_EN);
	dsb(sy);

	mutex_unlock(&arisc_tx_lock);

	pr_info_ratelimited("cpu_comm: arisc tx type=0x%02x len=%u seq=%u (%d words + doorbell)\n",
			    type, length, buf[1], nwords);
	return 0;
}
EXPORT_SYMBOL_GPL(cpu_comm_send_to_arisc);

/* ── Register mapping init/cleanup ─────────────────────────── */

int cpu_comm_hw_init(void)
{
	mips_ctrl_base = ioremap(MIPS_CTRL_BASE, 0x100);
	if (!mips_ctrl_base) {
		pr_err("cpu_comm: failed to map MIPS ctrl @ 0x%08x\n",
		       MIPS_CTRL_BASE);
		return -ENOMEM;
	}

	msgbox_ctrl_base = ioremap(0x03003000, 0x800);
	if (!msgbox_ctrl_base) {
		pr_err("cpu_comm: failed to map msgbox ctrl\n");
		iounmap(mips_ctrl_base);
		mips_ctrl_base = NULL;
		return -ENOMEM;
	}

	pr_info("cpu_comm: HW mapped — MIPS ctrl @ %p, msgbox @ %p\n",
		mips_ctrl_base, msgbox_ctrl_base);
	return 0;
}

void cpu_comm_hw_cleanup(void)
{
	int i;

	cpu_comm_msgbox_free_irq();

	for (i = 0; i < HW_SPINLOCK_COUNT; i++) {
		if (hwlocks[i]) {
			hwspin_lock_free(hwlocks[i]);
			hwlocks[i] = NULL;
		}
	}

	if (msgbox_ctrl_base) {
		iounmap(msgbox_ctrl_base);
		msgbox_ctrl_base = NULL;
	}
	if (mips_ctrl_base) {
		iounmap(mips_ctrl_base);
		mips_ctrl_base = NULL;
	}
}

/* ── CPU ID helpers ────────────────────────────────────────── */

u32 getCurCPUID(int dummy)
{
	return CPU_ID_ARM; /* we are always the ARM CPU */
}

const char *getCurCPUName(int dummy)
{
	return "ARM";
}

const char *getCPUIDName(u32 cpu_id)
{
	switch (cpu_id) {
	case CPU_ID_ARM:  return "ARM";
	case CPU_ID_MIPS: return "MIPS";
	default:          return "UNKNOWN";
	}
}

u32 CPUId2Index(u32 cpu_id)
{
	return cpu_id; /* identity mapping on H713 */
}
