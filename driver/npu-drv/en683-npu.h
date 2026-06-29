// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Eyenix Corporation
 * Eyenix <support6@eyenix.com>
 *
 * Eyenix EN683 SoC Video-DMA NPU controller.
 *
 */

#ifndef _EN683_NPU_H_
#define _EN683_NPU_H_

/*******************************************************************************
 *
 * register define
 *
*******************************************************************************/

#define EN683_NPU_REG_SRC_RADDR					0x0000
#define EN683_NPU_REG_DST_YADDR					0x0004
#define EN683_NPU_REG_DST_CADDR					0x0008
#define EN683_NPU_REG_CTRL						0x000C
#define		CTRL_AWQOS_MASK						GENMASK(31, 28)	// rw
#define		CTRL_ARQOS_MASK						GENMASK(27, 24)	// rw
#define		CTRL_CINV_BIT						BIT(8)			// rw
#define		CTRL_RMOD_MASK						GENMASK(6, 4)	// rw
#define		CTRL_RGB_SCALE_MASK					GENMASK(3, 2)	// rw
#define		CTRL_GO_BIT							BIT(0)			// rw
#define EN683_NPU_REG_RESOLUTION				0x0010
#define		RESOLUTION_VERT_MASK				GENMASK(27, 16)	// rw
#define		RESOLUTION_HORZ_MASK				GENMASK(12, 0)	// rw
#define EN683_NPU_REG_CSC_COEF0					0x0014
#define 	CSC_COEF0_YR_VAL					GENMASK(25, 16)	// rw
#define 	CSC_COEF0_YG_VAL					GENMASK(9, 0)	// rw
#define EN683_NPU_REG_CSC_COEF1					0x0018
#define 	CSC_COEF1_YB_VAL					GENMASK(25, 16)	// rw
#define 	CSC_COEF1_UB_VAL					GENMASK(9, 0)	// rw
#define EN683_NPU_REG_CSC_COEF2					0x001C
#define 	CSC_COEF2_VR_VAL					GENMASK(25, 16)	// rw
#define EN683_NPU_REG_IRQ_CTRL					0x0020
#define 	IRQ_CTRL_IRQ_T_CLR_BIT				BIT(9)			// w,sc
#define 	IRQ_CTRL_IRQ_T_EN_BIT				BIT(8)			// rw
#define 	IRQ_CTRL_IRQ_CLR_BIT				BIT(1)			// w,sc
#define 	IRQ_CTRL_IRQ_EN_BIT					BIT(0)			// rw
#define EN683_NPU_REG_IRQ_STATUS				0x0024
#define 	IRQ_STATUS_IRQ_BIT					BIT(0)			// r
#define EN683_NPU_REG_IRQ_T_STATUS				0x0028
#define 	IRQ_STATUS_IRQ_T_BIT				BIT(0)			// r


/*******************************************************************************
 *
 * define
 *
*******************************************************************************/

/**
 * struct npu_last_submit - snapshot of the most recent hw_run_once invocation
 * @valid: Set to 1 after the first successful capture.
 * @src_addr: Source address.
 * @dst_yaddr: Destination y address.
 * @dst_caddr: Destination c address.
 * @src_width: Source width.
 * @src_height: Source height.
 * @read_mode: Specifies the alignment of the NPU operation results.
 * @rgb_scale: RGB scale mode (e.g., bypass/scale by 2x/unsigned mode).
 * @color_invert: Color invert enable.
 * @submitter_pid: PID of the session that issued the submit.
 * @result: Last return value of hw_run_once (0 = success).
 *
 * Captured under @irq_lock by hw_run_once for debug visibility.
 */
struct npu_last_submit {
	u32   valid;
	u32   src_addr;
	u32   dst_yaddr;
	u32   dst_caddr;
	u32   src_width;
	u32   src_height;
	u32   read_mode;
	u32   rgb_scale;
	u32   color_invert;
	pid_t submitter_pid;
	int   result;
};

/**
 * struct npu_dev_state - Per-device state for the EN683 VDMA npu controller
 * @irq_count: Total number of hardware IRQs serviced. Incremented inside
 *     the IRQ handler under dev->irq_lock.
 * @last_irq_status: Snapshot of EN683_NPU_REG_IRQ_STATUS at the last IRQ.
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
struct npu_dev_state {
	/* IRQ statistics + last-submit snapshot (protected by dev->irq_lock) */
	u64 irq_count;
	u32 last_irq_status;
	u64 timeout_count;
	struct npu_last_submit last;
};

#endif /* _EN683_NPU_H_ */