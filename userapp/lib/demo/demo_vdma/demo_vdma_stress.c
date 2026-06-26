/*
 * Copyright (C) 2026 Eyenix Corporation
 * Eyenix <support6@eyenix.com>
 *
 * VDMA demo — stress / regression tests (case 8 sub-menu).
 *	1. Roundtrip loop		(PSNR drift over iters)
 *	2. JpegEnc loop
 *	3. JpegDec loop
 *	4. Scaler loop
 *	5. Font loop
 *	6. NPU loop
 *	7. Alloc/Free churn	(lifecycle only)
 *	8. Quota exhaustion
 *	9. Init/Exit cycle
 *	10. Concurrent			(pthread, race verification)
 *
 * Each test takes the bufs_active baseline AFTER per-test allocs so the
 * post-loop leak check measures only what the Execute path itself leaked.
 */
#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <pthread.h>

#include "demo_vdma.h"
#include "demo_vdma_util.h"


/*******************************************************************************
 *
 * Stress — Scaler loop
 *
 ******************************************************************************/

int DEMO_StressScaler(void)
{
	int N = ask_iters(1000);
	const int W = 1280, H = 720, OW = 640, OH = 360;
	struct buffer_alloc_track src = {0}, dst = {0};
	VDMA_DZ_CONFIG_S cfg;
	struct loop_stats stats = {0};
	long baseline;
	bool was_inited;
	int i, ret = -1;

	was_inited = g_state.scaler_inited[0];
	if (!was_inited) {
		if (ENX_DMA_Scaler_Init(ENX_VDMA_SCALER0) != 0) {
			EPRINTF("auto Scaler Init failed.\n"); return -1;
		}
		g_state.scaler_inited[0] = true;
		HPRINTF("[auto] Scaler Init\n");
	}

	if (stats_init(&stats, N) < 0) { EPRINTF("stats_init failed.\n"); goto out; }

	src.size = calculate_format_size(eFmt_YUV420SP_NV12, W, H);
	dst.size = calculate_format_size(eFmt_YUV420SP_NV12, OW, OH);
	src.buf  = ENX_DMA_Scaler_Buffer_Alloc(ENX_VDMA_SCALER0, VDMA_KIND_SRC, src.size);
	if (!src.buf) { EPRINTF("src alloc failed.\n"); goto out; }
	dst.buf  = ENX_DMA_Scaler_Buffer_Alloc(ENX_VDMA_SCALER0, VDMA_KIND_DST, dst.size);
	if (!dst.buf) { EPRINTF("dst alloc failed.\n"); goto out; }

	/* Baseline AFTER per-test allocs. */
	baseline = read_dbg_counter("enx_vdma_dz0", "stats", "bufs_active");

	fill_nv12_pattern(src.buf, W, H);

	memset(&cfg, 0, sizeof(cfg));
	cfg.src_cache_flush		= 1;
	cfg.dst_cache_flush		= 1;
	cfg.src_addr_flag		= ENX_VDMA_ADDR_TYPE_VIRT;
	cfg.dst_addr_flag		= ENX_VDMA_ADDR_TYPE_VIRT;
	cfg.src_addr			= (UInt64)(uintptr_t)src.buf;
	cfg.dst_addr			= (UInt64)(uintptr_t)dst.buf;
	cfg.src_width_total		= cfg.src_width		= W;
	cfg.src_height_total	= cfg.src_height	= H;
	cfg.src_format			= eFmt_YUV420SP_NV12;
	cfg.dst_width_total		= cfg.dst_width		= OW;
	cfg.dst_height			= OH;
	cfg.dst_format			= eFmt_YUV420SP_NV12;

	HPRINTF("Scaler loop x %d (%dx%d NV12 -> %dx%d NV12) ...\n", N, W, H, OW, OH);
	for (i = 0; i < N; i++) {
		uint64_t t0 = now_ns();
		int r = ENX_DMA_Scaler_Execute(ENX_VDMA_SCALER0, &cfg);
		uint64_t t1 = now_ns();
		stats_record(&stats, t1 - t0, r == 0);
		if ((i + 1) % 100 == 0) { printf("."); fflush(stdout); }
	}
	printf("\n");
	stats_print(&stats, "scaler");
	leak_check("enx_vdma_dz0", baseline);
	ret = 0;
out:
	if (src.buf) ENX_DMA_Scaler_Buffer_Free(ENX_VDMA_SCALER0, src.buf, src.size);
	if (dst.buf) ENX_DMA_Scaler_Buffer_Free(ENX_VDMA_SCALER0, dst.buf, dst.size);
	stats_free(&stats);
	if (!was_inited && g_state.scaler_inited[0]) {
		ENX_DMA_Scaler_Exit(ENX_VDMA_SCALER0);
		g_state.scaler_inited[0] = false;
		HPRINTF("[auto] Scaler Exit\n");
	}
	return ret;
}

/*******************************************************************************
 *
 * Stress — JpegEnc loop
 *
 ******************************************************************************/

int DEMO_StressJpegEnc(void)
{
	int N = ask_iters(1000);
	const int W = 1280, H = 720;
	struct buffer_alloc_track src = {0}, dst = {0};
	VDMA_JPEG_ENC_CONFIG_S cfg;
	struct loop_stats stats = {0};
	long baseline;
	bool was_inited;
	int i, ret = -1;

	was_inited = g_state.jenc_inited;
	if (!was_inited) {
		if (ENX_DMA_JpegEnc_Init() != 0) { EPRINTF("auto JpegEnc Init failed.\n"); return -1; }
		g_state.jenc_inited = true;
		HPRINTF("[auto] JpegEnc Init\n");
	}

	if (stats_init(&stats, N) < 0) { EPRINTF("stats_init failed.\n"); goto out; }

	src.size = calculate_format_size(eFmt_YUV420SP_NV12, W, H);
	dst.size = (size_t)W * H * 4;
	src.buf  = ENX_DMA_JpegEnc_Buffer_Alloc(VDMA_KIND_SRC, src.size);
	if (!src.buf) { EPRINTF("src alloc failed.\n"); goto out; }
	dst.buf  = ENX_DMA_JpegEnc_Buffer_Alloc(VDMA_KIND_DST, dst.size);
	if (!dst.buf) { EPRINTF("dst alloc failed.\n"); goto out; }

	/* Baseline AFTER per-test allocs. */
	baseline = read_dbg_counter("enx_vdma_jenc", "stats", "bufs_active");

	fill_nv12_pattern(src.buf, W, H);

	memset(&cfg, 0, sizeof(cfg));
	cfg.src_cache_flush		= 1;
	cfg.dst_cache_flush		= 1;
	cfg.src_addr_flag		= ENX_VDMA_ADDR_TYPE_VIRT;
	cfg.dst_addr_flag		= ENX_VDMA_ADDR_TYPE_VIRT;
	cfg.src_addr			= (UInt64)(uintptr_t)src.buf;
	cfg.dst_addr			= (UInt64)(uintptr_t)dst.buf;
	cfg.src_width_total		= cfg.src_width  = W;
	cfg.src_height_total	= cfg.src_height = H;
	cfg.src_format			= eFmt_YUV420SP_NV12;
	cfg.quality				= 75;

	HPRINTF("JpegEnc loop x %d (NV12 %dx%d q=%u) ...\n", N, W, H, cfg.quality);
	for (i = 0; i < N; i++) {
		size_t out_size = 0;
		uint64_t t0 = now_ns();
		int r = ENX_DMA_JpegEnc_Execute(&cfg, &out_size);
		uint64_t t1 = now_ns();
		stats_record(&stats, t1 - t0, r == 0 && out_size > 0);
		if ((i + 1) % 100 == 0) { printf("."); fflush(stdout); }
	}
	printf("\n");
	stats_print(&stats, "jpegenc");
	leak_check("enx_vdma_jenc", baseline);
	ret = 0;
out:
	if (src.buf) ENX_DMA_JpegEnc_Buffer_Free(src.buf, src.size);
	if (dst.buf) ENX_DMA_JpegEnc_Buffer_Free(dst.buf, dst.size);
	stats_free(&stats);
	if (!was_inited && g_state.jenc_inited) {
		ENX_DMA_JpegEnc_Exit();
		g_state.jenc_inited = false;
		HPRINTF("[auto] JpegEnc Exit\n");
	}
	return ret;
}

/*******************************************************************************
 *
 * Stress — JpegDec loop
 *
 * Needs a JPEG bitstream to decode. We auto-Init JpegEnc once at start to
 * produce one, then loop the JpegDec Execute over that prepared input.
 *
 ******************************************************************************/

int DEMO_StressJpegDec(void)
{
	int N = ask_iters(1000);
	const int W = 1280, H = 720;
	struct buffer_alloc_track jenc_src = {0}, jenc_dst = {0};
	struct buffer_alloc_track jdec_src = {0}, jdec_dst = {0};
	VDMA_JPEG_ENC_CONFIG_S enc_cfg;
	VDMA_JPEG_DEC_CONFIG_S dec_cfg;
	struct loop_stats stats = {0};
	long baseline;
	bool jenc_was, jdec_was;
	size_t jpeg_out = 0;
	int i, ret = -1;

	jenc_was = g_state.jenc_inited;
	jdec_was = g_state.jdec_inited;
	if (!jenc_was) {
		if (ENX_DMA_JpegEnc_Init() != 0) { EPRINTF("auto JpegEnc Init failed.\n"); return -1; }
		g_state.jenc_inited = true; HPRINTF("[auto] JpegEnc Init\n");
	}
	if (!jdec_was) {
		if (ENX_DMA_JpegDec_Init() != 0) { EPRINTF("auto JpegDec Init failed.\n"); goto out; }
		g_state.jdec_inited = true; HPRINTF("[auto] JpegDec Init\n");
	}

	if (stats_init(&stats, N) < 0) { EPRINTF("stats_init failed.\n"); goto out; }

	/* Step 1: produce a JPEG via JpegEnc (one-shot, not part of timing). */
	jenc_src.size = calculate_format_size(eFmt_YUV420SP_NV12, W, H);
	jenc_dst.size = (size_t)W * H * 4;
	jenc_src.buf  = ENX_DMA_JpegEnc_Buffer_Alloc(VDMA_KIND_SRC, jenc_src.size);
	jenc_dst.buf  = ENX_DMA_JpegEnc_Buffer_Alloc(VDMA_KIND_DST, jenc_dst.size);
	if (!jenc_src.buf || !jenc_dst.buf) { EPRINTF("jenc buf alloc failed.\n"); goto out; }
	fill_nv12_pattern(jenc_src.buf, W, H);

	memset(&enc_cfg, 0, sizeof(enc_cfg));
	enc_cfg.src_cache_flush		= enc_cfg.dst_cache_flush = 1;
	enc_cfg.src_addr_flag		= enc_cfg.dst_addr_flag = ENX_VDMA_ADDR_TYPE_VIRT;
	enc_cfg.src_addr			= (UInt64)(uintptr_t)jenc_src.buf;
	enc_cfg.dst_addr			= (UInt64)(uintptr_t)jenc_dst.buf;
	enc_cfg.src_width_total		= enc_cfg.src_width  = W;
	enc_cfg.src_height_total	= enc_cfg.src_height = H;
	enc_cfg.src_format			= eFmt_YUV420SP_NV12;
	enc_cfg.quality				= 75;
	if (ENX_DMA_JpegEnc_Execute(&enc_cfg, &jpeg_out) < 0 || !jpeg_out) {
		EPRINTF("prep JpegEnc_Execute failed.\n"); goto out;
	}
	HPRINTF("prepared JPEG: %zu bytes (q=75, %dx%d)\n", jpeg_out, W, H);

	/* Step 2: dec src/dst alloc, copy JPEG into dec src once. */
	jdec_src.size = jpeg_out;
	jdec_dst.size = calculate_format_size(eFmt_YUV420SP_NV12, W, H);
	jdec_src.buf  = ENX_DMA_JpegDec_Buffer_Alloc(VDMA_KIND_SRC, jdec_src.size);
	jdec_dst.buf  = ENX_DMA_JpegDec_Buffer_Alloc(VDMA_KIND_DST, jdec_dst.size);
	if (!jdec_src.buf || !jdec_dst.buf) { EPRINTF("jdec buf alloc failed.\n"); goto out; }
	memcpy(jdec_src.buf, jenc_dst.buf, jpeg_out);

	/* Baseline AFTER all per-test allocs (incl. the JpegEnc prep allocs),
	 * so the post-loop check measures only what the Decode loop leaks. */
	baseline = read_dbg_counter("enx_vdma_jdec", "stats", "bufs_active");

	memset(&dec_cfg, 0, sizeof(dec_cfg));
	dec_cfg.src_cache_flush		= dec_cfg.dst_cache_flush = 1;
	dec_cfg.src_addr_flag		= dec_cfg.dst_addr_flag = ENX_VDMA_ADDR_TYPE_VIRT;
	dec_cfg.src_addr			= (UInt64)(uintptr_t)jdec_src.buf;
	dec_cfg.dst_addr			= (UInt64)(uintptr_t)jdec_dst.buf;
	dec_cfg.src_jpeg_size		= (unsigned int)jdec_src.size;
	dec_cfg.dst_buf_size		= (unsigned int)jdec_dst.size;

	HPRINTF("JpegDec loop x %d (JPEG %zu bytes -> NV12 %dx%d) ...\n", N, jpeg_out, W, H);
	for (i = 0; i < N; i++) {
		size_t yuv_out = 0;
		uint64_t t0 = now_ns();
		int r = ENX_DMA_JpegDec_Execute(&dec_cfg, &yuv_out);
		uint64_t t1 = now_ns();
		stats_record(&stats, t1 - t0, r == 0 && yuv_out > 0);
		if ((i + 1) % 100 == 0) { printf("."); fflush(stdout); }
	}
	printf("\n");
	stats_print(&stats, "jpegdec");
	leak_check("enx_vdma_jdec", baseline);
	ret = 0;
out:
	if (jenc_src.buf) ENX_DMA_JpegEnc_Buffer_Free(jenc_src.buf, jenc_src.size);
	if (jenc_dst.buf) ENX_DMA_JpegEnc_Buffer_Free(jenc_dst.buf, jenc_dst.size);
	if (jdec_src.buf) ENX_DMA_JpegDec_Buffer_Free(jdec_src.buf, jdec_src.size);
	if (jdec_dst.buf) ENX_DMA_JpegDec_Buffer_Free(jdec_dst.buf, jdec_dst.size);
	stats_free(&stats);
	if (!jdec_was && g_state.jdec_inited) {
		ENX_DMA_JpegDec_Exit(); g_state.jdec_inited = false;
		HPRINTF("[auto] JpegDec Exit\n");
	}
	if (!jenc_was && g_state.jenc_inited) {
		ENX_DMA_JpegEnc_Exit(); g_state.jenc_inited = false;
		HPRINTF("[auto] JpegEnc Exit\n");
	}
	return ret;
}

/*******************************************************************************
 *
 * Stress — Roundtrip loop (JpegEnc + JpegDec each iter, PSNR drift check)
 *
 ******************************************************************************/

int DEMO_StressRoundtrip(void)
{
	int N = ask_iters(100);
	const int W = 1280, H = 720;
	struct buffer_alloc_track jenc_src = {0}, jenc_dst = {0};
	struct buffer_alloc_track jdec_src = {0}, jdec_dst = {0};
	VDMA_JPEG_ENC_CONFIG_S enc_cfg;
	VDMA_JPEG_DEC_CONFIG_S dec_cfg;
	struct loop_stats stats = {0};
	long base_enc = 0, base_dec = 0;
	bool jenc_was, jdec_was;
	double psnr_min = 0, psnr_max = 0;
	int i, ret = -1;

	jenc_was = g_state.jenc_inited;
	jdec_was = g_state.jdec_inited;
	if (!jenc_was) {
		if (ENX_DMA_JpegEnc_Init() != 0) { EPRINTF("auto JpegEnc Init failed.\n"); return -1; }
		g_state.jenc_inited = true; HPRINTF("[auto] JpegEnc Init\n");
	}
	if (!jdec_was) {
		if (ENX_DMA_JpegDec_Init() != 0) { EPRINTF("auto JpegDec Init failed.\n"); goto out; }
		g_state.jdec_inited = true; HPRINTF("[auto] JpegDec Init\n");
	}

	if (stats_init(&stats, N) < 0) { EPRINTF("stats_init failed.\n"); goto out; }

	jenc_src.size = calculate_format_size(eFmt_YUV420SP_NV12, W, H);
	jenc_dst.size = (size_t)W * H * 4;
	jdec_dst.size = jenc_src.size;
	jenc_src.buf  = ENX_DMA_JpegEnc_Buffer_Alloc(VDMA_KIND_SRC, jenc_src.size);
	jenc_dst.buf  = ENX_DMA_JpegEnc_Buffer_Alloc(VDMA_KIND_DST, jenc_dst.size);
	if (!jenc_src.buf || !jenc_dst.buf) { EPRINTF("jenc buf alloc failed.\n"); goto out; }
	fill_nv12_pattern(jenc_src.buf, W, H);

	jdec_dst.buf = ENX_DMA_JpegDec_Buffer_Alloc(VDMA_KIND_DST, jdec_dst.size);
	if (!jdec_dst.buf) { EPRINTF("jdec dst alloc failed.\n"); goto out; }

	memset(&enc_cfg, 0, sizeof(enc_cfg));
	enc_cfg.src_cache_flush		= enc_cfg.dst_cache_flush = 1;
	enc_cfg.src_addr_flag		= enc_cfg.dst_addr_flag = ENX_VDMA_ADDR_TYPE_VIRT;
	enc_cfg.src_addr			= (UInt64)(uintptr_t)jenc_src.buf;
	enc_cfg.dst_addr			= (UInt64)(uintptr_t)jenc_dst.buf;
	enc_cfg.src_width_total		= enc_cfg.src_width  = W;
	enc_cfg.src_height_total	= enc_cfg.src_height = H;
	enc_cfg.src_format			= eFmt_YUV420SP_NV12;
	enc_cfg.quality				= 75;

	memset(&dec_cfg, 0, sizeof(dec_cfg));
	dec_cfg.src_cache_flush		= dec_cfg.dst_cache_flush = 1;
	dec_cfg.src_addr_flag		= dec_cfg.dst_addr_flag = ENX_VDMA_ADDR_TYPE_VIRT;
	dec_cfg.dst_addr			= (UInt64)(uintptr_t)jdec_dst.buf;
	dec_cfg.dst_buf_size		= (unsigned int)jdec_dst.size;

	HPRINTF("Roundtrip loop x %d (NV12 %dx%d q=75) ...\n", N, W, H);
	for (i = 0; i < N; i++) {
		size_t jpeg_out = 0, yuv_out = 0;
		double psnr;
		uint64_t t0, t1;
		bool ok;

		t0 = now_ns();
		if (ENX_DMA_JpegEnc_Execute(&enc_cfg, &jpeg_out) < 0 || !jpeg_out) {
			t1 = now_ns(); stats_record(&stats, t1 - t0, false); continue;
		}

		/* JpegDec src buf size depends on encoded size — re-alloc per iter.
		 * This intentionally exercises the per-iter alloc/free path too. */
		if (jdec_src.buf) {
			ENX_DMA_JpegDec_Buffer_Free(jdec_src.buf, jdec_src.size);
			jdec_src.buf = NULL;
		}
		jdec_src.size = jpeg_out;
		jdec_src.buf  = ENX_DMA_JpegDec_Buffer_Alloc(VDMA_KIND_SRC, jdec_src.size);
		if (!jdec_src.buf) { stats_record(&stats, now_ns() - t0, false); continue; }
		memcpy(jdec_src.buf, jenc_dst.buf, jpeg_out);
		dec_cfg.src_addr		= (UInt64)(uintptr_t)jdec_src.buf;
		dec_cfg.src_jpeg_size	= (unsigned int)jpeg_out;

		/* Baseline AFTER all per-test allocs. The Roundtrip loop re-allocates
		 * jdec_src per iter (size varies with each encoded JPEG), so the per-
		 * iter alloc/free should net to zero — measured by these baselines. */
		base_enc = read_dbg_counter("enx_vdma_jenc", "stats", "bufs_active");
		base_dec = read_dbg_counter("enx_vdma_jdec", "stats", "bufs_active");

		if (ENX_DMA_JpegDec_Execute(&dec_cfg, &yuv_out) < 0 || !yuv_out) {
			t1 = now_ns(); stats_record(&stats, t1 - t0, false); continue;
		}
		t1 = now_ns();

		psnr = compute_psnr_y(jenc_src.buf, jdec_dst.buf, W, H);
		if (i == 0) { psnr_min = psnr_max = psnr; }
		else { if (psnr < psnr_min) psnr_min = psnr; if (psnr > psnr_max) psnr_max = psnr; }

		ok = true;
		stats_record(&stats, t1 - t0, ok);
		if ((i + 1) % 10 == 0) { printf("."); fflush(stdout); }
	}
	printf("\n");
	stats_print(&stats, "roundtrip");
	HPRINTF("PSNR(Y) over %d iters: min=%.3f max=%.3f drift=%.4f dB\n",
		stats.ok, psnr_min, psnr_max, psnr_max - psnr_min);
	if (stats.ok > 1 && (psnr_max - psnr_min) > 0.01)
		EPRINTF("PSNR drift > 0.01 dB — encoder/decoder state may not be stateless\n");
	leak_check("enx_vdma_jenc", base_enc);
	leak_check("enx_vdma_jdec", base_dec);
	ret = 0;
out:
	if (jenc_src.buf) ENX_DMA_JpegEnc_Buffer_Free(jenc_src.buf, jenc_src.size);
	if (jenc_dst.buf) ENX_DMA_JpegEnc_Buffer_Free(jenc_dst.buf, jenc_dst.size);
	if (jdec_src.buf) ENX_DMA_JpegDec_Buffer_Free(jdec_src.buf, jdec_src.size);
	if (jdec_dst.buf) ENX_DMA_JpegDec_Buffer_Free(jdec_dst.buf, jdec_dst.size);
	stats_free(&stats);
	if (!jdec_was && g_state.jdec_inited) {
		ENX_DMA_JpegDec_Exit(); g_state.jdec_inited = false; HPRINTF("[auto] JpegDec Exit\n");
	}
	if (!jenc_was && g_state.jenc_inited) {
		ENX_DMA_JpegEnc_Exit(); g_state.jenc_inited = false; HPRINTF("[auto] JpegEnc Exit\n");
	}
	return ret;
}

/*******************************************************************************
 *
 * Stress — Font loop (kernel mode only — Font_Update writes to dst buf)
 *
 ******************************************************************************/

int DEMO_StressFont(void)
{
	int N = ask_iters(1000);
	const int W = 640, H = 360;
	VDMA_FONT_CONFIG_S *cfg = NULL;
	struct buffer_alloc_track src = {0}, dst = {0};
	struct loop_stats stats = {0};
	long baseline;
	bool was_inited;
	int i, ret = -1;

	was_inited = g_state.font_inited;
	if (!was_inited) {
		if (ENX_DMA_Font_Init(ENX_VDMA_INIT_TYPE_KERNEL) != 0) {
			EPRINTF("auto Font Init failed.\n"); return -1;
		}
		g_state.font_inited = true;
		g_state.font_init_type = ENX_VDMA_INIT_TYPE_KERNEL;
		HPRINTF("[auto] Font Init (kernel)\n");
	}

	if (stats_init(&stats, N) < 0) { EPRINTF("stats_init failed.\n"); goto out; }

	cfg = ENX_DMA_Font_CfgAlloc(1, 0);
	if (!cfg) { EPRINTF("Font_CfgAlloc failed.\n"); goto out; }

	src.size = (size_t)W * H; /* Y-only src plane is enough for the loop */
	dst.size = ((size_t)W * H * 3) >> 1;
	src.buf  = ENX_DMA_Font_Buffer_Alloc(VDMA_KIND_SRC, src.size);
	if (!src.buf) { EPRINTF("font src alloc failed.\n"); goto out; }
	dst.buf  = ENX_DMA_Font_Buffer_Alloc(VDMA_KIND_DST, dst.size);
	if (!dst.buf) { EPRINTF("font dst alloc failed.\n"); goto out; }
	memset(src.buf, 0xFF, src.size);

	/* Baseline AFTER per-test allocs. */
	baseline = read_dbg_counter("enx_vdma_font", "stats", "bufs_active");

	cfg->font_info[0].src_addr_flag		= ENX_VDMA_ADDR_TYPE_VIRT;
	cfg->font_info[0].src_addr			= (unsigned long)(uintptr_t)src.buf;
	cfg->font_info[0].src_width			= W;
	cfg->font_info[0].src_height		= H;
	cfg->font_info[0].font_xpos			= 0;
	cfg->font_info[0].font_ypos			= 0;
	cfg->font_info[0].font_color_y		= 200;
	cfg->font_info[0].font_color_cb		= 128;
	cfg->font_info[0].font_color_cr		= 128;
	cfg->font_info[0].font_value		= 255;
	cfg->font_info[0].font_wr_mode		= 0;
	cfg->font_info[0].font_color_tone	= 1;
	cfg->font_info[0].font_alpha		= 256;
	cfg->dst_width						= W;
	cfg->dst_height						= H;
	cfg->background_overlay				= 0;
	cfg->dst_type_index					= ENX_VDMA_ADDR_TYPE_VIRT;
	cfg->font_info[0].dst_addr_y		= (unsigned long)(uintptr_t)dst.buf;

	HPRINTF("Font loop x %d (%dx%d) ...\n", N, W, H);
	for (i = 0; i < N; i++) {
		uint64_t t0 = now_ns();
		int r = ENX_DMA_Font_Update(cfg);
		uint64_t t1 = now_ns();
		stats_record(&stats, t1 - t0, r == 0);
		if ((i + 1) % 100 == 0) { printf("."); fflush(stdout); }
	}
	printf("\n");
	stats_print(&stats, "font");
	leak_check("enx_vdma_font", baseline);
	ret = 0;
out:
	if (src.buf) ENX_DMA_Font_Buffer_Free(src.buf, src.size);
	if (dst.buf) ENX_DMA_Font_Buffer_Free(dst.buf, dst.size);
	if (cfg) ENX_DMA_Font_CfgFree();
	stats_free(&stats);
	if (!was_inited && g_state.font_inited) {
		ENX_DMA_Font_Stop_All();
		ENX_DMA_Font_Exit();
		g_state.font_inited = false;
		HPRINTF("[auto] Font Exit\n");
	}
	return ret;
}

/*******************************************************************************
 *
 * Stress — Npu loop
 *
 ******************************************************************************/

int DEMO_StressNpu(void)
{
	int N = ask_iters(1000);
	const int W = 1280, H = 720;
	struct buffer_alloc_track src = {0}, dst = {0};
	VDMA_NPU_CONFIG_S cfg;
	struct loop_stats stats = {0};
	long baseline;
	bool was_inited;
	int i, ret = -1;

	was_inited = g_state.npu_inited;
	if (!was_inited) {
		if (ENX_DMA_Npu_Init() != 0) {
			EPRINTF("auto NPU Init failed.\n"); return -1;
		}
		g_state.npu_inited = true;
		HPRINTF("[auto] NPU Init\n");
	}

	if (stats_init(&stats, N) < 0) { EPRINTF("stats_init failed.\n"); goto out; }

	src.size = W * H * 4; /* RGBZ */
	dst.size = calculate_format_size(eFmt_YUV420SP_NV12, W, H);
	src.buf  = ENX_DMA_Npu_Buffer_Alloc(VDMA_KIND_SRC, src.size);
	if (!src.buf) { EPRINTF("src alloc failed.\n"); goto out; }
	dst.buf  = ENX_DMA_Npu_Buffer_Alloc(VDMA_KIND_DST, dst.size);
	if (!dst.buf) { EPRINTF("dst alloc failed.\n"); goto out; }

	/* Baseline AFTER per-test allocs. */
	baseline = read_dbg_counter("enx_vdma_npu", "stats", "bufs_active");

	fill_nv12_pattern(src.buf, W, H);

	memset(&cfg, 0, sizeof(cfg));
	cfg.src_cache_flush		= 1;
	cfg.dst_cache_flush		= 1;
	cfg.src_addr_flag		= ENX_VDMA_ADDR_TYPE_VIRT;
	cfg.dst_addr_flag		= ENX_VDMA_ADDR_TYPE_VIRT;
	cfg.src_addr			= (UInt64)(uintptr_t)src.buf;
	cfg.dst_addr			= (UInt64)(uintptr_t)dst.buf;
	cfg.src_width			= W;
	cfg.src_height			= H;
	cfg.read_mode			= 0; /* RGBZ */
	cfg.rgb_scale			= 0; /* Bypass */
	cfg.color_invert		= 0; /* No invert */

	HPRINTF("Npu loop x %d (%dx%d RGBZ -> %dx%d NV12) ...\n", N, W, H, W, H);
	for (i = 0; i < N; i++) {
		uint64_t t0 = now_ns();
		int r = ENX_DMA_Npu_Execute(&cfg);
		uint64_t t1 = now_ns();
		stats_record(&stats, t1 - t0, r == 0);
		if ((i + 1) % 100 == 0) { printf("."); fflush(stdout); }
	}
	printf("\n");
	stats_print(&stats, "npu");
	leak_check("enx_vdma_npu", baseline);
	ret = 0;
out:
	if (src.buf) ENX_DMA_Npu_Buffer_Free(src.buf, src.size);
	if (dst.buf) ENX_DMA_Npu_Buffer_Free(dst.buf, dst.size);
	stats_free(&stats);
	if (!was_inited && g_state.npu_inited) {
		ENX_DMA_Npu_Exit();
		g_state.npu_inited = false;
		HPRINTF("[auto] NPU Exit\n");
	}
	return ret;
}

/*******************************************************************************
 *
 * Stress — Alloc/Free churn (no Execute, lifecycle only)
 *
 * Exercises the buf/backing lifecycle path in isolation: each iteration
 * allocates one buffer then frees it. Catches regressions that the Execute
 * loops cannot — buf id leaks, backing kref imbalance, session list
 * corruption — because there is no Execute path in the way.
 *
 ******************************************************************************/

int DEMO_StressAllocFree(void)
{
	enum vmod_id m;
	int N;
	const size_t SIZE = 1024 * 1024;	/* 1 MB per buf, arbitrary */
	struct vmod_init_state ist = {0};
	struct loop_stats stats = {0};
	long baseline = -1;
	int i;

	m = ask_module();
	if (m == VMOD_INVALID) { EPRINTF("invalid module\n"); return -1; }
	N = ask_iters(10000);

	if (vmod_auto_init(m, &ist) < 0) { EPRINTF("auto Init failed.\n"); return -1; }
	if (stats_init(&stats, N) < 0) { EPRINTF("stats_init failed.\n"); goto out; }

	baseline = read_dbg_counter(vmod_dbg(m), "stats", "bufs_active");

	HPRINTF("Alloc/Free churn x %d on %s (size=%zu) ...\n", N, vmod_label(m), SIZE);
	for (i = 0; i < N; i++) {
		int kind = (i & 1) ? VDMA_KIND_SRC : VDMA_KIND_DST;
		void *buf;
		uint64_t t0 = now_ns();

		buf = vmod_alloc(m, kind, SIZE);
		if (!buf) { stats_record(&stats, now_ns() - t0, false); continue; }
		vmod_free(m, buf, SIZE);
		stats_record(&stats, now_ns() - t0, true);
		if ((i + 1) % 1000 == 0) { printf("."); fflush(stdout); }
	}
	printf("\n");
	stats_print(&stats, "alloc/free");
	leak_check(vmod_dbg(m), baseline);

out:
	stats_free(&stats);
	vmod_auto_exit(m, &ist);
	return 0;
}

/*******************************************************************************
 *
 * Stress — Quota exhaustion
 *
 * Allocs SRC bufs until the module rejects further allocation, then frees
 * everything and verifies the counter returns to the starting baseline.
 * Exercises the EDQUOT / inc-then-check enforcement path that the regular
 * loops never touch.
 *
 ******************************************************************************/

#define QSTRESS_MAX_BUFS 256

int DEMO_StressQuota(void)
{
	enum vmod_id m;
	const size_t SIZE = 1024 * 1024;
	struct vmod_init_state ist = {0};
	void *bufs[QSTRESS_MAX_BUFS] = {0};
	long baseline, after_alloc, final_cnt;
	int alloc_count = 0;
	int i;

	m = ask_module();
	if (m == VMOD_INVALID) { EPRINTF("invalid module\n"); return -1; }

	if (vmod_auto_init(m, &ist) < 0) { EPRINTF("auto Init failed.\n"); return -1; }

	baseline = read_dbg_counter(vmod_dbg(m), "stats", "bufs_active");
	HPRINTF("Quota exhaustion on %s (size=%zu, baseline=%ld) ...\n",
		vmod_label(m), SIZE, baseline);

	/* Alloc until the lib returns NULL — that's the quota cliff. */
	for (alloc_count = 0; alloc_count < QSTRESS_MAX_BUFS; alloc_count++) {
		errno = 0;
		bufs[alloc_count] = vmod_alloc(m, VDMA_KIND_SRC, SIZE);
		if (!bufs[alloc_count]) {
			HPRINTF("  alloc[%d] FAIL  errno=%d\n", alloc_count + 1, errno);
			break;
		}
	}
	HPRINTF("  alloc OK count: %d (cliff at %d, hard cap %d)\n",
		alloc_count, alloc_count + 1, QSTRESS_MAX_BUFS);

	after_alloc = read_dbg_counter(vmod_dbg(m), "stats", "bufs_active");
	if (after_alloc == baseline + alloc_count)
		HPRINTF("  counter consistent: bufs_active=%ld = baseline+alloc_count\n", after_alloc);
	else
		EPRINTF("  INCONSISTENT: bufs_active=%ld, expected %ld\n",
			after_alloc, baseline + alloc_count);

	for (i = 0; i < alloc_count; i++) vmod_free(m, bufs[i], SIZE);

	final_cnt = read_dbg_counter(vmod_dbg(m), "stats", "bufs_active");
	if (final_cnt == baseline)
		HPRINTF("  baseline restored: bufs_active=%ld\n", final_cnt);
	else
		EPRINTF("  LEAK after free-all: bufs_active=%ld (expected %ld)\n",
			final_cnt, baseline);

	vmod_auto_exit(m, &ist);
	return 0;
}

/*******************************************************************************
 *
 * Stress — Init/Exit cycle
 *
 * Cycles Init → tiny op → Exit repeatedly. Catches session lifecycle leaks
 * (sessions_open not returning to baseline) and any state that Init does
 * not re-establish after Exit. Requires the module to start in the un-
 * initialized state (any user Init must be undone first).
 *
 ******************************************************************************/

int DEMO_StressInitExit(void)
{
	enum vmod_id m;
	int N;
	const size_t SIZE = 64 * 1024;
	struct loop_stats stats = {0};
	long base_sess, base_bufs;
	int i;

	m = ask_module();
	if (m == VMOD_INVALID) { EPRINTF("invalid module\n"); return -1; }

	if (vmod_is_inited(m)) {
		EPRINTF("%s is already initialized — Exit it first via its own menu.\n",
			vmod_label(m));
		return -1;
	}

	N = ask_iters(100);
	if (stats_init(&stats, N) < 0) { EPRINTF("stats_init failed.\n"); return -1; }

	base_sess = read_dbg_counter(vmod_dbg(m), "stats", "sessions_open");
	base_bufs = read_dbg_counter(vmod_dbg(m), "stats", "bufs_active");
	HPRINTF("Init/Exit cycle x %d on %s (baseline sessions=%ld bufs=%ld) ...\n",
		N, vmod_label(m), base_sess, base_bufs);

	for (i = 0; i < N; i++) {
		struct vmod_init_state s = {0};
		void *buf;
		uint64_t t0 = now_ns();

		if (vmod_auto_init(m, &s) < 0) {
			stats_record(&stats, now_ns() - t0, false);
			continue;
		}
		buf = vmod_alloc(m, VDMA_KIND_SRC, SIZE);
		if (buf) vmod_free(m, buf, SIZE);
		vmod_auto_exit(m, &s);

		stats_record(&stats, now_ns() - t0, true);
		if ((i + 1) % 10 == 0) { printf("."); fflush(stdout); }
	}
	printf("\n");
	stats_print(&stats, "init/exit");

	{
		long now_sess = read_dbg_counter(vmod_dbg(m), "stats", "sessions_open");
		long now_bufs = read_dbg_counter(vmod_dbg(m), "stats", "bufs_active");
		if (now_sess == base_sess && now_bufs == base_bufs)
			HPRINTF("baseline   sessions=%ld bufs=%ld (stable)\n",
				now_sess, now_bufs);
		else
			EPRINTF("LEAK       sessions %ld -> %ld   bufs %ld -> %ld\n",
				base_sess, now_sess, base_bufs, now_bufs);
	}

	stats_free(&stats);
	return 0;
}

/*******************************************************************************
 *
 * Stress — Concurrent (pthread) for race verification
 *
 * Spawns T worker threads on a single module session. Each worker has its
 * own src/dst buffers but shares the device fd, so the test exercises :
 *	- dev->hw_lock serialization under contention
 *	- lib buf list / quota under concurrent alloc/free + lookup
 *	- submit path concurrency (workqueue + per-session vs->jobs)
 *
 * Limited to Scaler and JpegEnc:
 *	- Font's lib has ENX_DMA_Font_CfgFree(void) that frees ALL configs at
 *	  once — not thread-safe across workers.
 *	- JpegDec needs a valid JPEG bitstream as input — prep adds noise to
 *	  the race test. Skipped for now.
 *
 * All workers wait at a barrier before entering the Execute loop so the
 * test sees maximum concurrent pressure from t=0.
 *
 ******************************************************************************/

struct conc_arg {
	int					thread_id;
	int					iters;
	enum vmod_id		mod;
	pthread_barrier_t	*start;
	struct loop_stats	stats;
	int					alloc_failed;	/* set if per-thread alloc failed */
	/* JpegDec specific — shared, read-only across workers */
	const unsigned char	*jpeg_data;
	size_t				jpeg_size;
	size_t				yuv_size;		/* expected decoded NV12 size */
};

static void *conc_scaler_worker(void *p)
{
	struct conc_arg *a = (struct conc_arg *)p;
	const int W = 1280, H = 720, OW = 640, OH = 360;
	void *src = NULL, *dst = NULL;
	size_t src_size, dst_size;
	VDMA_DZ_CONFIG_S cfg;
	int i;

	src_size = calculate_format_size(eFmt_YUV420SP_NV12, W, H);
	dst_size = calculate_format_size(eFmt_YUV420SP_NV12, OW, OH);
	src = ENX_DMA_Scaler_Buffer_Alloc(ENX_VDMA_SCALER0, VDMA_KIND_SRC, src_size);
	dst = ENX_DMA_Scaler_Buffer_Alloc(ENX_VDMA_SCALER0, VDMA_KIND_DST, dst_size);
	if (!src || !dst) { a->alloc_failed = 1; goto out; }
	fill_nv12_pattern(src, W, H);

	memset(&cfg, 0, sizeof(cfg));
	cfg.src_cache_flush		= cfg.dst_cache_flush	= 1;
	cfg.src_addr_flag		= cfg.dst_addr_flag		= ENX_VDMA_ADDR_TYPE_VIRT;
	cfg.src_addr			= (UInt64)(uintptr_t)src;
	cfg.src_data_offset		= 0;
	cfg.dst_addr			= (UInt64)(uintptr_t)dst;
	cfg.dst_data_offset		= 0;
	cfg.src_width_total		= cfg.src_width	 = W;
	cfg.src_height_total	= cfg.src_height = H;
	cfg.src_format			= eFmt_YUV420SP_NV12;
	cfg.dst_width_total		= cfg.dst_width  = OW;
	cfg.dst_height			= OH;
	cfg.dst_format			= eFmt_YUV420SP_NV12;

	pthread_barrier_wait(a->start);

	for (i = 0; i < a->iters; i++) {
		uint64_t t0 = now_ns();
		int r = ENX_DMA_Scaler_Execute(ENX_VDMA_SCALER0, &cfg);
		uint64_t t1 = now_ns();
		stats_record(&a->stats, t1 - t0, r == 0);
	}

out:
	if (src) ENX_DMA_Scaler_Buffer_Free(ENX_VDMA_SCALER0, src, src_size);
	if (dst) ENX_DMA_Scaler_Buffer_Free(ENX_VDMA_SCALER0, dst, dst_size);
	return NULL;
}

static void *conc_jenc_worker(void *p)
{
	struct conc_arg *a = (struct conc_arg *)p;
	const int W = 1280, H = 720;
	void *src = NULL, *dst = NULL;
	size_t src_size, dst_size;
	VDMA_JPEG_ENC_CONFIG_S cfg;
	int i;

	src_size = calculate_format_size(eFmt_YUV420SP_NV12, W, H);
	dst_size = (size_t)W * H * 4;
	src = ENX_DMA_JpegEnc_Buffer_Alloc(VDMA_KIND_SRC, src_size);
	dst = ENX_DMA_JpegEnc_Buffer_Alloc(VDMA_KIND_DST, dst_size);
	if (!src || !dst) { a->alloc_failed = 1; goto out; }
	fill_nv12_pattern(src, W, H);

	memset(&cfg, 0, sizeof(cfg));
	cfg.src_cache_flush		= cfg.dst_cache_flush = 1;
	cfg.src_addr_flag		= cfg.dst_addr_flag   = ENX_VDMA_ADDR_TYPE_VIRT;
	cfg.src_addr			= (UInt64)(uintptr_t)src;
	cfg.src_data_offset		= 0;
	cfg.dst_addr			= (UInt64)(uintptr_t)dst;
	cfg.dst_data_offset		= 0;
	cfg.src_width_total		= cfg.src_width  = W;
	cfg.src_height_total	= cfg.src_height = H;
	cfg.src_format			= eFmt_YUV420SP_NV12;
	cfg.quality				= 75;

	pthread_barrier_wait(a->start);

	for (i = 0; i < a->iters; i++) {
		size_t out_size = 0;
		uint64_t t0 = now_ns();
		int r = ENX_DMA_JpegEnc_Execute(&cfg, &out_size);
		uint64_t t1 = now_ns();
		stats_record(&a->stats, t1 - t0, r == 0 && out_size > 0);
	}

out:
	if (src) ENX_DMA_JpegEnc_Buffer_Free(src, src_size);
	if (dst) ENX_DMA_JpegEnc_Buffer_Free(dst, dst_size);
	return NULL;
}

/*
 * Font concurrent worker — each thread CfgAlloc's its own config and runs
 * the Update loop. The matching CfgFree happens later in vmod_auto_exit
 * (single-threaded) so workers never touch the global lib-internal cfg
 * list mid-flight.
 *
 * Uses ID-mode addressing throughout so the lib's verify_buf_info() never
 * has to mutate cfg fields concurrently.
 */
static void *conc_font_worker(void *p)
{
	struct conc_arg *a = (struct conc_arg *)p;
	const int W = 640, H = 360;
	VDMA_FONT_CONFIG_S *cfg = NULL;
	void *src = NULL, *dst = NULL;
	UInt32 src_id = 0, dst_id = 0;
	size_t src_size, dst_size;
	int i;

	src_size = (size_t)W * H;				/* Y-only src */
	dst_size = ((size_t)W * H * 3) >> 1;	/* NV12 dst   */

	cfg = ENX_DMA_Font_CfgAlloc(1, 0);
	if (!cfg) { a->alloc_failed = 1; goto out; }

	src = ENX_DMA_Font_Buffer_Alloc_EX(VDMA_KIND_SRC, src_size, &src_id);
	dst = ENX_DMA_Font_Buffer_Alloc_EX(VDMA_KIND_DST, dst_size, &dst_id);
	if (!src || !dst) { a->alloc_failed = 1; goto out; }
	memset(src, 0xFF, src_size);

	cfg->font_info[0].src_addr_flag		= ENX_VDMA_ADDR_TYPE_ID;
	cfg->font_info[0].src_addr			= src_id;
	cfg->font_info[0].src_data_offset	= 0;
	cfg->font_info[0].src_width			= W;
	cfg->font_info[0].src_height		= H;
	cfg->font_info[0].font_xpos			= 0;
	cfg->font_info[0].font_ypos			= 0;
	cfg->font_info[0].font_color_y		= 200;
	cfg->font_info[0].font_color_cb		= 128;
	cfg->font_info[0].font_color_cr		= 128;
	cfg->font_info[0].font_value		= 255;
	cfg->font_info[0].font_wr_mode		= 0;
	cfg->font_info[0].font_color_tone	= 1;
	cfg->font_info[0].font_alpha		= 256;
	cfg->dst_width						= W;
	cfg->dst_height						= H;
	cfg->background_overlay				= 0;
	cfg->dst_type_index					= ENX_VDMA_ADDR_TYPE_ID;
	cfg->dst_data_offset				= 0;
	cfg->font_info[0].dst_addr_y		= dst_id;

	pthread_barrier_wait(a->start);

	for (i = 0; i < a->iters; i++) {
		uint64_t t0 = now_ns();
		int r = ENX_DMA_Font_Update(cfg);
		uint64_t t1 = now_ns();
		stats_record(&a->stats, t1 - t0, r == 0);
	}

out:
	if (src) ENX_DMA_Font_Buffer_Free(src, src_size);
	if (dst) ENX_DMA_Font_Buffer_Free(dst, dst_size);
	/* NOTE: NO CfgFree here — vmod_auto_exit will free all cfgs at end. */
	return NULL;
}

/*
 * JpegDec concurrent worker — each thread allocates its own src/dst, then
 * copies the shared (read-only) JPEG bitstream supplied by the main thread
 * into its src and runs the Decode loop. yuv_size was pre-computed from
 * the JPEG header by the main thread.
 */
static void *conc_jdec_worker(void *p)
{
	struct conc_arg *a = (struct conc_arg *)p;
	void *src = NULL, *dst = NULL;
	size_t src_size = a->jpeg_size;
	size_t dst_size = a->yuv_size;
	VDMA_JPEG_DEC_CONFIG_S cfg;
	int i;

	src = ENX_DMA_JpegDec_Buffer_Alloc(VDMA_KIND_SRC, src_size);
	dst = ENX_DMA_JpegDec_Buffer_Alloc(VDMA_KIND_DST, dst_size);
	if (!src || !dst) { a->alloc_failed = 1; goto out; }
	memcpy(src, a->jpeg_data, src_size);

	memset(&cfg, 0, sizeof(cfg));
	cfg.src_cache_flush	= cfg.dst_cache_flush	= 1;
	cfg.src_addr_flag	= cfg.dst_addr_flag		= ENX_VDMA_ADDR_TYPE_VIRT;
	cfg.src_addr		= (UInt64)(uintptr_t)src;
	cfg.src_data_offset	= 0;
	cfg.dst_addr		= (UInt64)(uintptr_t)dst;
	cfg.dst_data_offset	= 0;
	cfg.src_jpeg_size	= (unsigned int)src_size;
	cfg.dst_buf_size	= (unsigned int)dst_size;

	pthread_barrier_wait(a->start);

	for (i = 0; i < a->iters; i++) {
		size_t yuv_out = 0;
		uint64_t t0 = now_ns();
		int r = ENX_DMA_JpegDec_Execute(&cfg, &yuv_out);
		uint64_t t1 = now_ns();
		stats_record(&a->stats, t1 - t0, r == 0 && yuv_out > 0);
	}

out:
	if (src) ENX_DMA_JpegDec_Buffer_Free(src, src_size);
	if (dst) ENX_DMA_JpegDec_Buffer_Free(dst, dst_size);
	return NULL;
}

static void *conc_npu_worker(void *p)
{
	struct conc_arg *a = (struct conc_arg *)p;
	const int W = 1280, H = 720;
	void *src = NULL, *dst = NULL;
	size_t src_size, dst_size;
	VDMA_NPU_CONFIG_S cfg;
	int i;

	src_size = W * H * 4; /* RGBZ */
	dst_size = calculate_format_size(eFmt_YUV420SP_NV12, W,  H);
	src = ENX_DMA_Npu_Buffer_Alloc(VDMA_KIND_SRC, src_size);
	dst = ENX_DMA_Npu_Buffer_Alloc(VDMA_KIND_DST, dst_size);
	if (!src || !dst) { a->alloc_failed = 1; goto out; }
	fill_npu_input_pattern(src, W, H);

	memset(&cfg, 0, sizeof(cfg));
	cfg.src_cache_flush		= cfg.dst_cache_flush	= 1;
	cfg.src_addr_flag		= cfg.dst_addr_flag		= ENX_VDMA_ADDR_TYPE_VIRT;
	cfg.src_addr			= (UInt64)(uintptr_t)src;
	cfg.src_data_offset		= 0;
	cfg.dst_addr			= (UInt64)(uintptr_t)dst;
	cfg.dst_data_offset		= 0;
	cfg.src_width			= W;
	cfg.src_height			= H;
	cfg.read_mode			= 0; /* RGBZ */
	cfg.rgb_scale			= 0; /* Bypass */
	cfg.color_invert		= 0; /* No invert */

	pthread_barrier_wait(a->start);

	for (i = 0; i < a->iters; i++) {
		uint64_t t0 = now_ns();
		int r = ENX_DMA_Npu_Execute(&cfg);
		uint64_t t1 = now_ns();
		stats_record(&a->stats, t1 - t0, r == 0);
	}

out:
	if (src) ENX_DMA_Npu_Buffer_Free(src, src_size);
	if (dst) ENX_DMA_Npu_Buffer_Free(dst, dst_size);
	return NULL;
}

int DEMO_StressConcurrent(void)
{
	char cIn[256];
	enum vmod_id m;
	int T, N;
	pthread_t *tids = NULL;
	struct conc_arg *args = NULL;
	pthread_barrier_t start;
	struct vmod_init_state ist = {0};
	void *(*worker)(void *) = NULL;
	long baseline = -1;
	int i, alloc_fail_count = 0;
	uint64_t ok_total = 0, fail_total = 0;
	uint64_t aggregate_total_ns = 0;
	/* JpegDec prep (only used when m == VMOD_JDEC) */
	unsigned char *jpeg_data = NULL;
	size_t jpeg_size = 0, yuv_size = 0;
	int ret = -1;

	printf("Module? (1: scaler0, 2: jenc, 3: font, 4: jdec, 5: npu) : ");
	sGetMenu(cIn);
	switch (atoi(cIn)) {
	case 1: m = VMOD_SCALER0;	worker = conc_scaler_worker;	break;
	case 2: m = VMOD_JENC;		worker = conc_jenc_worker;		break;
	case 3: m = VMOD_FONT;		worker = conc_font_worker;		break;
	case 4: m = VMOD_JDEC;		worker = conc_jdec_worker;		break;
	case 5: m = VMOD_NPU;		worker = conc_npu_worker;		break;
	default: 					EPRINTF("invalid module\n");	return -1;
	}

	/* For JpegDec, ask the user for a JPEG file. We parse the header to
	 * pre-compute the expected YUV output size, then load the file once
	 * into a shared buffer; workers copy from this into their per-thread
	 * src buf and decode. */
	if (m == VMOD_JDEC) {
		char path[256];
		unsigned short w = 0, h = 0;
		ENX_VDMA_FORMAT_E fmt = eFmt_YUV444;

		printf("JPEG file path : ");
		sGetMenu(path);
		jpeg_size = file_get_jpeg_size(path, &w, &h, &fmt);
		if (!jpeg_size) { EPRINTF("file_get_jpeg_size failed for '%s'\n", path); return -1; }
		if (w & 1) w++;
		switch (fmt) {
		case eFmt_YUV444:			yuv_size = (size_t)w * h * 3;			break;
		case eFmt_YUV422_UYVY:		yuv_size = (size_t)w * h * 2;			break;
		case eFmt_YUV420SP_NV12:	yuv_size = ((size_t)w * h * 3) >> 1;	break;
		case eFmt_YUV400:			yuv_size = (size_t)w * h;				break;
		default: 					EPRINTF("unsupported JPEG fmt\n");		return -1;
		}
		jpeg_data = (unsigned char *)malloc(jpeg_size);
		if (!jpeg_data) { EPRINTF("malloc for JPEG failed\n"); return -1; }
		if (file_read(path, jpeg_data, jpeg_size)) {
			EPRINTF("file_read failed for '%s'\n", path);
			free(jpeg_data); return -1;
		}
		HPRINTF("JPEG loaded: %s  size=%zu  yuv_out=%zu\n", path, jpeg_size, yuv_size);
	}

	T = ask_thread_count(2);
	N = ask_iters(1000);

	if (vmod_auto_init(m, &ist) < 0) { EPRINTF("auto Init failed.\n"); goto out_free_jpeg; }

	baseline = read_dbg_counter(vmod_dbg(m), "stats", "bufs_active");

	tids = (pthread_t *)calloc((size_t)T, sizeof(*tids));
	args = (struct conc_arg *)calloc((size_t)T, sizeof(*args));
	if (!tids || !args) { EPRINTF("calloc failed.\n"); goto out; }
	if (pthread_barrier_init(&start, NULL, (unsigned)T) != 0) {
		EPRINTF("barrier_init failed.\n"); goto out;
	}

	for (i = 0; i < T; i++) {
		args[i].thread_id	= i;
		args[i].iters		= N;
		args[i].mod			= m;
		args[i].start		= &start;
		args[i].jpeg_data	= jpeg_data;	/* NULL for non-JDEC modules */
		args[i].jpeg_size	= jpeg_size;
		args[i].yuv_size	= yuv_size;
		if (stats_init(&args[i].stats, N) < 0) { EPRINTF("stats_init failed.\n"); goto barrier_destroy; }
	}

	HPRINTF("Concurrent stress on %s : T=%d threads x N=%d iters each ...\n",
		vmod_label(m), T, N);

	for (i = 0; i < T; i++) {
		if (pthread_create(&tids[i], NULL, worker, &args[i]) != 0) {
			EPRINTF("pthread_create[%d] failed: %s\n", i, strerror(errno));
			/* Release barrier waiters that already entered. Skip cleanly. */
			goto join_started;
		}
	}

	for (i = 0; i < T; i++) pthread_join(tids[i], NULL);

	/* Per-thread + aggregate reporting */
	for (i = 0; i < T; i++) {
		char label[16];
		snprintf(label, sizeof(label), "T%d", i);
		if (args[i].alloc_failed) {
			EPRINTF("  T%d: alloc failed (skipped)\n", i);
			alloc_fail_count++;
			continue;
		}
		stats_print(&args[i].stats, label);
		ok_total			+= (uint64_t)args[i].stats.ok;
		fail_total			+= (uint64_t)args[i].stats.fail;
		aggregate_total_ns	+= args[i].stats.total_ns;
	}

	if (ok_total > 0) {
		double secs = (double)aggregate_total_ns / 1.0e9 / (double)(T - alloc_fail_count);
		HPRINTF("aggregate  ok=%llu fail=%llu  wall %.3f s  combined %.1f ops/s\n",
			(unsigned long long)ok_total, (unsigned long long)fail_total,
			secs, (double)ok_total / secs);
	}

	leak_check(vmod_dbg(m), baseline);
	ret = 0;
	goto barrier_destroy;

join_started:
	/* If pthread_create failed mid-spawn, the others may be waiting at the
	 * barrier. We cannot easily abort them; the safest action is to leak
	 * the partial state and exit. This path is exceptional. */
	EPRINTF("concurrent stress aborted mid-spawn (partial threads may leak)\n");

barrier_destroy:
	pthread_barrier_destroy(&start);
out:
	if (args) {
		for (i = 0; i < T; i++) stats_free(&args[i].stats);
		free(args);
	}
	free(tids);
	vmod_auto_exit(m, &ist);
out_free_jpeg:
	free(jpeg_data);
	return ret;
}