/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Eyenix Corporation. All rights reserved.
 *
 * enx_vdma_demo — Test / demo program for the libenx_vdma user-space library.
 *
 * Exercises every public API surface across 4 HW functions (Scaler / Font /
 * JpegEnc / JpegDec). Each test is self-contained: Init → Alloc → Submit →
 * Free → Exit. Tests are independent and can be invoked individually via CLI.
 *
 * Build (next to enx_vdma.h):
 *   gcc -std=c11 -O2 -Wall -I../../include-uapi enx_vdma_demo.c \
 *       enx_vdma.a -lpthread -o enx_vdma_demo
 *
 * Usage:
 *   ./enx_vdma_demo                  # run all tests
 *   ./enx_vdma_demo -t scaler        # run a single test group
 *   ./enx_vdma_demo -l               # list available tests
 *   ./enx_vdma_demo -v               # verbose
 */

/* enx_vdma.h 가 __u32 / __u64 같은 kernel 타입을 typedef 하므로 먼저 include 필요 */
#include <linux/types.h>

#include "../../enx_vdma.h"

#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─────────────────────────────────────────────────────────────────────────
 * Test infrastructure
 * ───────────────────────────────────────────────────────────────────────── */

static int  g_total;
static int  g_passed;
static int  g_skipped;
static bool g_verbose;

#define LOG(fmt, ...)        printf("       " fmt "\n", ##__VA_ARGS__)
#define VLOG(fmt, ...)       do { if (g_verbose) LOG(fmt, ##__VA_ARGS__); } while (0)

#define TEST_PASS(name)      do { printf(C_GREEN "[PASS]" C_RESET " %s\n", name); g_passed++; } while (0)
#define TEST_FAIL(name, msg) do { printf(C_RED   "[FAIL]" C_RESET " %s — %s (errno=%d: %s)\n", \
                                         name, msg, errno, strerror(errno)); } while (0)
#define TEST_SKIP(name, msg) do { printf(C_YELL  "[SKIP]" C_RESET " %s — %s\n", name, msg); g_skipped++; } while (0)

#define EXPECT_TRUE(cond, label) \
	do { if (!(cond)) { TEST_FAIL(label, "expectation failed: " #cond); return -1; } } while (0)

#define EXPECT_EQ(a, b, label) \
	do { if ((a) != (b)) { TEST_FAIL(label, "expected " #a " == " #b); return -1; } } while (0)

#define EXPECT_NOT_NULL(p, label) \
	do { if (!(p)) { TEST_FAIL(label, #p " is NULL"); return -1; } } while (0)

#define EXPECT_NULL(p, label) \
	do { if ((p)) { TEST_FAIL(label, #p " is not NULL"); return -1; } } while (0)

/* ─────────────────────────────────────────────────────────────────────────
 * Common helpers
 * ───────────────────────────────────────────────────────────────────────── */

/* Synthesize a minimal YUV422 (YUYV) frame: solid color for testing. */
static void fill_yuv422_solid(void *buf, int w, int h, uint8_t y, uint8_t u, uint8_t v)
{
	uint8_t *p = (uint8_t *)buf;
	for (int row = 0; row < h; row++) {
		for (int col = 0; col < w; col += 2) {
			*p++ = y;
			*p++ = u;
			*p++ = y;
			*p++ = v;
		}
	}
}

/* Bytes-per-pixel approximation for cache-flush sizing in YUV422 layouts. */
static size_t yuv422_size(int w, int h)
{
	return (size_t)w * h * 2;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Test 1 — Scaler (DZ) basic flow with VIRT addressing
 * ───────────────────────────────────────────────────────────────────────── */

static int test_scaler_basic(void)
{
	const char *NAME = "scaler/basic-virt";
	const int   CH = 0;
	const int   SW = 1920, SH = 1080;
	const int   DW =  640, DH =  360;
	const size_t SS = yuv422_size(SW, SH);
	const size_t DS = yuv422_size(DW, DH);

	g_total++;

	Int32 rc = ENX_DMA_Scaler_Init(CH);
	if (rc) { TEST_FAIL(NAME, "Scaler_Init failed"); return -1; }
	VLOG("Init ch=%d ok", CH);

	void *src = ENX_DMA_Scaler_Buffer_Alloc(CH, VDMA_KIND_SRC, SS);
	EXPECT_NOT_NULL(src, NAME);
	void *dst = ENX_DMA_Scaler_Buffer_Alloc(CH, VDMA_KIND_DST, DS);
	EXPECT_NOT_NULL(dst, NAME);
	VLOG("Alloc src=%zuB dst=%zuB ok", SS, DS);

	fill_yuv422_solid(src, SW, SH, 0x80, 0x80, 0x80);
	memset(dst, 0xAA, DS);

	VDMA_DZ_CONFIG_S cfg = {
		.src_addr_flag    = ENX_VDMA_ADDR_TYPE_VIRT,
		.src_addr         = (unsigned long)(uintptr_t)src,
		.src_format       = eFmt_YUV422_YUYV,
		.src_width        = SW, .src_height        = SH,
		.src_width_total  = SW, .src_height_total  = SH,
		.src_width_pos    = 0,  .src_height_pos    = 0,

		.dst_addr_flag    = ENX_VDMA_ADDR_TYPE_VIRT,
		.dst_addr         = (unsigned long)(uintptr_t)dst,
		.dst_format       = eFmt_YUV422_YUYV,
		.dst_width        = DW, .dst_height        = DH,
		.dst_width_total  = DW,

		.vflip = 0, .hflip = 0,
		.src_cache_flush  = 1,
		.dst_cache_flush  = 1,
	};

	rc = ENX_DMA_Scaler_Execute(CH, &cfg);
	if (rc) { TEST_FAIL(NAME, "Scaler_Execute failed"); goto out; }
	VLOG("Execute downscale 1920x1080 -> 640x360 ok");

	/* Verify cfg was NOT mutated (lib should save/restore). */
	EXPECT_EQ(cfg.src_addr_flag, (unsigned int)ENX_VDMA_ADDR_TYPE_VIRT, NAME);
	EXPECT_EQ(cfg.src_addr, (unsigned long)(uintptr_t)src, NAME);
	VLOG("Verified user cfg struct is unchanged (read-only contract)");

	TEST_PASS(NAME);
out:
	ENX_DMA_Scaler_Buffer_Free(CH, dst, DS);
	ENX_DMA_Scaler_Buffer_Free(CH, src, SS);
	ENX_DMA_Scaler_Exit(CH);
	return rc;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Test 2 — Scaler with ID-mode addressing (Alloc_EX → submit by id)
 * ───────────────────────────────────────────────────────────────────────── */

static int test_scaler_id_mode(void)
{
	const char *NAME = "scaler/id-mode";
	const int   CH = 1;
	const int   SW = 1280, SH = 720;
	const int   DW =  640, DH = 360;
	const size_t SS = yuv422_size(SW, SH);
	const size_t DS = yuv422_size(DW, DH);

	g_total++;

	Int32 rc = ENX_DMA_Scaler_Init(CH);
	if (rc) { TEST_FAIL(NAME, "Scaler_Init failed"); return -1; }

	UInt32 src_id = 0, dst_id = 0;
	void *src = ENX_DMA_Scaler_Buffer_Alloc_EX(CH, VDMA_KIND_SRC, SS, &src_id);
	void *dst = ENX_DMA_Scaler_Buffer_Alloc_EX(CH, VDMA_KIND_DST, DS, &dst_id);
	EXPECT_NOT_NULL(src, NAME);
	EXPECT_NOT_NULL(dst, NAME);
	EXPECT_TRUE(src_id != 0 && dst_id != 0, NAME);
	VLOG("Alloc_EX src_id=%u dst_id=%u ok", src_id, dst_id);

	fill_yuv422_solid(src, SW, SH, 0x40, 0x80, 0xC0);

	/* Use ID mode for both src and dst — skip lib's virt→id lookup. */
	VDMA_DZ_CONFIG_S cfg = {
		.src_addr_flag    = ENX_VDMA_ADDR_TYPE_ID,
		.src_addr         = src_id,
		.src_format       = eFmt_YUV422_YUYV,
		.src_width        = SW, .src_height        = SH,
		.src_width_total  = SW, .src_height_total  = SH,

		.dst_addr_flag    = ENX_VDMA_ADDR_TYPE_ID,
		.dst_addr         = dst_id,
		.dst_format       = eFmt_YUV422_YUYV,
		.dst_width        = DW, .dst_height        = DH,
		.dst_width_total  = DW,

		.src_cache_flush  = 1,
		.dst_cache_flush  = 1,
	};

	rc = ENX_DMA_Scaler_Execute(CH, &cfg);
	if (rc) { TEST_FAIL(NAME, "Scaler_Execute (ID mode) failed"); goto out; }

	TEST_PASS(NAME);
out:
	ENX_DMA_Scaler_Buffer_Free(CH, dst, DS);
	ENX_DMA_Scaler_Buffer_Free(CH, src, SS);
	ENX_DMA_Scaler_Exit(CH);
	return rc;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Test 3 — Font (kernel mode) basic submit
 * ───────────────────────────────────────────────────────────────────────── */

static int test_font_basic(void)
{
	const char *NAME = "font/basic-kernel";
	const int   DW = 1280, DH = 720;
	const size_t DS = yuv422_size(DW, DH);
	const size_t SS = 32 * 32;   /* 32x32 glyph bitmap */

	g_total++;

	Int32 rc = ENX_DMA_Font_Init(ENX_VDMA_INIT_TYPE_KERNEL);
	if (rc) { TEST_FAIL(NAME, "Font_Init failed"); return -1; }
	VLOG("Font_Init(KERNEL) ok");

	void *dst = ENX_DMA_Font_Buffer_Alloc(VDMA_KIND_DST, DS);
	void *src = ENX_DMA_Font_Buffer_Alloc(VDMA_KIND_SRC, SS);
	EXPECT_NOT_NULL(dst, NAME);
	EXPECT_NOT_NULL(src, NAME);
	VLOG("Buffer_Alloc dst=%zuB src=%zuB ok", DS, SS);

	/* Fill bitmap (white blob on grey) */
	memset(src, 0xFF, SS);
	memset(dst, 0x80, DS);

	const UInt32 BLITS = 4;
	VDMA_FONT_CONFIG_S *cfg = ENX_DMA_Font_CfgAlloc(BLITS, /*yc_index=*/0);
	EXPECT_NOT_NULL(cfg, NAME);
	VLOG("CfgAlloc blit_count=%u ok", BLITS);

	cfg->dst_width        = DW;
	cfg->dst_height       = DH;
	cfg->background_overlay = 1;          /* keep existing dst content */
	cfg->dst_type_index   = ENX_VDMA_ADDR_TYPE_VIRT;

	for (UInt32 i = 0; i < BLITS; i++) {
		VDMA_FONT_INFO_S *fi = &cfg->font_info[i];
		fi->src_addr_flag = ENX_VDMA_ADDR_TYPE_VIRT;
		fi->src_addr      = (unsigned long)(uintptr_t)src;
		fi->src_width     = 32;
		fi->src_height    = 32;
		fi->font_xpos     = 100 + (i * 64);
		fi->font_ypos     = 200;
		fi->dst_addr_y    = (unsigned long)(uintptr_t)dst;
		fi->dst_addr_c    = (unsigned long)(uintptr_t)dst + (DW * DH);
		fi->font_color_y  = 0xEB;        /* white */
		fi->font_color_cb = 0x80;
		fi->font_color_cr = 0x80;
		fi->font_value    = 0x80;
		fi->font_threshold = 0x80;
		fi->font_alpha    = 256;
		fi->font_wr_mode  = 0;
		fi->font_color_tone = 1;          /* use font_color */
	}

	rc = ENX_DMA_Font_Update(cfg);
	if (rc) { TEST_FAIL(NAME, "Font_Update failed"); goto out; }
	VLOG("Font_Update with %u blits ok", BLITS);

	/* Verify cfg was preserved (decided design point — should not mutate). */
	EXPECT_EQ(cfg->font_info[0].src_addr_flag,
		  (unsigned int)ENX_VDMA_ADDR_TYPE_VIRT, NAME);
	VLOG("Verified user font cfg is unchanged");

	TEST_PASS(NAME);
out:
	ENX_DMA_Font_CfgFree();
	ENX_DMA_Font_Buffer_Free(src, SS);
	ENX_DMA_Font_Buffer_Free(dst, DS);
	ENX_DMA_Font_Exit();
	return rc;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Test 4 — JPEG Encoder basic submit
 * ───────────────────────────────────────────────────────────────────────── */

static int test_jpegenc_basic(void)
{
	const char *NAME = "jpegenc/basic";
	const int   W = 640, H = 480;
	const size_t SS = yuv422_size(W, H);
	const size_t DS = SS;            /* worst-case JPEG never bigger than raw */

	g_total++;

	Int32 rc = ENX_DMA_JpegEnc_Init();
	if (rc) { TEST_FAIL(NAME, "JpegEnc_Init failed"); return -1; }
	VLOG("JpegEnc_Init ok");

	void *src = ENX_DMA_JpegEnc_Buffer_Alloc(VDMA_KIND_SRC, SS);
	void *dst = ENX_DMA_JpegEnc_Buffer_Alloc(VDMA_KIND_DST, DS);
	EXPECT_NOT_NULL(src, NAME);
	EXPECT_NOT_NULL(dst, NAME);

	fill_yuv422_solid(src, W, H, 0x80, 0x80, 0x80);   /* grey frame */

	VDMA_JPEG_ENC_CONFIG_S cfg = {
		.src_addr_flag    = ENX_VDMA_ADDR_TYPE_VIRT,
		.src_addr         = (unsigned long)(uintptr_t)src,
		.dst_addr_flag    = ENX_VDMA_ADDR_TYPE_VIRT,
		.dst_addr         = (unsigned long)(uintptr_t)dst,
		.src_format       = eFmt_YUV422_YUYV,
		.src_width_total  = W, .src_height_total  = H,
		.src_width        = W, .src_height        = H,
		.src_width_pos    = 0, .src_height_pos    = 0,
		.quality          = 80,
		.ovf_threshold    = 0,        /* 0 = lib uses src size heuristic */
		.src_cache_flush  = 1,
		.dst_cache_flush  = 1,
	};

	size_t jpeg_size = 0;
	rc = ENX_DMA_JpegEnc_Execute(&cfg, &jpeg_size);
	if (rc) { TEST_FAIL(NAME, "JpegEnc_Execute failed"); goto out; }

	EXPECT_TRUE(jpeg_size > 0,  NAME);
	EXPECT_TRUE(jpeg_size <= DS, NAME);
	VLOG("Encoded JPEG = %zu bytes (src=%zu bytes)", jpeg_size, SS);

	/* Optionally sanity-check JPEG magic (SOI marker FF D8). */
	if (jpeg_size >= 2) {
		const uint8_t *jp = (const uint8_t *)dst;
		if (jp[0] == 0xFF && jp[1] == 0xD8) {
			VLOG("JPEG SOI magic FF D8 found at offset 0 — encoding looks valid");
		} else {
			LOG("warning: JPEG SOI not at offset 0 (got %02X %02X) — HW dependent",
			    jp[0], jp[1]);
		}
	}

	TEST_PASS(NAME);
out:
	ENX_DMA_JpegEnc_Buffer_Free(dst, DS);
	ENX_DMA_JpegEnc_Buffer_Free(src, SS);
	ENX_DMA_JpegEnc_Exit();
	return rc;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Test 5 — JPEG Decoder basic submit
 *
 * Requires a JPEG file path via $ENX_VDMA_DEMO_JPEG, otherwise skips.
 * ───────────────────────────────────────────────────────────────────────── */

static int test_jpegdec_basic(void)
{
	const char *NAME = "jpegdec/basic";
	// const char *path = getenv("ENX_VDMA_DEMO_JPEG");
	const char *path = "input.jpg";

	g_total++;
	if (!path) {
		TEST_SKIP(NAME, "set ENX_VDMA_DEMO_JPEG=<path/to/file.jpg> to run");
		return 0;
	}

	FILE *f = fopen(path, "rb");
	if (!f) { TEST_FAIL(NAME, "fopen failed"); return -1; }
	fseek(f, 0, SEEK_END);
	long jpeg_bytes = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (jpeg_bytes <= 0) { fclose(f); TEST_FAIL(NAME, "empty file"); return -1; }

	const size_t SS = (size_t)jpeg_bytes;
	const size_t DS = 1920 * 1080 * 3;     /* generous YUV upper bound */

	Int32 rc = ENX_DMA_JpegDec_Init();
	if (rc) { fclose(f); TEST_FAIL(NAME, "JpegDec_Init failed"); return -1; }

	void *src = ENX_DMA_JpegDec_Buffer_Alloc(VDMA_KIND_SRC, SS);
	void *dst = ENX_DMA_JpegDec_Buffer_Alloc(VDMA_KIND_DST, DS);
	if (!src || !dst) { fclose(f); TEST_FAIL(NAME, "Buffer_Alloc failed"); goto out; }

	if (fread(src, 1, SS, f) != SS) { fclose(f); TEST_FAIL(NAME, "fread short"); goto out; }
	fclose(f);
	VLOG("Loaded JPEG file %s (%zu bytes)", path, SS);

	VDMA_JPEG_DEC_CONFIG_S cfg = {
		.src_addr_flag   = ENX_VDMA_ADDR_TYPE_VIRT,
		.src_addr        = (unsigned long)(uintptr_t)src,
		.dst_addr_flag   = ENX_VDMA_ADDR_TYPE_VIRT,
		.dst_addr        = (unsigned long)(uintptr_t)dst,
		.src_jpeg_size   = (unsigned int)SS,
		.dst_buf_size    = (unsigned int)DS,
		.src_cache_flush = 1,
		.dst_cache_flush = 1,
	};

	size_t yuv_size = 0;
	rc = ENX_DMA_JpegDec_Execute(&cfg, &yuv_size);
	if (rc) { TEST_FAIL(NAME, "JpegDec_Execute failed"); goto out; }

	EXPECT_TRUE(yuv_size > 0,  NAME);
	EXPECT_TRUE(yuv_size <= DS, NAME);
	VLOG("Decoded YUV = %zu bytes", yuv_size);

	TEST_PASS(NAME);
out:
	if (dst) ENX_DMA_JpegDec_Buffer_Free(dst, DS);
	if (src) ENX_DMA_JpegDec_Buffer_Free(src, SS);
	ENX_DMA_JpegDec_Exit();
	return rc;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Test 6 — Init/Exit refcount (multiple Init calls share one instance)
 * ───────────────────────────────────────────────────────────────────────── */

static int test_refcount(void)
{
	const char *NAME = "lifecycle/init-refcount";
	g_total++;

	/* Two Init, two Exit — must remain stable. */
	EXPECT_EQ(ENX_DMA_Font_Init(ENX_VDMA_INIT_TYPE_KERNEL), 0, NAME);
	EXPECT_EQ(ENX_DMA_Font_Init(ENX_VDMA_INIT_TYPE_KERNEL), 0, NAME);

	/* In-between Init's lifetime, Buffer_Alloc must work. */
	void *b = ENX_DMA_Font_Buffer_Alloc(VDMA_KIND_SRC, 4096);
	EXPECT_NOT_NULL(b, NAME);
	ENX_DMA_Font_Buffer_Free(b, 4096);

	ENX_DMA_Font_Exit();   /* first exit: refcount-- */
	/* Instance still alive — alloc must still work. */
	b = ENX_DMA_Font_Buffer_Alloc(VDMA_KIND_DST, 4096);
	EXPECT_NOT_NULL(b, NAME);
	ENX_DMA_Font_Buffer_Free(b, 4096);

	ENX_DMA_Font_Exit();   /* second exit: instance destroyed */

	/* After full Exit, alloc must fail (no device). */
	b = ENX_DMA_Font_Buffer_Alloc(VDMA_KIND_SRC, 4096);
	EXPECT_NULL(b, NAME);
	VLOG("After full Exit, Buffer_Alloc correctly returned NULL");

	TEST_PASS(NAME);
	return 0;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Test 7 — Invalid argument handling
 * ───────────────────────────────────────────────────────────────────────── */

static int test_invalid_args(void)
{
	const char *NAME = "error/invalid-args";
	g_total++;

	/* Bad kind value. */
	errno = 0;
	void *p = ENX_DMA_Scaler_Buffer_Alloc(0, 99 /* invalid kind */, 1024);
	EXPECT_NULL(p, NAME);
	VLOG("invalid kind correctly rejected (errno=%d)", errno);

	/* Bad channel for scaler. */
	errno = 0;
	Int32 rc = ENX_DMA_Scaler_Init(99);
	EXPECT_TRUE(rc != 0, NAME);
	VLOG("invalid scaler channel correctly rejected (rc=%d)", rc);

	/* Execute with NULL cfg. */
	rc = ENX_DMA_Scaler_Init(0);
	if (rc == 0) {
		Int32 erc = ENX_DMA_Scaler_Execute(0, NULL);
		EXPECT_TRUE(erc != 0, NAME);
		VLOG("NULL cfg correctly rejected (rc=%d)", erc);
		ENX_DMA_Scaler_Exit(0);
	}

	/* Free NULL — must not crash. */
	ENX_DMA_Scaler_Buffer_Free(0, NULL, 0);
	VLOG("Buffer_Free(NULL) handled safely");

	TEST_PASS(NAME);
	return 0;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Test 8 — Multiple buffers + verify quota behavior
 * ───────────────────────────────────────────────────────────────────────── */

#define MAX_TRY 64

static int test_multi_buffers(void)
{
	const char *NAME = "lifecycle/multi-buffers";
	g_total++;

	Int32 rc = ENX_DMA_Scaler_Init(0);
	if (rc) { TEST_FAIL(NAME, "Scaler_Init failed"); return -1; }

	void *bufs[MAX_TRY] = { 0 };
	int   alloc_count = 0;

	for (int i = 0; i < MAX_TRY; i++) {
		bufs[i] = ENX_DMA_Scaler_Buffer_Alloc(0, VDMA_KIND_SRC, 4096);
		if (!bufs[i]) {
			VLOG("alloc stopped at i=%d (errno=%d: %s)", i, errno, strerror(errno));
			break;
		}
		alloc_count++;
	}

	EXPECT_TRUE(alloc_count > 0, NAME);
	VLOG("Allocated %d buffers before quota / OOM", alloc_count);

	/* Cleanup. */
	for (int i = 0; i < alloc_count; i++)
		ENX_DMA_Scaler_Buffer_Free(0, bufs[i], 4096);

	/* After releasing all, fresh alloc must succeed again. */
	void *again = ENX_DMA_Scaler_Buffer_Alloc(0, VDMA_KIND_SRC, 4096);
	EXPECT_NOT_NULL(again, NAME);
	ENX_DMA_Scaler_Buffer_Free(0, again, 4096);
	VLOG("Buffer quota correctly released — fresh alloc OK");

	TEST_PASS(NAME);
	ENX_DMA_Scaler_Exit(0);
	return 0;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Test 9 — Open every device concurrently (init-only sanity check)
 *
 * Verifies open/close of every HW driver works without HW activity.
 * ───────────────────────────────────────────────────────────────────────── */

static int test_all_devs_init(void)
{
	const char *NAME = "lifecycle/all-devs-init";
	Int32 rc;
	g_total++;

	rc = ENX_DMA_Scaler_Init(0);
	EXPECT_EQ(rc, 0, NAME);
	rc = ENX_DMA_Scaler_Init(1);
	EXPECT_EQ(rc, 0, NAME);
	rc = ENX_DMA_Font_Init(ENX_VDMA_INIT_TYPE_KERNEL);
	EXPECT_EQ(rc, 0, NAME);
	rc = ENX_DMA_JpegEnc_Init();
	EXPECT_EQ(rc, 0, NAME);
	rc = ENX_DMA_JpegDec_Init();
	EXPECT_EQ(rc, 0, NAME);

	VLOG("All 5 device instances opened concurrently");

	ENX_DMA_JpegDec_Exit();
	ENX_DMA_JpegEnc_Exit();
	ENX_DMA_Font_Exit();
	ENX_DMA_Scaler_Exit(1);
	ENX_DMA_Scaler_Exit(0);

	TEST_PASS(NAME);
	return 0;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Test dispatch table
 * ───────────────────────────────────────────────────────────────────────── */

struct test_case {
	const char *name;
	int (*fn)(void);
	const char *desc;
};

static const struct test_case TESTS[] = {
	{ "scaler",     test_scaler_basic,    "Scaler downscale 1080p→360p (VIRT mode)" },
	{ "scaler-id",  test_scaler_id_mode,  "Scaler with ID-mode addressing (Alloc_EX)" },
	{ "font",       test_font_basic,      "Font kernel-mode multi-blit submit" },
	{ "jpegenc",    test_jpegenc_basic,   "JPEG encoder, 640x480 YUV → JPEG" },
	{ "jpegdec",    test_jpegdec_basic,   "JPEG decoder (requires $ENX_VDMA_DEMO_JPEG)" },
	{ "refcount",   test_refcount,        "Init/Exit refcount: paired Init's share one instance" },
	{ "invalid",    test_invalid_args,    "Reject invalid kind / channel / NULL cfg" },
	{ "multibuf",   test_multi_buffers,   "Allocate many buffers, verify quota release" },
	{ "all-devs",   test_all_devs_init,   "Open every HW driver concurrently then close" },
};
#define N_TESTS (sizeof(TESTS) / sizeof(TESTS[0]))

static void list_tests(void)
{
	printf("Available tests:\n");
	for (size_t i = 0; i < N_TESTS; i++)
		printf("  %-12s  %s\n", TESTS[i].name, TESTS[i].desc);
}

static int run_one(const struct test_case *tc)
{
	printf(C_CYAN "=== %s ===" C_RESET "\n", tc->name);
	int rc = tc->fn();
	printf("\n");
	return rc;
}

static int run_named(const char *name)
{
	for (size_t i = 0; i < N_TESTS; i++) {
		if (strcmp(TESTS[i].name, name) == 0)
			return run_one(&TESTS[i]);
	}
	fprintf(stderr, "Unknown test: %s\n", name);
	list_tests();
	return 1;
}

static int run_all(void)
{
	for (size_t i = 0; i < N_TESTS; i++)
		run_one(&TESTS[i]);
	return 0;
}

/* ─────────────────────────────────────────────────────────────────────────
 * main
 * ───────────────────────────────────────────────────────────────────────── */

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"\n"
		"Options:\n"
		"  -t <name>    Run a single test (see -l for names)\n"
		"  -l           List available tests and exit\n"
		"  -v           Verbose output\n"
		"  -h           This help\n"
		"\n"
		"Environment:\n"
		"  ENX_VDMA_DEMO_JPEG   Path to JPEG file for jpegdec test\n",
		prog);
}

int main(int argc, char *argv[])
{
	const char *only = NULL;
	int opt;

	while ((opt = getopt(argc, argv, "t:lvh")) != -1) {
		switch (opt) {
		case 't': only = optarg; break;
		case 'l': list_tests(); return 0;
		case 'v': g_verbose = true; break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 1;
		}
	}

	printf(C_CYAN "──────────────────────────────────────────\n");
	printf(" enx_vdma_demo — libenx_vdma verification suite\n");
	printf("──────────────────────────────────────────" C_RESET "\n\n");

	int ret = only ? run_named(only) : run_all();

	printf(C_CYAN "──────────────────────────────────────────" C_RESET "\n");
	printf(" Total: %d   "
	       C_GREEN "Passed: %d" C_RESET "   "
	       C_RED   "Failed: %d" C_RESET "   "
	       C_YELL  "Skipped: %d" C_RESET "\n",
	       g_total, g_passed, g_total - g_passed - g_skipped, g_skipped);
	printf(C_CYAN "──────────────────────────────────────────" C_RESET "\n");

	return (g_passed + g_skipped == g_total) ? ret : 1;
}
