/*
 * Copyright (C) 2026 Eyenix Corporation All rights reserved.
 * Eyenix <support6@eyenix.com>
 *
 * enx_vdma - user-space API for Eyenix VDMA device
 *
 * Provides:
 *  - unified interface for all VDMA functions
 *	  (scaler, rotation, font, jpeg, ...).
 *
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/types.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include "enx_vdma.h"
#include "enx_vdma_errno.h"

/*******************************************************************************
 *
 * Internal define / type / structure for vdma implementation. (Not exposed to users.)
 *
*******************************************************************************/

#include "enx-vdma-uapi.h"
#include "en683-font-uapi.h"
#include "en683-dz-uapi.h"
#include "en683-jpeg-uapi.h"
#include "en683-npu-uapi.h"

#define SUPPORT_ACTIVE_MODE 0

#define VDMA_BUF_MAGIC		0x56444D41	/* "VDMA" */
#define VDMA_BUF_POISON		0xDEADBEEF

typedef void *vdma_addr_t;

/**
 * struct vdma_buf - per-buffer tracking record (lib-side mirror of kernel buf)
 * @magic:		Sentinel set to VDMA_BUF_MAGIC at alloc, poisoned to
 *				VDMA_BUF_POISON at destroy. Detects use-after-free / double-free.
 * @refcount:	Concurrent ref tracking. +1 by alloc / find_buf_by_virt /
 *				find_buf_by_id, -1 by buf_put. Reaches 0 → buf_destroy.
 *				Atomic for thread safety.
 * @next:		Singly-linked list node in owning device's @vdma_dev.bufs.
 * @id:			Kernel-side buf id (from VDMAIOSET_ALLOC / VDMAIOGET_IMPORT).
 *				Used as mmap_offset key and FREE/SUBMIT ioctl identifier.
 * @kind:		ENX_BUF_SRC or ENX_BUF_DST. Lib-side enforcement when matching
 *				submit args (e.g., a SRC buf cannot be used as dst).
 * @size:		mmap'd region size (bytes). Used for munmap and size validation.
 * @pvirt:		mmap'd virtual address — the only identifier exposed to users.
 *				Doubles as the lookup key in find_buf_by_virt().
 *
 * One record per buffer the user has allocated or imported on this device.
 * Lifetime: created by do_alloc()/vdma_import(), destroyed when refcount
 * reaches 0 (normal path) or via buf_force_destroy() at instance teardown.
 */
struct vdma_buf {
	uint32_t		magic;
	_Atomic int		refcount;
	struct vdma_buf	*next;
	uint32_t		id;
	uint32_t		kind;
	size_t			size;
	void			*pvirt;
};
typedef struct vdma_buf vdma_buf_t;

/**
 * struct vdma_dev - per-device instance (lib-internal, never exposed to users)
 * @fd:					Open fd to the kernel device node. -1 once invalidated.
 * @creator_pid:		PID at open. Used by dev_check() to detect a fork child.
 * @fork_gen_at_open:	Snapshot of @g_fork_generation at open. When fork()
 *						occurs, g_fork_generation is bumped so dev_check() fails.
 * @init_count:			Cumulative vdma_open() refcount.
 * @buf_count:			Current (live) buffer count: +1 on alloc/import,
 *						-1 on free/destroy. Compared against @max_buf for
 *						quota enforcement (not a cumulative total).
 * @invalidated:		Latched (set to 1) by a fatal ioctl failure or by
 *						vdma_close() before teardown. (atfork_child() instead
 *						clears it and relies on @fork_gen_at_open for staleness.)
 * @refcnt:				Lifetime: registry ref(1) + 1 per in-flight dev_get.
 * @max_buf:			Maximum buffers allowed for this device (from kernel).
 * @max_src:			Maximum sources allowed for this device (from kernel).
 * @lock:				Protects the @bufs list.
 * @bufs:				List of vdma_buf records owned by this device
 *						(virtual addr → buf lookup).
 */
struct vdma_dev {
	int				fd;
	pid_t			creator_pid;
	uint64_t		fork_gen_at_open;
	_Atomic int		init_count;
	_Atomic int		buf_count;
	_Atomic int		invalidated;
	_Atomic int		refcnt;		/* lifetime: registry ref(1) + 1 per in-flight dev_get */
	uint32_t		max_buf;
	uint32_t		max_src;
	pthread_mutex_t	lock;
	vdma_buf_t		*bufs;
};

/**
 * struct vdma_font_cfg - font configuration management
 * @active_mode:	Indicates the active mode status of the font device.
 *					(true: Video-core, false: Kernel)
 * @yc_idx:			Video-core Target YC Buffer index for each blit.
 * @blit_cnt:		Configured blit count for each YC buffer.
 * @acum_blit_cnt:	Accumulated assigned bullet count.
 * @cfg_count:		Counter for configuration updates.
 * @pFontCfg:		Pointer to the allocated font configuration address.
 * @active_cnt:		Active update count for Video-core mode.
 * @active_idx:		Video-core Target YC Buffer index for active updates.
 *
 * All mutable fields are protected by g_font_cfg_lock; the counters are plain
 * ints (no atomics) since every access is performed under that lock.
 */
struct vdma_font_cfg {
	bool active_mode;
	uint8_t yc_idx[MAX_VISP_YC_NUM];		// Video-core Target YC Buffer index
	uint32_t blit_cnt[MAX_VISP_YC_NUM];		// Configured blit count
	int acum_blit_cnt;
	int cfg_count;
	VDMA_FONT_CONFIG_S** pFontCfg;			// Allocated config address
	/* active(Video-core) update management - ref stop & update */
	int active_cnt;							// Active update count for Video-core mode
	uint8_t active_idx[MAX_VISP_YC_NUM];	// Video-core Target YC Buffer index for active updates
};

/**
 * struct vdma_font_stop_ctrl - font update stop control management
 * @stop_cnt:		Number of update stops configured.
 * @stop_yc_idx:	Video-core Target YC Buffer index for each update stop control.
 *
 * This structure is used to manage the stop control for font updates in the
 * Video-core mode. when transmitted via the LINK_VDMA_CMD_FONT_STOP command of
 * send_link_msg, the Video-core performs a stop request.
 */
struct vdma_font_stop_ctrl {
	uint8_t stop_cnt;
	uint8_t stop_yc_idx[MAX_VISP_YC_NUM];	// Video-core Target YC Buffer index
};

/**
 * struct vdma_font_active_update - font active mode(Video-core) update management
 * @font_number:	Font number for the active update.
 * @yc_idx:			Video-core Target YC Buffer index for the active update.
 * @array_paddr:	Physical address of the font configuration array for the active update.
 *
 * This structure is used to handle font updates in Video-core mode.
 * when transmitted via the LINK_VDMA_CMD_FONT_UPDATE command of
 * send_link_msg, the Video-core performs an update request.
 */
struct vdma_font_active_update {
	uint32_t font_number;
	uint32_t yc_idx;
	uint32_t array_paddr;
};

/**
 * struct vdma_irq_ctrl - VDMA IRQ control management (for enx_send_linkmsg)
 * @func_type:		VDMA function type (scaler, rotation, font, jpeg, etc.)
 * @init_type:		VDMA initialization type (kernel or Video-core)
 */
struct vdma_irq_ctrl {
	ENX_VDMA_FUNC_TYPE_E func_type;
	ENX_VDMA_INIT_TYPE_E init_type;
};

/*******************************************************************************
 *
 * Global registry (Per-process tracking of all device instances, for fork safety)
 *
*******************************************************************************/

/* Device-indexed array. */
static struct vdma_dev	*g_dev_inst[ENX_VDMA_FUNC_END];

/* Protects the lifecycle of the array and its slots.
 * Provides mutual exclusion against concurrent open/close. */
static pthread_mutex_t	 g_dev_lock = PTHREAD_MUTEX_INITIALIZER;

/* Bumped by atfork_child() so handles inherited from the parent
 * fast-fail in dev_check() until the child reopens. */
static _Atomic uint64_t	 g_fork_generation;

/* Guards one-time atfork handler registration. */
static pthread_once_t	 g_fork_once = PTHREAD_ONCE_INIT;

/* Font-config mgmt */
static struct vdma_font_cfg g_font_cfg_inst;

/* Protects the font configuration instance. */
static pthread_mutex_t g_font_cfg_lock = PTHREAD_MUTEX_INITIALIZER;

/*******************************************************************************
 *
 * Internal device-ctrl functions
 *
*******************************************************************************/

/* Default device paths per function */
static const char *default_path_for_dev(ENX_VDMA_FUNC_TYPE_E vdma_func)
{
	switch (vdma_func) {
	case ENX_VDMA_SCALER0:	return "/dev/enx_vdma_dz0";
	case ENX_VDMA_SCALER1:	return "/dev/enx_vdma_dz1";
	case ENX_VDMA_FONT:		return "/dev/enx_vdma_font";
	case ENX_VDMA_JPEG_ENC:	return "/dev/enx_vdma_jenc";
	case ENX_VDMA_JPEG_DEC:	return "/dev/enx_vdma_jdec";
	case ENX_VDMA_NPU:		return "/dev/enx_vdma_npu";
	default:				return NULL;
	}
}

/*
 * Handler invoked automatically in the child process right after fork().
 * Invalidates all lib state inherited from the parent:
 *	- Reinitialize mutexes, since the lock/unlock state inherited from the
 *	  parent is unknown.
 *	- Sever each instance by setting fd = -1 and bufs = NULL so the child
 *	  cannot issue raw ioctls.
 *	- Bump g_fork_generation so that every inherited handle fast-fails with
 *	  ENX_ERR_VDMA_FORK_STALE inside dev_check().
 *
 * To obtain a usable instance the child must fully reopen: since the inherited
 * g_dev_inst[] slot is left non-NULL (the struct cannot be freed here — not
 * async-signal-safe), a plain vdma_open() only bumps init_count and still
 * fast-fails. The child must therefore close (vdma_close()) to drop the slot,
 * then vdma_open() again — at which point a fresh instance is created.
 *
 * NOTE: only async-signal-safe functions are allowed here (no malloc/printf).
 */
static void atfork_child(void)
{
	pthread_mutex_init(&g_dev_lock, NULL);
	pthread_mutex_init(&g_font_cfg_lock, NULL);

	for (int i = 0; i < ENX_VDMA_FUNC_END; i++) {
		struct vdma_dev *dev = g_dev_inst[i];
		if (!dev) continue;
		pthread_mutex_init(&dev->lock, NULL);
		dev->fd = -1;
		dev->bufs = NULL;
		dev->max_buf = 0;
		dev->max_src = 0;
		atomic_store(&dev->init_count, 0);
		atomic_store(&dev->invalidated, 0);
		atomic_store(&dev->refcnt, 0);
	}
	atomic_fetch_add_explicit(&g_fork_generation, 1, memory_order_release);
}

/* Register only once per-process via pthread_once. */
static void register_atfork(void)
{
	pthread_atfork(NULL, NULL, atfork_child);
}

/*
 * Validates the instance at every API entry point.
 * Five distinct failure modes are surfaced via dedicated error codes:
 *	ENX_ERR_VDMA_NOT_INITIALIZED	: dev itself is NULL.
 *	ENX_ERR_VDMA_NO_DEVICE			: fd has been severed.
 *	ENX_ERR_VDMA_BAD_FD				: invalidated by a previous fatal ioctl error.
 *	ENX_ERR_VDMA_OWNER_DEAD			: pid mismatch.
 *	ENX_ERR_VDMA_FORK_STALE			: fork_generation mismatch.
 */
static int dev_check(struct vdma_dev *dev)
{
	if (!dev)
		return ENX_ERR_VDMA_NOT_INITIALIZED;
	if (dev->fd < 0)
		return ENX_ERR_VDMA_NO_DEVICE;
	if (atomic_load_explicit(&dev->invalidated, memory_order_acquire))
		return ENX_ERR_VDMA_BAD_FD;
	if (dev->creator_pid != getpid())
		return ENX_ERR_VDMA_OWNER_DEAD;
	if (dev->fork_gen_at_open !=
		atomic_load_explicit(&g_fork_generation, memory_order_acquire))
		return ENX_ERR_VDMA_FORK_STALE;
	return VDMA_OK;
}


/* ioctl wrapper — auto-detects fatal errnos. */
static int dev_ioctl(struct vdma_dev *dev, unsigned long cmd, void *p)
{
	int ret = ioctl(dev->fd, cmd, p);
	if (ret < 0) {
		ret = -errno;
		if (ret == -ENODEV || ret == -EIO || ret == -ESHUTDOWN)
			atomic_store_explicit(&dev->invalidated, 1, memory_order_release);
	}
	return ret;
}

/*
 * Device-indexed lookup with validity check.
 *
 * On success, a reference is taken on @dev so a concurrent vdma_close() cannot
 * free it out from under the operation. Every successful dev_get() MUST be
 * paired with exactly one dev_put().
 *
 * The lookup and the ref++ happen under g_dev_lock, serialized against
 * vdma_close() unpublishing the slot: a caller either observes the live slot
 * (and pins it) or observes NULL (and fails) — never a half-freed instance.
 */
static int dev_get(ENX_VDMA_FUNC_TYPE_E func, struct vdma_dev **out)
{
	struct vdma_dev *dev = NULL;
	int ret = 0;

	if (func >= ENX_VDMA_FUNC_END)
		return ENX_ERR_VDMA_FUNC_TYPE;

	pthread_mutex_lock(&g_dev_lock);
	dev = g_dev_inst[func];
	if (!dev) {
		pthread_mutex_unlock(&g_dev_lock);
		return ENX_ERR_VDMA_NOT_INITIALIZED;
	}
	ret = dev_check(dev);
	if (ret) {
		pthread_mutex_unlock(&g_dev_lock);
		return ret;
	}
	atomic_fetch_add_explicit(&dev->refcnt, 1, memory_order_acq_rel);
	pthread_mutex_unlock(&g_dev_lock);

	*out = dev;
	return VDMA_OK;
}

static void dev_force_destroy(struct vdma_dev *dev);
/*
 * Drop one reference taken by dev_get() (or the registry ref held by
 * vdma_close()). The thread that observes the final 1->0 transition tears the
 * instance down. This decouples vdma_close() from in-flight operations: Exit
 * never blocks, and a still-running operation simply defers teardown until its
 * own dev_put(). NULL-safe.
 */
static void dev_put(struct vdma_dev *dev)
{
	if (!dev)
		return;
	if (atomic_fetch_sub_explicit(&dev->refcnt, 1, memory_order_acq_rel) != 1)
		return;
	dev_force_destroy(dev);
	free(dev);
}

static int do_font_init(struct vdma_dev *dev, ENX_VDMA_INIT_TYPE_E init_type)
{
	int ret = VDMA_OK;
	u32 mode = ENX_VDMA_INIT_TYPE_KERNEL;
	u32 max_src = 0;

	if (ioctl(dev->fd, VDMAIOSET_MOD_INIT, &mode) < 0)
		return -errno;

	if (ioctl(dev->fd, VDMAIOGET_MAX_SRCS, &max_src) < 0)
		return -errno;
	if (!max_src) {
		EPRINTF("The font driver's max_src is set to 0.\n");
		return ENX_ERR_VDMA_INVALID_ARG;
	}
	dev->max_src = max_src;

#if SUPPORT_ACTIVE_MODE
	/* Request IRQ control from Video-core */
	struct vdma_irq_ctrl irq_ctrl = { .func_type = ENX_VDMA_FONT, .init_type = init_type };
	ret = enx_send_linkmsg(LINK_HEAD_DATA_ID_MSG, LINK_VDMA_CMD_IRQ_CONTROL, &irq_ctrl, sizeof(irq_ctrl), TRUE);
	if (ret) {
		EPRINTF("Failed to send LINK_VDMA_CMD_IRQ_CONTROL message to Video-core.\n");
		return ret;
	}
#endif

	VDMA_FONT_CONFIG_S** pcfg = calloc(MAX_VISP_YC_NUM, sizeof(VDMA_FONT_CONFIG_S*));
	if (!pcfg) {
		EPRINTF("Failed to allocate memory for font configuration pointers\n");
		return -errno;
	}

	/* initialize global cfg info */
	g_font_cfg_inst.pFontCfg = pcfg;
#if SUPPORT_ACTIVE_MODE
	g_font_cfg_inst.active_mode = (init_type==ENX_VDMA_INIT_TYPE_VIDEO_CORE);
#else
	g_font_cfg_inst.active_mode = 0;	// Kernel mode
#endif
	g_font_cfg_inst.acum_blit_cnt = 0;
	g_font_cfg_inst.cfg_count = 0;
	g_font_cfg_inst.active_cnt = 0;
	for (int i = 0; i < MAX_VISP_YC_NUM; i++) {
		g_font_cfg_inst.blit_cnt[i] = 0;
		g_font_cfg_inst.yc_idx[i] = 0;
		g_font_cfg_inst.active_idx[i] = 0;
	}

	return ret;
}

/* Adjust active mode via device-specific ioctl if needed. Called by vdma_open(). */
static int do_dev_init(struct vdma_dev *dev, ENX_VDMA_FUNC_TYPE_E func, ENX_VDMA_INIT_TYPE_E init_type)
{
	int ret = 0;
	u32 max_buf = 0;
	if (func == ENX_VDMA_FONT) {
		ret = do_font_init(dev, init_type);
		if (ret) return ret;
	} else {
		dev->max_src = 1;
	}

	if (ioctl(dev->fd, VDMAIOGET_MAX_BUFS, &max_buf) < 0)
		return -errno;
	dev->max_buf = max_buf;

	return VDMA_OK;
}

/*******************************************************************************
 *
 * Internal buffer-ctrl functions (Per-instance)
 *
*******************************************************************************/

static void buf_link(struct vdma_dev *dev, vdma_buf_t *buf)
{
	pthread_mutex_lock(&dev->lock);
	buf->next = dev->bufs;
	dev->bufs = buf;
	pthread_mutex_unlock(&dev->lock);
}

/* find & unlink */
static int buf_unlink(struct vdma_dev *dev, vdma_buf_t *buf)
{
	int found = 0;
	pthread_mutex_lock(&dev->lock);
	for (vdma_buf_t **pp = &dev->bufs; *pp; pp = &(*pp)->next) {
		if (*pp == buf) {
			*pp = buf->next;
			buf->next = NULL;
			found = 1;
			break;
		}
	}
	pthread_mutex_unlock(&dev->lock);
	return found;
}

/*
 * Invoked only after the reference count reaches 0.
 * flow: munmap → kernel FREE ioctl → magic poison → kfree.
 */
static void buf_destroy(struct vdma_dev *dev, vdma_buf_t *buf)
{
	struct vdma_free_args f = { .id = buf->id };

	if (buf->pvirt)
		munmap(buf->pvirt, buf->size);
	if (!atomic_load_explicit(&dev->invalidated, memory_order_acquire) && dev->fd >= 0)
		(void)ioctl(dev->fd, VDMAIOSET_FREE, &f);
	atomic_fetch_sub_explicit(&dev->buf_count, 1, memory_order_acq_rel);
	buf->magic = VDMA_BUF_POISON;
	free(buf);
}

/*
 * Decrement the refcount after using the buffer.
 * If it reaches 0, call buf_destroy.
 */
static void buf_put(struct vdma_dev *dev, vdma_buf_t *buf)
{
	if (atomic_fetch_sub_explicit(&buf->refcount, 1, memory_order_acq_rel) != 1)
		return;
	buf_destroy(dev, buf);
}

/*
 * Forced destruction on the instance-close path.
 * Claims refcount 1 → 0 via CAS. If another thread is holding a transient
 * ref, yields (sched_yield) and retries.
 *
 * NOTE: reached only from dev_force_destroy(), which runs after dev->refcnt
 * has dropped to 0 — i.e. no operation is in flight and thus no transient buf
 * ref can exist. The CAS therefore succeeds on the first try in practice; the
 * retry loop is kept purely as defensive insurance.
 */
static void buf_force_destroy(struct vdma_dev *dev, vdma_buf_t *b)
{
	int expected = 1;
	while (!atomic_compare_exchange_strong_explicit(
			&b->refcount, &expected, 0,
			memory_order_acq_rel, memory_order_acquire)) {
		sched_yield();
		expected = 1;
	}
	if (b->pvirt) munmap(b->pvirt, b->size);
	atomic_fetch_sub_explicit(&dev->buf_count, 1, memory_order_acq_rel);
	b->magic = VDMA_BUF_POISON;
	free(b);
}

/*
 * kernel-side buf id - vdma_buf_t* reverse lookup.
 *
 * Race-prevention key: the list lookup and ref++ are performed under the
 * same lock.
 * The caller must release the ref with buf_put() when done.
 */
static vdma_buf_t *find_buf_by_id(struct vdma_dev *dev, uint32_t id)
{
	vdma_buf_t *found = NULL;
	pthread_mutex_lock(&dev->lock);
	for (vdma_buf_t *b = dev->bufs; b; b = b->next) {
		if (b->id != id) continue;
		if (b->magic != VDMA_BUF_MAGIC) continue;
		atomic_fetch_add_explicit(&b->refcount, 1, memory_order_acq_rel);
		found = b;
		break;
	}
	pthread_mutex_unlock(&dev->lock);
	return found;
}

/*
 * virtual addr (= buf->pvirt) → vdma_buf_t* reverse lookup, acquiring a
 * transient ref.
 *
 * Race-prevention key: the list lookup and ref++ are performed under the
 * same lock.
 * The caller must release the ref with buf_put() when done.
 * If size_hint != 0, also checks that the buf size matches.
 */
static vdma_buf_t *find_buf_by_virt(struct vdma_dev *dev, vdma_addr_t addr,
					size_t size_hint)
{
	vdma_buf_t *found = NULL;

	if (!addr) return NULL;

	pthread_mutex_lock(&dev->lock);
	for (vdma_buf_t *b = dev->bufs; b; b = b->next) {
		if (b->pvirt != addr) continue;
		if (b->magic != VDMA_BUF_MAGIC) continue;
		if (size_hint && b->size != size_hint) continue;
		if (atomic_load_explicit(&b->refcount, memory_order_acquire) > 0) {
			atomic_fetch_add_explicit(&b->refcount, 1, memory_order_acq_rel);
			found = b;
		}
		break;
	}
	pthread_mutex_unlock(&dev->lock);
	return found;
}

/*******************************************************************************
 *
 * Internal Common functions (Per-instance)
 *
*******************************************************************************/

static int vdma_open(ENX_VDMA_FUNC_TYPE_E func, ENX_VDMA_INIT_TYPE_E init_type)
{
	struct vdma_dev *dev = NULL;
	const char *path = NULL;
	int fd, ret;

	if (func >= ENX_VDMA_FUNC_END)
		return ENX_ERR_VDMA_FUNC_TYPE;

	/* initialize global resources once */
	pthread_once(&g_fork_once, register_atfork);
	pthread_mutex_lock(&g_dev_lock);

	/* init check, instance != null -> init_count++ */
	if (g_dev_inst[func]) {
		struct vdma_dev *existing = g_dev_inst[func];
		if (existing->fd >= 0) {
			/* Genuinely initialized — just bump the refcount. */
			atomic_fetch_add_explicit(&existing->init_count, 1, memory_order_acq_rel);
			pthread_mutex_unlock(&g_dev_lock);
			return VDMA_OK;
		}
		/*
		 * Post-fork stale slot: atfork_child severed fd but could not free
		 * the struct (async-signal-safety constraints). We are now in a
		 * regular API context, so it is safe to discard the stale dev and
		 * fall through to a fresh open.
		 */
		pthread_mutex_destroy(&existing->lock);
		free(existing);
		g_dev_inst[func] = NULL;
	}

	path = default_path_for_dev(func);
	if (!path) {
		pthread_mutex_unlock(&g_dev_lock);
		return ENX_ERR_VDMA_FUNC_TYPE;
	}

	dev = calloc(1, sizeof(*dev));
	if (!dev) {
		pthread_mutex_unlock(&g_dev_lock);
		return ENX_ERR_VDMA_NO_MEMORY;
	}

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		ret = -errno;
		free(dev);
		pthread_mutex_unlock(&g_dev_lock);
		return ret;
	}

	pthread_mutex_init(&dev->lock, NULL);
	dev->fd = fd;
	dev->creator_pid = getpid();
	/* fork-safety baseline; mismatch later triggers ENX_ERR_VDMA_FORK_STALE. */
	dev->fork_gen_at_open	= atomic_load_explicit(&g_fork_generation, memory_order_acquire);
	atomic_store(&dev->invalidated, 0);
	atomic_store(&dev->init_count, 1);
	atomic_store(&dev->refcnt, 1);	/* registry reference, dropped by vdma_close() */

	ret = do_dev_init(dev, func, init_type);
	if (ret) {
		close(fd);
		pthread_mutex_destroy(&dev->lock);
		free(dev);
		pthread_mutex_unlock(&g_dev_lock);
		return ret;
	}

	g_dev_inst[func] = dev;
	pthread_mutex_unlock(&g_dev_lock);
	return VDMA_OK;
}

static void dev_force_destroy(struct vdma_dev *dev)
{
	vdma_buf_t *list = NULL, *next = NULL;
	int fd;

	atomic_store_explicit(&dev->invalidated, 1, memory_order_release);

	fd = dev->fd;
	dev->fd = -1;
	if (fd >= 0) close(fd);

	pthread_mutex_lock(&dev->lock);
	list = dev->bufs;
	dev->bufs = NULL;
	pthread_mutex_unlock(&dev->lock);

	while (list) {
		next = list->next;
		list->next = NULL;
		buf_force_destroy(dev, list);
		list = next;
	}

	pthread_mutex_destroy(&dev->lock);
}

static int vdma_close(ENX_VDMA_FUNC_TYPE_E func)
{
	struct vdma_dev *dev = NULL;
	int prev;

	if (func >= ENX_VDMA_FUNC_END)
		return ENX_ERR_VDMA_FUNC_TYPE;

	pthread_mutex_lock(&g_dev_lock);
	dev = g_dev_inst[func];
	if (!dev) {
		pthread_mutex_unlock(&g_dev_lock);
		return ENX_ERR_VDMA_NO_DEVICE;
	}

	prev = atomic_fetch_sub_explicit(&dev->init_count, 1, memory_order_acq_rel);
	if (prev != 1) {
		/* Another ref is still in use */
		pthread_mutex_unlock(&g_dev_lock);
		return VDMA_OK;
	}

	if (func == ENX_VDMA_FONT) {
		/* stop & free font resources */
		ENX_DMA_Font_Stop_All();
		ENX_DMA_Font_CfgFree();
		pthread_mutex_destroy(&g_font_cfg_lock);
		/* initialize font configuration instance */
		g_font_cfg_inst.active_mode = false;
		if (g_font_cfg_inst.pFontCfg) {
			free(g_font_cfg_inst.pFontCfg);
			g_font_cfg_inst.pFontCfg = NULL;
		}
	}

	/*
	 * Last init ref. Unpublish the slot (so no new dev_get() can find it) and
	 * mark invalidated, then drop the registry reference. In-flight operations
	 * hold their own refs; whichever thread drops the final ref runs the
	 * teardown. Exit does not block waiting for them.
	 */
	g_dev_inst[func] = NULL;
	atomic_store_explicit(&dev->invalidated, 1, memory_order_release);
	pthread_mutex_unlock(&g_dev_lock);

	dev_put(dev);	/* drop registry ref; frees here iff no op is in flight */
	return VDMA_OK;
}

static vdma_buf_t *do_alloc(struct vdma_dev *dev, uint32_t kind, size_t size)
{
	struct vdma_alloc_args a;
	vdma_buf_t *buf = NULL;
	void *p;

	if (size == 0 || size > UINT32_MAX) { errno = ENX_ERR_VDMA_INVALID_ARG; return NULL; }

	/* inc-then-check : atomic */
	if ((unsigned)atomic_fetch_add(&dev->buf_count, 1) >= dev->max_buf) {
		atomic_fetch_sub(&dev->buf_count, 1);
		errno = ENX_ERR_VDMA_QUOTA_EXCEEDED;
		EPRINTF("Buffer count quota exceeded: buf_cnt: %u, max_cnt: %u\n", atomic_load(&dev->buf_count), dev->max_buf);
		return NULL;
	}

	memset(&a, 0, sizeof(a));
	a.kind	= kind;
	a.size	= (uint32_t)size;

	/* buffer allocation, Returns the ID and mmap_offset for the new buffer. */
	if (dev_ioctl(dev, VDMAIOSET_ALLOC, &a) < 0) {
		atomic_fetch_sub(&dev->buf_count, 1);
		return NULL;
	}

	/* mmap the buffer into user space. */
	p = mmap(NULL, size, PROT_READ | PROT_WRITE,
		 MAP_SHARED, dev->fd, (off_t)a.mmap_offset);
	if (p == MAP_FAILED) {
		struct vdma_free_args f = { .id = a.id };
		(void)ioctl(dev->fd, VDMAIOSET_FREE, &f);
		atomic_fetch_sub(&dev->buf_count, 1);
		return NULL;
	}

	/* Create a new vdma_buf record for this buffer and link it to the instance. */
	buf = calloc(1, sizeof(*buf));
	if (!buf) {
		struct vdma_free_args f = { .id = a.id };
		munmap(p, size);
		(void)ioctl(dev->fd, VDMAIOSET_FREE, &f);
		atomic_fetch_sub(&dev->buf_count, 1);
		return NULL;
	}
	buf->magic = VDMA_BUF_MAGIC;
	atomic_store(&buf->refcount, 1);
	buf->id		= a.id;
	buf->kind	= kind;
	buf->size	= size;
	buf->pvirt	= p;

	buf_link(dev, buf);
	return buf;
}

static vdma_addr_t vdma_alloc_buf(ENX_VDMA_FUNC_TYPE_E func, ENX_VDMA_BUF_KIND_E kind, size_t size, UInt32 *out_id)
{
	struct vdma_dev *dev = NULL;
	vdma_addr_t buf = NULL;
	int ret = dev_get(func, &dev);
	if (ret) { errno = ret; return NULL; }

	if (kind != VDMA_KIND_SRC && kind != VDMA_KIND_DST) {
		EPRINTF("Invalid buffer kind: %u\n", kind);
		errno = ENX_ERR_VDMA_INVALID_ARG;
		goto out;
	}

	vdma_buf_t *b = do_alloc(dev, kind, size);
	if (b && out_id) *out_id = b->id;
	buf = b ? b->pvirt : NULL;
out:
	dev_put(dev);
	return buf;
}

/* buf_put(refcount--) → buf_destroy → munmap + kernel FREE ioctl. */
static int vdma_free(ENX_VDMA_FUNC_TYPE_E func, vdma_addr_t addr, size_t size)
{
	struct vdma_dev *dev = NULL;
	int ret = dev_get(func, &dev);
	if (ret) return ret;

	vdma_buf_t *buf = find_buf_by_virt(dev, addr, size);
	if (!buf) { dev_put(dev); return ENX_ERR_VDMA_INVALID_ARG; }

	if (buf_unlink(dev, buf))
		buf_put(dev, buf);	/* user ref drop */
	buf_put(dev, buf);		/* transient ref drop */
	dev_put(dev);
	return VDMA_OK;
}

/* Validity check handler */
static int verify_buf_info(struct vdma_dev *dev, uint32_t *kind,
							uint64_t *addr, vdma_buf_t **out_buf,
							const char *label)
{
	*out_buf = NULL;
	switch (*kind) {
	case ENX_VDMA_ADDR_TYPE_ID:
		*out_buf = find_buf_by_id(dev, (uint32_t)*addr);
		break;
	case ENX_VDMA_ADDR_TYPE_VIRT:
		*out_buf = find_buf_by_virt(dev, (void *)(uintptr_t)*addr, 0);
		if (*out_buf) {
			*kind = ENX_VDMA_ADDR_TYPE_ID;
			*addr = (*out_buf)->id;
		}
		break;
	case ENX_VDMA_ADDR_TYPE_PHYS:
		return VDMA_OK;
	default:
		EPRINTF("Invalid %s_addr_flag: %u\n", label, *kind);
		return ENX_ERR_VDMA_INVALID_ARG;
	}
	if (!*out_buf) {
		EPRINTF("Buffer lookup failed: Invalid buffer info[%s]\n", label);
		return ENX_ERR_VDMA_INVALID_ARG;
	}
	return VDMA_OK;
}

/*******************************************************************************
 *
 * Cross-fd buffer sharing — EXPORT / IMPORT
 *
*******************************************************************************/

int vdma_export(ENX_VDMA_FUNC_TYPE_E func, vdma_addr_t addr, int *export_fd_out)
{
	struct vdma_dev *dev = NULL;
	vdma_buf_t *buf = NULL;
	struct vdma_export_args e;
	int ret;

	if (!export_fd_out) return ENX_ERR_VDMA_INVALID_ARG;
	ret = dev_get(func, &dev);
	if (ret) return ret;

	buf = find_buf_by_virt(dev, addr, 0);
	if (!buf) { ret = ENX_ERR_VDMA_INVALID_ARG; goto out; }

	memset(&e, 0, sizeof(e));
	e.id = buf->id;
	if (dev_ioctl(dev, VDMAIOSET_EXPORT, &e) < 0) {
		ret = errno;
		goto out;
	}
	*export_fd_out = e.export_fd;
	ret = 0;
out:
	if (buf) buf_put(dev, buf);
	dev_put(dev);
	return ret;
}

vdma_addr_t vdma_import(ENX_VDMA_FUNC_TYPE_E func, int export_fd,
						ENX_VDMA_BUF_KIND_E kind, size_t *size_out)
{
	struct vdma_dev *dev = NULL;
	struct vdma_import_args im = {0};
	vdma_buf_t *buf = NULL;
	vdma_addr_t p = NULL;
	int save_errno = 0;
	int ret;
	bool quota_held = false;
	bool kernel_imported = false;

	if (export_fd < 0) { errno = ENX_ERR_VDMA_BAD_FD; return NULL; }

	ret = dev_get(func, &dev);
	if (ret) { errno = ret; return NULL; }

	/* inc-then-check : atomic */
	if ((unsigned)atomic_fetch_add(&dev->buf_count, 1) >= dev->max_buf) {
		atomic_fetch_sub(&dev->buf_count, 1);
		EPRINTF("Buffer count quota exceeded: buf_cnt: %u, max_cnt: %u\n",
				atomic_load(&dev->buf_count), dev->max_buf);
		save_errno = ENX_ERR_VDMA_QUOTA_EXCEEDED;
		goto out;
	}
	quota_held = true;

	im.export_fd	= export_fd;
	im.kind			= (kind == VDMA_KIND_SRC) ? ENX_BUF_SRC : ENX_BUF_DST;
	if (dev_ioctl(dev, VDMAIOGET_IMPORT, &im) < 0) {
		save_errno = -errno;
		/* Per-session dedup: vdma_import is idempotent on EEXIST. */
		if (save_errno == -EEXIST) {
			vdma_buf_t *existing = find_buf_by_id(dev, im.id);
			if (existing) {
				HPRINTF("vdma_import: buffer already imported, reusing.\n");
				if (size_out) *size_out = existing->size;
				p = existing->pvirt;
				buf_put(dev, existing);	/* drop find ref */
				save_errno = 0;			/* treat dedup as success */
			}
		}
		/* Kernel ioctl failed → quota inc must be undone */
		atomic_fetch_sub(&dev->buf_count, 1);
		quota_held = false;
		goto out;
	}
	kernel_imported = true;

	p = mmap(NULL, im.size, PROT_READ | PROT_WRITE,
			MAP_SHARED, dev->fd, (off_t)im.mmap_offset);
	if (p == MAP_FAILED) {
		save_errno = -errno;
		p = NULL;
		goto out;
	}

	buf = calloc(1, sizeof(*buf));
	if (!buf) {
		save_errno = -errno;
		munmap(p, im.size);
		p = NULL;
		goto out;
	}
	buf->magic = VDMA_BUF_MAGIC;
	atomic_store(&buf->refcount, 1);
	buf->id    = im.id;
	buf->kind  = im.kind;
	buf->size  = im.size;
	buf->pvirt = p;
	buf_link(dev, buf);

	if (size_out) *size_out = im.size;
	kernel_imported = false;

out:
	if (save_errno) {
		/* Roll back any state we set up before failing. */
		if (kernel_imported) {
			struct vdma_free_args f = { .id = im.id };
			(void)ioctl(dev->fd, VDMAIOSET_FREE, &f);
		}
		if (quota_held) {
			atomic_fetch_sub(&dev->buf_count, 1);
		}
		p = NULL;
	}
	dev_put(dev);
	if (save_errno) errno = save_errno;
	return p;
}

/*******************************************************************************
 *
 * DMA-Common functions (Buffer shared)
 *
*******************************************************************************/

Int32 ENX_DMA_Buffer_Export_Fd(ENX_VDMA_FUNC_TYPE_E func, void *pBuf, int *fd_out)
{
	return vdma_export(func, pBuf, fd_out);
}

Int32 ENX_DMA_Buffer_Import_Fd(ENX_VDMA_FUNC_TYPE_E func, ENX_VDMA_BUF_KIND_E kind,
									int fd, void **pBuf_out)
{
	size_t sz;
	vdma_addr_t p;
	int ret = VDMA_OK;

	if (!pBuf_out) {
		EPRINTF("Invalid output buffer pointer.\n");
		return ENX_ERR_VDMA_INVALID_ARG;
	}
	p = vdma_import(func, fd, kind, &sz);
	if (!p) {
		ret = errno;
		EPRINTF("Failed to import buffer from fd.");
		if (ret > 0) EPRINTF("(%#x)\n", ret);
		else EPRINTF("(%d)\n",ret);
		return ret;
	}
	*pBuf_out = p;
	return ret;
}

/*
 * Same-process one-shot. Goes Export → Import internally and discards the fd.
 * Caller never sees the fd; the new view is owned by dst_func and is freed via
 * the regular ENX_DMA_*_Buffer_Free().
 */
Int32 ENX_DMA_Buffer_Connect(ENX_VDMA_FUNC_TYPE_E src_func, void *pSrc_buf,
							ENX_VDMA_FUNC_TYPE_E dst_func, ENX_VDMA_BUF_KIND_E kind,
							void **pDst_buf_out)
{
	int fd, ret = VDMA_OK;
	size_t sz;
	vdma_addr_t p;

	if (!pDst_buf_out) {
		EPRINTF("Invalid output buffer pointer.\n");
		return ENX_ERR_VDMA_INVALID_ARG;
	}

	/* anon_inode fd takes +1 backing ref (kernel side) */
	ret = vdma_export(src_func, pSrc_buf, &fd);
	if (ret) {
		EPRINTF("Failed to export buffer from source function.");
		if (ret > 0) EPRINTF("(%#x)\n", ret);
		else EPRINTF("(%d)\n",ret);
		return ret;
	}

	/* new view on dst_func takes another +1 backing ref */
	p = vdma_import(dst_func, fd, kind, &sz);

	/*
	 * close transient fd unconditionally.
	 *	- Success : view owns the ref, closing fd is harmless.
	 *	- Failure : closing releases the backing ref the fd held.
	 */
	close(fd);

	if (!p) {
		ret = -errno;
		EPRINTF("Failed to import buffer to destination function.(%d)\n", ret);
		return ret;
	}
	*pDst_buf_out = p;
	return ret;
}

/*******************************************************************************
 *
 * DMA-SCALER functions
 *
*******************************************************************************/

static inline ENX_VDMA_FUNC_TYPE_E scaler_fn_type(int ch)
{
	return ch == 0 ? ENX_VDMA_SCALER0 :
		ch == 1 ? ENX_VDMA_SCALER1 : ENX_VDMA_FUNC_END;
}

Int32 ENX_DMA_Scaler_Init(Int32 ch)
{
	return vdma_open(scaler_fn_type(ch), ENX_VDMA_INIT_TYPE_KERNEL);
}

void ENX_DMA_Scaler_Exit(Int32 ch)
{
	(void)vdma_close(scaler_fn_type(ch));
}

void* ENX_DMA_Scaler_Buffer_Alloc(Int32 ch, ENX_VDMA_BUF_KIND_E kind, size_t size)
{
	return vdma_alloc_buf(scaler_fn_type(ch), kind, size, NULL);
}

void* ENX_DMA_Scaler_Buffer_Alloc_EX(Int32 ch, ENX_VDMA_BUF_KIND_E kind, size_t size, UInt32 *buf_id_out)
{
	return vdma_alloc_buf(scaler_fn_type(ch), kind, size, buf_id_out);
}

Int32 ENX_DMA_Scaler_Buffer_Free(Int32 ch, void *pBuf, size_t buf_size)
{
	return vdma_free(scaler_fn_type(ch), pBuf, buf_size);
}

Int32 ENX_DMA_Scaler_Execute(Int32 ch, VDMA_DZ_CONFIG_S *gVdmaDzCfg)
{
	ENX_VDMA_FUNC_TYPE_E vdma_func = scaler_fn_type(ch);
	struct vdma_dev *dev = NULL;
	vdma_buf_t *src = NULL, *dst = NULL;
	Int32 ret = VDMA_OK;
	UInt32 tmp_src_flag = VDMA_BUF_POISON, tmp_dst_flag = VDMA_BUF_POISON;
	UInt64 tmp_src_addr, tmp_dst_addr;

	/* check scaler init */
	ret = dev_get(vdma_func, &dev);
	if (ret) {
		EPRINTF("dma scaler%u is not initialized.\n", vdma_func);
		ret = ENX_ERR_VDMA_NOT_INITIALIZED;
		goto out;
	}

	/* check scaler config */
	if (!gVdmaDzCfg) {
		EPRINTF("Scaler config is nill.\n");
		ret = ENX_ERR_VDMA_CONFIG_NOT_ALLOCATION;
		goto out;
	}

	tmp_src_flag = gVdmaDzCfg->src_addr_flag;
	tmp_src_addr = gVdmaDzCfg->src_addr;
	/* check src_addr validity */
	ret = verify_buf_info(dev, &gVdmaDzCfg->src_addr_flag, &gVdmaDzCfg->src_addr, &src,
										ch == 0 ? "scaler0 src" : "scaler1 src");
	if (ret) { goto out; }

	tmp_dst_flag = gVdmaDzCfg->dst_addr_flag;
	tmp_dst_addr = gVdmaDzCfg->dst_addr;
	/* check dst_addr validity */
	ret = verify_buf_info(dev, &gVdmaDzCfg->dst_addr_flag, &gVdmaDzCfg->dst_addr, &dst,
										ch == 0 ? "scaler0 dst" : "scaler1 dst");
	if (ret) { goto out; }

	/* submit */
	ret = dev_ioctl(dev, VDMAIOSET_DZ_SUBMIT, gVdmaDzCfg);
	if (ret) {
		EPRINTF("DMA scaler%u submit failed: %d\n", ch, ret);
		goto out;
	}

out:
	/* recover original flags and addresses */
	if (tmp_src_flag != VDMA_BUF_POISON) {
		gVdmaDzCfg->src_addr_flag = tmp_src_flag;
		gVdmaDzCfg->src_addr = tmp_src_addr;
	}
	if (tmp_dst_flag != VDMA_BUF_POISON) {
		gVdmaDzCfg->dst_addr_flag = tmp_dst_flag;
		gVdmaDzCfg->dst_addr = tmp_dst_addr;
	}
	/* release transient refs */
	if (dst) buf_put(dev, dst);
	if (src) buf_put(dev, src);
	dev_put(dev);
	return ret;
}

/*******************************************************************************
 *
 * DMA-FONT functions
 *
*******************************************************************************/

Int32 ENX_DMA_Font_Init(ENX_VDMA_INIT_TYPE_E init_type)
{
	if (init_type != ENX_VDMA_INIT_TYPE_VIDEO_CORE && init_type != ENX_VDMA_INIT_TYPE_KERNEL) {
		EPRINTF("Invalid init_type for font: %u\n", init_type);
		return ENX_ERR_VDMA_INIT_TYPE;
	}
	return vdma_open(ENX_VDMA_FONT, init_type);
}

void ENX_DMA_Font_Exit(void)
{
	(void)vdma_close(ENX_VDMA_FONT);
}

void* ENX_DMA_Font_Buffer_Alloc(ENX_VDMA_BUF_KIND_E kind, size_t size)
{
	return vdma_alloc_buf(ENX_VDMA_FONT, kind, size, NULL);
}

void* ENX_DMA_Font_Buffer_Alloc_EX(ENX_VDMA_BUF_KIND_E kind, size_t size, UInt32 *buf_id_out)
{
	return vdma_alloc_buf(ENX_VDMA_FONT, kind, size, buf_id_out);
}

Int32 ENX_DMA_Font_Buffer_Free(void *pBuf, size_t buf_size)
{
	return vdma_free(ENX_VDMA_FONT, pBuf, buf_size);
}

/* Stop all active video-core font updates. Caller MUST hold g_font_cfg_lock. */
static Int32 font_stop_all_locked(void);

VDMA_FONT_CONFIG_S* ENX_DMA_Font_CfgAlloc(UInt32 blit_count, UInt32 yc_index)
{
	struct vdma_dev *dev;
	size_t total_size = sizeof(VDMA_FONT_CONFIG_S) + (sizeof(VDMA_FONT_INFO_S) * blit_count);
	VDMA_FONT_CONFIG_S *out_cfg = NULL;
	Int32 ret;
	int slot;

	/* get font device */
	ret = dev_get(ENX_VDMA_FONT, &dev);
	if (ret) {
		EPRINTF("dma font is not initialized.\n");
		errno = ret;
		return NULL;	/* no ref held on dev_get() failure */
	}

	/* check font number (no shared state — before taking the cfg lock) */
	if (blit_count == 0 || blit_count > dev->max_src) {
		EPRINTF("Invalid font number: req :%u, limit: %u\n", blit_count, dev->max_src);
		errno = ENX_ERR_VDMA_INVALID_ARG;
		dev_put(dev);
		return NULL;
	}

	pthread_mutex_lock(&g_font_cfg_lock);

	/* blit-count quota (video-core mode) */
	if (g_font_cfg_inst.active_mode &&
		(g_font_cfg_inst.acum_blit_cnt + (int)blit_count) > (int)dev->max_src) {
		EPRINTF("Font blit count quota exceeded: acum_blit_cnt: %u, max_src: %u\n",
			g_font_cfg_inst.acum_blit_cnt, dev->max_src);
		errno = ENX_ERR_VDMA_QUOTA_EXCEEDED;
		goto out;
	}

	/* config-array quota */
	if (g_font_cfg_inst.cfg_count >= MAX_VISP_YC_NUM) {
		EPRINTF("Font config array quota exceeded: cfg_cnt: %u, max_cnt: %u\n",
			g_font_cfg_inst.cfg_count, MAX_VISP_YC_NUM);
		errno = ENX_ERR_VDMA_QUOTA_EXCEEDED;
		goto out;
	}

	/* reserve the slot under the lock; commit cfg_count only after success */
	slot = g_font_cfg_inst.cfg_count;
	g_font_cfg_inst.pFontCfg[slot] = (VDMA_FONT_CONFIG_S*)calloc(1, total_size);
	if (!g_font_cfg_inst.pFontCfg[slot]) {
		EPRINTF("Failed to allocate memory for font configuration\n");
		errno = ENX_ERR_VDMA_NO_MEMORY;
		goto out;
	}
	g_font_cfg_inst.pFontCfg[slot]->font_number = blit_count;
	g_font_cfg_inst.cfg_count = slot + 1;	/* commit */
	if (g_font_cfg_inst.active_mode) {
		g_font_cfg_inst.acum_blit_cnt	+= (int)blit_count;
		g_font_cfg_inst.blit_cnt[slot]	 = blit_count;
		g_font_cfg_inst.yc_idx[slot]	 = yc_index;
	}

	out_cfg = g_font_cfg_inst.pFontCfg[slot];
out:
	pthread_mutex_unlock(&g_font_cfg_lock);
	dev_put(dev);
	return out_cfg;
}

VDMA_FONT_CONFIG_S* ENX_DMA_Font_CfgRealloc(VDMA_FONT_CONFIG_S* VdmaFontConfig, UInt32 blit_count, UInt32 yc_index)
{
	struct vdma_dev *dev;
	VDMA_FONT_CONFIG_S* new_config;
	VDMA_FONT_CONFIG_S* out_cfg = NULL;
	size_t total_size = sizeof(VDMA_FONT_CONFIG_S) + (sizeof(VDMA_FONT_INFO_S) * blit_count);
	Int32 ret, search = -1, diff = 0;

	/* check config pointer */
	if (!VdmaFontConfig) {
		EPRINTF("Font config pointer is null.\n");
		errno = ENX_ERR_VDMA_INVALID_ARG;
		return NULL;
	}

	/* get font device */
	ret = dev_get(ENX_VDMA_FONT, &dev);
	if (ret) {
		EPRINTF("dma font is not initialized.\n");
		errno = ret;
		return NULL;	/* no ref held on dev_get() failure */
	}

	/* check font number (no shared state — before taking the cfg lock) */
	if (blit_count == 0 || blit_count > dev->max_src) {
		EPRINTF("Invalid font number: req :%u, limit: %u\n", blit_count, dev->max_src);
		errno = ENX_ERR_VDMA_INVALID_ARG;
		dev_put(dev);
		return NULL;
	}

	pthread_mutex_lock(&g_font_cfg_lock);

	/* Index search */
	for (int i = 0; i < g_font_cfg_inst.cfg_count; i++) {
		if (g_font_cfg_inst.pFontCfg[i] == VdmaFontConfig) { search = i; break; }
	}

	if (search == -1) {
		EPRINTF("Font config pointer not found in the instance array.\n");
		errno = ENX_ERR_VDMA_INVALID_ARG;
		goto out;
	}

	/* Re-allocation conditions check */
	if (g_font_cfg_inst.active_mode) {
		/* Stop active updates before changing yc index or font number.
		 * We already hold g_font_cfg_lock, so use the _locked variant. */
		font_stop_all_locked();

		if (blit_count == g_font_cfg_inst.blit_cnt[search]) {
			g_font_cfg_inst.yc_idx[search] = yc_index;
			out_cfg = VdmaFontConfig;
			goto out;
		}
		diff = (Int32)blit_count - (Int32)g_font_cfg_inst.blit_cnt[search];
		if (diff > 0 &&
			(g_font_cfg_inst.acum_blit_cnt + diff) > (int)dev->max_src) {
			EPRINTF("Font blit count quota exceeded: req_diff: %u, current_acum: %u, max_src: %u\n",
				diff, g_font_cfg_inst.acum_blit_cnt, dev->max_src);
			errno = ENX_ERR_VDMA_QUOTA_EXCEEDED;
			goto out;
		}
	}

	new_config = (VDMA_FONT_CONFIG_S*)realloc(VdmaFontConfig, total_size);
	if (!new_config) {
		EPRINTF("Failed to reallocate memory for font configuration\n");
		errno = ENX_ERR_VDMA_NO_MEMORY;
		goto out;
	}
	g_font_cfg_inst.pFontCfg[search] = new_config;
	new_config->font_number = blit_count;

	if (g_font_cfg_inst.active_mode) {
		g_font_cfg_inst.acum_blit_cnt += diff;	/* diff may be negative (shrink) */
		g_font_cfg_inst.blit_cnt[search] = blit_count;
		g_font_cfg_inst.yc_idx[search] = yc_index;
	}

	out_cfg = g_font_cfg_inst.pFontCfg[search];
out:
	pthread_mutex_unlock(&g_font_cfg_lock);
	dev_put(dev);
	return out_cfg;
}

void ENX_DMA_Font_CfgFree(void)
{
	pthread_mutex_lock(&g_font_cfg_lock);
	for (Int32 i = 0; i < g_font_cfg_inst.cfg_count; i++) {
		if (g_font_cfg_inst.pFontCfg[i]) {
			free(g_font_cfg_inst.pFontCfg[i]);
			g_font_cfg_inst.pFontCfg[i] = NULL;
		}
		g_font_cfg_inst.blit_cnt[i]	 = 0;
		g_font_cfg_inst.yc_idx[i]	 = 0;
	}
	g_font_cfg_inst.cfg_count = 0;
	if (g_font_cfg_inst.active_mode)
		g_font_cfg_inst.acum_blit_cnt = 0;
	pthread_mutex_unlock(&g_font_cfg_lock);
}

Int32 ENX_DMA_Font_Update(VDMA_FONT_CONFIG_S* VdmaFontConfig)
{
	struct vdma_dev *dev;
	vdma_buf_t **src = NULL;
	vdma_buf_t *dst = NULL;
	Int32 status = VDMA_OK;
	Int32 i, j, search = -1;

	/* check config pointer */
	if (VdmaFontConfig == NULL) {
		EPRINTF("Font config pointer is null.\n");
		return ENX_ERR_VDMA_CONFIG_NOT_ALLOCATION;
	}

	/* get font device */
	status = dev_get(ENX_VDMA_FONT, &dev);
	if (status) {
		EPRINTF("dma font is not initialized.\n");
		return status;
	}

	pthread_mutex_lock(&g_font_cfg_lock);

	/* Allocated config index search */
	for (i = 0; i < g_font_cfg_inst.cfg_count; i++) {
		if (g_font_cfg_inst.pFontCfg[i] == VdmaFontConfig) { search = i; break; }
	}

	if (search == -1) {
		EPRINTF("Font config pointer not found in the instance array.\n");
		pthread_mutex_unlock(&g_font_cfg_lock);
		dev_put(dev);
		return ENX_ERR_VDMA_INVALID_ARG;
	}

	src = calloc(VdmaFontConfig->font_number, sizeof(vdma_buf_t *));
	if (!src) {
		EPRINTF("Failed to allocate memory for source buffer pointers.\n");
		pthread_mutex_unlock(&g_font_cfg_lock);
		dev_put(dev);
		return ENX_ERR_VDMA_NO_MEMORY;
	}

	/*
	 * If the buffer kind is a virtual address,
	 * convert it to ID mode so the kernel driver can process it in kernel space.
	 * The user's private data is safely preserved to prevent corruption.
	 */
	UInt32 blit_cnt = VdmaFontConfig->font_number;
	UInt32 tmp_dst_flag = VdmaFontConfig->dst_type_index;
	UInt64 tmp_dst_addr = VdmaFontConfig->font_info[0].dst_addr_y;
	UInt32 tmp_src_flag[blit_cnt];
	UInt64 tmp_src_addr[blit_cnt];
	for (j=0; j < (Int32)blit_cnt; j++) {
		tmp_src_flag[j] = VdmaFontConfig->font_info[j].src_addr_flag;
		tmp_src_addr[j] = VdmaFontConfig->font_info[j].src_addr;
	}

	if (g_font_cfg_inst.active_mode) {
		/* font_number check */
		if (VdmaFontConfig->font_number > g_font_cfg_inst.blit_cnt[search]) {
			EPRINTF("Font number exceeds allocated size: %u (allocated size: %u)\n", VdmaFontConfig->font_number, g_font_cfg_inst.blit_cnt[search]);
			status = -ENX_ERR_VDMA_FONT_MAX_SIZE_EXCEEDED;
			goto out;
		}

		/* calcul array offset */
		VdmaFontConfig->dst_type_index = 0;
		for (i=0; i<search; i++) {
			VdmaFontConfig->dst_type_index += g_font_cfg_inst.blit_cnt[i];
		}
	} else {
		/* check dst_addr validity */
		status = verify_buf_info(dev, &VdmaFontConfig->dst_type_index, &VdmaFontConfig->font_info[0].dst_addr_y, &dst, "font dst");
		if (status) { goto out; }
	}

	/* check src_addr validity */
	char label[32];
	for (j=0; j < (Int32)VdmaFontConfig->font_number; j++) {
		snprintf(label, sizeof(label), "font_info[%u] src", j);
		status = verify_buf_info(dev, &VdmaFontConfig->font_info[j].src_addr_flag, &VdmaFontConfig->font_info[j].src_addr, &src[j], label);
		if (status) { goto out; }
	}

	/* font submit */
	status = dev_ioctl(dev, VDMAIOSET_FONT_SUBMIT, VdmaFontConfig);
	if (status) {
		EPRINTF("dma font update failed. (%d)\n", status);
		goto out;
	}

#if SUPPORT_ACTIVE_MODE
	/* send the update command */
	if (g_font_cfg_inst.active_mode) {
		Int32 duplicate_search = -1;
		VdmaFontConfig->dst_type_index = g_font_cfg_inst.active_idx[search];
		struct vdma_font_active_update font_active_update = {
			.font_number = VdmaFontConfig->font_number,
			.yc_idx = VdmaFontConfig->dst_type_index,
			.array_paddr = VdmaFontConfig->font_info[0].reserved, // using reserved field for description paddr transfer
		};

		/* check for duplicate indexes */
		Int32 act_cnt = g_font_cfg_inst.active_cnt;
		for (i=0; i < act_cnt; i++) { if (g_font_cfg_inst.active_idx[i] == VdmaFontConfig->dst_type_index) { duplicate_search = i; break; } }

		/* Check if the update request is for an existing YC when update_cnt is at its maximum */
		if (act_cnt == MAX_VISP_YC_NUM && (duplicate_search < 0)) {
			EPRINTF("The number of font updates for YC has been exceeded. (update cnt: %u)\n", act_cnt);
			status = -ENX_ERR_VDMA_FONT_MAX_YC_IDX;
			goto out;
		}
		status = enx_send_linkmsg(LINK_HEAD_DATA_ID_MSG, LINK_VDMA_CMD_FONT_UPDATE, &font_active_update, sizeof(font_active_update), TRUE);
		if(status != VDMA_OK) {
			EPRINTF("dma font update failed : 0x%x\n", status);
			goto out;
		} else {
			/* Duplicate check & added yc update list */
			if (duplicate_search < 0) {
				g_font_cfg_inst.active_idx[act_cnt] = VdmaFontConfig->dst_type_index;
				g_font_cfg_inst.active_cnt++;
			}
		}
	}
#endif

out:
	/* Restore original buffer information */
	VdmaFontConfig->dst_type_index = tmp_dst_flag;
	VdmaFontConfig->font_info[0].dst_addr_y = tmp_dst_addr;
	for (j = 0; j < (Int32)blit_cnt; j++) {
		VdmaFontConfig->font_info[j].src_addr_flag = tmp_src_flag[j];
		VdmaFontConfig->font_info[j].src_addr = tmp_src_addr[j];
	}
	if (src) {
		for (j=0; j < (Int32)VdmaFontConfig->font_number; j++) {
			if (src[j]) buf_put(dev, src[j]);
		}
		free(src);
	}
	if (dst) buf_put(dev, dst);
	pthread_mutex_unlock(&g_font_cfg_lock);
	dev_put(dev);
	return status;
}

Int32 ENX_DMA_Font_Stop(UInt32 yc_num)
{
	Int32 status = VDMA_OK;

#if SUPPORT_ACTIVE_MODE
	pthread_mutex_lock(&g_font_cfg_lock);

	if (g_font_cfg_inst.active_mode) {
		Int32 act_cnt = g_font_cfg_inst.active_cnt;
		Int32 search = -1;
		struct vdma_font_stop_ctrl send_stop = {0,};

		/* check stop yc number */
		for (Int32 i = 0; i < act_cnt; i++) { if (g_font_cfg_inst.yc_idx[i] == yc_num) { search = i; break; } }
		if (search < 0) {
			HPRINTF("No font update for yc_num: %u\n", yc_num);
			goto out;
		}
		send_stop.stop_cnt = 1;
		send_stop.stop_yc_idx[0] = yc_num; // stop target yc_num

		status = enx_send_linkmsg(LINK_HEAD_DATA_ID_MSG, LINK_VDMA_CMD_FONT_STOP, &send_stop, sizeof(send_stop), TRUE);
		if (status) {
			EPRINTF("dma font stop failed. (0x%x)\n", status);
		} else {
			/* swap-remove the stopped yc from the active list */
			g_font_cfg_inst.active_cnt--;
			if (search != (act_cnt - 1))
				g_font_cfg_inst.active_idx[search] = g_font_cfg_inst.active_idx[act_cnt - 1];
			g_font_cfg_inst.active_idx[act_cnt - 1] = 0; // clear last index
		}
	}
	/* else: dma font stop is not used in kernel mode. */

out:
	pthread_mutex_unlock(&g_font_cfg_lock);
#endif
	return status;
}

/*
 * Stop all active video-core font updates.
 * Caller MUST hold g_font_cfg_lock (used directly by CfgRealloc, which already
 * holds the lock; the public ENX_DMA_Font_Stop_All() wraps it).
 */
static Int32 font_stop_all_locked(void)
{
	Int32 status = VDMA_OK;
	struct vdma_font_stop_ctrl send_stop = {0,};

	if (!g_font_cfg_inst.active_mode)	/* not used in kernel mode */
		return status;
	if (g_font_cfg_inst.active_cnt == 0)
		return status;
#if SUPPORT_ACTIVE_MODE
	send_stop.stop_cnt = g_font_cfg_inst.active_cnt;
	for (Int32 i = 0; i < send_stop.stop_cnt; i++)
		send_stop.stop_yc_idx[i] = g_font_cfg_inst.active_idx[i]; // stop target yc_num list

	status = enx_send_linkmsg(LINK_HEAD_DATA_ID_MSG, LINK_VDMA_CMD_FONT_STOP, &send_stop, sizeof(send_stop), TRUE);
	if (status) {
		EPRINTF("dma font stop failed. (0x%x)\n", status);
		return status;
	}

	/* clear all active info */
	g_font_cfg_inst.active_cnt = 0;
	for (Int32 i = 0; i < send_stop.stop_cnt; i++)
		g_font_cfg_inst.active_idx[i] = 0; // clear active yc index array
	HPRINTF("All active fonts are stopped. Stopped yc_num count: %u\n", send_stop.stop_cnt);
	return status;
#else
	(void)(send_stop);
	return status;
#endif
}

Int32 ENX_DMA_Font_Stop_All(void)
{
	Int32 status;
	pthread_mutex_lock(&g_font_cfg_lock);
	status = font_stop_all_locked();
	pthread_mutex_unlock(&g_font_cfg_lock);
	return status;
}

/*******************************************************************************
 *
 * DMA-JPEG common functions
 *
*******************************************************************************/

static void ENX_DMA_JPEG_Discard(ENX_VDMA_FUNC_TYPE_E func)
{
	struct vdma_dev *dev;
	Int32 ret;

	ret = dev_get(func, &dev);
	if (ret) {
		EPRINTF("dma jpeg-%s is not initialized.\n", func == ENX_VDMA_JPEG_ENC ? "enc" : "dec");
		errno = ret;
		return;
	}

	/* JPEG Discard */
	if (dev_ioctl(dev, VDMAIOSET_JPEG_DISCARD, 0) < 0) {
		EPRINTF("Failed to call VDMAIOSET_JPEG_DISCARD");
	}
	dev_put(dev);
}

/*******************************************************************************
 *
 * DMA-JPEG ENC functions
 *
*******************************************************************************/

Int32 ENX_DMA_JpegEnc_Init()
{
	return vdma_open(ENX_VDMA_JPEG_ENC, ENX_VDMA_INIT_TYPE_KERNEL);
}

void ENX_DMA_JpegEnc_Exit(void)
{
	(void)vdma_close(ENX_VDMA_JPEG_ENC);
}

void* ENX_DMA_JpegEnc_Buffer_Alloc(ENX_VDMA_BUF_KIND_E kind, size_t size)
{
	return vdma_alloc_buf(ENX_VDMA_JPEG_ENC, kind, size, NULL);
}

void* ENX_DMA_JpegEnc_Buffer_Alloc_EX(ENX_VDMA_BUF_KIND_E kind, size_t size, UInt32 *buf_id_out)
{
	return vdma_alloc_buf(ENX_VDMA_JPEG_ENC, kind, size, buf_id_out);
}

Int32 ENX_DMA_JpegEnc_Buffer_Free(void *pBuf, size_t buf_size)
{
	return vdma_free(ENX_VDMA_JPEG_ENC, pBuf, buf_size);
}

Int32 ENX_DMA_JpegEnc_Execute(VDMA_JPEG_ENC_CONFIG_S* gVdmaJpegEncCfg, size_t *jpeg_size_out)
{
	struct vdma_dev *dev = NULL;
	vdma_buf_t *src = NULL, *dst = NULL;
	struct enx_en683_jpegenc_submit_args jea = {0,};
	Int32 ret = VDMA_OK;

	if (!gVdmaJpegEncCfg) {
		EPRINTF("JPEG enc config is nill.\n");
		return ENX_ERR_VDMA_CONFIG_NOT_ALLOCATION;
	}

	if (!jpeg_size_out) {
		EPRINTF("JPEG enc output size pointer is nill.\n");
		return ENX_ERR_VDMA_INVALID_ARG;
	}

	/* check init */
	ret = dev_get(ENX_VDMA_JPEG_ENC, &dev);
	if (ret) {
		EPRINTF("dma jpeg-enc is not initialized.\n");
		return ret;
	}

	/* copy config to internal struct for ioctl transfer */
	memcpy(&jea.cfg, gVdmaJpegEncCfg, sizeof(jea.cfg));

	/* check src_addr validity */
	ret = verify_buf_info(dev, &jea.cfg.src_addr_flag, (uint64_t *)&jea.cfg.src_addr, &src, "jpeg-enc src");
	if (ret) { goto out; }

	/* check dst_addr validity */
	ret = verify_buf_info(dev, &jea.cfg.dst_addr_flag, (uint64_t *)&jea.cfg.dst_addr, &dst, "jpeg-enc dst");
	if (ret) { goto out; }

	/* submit */
	ret = dev_ioctl(dev, VDMAIOSET_JPEG_ENC_SUBMIT, &jea);
	if (ret) {
		EPRINTF("DMA jpeg-enc submit failed: %d\n", ret);
		goto out;
	}

	/* check resp */
	if (jea.resp.process_error.value) {
		EPRINTF("Failed to encoding: proc 0x%08x ",jea.resp.process_error.value);
		if (jea.resp.process_error.err_occur) EPRINTF("\t[OCCUR]\n");
		if (jea.resp.process_error.err_file_ovf) EPRINTF("\t[FILE OVERFLOW]\n");
		ret = ENX_ERR_VDMA_JPEG_ENC_FAIL;
	} else {
		*jpeg_size_out = jea.resp.jpeg_size;
	}

out:
	if (dst) buf_put(dev, dst);
	if (src) buf_put(dev, src);
	dev_put(dev);
	return ret;
}

void ENX_DMA_JpegEnc_Discard(void)
{
	ENX_DMA_JPEG_Discard(ENX_VDMA_JPEG_ENC);
}

/*******************************************************************************
 *
 * DMA-JPEG DEC functions
 *
*******************************************************************************/

Int32 ENX_DMA_JpegDec_Init()
{
	return vdma_open(ENX_VDMA_JPEG_DEC, ENX_VDMA_INIT_TYPE_KERNEL);
}

void ENX_DMA_JpegDec_Exit(void)
{
	(void)vdma_close(ENX_VDMA_JPEG_DEC);
}

void* ENX_DMA_JpegDec_Buffer_Alloc(ENX_VDMA_BUF_KIND_E kind, size_t size)
{
	return vdma_alloc_buf(ENX_VDMA_JPEG_DEC, kind, size, NULL);
}

void* ENX_DMA_JpegDec_Buffer_Alloc_EX(ENX_VDMA_BUF_KIND_E kind, size_t size, UInt32 *buf_id_out)
{
	return vdma_alloc_buf(ENX_VDMA_JPEG_DEC, kind, size, buf_id_out);
}

Int32 ENX_DMA_JpegDec_Buffer_Free(void *pBuf, size_t buf_size)
{
	return vdma_free(ENX_VDMA_JPEG_DEC, pBuf, buf_size);
}

Int32 ENX_DMA_JpegDec_Execute(VDMA_JPEG_DEC_CONFIG_S* gVdmaJpegDecCfg, size_t *yuv_size_out)
{
	struct vdma_dev *dev = NULL;
	vdma_buf_t *src = NULL, *dst = NULL;
	struct enx_en683_jpegdec_submit_args jda = {0,};
	Int32 ret = VDMA_OK;

	if (!gVdmaJpegDecCfg) {
		EPRINTF("JPEG dec config is nill.\n");
		return ENX_ERR_VDMA_CONFIG_NOT_ALLOCATION;
	}

	/* check init */
	ret = dev_get(ENX_VDMA_JPEG_DEC, &dev);
	if (ret) {
		EPRINTF("dma jpeg-dec is not initialized.\n");
		return ret;
	}

	/* copy config to internal struct for ioctl transfer */
	memcpy(&jda.cfg, gVdmaJpegDecCfg, sizeof(jda.cfg));

	/* check src validity */
	ret = verify_buf_info(dev, &jda.cfg.src_addr_flag, (uint64_t *)&jda.cfg.src_addr, &src, "jpeg-dec src");
	if (ret) { goto out; }

	/* check dst validity */
	ret = verify_buf_info(dev, &jda.cfg.dst_addr_flag, (uint64_t *)&jda.cfg.dst_addr, &dst, "jpeg-dec dst");
	if (ret) { goto out; }

	/* submit */
	ret = dev_ioctl(dev, VDMAIOSET_JPEG_DEC_SUBMIT, &jda);
	if (ret) {
		EPRINTF("DMA jpeg-dec submit failed: %d\n", ret);
		goto out;
	}

	/* check resp */
	if (jda.resp.header_error.value || jda.resp.process_error.value) {
		if (jda.resp.header_error.value) {
			EPRINTF("Failed to decoding: head 0x%08x ",jda.resp.header_error.value);
			if (jda.resp.header_error.dri_err)				printf("\t[DRI]\n");
			if (jda.resp.header_error.dqt_err)				printf("\t[DQT:0x%x]\n",	jda.resp.header_error.dqt_err);
			if (jda.resp.header_error.dht_err)				printf("\t[DHT:0x%x]\n",	jda.resp.header_error.dht_err);
			if (jda.resp.header_error.sof0_err)				printf("\t[SOF0:0x%x]\n",	jda.resp.header_error.sof0_err);
			if (jda.resp.header_error.sos_err)				printf("\t[SOS:0x%x]\n",	jda.resp.header_error.sos_err);
		}

		if (jda.resp.process_error.value) {
			EPRINTF("Failed to decoding: proc 0x%08x ", jda.resp.process_error.value);
			if (jda.resp.process_error.err_occur) 			printf("\t[OCCUR]\n");
			if (jda.resp.process_error.err_cycle) 			printf("\t[CYCLE]\n");
			if (jda.resp.process_error.err_rctl) 			printf("\t[RCTL]\n");
			if (jda.resp.process_error.err_header_proc) 	printf("\t[HEADER:0x%x]\n",	jda.resp.process_error.err_header_proc);
			if (jda.resp.process_error.err_ecs_proc) 		printf("\t[ECS:0x%x]\n",	jda.resp.process_error.err_ecs_proc);
			if (jda.resp.process_error.err_vld) 			printf("\t[VLD:0x%x]\n",	jda.resp.process_error.err_vld);
		}

		if (jda.resp.header_exist.unsup_exist) {
			EPRINTF("Unsupported header: marker 0x%02x\n", jda.resp.unsupport_marker.unsup_mrk);
		}
		ret = ENX_ERR_VDMA_JPEG_DEC_FAIL;
	}

	if (yuv_size_out) *yuv_size_out = jda.resp.dst_size;

out:
	if (dst) buf_put(dev, dst);
	if (src) buf_put(dev, src);
	dev_put(dev);
	return ret;
}

void ENX_DMA_JpegDec_Discard(void)
{
	ENX_DMA_JPEG_Discard(ENX_VDMA_JPEG_DEC);
}

/*******************************************************************************
 *
 * DMA-NPU functions
 *
*******************************************************************************/

Int32 ENX_DMA_Npu_Init()
{
	return vdma_open(ENX_VDMA_NPU, ENX_VDMA_INIT_TYPE_KERNEL);
}

void ENX_DMA_Npu_Exit(void)
{
	(void)vdma_close(ENX_VDMA_NPU);
}

void* ENX_DMA_Npu_Buffer_Alloc(ENX_VDMA_BUF_KIND_E kind, size_t size)
{
	return vdma_alloc_buf(ENX_VDMA_NPU, kind, size, NULL);
}

void* ENX_DMA_Npu_Buffer_Alloc_EX(ENX_VDMA_BUF_KIND_E kind, size_t size, UInt32 *buf_id_out)
{
	return vdma_alloc_buf(ENX_VDMA_NPU, kind, size, buf_id_out);
}

Int32 ENX_DMA_Npu_Buffer_Free(void *pBuf, size_t buf_size)
{
	return vdma_free(ENX_VDMA_NPU, pBuf, buf_size);
}

Int32 ENX_DMA_Npu_Execute(VDMA_NPU_CONFIG_S* gVdmaNpuCfg)
{
	struct vdma_dev *dev = NULL;
	vdma_buf_t *src = NULL, *dst = NULL;
	Int32 ret = VDMA_OK;
	UInt32 tmp_src_flag = VDMA_BUF_POISON, tmp_dst_flag = VDMA_BUF_POISON;
	UInt64 tmp_src_addr, tmp_dst_addr;

	/* check npu init */
	ret = dev_get(ENX_VDMA_NPU, &dev);
	if (ret) {
		EPRINTF("dma NPU is not initialized.\n");
		ret = ENX_ERR_VDMA_NOT_INITIALIZED;
		goto out;
	}

	/* check npu config */
	if (!gVdmaNpuCfg) {
		EPRINTF("NPU config is nill.\n");
		ret = ENX_ERR_VDMA_CONFIG_NOT_ALLOCATION;
		goto out;
	}

	tmp_src_flag = gVdmaNpuCfg->src_addr_flag;
	tmp_src_addr = gVdmaNpuCfg->src_addr;
	/* check src_addr validity */
	ret = verify_buf_info(dev, &gVdmaNpuCfg->src_addr_flag, &gVdmaNpuCfg->src_addr, &src, "npu src");
	if (ret) { goto out; }

	tmp_dst_flag = gVdmaNpuCfg->dst_addr_flag;
	tmp_dst_addr = gVdmaNpuCfg->dst_addr;
	/* check dst_addr validity */
	ret = verify_buf_info(dev, &gVdmaNpuCfg->dst_addr_flag, &gVdmaNpuCfg->dst_addr, &dst, "npu dst");
	if (ret) { goto out; }

	/* submit */
	ret = dev_ioctl(dev, VDMAIOSET_NPU_SUBMIT, gVdmaNpuCfg);
	if (ret) {
		EPRINTF("DMA NPU submit failed: %d\n", ret);
		goto out;
	}

out:
	/* recover original flags and addresses */
	if (tmp_src_flag != VDMA_BUF_POISON) {
		gVdmaNpuCfg->src_addr_flag = tmp_src_flag;
		gVdmaNpuCfg->src_addr = tmp_src_addr;
	}
	if (tmp_dst_flag != VDMA_BUF_POISON) {
		gVdmaNpuCfg->dst_addr_flag = tmp_dst_flag;
		gVdmaNpuCfg->dst_addr = tmp_dst_addr;
	}
	/* release transient refs */
	if (dst) buf_put(dev, dst);
	if (src) buf_put(dev, src);
	dev_put(dev);
	return ret;
}