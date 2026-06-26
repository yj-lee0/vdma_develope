// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Eyenix Corporation
 * Eyenix <support6@eyenix.com>
 *
 * Eyenix EN683 Video-DMA FONT function-specific UAPI
 *
 */

#ifndef _UAPI_EN683_FONT_H_
#define _UAPI_EN683_FONT_H_

#include "enx-vdma-uapi.h"

/*******************************************************************************
 *
 * VFONT Description info
 *
*******************************************************************************/

/* VFONT control info */
struct enx_en683_font_blit {
	/* Depending on the flag, the src_addr values ​​are determined to be addresses or offsets. */
	u32 src_addr_flag;		// 0: offset(src alloc index), 1: address(phys addr)

	/* font img start addr */
	u64 src_addr;
	u32 src_data_offset;	// byte offset into the source buffer (default 0)

	/* font img width / height (only even) */
	u16 src_width;
	u16 src_height;

	/* font img start position */
	u16 font_xpos;
	u16 font_ypos;

	/* font dst YC address */
	u64 dst_addr_y;
	u64 dst_addr_c;
	u32 reserved;

	/* font img color(YCbCr) */
	u8 font_color_y;
	u8 font_color_cb;
	u8 font_color_cr;

	/* It is recognized as the font value of font img based on the set value (Y). */
	u8 font_value;

	/* font img outline color(YCbCr) */
	u8 outline_color_y;
	u8 outline_color_cb;
	u8 outline_color_cr;

	/* It is recognized as the outline value of font img based on the set value (Y). */
	u8 outline_value;

	/* font img background color(YCbCr) */
	u8 bg_color_y;
	u8 bg_color_cb;
	u8 bg_color_cr;

	/* font write mode */
	u8 font_wr_mode; // 0: YC(RW), 1: Y(RW), 2: YC(Wonly), 3: Y(Wonly)

	/*
	 * font color tone :
	 * Determines whether to use the color of font img as the value of font_color
	 */
	u8 font_color_tone; // 0: bitmap color, 1: font_color

	/* font alpha blending*/
	u16 font_alpha; // 0~256 (0: transparent, 256: opaque)

	/*
	 * font threshold :
	 * Recognizes values ​​greater than the threshold as font img based on the threshold value
	 */
	u8 font_threshold;
};

/* VFONT config data */
struct enx_en683_font_submit_args {
	/*
	 * The init_type determines the destination type to which the font will be applied.
	 * - kernel     : Memory      -> 0 : offset, 1 : address
	 * - videocore  : destination ref index. (for multi-channel draw)
	 * Depending on the flag, the destination is determined as an address, offset, or destination index.
	 */
	u32 dst_type_index;
	u32 dst_data_offset;		// byte offset into the destination buffer (default 0)

	/* Used only when init_type is kernel. */
	u16 dst_width;				// Horizontal line offset
	u16 dst_height;				// calcul c addr
	u32 background_overlay;		// 0: no overlay(dst_buf memset 0), 1: Overlay on background image

	u32 font_number;
	struct enx_en683_font_blit blits[]; // flexible array -> [font_number]
};

/////////////To do target V2///////////////////
// struct enx_en683_font_submit_args {
// 	u32	dst_id;
// 	u32	src_count;
// 	u32	flags;						/* EN683_SUBMIT_* */
// 	u32	job_id;						/* OUT (ASYNC only) */
// 	u64	user_token;
// 	struct en683_font_info blits[]; /* flexible array -> [src_count] */
// };

/* VFONT ioctl cmd (0x30 ~ 0x3F) */
#define VDMAIOSET_FONT_SUBMIT		_IOWR(ENX_VDMA_IOC_MAGIC, 0x30, struct enx_en683_font_submit_args)
#define VDMAIOGET_MAX_SRCS			_IOWR(ENX_VDMA_IOC_MAGIC, 0x31, u32)

#endif /* _UAPI_EN683_FONT_H_ */
