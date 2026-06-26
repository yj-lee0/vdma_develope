// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
/*
 * Copyright (C) 2026 Eyenix Corporation
 *
 * Eyenix Vide-DMA - Common UAPI
 *
 */

#ifndef _ENX_VDMA_UAPI_H_
#define _ENX_VDMA_UAPI_H_

#include <linux/types.h>
#include <linux/ioctl.h>

#define ENX_VDMA_IOC_MAGIC	'D'

/* Buffer kind */
#define ENX_BUF_SRC			1u
#define ENX_BUF_DST			2u
#define ENX_BUF_RAW			3u
#define ENX_BUF_ID			4u
#define ENX_BUF_NONE		5u

/* Submit flags */
#define ENX_SUBMIT_SYNC		(0u << 0)
#define ENX_SUBMIT_ASYNC	(1u << 0)

enum enx_vdma_mode_type {
	ENX_VDMA_MODE_NONE,
	ENX_VDMA_MODE_KERNEL,
	ENX_VDMA_MODE_VIDEO_CORE,
};

enum enx_vdma_format {
	VDMA_FMT_RGB,
	VDMA_FMT_RBG,
	VDMA_FMT_GRB,
	VDMA_FMT_GBR,
	VDMA_FMT_BRG,
	VDMA_FMT_BGR,

	VDMA_FMT_RRR,
	VDMA_FMT_GGG,
	VDMA_FMT_BBB,
	VDMA_FMT_Rzz,
	VDMA_FMT_zGz,
	VDMA_FMT_zzB,

	VDMA_FMT_YUV444,
	VDMA_FMT_YUV444_YVU,
	VDMA_FMT_YUV444_UVY,
	VDMA_FMT_YUV444_UYV,
	VDMA_FMT_YUV444_VUY,
	VDMA_FMT_YUV444_VYU,
	VDMA_FMT_YUV444_Y,

	VDMA_FMT_YUV422_YUYV,
	VDMA_FMT_YUV422_YVYU,
	VDMA_FMT_YUV422_UYVY,
	VDMA_FMT_YUV422_VYUY,
	VDMA_FMT_YUV422_Y,

	VDMA_FMT_YUV420SP_NV12,
	VDMA_FMT_YVU420SP_NV21,
	VDMA_FMT_YUV420SP_NV_Y,

	VDMA_FMT_YUV400,
};

/**
 * struct vdma_alloc_args - Arguments for VDMA buffer allocation ioctl
 * @kind: IN  — Buffer kind/role identifier (ENX_BUF_SRC / ENX_BUF_DST).
 * @size: IN  — Requested buffer size in bytes. Will be page-aligned
 *     internally by the kernel.
 * @id: OUT — Buffer identifier assigned by the kernel on success.
 *     Used in subsequent ioctls (FREE, EXPORT, SUBMIT, etc.)
 *     and as the lookup key in the device's buffer XArray.
 * @__resv: Reserved, must be zero. Ensures explicit 8-byte alignment for
 *     ABI stability across 32/64-bit user space.
 * @mmap_offset: OUT — Offset to pass to mmap(2) for mapping this buffer
 *     into user space. Encodes @id internally.
 *
 * Used with VDMAIOSET_ALLOC. On success, both @id and @mmap_offset are
 * filled. The allocated buffer is owned by the calling session and is
 * automatically released when the file descriptor is closed.
 */
struct vdma_alloc_args {
	u32	kind;
	u32	size;
	u32	id;
	u32 __resv;
	u64	mmap_offset;
};

/**
 * struct vdma_free_args - Arguments for VDMA buffer free ioctl
 * @id: IN — Buffer identifier obtained from a previous ALLOC ioctl
 *     (or IMPORT, when releasing an imported buffer attachment).
 * @__resv: Reserved, must be zero. Ensures explicit 8-byte alignment for
 *     ABI stability across 32/64-bit user space.
 *
 * Used with VDMAIOSET_FREE. Removes the caller's ownership reference; the
 * underlying memory is only reclaimed when all outstanding references
 * (in-flight jobs, importers, mmap regions) are dropped.
 */
struct vdma_free_args {
	u32	id;
	u32	__resv;
};

/**
 * struct vdma_wait_args - Arguments for waiting on an asynchronous job
 * @job_id: IN  — Job identifier returned by an ASYNC submit ioctl.
 * @timeout_ms: IN  — Maximum wait time in milliseconds. A value of 0 means
 *     wait indefinitely.
 * @user_token: OUT — User-supplied token echoed back from the original
 *     submission, allowing the caller to correlate the completion with its
 *     own context.
 * @result: OUT — Hardware execution result: 0 on success, or a negative
 *     errno-style code on failure.
 * @__resv: Reserved, must be zero. Pads the structure for ABI stability.
 *
 * Used with VDMAIOSET_WAIT. Returns 0 on completion, -ETIMEDOUT on timeout,
 * or other negative errnos on error. Wait is only valid for jobs submitted
 * with ENX_SUBMIT_ASYNC.
 */
struct vdma_wait_args {
	u32	job_id;
	u32	timeout_ms;
	u64	user_token;
	s32	result;
	u32	__resv;
};

/**
 * struct vdma_export_args - Arguments for exporting a buffer across processes
 * @id: IN  — Buffer identifier (from a previous ALLOC) to be exported.
 *     Caller must own or have imported this buffer.
 * @__resv: Reserved, must be zero.
 * @export_fd: OUT — Anonymous inode file descriptor representing the buffer.
 *     The kernel grants the caller an additional reference on the buffer
 *     that is tied to this fd. The fd can be passed to another process via
 *     SCM_RIGHTS over a UNIX socket.
 *     The caller owns the fd and must close(2) it when no longer needed;
 *     closing drops the reference held by EXPORT.
 * @__resv2: Reserved, must be zero.
 *
 * Used with VDMAIOSET_EXPORT. EXPORT does not duplicate or transfer
 * ownership of the buffer — it only adds a reference and packages it into
 * a file descriptor. The underlying memory remains alive as long as any
 * reference (producer ownership, export fd, importer attachment) is held.
 *
 * Typical flow:
 *   Producer:  ALLOC → buf id X
 *              EXPORT(X) → export_fd
 *              sendmsg(SCM_RIGHTS, export_fd) → peer process
 *              close(export_fd)   // peer now holds the only fd-ref
 */
struct vdma_export_args {
	u32	id;
	u32	__resv;
	s32	export_fd;
	u32	__resv2;
};

/**
 * struct vdma_import_args - Arguments for importing a previously exported buffer
 * @export_fd: IN  — File descriptor received from a peer (typically via
 *     SCM_RIGHTS). May originate from VDMAIOSET_EXPORT on ANY VDMA device —
 *     cross-process and cross-device (e.g., scaler → rot) are both supported.
 * @__resv: Reserved, must be zero.
 * @kind: IN/OUT — Importer-declared usage tag (ENX_BUF_SRC or ENX_BUF_DST).
 *     The importer chooses how it intends to use this buffer; the value is
 *     stored on the new view independently of the exporter's tag. The
 *     kernel echoes the accepted value back on success.
 * @id: OUT — Buffer identifier valid within the importer's session. A
 *     fresh id is allocated in the importing device's id space — it is
 *     unrelated to the producer's id.
 * @size: OUT — Buffer size in bytes (page-aligned at original allocation).
 * @mmap_offset: OUT — Offset to pass to mmap(2) on this fd. Encodes @id.
 *
 * IMPORT creates a new per-session view over the shared physical memory and
 * registers it in the importing device's xa with its own id. The buffer's
 * underlying memory survives as long as ANY view holds a reference, so the
 * producer and consumer may free in any order.
 *
 * Cross-device example (scaler → jpegenc):
 *   fd_scaler = open(/dev/enx_vdma_scaler)
 *   fd_jpegenc    = open(/dev/enx_vdma_jenc)
 *   mid_s = ALLOC(fd_scaler, DST)
 *   submit_scaler(fd_scaler, ..., dst=mid_s)            // SYNC or ASYNC
 *   xfd = EXPORT(fd_scaler, mid_s)
 *   mid_r = IMPORT(fd_jpegenc, xfd, kind=SRC)           // re-tag as SRC
 *   close(xfd)
 *   submit_jpegenc(fd_jpegenc, ..., src=mid_r)          // SYNC or ASYNC
 */
struct vdma_import_args {
	s32	export_fd;
	u32	__resv;
	u32	kind;		/* IN/OUT */
	u32	id;
	u32	size;
	u32	__resv2;
	u64	mmap_offset;
};


/* Common ioctl cmd (0x01 ~ 0x0F) */
#define VDMAIOSET_ALLOC				_IOWR(ENX_VDMA_IOC_MAGIC, 0x01, struct vdma_alloc_args)
#define VDMAIOSET_FREE				_IOW (ENX_VDMA_IOC_MAGIC, 0x02, struct vdma_free_args)
#define VDMAIOSET_WAIT				_IOWR(ENX_VDMA_IOC_MAGIC, 0x03, struct vdma_wait_args)
#define VDMAIOSET_EXPORT			_IOWR(ENX_VDMA_IOC_MAGIC, 0x04, struct vdma_export_args)
#define VDMAIOGET_IMPORT			_IOWR(ENX_VDMA_IOC_MAGIC, 0x05, struct vdma_import_args)
#define VDMAIOGET_ABORT				_IOR (ENX_VDMA_IOC_MAGIC, 0x06, u32)
#define VDMAIOGET_MAX_BUFS			_IOWR(ENX_VDMA_IOC_MAGIC, 0x07, u32)
#define VDMAIOSET_MOD_INIT			_IOW (ENX_VDMA_IOC_MAGIC, 0x08, u32)

#endif /* _ENX_VDMA_UAPI_H_ */
