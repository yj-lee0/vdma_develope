// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Eyenix Corporation
 * Eyenix <support6@eyenix.com>
 *
 * Eyenix EN683 Video-DMA FONT internal header
 *
 */

#ifndef _EN683_FONT_H_
#define _EN683_FONT_H_

/*******************************************************************************
 *
 * register define
 *
*******************************************************************************/

#define EN683_FONT_REG_CTRL0					0x0000
#define		CTRL0_GO_BIT						BIT(0)			// w,sc
#define EN683_FONT_REG_FONT_NUM					0x0004
#define		FONT_NUM_MASK						GENMASK(15, 0)	// rw
#define EN683_FONT_REG_DESC_ADDR				0x0008
#define EN683_FONT_REG_LINE_STRIDE				0x000C
#define		LINE_STRIDE_MASK					GENMASK(12, 0)	// rw
#define EN683_FONT_REG_IRQ_CTRL					0x0010
#define		IRQ_CTRL_IRQ_CLR_T_BIT				BIT(9)			// w,sc
#define		IRQ_CTRL_IRQ_EN_T_BIT				BIT(8)			// rw
#define		IRQ_CTRL_IRQ_CLR_BIT				BIT(1)			// w,sc
#define		IRQ_CTRL_IRQ_EN_BIT					BIT(0)			// rw
#define EN683_FONT_REG_IRQ_STATUS				0x0014
#define		IRQ_STATUS_IRQ_T_BIT				BIT(1)			// r
#define		IRQ_STATUS_IRQ_BIT					BIT(0)			// r
#define EN683_FONT_REG_AXI_CTRL					0x0080
#define		AXI_CTRL_ARLEN_MASK					GENMASK(14, 12)	// rw
#define		AXI_CTRL_AWLEN_MASK					GENMASK(10, 8)	// rw
#define		AXI_CTRL_ARQOS_MASK					GENMASK(7, 4)	// rw
#define		AXI_CTRL_AWQOS_MASK					GENMASK(3, 0)	// rw

/*******************************************************************************
 *
 * define
 *
*******************************************************************************/

/* osd description - 64byte */
struct enx_en683a_osd_desc {
	/* osd ctrl0 */
	union {
		u32 value;
		struct {
			u32 type:2;
			u32 __rev0:6;
			u32 mode:2;
			u32 __rev1:6;
			u32 wr_mode:2;
			u32 __rev2:6;
			u32 __rev3:8;
		};
	} osd_ctrl0;
	/* object address */
	u32 obj_yaddr; // Font / Layer Y / Segment base address
	u32 obj_caddr; // Layer CbCr base address
	u32 reserved_0;
	/* object width(only even) and height(only even) */
	union {
		u32 value;
		struct {
			u32 obj_width:13;
			u32 __rev0:3;
			u32 obj_height:13;
			u32 __rev1:3;
		};
	} osd_size;
	u32 reserved_1;
	/* coordinate0 - Font / Layer / Mosaic / Box / Triangle */
	union {
		u32 value;
		struct {
			u32 y0:13;
			u32 __rev0:3;
			u32 x0:13;
			u32 __rev1:3;
		};
	} coord0;
	/* coordinate1 - Triangle */
	union {
		u32 value;
		struct {
			u32 y1:13;
			u32 __rev0:3;
			u32 x1:13;
			u32 __rev1:3;
		};
	} coord1;
	/* coordinate2 - Triangle */
	union {
		u32 value;
		struct {
			u32 y2:13;
			u32 __rev0:3;
			u32 x2:13;
			u32 __rev1:3;
		};
	} coord2;
	/* Mosaic grid pos */
	union {
		u32 value;
		struct {
			u32 y_grid:13;
			u32 __rev0:3;
			u32 x_grid:13;
			u32 __rev1:3;
		};
	} mosaic_grid;
	/* osd ctrl1 */
	union {
		u32 value;
		struct {
			u32 global_alpha:9; // 0~256 (0: transparent, 256: opaque)
			u32 __rev0:7;
			u32 mosaic_quant_step:2;
			u32 __rev1:14;
		};
	} osd_ctrl1;
	/* osd ctrl2 */
	union {
		u32 value;
		struct {
			u32 segment_threshold:8;
			u32 __rev0:8;
			u32 outline_key:8;
			u32 font_key:8;
		};
	} osd_ctrl2;
	/* background color - write-only mode */
	union {
		u32 value;
		struct {
			u32 bg_cr:8;
			u32 bg_cb:8;
			u32 bg_y:8;
			u32 __rev0:8;
		};
	} bg_color;
	/* Foreground/Font color */
	union {
		u32 value;
		struct {
			u32 fg_cr:8;
			u32 fg_cb:8;
			u32 fg_y:8;
			u32 __rev0:8;
		};
	} fg_color;
	/* outline color */
	union {
		u32 value;
		struct {
			u32 outline_cr:8;
			u32 outline_cb:8;
			u32 outline_y:8;
			u32 __rev0:8;
		};
	} outline_color;
	/* Alpha key - Layer Transparent processing */
	union {
		u32 value;
		struct {
			u32 alpha_cr:8;
			u32 alpha_cb:8;
			u32 alpha_y:8;
			u32 __rev0:5;
			u32 cr_en:1;
			u32 cb_en:1;
			u32 y_en:1;
		};
	} alpha_ctrl;
};

/* font description - 64byte */
struct enx_en683_font_desc {
	/* font img start addr */
	u32 font_src;
	/* font img width(only even) */
	union {
		u32 value;
		struct {
			u32 width:13;
			u32 __rev0:19;
		};
	} font_width;
	/* font img height(only even) */
	union {
		u32 value;
		struct {
			u32 height:13;
			u32 __rev0:19;
		};
	} font_height;
	/* font dst Y address */
	u32 font_yaddr0;
	u32 font_yaddr1;
	u32 font_yaddr2;
	/* font dst C address */
	u32 font_caddr0;
	u32 font_caddr1;
	u32 font_caddr2;
	/* font img color(YCbCr) */
	union {
		u32 value;
		struct {
			u32 font_cr:8;
			u32 font_cb:8;
			u32 font_y:8;
			/* It is recognized as the font value of font img based on the set value (Y). */
			u32 font:8;
		};
	} font_color;
	/* font img outline color(YCbCr) */
	union {
		u32 value;
		struct {
			u32 outline_cr:8;
			u32 outline_cb:8;
			u32 outline_y:8;
			/* It is recognized as the outline value of font img based on the set value (Y). */
			u32 outline:8;
		};
	} font_outline_color;
	/* font img background color(YCbCr) */
	union {
		u32 value;
		struct {
			u32 bg_cr:8;
			u32 bg_cb:8;
			u32 bg_y:8;
			u32 __rev0:8;
		};
	} font_bg_color;
	/* font write mode */
	union {
		u32 value;
		struct {
			/* If you select Wonly mode,
			 * it will be applied with the set background color without blending.
			 */
			u32 wr_mode:2;	// 0: YC(RW), 1: Y(RW), 2: YC(Wonly), 3: Y(Wonly)
			u32 __rev0:30;
		};
	} font_wr_mode;
	/* font color tone */
	union {
		u32 value;
		struct {
			/* Determines whether to use the color of font img as the value of font_color */
			u32 color_tone:1; // 0: bitmap color, 1: font_color
			u32 __rev0:31;
		};
	} font_color_tone;
	/* font alpha blending*/
	union {
		u32 value;
		struct {
			u32 alpha:9;	// 0~256 (0: transparent, 256: opaque)
			u32 __rev0:23;
		};
	} font_alpha;
	/* font threshold */
	union {
		u32 value;
		struct {
			/* Recognizes values ​​greater than the threshold as font img based on the threshold value */
			u32 threshold:8;
			u32 __rev0:24;
		};
	} font_threshold;
};

/**
 * struct font_last_submit - snapshot of the most recent hw_run_once invocation
 * @valid: Set to 1 after the first successful capture.
 * @mode: active_mode at the time of submit (KERNEL / VIDEO_CORE).
 * @font_number: Number of glyphs in the submit.
 * @dst_kind: ENX_BUF_ID / RAW / NONE for the destination.
 * @dst_width: Destination width (KERNEL mode only).
 * @dst_height: Destination height (KERNEL mode only).
 * @dst_paddr: Resolved destination physical address.
 * @desc_paddr: Physical address of the font_desc array used.
 * @submitter_pid: PID of the session that issued the submit.
 * @result: Last return value of hw_run_once (0 = success).
 *
 * Captured under @irq_lock by hw_run_once for debug visibility.
 */
struct font_last_submit {
	u32   valid;
	u32   mode;
	u32   font_number;
	u32   dst_kind;
	u32   dst_width;
	u32   dst_height;
	u64   dst_paddr;
	u64   desc_paddr;
	pid_t submitter_pid;
	int   result;
};

/**
 * struct font_dev_state - Per-device state for the EN683 VDMA font controller
 * @lock: Spinlock protecting @active_mode, @session_count and @owner_pid.
 *     Held for the smallest critical sections
 *     (mode arbitration and refcount tracking only).
 * @chip_type: EN683 or EN683A, determined at probe time by reading the
 *     EN683_SYSTEM_ID register.
 * @active_mode: Currently selected operating mode (ENX_VDMA_MODE_NONE /
 *     _KERNEL / _VIDEO_CORE). Set by the first mode_init() ioctl after open
 *     and reset to NONE when the last session closes.
 * @session_count: Number of open file descriptors that have joined this mode.
 *     VIDEO_CORE requires this to be exactly 1.
 * @owner_pid: PID of the task that activated VIDEO_CORE mode. Used only for
 *     diagnostic messages; meaningful only while
 *     @active_mode == ENX_VDMA_MODE_VIDEO_CORE.
 * @irq_count: Total number of hardware IRQs serviced. Incremented inside
 *     the IRQ handler under dev->irq_lock.
 * @last_irq_status: Snapshot of EN683_FONT_REG_IRQ_STATUS at the last IRQ.
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
struct font_dev_state {
    spinlock_t lock;
	u32 chip_type;		/* EN683 or EN683A */
	int active_mode;
    int session_count;
	pid_t owner_pid;	/* used only firmware mode */

	/* IRQ statistics + last-submit snapshot (protected by dev->irq_lock) */
	u64 irq_count;
	u32 last_irq_status;
	u64 timeout_count;
	struct font_last_submit last;
};

/* per-session driver private data */
struct font_session {
	struct enx_en683a_osd_desc *osd_desc;
	struct enx_en683_font_desc *font_desc;
	dma_addr_t desc_paddr;
};

#endif /* _EN683_FONT_H_ */