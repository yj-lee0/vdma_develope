// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Eyenix Corporation
 * Eyenix <support6@eyenix.com>
 *
 * Eyenix VDMA - core helpers (function-agnostic)
 *
 * Provides:
 *  - per-fd session lifecycle (open/release/mmap/poll)
 *  - common ioctl helpers (ALLOC/FREE/SUBMIT/WAIT/EXPORT/IMPORT)
 *  - generic SUBMIT helper (function-specific HW programming via core_ops)
 *  - CMA buffer alloc/free + mmap with VMA refcount
 *  - cross-fd buffer sharing (EXPORT/IMPORT via anon_inode)
 *  - workqueue-driven job execution + completion
 *  - device registration helpers for per-HW drivers
 *
 * Function-specific SUBMIT logic (vdma_font, vdma_dz, ...) lives in
 * per-HW driver modules under their own directories (font-drv/, dz-drv/, ...).
 */

#include <linux/anon_inodes.h>
#include <linux/cdev.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/errname.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#include "enx-vdma.h"

#define ENX_VDMA_MAX_DEVS	8	/* scaler0, scaler1, rotation, font, jpegenc, jpegdec ... reserve */

/*
 * /sys/class/enx_vdma/ Device node registration
 */
static struct class		*enx_vdma_class;
static dev_t			 enx_vdma_devid_base;	/* MKDEV(major, 0) */
static DEFINE_IDA(enx_vdma_minor_ida);

/*******************************************************************************
 *
 * Global status tracking (across all devices - debugfs)
 *
*******************************************************************************/

/* debugfs root: /sys/kernel/debug/enx_vdma/ */
static struct dentry	*enx_vdma_dbg_root;

/* Global list of all active backings for debug/tracking. */
static LIST_HEAD(enx_vdma_backings_all);
static DEFINE_SPINLOCK(enx_vdma_backings_all_lock);

/* Global counter — alive backing object count across all devices.
 * Compared with sum of per-dev bufs_active to detect cross-device sharing. */
static atomic_t			 enx_vdma_backings_active;	/* Total active backing objects across all devices */
static atomic_t			 enx_vdma_alloc_active;		/* Total active allocated buffers across all devices */
static atomic64_t		 enx_vdma_buf_bytes_inuse;	/* Total active allocated bytes across all devices */
static atomic64_t 		 enx_vdma_alloc_total;		/* Accumulated alloc calls (successful or not) */
static atomic64_t 		 enx_vdma_free_total;		/* Accumulated free calls (successful or not) */

/*******************************************************************************
 *
 * eyenix define / config debuglog
 *
*******************************************************************************/

#define DEVICE_NAME "ENX_VDMA"
#define DEBUGLOG_ERROR // Comment out if not in use.
#define DEBUGLOG_CHECK // Comment out if not in use.
// #define DEBUGLOG_PRINT // Comment out if not in use.
// #define DEBUGLOG_ENTER // Comment out if not in use.
// #define DEBUGLOG_EXIT  // Comment out if not in use.
#include <soc/eyenix/debuglog.h>

/*******************************************************************************
 *
 * Backing (shared physical memory) + Buffer view (per-device, per-session)
 *
*******************************************************************************/

static void enx_vdma_backing_release(struct kref *ref)
{
	struct enx_vdma_backing *bk = container_of(ref, struct enx_vdma_backing, ref);
	size_t size = bk->cmm ? bk->cmm->nbytes : 0;

	/* delete global tracking list. */
	spin_lock(&enx_vdma_backings_all_lock);
	list_del(&bk->node);
	spin_unlock(&enx_vdma_backings_all_lock);

	if (bk->cmm) {
		printlog("enx_vdma_backing_release: free cmm: %s(%#lx), size %zu\n", bk->cmm->name, bk->cmm->phys_start, size);
		ecmm_free(bk->cmm);
	}
	atomic_dec(&enx_vdma_backings_active);
	atomic64_inc(&enx_vdma_free_total);
	atomic64_sub((s64)size, &enx_vdma_buf_bytes_inuse);
	kfree(bk);
}

static struct enx_vdma_backing *enx_vdma_backing_alloc(const char *name, size_t size)
{
	struct enx_vdma_backing *bk;

	bk = kzalloc(sizeof(*bk), GFP_KERNEL);
	if (!bk)
		return NULL;

	bk->cmm = ecmm_alloc(name, size);
	if (!bk->cmm) {
		kfree(bk);
		return NULL;
	}
	printlog("enx_vdma_backing_alloc: alloc cmm: %s(%#lx), size %zu\n", bk->cmm->name, bk->cmm->phys_start, size);
	bk->cmm->pvirt = page_address(bk->cmm->ppage);
	kref_init(&bk->ref);

	spin_lock(&enx_vdma_backings_all_lock);
	list_add(&bk->node, &enx_vdma_backings_all);
	spin_unlock(&enx_vdma_backings_all_lock);
	atomic_inc(&enx_vdma_backings_active);
	atomic64_inc(&enx_vdma_alloc_total);
	atomic64_add((s64)size, &enx_vdma_buf_bytes_inuse);
	return bk;
}

static void enx_vdma_buf_release(struct kref *ref)
{
	struct enx_vdma_buf *buf = container_of(ref, struct enx_vdma_buf, ref);
	struct enx_vdma_dev *dev = buf->dev;

	/* xa entry was erased on owner-free / session-release; defensive erase
	 * in case last ref hit before erase (e.g. failed insertion paths). */
	xa_erase(&dev->buf_xa, buf->id);

	atomic_dec(&dev->bufs_active);
	if (buf->backing) {
		atomic64_sub(enx_vdma_buf_size(buf), &dev->bufs_bytes_inuse);
		kref_put(&buf->backing->ref, enx_vdma_backing_release);
	}
	atomic_dec(&enx_vdma_alloc_active);
	kfree(buf);
}

static struct enx_vdma_buf *enx_vdma_buf_lookup_get(struct enx_vdma_dev *dev,
							struct enx_vdma_sess *vs,
							u32 id)
{
	struct enx_vdma_buf *buf;

	xa_lock(&dev->buf_xa);
	buf = xa_load(&dev->buf_xa, id);
	if (!buf || buf->owner != vs) {
		xa_unlock(&dev->buf_xa);
		return NULL;
	}
	kref_get(&buf->ref);
	xa_unlock(&dev->buf_xa);
	return buf;
}

static int vdma_buffer_alloc(struct enx_vdma_sess *vs, struct vdma_alloc_args *a)
{
	struct enx_vdma_dev *dev = vs->dev;
	struct enx_vdma_backing *bk;
	struct enx_vdma_buf *buf;
	char name[ECMM_INAME_LEN_MAX];
	size_t size;
	u32 id;
	int ret;

	enterlog();

	if (a->kind != ENX_BUF_SRC && a->kind != ENX_BUF_DST) {
		ret = -EINVAL;
		goto out;
	}
	if (!a->size || a->size > SZ_64M) {
		ret = -EINVAL;
		goto out;
	}

	if (atomic_inc_return(&dev->buf_count) > dev->max_bufs) {
		atomic_dec(&dev->buf_count);
		errorlog("Session buffer count quota exceeded: buf_cnt: %u, max_cnt: %u\n", atomic_read(&dev->buf_count), dev->max_bufs);
		ret = -EDQUOT;
		goto out;
	}

	size = PAGE_ALIGN(a->size);

	snprintf(name, sizeof(name), "v%s/p%d",
		 dev->node_name ? dev->node_name + 9 : "?", vs->pid);
	bk = enx_vdma_backing_alloc(name, size);
	if (!bk) {
		atomic_dec(&dev->buf_count);
		errorlog("failed ecmm_alloc(name : %s, size : %zu).\n", name, size);
		ret = -ENOMEM;
		goto out;
	}

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf) {
		kref_put(&bk->ref, enx_vdma_backing_release);
		atomic_dec(&dev->buf_count);
		ret = -ENOMEM;
		goto out;
	}

	buf->backing = bk;	/* takes the initial backing ref */
	buf->kind = a->kind;
	buf->dev = dev;
	buf->owner = vs;
	buf->imported = false;
	INIT_LIST_HEAD(&buf->owner_node);
	kref_init(&buf->ref);

	/* memory clear */
	memset(enx_vdma_buf_vaddr(buf), 0, enx_vdma_buf_size(buf));

	ret = xa_alloc(&dev->buf_xa, &id, buf,
				XA_LIMIT(1, VDMA_BUF_ID_MAX), GFP_KERNEL);
	if (ret) {
		errorlog("xa_alloc(buf_xa) failed: %d (bufs_active=%d size=%zu)\n",
					ret, atomic_read(&dev->bufs_active), enx_vdma_buf_size(buf));
		kref_put(&bk->ref, enx_vdma_backing_release);
		kfree(buf);
		atomic_dec(&dev->buf_count);
		goto out;
	}
	buf->id = id;
	atomic_inc(&dev->bufs_active);
	atomic64_inc(&dev->stats_bufs_alloc);
	atomic64_add(enx_vdma_buf_size(buf), &dev->bufs_bytes_inuse);

	/* Global */
	atomic_inc(&enx_vdma_alloc_active);

	mutex_lock(&vs->lock);
	list_add_tail(&buf->owner_node, &vs->bufs);
	mutex_unlock(&vs->lock);

	a->id = buf->id;
	a->mmap_offset = (u64)buf->id << PAGE_SHIFT;

	printlog("alloc OK: id=%u kind=%s size=%zu pid=%d\n",
		buf->id, buf->kind == ENX_BUF_SRC ? "SRC" : "DST",
		enx_vdma_buf_size(buf), vs->pid);
out:
	exitlog();
	return ret;
}

static int vdma_buffer_free(struct enx_vdma_sess *vs, u32 id)
{
	struct enx_vdma_dev *dev = vs->dev;
	struct enx_vdma_buf *buf;
	int ret = 0;

	enterlog();

	xa_lock(&dev->buf_xa);
	buf = xa_load(&dev->buf_xa, id);
	if (!buf || buf->owner != vs) {
		ret = buf ? -EACCES : -ENOENT;
	} else {
		__xa_erase(&dev->buf_xa, id);
		buf->owner = NULL;
	}
	xa_unlock(&dev->buf_xa);
	if (ret)
		goto out;

	atomic_dec(&dev->buf_count);
	mutex_lock(&vs->lock);
	list_del_init(&buf->owner_node);
	mutex_unlock(&vs->lock);

	kref_put(&buf->ref, enx_vdma_buf_release);
out:
	exitlog();
	return ret;
}

/*******************************************************************************
 *
 * Cross-fd / cross-device buf sharing (EXPORT / IMPORT via anon_inode)
 *
 * EXPORT places a reference to the underlying &enx_vdma_backing on an
 * anon_inode file. IMPORT creates a fresh &enx_vdma_buf view in the importing
 * device's xa, sharing the same backing. This makes the mechanism work
 * uniformly across processes AND across devices (font ↔ scaler ↔ rot ↔ ...).
 *
*******************************************************************************/

static int vdma_export_release(struct inode *inode, struct file *filp)
{
	struct enx_vdma_backing *bk = filp->private_data;
	kref_put(&bk->ref, enx_vdma_backing_release);
	return 0;
}

static const struct file_operations vdma_export_fops = {
	.owner		= THIS_MODULE,
	.release	= vdma_export_release,
	.llseek		= noop_llseek,
};

static int vdma_do_export(struct enx_vdma_sess *vs, struct vdma_export_args *a)
{
	struct enx_vdma_dev *dev = vs->dev;
	struct enx_vdma_buf *buf;
	struct enx_vdma_backing *bk;
	int fd, ret = 0;

	enterlog();

	buf = enx_vdma_buf_lookup_get(dev, vs, a->id);
	if (!buf) {
		ret = -EACCES;
		goto out;
	}

	bk = buf->backing;
	kref_get(&bk->ref);	/* anon_inode owns this ref */

	fd = anon_inode_getfd("[vdma_buf]", &vdma_export_fops, bk,
					O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		kref_put(&bk->ref, enx_vdma_backing_release);
		ret = fd;
	} else {
		a->export_fd = fd;
	}

	kref_put(&buf->ref, enx_vdma_buf_release);
out:
	exitlog();
	return ret;
}

static int vdma_do_import(struct enx_vdma_sess *vs, struct vdma_import_args *a)
{
	struct enx_vdma_dev *dev = vs->dev;
	struct enx_vdma_backing *bk;
	struct enx_vdma_buf *buf, *existing;
	struct file *f;
	u32 id;
	int ret = 0;

	enterlog();

	if (a->kind != ENX_BUF_SRC && a->kind != ENX_BUF_DST) {
		errorlog("Invalid import kind: %u\n", a->kind);
		ret = -EINVAL;
		goto out;
	}

	if (atomic_inc_return(&dev->buf_count) > dev->max_bufs) {
		atomic_dec(&dev->buf_count);
		errorlog("Session buffer count quota exceeded: buf_cnt: %u, max_cnt: %u\n", atomic_read(&dev->buf_count), dev->max_bufs);
		ret = -EDQUOT;
		goto out;
	}

	f = fget(a->export_fd);
	if (!f) { ret = -EBADF; atomic_dec(&dev->buf_count); goto out; }
	if (f->f_op != &vdma_export_fops) { ret = -EINVAL; atomic_dec(&dev->buf_count); goto out_fput; }
	bk = f->private_data;

	/*
	 * Per-session dedup: Prevent multiple imports of the same backing buffer
	 * within a single session to avoid cache-aliasing issues.
	 *
	 * Hold vs->lock across check and insertion to block concurrent imports.
	 * On a dedup hit, refuse creation and populate @a with the existing
	 * view's metadata for reuse.
	 */
	mutex_lock(&vs->lock);
	list_for_each_entry(existing, &vs->bufs, owner_node) {
		if (existing->backing == bk) {
			a->id			= existing->id;
			a->kind			= existing->kind;
			a->size			= enx_vdma_buf_size(existing);
			a->mmap_offset	= (u64)existing->id << PAGE_SHIFT;
			atomic_dec(&dev->buf_count);
			mutex_unlock(&vs->lock);
			ret = -EEXIST;
			goto out_fput;
		}
	}

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf) {
		atomic_dec(&dev->buf_count);
		mutex_unlock(&vs->lock);
		ret = -ENOMEM;
		goto out_fput;
	}

	kref_get(&bk->ref);		/* the new view owns one backing ref */
	buf->backing = bk;
	buf->kind = a->kind;	/* IMPORT-side declared intent */
	buf->dev = dev;
	buf->owner = vs;
	buf->imported = true;
	INIT_LIST_HEAD(&buf->owner_node);
	kref_init(&buf->ref);

	ret = xa_alloc(&dev->buf_xa, &id, buf,
				XA_LIMIT(1, VDMA_BUF_ID_MAX), GFP_KERNEL);
	if (ret) {
		errorlog("xa_alloc(buf_xa) on import failed: %d\n", ret);
		kref_put(&bk->ref, enx_vdma_backing_release);
		kfree(buf);
		atomic_dec(&dev->buf_count);
		mutex_unlock(&vs->lock);
		goto out_fput;
	}
	buf->id = id;
	atomic_inc(&dev->bufs_active);
	atomic64_add(enx_vdma_buf_size(buf), &dev->bufs_bytes_inuse);

	atomic_inc(&enx_vdma_alloc_active);

	list_add_tail(&buf->owner_node, &vs->bufs);
	mutex_unlock(&vs->lock);

	a->id			= buf->id;
	a->kind			= buf->kind;
	a->size			= enx_vdma_buf_size(buf);
	a->mmap_offset	= (u64)buf->id << PAGE_SHIFT;

out_fput:
	fput(f);
out:
	exitlog();
	return ret;
}

/*******************************************************************************
 *
 * Job worker + completion (Processing ...)
 *
*******************************************************************************/

static void vdma_job_drop_buf_refs(struct enx_vdma_job *job)
{
	u32 i;

	for (i = 0; i < job->src_count; i++) {
		if (job->src[i].vbuf) {
			kref_put(&job->src[i].vbuf->ref, enx_vdma_buf_release);
			job->src[i].vbuf = NULL;
		}
	}
	if (job->dst.vbuf) {
		kref_put(&job->dst.vbuf->ref, enx_vdma_buf_release);
		job->dst.vbuf = NULL;
	}
}

static void vdma_job_finish(struct enx_vdma_job *job, int result)
{
	struct enx_vdma_sess *vs = job->submitter;
	struct enx_vdma_dev *dev = vs->dev;

	enterlog();

	vdma_job_drop_buf_refs(job);
	job->result = result;
	smp_store_release(&job->done, true);
	atomic_dec(&dev->in_flight);
	atomic64_inc(&dev->stats_completed);
	wake_up_all(&job->wq);
	wake_up_interruptible(&vs->poll_wq);

	exitlog();
}

static void vdma_job_worker(struct work_struct *w)
{
	struct enx_vdma_job *job = container_of(w, struct enx_vdma_job, work);
	struct enx_vdma_sess *vs = job->submitter;
	struct enx_vdma_dev *dev = vs->dev;
	const struct enx_vdma_core_ops *ops = dev->ops;
	int ret = 0;

	enterlog();

	mutex_lock(&dev->hw_lock);
	void *blit_i = job->blits;
	ret = ops->hw_run_once(vs, &job->dst, job->src, job->src_count, blit_i, job->flags);
	mutex_unlock(&dev->hw_lock);

	vdma_job_finish(job, ret);

	exitlog();
}

/*******************************************************************************
 *
 * Generic VDMA SUBMIT helper (Trigger HW logic)
 *
*******************************************************************************/

static int fill_vdma_submit_job_buf(struct enx_vdma_sess *vs, struct enx_vdma_job_buf *jbuf, struct enx_vdma_submit_buf *b)
{
	switch (b->kind) {
	case ENX_BUF_NONE:
		break;
	case ENX_BUF_ID:
		jbuf->vbuf = enx_vdma_buf_lookup_get(vs->dev, vs, b->id);
		if (!jbuf->vbuf) {
			errorlog("failed enx_vdma_buf_lookup_get. (req id : %u)\n", b->id);
			return -EACCES;
		}
		if (b->data_offset > enx_vdma_buf_size(jbuf->vbuf)) {
			errorlog("data_offset(%u) exceeds buffer(%zu).\n",
					b->data_offset, enx_vdma_buf_size(jbuf->vbuf));
			return -EINVAL;
		}
		/*
		 * Bounds (offset + size <= buf_size) are enforced only when size > 0;
 		 * size == 0 cases are left to the per-device hw_run to interpret.
		 */
		if (b->size && b->data_offset + b->size > enx_vdma_buf_size(jbuf->vbuf)) {
			errorlog("Requested size(data_offset(%u) + size(%zu)) exceeds allocated buffer(%zu).\n",
				 	b->data_offset, b->size, enx_vdma_buf_size(jbuf->vbuf));
			return -EINVAL;
		}
		jbuf->data_offset	= b->data_offset;
		jbuf->size			= b->size;
		break;

	case ENX_BUF_RAW:
		if (!b->paddr || !b->size) {
			errorlog("Invalid RAW Info (addr: %#08llX, size : %zu)\n", b->paddr, b->size);
			return -EINVAL;
		}
		if (b->paddr < DDR_BASE_ADDR || (b->paddr + b->size + b->data_offset) >= DDR_BASE_ADDR + DDR_MAX_SIZE) {
			errorlog("Address out of DDR range (addr: %#08llX, size: %zu, data_offset: %u)\n", b->paddr, b->size, b->data_offset);
			return -EINVAL;
		}
		jbuf->paddr			= b->paddr;
		jbuf->data_offset	= b->data_offset;
		jbuf->size			= b->size;
		break;

	default:
		errorlog("Unknown buf kind.(%u)\n",b->kind);
		return -EINVAL;
	}
	jbuf->kind = b->kind;

	return 0;
}

/* SYNC: not use workqueue, inline execution */
static int enx_vdma_submit_sync(struct enx_vdma_sess *vs, struct enx_vdma_submit_params *p)
{
	struct enx_vdma_dev *dev = vs->dev;
	struct enx_vdma_job *job;
	bool listed = false;
	int ret = 0, i;

	enterlog();

	atomic64_inc(&dev->stats_sync_submits);

	job = kzalloc(struct_size(job, src, p->src_count), GFP_KERNEL);
	if (!job) {
		errorlog("failed allocation job.\n");
		exitlog();
		return -ENOMEM;
	}
	INIT_LIST_HEAD(&job->node);
	job->submitter = vs;
	job->user_token = p->user_token;
	job->src_count = p->src_count;
	job->blit_size = p->blit_size;
	job->flags = p->flags;
	job->blits = p->blits;

	/* dst buf get */
	ret = fill_vdma_submit_job_buf(vs, &job->dst, &p->dst);
	if (ret) {
		errorlog("failed fill_vdma_submit_job_buf -> dst.\n");
		goto cleanup;
	}

	/* src buf get */
	for (i = 0; i < p->src_count; i++) {
		ret = fill_vdma_submit_job_buf(vs, &job->src[i], &p->src[i]);
		if (ret) {
			errorlog("failed fill_vdma_submit_job_buf -> src[%d].\n", i);
			goto cleanup;
		}
	}

	/*
	 * Register the job in vs->jobs so that session_release can recover
	 * the buf refs we just acquired if hw_run_once oopses or the calling
	 * thread is otherwise killed before reaching the cleanup label.
	 * The same machinery that catches async-worker oopses is used here.
	 */
	mutex_lock(&vs->lock);
	list_add_tail(&job->node, &vs->jobs);
	listed = true;
	mutex_unlock(&vs->lock);

	ret = mutex_lock_killable(&dev->hw_lock);
	if (ret) {
		errorlog("Sync submit interrupted by fatal signal. ret=%d\n", ret);
		goto cleanup;
	}
	atomic_inc(&dev->in_flight);
	ret = dev->ops->hw_run_once(vs, &job->dst, job->src, job->src_count,
						job->blits, job->flags);
	if (!ret) atomic64_inc(&dev->stats_completed);
	atomic_dec(&dev->in_flight);
	mutex_unlock(&dev->hw_lock);

	/* Mark done before unlinking so a concurrent session_release skips us. */
	smp_store_release(&job->done, true);

cleanup:
	if (listed) {
		mutex_lock(&vs->lock);
		list_del(&job->node);
		mutex_unlock(&vs->lock);
	}
	vdma_job_drop_buf_refs(job);
	kfree(job);
	exitlog();
	return ret;
}

/* ASYNC: based workqueue path */
static int enx_vdma_submit_async(struct enx_vdma_sess *vs, struct enx_vdma_submit_params *p)
{
	struct enx_vdma_dev *dev = vs->dev;
	struct enx_vdma_job *job;
	u32 job_id;
	int ret = 0, i;

	enterlog();

	atomic64_inc(&dev->stats_async_submits);

	job = kzalloc(struct_size(job, src, p->src_count), GFP_KERNEL);
	if (!job) {
		errorlog("failed allocation job.\n");
		ret = -ENOMEM;
		goto out;
	}
	init_waitqueue_head(&job->wq);
	INIT_LIST_HEAD(&job->node);
	INIT_WORK(&job->work, vdma_job_worker);
	job->submitter = vs;
	job->user_token = p->user_token;
	job->src_count = p->src_count;
	job->blit_size = p->blit_size;
	job->flags = p->flags;

	job->blits = kmemdup(p->blits, p->blit_size, GFP_KERNEL);
	if (!job->blits) {
		errorlog("Blits information not found. (kmemdup fail)\n");
		ret = -ENOMEM;
		goto err_free;
	}

	/* dst buf get */
	ret = fill_vdma_submit_job_buf(vs, &job->dst, &p->dst);
	if (ret) {
		errorlog("failed fill_vdma_submit_job_buf -> dst.\n");
		goto err_free;
	}

	/* src buf get */
	for (i = 0; i < p->src_count; i++) {
		ret = fill_vdma_submit_job_buf(vs, &job->src[i], &p->src[i]);
		if (ret) {
			errorlog("failed fill_vdma_submit_job_buf -> src[%d].\n", i);
			goto err_free;
		}
	}

	ret = xa_alloc(&dev->job_xa, &job_id, job,
				XA_LIMIT(1, UINT_MAX), GFP_KERNEL);
	if (ret) {
		errorlog("xa_alloc(job_xa) failed: %d (in_flight=%d)\n", ret, atomic_read(&dev->in_flight));
		goto err_free;
	}
	job->id = job_id;

	mutex_lock(&vs->lock);
	list_add_tail(&job->node, &vs->jobs);
	mutex_unlock(&vs->lock);

	job->submit_time = ktime_get();
	atomic_inc(&dev->in_flight);
	queue_work(dev->wq, &job->work);

	p->job_id_out = job->id;

	exitlog();
	return ret;

err_free:
	vdma_job_drop_buf_refs(job);
	kfree(job->blits);
	kfree(job);
out:
	exitlog();
	return ret;
}

int enx_vdma_submit(struct enx_vdma_sess *vs, struct enx_vdma_submit_params *p)
{
	struct enx_vdma_dev *dev = vs->dev;
	int ret = 0;

	enterlog();
	atomic64_inc(&dev->stats_submits);

	if (p->src_count == 0 || p->src_count > dev->max_src) {
		errorlog("Invalid source count. (max: %u, req: %u)\n", dev->max_src, p->src_count);
		ret = -EINVAL;
		goto out;
	}
	if (!p->blits || p->blit_size == 0) {
		errorlog("Invalid submit blits info. (size or ptr is null)");
		ret = -EINVAL;
		goto out;
	}
	if (dev->ops->blit_size && p->blit_size < (dev->ops->blit_size * p->src_count)) {
		ret = -EINVAL;
		goto out;
	}

	if (p->flags & ENX_SUBMIT_ASYNC)
		ret = enx_vdma_submit_async(vs, p);
	else
		ret = enx_vdma_submit_sync(vs, p);

out:
	if (ret < 0)
		atomic64_inc(&dev->stats_submits_failed);
	exitlog();
	return ret;
}
EXPORT_SYMBOL_GPL(enx_vdma_submit);

/* Only for async jobs */
static int vdma_do_wait(struct enx_vdma_sess *vs, struct vdma_wait_args *w)
{
	struct enx_vdma_dev *dev = vs->dev;
	struct enx_vdma_job *job;
	long timeout;
	int ret = 0;

	enterlog();

	job = xa_load(&dev->job_xa, w->job_id);
	if (!job || job->submitter != vs) {
		ret = -ENOENT;
		goto out;
	}
	if (job->flags != ENX_SUBMIT_ASYNC) {
		errorlog("Job %u is not async, cannot wait (flags=0x%x).\n", job->id, job->flags);
		ret = -EINVAL;
		goto out;
	}

	timeout = w->timeout_ms ? msecs_to_jiffies(w->timeout_ms)
				: MAX_SCHEDULE_TIMEOUT;
	ret = wait_event_interruptible_timeout(job->wq,
							smp_load_acquire(&job->done),
							timeout);
	if (ret == 0) {
		ret = -ETIMEDOUT;
		goto out;
	} else if (ret < 0) {
		goto out;
	} else {
		ret = 0; // success
	}

	/* Claim ownership atomically — only one thread proceeds with reap */
	if (xa_cmpxchg(&dev->job_xa, w->job_id, job, NULL, GFP_KERNEL) != job) {
		ret = -ENOENT;   /* someone else reaped first */
		goto out;
	}

	w->user_token = job->user_token;
	w->result = job->result;

	mutex_lock(&vs->lock);
	list_del(&job->node);
	mutex_unlock(&vs->lock);
	kfree(job->blits);
	kfree(job);
out:
	exitlog();
	return ret;
}

/*******************************************************************************
 *
 * Generic VDMA SUBMIT abort (HW reset, cleanup in-flight jobs)
 *
*******************************************************************************/

void enx_vdma_abort(struct enx_vdma_dev *dev)
{
	unsigned long flags;

	enterlog();

	if (dev->ops->hw_abort)
		dev->ops->hw_abort(dev);

	spin_lock_irqsave(&dev->irq_lock, flags);
	WRITE_ONCE(dev->hw_idle, true);
	spin_unlock_irqrestore(&dev->irq_lock, flags);

	exitlog();
}
EXPORT_SYMBOL_GPL(enx_vdma_abort);

/*******************************************************************************
 *
 * mmap VM ops
 *
*******************************************************************************/

static void vdma_vm_open(struct vm_area_struct *vma)
{
	struct enx_vdma_buf *buf = vma->vm_private_data;
	kref_get(&buf->ref);
}

static void vdma_vm_close(struct vm_area_struct *vma)
{
	struct enx_vdma_buf *buf = vma->vm_private_data;
	kref_put(&buf->ref, enx_vdma_buf_release);
}

static const struct vm_operations_struct vdma_vm_ops = {
	.open	= vdma_vm_open,
	.close	= vdma_vm_close,
};

/*******************************************************************************
 *
 * VDMA common file_operations callbacks (exported)
 *
*******************************************************************************/

int enx_vdma_open(struct inode *inode, struct file *filp)
{
	struct enx_vdma_dev *dev = container_of(inode->i_cdev, struct enx_vdma_dev, cdev);
	struct enx_vdma_sess *vs;
	int ret = 0;

	enterlog();

	vs = kzalloc(sizeof(*vs), GFP_KERNEL);
	if (!vs) {
		ret = -ENOMEM;
		goto out;
	}
	vs->dev = dev;
	mutex_init(&vs->lock);
	INIT_LIST_HEAD(&vs->bufs);
	INIT_LIST_HEAD(&vs->jobs);
	init_waitqueue_head(&vs->poll_wq);
	vs->pid = current->pid;

	filp->private_data = vs;
	if (dev->ops->session_open) {
		ret = dev->ops->session_open(vs);
		if (ret) { mutex_destroy(&vs->lock); kfree(vs); filp->private_data = NULL; goto out; }
	}

	INIT_LIST_HEAD(&vs->dev_node);
	mutex_lock(&dev->sessions_lock);
	list_add_tail(&vs->dev_node, &dev->sessions);
	mutex_unlock(&dev->sessions_lock);

	atomic_inc(&dev->sessions_open);
out:
	exitlog();
	return ret;
}
EXPORT_SYMBOL_GPL(enx_vdma_open);

int enx_vdma_release(struct inode *inode, struct file *filp)
{
	struct enx_vdma_sess *vs = filp->private_data;
	struct enx_vdma_dev *dev = vs->dev;
	struct enx_vdma_buf *buf, *bt;
	struct enx_vdma_job *job, *jt;

	enterlog();

	/* unlink from device session registry so concurrent debugfs walks
	 * cannot observe this session while it is being torn down. */
	mutex_lock(&dev->sessions_lock);
	list_del(&vs->dev_node);
	mutex_unlock(&dev->sessions_lock);

	/* drain in-flight jobs */
	list_for_each_entry_safe(job, jt, &vs->jobs, node) {
		/*
		 * Only ASYNC jobs are queued on the workqueue and thus have an
		 * initialized work_struct. A SYNC job runs inline and is removed
		 * from vs->jobs before release; it can only linger here if
		 * hw_run_once oopsed mid-flight, in which case its work_struct is
		 * still zero-initialized and must NOT be flushed.
		 */
		if (job->flags == ENX_SUBMIT_ASYNC)
			flush_work(&job->work);
		/*
		 * Defensive cleanup — when worker did not run vdma_job_finish
		 * (e.g. worker died from oops or hang abort), the buf refs the job
		 * was holding were never dropped. Release them here so the buf can
		 * actually reach refcount 0 in the buf loop below.
		 *
		 * Normal path: job->done == true and finish has already dropped these
		 * refs, so this branch is skipped.
		 */
		if (!smp_load_acquire(&job->done)) {
			pr_warn("enx_vdma: job %u (flags=0x%x) released without finish (worker died?)\n",
					job->id, job->flags);
			vdma_job_drop_buf_refs(job);
			/*
			 * Since finish() hasn't been executed yet, we decrement the in_flight
 			 * counter that was incremented at the time of submission.
			 */
			atomic_dec(&dev->in_flight);
		}

		list_del(&job->node);
		if (job->flags == ENX_SUBMIT_ASYNC) {
			xa_erase(&dev->job_xa, job->id);
			kfree(job->blits);
		}
		kfree(job);
	}

	/* drop refs on all views (allocated + imported). xa entry is removed
	 * here so concurrent lookups see the buf gone immediately. */
	list_for_each_entry_safe(buf, bt, &vs->bufs, owner_node) {
		list_del(&buf->owner_node);
		xa_erase(&dev->buf_xa, buf->id);
		WRITE_ONCE(buf->owner, NULL);
		atomic_dec(&dev->buf_count);
		kref_put(&buf->ref, enx_vdma_buf_release);
	}

	/* driver's session release callback */
	if (dev->ops->session_release)
		dev->ops->session_release(vs);

	mutex_destroy(&vs->lock);
	kfree(vs);
	atomic_dec(&dev->sessions_open);
	exitlog();
	return 0;
}
EXPORT_SYMBOL_GPL(enx_vdma_release);

int enx_vdma_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct enx_vdma_sess *vs = filp->private_data;
	struct enx_vdma_dev *dev = vs->dev;
	struct enx_vdma_buf *buf;
	u32 id = (u32)vma->vm_pgoff;
	size_t len = vma->vm_end - vma->vm_start;
	int ret = 0;

	enterlog();

	buf = enx_vdma_buf_lookup_get(dev, vs, id);
	if (!buf) {
		errorlog("failed enx_vdma_buf_lookup_get. (req id : %u)\n", id);
		ret = -EACCES;
		goto out;
	}
	if (len > enx_vdma_buf_size(buf)) {
		errorlog("Requested size(%zu) exceeds allocated buffer(%zu).\n",
			 len, enx_vdma_buf_size(buf));
		kref_put(&buf->ref, enx_vdma_buf_release);
		ret = -EINVAL;
		goto out;
	}

	vma->vm_pgoff = 0;
	vma->vm_private_data = buf;
	vma->vm_ops = &vdma_vm_ops;
	vm_flags_set(vma, VM_DONTCOPY | VM_PFNMAP);

	/* Cached mapping by default; user controls L2 cache flush explicitly. */
	ret = remap_pfn_range(vma, vma->vm_start,
					page_to_pfn(buf->backing->cmm->ppage),
					len, vma->vm_page_prot);
	if (ret) {
		errorlog("failed mmap.\n");
		kref_put(&buf->ref, enx_vdma_buf_release);
	}

out:
	exitlog();
	return ret;
}
EXPORT_SYMBOL_GPL(enx_vdma_mmap);

__poll_t enx_vdma_poll(struct file *filp, struct poll_table_struct *wait)
{
	struct enx_vdma_sess *vs = filp->private_data;
	struct enx_vdma_job *job;
	__poll_t mask = 0;

	enterlog();

	poll_wait(filp, &vs->poll_wq, wait);

	mutex_lock(&vs->lock);
	list_for_each_entry(job, &vs->jobs, node) {
		if (smp_load_acquire(&job->done)) {
			mask |= EPOLLIN | EPOLLRDNORM;
			break;
		}
	}
	mutex_unlock(&vs->lock);

	exitlog();
	return mask;
}
EXPORT_SYMBOL_GPL(enx_vdma_poll);

/*******************************************************************************
 *
 * VDMA common ioctl helpers (called from each driver's ioctl dispatcher)
 *
*******************************************************************************/

long enx_vdma_ioctl_alloc(struct enx_vdma_sess *vs, void __user *uarg)
{
	struct vdma_alloc_args a;
	int ret;
	enterlog();
	if (copy_from_user(&a, uarg, sizeof(a))) {
		errorlog("Failed to copy alloc args from user.\n");
		ret = -EFAULT;
		goto out;
	}
	ret = vdma_buffer_alloc(vs, &a);
	if (ret) goto out;
	if (copy_to_user(uarg, &a, sizeof(a))) {
		errorlog("Failed to copy alloc results to user.\n");
		ret = -EFAULT;
	}
out:
	exitlog();
	return ret;
}
EXPORT_SYMBOL_GPL(enx_vdma_ioctl_alloc);

long enx_vdma_ioctl_free(struct enx_vdma_sess *vs, void __user *uarg)
{
	struct vdma_free_args f;
	int ret = 0;
	enterlog();
	if (copy_from_user(&f, uarg, sizeof(f))) {
		errorlog("Failed to copy free args from user.\n");
		ret = -EFAULT;
	}
	if (!ret)
		ret = vdma_buffer_free(vs, f.id);
	exitlog();
	return ret;
}
EXPORT_SYMBOL_GPL(enx_vdma_ioctl_free);

long enx_vdma_ioctl_wait(struct enx_vdma_sess *vs, void __user *uarg)
{
	struct vdma_wait_args w;
	int ret;
	enterlog();
	if (copy_from_user(&w, uarg, sizeof(w))) {
		errorlog("Failed to copy wait args from user.\n");
		ret = -EFAULT;
		goto out;
	}
	ret = vdma_do_wait(vs, &w);
	if (ret) goto out;
	if (copy_to_user(uarg, &w, sizeof(w))) {
		errorlog("Failed to copy wait results to user.\n");
		ret = -EFAULT;
	}
out:
	exitlog();
	return ret;
}
EXPORT_SYMBOL_GPL(enx_vdma_ioctl_wait);

long enx_vdma_ioctl_export(struct enx_vdma_sess *vs, void __user *uarg)
{
	struct vdma_export_args e;
	int ret;
	enterlog();
	if (copy_from_user(&e, uarg, sizeof(e))) {
		errorlog("Failed to copy export args from user.\n");
		ret = -EFAULT;
		goto out;
	}
	ret = vdma_do_export(vs, &e);
	if (ret) goto out;
	if (copy_to_user(uarg, &e, sizeof(e))) {
		errorlog("Failed to copy export results to user.\n");
		ret = -EFAULT;
	}
out:
	exitlog();
	return ret;
}
EXPORT_SYMBOL_GPL(enx_vdma_ioctl_export);

long enx_vdma_ioctl_import(struct enx_vdma_sess *vs, void __user *uarg)
{
	struct vdma_import_args im;
	int ret;
	enterlog();
	if (copy_from_user(&im, uarg, sizeof(im))) {
		errorlog("Failed to copy import args from user.\n");
		ret = -EFAULT;
		goto out;
	}
	ret = vdma_do_import(vs, &im);
	if (ret) goto out;
	if (copy_to_user(uarg, &im, sizeof(im))) {
		errorlog("Failed to copy import results to user.\n");
		ret = -EFAULT;
	}
out:
	exitlog();
	return ret;
}
EXPORT_SYMBOL_GPL(enx_vdma_ioctl_import);

long enx_vdma_ioctl_abort(struct enx_vdma_sess *vs, void __user *uarg)
{
	struct enx_vdma_dev *dev = vs->dev;
	int ret;

	enterlog();

	ret = mutex_lock_killable(&dev->hw_lock);
	if (ret) {
		errorlog("Abort ioctl interrupted by fatal signal. ret=%d\n", ret);
		exitlog();
		return ret;
	}
	atomic64_inc(&dev->stats_aborts);
	enx_vdma_abort(dev);
	mutex_unlock(&dev->hw_lock);

	exitlog();
	return 0;
}
EXPORT_SYMBOL_GPL(enx_vdma_ioctl_abort);

long enx_vdma_ioctl_maxbufs(struct enx_vdma_sess *vs, void __user *uarg)
{
	struct enx_vdma_dev *dev = vs->dev;
	u32 max_bufs;
	int ret = 0;
	max_bufs = dev->max_bufs;
	enterlog();
	if (copy_to_user(uarg, &max_bufs, sizeof(u32))) {
		errorlog("Failed to copy max_bufs to user.\n");
		ret = -EFAULT;
	}
	exitlog();
	return ret;
}
EXPORT_SYMBOL_GPL(enx_vdma_ioctl_maxbufs);

/*******************************************************************************
 *
 * debugfs — Per-device monitoring
 *
 * Layout (created in core_register / removed in core_unregister):
 * - debugfs must be mounted beforehand.
 *	(e.g., mount -t debugfs none /sys/kernel/debug)
 *	/sys/kernel/debug/enx_vdma/<node_name>/
 *	- stats		— aggregated counters (in_flight, submits, completed, ...)
 *	- bufs		— current entries in dev->buf_xa
 *	- jobs		— current entries in dev->job_xa
 *	- sessions	— current open sessions with per-fd resource summary
 *				(pid, owned bufs, imports, in-flight jobs, bytes owned)
 *
*******************************************************************************/

static int enx_vdma_dbg_stats_show(struct seq_file *s, void *v)
{
	struct enx_vdma_dev *dev = s->private;

	seq_printf(s, "%-20s:        %d\n", "in_flight", atomic_read(&dev->in_flight));
	seq_printf(s, "%-20s:        %d\n", "bufs_active", atomic_read(&dev->bufs_active));
	seq_printf(s, "%-20s:        %lld\n", "bufs_bytes_inuse",
		   (long long)atomic64_read(&dev->bufs_bytes_inuse));
	seq_printf(s, "%-20s:        %d\n", "sessions_open", atomic_read(&dev->sessions_open));
	seq_printf(s, "%-20s:        %lld\n", "submits_total",
		   (long long)atomic64_read(&dev->stats_submits));
	seq_printf(s, "%-20s:        %lld\n", "submits_sync",
		   (long long)atomic64_read(&dev->stats_sync_submits));
	seq_printf(s, "%-20s:        %lld\n", "submits_async",
		   (long long)atomic64_read(&dev->stats_async_submits));
	seq_printf(s, "%-20s:        %lld\n", "submits_failed",
		   (long long)atomic64_read(&dev->stats_submits_failed));
	seq_printf(s, "%-20s:        %lld\n", "completed",
		   (long long)atomic64_read(&dev->stats_completed));
	seq_printf(s, "%-20s:        %lld\n", "bufs_total_alloc",
		   (long long)atomic64_read(&dev->stats_bufs_alloc));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(enx_vdma_dbg_stats);

static int enx_vdma_dbg_sessions_show(struct seq_file *s, void *v)
{
	struct enx_vdma_dev *dev = s->private;
	struct enx_vdma_sess *vs;

	seq_puts(s, "pid    alloc imports jobs  bytes_owned\n");

	/* Lock order: sessions_lock (outer) → vs->lock (inner). */
	mutex_lock(&dev->sessions_lock);
	list_for_each_entry(vs, &dev->sessions, dev_node) {
		struct enx_vdma_buf *buf;
		struct enx_vdma_job *job;
		size_t bytes_owned = 0;
		unsigned int n_alloc = 0, n_imports = 0, n_jobs = 0;

		mutex_lock(&vs->lock);
		list_for_each_entry(buf, &vs->bufs, owner_node) {
			if (buf->imported)
				n_imports++;
			else
				n_alloc++;
			bytes_owned += enx_vdma_buf_size(buf);
		}
		list_for_each_entry(job, &vs->jobs, node)
			n_jobs++;
		mutex_unlock(&vs->lock);

		seq_printf(s, "%-6d %-5u %-7u %-5u %zu\n",
			   vs->pid, n_alloc, n_imports, n_jobs, bytes_owned);
	}
	mutex_unlock(&dev->sessions_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(enx_vdma_dbg_sessions);

static const char *kind_str(u32 kind)
{
	switch (kind) {
	case ENX_BUF_SRC:	return "SRC";
	case ENX_BUF_DST:	return "DST";
	// bufs kind only src or dst, other kinds are error cases.
	case ENX_BUF_RAW:	return "RAW";
	case ENX_BUF_ID:	return "ID";
	case ENX_BUF_NONE:	return "NONE";
	default:			return "?";
	}
}

static int enx_vdma_dbg_bufs_show(struct seq_file *s, void *v)
{
	struct enx_vdma_dev *dev = s->private;
	struct enx_vdma_buf *buf;
	unsigned long id;

	seq_puts(s, "| id      | kind | size      | refs | imp | owner_pid\n");
	xa_lock(&dev->buf_xa);
	xa_for_each(&dev->buf_xa, id, buf) {
		seq_printf(s, "| %-7lu | %-4s | %-10zu| %-5d|  %c  | %d\n",
			   id, kind_str(buf->kind), enx_vdma_buf_size(buf),
			   kref_read(&buf->ref),
			   buf->imported ? 'I' : '-',
			   buf->owner ? buf->owner->pid : -1);
	}
	xa_unlock(&dev->buf_xa);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(enx_vdma_dbg_bufs);

static int enx_vdma_dbg_jobs_show(struct seq_file *s, void *v)
{
	struct enx_vdma_dev *dev = s->private;
	struct enx_vdma_job *job;
	unsigned long id;
	ktime_t now = ktime_get();

	seq_puts(s, "| id      | srcs | done | age(ms) | result   | pid\n");
	xa_lock(&dev->job_xa);
	xa_for_each(&dev->job_xa, id, job) {
		seq_printf(s, "| %-7lu | %-4u | %-4s | %-7lld | %-8s | %d\n",
			   id,
			   job->src_count,
			   smp_load_acquire(&job->done) ? "O" : "X",
			   ktime_to_ms(ktime_sub(now, job->submit_time)),
			   job->result ? errname(job->result) : "SUCCESS",
			   job->submitter ? job->submitter->pid : -1);
	}
	xa_unlock(&dev->job_xa);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(enx_vdma_dbg_jobs);

static int enx_vdma_dbg_version_show(struct seq_file *s, void *v)
{
	struct enx_vdma_dev *dev = s->private;
	seq_printf(s, "%s\n", dev->ops && dev->ops->version
				? dev->ops->version : "unknown");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(enx_vdma_dbg_version);

static void enx_vdma_dbg_dev_init(struct enx_vdma_dev *dev)
{
	if (!enx_vdma_dbg_root)
		return;
	dev->dbg_dir = debugfs_create_dir(dev->node_name, enx_vdma_dbg_root);
	if (IS_ERR(dev->dbg_dir)) {
		dev->dbg_dir = NULL;
		return;
	}
	debugfs_create_file("stats",	0444, dev->dbg_dir, dev,
				&enx_vdma_dbg_stats_fops);
	debugfs_create_file("bufs",		0444, dev->dbg_dir, dev,
				&enx_vdma_dbg_bufs_fops);
	debugfs_create_file("jobs",		0444, dev->dbg_dir, dev,
				&enx_vdma_dbg_jobs_fops);
	debugfs_create_file("sessions",	0444, dev->dbg_dir, dev,
				&enx_vdma_dbg_sessions_fops);
	debugfs_create_file("version",	0444, dev->dbg_dir, dev,
				&enx_vdma_dbg_version_fops);
}

static int enx_vdma_dbg_dev_version_iter(struct device *cdev, void *data)
{
	struct seq_file *s = data;
	struct enx_vdma_dev *dev = dev_get_drvdata(cdev);
	if (!dev || !dev->ops) return 0;
	seq_printf(s, "    %-18s: %s\n",
				dev->node_name,
				dev->ops->version ? dev->ops->version : "?");
	return 0;
}

static int enx_vdma_dbg_global_ver_show(struct seq_file *s, void *v)
{
	seq_puts  (s, "------- VDMA Version Info -------\n");
	seq_printf(s, "%-22s: %s\n", "core_version", PKG_BUILD_VER);
	seq_printf(s, "%-22s: %s (%s)\n", "core_built", PKG_BUILD_TS, PKG_BUILD_TZ);
	seq_puts(s, "registered devices:\n");
	class_for_each_device(enx_vdma_class, NULL, s, enx_vdma_dbg_dev_version_iter);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(enx_vdma_dbg_global_ver);

static int enx_vdma_dbg_backings_show(struct seq_file *s, void *v)
{
	struct enx_vdma_backing *bk;
	int total = 0;
	long long total_bytes = 0;

	seq_puts(s,   "-------------- Global VDMA buffer status --------------\n");
	seq_printf(s, " %-22s: %d\n",   "backings_active",
				atomic_read(&enx_vdma_backings_active));
	seq_printf(s, " %-22s: %d\n",   "alloc_active",
				atomic_read(&enx_vdma_alloc_active));
	seq_printf(s, " %-22s: %lld\n", "buf_bytes_inuse",
				(long long)atomic64_read(&enx_vdma_buf_bytes_inuse));
	seq_printf(s, " %-22s: %lld\n", "alloc_total",
				(long long)atomic64_read(&enx_vdma_alloc_total));
	seq_printf(s, " %-22s: %lld\n", "free_total",
				(long long)atomic64_read(&enx_vdma_free_total));
	seq_puts(s,   "---------------- VDMA CMA(backing) INFO ---------------\n");
	seq_printf(s, "| %-16s | %-4s | %-10s |  %-10s  |\n", "name", "refs", "size", "phys_addr");
	seq_puts(s,   "|------------------|------|------------|--------------|\n");

	spin_lock(&enx_vdma_backings_all_lock);
	list_for_each_entry(bk, &enx_vdma_backings_all, node) {
		size_t sz = bk->cmm ? bk->cmm->nbytes : 0;
		seq_printf(s, "| %-16s | %-4d | %-10zu |  0x%08lx  |\n",
				bk->cmm ? bk->cmm->name : "?",
				kref_read(&bk->ref),
				sz,
				bk->cmm ? bk->cmm->phys_start : 0UL);
		total++;
		total_bytes += sz;
	}
	spin_unlock(&enx_vdma_backings_all_lock);
	seq_puts(s,   "|-------------------------|---------------------------|\n");
	seq_printf(s, "| total backings: %6d  | total size: %6lld KBytes |\n", total, (total_bytes >> 10));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(enx_vdma_dbg_backings);

static void enx_vdma_dbg_dev_fini(struct enx_vdma_dev *dev)
{
	debugfs_remove_recursive(dev->dbg_dir);
	dev->dbg_dir = NULL;
}

/*******************************************************************************
 *
 * Device lifecycle helpers
 *
*******************************************************************************/

struct enx_vdma_dev *enx_vdma_core_alloc(struct device *parent)
{
	struct enx_vdma_dev *dev;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return NULL;
	dev->dev = parent;
	mutex_init(&dev->hw_lock);
	spin_lock_init(&dev->irq_lock);
	xa_init_flags(&dev->buf_xa, XA_FLAGS_ALLOC1);
	xa_init_flags(&dev->job_xa, XA_FLAGS_ALLOC1);
	init_waitqueue_head(&dev->hw_wq);
	INIT_LIST_HEAD(&dev->sessions);
	mutex_init(&dev->sessions_lock);
	return dev;
}
EXPORT_SYMBOL_GPL(enx_vdma_core_alloc);

int enx_vdma_core_register(struct enx_vdma_dev *dev,
			   const struct enx_vdma_core_ops *ops,
			   const struct file_operations *fops,
			   const char *node_name)
{
	struct device *cdev_dev;
	int minor;
	int ret;

	if (!dev || !ops || !ops->hw_run_once || !fops || !node_name)
		return -EINVAL;

	/* per-device ordered workqueue */
	dev->wq = alloc_ordered_workqueue("enx_%s",
					  WQ_MEM_RECLAIM, node_name);
	if (!dev->wq)
		return -ENOMEM;

	/* get minor number */
	minor = ida_alloc_max(&enx_vdma_minor_ida,
				ENX_VDMA_MAX_DEVS - 1, GFP_KERNEL);
	if (minor < 0) {
		ret = minor;
		goto err_wq;
	}
	dev->devid = MKDEV(MAJOR(enx_vdma_devid_base), minor);

	cdev_init(&dev->cdev, fops);
	dev->cdev.owner = fops->owner;
	ret = cdev_add(&dev->cdev, dev->devid, 1);
	if (ret)
		goto err_ida;

	/* Create class - /sys/class/enx_vdma/<name> */
	cdev_dev = device_create(enx_vdma_class, dev->dev,
				 dev->devid, dev, "%s", node_name);
	if (IS_ERR(cdev_dev)) {
		ret = PTR_ERR(cdev_dev);
		goto err_cdev;
	}

	dev->ops		= ops;
	dev->fops		= fops;
	dev->node_name	= node_name;

	enx_vdma_dbg_dev_init(dev);

	dev_info(dev->dev, "registered /dev/%s (major=%d minor=%d, ver=%s)\n",
		 node_name, MAJOR(dev->devid), minor, ops->version ? ops->version : "?");
	return 0;

err_cdev:
	cdev_del(&dev->cdev);
err_ida:
	ida_free(&enx_vdma_minor_ida, minor);
err_wq:
	destroy_workqueue(dev->wq);
	dev->wq = NULL;
	return ret;
}
EXPORT_SYMBOL_GPL(enx_vdma_core_register);

void enx_vdma_core_unregister(struct enx_vdma_dev *dev)
{
	struct enx_vdma_buf *buf;
	struct enx_vdma_job *job;
	unsigned long id;

	if (!dev) return;

	enx_vdma_dbg_dev_fini(dev);

	/* Destroy the class, ensuring no new open requests are accepted. */
	if (enx_vdma_class)
		device_destroy(enx_vdma_class, dev->devid);
	cdev_del(&dev->cdev);
	ida_free(&enx_vdma_minor_ida, MINOR(dev->devid));

	/* Drain queued work to ensure no further items run against this dev. */
	if (dev->wq) {
		destroy_workqueue(dev->wq);
		dev->wq = NULL;
	}

	/*
	 * Defensive scrub of the primary lookup tables.
	 *
	 * In a healthy lifecycle these are guaranteed empty here:
	 *	- All fds are closed (THIS_MODULE refcount reached 0).
	 *	- Each session_release iterated vs->jobs / vs->bufs
	 *		and called xa_erase on every entry.
	 *
	 * If something is still present it is the result of an alloc /
	 * submit error path that forgot to xa_erase on failure. Catch the
	 * bug loudly and force-free the leaked object so the device tear-
	 * down stays clean.
	 */
	xa_for_each(&dev->job_xa, id, job) {
		WARN(1, "enx_vdma: orphan job id=%lu at unregister\n", id);
		xa_erase(&dev->job_xa, id);
		if (job->flags == ENX_SUBMIT_ASYNC)
			kfree(job->blits);
		kfree(job);
	}

	xa_for_each(&dev->buf_xa, id, buf) {
		int refs = kref_read(&buf->ref);
		WARN(1, "enx_vdma: orphan buf id=%lu refs=%d size=%zu at unregister\n",
			id, refs, buf->backing->cmm->nbytes);
		xa_erase(&dev->buf_xa, id);
		atomic_dec(&dev->buf_count);
		/* Force leaked refcount to 0 to trigger the release of the buffer. */
		while (kref_read(&buf->ref) > 0)
			kref_put(&buf->ref, enx_vdma_buf_release);
	}

	dev->ops = NULL;
}
EXPORT_SYMBOL_GPL(enx_vdma_core_unregister);

void enx_vdma_core_free(struct enx_vdma_dev *dev)
{
	if (!dev) return;
	xa_destroy(&dev->buf_xa);
	xa_destroy(&dev->job_xa);
	mutex_destroy(&dev->hw_lock);
	mutex_destroy(&dev->sessions_lock);
	kfree(dev);
}
EXPORT_SYMBOL_GPL(enx_vdma_core_free);

/*******************************************************************************
 *
 * Module init / exit — shared class + chrdev region
 *
*******************************************************************************/

static int __init enx_vdma_core_init(void)
{
	int ret = 0;

	enterlog();

	ret = alloc_chrdev_region(&enx_vdma_devid_base, 0,
				  ENX_VDMA_MAX_DEVS, "enx_vdma");
	if (ret)
		goto out;

	enx_vdma_class = class_create("enx_vdma");
	if (IS_ERR(enx_vdma_class)) {
		ret = PTR_ERR(enx_vdma_class);
		enx_vdma_class = NULL;
		unregister_chrdev_region(enx_vdma_devid_base, ENX_VDMA_MAX_DEVS);
		goto out;
	}

	/* debugfs root — best-effort; failure is non-fatal */
	enx_vdma_dbg_root = debugfs_create_dir("enx_vdma", NULL);
	if (IS_ERR(enx_vdma_dbg_root))
		enx_vdma_dbg_root = NULL;
	if (enx_vdma_dbg_root) {
		debugfs_create_file("version_info",	0444, enx_vdma_dbg_root, NULL,
							&enx_vdma_dbg_global_ver_fops);
		debugfs_create_file("meminfo",		0444, enx_vdma_dbg_root, NULL,
							&enx_vdma_dbg_backings_fops);
	}

	pr_info("enx_vdma: core initialization complete. (major=%d, max_devs=%d)\n",
		MAJOR(enx_vdma_devid_base), ENX_VDMA_MAX_DEVS);
out:
	exitlog();
	return ret;
}
module_init(enx_vdma_core_init);

static void __exit enx_vdma_core_exit(void)
{
	enterlog();

	struct enx_vdma_backing *bk, *bt;
	list_for_each_entry_safe(bk, bt, &enx_vdma_backings_all, node) {
		WARN(1, "enx_vdma: orphan backing %s refs=%d size=%zu at module exit\n",
			bk->cmm ? bk->cmm->name : "?",
			kref_read(&bk->ref),
			bk->cmm ? bk->cmm->nbytes : 0);
		list_del(&bk->node);
		if (bk->cmm) ecmm_free(bk->cmm);
		kfree(bk);
	}

	debugfs_remove_recursive(enx_vdma_dbg_root);
	enx_vdma_dbg_root = NULL;
	if (enx_vdma_class) {
		class_destroy(enx_vdma_class);
		enx_vdma_class = NULL;
	}
	unregister_chrdev_region(enx_vdma_devid_base, ENX_VDMA_MAX_DEVS);
	ida_destroy(&enx_vdma_minor_ida);
	exitlog();
}
module_exit(enx_vdma_core_exit);

MODULE_DESCRIPTION("EYENIX VDMA core driver");
MODULE_AUTHOR("YongJae Lee <yjlee@eyenix.com>");
MODULE_LICENSE("GPL v2");