/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Eyenix Corporation. All rights reserved.
 *
 * enx_vdma_loop — Submit loop stress / throughput test for libenx_vdma.
 *
 * 한 HW 동작을 N 회 반복 submit 하면서 :
 *   - per-iteration latency (min / avg / max / p50 / p99)
 *   - 전체 throughput (ops/sec, MB/sec)
 *   - failure count
 * 를 측정합니다. 버퍼는 기본적으로 한 번 alloc 후 재사용하지만 `--realloc` 으로
 * 매 iter alloc/free 도 가능. release 후 누수 없이 정리되는지 확인 가능.
 *
 * Build (next to enx_vdma.h):
 *   gcc -std=c11 -O2 -Wall -I../../include-uapi enx_vdma_loop.c \
 *       enx_vdma.a -lpthread -o enx_vdma_loop
 *
 * Usage examples:
 *   ./enx_vdma_loop                       # default: scaler ×1000
 *   ./enx_vdma_loop -t scaler -n 10000
 *   ./enx_vdma_loop -t jpegenc -n 500 -v
 *   ./enx_vdma_loop -t scaler --realloc -n 100   # alloc/free 매 iter
 *   ./enx_vdma_loop -t scaler -W 3840 -H 2160 -n 100   # 4K
 */

/* clock_gettime, CLOCK_MONOTONIC 노출 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <linux/types.h>

#include "../../enx_vdma.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ─────────────────────────────────────────────────────────────────────────
 * Output / timing
 * ───────────────────────────────────────────────────────────────────────── */

static bool g_verbose;

#define LOG(fmt, ...)        printf(fmt "\n", ##__VA_ARGS__)
#define VLOG(fmt, ...)       do { if (g_verbose) printf(C_DIM "       " fmt C_RESET "\n", ##__VA_ARGS__); } while (0)

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* qsort comparator for uint64_t */
static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Stats — gathered per iteration, summarized at the end
 * ───────────────────────────────────────────────────────────────────────── */

struct stats {
	int       iters;
	int       ok;
	int       fail;
	uint64_t *lat_ns;     /* per-iter latency, size = iters */
	uint64_t  total_ns;
	size_t    bytes_per_iter;
};

static int stats_init(struct stats *s, int iters, size_t bytes_per_iter)
{
	memset(s, 0, sizeof(*s));
	s->iters = iters;
	s->bytes_per_iter = bytes_per_iter;
	s->lat_ns = calloc((size_t)iters, sizeof(uint64_t));
	return s->lat_ns ? 0 : -1;
}

static void stats_free(struct stats *s)
{
	free(s->lat_ns);
	s->lat_ns = NULL;
}

static void stats_record(struct stats *s, int idx, uint64_t lat_ns, bool ok)
{
	if (idx < 0 || idx >= s->iters) return;
	s->lat_ns[idx] = lat_ns;
	if (ok) s->ok++; else s->fail++;
}

static void stats_print(const struct stats *s, const char *test_name)
{
	if (s->ok == 0) {
		LOG(C_RED "%s: no successful iterations (%d failed)" C_RESET,
		    test_name, s->fail);
		return;
	}

	/* Collect only successful latencies for percentiles. */
	uint64_t *lat = malloc(s->ok * sizeof(uint64_t));
	if (!lat) { LOG("OOM in stats_print"); return; }
	int n = 0;
	for (int i = 0; i < s->iters; i++) {
		if (s->lat_ns[i] > 0) lat[n++] = s->lat_ns[i];
	}
	qsort(lat, n, sizeof(uint64_t), cmp_u64);

	uint64_t lo = lat[0];
	uint64_t hi = lat[n - 1];
	uint64_t p50 = lat[n * 50 / 100];
	uint64_t p99 = lat[n * 99 / 100];
	uint64_t sum = 0;
	for (int i = 0; i < n; i++) sum += lat[i];
	uint64_t avg = sum / (uint64_t)n;
	double   secs = (double)s->total_ns / 1e9;
	double   ops_per_sec = (double)s->ok / secs;
	double   mb_per_sec  = ((double)s->ok * s->bytes_per_iter) / secs / (1024.0 * 1024.0);

	LOG("");
	LOG(C_CYAN "── %s ──────────────────────────────────────" C_RESET, test_name);
	LOG("  iterations    : %d (ok=%d, fail=%d)", s->iters, s->ok, s->fail);
	LOG("  elapsed total : %.3f s", secs);
	LOG("  per-iter (μs) : min=%.1f  avg=%.1f  p50=%.1f  p99=%.1f  max=%.1f",
	    lo / 1000.0, avg / 1000.0, p50 / 1000.0, p99 / 1000.0, hi / 1000.0);
	LOG("  throughput    : %.1f ops/sec  (%.1f MB/sec @ %zuB/iter)",
	    ops_per_sec, mb_per_sec, s->bytes_per_iter);

	free(lat);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Configuration
 * ───────────────────────────────────────────────────────────────────────── */

struct cfg {
	const char *target;        /* scaler / font / jpegenc / jpegdec */
	int  iters;                /* loop count */
	int  ch;                   /* scaler channel */
	int  src_w, src_h;
	int  dst_w, dst_h;
	bool realloc_each;         /* alloc/free per iter */
	int  quality;              /* jpegenc */
};

static const struct cfg DEFAULT_CFG = {
	.target       = "scaler",
	.iters        = 1000,
	.ch           = 0,
	.src_w        = 1920, .src_h = 1080,
	.dst_w        = 640,  .dst_h = 360,
	.realloc_each = false,
	.quality      = 80,
};

/* ─────────────────────────────────────────────────────────────────────────
 * Test : Scaler submit loop
 * ───────────────────────────────────────────────────────────────────────── */

static int loop_scaler(const struct cfg *cfg)
{
	const size_t SS = (size_t)cfg->src_w * cfg->src_h * 2;
	const size_t DS = (size_t)cfg->dst_w * cfg->dst_h * 2;
	struct stats s;

	if (ENX_DMA_Scaler_Init(cfg->ch)) {
		LOG(C_RED "Scaler_Init failed: %s" C_RESET, strerror(errno));
		return 1;
	}

	void *src = NULL, *dst = NULL;
	if (!cfg->realloc_each) {
		src = ENX_DMA_Scaler_Buffer_Alloc(cfg->ch, VDMA_KIND_SRC, SS);
		dst = ENX_DMA_Scaler_Buffer_Alloc(cfg->ch, VDMA_KIND_DST, DS);
		if (!src || !dst) {
			LOG(C_RED "Buffer_Alloc failed" C_RESET);
			goto out;
		}
		memset(src, 0x7F, SS);
	}

	if (stats_init(&s, cfg->iters, SS + DS) != 0) goto out;

	LOG("Scaler loop: ch=%d %dx%d -> %dx%d, %d iter%s%s",
	    cfg->ch, cfg->src_w, cfg->src_h, cfg->dst_w, cfg->dst_h,
	    cfg->iters, cfg->iters > 1 ? "s" : "",
	    cfg->realloc_each ? " (realloc each iter)" : "");

	uint64_t t0 = now_ns();
	for (int i = 0; i < cfg->iters; i++) {
		if (cfg->realloc_each) {
			src = ENX_DMA_Scaler_Buffer_Alloc(cfg->ch, VDMA_KIND_SRC, SS);
			dst = ENX_DMA_Scaler_Buffer_Alloc(cfg->ch, VDMA_KIND_DST, DS);
			if (!src || !dst) {
				stats_record(&s, i, 0, false);
				if (src) ENX_DMA_Scaler_Buffer_Free(cfg->ch, src, SS);
				if (dst) ENX_DMA_Scaler_Buffer_Free(cfg->ch, dst, DS);
				continue;
			}
			memset(src, 0x7F, SS);
		}

		VDMA_DZ_CONFIG_S c = {
			.src_addr_flag    = ENX_VDMA_ADDR_TYPE_VIRT,
			.src_addr         = (unsigned long)(uintptr_t)src,
			.src_format       = eFmt_YUV422_YUYV,
			.src_width        = cfg->src_w, .src_height        = cfg->src_h,
			.src_width_total  = cfg->src_w, .src_height_total  = cfg->src_h,

			.dst_addr_flag    = ENX_VDMA_ADDR_TYPE_VIRT,
			.dst_addr         = (unsigned long)(uintptr_t)dst,
			.dst_format       = eFmt_YUV422_YUYV,
			.dst_width        = cfg->dst_w, .dst_height        = cfg->dst_h,
			.dst_width_total  = cfg->dst_w,

			.src_cache_flush  = 1, .dst_cache_flush  = 1,
		};

		uint64_t t1 = now_ns();
		Int32 rc = ENX_DMA_Scaler_Execute(cfg->ch, &c);
		uint64_t t2 = now_ns();

		stats_record(&s, i, t2 - t1, rc == 0);
		if (rc && g_verbose)
			VLOG("iter[%d] failed rc=%d errno=%d", i, rc, errno);

		if (cfg->realloc_each) {
			ENX_DMA_Scaler_Buffer_Free(cfg->ch, dst, DS);
			ENX_DMA_Scaler_Buffer_Free(cfg->ch, src, SS);
			src = dst = NULL;
		}
	}
	uint64_t t_end = now_ns();
	s.total_ns = t_end - t0;

	stats_print(&s, "scaler");
	stats_free(&s);

out:
	if (!cfg->realloc_each) {
		if (dst) ENX_DMA_Scaler_Buffer_Free(cfg->ch, dst, DS);
		if (src) ENX_DMA_Scaler_Buffer_Free(cfg->ch, src, SS);
	}
	ENX_DMA_Scaler_Exit(cfg->ch);
	return 0;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Test : Font submit loop (KERNEL mode)
 * ───────────────────────────────────────────────────────────────────────── */

static int loop_font(const struct cfg *cfg)
{
	const int    DW = cfg->dst_w, DH = cfg->dst_h;
	const size_t DS = (size_t)DW * DH * 2;
	const size_t SS = 32 * 32;
	const UInt32 BLITS = 4;
	struct stats s;

	if (ENX_DMA_Font_Init(ENX_VDMA_INIT_TYPE_KERNEL)) {
		LOG(C_RED "Font_Init failed: %s" C_RESET, strerror(errno));
		return 1;
	}

	void *src = ENX_DMA_Font_Buffer_Alloc(VDMA_KIND_SRC, SS);
	void *dst = ENX_DMA_Font_Buffer_Alloc(VDMA_KIND_DST, DS);
	if (!src || !dst) { LOG(C_RED "Buffer_Alloc failed" C_RESET); goto out; }
	memset(src, 0xFF, SS);

	VDMA_FONT_CONFIG_S *fcfg = ENX_DMA_Font_CfgAlloc(BLITS, /*yc_index=*/0);
	if (!fcfg) { LOG(C_RED "Font_CfgAlloc failed" C_RESET); goto out; }
	fcfg->dst_width        = DW;
	fcfg->dst_height       = DH;
	fcfg->background_overlay = 1;
	fcfg->dst_type_index   = ENX_VDMA_ADDR_TYPE_VIRT;
	for (UInt32 i = 0; i < BLITS; i++) {
		VDMA_FONT_INFO_S *fi = &fcfg->font_info[i];
		fi->src_addr_flag = ENX_VDMA_ADDR_TYPE_VIRT;
		fi->src_addr      = (unsigned long)(uintptr_t)src;
		fi->src_width     = 32; fi->src_height = 32;
		fi->font_xpos     = 64 * (int)i;
		fi->font_ypos     = 100;
		fi->dst_addr_y    = (unsigned long)(uintptr_t)dst;
		fi->dst_addr_c    = (unsigned long)(uintptr_t)dst + (DW * DH);
		fi->font_color_y  = 0xEB;
		fi->font_color_cb = 0x80;
		fi->font_color_cr = 0x80;
		fi->font_threshold = 0x80;
		fi->font_alpha    = 256;
		fi->font_color_tone = 1;
	}

	if (stats_init(&s, cfg->iters, DS) != 0) goto out;

	LOG("Font loop: %dx%d dst, %u blits, %d iters",
	    DW, DH, BLITS, cfg->iters);

	uint64_t t0 = now_ns();
	for (int i = 0; i < cfg->iters; i++) {
		uint64_t t1 = now_ns();
		Int32 rc = ENX_DMA_Font_Update(fcfg);
		uint64_t t2 = now_ns();

		stats_record(&s, i, t2 - t1, rc == 0);
		if (rc && g_verbose)
			VLOG("iter[%d] failed rc=%d", i, rc);
	}
	uint64_t t_end = now_ns();
	s.total_ns = t_end - t0;

	stats_print(&s, "font");
	stats_free(&s);

out:
	ENX_DMA_Font_CfgFree();
	if (src) ENX_DMA_Font_Buffer_Free(src, SS);
	if (dst) ENX_DMA_Font_Buffer_Free(dst, DS);
	ENX_DMA_Font_Exit();
	return 0;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Test : JPEG encoder submit loop
 * ───────────────────────────────────────────────────────────────────────── */

static int loop_jpegenc(const struct cfg *cfg)
{
	const int    W = cfg->src_w, H = cfg->src_h;
	const size_t SS = (size_t)W * H * 2;
	const size_t DS = SS;
	struct stats s;
	size_t total_jpeg = 0;

	if (ENX_DMA_JpegEnc_Init()) {
		LOG(C_RED "JpegEnc_Init failed: %s" C_RESET, strerror(errno));
		return 1;
	}

	void *src = ENX_DMA_JpegEnc_Buffer_Alloc(VDMA_KIND_SRC, SS);
	void *dst = ENX_DMA_JpegEnc_Buffer_Alloc(VDMA_KIND_DST, DS);
	if (!src || !dst) { LOG(C_RED "Buffer_Alloc failed" C_RESET); goto out; }

	memset(src, 0x80, SS);    /* solid grey frame */

	if (stats_init(&s, cfg->iters, SS) != 0) goto out;

	LOG("JpegEnc loop: %dx%d YUV422 -> JPEG q=%d, %d iters",
	    W, H, cfg->quality, cfg->iters);

	uint64_t t0 = now_ns();
	for (int i = 0; i < cfg->iters; i++) {
		VDMA_JPEG_ENC_CONFIG_S c = {
			.src_addr_flag    = ENX_VDMA_ADDR_TYPE_VIRT,
			.src_addr         = (unsigned long)(uintptr_t)src,
			.dst_addr_flag    = ENX_VDMA_ADDR_TYPE_VIRT,
			.dst_addr         = (unsigned long)(uintptr_t)dst,
			.src_format       = eFmt_YUV422_YUYV,
			.src_width_total  = W, .src_height_total = H,
			.src_width        = W, .src_height       = H,
			.quality          = (unsigned)cfg->quality,
			.src_cache_flush  = 1, .dst_cache_flush  = 1,
		};

		size_t jpeg_size = 0;
		uint64_t t1 = now_ns();
		Int32 rc = ENX_DMA_JpegEnc_Execute(&c, &jpeg_size);
		uint64_t t2 = now_ns();

		stats_record(&s, i, t2 - t1, rc == 0);
		if (rc == 0) total_jpeg += jpeg_size;
		else if (g_verbose) VLOG("iter[%d] failed rc=%d", i, rc);
	}
	uint64_t t_end = now_ns();
	s.total_ns = t_end - t0;

	stats_print(&s, "jpegenc");
	if (s.ok > 0) {
		LOG("  avg JPEG size : %zu bytes (compression %.1f%%)",
		    total_jpeg / (size_t)s.ok,
		    100.0 * (double)total_jpeg / (double)(s.ok * (int)SS));
	}
	stats_free(&s);

out:
	if (dst) ENX_DMA_JpegEnc_Buffer_Free(dst, DS);
	if (src) ENX_DMA_JpegEnc_Buffer_Free(src, SS);
	ENX_DMA_JpegEnc_Exit();
	return 0;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Test : JPEG decoder submit loop
 *
 * Requires a JPEG file via $ENX_VDMA_DEMO_JPEG; uses it for every iteration.
 * ───────────────────────────────────────────────────────────────────────── */

static int loop_jpegdec(const struct cfg *cfg)
{
#if 0
	const char *path = getenv("ENX_VDMA_DEMO_JPEG");
	if (!path) {
		LOG(C_YELL "[SKIP] jpegdec: set ENX_VDMA_DEMO_JPEG=<file.jpg>" C_RESET);
		return 0;
	}
#else
	const char *path = "input.jpg";
#endif

	FILE *f = fopen(path, "rb");
	if (!f) { LOG(C_RED "fopen failed: %s" C_RESET, strerror(errno)); return 1; }
	fseek(f, 0, SEEK_END);
	long jpeg_bytes = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (jpeg_bytes <= 0) { fclose(f); LOG(C_RED "empty JPEG file" C_RESET); return 1; }

	const size_t SS = (size_t)jpeg_bytes;
	const size_t DS = (size_t)cfg->src_w * cfg->src_h * 3;   /* generous YUV upper bound */
	struct stats s;

	if (ENX_DMA_JpegDec_Init()) {
		fclose(f);
		LOG(C_RED "JpegDec_Init failed" C_RESET);
		return 1;
	}

	void *src = ENX_DMA_JpegDec_Buffer_Alloc(VDMA_KIND_SRC, SS);
	void *dst = ENX_DMA_JpegDec_Buffer_Alloc(VDMA_KIND_DST, DS);
	if (!src || !dst) { fclose(f); LOG(C_RED "Buffer_Alloc failed" C_RESET); goto out; }

	if (fread(src, 1, SS, f) != SS) { fclose(f); LOG(C_RED "fread short" C_RESET); goto out; }
	fclose(f);

	if (stats_init(&s, cfg->iters, SS) != 0) goto out;

	LOG("JpegDec loop: file=%s (%zu B), %d iters", path, SS, cfg->iters);

	uint64_t t0 = now_ns();
	for (int i = 0; i < cfg->iters; i++) {
		VDMA_JPEG_DEC_CONFIG_S c = {
			.src_addr_flag   = ENX_VDMA_ADDR_TYPE_VIRT,
			.src_addr        = (unsigned long)(uintptr_t)src,
			.dst_addr_flag   = ENX_VDMA_ADDR_TYPE_VIRT,
			.dst_addr        = (unsigned long)(uintptr_t)dst,
			.src_jpeg_size   = (unsigned int)SS,
			.dst_buf_size    = (unsigned int)DS,
			.src_cache_flush = 1, .dst_cache_flush = 1,
		};

		size_t yuv_size = 0;
		uint64_t t1 = now_ns();
		Int32 rc = ENX_DMA_JpegDec_Execute(&c, &yuv_size);
		uint64_t t2 = now_ns();

		stats_record(&s, i, t2 - t1, rc == 0);
		if (rc && g_verbose) VLOG("iter[%d] failed rc=%d", i, rc);
	}
	uint64_t t_end = now_ns();
	s.total_ns = t_end - t0;

	stats_print(&s, "jpegdec");
	stats_free(&s);

out:
	if (dst) ENX_DMA_JpegDec_Buffer_Free(dst, DS);
	if (src) ENX_DMA_JpegDec_Buffer_Free(src, SS);
	ENX_DMA_JpegDec_Exit();
	return 0;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Dispatch
 * ───────────────────────────────────────────────────────────────────────── */

struct target {
	const char *name;
	int (*fn)(const struct cfg *);
};

static const struct target TARGETS[] = {
	{ "scaler",   loop_scaler  },
	{ "font",     loop_font    },
	{ "jpegenc",  loop_jpegenc },
	{ "jpegdec",  loop_jpegdec },
};
#define N_TARGETS (sizeof(TARGETS) / sizeof(TARGETS[0]))

/* ─────────────────────────────────────────────────────────────────────────
 * main
 * ───────────────────────────────────────────────────────────────────────── */

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"\n"
		"Options:\n"
		"  -t <name>      Target: scaler | font | jpegenc | jpegdec  (default: scaler)\n"
		"  -n <count>     Iteration count (default: 1000)\n"
		"  -c <ch>        Scaler channel: 0|1 (default: 0)\n"
		"  -W <w>         Source width  (default: 1920)\n"
		"  -H <h>         Source height (default: 1080)\n"
		"  -w <w>         Destination width  (default: 640)\n"
		"  -h <h>         Destination height (default: 360)\n"
		"  -q <q>         JPEG quality 1..100 (default: 80)\n"
		"     --realloc   Alloc/Free buffers each iteration (scaler only)\n"
		"  -v             Verbose\n"
		"  -?, --help     This help\n"
		"\n"
		"Examples:\n"
		"  %s -t scaler -n 10000\n"
		"  %s -t jpegenc -W 3840 -H 2160 -n 100 -q 90\n"
		"  ENX_VDMA_DEMO_JPEG=in.jpg %s -t jpegdec -n 1000\n",
		prog, prog, prog, prog);
}

int main(int argc, char *argv[])
{
	struct cfg cfg = DEFAULT_CFG;

	static const struct option longopts[] = {
		{ "realloc", no_argument, NULL, 'R' },
		{ "help",    no_argument, NULL, '?' },
		{ NULL,      0,           NULL,  0  },
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "t:n:c:W:H:w:h:q:vR?", longopts, NULL)) != -1) {
		switch (opt) {
		case 't': cfg.target = optarg;            break;
		case 'n': cfg.iters  = atoi(optarg);      break;
		case 'c': cfg.ch     = atoi(optarg);      break;
		case 'W': cfg.src_w  = atoi(optarg);      break;
		case 'H': cfg.src_h  = atoi(optarg);      break;
		case 'w': cfg.dst_w  = atoi(optarg);      break;
		case 'h': cfg.dst_h  = atoi(optarg);      break;
		case 'q': cfg.quality= atoi(optarg);      break;
		case 'R': cfg.realloc_each = true;        break;
		case 'v': g_verbose  = true;              break;
		case '?': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 1;
		}
	}

	if (cfg.iters <= 0) {
		fprintf(stderr, "Iteration count must be > 0\n");
		return 1;
	}

	LOG(C_CYAN "──────────────────────────────────────────");
	LOG(" enx_vdma_loop — submit-loop benchmark");
	LOG("──────────────────────────────────────────" C_RESET);
	LOG("  target=%s iters=%d ch=%d src=%dx%d dst=%dx%d realloc=%s%s",
	    cfg.target, cfg.iters, cfg.ch, cfg.src_w, cfg.src_h,
	    cfg.dst_w, cfg.dst_h, cfg.realloc_each ? "yes" : "no",
	    g_verbose ? " verbose" : "");
	LOG("");

	for (size_t i = 0; i < N_TARGETS; i++) {
		if (strcmp(TARGETS[i].name, cfg.target) == 0)
			return TARGETS[i].fn(&cfg);
	}

	fprintf(stderr, C_RED "Unknown target: %s" C_RESET "\n", cfg.target);
	usage(argv[0]);
	return 1;
}
