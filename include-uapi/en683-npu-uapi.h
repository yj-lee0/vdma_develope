// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Eyenix Corporation
 * Eyenix <support6@eyenix.com>
 *
 * Eyenix EN683 Video-DMA NPU function-specific UAPI
 *
 */

#ifndef _UAPI_EN683_NPU_H_
#define _UAPI_EN683_NPU_H_

#include "enx-vdma-uapi.h"

/*******************************************************************************
 *
 * VNPU Description info
 *
*******************************************************************************/

/* VNPU config data */
struct enx_en683_npu_submit_args {
	/* Depending on the flag, the src/dst_addr values ​​are determined to be addresses or offsets. */
	u32 src_addr_flag;	// 0: offset(src alloc index), 1: address(src phys addr)
	u32 dst_addr_flag;	// 0: offset(dst alloc index), 1: address(dst phys addr)

	u64 src_addr;
	u64 dst_addr;

	/* data_offset */
	u32 src_data_offset;	// byte offset into the source buffer (default 0)
	u32 dst_data_offset;	// byte offset into the destination buffer (default 0)

	/* source info */
	u16 src_width; // must be 32-byte aligned
	u16 src_height;

	/* Specifies the alignment of the NPU Operation Results. */
	u8 read_mode; // 0: 4Byte, 1: 8Byte, 2: 16Byte, 3: 32Byte

	/*
	 * Setting Data Scale When Reading NPU Operation Results.
	 * 0 : Bypass
	 * 1 : x2
	 * 2 : unsigned mode (Invert the 7th bit value of the NPU operation result and use it)
	 */
	u8 rgb_scale;

	u8 color_invert; // 0: normal, 1: invert

	/* cache flush */
	u32 src_cache_flush;	// source cache flush
	u32 dst_cache_flush;	// destination cache flush
};

/* VNPU config data */
struct eyenix_en683_npu_config_data {

};

/* VNPU ioctl cmd (0x50 ~ 0x5F) */
#define VDMAIOSET_NPU_SUBMIT			_IOWR(ENX_VDMA_IOC_MAGIC, 0x50, struct enx_en683_npu_submit_args)

#endif /* _UAPI_EN683_NPU_H_ */
