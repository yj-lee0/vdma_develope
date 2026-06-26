// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Eyenix Corporation
 * Eyenix <support6@eyenix.com>
 *
 * Eyenix EN683 Video-DMA JPEG Encoder internal header
 *
 */

#ifndef _EN683_JPEG_ENC_H_
#define _EN683_JPEG_ENC_H_

/*******************************************************************************
 *
 * register define
 *
*******************************************************************************/

#define EN683_JENC_REG_AXI_CTRL					0x0000
#define		AXI_CTRL_AWMOR_BIT					BIT(11)			// rw
#define		AXI_CTRL_AWLEN_MASK					GENMASK(10, 9)	// rw
#define		AXI_CTRL_ARMOR_MASK					GENMASK(8, 6)	// rw
#define		AXI_CTRL_ARLEN_MASK					GENMASK(5, 4)	// rw
#define		AXI_CTRL_AXI_EDN_BIT				BIT(3)			// rw
#define EN683_JENC_REG_SRC_RD_SEL				0x0004
#define		SRC_RD_SEL_YC420_BIT				BIT(24)			// rw
#define		SRC_RD_SEL_YC422_MASK				GENMASK(17, 16)	// rw
#define		SRC_RD_SEL_YC444_MASK				GENMASK(10, 8)	// rw
#define EN683_JENC_REG_SRC_IMG_FMT				0x0008
#define		SRC_FMT_MASK						GENMASK(1, 0)	// rw
#define EN683_JENC_REG_SRC_YADDR				0x000C
#define EN683_JENC_REG_SRC_CADDR				0x0010
#define EN683_JENC_REG_SRC_RESOLUTION			0x0014
#define		SRC_RESOLUTION_VERT_MASK			GENMASK(31, 16)	// rw
#define		SRC_RESOLUTION_HORZ_MASK			GENMASK(15, 0)	// rw
#define EN683_JENC_REG_SRC_CROP_CTRL0			0x0018
#define 	CROP_CTRL0_VERT_MASK				GENMASK(31, 16)	// rw
#define 	CROP_CTRL0_HORZ_MASK				GENMASK(15, 0)	// rw
#define EN683_JENC_REG_SRC_CROP_CTRL1			0x001C
#define 	CROP_CTRL1_COORD_X_MASK				GENMASK(29, 16)	// rw
#define 	CROP_CTRL1_COORD_Y_MASK				GENMASK(15, 0)	// rw
#define EN683_JENC_REG_DST_ADDR					0x0020
#define EN683_JENC_REG_OPT_CTRL					0x0024
#define 	OPT_CTRL_SOI_JFIF_WR_BIT			BIT(31)			// rw
#define 	OPT_CTRL_OVF_AUTO_STOP_BIT			BIT(30)			// rw
#define 	OPT_CTRL_OVF_THRESHOLD_MASK			GENMASK(29, 0)	// rw
#define EN683_JENC_REG_QTBL_CTRL				0x0028
#define 	QTBL_CTRL_DC_C_BIT					GENMASK(31, 24)	// rw
#define 	QTBL_CTRL_DC_Y_BIT					GENMASK(23, 16)	// rw
#define 	QTBL_CTRL_DC_BYPASS_BIT				BIT(2)			// rw
#define 	QTBL_CTRL_BYPASS_BIT				BIT(1)			// rw
#define 	QTBL_CTRL_SELECT_BIT				BIT(0)			// rw
#define EN683_JENC_REG_QUALITY					0x002C
#define 	QUALITY_MASK						GENMASK(6, 0)	// rw
#define EN683_JENC_REG_CTRL						0x0030
#define 	CTRL_DISCARD_BIT					BIT(1)			// rw
#define 	CTRL_GO_BIT							BIT(0)			// rw
#define EN683_JENC_REG_IRQ_CTRL					0x0034
#define 	IRQ_CTRL_IRQ_CLR_MASK				GENMASK(9, 8)	// w,sc
#define 	IRQ_CTRL_IRQ_EN_MASK				GENMASK(1, 0)	// rw
#define EN683_JENC_REG_IRQ_STATUS				0x0038
#define 	IRQ_STATUS_OVERRUN_IRQ_BIT			BIT(1)			// r
#define 	IRQ_STATUS_DONE_IRQ_BIT				BIT(0)			// r
#define EN683_JENC_REG_ENC_CYCLE_CNT			0x0040			// r
#define EN683_JENC_REG_ENC_FILE_SIZE			0x0044			// r
#define EN683_JENC_REG_ENC_RESPONSE				0x0048			// r
#define		ENC_RESPONSE_ERR_FILE_OVF_BIT		BIT(1)			// r
#define		ENC_RESPONSE_ERR_OCCUR_BIT			BIT(0)			// r

/*******************************************************************************
 *
 * define
 *
*******************************************************************************/

enum {
	eSRC_YUV444 = 0,
	eSRC_YUV422,
	eSRC_YUV420,
	eSRC_YUV400
};

/**
 * struct jenc_last_submit - snapshot of the most recent hw_run_once invocation
 * @valid: Set to 1 after the first successful capture.
 * @src_yaddr: Source y address.
 * @src_caddr: Source c address.
 * @dst_yaddr: Destination y address.
 * @src_format: JPEG encoding image format.
 * @src_width_total: Total source width.
 * @src_height_total: Total source height.
 * @src_width: Source width (crop).
 * @src_height: Source height (crop).
 * @src_width_pos: Source width position (crop).
 * @src_height_pos: Source height position (crop).
 * @ovf_threshold: Overflow threshold value.
 * @quality: JPEG encoding quality factor.
 * @submitter_pid: PID of the session that issued the submit.
 * @result: Last return value of hw_run_once (0 = success).
 * @cycle: Cycle count of the last encoding operation.
 * @jpeg_size: Size of the generated JPEG file in bytes.
 * @process_error: Bitfield of error status from the last encoding operation.
 *
 * Captured under @irq_lock by hw_run_once for debug visibility.
 */
struct jenc_last_submit {
	u32   valid;
	u32   src_yaddr;
	u32   src_caddr;
	u32   dst_yaddr;
	u32   src_format;
	u32   src_width_total;
	u32   src_height_total;
	u32   src_width;
	u32   src_height;
	u32   src_width_pos;
	u32   src_height_pos;
	u32   ovf_threshold;
	u32   quality;
	pid_t submitter_pid;
	int   result;
	u32   cycle;
	u32   jpeg_size;
	union {
		u32 value;
		struct {
			u32 err_occur:1;
			u32 err_file_ovf:1;
			u32 __rev0:30;
		};
	} process_error;
};

/**
 * struct jenc_dev_state - Per-device state for the EN683 VDMA JPEG Encoder controller
 * @irq_count: Total number of hardware IRQs serviced. Incremented inside
 *     the IRQ handler under dev->irq_lock.
 * @done_irq_count: Total number of "done" IRQs serviced. Incremented inside
 *     the IRQ handler under dev->irq_lock.
 * @err_irq_count: Total number of "error" IRQs serviced. Incremented inside
 *     the IRQ handler under dev->irq_lock.
 * @last_irq_status: Snapshot of EN683_JENC_REG_IRQ_STATUS at the last IRQ.
 *     Updated alongside @irq_count for debug visibility.
 * @timeout_count: Number of hw_run_once() invocations that aborted with
 *     -ETIMEDOUT while waiting for the completion IRQ.
 * @last: Snapshot of the most recent hw_run_once() invocation, captured at
 *     function exit (success or failure path).
 *
 * IRQ statistics (@irq_count, @last_irq_status, @timeout_count) and the
 * @last snapshot are protected by dev->irq_lock; ISR uses spin_lock(),
 * process context (e.g., debugfs readers) uses spin_lock_irqsave().
 * They participate in monitoring only and must not be used for
 * synchronization decisions.
 */
struct jenc_dev_state {
	/* IRQ statistics + last-submit snapshot (protected by dev->irq_lock) */
	u64 irq_count;
	u64 done_irq_count;
	u64 err_irq_count;
	u32 last_irq_status;
	u64 timeout_count;
	struct jenc_last_submit last;
};

#endif /* _EN683_JPEG_ENC_H_ */