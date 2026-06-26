/* SPDX-License-Identifier: MIT */
/*
 * libvdma_scaler - user-space API for EN683 DZ (scaler) VDMA (SYNC only)
 *
 * Contract:
 *   * Process-singleton 모델 — 한 프로세스 안에서 디바이스 fd 를 1개만 유지.
 *     라이브러리가 내부적으로 fd 와 버퍼 자료구조를 모두 관리. 외부에는
 *     mmap 가상주소(vdma_addr_t)만 노출.
 *   * vdma_scaler_open() / vdma_scaler_close() 는 refcount 로 매칭. 두 번째
 *     이후의 open() 에서 path 가 다르면 -EBUSY. NULL path 는
 *     /dev/enx_vdma_dz (기본 디바이스).
 *   * DZ 는 mode 분기 없음 — MOD_INIT 호출 없이 바로 동작 가능.
 *   * fork() 후 자식은 라이브러리 상태가 무효화되어 모든 API 가
 *     -EOWNERDEAD / -ENODEV 반환. 자식이 새로 open() 해야 사용 가능.
 *   * SYNC 만 지원. submit 은 HW 완료까지 block.
 *   * dst 버퍼는 submit 리턴 이후에만 유효한 결과를 가짐.
 *   * close 가 마지막 open-ref 를 떨굴 때 모든 자원(살아있는 buf, mmap,
 *     fd)을 강제 해제. 사용자가 들고 있던 vdma_addr_t 는 그 시점 이후
 *     접근 금지.
 *
 * Thread safety:
 *   Safe to call concurrently:
 *     - 서로 다른 vdma_addr_t 에 대한 alloc / free / submit
 *     - submit 와 다른 addr 를 대상으로 한 free
 *     - 동일 addr 에 대한 submit (read-only 사용)
 *     - open / close (refcount 매칭)
 *
 *   금지 (UB):
 *     - 시그널 핸들러에서 lib API 호출 (async-signal-safe 아님)
 *     - close 이후 stale vdma_addr_t 접근
 */

#ifndef _LIBVDMA_SCALER_H_
#define _LIBVDMA_SCALER_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VDMA_SCALER_ABI_VERSION		1u

/* 사용자에게 노출되는 유일한 식별자. mmap 한 가상주소 그대로 — 그 위에
 * memset/memcpy 등 일반 메모리 연산 가능. */
typedef void *vdma_addr_t;

/**
 * struct vdma_scaler_req - one submit call payload
 * @src:               source buffer (allocated by vdma_scaler_alloc_src)
 * @dst:               destination buffer (allocated by vdma_scaler_alloc_dst)
 *
 * @src_format:        enum enx_vdma_format value for source layout
 * @src_width:         active source width in pixels
 * @src_height:        active source height in pixels
 * @src_width_total:   physical pitch / stride width
 * @src_height_total:  physical stride height (used for chroma offset)
 * @src_crop_x:        source crop X offset (pixels from src origin)
 * @src_crop_y:        source crop Y offset (pixels from src origin)
 *
 * @dst_format:        enum enx_vdma_format value for destination layout
 * @dst_width:         destination width in pixels
 * @dst_height:        destination height in pixels
 * @dst_width_total:   destination physical pitch
 *
 * @vflip:             1 = vertical flip
 * @hflip:             1 = horizontal flip
 *
 * @src_cache_flush:   1 = L2-flush source range before submit
 * @dst_cache_flush:   1 = L2-flush destination range before submit
 *
 * NOTE: src_format / dst_format expects values from enum enx_vdma_format
 *       (defined in enx-vdma-uapi.h). 사용자 코드에서는 enum 값을 그대로
 *       uint32_t 로 캐스팅해 넣으면 됩니다.
 */
struct vdma_scaler_req {
	vdma_addr_t	src;
	vdma_addr_t	dst;

	uint32_t	src_format;
	uint16_t	src_width;
	uint16_t	src_height;
	uint16_t	src_width_total;
	uint16_t	src_height_total;
	uint16_t	src_crop_x;
	uint16_t	src_crop_y;

	uint32_t	dst_format;
	uint16_t	dst_width;
	uint16_t	dst_height;
	uint16_t	dst_width_total;

	uint8_t		vflip;
	uint8_t		hflip;

	uint8_t		src_cache_flush;
	uint8_t		dst_cache_flush;
};

/* ---- Lifecycle (refcounted per device path) ------------------------------ */
int  vdma_scaler_open  (const char *path);	/* NULL → /dev/enx_vdma_dz */
void vdma_scaler_close (const char *path);

/* ---- Buffer alloc / free (vir_addr 기반) -------------------------------- */
/* 성공 시 mmap 된 가상주소 반환, 실패 시 NULL + errno. */
vdma_addr_t vdma_scaler_alloc_src(size_t size);
vdma_addr_t vdma_scaler_alloc_dst(size_t size);

/* size 가 0 이면 size 검증 생략, 0 이 아니면 alloc 시 size 와 일치해야 함. */
int vdma_scaler_free(vdma_addr_t addr, size_t size);

/* ---- Submission --------------------------------------------------------- */
/* SYNC: HW 완료까지 블록. 반환 = HW result (0 또는 음수 errno). */
int vdma_scaler_submit(const struct vdma_scaler_req *req);

/* ---- Cross-fd buffer sharing (anon_inode based) ------------------------- */
/* Producer 가 자기 addr 를 다른 프로세스에 보낼 수 있는 fd 로 변환.
 * 호출자가 fd 소유, close 책임. */
int vdma_scaler_export(vdma_addr_t addr, int *export_fd_out);

/* SCM_RIGHTS 로 받은 fd 를 import. kind 는 IN 인자 — IMPORT 측이 이 buf 를
 * 어떻게 쓸지 (ENX_BUF_SRC 또는 ENX_BUF_DST) 선언. 성공 시 새 mmap 가상주소
 * 반환. size_out 은 NULL 허용. */
vdma_addr_t vdma_scaler_import(int export_fd, uint32_t kind,
			       size_t *size_out);

#ifdef __cplusplus
}
#endif
#endif /* _LIBVDMA_SCALER_H_ */
