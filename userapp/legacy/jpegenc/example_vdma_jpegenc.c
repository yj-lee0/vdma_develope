// SPDX-License-Identifier: MIT
/*
 * example_vdma_jpegenc - usage examples for libvdma_jpegenc (SYNC only)
 *
 *   demo_basic        - 1920x1080 YUV → JPEG (quality 90)
 *   demo_crop         - 중앙 1280x720 영역만 JPEG 로
 *   demo_quality_loop - quality 50/75/95 비교
 *   demo_fork_recover - fork 후 자식이 open() 으로 재시작
 *
 * Build:
 *   gcc -std=c11 -O2 -Wall -Wextra example_vdma_jpegenc.c libvdma_jpegenc.c \
 *       -lpthread -o example_vdma_jpegenc
 */

#include "libvdma_jpegenc.h"

#include <linux/types.h>
typedef __u32 u32;
typedef __u64 u64;
typedef __s32 s32;
typedef __u16 u16;
typedef __u8  u8;
#include "en683-jpegenc-uapi.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* -------------------------------------------------------------------------- */
/* 입력: 1920x1080 YUV420SP. 없으면 checkerboard 자동 생성.                    */
/* -------------------------------------------------------------------------- */
#define INPUT_FILE	"input_1920x1080.yuv"
#define SRC_W		1920
#define SRC_H		1080
#define SRC_FMT		eFmt_YUV420SP_NV12
#define SRC_SZ		((SRC_W) * (SRC_H) * 3 / 2)

static int die(const char *what, int err)
{
	fprintf(stderr, "%s: %s\n", what, strerror(err > 0 ? err : -err));
	return 1;
}

static void fill_test_pattern(void *yuv, int w, int h)
{
	uint8_t *y  = yuv;
	uint8_t *uv = (uint8_t *)yuv + w * h;

	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++) {
			int cx = (i / 64) & 1;
			int cy = (j / 64) & 1;
			y[j * w + i] = (cx ^ cy) ? (uint8_t)(i * 255 / w)
						  : (uint8_t)(j * 255 / h);
		}
	for (int j = 0; j < h / 2; j++)
		for (int i = 0; i < w / 2; i++) {
			uv[(j * (w / 2) + i) * 2 + 0] = (uint8_t)(0x80 + (i & 0x3F));
			uv[(j * (w / 2) + i) * 2 + 1] = (uint8_t)(0x80 + (j & 0x3F));
		}
}

static int load_src(void *dst, size_t expected)
{
	FILE *fp = fopen(INPUT_FILE, "rb");
	if (!fp) {
		fprintf(stderr, "[load] %s not found — generating pattern\n",
			INPUT_FILE);
		fill_test_pattern(dst, SRC_W, SRC_H);
		return 0;
	}
	size_t n = fread(dst, 1, expected, fp);
	fclose(fp);
	if (n != expected) {
		fprintf(stderr, "[load] short read: %zu / %zu\n", n, expected);
		return -1;
	}
	printf("[load] loaded %s (%zu bytes)\n", INPUT_FILE, expected);
	return 0;
}

static int save_jpeg(const char *path, const void *data, size_t size)
{
	FILE *fp = fopen(path, "wb");
	if (!fp) {
		fprintf(stderr, "[save] fopen(%s): %s\n", path, strerror(errno));
		return -1;
	}
	size_t w = fwrite(data, 1, size, fp);
	fclose(fp);
	if (w != size) {
		fprintf(stderr, "[save] short write: %zu / %zu\n", w, size);
		return -1;
	}
	printf("[save] wrote %s (%zu bytes)\n", path, size);
	return 0;
}

/* -------------------------------------------------------------------------- */
/* 1. Basic: 1920x1080 YUV420SP → JPEG (quality 90)                            */
/* -------------------------------------------------------------------------- */
static int demo_basic(void)
{
	size_t dst_cap = vdma_jpegenc_dst_safe_size(SRC_FMT, SRC_W, SRC_H);
	vdma_addr_t src, dst;
	struct vdma_jpegenc_req req;
	int r;

	src = vdma_jpegenc_alloc_src(SRC_SZ);
	if (!src) return die("alloc src", errno);
	dst = vdma_jpegenc_alloc_dst(dst_cap);
	if (!dst) { vdma_jpegenc_free(src, SRC_SZ); return die("alloc dst", errno); }

	if (load_src(src, SRC_SZ) < 0) {
		vdma_jpegenc_free(src, SRC_SZ);
		vdma_jpegenc_free(dst, dst_cap);
		return 1;
	}
	while (1) {
		req = (struct vdma_jpegenc_req){
			.src				= src,
			.dst				= dst,
			.src_format			= SRC_FMT,
			.src_width_total	= SRC_W,
			.src_height_total	= SRC_H,
			.src_width			= SRC_W,
			.src_height			= SRC_H,
			.quality			= 90,
			.src_cache_flush	= 1,
			.dst_cache_flush	= 1,
		};

		r = vdma_jpegenc_submit(&req);
	}
	printf("[basic] submit=%d  jpeg_size=%u  cycle=%u  ovf=%d\n",
	       r, req.resp.jpeg_size, req.resp.cycle, req.resp.err_file_ovf);
	if (r == 0 && req.resp.jpeg_size)
		save_jpeg("out_basic_q90.jpg", dst, req.resp.jpeg_size);

	vdma_jpegenc_free(src, SRC_SZ);
	vdma_jpegenc_free(dst, dst_cap);
	return r;
}

/* -------------------------------------------------------------------------- */
/* 2. Crop: 중앙 1280x720 만 JPEG 로                                           */
/* -------------------------------------------------------------------------- */
static int demo_crop(void)
{
	enum { CW = 1280, CH = 720 };
	enum { CX = (SRC_W - CW) / 2, CY = (SRC_H - CH) / 2 };
	size_t dst_cap = vdma_jpegenc_dst_safe_size(SRC_FMT, CW, CH);
	vdma_addr_t src, dst;
	struct vdma_jpegenc_req req;
	int r;

	src = vdma_jpegenc_alloc_src(SRC_SZ);
	if (!src) return die("alloc src", errno);
	dst = vdma_jpegenc_alloc_dst(dst_cap);
	if (!dst) { vdma_jpegenc_free(src, SRC_SZ); return die("alloc dst", errno); }

	if (load_src(src, SRC_SZ) < 0) {
		vdma_jpegenc_free(src, SRC_SZ);
		vdma_jpegenc_free(dst, dst_cap);
		return 1;
	}

	req = (struct vdma_jpegenc_req){
		.src              = src,
		.dst              = dst,
		.src_format       = SRC_FMT,
		.src_width_total  = SRC_W,
		.src_height_total = SRC_H,
		.src_width        = CW,
		.src_height       = CH,
		.src_width_pos    = CX,
		.src_height_pos   = CY,
		.quality          = 85,
		.src_cache_flush  = 1,
		.dst_cache_flush  = 1,
	};

	r = vdma_jpegenc_submit(&req);
	printf("[crop] submit=%d  jpeg_size=%u (%dx%d crop)\n",
	       r, req.resp.jpeg_size, CW, CH);
	if (r == 0 && req.resp.jpeg_size)
		save_jpeg("out_crop_1280x720.jpg", dst, req.resp.jpeg_size);

	vdma_jpegenc_free(src, SRC_SZ);
	vdma_jpegenc_free(dst, dst_cap);
	return r;
}

/* -------------------------------------------------------------------------- */
/* 3. Quality 비교: 50 / 75 / 95                                                */
/* -------------------------------------------------------------------------- */
static int demo_quality_loop(void)
{
	const uint32_t qs[] = { 50, 75, 95 };
	size_t dst_cap = vdma_jpegenc_dst_safe_size(SRC_FMT, SRC_W, SRC_H);
	vdma_addr_t src, dst;
	int rc = 0;

	src = vdma_jpegenc_alloc_src(SRC_SZ);
	if (!src) return die("alloc src", errno);
	dst = vdma_jpegenc_alloc_dst(dst_cap);
	if (!dst) { vdma_jpegenc_free(src, SRC_SZ); return die("alloc dst", errno); }

	if (load_src(src, SRC_SZ) < 0) {
		vdma_jpegenc_free(src, SRC_SZ);
		vdma_jpegenc_free(dst, dst_cap);
		return 1;
	}

	for (size_t i = 0; i < sizeof(qs) / sizeof(qs[0]); i++) {
		char path[64];
		struct vdma_jpegenc_req req = {
			.src				= src,
			.dst				= dst,
			.src_format			= SRC_FMT,
			.src_width_total	= SRC_W,
			.src_height_total	= SRC_H,
			.src_width			= SRC_W,
			.src_height			= SRC_H,
			.quality			= qs[i],
			.src_cache_flush	= 1,
			.dst_cache_flush	= 1,
		};
		int r = vdma_jpegenc_submit(&req);
		printf("[qloop] q=%u  submit=%d  jpeg_size=%u  cycle=%u\n",
		       qs[i], r, req.resp.jpeg_size, req.resp.cycle);
		if (r == 0 && req.resp.jpeg_size) {
			snprintf(path, sizeof(path), "out_q%u.jpg", qs[i]);
			save_jpeg(path, dst, req.resp.jpeg_size);
		}
		if (r) rc = r;
	}

	vdma_jpegenc_free(src, SRC_SZ);
	vdma_jpegenc_free(dst, dst_cap);
	return rc;
}

/* -------------------------------------------------------------------------- */
/* 4. fork 후 자식이 open() 으로 재시작                                          */
/* -------------------------------------------------------------------------- */
static int demo_fork_recover(void)
{
	pid_t pid = fork();
	if (pid == 0) {
		vdma_addr_t addr = vdma_jpegenc_alloc_src(4096);
		if (addr || errno != ENODEV) {
			fprintf(stderr, "[child] expected ENODEV, got %p errno=%d\n",
				addr, errno);
			if (addr) vdma_jpegenc_free(addr, 4096);
			_exit(1);
		}
		if (vdma_jpegenc_open(NULL) < 0) {
			fprintf(stderr, "[child] re-open failed\n");
			_exit(1);
		}
		addr = vdma_jpegenc_alloc_src(4096);
		if (!addr) {
			vdma_jpegenc_close(NULL);
			_exit(1);
		}
		fprintf(stderr, "[child] OK\n");
		vdma_jpegenc_free(addr, 4096);
		vdma_jpegenc_close(NULL);
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

	if (vdma_jpegenc_open(path) < 0) {
		fprintf(stderr, "vdma_jpegenc_open(%s): %s\n",
			path ? path : "<default>", strerror(errno));
		return 1;
	}

	demo_basic();
	demo_crop();
	demo_quality_loop();
	demo_fork_recover();

	vdma_jpegenc_close(path);
	return 0;
}
