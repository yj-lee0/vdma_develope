// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Eyenix Corporation
 * Eyenix <support6@eyenix.com>
 *
 * Eyenix EN683 SoC Video-DMA JPEG Encoder controller.
 *  - YUV to JPEG encoding
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
#include "en683-jpegenc.h"
#include "en683-jpeg-uapi.h"

/*******************************************************************************
 *
 * eyenix define / config debuglog
 *
*******************************************************************************/

#include <soc/eyenix/cache.h>

#define JENC_DEV_NAME	"enx_vdma_jenc"
#define DEVICE_NAME		"VJENC"
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
 * DMA-JENC define & Module parameters
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
static const char *jenc_format_str(u32 k);

static int enx_en683_jenc_fmt_selecter(int format, u32 *mod, u32 *sel)
{
	switch (format) {
	case VDMA_FMT_YUV444:			*mod = eSRC_YUV444; *sel = 0; break;
	case VDMA_FMT_YUV444_YVU:		*mod = eSRC_YUV444; *sel = 1; break;
	case VDMA_FMT_YUV444_UVY:		*mod = eSRC_YUV444; *sel = 2; break;
	case VDMA_FMT_YUV444_UYV:		*mod = eSRC_YUV444; *sel = 3; break;
	case VDMA_FMT_YUV444_VUY:		*mod = eSRC_YUV444; *sel = 4; break;
	case VDMA_FMT_YUV444_VYU:		*mod = eSRC_YUV444; *sel = 5; break;

	case VDMA_FMT_YUV422_YUYV:		*mod = eSRC_YUV422; *sel = 2; break;
	case VDMA_FMT_YUV422_YVYU:		*mod = eSRC_YUV422; *sel = 3; break;
	case VDMA_FMT_YUV422_UYVY:		*mod = eSRC_YUV422; *sel = 0; break;
	case VDMA_FMT_YUV422_VYUY:		*mod = eSRC_YUV422; *sel = 1; break;

	case VDMA_FMT_YUV420SP_NV12:	*mod = eSRC_YUV420; *sel = 0; break;
	case VDMA_FMT_YVU420SP_NV21:	*mod = eSRC_YUV420; *sel = 1; break;

	case VDMA_FMT_YUV400:			*mod = eSRC_YUV400; *sel = 0; break;

	default:
		return -EFAULT;
	}
	return 0;
}

static u32 get_flush_size(int format, u32 width, u32 height)
{
	switch (format) {
	// x3
	case VDMA_FMT_YUV444:
	case VDMA_FMT_YUV444_YVU:
	case VDMA_FMT_YUV444_UVY:
	case VDMA_FMT_YUV444_UYV:
	case VDMA_FMT_YUV444_VUY:
	case VDMA_FMT_YUV444_VYU:
		return width * height * 3;

	// x2
	case VDMA_FMT_YUV422_YUYV:
	case VDMA_FMT_YUV422_YVYU:
	case VDMA_FMT_YUV422_UYVY:
	case VDMA_FMT_YUV422_VYUY:
		return width * height * 2;

	// x1.5
	case VDMA_FMT_YUV420SP_NV12:
	case VDMA_FMT_YVU420SP_NV21:
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
 * enx_vdma_core_ops - DMA JPEG Encoder HW Trigger
 *
*******************************************************************************/

static void jenc_hw_abort(struct enx_vdma_dev *dev)
{
	u32 val;

	/* IRQ disable + clear */
	val = enx_vdma_reg_rd(dev, EN683_JENC_REG_IRQ_CTRL);
	FIELD_MODIFY(IRQ_CTRL_IRQ_EN_MASK, &val, 0);
	FIELD_MODIFY(IRQ_CTRL_IRQ_CLR_MASK, &val, 0x3);
	enx_vdma_reg_wr(dev, EN683_JENC_REG_IRQ_CTRL, val);

	/* Set Discard */
	val = enx_vdma_reg_rd(dev, EN683_JENC_REG_CTRL);
	FIELD_MODIFY(CTRL_DISCARD_BIT, &val, 1);
	enx_vdma_reg_wr(dev, EN683_JENC_REG_CTRL, val);
}

static int jenc_hw_run_once(struct enx_vdma_sess *vs,
				struct enx_vdma_job_buf *dst, struct enx_vdma_job_buf *src,
				u32 src_count, void *blit_p, u32 job_flags)
{
	struct enx_vdma_dev *dev = vs->dev;
	struct jenc_dev_state *jds = NULL;
	struct enx_en683_jpegenc_submit_args *jsa = blit_p;
	phys_addr_t spa = 0, dpa = 0;
	u32 val, enc_reg_mod, enc_reg_sel, get_resp = 0;
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

	/* source resolution check */
	if (!jsa->cfg.src_width_total || !jsa->cfg.src_height_total) {
		errorlog("Invalid size: Source total_width(%hu) or total_height(%hu) is null.\n", jsa->cfg.src_width_total, jsa->cfg.src_height_total);
		ret = -EINVAL;
		goto out;
	}

	if (!jsa->cfg.src_width || !jsa->cfg.src_height) {
		errorlog("Invalid size: Set width(%hu)/height(%hu) to total_width/total_height when crop is disabled.\n", jsa->cfg.src_width, jsa->cfg.src_height);
		ret = -EINVAL;
		goto out;
	}

	/* crop range check */
	if (jsa->cfg.src_width && jsa->cfg.src_height) {
		if ((jsa->cfg.src_width_total < (jsa->cfg.src_width_pos + jsa->cfg.src_width)) || (jsa->cfg.src_height_total < (jsa->cfg.src_height_pos + jsa->cfg.src_height))) {
			errorlog("Invalid size: crop size exceeded the resolution\n");
			ret = -EINVAL;
			goto out;
		}
	}

	/* quality check */
	if (jsa->cfg.quality > 100) {
		errorlog("Invalid size: quality should be in the range of 1 to 100\n");
		ret = -EINVAL;
		goto out;
	}

	spa = src->data_offset + (jsa->cfg.src_addr_flag ? src->paddr : enx_vdma_buf_phys(src->vbuf));
	dpa = dst->data_offset + (jsa->cfg.dst_addr_flag ? dst->paddr : enx_vdma_buf_phys(dst->vbuf));

	printlog("JPEG ENC info\n");
	printlog(" - base          : 0x%08llx\n", spa);
	printlog(" - format        : %s\n", jenc_format_str(jsa->cfg.src_format));
	printlog(" - w x h total   : %d x %d\n", jsa->cfg.src_width_total, jsa->cfg.src_height_total);
	printlog(" - w x h  (crop) : %d x %d\n", jsa->cfg.src_width, jsa->cfg.src_height);
	printlog(" - offset (crop) : %d x %d\n", jsa->cfg.src_width_pos, jsa->cfg.src_height_pos);
	printlog(" - quality       : %d\n", jsa->cfg.quality);
	printlog(" - ovf_threshold : %d\n", jsa->cfg.ovf_threshold);

	/* src & dst cache flush */
	if (jsa->cfg.src_cache_flush) {
		printlog("cache flush->[SRC], addr %#llx, size %zu\n", spa, src->size);
		enx_en683_l2cache_flush64(spa, src->size);
	}
	if (jsa->cfg.dst_cache_flush) {
		printlog("cache flush->[DST], addr %#llx, size %zu\n", dpa, dst->size);
		enx_en683_l2cache_flush64(dpa, dst->size);
	}

	/* get source & destination format value */
	ret = enx_en683_jenc_fmt_selecter(jsa->cfg.src_format, &enc_reg_mod, &enc_reg_sel);
	if (ret != 0) {
		errorlog("Invalid source format %d\n", jsa->cfg.src_format);
		ret = -EINVAL;
		goto out;
	}

	/* hw register setting */
	{
		/* rd_sel & format setting */
		if (enc_reg_mod == eSRC_YUV444) {
			val = FIELD_PREP(SRC_FMT_MASK, enc_reg_mod);
			enx_vdma_reg_wr(dev, EN683_JENC_REG_SRC_IMG_FMT, val);
			val = FIELD_PREP(SRC_RD_SEL_YC444_MASK, enc_reg_sel);
			enx_vdma_reg_wr(dev, EN683_JENC_REG_SRC_RD_SEL, val);
		} else if (enc_reg_mod == eSRC_YUV422) {
			if ((jsa->cfg.src_width_total % 2 != 0) || (jsa->cfg.src_width_pos % 2 != 0)) {
				errorlog("If the width or src_width_pos is odd, not supported\n");
				ret = -EINVAL;
				goto out;
			}
			val = FIELD_PREP(SRC_FMT_MASK, enc_reg_mod);
			enx_vdma_reg_wr(dev, EN683_JENC_REG_SRC_IMG_FMT, val);
			val = FIELD_PREP(SRC_RD_SEL_YC422_MASK, enc_reg_sel);
			enx_vdma_reg_wr(dev, EN683_JENC_REG_SRC_RD_SEL, val);
		} else if (enc_reg_mod == eSRC_YUV420) {
			if ((jsa->cfg.src_width_total % 2 != 0) || (jsa->cfg.src_width_pos % 2 != 0)) {
				errorlog("If the width or src_width_pos is odd, not supported\n");
				ret = -EINVAL;
				goto out;
			}
			if ((jsa->cfg.src_height_pos % 2 != 0)) {
				errorlog("If the src_height_pos is odd, not supported\n");
				ret = -EINVAL;
				goto out;
			}
			if ((jsa->cfg.src_width_total < 16) || (jsa->cfg.src_height_total < 16)) {
				errorlog("YUV420 Minimum Size: 16x16\n");
				ret = -EINVAL;
				goto out;
			}
			val = FIELD_PREP(SRC_FMT_MASK, enc_reg_mod);
			enx_vdma_reg_wr(dev, EN683_JENC_REG_SRC_IMG_FMT, val);
			val = FIELD_PREP(SRC_RD_SEL_YC420_BIT, enc_reg_sel);
			enx_vdma_reg_wr(dev, EN683_JENC_REG_SRC_RD_SEL, val);
		} else if (enc_reg_mod == eSRC_YUV400) {
			val = FIELD_PREP(SRC_FMT_MASK, enc_reg_mod);
			enx_vdma_reg_wr(dev, EN683_JENC_REG_SRC_IMG_FMT, val);
		}

		/* src & dst address setting */
		enx_vdma_reg_wr(dev, EN683_JENC_REG_SRC_YADDR, spa);
		enx_vdma_reg_wr(dev, EN683_JENC_REG_SRC_CADDR, spa + (u32)(jsa->cfg.src_width_total * jsa->cfg.src_height_total));
		enx_vdma_reg_wr(dev, EN683_JENC_REG_DST_ADDR, dpa);

		/* source resolution setting */
		val = FIELD_PREP(SRC_RESOLUTION_VERT_MASK, jsa->cfg.src_height_total) | FIELD_PREP(SRC_RESOLUTION_HORZ_MASK, jsa->cfg.src_width_total);
		enx_vdma_reg_wr(dev, EN683_JENC_REG_SRC_RESOLUTION, val);

		/* crop info */
		if (jsa->cfg.src_width && jsa->cfg.src_height) {
			val = FIELD_PREP(CROP_CTRL0_VERT_MASK, jsa->cfg.src_height) | FIELD_PREP(CROP_CTRL0_HORZ_MASK, jsa->cfg.src_width);
			enx_vdma_reg_wr(dev, EN683_JENC_REG_SRC_CROP_CTRL0, val);
			val = FIELD_PREP(CROP_CTRL1_COORD_X_MASK, jsa->cfg.src_width_pos) | FIELD_PREP(CROP_CTRL1_COORD_Y_MASK, jsa->cfg.src_height_pos);
			enx_vdma_reg_wr(dev, EN683_JENC_REG_SRC_CROP_CTRL1, val);
		}

		/* quality & overflow threshold setting */
		val = enx_vdma_reg_rd(dev, EN683_JENC_REG_OPT_CTRL);
		if (jsa->cfg.ovf_threshold) {
			FIELD_MODIFY(OPT_CTRL_OVF_AUTO_STOP_BIT, &val, 1);
			FIELD_MODIFY(OPT_CTRL_OVF_THRESHOLD_MASK, &val, jsa->cfg.ovf_threshold);
			enx_vdma_reg_wr(dev, EN683_JENC_REG_OPT_CTRL, val);
		} else {
			/* The default overflow threshold is set to the allocated maximum destination buffer size. */
			FIELD_MODIFY(OPT_CTRL_OVF_THRESHOLD_MASK, &val, dst->size);
			FIELD_MODIFY(OPT_CTRL_OVF_AUTO_STOP_BIT, &val, 1);
			enx_vdma_reg_wr(dev, EN683_JENC_REG_OPT_CTRL, val);
		}
		if (jsa->cfg.quality) {
			enx_vdma_reg_wr(dev, EN683_JENC_REG_QUALITY, jsa->cfg.quality);
		} else {
			/* The default quality is set to 50, which is a generally used value. */
			enx_vdma_reg_wr(dev, EN683_JENC_REG_QUALITY, 0x32);
		}

		/* Set hw busy */
		spin_lock_irqsave(&dev->irq_lock, flags);
		WRITE_ONCE(dev->hw_idle, false);
		spin_unlock_irqrestore(&dev->irq_lock, flags);

		/* Set IRQ enable */
		val = FIELD_PREP(IRQ_CTRL_IRQ_EN_MASK, 3); // Enable both DONE and ERR IRQ
		enx_vdma_reg_wr(dev, EN683_JENC_REG_IRQ_CTRL, val);

		/* start rendering */
		val = enx_vdma_reg_rd(dev, EN683_JENC_REG_CTRL);
		FIELD_MODIFY(CTRL_GO_BIT, &val, 1);
		enx_vdma_reg_wr(dev, EN683_JENC_REG_CTRL, val);

		printlog("Start jpeg-enc rendering.\n\n");
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
		jsa->resp.cycle		= enx_vdma_reg_rd(dev, EN683_JENC_REG_ENC_CYCLE_CNT);
		jsa->resp.jpeg_size	= enx_vdma_reg_rd(dev, EN683_JENC_REG_ENC_FILE_SIZE);
		jsa->resp.process_error.value = enx_vdma_reg_rd(dev, EN683_JENC_REG_ENC_RESPONSE);
	}

out:
	/* Capture last-submit snapshot for debugfs (best-effort). */
	if (jds) {
		spin_lock_irqsave(&dev->irq_lock, flags);
		jds->last.valid					  = get_resp ? 2 : 1;
		jds->last.src_yaddr				  = spa;
		jds->last.src_caddr				  = spa + (jsa->cfg.src_width_total * jsa->cfg.src_height_total);
		jds->last.dst_yaddr				  = dpa;
		jds->last.src_format			  = jsa->cfg.src_format;
		jds->last.src_width_total		  = jsa->cfg.src_width_total;
		jds->last.src_height_total		  = jsa->cfg.src_height_total;
		jds->last.src_width				  = jsa->cfg.src_width;
		jds->last.src_height			  = jsa->cfg.src_height;
		jds->last.src_width_pos			  = jsa->cfg.src_width_pos;
		jds->last.src_height_pos		  = jsa->cfg.src_height_pos;
		jds->last.ovf_threshold			  = jsa->cfg.ovf_threshold;
		jds->last.quality				  = jsa->cfg.quality;
		jds->last.submitter_pid			  = vs->pid;
		jds->last.result				  = ret;
		if (get_resp) {
			printlog("JPEG ENC response\n");
			jds->last.cycle				  = jsa->resp.cycle;
			jds->last.jpeg_size			  = jsa->resp.jpeg_size;
			jds->last.process_error.value = jsa->resp.process_error.value;
		}
		spin_unlock_irqrestore(&dev->irq_lock, flags);
	}
	exitlog();
	return ret;
}

static const struct enx_vdma_core_ops jenc_ops = {
	.hw_run_once	= jenc_hw_run_once,
	.hw_abort		= jenc_hw_abort,
	.session_open	= NULL,
	.session_release= NULL,
	.blit_size		= sizeof(struct enx_en683_jpegenc_submit_args),
	.version		= PKG_BUILD_VER,
};


/*******************************************************************************
 *
 * SUBMIT ioctl handler (function-specific UAPI struct → core helper)
 *
*******************************************************************************/

/* Legacy mode (Sync only) */
static long jenc_ioctl_submit(struct enx_vdma_sess *sess, void __user *uarg)
{
	// struct enx_vdma_dev *dev = sess->dev;
	struct enx_en683_jpegenc_submit_args args;
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
	ref_kind = args.cfg.src_addr_flag ? ENX_BUF_RAW : ENX_BUF_ID;
	src_size = get_flush_size(args.cfg.src_format, args.cfg.src_width_total, args.cfg.src_height_total);
	if (src_size == 0) {
		errorlog("Invalid source format %d\n", args.cfg.src_format);
		ret = -EINVAL;
		goto out;
	}

	src_buf.kind		= ref_kind;
	src_buf.data_offset	= args.cfg.src_data_offset;
	src_buf.size		= src_size;
	if (ref_kind == ENX_BUF_ID) {
		src_buf.id		= args.cfg.src_addr;
	} else {
		src_buf.paddr	= args.cfg.src_addr;
	}

	/* setting destination buffer info */
	ref_kind = args.cfg.dst_addr_flag ? ENX_BUF_RAW : ENX_BUF_ID;
	dst_size = args.cfg.ovf_threshold ? args.cfg.ovf_threshold : get_flush_size(args.cfg.src_format, args.cfg.src_width_total, args.cfg.src_height_total);
	if (dst_size == 0) {
		errorlog("Invalid destination format %d\n", args.cfg.src_format);
		ret = -EINVAL;
		goto out;
	}

	{ // fill struct enx_vdma_submit_params
		p.dst.kind			= ref_kind;
		p.dst.data_offset	= args.cfg.dst_data_offset;
		p.dst.size			= dst_size;
		if (ref_kind == ENX_BUF_ID) {
			p.dst.id		= args.cfg.dst_addr;
		} else {
			p.dst.paddr		= args.cfg.dst_addr;
		}
		p.src				= &src_buf;
		p.src_count			= 1; 				// must be 1
		p.blits				= (void *)&args;
		p.blit_size			= sizeof(struct enx_en683_jpegenc_submit_args);
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

static long jenc_ioctl_discard(struct enx_vdma_sess *sess, void __user *uarg)
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

static long enx_en683_jenc_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
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
	case VDMAIOSET_JPEG_ENC_SUBMIT:	ret = jenc_ioctl_submit(sess, uarg); break;
	case VDMAIOSET_JPEG_DISCARD:	ret = jenc_ioctl_discard(sess, uarg); break;

	default:
		ret = -ENOTTY;
	}

	exitlog();
	return ret;
}

static const struct file_operations jenc_fops = {
	.owner			= THIS_MODULE,
	.open			= enx_vdma_open,
	.release		= enx_vdma_release,
	.unlocked_ioctl	= enx_en683_jenc_ioctl,
	.compat_ioctl	= enx_en683_jenc_ioctl,
	.mmap			= enx_vdma_mmap,
	.poll			= enx_vdma_poll,
	.llseek			= noop_llseek,
};

/*******************************************************************************
 *
 * interrupt
 *
*******************************************************************************/

static irqreturn_t enx_en683_jenc_irq(int irq, void *dev_id)
{
	struct enx_vdma_dev *dev = dev_id;
	struct jenc_dev_state *jds = dev->dev_priv;
	irqreturn_t ret = IRQ_NONE;
	u32 status, val;

	enterlog();

	spin_lock(&dev->irq_lock);
	status = enx_vdma_reg_rd(dev, EN683_JENC_REG_IRQ_STATUS);
	if (FIELD_GET(IRQ_STATUS_DONE_IRQ_BIT, status)) {
		jds->done_irq_count++;
	}
	if (FIELD_GET(IRQ_STATUS_OVERRUN_IRQ_BIT, status)) {
		jds->err_irq_count++;
	}

	if (status) {
		val = enx_vdma_reg_rd(dev, EN683_JENC_REG_IRQ_CTRL);
		FIELD_MODIFY(IRQ_CTRL_IRQ_CLR_MASK, &val, status);
		enx_vdma_reg_wr(dev, EN683_JENC_REG_IRQ_CTRL, val);

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

static int enx_en683_jenc_dbg_irq_stats_show(struct seq_file *s, void *v)
{
	/* private is the parent enx_vdma_dev so we can use dev->irq_lock. */
	struct enx_vdma_dev *dev = s->private;
	struct jenc_dev_state *jds = dev->dev_priv;
	u64 irq_count, timeout_count, done_irq_count, err_irq_count;
	u32 last_status;
	unsigned long flags;

	spin_lock_irqsave(&dev->irq_lock, flags);
	irq_count		= jds->irq_count;
	done_irq_count	= jds->done_irq_count;
	err_irq_count	= jds->err_irq_count;
	last_status		= jds->last_irq_status;
	timeout_count	= jds->timeout_count;
	spin_unlock_irqrestore(&dev->irq_lock, flags);

	seq_printf(s, "total_irq_count:     %llu\n", irq_count);
	seq_printf(s, "done_irq_count:      %llu\n", done_irq_count);
	seq_printf(s, "error_irq_count:     %llu\n", err_irq_count);
	seq_printf(s, "last_status:         0x%08x\n", last_status);
	seq_printf(s, "timeout_count:       %llu\n", timeout_count);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(enx_en683_jenc_dbg_irq_stats);

static const char *jenc_format_str(u32 k)
{
	switch (k) {
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

static int enx_en683_jenc_dbg_last_submit_show(struct seq_file *s, void *v)
{
	struct enx_vdma_dev *dev = s->private;
	struct jenc_dev_state *jds = dev->dev_priv;
	struct jenc_last_submit snap;
	unsigned long flags;

	spin_lock_irqsave(&dev->irq_lock, flags);
	snap = jds->last;
	spin_unlock_irqrestore(&dev->irq_lock, flags);

	if (!snap.valid) {
		seq_puts(s, "(no submit yet)\n");
		return 0;
	}

	seq_printf(s, "src_yaddr:         %#08X\n", snap.src_yaddr);
	seq_printf(s, "src_caddr:         %#08X\n", snap.src_caddr);
	seq_printf(s, "dst_yaddr:         %#08X\n", snap.dst_yaddr);
	seq_printf(s, "src_format:        %s\n", jenc_format_str(snap.src_format));
	seq_printf(s, "src_width_total:   %u\n", snap.src_width_total);
	seq_printf(s, "src_height_total:  %u\n", snap.src_height_total);
	seq_printf(s, "src_width:         %u\n", snap.src_width);
	seq_printf(s, "src_height:        %u\n", snap.src_height);
	seq_printf(s, "src_width_pos:     %u\n", snap.src_width_pos);
	seq_printf(s, "src_height_pos:    %u\n", snap.src_height_pos);
	seq_printf(s, "ovf_threshold:     %u\n", snap.ovf_threshold);
	seq_printf(s, "quality:           %u\n", snap.quality);
	seq_printf(s, "submitter_pid:     %d\n", snap.submitter_pid);
	seq_printf(s, "result:            %s\n", snap.result  ? errname(snap.result) : "SUCCESS");
	if (snap.valid == 2) {
		seq_printf(s, "cycle:             %u\n", snap.cycle);
		seq_printf(s, "jpeg_size:         %u\n", snap.jpeg_size);
		seq_printf(s, "process_error:     %#08X\n", snap.process_error.value);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(enx_en683_jenc_dbg_last_submit);

static int enx_en683_jenc_dbg_regs_show(struct seq_file *s, void *v)
{
	struct enx_vdma_dev *dev = s->private;
	u32 base_addr = dev->resource->start;
	u32 val;

	seq_printf(s, "Eyenix EN683 VDMA JPEG Encoder Register Map\n");
	val = enx_vdma_reg_rd(dev, EN683_JENC_REG_AXI_CTRL);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_JENC_REG_AXI_CTRL, val);
	seq_printf(s, "%-20s: %lu\n", "JENC_AWMOR", FIELD_GET(AXI_CTRL_AWMOR_BIT, val));
	seq_printf(s, "%-20s: %lu\n", "JENC_AWLEN", FIELD_GET(AXI_CTRL_AWLEN_MASK, val));
	seq_printf(s, "%-20s: %lu\n", "JENC_ARMOR", FIELD_GET(AXI_CTRL_ARMOR_MASK, val));
	seq_printf(s, "%-20s: %lu\n", "JENC_ARLEN", FIELD_GET(AXI_CTRL_ARLEN_MASK, val));
	seq_printf(s, "%-20s: %lu\n", "JENC_AXI_EDN", FIELD_GET(AXI_CTRL_AXI_EDN_BIT, val));
	val = enx_vdma_reg_rd(dev, EN683_JENC_REG_SRC_RD_SEL);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_JENC_REG_SRC_RD_SEL, val);
	seq_printf(s, "%-20s: %lu\n", "JENC_SEL_YC420", FIELD_GET(SRC_RD_SEL_YC420_BIT, val));
	seq_printf(s, "%-20s: %lu\n", "JENC_SEL_YC422", FIELD_GET(SRC_RD_SEL_YC422_MASK, val));
	seq_printf(s, "%-20s: %lu\n", "JENC_SEL_YC444", FIELD_GET(SRC_RD_SEL_YC444_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_JENC_REG_SRC_IMG_FMT);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_JENC_REG_SRC_IMG_FMT, val);
	seq_printf(s, "%-20s: %lu\n", "JENC_SRC_FMT", FIELD_GET(SRC_FMT_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_JENC_REG_SRC_YADDR);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_JENC_REG_SRC_YADDR, val);
	seq_printf(s, "%-20s: %#08X\n", "JENC_SRC_YADDR", val);
	val = enx_vdma_reg_rd(dev, EN683_JENC_REG_SRC_CADDR);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_JENC_REG_SRC_CADDR, val);
	seq_printf(s, "%-20s: %#08X\n", "JENC_SRC_CADDR", val);
	val = enx_vdma_reg_rd(dev, EN683_JENC_REG_SRC_RESOLUTION);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_JENC_REG_SRC_RESOLUTION, val);
	seq_printf(s, "%-20s: %lu\n", "JENC_SRC_VERT", FIELD_GET(SRC_RESOLUTION_VERT_MASK, val));
	seq_printf(s, "%-20s: %lu\n", "JENC_SRC_HORZ", FIELD_GET(SRC_RESOLUTION_HORZ_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_JENC_REG_SRC_CROP_CTRL0);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_JENC_REG_SRC_CROP_CTRL0, val);
	seq_printf(s, "%-20s: %lu\n", "JENC_CROP_VERT", FIELD_GET(CROP_CTRL0_VERT_MASK, val));
	seq_printf(s, "%-20s: %lu\n", "JENC_CROP_HORZ", FIELD_GET(CROP_CTRL0_HORZ_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_JENC_REG_SRC_CROP_CTRL1);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_JENC_REG_SRC_CROP_CTRL1, val);
	seq_printf(s, "%-20s: %lu\n", "JENC_CROP_COORD_X", FIELD_GET(CROP_CTRL1_COORD_X_MASK, val));
	seq_printf(s, "%-20s: %lu\n", "JENC_CROP_COORD_Y", FIELD_GET(CROP_CTRL1_COORD_Y_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_JENC_REG_DST_ADDR);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_JENC_REG_DST_ADDR, val);
	seq_printf(s, "%-20s: %#08X\n", "JENC_DST_ADDR", val);
	val = enx_vdma_reg_rd(dev, EN683_JENC_REG_OPT_CTRL);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_JENC_REG_OPT_CTRL, val);
	seq_printf(s, "%-20s: %lu\n", "JENC_SOI_JFIF_WR", FIELD_GET(OPT_CTRL_SOI_JFIF_WR_BIT, val));
	seq_printf(s, "%-20s: %lu\n", "JENC_OVF_AUTO_STOP", FIELD_GET(OPT_CTRL_OVF_AUTO_STOP_BIT, val));
	seq_printf(s, "%-20s: %lu\n", "JENC_OVF_THRESHOLD", FIELD_GET(OPT_CTRL_OVF_THRESHOLD_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_JENC_REG_QTBL_CTRL);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_JENC_REG_QTBL_CTRL, val);
	seq_printf(s, "%-20s: %lu\n", "JENC_QTBL_DC_C", FIELD_GET(QTBL_CTRL_DC_C_BIT, val));
	seq_printf(s, "%-20s: %lu\n", "JENC_QTBL_DC_Y", FIELD_GET(QTBL_CTRL_DC_Y_BIT, val));
	seq_printf(s, "%-20s: %lu\n", "JENC_QTBL_DC_BYPASS", FIELD_GET(QTBL_CTRL_DC_BYPASS_BIT, val));
	seq_printf(s, "%-20s: %lu\n", "JENC_QTBL_BYPASS", FIELD_GET(QTBL_CTRL_BYPASS_BIT, val));
	seq_printf(s, "%-20s: %lu\n", "JENC_QTBL_SELECT", FIELD_GET(QTBL_CTRL_SELECT_BIT, val));
	val = enx_vdma_reg_rd(dev, EN683_JENC_REG_QUALITY);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_JENC_REG_QUALITY, val);
	seq_printf(s, "%-20s: %lu\n", "JENC_QUALITY", FIELD_GET(QUALITY_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_JENC_REG_CTRL);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_JENC_REG_CTRL, val);
	seq_printf(s, "%-20s: %lu\n", "JENC_CTRL_DISCARD", FIELD_GET(CTRL_DISCARD_BIT, val));
	seq_printf(s, "%-20s: %lu\n", "JENC_CTRL_GO", FIELD_GET(CTRL_GO_BIT, val));
	val = enx_vdma_reg_rd(dev, EN683_JENC_REG_IRQ_CTRL);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_JENC_REG_IRQ_CTRL, val);
	seq_printf(s, "%-20s: %lu\n", "JENC_IRQ_CLR", FIELD_GET(IRQ_CTRL_IRQ_CLR_MASK, val));
	seq_printf(s, "%-20s: %lu\n", "JENC_IRQ_EN", FIELD_GET(IRQ_CTRL_IRQ_EN_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_JENC_REG_IRQ_STATUS);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_JENC_REG_IRQ_STATUS, val);
	seq_printf(s, "%-20s: %c\n", "JENC_OVERRUN_IRQ", FIELD_GET(IRQ_STATUS_OVERRUN_IRQ_BIT, val) ? 'O' : 'X');
	seq_printf(s, "%-20s: %c\n", "JENC_DONE_IRQ", FIELD_GET(IRQ_STATUS_DONE_IRQ_BIT, val) ? 'O' : 'X');
	val = enx_vdma_reg_rd(dev, EN683_JENC_REG_ENC_CYCLE_CNT);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_JENC_REG_ENC_CYCLE_CNT, val);
	seq_printf(s, "%-20s: %u\n", "JENC_ENC_CYCLE_CNT", val);
	val = enx_vdma_reg_rd(dev, EN683_JENC_REG_ENC_FILE_SIZE);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_JENC_REG_ENC_FILE_SIZE, val);
	seq_printf(s, "%-20s: %u\n", "JENC_ENC_FILE_SIZE", val);
	val = enx_vdma_reg_rd(dev, EN683_JENC_REG_ENC_RESPONSE);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_JENC_REG_ENC_RESPONSE, val);
	seq_printf(s, "%-20s: %lu\n", "JENC_ERR_FILE_OVF", FIELD_GET(ENC_RESPONSE_ERR_FILE_OVF_BIT, val));
	seq_printf(s, "%-20s: %lu\n", "JENC_ERR_OCCUR", FIELD_GET(ENC_RESPONSE_ERR_OCCUR_BIT, val));
	seq_printf(s, "--------------------------------\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(enx_en683_jenc_dbg_regs);

static void enx_en683_jenc_dbg_init(struct enx_vdma_dev *dev)
{
	if (!dev->dbg_dir)
		return;
	debugfs_create_file("regs",        0444, dev->dbg_dir, dev,
						&enx_en683_jenc_dbg_regs_fops);
	debugfs_create_file("irq_stats",   0444, dev->dbg_dir, dev,
						&enx_en683_jenc_dbg_irq_stats_fops);
	debugfs_create_file("last_submit", 0444, dev->dbg_dir, dev,
						&enx_en683_jenc_dbg_last_submit_fops);
}

/*******************************************************************************
 *
 * probe / remove
 *
*******************************************************************************/

static int enx_en683_jenc_probe(struct platform_device *pdev)
{
	struct enx_vdma_dev *dev;
	struct jenc_dev_state *jds;
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
	printlog("%s base[%px], real[0x%x]\n", JENC_DEV_NAME, dev->regs, (unsigned int)dev->resource->start);

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

	ret = request_irq(dev->irq, enx_en683_jenc_irq, 0, JENC_DEV_NAME, dev);
	if (ret) {
		errorlog("failed(%d) to request irq for %s\n", ret, JENC_DEV_NAME);
		goto err_free_core;
	}

	ret = enx_vdma_core_register(dev, &jenc_ops, &jenc_fops, JENC_DEV_NAME);
	if (ret) {
		errorlog("failed(%d) to register %s core\n", ret, JENC_DEV_NAME);
		goto err_free_irq;
	}

	/* driver-side debugfs files under core's dbg_dir (lifetime tied to core) */
	enx_en683_jenc_dbg_init(dev);

	platform_set_drvdata(pdev, dev);

	/* Set version info for enxlog */
	SetModuleVersion_t func_ptr = (SetModuleVersion_t)symbol_get(SetModuleVersion);
	if (func_ptr) {
		func_ptr(VER_VJENC, PKG_BUILD_VER);
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

static void enx_en683_jenc_remove(struct platform_device *pdev)
{
	struct enx_vdma_dev *dev = platform_get_drvdata(pdev);
	u32 val;

	/* Unregister the core */
	enx_vdma_core_unregister(dev);

	/* Disable IRQ */
	val = enx_vdma_reg_rd(dev, EN683_JENC_REG_IRQ_CTRL);
	FIELD_MODIFY(IRQ_CTRL_IRQ_EN_MASK, &val, 0);
	enx_vdma_reg_wr(dev, EN683_JENC_REG_IRQ_CTRL, val);
	free_irq(dev->irq, dev);

	/* Device free */
	enx_vdma_core_free(dev);
}

static const struct of_device_id enx_en683_jenc_of_match[] = {
	{ .compatible = "eyenix,en683-dma-jpegenc" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, enx_en683_jenc_of_match);

static struct platform_driver enx_en683_jenc_driver = {
	.probe  = enx_en683_jenc_probe,
	.remove = enx_en683_jenc_remove,
	.driver = {
		.name		= DEVICE_NAME,
		.of_match_table	= enx_en683_jenc_of_match,
	},
};
module_platform_driver(enx_en683_jenc_driver);

MODULE_DESCRIPTION("EYENIX EN683 VDMA JPEG Encoder driver");
MODULE_AUTHOR("YongJae Lee <yjlee@eyenix.com>");
MODULE_LICENSE("GPL v2");
