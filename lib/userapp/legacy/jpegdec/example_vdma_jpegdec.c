// SPDX-License-Identifier: MIT
/*
 * example_vdma_jpegdec - usage examples for libvdma_jpegdec (SYNC only)
 *
 *   demo_basic        - input.jpg → 디코드 → out_decoded.yuv 저장
 *   demo_fork_recover - fork 후 자식이 open() 으로 재시작
 *
 * Build:
 *   gcc -std=c11 -O2 -Wall -Wextra example_vdma_jpegdec.c libvdma_jpegdec.c \
 *       -lpthread -o example_vdma_jpegdec
 */

#include "libvdma_jpegdec.h"

#include <linux/types.h>
typedef __u32 u32;
typedef __u64 u64;
typedef __s32 s32;
typedef __u16 u16;
typedef __u8  u8;
#include "en683-jpegdec-uapi.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* -------------------------------------------------------------------------- */
/* Configuration                                                              */
/* -------------------------------------------------------------------------- */
#define INPUT_FILE	"input.jpg"
#define MAX_W		1920
#define MAX_H		1080

static int die(const char *what, int err)
{
	fprintf(stderr, "%s: %s\n", what, strerror(err > 0 ? err : -err));
	return 1;
}

/* file → src buffer 로 읽기. 반환: 실제 읽은 바이트 수, 실패시 < 0 */
static long load_jpeg(const char *path, void *dst, size_t cap)
{
	FILE *fp;
	struct stat st;
	size_t n;

	if (stat(path, &st) < 0) {
		fprintf(stderr, "[load] stat(%s): %s\n", path, strerror(errno));
		return -1;
	}
	if ((size_t)st.st_size > cap) {
		fprintf(stderr, "[load] file (%lld) larger than buffer (%zu)\n",
			(long long)st.st_size, cap);
		return -1;
	}

	fp = fopen(path, "rb");
	if (!fp) {
		fprintf(stderr, "[load] fopen(%s): %s\n", path, strerror(errno));
		return -1;
	}
	n = fread(dst, 1, (size_t)st.st_size, fp);
	fclose(fp);
	if (n != (size_t)st.st_size) {
		fprintf(stderr, "[load] short read: %zu / %lld\n",
			n, (long long)st.st_size);
		return -1;
	}
	printf("[load] %s — %zu bytes\n", path, n);
	return (long)n;
}

static int save_yuv(const char *path, const void *src, size_t size)
{
	FILE *fp = fopen(path, "wb");
	if (!fp) {
		fprintf(stderr, "[save] fopen(%s): %s\n", path, strerror(errno));
		return -1;
	}
	size_t w = fwrite(src, 1, size, fp);
	fclose(fp);
	if (w != size) {
		fprintf(stderr, "[save] short write: %zu / %zu\n", w, size);
		return -1;
	}
	printf("[save] %s — %zu bytes\n", path, size);
	return 0;
}

static const char *fmt_str(int fmt)
{
	switch (fmt) {
	case 0: return "YUV444";
	case 1: return "YUV422";
	case 2: return "YUV420";
	case 3: return "YUV400";
	default: return "?";
	}
}

/* -------------------------------------------------------------------------- */
/* 1. Basic: input.jpg → decoded YUV                                            */
/* -------------------------------------------------------------------------- */
static int demo_basic(void)
{
	/* src capacity: 입력 JPEG 파일 크기는 1MB 정도면 충분. 더 크면 늘리기. */
	const size_t SRC_CAP = 1 * 1024 * 1024;
	/* dst capacity: 최대 해상도 worst-case 로 미리 계산. */
	const size_t DST_CAP = vdma_jpegdec_dst_estimate(MAX_W, MAX_H);

	vdma_addr_t src, dst;
	struct vdma_jpegdec_req req;
	long jpeg_size;
	int r;

	src = vdma_jpegdec_alloc_src(SRC_CAP);
	if (!src) return die("alloc src", errno);
	dst = vdma_jpegdec_alloc_dst(DST_CAP);
	if (!dst) { vdma_jpegdec_free(src, SRC_CAP); return die("alloc dst", errno); }

	jpeg_size = load_jpeg(INPUT_FILE, src, SRC_CAP);
	if (jpeg_size < 0) {
		vdma_jpegdec_free(src, SRC_CAP);
		vdma_jpegdec_free(dst, DST_CAP);
		fprintf(stderr, "[basic] please prepare %s (or rerun with a valid JPEG)\n",
			INPUT_FILE);
		return 1;
	}

	/* loop test */
	while(1) {
		memset(dst, 0, DST_CAP);

		req = (struct vdma_jpegdec_req){
			.src              = src,
			.dst              = dst,
			.src_jpeg_size    = (uint32_t)jpeg_size,
			.dst_buf_size     = (uint32_t)DST_CAP,
			.src_cache_flush  = 1,
			.dst_cache_flush  = 0,
		};

		r = vdma_jpegdec_submit(&req);
	}
	printf("[basic] submit=%d  res=%ux%u  fmt=%s  dst_size=%u  cycle=%u\n",
			r, req.resp.hsize, req.resp.vsize,
			fmt_str(req.resp.fmt), req.resp.dst_size, req.resp.cycle);

	if (r == 0 && req.resp.dst_size) {
		char outpath[64];
		snprintf(outpath, sizeof(outpath),
			 "out_decoded_%ux%u_%s.yuv",
			 req.resp.hsize_wr, req.resp.vsize_wr,
			 fmt_str(req.resp.fmt));
		save_yuv(outpath, dst, req.resp.dst_size);
	} else if (r == 0) {
		fprintf(stderr, "[basic] decoded successfully but dst_size=0?\n");
	} else if (req.resp.process_error) {
		fprintf(stderr, "[basic] process_error: 0x%08X\n", req.resp.process_error);
	}

	vdma_jpegdec_free(src, SRC_CAP);
	vdma_jpegdec_free(dst, DST_CAP);
	return r;
}

/* -------------------------------------------------------------------------- */
/* 2. fork 후 자식이 open() 으로 재시작                                          */
/* -------------------------------------------------------------------------- */
static int demo_fork_recover(void)
{
	pid_t pid = fork();
	if (pid == 0) {
		vdma_addr_t addr = vdma_jpegdec_alloc_src(4096);
		if (addr || errno != ENODEV) {
			fprintf(stderr, "[child] expected ENODEV, got %p errno=%d\n",
				addr, errno);
			if (addr) vdma_jpegdec_free(addr, 4096);
			_exit(1);
		}
		if (vdma_jpegdec_open(NULL) < 0) {
			fprintf(stderr, "[child] re-open failed\n");
			_exit(1);
		}
		addr = vdma_jpegdec_alloc_src(4096);
		if (!addr) {
			vdma_jpegdec_close(NULL);
			_exit(1);
		}
		fprintf(stderr, "[child] OK\n");
		vdma_jpegdec_free(addr, 4096);
		vdma_jpegdec_close(NULL);
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

	if (vdma_jpegdec_open(path) < 0) {
		fprintf(stderr, "vdma_jpegdec_open(%s): %s\n",
			path ? path : "<default>", strerror(errno));
		return 1;
	}

	demo_basic();
	demo_fork_recover();

	vdma_jpegdec_close(path);
	return 0;
}
