/* SPDX-License-Identifier: MIT */
/*
 * libvdma_font - user-space API for EN683 Font VDMA (SYNC only)
 *
 * Contract:
 *   * Process-singleton 모델 — 한 프로세스 안에서 디바이스 fd 를 1개만 유지.
 *     라이브러리가 내부적으로 fd 와 버퍼 자료구조를 모두 관리. 외부에는
 *     mmap 가상주소(vdma_addr_t)만 노출.
 *   * vdma_font_open() / vdma_font_close() 는 refcount 로 매칭. 두 번째
 *     이후의 open() 에서 path 가 다르면 -EBUSY. NULL path 는
 *     /dev/vdma_font (기본 디바이스).
 *   * 첫 open() 이 자동으로 KERNEL 모드 진입 (VDMAIOSET_MOD_INIT).
 *     VIDEO_CORE 모드는 본 라이브러리에서 지원하지 않음 (필요 시 별도 ioctl
 *     직접 호출).
 *   * fork() 후 자식은 라이브러리 상태가 무효화되어 모든 API 가
 *     -EOWNERDEAD / -ENODEV 반환. 자식이 새로 open() 해야 사용 가능.
 *   * SYNC 만 지원. submit 은 HW 완료까지 block. SIGKILL 외의 시그널은
 *     커널 측에서 흡수됨.
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
 *
 *   ASYNC 는 아직 미지원입니다. 도입 계획은 ref_md/ref_submit-v2.md 참고.
 */

#ifndef _LIBVDMA_FONT_H_
#define _LIBVDMA_FONT_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VDMA_FONT_ABI_VERSION		4u

/* 사용자에게 노출되는 유일한 식별자. mmap 한 가상주소 그대로 — 그 위에
 * memset/memcpy 등 일반 메모리 연산 가능. */
typedef void *vdma_addr_t;

/**
 * struct vdma_blit - per-glyph blit parameters
 * @src:           source glyph buffer (allocated by vdma_font_alloc_src)
 * @width, @height: source bitmap dimensions
 * @dst_x, @dst_y: destination position (in pixels) on the canvas
 * @font_color:    packed YCbCr font color (Y<<16 | Cb<<8 | Cr)
 * @outline_color: packed YCbCr outline color
 * @bg_color:      packed YCbCr background color
 * @font_value:    threshold value for font_color match (8-bit)
 * @outline_value: threshold value for outline_color match (8-bit)
 * @wr_mode:       write mode (0:YC RW, 1:Y RW, 2:YC Wonly, 3:Y Wonly)
 * @color_tone:    0 = use src bitmap color, 1 = use @font_color
 * @alpha:         blending alpha (0..256)
 * @threshold:     threshold for src bitmap (0..255)
 */
struct vdma_blit {
	vdma_addr_t	src;
	uint32_t	width, height;
	uint32_t	dst_x, dst_y;
	uint32_t	font_color;		/* YCbCr packed */
	uint32_t	outline_color;
	uint32_t	bg_color;
	uint32_t	font_value;
	uint32_t	outline_value;
	uint32_t	wr_mode;
	uint32_t	color_tone;
	uint32_t	alpha;
	uint32_t	threshold;
};

/**
 * struct vdma_submit_req - one submit call payload
 * @dst:                destination canvas buffer (allocated by vdma_font_alloc_dst)
 * @dst_width:          canvas width  (KERNEL mode only)
 * @dst_height:         canvas height (KERNEL mode only)
 * @background_overlay: 0 = zero dst before draw, 1 = overlay on existing content
 * @blits:              array of per-glyph blit descriptors
 * @nblit:              number of entries in @blits
 */
struct vdma_submit_req {
	vdma_addr_t			dst;
	uint32_t			dst_width;
	uint32_t			dst_height;
	uint32_t			background_overlay;
	const struct vdma_blit	       *blits;
	size_t				nblit;
};

/* ---- Lifecycle (refcounted per device path) ------------------------------ */
int  vdma_font_open    (const char *path);	/* NULL → /dev/vdma_font */
void vdma_font_close   (const char *path);

/* ---- Buffer alloc / free (vir_addr 기반) -------------------------------- */
/* 성공 시 mmap 된 가상주소 반환, 실패 시 NULL + errno. */
vdma_addr_t vdma_font_alloc_src(size_t size);
vdma_addr_t vdma_font_alloc_dst(size_t size);

/* size 가 0 이면 size 검증 생략, 0 이 아니면 alloc 시 size 와 일치해야 함. */
int vdma_font_free(vdma_addr_t addr, size_t size);

/* ---- Submission --------------------------------------------------------- */
/* SYNC: HW 완료까지 블록. 반환 = HW result (0 또는 음수 errno). */
int vdma_font_submit(const struct vdma_submit_req *req);

/* ---- Cross-fd buffer sharing (anon_inode based) ------------------------- */
/* Producer 가 자기 addr 를 다른 프로세스에 보낼 수 있는 fd 로 변환.
 * 호출자가 fd 소유, close 책임. */
int vdma_font_export(vdma_addr_t addr, int *export_fd_out);

/* SCM_RIGHTS 로 받은 fd 를 import. 성공 시 새 mmap 가상주소 반환.
 * size_out / kind_out 은 NULL 허용. */
vdma_addr_t vdma_font_import(int export_fd,
			     size_t *size_out, uint32_t *kind_out);

#ifdef __cplusplus
}
#endif
#endif /* _LIBVDMA_FONT_H_ */
