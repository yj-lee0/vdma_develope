/*
 * Copyright (C) 2026 Eyenix Corporation
 * Eyenix <support6@eyenix.com>
 *
 * VDMA demo — shared types, macros, globals and helper prototypes.
 *
 * This header is the internal API surface used across the split demo_vdma_*
 * translation units (util / main+exec / roundtrip / stress / pipeline).
 * External callers should still include only demo_vdma.h.
 */

#ifndef __DEMO_VDMA_UTIL_H__
#define __DEMO_VDMA_UTIL_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

#include "../../../enx_vdma.h"

/*******************************************************************************
 *
 * Shared types
 *
 ******************************************************************************/

typedef void *vdma_addr_t;

struct buffer_alloc_track {
	unsigned char *buf;
	size_t size;
};

/*
 * Tracks which modules the user has Init'ed so Exec / Update can give a
 * friendly "call Init first" instead of letting the lib return a bare errno.
 * font_init_type also remembers KERNEL vs VIDEO_CORE.
 */
struct demo_g_state {
	bool scaler_inited[2];	/* SCALER0, SCALER1 */
	bool font_inited;
	int font_init_type;	/* ENX_VDMA_INIT_TYPE_* */
	bool jenc_inited;
	bool jdec_inited;
	bool npu_inited;
};

extern struct demo_g_state g_state;

/* Loop stats — used by stress / concurrent tests. */
struct loop_stats {
	int planned;
	int ok;
	int fail;
	uint64_t *lat_ns;
	int idx;
	uint64_t total_ns;
};

/* Module dispatcher — used by the generic stress / pipeline helpers. */
enum vmod_id {
	VMOD_SCALER0 = 0,
	VMOD_FONT,
	VMOD_JENC,
	VMOD_JDEC,
	VMOD_NPU,
	VMOD_INVALID,
};

struct vmod_init_state {
	bool was_inited;
};

/* Shared macros — printed-error / required-init guards used in every Exec */
#define REQUIRE_INIT(flag, name) do {									\
		if (!(flag)) {													\
			EPRINTF("'%s' Init has not been called yet.\n", (name));	\
			return -1;													\
		}																\
	} while (0)

#define GET_UINT_OR_ERR(label, field) do {								\
		unsigned long _v;												\
		sGetMenu(cIn);													\
		if (parse_uint(cIn, &_v)) {										\
			EPRINTF("%s: '%s' is not a valid number\n", (label), cIn);	\
			goto err;													\
		}																\
		(field) = _v;													\
	} while (0)


/*******************************************************************************
 *
 * Input / time / debugfs
 *
 ******************************************************************************/

void printMenu(const char * const pMenuList[], int nMenuCnt, int nIdx);
void sGetMenu(char *buf);
int parse_uint(const char *s, unsigned long *out);
int ask_iters(int def);
int ask_thread_count(int def);
uint64_t now_ns(void);

void show_dev_stats(const char *dev_name);
long read_dbg_counter(const char *dev_name, const char *node, const char *key);
void leak_check(const char *dev_name, long baseline);


/*******************************************************************************
 *
 * File I/O
 *
 ******************************************************************************/

size_t get_file_size(char *file_name);
int file_read(char *file_name, unsigned char *buffer, size_t file_size);
int file_write(char *file_name, unsigned char *buffer, size_t file_size);
void file_newname(char *filename);

/*******************************************************************************
 *
 * Format helpers / pattern generation / PSNR
 *
 ******************************************************************************/

extern const char *fmt_name[];

size_t calculate_format_size(int format, int width, int height);
size_t file_get_jpeg_size(char *file_name, unsigned short *width,
					unsigned short *height, ENX_VDMA_FORMAT_E *fmt);

void fill_nv12_pattern(unsigned char *buf, int width, int height);
void fill_npu_input_pattern(unsigned char *buf, int width, int height);
double compute_psnr_y(const unsigned char *a, const unsigned char *b,
					int width, int height);

/*******************************************************************************
 *
 * Loop stats
 *
 ******************************************************************************/

int  stats_init(struct loop_stats *s, int n);
void stats_record(struct loop_stats *s, uint64_t ns, bool succ);
void stats_print(struct loop_stats *s, const char *label);
void stats_free(struct loop_stats *s);

/*******************************************************************************
 *
 * Module dispatcher (vmod_*)
 *
 ******************************************************************************/

const char *vmod_label(enum vmod_id m);
const char *vmod_dbg(enum vmod_id m);
enum vmod_id ask_module(void);
bool vmod_is_inited(enum vmod_id m);
int vmod_auto_init(enum vmod_id m, struct vmod_init_state *s);
void vmod_auto_exit(enum vmod_id m, struct vmod_init_state *s);
void *vmod_alloc(enum vmod_id m, int kind, size_t size);
void vmod_free(enum vmod_id m, void *buf, size_t size);

/*******************************************************************************
 *
 * Cross-TU DEMO entry points (called from the main loop dispatcher)
 *
 ******************************************************************************/

int DEMO_RoundtripExec(void);

int DEMO_StressRoundtrip(void);
int DEMO_StressJpegEnc(void);
int DEMO_StressJpegDec(void);
int DEMO_StressScaler(void);
int DEMO_StressFont(void);
int DEMO_StressNpu(void);
int DEMO_StressAllocFree(void);
int DEMO_StressQuota(void);
int DEMO_StressInitExit(void);
int DEMO_StressConcurrent(void);

int DEMO_PipelineRoundtripConnect(void);
int DEMO_PipelineFontJenc(void);
int DEMO_PipelineScalerJenc(void);
int DEMO_PipelineJdecScaler(void);
int DEMO_PipelineFdSanity(void);
int DEMO_PipelineForkScm(void);

#endif /* __DEMO_VDMA_UTIL_H__ */
