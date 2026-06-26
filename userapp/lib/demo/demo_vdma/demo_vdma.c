/*
 * Copyright (C) 2026 Eyenix Corporation
 * Eyenix <support6@eyenix.com>
 *
 * Interactive demo for enx_vdma — main entry + per-module Exec functions.
 *
 * The verification harness (Roundtrip, Stress, Pipeline) lives in sibling
 * files in this directory:
 *   demo_vdma_util.{c,h}     — shared helpers / types
 *   demo_vdma_roundtrip.c    — JpegEnc → JpegDec PSNR roundtrip (case 7)
 *   demo_vdma_stress.c       — stress / regression loops (case 8)
 *   demo_vdma_pipeline.c     — cross-device buffer share (case 9)
 *
 * ----------------------------------------------------------------------------
 * *NOTE) Buffer addressing — how to populate src_addr / dst_addr fields
 * ----------------------------------------------------------------------------
 *
 *  All ENX_DMA_*_Buffer_Alloc() variants return a user-space virtual pointer
 *  (void *) to the allocated region. The config struct's addr field can be
 *  populated using one of three address types, selected by the matching
 *  *_addr_flag (or dst_type_index for Font):
 *
 *  - ENX_VDMA_ADDR_TYPE_VIRT (used throughout this demo)
 *      addr  = (unsigned long)(uintptr_t)pBuf;   // the returned virt pointer
 *      flag  = ENX_VDMA_ADDR_TYPE_VIRT;
 *      Simple: caller never needs to track ids. The library resolves the
 *      virt pointer to the underlying buffer internally.
 *
 *  - ENX_VDMA_ADDR_TYPE_ID
 *      pBuf = ENX_DMA_*_Buffer_Alloc_EX(kind, size, &buf_id);
 *      addr = buf_id;
 *      flag = ENX_VDMA_ADDR_TYPE_ID;
 *      Useful when the caller wants to pass the buffer reference around
 *      without exposing the virt pointer (e.g., across IPC or for logging).
 *
 *  - ENX_VDMA_ADDR_TYPE_PHYS
 *      addr = <physical address obtained outside libvdma>
 *      flag = ENX_VDMA_ADDR_TYPE_PHYS;
 *      For interoperating with buffers the caller already owns physically
 *      (e.g., from another DMA framework). The demo does not exercise this.
 *
 *  This demo uses ENX_VDMA_ADDR_TYPE_VIRT uniformly across Scaler / Font /
 *  JpegEnc / JpegDec so the flow is consistent and the caller never needs
 *  to manage buffer ids.
 * ----------------------------------------------------------------------------
 */
#define _POSIX_C_SOURCE 200809L

#include "demo_vdma.h"
#include "demo_vdma_util.h"

/* The max_src value is determined by the font driver's max_src parameter (default: 16). */
#define MAX_FONT_NUM	16

/*******************************************************************************
 *
 * DMA-Scaler
 *
 ******************************************************************************/

static int DEMO_ScalerExec(void)
{
	int nSel;
	char cIn[256] = {0};
	VDMA_DZ_CONFIG_S cfg;
	struct buffer_alloc_track src = {0}, dst = {0};
	size_t cal_size;
	char file_name[256] = {0};
	int ret = -1;
	uint64_t t0, t1;

	REQUIRE_INIT(g_state.scaler_inited[0], "Scaler");

	memset(&cfg, 0, sizeof(cfg));
	cfg.src_cache_flush = 1;
	cfg.dst_cache_flush = 1;
	cfg.src_addr_flag	= ENX_VDMA_ADDR_TYPE_VIRT;
	cfg.dst_addr_flag	= ENX_VDMA_ADDR_TYPE_VIRT;

	printMenu(gVdmaExecMenu, 5, 0); sGetMenu(cIn); nSel = atoi(cIn);

	if (nSel == 1) {
		/* Example: 1280x720 NV12 generated pattern → 640x360 NV12 (50% down) */
		cfg.src_width_total		= cfg.src_width  = 1280;
		cfg.src_height_total	= cfg.src_height = 720;
		cfg.src_format			= eFmt_YUV420SP_NV12;
		cfg.dst_width_total		= cfg.dst_width  = 640;
		cfg.dst_height			= 360;
		cfg.dst_format			= eFmt_YUV420SP_NV12;

		src.size = calculate_format_size(eFmt_YUV420SP_NV12, 1280, 720);
		dst.size = calculate_format_size(eFmt_YUV420SP_NV12,  640, 360);

		src.buf = ENX_DMA_Scaler_Buffer_Alloc(ENX_VDMA_SCALER0, VDMA_KIND_SRC, src.size);
		if (!src.buf) { EPRINTF("Scaler SRC alloc failed.\n"); goto err; }
		MPRINTF("scaler SRC alloc: virt=%p size=%zu\n", src.buf, src.size);

		fill_nv12_pattern(src.buf, 1280, 720);
		HPRINTF("Generated 1280x720 NV12 color-bar pattern (no file input).\n");

		snprintf(file_name, sizeof(file_name), "output_640x360.nv12");
	}
	else if (nSel == 2) {
		printf("===== [Scaler Info] ======\n");
		printf("Please enter the scaler settings below.\n");

		printf(" - Enter the name of the source input image file.\n");
		printf("file_name: "); sGetMenu(cIn); snprintf(file_name, sizeof(file_name), "%s", cIn);
		src.size = get_file_size(file_name);
		if (!src.size) { EPRINTF("get_file_size failed for %s\n", file_name); goto err; }

		printf(" - Enter the source image resolution.\n");
		printf("src_width_total: ");  GET_UINT_OR_ERR("src_width_total", cfg.src_width_total);
		if (!cfg.src_width_total)  { EPRINTF("src_width_total must be > 0\n"); goto err; }
		printf("src_height_total: "); GET_UINT_OR_ERR("src_height_total", cfg.src_height_total);
		if (!cfg.src_height_total) { EPRINTF("src_height_total must be > 0\n"); goto err; }

		src.buf = ENX_DMA_Scaler_Buffer_Alloc(ENX_VDMA_SCALER0, VDMA_KIND_SRC, src.size);
		if (!src.buf) { EPRINTF("ENX_DMA_Scaler_Buffer_Alloc(SRC) failed.\n"); goto err; }
		MPRINTF("scaler SRC alloc: virt=%p size=%zu\n", src.buf, src.size);

		if (file_read(file_name, src.buf, src.size)) {
			EPRINTF("file_read failed for %s\n", file_name); goto err;
		}

		printf(" - Enter the source image format: (0~27)\n");
		printf("   [RGB]    -  0: RGB,   1: RBG,   2: GRB,   3: GBR,   4: BRG,   5: BGR \n");
		printf("               6: RRR,   7: GGG,   8: BBB,   9: Rzz,  10: zGz,  11: zzB \n");
		printf("   [YUV444] - 12: YUV,  18: Y__\n");
		printf("   [YUV422] - 19: YUYV, 20: YVYU, 21: UYUV, 22: VYUY, 23: Y__\n");
		printf("   [YUV420] - 24: NV12, 25: NV21, 26: Y__\n");
		printf("   [YUV400] - 27: Y\n");
		printf("src_fmt: "); sGetMenu(cIn); cfg.src_format = atoi(cIn);
		cal_size = calculate_format_size(cfg.src_format, cfg.src_width_total, cfg.src_height_total);
		if (!cal_size || src.size < cal_size) {
			if (cal_size) EPRINTF("file size smaller than expected for fmt(%d)\n", cfg.src_format);
			else EPRINTF("Invalid format(%d)\n", cfg.src_format);
			goto err;
		}

		printf(" - Crop the source image? (1: crop, other: no)\n");
		printf("crop: "); sGetMenu(cIn);
		if (atoi(cIn) == 1) {
			printf("src_width_pos: ");	sGetMenu(cIn); cfg.src_width_pos	= atoi(cIn);
			printf("src_width: ");		sGetMenu(cIn); cfg.src_width		= atoi(cIn);
			printf("src_height_pos: ");	sGetMenu(cIn); cfg.src_height_pos	= atoi(cIn);
			printf("src_height: ");		sGetMenu(cIn); cfg.src_height		= atoi(cIn);
			if (cfg.src_width_total  < cfg.src_width  + cfg.src_width_pos ||
				cfg.src_height_total < cfg.src_height + cfg.src_height_pos) {
				EPRINTF("crop region exceeds source.\n"); goto err;
			}
		} else {
			cfg.src_width  = cfg.src_width_total;
			cfg.src_height = cfg.src_height_total;
		}

		printf(" - flip/mirror? (1: flip, 2: mirror, 3: both, other: none)\n");
		printf("flip_mirror: "); sGetMenu(cIn);
		switch (atoi(cIn)) {
		case 1: cfg.hflip = 0; cfg.vflip = 1; break;
		case 2: cfg.hflip = 1; cfg.vflip = 0; break;
		case 3: cfg.hflip = 1; cfg.vflip = 1; break;
		default: cfg.hflip = 0; cfg.vflip = 0; break;
		}

		printf(" - Enter the destination image resolution.\n");
		printf("dst_width: ");	GET_UINT_OR_ERR("dst_width", cfg.dst_width);
		if (!cfg.dst_width)  { EPRINTF("dst_width must be > 0\n");	goto err; }
		cfg.dst_width_total = cfg.dst_width;
		printf("dst_height: ");	GET_UINT_OR_ERR("dst_height", cfg.dst_height);
		if (!cfg.dst_height) { EPRINTF("dst_height must be > 0\n");		goto err; }

		printf(" - Enter the destination image format: (0~27)\n");
		printf("dst_fmt: "); sGetMenu(cIn); cfg.dst_format = atoi(cIn);
		dst.size = calculate_format_size(cfg.dst_format, cfg.dst_width_total, cfg.dst_height);
		if (!dst.size) { EPRINTF("Invalid format(%d)\n", cfg.dst_format); goto err; }

		snprintf(file_name, sizeof(file_name), "output_%dx%d_%s.%s",
			 cfg.dst_width, cfg.dst_height,
			 fmt_name[cfg.dst_format],
			 cfg.dst_format < 12 ? "rgb" : "yuv");
	}
	else {
		EPRINTF("Exit Scaler Exec.\n");
		return -1;
	}

	/* Common: dst alloc → execute → save */
	dst.buf = ENX_DMA_Scaler_Buffer_Alloc(ENX_VDMA_SCALER0, VDMA_KIND_DST, dst.size);
	if (!dst.buf) { EPRINTF("ENX_DMA_Scaler_Buffer_Alloc(DST) failed.\n"); goto err; }
	MPRINTF("scaler DST alloc: virt=%p size=%zu\n", dst.buf, dst.size);

	cfg.src_addr 		= (UInt64)(uintptr_t)src.buf;
	cfg.src_data_offset	= 0;
	cfg.dst_addr 		= (UInt64)(uintptr_t)dst.buf;
	cfg.dst_data_offset	= 0;

	t0 = now_ns();
	if (ENX_DMA_Scaler_Execute(ENX_VDMA_SCALER0, &cfg) < 0) {
		EPRINTF("ENX_DMA_Scaler_Execute failed.\n"); goto err;
	}
	t1 = now_ns();
	HPRINTF("Scaler Execute: %.3f ms\n", (t1 - t0) / 1.0e6);
	show_dev_stats("enx_vdma_dz0");

	file_newname(file_name);
	if (file_write(file_name, dst.buf, dst.size)) {
		EPRINTF("Failed to file write.\n"); goto err;
	}

	HPRINTF("Scaler0 output file: %s, size: %zu bytes\n", file_name, dst.size);
	ret = 0;
err:
	if (src.buf) ENX_DMA_Scaler_Buffer_Free(ENX_VDMA_SCALER0, src.buf, src.size);
	if (dst.buf) ENX_DMA_Scaler_Buffer_Free(ENX_VDMA_SCALER0, dst.buf, dst.size);
	return ret;
}

/*******************************************************************************
 *
 * DMA-Font
 *
 ******************************************************************************/

static void print_font_info(VDMA_FONT_CONFIG_S *cfg)
{
	printf("============= Font config =============\n");
	printf("%20s : %d\n", "dst type index",		cfg->dst_type_index);
	printf("%20s : %#x\n", "dst data offset",	cfg->dst_data_offset);
	printf("%20s : %d\n", "dst width",			cfg->dst_width);
	printf("%20s : %d\n", "dst height",			cfg->dst_height);
	printf("%20s : %d\n", "bg overlay",			cfg->background_overlay);
	printf("%20s : %d\n", "font number",		cfg->font_number);

	for (unsigned int i = 0; i < cfg->font_number; i++) {
		printf("------------- Font info[%d] -------------\n", i);
		printf("%20s : %d\n",  "src address flag",	cfg->font_info[i].src_addr_flag);
		printf("%20s : %#lx\n","src addr",			cfg->font_info[i].src_addr);
		printf("%20s : %#x\n","src data offset",	cfg->font_info[i].src_data_offset);
		printf("%20s : %d\n",  "src width",			cfg->font_info[i].src_width);
		printf("%20s : %d\n",  "src height",		cfg->font_info[i].src_height);
		printf("%20s : %d\n",  "font xpos",			cfg->font_info[i].font_xpos);
		printf("%20s : %d\n",  "font ypos",			cfg->font_info[i].font_ypos);
		printf("%20s : %d\n",  "font value",		cfg->font_info[i].font_value);
		printf("%20s : %d\n",  "outline value",		cfg->font_info[i].outline_value);
		printf("%20s : %d\n",  "write mode",		cfg->font_info[i].font_wr_mode);
		printf("%20s : %d\n",  "color tone",		cfg->font_info[i].font_color_tone);
		printf("%20s : %d\n",  "alpha blending",	cfg->font_info[i].font_alpha);
		printf("%20s : %d\n",  "font threshold",	cfg->font_info[i].font_threshold);
	}
}

static int DEMO_FontUpdate(int init_type)
{
	int nSel = 0;
	char cIn[256] = {0};
	VDMA_FONT_CONFIG_S *cfg = NULL;
	struct buffer_alloc_track srcs[MAX_FONT_NUM] = {0}, dst = {0};
	unsigned int font_number = 0, yc_channel = 0;
	int src_count = 0;
	size_t dst_input_size = 0;
	char file_name[256] = {0};
	char dst_file_name[256] = {0};
	bool is_color = false;
	bool is_dst_input = false;
	uint64_t t0, t1;

	REQUIRE_INIT(g_state.font_inited, "Font");

	printMenu(gVdmaExecMenu, 5, 0); sGetMenu(cIn); nSel = atoi(cIn);

	if (nSel == 1) {
		/* Example setting — single font, 270x122 YUV file */
		font_number = 1;
		yc_channel = 3;
		cfg = ENX_DMA_Font_CfgAlloc(font_number, yc_channel);
		if (!cfg) { EPRINTF("VDMA_FONT_CONFIG_S alloc failed.\n"); return -1; }

		snprintf(file_name, sizeof(file_name), "%s", "font_270x122_TEST.yuv");
		srcs[0].size = get_file_size(file_name);
		if (!srcs[0].size) { EPRINTF("get_file_size failed for %s\n", file_name); goto err; }

		srcs[0].buf = ENX_DMA_Font_Buffer_Alloc(VDMA_KIND_SRC, srcs[0].size);
		if (!srcs[0].buf) { EPRINTF("Font src alloc failed.\n"); goto err; }
		MPRINTF("font SRC alloc[0]: virt=%p size=%zu\n", srcs[0].buf, srcs[0].size);
		src_count = 1;

		if (file_read(file_name, srcs[0].buf, srcs[0].size)) {
			EPRINTF("file_read failed for %s\n", file_name); goto err;
		}

		cfg->font_info[0].src_addr_flag		= ENX_VDMA_ADDR_TYPE_VIRT;
		cfg->font_info[0].src_addr			= (unsigned long)(uintptr_t)srcs[0].buf;
		cfg->font_info[0].src_data_offset	= 0;
		cfg->font_info[0].src_width			= 270;
		cfg->font_info[0].src_height		= 122;
		cfg->font_info[0].font_color_y		= 150;
		cfg->font_info[0].font_color_cb		= 170;
		cfg->font_info[0].font_color_cr		= 140;
		cfg->font_info[0].font_value		= 255;
		cfg->font_info[0].outline_color_y	= 0;
		cfg->font_info[0].outline_color_cb	= 128;
		cfg->font_info[0].outline_color_cr	= 128;
		cfg->font_info[0].outline_value		= 128;
		cfg->font_info[0].bg_color_y		= 0;
		cfg->font_info[0].bg_color_cb		= 0;
		cfg->font_info[0].bg_color_cr		= 0;
		cfg->font_info[0].font_wr_mode		= 0;	/* YC(RW) */
		cfg->font_info[0].font_color_tone	= 1;
		cfg->font_info[0].font_alpha		= 256;	/* opaque */
		cfg->font_info[0].font_threshold	= 0;

		if (init_type == ENX_VDMA_INIT_TYPE_KERNEL) {
			cfg->font_info[0].font_xpos = 0;
			cfg->font_info[0].font_ypos = 0;
			cfg->dst_width  = cfg->font_info[0].src_width;
			cfg->dst_height = cfg->font_info[0].src_height;
			cfg->background_overlay = 0;

			if (cfg->font_info[0].font_wr_mode == 1 || cfg->font_info[0].font_wr_mode == 3) {
				dst.size = (size_t)cfg->dst_width * cfg->dst_height;
			} else {
				dst.size = ((size_t)cfg->dst_width * cfg->dst_height * 3) >> 1;
				is_color = true;
			}

			dst.buf = ENX_DMA_Font_Buffer_Alloc(VDMA_KIND_DST, dst.size);
			if (!dst.buf) { EPRINTF("Font dst alloc failed.\n"); goto err; }
			MPRINTF("font DST alloc: virt=%p size=%zu\n", dst.buf, dst.size);

			cfg->dst_type_index = ENX_VDMA_ADDR_TYPE_VIRT;
			cfg->dst_data_offset = 0;
			cfg->font_info[0].dst_addr_y = (unsigned long)(uintptr_t)dst.buf;

			snprintf(file_name, sizeof(file_name), "output_%dx%d_%s.nv12",
				 cfg->dst_width, cfg->dst_height, is_color ? "color" : "gray");
		} else {
			cfg->dst_type_index = 3;  /* Target YC idx */
			cfg->dst_data_offset = 0;
			cfg->font_info[0].font_xpos = 100;
			cfg->font_info[0].font_ypos = 100;
			/* unused */
			cfg->dst_width  = 0;
			cfg->dst_height = 0;
		}
	}
	else if (nSel == 2) {
		printf("======= [Font Info] ========\n");
		printf("Please enter the font settings below.\n");

		if (init_type == ENX_VDMA_INIT_TYPE_VIDEO_CORE) {
			printf(" - YC channel (0~%d)\n", MAX_VISP_YC_NUM - 1);
			printf("yc_channel: "); sGetMenu(cIn); yc_channel = atoi(cIn);
			if (yc_channel >= MAX_VISP_YC_NUM) { EPRINTF("Invalid argument\n"); return -1; }
		}

		printf(" - Number of font images:\n");
		printf("font_number: "); GET_UINT_OR_ERR("font_number", font_number);
		if (!font_number || font_number > MAX_FONT_NUM) {
			EPRINTF("font_number must be 1..%d\n", MAX_FONT_NUM); return -1;
		}

		cfg = ENX_DMA_Font_CfgAlloc(font_number, yc_channel);
		if (!cfg) { EPRINTF("VDMA_FONT_CONFIG_S alloc failed.\n"); return -1; }

		if (init_type == ENX_VDMA_INIT_TYPE_KERNEL) {
			cfg->dst_type_index = 0;
			printf(" - Destination input?\n   (1: Font overlay on input file, other: Generate only)\n");
			printf("is_dst_input: "); sGetMenu(cIn); is_dst_input = (atoi(cIn) == 1);
			if (is_dst_input) {
				cfg->background_overlay = 1;
				printf(" - dst overlay file: ");
				sGetMenu(cIn); snprintf(dst_file_name, sizeof(dst_file_name), "%s", cIn);
				dst_input_size = get_file_size(dst_file_name);
				if (!dst_input_size) { EPRINTF("get_file_size failed for %s\n", dst_file_name); goto err; }
			}
			printf(" - dst resolution: %s\n", is_dst_input ? "(overlay)" : "(output)");
			printf("dst_width: ");  GET_UINT_OR_ERR("dst_width", cfg->dst_width);
			printf("dst_height: "); GET_UINT_OR_ERR("dst_height", cfg->dst_height);
			if (!cfg->dst_width || cfg->dst_width > 3840 ||
				!cfg->dst_height || cfg->dst_height > 3840) {
				EPRINTF("dst resolution out of range (1..3840)\n"); goto err;
			}
		}
		cfg->dst_data_offset = 0;

		for (unsigned int i = 0; i < font_number; i++) {
			printf("-[Font%d]--------------------\n", i + 1);
			cfg->font_info[i].src_addr_flag = ENX_VDMA_ADDR_TYPE_VIRT;
			cfg->font_info[i].src_data_offset = 0;

			printf(" - source file[%d]: ", i);
			sGetMenu(cIn); snprintf(file_name, sizeof(file_name), "%s", cIn);
			srcs[i].size = get_file_size(file_name);
			if (!srcs[i].size) { EPRINTF("get_file_size failed for %s\n", file_name); goto err; }

			printf("src_width[%d]: ",  i); sGetMenu(cIn); cfg->font_info[i].src_width  = atoi(cIn);
			printf("src_height[%d]: ", i); sGetMenu(cIn); cfg->font_info[i].src_height = atoi(cIn);

			if (init_type == ENX_VDMA_INIT_TYPE_KERNEL) {
				if (cfg->font_info[i].src_width  > cfg->dst_width ||
					cfg->font_info[i].src_height > cfg->dst_height) {
					EPRINTF("src exceeds dst.\n"); goto err;
				}
			}
			if (srcs[i].size < (size_t)cfg->font_info[i].src_width * cfg->font_info[i].src_height) {
				EPRINTF("file too small for declared resolution.\n"); goto err;
			}

			printf("font_xpos[%d]: ", i); sGetMenu(cIn); cfg->font_info[i].font_xpos = atoi(cIn);
			printf("font_ypos[%d]: ", i); sGetMenu(cIn); cfg->font_info[i].font_ypos = atoi(cIn);
			if (init_type == ENX_VDMA_INIT_TYPE_KERNEL) {
				if ((cfg->font_info[i].font_xpos + cfg->font_info[i].src_width)  > cfg->dst_width ||
					(cfg->font_info[i].font_ypos + cfg->font_info[i].src_height) > cfg->dst_height) {
					EPRINTF("src + pos exceeds dst.\n"); goto err;
				}
			}

			srcs[i].buf = ENX_DMA_Font_Buffer_Alloc(VDMA_KIND_SRC, srcs[i].size);
			if (!srcs[i].buf) { EPRINTF("Font src alloc failed.\n"); goto err; }
			MPRINTF("font SRC alloc[%d]: virt=%p size=%zu\n", i, srcs[i].buf, srcs[i].size);
			src_count = i + 1;

			cfg->font_info[i].src_addr = (unsigned long)(uintptr_t)srcs[i].buf;

			if (file_read(file_name, srcs[i].buf, srcs[i].size)) {
				EPRINTF("file_read failed for %s\n", file_name); goto err;
			}

			printf("font_value    (0~255, def 255): "); sGetMenu(cIn); cfg->font_info[i].font_value		= atoi(cIn);
			printf("outline_value (0~255, def 128): "); sGetMenu(cIn); cfg->font_info[i].outline_value	= atoi(cIn);
			printf("font_color_y:     "); sGetMenu(cIn); cfg->font_info[i].font_color_y		= atoi(cIn);
			printf("font_color_cb:    "); sGetMenu(cIn); cfg->font_info[i].font_color_cb	= atoi(cIn);
			printf("font_color_cr:    "); sGetMenu(cIn); cfg->font_info[i].font_color_cr	= atoi(cIn);
			printf("outline_color_y:  "); sGetMenu(cIn); cfg->font_info[i].outline_color_y	= atoi(cIn);
			printf("outline_color_cb: "); sGetMenu(cIn); cfg->font_info[i].outline_color_cb	= atoi(cIn);
			printf("outline_color_cr: "); sGetMenu(cIn); cfg->font_info[i].outline_color_cr	= atoi(cIn);
			printf("bg_color_y:       "); sGetMenu(cIn); cfg->font_info[i].bg_color_y		= atoi(cIn);
			printf("bg_color_cb:      "); sGetMenu(cIn); cfg->font_info[i].bg_color_cb		= atoi(cIn);
			printf("bg_color_cr:      "); sGetMenu(cIn); cfg->font_info[i].bg_color_cr		= atoi(cIn);

			printf("font_wr_mode (0:YC[RW] 1:Y[RW] 2:YC[W] 3:Y[W]): ");
			sGetMenu(cIn); cfg->font_info[i].font_wr_mode = atoi(cIn);
			if (cfg->font_info[i].font_wr_mode > 3) { EPRINTF("Invalid.\n"); goto err; }

			if (init_type == ENX_VDMA_INIT_TYPE_KERNEL) {
				if (cfg->font_info[i].font_wr_mode == 0 ||
					cfg->font_info[i].font_wr_mode == 2)
					is_color = true;
			}

			printf("color_tone (0:bitmap 1:font): ");
			sGetMenu(cIn); cfg->font_info[i].font_color_tone = atoi(cIn);
			if (cfg->font_info[i].font_color_tone > 1) { EPRINTF("Invalid.\n"); goto err; }

			printf("font_alpha (0~256): ");
			sGetMenu(cIn); cfg->font_info[i].font_alpha = atoi(cIn);
			if (cfg->font_info[i].font_alpha > 256) { EPRINTF("Invalid.\n"); goto err; }

			printf("font_threshold (0~255): ");
			sGetMenu(cIn); cfg->font_info[i].font_threshold = atoi(cIn);
		}

		if (init_type == ENX_VDMA_INIT_TYPE_KERNEL) {
			dst.size = is_color
				? (((size_t)cfg->dst_width * cfg->dst_height * 3) >> 1)
				:  ((size_t)cfg->dst_width * cfg->dst_height);

			dst.buf = ENX_DMA_Font_Buffer_Alloc(VDMA_KIND_DST, dst.size);
			if (!dst.buf) { EPRINTF("Font dst alloc failed.\n"); goto err; }
			MPRINTF("font DST alloc: virt=%p size=%zu\n", dst.buf, dst.size);

			/* All blits share this single dst — fill each font_info[i]. */
			cfg->dst_type_index = ENX_VDMA_ADDR_TYPE_VIRT;
			cfg->font_info[0].dst_addr_y = (unsigned long)(uintptr_t)dst.buf;

			if (is_dst_input) {
				if (file_read(dst_file_name, dst.buf,
						dst_input_size > dst.size ? dst.size : dst_input_size)) {
					EPRINTF("file_read failed for %s\n", dst_file_name); goto err;
				}
			}

			snprintf(file_name, sizeof(file_name), "output_%dx%d_%s.yuv",
				 cfg->dst_width, cfg->dst_height, is_color ? "color" : "gray");
		}
	} else {
		EPRINTF("Exit Font Update.\n");
		return -1;
	}

	t0 = now_ns();
	if (ENX_DMA_Font_Update(cfg)) { EPRINTF("ENX_DMA_Font_Update failed.\n"); goto err; }
	t1 = now_ns();
	HPRINTF("Font Update: %.3f ms\n", (t1 - t0) / 1.0e6);
	show_dev_stats("enx_vdma_font");

	print_font_info(cfg);

	if (init_type == ENX_VDMA_INIT_TYPE_KERNEL && dst.buf && dst.size) {
		file_newname(file_name);
		if (file_write(file_name, dst.buf, dst.size)) {
			EPRINTF("Failed to file write.\n"); goto err;
		}
		HPRINTF("Font output file: %s, size: %zu bytes\n", file_name, dst.size);
	}

	for (int i = 0; i < src_count; i++)
		if (srcs[i].buf) ENX_DMA_Font_Buffer_Free(srcs[i].buf, srcs[i].size);
	if (dst.buf) ENX_DMA_Font_Buffer_Free(dst.buf, dst.size);
	ENX_DMA_Font_CfgFree();
	return 0;

err:
	ENX_DMA_Font_Stop_All();
	for (int i = 0; i < src_count; i++)
		if (srcs[i].buf) ENX_DMA_Font_Buffer_Free(srcs[i].buf, srcs[i].size);
	if (dst.buf) ENX_DMA_Font_Buffer_Free(dst.buf, dst.size);
	ENX_DMA_Font_CfgFree();
	return -1;
}

/*******************************************************************************
 *
 * DMA-JpegEnc
 *
 ******************************************************************************/

static int DEMO_JpegEncExec(void)
{
	int nSel;
	char cIn[256] = {0};
	VDMA_JPEG_ENC_CONFIG_S cfg;
	struct buffer_alloc_track src = {0}, dst = {0};
	size_t out_size = 0;
	char file_name[256] = {0};
	int ret = -1, fmt = 0;
	uint64_t t0, t1;

	REQUIRE_INIT(g_state.jenc_inited, "JpegEnc");

	memset(&cfg, 0, sizeof(cfg));
	cfg.src_cache_flush = 1;
	cfg.dst_cache_flush = 1;
	cfg.src_addr_flag	= ENX_VDMA_ADDR_TYPE_VIRT;
	cfg.dst_addr_flag	= ENX_VDMA_ADDR_TYPE_VIRT;

	printMenu(gVdmaExecMenu, 5, 0); sGetMenu(cIn); nSel = atoi(cIn);

	if (nSel == 1) {
		/* Example: 1280x720 NV12 generated pattern → JPEG (quality 75) */
		cfg.src_width_total		= cfg.src_width  = 1280;
		cfg.src_height_total	= cfg.src_height = 720;
		cfg.src_format			= eFmt_YUV420SP_NV12;
		cfg.ovf_threshold		= 0;
		cfg.quality				= 75;
		fmt = 2; /* nv12 */

		src.size = calculate_format_size(eFmt_YUV420SP_NV12, 1280, 720);
		dst.size = (size_t)1280 * 720 * 4;	/* generous upper bound */

		src.buf = ENX_DMA_JpegEnc_Buffer_Alloc(VDMA_KIND_SRC, src.size);
		if (!src.buf) { EPRINTF("JpegEnc src alloc failed.\n"); goto err; }
		MPRINTF("jenc SRC alloc: virt=%p size=%zu\n", src.buf, src.size);

		fill_nv12_pattern(src.buf, 1280, 720);
		HPRINTF("Generated 1280x720 NV12 color-bar pattern (no file input).\n");

		snprintf(file_name, sizeof(file_name), "output_1280x720_nv12.jpg");
	}
	else if (nSel == 2) {
		printf("======= [Jenc Info] ========\n");
		printf("Please enter the jpeg-enc settings below.\n");

		printf(" - source file: ");
		sGetMenu(cIn); snprintf(file_name, sizeof(file_name), "%s", cIn);
		src.size = get_file_size(file_name);
		if (!src.size) { EPRINTF("get_file_size failed for %s\n", file_name); goto err; }

		printf("src_width_total: ");	GET_UINT_OR_ERR("src_width_total", cfg.src_width_total);
		printf("src_height_total: ");	GET_UINT_OR_ERR("src_height_total", cfg.src_height_total);
		if (!cfg.src_width_total || !cfg.src_height_total) { EPRINTF("src_width/height_total must be > 0\n"); goto err; }

		printf(" - Crop? (1: crop, other: no)\n");
		printf("crop: "); sGetMenu(cIn);
		if (atoi(cIn) == 1) {
			printf("src_width_pos: ");	sGetMenu(cIn); cfg.src_width_pos	= atoi(cIn);
			printf("src_width: ");		sGetMenu(cIn); cfg.src_width		= atoi(cIn);
			printf("src_height_pos: ");	sGetMenu(cIn); cfg.src_height_pos	= atoi(cIn);
			printf("src_height: ");		sGetMenu(cIn); cfg.src_height		= atoi(cIn);
			if (cfg.src_width_total  < cfg.src_width  + cfg.src_width_pos ||
				cfg.src_height_total < cfg.src_height + cfg.src_height_pos) {
				EPRINTF("crop region exceeds source.\n"); goto err;
			}
		} else {
			cfg.src_width  = cfg.src_width_total;
			cfg.src_height = cfg.src_height_total;
		}

		printf(" - source fmt: (0:YUV444 1:YUV422 2:YUV420 3:Y)\n");
		printf("src_fmt: "); sGetMenu(cIn); fmt = atoi(cIn);
		switch (fmt) {
		case 0: cfg.src_format = eFmt_YUV444;
			if (src.size < (size_t)cfg.src_width_total * cfg.src_height_total * 3) goto err_size;
			break;
		case 1: cfg.src_format = eFmt_YUV422_UYVY;
			if (src.size < (size_t)cfg.src_width_total * cfg.src_height_total * 2) goto err_size;
			break;
		case 2: cfg.src_format = eFmt_YUV420SP_NV12;
			if (src.size < ((size_t)cfg.src_width_total * cfg.src_height_total * 3) >> 1) goto err_size;
			break;
		case 3: cfg.src_format = eFmt_YUV400;
			if (src.size < (size_t)cfg.src_width_total * cfg.src_height_total) goto err_size;
			break;
		default: EPRINTF("Invalid argument\n"); goto err;
		}

		src.buf = ENX_DMA_JpegEnc_Buffer_Alloc(VDMA_KIND_SRC, src.size);
		if (!src.buf) { EPRINTF("JpegEnc src alloc failed.\n"); goto err; }
		MPRINTF("jenc SRC alloc: virt=%p size=%zu\n", src.buf, src.size);

		if (file_read(file_name, src.buf, src.size)) {
			EPRINTF("file_read failed for %s\n", file_name); goto err;
		}

		printf("ovf_threshold (KB, 0:not use): ");
		sGetMenu(cIn); cfg.ovf_threshold = (unsigned int)atoi(cIn) * 1024;

		printf("quality (0~100): ");
		sGetMenu(cIn); cfg.quality = atoi(cIn);
		if (cfg.quality > 100) { EPRINTF("Invalid quality.\n"); goto err; }

		dst.size = cfg.ovf_threshold ? cfg.ovf_threshold
				: (size_t)cfg.src_width_total * cfg.src_height_total * 4;

		snprintf(file_name, sizeof(file_name), "output_%dx%d_%s.jpg",
			 cfg.src_width, cfg.src_height,
			 fmt == 0 ? "yuv" : fmt == 1 ? "uyvy" : fmt == 2 ? "nv12" : "yuv400");
	}
	else {
		EPRINTF("Exit JpegEnc Exec.\n");
		return -1;
	}

	/* Common: dst alloc → execute → save */
	dst.buf = ENX_DMA_JpegEnc_Buffer_Alloc(VDMA_KIND_DST, dst.size);
	if (!dst.buf) { EPRINTF("JpegEnc dst alloc failed.\n"); goto err; }
	MPRINTF("jenc DST alloc: virt=%p size=%zu\n", dst.buf, dst.size);

	cfg.src_addr 		= (UInt64)(uintptr_t)src.buf;
	cfg.src_data_offset	= 0;
	cfg.dst_addr 		= (UInt64)(uintptr_t)dst.buf;
	cfg.dst_data_offset	= 0;

	t0 = now_ns();
	if (ENX_DMA_JpegEnc_Execute(&cfg, &out_size) < 0 || !out_size) {
		EPRINTF("ENX_DMA_JpegEnc_Execute failed.\n"); goto err;
	}
	t1 = now_ns();
	HPRINTF("JpegEnc Execute: %.3f ms (encoded %zu bytes, ratio=%.2fx)\n",
		(t1 - t0) / 1.0e6, out_size, src.size ? (double)src.size / out_size : 0.0);
	show_dev_stats("enx_vdma_jenc");

	file_newname(file_name);
	if (file_write(file_name, dst.buf, out_size)) {
		EPRINTF("Failed to file write.\n"); goto err;
	}

	HPRINTF("JpegEnc output file: %s, size: %zu bytes\n", file_name, out_size);
	ret = 0;
	goto err; /* shared cleanup */

err_size:
	EPRINTF("file size smaller than expected for fmt(%d).\n", fmt);
	/* fall through */
err:
	if (src.buf) ENX_DMA_JpegEnc_Buffer_Free(src.buf, src.size);
	if (dst.buf) ENX_DMA_JpegEnc_Buffer_Free(dst.buf, dst.size);
	return ret;
}

/*******************************************************************************
 *
 * DMA-JpegDec
 *
 ******************************************************************************/

static int DEMO_JpegDecExec(void)
{
	char cIn[256] = {0};
	VDMA_JPEG_DEC_CONFIG_S cfg;
	struct buffer_alloc_track src = {0}, dst = {0};
	size_t yuv_out = 0;
	unsigned short width = 0, height = 0;
	ENX_VDMA_FORMAT_E fmt = eFmt_YUV444;
	char file_name[256] = {0};
	int ret = -1;
	uint64_t t0, t1;

	REQUIRE_INIT(g_state.jdec_inited, "JpegDec");

	memset(&cfg, 0, sizeof(cfg));
	cfg.src_cache_flush = 1;
	cfg.dst_cache_flush = 1;
	cfg.src_addr_flag	= ENX_VDMA_ADDR_TYPE_VIRT;
	cfg.dst_addr_flag	= ENX_VDMA_ADDR_TYPE_VIRT;

	printf("======= [Jdec Info] ========\n");
	printf(" - JPEG file: ");
	sGetMenu(cIn); snprintf(file_name, sizeof(file_name), "%s", cIn);

	src.size = file_get_jpeg_size(file_name, &width, &height, &fmt);
	if (!src.size) { EPRINTF("file_get_jpeg_size failed for %s\n", file_name); goto err; }

	if (width % 2 == 1) width++;

	switch (fmt) {
	case eFmt_YUV444:			dst.size = (size_t)width * height * 3;			break;
	case eFmt_YUV422_UYVY:		dst.size = (size_t)width * height * 2;			break;
	case eFmt_YUV420SP_NV12:	dst.size = ((size_t)width * height * 3) >> 1;	break;
	case eFmt_YUV400:			dst.size = (size_t)width * height;				break;
	default: 					EPRINTF("Unsupported fmt.\n");					goto err;
	}

	src.buf = ENX_DMA_JpegDec_Buffer_Alloc(VDMA_KIND_SRC, src.size);
	if (!src.buf) { EPRINTF("JpegDec src alloc failed.\n"); goto err; }
	MPRINTF("jdec SRC alloc: virt=%p size=%zu\n", src.buf, src.size);

	if (file_read(file_name, src.buf, src.size)) {
		EPRINTF("file_read failed for %s\n", file_name); goto err;
	}

	dst.buf = ENX_DMA_JpegDec_Buffer_Alloc(VDMA_KIND_DST, dst.size);
	if (!dst.buf) { EPRINTF("JpegDec dst alloc failed.\n"); goto err; }
	MPRINTF("jdec DST alloc: virt=%p size=%zu\n", dst.buf, dst.size);

	cfg.src_addr_flag	= ENX_VDMA_ADDR_TYPE_VIRT;
	cfg.src_addr		= (UInt64)(uintptr_t)src.buf;
	cfg.src_data_offset	= 0;
	cfg.dst_addr_flag	= ENX_VDMA_ADDR_TYPE_VIRT;
	cfg.dst_addr		= (UInt64)(uintptr_t)dst.buf;
	cfg.dst_data_offset	= 0;
	cfg.src_jpeg_size	= (unsigned int)src.size;
	cfg.dst_buf_size	= (unsigned int)dst.size;

	snprintf(file_name, sizeof(file_name), "output_%dx%d_%s.yuv",
		 width, height,
		 fmt == eFmt_YUV444			? "yuv"  :
		 fmt == eFmt_YUV422_UYVY	? "uyvy" :
		 fmt == eFmt_YUV420SP_NV12	? "nv12" : "yuv400");

	t0 = now_ns();
	if (ENX_DMA_JpegDec_Execute(&cfg, &yuv_out) < 0 || !yuv_out) {
		EPRINTF("ENX_DMA_JpegDec_Execute failed.\n"); goto err;
	}
	t1 = now_ns();
	HPRINTF("JpegDec Execute: %.3f ms (decoded %zu bytes)\n", (t1 - t0) / 1.0e6, yuv_out);
	show_dev_stats("enx_vdma_jdec");

	file_newname(file_name);
	if (file_write(file_name, dst.buf, yuv_out)) {
		EPRINTF("Failed to file write.\n"); goto err;
	}

	HPRINTF("JpegDec output file: %s, size: %zu bytes\n", file_name, yuv_out);
	ret = 0;
err:
	if (src.buf) ENX_DMA_JpegDec_Buffer_Free(src.buf, src.size);
	if (dst.buf) ENX_DMA_JpegDec_Buffer_Free(dst.buf, dst.size);
	return ret;
}

/*******************************************************************************
 *
 * DMA-Npu
 *
 ******************************************************************************/

static int DEMO_NpuExec(void)
{
	int nSel;
	char cIn[256] = {0};
	VDMA_NPU_CONFIG_S cfg;
	struct buffer_alloc_track src = {0}, dst = {0};
	size_t cal_size;
	char file_name[256] = {0};
	int ret = -1;
	uint64_t t0, t1;

	REQUIRE_INIT(g_state.npu_inited, "Npu");

	memset(&cfg, 0, sizeof(cfg));
	cfg.src_cache_flush = 1;
	cfg.dst_cache_flush = 1;
	cfg.src_addr_flag	= ENX_VDMA_ADDR_TYPE_VIRT;
	cfg.dst_addr_flag	= ENX_VDMA_ADDR_TYPE_VIRT;

	printMenu(gVdmaExecMenu, 5, 0); sGetMenu(cIn); nSel = atoi(cIn);

	if (nSel == 1) {
		/* Example: 1280x720 RGBZ generated pattern → NV12 */
		cfg.src_width			= 1280;
		cfg.src_height			= 720;
		cfg.read_mode			= 0; /* RGBZ - 4byte aligned */
		cfg.rgb_scale			= 0; /* Bypass */
		cfg.color_invert		= 0;

		src.size = cfg.src_width * cfg.src_height * 4; /* RGBZ */
		dst.size = calculate_format_size(eFmt_YUV420SP_NV12,  cfg.src_width, cfg.src_height); /* support only NV12 */

		src.buf = ENX_DMA_Npu_Buffer_Alloc(VDMA_KIND_SRC, src.size);
		if (!src.buf) { EPRINTF("Npu SRC alloc failed.\n"); goto err; }
		MPRINTF("Npu SRC alloc: virt=%p size=%zu\n", src.buf, src.size);

		fill_npu_input_pattern(src.buf, 1280, 720);
		HPRINTF("Generated 1280x720 RGBZ color-bar pattern (no file input).\n");

		snprintf(file_name, sizeof(file_name), "output_1280x720.nv12");
	} else if (nSel == 2) {
		printf("======= [Npu Info] ========\n");
		printf("Please enter the npu settings below.\n");

		printf(" - source file: ");
		sGetMenu(cIn); snprintf(file_name, sizeof(file_name), "%s", cIn);
		src.size = get_file_size(file_name);
		if (!src.size) { EPRINTF("get_file_size failed for %s\n", file_name); goto err; }

		printf("src_width: ");	sGetMenu(cIn); cfg.src_width  = atoi(cIn);
		printf("src_height: ");	sGetMenu(cIn); cfg.src_height = atoi(cIn);
		if (!cfg.src_width || !cfg.src_height) { EPRINTF("src_width/height must be > 0\n"); goto err; }

		printf(" - color_invert: (0: no invert, 1: invert)\n");
		printf("color_invert: ");	sGetMenu(cIn); cfg.color_invert = atoi(cIn);
		if (cfg.color_invert > 1) { EPRINTF("Invalid argument\n"); goto err; }

		printf(" - read_mode: (0:4Byte aligned 1:8Byte aligned 2:16Byte aligned 3:32Byte aligned)\n");
		printf("read_mode: ");	sGetMenu(cIn); cfg.read_mode = atoi(cIn);
		if (cfg.read_mode > 3) { EPRINTF("Invalid argument\n"); goto err; }

		int scale;
		switch (cfg.read_mode) {
		case 0: scale = 4; break;
			case 1: scale = 8; break;
			case 2: scale = 16; break;
			case 3: scale = 32; break;
			default: EPRINTF("Invalid read_mode\n"); goto err;
		}
		cal_size = cfg.src_width * cfg.src_height * scale;
		if (src.size < cal_size) {
			EPRINTF("file size smaller than expected for calculated input.\n"); goto err;
		}

		src.buf = ENX_DMA_Npu_Buffer_Alloc(VDMA_KIND_SRC, src.size);
		if (!src.buf) { EPRINTF("Npu SRC alloc failed.\n"); goto err; }
		MPRINTF("Npu SRC alloc: virt=%p size=%zu\n", src.buf, src.size);

		if (file_read(file_name, src.buf, src.size)) {
			EPRINTF("file_read failed for %s\n", file_name); goto err;
		}

		snprintf(file_name, sizeof(file_name), "output_%dx%d.nv12", cfg.src_width, cfg.src_height);

		dst.size = calculate_format_size(eFmt_YUV420SP_NV12,  cfg.src_width, cfg.src_height);
	} else {
		EPRINTF("Exit Npu Exec.\n");
		return -1;
	}

	/* Common: dst alloc → execute → save */
	dst.buf = ENX_DMA_Npu_Buffer_Alloc(VDMA_KIND_DST, dst.size);
	if (!dst.buf) { EPRINTF("ENX_DMA_Npu_Buffer_Alloc(DST) failed.\n"); goto err; }
	MPRINTF("npu DST alloc: virt=%p size=%zu\n", dst.buf, dst.size);

	cfg.src_addr 		= (UInt64)(uintptr_t)src.buf;
	cfg.src_data_offset = 0;
	cfg.dst_addr 		= (UInt64)(uintptr_t)dst.buf;
	cfg.dst_data_offset = 0;

	t0 = now_ns();
	if (ENX_DMA_Npu_Execute(&cfg) < 0) {
		EPRINTF("ENX_DMA_Npu_Execute failed.\n"); goto err;
	}
	t1 = now_ns();
	HPRINTF("Npu Execute: %.3f ms\n", (t1 - t0) / 1.0e6);
	show_dev_stats("enx_vdma_npu");

	file_newname(file_name);
	if (file_write(file_name, dst.buf, dst.size)) {
		EPRINTF("Failed to file write.\n"); goto err;
	}

	HPRINTF("Npu output file: %s, size: %zu bytes\n", file_name, dst.size);
	ret = 0;
err:
	if (src.buf) ENX_DMA_Npu_Buffer_Free(src.buf, src.size);
	if (dst.buf) ENX_DMA_Npu_Buffer_Free(dst.buf, dst.size);
	return ret;
}

/*******************************************************************************
 *
 * main loop
 *
 ******************************************************************************/

int main(void)
{
	char cIn[256];
	int nSel = 0, subLoop = 0, sub_nSel = 0, init_type = 0;
	int nMenuCnt = sizeof(gVdmaMenu) / sizeof(gVdmaMenu[0]);

	printMenu(gVdmaMenu, nMenuCnt, 0);

	while (1) {
		sGetMenu(cIn);
		/* Ignore a bare Enter at the top menu so the user does not
		 * accidentally fall through to case 0 (exit). */
		if (cIn[0] == '\0') {
			printMenu(gVdmaMenu, nMenuCnt, 0);
			continue;
		}
		nSel = atoi(cIn);
		MPRINTF("nSel %d\n", nSel);

		switch (nSel) {
		case 1: /* Scaler */
			subLoop = 1;
			while (subLoop) {
				printMenu(gVdmaDzMenu, 5, 0); sGetMenu(cIn); sub_nSel = atoi(cIn);
				switch (sub_nSel) {
				case 0:
					ENX_DMA_Scaler_Exit(ENX_VDMA_SCALER0);
					g_state.scaler_inited[0] = false;
					subLoop = 0;
					break;
				case 1:
					if (ENX_DMA_Scaler_Init(ENX_VDMA_SCALER0) != 0) {
						EPRINTF("DMA Scaler init error\n"); break;
					}
					g_state.scaler_inited[0] = true;
					HPRINTF("DMA Scaler initialized\n");
					break;
				case 2:
					if (DEMO_ScalerExec() != 0) { EPRINTF("DEMO_ScalerExec failed\n"); break; }
					HPRINTF("DEMO_ScalerExec success\n");
					break;
				default: break;
				}
			}
			break;
		case 2: /* Font */
			subLoop = 1;
			while (subLoop) {
				printMenu(gVdmaFontMenu, 6, 0); sGetMenu(cIn); sub_nSel = atoi(cIn);
				switch (sub_nSel) {
				case 0:
					ENX_DMA_Font_Stop_All();
					ENX_DMA_Font_Exit();
					g_state.font_inited = false;
					subLoop = 0;
					break;
				case 1:
					printMenu(gVdmaInitMenu, 4, 0); sGetMenu(cIn); init_type = atoi(cIn);
					if (init_type == ENX_VDMA_INIT_TYPE_VIDEO_CORE) {
						EPRINTF("Video-core init not supported in this demo.\n"); break;
					}
					if (ENX_DMA_Font_Init(init_type) != 0) {
						EPRINTF("DMA Font init error\n"); break;
					}
					g_state.font_inited = true;
					g_state.font_init_type = init_type;
					HPRINTF("DMA Font initialized (type=%s)\n",
						init_type == ENX_VDMA_INIT_TYPE_KERNEL ? "kernel" :
						init_type == ENX_VDMA_INIT_TYPE_VIDEO_CORE ? "video-core" : "?");
					break;
				case 2:
					if (DEMO_FontUpdate(init_type) != 0) { EPRINTF("DEMO_FontUpdate failed\n"); break; }
					HPRINTF("DEMO_FontUpdate success\n");
					break;
				case 3:
					ENX_DMA_Font_Stop_All();
					HPRINTF("Font stopped.\n");
					break;
				default: break;
				}
			}
			break;
		case 3: /* JpegEnc */
			subLoop = 1;
			while (subLoop) {
				printMenu(gVdmaJencMenu, 5, 0); sGetMenu(cIn); sub_nSel = atoi(cIn);
				switch (sub_nSel) {
				case 0:
					ENX_DMA_JpegEnc_Exit();
					g_state.jenc_inited = false;
					subLoop = 0;
					break;
				case 1:
					if (ENX_DMA_JpegEnc_Init() != 0) {
						EPRINTF("DMA JpegEnc init error\n"); break;
					}
					g_state.jenc_inited = true;
					HPRINTF("DMA JpegEnc initialized\n");
					break;
				case 2:
					if (DEMO_JpegEncExec() != 0) { EPRINTF("DEMO_JpegEncExec failed\n"); break; }
					HPRINTF("DEMO_JpegEncExec success\n");
					break;
				default: break;
				}
			}
			break;
		case 4: /* JpegDec */
			subLoop = 1;
			while (subLoop) {
				printMenu(gVdmaJdecMenu, 5, 0); sGetMenu(cIn); sub_nSel = atoi(cIn);
				switch (sub_nSel) {
				case 0:
					ENX_DMA_JpegDec_Exit();
					g_state.jdec_inited = false;
					subLoop = 0;
					break;
				case 1:
					if (ENX_DMA_JpegDec_Init() != 0) {
						EPRINTF("DMA JpegDec init error\n"); break;
					}
					g_state.jdec_inited = true;
					HPRINTF("DMA JpegDec initialized\n");
					break;
				case 2:
					if (DEMO_JpegDecExec() != 0) { EPRINTF("DEMO_JpegDecExec failed\n"); break; }
					HPRINTF("DEMO_JpegDecExec success\n");
					break;
				default: break;
				}
			}
			break;
		case 5: /* NPU */
			subLoop = 1;
			while (subLoop) {
				printMenu(gVdmaNpuMenu, 5, 0); sGetMenu(cIn); sub_nSel = atoi(cIn);
				switch (sub_nSel) {
				case 0:
					ENX_DMA_Npu_Exit();
					g_state.npu_inited = false;
					subLoop = 0;
					break;
				case 1:
					if (ENX_DMA_Npu_Init() != 0) {
						EPRINTF("DMA NPU init error\n"); break;
					}
					g_state.npu_inited = true;
					HPRINTF("DMA NPU initialized\n");
					break;
				case 2:
					if (DEMO_NpuExec() != 0) { EPRINTF("DEMO_NpuExec failed\n"); break; }
					HPRINTF("DEMO_NpuExec success\n");
					break;
				default: break;
				}
			}
			break;
		case 6: /* Roundtrip (JpegEnc -> JpegDec + PSNR) */
			if (DEMO_RoundtripExec() != 0) { EPRINTF("DEMO_RoundtripExec failed\n"); break; }
			HPRINTF("DEMO_RoundtripExec success\n");
			break;
		case 7: /* Stress sub-menu */
			subLoop = 1;
			while (subLoop) {
				printMenu(gVdmaStressMenu, sizeof(gVdmaStressMenu) / sizeof(gVdmaStressMenu[0]), 0);
				sGetMenu(cIn); sub_nSel = atoi(cIn);
				switch (sub_nSel) {
				case 0:
					subLoop = 0;
					break;
				case 1:
					if (DEMO_StressRoundtrip() != 0)	EPRINTF("StressRoundtrip failed\n");
					else HPRINTF("StressRoundtrip success\n");
					break;
				case 2:
					if (DEMO_StressJpegEnc() != 0)		EPRINTF("StressJpegEnc failed\n");
					else HPRINTF("StressJpegEnc success\n");
					break;
				case 3:
					if (DEMO_StressJpegDec() != 0)		EPRINTF("StressJpegDec failed\n");
					else HPRINTF("StressJpegDec success\n");
					break;
				case 4:
					if (DEMO_StressScaler() != 0)		EPRINTF("StressScaler failed\n");
					else HPRINTF("StressScaler success\n");
					break;
				case 5:
					if (DEMO_StressFont() != 0)			EPRINTF("StressFont failed\n");
					else HPRINTF("StressFont success\n");
					break;
				case 6:
					if (DEMO_StressNpu() != 0)			EPRINTF("StressNpu failed\n");
					else HPRINTF("StressNpu success\n");
					break;
				case 7:
					if (DEMO_StressAllocFree() != 0)	EPRINTF("StressAllocFree failed\n");
					else HPRINTF("StressAllocFree success\n");
					break;
				case 8:
					if (DEMO_StressQuota() != 0)		EPRINTF("StressQuota failed\n");
					else HPRINTF("StressQuota success\n");
					break;
				case 9:
					if (DEMO_StressInitExit() != 0)		EPRINTF("StressInitExit failed\n");
					else HPRINTF("StressInitExit success\n");
					break;
				case 10:
					if (DEMO_StressConcurrent() != 0)	EPRINTF("StressConcurrent failed\n");
					else HPRINTF("StressConcurrent success\n");
					break;
				default: break;
				}
			}
			break;
		case 8: /* Pipeline (cross-device buffer share) */
			subLoop = 1;
			while (subLoop) {
				printMenu(gVdmaPipelineMenu, sizeof(gVdmaPipelineMenu) / sizeof(gVdmaPipelineMenu[0]), 0);
				sGetMenu(cIn); sub_nSel = atoi(cIn);
				switch (sub_nSel) {
				case 0:
					subLoop = 0;
					break;
				case 1:
					if (DEMO_PipelineRoundtripConnect() != 0)	EPRINTF("PipelineRoundtripConnect failed\n");
					else HPRINTF("PipelineRoundtripConnect success\n");
					break;
				case 2:
					if (DEMO_PipelineFontJenc() != 0)			EPRINTF("PipelineFontJenc failed\n");
					else HPRINTF("PipelineFontJenc success\n");
					break;
				case 3:
					if (DEMO_PipelineScalerJenc() != 0)			EPRINTF("PipelineScalerJenc failed\n");
					else HPRINTF("PipelineScalerJenc success\n");
					break;
				case 4:
					if (DEMO_PipelineJdecScaler() != 0)			EPRINTF("PipelineJdecScaler failed\n");
					else HPRINTF("PipelineJdecScaler success\n");
					break;
				case 5:
					if (DEMO_PipelineFdSanity() != 0)			EPRINTF("PipelineFdSanity failed\n");
					else HPRINTF("PipelineFdSanity success\n");
					break;
				case 6:
					if (DEMO_PipelineForkScm() != 0)			EPRINTF("PipelineForkScm failed\n");
					else HPRINTF("PipelineForkScm success\n");
					break;
				default: break;
				}
			}
			break;
		case 0:
			/* Treat non-numeric garbage that atoi turned into 0 the same
			 * as an unrecognized command — only an explicit "0" exits. */
			if (strcmp(cIn, "0") != 0) break;
			/* Tear down anything the user left initialized, in reverse
			 * order. Each Exit is no-op if the corresponding module was
			 * never initialized in this session. */
			if (g_state.scaler_inited[0])	ENX_DMA_Scaler_Exit(ENX_VDMA_SCALER0);
			if (g_state.scaler_inited[1])	ENX_DMA_Scaler_Exit(ENX_VDMA_SCALER1);
			if (g_state.font_inited)		{ ENX_DMA_Font_Stop_All(); ENX_DMA_Font_Exit(); }
			if (g_state.jenc_inited)		ENX_DMA_JpegEnc_Exit();
			if (g_state.jdec_inited)		ENX_DMA_JpegDec_Exit();
			if (g_state.npu_inited)			ENX_DMA_Npu_Exit();
			HPRINTF("Bye.\n");
			return 0;
		default:
			break;
		}

		printMenu(gVdmaMenu, nMenuCnt, nSel);
	}
}