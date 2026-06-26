// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Eyenix Corporation
 * Eyenix <support6@eyenix.com>
 *
 * Eyenix EN683 Video-DMA JPEG Encoder/Decoder function-specific UAPI
 *
 */

#ifndef _UAPI_EN683_JPEG_H_
#define _UAPI_EN683_JPEG_H_

#include "enx-vdma-uapi.h"

/*******************************************************************************
 *
 * JPEG Encoder/Decoder Description info
 *
*******************************************************************************/

/* JPEG Encoder response format */
struct enx_en683_jpegenc_response {
	u32 cycle;
	u32 jpeg_size;
	union {
		u32 value;
		struct {
			u32 err_occur:1;
			u32 err_file_ovf:1;
			u32 __rev0:30;
		};
	} process_error;
};

/* JPEG Encoder config data */
struct enx_en683_jpegenc_config {
	/* Depending on the flag, the src/dst_addr values ​​are determined to be addresses or offsets. */
	u32 src_addr_flag;		// 0: offset(src alloc index), 1: address(src phys addr)
	u32 dst_addr_flag;		// 0: offset(dst alloc index), 1: address(dst phys addr)

	u64 src_addr;
	u64 dst_addr;

	/* data_offset */
	u32 src_data_offset;	// byte offset into the source buffer (default 0)
	u32 dst_data_offset;	// byte offset into the destination buffer (default 0)

	/* source info */
	u16 src_width_total;
	u16 src_height_total;
	enum enx_vdma_format src_format;

	/* crop info */
	u16 src_width;
	u16 src_height;
	u16 src_width_pos;
	u16 src_height_pos;

	/* jpeg option */
	u32 ovf_threshold;		// JPEG size overflow threshold
	u32 quality;			// JPEG quality (0~100)

	/* cache flush */
	u32 src_cache_flush;	// source cache flush
	u32 dst_cache_flush;	// destination cache flush
};

/* JPEG Encoder submit data */
struct enx_en683_jpegenc_submit_args {
	struct enx_en683_jpegenc_config		cfg;
	struct enx_en683_jpegenc_response	resp;
};

/* JPEG Decoder response format */
struct enx_en683_jpegdec_response {
	u32 dst_size;
	u32 cycle;
	u16 hsize;
	u16 vsize;
	u16 hsize_wr;
	u16 vsize_wr;
	int fmt;
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

/* JPEG Decoder config data */
struct enx_en683_jpegdec_config {
	/* Depending on the flag, the src/dst_addr values ​​are determined to be addresses or offsets. */
	u32 src_addr_flag;		// 0: offset(src alloc), 1: address(src phys addr)
	u32 dst_addr_flag;		// 0: offset(dst alloc), 1: address(dst phys addr)

	u64 src_addr;
	u64 dst_addr;

	/* data_offset */
	u32 src_data_offset;	// byte offset into the source buffer (default 0)
	u32 dst_data_offset;	// byte offset into the destination buffer (default 0)

	/* jpeg img size */
	u32 src_jpeg_size;

	/*
	 * dst_buf_size — destination buffer capacity in bytes. REQUIRED.
	 *
	 * While not strictly required by the HW logic itself, always provide
	 * the pre-calculated size here. This prevents unexpected malfunctions
	 * that can occur if the generated output exceeds the allocated
	 * destination buffer.
	 */
	u32 dst_buf_size;

	/* cache flush */
	u32 src_cache_flush;	// source cache flush
	u32 dst_cache_flush;	// destination cache flush
};

/* JPEG Decoder submit data */
struct enx_en683_jpegdec_submit_args {
	struct enx_en683_jpegdec_config		cfg;
	struct enx_en683_jpegdec_response	resp;
};

/* JPEG ioctl cmd (0x40 ~ 0x4F) */
#define VDMAIOSET_JPEG_ENC_SUBMIT	_IOWR(ENX_VDMA_IOC_MAGIC, 0x40, struct enx_en683_jpegenc_submit_args)
#define VDMAIOSET_JPEG_DEC_SUBMIT	_IOWR(ENX_VDMA_IOC_MAGIC, 0x41, struct enx_en683_jpegdec_submit_args)
#define VDMAIOSET_JPEG_DISCARD		_IOW (ENX_VDMA_IOC_MAGIC, 0x42, u32)

#endif /* _UAPI_EN683_JPEG_H_ */
