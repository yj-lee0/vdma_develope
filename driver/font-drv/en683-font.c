// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Eyenix Corporation
 * Eyenix <support6@eyenix.com>
 *
 * Eyenix EN683 SoC Video-DMA Font controller.
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
#include "en683-font.h"
#include "en683-font-uapi.h"

/*******************************************************************************
 *
 * eyenix define / config debuglog
 *
*******************************************************************************/

#include <soc/eyenix/cache.h>

#define FONT_DEV_NAME	"enx_vdma_font"
#define DEVICE_NAME		"VFONT"
#define DEBUGLOG_ERROR // Comment out if not in use.
#define DEBUGLOG_CHECK // Comment out if not in use.
#define DEBUGLOG_PRINT // Comment out if not in use.
// #define DEBUGLOG_ENTER // Comment out if not in use.
// #define DEBUGLOG_EXIT  // Comment out if not in use.
#include <soc/eyenix/debuglog.h>

#include <dev_linksys_priv.h>	// for SetModuleVersion()
typedef void (*SetModuleVersion_t)(int nMode, char *ver);

/*******************************************************************************
 *
 * DMA-Font define & Module parameters
 *
*******************************************************************************/

static unsigned int max_bufs = VDMA_BUF_DEFAULT;
module_param(max_bufs, uint, 0444);
MODULE_PARM_DESC(max_bufs, "Max simultaneous buffers (default: 16)");

static unsigned int max_src = VDMA_BUF_DEFAULT;
module_param(max_src, uint, 0444);
MODULE_PARM_DESC(max_src, "Max simultaneous draw src (default: 16)");

#define EN683_FONT_CHIP_TYPE_EN683		(0)
#define EN683_FONT_CHIP_TYPE_EN683A		(1)

/*******************************************************************************
 *
 * enx_vdma_core_ops - DMA Font HW Trigger
 *
*******************************************************************************/

static void font_hw_abort(struct enx_vdma_dev *dev)
{
	/* IRQ disable + clear */
	u32 val = 0;
	FIELD_MODIFY(IRQ_CTRL_IRQ_CLR_BIT, &val, 1);
	FIELD_MODIFY(IRQ_CTRL_IRQ_CLR_T_BIT, &val, 1);
	enx_vdma_reg_wr(dev, EN683_FONT_REG_IRQ_CTRL, val);
}

static int en683_font_hw_run_once(struct enx_vdma_sess *vs,
				struct enx_vdma_job_buf *dst, struct enx_vdma_job_buf *src,
				u32 src_count, void *blit_p, u32 job_flags)
{
	struct enx_vdma_dev *dev = vs->dev;
	struct font_dev_state *fds = dev->dev_priv;
	struct font_session *fs;
	struct enx_en683_font_desc *desc;
	struct enx_en683_font_submit_args *fsa = blit_p;
	u32 val, dst_yaddr = 0, dst_caddr = 0, y_offset = 0, c_offset = 0;
	u32 active_mode;
	int ret = 0, i;
	unsigned long flags;

	enterlog();

	if (!dev->dev_priv) {
		errorlog("device private data is NULL.\n");
		ret = -EINVAL;
		goto out;
	}
	fds = dev->dev_priv;

	if (!vs->sess_priv) {
		errorlog("session private data is NULL.\n");
		ret = -EINVAL;
		goto out;
	}
	fs = vs->sess_priv;
	desc = fs->font_desc;

	active_mode = READ_ONCE(fds->active_mode);
	/* Check active mode */
	if (active_mode == ENX_VDMA_MODE_NONE) {
		errorlog("Invalid operation for current mode (active_mode: ENX_VDMA_MODE_NONE)\n");
		ret = -EINVAL;
		goto out;
	} else if (active_mode == ENX_VDMA_MODE_KERNEL) {
		/* busy check */
		if (!READ_ONCE(dev->hw_idle)) {
			errorlog("%s is busy.\n", DEVICE_NAME);
			ret = -EBUSY;
			goto out;
		}

		if (!fsa->dst_width || !fsa->dst_height) {
			errorlog("Invalid size: Destination width(%d) or height(%d) is null.\n", fsa->dst_width, fsa->dst_height);
			ret = -EINVAL;
			goto out;
		}
	} else {
		if ((fsa->dst_width || fsa->dst_height)) {
			errorlog("Invalid size: Since init_type is Videocore, Destination width(%d) and height(%d) are not used.\n", fsa->dst_width, fsa->dst_height);
			ret = -EINVAL;
			goto out;
		}

		if (fsa->font_number + fsa->dst_type_index > dev->max_src) {
			errorlog("Invalid size: The total number of font src (font_number: %d) and desc_idx (dst_type_index: %d) exceeds the maximum source number supported by the device (max_src: %u).\n",
						fsa->font_number, fsa->dst_type_index, dev->max_src);
			ret = -EINVAL;
			goto out;
		}
	}

	/* check destination size */
	if (dst->size < (u32)fsa->dst_width * fsa->dst_height * 3 >> 1) {
		errorlog("Invalid size: Destination buffer size(%lu) is smaller than required size(%u).\n", dst->size, fsa->dst_width * fsa->dst_height * 3 >> 1);
		ret = -EINVAL;
		goto out;
	}

	/* destination cache flush */
	if (active_mode == ENX_VDMA_MODE_KERNEL) {
		if (dst->kind == ENX_BUF_ID) {
			phys_addr_t pa = enx_vdma_buf_phys(dst->vbuf);
			printlog("cache flush->[DST], addr %#llx, size %u\n", (u64)pa, (u32)fsa->dst_width * fsa->dst_height);
			if (fsa->background_overlay) { memset(enx_vdma_buf_vaddr(dst->vbuf), 0, fsa->dst_width * fsa->dst_height * 3 >> 1); }
			enx_en683_l2cache_flush64(pa, fsa->dst_width * fsa->dst_height * 3 >> 1);
		} else {
			printlog("cache flush->[DST], addr %#llx, size %u\n", dst->paddr, (u32)fsa->dst_width * fsa->dst_height);
			if (fsa->background_overlay) { printlog("not supported background overlay for raw buffer. Please use buffer allocated by vdma driver.\n"); }
			enx_en683_l2cache_flush64(dst->paddr, dst->size);
		}
	}

	/* Buffer-level validation is handled by core's fill_vdma_submit_job_buf(). */
	for (i=0; i<fsa->font_number; i++) {
		/* src buf size check */
		if (src[i].kind == ENX_BUF_ID) {
			if (src[i].size < fsa->blits[i].src_width * fsa->blits[i].src_height) {
				errorlog("Invalid size: Source buffer size(%lu) is smaller than required size(%u). (idx : %d)\n",
							src[i].size, fsa->blits[i].src_width * fsa->blits[i].src_height, i);
				ret = -EINVAL;
				goto out;
			}
		}

		/* source cache flush */
		if (fsa->blits[i].src_width != 1 || fsa->blits[i].src_height != 1) {
			if (src[i].kind == ENX_BUF_ID) {
				phys_addr_t pa = enx_vdma_buf_phys(src[i].vbuf);
				printlog("cache flush->[SRC%d], addr %#llx, size %u\n", i, (u64)pa, (u32)fsa->blits[i].src_width * fsa->blits[i].src_height);
				enx_en683_l2cache_flush64(pa, fsa->blits[i].src_width * fsa->blits[i].src_height);
			} else {
				printlog("cache flush->[SRC%d], addr %#llx, size %u\n", i, src[i].paddr, (u32)fsa->blits[i].src_width * fsa->blits[i].src_height);
				enx_en683_l2cache_flush64(src[i].paddr, fsa->blits[i].src_width * fsa->blits[i].src_height);
			}
		}
	}

	/* font description setting */
	dst_yaddr = dst->data_offset + (dst->kind == ENX_BUF_ID ? enx_vdma_buf_phys(dst->vbuf) : dst->paddr);
	dst_caddr = dst_yaddr + fsa->dst_width * fsa->dst_height;
	for (i=0; i<fsa->font_number; i++) {
		if (!fsa->blits[i].src_width || !fsa->blits[i].src_height) {
			errorlog("Invalid size: Source width(%d) and height(%d) are not valid for font_info[%d]\n", fsa->blits[i].src_width, fsa->blits[i].src_height, i);
			ret = -EINVAL;
			goto out;
		}

		if (active_mode == ENX_VDMA_MODE_KERNEL) {
			if (fsa->blits[i].font_xpos || fsa->blits[i].font_ypos) {
				if (fsa->blits[i].src_width + fsa->blits[i].font_xpos > fsa->dst_width ||
					fsa->blits[i].src_height + fsa->blits[i].font_ypos > fsa->dst_height) {
					errorlog("Error, Invalid font size or position.\n");
					ret = -EINVAL;
					goto out;
				}
			}
			y_offset = fsa->blits[i].font_xpos + (fsa->blits[i].font_ypos * fsa->dst_width);
			c_offset = fsa->blits[i].font_xpos + (fsa->blits[i].font_ypos * fsa->dst_width / 2);

			desc[i].font_src						=	src[i].data_offset + (src[i].kind == ENX_BUF_ID ?
																enx_vdma_buf_phys(src[i].vbuf) : src[i].paddr);
			desc[i].font_width.width				=	fsa->blits[i].src_width;
			desc[i].font_height.height				=	fsa->blits[i].src_height;
			desc[i].font_yaddr0						=	dst_yaddr + y_offset;
			desc[i].font_caddr0						=	dst_caddr + c_offset;
			desc[i].font_color.font_y				=	fsa->blits[i].font_color_y;
			desc[i].font_color.font_cb				=	fsa->blits[i].font_color_cb;
			desc[i].font_color.font_cr				=	fsa->blits[i].font_color_cr;
			desc[i].font_color.font					=	fsa->blits[i].font_value;
			desc[i].font_outline_color.outline_y	=	fsa->blits[i].outline_color_y;
			desc[i].font_outline_color.outline_cb	=	fsa->blits[i].outline_color_cb;
			desc[i].font_outline_color.outline_cr	=	fsa->blits[i].outline_color_cr;
			desc[i].font_outline_color.outline		=	fsa->blits[i].outline_value;
			desc[i].font_bg_color.bg_y				=	fsa->blits[i].bg_color_y;
			desc[i].font_bg_color.bg_cb				=	fsa->blits[i].bg_color_cb;
			desc[i].font_bg_color.bg_cr				=	fsa->blits[i].bg_color_cr;
			desc[i].font_wr_mode.wr_mode			=	fsa->blits[i].font_wr_mode;
			desc[i].font_color_tone.color_tone		=	fsa->blits[i].font_color_tone;
			desc[i].font_alpha.alpha				=	fsa->blits[i].font_alpha;
			desc[i].font_threshold.threshold		=	fsa->blits[i].font_threshold;
		} else if (active_mode == ENX_VDMA_MODE_VIDEO_CORE) {
			desc[i + fsa->dst_type_index].font_src						=	src[i].kind == ENX_BUF_ID ?
																			enx_vdma_buf_phys(src[i].vbuf) : src[i].paddr;
			desc[i + fsa->dst_type_index].font_width.width				=	fsa->blits[i].src_width;
			desc[i + fsa->dst_type_index].font_height.height			=	fsa->blits[i].src_height;
			desc[i + fsa->dst_type_index].font_yaddr2					=	fsa->blits[i].font_xpos;
			desc[i + fsa->dst_type_index].font_caddr2					=	fsa->blits[i].font_ypos;
			desc[i + fsa->dst_type_index].font_color.font_y				=	fsa->blits[i].font_color_y;
			desc[i + fsa->dst_type_index].font_color.font_cb			=	fsa->blits[i].font_color_cb;
			desc[i + fsa->dst_type_index].font_color.font_cr			=	fsa->blits[i].font_color_cr;
			desc[i + fsa->dst_type_index].font_color.font				=	fsa->blits[i].font_value;
			desc[i + fsa->dst_type_index].font_outline_color.outline_y	=	fsa->blits[i].outline_color_y;
			desc[i + fsa->dst_type_index].font_outline_color.outline_cb	=	fsa->blits[i].outline_color_cb;
			desc[i + fsa->dst_type_index].font_outline_color.outline_cr	=	fsa->blits[i].outline_color_cr;
			desc[i + fsa->dst_type_index].font_outline_color.outline	=	fsa->blits[i].outline_value;
			desc[i + fsa->dst_type_index].font_bg_color.bg_y			=	fsa->blits[i].bg_color_y;
			desc[i + fsa->dst_type_index].font_bg_color.bg_cb			=	fsa->blits[i].bg_color_cb;
			desc[i + fsa->dst_type_index].font_bg_color.bg_cr			=	fsa->blits[i].bg_color_cr;
			desc[i + fsa->dst_type_index].font_wr_mode.wr_mode			=	fsa->blits[i].font_wr_mode;
			desc[i + fsa->dst_type_index].font_color_tone.color_tone	=	fsa->blits[i].font_color_tone;
			desc[i + fsa->dst_type_index].font_alpha.alpha				=	fsa->blits[i].font_alpha;
			desc[i + fsa->dst_type_index].font_threshold.threshold		=	fsa->blits[i].font_threshold;

			if (i==0) {
				fsa->blits[0].reserved = (u32)fs->desc_paddr + (sizeof(struct enx_en683_font_desc) * fsa->dst_type_index);
			}
		}
	}

	if (active_mode == ENX_VDMA_MODE_KERNEL) {
		/* font_array cache flush */
		enx_en683_l2cache_flush64(fs->desc_paddr, sizeof(struct enx_en683_font_desc) * fsa->font_number);

		/* hw register setting */
		{
			/* Font cnt & desc addr */
			val = FIELD_PREP(FONT_NUM_MASK, fsa->font_number);
			enx_vdma_reg_wr(dev, EN683_FONT_REG_FONT_NUM, val);
			enx_vdma_reg_wr(dev, EN683_FONT_REG_DESC_ADDR, fs->desc_paddr);

			/* Horizontal size of the input image -> h_total */
			val = FIELD_PREP(LINE_STRIDE_MASK, fsa->dst_width);
			enx_vdma_reg_wr(dev, EN683_FONT_REG_LINE_STRIDE, val);

			/* Set hw busy */
			spin_lock_irqsave(&dev->irq_lock, flags);
			WRITE_ONCE(dev->hw_idle, false);
			spin_unlock_irqrestore(&dev->irq_lock, flags);

			/* Set IRQ enable */
			val = FIELD_PREP(IRQ_CTRL_IRQ_EN_BIT, 1);
			enx_vdma_reg_wr(dev, EN683_FONT_REG_IRQ_CTRL, val);

			/* Start font rendering	*/
			val = FIELD_PREP(CTRL0_GO_BIT, 1);
			printlog("Submit font rendering job to HW (GO : %u).\n", val);
			enx_vdma_reg_wr(dev, EN683_FONT_REG_CTRL0, val);

			printlog("Start font rendering.\n\n");
		}

		/* wait for IRQ */
		ret = wait_event_interruptible_timeout(dev->hw_wq, READ_ONCE(dev->hw_idle) == true, msecs_to_jiffies(1000));
		if (ret == 0) {
			errorlog("Rendering timed out.\n");
			spin_lock_irqsave(&dev->irq_lock, flags);
			fds->timeout_count++;
			spin_unlock_irqrestore(&dev->irq_lock, flags);
			ret = -ETIMEDOUT;
			enx_vdma_abort(dev);
		} else if (ret < 0) {
			errorlog("Rendering interrupted.\n");
			enx_vdma_abort(dev);
		} else {
			ret = 0; // success
		}
	}
out:
	/* Capture last-submit snapshot for debugfs (best-effort). */
	if (fds) {
		spin_lock_irqsave(&dev->irq_lock, flags);
		fds->last.valid			= 1;
		fds->last.mode			= active_mode;
		fds->last.font_number	= fsa->font_number;
		fds->last.dst_kind		= dst->kind;
		fds->last.dst_width		= fsa->dst_width;
		fds->last.dst_height	= fsa->dst_height;
		fds->last.dst_paddr		= (active_mode == ENX_VDMA_MODE_KERNEL) ? dst_yaddr : 0;
		fds->last.desc_paddr	= fs ? (u64)fs->desc_paddr : 0;
		fds->last.submitter_pid = vs->pid;
		fds->last.result		= ret;
		spin_unlock_irqrestore(&dev->irq_lock, flags);
	}
	exitlog();
	return ret;
}

static int en683a_osd_hw_run_once(struct enx_vdma_sess *vs,
				struct enx_vdma_job_buf *dst, struct enx_vdma_job_buf *src,
				u32 src_count, void *blit_p, u32 job_flags)
{
	return 0;
}

static int font_hw_run_once(struct enx_vdma_sess *vs,
				struct enx_vdma_job_buf *dst, struct enx_vdma_job_buf *src,
				u32 src_count, void *blit_p, u32 job_flags)
{
	struct enx_vdma_dev *dev = vs->dev;
	struct font_dev_state *fds = NULL;
	int ret = 0;

	enterlog();

	if (!dev->dev_priv) {
		errorlog("device private data is NULL.\n");
		ret = -EINVAL;
		goto out;
	}
	fds = dev->dev_priv;

	if (fds->chip_type == EN683_FONT_CHIP_TYPE_EN683) {
		ret = en683_font_hw_run_once(vs, dst, src, src_count, blit_p, job_flags);
	} else if (fds->chip_type == EN683_FONT_CHIP_TYPE_EN683A) {
		ret = en683a_osd_hw_run_once(vs, dst, src, src_count, blit_p, job_flags);
	} else {
		errorlog("Unknown chip type: %d\n", fds->chip_type);
		ret = -EINVAL;
	}

out:
	exitlog();
	return ret;
}

static int font_session_open(struct enx_vdma_sess *vs)
{
	struct enx_vdma_dev *dev = vs->dev;
	struct font_dev_state *fds = vs->dev->dev_priv;
	struct font_session *fs;
	int ret = 0;

	enterlog();

	/* Check duplicate session */
	spin_lock(&fds->lock);
	if(fds->active_mode == ENX_VDMA_MODE_VIDEO_CORE) {
		spin_unlock(&fds->lock);
		errorlog("vdma_font: firmware mode is exclusive; another session (pid=%d) already holds it — open rejected\n", fds->owner_pid);
		ret = -EBUSY;
		goto out;
	}
	fds->session_count++;
	spin_unlock(&fds->lock);

	/* font private data */
	fs = kzalloc(sizeof(*fs), GFP_KERNEL);
	if (!fs) {
		errorlog("Failed to allocate memory for font session.\n");
		spin_lock(&fds->lock);
		fds->session_count--;
		spin_unlock(&fds->lock);
		ret = -ENOMEM;
		goto out;
	}

	fs->font_desc = dma_alloc_coherent(dev->dev, sizeof(struct enx_en683_font_desc) * dev->max_src, &fs->desc_paddr, GFP_KERNEL);
	if (!fs->font_desc) {
		errorlog("Failed to allocate coherent memory for font descriptions.\n");
		spin_lock(&fds->lock);
		fds->session_count--;
		spin_unlock(&fds->lock);
		kfree(fs);
		ret = -ENOMEM;
		goto out;
	}

	vs->sess_priv = fs;
out:
	exitlog();
	return ret;
}

static void font_session_release(struct enx_vdma_sess *vs)
{
	struct enx_vdma_dev *dev = vs->dev;
	struct font_dev_state *fds = vs->dev->dev_priv;
	struct font_session *fs = vs->sess_priv;
	int prev_mode = ENX_VDMA_MODE_NONE;

	enterlog();

	spin_lock(&fds->lock);
	if (--fds->session_count == 0) {
		prev_mode = fds->active_mode;
		fds->active_mode = ENX_VDMA_MODE_NONE;	/* last close resets mode */
		fds->owner_pid = 0;
	}
	spin_unlock(&fds->lock);

	if (prev_mode == ENX_VDMA_MODE_KERNEL) {
		/* Due to the dependency between font and videocore control,
		 * we deliberately disable hw_idle (hw_idle = false) upon releasing
		 * all sessions, provided it was under kernel control. */
		disable_irq(dev->irq);
		WRITE_ONCE(dev->hw_idle, false);
	}

	dma_free_coherent(dev->dev, sizeof(struct enx_en683_font_desc) * dev->max_src, fs->font_desc, fs->desc_paddr);
	kfree(fs);
	exitlog();
}

static const struct enx_vdma_core_ops font_ops = {
	.hw_run_once	= font_hw_run_once,
	.hw_abort		= font_hw_abort,	// not support
	.session_open	= font_session_open,
	.session_release= font_session_release,
	.blit_size		= sizeof(struct enx_en683_font_blit),
	.version		= PKG_BUILD_VER,
};


/*******************************************************************************
 *
 * SUBMIT ioctl handler (function-specific UAPI struct → core helper)
 *
*******************************************************************************/

/* Legacy mode (Sync only) */
static long font_ioctl_submit(struct enx_vdma_sess *sess, void __user *uarg)
{
	struct enx_vdma_dev *dev = sess->dev;
	struct font_dev_state *fds = dev->dev_priv;
	struct enx_en683_font_submit_args hdr, *args;
	struct enx_vdma_submit_buf *src_bufs;
	struct enx_vdma_submit_params p = {0,};
	size_t blits_size;
	u32 ref_kind, src_size, dst_size, active_mode = READ_ONCE(fds->active_mode);
	int ret = 0, i;

	enterlog();

	if (copy_from_user(&hdr, uarg, sizeof(hdr))) {
		ret = -EFAULT;
		errorlog("failed(%d) to copy_from_user\n", ret);
		goto out;
	}

	/* Check active mode */
	if (active_mode == ENX_VDMA_MODE_NONE) {
		errorlog("Invalid operation for current mode (active_mode: ENX_VDMA_MODE_NONE)\n");
		ret = -EINVAL;
		goto out;
	}

	/* Check font number */
	if (hdr.font_number == 0 || hdr.font_number > dev->max_src) {
		errorlog("Invalid src_count. (max_cnt : %u, req_cnt : %u)\n", dev->max_src, hdr.font_number);
		ret = -EINVAL;
		goto out;
	}

	blits_size = struct_size(args, blits, hdr.font_number);
	args = memdup_user(uarg, blits_size);
	if (IS_ERR(args)) {
		ret = PTR_ERR(args);
		errorlog("failed(%d) to memdup_user\n", ret);
		goto out;
	}
	if (args->font_number != hdr.font_number) {
		errorlog("font_number mismatch: hdr=%u args=%u (TOCTOU?)\n", hdr.font_number, args->font_number);
		ret = -EINVAL;
		kfree(args);
		goto out;
	}

	src_bufs = kmalloc_array(args->font_number, sizeof(*src_bufs), GFP_KERNEL);
	if (!src_bufs) {
		ret = -ENOMEM;
		errorlog("failed(%d) to kmalloc_array(src_bufs)\n", ret);
		kfree(args);
		goto out;
	}

	/* setting source buffer info */
	for (i = 0; i < args->font_number; i++) {
		ref_kind = args->blits[i].src_addr_flag ? ENX_BUF_RAW : ENX_BUF_ID;
		src_size = args->blits[i].src_width * args->blits[i].src_height;
		if (src_size == 0) {
			errorlog("Source width(%d) or height(%d) is null for blit[%d].\n", args->blits[i].src_width, args->blits[i].src_height, i);
			ret = -EINVAL;
			kfree(src_bufs);
			kfree(args);
			goto out;
		}

		src_bufs[i].kind		= ref_kind;
		src_bufs[i].data_offset	= args->blits[i].src_data_offset;
		src_bufs[i].size		= src_size;
		if (ref_kind == ENX_BUF_ID) {
			src_bufs[i].id		= args->blits[i].src_addr;
		} else {
			src_bufs[i].paddr	= args->blits[i].src_addr;
		}
	}

	/* setting destination buffer info */
	ref_kind = active_mode == ENX_VDMA_MODE_VIDEO_CORE ? ENX_BUF_NONE : \
						args->dst_type_index ? ENX_BUF_RAW : ENX_BUF_ID;
	if (active_mode==ENX_VDMA_MODE_KERNEL) {
		dst_size = args->dst_width * args->dst_height * 3 >> 1;
		if (dst_size == 0) {
			errorlog("Destination width(%d) or height(%d) is null.\n", args->dst_width, args->dst_height);
			ret = -EINVAL;
			kfree(src_bufs);
			kfree(args);
			goto out;
		}
	} else {
		dst_size = 0; // Not used in video core mode, so set to 0.
	}

	{ // fill struct enx_vdma_submit_params
		p.dst.kind			= ref_kind;
		p.dst.data_offset	= args->dst_data_offset;
		p.dst.size			= dst_size;
		if (ref_kind == ENX_BUF_ID) {
			p.dst.id		= args->blits[0].dst_addr_y;
		} else {
			p.dst.paddr		= args->blits[0].dst_addr_y;
		}
		p.src				= src_bufs;
		p.src_count			= args->font_number;
		p.blits				= (void *)args;
		p.blit_size			= blits_size;
		p.flags				= ENX_SUBMIT_SYNC; // Sync mode only
		p.user_token		= 0; // Sync mode only
	}

	ret = enx_vdma_submit(sess, &p);
	if (ret) {
		errorlog("Failed enx_vdma_submit. ret=%d\n", ret);
	} else {
		if (active_mode == ENX_VDMA_MODE_VIDEO_CORE) {
			if (copy_to_user(uarg, args, blits_size)) {
				ret = -EFAULT;
				errorlog("failed(%d) to copy_to_user\n", ret);
			}
		}
	}

	kfree(src_bufs);
	kfree(args);
out:
	exitlog();
	return ret;
}

/*******************************************************************************
 *
 * ioctl control
 *
*******************************************************************************/

/*
 * This function must be called by user-space immediately after
 * font_session_open(). Standalone or out-of-order invocations
 * may lead to undefined behavior or hardware malfunctions.
 */
static long font_ioctl_mode_init(struct enx_vdma_sess *sess, void __user *uarg)
{
	struct enx_vdma_dev *dev = sess->dev;
	struct font_dev_state *fds = dev->dev_priv;
	u32 req_mode;
	int ret = 0;

	enterlog();

	if (copy_from_user(&req_mode, (void __user*)uarg, sizeof(u32))) {
		ret = -EFAULT;
		errorlog("failed(%d) to copy_from_user\n", ret);
		goto out;
	}

	switch (req_mode) {
	case ENX_VDMA_MODE_NONE:
		ret = -EINVAL;
		errorlog("Invalid request active mode.\n");
		break;
	case ENX_VDMA_MODE_KERNEL:
		spin_lock(&fds->lock);
		if (fds->active_mode == ENX_VDMA_MODE_NONE) {
			fds->active_mode = req_mode;
			spin_unlock(&fds->lock);
			enable_irq(dev->irq);
			WRITE_ONCE(dev->hw_idle, true);
			printlog("VDMA FONT IRQ enabled\n");
		} else {
			// active_mode is only kernel. (Error escape branch already in API)
			spin_unlock(&fds->lock);
		}
		break;
	case ENX_VDMA_MODE_VIDEO_CORE:
		spin_lock(&fds->lock);
		if (fds->session_count != 1 || fds->active_mode != ENX_VDMA_MODE_NONE) {
			ret = -EBUSY;
			errorlog("vdma_font: firmware mode is exclusive; another session already holds it — open rejected\n");
		} else {
			fds->active_mode = req_mode;
			fds->owner_pid = sess->pid;
		}
		spin_unlock(&fds->lock);
		break;
	default:
		ret = -EINVAL;
		errorlog("Invalid active mode (%u)\n", req_mode);
	}

out:
	exitlog();
	return ret;
}

static long font_ioctl_maxsrcs(struct enx_vdma_sess *sess, void __user *uarg)
{
	struct enx_vdma_dev *dev = sess->dev;
	u32 max_src = dev->max_src;
	int ret = 0;

	enterlog();

	if (copy_to_user(uarg, &max_src, sizeof(max_src))) {
		ret = -EFAULT;
		errorlog("failed(%d) to copy_to_user\n", ret);
	}

	exitlog();
	return ret;
}

static long enx_en683_font_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
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
	case VDMAIOGET_MAX_BUFS:	ret = enx_vdma_ioctl_maxbufs(sess, uarg); break;
	case VDMAIOSET_MOD_INIT:	ret = font_ioctl_mode_init(sess, uarg); break;
	/* Function-specific */
	case VDMAIOSET_FONT_SUBMIT:	ret = font_ioctl_submit(sess, uarg); break;
	case VDMAIOGET_MAX_SRCS:	ret = font_ioctl_maxsrcs(sess, uarg); break;

	default:
		ret = -ENOTTY;
	}

	exitlog();
	return ret;
}

static const struct file_operations font_fops = {
	.owner			= THIS_MODULE,
	.open			= enx_vdma_open,
	.release		= enx_vdma_release,
	.unlocked_ioctl	= enx_en683_font_ioctl,
	.compat_ioctl	= enx_en683_font_ioctl,
	.mmap			= enx_vdma_mmap,
	.poll			= enx_vdma_poll,
	.llseek			= noop_llseek,
};

/*******************************************************************************
 *
 * interrupt
 *
*******************************************************************************/

static irqreturn_t enx_en683_font_irq(int irq, void *dev_id)
{
	struct enx_vdma_dev *dev = dev_id;
	struct font_dev_state *fds = dev->dev_priv;
	irqreturn_t ret = IRQ_NONE;
	u32 status, val;

	enterlog();

	spin_lock(&dev->irq_lock);
	status = enx_vdma_reg_rd(dev, EN683_FONT_REG_IRQ_STATUS);
	if (FIELD_GET(IRQ_STATUS_IRQ_BIT, status)) {
		val = enx_vdma_reg_rd(dev, EN683_FONT_REG_IRQ_CTRL);
		FIELD_MODIFY(IRQ_CTRL_IRQ_CLR_BIT, &val, 1);
		enx_vdma_reg_wr(dev, EN683_FONT_REG_IRQ_CTRL, val);
		WRITE_ONCE(dev->hw_idle, true);

		/* debug counters */
		fds->irq_count++;
		fds->last_irq_status = status;

		wake_up_interruptible(&dev->hw_wq);
		ret = IRQ_HANDLED;
	}
	spin_unlock(&dev->irq_lock);

	exitlog();
	return ret;
}

/*******************************************************************************
 *
 * debugfs — Font device monitoring
 *
 * Layout (subtree under core's dev->dbg_dir; lifecycle tied to core_register):
 * - debugfs must be mounted beforehand.
 *	(e.g., mount -t debugfs none /sys/kernel/debug)
 *	/sys/kernel/debug/enx_vdma/<node_name>/
 *	- regs			— HW register snapshot (raw + decoded fields)
 *	- mode			— active_mode, session_count, owner_pid (firmware mode only)
 *	- irq_stats		— irq_count, last_irq_status, timeout_count
 *	- last_submit	— snapshot of the most recent hw_run_once invocation
 *
 * IRQ statistics and last_submit are protected by dev->irq_lock; readers use
 * spin_lock_irqsave().
 *
*******************************************************************************/

static const char *font_mode_str(int m)
{
	switch (m) {
	case ENX_VDMA_MODE_KERNEL:		return "KERNEL";
	case ENX_VDMA_MODE_VIDEO_CORE:	return "VIDEO_CORE";
	case ENX_VDMA_MODE_NONE:		return "NONE";
	default:						return "?";
	}
}

static int enx_en683_font_dbg_mode_show(struct seq_file *s, void *v)
{
	struct font_dev_state *fds = s->private;
	int mode, count;
	pid_t owner;

	spin_lock(&fds->lock);
	mode  = fds->active_mode;
	count = fds->session_count;
	owner = fds->owner_pid;
	spin_unlock(&fds->lock);

	seq_printf(s, "active_mode:   %s\n", font_mode_str(mode));
	seq_printf(s, "session_count: %d\n", count);
	seq_printf(s, "owner_pid:     %d %s\n",
			owner, mode == ENX_VDMA_MODE_VIDEO_CORE ? "" : "(unused)");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(enx_en683_font_dbg_mode);

static int enx_en683_font_dbg_irq_stats_show(struct seq_file *s, void *v)
{
	/* private is the parent enx_vdma_dev so we can use dev->irq_lock. */
	struct enx_vdma_dev *dev = s->private;
	struct font_dev_state *fds = dev->dev_priv;
	u64 irq_count, timeout_count;
	u32 last_status;
	unsigned long flags;

	spin_lock_irqsave(&dev->irq_lock, flags);
	irq_count		= fds->irq_count;
	last_status		= fds->last_irq_status;
	timeout_count	= fds->timeout_count;
	spin_unlock_irqrestore(&dev->irq_lock, flags);

	seq_printf(s, "irq_count:     %llu\n", irq_count);
	seq_printf(s, "last_status:   0x%08x\n", last_status);
	seq_printf(s, "timeout_count: %llu\n", timeout_count);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(enx_en683_font_dbg_irq_stats);

static const char *font_kind_str(u32 k)
{
	switch (k) {
	case ENX_BUF_ID:	return "ID";
	case ENX_BUF_RAW:	return "RAW";
	case ENX_BUF_NONE:	return "NONE";
	default:			return "?";
	}
}

static int enx_en683_font_dbg_last_submit_show(struct seq_file *s, void *v)
{
	struct enx_vdma_dev *dev = s->private;
	struct font_dev_state *fds = dev->dev_priv;
	struct font_last_submit snap;
	unsigned long flags;

	spin_lock_irqsave(&dev->irq_lock, flags);
	snap = fds->last;
	spin_unlock_irqrestore(&dev->irq_lock, flags);

	if (!snap.valid) {
		seq_puts(s, "(no submit yet)\n");
		return 0;
	}

	seq_printf(s, "mode:          %s\n", font_mode_str(snap.mode));
	seq_printf(s, "font_number:   %u\n", snap.font_number);
	seq_printf(s, "dst_kind:      %s\n", font_kind_str(snap.dst_kind));
	seq_printf(s, "dst_w x dst_h: %u x %u\n", snap.dst_width, snap.dst_height);
	seq_printf(s, "dst_paddr:     0x%llx\n", snap.dst_paddr);
	seq_printf(s, "desc_paddr:    0x%llx\n", snap.desc_paddr);
	seq_printf(s, "submitter_pid: %d\n", snap.submitter_pid);
	seq_printf(s, "result:        %s\n", snap.result  ? errname(snap.result) : "SUCCESS");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(enx_en683_font_dbg_last_submit);

static int enx_en683_font_dbg_regs_show(struct seq_file *s, void *v)
{
	struct enx_vdma_dev *dev = s->private;
	u32 base_addr = dev->resource->start;
	u32 val;

	seq_printf(s, "Eyenix EN683 VDMA FONT Register Map\n");
	val = enx_vdma_reg_rd(dev, EN683_FONT_REG_CTRL0);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_FONT_REG_CTRL0, val);
	seq_printf(s, "%-20s: %lu\n", "FONT_GO", FIELD_GET(CTRL0_GO_BIT, val));
	val = enx_vdma_reg_rd(dev, EN683_FONT_REG_FONT_NUM);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_FONT_REG_FONT_NUM, val);
	seq_printf(s, "%-20s: %lu\n", "FONT_NUM", FIELD_GET(FONT_NUM_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_FONT_REG_DESC_ADDR);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_FONT_REG_DESC_ADDR, val);
	seq_printf(s, "%-20s: %#.08X\n", "FONT_DESC_ADDR", val);
	val = enx_vdma_reg_rd(dev, EN683_FONT_REG_LINE_STRIDE);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_FONT_REG_LINE_STRIDE, val);
	seq_printf(s, "%-20s: %lu\n", "FONT_LINE_STRIDE", FIELD_GET(LINE_STRIDE_MASK, val));
	val = enx_vdma_reg_rd(dev, EN683_FONT_REG_IRQ_CTRL);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_FONT_REG_IRQ_CTRL, val);
	seq_printf(s, "%-20s: %lu\n", "FONT_IRQ_CLR_T", FIELD_GET(IRQ_CTRL_IRQ_CLR_T_BIT, val));
	seq_printf(s, "%-20s: %lu\n", "FONT_IRQ_EN_T", FIELD_GET(IRQ_CTRL_IRQ_EN_T_BIT, val));
	seq_printf(s, "%-20s: %lu\n", "FONT_IRQ_CLR", FIELD_GET(IRQ_CTRL_IRQ_CLR_BIT, val));
	seq_printf(s, "%-20s: %lu\n", "FONT_IRQ_EN", FIELD_GET(IRQ_CTRL_IRQ_EN_BIT, val));
	val = enx_vdma_reg_rd(dev, EN683_FONT_REG_IRQ_STATUS);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_FONT_REG_IRQ_STATUS, val);
	seq_printf(s, "%-20s: %lu\n", "FONT_IRQ_T", FIELD_GET(IRQ_STATUS_IRQ_T_BIT, val));
	seq_printf(s, "%-20s: %lu\n", "FONT_IRQ", FIELD_GET(IRQ_STATUS_IRQ_BIT, val));
	val = enx_vdma_reg_rd(dev, EN683_FONT_REG_AXI_CTRL);
	seq_printf(s, "----------%#.08X: %#.08X\n", base_addr + EN683_FONT_REG_AXI_CTRL, val);
	seq_printf(s, "%-20s: %lu\n", "FONT_ARLEN", FIELD_GET(AXI_CTRL_ARLEN_MASK, val));
	seq_printf(s, "%-20s: %lu\n", "FONT_AWLEN", FIELD_GET(AXI_CTRL_AWLEN_MASK, val));
	seq_printf(s, "%-20s: %lu\n", "FONT_ARQOS", FIELD_GET(AXI_CTRL_ARQOS_MASK, val));
	seq_printf(s, "%-20s: %lu\n", "FONT_AWQOS", FIELD_GET(AXI_CTRL_AWQOS_MASK, val));
	seq_printf(s, "--------------------------------\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(enx_en683_font_dbg_regs);

static void enx_en683_font_dbg_init(struct enx_vdma_dev *dev)
{
	struct font_dev_state *fds = dev->dev_priv;

	if (!dev->dbg_dir)
		return;
	debugfs_create_file("regs",			0444, dev->dbg_dir, dev,
						&enx_en683_font_dbg_regs_fops);
	debugfs_create_file("mode",			0444, dev->dbg_dir, fds,
						&enx_en683_font_dbg_mode_fops);
	debugfs_create_file("irq_stats",	0444, dev->dbg_dir, dev,
						&enx_en683_font_dbg_irq_stats_fops);
	debugfs_create_file("last_submit",	0444, dev->dbg_dir, dev,
						&enx_en683_font_dbg_last_submit_fops);
}

/*******************************************************************************
 *
 * probe / remove
 *
*******************************************************************************/

static int enx_en683_font_probe(struct platform_device *pdev)
{
	struct enx_vdma_dev *dev;
	struct font_dev_state *fds;
	int ret = 0;

	enterlog();
	printlog("Start probe(%s)\n", dev_name(&pdev->dev));

	/* check max_bufs & max_src */
	if (!max_src || !max_bufs) {
		errorlog("Invalid max_src(%u) or max_bufs (%u)\n", max_src, max_bufs);
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
	fds = devm_kzalloc(&pdev->dev, sizeof(*fds), GFP_KERNEL);
	if (!fds) {
		ret = -ENOMEM;
		errorlog("failed(%d) create device state.\n", ret);
		goto err_free_core;
	}
	spin_lock_init(&fds->lock);
	ret = of_property_read_u32(pdev->dev.of_node, "enx,chip_type", &fds->chip_type);
	if (ret < 0) {
		fds->chip_type = EN683_FONT_CHIP_TYPE_EN683;
	}
	fds->active_mode = ENX_VDMA_MODE_NONE;
	dev->dev_priv = fds;

	/* Get resource */
	dev->resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (IS_ERR(dev->resource)) {
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
	printlog("%s base[%px], real[0x%x]\n", DEVICE_NAME, dev->regs, (unsigned int)dev->resource->start);

	dev->max_bufs = max_bufs;
	dev->max_src = max_src;

	ret = platform_get_irq(pdev, 0);
	if (ret <= 0) {
		errorlog("failed(%d) to platform_get_irq\n", ret);
		goto err_free_core;
	}
	dev->irq = ret;

	ret = request_irq(dev->irq, enx_en683_font_irq, 0, FONT_DEV_NAME, dev);
	if (ret) {
		errorlog("failed(%d) to request irq for %s\n", ret, FONT_DEV_NAME);
		goto err_free_core;
	}

	ret = enx_vdma_core_register(dev, &font_ops, &font_fops, FONT_DEV_NAME);
	if (ret) {
		errorlog("failed(%d) to register %s core\n", ret, FONT_DEV_NAME);
		goto err_free_irq;
	}

	/* driver-side debugfs files under core's dbg_dir (lifetime tied to core) */
	enx_en683_font_dbg_init(dev);

	platform_set_drvdata(pdev, dev);

	/*
	 * Initially set hw_idle to false to prevent any job submissions
	 * before the probe completes. Once all configurations are done,
	 * the IRQ is enabled according to the mode requested by VDMAIOSET_MOD_INIT.
	 */
	disable_irq(dev->irq);
	WRITE_ONCE(dev->hw_idle, false);

	/* Set version info for enxlog */
	SetModuleVersion_t func_ptr = (SetModuleVersion_t)symbol_get(SetModuleVersion);
	if (func_ptr) {
		func_ptr(VER_VFONT, PKG_BUILD_VER);
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

static void enx_en683_font_remove(struct platform_device *pdev)
{
	struct enx_vdma_dev *dev = platform_get_drvdata(pdev);
	u32 val;

	/* Unregister the core */
	enx_vdma_core_unregister(dev);

	/* Disable IRQ */
	val = enx_vdma_reg_rd(dev, EN683_FONT_REG_IRQ_CTRL);
	FIELD_MODIFY(IRQ_CTRL_IRQ_EN_BIT, &val, 0);
	enx_vdma_reg_wr(dev, EN683_FONT_REG_IRQ_CTRL, val);
	free_irq(dev->irq, dev);

	/* Device free */
	enx_vdma_core_free(dev);
}

static const struct of_device_id enx_en683_font_of_match[] = {
	{ .compatible = "eyenix,en683-dma-font" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, enx_en683_font_of_match);

static struct platform_driver enx_en683_font_driver = {
	.probe  = enx_en683_font_probe,
	.remove = enx_en683_font_remove,
	.driver = {
		.name		= DEVICE_NAME,
		.of_match_table	= enx_en683_font_of_match,
	},
};
module_platform_driver(enx_en683_font_driver);

MODULE_DESCRIPTION("EYENIX EN683 VDMA Font driver");
MODULE_AUTHOR("YongJae Lee <yjlee@eyenix.com>");
MODULE_LICENSE("GPL v2");
