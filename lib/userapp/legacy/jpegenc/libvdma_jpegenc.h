/* SPDX-License-Identifier: MIT */
/*
 * libvdma_jpegenc - user-space API for EN683 JPEG Encoder VDMA (SYNC only)
 *
 * Contract:
 *   * Process-singleton — 한 프로세스 안에서 디바이스 fd 1개만 유지.
 *   * vdma_jpegenc_open() / vdma_jpegenc_close() 는 refcount 매칭. NULL path 는
 *     /dev/enx_vdma_jenc.
 *   * MOD_INIT 호출 없이 바로 동작 (mode 분기 없음).
 *   * fork() 후 자식은 라이브러리 상태가 무효화되어 모든 API 가
 *     -EOWNERDEAD / -ENODEV 반환.
 *   * SYNC 만 지원. submit 은 HW 완료까지 block.
 *   * dst 버퍼는 submit 리턴 이후 JPEG bitstream 을 담음. 유효 크기는
 *     vdma_jpegenc_req.resp.jpeg_size 로 확인.
 *
 * JPEG 출력 크기 가이드:
 *   인코더 출력 크기는 사전 예측 불가. dst 버퍼는 worst-case 크기로 잡아야
 *   안전. helper vdma_jpegenc_dst_safe_size() / vdma_jpegenc_dst_estimate()
 *   사용 권장.
 */

#ifndef _LIBVDMA_JPEGENC_H_
#define _LIBVDMA_JPEGENC_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VDMA_JPEGENC_ABI_VERSION		1u

typedef void *vdma_addr_t;

/**
 * struct vdma_jpegenc_response - HW 가 채워주는 결과 정보
 * @cycle:        encoder 가 소요한 cycle count
 * @jpeg_size:    실제 JPEG bitstream 크기 (bytes)
 * @err_occur:    error 발생 flag
 * @err_file_ovf: dst 버퍼 overflow 발생 flag
 */
struct vdma_jpegenc_response {
	uint32_t	cycle;
	uint32_t	jpeg_size;
	uint8_t		err_occur;
	uint8_t		err_file_ovf;
};

/**
 * struct vdma_jpegenc_req - one submit call payload
 * @src:               source raw YUV buffer
 * @dst:               destination JPEG bitstream buffer (worst-case sized)
 *
 * @src_format:        enum enx_vdma_format 값
 * @src_width_total:   source physical pitch (pixels)
 * @src_height_total:  source physical height (pixels)
 *
 * @src_width:         crop width  (0 = no crop, encode 전체)
 * @src_height:        crop height (0 = no crop)
 * @src_width_pos:     crop x offset
 * @src_height_pos:    crop y offset
 *
 * @quality:           JPEG quality (1~100)
 * @ovf_threshold:     overflow threshold (0 = dst capacity 사용)
 *
 * @src_cache_flush:   1 = L2-flush source range
 * @dst_cache_flush:   1 = L2-flush destination range
 *
 * @resp:              OUT — submit 성공 후 HW 결과 echo
 */
struct vdma_jpegenc_req {
	vdma_addr_t		src;
	vdma_addr_t		dst;

	uint32_t		src_format;
	uint16_t		src_width_total;
	uint16_t		src_height_total;

	uint16_t		src_width;
	uint16_t		src_height;
	uint16_t		src_width_pos;
	uint16_t		src_height_pos;

	uint32_t		quality;
	uint32_t		ovf_threshold;

	uint8_t			src_cache_flush;
	uint8_t			dst_cache_flush;

	/* OUT — submit 성공 후 채워짐 */
	struct vdma_jpegenc_response resp;
};

/* ---- Lifecycle (refcounted per device path) ------------------------------ */
int  vdma_jpegenc_open  (const char *path);	/* NULL → /dev/enx_vdma_jenc */
void vdma_jpegenc_close (const char *path);

/* ---- Buffer alloc / free ------------------------------------------------ */
vdma_addr_t vdma_jpegenc_alloc_src(size_t size);
vdma_addr_t vdma_jpegenc_alloc_dst(size_t size);
int         vdma_jpegenc_free(vdma_addr_t addr, size_t size);

/* ---- Submission --------------------------------------------------------- */
/* SYNC: HW 완료까지 block. 성공 시 req->resp 가 채워짐.
 * 반환: 0 (성공), -ENOSPC (dst overflow), 기타 음수 errno. */
int vdma_jpegenc_submit(struct vdma_jpegenc_req *req);

/* ---- Cross-fd buffer sharing -------------------------------------------- */
int         vdma_jpegenc_export(vdma_addr_t addr, int *export_fd_out);
vdma_addr_t vdma_jpegenc_import(int export_fd, uint32_t kind,
				size_t *size_out);

/* ---- Helpers — dst capacity 계산 ---------------------------------------- */

/* worst-case bound: 어떤 입력이든 안전. raw size 의 ~1.1× + header overhead. */
size_t vdma_jpegenc_dst_safe_size(uint32_t format, uint32_t width, uint32_t height);

/* quality 기반 추정: 메모리 효율, 드물게 overflow 가능. */
size_t vdma_jpegenc_dst_estimate(uint32_t format, uint32_t width, uint32_t height,
				 uint32_t quality);

#ifdef __cplusplus
}
#endif
#endif /* _LIBVDMA_JPEGENC_H_ */
