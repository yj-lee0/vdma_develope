// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Eyenix Corporation
 * Eyenix <support6@eyenix.com>
 *
 * Eyenix EN683 SoC Video-DMA JPEG Decoder controller.
 *  - JPEG to YUV decoding
 *
 */

#include <linux/bitfield.h>
#include <linux/dma-mapping.h>
#include <linux/errname.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "../enx-vdma.h"
#include "en683-jpegdec.h"
#include "en683-jpeg-uapi.h"

/*******************************************************************************
 *
 * eyenix define / config debuglog
 *
*******************************************************************************/

#include <soc/eyenix/cache.h>

#define JDEC_DEV_NAME	"enx_vdma_jdec"
#define DEVICE_NAME		"VJDEC"
#define DEBUGLOG_ERROR // Comment out if not in use.
#define DEBUGLOG_CHECK // Comment out if not in use.
// #define DEBUGLOG_PRINT // Comment out if not in use.
// #define DEBUGLOG_ENTER // Comment out if not in use.
// #define DEBUGLOG_EXIT  // Comment out if not in use.
#include <soc/eyenix/debuglog.h>

#include <dev_linksys_priv.h>	// for SetModuleVersion()
typedef void (*SetModuleVersion_t)(int nMode, char *ver);

/*******************************************************************************
 *
 * DMA-JDEC define & Module parameters
 *
*******************************************************************************/

static unsigned int max_bufs = VDMA_BUF_DEFAULT;
module_param(max_bufs, uint, 0444);
MODULE_PARM_DESC(max_bufs, "Max simultaneous buffers (default: 16)");

/*******************************************************************************
 *
 * internal helper functions
 *
*******************************************************************************/

static u32 get_flush_size(int format, u32 width, u32 height)
{
	switch (format) {
	// x3
	case eSRC_YUV444: return width * height * 3;
	// x2
	case eSRC_YUV422: return width * height * 2;
	// x1.5
	case eSRC_YUV420: return (width * height * 3) >> 1;
	// x1
	case eSRC_YUV400: return width * height;
	default:
		return 0;
	}
}

/*******************************************************************************
 *
 * enx_vdma_core_ops - DMA JPEG Decoder HW Trigger
 *
*******************************************************************************/

static void jdec_hw_abort(struct enx_vdma_dev *dev)
{
	u32 val;

	/* IRQ disable + clear */
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_IRQ_CTRL);
	FIELD_MODIFY(IRQ_CTRL_IRQ_EN_MASK, &val, 0);
	FIELD_MODIFY(IRQ_CTRL_IRQ_CLR_MASK, &val, 0x3F);
	enx_vdma_reg_wr(dev, EN683_JDEC_REG_IRQ_CTRL, val);

	/* Set Discard */
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_CTRL);
	FIELD_MODIFY(CTRL_DISCARD_BIT, &val, 1);
	enx_vdma_reg_wr(dev, EN683_JDEC_REG_CTRL, val);
}

static int jdec_hw_run_once(struct enx_vdma_sess *vs,
				struct enx_vdma_job_buf *dst, struct enx_vdma_job_buf *src,
				u32 src_count, void *blit_p, u32 job_flags)
{
	struct enx_vdma_dev *dev = vs->dev;
	struct jdec_dev_state *jds = NULL;
	struct enx_en683_jpegdec_submit_args *jsa = blit_p;
	phys_addr_t spa = 0, dpa = 0;
	u32 val, get_resp = 0, fmt = 0;
	int ret = 0;
	unsigned long flags;

	enterlog();

	if (!dev->dev_priv) {
		errorlog("device private data is NULL.\n");
		ret = -EINVAL;
		goto out;
	}
	jds = dev->dev_priv;

	/* hw busy check */
	if (!READ_ONCE(dev->hw_idle)) {
		errorlog("%s is busy.\n", DEVICE_NAME);
		ret = -EBUSY;
		goto out;
	}

	spa = src->data_offset + (jsa->cfg.src_addr_flag ? src->paddr : enx_vdma_buf_phys(src->vbuf));
	dpa = dst->data_offset + (jsa->cfg.dst_addr_flag ? dst->paddr : enx_vdma_buf_phys(dst->vbuf));

	printlog("JPEG DEC info\n");
	printlog(" - src base       : 0x%08llx\n", spa);
	printlog(" - dst base       : 0x%08llx\n", dpa);
	printlog(" - jpeg file size : %u\n", jsa->cfg.src_jpeg_size);
	printlog(" - dst buffer size: %u\n", jsa->cfg.dst_buf_size);

	/* src & dst cache flush */
	if (jsa->cfg.src_cache_flush) {
		printlog("cache flush->[SRC], addr %#llx, size %zu\n", spa, src->size);
		enx_en683_l2cache_flush64(spa, src->size);
	}
	if (jsa->cfg.dst_cache_flush) {
		printlog("cache flush->[DST], addr %#llx, size %zu\n", dpa, dst->size);
		enx_en683_l2cache_flush64(dpa, dst->size);
	}

	/* hw register setting */
	{
		/* src & dst address setting */
		enx_vdma_reg_wr(dev, EN683_JDEC_REG_SRC_ADDR, spa);
		enx_vdma_reg_wr(dev, EN683_JDEC_REG_SRC_SIZE, jsa->cfg.src_jpeg_size);
		enx_vdma_reg_wr(dev, EN683_JDEC_REG_DST_ADDR, dpa);

		/* Set hw busy */
		spin_lock_irqsave(&dev->irq_lock, flags);
		WRITE_ONCE(dev->hw_idle, false);
		spin_unlock_irqrestore(&dev->irq_lock, flags);

		/* Set IRQ enable */
		val = FIELD_PREP(IRQ_CTRL_IRQ_EN_MASK, 0x3F);		// Enable all IRQs (done + error)
		enx_vdma_reg_wr(dev, EN683_JDEC_REG_IRQ_CTRL, val);

		/* start rendering */
		val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_CTRL);
		FIELD_MODIFY(CTRL_GO_BIT, &val, 1);
		enx_vdma_reg_wr(dev, EN683_JDEC_REG_CTRL, val);

		printlog("Start jpeg-dec rendering.\n\n");
	}

	/* wait for IRQ */
	ret = wait_event_interruptible_timeout(dev->hw_wq, READ_ONCE(dev->hw_idle) == true, msecs_to_jiffies(1000));
	if (ret == 0) {
		errorlog("Rendering timed out.\n");
		spin_lock_irqsave(&dev->irq_lock, flags);
		jds->timeout_count++;
		spin_unlock_irqrestore(&dev->irq_lock, flags);
		ret = -ETIMEDOUT;
		enx_vdma_abort(dev);
		goto out;
	} else if (ret < 0) {
		errorlog("Rendering interrupted.\n");
		enx_vdma_abort(dev);
		goto out;
	} else {
		ret = 0; // success
	}

	/* Fetch the execution results to construct the final response. */
	{
		get_resp = 1;
		fmt = FIELD_GET(IMG_FORMAT_MASK, enx_vdma_reg_rd(dev, EN683_JDEC_REG_IMG_FORMAT));
		val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_IMG_RESOLUTION);
		jsa->resp.dst_size				 = get_flush_size(fmt, FIELD_GET(IMG_RESOLUTION_HORZ_MASK, val), FIELD_GET(IMG_RESOLUTION_VERT_MASK, val));
		jsa->resp.cycle					 = enx_vdma_reg_rd(dev, EN683_JDEC_REG_DEC_CYCLE);
		jsa->resp.hsize					 = FIELD_GET(IMG_RESOLUTION_HORZ_MASK, val);
		jsa->resp.vsize					 = FIELD_GET(IMG_RESOLUTION_VERT_MASK, val);
		val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_IMG_RESOLUTION_WR);
		jsa->resp.hsize_wr				 = FIELD_GET(IMG_RESOLUTION_WR_HORZ_MASK, val);
		jsa->resp.vsize_wr				 = FIELD_GET(IMG_RESOLUTION_WR_VERT_MASK, val);
		jsa->resp.fmt					 = fmt;
		jsa->resp.header_exist.value	 = enx_vdma_reg_rd(dev, EN683_JDEC_REG_HDR_ERROR);
		jsa->resp.unsupport_marker.value = enx_vdma_reg_rd(dev, EN683_JDEC_REG_UNSUP_INFO);
		jsa->resp.header_error.value	 = enx_vdma_reg_rd(dev, EN683_JDEC_REG_ERR_STATUS0);
		jsa->resp.process_error.value	 = enx_vdma_reg_rd(dev, EN683_JDEC_REG_ERR_STATUS1);
	}

out:
	/* Capture last-submit snapshot for debugfs (best-effort). */
	if (jds) {
		spin_lock_irqsave(&dev->irq_lock, flags);
		jds->last.valid						 = get_resp ? 2 : 1;
		jds->last.src_yaddr					 = spa;
		jds->last.dst_yaddr					 = dpa;
		jds->last.src_jpeg_size				 = jsa->cfg.src_jpeg_size;
		jds->last.submitter_pid				 = vs->pid;
		jds->last.result					 = ret;
		if (get_resp) {
			jds->last.dst_size				 = jsa->resp.dst_size;
			jds->last.cycle					 = jsa->resp.cycle;
			jds->last.hsize					 = jsa->resp.hsize;
			jds->last.vsize					 = jsa->resp.vsize;
			jds->last.fmt					 = jsa->resp.fmt;
			jds->last.header_exist.value	 = jsa->resp.header_exist.value;
			jds->last.unsupport_marker.value = jsa->resp.unsupport_marker.value;
			jds->last.header_error.value	 = jsa->resp.header_error.value;
			jds->last.process_error.value	 = jsa->resp.process_error.value;
		}
		spin_unlock_irqrestore(&dev->irq_lock, flags);
	}
	exitlog();
	return ret;
}

static const struct enx_vdma_core_ops jdec_ops = {
	.hw_run_once	= jdec_hw_run_once,
	.hw_abort		= jdec_hw_abort,
	.session_open	= NULL,
	.session_release= NULL,
	.blit_size		= sizeof(struct enx_en683_jpegdec_submit_args),
	.version		= PKG_BUILD_VER,
};


/*******************************************************************************
 *
 * ioctl handler (function-specific UAPI struct → core helper)
 *
*******************************************************************************/

/* Legacy mode (Sync only) */
static long jdec_ioctl_submit(struct enx_vdma_sess *sess, void __user *uarg)
{
	// struct enx_vdma_dev *dev = sess->dev;
	struct enx_en683_jpegdec_submit_args args;
	struct enx_vdma_submit_buf src_buf;
	struct enx_vdma_submit_params p = {0,};
	u32 ref_kind;
	int ret = 0;

	enterlog();

	if (copy_from_user(&args, uarg, sizeof(args))) {
		ret = -EFAULT;
		errorlog("failed(%d) to copy_from_user\n", ret);
		goto out;
	}

	if (args.cfg.src_jpeg_size == 0) {
		errorlog("src_jpeg_size is required\n");
		ret = -EINVAL;
		goto out;
	}

	if (args.cfg.dst_buf_size == 0) {
		errorlog("dst_buf_size is required (caller must pre-calculate decoded size)\n");
		ret = -EINVAL;
		goto out;
	}

	ref_kind = args.cfg.src_addr_flag ? ENX_BUF_RAW : ENX_BUF_ID;
	src_buf.kind		= ref_kind;
	src_buf.data_offset	= args.cfg.src_data_offset;
	src_buf.size		= args.cfg.src_jpeg_size;
	if (ref_kind == ENX_BUF_ID) {
		src_buf.id		= args.cfg.src_addr;
	} else {
		src_buf.paddr	= args.cfg.src_addr;
	}

	ref_kind = args.cfg.dst_addr_flag ? ENX_BUF_RAW : ENX_BUF_ID;
	{ // fill struct enx_vdma_submit_params
		p.dst.kind			= ref_kind;
		p.dst.data_offset	= args.cfg.dst_data_offset;
		p.dst.size			= args.cfg.dst_buf_size;
		if (ref_kind == ENX_BUF_ID) {
			p.dst.id		= args.cfg.dst_addr;
		} else {
			p.dst.paddr		= args.cfg.dst_addr;
		}
		p.src				= &src_buf;
		p.src_count			= 1; 				// must be 1
		p.blits				= (void *)&args;
		p.blit_size			= sizeof(struct enx_en683_jpegdec_submit_args);
		p.flags				= ENX_SUBMIT_SYNC;	// Sync mode only
		p.user_token		= 0; 				// Sync mode only
	}

	ret = enx_vdma_submit(sess, &p);
	if (ret) {
		errorlog("Failed enx_vdma_submit. ret=%d\n", ret);
	} else {
		/* Copy response data back to user space */
		if (copy_to_user(uarg, &args, sizeof(args))) {
			ret = -EFAULT;
			errorlog("failed(%d) to copy_to_user\n", ret);
		}
	}

out:
	exitlog();
	return ret;
}

static long jdec_ioctl_discard(struct enx_vdma_sess *sess, void __user *uarg)
{
	struct enx_vdma_dev *dev = sess->dev;

	enterlog();

	enx_vdma_abort(dev);

	exitlog();
	return 0;
}

/*******************************************************************************
 *
 * ioctl control
 *
*******************************************************************************/

static long enx_en683_jdec_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct enx_vdma_sess *sess = filp->private_data;
	void __user *uarg = (void __user *)arg;
	int ret = 0;

	enterlog();

	switch (cmd) {
	/* Common → core */
	case VDMAIOSET_ALLOC:		ret = enx_vdma_ioctl_alloc (sess, uarg); break;
	case VDMAIOSET_FREE:		ret = enx_vdma_ioctl_free  (sess, uarg); break;
	case VDMAIOSET_WAIT:		ret = enx_vdma_ioctl_wait  (sess, uarg); break;
	case VDMAIOSET_EXPORT:		ret = enx_vdma_ioctl_export(sess, uarg); break;
	case VDMAIOGET_IMPORT:		ret = enx_vdma_ioctl_import(sess, uarg); break;
	case VDMAIOGET_ABORT:		ret = enx_vdma_ioctl_abort (sess, uarg); break;
	case VDMAIOGET_MAX_BUFS:	ret = enx_vdma_ioctl_maxbufs(sess, uarg); break;
	/* Function-specific */
	case VDMAIOSET_JPEG_DEC_SUBMIT:	ret = jdec_ioctl_submit(sess, uarg); break;
	case VDMAIOSET_JPEG_DISCARD:	ret = jdec_ioctl_discard(sess, uarg); break;

	default:
		ret = -ENOTTY;
	}

	exitlog();
	return ret;
}

static const struct file_operations jdec_fops = {
	.owner			= THIS_MODULE,
	.open			= enx_vdma_open,
	.release		= enx_vdma_release,
	.unlocked_ioctl	= enx_en683_jdec_ioctl,
	.compat_ioctl	= enx_en683_jdec_ioctl,
	.mmap			= enx_vdma_mmap,
	.poll			= enx_vdma_poll,
	.llseek			= noop_llseek,
};

/*******************************************************************************
 *
 * interrupt
 *
*******************************************************************************/

static irqreturn_t enx_en683_jdec_irq(int irq, void *dev_id)
{
	struct enx_vdma_dev *dev = dev_id;
	struct jdec_dev_state *jds = dev->dev_priv;
	irqreturn_t ret = IRQ_NONE;
	u32 status, val;

	enterlog();

	spin_lock(&dev->irq_lock);
	status = enx_vdma_reg_rd(dev, EN683_JDEC_REG_IRQ_STATUS);
	if (FIELD_GET(IRQ_STATUS_DONE_BIT, status)) {
		jds->done_irq_count++;
	}
	if (FIELD_GET(IRQ_STATUS_ERR_OVERRUN_BIT, status)) {
		jds->err_overrun_irq_count++;
	}
	if (FIELD_GET(IRQ_STATUS_ERR_RD_CTRL_BIT, status)) {
		jds->err_rd_ctrl_irq_count++;
	}
	if (FIELD_GET(IRQ_STATUS_ERR_HEADER_BIT, status)) {
		jds->err_header_irq_count++;
	}
	if (FIELD_GET(IRQ_STATUS_ERR_ECS_BIT, status)) {
		jds->err_ecs_irq_count++;
	}
	if (FIELD_GET(IRQ_STATUS_ERR_VLD_BIT, status)) {
		jds->err_vld_irq_count++;
	}

	if (status) {
		val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_IRQ_CTRL);
		FIELD_MODIFY(IRQ_CTRL_IRQ_CLR_MASK, &val, status);
		enx_vdma_reg_wr(dev, EN683_JDEC_REG_IRQ_CTRL, val);

		WRITE_ONCE(dev->hw_idle, true);
		wake_up_interruptible(&dev->hw_wq);

		/* debug counters */
		jds->irq_count++;
		jds->last_irq_status = status;
		ret = IRQ_HANDLED;
	}
	spin_unlock(&dev->irq_lock);

	exitlog();
	return ret;
}

/*******************************************************************************
 *
 * debugfs — JPEG device monitoring
 *
 * Layout (subtree under core's dev->dbg_dir; lifecycle tied to core_register):
 * - debugfs must be mounted beforehand.
 *	(e.g., mount -t debugfs none /sys/kernel/debug)
 *	/sys/kernel/debug/enx_vdma/<node_name>/
 *	- regs			— HW register snapshot (raw + decoded fields)
 *	- irq_stats		— *irq_count, last_irq_status, timeout_count
 *	- last_submit	— snapshot of the most recent hw_run_once invocation
 *
 * IRQ statistics and last_submit are protected by dev->irq_lock; readers use
 * spin_lock_irqsave().
 *
*******************************************************************************/

static int enx_en683_jdec_dbg_irq_stats_show(struct seq_file *s, void *v)
{
	/* private is the parent enx_vdma_dev so we can use dev->irq_lock. */
	struct enx_vdma_dev *dev = s->private;
	struct jdec_dev_state *jds = dev->dev_priv;
	u64 irq_count, timeout_count, done_irq_count, err_overrun_irq_count, err_rd_ctrl_irq_count, \
		 		err_header_irq_count, err_ecs_irq_count, err_vld_irq_count;
	u32 last_status;
	unsigned long flags;

	spin_lock_irqsave(&dev->irq_lock, flags);
	irq_count				= jds->irq_count;
	done_irq_count			= jds->done_irq_count;
	err_overrun_irq_count	= jds->err_overrun_irq_count;
	err_rd_ctrl_irq_count	= jds->err_rd_ctrl_irq_count;
	err_header_irq_count	= jds->err_header_irq_count;
	err_ecs_irq_count		= jds->err_ecs_irq_count;
	err_vld_irq_count		= jds->err_vld_irq_count;
	last_status				= jds->last_irq_status;
	timeout_count			= jds->timeout_count;
	spin_unlock_irqrestore(&dev->irq_lock, flags);

	seq_printf(s, "total_irq_count:          %llu\n", irq_count);
	seq_printf(s, "done_irq_count:           %llu\n", done_irq_count);
	seq_printf(s, "err_overrun_irq_count:    %llu\n", err_overrun_irq_count);
	seq_printf(s, "err_rd_ctrl_irq_count:    %llu\n", err_rd_ctrl_irq_count);
	seq_printf(s, "err_header_irq_count:     %llu\n", err_header_irq_count);
	seq_printf(s, "err_ecs_irq_count:        %llu\n", err_ecs_irq_count);
	seq_printf(s, "err_vld_irq_count:        %llu\n", err_vld_irq_count);
	seq_printf(s, "last_status:              0x%08x\n", last_status);
	seq_printf(s, "timeout_count:            %llu\n", timeout_count);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(enx_en683_jdec_dbg_irq_stats);

static const char *jdec_format_str(u32 k)
{
	switch (k) {
	case eSRC_YUV444:	return "YUV444";
	case eSRC_YUV422:	return "YUV422";
	case eSRC_YUV420:	return "YUV420";
	case eSRC_YUV400:	return "YUV400";
	default:			return "?";
	}
}

static int enx_en683_jdec_dbg_last_submit_show(struct seq_file *s, void *v)
{
	struct enx_vdma_dev *dev = s->private;
	struct jdec_dev_state *jds = dev->dev_priv;
	struct jdec_last_submit snap;
	unsigned long flags;

	spin_lock_irqsave(&dev->irq_lock, flags);
	snap = jds->last;
	spin_unlock_irqrestore(&dev->irq_lock, flags);

	if (!snap.valid) {
		seq_puts(s, "(no submit yet)\n");
		return 0;
	}

	seq_printf(s, "src_yaddr:         %#08X\n", snap.src_yaddr);
	seq_printf(s, "dst_yaddr:         %#08X\n", snap.dst_yaddr);
	seq_printf(s, "src_jpeg_size:     %u\n", snap.src_jpeg_size);
	seq_printf(s, "submitter_pid:     %d\n", snap.submitter_pid);
	seq_printf(s, "result:            %s\n", snap.result  ? errname(snap.result) : "SUCCESS");
	if (snap.valid == 2) {
		seq_printf(s, "dst_size:         %u\n", snap.dst_size);
		seq_printf(s, "cycle:            %u\n", snap.cycle);
		seq_printf(s, "hsize:            %u\n", snap.hsize);
		seq_printf(s, "vsize:            %u\n", snap.vsize);
		seq_printf(s, "fmt:              %s\n", jdec_format_str(snap.fmt));
		seq_printf(s, "header_exist:     %#08X\n", snap.header_exist.value);
		seq_printf(s, "unsupport_marker: %#08X\n", snap.unsupport_marker.value);
		seq_printf(s, "header_error:     %#08X\n", snap.header_error.value);
		seq_printf(s, "process_error:    %#08X\n", snap.process_error.value);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(enx_en683_jdec_dbg_last_submit);

static int enx_en683_jdec_dbg_regs_show(struct seq_file *s, void *v)
{
	struct enx_vdma_dev *dev = s->private;
	u32 base_addr = dev->resource->start;
	u32 val;

	seq_printf(s, "Eyenix EN683 VDMA JPEG Decoder Register Map\n");
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_AXI_CTRL);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_AXI_CTRL, val);
	seq_printf(s, "%-25s: %lu\n", "JDEC_AWMOR", FIELD_GET(AXI_CTRL_AWMOR_BIT, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_AWLEN", FIELD_GET(AXI_CTRL_AWLEN_MASK, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_ARMOR", FIELD_GET(AXI_CTRL_ARMOR_MASK, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_ARLEN", FIELD_GET(AXI_CTRL_ARLEN_MASK, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_AXI_EDN", FIELD_GET(AXI_CTRL_AXI_EDN_BIT, val));
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_MAX_RES);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_MAX_RES, val);
	seq_printf(s, "%-25s: %lu\n", "JDEC_MAX_VERT", FIELD_GET(MAX_RES_VERT_MASK, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_MAX_VERT", FIELD_GET(MAX_RES_HORZ_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_DST_WR_SEL);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_DST_WR_SEL, val);
	seq_printf(s, "%-25s: %lu\n", "JDEC_DST_WR_SEL_YC420", FIELD_GET(DST_WR_SEL_YC420_BIT, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_DST_WR_SEL_YC422", FIELD_GET(DST_WR_SEL_YC422_MASK, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_DST_WR_SEL_YC444", FIELD_GET(DST_WR_SEL_YC444_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_CYCLE_THRESHOLD);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_CYCLE_THRESHOLD, val);
	seq_printf(s, "%-25s: %#08X\n", "JDEC_CYCLE_THRESHOLD", val);
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_SRC_ADDR);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_SRC_ADDR, val);
	seq_printf(s, "%-25s: %#08X\n", "JDEC_SRC_ADDR", val);
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_SRC_SIZE);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_SRC_SIZE, val);
	seq_printf(s, "%-25s: %u\n", "JDEC_SRC_SIZE", val);
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_DST_ADDR);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_DST_ADDR, val);
	seq_printf(s, "%-25s: %u\n", "JDEC_DST_ADDR", val);
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_CTRL);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_CTRL, val);
	seq_printf(s, "%-25s: %lu\n", "JDEC_DISCARD", FIELD_GET(CTRL_DISCARD_BIT, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_GO", FIELD_GET(CTRL_GO_BIT, val));
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_IRQ_CTRL);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_IRQ_CTRL, val);
	seq_printf(s, "%-25s: %#lX\n", "JDEC_IRQ_CLR", FIELD_GET(IRQ_CTRL_IRQ_CLR_MASK, val));
	seq_printf(s, "%-25s: %#lX\n", "JDEC_IRQ_EN", FIELD_GET(IRQ_CTRL_IRQ_EN_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_IRQ_STATUS);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_IRQ_STATUS, val);
	seq_printf(s, "%-25s: %c\n", "JDEC_IRQ_DONE", FIELD_GET(IRQ_STATUS_DONE_BIT, val) ? 'O':'X');
	seq_printf(s, "%-25s: %c\n", "JDEC_IRQ_ERR_OVERRUN", FIELD_GET(IRQ_STATUS_ERR_OVERRUN_BIT, val) ? 'O':'X');
	seq_printf(s, "%-25s: %c\n", "JDEC_IRQ_ERR_RD_CTRL", FIELD_GET(IRQ_STATUS_ERR_RD_CTRL_BIT, val) ? 'O':'X');
	seq_printf(s, "%-25s: %c\n", "JDEC_IRQ_ERR_HEADER", FIELD_GET(IRQ_STATUS_ERR_HEADER_BIT, val) ? 'O':'X');
	seq_printf(s, "%-25s: %c\n", "JDEC_IRQ_ERR_ECS", FIELD_GET(IRQ_STATUS_ERR_ECS_BIT, val) ? 'O':'X');
	seq_printf(s, "%-25s: %c\n", "JDEC_IRQ_ERR_VLD", FIELD_GET(IRQ_STATUS_ERR_VLD_BIT, val) ? 'O':'X');
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_IMG_RESOLUTION);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_IMG_RESOLUTION, val);
	seq_printf(s, "%-25s: %lu\n", "JDEC_IMG_RES_VERT", FIELD_GET(IMG_RESOLUTION_VERT_MASK, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_IMG_RES_HORZ", FIELD_GET(IMG_RESOLUTION_HORZ_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_IMG_RESOLUTION_WR);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_IMG_RESOLUTION_WR, val);
	seq_printf(s, "%-25s: %lu\n", "JDEC_IMG_RES_WR_VERT", FIELD_GET(IMG_RESOLUTION_WR_VERT_MASK, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_IMG_RES_WR_HORZ", FIELD_GET(IMG_RESOLUTION_WR_HORZ_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_IMG_FORMAT);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_IMG_FORMAT, val);
	seq_printf(s, "%-25s: %s\n", "JDEC_IMG_FORMAT", jdec_format_str(FIELD_GET(IMG_FORMAT_MASK, val)));
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_DST_CADDR);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_DST_CADDR, val);
	seq_printf(s, "%-25s: %#08X\n", "JDEC_DST_CADDR", val);
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_DEC_CYCLE);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_DEC_CYCLE, val);
	seq_printf(s, "%-25s: %u\n", "JDEC_DEC_CYCLE", val);
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_HDR_ERROR);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_HDR_ERROR, val);
	seq_printf(s, "%-25s: %lu\n", "JDEC_HDR_ERROR_UNSUP", FIELD_GET(HDR_ERROR_UNSUP_BIT, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_HDR_ERROR_COM", FIELD_GET(HDR_ERROR_COM_BIT, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_HDR_ERROR_DQT", FIELD_GET(HDR_ERROR_DQT_MASK, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_HDR_ERROR_DHT", FIELD_GET(HDR_ERROR_DHT_MASK, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_HDR_ERROR_APP_CNT", FIELD_GET(HDR_ERROR_APP_CNT_MASK, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_HDR_ERROR_DRI", FIELD_GET(HDR_ERROR_DRI_BIT, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_HDR_ERROR_SOS", FIELD_GET(HDR_ERROR_SOS_BIT, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_HDR_ERROR_SOF0", FIELD_GET(HDR_ERROR_SOF0_BIT, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_HDR_ERROR_SOI", FIELD_GET(HDR_ERROR_SOI_BIT, val));
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_APP_OFF0);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_APP_OFF0, val);
	seq_printf(s, "%-25s: %u\n", "JDEC_APP_OFF0", val);
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_APP_OFF1);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_APP_OFF1, val);
	seq_printf(s, "%-25s: %u\n", "JDEC_APP_OFF1", val);
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_APP_OFF2);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_APP_OFF2, val);
	seq_printf(s, "%-25s: %u\n", "JDEC_APP_OFF2", val);
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_APP_OFF3);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_APP_OFF3, val);
	seq_printf(s, "%-25s: %u\n", "JDEC_APP_OFF3", val);
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_APP_OFF4);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_APP_OFF4, val);
	seq_printf(s, "%-25s: %u\n", "JDEC_APP_OFF4", val);
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_APP_OFF5);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_APP_OFF5, val);
	seq_printf(s, "%-25s: %u\n", "JDEC_APP_OFF5", val);
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_APP_OFF6);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_APP_OFF6, val);
	seq_printf(s, "%-25s: %u\n", "JDEC_APP_OFF6", val);
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_APP_OFF7);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_APP_OFF7, val);
	seq_printf(s, "%-25s: %u\n", "JDEC_APP_OFF7", val);

	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_UNSUP_INFO);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_UNSUP_INFO, val);
	seq_printf(s, "%-25s: %lu\n", "JDEC_UNSUP_INFO_MRK", FIELD_GET(UNSUP_INFO_MRK_MASK, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_UNSUP_INFO_OFF", FIELD_GET(UNSUP_INFO_OFF_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_DRI_RST_IVL);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_DRI_RST_IVL, val);
	seq_printf(s, "%-25s: %u\n", "JDEC_DRI_RST_IVL", val);
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_ERR_STATUS0);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_ERR_STATUS0, val);
	seq_printf(s, "%-25s: %lu\n", "JDEC_ERR_SOS", FIELD_GET(ERR_STATUS0_SOS_MASK, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_ERR_SOF0", FIELD_GET(ERR_STATUS0_SOF0_MASK, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_ERR_DHT", FIELD_GET(ERR_STATUS0_DHT_MASK, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_ERR_DQT", FIELD_GET(ERR_STATUS0_DQT_MASK, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_ERR_DRI", FIELD_GET(ERR_STATUS0_DRI_BIT, val));
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_ERR_STATUS1);
	seq_printf(s, "---------------%#.08X: %#.08X\n", base_addr + EN683_JDEC_REG_ERR_STATUS1, val);
	seq_printf(s, "%-25s: %lu\n", "JDEC_ERR_VLD", FIELD_GET(ERR_STATUS1_VLD_MASK, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_ERR_ECS_PROC", FIELD_GET(ERR_STATUS1_ECS_PROC_MASK, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_ERR_HDR_PROC", FIELD_GET(ERR_STATUS1_HDR_PROC_MASK, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_ERR_RCTL", FIELD_GET(ERR_STATUS1_RCTL_BIT, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_ERR_CYCLE", FIELD_GET(ERR_STATUS1_CYCLE_BIT, val));
	seq_printf(s, "%-25s: %lu\n", "JDEC_ERR_OCCUR", FIELD_GET(ERR_STATUS1_OCCUR_BIT, val));
	seq_printf(s, "-------------------------------------\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(enx_en683_jdec_dbg_regs);

static void enx_en683_jdec_dbg_init(struct enx_vdma_dev *dev)
{
	if (!dev->dbg_dir)
		return;
	debugfs_create_file("regs",			0444, dev->dbg_dir, dev,
						&enx_en683_jdec_dbg_regs_fops);
	debugfs_create_file("irq_stats",	0444, dev->dbg_dir, dev,
						&enx_en683_jdec_dbg_irq_stats_fops);
	debugfs_create_file("last_submit",	0444, dev->dbg_dir, dev,
						&enx_en683_jdec_dbg_last_submit_fops);
}

/*******************************************************************************
 *
 * probe / remove
 *
*******************************************************************************/

static int enx_en683_jdec_probe(struct platform_device *pdev)
{
	struct enx_vdma_dev *dev;
	struct jdec_dev_state *jds;
	int ret = 0;

	enterlog();
	printlog("Start probe(%s)\n", dev_name(&pdev->dev));

	/* check max_bufs */
	if (!max_bufs) {
		errorlog("Invalid max_bufs (%u)\n", max_bufs);
		ret = -EINVAL;
		goto err_out;
	}

	dev = enx_vdma_core_alloc(&pdev->dev);
	if (!dev) {
		ret = -ENOMEM;
		errorlog("failed(%d) allocating kernel memory for EN683 %s device driver\n", ret, DEVICE_NAME);
		goto err_out;
	}

	/* create dev_state */
	jds = devm_kzalloc(&pdev->dev, sizeof(*jds), GFP_KERNEL);
	if (!jds) {
		ret = -ENOMEM;
		errorlog("failed(%d) create device state.\n", ret);
		goto err_free_core;
	}
	dev->dev_priv = jds;

	/* Get resource */
	dev->resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!dev->resource) {
		ret = PTR_ERR(dev->resource);
		errorlog("failed(%d) to platform_get_resource(base)\n", ret);
		goto err_free_core;
	}
	dev->regs = devm_ioremap_resource(&pdev->dev, dev->resource);
	if (IS_ERR(dev->regs)) {
		ret = PTR_ERR(dev->regs);
		errorlog("failed(%d) to ioremap_resource(base)\n", ret);
		goto err_free_core;
	}
	printlog("%s base[%px], real[0x%x]\n", JDEC_DEV_NAME, dev->regs, (unsigned int)dev->resource->start);

	dev->max_bufs = max_bufs;
	dev->max_src = 1; // only single source

	/* Initialize hardware idle state */
	WRITE_ONCE(dev->hw_idle, true);

	ret = platform_get_irq(pdev, 0);
	if (ret <= 0) {
		errorlog("failed(%d) to platform_get_irq\n", ret);
		goto err_free_core;
	}
	dev->irq = ret;

	ret = request_irq(dev->irq, enx_en683_jdec_irq, 0, JDEC_DEV_NAME, dev);
	if (ret) {
		errorlog("failed(%d) to request irq for %s\n", ret, JDEC_DEV_NAME);
		goto err_free_core;
	}

	ret = enx_vdma_core_register(dev, &jdec_ops, &jdec_fops, JDEC_DEV_NAME);
	if (ret) {
		errorlog("failed(%d) to register %s core\n", ret, JDEC_DEV_NAME);
		goto err_free_irq;
	}

	/* driver-side debugfs files under core's dbg_dir (lifetime tied to core) */
	enx_en683_jdec_dbg_init(dev);

	platform_set_drvdata(pdev, dev);

	/* Set version info for enxlog */
	SetModuleVersion_t func_ptr = (SetModuleVersion_t)symbol_get(SetModuleVersion);
	if (func_ptr) {
		func_ptr(VER_VJDEC, PKG_BUILD_VER);
		symbol_put(SetModuleVersion);
	}

	exitlog();
	return 0;

err_free_irq:
	free_irq(dev->irq, dev);
err_free_core:
	enx_vdma_core_free(dev);
err_out:
	exitlog();
	return ret;
}

static void enx_en683_jdec_remove(struct platform_device *pdev)
{
	struct enx_vdma_dev *dev = platform_get_drvdata(pdev);
	u32 val;

	/* Unregister the core */
	enx_vdma_core_unregister(dev);

	/* Disable IRQ */
	val = enx_vdma_reg_rd(dev, EN683_JDEC_REG_IRQ_CTRL);
	FIELD_MODIFY(IRQ_CTRL_IRQ_EN_MASK, &val, 0);
	enx_vdma_reg_wr(dev, EN683_JDEC_REG_IRQ_CTRL, val);
	free_irq(dev->irq, dev);

	/* Device free */
	enx_vdma_core_free(dev);
}

static const struct of_device_id enx_en683_jdec_of_match[] = {
	{ .compatible = "eyenix,en683-dma-jpegdec" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, enx_en683_jdec_of_match);

static struct platform_driver enx_en683_jdec_driver = {
	.probe  = enx_en683_jdec_probe,
	.remove = enx_en683_jdec_remove,
	.driver = {
		.name		= DEVICE_NAME,
		.of_match_table	= enx_en683_jdec_of_match,
	},
};
module_platform_driver(enx_en683_jdec_driver);

MODULE_DESCRIPTION("EYENIX EN683 VDMA JPEG Decoder driver");
MODULE_AUTHOR("YongJae Lee <yjlee@eyenix.com>");
MODULE_LICENSE("GPL v2");
