// SPDX-License-Identifier: MIT
/*
 * example_vdma_font - usage examples for libvdma_font (SYNC only)
 *
 * demo_sync         - 가장 단순한 한 번의 동기 호출
 * demo_multi_blit   - 한 submit 에 N 개의 glyph 합성
 * demo_fork_recover - fork 후 자식이 open() 으로 깨끗하게 재시작
 *
 * Build:
 *   gcc -std=c11 -O2 -Wall -Wextra example_vdma_font.c libvdma_font.c \
 *       -lpthread -o example_vdma_font
 */
#define _POSIX_C_SOURCE 199309L	/* for clock_gettime, struct timespec */
#include "libvdma_font.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

#define DST_W	1920
#define DST_H	1080
#define DST_SZ	((DST_W) * (DST_H) * 3 / 2)	/* YUV420SP — Y + UV/2 */

static int die(const char *what, int err)
{
	fprintf(stderr, "%s: %s\n", what, strerror(err > 0 ? err : -err));
	return 1;
}

/* -------------------------------------------------------------------------- */
/* 1. SYNC: 파일 입력 → 합성 → 파일 출력                                       */
/* -------------------------------------------------------------------------- */
#define INPUT_FILE	"font_270x122_TEST.yuv"
#define IN_W		270
#define IN_H		122
#define IN_SZ		((IN_W) * (IN_H))

static int demo_sync(void)
{
	char outfile[64];
	vdma_addr_t dst, src;
	struct vdma_blit blit;
	struct vdma_submit_req req;
	FILE *fp;
	size_t n;
	int r;

	snprintf(outfile, sizeof(outfile), "output_%ux%u.yuv", DST_W, DST_H);

	/* alloc */
	dst = vdma_font_alloc_dst(DST_SZ);
	if (!dst) return die("alloc dst", errno);
	src = vdma_font_alloc_src(IN_SZ);
	if (!src) { vdma_font_free(dst, DST_SZ); return die("alloc src", errno); }

	/* load src from file */
	fp = fopen(INPUT_FILE, "rb");
	if (!fp) {
		fprintf(stderr, "[sync] fopen(%s): %s\n",
			INPUT_FILE, strerror(errno));
		vdma_font_free(src, IN_SZ);
		vdma_font_free(dst, DST_SZ);
		return 1;
	}
	n = fread(src, 1, IN_SZ, fp);
	fclose(fp);
	if (n != IN_SZ) {
		fprintf(stderr, "[sync] short read: got %zu / expected %u\n",
			n, IN_SZ);
		vdma_font_free(src, IN_SZ);
		vdma_font_free(dst, DST_SZ);
		return 1;
	}
	printf("[sync] loaded %s (%ux%u, %u bytes)\n",
	       INPUT_FILE, IN_W, IN_H, IN_SZ);

	struct timespec ts = {0, 100 * 1000 * 1000}; // 100ms
	while(1) {
		/* clear dst canvas */
		memset(dst, 0, DST_SZ);

		blit = (struct vdma_blit){
			.src		= src,
			.width		= IN_W,		.height		= IN_H,
			.dst_x		= 100,		.dst_y		= 100,
			.font_color	= 0xFFFFFF,	/* Y=0xFF, Cb=0xFF, Cr=0xFF */
			.font_value	= 0xFF,
			.alpha		= 256,
			.color_tone	= 1,
			.threshold	= 0,
		};
		req = (struct vdma_submit_req){
			.dst                = dst,
			.dst_width          = DST_W,
			.dst_height         = DST_H,
			.background_overlay = 0,
			.blits              = &blit,
			.nblit              = 1,
		};

		r = vdma_font_submit(&req);
		printf("[sync] submit returned %d\n", r);

		if (0) {//(r == 0) {
			fp = fopen(outfile, "wb");
			if (!fp) {
				fprintf(stderr, "[sync] fopen(%s): %s\n",
					outfile, strerror(errno));
				r = 1;
			} else {
				size_t w = fwrite(dst, 1, DST_SZ, fp);
				fclose(fp);
				if (w != DST_SZ) {
					fprintf(stderr,
							"[sync] short write: %zu / %u\n",
							w, DST_SZ);
					r = 1;
				} else {
					printf("[sync] wrote %s (%ux%u, %u bytes)\n",
						outfile, DST_W, DST_H, DST_SZ);
				}
			}
		}
		nanosleep(&ts, NULL);
	}

	vdma_font_free(src, IN_SZ);
	vdma_font_free(dst, DST_SZ);
	return r;
}

/* -------------------------------------------------------------------------- */
/* 2. SYNC: 파일 입력 → grid 합성 → 파일 출력                                   */
/* -------------------------------------------------------------------------- */
static int demo_multi_blit(void)
{
	enum { COLS = 4, ROWS = 2, N = COLS * ROWS, GAP = 10 };
	char outfile[64];
	vdma_addr_t dst, src;
	struct vdma_blit blits[N];
	struct vdma_submit_req req;
	FILE *fp;
	size_t n;
	int r;

	snprintf(outfile, sizeof(outfile),
		 "output_multi_%ux%u.yuv", DST_W, DST_H);

	/* alloc */
	dst = vdma_font_alloc_dst(DST_SZ);
	if (!dst) return die("alloc dst", errno);
	src = vdma_font_alloc_src(IN_SZ);
	if (!src) { vdma_font_free(dst, DST_SZ); return die("alloc src", errno); }

	/* load src from file */
	fp = fopen(INPUT_FILE, "rb");
	if (!fp) {
		fprintf(stderr, "[multi] fopen(%s): %s\n",
			INPUT_FILE, strerror(errno));
		vdma_font_free(src, IN_SZ);
		vdma_font_free(dst, DST_SZ);
		return 1;
	}
	n = fread(src, 1, IN_SZ, fp);
	fclose(fp);
	if (n != IN_SZ) {
		fprintf(stderr, "[multi] short read: got %zu / expected %u\n",
			n, IN_SZ);
		vdma_font_free(src, IN_SZ);
		vdma_font_free(dst, DST_SZ);
		return 1;
	}
	printf("[multi] loaded %s (%ux%u, %u bytes)\n",
	       INPUT_FILE, IN_W, IN_H, IN_SZ);

	/* clear dst canvas */
	memset(dst, 0, DST_SZ);

	/* grid layout: COLS×ROWS glyphs starting at (50, 50) */
	for (int i = 0; i < N; i++) {
		int col = i % COLS;
		int row = i / COLS;
		blits[i] = (struct vdma_blit){
			.src		= src,
			.width		= IN_W,		.height		= IN_H,
			.dst_x		= (uint32_t)(50 + col * (IN_W + GAP)),
			.dst_y		= (uint32_t)(50 + row * (IN_H + GAP)),
			.font_color	= 0x80C040,
			.font_value	= 0xFF,
			.alpha		= 200,
			.color_tone	= 1,
			.threshold	= 128,
		};
	}
	req = (struct vdma_submit_req){
		.dst                = dst,
		.dst_width          = DST_W,
		.dst_height         = DST_H,
		.background_overlay = 1,
		.blits              = blits,
		.nblit              = N,
	};

	r = vdma_font_submit(&req);
	printf("[multi] submit returned %d (N=%d)\n", r, N);

	if (r == 0) {
		fp = fopen(outfile, "wb");
		if (!fp) {
			fprintf(stderr, "[multi] fopen(%s): %s\n",
				outfile, strerror(errno));
			r = 1;
		} else {
			size_t w = fwrite(dst, 1, DST_SZ, fp);
			fclose(fp);
			if (w != DST_SZ) {
				fprintf(stderr,
				        "[multi] short write: %zu / %u\n",
				        w, DST_SZ);
				r = 1;
			} else {
				printf("[multi] wrote %s (%ux%u, %u bytes)\n",
				       outfile, DST_W, DST_H, DST_SZ);
			}
		}
	}

	vdma_font_free(src, IN_SZ);
	vdma_font_free(dst, DST_SZ);
	return r;
}

/* -------------------------------------------------------------------------- */
/* 3. fork 후 자식이 open() 으로 깨끗하게 재시작                                 */
/* -------------------------------------------------------------------------- */
static int demo_fork_recover(void)
{
	pid_t pid = fork();
	if (pid == 0) {
		/* 자식: atfork 가 parent's inst 무효화. alloc 하면 -ENODEV. */
		vdma_addr_t addr = vdma_font_alloc_src(4096);
		if (addr || errno != ENODEV) {
			fprintf(stderr, "[child] expected ENODEV before open, "
				"got %p errno=%d\n", addr, errno);
			if (addr) vdma_font_free(addr, 4096);
			_exit(1);
		}

		/* 자식이 직접 open() 하면 새 fd 가 열리고 사용 가능. */
		if (vdma_font_open(NULL) < 0) {
			fprintf(stderr, "[child] re-open failed: %s\n",
				strerror(errno));
			_exit(1);
		}
		addr = vdma_font_alloc_src(4096);
		if (!addr) {
			fprintf(stderr, "[child] alloc after re-open failed\n");
			vdma_font_close(NULL);
			_exit(1);
		}
		fprintf(stderr, "[child] OK: ENODEV before open, alloc OK after\n");
		vdma_font_free(addr, 4096);
		vdma_font_close(NULL);
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

	if (vdma_font_open(path) < 0) {
		fprintf(stderr, "vdma_font_open(%s): %s\n",
			path ? path : "<default>", strerror(errno));
		return 1;
	}

	demo_sync();
	demo_multi_blit();
	demo_fork_recover();

	vdma_font_close(path);
	return 0;
}
