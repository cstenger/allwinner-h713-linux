// SPDX-License-Identifier: GPL-2.0
/*
 * cpu_comm_proto.c — Communication protocol layer
 *
 * Message sending (SendCommLow, SendComm2CPUEx), message receiving
 * (command_action, ack_action), and the 4 message type handlers
 * (call, return, callACK, returnACK).
 *
 * RE source: HAL_SX6/Kernel_Driver/cpu_comm/cpu_comm_core.c
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/semaphore.h>
#include <linux/delay.h>
#include "cpu_comm.h"

/* ── Interrupt semaphore array ─────────────────────────────── */

/*
 * comm_intrsem layout (from IDA):
 * [0..7]: semaphore-like structures
 * [8..11]: cached call pointers per (cpu, direction) pair
 * Indexed as: comm_intrsem[2 * cpu + direction + 8]
 */
u32 comm_intrsem[16];
static struct semaphore intr_sems[4]; /* 2 CPUs × 2 directions */

static const char *cpu_comm_type_name(int type)
{
	switch (type) {
	case MSG_TYPE_CALL:
		return "CALL";
	case MSG_TYPE_RETURN:
		return "RETURN";
	case MSG_TYPE_CALL_ACK:
		return "CALL_ACK";
	case MSG_TYPE_RETURN_ACK:
		return "RETURN_ACK";
	default:
		return "UNKNOWN";
	}
}

static const char *cpu_comm_dir_name(int direction)
{
	switch (direction) {
	case 0:
		return "LOCAL/ARM";
	case 2:
		return "REMOTE/MIPS";
	default:
		return "OTHER";
	}
}

static void cpu_comm_trace_ipc(const char *stage, int type, int direction, int cpu)
{
	u32 arm_flag = 0;
	u32 mips_flag = 0;

	if (ShMemAddrBase) {
		arm_flag = *(u32 *)(ShMemAddrBase + 0x4CDC);
		mips_flag = *(u32 *)(ShMemAddrBase + 0x4CE0);
	}

	/* 2026-05-03 DIAGNOSTIC: dropped ratelimit to catch every event so
	 * we can verify whether type=1 (RETURN) msgbox-IRQs reach ARM. The
	 * "callbacks suppressed" message confirmed the limiter was hiding
	 * events. Revert to pr_info_ratelimited once RETURN-path bug is
	 * understood. */
	pr_info("cpu_comm: IPC[%s] type=%d(%s) dir=%d(%s) cpu=%d ARM=0x%x MIPS=0x%x\n",
		stage,
		type, cpu_comm_type_name(type),
		direction, cpu_comm_dir_name(direction),
		cpu, arm_flag, mips_flag);
}

void init_intrsem(int dummy)
{
	int i;

	memset(comm_intrsem, 0, sizeof(comm_intrsem));
	for (i = 0; i < 4; i++)
		sema_init(&intr_sems[i], 0);
}

/* ── SendCommLow — Low-level message dispatch ──────────────── */

/*
 * Writes message metadata into a shared-memory FIFO slot,
 * then triggers either local processing (queueAction) or
 * remote interrupt (sunxi_cpu_comm_send_intr_to_mips).
 *
 * @seq:    pointer to share sequence structure
 * @slot:   FIFO slot index
 * @type:   message type (cmd_type field)
 * @param:  parameter value
 * @sem:    semaphore pointer for completion notification
 */
int SendCommLow(u8 *seq, u32 slot, u8 type, u32 param, u32 sem_ptr)
{
	u8 src, dst, dir;
	u32 cur_cpu;

	if (WARN_ON(!seq))
		return -EINVAL;

	src = seq[1];	/* source CPU */
	dst = seq[0];	/* destination CPU - note: reversed in share_seq */

	cur_cpu = getCurCPUID(0);
	if (WARN_ON(src != cur_cpu))
		return -EINVAL;

	dir = seq[2];	/* direction: 0=call, 1=return */
	if (WARN_ON(dir > 1 || slot >= FIFO_DEFAULT_CAP))
		return -EINVAL;

	/* Fill message metadata */
	seq[16] = slot;		/* sequence number */
	*(u32 *)(seq + 20) = param;
	*(u64 *)(seq + 24) = (u64)sem_ptr;
	*(u32 *)(seq + 4) += 1;	/* increment counter */
	seq[8] = type;

	/* Memory barrier — ensure writes visible before signaling */
	dmb(ish);

	if (dst == src) {
		/* Local message — process directly via work queue */
		return queueAction(dir, cur_cpu);
	}

	/* Remote message — set "sent" flag and notify via msgbox */
	seq[8] |= MSG_FLAG_SENT;
	dmb(ish);

	/* Wait for flag acknowledgement */
	while (!(seq[8] & MSG_FLAG_SENT))
		;

	/* Send interrupt to MIPS via msgbox.
	 * MIPS firmware cpu_comm_msg_cb (sub_8B12254C, trid_cpucomm_hal.c:18)
	 * dispatches based on the low-16 bits (comm_type):
	 *   0=CALL, 1=RETURN, 2=CALL_ACK, 3=RETURN_ACK
	 * dir==0 means a fresh forward CALL, dir==1 means a RETURN response.
	 * High-16 is the sender CPU (ARM=0). Mapping is direct. */
	{
		u32 comm_type = (dir == 0) ? MSG_TYPE_CALL : MSG_TYPE_RETURN;

		sunxi_cpu_comm_send_intr_to_mips(0 /*sender=ARM*/, comm_type, 0);
	}
	return 0;
}

/* ── SendComm2CPUEx — Extended message send ────────────────── */

/*
 * The main message sending function. Full implementation matching IDA @ 0xd164.
 *
 * Handles:
 *  1. Finding target component (FindRoutine for local CPU, direct read for remote)
 *  2. Acquiring the correct sequence semaphore (sem_ptr depends on target_cpu)
 *  3. Waiting for FIFO space
 *  4. Getting or reusing a cached free call slot
 *  5. Copying message data (copy_from_user or memcpy)
 *  6. Setting slot index, msg_type, session ID, routine fields, pid
 *  7. Allocating a wait object when a response is expected
 *  8. Sending via SendCommLow and waiting for ACK (500 jiffies timeout)
 *  9. Cleaning up wait object on timeout
 *
 * @msg_ptr:    u32 pointer to 104-byte message buffer (user or kernel)
 * @target_cpu: 0=local (ARM), 1=remote (MIPS)
 * @from_user:  non-zero if msg_ptr is in userspace
 */
int SendComm2CPUEx(u32 msg_ptr, u32 target_cpu, int from_user)
{
	u8     *share_seq_w;	/* write-side share sequence */
	u32     seq_base;	/* comm_socket base for dst_cpu */
	u8     *sem_ptr;	/* pointer to the acquire semaphore */
	u8     *call_slot;	/* FIFO slot for this message */
	u16     slot_idx;	/* slot index saved before copy */
	u8      msg_type;	/* 1=call-to-local, 2=call-to-remote */
	u32     dst_cpu;	/* resolved destination CPU */
	u16     flags;		/* message flags from slot+6 */
	u8      direction;	/* 0=call (forward), 1=return (backward) */
	u32     session_id;	/* generated session ID */
	u32    *wait_ptr;	/* allocated wait object, or NULL */
	int     ret = 0;
	/*
	 * routine_find_buf: filled by FindRoutine when target_cpu==0.
	 * Must be 96 bytes — FindRoutineEx copies full entry (96 bytes)
	 * including mmiocpy(buf+12, ..., 64) and *(u64*)(buf+80).
	 * BUG-18 fix: was 24 bytes → stack overflow!
	 */
	u8      routine_find_buf[96];
	bool    routine_found = false;

	/* ── Step 1: basic validation ── */
	if (!msg_ptr || target_cpu > 1) {
		pr_err("cpu_comm: SendComm2CPUEx: invalid args msg=%08x cpu=%u\n",
		       msg_ptr, target_cpu);
		return -EINVAL;
	}

	/* ── Step 2: resolve dst_cpu ── */

	if (target_cpu == 0) {
		/*
		 * Local CPU path: use component_id from msg+40 to look up
		 * the target via FindRoutine.  FindRoutine fills a 24-byte
		 * result buffer; byte [2] is the destination CPU.
		 */
		u32 comp_id;

		if (from_user) {
			if (get_user(comp_id, (u32 __user *)(msg_ptr + 40)))
				return -EFAULT;
		} else {
			comp_id = *(u32 *)(msg_ptr + 40);
		}

		if (FindRoutine(comp_id, routine_find_buf) != 0)
			return -3; /* ESRCH — routine not found */

		routine_found = true;
		dst_cpu = (u32)(u8)routine_find_buf[2];

		/*
		 * HY310-fix: entry[2] is used dual-purpose — as routing dst_cpu (0/1)
		 * AND as MIPS channel-key high nibble (can be >1, e.g. thread_id=3
		 * for SetMirrorMode channel 0x30). If routine_find_buf[2] > 1 we
		 * treat it as a thread_id for channel-key population, and read the
		 * actual routing dst_cpu from msg+2 (caller hint).
		 */
		if (dst_cpu > 1) {
			u16 dc;

			if (from_user) {
				if (get_user(dc, (u16 __user *)(msg_ptr + 2)))
					return -EFAULT;
			} else {
				dc = *(u16 *)(msg_ptr + 2);
			}
			pr_info("cpu_comm: HY310-fix: routine thread_id=%u, "
				"using msg[2]=%u as dst_cpu\n", dst_cpu, dc);
			dst_cpu = (u32)dc;
		}
	} else {
		/* Remote CPU path: dst_cpu from msg+2 (u16) */
		u16 dc;

		if (from_user) {
			if (get_user(dc, (u16 __user *)(msg_ptr + 2)))
				return -EFAULT;
		} else {
			dc = *(u16 *)(msg_ptr + 2);
		}
		dst_cpu = (u32)dc;
	}

	/* ── Step 3: CPU-reset and range check ── */
	if (IsCPUReset(dst_cpu))
		return -512;

	if (dst_cpu > 1)
		return -EINVAL;

	/* ── Step 4: get share sequence and seq socket ── */
	share_seq_w = (u8 *)getShareSeqW(dst_cpu, target_cpu);
	if (!share_seq_w) {
		pr_err("cpu_comm: SendComm2CPUEx: no share_seq for dst=%u dir=%u\n",
		       dst_cpu, target_cpu);
		return -EINVAL;
	}

	seq_base = getSeq(dst_cpu);

	/*
	 * sem_ptr selection (from IDA):
	 *   target_cpu != 0 (remote): seq_base + 1036
	 *   target_cpu == 0 (local):  seq_base + 8
	 */
	if (target_cpu)
		sem_ptr = (u8 *)seq_base + 1036;
	else
		sem_ptr = (u8 *)seq_base + 8;

	/* ── Step 5: acquire sequence semaphore ── */
	/* DEBUG: use timeout instead of interruptible.  Without MIPS
	 * ACK the sem is never released, so bypass after 100ms. */
	ret = cpu_comm_sem_down_timeout((void *)sem_ptr, msecs_to_jiffies(100));
	if (ret) {
		pr_info_ratelimited("cpu_comm: sem timeout, bypassing (cpu=%u)\n",
				    dst_cpu);
		ret = 0;
	}

	/* Re-check reset status after acquiring semaphore */
	if (IsCPUReset(dst_cpu)) {
		ret = -512;
		goto out_up_sem;
	}

	/* ── Step 6: check isCPUAppReady ── */
	if (!isCPUAppReady(dst_cpu)) {
		ret = -1;
		goto out_up_sem;
	}

	{
		int spins = 0;
		while (fifo_isNearlyFull((u32 *)(share_seq_w + 32), 1)) {
			schedule();
			if (++spins > 100) {
				pr_err("DBG1: FIFO nearly full >100 spins, bailing\n");
				ret = -EBUSY;
				goto out_up_sem;
			}
		}
	}
	/* ── Step 8: dequeue a free call slot from the pool ──
	 *
	 * Stock behaviour: fresh Comm_GetFreeCall per send. MIPS's
	 * Comm_ReleaseFreeCall enqueues the slot back after processing,
	 * working end-to-end since the session-K roundtrip fixes.
	 *
	 * The previous cache hack (keep slot cached for lifetime of module)
	 * collided with MIPS's release: GetFreeCall ran once → no dequeue →
	 * MIPS release on every cycle → FreeCall FIFO overflow after 1 cycle.
	 *
	 * ════════════════════════════════════════════════════════════════
	 * TODO: SLOT-RELEASE-WAIT — proper kernel-side fix
	 * ════════════════════════════════════════════════════════════════
	 * The 100-spin poll below is a workaround. The 21-slot FreeCall pool
	 * at (share_seq_w + 120) is replenished MIPS-side via
	 * Comm_ReleaseFreeCall after BG_Thread processes a call. With
	 * back-to-back ARM sends (no userspace throttle) the pool drains
	 * after ~19-21 calls → -EBUSY → IOCTL_CALL maps to -EFAULT.
	 *
	 * Stock kernel uses a semaphore-based wait pattern (per IDA RE @
	 * 0x3c40 in stock cpu_comm_dev.ko): MIPS Comm_ReleaseFreeCall
	 * does sem_up on s_CommSockt[1248*remote+235] (sock byte offset 940);
	 * stock SendComm2CPUEx-equivalent does sem_down on the same sem
	 * before allocating a slot, so it blocks correctly when pool is
	 * empty and wakes immediately when a slot is released — no busy
	 * spin, no false EBUSY.
	 *
	 * We currently rely on the 100-spin poll + userspace throttle in
	 * hy310-hdmird (CALL_GAP_MS=500). Reference: HANDOFF-IPC-SESSION-
	 * 20260503-W.md "FreeCall FIFO drain" + session-K-night handoff.
	 *
	 * Proper fix shape:
	 *   - Replace the 100-spin loop with cpu_comm_sem_down_timeout on
	 *     the slot-release sem (offset +235 in s_CommSockt[remote]),
	 *     long timeout (~5s).
	 *   - Verify Comm_ReleaseFreeCall (cpu_comm_channel.c:574) already
	 *     does sem_up on the matching sem on the release path —
	 *     looks like yes per the comment "sock[235] (sem count)".
	 *   - Remove userspace throttle in hdmird main.cpp once verified.
	 * ════════════════════════════════════════════════════════════════
	 */
	{
		int spins = 0;
		do {
			call_slot = (u8 *)Comm_GetFreeCall(share_seq_w);
			if (!call_slot) {
				schedule();
				if (++spins > 100) {
					pr_err("DBG2: no free slot >100 spins (FreeCall FIFO drain — see SLOT-RELEASE-WAIT TODO)\n");
					ret = -EBUSY;
					goto out_up_sem;
				}
			}
		} while (!call_slot);
	}
	/* ── Step 9: save slot_index before copy overwrites it ── */
	slot_idx = *(u16 *)(call_slot + 4);

	/* ── Step 10: copy message into slot ── */
	if (from_user) {
		if (copy_from_user(call_slot, (void __user *)msg_ptr, COMM_MSG_SIZE)) {
			ret = -EFAULT;
			goto out_up_sem;
		}
	} else {
		memcpy(call_slot, (void *)msg_ptr, COMM_MSG_SIZE);
	}

	/* ── Step 11: barrier + restore slot_idx ── */
	dmb(ish);
	*(u16 *)(call_slot + 4) = slot_idx;

	/* ── Step 12: set msg_type (1=local-call, 2=remote-call) ── */
	msg_type = target_cpu ? 2 : 1;
	*(u16 *)(call_slot + 10) = msg_type;

	/* ── Step 13: verify slot address matches FIFO table ── */
	if (call_slot != (share_seq_w + 104 * (u32)slot_idx + 360)) {
		pr_err("cpu_comm: SendComm2CPUEx: slot address mismatch!\n");
		return -EINVAL;
	}

	/* ── Step 14: direction from share_seq header byte [2] ── */
	direction = share_seq_w[2];

	wait_ptr = NULL;

	if (direction == 0) {
		/*
		 * Forward call path: assign session ID and populate
		 * routing fields from FindRoutine result (or re-read
		 * for the remote path which didn't call FindRoutine).
		 */
		u32 cur_cpu = getCurCPUID(0);

		/* Generate session ID */
		s_CallSessionId = (s_CallSessionId + 1) & SESSION_ID_MASK;
		session_id = s_CallSessionId | (cur_cpu << 30);

		if (routine_found) {
			/*
			 * Local path: reuse the FindRoutine result from step 2.
			 * Populate routing fields in the call slot from the buf:
			 *   [0]: routine_name (u16)
			 *   [2]: routine_dst_cpu (u16)
			 *   [16],[24]: dst cpu expanded to u32
			 */
			*(u16 *)(call_slot +  0) = *(u16 *)(routine_find_buf + 0); /* routine_name */
			*(u16 *)(call_slot +  2) = *(u16 *)(routine_find_buf + 2); /* routine_dst_cpu */
			/*
			 * HY310 2026-04-22: call_slot[16..19] is the "pid" field
			 * read by MIPS Comm_Add2NewCallFifo to compute ChanPID key:
			 *   ChanPID = (16 * call_slot[16..19]) | (call_slot[0..1] & 0xF)
			 * MIPS registers channels with ChanPID = 16*routine.pid | chan,
			 * where routine.pid = u32 at routine_find_buf+4 (e.g. 0x8B8F3160).
			 * Previous code wrote (u8)routine_find_buf[2] = 1 (dst_cpu),
			 * causing ChanPID=16 on ARM vs 0xB8F31600 on MIPS → mismatch.
			 */
			*(u32 *)(call_slot + 16) = *(u32 *)(routine_find_buf + 4);
			*(u32 *)(call_slot + 24) = (u32)(u8)routine_find_buf[2];   /* dst cpu (u8 expanded) */
		}

		/* Fields set regardless of local/remote */
		*(u32 *)(call_slot + 12) = session_id;	/* session_id field */
		*(u32 *)(call_slot + 28) = (u32)current->pid;

		flags = *(u16 *)(call_slot + 6);

		if ((flags & MSG_FLAG_NOTIFY) && !(flags & MSG_FLAG_RETURN_ACK)) {
			/* Fire-and-forget: no wait object needed */
			*(u64 *)(call_slot + 32) = 0ULL;
		} else {
			/* Normal call or return-ack: allocate a wait object */
			u32 wait_fifo_base = getSeq(dst_cpu) + 136;

			do {
				ret = GetFreeWaitComm((void *)wait_fifo_base, &wait_ptr);
				if (ret || !wait_ptr) {
					schedule();
					wait_ptr = NULL;
				}
			} while (!wait_ptr);

			*wait_ptr       = session_id;
			*(wait_ptr + 1) = (u32)current->pid;

			AddtoWaitComm((void *)(getSeq(dst_cpu) + 40), (u32)wait_ptr);

			*(u64 *)(call_slot + 32) = (u64)(u32)wait_ptr;

			/* Return-ack path: also fill offsets 60/64 */
			if (flags & MSG_FLAG_RETURN_ACK) {
				*(u32 *)(call_slot + 60) = session_id;
				*(u32 *)(call_slot + 64) = (u32)wait_ptr;
			}
		}
	} else {
		/* Return direction: read session_id from what was already in slot */
		session_id = *(u32 *)(call_slot + 12);
		flags      = *(u16 *)(call_slot + 6);
	}

	/* ── Step 15: second barrier ── */
	dmb(ish);

	/* ── Step 16: copy-back session_id so caller can find the wait object ── */
	if (from_user) {
		if (copy_to_user((void __user *)msg_ptr, call_slot, COMM_MSG_SIZE)) {
			ret = -EFAULT;
			goto out_cleanup_wait;
		}
	} else if (direction == 0) {
		/* Kernel path: write session_id AND resolved dst_cpu back so
		 * CPUComm_CallEx can locate the wait obj in the correct list.
		 * dst_cpu here is the result of FindRoutine/thread_id override
		 * and may differ from the hint the caller wrote at msg[2]. */
		*(u32 *)(msg_ptr + 12) = session_id;
		*(u16 *)(msg_ptr + 2)  = (u16)dst_cpu;
	}

	/* ── Step 17: mark "sent" in flags2 ── */
	*(u16 *)(call_slot + 10) |= MSG_FLAG_SENT;

	/* ── Step 18: dispatch via SendCommLow ── */
	SendCommLow(share_seq_w, (u32)slot_idx, msg_type, session_id,
		    (u32)(sem_ptr + 16));

	/* ── Step 19: wait for ACK (500 jiffies) ── */
	ret = cpu_comm_sem_down_timeout((void *)(sem_ptr + 16),
			   SEND_TIMEOUT_JIFFIES);
	if (ret) {
		pr_err("cpu_comm: SendComm2CPUEx: ACK timeout (cpu=%u session=0x%08x)\n",
		       dst_cpu, session_id);

		/* Clear "sent" bit from share-seq status */
		share_seq_w[8] &= ~MSG_FLAG_SENT;

		/* Release the wait object if we allocated one */
		if (direction == 0 && wait_ptr) {
			u8 tmp_buf[WAIT_ENTRY_SIZE];

			GetWaitbySessionId((void *)(getSeq(dst_cpu) + 40),
					   *wait_ptr, tmp_buf);
			ReleaseWaitComm(dst_cpu, (u32)wait_ptr);
		}
	} else {
		/* Advance CMD-FIFO Rd on ARM side — MIPS's update via KSEG0 cache
		 * never propagates to ARM DDR view. Without this: -EBUSY at 19. */
		fifo_ItemRdNext((u32 *)(share_seq_w + 32));
	}

out_cleanup_wait:
	/* fall-through to sem release on error */

out_up_sem:
	cpu_comm_sem_up((void *)sem_ptr);
	return ret;
}

int SendComm2CPU(u32 msg_ptr, int from_user)
{
	u16 target;

	if (from_user) {
		if (get_user(target, (u16 __user *)(msg_ptr + 2)))
			return -EFAULT;
	} else {
		target = *(u16 *)(msg_ptr + 2);
	}

	return SendComm2CPUEx(msg_ptr, target, from_user);
}

/* ── SendAckLow — Send acknowledgement ────────────────────── */

/*
 * SendAckLow — Send ACK for received message.
 *
 * From IDA @ 0xd9b0. DIFFERENT from SendCommLow!
 * Writes to ACK-specific offsets in the share sequence:
 *   +104: sequence number (a2)
 *   +105: status/type (a3)
 *   +108: param (a4)
 *   +112: sem_ptr (a5, stored as u64)
 *
 * Then dispatches as type 2 (CALL_ACK) or 3 (RETURN_ACK) based on
 * the share sequence direction.
 *
 * Previously this was incorrectly delegating to SendCommLow which
 * writes to offsets 16/20/24/4/8 instead — completely wrong!
 */
void SendAckLow(void *data, u32 a2, u8 a3, u32 a4, u32 a5)
{
	u8 *seq = data;
	u8 src, dst, dir;
	u32 cur_cpu;

	if (WARN_ON(!seq))
		return;

	src = seq[1];	/* source CPU */
	dst = seq[0];	/* destination CPU */
	cur_cpu = getCurCPUID(0);

	if (WARN_ON(src != cur_cpu))
		return;
	if (WARN_ON(seq[2] > 1))
		return;

	/* Validate: ACK sent flag must not already be set */
	if (seq[105] & 4) {
		pr_err("cpu_comm: SendAckLow: ACK sent flag already set!\n");
		return;
	}

	/* Write ACK metadata to ACK-specific offsets */
	seq[104] = (u8)a2;		/* sequence number */
	*(u32 *)(seq + 108) = a4;	/* param */
	seq[105] = a3;			/* status/type */
	*(u64 *)(seq + 112) = (u64)a5;	/* sem_ptr */

	dmb(ish);

	dir = seq[2];

	if (dst == src) {
		/* Local ACK — dispatch directly */
		u32 ack_type = (dir == 0) ? MSG_TYPE_CALL_ACK : MSG_TYPE_RETURN_ACK;

		queueAction(ack_type, getCurCPUID(0));
	} else {
		/* Remote ACK — set sent flag and notify MIPS */
		seq[105] |= 4;	/* sent flag */
		dmb(ish);

		while (!(seq[105] & 4))
			;

		{
			u32 ack_type = (dir == 0) ? MSG_TYPE_CALL_ACK
						  : MSG_TYPE_RETURN_ACK;

			/* Same encoding as forward path: high-16=sender, low-16=comm_type.
			 * MIPS dispatches case 2 (CALL_ACK) -> sub_8B11F4F0 or
			 * case 3 (RETURN_ACK) -> sub_8B11F6D8. */
			sunxi_cpu_comm_send_intr_to_mips(0 /*sender=ARM*/, ack_type, -1);
		}
	}
}

/* ── Message handlers — called from cpu_comm_msg_cb ────────── */

/*
 * These handle incoming messages from MIPS.
 * The msg_cb receives a 4-byte type from RPMSG, maps it to a handler.
 *
 * Each handler:
 * 1. Reads from the appropriate FIFO
 * 2. Processes the message
 * 3. Signals waiting threads if applicable
 */

/*
 * Stock-pattern memory-coherency check.
 *
 * Stock cpu_comm_dev.ko (verified via IDA disasm @ 0xcc94) does NOT
 * directly process incoming MIPS messages in IRQ-context. It first
 * synchronises with MIPS' L1-cache via DMB ISHST + wait-flag-cleared loop,
 * THEN defers via queueAction. Without this, ARM races MIPS' cache flush
 * and reads stale shared-mem data → garbage pointers leak into kernel
 * memory, observed as kernel-MM corruption with MIPS-VA (0x8B...) leaking
 * into adjacent task_structs (2026-05-03 hdmird crash).
 *
 * Stock pseudocode:
 *   r0 = getShareSeqR(cpu, direction)
 *   r3 = byte[r0+8]
 *   if (r3 & 4) {
 *       r3 &= ~4; byte[r0+8] = r3;
 *       dmb(ishst);
 *       while (byte[r0+8] & 4) ;
 *       queueAction(direction, cpu);
 *   }
 */
/*
 * Stock disasm reveals different flag offsets per message type:
 *   CALL       (dir=0, offset +8)    queueAction type 0
 *   RETURN     (dir=1, offset +8)    queueAction type 1
 *   CALL_ACK   (dir=0, offset +105)  queueAction type 2
 *   RETURN_ACK (dir=1, offset +105)  queueAction type 3
 */
static inline bool cpu_comm_sync_mips_cache(int cpu, int direction, u32 flag_offset)
{
	u32 share_seq_r = getShareSeqR(cpu, direction);
	u8 *flag_byte;

	if (!share_seq_r)
		return false;

	flag_byte = (u8 *)(share_seq_r + flag_offset);
	if (!(*flag_byte & 4))
		return false;

	*flag_byte &= ~4u;
	dmb(ishst);
	while (*(volatile u8 *)flag_byte & 4)
		cpu_relax();
	return true;
}

void cpu_comm_handle_CPU2_call(int a1, int a2, int cpu)
{
	cpu_comm_trace_ipc("handle_call", MSG_TYPE_CALL, -1, cpu);
	if (cpu_comm_sync_mips_cache(cpu, 0, 8))
		queueAction(MSG_TYPE_CALL, cpu);
}

void cpu_comm_handle_CPU2_return(int a1, int a2, int cpu)
{
	cpu_comm_trace_ipc("handle_return", MSG_TYPE_RETURN, -1, cpu);
	/*
	 * sync_mips_cache is REQUIRED — verified empirically 2026-05-03:
	 * removing it caused immediate kernel-MM corruption (radix_tree_delete
	 * dereferencing 0x8b15b... MIPS-VA leaked into kernel data structures,
	 * cascade of __dabt_svc page-faults). The "silent drop" when bit-2 is
	 * not set is intentional protection — without MIPS' cache flush,
	 * share_seq fields are stale and contain MIPS-VA pointers that ARM
	 * cannot dereference safely.
	 *
	 * 2026-05-04 Y2: defer comm_Action via queueAction (workqueue) to
	 * match stock @ 0xcc94 → queueAction(1, cpu). Direct call ran the
	 * RETURN-handler in IRQ-context and was correlated with kernel-MM
	 * corruption / systemd PID 1 segfault under load.
	 */
	if (cpu_comm_sync_mips_cache(cpu, 1, 8))
		queueAction(MSG_TYPE_RETURN, cpu);
}

void cpu_comm_handle_CPU2_callACK(int a1, int a2, int cpu)
{
	/* ACK-types: skip cache-sync — ack_action already does the
	 * inverted-bit-check from session-K (memory feedback_inverted_bit_checks.md).
	 * Adding cache-sync here would clear the bit before ack_action sees it. */
	cpu_comm_trace_ipc("handle_call_ack", MSG_TYPE_CALL_ACK, -1, cpu);
	queueAction(MSG_TYPE_CALL_ACK, cpu);
}

void cpu_comm_handle_CPU2_returnACK(int a1, int a2, int cpu)
{
	cpu_comm_trace_ipc("handle_return_ack", MSG_TYPE_RETURN_ACK, -1, cpu);
	queueAction(MSG_TYPE_RETURN_ACK, cpu);
}

/* ── comm_Action — Dispatch incoming message by type ───────── */

/*
 * From IDA @ 0xe00c: Stock takes a single struct_ptr argument and reads
 * cpu from *(ptr+36) and type from *(ptr+40). Our version takes (type, cpu)
 * directly since our callers (cpu_comm_handle_CPU2_*) already have these
 * as separate values. The dispatch logic is identical.
 *
 * IDA dispatch:
 *   type 0,1 → command_action(cpu, type)
 *   type 2   → ack_action(cpu, 0)
 *   type 3   → ack_action(cpu, 1)
 */
void comm_Action(int type, int cpu)
{
	cpu_comm_trace_ipc("dispatch", type, -1, cpu);

	switch (type) {
	case MSG_TYPE_CALL:
		command_action((u32)cpu, 0);
		break;
	case MSG_TYPE_RETURN:
		command_action((u32)cpu, 1);
		break;
	case MSG_TYPE_CALL_ACK:
		ack_action((u32)cpu, 0);
		break;
	case MSG_TYPE_RETURN_ACK:
		ack_action((u32)cpu, 1);
		break;
	default:
		pr_err("cpu_comm: comm_Action: unknown type %d\n", type);
		return;
	}
}

/* ── command_action — Process incoming call from MIPS ──────── */

/*
 * From IDA @ 0xdbf8. Handles incoming messages from a remote CPU.
 *
 * Parameters (passed via comm_Action → data cast to u32):
 *   a1 (data): CPU index of the remote sender
 *   a2: direction (0=call, 1=return)
 *
 * For direction 0 (call): reads the call entry from the MIPS→ARM shared
 * sequence, dispatches it (via Comm_Add2NewCallFifo for low-priority or
 * Comm_Add2Call2WQ for high-priority), then sends an ACK back.
 *
 * For direction 1 (return): handles a return from a previous call we made
 * to MIPS. Signals the waiting semaphore to unblock SendComm2CPUEx.
 */
void command_action(u32 remote_cpu, u32 direction)
{
	u32 share_seq_r;	/* read sequence (messages FROM remote) */
	u32 share_seq_w;	/* write sequence (for sending ACK TO remote) */
	u32 seq_idx;		/* current sequence index */
	u32 entry_base;		/* virtual address of the call entry */
	u16 component_id;
	u32 session_id;

	if (WARN_ON(remote_cpu > 1))
		return;

	share_seq_r = getShareSeqR(remote_cpu, direction);
	share_seq_w = getShareSeqW(remote_cpu, direction);
	if (!share_seq_r || !share_seq_w)
		return;

	/* Read current sequence index */
	seq_idx = *(u8 *)(share_seq_r + 16);	/* max_items field */
	if (seq_idx >= FIFO_DEFAULT_CAP) {
		pr_warn("cpu_comm: command_action: seq_idx %u out of range\n", seq_idx);
		return;
	}

	/* 2026-05-04 Y2: bit-2 check + clear is already done by
	 * cpu_comm_sync_mips_cache() wrapper in cpu_comm_handle_CPU2_call/
	 * _return. Re-checking here always failed (flag was just consumed)
	 * → "no pending msg" → all 28 hdmird calls returned -EFAULT.
	 * Removed the redundant block; sync_mips_cache is the single
	 * authoritative gate. */

	/* Get the call entry at the current sequence index */
	entry_base = share_seq_r + 104 * seq_idx + 360;

	/* Mark as received (set bit 3 on flags at entry + 10) */
	*(u16 *)(entry_base + 10) |= 8;

	/* Read component_id from the call entry */
	component_id = *(u16 *)(entry_base + 2);

	/* Verify destination matches us */
	if (component_id != getCurCPUID(0)) {
		/* Destination mismatch — but this may be the component ID,
		 * not CPU ID. Stock asserts here. Log and continue.
		 */
		pr_debug("cpu_comm: command_action: comp_id %u (seq_idx=%u)\n",
			 component_id, seq_idx);
	}

	/* Data memory barrier before processing */
	dmb(ish);

	/* Swap src/dst in the entry to reflect that we're now the handler */
	*(u16 *)(entry_base + 2) = remote_cpu;

	if (direction == 0) {
		/*
		 * Incoming CALL from MIPS.
		 * Dispatch based on component priority (entry + 0 = component_id word):
		 *   ≤4: enqueue via Comm_Add2NewCallFifo (normal priority)
		 *   >4: enqueue via Comm_Add2Call2WQ (high priority / workqueue)
		 */
		u16 entry_cmd = *(u16 *)(entry_base);
		{
			u32 comp_id = *(u32 *)(entry_base + 40);
			u32 pid     = *(u32 *)(entry_base + 52);
			u16 param_cnt = *(u16 *)(entry_base + 8);
			u32 p0 = *(u32 *)(entry_base + 44);
			u32 p1 = *(u32 *)(entry_base + 48);
			u32 p2 = *(u32 *)(entry_base + 56);
			u32 p3 = *(u32 *)(entry_base + 60);
			pr_info_ratelimited(
				"cpu_comm: RX-CALL from cpu=%u cmd=0x%x comp_id=0x%08x pid=%u params=%u [%08x %08x %08x %08x]\n",
				remote_cpu, entry_cmd, comp_id, pid, param_cnt,
				p0, p1, p2, p3);
		}
		/* DELIVER-USERSPACE-PROTO 2026-05-07: copy CALL to any open
		 * /dev/cpu_comm fd before kernel-side dispatch. Mainline has
		 * no per-channel dispatch thread, so without this entry_cmd<=4
		 * messages (e.g. MipsHalCallback_SignalChange) sit in the channel
		 * FIFO unread. Direct deliver bypasses that path entirely. */
		cpu_comm_userspace_deliver((const void *)entry_base);

		if (entry_cmd <= 4)
			Comm_Add2NewCallFifo(
				(void *)((u8 *)pcpu_comm_dev + 48),
				(u32 *)(share_seq_r + 32),
				entry_base);
		else
			Comm_Add2Call2WQ((void *)entry_base);

		/* Increment incoming call counter */
		if (pcpu_comm_dev)
			*(u32 *)((u8 *)pcpu_comm_dev + 4) += 1;
	} else {
		/*
		 * Incoming RETURN from MIPS (response to our previous call).
		 * Add to return FIFO, then wake up the waiting thread.
		 */
		/*
		 * First arg is the FIFO pointer (from share_seq + 32,
		 * which is the CallCmd/ReturnCmd FIFO embedded in the
		 * share sequence). Pass as pointer, not dereferenced.
		 */
		AddReturn2Fifo((u32 *)(share_seq_r + 32),
			       (void *)(share_seq_r + 104 * seq_idx + 360));

		if (pcpu_comm_dev) {
			*(u32 *)pcpu_comm_dev += 1;
			*(u32 *)((u8 *)pcpu_comm_dev + 1628) += 1;
		}

		/* Find and signal the waiting thread */
		session_id = *(u32 *)(entry_base + 12);
		if (*(u16 *)(entry_base + 6) & MSG_FLAG_RETURN_ACK) {
			/* Has return-ack flag — find wait by session ID */
			u32 seq_base = getSeq(remote_cpu);
			u32 wait_obj = 0;

			FindWaitBySessionId((void *)(seq_base + 40),
					    session_id, (u32 **)&wait_obj);
			if (!wait_obj) {
				pr_err("cpu_comm: command_action: wait not found for session 0x%x\n",
				       session_id);
    return;
			}

			/* Verify session match */
			if (*(u32 *)(entry_base + 12) != *(u32 *)wait_obj) {
				pr_err("cpu_comm: command_action: session mismatch\n");
    return;
			}

			dmb(ish);
			*(u16 *)(entry_base + 10) |= 0x40;
			cpu_comm_sem_up((void *)(wait_obj + 8));
		} else {
			/* No return-ack — use embedded wait pointer */
			u64 wait_ptr = *(u64 *)(entry_base + 32);

			if (!wait_ptr) {
				pr_err("cpu_comm: command_action: no wait pointer\n");
    return;
			}

			/* Y2-followup: skip if MIPS-internal handle leaked
			 * into wait_ptr — would cause kernel-MM corruption. */
			if (cpu_comm_is_mips_va((u32)wait_ptr)) {
				pr_warn_ratelimited(
					"cpu_comm: command_action: skip MIPS-VA wait_ptr=0x%08x session=0x%x\n",
					(u32)wait_ptr,
					*(u32 *)(entry_base + 12));
				return;
			}

			/* Verify session match */
			if (*(u32 *)(entry_base + 12) != *(u32 *)(u32)wait_ptr) {
				pr_err("cpu_comm: command_action: session mismatch (embedded)\n");
    return;
			}

			dmb(ish);
			*(u16 *)(entry_base + 10) |= 0x40;
			cpu_comm_sem_up((void *)((u32)wait_ptr + 8));
		}
	}

	/* Reset sequence index and send ACK */
	*(u8 *)(share_seq_r + 16) = 20;
	SendAckLow((void *)share_seq_w,
		   *(u8 *)(share_seq_r + 16),
		   *(u8 *)(share_seq_r + 8),
		   *(u32 *)(share_seq_r + 20),
		   *(u32 *)(share_seq_r + 24));
}

/* ── ack_action — Process ACK from MIPS ────────────────────── */

/*
 * From IDA @ 0xc9bc: ack_action(u32 cpu, u32 direction)
 *
 * Gets the share sequence for the given CPU pair using the DIRECTION
 * parameter (0=call-ack, 1=return-ack). This is critical — using the
 * wrong direction signals the wrong semaphore.
 *
 * Validates status, then calls up() on the semaphore at offset 112
 * to wake up the thread waiting in SendCommLow/SendComm2CPUEx.
 */
void ack_action(u32 cpu, u32 direction)
{
	u32 share_seq;

	if (WARN_ON(cpu > 1))
		return;
	if (WARN_ON(direction > 1))
		return;

	/* Get share sequence using the correct DIRECTION (BUG-1 fix) */
	share_seq = getShareSeqR(cpu, direction);
	if (!share_seq)
		return;

	/* 2026-04-21: Logic inverted from original RE.
	 * MIPS's SendAckLow sets bit 2 in status2 BEFORE triggering the msgbox
	 * IRQ — the bit means "ACK has been deposited by sender". ARM must:
	 *   1. Confirm bit IS set (else: spurious IRQ, nothing to deliver).
	 *   2. Clear the bit (consume the ACK).
	 *   3. Signal the waiter.
	 * Stock firmware presumably had a prior consumer that cleared the bit
	 * before ack_action ran, making the original "must not be set" check
	 * logically consistent. We have no such consumer. */
	{
		u8 stat2 = *(u8 *)(share_seq + 105);

		if (!(stat2 & 4)) {
			pr_info_ratelimited("cpu_comm: ack_action: no ACK pending (stat2=0x%02x cpu=%u dir=%u)\n",
					    stat2, cpu, direction);
			return;
		}
		*(u8 *)(share_seq + 105) = stat2 & ~4;
		dmb(ish);
	}

	/* Signal the semaphore at offset 112 in the share sequence.
	 * This wakes up SendCommLow which is waiting for the ACK.
	 */
	cpu_comm_sem_up((void *)(unsigned long)*(u32 *)(share_seq + 112));
}

/* ── comm_WorkAction — Work queue dispatch via actionFuncs table ── */

/*
 * From IDA @ 0x6f4. Called as the work_struct callback for message
 * processing. Stock pre-allocates 8 work_structs at fixed offsets in
 * pcpu_comm_dev (one per cpu × type combo). Each work-struct embeds
 * its own (cpu, prio) so the handler reads them as instance state.
 *
 * 2026-05-04 Y2: real workqueue dispatch (was a stub that ran direct).
 */
struct cpu_comm_action_work {
	struct work_struct work;
	u32 cpu;
	u32 prio;
};

static struct cpu_comm_action_work s_action_work[2][4];
static bool s_action_work_inited;

void comm_WorkAction(struct work_struct *work)
{
	struct cpu_comm_action_work *cw =
		container_of(work, struct cpu_comm_action_work, work);

	if (WARN_ON(cw->cpu > 1))
		return;
	if (WARN_ON(cw->prio > 3))
		return;

	comm_Action(cw->prio, cw->cpu);
}

void cpu_comm_init_action_work(void)
{
	int cpu, type;

	if (s_action_work_inited)
		return;
	for (cpu = 0; cpu < 2; cpu++) {
		for (type = 0; type < 4; type++) {
			INIT_WORK(&s_action_work[cpu][type].work,
				  comm_WorkAction);
			s_action_work[cpu][type].cpu = cpu;
			s_action_work[cpu][type].prio = type;
		}
	}
	s_action_work_inited = true;
}

/* ── queueAction — Queue a work item for deferred dispatch ──── */

/*
 * From IDA @ 0xcad0. Defers comm_Action to the kernel workqueue, so
 * the dispatch (incl. command_action / ack_action) runs in process
 * context instead of IRQ context. Stock pattern, mirrors queue_work_on
 * call inside stock @ 0xcbac.
 *
 * @direction: message type / priority (0=CALL, 1=RETURN, 2=ACK, 3=RET_ACK)
 * @cpu:       source CPU id (0=ARM, 1=MIPS)
 *
 * NOTE on coalescing: schedule_work() is a no-op if the work is already
 * pending. MIPS sends one event per (cpu, type) at a time and waits for
 * ARM ACK before the next, so at most one is pending → no coalescing
 * loss in practice. Each (cpu, type) gets its own work_struct slot.
 */
int queueAction(u32 direction, u32 cpu)
{
	if (cpu > 1 || direction > 3) {
		pr_err("cpu_comm: queueAction: bad args cpu=%u dir=%u\n",
		       cpu, direction);
		return -EINVAL;
	}
	if (!s_action_work_inited)
		cpu_comm_init_action_work();
	schedule_work(&s_action_work[cpu][direction].work);
	return 0;
}

/* ── Message callback from RPMSG layer ─────────────────────── */

/*
 * Called by the built-in sunxi_cpu_comm RPMSG client when a
 * 4-byte message arrives from MIPS via msgbox.
 *
 * @type:      message type (0=call, 1=return, 2=callACK, 3=returnACK)
 * @direction: source direction (remapped: 0→0, 2→1, else→2)
 */
void cpu_comm_msg_cb(int type, int direction)
{
	int cpu;

	/* Map direction to CPU ID */
	if (direction == 0)
		cpu = 0;
	else if (direction == 2)
		cpu = 1;
	else
		cpu = 2;

	cpu_comm_trace_ipc("msg_cb", type, direction, cpu);

	switch (type) {
	case MSG_TYPE_CALL:
		cpu_comm_handle_CPU2_call(-1, 0, cpu);
		break;
	case MSG_TYPE_RETURN:
		cpu_comm_handle_CPU2_return(-1, 0, cpu);
		break;
	case MSG_TYPE_CALL_ACK:
		cpu_comm_handle_CPU2_callACK(-1, 0, cpu);
		break;
	case MSG_TYPE_RETURN_ACK:
		cpu_comm_handle_CPU2_returnACK(-1, 0, cpu);
		break;
	default:
		pr_warn("cpu_comm: unknown msg type %d from dir %d\n",
			type, direction);
		break;
	}
}
