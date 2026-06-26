/* SPDX-License-Identifier: MIT */
/*
 * libvdma — Error codes
 *
 * Return convention:
 *   - All libvdma APIs return 0 on success, negative errno on failure
 *     (i.e., `return -ENX_ERR_VDMA_INVALID_ARG;`).
 *   - Pointer-returning APIs return NULL on failure and set errno.
 *
 * Two ranges :
 *   (1) Standard POSIX errno aliases  — strerror() compatible.
 *   (2) libvdma extension codes       — 0x1000+, no errno collision.
 */

#ifndef _VDMA_ERROR_H_
#define _VDMA_ERROR_H_

#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VDMA_OK                     0

/* ============================================================
 * (1) Standard errno aliases
 * ============================================================ */

/* Argument / state */
#define ENX_ERR_VDMA_INVALID_ARG            -EINVAL       /* NULL pointer, malformed struct */
#define ENX_ERR_VDMA_ALREADY_IMPORTED       -EEXIST       /* same backing already imported in this session */
#define ENX_ERR_VDMA_OUT_OF_RANGE           -ERANGE       /* numeric value outside accepted range */
#define ENX_ERR_VDMA_TOO_LARGE              -EFBIG        /* request size exceeds limit */
#define ENX_ERR_VDMA_OVERFLOW               -EOVERFLOW    /* result exceeds output buffer / counter */
#define ENX_ERR_VDMA_NOT_SUPPORTED          -ENOTSUP      /* operation not supported on this HW */

/* Resource */
#define ENX_ERR_VDMA_NO_MEMORY              -ENOMEM       /* alloc failure (lib or kernel) */
#define ENX_ERR_VDMA_QUOTA_EXCEEDED         -EDQUOT       /* per-device max_bufs reached */
#define ENX_ERR_VDMA_AGAIN                  -EAGAIN       /* transient — retry later */

/* Device / fd */
#define ENX_ERR_VDMA_NO_DEVICE              -ENODEV       /* device not opened, or detached */
#define ENX_ERR_VDMA_NOT_FOUND              -ENOENT       /* /dev node missing */
#define ENX_ERR_VDMA_BAD_FD                 -EBADF        /* fd invalidated after fatal ioctl */
#define ENX_ERR_VDMA_ACCESS_DENIED          -EACCES       /* permission, or buf not owned by session */

/* Execution */
#define ENX_ERR_VDMA_BUSY                   -EBUSY        /* HW engine busy */
#define ENX_ERR_VDMA_TIMED_OUT              -ETIMEDOUT    /* WAIT / SUBMIT exceeded timeout */
#define ENX_ERR_VDMA_INTERRUPTED            -EINTR        /* signal-interrupted blocking call */
#define ENX_ERR_VDMA_IO_ERROR               -EIO          /* low-level HW / register-level failure */

/* Process / session */
#define ENX_ERR_VDMA_OWNER_DEAD             -EOWNERDEAD   /* owning process / pid no longer matches */

/* ============================================================
 * (2) libvdma extension codes  (range : 0x7000A000 ~ 0x7000AFFF)
 *     Not in POSIX errno space. Use vdma_strerror() for messages.
 * ============================================================ */
#define ENX_ERR_VDMA_BASE                   0x7000A000

/* --- API contract violations --- */

/* The device passed to a per-HW submit function does not match.
 *   e.g.  vdma_font_submit(ENX_VDMA_JPEG_ENC, ...)  → always rejected. */
#define ENX_ERR_VDMA_DRV_MISMATCH           (ENX_ERR_VDMA_BASE + 0x01)

/* Buffer kind (SRC vs DST) does not match expected role at call site.
 *   e.g.  a SRC buf passed where DST is expected. */
#define ENX_ERR_VDMA_KIND_MISMATCH          (ENX_ERR_VDMA_BASE + 0x02)

/* Library / kernel ABI version mismatch detected at init. */
#define ENX_ERR_VDMA_ABI_MISMATCH           (ENX_ERR_VDMA_BASE + 0x03)

/* Library not initialized (open never succeeded, or after fatal teardown). */
#define ENX_ERR_VDMA_NOT_INITIALIZED        (ENX_ERR_VDMA_BASE + 0x04)

/* --- Process lifecycle --- */

/* Instance is stale after fork() — child inherited parent's lib state.
 *   Caller must close() then open() to re-establish its own fd. */
#define ENX_ERR_VDMA_FORK_STALE             (ENX_ERR_VDMA_BASE + 0x10)

/* --- EXPORT / IMPORT --- */

/* Buffer is not currently in an exportable state. */
#define ENX_ERR_VDMA_NOT_EXPORTABLE         (ENX_ERR_VDMA_BASE + 0x20)

/* fd passed to vdma_import() is not a valid vdma export fd. */
#define ENX_ERR_VDMA_BAD_EXPORT_FD          (ENX_ERR_VDMA_BASE + 0x21)

/* Optional : runtime check detected concurrent unsynchronized access
 *            to a shared backing (cross-device race). */
#define ENX_ERR_VDMA_SHARED_RACE            (ENX_ERR_VDMA_BASE + 0x22)

/* --- HW result --- */

/* HW reported an error during submit (e.g. JPEG overflow, decode header bad).
 *   Detailed sub-status is available in the per-driver response struct. */
#define ENX_ERR_VDMA_HW_FAULT               (ENX_ERR_VDMA_BASE + 0x30)

/* Operation was forcibly aborted (user vdma_abort or driver-initiated reset). */
#define ENX_ERR_VDMA_HW_ABORTED             (ENX_ERR_VDMA_BASE + 0x31)

/* Sentinel — one past the last defined extension code.
 *   New codes go below this; assertions/tables may compare against it. */
#define ENX_ERR_VDMA_LAST                   (ENX_ERR_VDMA_BASE + 0x100)

#ifdef __cplusplus
}
#endif
#endif /* _VDMA_ERROR_H_ */