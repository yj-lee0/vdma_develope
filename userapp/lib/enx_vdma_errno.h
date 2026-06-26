/* SPDX-License-Identifier: MIT */
/*
 * VDMA — Error codes
 *
 * Return convention:
 *   - All libvdma APIs return 0 on success, negative errno on failure
 *     (i.e., `return -ENX_ERR_VDMA_INVALID_ARG;`).
 *   - Pointer-returning APIs return NULL on failure and set errno.
 */

#define VDMA_OK 									0

/*
 * VDMA ERROR number
 */
#define ENX_ERR_VDMA_BASE							0x7000A000
#define ENX_ERR_VDMA_FORK_STALE						(ENX_ERR_VDMA_BASE + 0x00)
#define ENX_ERR_VDMA_NOT_INITIALIZED				(ENX_ERR_VDMA_BASE + 0x01)
#define ENX_ERR_VDMA_INVALID_ARG					(ENX_ERR_VDMA_BASE + 0x02)
#define ENX_ERR_VDMA_NO_MEMORY						(ENX_ERR_VDMA_BASE + 0x03)
#define ENX_ERR_VDMA_QUOTA_EXCEEDED					(ENX_ERR_VDMA_BASE + 0x04)
#define ENX_ERR_VDMA_NO_DEVICE						(ENX_ERR_VDMA_BASE + 0x05)
#define ENX_ERR_VDMA_BAD_FD							(ENX_ERR_VDMA_BASE + 0x06)
#define ENX_ERR_VDMA_OWNER_DEAD						(ENX_ERR_VDMA_BASE + 0x07)
#define ENX_ERR_VDMA_INIT_TYPE						(ENX_ERR_VDMA_BASE + 0x08)
#define ENX_ERR_VDMA_FUNC_TYPE						(ENX_ERR_VDMA_BASE + 0x09)
#define ENX_ERR_VDMA_CONFIG_NOT_ALLOCATION			(ENX_ERR_VDMA_BASE + 0x0A)
#define ENX_ERR_VDMA_ALREADY_IMPORTED				(ENX_ERR_VDMA_BASE + 0x0B)
#define ENX_ERR_VDMA_FONT_MAX_SIZE_EXCEEDED			(ENX_ERR_VDMA_BASE + 0x0C)
#define ENX_ERR_VDMA_FONT_MAX_YC_IDX				(ENX_ERR_VDMA_BASE + 0x0D)
#define ENX_ERR_VDMA_JPEG_ENC_FAIL					(ENX_ERR_VDMA_BASE + 0x0E)
#define ENX_ERR_VDMA_JPEG_DEC_FAIL					(ENX_ERR_VDMA_BASE + 0x0F)