/*
 * Copyright (C) 2026 Eyenix Corporation
 * Eyenix <support6@eyenix.com>
 *
 * Interactive demo for libenx_vdma.a — Scaler / Font / JPEG ENC / JPEG DEC
 * User I/F is modeled after demo_vdma.c.
 */

#ifndef __DEMO_VDMA_H__
#define __DEMO_VDMA_H__

static const char * const gVdmaMenu[] = {
	"========== [VDMA] ==========",
	"1. VDMA Init",
	"2. VDMA Scaler Ccontrol",
	"3. VDMA Font Ccontrol",
	"4. VDMA JPEG-ENC Ccontrol",
	"5. VDMA JPEG-DEC Ccontrol",
	"0. Exit",
	"============================",
};

static const char * const gVdmaInitMenu[] = {
	"======= [INIT TYPE] ========",
	"1. kernel",
	"2. video-core",
	"============================",
};

static const char * const gVdmaExecMenu[] = {
	"====== [VDMA Update] =======",
	"1. Example setting",
	"2. Custom setting",
	"(Any other key to exit)",
	"============================",
};

static const char * const gVdmaDzMenu[] = {
	"====== [VDMA Scaler] =======",
	"1. Scaler Init",
	"2. Scaler Exec",
	"0. Exit",
	"============================",
};

static const char * const gVdmaFontMenu[] = {
	"======= [VDMA Font] ========",
	"1. Font Init",
	"2. Font Update",
	"3. Font Stop",
	"0. Exit",
	"============================",
};

static const char * const gVdmaJencMenu[] = {
	"===== [VDMA Jpeg enc] ======",
	"1. Jpeg enc Init",
	"2. Jpeg enc Exec",
	"0. Exit",
	"============================",
};

static const char * const gVdmaJdecMenu[] = {
	"===== [VDMA Jpeg dec] ======",
	"1. Jpeg dec Init",
	"2. Jpeg dec Exec",
	"0. Exit",
	"============================",
};

#endif /* __DEMO_VDMA_H__ */
