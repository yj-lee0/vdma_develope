// SPDX-License-Identifier: MIT
/*
 * example_vdma_scaler - usage examples for libvdma_scaler (SYNC only)
 *
 *   demo_downscale     - 1920x1080 (YUV420SP) → 640x360 다운스케일
 *   demo_format_conv   - YUV420SP → YUV422_UYVY 포맷 변환
 *   demo_crop_flip     - 일부 영역만 crop + flip
 *   demo_fork_recover  - fork 후 자식이 open() 으로 재시작
 *
 * Build:
 *   gcc -std=c11 -O2 -Wall -Wextra example_vdma_scaler.c libvdma_scaler.c \
 *       -lpthread -o example_vdma_scaler
 */

#include "libvdma_scaler.h"

/* enum enx_vdma_format 값 그대로 사용. UAPI shim 은 lib 내부에서 처리. */
#include <linux/types.h>
typedef __u32 u32;
typedef __u64 u64;
typedef __s32 s32;
typedef __u16 u16;
typedef __u8  u8;
#include "en683-dz-uapi.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* -------------------------------------------------------------------------- */
/* 기본 입력: 1920x1080 YUV420SP. 미존재 시 checkerboard 자동 생성.            */
/* -------------------------------------------------------------------------- */
#define INPUT_FILE	"input_1920x1080.yuv"
#define SRC_W		1920
#define SRC_H		1080
#define SRC_SZ		((SRC_W) * (SRC_H) * 3 / 2)	/* YUV420SP */

static int die(const char *what, int err)
{
	fprintf(stderr, "%s: %s\n", what, strerror(err > 0 ? err : -err));
	return 1;
}

/* 입력 파일이 없을 때 간단한 그라데이션 + 체커보드 패턴을 src 에 채움 */
static void fill_test_pattern(void *yuv, int w, int h)
{
	uint8_t *y  = yuv;
	uint8_t *uv = (uint8_t *)yuv + w * h;

	for (int j = 0; j < h; j++) {
		for (int i = 0; i < w; i++) {
			int cx = (i / 64) & 1;
			int cy = (j / 64) & 1;
			y[j * w + i] = (cx ^ cy) ? (uint8_t)(i * 255 / w)
						  : (uint8_t)(j * 255 / h);
		}
	}
	for (int j = 0; j < h / 2; j++) {
		for (int i = 0; i < w / 2; i++) {
			uv[(j * (w / 2) + i) * 2 + 0] = (uint8_t)(0x80 + (i & 0x3F));
			uv[(j * (w / 2) + i) * 2 + 1] = (uint8_t)(0x80 + (j & 0x3F));
		}
	}
}

static int load_src(void *dst_addr, size_t expected)
{
	FILE *fp = fopen(INPUT_FILE, "rb");
	if (!fp) {
		fprintf(stderr, "[load] %s not found — generating checkerboard pattern\n",
			INPUT_FILE);
		fill_test_pattern(dst_addr, SRC_W, SRC_H);
		return 0;
	}
	size_t n = fread(dst_addr, 1, expected, fp);
	fclose(fp);
	if (n != expected) {
		fprintf(stderr, "[load] short read: %zu / %zu\n", n, expected);
		return -1;
	}
	printf("[load] loaded %s (%zu bytes)\n", INPUT_FILE, expected);
	return 0;
}

static int save_dst(const char *path, void *src, size_t bytes)
{
	FILE *fp = fopen(path, "wb");
	if (!fp) {
		fprintf(stderr, "[save] fopen(%s): %s\n", path, strerror(errno));
		return -1;
	}
	size_t w = fwrite(src, 1, bytes, fp);
	fclose(fp);
	if (w != bytes) {
		fprintf(stderr, "[save] short write: %zu / %zu\n", w, bytes);
		return -1;
	}
	printf("[save] wrote %s (%zu bytes)\n", path, bytes);
	return 0;
}

static size_t bytes_for_fmt(uint32_t fmt, int w, int h)
{
	/* 보수적 계산: format 별 bpp 매핑 — DZ 가 받는 'flush size' 와 동일 */
	switch (fmt) {
	/* x3 (24bpp) */
	case eFmt_RGB: case eFmt_RBG: case eFmt_GRB: case eFmt_GBR:
	case eFmt_BRG: case eFmt_BGR:
	case eFmt_RRR: case eFmt_GGG: case eFmt_BBB:
	case eFmt_Rzz: case eFmt_zGz: case eFmt_zzB:
	case eFmt_YUV444: case eFmt_YUV444_Y:
		return (size_t)w * h * 3;
	/* x2 (16bpp) */
	case eFmt_YUV422_YUYV: case eFmt_YUV422_YVYU:
	case eFmt_YUV422_UYVY: case eFmt_YUV422_VYUY:
	case eFmt_YUV422_Y:
		return (size_t)w * h * 2;
	/* x1.5 (12bpp) */
	case eFmt_YUV420SP_NV12: case eFmt_YVU420SP_NV21: case eFmt_YUV420SP_NV_Y:
		return ((size_t)w * h * 3) / 2;
	/* x1 (8bpp) */
	case eFmt_YUV400:
	default:
		return (size_t)w * h;
	}
}

/* -------------------------------------------------------------------------- */
/* 1. Downscale: 1920x1080 → 640x360, 포맷 동일 (YUV420SP)                     */
/* -------------------------------------------------------------------------- */
static int demo_downscale(void)
{
	enum { DST_W = 640, DST_H = 360 };
	size_t dst_sz = (size_t)DST_W * DST_H * 3 / 2;
	vdma_addr_t src, dst;
	struct vdma_scaler_req req;
	char outfile[64];
	int r;

	snprintf(outfile, sizeof(outfile),
		 "scaled_%ux%u.yuv", DST_W, DST_H);

	src = vdma_scaler_alloc_src(SRC_SZ);
	if (!src) return die("alloc src", errno);
	dst = vdma_scaler_alloc_dst(dst_sz);
	if (!dst) { vdma_scaler_free(src, SRC_SZ); return die("alloc dst", errno); }

	if (load_src(src, SRC_SZ) < 0) {
		vdma_scaler_free(src, SRC_SZ);
		vdma_scaler_free(dst, dst_sz);
		return 1;
	}
	memset(dst, 0, dst_sz);
	while(1) {
		req = (struct vdma_scaler_req){
			.src              = src,
			.dst              = dst,

			.src_format       = eFmt_YUV420SP_NV12,
			.src_width        = SRC_W,
			.src_height       = SRC_H,
			.src_width_total  = SRC_W,
			.src_height_total = SRC_H,
			.src_crop_x       = 0,
			.src_crop_y       = 0,

			.dst_format       = eFmt_YUV420SP_NV12,
			.dst_width        = DST_W,
			.dst_height       = DST_H,
			.dst_width_total  = DST_W,

			.vflip            = 0,
			.hflip            = 0,
			.src_cache_flush  = 1,
			.dst_cache_flush  = 1,
		};

		r = vdma_scaler_submit(&req);
	}
	printf("[downscale] submit returned %d\n", r);
	if (r == 0) save_dst(outfile, dst, dst_sz);

	vdma_scaler_free(src, SRC_SZ);
	vdma_scaler_free(dst, dst_sz);
	return r;
}

/* -------------------------------------------------------------------------- */
/* 2. Format convert: YUV420SP → YUV422_UYVY (해상도 유지)                     */
/* -------------------------------------------------------------------------- */
static int demo_format_conv(void)
{
	size_t dst_sz = bytes_for_fmt(eFmt_YUV422_UYVY, SRC_W, SRC_H);
	vdma_addr_t src, dst;
	struct vdma_scaler_req req;
	char outfile[64];
	int r;

	snprintf(outfile, sizeof(outfile),
		 "conv_uyvy_%ux%u.yuv", SRC_W, SRC_H);

	src = vdma_scaler_alloc_src(SRC_SZ);
	if (!src) return die("alloc src", errno);
	dst = vdma_scaler_alloc_dst(dst_sz);
	if (!dst) { vdma_scaler_free(src, SRC_SZ); return die("alloc dst", errno); }

	if (load_src(src, SRC_SZ) < 0) {
		vdma_scaler_free(src, SRC_SZ);
		vdma_scaler_free(dst, dst_sz);
		return 1;
	}
	memset(dst, 0, dst_sz);

	req = (struct vdma_scaler_req){
		.src              = src,
		.dst              = dst,

		.src_format       = eFmt_YUV420SP_NV12,
		.src_width        = SRC_W,
		.src_height       = SRC_H,
		.src_width_total  = SRC_W,
		.src_height_total = SRC_H,

		.dst_format       = eFmt_YUV422_UYVY,
		.dst_width        = SRC_W,
		.dst_height       = SRC_H,
		.dst_width_total  = SRC_W,

		.src_cache_flush  = 1,
		.dst_cache_flush  = 1,
	};

	r = vdma_scaler_submit(&req);
	printf("[conv] submit returned %d\n", r);
	if (r == 0) save_dst(outfile, dst, dst_sz);

	vdma_scaler_free(src, SRC_SZ);
	vdma_scaler_free(dst, dst_sz);
	return r;
}

/* -------------------------------------------------------------------------- */
/* 3. Crop + flip: 1920x1080 중앙 1280x720 영역만 추출하고 수평 flip          */
/* -------------------------------------------------------------------------- */
static int demo_crop_flip(void)
{
	enum { CROP_W = 1280, CROP_H = 720 };
	enum { CROP_X = (SRC_W - CROP_W) / 2, CROP_Y = (SRC_H - CROP_H) / 2 };
	size_t dst_sz = (size_t)CROP_W * CROP_H * 3 / 2;
	vdma_addr_t src, dst;
	struct vdma_scaler_req req;
	char outfile[64];
	int r;

	snprintf(outfile, sizeof(outfile),
		 "crop_hflip_%ux%u.yuv", CROP_W, CROP_H);

	src = vdma_scaler_alloc_src(SRC_SZ);
	if (!src) return die("alloc src", errno);
	dst = vdma_scaler_alloc_dst(dst_sz);
	if (!dst) { vdma_scaler_free(src, SRC_SZ); return die("alloc dst", errno); }

	if (load_src(src, SRC_SZ) < 0) {
		vdma_scaler_free(src, SRC_SZ);
		vdma_scaler_free(dst, dst_sz);
		return 1;
	}
	memset(dst, 0, dst_sz);

	req = (struct vdma_scaler_req){
		.src              = src,
		.dst              = dst,

		.src_format       = eFmt_YUV420SP_NV12,
		.src_width        = CROP_W,
		.src_height       = CROP_H,
		.src_width_total  = SRC_W,
		.src_height_total = SRC_H,
		.src_crop_x       = CROP_X,
		.src_crop_y       = CROP_Y,

		.dst_format       = eFmt_YUV420SP_NV12,
		.dst_width        = CROP_W,
		.dst_height       = CROP_H,
		.dst_width_total  = CROP_W,

		.hflip            = 1,
		.src_cache_flush  = 1,
		.dst_cache_flush  = 1,
	};

	r = vdma_scaler_submit(&req);
	printf("[crop+flip] submit returned %d\n", r);
	if (r == 0) save_dst(outfile, dst, dst_sz);

	vdma_scaler_free(src, SRC_SZ);
	vdma_scaler_free(dst, dst_sz);
	return r;
}

/* -------------------------------------------------------------------------- */
/* 4. fork 후 자식이 open() 으로 깨끗하게 재시작                                 */
/* -------------------------------------------------------------------------- */
static int demo_fork_recover(void)
{
	pid_t pid = fork();
	if (pid == 0) {
		vdma_addr_t addr = vdma_scaler_alloc_src(4096);
		if (addr || errno != ENODEV) {
			fprintf(stderr, "[child] expected ENODEV before open, "
				"got %p errno=%d\n", addr, errno);
			if (addr) vdma_scaler_free(addr, 4096);
			_exit(1);
		}
		if (vdma_scaler_open(NULL) < 0) {
			fprintf(stderr, "[child] re-open failed: %s\n",
				strerror(errno));
			_exit(1);
		}
		addr = vdma_scaler_alloc_src(4096);
		if (!addr) {
			fprintf(stderr, "[child] alloc after re-open failed\n");
			vdma_scaler_close(NULL);
			_exit(1);
		}
		fprintf(stderr, "[child] OK: ENODEV before open, alloc OK after\n");
		vdma_scaler_free(addr, 4096);
		vdma_scaler_close(NULL);
		_exit(0);
	}

	int st;
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR);
	return 0;
}

/* -------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	const char *path = (argc > 1) ? argv[1] : NULL;

	if (vdma_scaler_open(path) < 0) {
		fprintf(stderr, "vdma_scaler_open(%s): %s\n",
			path ? path : "<default>", strerror(errno));
		return 1;
	}

	demo_downscale();
	demo_format_conv();
	demo_crop_flip();
	demo_fork_recover();

	vdma_scaler_close(path);
	return 0;
}
