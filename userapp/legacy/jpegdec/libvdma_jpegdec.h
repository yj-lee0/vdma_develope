/* SPDX-License-Identifier: MIT */
/*
 * libvdma_jpegdec - user-space API for EN683 JPEG Decoder VDMA (SYNC only)
 *
 * Contract:
 *   * Process-singleton. NULL path → /dev/enx_vdma_jdec.
 *   * MOD_INIT 호출 없이 바로 동작.
 *   * SYNC 만 지원. submit 은 HW 완료까지 block.
 *   * dst_buf_size 는 REQUIRED — JPEG 디코딩 출력은 입력에 의존하므로
 *     호출자가 충분한 크기로 dst 를 alloc 하고 그 크기를 명시해야 함.
 *     UAPI 가 강제 검사함.
 */

#ifndef _LIBVDMA_JPEGDEC_H_
#define _LIBVDMA_JPEGDEC_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VDMA_JPEGDEC_ABI_VERSION		1u

typedef void *vdma_addr_t;

/**
 * struct vdma_jpegdec_response - HW 가 채워주는 결과 정보
 * @dst_size:     디코딩된 YUV 의 size (bytes)
 * @cycle:        decoder cycle count
 * @hsize/vsize:  디코딩된 해상도
 * @hsize_wr/vsize_wr: 실제 dst 에 쓰인 해상도 (alignment 적용)
 * @fmt:          디코딩 결과 format (eSRC_* enum 값)
 * @header_*:     JPEG header parse 결과 flags
 * @unsupport_marker: 미지원 marker 정보
 * @header_error: header 파싱 오류 비트
 * @process_error: HW 처리 오류 비트
 */
struct vdma_jpegdec_response {
	uint32_t	dst_size;
	uint32_t	cycle;
	uint16_t	hsize, vsize;
	uint16_t	hsize_wr, vsize_wr;
	int		fmt;
	uint32_t	header_exist;
	uint32_t	unsupport_marker;
	uint32_t	header_error;
	uint32_t	process_error;
};

/**
 * struct vdma_jpegdec_req - one submit call payload
 * @src:               source JPEG bitstream buffer
 * @dst:               destination decoded YUV buffer
 *
 * @src_jpeg_size:     실제 JPEG file size (bytes) — REQUIRED
 * @dst_buf_size:      dst 버퍼 capacity — REQUIRED. 호출자가 미리 계산해서
 *                     충분한 크기로 alloc 한 뒤 그 크기를 그대로 전달.
 *
 * @src_cache_flush:   1 = L2-flush source range
 * @dst_cache_flush:   1 = L2-flush destination range
 *
 * @resp:              OUT — submit 성공 후 HW 결과 echo
 */
struct vdma_jpegdec_req {
	vdma_addr_t		src;
	vdma_addr_t		dst;

	uint32_t		src_jpeg_size;
	uint32_t		dst_buf_size;

	uint8_t			src_cache_flush;
	uint8_t			dst_cache_flush;

	/* OUT */
	struct vdma_jpegdec_response resp;
};

/* ---- Lifecycle ----------------------------------------------------------- */
int  vdma_jpegdec_open  (const char *path);	/* NULL → /dev/enx_vdma_jdec */
void vdma_jpegdec_close (const char *path);

/* ---- Buffer alloc / free ------------------------------------------------ */
vdma_addr_t vdma_jpegdec_alloc_src(size_t size);
vdma_addr_t vdma_jpegdec_alloc_dst(size_t size);
int         vdma_jpegdec_free(vdma_addr_t addr, size_t size);

/* ---- Submission --------------------------------------------------------- */
int vdma_jpegdec_submit(struct vdma_jpegdec_req *req);

/* ---- Cross-fd buffer sharing -------------------------------------------- */
int         vdma_jpegdec_export(vdma_addr_t addr, int *export_fd_out);
vdma_addr_t vdma_jpegdec_import(int export_fd, uint32_t kind,
				size_t *size_out);

/* ---- Helpers — dst capacity 계산 ---------------------------------------- */
/* 예상 디코드 출력 크기. format hint 가 없으면 worst-case YUV444 (w*h*3) 기준. */
size_t vdma_jpegdec_dst_estimate(uint32_t max_width, uint32_t max_height);

#ifdef __cplusplus
}
#endif
#endif /* _LIBVDMA_JPEGDEC_H_ */
