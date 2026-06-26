/*
 * Copyright (C) 2026 Eyenix Corporation
 * Eyenix <support6@eyenix.com>
 *
 * VDMA demo — shared utility implementations.
 *   - input / EOF-safe line read, parse_uint
 *   - file I/O
 *   - format helpers / NV12 pattern / PSNR
 *   - JPEG header parser
 *   - debugfs counter readers + dump helpers
 *   - loop_stats (latency percentile collector)
 *   - vmod_* module dispatcher
 *   - input prompts (ask_iters / ask_thread_count / ask_module)
 */
#define _POSIX_C_SOURCE 200809L

#include <math.h>

#include "demo_vdma.h"
#include "demo_vdma_util.h"

/* The max_src value is determined by the font driver's max_src parameter (default: 16). */
#define MAX_FONT_NUM	16

/* Singleton init-state tracker — the lone definition for the extern in util.h. */
struct demo_g_state g_state;

/*******************************************************************************
 *
 * Local helpers — printing, menu, line input, timing, debugfs
 *
 ******************************************************************************/

/*
 * printMenu — prints a menu list with the nIdx-th entry highlighted with ">> ".
 * The caller must pass a NULL-terminated array of strings and the number of
 * entries in the array. The first entry (index 0) is treated as a title and
 * is never highlighted.
 */
void printMenu(const char * const pMenuList[], int nMenuCnt, int nIdx)
{
	int i = 0;
	char szSelect[4] = ">> ";

	printf("\n");
	for(i = 0; i < nMenuCnt; i++)
	{
		if(i == nIdx && i != 0)
			printf("%s%s\n", szSelect, pMenuList[i]);
		else
			printf("%s\n", pMenuList[i]); 
	}
	printf("Select : ");
}

/*
 * sGetMenu — read one trimmed line. On EOF (Ctrl-D or stdin closed), prints
 * a notice and terminates the demo cleanly so the user does not get a
 * silent / accidental exit from an unread token. Caller must pass a buffer
 * of at least 256 bytes (matches the cIn[256] pattern used throughout).
 */
void sGetMenu(char *buf)
{
	if (!fgets(buf, 256, stdin)) {
		fprintf(stdout, "\n[EOF] terminating.\n");
		exit(0);
	}
	buf[strcspn(buf, "\n")] = 0;
}

/*
 * parse_uint — strict decimal parser. Distinguishes a legitimate "0" from
 * non-numeric junk (which atoi() silently turns into 0). Returns 0 on
 * success with the value in *out, -1 on parse failure.
 *
 * Use this where the caller needs to tell "user typed garbage" apart from
 * "user typed 0", typically for sizes / counts where 0 is also invalid.
 */
int parse_uint(const char *s, unsigned long *out)
{
	char *end;
	unsigned long v;

	if (!s || !*s) return -1;
	errno = 0;
	v = strtoul(s, &end, 10);
	if (errno || end == s || *end) return -1;
	*out = v;
	return 0;
}
uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/*
 * Dump one debugfs file under /sys/kernel/debug/enx_vdma/<dev>/. If debugfs
 * is not mounted or the node is missing, silently skip.
 */
static void dump_debugfs(const char *dev_name, const char *node)
{
	char path[128];
	char line[256];
	FILE *fp;

	snprintf(path, sizeof(path), "/sys/kernel/debug/enx_vdma/%s/%s", dev_name, node);
	fp = fopen(path, "r");
	if (!fp) return;
	printf("%s--- debugfs:%s/%s ---%s\n", C_CYAN, dev_name, node, C_RESET);
	while (fgets(line, sizeof(line), fp))
		printf("  %s", line);
	fclose(fp);
}

/*
 * Dump both stats and bufs for one vdma device. Called after each Execute
 * so the user can observe both global counters (in_flight, bufs_active,
 * etc.) and per-buffer state (id, refs, owner) without leaving the demo.
 */
void show_dev_stats(const char *dev_name)
{
	dump_debugfs(dev_name, "stats");
	dump_debugfs(dev_name, "bufs");
}
/*******************************************************************************
 *
 * file io
 *
 ******************************************************************************/

size_t get_file_size(char *file_name)
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

int file_read(char *file_name, unsigned char *buffer, size_t file_size)
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

int file_write(char *file_name, unsigned char *buffer, size_t file_size)
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

void file_newname(char *filename)
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
 * Support Format helpers
 *
 ******************************************************************************/

const char *fmt_name[] = {
	"rgb", "rbg", "grb", "gbr", "brg", "bgr",
	"rrr", "ggg", "bbb", "rzz", "zgz", "zzb",
	"yuv", "yvu", "uvy", "uyv", "vuy", "vyu", "yuv_y",
	"yuyv", "yvyu", "uyvy", "vyuy", "yuv422_y",
	"nv12", "nv21", "nv_y",
	"yuv400",
};

size_t calculate_format_size(int format, int width, int height)
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

/*
 * Fill a NV12 buffer with a 7-bar SMPTE-style vertical color pattern, plus
 * a deterministic per-pixel noise dither so the image carries realistic
 * high-frequency content. Without the noise, every 8x8 DCT block would be
 * almost pure DC and the JPEG encoder would produce artificially high
 * compression ratios / PSNR — meaning the Roundtrip test cannot
 * distinguish a healthy codec from a broken one.
 *
 * Layout: Y plane (width * height bytes) followed by interleaved UV plane
 * (width * height / 2 bytes, 4:2:0).
 */
void fill_nv12_pattern(unsigned char *buf, int width, int height)
{
	static const struct { unsigned char y, u, v; } bars[7] = {
		{ 235, 128, 128 },	/* White	*/
		{ 210,  16, 146 },	/* Yellow	*/
		{ 170, 166,  16 },	/* Cyan		*/
		{ 145,  54,  34 },	/* Green	*/
		{ 106, 202, 222 },	/* Magenta	*/
		{  81,  90, 240 },	/* Red		*/
		{  41, 240, 110 },	/* Blue		*/
	};
	unsigned char *y_plane  = buf;
	unsigned char *uv_plane = buf + (size_t)width * height;
	int bar_w = width / 7;
	int row, col;

	if (bar_w <= 0) bar_w = 1;

	/* Y plane — bar color + xorshift per-pixel noise (±15). Deterministic
	 * so the test is reproducible across runs. */
	for (row = 0; row < height; row++) {
		for (col = 0; col < width; col++) {
			int bar = col / bar_w;
			unsigned int h;
			int noise, v;

			if (bar > 6) bar = 6;
			h  = (unsigned int)(row * 12345u + col * 67890u);
			h ^= h << 13; h ^= h >> 17; h ^= h << 5;
			noise = (int)(h % 31) - 15;
			v = (int)bars[bar].y + noise;
			if (v < 0) v = 0; else if (v > 255) v = 255;
			y_plane[row * width + col] = (unsigned char)v;
		}
	}

	/* UV plane — bar color + smaller noise (±7), independent U / V streams.
	 * NV12: 4:2:0, U/V interleaved. */
	for (row = 0; row < height / 2; row++) {
		for (col = 0; col < width / 2; col++) {
			int bar = (col * 2) / bar_w;
			unsigned int hu, hv;
			int nu, nv, vu, vv;

			if (bar > 6) bar = 6;
			hu  = (unsigned int)(row * 23456u + col * 78901u);
			hu ^= hu << 13; hu ^= hu >> 17; hu ^= hu << 5;
			hv  = (unsigned int)(row * 34567u + col * 89012u);
			hv ^= hv << 13; hv ^= hv >> 17; hv ^= hv << 5;
			nu = (int)(hu % 15) - 7;
			nv = (int)(hv % 15) - 7;
			vu = (int)bars[bar].u + nu;
			vv = (int)bars[bar].v + nv;
			if (vu < 0) vu = 0; else if (vu > 255) vu = 255;
			if (vv < 0) vv = 0; else if (vv > 255) vv = 255;
			uv_plane[row * width + col * 2 + 0] = (unsigned char)vu;
			uv_plane[row * width + col * 2 + 1] = (unsigned char)vv;
		}
	}
}

void fill_npu_input_pattern(unsigned char *buf, int width, int height)
{
	static const struct { unsigned char r, g, b, z; } bars[7] = {
		{ 255, 255, 255, 0 },	/* White   */
		{ 255, 255,   0, 0 },	/* Yellow  */
		{   0, 255, 255, 0 },	/* Cyan    */
		{   0, 255,   0, 0 },	/* Green   */
		{ 255,   0, 255, 0 },	/* Magenta */
		{ 255,   0,   0, 0 },	/* Red     */
		{   0,   0, 255, 0 },	/* Blue    */
	};
	unsigned char *raw_data = buf;
	int bar_w = width / 7;
	int row, col;

	if (bar_w <= 0) bar_w = 1;

	for (row = 0; row < height; row++) {
		for (col = 0; col < width; col++) {
			int bar = col / bar_w;
			if (bar > 6) bar = 6;
			raw_data[(row * width + col) * 4 + 0] = bars[bar].r;
			raw_data[(row * width + col) * 4 + 1] = bars[bar].g;
			raw_data[(row * width + col) * 4 + 2] = bars[bar].b;
			raw_data[(row * width + col) * 4 + 3] = bars[bar].z;
		}
	}
}

/*
 * Compute PSNR (in dB) over the Y plane of two NV12 images of the same
 * dimensions. Returns +INFINITY if the images are bit-identical.
 *
 * Used by the Roundtrip test to quantify JpegEnc → JpegDec fidelity. We
 * compare only Y because chroma is sub-sampled (4:2:0) and the Y plane
 * dominates the perceived quality; this also keeps the comparison cheap.
 */
double compute_psnr_y(const unsigned char *a, const unsigned char *b,
									int width, int height)
{
	double mse = 0.0;
	long n = (long)width * height;
	long i;

	for (i = 0; i < n; i++) {
		int d = (int)a[i] - (int)b[i];
		mse += (double)(d * d);
	}
	if (mse == 0.0) return INFINITY;
	mse /= (double)n;
	return 10.0 * log10((255.0 * 255.0) / mse);
}

/* JPEG marker parsing — used to compute YUV output size before JpegDec alloc */
#define SOI		0xD8
#define SOF0	0xC0
#define SOF2	0xC2
#define EOI		0xD9

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

size_t file_get_jpeg_size(char *file_name,
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
			fgetc(fp); fgetc(fp);	/* marker length */
			fgetc(fp);				/* sample precision */
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
 * Stress / regression — loop helpers
 *
 * Each stress test pre-allocates its buffers once, then exercises only the
 * Execute path in a tight loop. The intent is to surface :
 *	1. accumulated leaks	— bufs_active should return to baseline at end
 *	2. perf regressions		— per-iter timing min/avg/p99/max
 *	3. data drift			— Roundtrip prints PSNR range across all iters
 *
 ******************************************************************************/


int stats_init(struct loop_stats *s, int n)
{
	memset(s, 0, sizeof(*s));
	s->planned = n;
	s->lat_ns  = (uint64_t *)calloc((size_t)n, sizeof(uint64_t));
	return s->lat_ns ? 0 : -1;
}

void stats_record(struct loop_stats *s, uint64_t ns, bool succ)
{
	if (succ) {
		if (s->idx < s->planned) s->lat_ns[s->idx++] = ns;
		s->ok++;
	} else {
		s->fail++;
	}
	s->total_ns += ns;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return (x < y) ? -1 : (x > y) ? 1 : 0;
}

void stats_print(struct loop_stats *s, const char *label)
{
	uint64_t mn, mx, av, p50, p99;
	double secs;

	if (s->ok == 0) {
		EPRINTF("  %s: all %d iters failed\n", label, s->fail);
		return;
	}
	qsort(s->lat_ns, (size_t)s->idx, sizeof(uint64_t), cmp_u64);
	mn  = s->lat_ns[0];
	mx  = s->lat_ns[s->idx - 1];
	av  = s->total_ns / (uint64_t)s->ok;
	p50 = s->lat_ns[s->idx / 2];
	p99 = s->lat_ns[(int)((double)s->idx * 0.99)];
	secs = (double)s->total_ns / 1.0e9;
	printf("  %-10s ok=%d fail=%d  per-iter μs: min=%.1f avg=%.1f p50=%.1f p99=%.1f max=%.1f\n",
		label, s->ok, s->fail,
		mn / 1.0e3, av / 1.0e3, p50 / 1.0e3, p99 / 1.0e3, mx / 1.0e3);
	printf("  %-10s total %.3f s  throughput %.1f ops/s\n",
		label, secs, (double)s->ok / secs);
}

void stats_free(struct loop_stats *s)
{
	free(s->lat_ns);
	s->lat_ns = NULL;
}

/* Read a single counter from /sys/kernel/debug/enx_vdma/<dev>/<node>. */
long read_dbg_counter(const char *dev_name, const char *node, const char *key)
{
	char path[128], line[256];
	FILE *fp;
	long val = -1;
	int lines = 0;

	snprintf(path, sizeof(path), "/sys/kernel/debug/enx_vdma/%s/%s", dev_name, node);
	fp = fopen(path, "r");
	if (!fp) {
		EPRINTF("read_dbg_counter: fopen('%s') failed: %s\n",
			path, strerror(errno));
		return -1;
	}
	while (fgets(line, sizeof(line), fp)) {
		char *p = strstr(line, key);
		lines++;
		if (p) {
			char *colon = strchr(p, ':');
			if (colon) { val = strtol(colon + 1, NULL, 10); break; }
		}
	}
	fclose(fp);
	if (val < 0)
		EPRINTF("read_dbg_counter: key '%s' not found in '%s' (%d lines)\n",
			key, path, lines);
	return val;
}

void leak_check(const char *dev_name, long baseline)
{
	long now = read_dbg_counter(dev_name, "stats", "bufs_active");
	if (baseline < 0 || now < 0) {
		printf("  baseline   debugfs not readable, skipping leak check\n");
		return;
	}
	if (now == baseline)
		HPRINTF("baseline   %s/bufs_active=%ld (stable)\n", dev_name, now);
	else
		EPRINTF("LEAK       %s/bufs_active: %ld -> %ld (delta=%+ld)\n",
			dev_name, baseline, now, now - baseline);
}

int ask_iters(int def)
{
	char buf[64];

	printf("iterations (default %d, Enter for default): ", def);
	sGetMenu(buf);
	if (!buf[0]) return def;
	{
		unsigned long n;
		if (parse_uint(buf, &n) || n < 1) {
			printf("  invalid input — using default %d\n", def);
			return def;
		}
		return (int)n;
	}
}

/*******************************************************************************
 *
 * Stress — module-agnostic helpers
 *
 * The next three stress functions (alloc/free churn, quota exhaustion,
 * Init/Exit cycle) all need to be dispatched onto one of the four modules.
 * Centralizing the module selector + per-module thunks here avoids open-
 * coding the same switch four times.
 *
 ******************************************************************************/

const char *vmod_label(enum vmod_id m)
{
	switch (m) {
	case VMOD_SCALER0:	return "scaler0";
	case VMOD_FONT:		return "font";
	case VMOD_JENC:		return "jenc";
	case VMOD_JDEC:		return "jdec";
	case VMOD_NPU:		return "npu";
	default:			return "?";
	}
}

const char *vmod_dbg(enum vmod_id m)
{
	switch (m) {
	case VMOD_SCALER0:	return "enx_vdma_dz0";
	case VMOD_FONT:		return "enx_vdma_font";
	case VMOD_JENC:		return "enx_vdma_jenc";
	case VMOD_JDEC:		return "enx_vdma_jdec";
	case VMOD_NPU:		return "enx_vdma_npu";
	default:			return "?";
	}
}

enum vmod_id ask_module(void)
{
	char cIn[16];
	printf("Module? (1: scaler0, 2: font, 3: jenc, 4: jdec, 5: npu) : ");
	sGetMenu(cIn);
	switch (atoi(cIn)) {
	case 1: return VMOD_SCALER0;
	case 2: return VMOD_FONT;
	case 3: return VMOD_JENC;
	case 4: return VMOD_JDEC;
	case 5: return VMOD_NPU;
	default: return VMOD_INVALID;
	}
}

/* Track caller's pre-existing init state so we can restore it on exit. */

bool vmod_is_inited(enum vmod_id m)
{
	switch (m) {
	case VMOD_SCALER0:	return g_state.scaler_inited[0];
	case VMOD_FONT:		return g_state.font_inited;
	case VMOD_JENC:		return g_state.jenc_inited;
	case VMOD_JDEC:		return g_state.jdec_inited;
	case VMOD_NPU:		return g_state.npu_inited;
	default:			return false;
	}
}

int vmod_auto_init(enum vmod_id m, struct vmod_init_state *s)
{
	s->was_inited = vmod_is_inited(m);
	if (s->was_inited) return 0;
	switch (m) {
	case VMOD_SCALER0:
		if (ENX_DMA_Scaler_Init(ENX_VDMA_SCALER0) != 0) return -1;
		g_state.scaler_inited[0] = true;
		HPRINTF("[auto] Scaler Init\n");
		return 0;
	case VMOD_FONT:
		if (ENX_DMA_Font_Init(ENX_VDMA_INIT_TYPE_KERNEL) != 0) return -1;
		g_state.font_inited = true;
		g_state.font_init_type = ENX_VDMA_INIT_TYPE_KERNEL;
		HPRINTF("[auto] Font Init\n");
		return 0;
	case VMOD_JENC:
		if (ENX_DMA_JpegEnc_Init() != 0) return -1;
		g_state.jenc_inited = true;
		HPRINTF("[auto] JpegEnc Init\n");
		return 0;
	case VMOD_JDEC:
		if (ENX_DMA_JpegDec_Init() != 0) return -1;
		g_state.jdec_inited = true;
		HPRINTF("[auto] JpegDec Init\n");
		return 0;
	case VMOD_NPU:
		if (ENX_DMA_Npu_Init() != 0) return -1;
		g_state.npu_inited = true;
		HPRINTF("[auto] NPU Init\n");
		return 0;
	default: return -1;
	}
}

void vmod_auto_exit(enum vmod_id m, struct vmod_init_state *s)
{
	if (s->was_inited) return;	/* leave user state alone */
	switch (m) {
	case VMOD_SCALER0:
		if (g_state.scaler_inited[0]) {
			ENX_DMA_Scaler_Exit(ENX_VDMA_SCALER0);
			g_state.scaler_inited[0] = false;
			HPRINTF("[auto] Scaler Exit\n");
		}
		break;
	case VMOD_FONT:
		if (g_state.font_inited) {
			ENX_DMA_Font_Stop_All();
			/* CfgFree drains ALL cfgs the lib still holds — must be
			 * single-threaded. We do it here so concurrent workers can
			 * each CfgAlloc without ever having to free; this teardown
			 * frees the lot at once. */
			ENX_DMA_Font_CfgFree();
			ENX_DMA_Font_Exit();
			g_state.font_inited = false;
			HPRINTF("[auto] Font Exit (Stop_All + CfgFree + Exit)\n");
		}
		break;
	case VMOD_JENC:
		if (g_state.jenc_inited) {
			ENX_DMA_JpegEnc_Exit();
			g_state.jenc_inited = false;
			HPRINTF("[auto] JpegEnc Exit\n");
		}
		break;
	case VMOD_JDEC:
		if (g_state.jdec_inited) {
			ENX_DMA_JpegDec_Exit();
			g_state.jdec_inited = false;
			HPRINTF("[auto] JpegDec Exit\n");
		}
		break;
	case VMOD_NPU:
		if (g_state.npu_inited) {
			ENX_DMA_Npu_Exit();
			g_state.npu_inited = false;
			HPRINTF("[auto] NPU Exit\n");
		}
		break;
	default: break;
	}
}

void *vmod_alloc(enum vmod_id m, int kind, size_t size)
{
	switch (m) {
	case VMOD_SCALER0:	return ENX_DMA_Scaler_Buffer_Alloc(ENX_VDMA_SCALER0, kind, size);
	case VMOD_FONT:		return ENX_DMA_Font_Buffer_Alloc(kind, size);
	case VMOD_JENC:		return ENX_DMA_JpegEnc_Buffer_Alloc(kind, size);
	case VMOD_JDEC:		return ENX_DMA_JpegDec_Buffer_Alloc(kind, size);
	case VMOD_NPU:		return ENX_DMA_Npu_Buffer_Alloc(kind, size);
	default:			return NULL;
	}
}

void vmod_free(enum vmod_id m, void *buf, size_t size)
{
	if (!buf) return;
	switch (m) {
	case VMOD_SCALER0:	ENX_DMA_Scaler_Buffer_Free(ENX_VDMA_SCALER0, buf, size); break;
	case VMOD_FONT:		ENX_DMA_Font_Buffer_Free(buf, size); break;
	case VMOD_JENC:		ENX_DMA_JpegEnc_Buffer_Free(buf, size); break;
	case VMOD_JDEC:		ENX_DMA_JpegDec_Buffer_Free(buf, size); break;
	case VMOD_NPU:		ENX_DMA_Npu_Buffer_Free(buf, size); break;
	default: break;
	}
}

int ask_thread_count(int def)
{
	char buf[32];
	unsigned long n;

	printf("threads (default %d, Enter for default) : ", def);
	sGetMenu(buf);
	if (!buf[0]) return def;
	if (parse_uint(buf, &n) || n < 1 || n > 32) {
		printf("  invalid — using default %d\n", def);
		return def;
	}
	return (int)n;
}