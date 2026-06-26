// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Eyenix Corporation
 * Eyenix <support6@eyenix.com>
 *
 * Eyenix VDMA - core internal header
 *
 * Architecture:
 *   The VDMA stack splits into a shared core (enx-vdma-core) that owns
 *   cdev / session / buffer / submit lifecycle, and a set of per-HW
 *   drivers (font / dz / jpegenc / jpegdec) that register HW-specific
 *   callbacks via &struct enx_vdma_core_ops.
 */

#ifndef _ENX_VDMA_H_
#define _ENX_VDMA_H_

#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/debugfs.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#include <soc/eyenix/ecmm.h>

#include <enx-vdma-uapi.h>

/* Cap buffer ids so (id << PAGE_SHIFT) fits a 32-bit pgoff_t even on
 * 32-bit hosts with 4 KiB pages. */
#define VDMA_BUF_ID_MAX		((1u << 19) - 1)
#define VDMA_BUF_DEFAULT	(16)

/* DDR range definition for physical address validation in BUF_RAW cases. */
#define DDR_BASE_ADDR		(0x80000000)
#define DDR_MAX_SIZE		(0x40000000)

/* reg filed modify */
#ifndef FIELD_MODIFY
#define FIELD_MODIFY(reg_p, mask, val)   do { (*(reg_p)) = (((*(reg_p)) & ~(mask)) | FIELD_PREP((mask), (val))); } while(0)
#endif

struct enx_vdma_dev;
struct enx_vdma_sess;
struct enx_vdma_core_ops;

/*******************************************************************************
 *
 * VDMA Core data structures
 *
*******************************************************************************/

/**
 * struct enx_vdma_backing - Shared physical memory backing for VDMA buffers
 * @ref: Reference count managing the underlying memory lifecycle. Refs are
 *     held by &enx_vdma_buf views and outstanding EXPORT fds. Reaching
 *     zero releases the memory via Eyenix CMM.
 * @cmm: Pointer to the Eyenix CMM object. Owns all physical properties
 *     (addresses, page, size). Do not duplicate state; always query size
 *     via enx_vdma_backing_size().
 * @node: Global list node for tracking all active backings (enx_vdma_backings_all).
 *
 * A backing represents a single contiguous memory region, created once per
 * allocation. It is shared across one or more &enx_vdma_buf "views".
 *
 * Views are per-device, per-session handles to this backing. They are
 * created during initial ALLOC or via IMPORT (sharing an existing backing
 * through an EXPORT fd). Because IMPORT can cross device and process
 * boundaries, a single backing may be used simultaneously by multiple
 * devices (e.g., scaler and jpeg).
 *
 * The physical memory outlives individual views, persisting until the last
 * view dies and all EXPORT fds are closed.
 */
struct enx_vdma_backing {
	struct kref ref;
	struct eyenix_cmm_item *cmm;
	struct list_head node;		/* global enx_vdma_backings_all */
};

/**
 * struct enx_vdma_buf - Per-device, per-session view over a backing
 * @id: Identifier in the owning device's @buf_xa. Valid only within @dev.
 * @kind: View direction (ENX_BUF_SRC/DST). Independent of the backing;
 *     IMPORT may re-tag this (e.g., scaler DST imported as jpeg SRC).
 * @imported: True if created via IMPORT, false if ALLOC. Used for debugfs.
 * @backing: Shared physical memory. This view holds one ref on it. Physical
 *     queries must route through here (no duplicate state in view).
 * @dev: Owning &enx_vdma_dev. Fixed at creation; views do not migrate.
 * @owner: Session that created this view (ALLOC or IMPORT). NULL after FREE.
 *     The view itself may outlive this transition (mmap, in-flight job).
 * @owner_node: List node into &enx_vdma_sess.bufs.
 * @ref: View lifetime refcount (held by xa, mmap, in-flight jobs). Reaching
 *     zero destroys the view and drops the backing ref.
 *
 * Lifecycle summary:
 *   ALLOC         - new backing (kref=1) + new view in dev_A (kref=1)
 *   EXPORT        - backing kref++; new fd owns that ref
 *   IMPORT        - new view in dev_B (kref=1) + backing kref++
 *   FREE          - xa_erase + view kref--  (memory survives if backing kref > 0)
 *   close fd      - backing kref--
 *   session close - all owned views get xa_erase + kref--
 *   last ref      - ecmm_free + kfree
 *
 * The dual kref system ensures safe, order-independent teardown across all
 * producers and consumers.
 */
struct enx_vdma_buf {
	u32 id;
	u32 kind;							/* ENX_BUF_* (per-view) */
	bool imported;

	struct enx_vdma_backing *backing;	/* shared, kref'd */

	struct enx_vdma_dev *dev;
	struct enx_vdma_sess *owner;		/* creator/importer; NULL after FREE */
	struct list_head owner_node;

	struct kref ref;
};

/**
 * struct enx_vdma_job_buf - Job-specific wrapper for DMA buffers
 * @vbuf: Backing buffer object from XArray.
 *     NULL if the user submitted a direct physical address.
 * @paddr: Direct physical address provided by the user.
 *     For ID kind, the driver resolves the physical address via
 *     enx_vdma_buf_phys(@vbuf); for RAW it uses @paddr directly.
 * @size: Size of the buffer in bytes.
 * @data_offset: Byte offset into the backing buffer (per-view).
 * @kind: Buffer direction/type (e.g., ENX_BUF_RAW or ENX_BUF_ID).
 *
 * This structure abstracts the buffer's origin. The hardware driver can
 * determine whether to use enx_vdma_buf_phys(@vbuf) or the direct @paddr by
 * checking the value of @kind.
 */
struct enx_vdma_job_buf {
	struct enx_vdma_buf *vbuf;
	dma_addr_t paddr;
	size_t size;
	u32 data_offset;	/* byte offset into the backing buffer (per-view) */
	u32 kind;
};

/**
 * struct enx_vdma_job - Represents a single hardware operation unit
 * @id: Unique identifier allocated from the device's job XArray.
 * @user_token: Opaque token provided by user-space to track async completions.
 * @submitter: The session context that originated this job.
 * @result: Execution status (0 on success, negative error code on failure).
 * @done: True if the hardware execution has finished.
 * @flags: Job submission flags (e.g., ENX_SUBMIT_ASYNC or ENX_SUBMIT_SYNC).
 * @submit_time: Timestamp recorded at submission for async jobs, used in debugfs.
 * @blits: Kernel-space snapshot of function-specific blit parameters.
 * @blit_size: Size of a single blit array element in bytes.
 * @work: Work struct for scheduling this job onto the VDMA workqueue.
 * @node: List node for chaining jobs (e.g., in a pending or active list).
 * @wq: Wait queue for synchronous submissions or the WAIT ioctl.
 * @dst: Destination buffer wrapper.
 * @src_count: Number of valid source buffers in the @src array.
 * @src: Flexible array of source buffer wrappers.
 *
 * This structure encapsulates everything needed to execute and track a VDMA
 * operation. Because it uses a flexible array member (@src), it must be
 * allocated dynamically using struct_size(). The @blits pointer holds a
 * safe copy of the user's parameters, ensuring the hardware driver does not
 * access user-space memory directly during execution.
 */
struct enx_vdma_job {
	u32 id;
	u64 user_token;
	struct enx_vdma_sess *submitter;
	int result;
	bool done;
	u32	flags; /* ENX_SUBMIT_* - sync or async */

	/* For debugfs: tracks queue/processing time for ASYNC jobs. */
	ktime_t submit_time;

	/* Function-specific blit array snapshot. Sized blit_size * src_count. */
	void *blits;
	size_t blit_size;

	struct work_struct work;
	struct list_head node;
	wait_queue_head_t wq;

	struct enx_vdma_job_buf	dst;
	u32 src_count;
	struct enx_vdma_job_buf	src[]; /* flexible array -> [src_count] */
};

/**
 * struct enx_vdma_sess - Per-file descriptor session context
 * @dev: Pointer to the parent VDMA device instance.
 * @lock: Mutex to serialize access and protect the session's internal lists.
 * @bufs: List of &struct enx_vdma_buf views owned by this session — both
 *     allocated locally and imported. Distinguished by buf->imported.
 * @jobs: List of pending or active &struct enx_vdma_job submitted by this session.
 * @poll_wq: Wait queue head for the poll() system call. Awakened when an async
 *     job belonging to this session completes.
 * @pid: Process ID of the task that opened this session (useful for debugging).
 * @sess_priv: Opaque pointer for the driver's per-session private data.
 *     Typically allocated and assigned during the &enx_vdma_core_ops.session_open()
 *     callback, and safely freed during session_release().
 * @dev_node: List node linking this session into dev->sessions.
 *     Inserted on enx_vdma_open() success, removed at start of
 *     enx_vdma_release(). Protected by dev->sessions_lock.
 *
 * This structure tracks all resources tied to a specific open file descriptor.
 * By keeping isolated lists for allocations, imports, and jobs, the core driver
 * can guarantee a safe and complete teardown of all resources when the file
 * is closed, even if the user-space process crashes unexpectedly.
 */
struct enx_vdma_sess {
	struct enx_vdma_dev	*dev;

	struct mutex lock;
	struct list_head bufs;		/* both alloc'd and imported views */
	struct list_head jobs;

	wait_queue_head_t poll_wq;
	pid_t pid;

	void *sess_priv;

	struct list_head dev_node;	/* linked into dev->sessions */
};

/**
 * struct enx_vdma_dev - Global device context for a VDMA hardware engine
 * @dev: Pointer to the underlying physical device (e.g., platform_device).
 * @cdev: Character device instance for user-space interfaces.
 * @devid: Allocated major and minor numbers for the character device.
 * @resource: Pointer to the physical memory resource descriptor.
 * @regs: I/O mapped virtual address for direct HW register access (MMIO).
 * @irq: Hardware interrupt number.
 * @buf_xa: Global XArray tracking all allocated &struct enx_vdma_buf objects.
 * @job_xa: Global XArray tracking all submitted &struct enx_vdma_job objects.
 * @hw_lock: Mutex protecting HW registers and global state transitions.
 * @wq: Dedicated ordered workqueue for serializing job execution.
 * @hw_wq: Wait queue for waiting on hardware completion interrupts.
 * @hw_idle: True if the hardware engine is currently free and not processing a job.
 * @irq_lock: protects SW state shared between ISR and process context (e.g., procfs, abort).
 *     Use spin_lock() in ISR, spin_lock_irqsave() in process ctx. Separate from
 *     @hw_lock (which serializes HW register programming).
 * @max_bufs: Maximum number of buffers allowed globally for this device.
 * @max_src: Maximum number of source buffers supported by this specific HW engine.
 * @dev_priv: Opaque pointer for the driver's per-device private data.
 * @ops: Function-specific HW callbacks, populated by enx_vdma_core_register().
 * @fops: File operations provided by the driver, populated during registration.
 * @node_name: Device node name (e.g., "vdma_scaler") exposed to user-space.
 * @dbg_dir: debugfs directory root for this device.(/sys/kernel/debug/enx_vdma/<node_name>)
 *     NULL when CONFIG_DEBUG_FS=n or creation failed.
 * @in_flight: Current queued + executing job count.
 *     Incremented in submit (just before queue_work), decremented in vdma_job_finish().
 * @bufs_active: Currently allocated (alive) &enx_vdma_buf objects.
 *     Reflects real memory occupancy, not cumulative.
 * @sessions_open: Currently open file descriptors on this device.
 * @stats_submits: Cumulative count of enx_vdma_submit() entries(any outcome).
 * @stats_sync_submits: Cumulative count of synchronous enx_vdma_submit() entries.
 * @stats_async_submits: Cumulative count of asynchronous enx_vdma_submit() entries.
 * @stats_submits_failed: Cumulative count of submits that returned non-zero.
 * @stats_completed: Cumulative count of finished jobs.
 *     (async - reched vdma_job_finish(); sync - returned from submit).
 * @stats_bufs_alloc: Cumulative successful buffer allocations.
 * @bufs_bytes_inuse: Sum of @size of all alive bufs (in bytes).
 *     Useful for CMA pressure monitoring.
 * @sessions: List head of currently open sessions;
 *     each &enx_vdma_sess.dev_node links here. Protected by @sessions_lock.
 * @sessions_lock: Mutex guarding @sessions list operations.
 *     Lock order: sessions_lock (outer) → vs->lock (inner).
 *     MUST NOT be acquired while holding vs->lock.
 *
 * This structure acts as the central hub for a specific VDMA hardware instance.
 * It manages the global lifecycle of buffers and jobs via XArrays, handles
 * asynchronous execution via a dedicated workqueue, and stores the callbacks
 * (@ops) required to delegate function-specific logic to the HW driver.
 *
 * (Atomic counters use relaxed semantics — they are advisory and do not
 * participate in synchronization. Use only for monitoring and debug.)
 */
struct enx_vdma_dev {
	struct device *dev;
	struct cdev cdev;
	dev_t devid;

	struct resource *resource;
	void __iomem *regs;
	int irq;

	struct xarray buf_xa;
	struct xarray job_xa;

	struct mutex hw_lock;
	struct workqueue_struct	*wq;
	wait_queue_head_t hw_wq;
	bool hw_idle;
	spinlock_t irq_lock;

	u32 max_bufs;
	u32 max_src;
	atomic_t buf_count;			/* alloc/import +1, free -1 */
	void *dev_priv;

	/* set by enx_vdma_core_register */
	const struct enx_vdma_core_ops *ops;
	const struct file_operations *fops;
	const char *node_name;

	/* Debug / monitoring (debugfs-exposed) */
	struct dentry *dbg_dir;
	atomic_t   in_flight;				/* queued + running jobs */
	atomic_t   bufs_active;				/* alive buf objects */
	atomic_t   sessions_open;			/* open fds */
	atomic64_t stats_submits;			/* total submit calls */
	atomic64_t stats_sync_submits;		/* sync submit calls */
	atomic64_t stats_async_submits;		/* async submit calls */
	atomic64_t stats_submits_failed;	/* failed before queue */
	atomic64_t stats_completed;			/* jobs that reached finish */
	atomic64_t stats_aborts;			/* cumulative abort calls */
	atomic64_t stats_bufs_alloc;		/* total alloc requests */
	atomic64_t bufs_bytes_inuse;		/* total alive buf bytes */

	/* Per-device session registry (for enumeration / debug walk).
	 * Lock order: sessions_lock (outer) → vs->lock (inner). */
	struct list_head sessions;
	struct mutex     sessions_lock;
};

/**
 * struct enx_vdma_submit_buf - Flexible buffer argument for job submission
 * @kind: Buffer type indicator (e.g., ENX_BUF_RAW or ENX_BUF_ID).
 * @data_offset: Byte offset into the buffer (used if kind == ID).
 * @size: Size of the raw buffer in bytes.
 * @id: XArray ID of a pre-allocated buffer (used if kind == ID).
 * @paddr: Direct physical/DMA address (used if kind == RAW).
 */
struct enx_vdma_submit_buf {
	u32 kind;
	u32 data_offset;
	size_t size;
	union {
		u32 id;				/* kind - ENX_BUF_ID */
		dma_addr_t paddr;	/* kind - ENX_BUF_RAW */
	};
};

/**
 * struct enx_vdma_submit_params - Generic parameters for VDMA job submission
 * @dst: Destination buffer argument.
 * @src: Pointer to an array of source buffer arguments. The HW driver
 *     must populate this array before calling the core submit function.
 * @src_count: Number of valid elements in the @src array.
 * @blits: Opaque pointer to an array of function-specific blit instructions
 *     (e.g., array of &struct en683_font_blit).
 * @blit_size: Exact byte size of a single blit instruction structure.
 * @user_token: Opaque 64-bit token provided by the user for async tracking.
 * @flags: Submission behavior flags (e.g., ENX_SUBMIT_SYNC or ASYNC).
 * @job_id_out: Output parameter. For ASYNC jobs, the core writes the newly
 *     allocated Job ID here so the driver can return it to user-space.
 *
 * This structure bridges the function-specific HW drivers with the generic
 * VDMA core. The HW driver converts buffer requirements to @dst and @src,
 * then passes hardware-specific payloads via @blits, enabling the core to safely
 * manage its lifecycle.
 */
struct enx_vdma_submit_params {
	struct enx_vdma_submit_buf dst;
	struct enx_vdma_submit_buf *src;
	u32	src_count;
	void *blits;		/* function-specific blit array */
	size_t	blit_size;	/* sizeof(one blit) */
	u64	user_token;		/* ASYNC only */
	u32	flags;			/* ENX_SUBMIT_* - sync or async */
	u32	job_id_out;		/* OUT (ASYNC only) */
};

/**
 * struct enx_vdma_core_ops - Hardware-specific callbacks provided by HW drivers
 * @hw_run_once: Callback to program the HW registers and execute a single job.
 *     The core provides the resolved DMA buffers (@dst, @src) and an opaque pointer
 *     to the copied user-space @blit_params. This function is called from the core's
 *     workqueue context.
 * @hw_abort: Callback to forcefully stop or reset the hardware engine.
 *     Typically invoked during driver removal or on job timeouts.
 * @session_open: Callback invoked when a new user-space session (file descriptor)
 *     is opened. Allows the driver to allocate per-session private data
 *     or initialize specific context. Returns 0 on success, or a negative error code.
 * @session_release: Callback invoked when a session is closed (file released).
 *     Allows the driver to free any per-session private data and tear down
 *     specific contexts allocated during @session_open.
 * @blit_size: The exact byte size of the per-driver blit structure
 *     (e.g., sizeof(struct en683_font_blit)). Used by the core
 *     to safely copy user-space parameters without knowing their layout.
 * @version: A string representing the version of the HW driver.
 *
 * This structure acts as a bridge between the function-agnostic VDMA core
 * and the function-specific HW drivers (like font or scaler). HW drivers
 * must register their operations via enx_vdma_core_register().
 */
struct enx_vdma_core_ops {
	int  (*hw_run_once)(struct enx_vdma_sess *vs,
			   struct enx_vdma_job_buf *dst, struct enx_vdma_job_buf *src,
			   u32 src_count, void *blit_params, u32 flags);
	void (*hw_abort)(struct enx_vdma_dev *dev);
	int  (*session_open)(struct enx_vdma_sess *vs);
	void (*session_release)(struct enx_vdma_sess *vs);
	size_t blit_size;
	const char *version;
};

/*******************************************************************************
 *
 * register read / write
 *
 *******************************************************************************/
static inline u32 enx_vdma_reg_rd(struct enx_vdma_dev *dev, u32 off)
{
 	return readl(dev->regs + off);
}

static inline void enx_vdma_reg_wr(struct enx_vdma_dev *dev, u32 off, u32 val)
{
	writel(val, dev->regs + off);
}

/*******************************************************************************
 *
 * Backend accessors — keep callsites buf-API neutral.
 *
*******************************************************************************/

static inline phys_addr_t enx_vdma_buf_phys(const struct enx_vdma_buf *b)
{
	return (phys_addr_t)b->backing->cmm->phys_start;
}

static inline void *enx_vdma_buf_vaddr(const struct enx_vdma_buf *b)
{
	return b->backing->cmm->pvirt;
}

static inline size_t enx_vdma_buf_size(const struct enx_vdma_buf *b)
{
	return (size_t)b->backing->cmm->nbytes;
}

static inline size_t enx_vdma_backing_size(const struct enx_vdma_backing *bk)
{
	return (size_t)bk->cmm->nbytes;
}

/*******************************************************************************
 *
 * Public API — EXPORT_SYMBOL_GPL in core
 *
*******************************************************************************/

/* file_operations callback function. */
int			enx_vdma_open	(struct inode *, struct file *);
int			enx_vdma_release(struct inode *, struct file *);
int			enx_vdma_mmap	(struct file *, struct vm_area_struct *);
__poll_t	enx_vdma_poll	(struct file *, struct poll_table_struct *);

/* VDMA Common ioctl helpers */
long enx_vdma_ioctl_alloc  (struct enx_vdma_sess *, void __user *);
long enx_vdma_ioctl_free   (struct enx_vdma_sess *, void __user *);
long enx_vdma_ioctl_wait   (struct enx_vdma_sess *, void __user *);
long enx_vdma_ioctl_export (struct enx_vdma_sess *, void __user *);
long enx_vdma_ioctl_import (struct enx_vdma_sess *, void __user *);
long enx_vdma_ioctl_abort  (struct enx_vdma_sess *, void __user *);
long enx_vdma_ioctl_maxbufs(struct enx_vdma_sess *, void __user *);
int  enx_vdma_submit       (struct enx_vdma_sess *, struct enx_vdma_submit_params *);
void enx_vdma_abort        (struct enx_vdma_dev *dev);

/* Platform_driver probe / remove */
struct enx_vdma_dev *enx_vdma_core_alloc(struct device *parent);
int  enx_vdma_core_register(struct enx_vdma_dev *dev,
						const struct enx_vdma_core_ops *ops,
						const struct file_operations *fops,
						const char *node_name);
void enx_vdma_core_unregister(struct enx_vdma_dev *dev);
void enx_vdma_core_free(struct enx_vdma_dev *dev);

#endif /* _ENX_VDMA_H_ */
