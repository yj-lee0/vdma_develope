/*
 * Copyright (C) 2026 Eyenix Corporation
 * Eyenix <support6@eyenix.com>
 *
 * VDMA demo — JpegEnc → JpegDec roundtrip + PSNR (case 7).
 *
 * Generates an NV12 pattern, encodes it via JpegEnc, decodes via JpegDec,
 * and reports PSNR(Y) plus saves orig / encoded / decoded artifacts to
 * disk. Auto-Init's JpegEnc/JpegDec on entry and restores the user's
 * prior init state on return.
 */
#define _POSIX_C_SOURCE 200809L

#include <math.h>

#include "demo_vdma.h"
#include "demo_vdma_util.h"

/*******************************************************************************
 *
 * DMA-Roundtrip — JpegEnc -> JpegDec end-to-end fidelity test
 *
 * Generates an NV12 pattern, encodes it to JPEG via JpegEnc, decodes the
 * JPEG back to NV12 via JpegDec, then reports PSNR(Y) between the original
 * and the decoded image. Verifies the full lib + driver + HW stack with a
 * single quantitative number. Requires both JpegEnc and JpegDec Init.
 *
 ******************************************************************************/

int DEMO_RoundtripExec(void)
{
	VDMA_JPEG_ENC_CONFIG_S enc_cfg;
	VDMA_JPEG_DEC_CONFIG_S dec_cfg;
	struct buffer_alloc_track jenc_src = {0}, jenc_dst = {0};
	struct buffer_alloc_track jdec_src = {0}, jdec_dst = {0};
	const int W = 1280, H = 720;
	size_t nv12_size, jpeg_buf_size;
	size_t jpeg_out = 0, yuv_out = 0;
	double psnr;
	uint64_t t_enc0, t_enc1, t_dec0, t_dec1;
	int ret = -1;
	bool jenc_was_inited = g_state.jenc_inited;
	bool jdec_was_inited = g_state.jdec_inited;

	if (!jenc_was_inited) {
		if (ENX_DMA_JpegEnc_Init() != 0) {
			EPRINTF("auto JpegEnc Init failed.\n");
			return -1;
		}
		g_state.jenc_inited = true;
		HPRINTF("[auto] JpegEnc Init\n");
	}
	if (!jdec_was_inited) {
		if (ENX_DMA_JpegDec_Init() != 0) {
			EPRINTF("auto JpegDec Init failed.\n");
			goto err;
		}
		g_state.jdec_inited = true;
		HPRINTF("[auto] JpegDec Init\n");
	}

	nv12_size		= calculate_format_size(eFmt_YUV420SP_NV12, W, H);
	jpeg_buf_size	= (size_t)W * H * 4; /* generous upper bound */

	printf("===== [Roundtrip] %dx%d NV12 -> JPEG -> NV12 =====\n", W, H);

	/*
	 * Stage 1 — JpegEnc: produce the JPEG bitstream from the pattern.
	 * Keep jenc_src alive past Encode so we can compare against decoded
	 * output at PSNR time.
	 */
	memset(&enc_cfg, 0, sizeof(enc_cfg));
	enc_cfg.src_cache_flush		= 1;
	enc_cfg.dst_cache_flush		= 1;
	enc_cfg.src_addr_flag		= ENX_VDMA_ADDR_TYPE_VIRT;
	enc_cfg.dst_addr_flag		= ENX_VDMA_ADDR_TYPE_VIRT;
	enc_cfg.src_width_total		= enc_cfg.src_width		= W;
	enc_cfg.src_height_total	= enc_cfg.src_height	= H;
	enc_cfg.src_format			= eFmt_YUV420SP_NV12;
	enc_cfg.quality				= 75;

	jenc_src.size = nv12_size;
	jenc_src.buf  = ENX_DMA_JpegEnc_Buffer_Alloc(VDMA_KIND_SRC, jenc_src.size);
	if (!jenc_src.buf) { EPRINTF("jenc src alloc failed.\n"); goto err; }
	MPRINTF("jenc SRC alloc: virt=%p size=%zu\n", jenc_src.buf, jenc_src.size);

	fill_nv12_pattern(jenc_src.buf, W, H);
	HPRINTF("Generated %dx%d NV12 pattern.\n", W, H);

	/* Dump the original pattern so it can be compared against the decoded
	 * output side-by-side outside the demo. */
	{
		char name[64];
		snprintf(name, sizeof(name), "roundtrip_orig_%dx%d.nv12", W, H);
		file_newname(name);
		if (file_write(name, jenc_src.buf, jenc_src.size) == 0)
			HPRINTF("saved: %s (%zu bytes)\n", name, jenc_src.size);
	}

	jenc_dst.size = jpeg_buf_size;
	jenc_dst.buf  = ENX_DMA_JpegEnc_Buffer_Alloc(VDMA_KIND_DST, jenc_dst.size);
	if (!jenc_dst.buf) { EPRINTF("jenc dst alloc failed.\n"); goto err; }
	MPRINTF("jenc DST alloc: virt=%p size=%zu\n", jenc_dst.buf, jenc_dst.size);

	enc_cfg.src_addr 		= (UInt64)(uintptr_t)jenc_src.buf;
	enc_cfg.src_data_offset	= 0;
	enc_cfg.dst_addr 		= (UInt64)(uintptr_t)jenc_dst.buf;
	enc_cfg.dst_data_offset	= 0;

	t_enc0 = now_ns();
	if (ENX_DMA_JpegEnc_Execute(&enc_cfg, &jpeg_out) < 0 || !jpeg_out) {
		EPRINTF("JpegEnc_Execute failed.\n"); goto err;
	}
	t_enc1 = now_ns();
	HPRINTF("JpegEnc: %zu -> %zu bytes (q=%u, ratio=%.2fx, %.3f ms)\n",
		jenc_src.size, jpeg_out, enc_cfg.quality,
		(double)jenc_src.size / jpeg_out,
		(t_enc1 - t_enc0) / 1.0e6);

	/* Dump the encoded JPEG so the user can verify it opens in any viewer. */
	{
		char name[64];
		snprintf(name, sizeof(name), "roundtrip_enc_%dx%d_q%u.jpg", W, H, enc_cfg.quality);
		file_newname(name);
		if (file_write(name, jenc_dst.buf, jpeg_out) == 0)
			HPRINTF("saved: %s (%zu bytes)\n", name, jpeg_out);
	}

	/*
	 * Stage 2 — JpegDec: feed the JPEG back through the decoder. JpegEnc
	 * and JpegDec are separate device sessions with their own backings,
	 * so the encoded bitstream must be copied across (no EXPORT/IMPORT in
	 * the public API yet).
	 */
	memset(&dec_cfg, 0, sizeof(dec_cfg));
	dec_cfg.src_cache_flush	= 1;
	dec_cfg.dst_cache_flush	= 1;
	dec_cfg.src_addr_flag	= ENX_VDMA_ADDR_TYPE_VIRT;
	dec_cfg.dst_addr_flag	= ENX_VDMA_ADDR_TYPE_VIRT;

	jdec_src.size = jpeg_out;
	jdec_src.buf  = ENX_DMA_JpegDec_Buffer_Alloc(VDMA_KIND_SRC, jdec_src.size);
	if (!jdec_src.buf) { EPRINTF("jdec src alloc failed.\n"); goto err; }
	MPRINTF("jdec SRC alloc: virt=%p size=%zu\n", jdec_src.buf, jdec_src.size);

	memcpy(jdec_src.buf, jenc_dst.buf, jpeg_out);

	jdec_dst.size = nv12_size;
	jdec_dst.buf  = ENX_DMA_JpegDec_Buffer_Alloc(VDMA_KIND_DST, jdec_dst.size);
	if (!jdec_dst.buf) { EPRINTF("jdec dst alloc failed.\n"); goto err; }
	MPRINTF("jdec DST alloc: virt=%p size=%zu\n", jdec_dst.buf, jdec_dst.size);

	dec_cfg.src_addr		= (UInt64)(uintptr_t)jdec_src.buf;
	dec_cfg.src_data_offset	= 0;
	dec_cfg.dst_addr		= (UInt64)(uintptr_t)jdec_dst.buf;
	dec_cfg.dst_data_offset	= 0;
	dec_cfg.src_jpeg_size	= (unsigned int)jdec_src.size;
	dec_cfg.dst_buf_size	= (unsigned int)jdec_dst.size;

	t_dec0 = now_ns();
	if (ENX_DMA_JpegDec_Execute(&dec_cfg, &yuv_out) < 0 || !yuv_out) {
		EPRINTF("JpegDec_Execute failed.\n"); goto err;
	}
	t_dec1 = now_ns();
	HPRINTF("JpegDec: %zu -> %zu bytes (%.3f ms)\n",
		jdec_src.size, yuv_out, (t_dec1 - t_dec0) / 1.0e6);

	/* Dump the decoded NV12 so it can be visually diffed against the orig. */
	{
		char name[64];
		snprintf(name, sizeof(name), "roundtrip_dec_%dx%d.nv12", W, H);
		file_newname(name);
		if (file_write(name, jdec_dst.buf, yuv_out) == 0)
			HPRINTF("saved: %s (%zu bytes)\n", name, yuv_out);
	}

	/* Stage 3 — quantitative comparison. */
	psnr = compute_psnr_y(jenc_src.buf, jdec_dst.buf, W, H);
	if (isinf(psnr))
		HPRINTF("PSNR(Y): infinite (bit-identical, unexpected for lossy JPEG)\n");
	else
		HPRINTF("PSNR(Y): %.2f dB  %s\n", psnr,
			psnr >= 30.0 ? "(OK)" :
			psnr >= 20.0 ? "(suspect — investigate)" :
				"(BROKEN — likely cache/buffer issue)");

	show_dev_stats("enx_vdma_jenc");
	show_dev_stats("enx_vdma_jdec");
	ret = 0;

err:
	if (jenc_src.buf) ENX_DMA_JpegEnc_Buffer_Free(jenc_src.buf, jenc_src.size);
	if (jenc_dst.buf) ENX_DMA_JpegEnc_Buffer_Free(jenc_dst.buf, jenc_dst.size);
	if (jdec_src.buf) ENX_DMA_JpegDec_Buffer_Free(jdec_src.buf, jdec_src.size);
	if (jdec_dst.buf) ENX_DMA_JpegDec_Buffer_Free(jdec_dst.buf, jdec_dst.size);
	/* Undo only the auto-Init we performed; leave user-initiated state alone. */
	if (!jdec_was_inited && g_state.jdec_inited) {
		ENX_DMA_JpegDec_Exit();
		g_state.jdec_inited = false;
		HPRINTF("[auto] JpegDec Exit\n");
	}
	if (!jenc_was_inited && g_state.jenc_inited) {
		ENX_DMA_JpegEnc_Exit();
		g_state.jenc_inited = false;
		HPRINTF("[auto] JpegEnc Exit\n");
	}
	return ret;
}
