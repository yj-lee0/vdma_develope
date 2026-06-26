// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Eyenix Corporation
 * Eyenix <support6@eyenix.com>
 *
 * Eyenix EN683 SoC Video-DMA NPU controller.
 *  - Copy of npu inference source data
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
#include "en683-npu.h"
#include "en683-npu-uapi.h"

/*******************************************************************************
 *
 * eyenix define / config debuglog
 *
*******************************************************************************/

#include <soc/eyenix/cache.h>

#define NPU_DEV_NAME	"enx_vdma_npu"
#define DEVICE_NAME		"VNPU"
#define DEBUGLOG_ERROR // Comment out if not in use.
#define DEBUGLOG_CHECK // Comment out if not in use.
// #define DEBUGLOG_PRINT // Comment out if not in use.
// #define DEBUGLOG_ENTER // Comment out if not in use.
// #define DEBUGLOG_EXIT  // Comment out if not in use.
#include <soc/eyenix/debuglog.h>

/*******************************************************************************
 *
 * DMA-NPU define & Module parameters
 *
*******************************************************************************/

static unsigned int max_bufs = VDMA_BUF_DEFAULT;
module_param(max_bufs, uint, 0444);
MODULE_PARM_DESC(max_bufs, "Max simultaneous buffers (default: 16)");

/*******************************************************************************
 *
 * enx_vdma_core_ops - DMA NPU HW Trigger
 *
*******************************************************************************/

static void npu_hw_abort(struct enx_vdma_dev *dev)
{
	u32 val;

	/* IRQ disable + clear */
	val = enx_vdma_reg_rd(dev, EN683_NPU_REG_IRQ_CTRL);
	FIELD_MODIFY(IRQ_CTRL_IRQ_EN_BIT, &val, 0);
	FIELD_MODIFY(IRQ_CTRL_IRQ_CLR_BIT, &val, 0x1);
	enx_vdma_reg_wr(dev, EN683_NPU_REG_IRQ_CTRL, val);
}

static int npu_hw_run_once(struct enx_vdma_sess *vs,
				struct enx_vdma_job_buf *dst, struct enx_vdma_job_buf *src,
				u32 src_count, void *blit_p, u32 job_flags)
{
	struct enx_vdma_dev *dev = vs->dev;
	struct npu_dev_state *nds = NULL;
	struct enx_en683_npu_submit_args *nsa = blit_p;
	phys_addr_t spa = 0, dpa = 0;
	u32 val;
	int ret = 0;
	unsigned long flags;

	enterlog();

	if (!dev->dev_priv) {
		errorlog("device private data is NULL.\n");
		ret = -EINVAL;
		goto out;
	}
	nds = dev->dev_priv;

	/* hw busy check */
	if (!READ_ONCE(dev->hw_idle)) {
		errorlog("%s is busy.\n", DEVICE_NAME);
		ret = -EBUSY;
		goto out;
	}

	/* source resolution check */
	if (!nsa->src_width || !nsa->src_height) {
		errorlog("Invalid size: Source width(%hu) or height(%hu) is null.\n", nsa->src_width, nsa->src_height);
		ret = -EINVAL;
		goto out;
	}

	/* check rgb_scale */
	if (nsa->rgb_scale > 2) {
		errorlog("Invalid rgb_scale: %hhu\n", nsa->rgb_scale);
		ret = -EINVAL;
		goto out;
	}

	/* check read_mode */
	if (nsa->read_mode > 3) {
		errorlog("Invalid read_mode: %hhu\n", nsa->read_mode);
		ret = -EINVAL;
		goto out;
	}

	spa = src->data_offset + (nsa->src_addr_flag ? src->paddr : enx_vdma_buf_phys(src->vbuf));
	dpa = dst->data_offset + (nsa->dst_addr_flag ? dst->paddr : enx_vdma_buf_phys(dst->vbuf));

	printlog("VNPU info\n");
	printlog(" - base          : 0x%08llx\n", spa);
	printlog(" - w x h         : %hu x %hu\n", nsa->src_width, nsa->src_height);
	printlog(" - read_mode     : %hhu\n", nsa->read_mode);
	printlog(" - rgb_scale     : %hhu\n", nsa->rgb_scale);
	printlog(" - color_invert  : %hhu\n", nsa->color_invert);

	/* src & dst cache flush */
	if (nsa->src_cache_flush) {
		printlog("cache flush->[SRC], addr %#llx, size %zu\n", spa, src->size);
		enx_en683_l2cache_flush64(spa, src->size);
	}
	if (nsa->dst_cache_flush) {
		printlog("cache flush->[DST], addr %#llx, size %zu\n", dpa, dst->size);
		enx_en683_l2cache_flush64(dpa, dst->size);
	}

	/* hw register setting */
	{
		/* src & dst address setting */
		enx_vdma_reg_wr(dev, EN683_NPU_REG_SRC_RADDR, spa);
		enx_vdma_reg_wr(dev, EN683_NPU_REG_DST_YADDR, dpa);
		enx_vdma_reg_wr(dev, EN683_NPU_REG_DST_CADDR, dpa + (u32)(nsa->src_width * nsa->src_height));

		/* source resolution setting */
		val = FIELD_PREP(RESOLUTION_VERT_MASK, nsa->src_height) | FIELD_PREP(RESOLUTION_HORZ_MASK, nsa->src_width);
		enx_vdma_reg_wr(dev, EN683_NPU_REG_RESOLUTION, val);

		/* Set hw busy */
		spin_lock_irqsave(&dev->irq_lock, flags);
		WRITE_ONCE(dev->hw_idle, false);
		spin_unlock_irqrestore(&dev->irq_lock, flags);

		/* Set IRQ enable */
		val = FIELD_PREP(IRQ_CTRL_IRQ_EN_BIT, 1); // Enable both DONE and ERR IRQ
		enx_vdma_reg_wr(dev, EN683_NPU_REG_IRQ_CTRL, val);

		/* start rendering */
		val = enx_vdma_reg_rd(dev, EN683_NPU_REG_CTRL);
		if (nsa->color_invert) {
			FIELD_MODIFY(CTRL_CINV_BIT, &val, 1);
		} else {
			FIELD_MODIFY(CTRL_CINV_BIT, &val, 0);
		}
		FIELD_MODIFY(CTRL_RMOD_MASK, &val, nsa->read_mode);
		FIELD_MODIFY(CTRL_RGB_SCALE_MASK, &val, nsa->rgb_scale);
		FIELD_MODIFY(CTRL_GO_BIT, &val, 1);
		enx_vdma_reg_wr(dev, EN683_NPU_REG_CTRL, val);

		printlog("Start NPU rendering.\n\n");
	}

	/* wait for IRQ */
	ret = wait_event_interruptible_timeout(dev->hw_wq, READ_ONCE(dev->hw_idle) == true, msecs_to_jiffies(1000));
	if (ret == 0) {
		errorlog("Rendering timed out.\n");
		spin_lock_irqsave(&dev->irq_lock, flags);
		nds->timeout_count++;
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

out:
	/* Capture last-submit snapshot for debugfs (best-effort). */
	if (nds) {
		spin_lock_irqsave(&dev->irq_lock, flags);
		nds->last.valid					  = 1;
		nds->last.src_addr				  = spa;
		nds->last.dst_yaddr				  = dpa;
		nds->last.dst_caddr				  = dpa + (nsa->src_width * nsa->src_height);
		nds->last.src_width				  = nsa->src_width;
		nds->last.src_height			  = nsa->src_height;
		nds->last.read_mode				  = nsa->read_mode;
		nds->last.rgb_scale				  = nsa->rgb_scale;
		nds->last.color_invert			  = nsa->color_invert;
		nds->last.submitter_pid			  = vs->pid;
		nds->last.result				  = ret;
		spin_unlock_irqrestore(&dev->irq_lock, flags);
	}
	exitlog();
	return ret;
}

static const struct enx_vdma_core_ops npu_ops = {
	.hw_run_once	= npu_hw_run_once,
	.hw_abort		= npu_hw_abort,
	.session_open	= NULL,
	.session_release= NULL,
	.blit_size		= sizeof(struct enx_en683_npu_submit_args),
	.version		= PKG_BUILD_VER,
};


/*******************************************************************************
 *
 * SUBMIT ioctl handler (function-specific UAPI struct → core helper)
 *
*******************************************************************************/

/* Legacy mode (Sync only) */
static long npu_ioctl_submit(struct enx_vdma_sess *sess, void __user *uarg)
{
	// struct enx_vdma_dev *dev = sess->dev;
	struct enx_en683_npu_submit_args args;
	struct enx_vdma_submit_buf src_buf;
	struct enx_vdma_submit_params p = {0,};
	u32 ref_kind, src_size, dst_size, scale;
	int ret = 0;

	enterlog();

	if (copy_from_user(&args, uarg, sizeof(args))) {
		ret = -EFAULT;
		errorlog("failed(%d) to copy_from_user\n", ret);
		goto out;
	}

	/* check resolution */
	if (args.src_width == 0 || args.src_height == 0) {
		errorlog("Invalid source resolution %dx%d\n", args.src_width, args.src_height);
		ret = -EINVAL;
		goto out;
	}

	/* check read_mode */
	if (args.read_mode > 3) {
		errorlog("Invalid read mode %d\n", args.read_mode);
		ret = -EINVAL;
		goto out;
	}

	switch (args.read_mode) {
	case 0: scale = 4; break;
	case 1: scale = 8; break;
	case 2: scale = 16; break;
	case 3: scale = 32; break;
	default:
		errorlog("Invalid read mode %d\n", args.read_mode);
		ret = -EINVAL;
		goto out;
	}

	/* setting source buffer info */
	ref_kind 			= args.src_addr_flag ? ENX_BUF_RAW : ENX_BUF_ID;
	src_size 			= args.src_width * args.src_height * scale;
	src_buf.kind		= ref_kind;
	src_buf.data_offset = args.src_data_offset;
	src_buf.size		= src_size;
	if (ref_kind == ENX_BUF_ID) {
		src_buf.id		= args.src_addr;
	} else {
		src_buf.paddr	= args.src_addr;
	}

	/* setting destination buffer info */
	ref_kind = args.dst_addr_flag ? ENX_BUF_RAW : ENX_BUF_ID;
	dst_size = args.src_width * args.src_height * 3 >> 1; // YUV420 size

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
		p.blit_size			= sizeof(struct enx_en683_npu_submit_args);
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

/*******************************************************************************
 *
 * ioctl control
 *
*******************************************************************************/

static long enx_en683_npu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
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
	case VDMAIOSET_NPU_SUBMIT:	ret = npu_ioctl_submit(sess, uarg); break;

	default:
		ret = -ENOTTY;
	}

	exitlog();
	return ret;
}

static const struct file_operations npu_fops = {
	.owner			= THIS_MODULE,
	.open			= enx_vdma_open,
	.release		= enx_vdma_release,
	.unlocked_ioctl	= enx_en683_npu_ioctl,
	.compat_ioctl	= enx_en683_npu_ioctl,
	.mmap			= enx_vdma_mmap,
	.poll			= enx_vdma_poll,
	.llseek			= noop_llseek,
};

/*******************************************************************************
 *
 * interrupt
 *
*******************************************************************************/

static irqreturn_t enx_en683_npu_irq(int irq, void *dev_id)
{
	struct enx_vdma_dev *dev = dev_id;
	struct npu_dev_state *nds = dev->dev_priv;
	irqreturn_t ret = IRQ_NONE;
	u32 status, val;

	enterlog();

	spin_lock(&dev->irq_lock);
	status = enx_vdma_reg_rd(dev, EN683_NPU_REG_IRQ_STATUS);
	if (FIELD_GET(IRQ_STATUS_IRQ_BIT, status)) {
		val = enx_vdma_reg_rd(dev, EN683_NPU_REG_IRQ_CTRL);
		FIELD_MODIFY(IRQ_CTRL_IRQ_CLR_BIT, &val, 1);
		enx_vdma_reg_wr(dev, EN683_NPU_REG_IRQ_CTRL, val);
		WRITE_ONCE(dev->hw_idle, true);

		/* debug counters */
		nds->irq_count++;
		nds->last_irq_status = status;

		wake_up_interruptible(&dev->hw_wq);
		ret = IRQ_HANDLED;
	}
	spin_unlock(&dev->irq_lock);

	exitlog();
	return ret;
}

/*******************************************************************************
 *
 * debugfs — NPU device monitoring
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

static int enx_en683_npu_dbg_irq_stats_show(struct seq_file *s, void *v)
{
	/* private is the parent enx_vdma_dev so we can use dev->irq_lock. */
	struct enx_vdma_dev *dev = s->private;
	struct npu_dev_state *nds = dev->dev_priv;
	u64 irq_count, timeout_count;
	u32 last_status;
	unsigned long flags;

	spin_lock_irqsave(&dev->irq_lock, flags);
	irq_count		= nds->irq_count;
	last_status		= nds->last_irq_status;
	timeout_count	= nds->timeout_count;
	spin_unlock_irqrestore(&dev->irq_lock, flags);

	seq_printf(s, "total_irq_count:     %llu\n", irq_count);
	seq_printf(s, "last_status:         0x%08x\n", last_status);
	seq_printf(s, "timeout_count:       %llu\n", timeout_count);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(enx_en683_npu_dbg_irq_stats);

static const char *npu_rmod_str(u32 k)
{
	switch (k) {
	case 0:		return "4Byte aligned";
	case 1:		return "8Byte aligned";
	case 2:		return "16Byte aligned";
	case 3:		return "32Byte aligned";
	default:	return "?";
	}
}

static const char *npu_rgb_scale_str(u32 k)
{
	switch (k) {
	case 0:		return "Bypass mode";
	case 1:		return "2X scale mode";
	case 2:		return "unsigned mode";
	default:	return "?";
	}
}

static int enx_en683_npu_dbg_last_submit_show(struct seq_file *s, void *v)
{
	struct enx_vdma_dev *dev = s->private;
	struct npu_dev_state *nds = dev->dev_priv;
	struct npu_last_submit snap;
	unsigned long flags;

	spin_lock_irqsave(&dev->irq_lock, flags);
	snap = nds->last;
	spin_unlock_irqrestore(&dev->irq_lock, flags);

	if (!snap.valid) {
		seq_puts(s, "(no submit yet)\n");
		return 0;
	}

	seq_printf(s, "src_addr:          %#08X\n", snap.src_addr);
	seq_printf(s, "dst_yaddr:         %#08X\n", snap.dst_yaddr);
	seq_printf(s, "dst_caddr:         %#08X\n", snap.dst_caddr);
	seq_printf(s, "src_width:         %u\n", snap.src_width);
	seq_printf(s, "src_height:        %u\n", snap.src_height);
	seq_printf(s, "read_mode:         %s\n", npu_rmod_str(snap.read_mode));
	seq_printf(s, "rgb_scale:         %s\n", npu_rgb_scale_str(snap.rgb_scale));
	seq_printf(s, "color_invert:      %u\n", snap.color_invert);
	seq_printf(s, "submitter_pid:     %d\n", snap.submitter_pid);
	seq_printf(s, "result:            %s\n", snap.result  ? errname(snap.result) : "SUCCESS");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(enx_en683_npu_dbg_last_submit);

static int enx_en683_npu_dbg_regs_show(struct seq_file *s, void *v)
{
	struct enx_vdma_dev *dev = s->private;
	u32 base_addr = dev->resource->start;
	u32 val;

	seq_printf(s, "Eyenix EN683 VDMA NPU Register Map\n");
	val = enx_vdma_reg_rd(dev, EN683_NPU_REG_SRC_RADDR);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_NPU_REG_SRC_RADDR, val);
	seq_printf(s, "%-20s: %#08X\n", "NPU_SRC_RADDR", val);
	val = enx_vdma_reg_rd(dev, EN683_NPU_REG_DST_YADDR);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_NPU_REG_DST_YADDR, val);
	seq_printf(s, "%-20s: %#08X\n", "NPU_DST_YADDR", val);
	val = enx_vdma_reg_rd(dev, EN683_NPU_REG_DST_CADDR);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_NPU_REG_DST_CADDR, val);
	seq_printf(s, "%-20s: %#08X\n", "NPU_DST_CADDR", val);
	val = enx_vdma_reg_rd(dev, EN683_NPU_REG_CTRL);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_NPU_REG_CTRL, val);
	seq_printf(s, "%-20s: %lu\n", "NPU_AWQOS", FIELD_GET(CTRL_AWQOS_MASK, val));
	seq_printf(s, "%-20s: %lu\n", "NPU_ARQOS", FIELD_GET(CTRL_ARQOS_MASK, val));
	seq_printf(s, "%-20s: %lu\n", "NPU_CINV", FIELD_GET(CTRL_CINV_BIT, val));
	seq_printf(s, "%-20s: %s\n", "NPU_RMOD", npu_rmod_str(FIELD_GET(CTRL_RMOD_MASK, val)));
	seq_printf(s, "%-20s: %s\n", "NPU_RGB_SCALE", npu_rgb_scale_str(FIELD_GET(CTRL_RGB_SCALE_MASK, val)));
	seq_printf(s, "%-20s: %lu\n", "NPU_GO", FIELD_GET(CTRL_GO_BIT, val));
	val = enx_vdma_reg_rd(dev, EN683_NPU_REG_RESOLUTION);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_NPU_REG_RESOLUTION, val);
	seq_printf(s, "%-20s: %lu\n", "NPU_SRC_VERT", FIELD_GET(RESOLUTION_VERT_MASK, val));
	seq_printf(s, "%-20s: %lu\n", "NPU_SRC_HORZ", FIELD_GET(RESOLUTION_HORZ_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_NPU_REG_CSC_COEF0);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_NPU_REG_CSC_COEF0, val);
	seq_printf(s, "%-20s: %lu\n", "NPU_CSC_COEF_YR", FIELD_GET(CSC_COEF0_YR_VAL, val));
	seq_printf(s, "%-20s: %lu\n", "NPU_CSC_COEF_YG", FIELD_GET(CSC_COEF0_YG_VAL, val));
	val = enx_vdma_reg_rd(dev, EN683_NPU_REG_CSC_COEF1);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_NPU_REG_CSC_COEF1, val);
	seq_printf(s, "%-20s: %lu\n", "NPU_CSC_COEF_YB", FIELD_GET(CSC_COEF1_YB_VAL, val));
	seq_printf(s, "%-20s: %lu\n", "NPU_CSC_COEF_UB", FIELD_GET(CSC_COEF1_UB_VAL, val));
	val = enx_vdma_reg_rd(dev, EN683_NPU_REG_CSC_COEF2);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_NPU_REG_CSC_COEF2, val);
	seq_printf(s, "%-20s: %lu\n", "NPU_CSC_COEF_VR", FIELD_GET(CSC_COEF2_VR_VAL, val));

	val = enx_vdma_reg_rd(dev, EN683_NPU_REG_IRQ_CTRL);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_NPU_REG_IRQ_CTRL, val);
	seq_printf(s, "%-20s: %lu\n", "NPU_IRQ_T_CLR", FIELD_GET(IRQ_CTRL_IRQ_T_CLR_BIT, val));
	seq_printf(s, "%-20s: %lu\n", "NPU_IRQ_T_EN", FIELD_GET(IRQ_CTRL_IRQ_T_EN_BIT, val));
	seq_printf(s, "%-20s: %lu\n", "NPU_IRQ_CLR", FIELD_GET(IRQ_CTRL_IRQ_CLR_BIT, val));
	seq_printf(s, "%-20s: %lu\n", "NPU_IRQ_EN", FIELD_GET(IRQ_CTRL_IRQ_EN_BIT, val));
	val = enx_vdma_reg_rd(dev, EN683_NPU_REG_IRQ_STATUS);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_NPU_REG_IRQ_STATUS, val);
	seq_printf(s, "%-20s: %c\n", "NPU_IRQ", FIELD_GET(IRQ_STATUS_IRQ_BIT, val) ? 'O' : 'X');
	val = enx_vdma_reg_rd(dev, EN683_NPU_REG_IRQ_T_STATUS);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_NPU_REG_IRQ_T_STATUS, val);
	seq_printf(s, "%-20s: %c\n", "NPU_IRQ_T", FIELD_GET(IRQ_STATUS_IRQ_T_BIT, val) ? 'O' : 'X');
	seq_printf(s, "--------------------------------\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(enx_en683_npu_dbg_regs);

static void enx_en683_npu_dbg_init(struct enx_vdma_dev *dev)
{
	if (!dev->dbg_dir)
		return;
	debugfs_create_file("regs",        0444, dev->dbg_dir, dev,
						&enx_en683_npu_dbg_regs_fops);
	debugfs_create_file("irq_stats",   0444, dev->dbg_dir, dev,
						&enx_en683_npu_dbg_irq_stats_fops);
	debugfs_create_file("last_submit", 0444, dev->dbg_dir, dev,
						&enx_en683_npu_dbg_last_submit_fops);
}

/*******************************************************************************
 *
 * probe / remove
 *
*******************************************************************************/

static int enx_en683_npu_probe(struct platform_device *pdev)
{
	struct enx_vdma_dev *dev;
	struct npu_dev_state *nds;
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
	nds = devm_kzalloc(&pdev->dev, sizeof(*nds), GFP_KERNEL);
	if (!nds) {
		ret = -ENOMEM;
		errorlog("failed(%d) create device state.\n", ret);
		goto err_free_core;
	}
	dev->dev_priv = nds;

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
	printlog("%s base[%px], real[0x%x]\n", NPU_DEV_NAME, dev->regs, (unsigned int)dev->resource->start);

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

	ret = request_irq(dev->irq, enx_en683_npu_irq, 0, NPU_DEV_NAME, dev);
	if (ret) {
		errorlog("failed(%d) to request irq for %s\n", ret, NPU_DEV_NAME);
		goto err_free_core;
	}

	ret = enx_vdma_core_register(dev, &npu_ops, &npu_fops, NPU_DEV_NAME);
	if (ret) {
		errorlog("failed(%d) to register %s core\n", ret, NPU_DEV_NAME);
		goto err_free_irq;
	}

	/* driver-side debugfs files under core's dbg_dir (lifetime tied to core) */
	enx_en683_npu_dbg_init(dev);

	platform_set_drvdata(pdev, dev);

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

static void enx_en683_npu_remove(struct platform_device *pdev)
{
	struct enx_vdma_dev *dev = platform_get_drvdata(pdev);
	u32 val;

	/* Unregister the core */
	enx_vdma_core_unregister(dev);

	/* Disable IRQ */
	val = enx_vdma_reg_rd(dev, EN683_NPU_REG_IRQ_CTRL);
	FIELD_MODIFY(IRQ_CTRL_IRQ_EN_BIT, &val, 0);
	enx_vdma_reg_wr(dev, EN683_NPU_REG_IRQ_CTRL, val);
	free_irq(dev->irq, dev);

	/* Device free */
	enx_vdma_core_free(dev);
}

static const struct of_device_id enx_en683_npu_of_match[] = {
	{ .compatible = "eyenix,en683-dma-npu" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, enx_en683_npu_of_match);

static struct platform_driver enx_en683_npu_driver = {
	.probe  = enx_en683_npu_probe,
	.remove = enx_en683_npu_remove,
	.driver = {
		.name		= DEVICE_NAME,
		.of_match_table	= enx_en683_npu_of_match,
	},
};
module_platform_driver(enx_en683_npu_driver);

MODULE_DESCRIPTION("EYENIX EN683 VDMA NPU driver");
MODULE_AUTHOR("YongJae Lee <yjlee@eyenix.com>");
MODULE_LICENSE("GPL v2");
