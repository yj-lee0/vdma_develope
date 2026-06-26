/*
 * Copyright (C) 2026 Eyenix Corporation
 * Eyenix <support6@eyenix.com>
 *
 * VDMA demo — cross-device pipeline (case 9 sub-menu).
 *
 * Exercises the buffer-sharing API:
 *	ENX_DMA_Buffer_Connect		(same-process one-shot)
 *	ENX_DMA_Buffer_Export_Fd	(provider side, returns fd)
 *	ENX_DMA_Buffer_Import_Fd	(consumer side, takes fd)
 *
 * Scenarios:
 *	1. Roundtrip via Connect	(memcpy-free PSNR check)
 *	2. Font  -> JpegEnc			(Connect)
 *	3. Scaler -> JpegEnc		(Connect)
 *	4. JpegDec -> Scaler		(Connect)
 *	5. Export_Fd/Import_Fd		(same-process sanity)
 *	6. Fork + SCM_RIGHTS		(cross-process verification)
 */
#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include "demo_vdma.h"
#include "demo_vdma_util.h"


/*******************************************************************************
 *
 * Pipeline — exercises the cross-device buffer sharing API
 *	ENX_DMA_Buffer_Connect		(same-process one-shot)
 *	ENX_DMA_Buffer_Export_Fd	(provider side, returns fd)
 *	ENX_DMA_Buffer_Import_Fd	(consumer side, takes fd)
 *
 * Each entry auto-Init's only what it needs and Exit's only what it Init'd,
 * preserving the user's prior module init state.
 *
 ******************************************************************************/

/*
 * 1. Roundtrip via Connect — same path as DEMO_RoundtripExec but the
 * JpegEnc dst → JpegDec src hand-off uses Buffer_Connect instead of
 * memcpy. PSNR(Y) should match the memcpy baseline within float noise.
 */
int DEMO_PipelineRoundtripConnect(void)
{
	VDMA_JPEG_ENC_CONFIG_S enc_cfg;
	VDMA_JPEG_DEC_CONFIG_S dec_cfg;
	struct buffer_alloc_track jenc_src = {0}, jenc_dst = {0}, jdec_dst = {0};
	vdma_addr_t jdec_src = NULL;
	const int W = 1280, H = 720;
	size_t nv12_size, jpeg_buf_size, jpeg_out = 0, yuv_out = 0;
	double psnr;
	bool jenc_was, jdec_was;
	int ret = -1;

	jenc_was = g_state.jenc_inited;
	jdec_was = g_state.jdec_inited;
	if (!jenc_was) {
		if (ENX_DMA_JpegEnc_Init() != 0) { EPRINTF("auto JpegEnc Init failed.\n"); return -1; }
		g_state.jenc_inited = true; HPRINTF("[auto] JpegEnc Init\n");
	}
	if (!jdec_was) {
		if (ENX_DMA_JpegDec_Init() != 0) { EPRINTF("auto JpegDec Init failed.\n"); goto err; }
		g_state.jdec_inited = true; HPRINTF("[auto] JpegDec Init\n");
	}

	nv12_size		= calculate_format_size(eFmt_YUV420SP_NV12, W, H);
	jpeg_buf_size	= (size_t)W * H * 4;

	/* JpegEnc */
	memset(&enc_cfg, 0, sizeof(enc_cfg));
	enc_cfg.src_cache_flush		= enc_cfg.dst_cache_flush	= 1;
	enc_cfg.src_addr_flag		= enc_cfg.dst_addr_flag		= ENX_VDMA_ADDR_TYPE_VIRT;
	enc_cfg.src_width_total		= enc_cfg.src_width			= W;
	enc_cfg.src_height_total	= enc_cfg.src_height		= H;
	enc_cfg.src_format			= eFmt_YUV420SP_NV12;
	enc_cfg.quality				= 75;

	jenc_src.size = nv12_size;
	jenc_dst.size = jpeg_buf_size;
	jenc_src.buf  = ENX_DMA_JpegEnc_Buffer_Alloc(VDMA_KIND_SRC, jenc_src.size);
	jenc_dst.buf  = ENX_DMA_JpegEnc_Buffer_Alloc(VDMA_KIND_DST, jenc_dst.size);
	if (!jenc_src.buf || !jenc_dst.buf) { EPRINTF("jenc alloc failed.\n"); goto err; }
	fill_nv12_pattern(jenc_src.buf, W, H);
	enc_cfg.src_addr 		= (UInt64)(uintptr_t)jenc_src.buf;
	enc_cfg.src_data_offset	= 0;
	enc_cfg.dst_addr 		= (UInt64)(uintptr_t)jenc_dst.buf;
	enc_cfg.dst_data_offset	= 0;

	if (ENX_DMA_JpegEnc_Execute(&enc_cfg, &jpeg_out) != 0 || !jpeg_out) {
		EPRINTF("JpegEnc_Execute failed.\n"); goto err;
	}
	HPRINTF("JpegEnc done: %zu -> %zu bytes (q=75)\n", jenc_src.size, jpeg_out);

	/* Connect : JpegEnc.dst → JpegDec.src (NO memcpy) */
	{
		Int32 r = ENX_DMA_Buffer_Connect(
			ENX_VDMA_JPEG_ENC, jenc_dst.buf,
			ENX_VDMA_JPEG_DEC, VDMA_KIND_SRC, &jdec_src);
		if (r < 0 || !jdec_src) {
			EPRINTF("Buffer_Connect (jenc_dst → jdec_src) failed: %d\n", r);
			goto err;
		}
		HPRINTF("Connected: jenc_dst(%p) → jdec_src(%p)\n", jenc_dst.buf, jdec_src);
	}

	/* JpegDec */
	jdec_dst.size	= nv12_size;
	jdec_dst.buf	= ENX_DMA_JpegDec_Buffer_Alloc(VDMA_KIND_DST, jdec_dst.size);
	if (!jdec_dst.buf) { EPRINTF("jdec dst alloc failed.\n"); goto err; }

	memset(&dec_cfg, 0, sizeof(dec_cfg));
	dec_cfg.src_cache_flush	= dec_cfg.dst_cache_flush	= 1;
	dec_cfg.src_addr_flag	= dec_cfg.dst_addr_flag		= ENX_VDMA_ADDR_TYPE_VIRT;
	dec_cfg.src_addr		= (UInt64)(uintptr_t)jdec_src;
	dec_cfg.src_data_offset	= 0;
	dec_cfg.dst_addr		= (UInt64)(uintptr_t)jdec_dst.buf;
	dec_cfg.dst_data_offset	= 0;
	dec_cfg.src_jpeg_size	= (unsigned int)jpeg_out;
	dec_cfg.dst_buf_size	= (unsigned int)jdec_dst.size;

	if (ENX_DMA_JpegDec_Execute(&dec_cfg, &yuv_out) != 0 || !yuv_out) {
		EPRINTF("JpegDec_Execute failed.\n"); goto err;
	}
	HPRINTF("JpegDec done: %zu -> %zu bytes (via Connect)\n", jpeg_out, yuv_out);

	psnr = compute_psnr_y(jenc_src.buf, jdec_dst.buf, W, H);
	if (isinf(psnr))
		HPRINTF("PSNR(Y): infinite (bit-identical, unexpected for lossy JPEG)\n");
	else
		HPRINTF("PSNR(Y): %.2f dB  %s  [Connect-based path]\n", psnr,
			psnr >= 30.0 ? "(OK)" : psnr >= 20.0 ? "(suspect)" : "(BROKEN)");

	ret = 0;
err:
	if (jdec_dst.buf)	ENX_DMA_JpegDec_Buffer_Free(jdec_dst.buf, jdec_dst.size);
	if (jdec_src)		ENX_DMA_JpegDec_Buffer_Free(jdec_src, jpeg_out);	/* Connect-imported view */
	if (jenc_dst.buf)	ENX_DMA_JpegEnc_Buffer_Free(jenc_dst.buf, jenc_dst.size);
	if (jenc_src.buf)	ENX_DMA_JpegEnc_Buffer_Free(jenc_src.buf, jenc_src.size);
	if (!jdec_was && g_state.jdec_inited) { ENX_DMA_JpegDec_Exit(); g_state.jdec_inited = false; HPRINTF("[auto] JpegDec Exit\n"); }
	if (!jenc_was && g_state.jenc_inited) { ENX_DMA_JpegEnc_Exit(); g_state.jenc_inited = false; HPRINTF("[auto] JpegEnc Exit\n"); }
	return ret;
}

/*
 * 2. Font → JpegEnc via Connect — Font writes the overlay, JpegEnc reads
 * the same backing as its source. Saves the resulting JPEG.
 * Requires Font in KERNEL mode (so dst is a real memory backing).
 */
int DEMO_PipelineFontJenc(void)
{
	VDMA_FONT_CONFIG_S *fcfg = NULL;
	VDMA_JPEG_ENC_CONFIG_S ecfg;
	struct buffer_alloc_track f_src = {0}, f_dst = {0}, e_dst = {0};
	vdma_addr_t e_src = NULL;
	UInt32 fsrc_id = 0, fdst_id = 0;
	const int W = 640, H = 360;
	size_t dst_size, jpeg_out = 0;
	bool font_was, jenc_was;
	int ret = -1;

	font_was = g_state.font_inited;
	jenc_was = g_state.jenc_inited;
	if (!font_was) {
		if (ENX_DMA_Font_Init(ENX_VDMA_INIT_TYPE_KERNEL) != 0) { EPRINTF("auto Font Init failed.\n"); return -1; }
		g_state.font_inited = true; g_state.font_init_type = ENX_VDMA_INIT_TYPE_KERNEL;
		HPRINTF("[auto] Font Init (kernel)\n");
	}
	if (!jenc_was) {
		if (ENX_DMA_JpegEnc_Init() != 0) { EPRINTF("auto JpegEnc Init failed.\n"); goto err; }
		g_state.jenc_inited = true; HPRINTF("[auto] JpegEnc Init\n");
	}

	/* Font setup */
	fcfg = ENX_DMA_Font_CfgAlloc(1, 0);
	if (!fcfg) { EPRINTF("Font_CfgAlloc failed.\n"); goto err; }

	f_src.size	= (size_t)W * H;				/* Y-only font img */
	dst_size	= ((size_t)W * H * 3) >> 1;	/* NV12 dst */
	f_src.buf	= ENX_DMA_Font_Buffer_Alloc_EX(VDMA_KIND_SRC, f_src.size, &fsrc_id);
	f_dst.buf	= ENX_DMA_Font_Buffer_Alloc_EX(VDMA_KIND_DST, dst_size, &fdst_id);
	f_dst.size	= dst_size;
	if (!f_src.buf || !f_dst.buf) { EPRINTF("Font alloc failed.\n"); goto err; }
	memset(f_src.buf, 0xFF, f_src.size);	/* solid src for visible overlay */

	fcfg->font_info[0].src_addr_flag	= ENX_VDMA_ADDR_TYPE_ID;
	fcfg->font_info[0].src_addr			= fsrc_id;
	fcfg->font_info[0].src_data_offset	= 0;
	fcfg->font_info[0].src_width		= W;
	fcfg->font_info[0].src_height		= H;
	fcfg->font_info[0].font_color_y		= 200;
	fcfg->font_info[0].font_color_cb	= 128;
	fcfg->font_info[0].font_color_cr	= 128;
	fcfg->font_info[0].font_value		= 255;
	fcfg->font_info[0].font_wr_mode		= 0;
	fcfg->font_info[0].font_color_tone	= 1;
	fcfg->font_info[0].font_alpha		= 256;
	fcfg->dst_width						= W;
	fcfg->dst_height					= H;
	fcfg->background_overlay			= 0;
	fcfg->dst_type_index				= ENX_VDMA_ADDR_TYPE_ID;
	fcfg->font_info[0].dst_addr_y		= fdst_id;
	fcfg->dst_data_offset				= 0;

	if (ENX_DMA_Font_Update(fcfg) != 0) { EPRINTF("Font_Update failed.\n"); goto err; }
	HPRINTF("Font_Update done: %dx%d NV12 in f_dst\n", W, H);

	/* Connect : Font.dst → JpegEnc.src */
	{
		Int32 r = ENX_DMA_Buffer_Connect(
			ENX_VDMA_FONT, f_dst.buf,
			ENX_VDMA_JPEG_ENC, VDMA_KIND_SRC, &e_src);
		if (r < 0 || !e_src) {
			EPRINTF("Buffer_Connect (font_dst → jenc_src) failed: %d\n", r);
			goto err;
		}
		HPRINTF("Connected: font_dst(%p) → jenc_src(%p)\n", f_dst.buf, e_src);
	}

	/* JpegEnc */
	e_dst.size = (size_t)W * H * 4;
	e_dst.buf  = ENX_DMA_JpegEnc_Buffer_Alloc(VDMA_KIND_DST, e_dst.size);
	if (!e_dst.buf) { EPRINTF("jenc dst alloc failed.\n"); goto err; }

	memset(&ecfg, 0, sizeof(ecfg));
	ecfg.src_cache_flush	= ecfg.dst_cache_flush	= 1;
	ecfg.src_addr_flag		= ecfg.dst_addr_flag	= ENX_VDMA_ADDR_TYPE_VIRT;
	ecfg.src_addr			= (UInt64)(uintptr_t)e_src;
	ecfg.src_data_offset	= 0;
	ecfg.dst_addr			= (UInt64)(uintptr_t)e_dst.buf;
	ecfg.dst_data_offset	= 0;
	ecfg.src_width_total	= ecfg.src_width	= W;
	ecfg.src_height_total	= ecfg.src_height	= H;
	ecfg.src_format			= eFmt_YUV420SP_NV12;
	ecfg.quality			= 75;
	if (ENX_DMA_JpegEnc_Execute(&ecfg, &jpeg_out) != 0 || !jpeg_out) {
		EPRINTF("JpegEnc_Execute failed.\n"); goto err;
	}

	{
		char name[64];
		snprintf(name, sizeof(name), "pipeline_font_jenc_%dx%d.jpg", W, H);
		file_newname(name);
		if (file_write(name, e_dst.buf, jpeg_out) == 0)
			HPRINTF("saved: %s (%zu bytes)\n", name, jpeg_out);
	}
	ret = 0;

err:
	if (e_dst.buf)	ENX_DMA_JpegEnc_Buffer_Free(e_dst.buf, e_dst.size);
	if (e_src)		ENX_DMA_JpegEnc_Buffer_Free(e_src, f_dst.size);	/* Connect view */
	if (f_dst.buf)	ENX_DMA_Font_Buffer_Free(f_dst.buf, f_dst.size);
	if (f_src.buf)	ENX_DMA_Font_Buffer_Free(f_src.buf, f_src.size);
	/* CfgFree happens in auto-exit below for safety */
	if (!jenc_was && g_state.jenc_inited) { ENX_DMA_JpegEnc_Exit(); g_state.jenc_inited = false; HPRINTF("[auto] JpegEnc Exit\n"); }
	if (!font_was && g_state.font_inited) {
		ENX_DMA_Font_Stop_All(); ENX_DMA_Font_CfgFree(); ENX_DMA_Font_Exit();
		g_state.font_inited = false; HPRINTF("[auto] Font Exit\n");
	} else if (fcfg) {
		ENX_DMA_Font_CfgFree();	/* user kept Font Init'd; just drop our cfg */
	}
	return ret;
}

/*
 * 3. Scaler → JpegEnc via Connect — resize then compress with no memcpy.
 */
int DEMO_PipelineScalerJenc(void)
{
	VDMA_DZ_CONFIG_S scfg;
	VDMA_JPEG_ENC_CONFIG_S ecfg;
	struct buffer_alloc_track s_src = {0}, s_dst = {0}, e_dst = {0};
	vdma_addr_t e_src = NULL;
	const int W = 1280, H = 720, OW = 640, OH = 360;
	size_t jpeg_out = 0;
	bool scl_was, jenc_was;
	int ret = -1;

	scl_was  = g_state.scaler_inited[0];
	jenc_was = g_state.jenc_inited;
	if (!scl_was) {
		if (ENX_DMA_Scaler_Init(ENX_VDMA_SCALER0) != 0) {
			EPRINTF("auto Scaler Init failed.\n"); return -1;
		}
		g_state.scaler_inited[0] = true; HPRINTF("[auto] Scaler Init\n");
	}
	if (!jenc_was) {
		if (ENX_DMA_JpegEnc_Init() != 0) { EPRINTF("auto JpegEnc Init failed.\n"); goto err; }
		g_state.jenc_inited = true; HPRINTF("[auto] JpegEnc Init\n");
	}

	/* Scaler : 1280x720 → 640x360 */
	s_src.size	= calculate_format_size(eFmt_YUV420SP_NV12, W, H);
	s_dst.size	= calculate_format_size(eFmt_YUV420SP_NV12, OW, OH);
	s_src.buf	= ENX_DMA_Scaler_Buffer_Alloc(ENX_VDMA_SCALER0, VDMA_KIND_SRC, s_src.size);
	s_dst.buf	= ENX_DMA_Scaler_Buffer_Alloc(ENX_VDMA_SCALER0, VDMA_KIND_DST, s_dst.size);
	if (!s_src.buf || !s_dst.buf) { EPRINTF("Scaler alloc failed.\n"); goto err; }
	fill_nv12_pattern(s_src.buf, W, H);

	memset(&scfg, 0, sizeof(scfg));
	scfg.src_cache_flush	= scfg.dst_cache_flush	= 1;
	scfg.src_addr_flag		= scfg.dst_addr_flag	= ENX_VDMA_ADDR_TYPE_VIRT;
	scfg.src_addr			= (UInt64)(uintptr_t)s_src.buf;
	scfg.src_data_offset	= 0;
	scfg.dst_addr			= (UInt64)(uintptr_t)s_dst.buf;
	scfg.dst_data_offset	= 0;
	scfg.src_width_total	= scfg.src_width	= W;
	scfg.src_height_total	= scfg.src_height	= H;
	scfg.src_format			= eFmt_YUV420SP_NV12;
	scfg.dst_width_total	= scfg.dst_width	= OW;
	scfg.dst_height			= OH;
	scfg.dst_format			= eFmt_YUV420SP_NV12;
	if (ENX_DMA_Scaler_Execute(ENX_VDMA_SCALER0, &scfg) != 0) {
		EPRINTF("Scaler_Execute failed.\n"); goto err;
	}

	/* Connect : Scaler.dst → JpegEnc.src */
	{
		Int32 r = ENX_DMA_Buffer_Connect(
			ENX_VDMA_SCALER0, s_dst.buf,
			ENX_VDMA_JPEG_ENC, VDMA_KIND_SRC, &e_src);
		if (r < 0 || !e_src) {
			EPRINTF("Buffer_Connect (scaler_dst → jenc_src) failed: %d\n", r);
			goto err;
		}
		HPRINTF("Connected: scaler_dst(%p) → jenc_src(%p)\n", s_dst.buf, e_src);
	}

	/* JpegEnc on resized image */
	e_dst.size = (size_t)OW * OH * 4;
	e_dst.buf  = ENX_DMA_JpegEnc_Buffer_Alloc(VDMA_KIND_DST, e_dst.size);
	if (!e_dst.buf) { EPRINTF("jenc dst alloc failed.\n"); goto err; }

	memset(&ecfg, 0, sizeof(ecfg));
	ecfg.src_cache_flush	= ecfg.dst_cache_flush	= 1;
	ecfg.src_addr_flag		= ecfg.dst_addr_flag	= ENX_VDMA_ADDR_TYPE_VIRT;
	ecfg.src_addr			= (UInt64)(uintptr_t)e_src;
	ecfg.src_data_offset	= 0;
	ecfg.dst_addr			= (UInt64)(uintptr_t)e_dst.buf;
	ecfg.dst_data_offset	= 0;
	ecfg.src_width_total	= ecfg.src_width	= OW;
	ecfg.src_height_total	= ecfg.src_height	= OH;
	ecfg.src_format			= eFmt_YUV420SP_NV12;
	ecfg.quality			= 75;
	if (ENX_DMA_JpegEnc_Execute(&ecfg, &jpeg_out) < 0 || !jpeg_out) {
		EPRINTF("JpegEnc_Execute failed.\n"); goto err;
	}

	{
		char name[64];
		snprintf(name, sizeof(name), "pipeline_scaler_jenc_%dx%d.jpg", OW, OH);
		file_newname(name);
		if (file_write(name, e_dst.buf, jpeg_out) == 0)
			HPRINTF("saved: %s (%zu bytes, src 1280x720 → 640x360)\n", name, jpeg_out);
	}
	ret = 0;

err:
	if (e_dst.buf)	ENX_DMA_JpegEnc_Buffer_Free(e_dst.buf, e_dst.size);
	if (e_src)		ENX_DMA_JpegEnc_Buffer_Free(e_src, s_dst.size);
	if (s_dst.buf)	ENX_DMA_Scaler_Buffer_Free(ENX_VDMA_SCALER0, s_dst.buf, s_dst.size);
	if (s_src.buf)	ENX_DMA_Scaler_Buffer_Free(ENX_VDMA_SCALER0, s_src.buf, s_src.size);
	if (!jenc_was && g_state.jenc_inited) { ENX_DMA_JpegEnc_Exit(); g_state.jenc_inited = false; HPRINTF("[auto] JpegEnc Exit\n"); }
	if (!scl_was  && g_state.scaler_inited[0]) { ENX_DMA_Scaler_Exit(ENX_VDMA_SCALER0); g_state.scaler_inited[0] = false; HPRINTF("[auto] Scaler Exit\n"); }
	return ret;
}

/*
 * 4. JpegDec → Scaler via Connect — decode a user-supplied JPEG, then
 * resize the decoded NV12 without an intermediate copy.
 */
int DEMO_PipelineJdecScaler(void)
{
	char path[256], cIn[256];
	VDMA_JPEG_DEC_CONFIG_S dcfg;
	VDMA_DZ_CONFIG_S scfg;
	struct buffer_alloc_track d_src = {0}, d_dst = {0}, s_dst = {0};
	vdma_addr_t s_src = NULL;
	unsigned short jw = 0, jh = 0;
	ENX_VDMA_FORMAT_E fmt = eFmt_YUV444;
	size_t jpeg_size, yuv_size, yuv_out = 0;
	const int OW = 640, OH = 360;
	bool jdec_was, scl_was;
	int ret = -1;

	jdec_was = g_state.jdec_inited;
	scl_was  = g_state.scaler_inited[0];
	if (!jdec_was) {
		if (ENX_DMA_JpegDec_Init() != 0) { EPRINTF("auto JpegDec Init failed.\n"); return -1; }
		g_state.jdec_inited = true; HPRINTF("[auto] JpegDec Init\n");
	}
	if (!scl_was) {
		if (ENX_DMA_Scaler_Init(ENX_VDMA_SCALER0) != 0) {
			EPRINTF("auto Scaler Init failed.\n"); goto err;
		}
		g_state.scaler_inited[0] = true; HPRINTF("[auto] Scaler Init\n");
	}

	printf("JPEG file path : ");
	sGetMenu(cIn); snprintf(path, sizeof(path), "%s", cIn);
	jpeg_size = file_get_jpeg_size(path, &jw, &jh, &fmt);
	if (!jpeg_size) { EPRINTF("file_get_jpeg_size failed.\n"); goto err; }
	if (jw & 1) jw++;
	switch (fmt) {
	case eFmt_YUV444:			yuv_size = (size_t)jw * jh * 3; break;
	case eFmt_YUV422_UYVY:		yuv_size = (size_t)jw * jh * 2; break;
	case eFmt_YUV420SP_NV12:	yuv_size = ((size_t)jw * jh * 3) >> 1; break;
	case eFmt_YUV400:			yuv_size = (size_t)jw * jh; break;
	default: EPRINTF("unsupported JPEG fmt.\n"); goto err;
	}

	/* JpegDec : load + decode */
	d_src.size	= jpeg_size; d_dst.size = yuv_size;
	d_src.buf	= ENX_DMA_JpegDec_Buffer_Alloc(VDMA_KIND_SRC, d_src.size);
	d_dst.buf	= ENX_DMA_JpegDec_Buffer_Alloc(VDMA_KIND_DST, d_dst.size);
	if (!d_src.buf || !d_dst.buf) { EPRINTF("jdec alloc failed.\n"); goto err; }
	if (file_read(path, d_src.buf, jpeg_size)) { EPRINTF("file_read failed.\n"); goto err; }

	memset(&dcfg, 0, sizeof(dcfg));
	dcfg.src_cache_flush	= dcfg.dst_cache_flush	= 1;
	dcfg.src_addr_flag		= dcfg.dst_addr_flag	= ENX_VDMA_ADDR_TYPE_VIRT;
	dcfg.src_addr			= (UInt64)(uintptr_t)d_src.buf;
	dcfg.src_data_offset	= 0;
	dcfg.dst_addr			= (UInt64)(uintptr_t)d_dst.buf;
	dcfg.dst_data_offset	= 0;
	dcfg.src_jpeg_size		= (unsigned int)jpeg_size;
	dcfg.dst_buf_size		= (unsigned int)yuv_size;
	if (ENX_DMA_JpegDec_Execute(&dcfg, &yuv_out) < 0 || !yuv_out) {
		EPRINTF("JpegDec_Execute failed.\n"); goto err;
	}

	/* Connect : JpegDec.dst → Scaler.src */
	{
		Int32 r = ENX_DMA_Buffer_Connect(
			ENX_VDMA_JPEG_DEC, d_dst.buf,
			ENX_VDMA_SCALER0,  VDMA_KIND_SRC, &s_src);
		if (r < 0 || !s_src) {
			EPRINTF("Buffer_Connect (jdec_dst → scaler_src) failed: %d\n", r);
			goto err;
		}
		HPRINTF("Connected: jdec_dst(%p) → scaler_src(%p)\n", d_dst.buf, s_src);
	}

	/* Scaler : decoded NV12 → 640x360 */
	s_dst.size = calculate_format_size(eFmt_YUV420SP_NV12, OW, OH);
	s_dst.buf  = ENX_DMA_Scaler_Buffer_Alloc(ENX_VDMA_SCALER0, VDMA_KIND_DST, s_dst.size);
	if (!s_dst.buf) { EPRINTF("scaler dst alloc failed.\n"); goto err; }

	memset(&scfg, 0, sizeof(scfg));
	scfg.src_cache_flush	= scfg.dst_cache_flush	= 1;
	scfg.src_addr_flag		= scfg.dst_addr_flag	= ENX_VDMA_ADDR_TYPE_VIRT;
	scfg.src_addr			= (UInt64)(uintptr_t)s_src;
	scfg.src_data_offset	= 0;
	scfg.dst_addr			= (UInt64)(uintptr_t)s_dst.buf;
	scfg.dst_data_offset	= 0;
	scfg.src_width_total	= scfg.src_width  = jw;
	scfg.src_height_total	= scfg.src_height = jh;
	scfg.src_format			= fmt;
	scfg.dst_width_total	= scfg.dst_width  = OW;
	scfg.dst_height			= OH;
	scfg.dst_format			= eFmt_YUV420SP_NV12;
	if (ENX_DMA_Scaler_Execute(ENX_VDMA_SCALER0, &scfg) < 0) {
		EPRINTF("Scaler_Execute failed.\n"); goto err;
	}

	{
		char name[64];
		snprintf(name, sizeof(name), "pipeline_jdec_scaler_%dx%d.nv12", OW, OH);
		file_newname(name);
		if (file_write(name, s_dst.buf, s_dst.size) == 0)
			HPRINTF("saved: %s (%zu bytes, %dx%d → %dx%d)\n", name, s_dst.size, jw, jh, OW, OH);
	}
	ret = 0;

err:
	if (s_dst.buf)	ENX_DMA_Scaler_Buffer_Free(ENX_VDMA_SCALER0, s_dst.buf, s_dst.size);
	if (s_src)		ENX_DMA_Scaler_Buffer_Free(ENX_VDMA_SCALER0, s_src, d_dst.size);
	if (d_dst.buf)	ENX_DMA_JpegDec_Buffer_Free(d_dst.buf, d_dst.size);
	if (d_src.buf)	ENX_DMA_JpegDec_Buffer_Free(d_src.buf, d_src.size);
	if (!scl_was  && g_state.scaler_inited[0])	{ ENX_DMA_Scaler_Exit(ENX_VDMA_SCALER0); g_state.scaler_inited[0] = false; HPRINTF("[auto] Scaler Exit\n"); }
	if (!jdec_was && g_state.jdec_inited)		{ ENX_DMA_JpegDec_Exit(); g_state.jdec_inited = false; HPRINTF("[auto] JpegDec Exit\n"); }
	return ret;
}

/*
 * 5. Same-process Export_Fd / Import_Fd sanity. Writes a known pattern
 * via Scaler, exports the fd, imports into JpegEnc, and verifies the
 * consumer sees identical bytes through the shared backing.
 */
int DEMO_PipelineFdSanity(void)
{
	const int W = 320, H = 240;
	struct buffer_alloc_track s_src = {0};
	vdma_addr_t imported = NULL;
	int fd = -1;
	bool scl_was, jenc_was;
	Int32 r;
	int ret = -1;
	size_t size = calculate_format_size(eFmt_YUV420SP_NV12, W, H);

	scl_was  = g_state.scaler_inited[0];
	jenc_was = g_state.jenc_inited;
	if (!scl_was) {
		if (ENX_DMA_Scaler_Init(ENX_VDMA_SCALER0) != 0) {
			EPRINTF("auto Scaler Init failed.\n"); return -1;
		}
		g_state.scaler_inited[0] = true; HPRINTF("[auto] Scaler Init\n");
	}
	if (!jenc_was) {
		if (ENX_DMA_JpegEnc_Init() != 0) { EPRINTF("auto JpegEnc Init failed.\n"); goto err; }
		g_state.jenc_inited = true; HPRINTF("[auto] JpegEnc Init\n");
	}

	s_src.size = size;
	s_src.buf  = ENX_DMA_Scaler_Buffer_Alloc(ENX_VDMA_SCALER0, VDMA_KIND_SRC, size);
	if (!s_src.buf) { EPRINTF("scaler src alloc failed.\n"); goto err; }
	fill_nv12_pattern(s_src.buf, W, H);
	HPRINTF("Scaler SRC alloc'd at %p, size=%zu, pattern filled\n", s_src.buf, size);

	r = ENX_DMA_Buffer_Export_Fd(ENX_VDMA_SCALER0, s_src.buf, &fd);
	if (r < 0 || fd < 0) { EPRINTF("Export_Fd failed: r=%d fd=%d\n", r, fd); goto err; }
	HPRINTF("Export_Fd ok: fd=%d (backing ref +1 held by fd)\n", fd);

	r = ENX_DMA_Buffer_Import_Fd(ENX_VDMA_JPEG_ENC, VDMA_KIND_SRC, fd, &imported);
	if (r < 0 || !imported) { EPRINTF("Import_Fd failed: r=%d imported=%p\n", r, imported); goto err; }
	HPRINTF("Import_Fd ok: imported=%p (new view on same backing)\n", imported);

	/* Memory check — same backing implies byte-identical content. */
	if (memcmp(s_src.buf, imported, 128) == 0)
		HPRINTF("memcmp OK — same physical backing confirmed (first 128 B match)\n");
	else
		EPRINTF("memcmp FAIL — Export_Fd/Import_Fd did not share backing!\n");

	ret = 0;
err:
	if (imported)	ENX_DMA_JpegEnc_Buffer_Free(imported, size);
	if (fd >= 0)	close(fd);
	if (s_src.buf)	ENX_DMA_Scaler_Buffer_Free(ENX_VDMA_SCALER0, s_src.buf, size);
	if (!jenc_was && g_state.jenc_inited)		{ ENX_DMA_JpegEnc_Exit(); g_state.jenc_inited = false; HPRINTF("[auto] JpegEnc Exit\n"); }
	if (!scl_was  && g_state.scaler_inited[0])	{ ENX_DMA_Scaler_Exit(ENX_VDMA_SCALER0); g_state.scaler_inited[0] = false; HPRINTF("[auto] Scaler Exit\n"); }
	return ret;
}

/*
 * 6. Fork + SCM_RIGHTS — cross-process verification. Parent allocates a
 * buffer in JpegEnc, fills a sentinel pattern, exports an fd, and ships
 * it to a forked child over a socketpair. The child imports the fd in
 * its own lib instance (post-fork re-init) and reads back the sentinel.
 */
static int demo_send_fd(int sock, int payload_fd)
{
	struct msghdr msg = {0};
	struct iovec iov;
	char buf[1] = {'F'};
	char cmsgbuf[CMSG_SPACE(sizeof(int))];
	struct cmsghdr *cm;

	iov.iov_base		= buf;
	iov.iov_len			= 1;
	msg.msg_iov			= &iov;
	msg.msg_iovlen		= 1;
	msg.msg_control		= cmsgbuf;
	msg.msg_controllen	= sizeof(cmsgbuf);

	cm				= CMSG_FIRSTHDR(&msg);
	cm->cmsg_level	= SOL_SOCKET;
	cm->cmsg_type	= SCM_RIGHTS;
	cm->cmsg_len	= CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cm), &payload_fd, sizeof(int));

	return sendmsg(sock, &msg, 0) < 0 ? -errno : 0;
}

static int demo_recv_fd(int sock, int *fd_out)
{
	struct msghdr msg = {0};
	struct iovec iov;
	char buf[1];
	char cmsgbuf[CMSG_SPACE(sizeof(int))];
	struct cmsghdr *cm;

	iov.iov_base		= buf;
	iov.iov_len			= 1;
	msg.msg_iov			= &iov;
	msg.msg_iovlen		= 1;
	msg.msg_control		= cmsgbuf;
	msg.msg_controllen	= sizeof(cmsgbuf);

	if (recvmsg(sock, &msg, 0) < 0) return -errno;
	cm = CMSG_FIRSTHDR(&msg);
	if (!cm || cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS)
		return -EBADMSG;
	memcpy(fd_out, CMSG_DATA(cm), sizeof(int));
	return 0;
}

int DEMO_PipelineForkScm(void)
{
	const int W = 320, H = 240;
	const size_t size = calculate_format_size(eFmt_YUV420SP_NV12, W, H);
	int sv[2] = {-1, -1};
	pid_t pid;
	bool jenc_was;
	int ret = -1;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
		EPRINTF("socketpair failed: %s\n", strerror(errno)); return -1;
	}

	jenc_was = g_state.jenc_inited;
	if (!jenc_was) {
		if (ENX_DMA_JpegEnc_Init() != 0) { EPRINTF("auto JpegEnc Init failed.\n"); goto out; }
		g_state.jenc_inited = true; HPRINTF("[auto] JpegEnc Init (parent)\n");
	}

	pid = fork();
	if (pid < 0) { EPRINTF("fork failed: %s\n", strerror(errno)); goto out; }

	/* child */
	if (pid == 0) {
		int rfd = -1;
		vdma_addr_t imported = NULL;
		Int32 r;

		close(sv[0]);
		/* The lib's pthread_atfork handler resets per-leaf singletons in
		 * the child, so we must Init JpegEnc fresh here. */
		if (ENX_DMA_JpegEnc_Init() != 0) { _exit(11); }

		if (demo_recv_fd(sv[1], &rfd) < 0) { _exit(12); }

		r = ENX_DMA_Buffer_Import_Fd(ENX_VDMA_JPEG_ENC, VDMA_KIND_SRC, rfd, &imported);
		if (r != 0 || !imported) { close(rfd); _exit(13); }

		/* Sentinel check : parent wrote 0xA5 over the whole buffer. */
		unsigned char *p = (unsigned char *)imported;
		int mismatches = 0;
		for (size_t i = 0; i < 64; i++) if (p[i] != 0xA5) mismatches++;

		ENX_DMA_JpegEnc_Buffer_Free(imported, size);
		close(rfd);
		ENX_DMA_JpegEnc_Exit();
		_exit(mismatches == 0 ? 0 : 20);
	}

	/* parent */
	{
		struct buffer_alloc_track p_buf = {.size = size};
		int efd = -1;
		Int32 r;
		int wstatus = 0;

		close(sv[1]);

		p_buf.buf = ENX_DMA_JpegEnc_Buffer_Alloc(VDMA_KIND_SRC, size);
		if (!p_buf.buf) { EPRINTF("parent alloc failed.\n"); close(sv[0]); goto out; }
		memset(p_buf.buf, 0xA5, size);	/* sentinel */

		r = ENX_DMA_Buffer_Export_Fd(ENX_VDMA_JPEG_ENC, p_buf.buf, &efd);
		if (r != 0 || efd < 0) {
			EPRINTF("Export_Fd failed: r=%d\n", r);
			ENX_DMA_JpegEnc_Buffer_Free(p_buf.buf, size);
			close(sv[0]); goto out;
		}
		HPRINTF("parent: exported fd=%d, sending to child PID %d\n", efd, (int)pid);

		if (demo_send_fd(sv[0], efd) < 0) {
			EPRINTF("send_fd failed.\n");
		}
		close(efd);

		waitpid(pid, &wstatus, 0);
		if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0)
			HPRINTF("child verified sentinel — cross-process backing share OK\n");
		else
			EPRINTF("child exit status %d — sentinel mismatch or error\n",
				WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1);

		ENX_DMA_JpegEnc_Buffer_Free(p_buf.buf, size);
		close(sv[0]);
		ret = 0;
	}

out:
	if (sv[0] >= 0) close(sv[0]);
	if (!jenc_was && g_state.jenc_inited) {
		ENX_DMA_JpegEnc_Exit();
		g_state.jenc_inited = false;
		HPRINTF("[auto] JpegEnc Exit (parent)\n");
	}
	return ret;
}