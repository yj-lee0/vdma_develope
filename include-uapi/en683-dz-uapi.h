// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Eyenix Corporation
 * Eyenix <support6@eyenix.com>
 *
 * Eyenix EN683 Video-DMA DZ function-specific UAPI
 *
 */

#ifndef _UAPI_EN683_DZ_H_
#define _UAPI_EN683_DZ_H_

#include "enx-vdma-uapi.h"

/*******************************************************************************
 *
 * VDZ Description info
 *
*******************************************************************************/

/* VDZ config data */
struct enx_en683_dz_submit_args {
	/* Depending on the flag, the src/dst_addr values ​​are determined to be addresses or offsets. */
	u32 src_addr_flag;		// 0: offset(src alloc index), 1: address(src phys addr)
	u32 dst_addr_flag;		// 0: offset(dst alloc index), 1: address(dst phys addr)

	/* buf_id or phys_addr */
	u64 src_addr;
	u64 dst_addr;

	/* data_offset */
	u32 src_data_offset;	// byte offset into the source buffer (default 0)
	u32 dst_data_offset;	// byte offset into the destination buffer (default 0)

	/* source info */
	u16 src_width;
	u16 src_width_total;
	u16 src_height;
	u16 src_height_total;
	enum enx_vdma_format src_format;

	/* source crop info */
	u16 src_width_pos;
	u16 src_height_pos;

	/* destination info */
	u16 dst_width;
	u16 dst_width_total;
	u16 dst_height;
	enum enx_vdma_format dst_format;

	/* flip */
	u8 vflip;
	u8 hflip;

	/* cache flush */
	u32 src_cache_flush;	// source cache flush
	u32 dst_cache_flush;	// destination cache flush
};

/////////////To do - ASYNC ///////////////////
// struct enx_en683_dz_submit_async_args {
// 	u32	dst_id;
// 	u32	src_count;
// 	u32	flags;						/* EN683_SUBMIT_* */
// 	u32	job_id;						/* OUT (ASYNC only) */
// 	u64	user_token;
// 	struct enx_en683_dz_submit_args blits;
// };

/* VDZ ioctl cmd (0x10 ~ 0x1F) */
#define VDMAIOSET_DZ_SUBMIT			_IOWR(ENX_VDMA_IOC_MAGIC, 0x10, struct enx_en683_dz_submit_args)

#endif /* _UAPI_EN683_DZ_H_ */
