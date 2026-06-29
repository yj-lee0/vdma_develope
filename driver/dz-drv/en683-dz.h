// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Eyenix Corporation
 * Eyenix <support6@eyenix.com>
 *
 * Eyenix EN683 Video-DMA DZ internal header
 *
 */

#ifndef _EN683_DZ_H_
#define _EN683_DZ_H_

/*******************************************************************************
 *
 * register define
 *
*******************************************************************************/

#define EN683_DZ_REG_YRADR						0x0000
#define EN683_DZ_REG_CRADR						0x0004
#define EN683_DZ_REG_YWADR						0x0008
#define EN683_DZ_REG_CWADR						0x000C
#define EN683_DZ_REG_OFFSET						0x0010
#define		OFFSET_CTRL_SVSP_MASK				GENMASK(28, 16)	// rw
#define		OFFSET_CTRL_SHSP_MASK				GENMASK(12, 0)	// rw
#define EN683_DZ_REG_RESOLUTION0				0x0014
#define		RESOLUTION0_SHLEN_MASK				GENMASK(28, 16)	// rw
#define		RESOLUTION0_SHLEN_TOT_MASK			GENMASK(12, 0)	// rw
#define EN683_DZ_REG_RESOLUTION1				0x0018
#define		RESOLUTION1_DHLEN_MASK				GENMASK(28, 16)	// rw
#define		RESOLUTION1_DHLEN_TOT_MASK			GENMASK(12, 0)	// rw
#define EN683_DZ_REG_RESOLUTION2				0x001C
#define		RESOLUTION2_DVLEN_MASK				GENMASK(28, 16)	// rw
#define		RESOLUTION2_SVLEN_MASK				GENMASK(12, 0)	// rw
#define EN683_DZ_REG_CTRL						0x0020
#define		CTRL_VFLIP_BIT						BIT(19)			// rw
#define		CTRL_HFLIP_BIT						BIT(18)			// rw
#define		CTRL_CSUB_SEL_BIT					BIT(17)			// rw
#define		CTRL_YC2_444_CINV_BIT				BIT(16)			// rw
#define		CTRL_WSEL_MASK						GENMASK(15, 12)	// rw
#define		CTRL_RSEL_MASK						GENMASK(11, 8)	// rw
#define		CTRL_WMOD_MASK						GENMASK(7, 6)	// rw
#define		CTRL_RMOD_MASK						GENMASK(5, 4)	// rw
#define		CTRL_GO_BIT							BIT(0)			// rw
#define EN683_DZ_REG_AXI_CTRL					0x0024
#define		AXI_CTRL_AWMOR_BIT					BIT(22)			// rw
#define		AXI_CTRL_AWLEN_MASK					GENMASK(21, 20)	// rw
#define		AXI_CTRL_ARMOR_MASK					GENMASK(19, 18)	// rw
#define		AXI_CTRL_ARLEN_MASK					GENMASK(17, 16)	// rw
#define		AXI_CTRL_AXI_EDN_BIT				BIT(8)			// rw
#define		AXI_CTRL_ARQOS_MASK					GENMASK(7, 4)	// rw
#define		AXI_CTRL_AWQOS_MASK					GENMASK(3, 0)	// rw
#define EN683_DZ_REG_IRQ_CTRL					0x0028
#define		IRQ_CTRL_IRQ_CLR_T_BIT				BIT(9)			// w,sc
#define		IRQ_CTRL_IRQ_EN_T_BIT				BIT(8)			// rw
#define		IRQ_CTRL_IRQ_CLR_BIT				BIT(1)			// w,sc
#define		IRQ_CTRL_IRQ_EN_BIT					BIT(0)			// rw
#define EN683_DZ_REG_IRQ_STATUS					0x002C
#define		IRQ_STATUS_IRQ_BIT					BIT(0)			// r
#define EN683_DZ_REG_IRQ_T_STATUS				0x0030
#define		IRQ_STATUS_IRQ_T_BIT				BIT(0)			// r
#define EN683_DZ_REG_RGB2YUV_YR					0x0040
#define		RGB2YUV_YR_MASK						GENMASK(9, 0)	// rw
#define EN683_DZ_REG_RGB2YUV_YG					0x0044
#define		RGB2YUV_YG_MASK						GENMASK(9, 0)	// rw
#define EN683_DZ_REG_RGB2YUV_YB					0x0048
#define		RGB2YUV_YB_MASK						GENMASK(9, 0)	// rw
#define EN683_DZ_REG_RGB2YUV_UB					0x004C
#define		RGB2YUV_UB_MASK						GENMASK(9, 0)	// rw
#define EN683_DZ_REG_RGB2YUV_VR					0x0050
#define		RGB2YUV_VR_MASK						GENMASK(9, 0)	// rw
#define EN683_DZ_REG_YC2RGB_RR					0x0054
#define		YC2RGB_RR_MASK						GENMASK(9, 0)	// rw
#define EN683_DZ_REG_YC2RGB_GR					0x0058
#define		YC2RGB_GR_MASK						GENMASK(9, 0)	// rw
#define EN683_DZ_REG_YC2RGB_GB					0x005C
#define		YC2RGB_GB_MASK						GENMASK(9, 0)	// rw
#define EN683_DZ_REG_YC2RGB_BB					0x0060
#define		YC2RGB_BB_MASK						GENMASK(9, 0)	// rw


/*******************************************************************************
 *
 * define
 *
*******************************************************************************/

/**
 * struct dz_last_submit - snapshot of the most recent hw_run_once invocation
 * @valid: Set to 1 after the first successful capture.
 * @src_yaddr: Source y address.
 * @src_caddr: Source c address.
 * @dst_yaddr: Destination y address.
 * @dst_caddr: Destination c address.
 * @src_width: Source width.
 * @src_width_total: Total source width.
 * @src_height: Source height.
 * @src_height_total: Total source height.
 * @dst_width: Destination width.
 * @dst_width_total: Total destination width.
 * @dst_height: Destination height.
 * @flip: Flip mode (0 = no flip, 1 = horizontal, 2 = vertical, 3 = both).
 * @src_fmt: enum enx_vdma_format value for the source format.
 * @dst_fmt: enum enx_vdma_format value for the destination format.
 * @submitter_pid: PID of the session that issued the submit.
 * @result: Last return value of hw_run_once (0 = success).
 *
 * Captured under @irq_lock by hw_run_once for debug visibility.
 */
struct dz_last_submit {
	u32   valid;
	u32   src_yaddr;
	u32   src_caddr;
	u32   dst_yaddr;
	u32   dst_caddr;
	u32   src_width;
	u32   src_width_total;
	u32   src_height;
	u32   src_height_total;
	u32   dst_width;
	u32   dst_width_total;
	u32   dst_height;
	u32   flip;
	u32   src_fmt;
	u32   dst_fmt;
	pid_t submitter_pid;
	int   result;
};

/**
 * struct dz_dev_state - Per-device state for the EN683 VDMA dz controller
 * @instance_id: Instance ID for this device (e.g., 0 for /dev/enx_vdma_dz0).
 * @irq_count: Total number of hardware IRQs serviced. Incremented inside
 *     the IRQ handler under dev->irq_lock.
 * @last_irq_status: Snapshot of EN683_DZ_REG_IRQ_STATUS at the last IRQ.
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
struct dz_dev_state {
	int instance_id;

	/* IRQ statistics + last-submit snapshot (protected by dev->irq_lock) */
	u64 irq_count;
	u32 last_irq_status;
	u64 timeout_count;
	struct dz_last_submit last;
};

#endif /* _EN683_DZ_H_ */