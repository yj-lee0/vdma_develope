/*
 * Copyright (C) 2026 Eyenix Corporation
 * Eyenix <support6@eyenix.com>
 *
 * Interactive demo for enx_vdma — Scaler / Font / JPEG ENC / JPEG DEC.
 *
 * User I/F is modeled after demo/en683_demo/demo_vdma.c, but all calls
 * route through the new userapp/lib/ API (ENX_DMA_*_Buffer_Alloc / Free,
 * VDMA_JPEG_{ENC,DEC}_CONFIG_S, JpegEnc_Execute size-out, JpegDec
 * inferring width/height from the JPEG header).
 *
 * Build (from skeleton/userapp/lib):
 *   make Demo_vdma
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <linux/types.h>

#include "../enx_vdma.h"
#include "Demo_vdma.h"

#define MAX_FONT_NUM        16

/*******************************************************************************
 *
 * Local helpers — printing, menu, line input, timing, debugfs
 *
 ******************************************************************************/

/*
 * GetMenu — read one trimmed line. On EOF (Ctrl-D or stdin closed), prints
 * a notice and terminates the demo cleanly so the user does not get a
 * silent / accidental exit from an unread token. Caller must pass a buffer
 * of at least 256 bytes (matches the cIn[256] pattern used throughout).
 */
static void GetMenu(char *buf)
{
	if (!fgets(buf, 256, stdin)) {
		fprintf(stdout, "\n[EOF] terminating.\n");
		exit(0);
	}
	buf[strcspn(buf, "\n")] = 0;
}

static void printMenu(const char * const *menu, int count, int selected)
{
	int i;
	(void)selected;
	printf("\n");
	for (i = 0; i < count; i++)
		printf("%s\n", menu[i]);
	printf("> ");
	fflush(stdout);
}

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* Dump a single debugfs `stats` file for the named vdma device. Best-effort:
 * if debugfs is not mounted or the dev node is missing, silently skip. */
static void show_dev_stats(const char *dev_name)
{
	char path[128];
	char line[256];
	FILE *fp;

	snprintf(path, sizeof(path), "/sys/kernel/debug/enx_vdma/%s/stats", dev_name);
	fp = fopen(path, "r");
	if (!fp) return;
	printf("%s--- debugfs:%s ---%s\n", C_CYAN, dev_name, C_RESET);
	while (fgets(line, sizeof(line), fp))
		printf("  %s", line);
	fclose(fp);
}

/*
 * Tracks which modules the user has Init'ed so Exec / Update can give a
 * friendly "call Init first" instead of letting the lib return a bare
 * errno. font_init_type also remembers KERNEL vs VIDEO_CORE.
 */
static struct {
	bool scaler_inited[2];          /* SCALER0, SCALER1 */
	bool font_inited;
	int  font_init_type;            /* ENX_VDMA_INIT_TYPE_* */
	bool jenc_inited;
	bool jdec_inited;
} g_state;

#define REQUIRE_INIT(flag, name) do {                                  \
		if (!(flag)) {                                                  \
			EPRINTF("'%s' Init has not been called yet.\n", (name));    \
			return -1;                                                  \
		}                                                                \
	} while (0)

/*******************************************************************************
 *
 * file io
 *
 ******************************************************************************/

static size_t get_file_size(char *file_name)
{
	size_t file_size;
	FILE *fp;

	if (!file_name) {
		EPRINTF("file name is null.\n");
		return 0;
	}

	printf("[get file] file_name : %s\n", file_name);

	fp = fopen(file_name, "rb");
	if (!fp) {
		EPRINTF("Failed to open file %s.\n", file_name);
		return 0;
	}

	fseek(fp, 0L, SEEK_END);
	file_size = ftell(fp);
	fclose(fp);
	return file_size;
}

static int file_read(char *file_name, unsigned char *buffer, size_t file_size)
{
	size_t bytes_read = 0;
	FILE *fp;

	if (!file_name || !buffer) {
		EPRINTF("Invalid argument\n");
		return -1;
	}

	fp = fopen(file_name, "rb");
	if (!fp) {
		EPRINTF("fail, %s open\n", file_name);
		return -1;
	}

	while (bytes_read < file_size) {
		size_t bytes_left = file_size - bytes_read;
		size_t block_size = bytes_left < 4096 ? bytes_left : 4096;
		size_t read = fread(buffer + bytes_read, 1, block_size, fp);
		if (read != block_size) {
			printf("fail, %s read (%zu/%zu)\n", file_name, bytes_read, file_size);
			fclose(fp);
			return 1;
		}
		bytes_read += read;
	}

	fclose(fp);
	return 0;
}

static int file_write(char *file_name, unsigned char *buffer, size_t file_size)
{
	size_t bytes_write = 0;
	FILE *fp;

	if (!file_name || !buffer || !file_size) {
		EPRINTF("Invalid argument\n");
		return -1;
	}

	fp = fopen(file_name, "wb");
	if (!fp) {
		EPRINTF("fail, %s open\n", file_name);
		return -1;
	}

	while (bytes_write < file_size) {
		size_t bytes_left = file_size - bytes_write;
		size_t block_size = bytes_left < 4096 ? bytes_left : 4096;
		size_t write = fwrite(buffer + bytes_write, 1, block_size, fp);
		if (write != block_size) {
			printf("fail, %s write (%zu/%zu)\n", file_name, bytes_write, file_size);
			fclose(fp);
			return 1;
		}
		bytes_write += write;
	}

	fclose(fp);
	return 0;
}

static void file_newname(char *filename)
{
	FILE *fp;
	char tempName[256];
	int count = 1;
	char *extension;

	if (!filename) return;

	extension = strrchr(filename, '.');
	if (!extension) return;

	if ((fp = fopen(filename, "r")) != NULL) {
		fclose(fp);
		do {
			snprintf(tempName, sizeof(tempName), "%.*s_%04d%s",
				 (int)(extension - filename), filename, count, extension);
			count++;
		} while ((fp = fopen(tempName, "r")) != NULL);
		strcpy(filename, tempName);
	}
}

/*******************************************************************************
 *
 * Format helpers
 *
 ******************************************************************************/

static const char *fmt_name[] = {
	"rgb", "rbg", "grb", "gbr", "brg", "bgr",
	"rrr", "ggg", "bbb", "rzz", "zgz", "zzb",
	"yuv", "yvu", "uvy", "uyv", "vuy", "vyu", "yuv_y",
	"yuyv", "yvyu", "uyvy", "vyuy", "yuv422_y",
	"nv12", "nv21", "nv_y",
	"yuv400",
};

static size_t calculate_format_size(int format, int width, int height)
{
	switch (format) {
	case eFmt_RGB:
	case eFmt_RBG:
	case eFmt_GRB:
	case eFmt_GBR:
	case eFmt_BRG:
	case eFmt_BGR:
	case eFmt_RRR:
	case eFmt_GGG:
	case eFmt_BBB:
	case eFmt_Rzz:
	case eFmt_zGz:
	case eFmt_zzB:
	case eFmt_YUV444:
	case eFmt_YUV444_Y:
		return (size_t)width * height * 3;
	case eFmt_YUV422_YUYV:
	case eFmt_YUV422_YVYU:
	case eFmt_YUV422_UYVY:
	case eFmt_YUV422_VYUY:
	case eFmt_YUV422_Y:
		return (size_t)width * height * 2;
	case eFmt_YUV420SP_NV12:
	case eFmt_YVU420SP_NV21:
	case eFmt_YUV420SP_NV_Y:
		return ((size_t)width * height * 3) >> 1;
	case eFmt_YUV400:
		return (size_t)width * height;
	default:
		return 0;
	}
}

/* JPEG marker parsing — used to compute YUV output size before JpegDec alloc */
#define SOI    0xD8
#define SOF0   0xC0
#define SOF2   0xC2
#define EOI    0xD9

static int find_marker(FILE *fp)
{
	int marker = 0;
	while (!feof(fp)) {
		if (fgetc(fp) == 0xFF) {
			marker = fgetc(fp);
			if (marker != 0xFF && marker != 0x00)
				return marker;
		}
	}
	return -1;
}

static size_t file_get_jpeg_size(char *file_name,
				 unsigned short *width,
				 unsigned short *height,
				 ENX_VDMA_FORMAT_E *fmt)
{
	size_t file_size;
	FILE *fp;
	int h_sampling[3] = {0}, v_sampling[3] = {0};
	int marker, num_components = 0;

	if (!file_name || !width || !height || !fmt) {
		EPRINTF("Invalid argument\n");
		return 0;
	}

	fp = fopen(file_name, "rb");
	if (!fp) {
		EPRINTF("fail, %s open\n", file_name);
		return 0;
	}

	if (fgetc(fp) != 0xFF || fgetc(fp) != SOI) {
		EPRINTF("fail, not a valid JPEG file (%s)\n", file_name);
		fclose(fp);
		return 0;
	}

	*width = 0;
	*height = 0;
	while ((marker = find_marker(fp)) != EOI && marker != -1) {
		if (marker == SOF0 || marker == SOF2) {
			fgetc(fp); fgetc(fp);   /* marker length */
			fgetc(fp);              /* sample precision */
			*height = (fgetc(fp) << 8) + fgetc(fp);
			*width  = (fgetc(fp) << 8) + fgetc(fp);
			num_components = fgetc(fp);
			for (int i = 0; i < num_components; i++) {
				fgetc(fp); /* component ID */
				int sf = fgetc(fp);
				h_sampling[i] = (sf >> 4) & 0xF;
				v_sampling[i] = sf & 0xF;
				fgetc(fp); /* table ID */
			}
			break;
		} else {
			int length = (fgetc(fp) << 8) + fgetc(fp);
			fseek(fp, length - 2, SEEK_CUR);
		}
	}

	if (*width == 0 || *height == 0) {
		EPRINTF("Failed to get dimensions (%s)\n\n", file_name);
		fclose(fp);
		return 0;
	}

	printf("Image size: %d x %d, ", *width, *height);
	if (num_components == 1) {
		*fmt = eFmt_YUV400;
		printf("YUV 4:0:0 format\n");
	} else if (h_sampling[1] == 1 && v_sampling[1] == 1 &&
		   h_sampling[2] == 1 && v_sampling[2] == 1) {
		if (h_sampling[0] == 1 && v_sampling[0] == 1) {
			*fmt = eFmt_YUV444;
			printf("YUV 4:4:4 format\n");
		} else if (h_sampling[0] == 2 && v_sampling[0] == 1) {
			*fmt = eFmt_YUV422_UYVY;
			printf("YUV 4:2:2 format\n");
		} else if (h_sampling[0] == 2 && v_sampling[0] == 2) {
			*fmt = eFmt_YUV420SP_NV12;
			printf("YUV 4:2:0 format\n");
		} else {
			printf("Unknown format H(%d:%d:%d) V(%d:%d:%d)\n",
			       h_sampling[0], h_sampling[1], h_sampling[2],
			       v_sampling[0], v_sampling[1], v_sampling[2]);
			fclose(fp);
			return 0;
		}
	}
	fclose(fp);

	file_size = get_file_size(file_name);
	if (!file_size) {
		EPRINTF("Failed to get_file_size(%zu)\n\n", file_size);
		return 0;
	}
	return file_size;
}

/*******************************************************************************
 *
 * DMA-Scaler
 *
 ******************************************************************************/

static int DEMO_ScalerExec(void)
{
	char cIn[256] = {0};
	VDMA_DZ_CONFIG_S cfg;
	unsigned char *src_buf = NULL, *dst_buf = NULL;
	UInt32 src_id = 0, dst_id = 0;
	size_t src_size = 0, dst_size = 0, cal_size;
	char file_name[256] = {0};
	int ret = -1;
	uint64_t t0, t1;

	REQUIRE_INIT(g_state.scaler_inited[0], "Scaler");

	memset(&cfg, 0, sizeof(cfg));
	cfg.src_cache_flush = 1;
	cfg.dst_cache_flush = 1;
	cfg.src_addr_flag   = 0;   /* offset (id) */
	cfg.dst_addr_flag   = 0;   /* offset (id) */

	printf("===== [Scaler Info] ======\n");
	printf("Please enter the scaler settings below.\n");

	printf(" - Enter the name of the source input image file.\n");
	printf("file_name: "); GetMenu(cIn); snprintf(file_name, sizeof(file_name), "%s", cIn);
	src_size = get_file_size(file_name);
	if (!src_size) { EPRINTF("get_file_size failed for %s\n", file_name); goto err; }

	printf(" - Enter the source image resolution.\n");
	printf("src_width_total: ");  GetMenu(cIn); cfg.src_width_total  = atoi(cIn);
	if (!cfg.src_width_total)  { EPRINTF("Invalid horizontal size.\n"); goto err; }
	printf("src_height_total: "); GetMenu(cIn); cfg.src_height_total = atoi(cIn);
	if (!cfg.src_height_total) { EPRINTF("Invalid vertical size.\n");   goto err; }

	src_buf = ENX_DMA_Scaler_Buffer_Alloc_EX(ENX_VDMA_SCALER0, VDMA_KIND_SRC, src_size, &src_id);
	if (!src_buf) { EPRINTF("ENX_DMA_Scaler_Buffer_Alloc(SRC) failed.\n"); goto err; }
	MPRINTF("scaler SRC alloc: id=%u virt=%p size=%zu\n", src_id, src_buf, src_size);

	if (file_read(file_name, src_buf, src_size)) {
		EPRINTF("file_read failed for %s\n", file_name); goto err;
	}

	printf(" - Enter the source image format: (0~27)\n");
	printf("   [RGB]    -  0: RGB,   1: RBG,   2: GRB,   3: GBR,   4: BRG,   5: BGR \n");
	printf("               6: RRR,   7: GGG,   8: BBB,   9: Rzz,  10: zGz,  11: zzB \n");
	printf("   [YUV444] - 12: YUV,  18: Y__\n");
	printf("   [YUV422] - 19: YUYV, 20: YVYU, 21: UYUV, 22: VYUY, 23: Y__\n");
	printf("   [YUV420] - 24: NV12, 25: NV21, 26: Y__\n");
	printf("   [YUV400] - 27: Y\n");
	printf("src_fmt: "); GetMenu(cIn); cfg.src_format = atoi(cIn);
	cal_size = calculate_format_size(cfg.src_format, cfg.src_width_total, cfg.src_height_total);
	if (!cal_size || src_size < cal_size) {
		EPRINTF(cal_size ? "file size smaller than expected for fmt(%d)\n"
				 : "Invalid format(%d)\n", cfg.src_format);
		goto err;
	}

	printf(" - Crop the source image? (1: crop, other: no)\n");
	printf("crop: "); GetMenu(cIn);
	if (atoi(cIn) == 1) {
		printf("src_width_pos: ");  GetMenu(cIn); cfg.src_width_pos  = atoi(cIn);
		printf("src_width: ");      GetMenu(cIn); cfg.src_width      = atoi(cIn);
		printf("src_height_pos: "); GetMenu(cIn); cfg.src_height_pos = atoi(cIn);
		printf("src_height: ");     GetMenu(cIn); cfg.src_height     = atoi(cIn);
		if (cfg.src_width_total  < cfg.src_width  + cfg.src_width_pos ||
		    cfg.src_height_total < cfg.src_height + cfg.src_height_pos) {
			EPRINTF("crop region exceeds source.\n"); goto err;
		}
	} else {
		cfg.src_width  = cfg.src_width_total;
		cfg.src_height = cfg.src_height_total;
	}

	printf(" - flip/mirror? (1: flip, 2: mirror, 3: both, other: none)\n");
	printf("flip_mirror: "); GetMenu(cIn);
	switch (atoi(cIn)) {
	case 1: cfg.hflip = 0; cfg.vflip = 1; break;
	case 2: cfg.hflip = 1; cfg.vflip = 0; break;
	case 3: cfg.hflip = 1; cfg.vflip = 1; break;
	default: cfg.hflip = 0; cfg.vflip = 0; break;
	}

	printf(" - Enter the destination image resolution.\n");
	printf("dst_width: ");  GetMenu(cIn); cfg.dst_width  = atoi(cIn);
	if (!cfg.dst_width)  { EPRINTF("Invalid horizontal size.\n"); goto err; }
	cfg.dst_width_total = cfg.dst_width;
	printf("dst_height: "); GetMenu(cIn); cfg.dst_height = atoi(cIn);
	if (!cfg.dst_height) { EPRINTF("Invalid vertical size.\n");   goto err; }

	printf(" - Enter the destination image format: (0~27)\n");
	printf("dst_fmt: "); GetMenu(cIn); cfg.dst_format = atoi(cIn);
	dst_size = calculate_format_size(cfg.dst_format, cfg.dst_width_total, cfg.dst_height);
	if (!dst_size) { EPRINTF("Invalid format(%d)\n", cfg.dst_format); goto err; }

	dst_buf = ENX_DMA_Scaler_Buffer_Alloc_EX(ENX_VDMA_SCALER0, VDMA_KIND_DST, dst_size, &dst_id);
	if (!dst_buf) { EPRINTF("ENX_DMA_Scaler_Buffer_Alloc(DST) failed.\n"); goto err; }
	MPRINTF("scaler DST alloc: id=%u virt=%p size=%zu\n", dst_id, dst_buf, dst_size);

	cfg.src_addr = src_id;
	cfg.dst_addr = dst_id;

	snprintf(file_name, sizeof(file_name), "output_%dx%d_%s.%s",
		 cfg.dst_width, cfg.dst_height,
		 fmt_name[cfg.dst_format],
		 cfg.dst_format < 12 ? "rgb" : "yuv");

	t0 = now_ns();
	if (ENX_DMA_Scaler_Execute(ENX_VDMA_SCALER0, &cfg) < 0) {
		EPRINTF("ENX_DMA_Scaler_Execute failed.\n"); goto err;
	}
	t1 = now_ns();
	HPRINTF("Scaler Execute: %.3f ms\n", (t1 - t0) / 1.0e6);
	show_dev_stats("vdma_dz");

	file_newname(file_name);
	if (file_write(file_name, dst_buf, dst_size)) {
		EPRINTF("Failed to file write.\n"); goto err;
	}

	HPRINTF("Scaler0 output file: %s, size: %zu bytes\n", file_name, dst_size);
	ret = 0;
err:
	if (src_buf) ENX_DMA_Scaler_Buffer_Free(ENX_VDMA_SCALER0, src_buf, src_size);
	if (dst_buf) ENX_DMA_Scaler_Buffer_Free(ENX_VDMA_SCALER0, dst_buf, dst_size);
	return ret;
}

/*******************************************************************************
 *
 * DMA-Font
 *
 ******************************************************************************/

struct font_alloc_track {
	unsigned char *buf;
	size_t size;
};

static void print_font_info(VDMA_FONT_CONFIG_S *cfg)
{
	printf("============= Font config =============\n");
	printf("%20s : %d\n",  "dst type index",  cfg->dst_type_index);
	printf("%20s : %d\n",  "dst width",       cfg->dst_width);
	printf("%20s : %d\n",  "dst height",      cfg->dst_height);
	printf("%20s : %d\n",  "bg overlay",      cfg->background_overlay);
	printf("%20s : %d\n",  "font number",     cfg->font_number);

	for (unsigned int i = 0; i < cfg->font_number; i++) {
		printf("------------- Font info[%d] -------------\n", i);
		printf("%20s : %d\n",  "src address flag", cfg->font_info[i].src_addr_flag);
		printf("%20s : %#lx\n","src addr",         cfg->font_info[i].src_addr);
		printf("%20s : %d\n",  "src width",        cfg->font_info[i].src_width);
		printf("%20s : %d\n",  "src height",       cfg->font_info[i].src_height);
		printf("%20s : %d\n",  "font xpos",        cfg->font_info[i].font_xpos);
		printf("%20s : %d\n",  "font ypos",        cfg->font_info[i].font_ypos);
		printf("%20s : %d\n",  "font value",       cfg->font_info[i].font_value);
		printf("%20s : %d\n",  "outline value",    cfg->font_info[i].outline_value);
		printf("%20s : %d\n",  "write mode",       cfg->font_info[i].font_wr_mode);
		printf("%20s : %d\n",  "color tone",       cfg->font_info[i].font_color_tone);
		printf("%20s : %d\n",  "alpha blending",   cfg->font_info[i].font_alpha);
		printf("%20s : %d\n",  "font threshold",   cfg->font_info[i].font_threshold);
	}
}

static int DEMO_FontUpdate(int init_type)
{
	int nSel = 0;
	char cIn[256] = {0};
	VDMA_FONT_CONFIG_S *cfg = NULL;
	struct font_alloc_track srcs[MAX_FONT_NUM] = {0};
	unsigned int font_number = 0, yc_channel = 0;
	int src_count = 0;
	unsigned char *dst_buf = NULL;
	size_t dst_size = 0, dst_input_size = 0;
	char file_name[256] = {0};
	char dst_file_name[256] = {0};
	bool is_color = false;
	bool is_dst_input = false;
	uint64_t t0, t1;

	REQUIRE_INIT(g_state.font_inited, "Font");

	printMenu(gVdmaExecMenu, 5, 0); GetMenu(cIn); nSel = atoi(cIn);

	if (nSel == 1) {
		/* Example setting — single font, 270x122 YUV file */
		font_number = 1;
		yc_channel = 3;
		cfg = ENX_DMA_Font_CfgAlloc(font_number, yc_channel);
		if (!cfg) { EPRINTF("VDMA_FONT_CONFIG_S alloc failed.\n"); return -1; }

		snprintf(file_name, sizeof(file_name), "%s", "font_270x122_TEST.yuv");
		size_t src_size = get_file_size(file_name);
		if (!src_size) { EPRINTF("get_file_size failed for %s\n", file_name); goto err; }

		UInt32 src_id;
		srcs[0].buf = ENX_DMA_Font_Buffer_Alloc_EX(VDMA_KIND_SRC, src_size, &src_id);
		if (!srcs[0].buf) { EPRINTF("Font src alloc failed.\n"); goto err; }
		MPRINTF("font SRC alloc[0]: id=%u virt=%p size=%zu\n", src_id, srcs[0].buf, src_size);
		srcs[0].size = src_size;
		src_count = 1;

		if (file_read(file_name, srcs[0].buf, src_size)) {
			EPRINTF("file_read failed for %s\n", file_name); goto err;
		}

		cfg->font_info[0].src_addr_flag    = 0;
		cfg->font_info[0].src_addr         = src_id;
		cfg->font_info[0].src_width        = 270;
		cfg->font_info[0].src_height       = 122;
		cfg->font_info[0].font_color_y     = 150;
		cfg->font_info[0].font_color_cb    = 170;
		cfg->font_info[0].font_color_cr    = 140;
		cfg->font_info[0].font_value       = 255;
		cfg->font_info[0].outline_color_y  = 0;
		cfg->font_info[0].outline_color_cb = 128;
		cfg->font_info[0].outline_color_cr = 128;
		cfg->font_info[0].outline_value    = 128;
		cfg->font_info[0].bg_color_y       = 0;
		cfg->font_info[0].bg_color_cb      = 0;
		cfg->font_info[0].bg_color_cr      = 0;
		cfg->font_info[0].font_wr_mode     = 0;   /* YC(RW) */
		cfg->font_info[0].font_color_tone  = 1;
		cfg->font_info[0].font_alpha       = 256; /* opaque */
		cfg->font_info[0].font_threshold   = 0;

		if (init_type == ENX_VDMA_INIT_TYPE_KERNEL) {
			cfg->font_info[0].font_xpos = 0;
			cfg->font_info[0].font_ypos = 0;
			cfg->dst_type_index = 0;
			cfg->dst_width  = cfg->font_info[0].src_width;
			cfg->dst_height = cfg->font_info[0].src_height;
			cfg->background_overlay = 0;

			if (cfg->font_info[0].font_wr_mode == 1 || cfg->font_info[0].font_wr_mode == 3) {
				dst_size = (size_t)cfg->dst_width * cfg->dst_height;
			} else {
				dst_size = ((size_t)cfg->dst_width * cfg->dst_height * 3) >> 1;
				is_color = true;
			}

			dst_buf = ENX_DMA_Font_Buffer_Alloc(VDMA_KIND_DST, dst_size);
			if (!dst_buf) { EPRINTF("Font dst alloc failed.\n"); goto err; }
			MPRINTF("font DST alloc: virt=%p size=%zu\n", dst_buf, dst_size);

			snprintf(file_name, sizeof(file_name), "output_%dx%d_%s.yuv",
				 cfg->dst_width, cfg->dst_height, is_color ? "color" : "gray");
		} else {
			cfg->font_info[0].font_xpos = 100;
			cfg->font_info[0].font_ypos = 100;
			cfg->dst_width  = 0;
			cfg->dst_height = 0;
		}
	}
	else if (nSel == 2) {
		printf("======= [Font Info] ========\n");
		printf("Please enter the font settings below.\n");

		if (init_type == ENX_VDMA_INIT_TYPE_VIDEO_CORE) {
			printf(" - YC channel (0~%d)\n", MAX_VISP_YC_NUM - 1);
			printf("yc_channel: "); GetMenu(cIn); yc_channel = atoi(cIn);
			if (yc_channel >= MAX_VISP_YC_NUM) { EPRINTF("Invalid argument\n"); return -1; }
		}

		printf(" - Number of font images:\n");
		printf("font_number: "); GetMenu(cIn); font_number = atoi(cIn);
		if (!font_number || font_number > MAX_FONT_NUM) {
			EPRINTF("Invalid font_number (max %d).\n", MAX_FONT_NUM); return -1;
		}

		cfg = ENX_DMA_Font_CfgAlloc(font_number, yc_channel);
		if (!cfg) { EPRINTF("VDMA_FONT_CONFIG_S alloc failed.\n"); return -1; }

		if (init_type == ENX_VDMA_INIT_TYPE_KERNEL) {
			cfg->dst_type_index = 0;
			printf(" - Destination input?\n   (1: Font overlay on input file, other: Generate only)\n");
			printf("is_dst_input: "); GetMenu(cIn); is_dst_input = (atoi(cIn) == 1);
			if (is_dst_input) {
				cfg->background_overlay = 1;
				printf(" - dst overlay file: ");
				GetMenu(cIn); snprintf(dst_file_name, sizeof(dst_file_name), "%s", cIn);
				dst_input_size = get_file_size(dst_file_name);
				if (!dst_input_size) { EPRINTF("get_file_size failed for %s\n", dst_file_name); goto err; }
			}
			printf(" - dst resolution: %s\n", is_dst_input ? "(overlay)" : "(output)");
			printf("dst_width: ");  GetMenu(cIn); cfg->dst_width  = atoi(cIn);
			printf("dst_height: "); GetMenu(cIn); cfg->dst_height = atoi(cIn);
			if (!cfg->dst_width || cfg->dst_width > 3840 ||
			    !cfg->dst_height || cfg->dst_height > 3840) {
				EPRINTF("Invalid dst resolution.\n"); goto err;
			}
		}

		for (unsigned int i = 0; i < font_number; i++) {
			UInt32 src_id;
			size_t src_size;

			printf("-[Font%d]--------------------\n", i + 1);
			cfg->font_info[i].src_addr_flag = 0;

			printf(" - source file[%d]: ", i);
			GetMenu(cIn); snprintf(file_name, sizeof(file_name), "%s", cIn);
			src_size = get_file_size(file_name);
			if (!src_size) { EPRINTF("get_file_size failed for %s\n", file_name); goto err; }

			printf("src_width[%d]: ",  i); GetMenu(cIn); cfg->font_info[i].src_width  = atoi(cIn);
			printf("src_height[%d]: ", i); GetMenu(cIn); cfg->font_info[i].src_height = atoi(cIn);

			if (init_type == ENX_VDMA_INIT_TYPE_KERNEL) {
				if (cfg->font_info[i].src_width  > cfg->dst_width ||
				    cfg->font_info[i].src_height > cfg->dst_height) {
					EPRINTF("src exceeds dst.\n"); goto err;
				}
			}
			if (src_size < (size_t)cfg->font_info[i].src_width * cfg->font_info[i].src_height) {
				EPRINTF("file too small for declared resolution.\n"); goto err;
			}

			printf("font_xpos[%d]: ", i); GetMenu(cIn); cfg->font_info[i].font_xpos = atoi(cIn);
			printf("font_ypos[%d]: ", i); GetMenu(cIn); cfg->font_info[i].font_ypos = atoi(cIn);
			if (init_type == ENX_VDMA_INIT_TYPE_KERNEL) {
				if ((cfg->font_info[i].font_xpos + cfg->font_info[i].src_width)  > cfg->dst_width ||
				    (cfg->font_info[i].font_ypos + cfg->font_info[i].src_height) > cfg->dst_height) {
					EPRINTF("src + pos exceeds dst.\n"); goto err;
				}
			}

			srcs[i].buf = ENX_DMA_Font_Buffer_Alloc_EX(VDMA_KIND_SRC, src_size, &src_id);
			if (!srcs[i].buf) { EPRINTF("Font src alloc failed.\n"); goto err; }
			MPRINTF("font SRC alloc[%d]: id=%u virt=%p size=%zu\n", i, src_id, srcs[i].buf, src_size);
			srcs[i].size = src_size;
			src_count = i + 1;

			cfg->font_info[i].src_addr = src_id;

			if (file_read(file_name, srcs[i].buf, src_size)) {
				EPRINTF("file_read failed for %s\n", file_name); goto err;
			}

			printf("font_value    (0~255, def 255): "); GetMenu(cIn); cfg->font_info[i].font_value    = atoi(cIn);
			printf("outline_value (0~255, def 128): "); GetMenu(cIn); cfg->font_info[i].outline_value = atoi(cIn);
			printf("font_color_y:     "); GetMenu(cIn); cfg->font_info[i].font_color_y     = atoi(cIn);
			printf("font_color_cb:    "); GetMenu(cIn); cfg->font_info[i].font_color_cb    = atoi(cIn);
			printf("font_color_cr:    "); GetMenu(cIn); cfg->font_info[i].font_color_cr    = atoi(cIn);
			printf("outline_color_y:  "); GetMenu(cIn); cfg->font_info[i].outline_color_y  = atoi(cIn);
			printf("outline_color_cb: "); GetMenu(cIn); cfg->font_info[i].outline_color_cb = atoi(cIn);
			printf("outline_color_cr: "); GetMenu(cIn); cfg->font_info[i].outline_color_cr = atoi(cIn);
			printf("bg_color_y:       "); GetMenu(cIn); cfg->font_info[i].bg_color_y       = atoi(cIn);
			printf("bg_color_cb:      "); GetMenu(cIn); cfg->font_info[i].bg_color_cb      = atoi(cIn);
			printf("bg_color_cr:      "); GetMenu(cIn); cfg->font_info[i].bg_color_cr      = atoi(cIn);

			printf("font_wr_mode (0:YC[RW] 1:Y[RW] 2:YC[W] 3:Y[W]): ");
			GetMenu(cIn); cfg->font_info[i].font_wr_mode = atoi(cIn);
			if (cfg->font_info[i].font_wr_mode > 3) { EPRINTF("Invalid.\n"); goto err; }

			if (init_type == ENX_VDMA_INIT_TYPE_KERNEL) {
				if (cfg->font_info[i].font_wr_mode == 0 ||
				    cfg->font_info[i].font_wr_mode == 2)
					is_color = true;
			}

			printf("color_tone (0:bitmap 1:font): ");
			GetMenu(cIn); cfg->font_info[i].font_color_tone = atoi(cIn);
			if (cfg->font_info[i].font_color_tone > 1) { EPRINTF("Invalid.\n"); goto err; }

			printf("font_alpha (0~256): ");
			GetMenu(cIn); cfg->font_info[i].font_alpha = atoi(cIn);
			if (cfg->font_info[i].font_alpha > 256) { EPRINTF("Invalid.\n"); goto err; }

			printf("font_threshold (0~255): ");
			GetMenu(cIn); cfg->font_info[i].font_threshold = atoi(cIn);
		}

		if (init_type == ENX_VDMA_INIT_TYPE_KERNEL) {
			dst_size = is_color
				? (((size_t)cfg->dst_width * cfg->dst_height * 3) >> 1)
				:  ((size_t)cfg->dst_width * cfg->dst_height);

			dst_buf = ENX_DMA_Font_Buffer_Alloc(VDMA_KIND_DST, dst_size);
			if (!dst_buf) { EPRINTF("Font dst alloc failed.\n"); goto err; }
			MPRINTF("font DST alloc: virt=%p size=%zu\n", dst_buf, dst_size);

			if (is_dst_input) {
				if (file_read(dst_file_name, dst_buf,
					      dst_input_size > dst_size ? dst_size : dst_input_size)) {
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
	show_dev_stats("vdma_font");

	print_font_info(cfg);

	if (init_type == ENX_VDMA_INIT_TYPE_KERNEL && dst_buf && dst_size) {
		file_newname(file_name);
		if (file_write(file_name, dst_buf, dst_size)) {
			EPRINTF("Failed to file write.\n"); goto err;
		}
		HPRINTF("Font output file: %s, size: %zu bytes\n", file_name, dst_size);
	}

	for (int i = 0; i < src_count; i++)
		if (srcs[i].buf) ENX_DMA_Font_Buffer_Free(srcs[i].buf, srcs[i].size);
	if (dst_buf) ENX_DMA_Font_Buffer_Free(dst_buf, dst_size);
	ENX_DMA_Font_CfgFree();
	return 0;

err:
	ENX_DMA_Font_Stop_All();
	for (int i = 0; i < src_count; i++)
		if (srcs[i].buf) ENX_DMA_Font_Buffer_Free(srcs[i].buf, srcs[i].size);
	if (dst_buf) ENX_DMA_Font_Buffer_Free(dst_buf, dst_size);
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
	char cIn[256] = {0};
	VDMA_JPEG_ENC_CONFIG_S cfg;
	unsigned char *src_buf = NULL, *dst_buf = NULL;
	UInt32 src_id = 0, dst_id = 0;
	size_t src_size = 0, dst_size = 0, out_size = 0;
	char file_name[256] = {0};
	int ret = -1, fmt = 0;
	uint64_t t0, t1;

	REQUIRE_INIT(g_state.jenc_inited, "JpegEnc");

	memset(&cfg, 0, sizeof(cfg));
	cfg.src_cache_flush = 1;
	cfg.dst_cache_flush = 1;
	cfg.src_addr_flag   = 0;
	cfg.dst_addr_flag   = 0;

	printf("======= [Jenc Info] ========\n");
	printf("Please enter the jpeg-enc settings below.\n");

	printf(" - source file: ");
	GetMenu(cIn); snprintf(file_name, sizeof(file_name), "%s", cIn);
	src_size = get_file_size(file_name);
	if (!src_size) { EPRINTF("get_file_size failed for %s\n", file_name); goto err; }

	printf("src_width_total: ");  GetMenu(cIn); cfg.src_width_total  = atoi(cIn);
	printf("src_height_total: "); GetMenu(cIn); cfg.src_height_total = atoi(cIn);
	if (!cfg.src_width_total || !cfg.src_height_total) { EPRINTF("Invalid size.\n"); goto err; }

	printf(" - Crop? (1: crop, other: no)\n");
	printf("crop: "); GetMenu(cIn);
	if (atoi(cIn) == 1) {
		printf("src_width_pos: ");  GetMenu(cIn); cfg.src_width_pos  = atoi(cIn);
		printf("src_width: ");      GetMenu(cIn); cfg.src_width      = atoi(cIn);
		printf("src_height_pos: "); GetMenu(cIn); cfg.src_height_pos = atoi(cIn);
		printf("src_height: ");     GetMenu(cIn); cfg.src_height     = atoi(cIn);
		if (cfg.src_width_total  < cfg.src_width  + cfg.src_width_pos ||
		    cfg.src_height_total < cfg.src_height + cfg.src_height_pos) {
			EPRINTF("crop region exceeds source.\n"); goto err;
		}
	} else {
		cfg.src_width  = cfg.src_width_total;
		cfg.src_height = cfg.src_height_total;
	}

	printf(" - source fmt: (0:YUV444 1:YUV422 2:YUV420 3:Y)\n");
	printf("src_fmt: "); GetMenu(cIn); fmt = atoi(cIn);
	switch (fmt) {
	case 0: cfg.src_format = eFmt_YUV444;
		if (src_size < (size_t)cfg.src_width_total * cfg.src_height_total * 3) goto err_size;
		break;
	case 1: cfg.src_format = eFmt_YUV422_UYVY;
		if (src_size < (size_t)cfg.src_width_total * cfg.src_height_total * 2) goto err_size;
		break;
	case 2: cfg.src_format = eFmt_YUV420SP_NV12;
		if (src_size < ((size_t)cfg.src_width_total * cfg.src_height_total * 3) >> 1) goto err_size;
		break;
	case 3: cfg.src_format = eFmt_YUV400;
		if (src_size < (size_t)cfg.src_width_total * cfg.src_height_total) goto err_size;
		break;
	default: EPRINTF("Invalid argument\n"); goto err;
	}

	src_buf = ENX_DMA_JpegEnc_Buffer_Alloc_EX(VDMA_KIND_SRC, src_size, &src_id);
	if (!src_buf) { EPRINTF("JpegEnc src alloc failed.\n"); goto err; }
	MPRINTF("jenc SRC alloc: id=%u virt=%p size=%zu\n", src_id, src_buf, src_size);

	if (file_read(file_name, src_buf, src_size)) {
		EPRINTF("file_read failed for %s\n", file_name); goto err;
	}

	printf("ovf_threshold (KB, 0:not use): ");
	GetMenu(cIn); cfg.ovf_threshold = (unsigned int)atoi(cIn) * 1024;

	printf("quality (0~100): ");
	GetMenu(cIn); cfg.quality = atoi(cIn);
	if (cfg.quality > 100) { EPRINTF("Invalid quality.\n"); goto err; }

	dst_size = cfg.ovf_threshold ? cfg.ovf_threshold
				     : (size_t)cfg.src_width_total * cfg.src_height_total * 4;
	dst_buf = ENX_DMA_JpegEnc_Buffer_Alloc_EX(VDMA_KIND_DST, dst_size, &dst_id);
	if (!dst_buf) { EPRINTF("JpegEnc dst alloc failed.\n"); goto err; }
	MPRINTF("jenc DST alloc: id=%u virt=%p size=%zu\n", dst_id, dst_buf, dst_size);

	cfg.src_addr = src_id;
	cfg.dst_addr = dst_id;

	snprintf(file_name, sizeof(file_name), "output_%dx%d_%s.jpg",
		 cfg.src_width, cfg.src_height,
		 fmt == 0 ? "yuv" : fmt == 1 ? "uyvy" : fmt == 2 ? "nv12" : "yuv400");

	t0 = now_ns();
	if (ENX_DMA_JpegEnc_Execute(&cfg, &out_size) < 0 || !out_size) {
		EPRINTF("ENX_DMA_JpegEnc_Execute failed.\n"); goto err;
	}
	t1 = now_ns();
	HPRINTF("JpegEnc Execute: %.3f ms (encoded %zu bytes, ratio=%.2fx)\n",
		(t1 - t0) / 1.0e6, out_size, src_size ? (double)src_size / out_size : 0.0);
	show_dev_stats("vdma_jpegenc");

	file_newname(file_name);
	if (file_write(file_name, dst_buf, out_size)) {
		EPRINTF("Failed to file write.\n"); goto err;
	}

	HPRINTF("JpegEnc output file: %s, size: %zu bytes\n", file_name, out_size);
	ret = 0;
	goto out;

err_size:
	EPRINTF("file size smaller than expected for fmt(%d).\n", fmt);
err:
out:
	if (src_buf) ENX_DMA_JpegEnc_Buffer_Free(src_buf, src_size);
	if (dst_buf) ENX_DMA_JpegEnc_Buffer_Free(dst_buf, dst_size);
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
	unsigned char *src_buf = NULL, *dst_buf = NULL;
	UInt32 src_id = 0, dst_id = 0;
	size_t src_size = 0, dst_size = 0, yuv_out = 0;
	unsigned short width = 0, height = 0;
	ENX_VDMA_FORMAT_E fmt = eFmt_YUV444;
	char file_name[256] = {0};
	int ret = -1;
	uint64_t t0, t1;

	REQUIRE_INIT(g_state.jdec_inited, "JpegDec");

	memset(&cfg, 0, sizeof(cfg));
	cfg.src_cache_flush = 1;
	cfg.dst_cache_flush = 1;
	cfg.src_addr_flag   = 0;
	cfg.dst_addr_flag   = 0;

	printf("======= [Jdec Info] ========\n");
	printf(" - JPEG file: ");
	GetMenu(cIn); snprintf(file_name, sizeof(file_name), "%s", cIn);

	src_size = file_get_jpeg_size(file_name, &width, &height, &fmt);
	if (!src_size) { EPRINTF("file_get_jpeg_size failed for %s\n", file_name); goto err; }

	if (width % 2 == 1) width++;

	switch (fmt) {
	case eFmt_YUV444:        dst_size = (size_t)width * height * 3; break;
	case eFmt_YUV422_UYVY:   dst_size = (size_t)width * height * 2; break;
	case eFmt_YUV420SP_NV12: dst_size = ((size_t)width * height * 3) >> 1; break;
	case eFmt_YUV400:        dst_size = (size_t)width * height; break;
	default: EPRINTF("Unsupported fmt.\n"); goto err;
	}

	src_buf = ENX_DMA_JpegDec_Buffer_Alloc_EX(VDMA_KIND_SRC, src_size, &src_id);
	if (!src_buf) { EPRINTF("JpegDec src alloc failed.\n"); goto err; }
	MPRINTF("jdec SRC alloc: id=%u virt=%p size=%zu\n", src_id, src_buf, src_size);

	if (file_read(file_name, src_buf, src_size)) {
		EPRINTF("file_read failed for %s\n", file_name); goto err;
	}

	dst_buf = ENX_DMA_JpegDec_Buffer_Alloc_EX(VDMA_KIND_DST, dst_size, &dst_id);
	if (!dst_buf) { EPRINTF("JpegDec dst alloc failed.\n"); goto err; }
	MPRINTF("jdec DST alloc: id=%u virt=%p size=%zu\n", dst_id, dst_buf, dst_size);

	cfg.src_addr      = src_id;
	cfg.dst_addr      = dst_id;
	cfg.src_jpeg_size = (unsigned int)src_size;
	cfg.dst_buf_size  = (unsigned int)dst_size;

	snprintf(file_name, sizeof(file_name), "output_%dx%d_%s.yuv",
		 width, height,
		 fmt == eFmt_YUV444        ? "yuv"  :
		 fmt == eFmt_YUV422_UYVY   ? "uyvy" :
		 fmt == eFmt_YUV420SP_NV12 ? "nv12" : "yuv400");

	t0 = now_ns();
	if (ENX_DMA_JpegDec_Execute(&cfg, &yuv_out) < 0 || !yuv_out) {
		EPRINTF("ENX_DMA_JpegDec_Execute failed.\n"); goto err;
	}
	t1 = now_ns();
	HPRINTF("JpegDec Execute: %.3f ms (decoded %zu bytes)\n", (t1 - t0) / 1.0e6, yuv_out);
	show_dev_stats("vdma_jpegdec");

	file_newname(file_name);
	if (file_write(file_name, dst_buf, yuv_out)) {
		EPRINTF("Failed to file write.\n"); goto err;
	}

	HPRINTF("JpegDec output file: %s, size: %zu bytes\n", file_name, yuv_out);
	ret = 0;
err:
	if (src_buf) ENX_DMA_JpegDec_Buffer_Free(src_buf, src_size);
	if (dst_buf) ENX_DMA_JpegDec_Buffer_Free(dst_buf, dst_size);
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
		GetMenu(cIn);
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
				printMenu(gVdmaDzMenu, 5, 0); GetMenu(cIn); sub_nSel = atoi(cIn);
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
				printMenu(gVdmaFontMenu, 6, 0); GetMenu(cIn); sub_nSel = atoi(cIn);
				switch (sub_nSel) {
				case 0:
					ENX_DMA_Font_Stop_All();
					ENX_DMA_Font_Exit();
					g_state.font_inited = false;
					subLoop = 0;
					break;
				case 1:
					printMenu(gVdmaInitMenu, 4, 0); GetMenu(cIn); init_type = atoi(cIn);
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
				printMenu(gVdmaJencMenu, 5, 0); GetMenu(cIn); sub_nSel = atoi(cIn);
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
				printMenu(gVdmaJdecMenu, 5, 0); GetMenu(cIn); sub_nSel = atoi(cIn);
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
		case 0:
			/* Treat non-numeric garbage that atoi turned into 0 the same
			 * as an unrecognized command — only an explicit "0" exits. */
			if (strcmp(cIn, "0") != 0) break;
			HPRINTF("Bye.\n");
			return 0;
		default:
			break;
		}

		printMenu(gVdmaMenu, nMenuCnt, nSel);
	}
}
