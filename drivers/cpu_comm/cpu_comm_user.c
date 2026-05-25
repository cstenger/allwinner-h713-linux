// SPDX-License-Identifier: GPL-2.0
/*
 * cpu_comm_user.c — Userspace delivery of MIPS→ARM CALL messages
 *
 * Adds a /dev/cpu_comm read()/poll() interface so that userspace
 * processes (e.g. hy310-hdmird) can observe incoming MIPS callbacks
 * (MipsHalCallback_SignalChange, HotPlug, etc.) that were previously
 * dropped because IOCTL_INSTALL_RT only registers a name+pid without
 * a kernel callback function pointer.
 *
 * Architecture:
 *   1. Each open() of /dev/cpu_comm allocates a per-fd context with
 *      its own ring buffer of incoming messages and a wait_queue.
 *   2. comm_CallWorkAction() (cpu_comm_rpc.c) hooks into
 *      cpu_comm_userspace_deliver() before the kernel-callback dispatch
 *      and pushes a copy of the 104-byte message to every open fd's
 *      queue, then wakes any blocked readers.
 *   3. read() returns one full COMM_MSG_SIZE (104B) message per call.
 *      poll() reports POLLIN when the queue is non-empty.
 *   4. Backpressure: the queue is bounded (32 entries); oldest is
 *      dropped on overflow with a rate-limited warning.
 *
 * The kernel still synchronously ACKs MIPS (SendComm2CPUEx) so that
 * MIPS does not stall waiting for a response. This delivery channel
 * is observation-only — userspace does not write a response back here.
 *
 * 2026-05-06 CC-night: built to unblock LVDS-source-switch flow that
 * needs SignalChange(state=3) to trigger the post-signal call sequence
 * (sessions 35-45 in stock_rpc_HDMI_SWITCHED.txt).
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/list.h>
#include "cpu_comm.h"

#define USER_QUEUE_MAX	32

struct cpu_comm_msg_node {
	struct list_head link;
	u8 msg[COMM_MSG_SIZE];
};

struct cpu_comm_user_ctx {
	struct list_head global_link;	/* link in cpu_comm_user_ctxs */
	struct list_head queue;		/* of struct cpu_comm_msg_node */
	int		 queue_count;
	spinlock_t	 lock;
	wait_queue_head_t waitq;
	pid_t		 owner_pid;
	u64		 dropped;	/* overflow counter */
};

static LIST_HEAD(cpu_comm_user_ctxs);
static DEFINE_SPINLOCK(cpu_comm_user_ctxs_lock);

/* ── Public API ─────────────────────────────────────────── */

/*
 * cpu_comm_userspace_deliver — push a copy of a 104-byte CALL message
 * to every open fd's queue. Called from comm_CallWorkAction in IRQ-safe
 * (work queue) context — must be non-blocking.
 *
 * @msg: pointer to the 104-byte message (caller's local copy is fine)
 */
void cpu_comm_userspace_deliver(const void *msg)
{
	struct cpu_comm_user_ctx *ctx;
	unsigned long flags;
	int ctx_count = 0;
	u32 comp_id;

	if (!msg)
		return;

	/* DBG-USERSPACE-DELIVER 2026-05-07 */
	comp_id = *(const u32 *)((const u8 *)msg + 40);

	spin_lock_irqsave(&cpu_comm_user_ctxs_lock, flags);
	list_for_each_entry(ctx, &cpu_comm_user_ctxs, global_link)
		ctx_count++;
	pr_info("cpu_comm: userspace_deliver comp=0x%08x ctx_count=%d\n",
		comp_id, ctx_count);
	list_for_each_entry(ctx, &cpu_comm_user_ctxs, global_link) {
		struct cpu_comm_msg_node *node;
		unsigned long ctx_flags;

		node = kmalloc(sizeof(*node), GFP_ATOMIC);
		if (!node) {
			ctx->dropped++;
			continue;
		}
		memcpy(node->msg, msg, COMM_MSG_SIZE);

		spin_lock_irqsave(&ctx->lock, ctx_flags);
		if (ctx->queue_count >= USER_QUEUE_MAX) {
			/* Drop oldest entry to make room */
			struct cpu_comm_msg_node *old = list_first_entry(
				&ctx->queue, struct cpu_comm_msg_node, link);
			list_del(&old->link);
			ctx->queue_count--;
			ctx->dropped++;
			kfree(old);
		}
		list_add_tail(&node->link, &ctx->queue);
		ctx->queue_count++;
		spin_unlock_irqrestore(&ctx->lock, ctx_flags);

		wake_up_interruptible(&ctx->waitq);
	}
	spin_unlock_irqrestore(&cpu_comm_user_ctxs_lock, flags);
}
EXPORT_SYMBOL(cpu_comm_userspace_deliver);

/* ── file_operations hooks ─────────────────────────────── */

int cpu_comm_user_open(struct inode *inode, struct file *file)
{
	struct cpu_comm_user_ctx *ctx;
	unsigned long flags;

	(void)inode;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	INIT_LIST_HEAD(&ctx->queue);
	INIT_LIST_HEAD(&ctx->global_link);
	spin_lock_init(&ctx->lock);
	init_waitqueue_head(&ctx->waitq);
	ctx->owner_pid = current->pid;

	spin_lock_irqsave(&cpu_comm_user_ctxs_lock, flags);
	list_add_tail(&ctx->global_link, &cpu_comm_user_ctxs);
	spin_unlock_irqrestore(&cpu_comm_user_ctxs_lock, flags);

	file->private_data = ctx;
	return 0;
}

void cpu_comm_user_release(struct file *file)
{
	struct cpu_comm_user_ctx *ctx = file->private_data;
	struct cpu_comm_msg_node *node, *tmp;
	unsigned long flags;

	if (!ctx)
		return;

	spin_lock_irqsave(&cpu_comm_user_ctxs_lock, flags);
	list_del(&ctx->global_link);
	spin_unlock_irqrestore(&cpu_comm_user_ctxs_lock, flags);

	spin_lock_irqsave(&ctx->lock, flags);
	list_for_each_entry_safe(node, tmp, &ctx->queue, link) {
		list_del(&node->link);
		kfree(node);
	}
	ctx->queue_count = 0;
	spin_unlock_irqrestore(&ctx->lock, flags);

	kfree(ctx);
	file->private_data = NULL;
}

ssize_t cpu_comm_user_read(struct file *file, char __user *buf,
			   size_t count, loff_t *ppos)
{
	struct cpu_comm_user_ctx *ctx = file->private_data;
	struct cpu_comm_msg_node *node;
	unsigned long flags;
	int rc;

	(void)ppos;

	if (!ctx)
		return -EBADF;
	if (count < COMM_MSG_SIZE)
		return -EINVAL;

	for (;;) {
		spin_lock_irqsave(&ctx->lock, flags);
		if (!list_empty(&ctx->queue)) {
			node = list_first_entry(&ctx->queue,
						struct cpu_comm_msg_node, link);
			list_del(&node->link);
			ctx->queue_count--;
			spin_unlock_irqrestore(&ctx->lock, flags);
			break;
		}
		spin_unlock_irqrestore(&ctx->lock, flags);

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		rc = wait_event_interruptible(ctx->waitq,
					      !list_empty(&ctx->queue));
		if (rc)
			return rc;
	}

	if (copy_to_user(buf, node->msg, COMM_MSG_SIZE)) {
		kfree(node);
		return -EFAULT;
	}
	kfree(node);
	return COMM_MSG_SIZE;
}

unsigned int cpu_comm_user_poll(struct file *file, poll_table *wait)
{
	struct cpu_comm_user_ctx *ctx = file->private_data;
	unsigned int mask = 0;
	unsigned long flags;

	if (!ctx)
		return POLLERR;

	poll_wait(file, &ctx->waitq, wait);

	spin_lock_irqsave(&ctx->lock, flags);
	if (!list_empty(&ctx->queue))
		mask |= POLLIN | POLLRDNORM;
	spin_unlock_irqrestore(&ctx->lock, flags);

	return mask;
}
