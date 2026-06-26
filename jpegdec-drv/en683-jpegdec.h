// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Eyenix Corporation
 * Eyenix <support6@eyenix.com>
 *
 * Eyenix EN683 Video-DMA JPEG Decoder internal header
 *
 */

#ifndef _EN683_JPEG_DEC_H_
#define _EN683_JPEG_DEC_H_

/*******************************************************************************
 *
 * register define
 *
*******************************************************************************/

#define EN683_JDEC_REG_AXI_CTRL					0x0000
#define		AXI_CTRL_AWMOR_BIT					BIT(10)			// rw
#define		AXI_CTRL_AWLEN_MASK					GENMASK(9, 8)	// rw
#define		AXI_CTRL_ARMOR_MASK					GENMASK(7, 6)	// rw
#define		AXI_CTRL_ARLEN_MASK					GENMASK(5, 4)	// rw
#define		AXI_CTRL_AXI_EDN_BIT				BIT(3)			// rw
#define EN683_JDEC_REG_MAX_RES					0x0004
#define		MAX_RES_VERT_MASK					GENMASK(31, 16)	// rw
#define		MAX_RES_HORZ_MASK					GENMASK(15, 0)	// rw
#define EN683_JDEC_REG_DST_WR_SEL				0x0008
#define		DST_WR_SEL_YC420_BIT				GENMASK(25, 24)	// rw
#define		DST_WR_SEL_YC422_MASK				GENMASK(18, 16)	// rw
#define		DST_WR_SEL_YC444_MASK				GENMASK(11, 8)	// rw
#define EN683_JDEC_REG_CYCLE_THRESHOLD			0x000C
#define EN683_JDEC_REG_SRC_ADDR					0x0010
#define EN683_JDEC_REG_SRC_SIZE					0x0014
#define EN683_JDEC_REG_DST_ADDR					0x0018
#define EN683_JDEC_REG_CTRL						0x001C
#define 	CTRL_DISCARD_BIT					BIT(1)			// rw
#define 	CTRL_GO_BIT							BIT(0)			// rw
#define EN683_JDEC_REG_IRQ_CTRL					0x0020
#define 	IRQ_CTRL_IRQ_CLR_MASK				GENMASK(13, 8)	// w,sc
#define 	IRQ_CTRL_IRQ_EN_MASK				GENMASK(5, 0)	// rw
#define EN683_JDEC_REG_IRQ_STATUS				0x0024
#define 	IRQ_STATUS_DONE_BIT					BIT(0)			// r
#define 	IRQ_STATUS_ERR_OVERRUN_BIT			BIT(1)			// r
#define 	IRQ_STATUS_ERR_RD_CTRL_BIT			BIT(2)			// r
#define 	IRQ_STATUS_ERR_HEADER_BIT			BIT(3)			// r
#define 	IRQ_STATUS_ERR_ECS_BIT				BIT(4)			// r
#define 	IRQ_STATUS_ERR_VLD_BIT				BIT(5)			// r
#define EN683_JDEC_REG_IMG_RESOLUTION			0x0028
#define		IMG_RESOLUTION_VERT_MASK			GENMASK(31, 16)	// r
#define		IMG_RESOLUTION_HORZ_MASK			GENMASK(15, 0)	// r
#define EN683_JDEC_REG_IMG_RESOLUTION_WR		0x002C
#define		IMG_RESOLUTION_WR_VERT_MASK			GENMASK(31, 16)	// r
#define		IMG_RESOLUTION_WR_HORZ_MASK			GENMASK(15, 0)	// r
#define EN683_JDEC_REG_IMG_FORMAT				0x0030
#define		IMG_FORMAT_MASK						GENMASK(1, 0)	// r
#define EN683_JDEC_REG_DST_CADDR				0x0034 			// r
#define EN683_JDEC_REG_DEC_CYCLE				0x0038 			// r
#define EN683_JDEC_REG_HDR_ERROR				0x0040 			// r
#define		HDR_ERROR_UNSUP_BIT					BIT(15)			// r
#define		HDR_ERROR_COM_BIT					BIT(14)			// r
#define		HDR_ERROR_DQT_MASK					GENMASK(13, 12)	// r
#define		HDR_ERROR_DHT_MASK					GENMASK(11, 8)	// r
#define		HDR_ERROR_APP_CNT_MASK				GENMASK(7, 4)	// r
#define		HDR_ERROR_DRI_BIT					BIT(3)			// r
#define		HDR_ERROR_SOS_BIT					BIT(2)			// r
#define		HDR_ERROR_SOF0_BIT					BIT(1)			// r
#define		HDR_ERROR_SOI_BIT					BIT(0)			// r
#define EN683_JDEC_REG_APP_OFF0					0x0044 			// r
#define EN683_JDEC_REG_APP_OFF1					0x0048 			// r
#define EN683_JDEC_REG_APP_OFF2					0x004C 			// r
#define EN683_JDEC_REG_APP_OFF3					0x0050 			// r
#define EN683_JDEC_REG_APP_OFF4					0x0054 			// r
#define EN683_JDEC_REG_APP_OFF5					0x0058 			// r
#define EN683_JDEC_REG_APP_OFF6					0x005C 			// r
#define EN683_JDEC_REG_APP_OFF7					0x0060 			// r
#define EN683_JDEC_REG_COM_OFF					0x0064 			// r
#define EN683_JDEC_REG_UNSUP_INFO				0x0068 			// r
#define		UNSUP_INFO_MRK_MASK					GENMASK(31, 24)	// r
#define		UNSUP_INFO_OFF_MASK					GENMASK(23, 0)	// r
#define EN683_JDEC_REG_DRI_RST_IVL				0x006C 			// r
#define EN683_JDEC_REG_ERR_STATUS0				0x0078 			// r
#define		ERR_STATUS0_SOS_MASK				GENMASK(21, 15)	// r
#define		ERR_STATUS0_SOF0_MASK				GENMASK(14, 8)	// r
#define		ERR_STATUS0_DHT_MASK				GENMASK(7, 4)	// r
#define		ERR_STATUS0_DQT_MASK				GENMASK(3, 1)	// r
#define		ERR_STATUS0_DRI_BIT					BIT(0)			// r
#define EN683_JDEC_REG_ERR_STATUS1				0x007C 			// r
#define		ERR_STATUS1_VLD_MASK				GENMASK(9, 8)	// r
#define		ERR_STATUS1_ECS_PROC_MASK			GENMASK(7, 6)	// r
#define		ERR_STATUS1_HDR_PROC_MASK			GENMASK(5, 3)	// r
#define		ERR_STATUS1_RCTL_BIT				BIT(2)			// r
#define		ERR_STATUS1_CYCLE_BIT				BIT(1)			// r
#define		ERR_STATUS1_OCCUR_BIT				BIT(0)			// r

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
 * struct jdec_last_submit - snapshot of the most recent hw_run_once invocation
 * @valid: Set to 1 after the first successful capture.
 * @src_yaddr: Source y address.
 * @dst_yaddr: Destination y address.
 * @src_jpeg_size: Size of the source JPEG file.
 * @submitter_pid: PID of the session that issued the submit.
 * @result: Last return value of hw_run_once (0 = success).
 * @cycle: Decoder cycle count from the last submission.
 * @dst_size: Size of the decoded output from the last submission.
 * @hsize: Horizontal size of the decoded output from the last submission.
 * @vsize: Vertical size of the decoded output from the last submission.
 * @hsize_wr: Horizontal size of the decoded output written to memory from
 * 	   the last submission (may differ from @hsize when cropping is involved).
 * @vsize_wr: Vertical size of the decoded output written to memory from
 * 	   the last submission (may differ from @vsize when cropping is involved).
 * @fmt: Image format of the decoded output from the last submission.
 * @header_exist: Bitmap of JPEG header segments found in the source file of
 * 	   the last submission (captured on failure for debug purposes).
 * @unsupport_marker: Bitmap of unsupported JPEG markers found in the source
 * 	   file of the last submission, including the marker type and its offset
 * 	   (captured on failure for debug purposes).
 * @header_error: Bitmap of errors related to JPEG header parsing in the
 * 	   last submission (captured on failure for debug purposes).
 * @process_error: Bitmap of errors related to JPEG decoding process in the
 * 	   last submission (captured on failure for debug purposes).
 *
 * Captured under @irq_lock by hw_run_once for debug visibility.
 */
struct jdec_last_submit {
	u32   valid;
	u32   src_yaddr;
	u32   dst_yaddr;
	u32   src_jpeg_size;
	pid_t submitter_pid;
	int   result;
	u32 cycle;
	u32 dst_size;
	u16 hsize;
	u16 vsize;
	u16 hsize_wr;
	u16 vsize_wr;
	u32 fmt;
	/* Additional fields captured on fail (result != 0) */
	union {
		u32 value;
		struct {
			u32 soi_exist:1;
			u32 sof0_exist:1;
			u32 sos_exist:1;
			u32 dri_exist:1;
			u32 appn_cnt:4;
			u32 dht_exist:4;
			u32 dqt_exist:2;
			u32 com_exist:1;
			u32 unsup_exist:1;
			u32 __rev0:16;
		};
	} header_exist;
	union {
		u32 value;
		struct {
			u32 unsup_off:24;
			u32 unsup_mrk:8;
		};
	} unsupport_marker;
	union {
		u32 value;
		struct {
			u32 dri_err:1;
			u32 dqt_err:3;
			u32 dht_err:4;
			u32 sof0_err:7;
			u32 sos_err:7;
			u32 __rev0:10;
		};
	} header_error;
	union {
		u32 value;
		struct {
			u32 err_occur:1;
			u32 err_cycle:1;
			u32 err_rctl:1;
			u32 err_header_proc:3;
			u32 err_ecs_proc:2;
			u32 err_vld:2;
			u32 __rev0:22;
		};
	} process_error;
};

/**
 * struct jdec_dev_state - Per-device state for the EN683 VDMA JPEG Decoder controller
 * @irq_count: Total number of hardware IRQs serviced. Incremented inside
 *     the IRQ handler under dev->irq_lock.
 * @done_irq_count: Total number of "done" IRQs serviced. Incremented inside
 *     the IRQ handler under dev->irq_lock.
 * @err_overrun_irq_count: Total number of "error: overrun" IRQs
 *     serviced. Incremented inside the IRQ handler under dev->irq_lock.
 * @err_rd_ctrl_irq_count: Total number of "error: read control" IRQs
 *     serviced. Incremented inside the IRQ handler under dev->irq_lock.
 * @err_header_irq_count: Total number of "error: header" IRQs
 *     serviced. Incremented inside the IRQ handler under dev->irq_lock.
 * @err_ecs_irq_count: Total number of "error: ECS" IRQs
 *     serviced. Incremented inside the IRQ handler under dev->irq_lock.
 * @err_vld_irq_count: Total number of "error: VLD" IRQs
 *     serviced. Incremented inside the IRQ handler under dev->irq_lock.
 * @last_irq_status: Snapshot of EN683_JDEC_REG_IRQ_STATUS at the last IRQ.
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
struct jdec_dev_state {
	/* IRQ statistics + last-submit snapshot (protected by dev->irq_lock) */
	u64 irq_count;
	u64 done_irq_count;
	u64 err_overrun_irq_count;
	u64 err_rd_ctrl_irq_count;
	u64 err_header_irq_count;
	u64 err_ecs_irq_count;
	u64 err_vld_irq_count;
	u32 last_irq_status;
	u64 timeout_count;
	struct jdec_last_submit last;
};

#endif /* _EN683_JPEG_DEC_H_ */