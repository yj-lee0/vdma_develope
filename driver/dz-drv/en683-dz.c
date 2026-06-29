// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Eyenix Corporation
 * Eyenix <support6@eyenix.com>
 *
 * Eyenix EN683 SoC Video-DMA DZ controller.
 *  - image format conversion
 *  - image up/down scaler
 *  - image crop
 *  - image flip
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
#include "en683-dz.h"
#include "en683-dz-uapi.h"

/*******************************************************************************
 *
 * eyenix define / config debuglog
 *
*******************************************************************************/

#include <soc/eyenix/cache.h>

#define DZ_DEV_NAME		"enx_vdma_dz"
#define DEVICE_NAME		"VDZ"
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
 * DMA-DZ define & Module parameters
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

static int enx_en683_dz_fmt_selecter(int format, u32 *mod, u32 *sel)
{
	switch (format) {
	case VDMA_FMT_RGB:				*mod = 0; *sel = 0; break;
	case VDMA_FMT_RBG:				*mod = 0; *sel = 1; break;
	case VDMA_FMT_GRB:				*mod = 0; *sel = 2; break;
	case VDMA_FMT_GBR:				*mod = 0; *sel = 3; break;
	case VDMA_FMT_BRG:				*mod = 0; *sel = 4; break;
	case VDMA_FMT_BGR:				*mod = 0; *sel = 5; break;
	case VDMA_FMT_RRR:				*mod = 0; *sel = 6; break;
	case VDMA_FMT_GGG:				*mod = 0; *sel = 7; break;
	case VDMA_FMT_BBB:				*mod = 0; *sel = 8; break;
	case VDMA_FMT_Rzz:				*mod = 0; *sel = 9; break;
	case VDMA_FMT_zGz:				*mod = 0; *sel = 10; break;
	case VDMA_FMT_zzB:				*mod = 0; *sel = 11; break;

	case VDMA_FMT_YUV444:			*mod = 1; *sel = 0; break;
	case VDMA_FMT_YUV444_Y:			*mod = 1; *sel = 12; break;

	case VDMA_FMT_YUV422_YUYV:		*mod = 2; *sel = 0; break;
	case VDMA_FMT_YUV422_YVYU:		*mod = 2; *sel = 1; break;
	case VDMA_FMT_YUV422_UYVY:		*mod = 2; *sel = 2; break;
	case VDMA_FMT_YUV422_VYUY:		*mod = 2; *sel = 3; break;
	case VDMA_FMT_YUV422_Y:			*mod = 2; *sel = 4; break;

	case VDMA_FMT_YUV420SP_NV12:	*mod = 3; *sel = 0; break;
	case VDMA_FMT_YVU420SP_NV21:	*mod = 3; *sel = 1; break;
	case VDMA_FMT_YUV420SP_NV_Y:	*mod = 3; *sel = 4; break;

	case VDMA_FMT_YUV400:			*mod = 3; *sel = 8; break;

	default:
		return -EFAULT;
	}
	return 0;
}

static u32 get_flush_size(int format, u32 width, u32 height)
{
	switch (format) {
	// x3
	case VDMA_FMT_RGB:
	case VDMA_FMT_RBG:
	case VDMA_FMT_GRB:
	case VDMA_FMT_GBR:
	case VDMA_FMT_BRG:
	case VDMA_FMT_BGR:
	case VDMA_FMT_RRR:
	case VDMA_FMT_GGG:
	case VDMA_FMT_BBB:
	case VDMA_FMT_Rzz:
	case VDMA_FMT_zGz:
	case VDMA_FMT_zzB:
	case VDMA_FMT_YUV444:
	case VDMA_FMT_YUV444_Y:
		return width * height * 3;

	// x2
	case VDMA_FMT_YUV422_YUYV:
	case VDMA_FMT_YUV422_YVYU:
	case VDMA_FMT_YUV422_UYVY:
	case VDMA_FMT_YUV422_VYUY:
	case VDMA_FMT_YUV422_Y:
		return width * height * 2;

	// x1.5
	case VDMA_FMT_YUV420SP_NV12:
	case VDMA_FMT_YVU420SP_NV21:
	case VDMA_FMT_YUV420SP_NV_Y:
		return (width * height * 3) >> 1;

	// x1
	case VDMA_FMT_YUV400:
		return width * height;
	default:
		return 0;
	}
}

/*******************************************************************************
 *
 * enx_vdma_core_ops - DMA DZ HW Trigger
 *
*******************************************************************************/

static void dz_hw_abort(struct enx_vdma_dev *dev)
{
	/* IRQ disable + clear */
	u32 val = 0;
	FIELD_MODIFY(IRQ_CTRL_IRQ_CLR_BIT, &val, 1);
	FIELD_MODIFY(IRQ_CTRL_IRQ_CLR_T_BIT, &val, 1);
	enx_vdma_reg_wr(dev, EN683_DZ_REG_IRQ_CTRL, val);
}

static int dz_hw_run_once(struct enx_vdma_sess *vs,
				struct enx_vdma_job_buf *dst, struct enx_vdma_job_buf *src,
				u32 src_count, void *blit_p, u32 job_flags)
{
	struct enx_vdma_dev *dev = vs->dev;
	struct dz_dev_state *dds = NULL;
	struct enx_en683_dz_submit_args *dsa = blit_p;
	phys_addr_t spa = 0, dpa = 0;
	u32 src_reg_mod, src_reg_sel;
	u32 dst_reg_mod, dst_reg_sel;
	u32 val;
	int ret = 0;
	unsigned long flags;

	enterlog();

	if (!dev->dev_priv) {
		errorlog("device private data is NULL.\n");
		ret = -EINVAL;
		goto out;
	}
	dds = dev->dev_priv;

	/* hw busy check */
	if (!READ_ONCE(dev->hw_idle)) {
		errorlog("%s is busy.\n", DEVICE_NAME);
		ret = -EBUSY;
		goto out;
	}

	/* source resolution check */
	if (!dsa->src_width || !dsa->src_height) {
		errorlog("Invalid size: Source width(%d) or height(%d) is null.\n", dsa->src_width, dsa->src_height);
		ret = -EINVAL;
		goto out;
	}

	/* destination resolution check */
	if (!dsa->dst_width || !dsa->dst_height) {
		errorlog("Invalid size: Destination width(%d) or height(%d) is null.\n", dsa->dst_width, dsa->dst_height);
		ret = -EINVAL;
		goto out;
	}

	printlog("Source info\n");
	spa = dsa->src_data_offset + (dsa->src_addr_flag ? src->paddr : enx_vdma_buf_phys(src->vbuf));
	printlog(" - base        : 0x%08llx\n", spa);
	printlog(" - format      : %d\n", dsa->src_format);
	printlog(" - w x h total : %d x %d\n", dsa->src_width_total, dsa->src_height_total);
	printlog(" - w x h       : %d x %d\n", dsa->src_width, dsa->src_height);
	printlog(" - offset      : %d x %d\n", dsa->src_width_pos, dsa->src_height_pos);

	printlog("Destination info\n");
	dpa = dsa->dst_data_offset + (dsa->dst_addr_flag ? dst->paddr : enx_vdma_buf_phys(dst->vbuf));
	printlog(" - base        : 0x%08llx\n", dpa);
	printlog(" - format      : %d\n", dsa->dst_format);
	printlog(" - w total     : %d\n", dsa->dst_width_total);
	printlog(" - w x h       : %d x %d\n", dsa->dst_width, dsa->dst_height);

	printlog("Flip/Rotator info\n");
	printlog(" - flip        : v:%d h:%d\n", dsa->vflip, dsa->hflip);

	/* src & dst cache flush */
	if (dsa->src_cache_flush) {
		printlog("cache flush->[SRC], addr %#llx, size %zu\n", spa, src->size);
		enx_en683_l2cache_flush64(spa, src->size);
	}
	if (dsa->dst_cache_flush) {
		printlog("cache flush->[DST], addr %#llx, size %zu\n", dpa, dst->size);
		enx_en683_l2cache_flush64(dpa, dst->size);
	}

	/* get source & destination format value */
	ret = enx_en683_dz_fmt_selecter(dsa->src_format, &src_reg_mod, &src_reg_sel);
	if (ret != 0) {
		errorlog("Invalid source format %d\n", dsa->src_format);
		ret = -EINVAL;
		goto out;
	}
	ret = enx_en683_dz_fmt_selecter(dsa->dst_format, &dst_reg_mod, &dst_reg_sel);
	if (ret != 0) {
		errorlog("Invalid destination format %d\n", dsa->dst_format);
		ret = -EINVAL;
		goto out;
	}

	/* Exception handling when source format is gray scale */
	if (dsa->src_format == VDMA_FMT_YUV444_Y || dsa->src_format == VDMA_FMT_YUV422_Y || dsa->src_format == VDMA_FMT_YUV400) {
		src_reg_sel = 0;
	}

	/* hw register setting */
	{
		/* src & dst address setting */
		enx_vdma_reg_wr(dev, EN683_DZ_REG_YRADR, spa);
		enx_vdma_reg_wr(dev, EN683_DZ_REG_CRADR, spa + (u32)(dsa->src_width_total * dsa->src_height_total));
		enx_vdma_reg_wr(dev, EN683_DZ_REG_YWADR, dpa);
		enx_vdma_reg_wr(dev, EN683_DZ_REG_CWADR, dpa + (u32)(dsa->dst_width_total * dsa->dst_height));

		/* scale adjust & offset setting */
		val = FIELD_PREP(OFFSET_CTRL_SVSP_MASK, dsa->src_height_pos) | FIELD_PREP(OFFSET_CTRL_SHSP_MASK, dsa->src_width_pos);
		enx_vdma_reg_wr(dev, EN683_DZ_REG_OFFSET, val);
		val = FIELD_PREP(RESOLUTION0_SHLEN_MASK, dsa->src_width) | FIELD_PREP(RESOLUTION0_SHLEN_TOT_MASK, dsa->src_width_total);
		enx_vdma_reg_wr(dev, EN683_DZ_REG_RESOLUTION0, val);
		val = FIELD_PREP(RESOLUTION1_DHLEN_MASK, dsa->dst_width) | FIELD_PREP(RESOLUTION1_DHLEN_TOT_MASK, dsa->dst_width_total);
		enx_vdma_reg_wr(dev, EN683_DZ_REG_RESOLUTION1, val);
		val = FIELD_PREP(RESOLUTION2_DVLEN_MASK, dsa->dst_height) | FIELD_PREP(RESOLUTION2_SVLEN_MASK, dsa->src_height);
		enx_vdma_reg_wr(dev, EN683_DZ_REG_RESOLUTION2, val);

		/* Set hw busy */
		spin_lock_irqsave(&dev->irq_lock, flags);
		WRITE_ONCE(dev->hw_idle, false);
		spin_unlock_irqrestore(&dev->irq_lock, flags);

		/* Set IRQ enable */
		val = FIELD_PREP(IRQ_CTRL_IRQ_EN_BIT, 1);
		enx_vdma_reg_wr(dev, EN683_DZ_REG_IRQ_CTRL, val);

		/* flip & color mode setting and start rendering */
		val = enx_vdma_reg_rd(dev, EN683_DZ_REG_CTRL);
		FIELD_MODIFY(CTRL_VFLIP_BIT, &val, dsa->vflip);
		FIELD_MODIFY(CTRL_HFLIP_BIT, &val, dsa->hflip);
		FIELD_MODIFY(CTRL_WSEL_MASK, &val, dst_reg_sel);
		FIELD_MODIFY(CTRL_RSEL_MASK, &val, src_reg_sel);
		FIELD_MODIFY(CTRL_WMOD_MASK, &val, dst_reg_mod);
		FIELD_MODIFY(CTRL_RMOD_MASK, &val, src_reg_mod);
		FIELD_MODIFY(CTRL_GO_BIT, &val, 1);
		enx_vdma_reg_wr(dev, EN683_DZ_REG_CTRL, val);

		printlog("Start dz rendering.\n\n");
	}

	/* wait for IRQ */
	ret = wait_event_interruptible_timeout(dev->hw_wq, READ_ONCE(dev->hw_idle) == true, msecs_to_jiffies(1000));
	if (ret == 0) {
		errorlog("Rendering timed out.\n");
		spin_lock_irqsave(&dev->irq_lock, flags);
		dds->timeout_count++;
		spin_unlock_irqrestore(&dev->irq_lock, flags);
		ret = -ETIMEDOUT;
		enx_vdma_abort(dev);
	} else if (ret < 0) {
		errorlog("Rendering interrupted.\n");
		enx_vdma_abort(dev);
	} else {
		ret = 0; // success
	}

out:
	/* Capture last-submit snapshot for debugfs (best-effort). */
	if (dds) {
		spin_lock_irqsave(&dev->irq_lock, flags);
		dds->last.valid				= 1;
		dds->last.src_yaddr			= spa;
		dds->last.src_caddr			= spa + (dsa->src_width_total * dsa->src_height_total);
		dds->last.dst_yaddr			= dpa;
		dds->last.dst_caddr			= dpa + (dsa->dst_width_total * dsa->dst_height);
		dds->last.src_width			= dsa->src_width;
		dds->last.src_width_total	= dsa->src_width_total;
		dds->last.src_height		= dsa->src_height;
		dds->last.src_height_total	= dsa->src_height_total;
		dds->last.dst_width			= dsa->dst_width;
		dds->last.dst_width_total	= dsa->dst_width_total;
		dds->last.dst_height		= dsa->dst_height;
		dds->last.flip				= (dsa->vflip << 1) | dsa->hflip;
		dds->last.src_fmt			= dsa->src_format;
		dds->last.dst_fmt			= dsa->dst_format;
		dds->last.submitter_pid		= vs->pid;
		dds->last.result			= ret;
		spin_unlock_irqrestore(&dev->irq_lock, flags);
	}
	exitlog();
	return ret;
}

static const struct enx_vdma_core_ops dz_ops = {
	.hw_run_once	= dz_hw_run_once,
	.hw_abort		= dz_hw_abort,
	.session_open	= NULL,
	.session_release= NULL,
	.blit_size		= sizeof(struct enx_en683_dz_submit_args),
	.version		= PKG_BUILD_VER,
};


/*******************************************************************************
 *
 * SUBMIT ioctl handler (function-specific UAPI struct → core helper)
 *
*******************************************************************************/

/* Legacy mode (Sync only) */
static long dz_ioctl_submit(struct enx_vdma_sess *sess, void __user *uarg)
{
	// struct enx_vdma_dev *dev = sess->dev;
	struct enx_en683_dz_submit_args args;
	struct enx_vdma_submit_buf src_buf;
	struct enx_vdma_submit_params p = {0,};
	u32 ref_kind, src_size, dst_size;
	int ret = 0;

	enterlog();

	if (copy_from_user(&args, uarg, sizeof(args))) {
		ret = -EFAULT;
		errorlog("failed(%d) to copy_from_user\n", ret);
		goto out;
	}

	/* setting source buffer info */
	ref_kind = args.src_addr_flag ? ENX_BUF_RAW : ENX_BUF_ID;
	src_size = get_flush_size(args.src_format, args.src_width_total, args.src_height_total);
	if (src_size == 0) {
		errorlog("Invalid source format %d\n", args.src_format);
		ret = -EINVAL;
		goto out;
	}

	src_buf.kind			= ref_kind;
	src_buf.data_offset     = args.src_data_offset;
	src_buf.size			= src_size;
	if (ref_kind == ENX_BUF_ID) {
		src_buf.id			= args.src_addr;
	} else {
		src_buf.paddr		= args.src_addr;
	}

	/* setting destination buffer info */
	ref_kind = args.dst_addr_flag ? ENX_BUF_RAW : ENX_BUF_ID;
	dst_size = get_flush_size(args.dst_format, args.dst_width_total, args.dst_height);
	if (dst_size == 0) {
		errorlog("Invalid destination format %d\n", args.dst_format);
		ret = -EINVAL;
		goto out;
	}

	{ // fill struct enx_vdma_submit_params
		p.dst.kind			= ref_kind;
		p.dst.data_offset	= args.dst_data_offset;
		p.dst.size			= dst_size;
		if (ref_kind == ENX_BUF_ID) {
			p.dst.id		= args.dst_addr;
		} else {
			p.dst.paddr		= args.dst_addr;
		}
		p.src				= &src_buf;
		p.src_count			= 1; 				// must be 1
		p.blits				= (void *)&args;
		p.blit_size			= sizeof(struct enx_en683_dz_submit_args);
		p.flags				= ENX_SUBMIT_SYNC;	// Sync mode only
		p.user_token		= 0; 				// Sync mode only
	}

	ret = enx_vdma_submit(sess, &p);
	if (ret) {
		errorlog("Failed enx_vdma_submit. ret=%d\n", ret);
	}

out:
	exitlog();
	return ret;
}

/*******************************************************************************
 *
 * ioctl control
 *
*******************************************************************************/

static long enx_en683_dz_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
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
	case VDMAIOSET_DZ_SUBMIT:	ret = dz_ioctl_submit(sess, uarg); break;

	default:
		ret = -ENOTTY;
	}

	exitlog();
	return ret;
}

static const struct file_operations dz_fops = {
	.owner			= THIS_MODULE,
	.open			= enx_vdma_open,
	.release		= enx_vdma_release,
	.unlocked_ioctl	= enx_en683_dz_ioctl,
	.compat_ioctl	= enx_en683_dz_ioctl,
	.mmap			= enx_vdma_mmap,
	.poll			= enx_vdma_poll,
	.llseek			= noop_llseek,
};

/*******************************************************************************
 *
 * interrupt
 *
*******************************************************************************/

static irqreturn_t enx_en683_dz_irq(int irq, void *dev_id)
{
	struct enx_vdma_dev *dev = dev_id;
	struct dz_dev_state *dds = dev->dev_priv;
	irqreturn_t ret = IRQ_NONE;
	u32 status, val;

	enterlog();

	spin_lock(&dev->irq_lock);
	status = enx_vdma_reg_rd(dev, EN683_DZ_REG_IRQ_STATUS);
	if (FIELD_GET(IRQ_STATUS_IRQ_BIT, status)) {
		val = enx_vdma_reg_rd(dev, EN683_DZ_REG_IRQ_CTRL);
		FIELD_MODIFY(IRQ_CTRL_IRQ_CLR_BIT, &val, 1);
		enx_vdma_reg_wr(dev, EN683_DZ_REG_IRQ_CTRL, val);
		WRITE_ONCE(dev->hw_idle, true);

		/* debug counters */
		dds->irq_count++;
		dds->last_irq_status = status;

		wake_up_interruptible(&dev->hw_wq);
		ret = IRQ_HANDLED;
	}
	spin_unlock(&dev->irq_lock);

	exitlog();
	return ret;
}

/*******************************************************************************
 *
 * debugfs — DZ device monitoring
 *
 * Layout (subtree under core's dev->dbg_dir; lifecycle tied to core_register):
 * - debugfs must be mounted beforehand.
 *	(e.g., mount -t debugfs none /sys/kernel/debug)
 *	/sys/kernel/debug/enx_vdma/<node_name>/
 *	- regs			— HW register snapshot (raw + decoded fields)
 *	- irq_stats		— irq_count, last_irq_status, timeout_count
 *	- last_submit	— snapshot of the most recent hw_run_once invocation
 *
 * IRQ statistics and last_submit are protected by dev->irq_lock; readers use
 * spin_lock_irqsave().
 *
*******************************************************************************/

static int enx_en683_dz_dbg_irq_stats_show(struct seq_file *s, void *v)
{
	/* private is the parent enx_vdma_dev so we can use dev->irq_lock. */
	struct enx_vdma_dev *dev = s->private;
	struct dz_dev_state *dds = dev->dev_priv;
	u64 irq_count, timeout_count;
	u32 last_status;
	unsigned long flags;

	spin_lock_irqsave(&dev->irq_lock, flags);
	irq_count		= dds->irq_count;
	last_status		= dds->last_irq_status;
	timeout_count	= dds->timeout_count;
	spin_unlock_irqrestore(&dev->irq_lock, flags);

	seq_printf(s, "irq_count:     %llu\n", irq_count);
	seq_printf(s, "last_status:   0x%08x\n", last_status);
	seq_printf(s, "timeout_count: %llu\n", timeout_count);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(enx_en683_dz_dbg_irq_stats);

static const char *dz_format_str(u32 k)
{
	switch (k) {
	case VDMA_FMT_RGB:				return "RGB";
	case VDMA_FMT_RBG:				return "RBG";
	case VDMA_FMT_GRB:				return "GRB";
	case VDMA_FMT_GBR:				return "GBR";
	case VDMA_FMT_BRG:				return "BRG";
	case VDMA_FMT_BGR:				return "BGR";
	case VDMA_FMT_RRR:				return "RRR";
	case VDMA_FMT_GGG:				return "GGG";
	case VDMA_FMT_BBB:				return "BBB";
	case VDMA_FMT_Rzz:				return "Rzz";
	case VDMA_FMT_zGz:				return "zGz";
	case VDMA_FMT_zzB:				return "zzB";
	case VDMA_FMT_YUV444:			return "YUV444";
	case VDMA_FMT_YUV444_Y:			return "YUV444_Y";
	case VDMA_FMT_YUV422_YUYV:		return "YUV422_YUYV";
	case VDMA_FMT_YUV422_YVYU:		return "YUV422_YVYU";
	case VDMA_FMT_YUV422_UYVY:		return "YUV422_UYVY";
	case VDMA_FMT_YUV422_VYUY:		return "YUV422_VYUY";
	case VDMA_FMT_YUV422_Y:			return "YUV422_Y";
	case VDMA_FMT_YUV420SP_NV12:	return "YUV420SP_NV12";
	case VDMA_FMT_YVU420SP_NV21:	return "YVU420SP_NV21";
	case VDMA_FMT_YUV420SP_NV_Y:	return "YUV420SP_NV_Y";
	case VDMA_FMT_YUV400:			return "YUV400";
	default:						return "?";
	}
}

static int enx_en683_dz_dbg_last_submit_show(struct seq_file *s, void *v)
{
	struct enx_vdma_dev *dev = s->private;
	struct dz_dev_state *dds = dev->dev_priv;
	struct dz_last_submit snap;
	unsigned long flags;

	spin_lock_irqsave(&dev->irq_lock, flags);
	snap = dds->last;
	spin_unlock_irqrestore(&dev->irq_lock, flags);

	if (!snap.valid) {
		seq_puts(s, "(no submit yet)\n");
		return 0;
	}

	seq_printf(s, "src_yaddr:        %#08X\n", snap.src_yaddr);
	seq_printf(s, "src_caddr:        %#08X\n", snap.src_caddr);
	seq_printf(s, "dst_yaddr:        %#08X\n", snap.dst_yaddr);
	seq_printf(s, "dst_caddr:        %#08X\n", snap.dst_caddr);
	seq_printf(s, "src_width:        %u\n", snap.src_width);
	seq_printf(s, "src_width_total:  %u\n", snap.src_width_total);
	seq_printf(s, "src_height:       %u\n", snap.src_height);
	seq_printf(s, "src_height_total: %u\n", snap.src_height_total);
	seq_printf(s, "dst_width:        %u\n", snap.dst_width);
	seq_printf(s, "dst_width_total:  %u\n", snap.dst_width_total);
	seq_printf(s, "dst_height:       %u\n", snap.dst_height);
	seq_printf(s, "flip:             h(%c) v(%c)\n", snap.flip & 0x1 ? 'O' : 'X', snap.flip & 0x2 ? 'O' : 'X');
	seq_printf(s, "src_fmt:          %s\n", dz_format_str(snap.src_fmt));
	seq_printf(s, "dst_fmt:          %s\n", dz_format_str(snap.dst_fmt));
	seq_printf(s, "submitter_pid:    %d\n",   snap.submitter_pid);
	seq_printf(s, "result:           %s\n",   snap.result  ? errname(snap.result) : "SUCCESS");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(enx_en683_dz_dbg_last_submit);

static int enx_en683_dz_dbg_regs_show(struct seq_file *s, void *v)
{
	struct enx_vdma_dev *dev = s->private;
	u32 base_addr = dev->resource->start;
	u32 val;

	seq_printf(s, "Eyenix EN683 VDMA DZ Register Map\n");
	val = enx_vdma_reg_rd(dev, EN683_DZ_REG_YRADR);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_DZ_REG_YRADR, val);
	seq_printf(s, "%-20s: %#08X\n", "DZ_YRADR", val);
	val = enx_vdma_reg_rd(dev, EN683_DZ_REG_CRADR);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_DZ_REG_CRADR, val);
	seq_printf(s, "%-20s: %#08X\n", "DZ_CRADR", val);
	val = enx_vdma_reg_rd(dev, EN683_DZ_REG_YWADR);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_DZ_REG_YWADR, val);
	seq_printf(s, "%-20s: %#08X\n", "DZ_YWADR", val);
	val = enx_vdma_reg_rd(dev, EN683_DZ_REG_CWADR);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_DZ_REG_CWADR, val);
	seq_printf(s, "%-20s: %#08X\n", "DZ_CWADR", val);
	val = enx_vdma_reg_rd(dev, EN683_DZ_REG_OFFSET);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_DZ_REG_OFFSET, val);
	seq_printf(s, "%-20s: %lu\n", "DZ_SVSP", FIELD_GET(OFFSET_CTRL_SVSP_MASK, val));
	seq_printf(s, "%-20s: %lu\n", "DZ_SHSP", FIELD_GET(OFFSET_CTRL_SHSP_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_DZ_REG_RESOLUTION0);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_DZ_REG_RESOLUTION0, val);
	seq_printf(s, "%-20s: %lu\n", "DZ_SHLEN", FIELD_GET(RESOLUTION0_SHLEN_MASK, val));
	seq_printf(s, "%-20s: %lu\n", "DZ_SHLEN_TOT", FIELD_GET(RESOLUTION0_SHLEN_TOT_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_DZ_REG_RESOLUTION1);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_DZ_REG_RESOLUTION1, val);
	seq_printf(s, "%-20s: %lu\n", "DZ_DHLEN", FIELD_GET(RESOLUTION1_DHLEN_MASK, val));
	seq_printf(s, "%-20s: %lu\n", "DZ_DHLEN_TOT", FIELD_GET(RESOLUTION1_DHLEN_TOT_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_DZ_REG_RESOLUTION2);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_DZ_REG_RESOLUTION2, val);
	seq_printf(s, "%-20s: %lu\n", "DZ_DVLEN", FIELD_GET(RESOLUTION2_DVLEN_MASK, val));
	seq_printf(s, "%-20s: %lu\n", "DZ_SVLEN", FIELD_GET(RESOLUTION2_SVLEN_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_DZ_REG_CTRL);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_DZ_REG_CTRL, val);
	seq_printf(s, "%-20s: %lu\n", "DZ_VFLIP", FIELD_GET(CTRL_VFLIP_BIT, val));
	seq_printf(s, "%-20s: %lu\n", "DZ_HFLIP", FIELD_GET(CTRL_HFLIP_BIT, val));
	seq_printf(s, "%-20s: %lu\n", "DZ_CSUB_SEL", FIELD_GET(CTRL_CSUB_SEL_BIT, val));
	seq_printf(s, "%-20s: %lu\n", "DZ_YC2_444_CINV", FIELD_GET(CTRL_YC2_444_CINV_BIT, val));
	seq_printf(s, "%-20s: %lu\n", "DZ_WSEL", FIELD_GET(CTRL_WSEL_MASK, val));
	seq_printf(s, "%-20s: %lu\n", "DZ_RSEL", FIELD_GET(CTRL_RSEL_MASK, val));
	seq_printf(s, "%-20s: %lu\n", "DZ_WMOD", FIELD_GET(CTRL_WMOD_MASK, val));
	seq_printf(s, "%-20s: %lu\n", "DZ_RMOD", FIELD_GET(CTRL_RMOD_MASK, val));
	seq_printf(s, "%-20s: %lu\n", "DZ_GO", FIELD_GET(CTRL_GO_BIT, val));
	val = enx_vdma_reg_rd(dev, EN683_DZ_REG_AXI_CTRL);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_DZ_REG_AXI_CTRL, val);
	seq_printf(s, "%-20s: %lu\n", "DZ_AWMOR", FIELD_GET(AXI_CTRL_AWMOR_BIT, val));
	seq_printf(s, "%-20s: %lu\n", "DZ_ARLEN", FIELD_GET(AXI_CTRL_ARLEN_MASK, val));
	seq_printf(s, "%-20s: %lu\n", "DZ_ARMOR", FIELD_GET(AXI_CTRL_ARMOR_MASK, val));
	seq_printf(s, "%-20s: %lu\n", "DZ_AWLEN", FIELD_GET(AXI_CTRL_AWLEN_MASK, val));
	seq_printf(s, "%-20s: %lu\n", "DZ_AXI_EDN", FIELD_GET(AXI_CTRL_AXI_EDN_BIT, val));
	seq_printf(s, "%-20s: %lu\n", "DZ_ARQOS", FIELD_GET(AXI_CTRL_ARQOS_MASK, val));
	seq_printf(s, "%-20s: %lu\n", "DZ_AWQOS", FIELD_GET(AXI_CTRL_AWQOS_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_DZ_REG_IRQ_CTRL);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_DZ_REG_IRQ_CTRL, val);
	seq_printf(s, "%-20s: %lu\n", "DZ_IRQ_CLR_T", FIELD_GET(IRQ_CTRL_IRQ_CLR_T_BIT, val));
	seq_printf(s, "%-20s: %lu\n", "DZ_IRQ_EN_T", FIELD_GET(IRQ_CTRL_IRQ_EN_T_BIT, val));
	seq_printf(s, "%-20s: %lu\n", "DZ_IRQ_CLR", FIELD_GET(IRQ_CTRL_IRQ_CLR_BIT, val));
	seq_printf(s, "%-20s: %lu\n", "DZ_IRQ_EN", FIELD_GET(IRQ_CTRL_IRQ_EN_BIT, val));
	val = enx_vdma_reg_rd(dev, EN683_DZ_REG_IRQ_STATUS);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_DZ_REG_IRQ_STATUS, val);
	seq_printf(s, "%-20s: %lu\n", "DZ_IRQ", FIELD_GET(IRQ_STATUS_IRQ_BIT, val));
	val = enx_vdma_reg_rd(dev, EN683_DZ_REG_IRQ_T_STATUS);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_DZ_REG_IRQ_T_STATUS, val);
	seq_printf(s, "%-20s: %lu\n", "DZ_IRQ_T", FIELD_GET(IRQ_STATUS_IRQ_T_BIT, val));
	val = enx_vdma_reg_rd(dev, EN683_DZ_REG_RGB2YUV_YR);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_DZ_REG_RGB2YUV_YR, val);
	seq_printf(s, "%-20s: %lu\n", "DZ_RGB2YUV_YR", FIELD_GET(RGB2YUV_YR_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_DZ_REG_RGB2YUV_YG);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_DZ_REG_RGB2YUV_YG, val);
	seq_printf(s, "%-20s: %lu\n", "DZ_RGB2YUV_YG", FIELD_GET(RGB2YUV_YG_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_DZ_REG_RGB2YUV_YB);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_DZ_REG_RGB2YUV_YB, val);
	seq_printf(s, "%-20s: %lu\n", "DZ_RGB2YUV_YB", FIELD_GET(RGB2YUV_YB_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_DZ_REG_RGB2YUV_UB);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_DZ_REG_RGB2YUV_UB, val);
	seq_printf(s, "%-20s: %lu\n", "DZ_RGB2YUV_UB", FIELD_GET(RGB2YUV_UB_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_DZ_REG_RGB2YUV_VR);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_DZ_REG_RGB2YUV_VR, val);
	seq_printf(s, "%-20s: %lu\n", "DZ_RGB2YUV_VR", FIELD_GET(RGB2YUV_VR_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_DZ_REG_YC2RGB_RR);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_DZ_REG_YC2RGB_RR, val);
	seq_printf(s, "%-20s: %lu\n", "DZ_YC2RGB_RR", FIELD_GET(YC2RGB_RR_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_DZ_REG_YC2RGB_GR);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_DZ_REG_YC2RGB_GR, val);
	seq_printf(s, "%-20s: %lu\n", "DZ_YC2RGB_GR", FIELD_GET(YC2RGB_GR_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_DZ_REG_YC2RGB_GB);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_DZ_REG_YC2RGB_GB, val);
	seq_printf(s, "%-20s: %lu\n", "DZ_YC2RGB_GB", FIELD_GET(YC2RGB_GB_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_DZ_REG_YC2RGB_BB);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_DZ_REG_YC2RGB_BB, val);
	seq_printf(s, "%-20s: %lu\n", "DZ_YC2RGB_BB", FIELD_GET(YC2RGB_BB_MASK, val));
	seq_printf(s, "--------------------------------\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(enx_en683_dz_dbg_regs);

static void enx_en683_dz_dbg_init(struct enx_vdma_dev *dev)
{
	if (!dev->dbg_dir)
		return;
	debugfs_create_file("regs",			0444, dev->dbg_dir, dev,
						&enx_en683_dz_dbg_regs_fops);
	debugfs_create_file("irq_stats",	0444, dev->dbg_dir, dev,
						&enx_en683_dz_dbg_irq_stats_fops);
	debugfs_create_file("last_submit",	0444, dev->dbg_dir, dev,
						&enx_en683_dz_dbg_last_submit_fops);
}

/*******************************************************************************
 *
 * probe / remove
 *
*******************************************************************************/

static int enx_en683_dz_probe(struct platform_device *pdev)
{
	struct enx_vdma_dev *dev;
	struct dz_dev_state *dds;
	const char *name;
	int id;
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
	dds = devm_kzalloc(&pdev->dev, sizeof(*dds), GFP_KERNEL);
	if (!dds) {
		ret = -ENOMEM;
		errorlog("failed(%d) create device state.\n", ret);
		goto err_free_core;
	}
	dev->dev_priv = dds;

	/* Get instance ID */
	ret = of_property_read_s32(pdev->dev.of_node, "enx,ch", &id);
	if (ret) {
		errorlog("failed(%d) to get property 'enx,ch'\n", ret);
		goto err_free_core;
	}
	dds->instance_id = id;

	name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "%s%d", DZ_DEV_NAME, id);
	if (!name) {
		ret = -ENOMEM;
		goto err_free_core;
	}

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
	printlog("%s base[%px], real[0x%x]\n", name, dev->regs, (unsigned int)dev->resource->start);

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

	ret = request_irq(dev->irq, enx_en683_dz_irq, 0, name, dev);
	if (ret) {
		errorlog("failed(%d) to request irq for %s\n", ret, name);
		goto err_free_core;
	}

	ret = enx_vdma_core_register(dev, &dz_ops, &dz_fops, name);
	if (ret) {
		errorlog("failed(%d) to register %s core\n", ret, name);
		goto err_free_irq;
	}

	/* driver-side debugfs files under core's dbg_dir (lifetime tied to core) */
	enx_en683_dz_dbg_init(dev);

	platform_set_drvdata(pdev, dev);

	/* Set version info for enxlog */
	SetModuleVersion_t func_ptr = (SetModuleVersion_t)symbol_get(SetModuleVersion);
	if (func_ptr) {
		func_ptr(VER_VDZ, PKG_BUILD_VER);
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

static void enx_en683_dz_remove(struct platform_device *pdev)
{
	struct enx_vdma_dev *dev = platform_get_drvdata(pdev);
	u32 val;

	/* Unregister the core */
	enx_vdma_core_unregister(dev);

	/* Disable IRQ */
	val = enx_vdma_reg_rd(dev, EN683_DZ_REG_IRQ_CTRL);
	FIELD_MODIFY(IRQ_CTRL_IRQ_EN_BIT, &val, 0);
	enx_vdma_reg_wr(dev, EN683_DZ_REG_IRQ_CTRL, val);
	free_irq(dev->irq, dev);

	/* Device free */
	enx_vdma_core_free(dev);
}

static const struct of_device_id enx_en683_dz_of_match[] = {
	{ .compatible = "eyenix,en683-dma-dz" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, enx_en683_dz_of_match);

static struct platform_driver enx_en683_dz_driver = {
	.probe  = enx_en683_dz_probe,
	.remove = enx_en683_dz_remove,
	.driver = {
		.name		= DEVICE_NAME,
		.of_match_table	= enx_en683_dz_of_match,
	},
};
module_platform_driver(enx_en683_dz_driver);

MODULE_DESCRIPTION("EYENIX EN683 VDMA DZ driver");
MODULE_AUTHOR("YongJae Lee <yjlee@eyenix.com>");
MODULE_LICENSE("GPL v2");
