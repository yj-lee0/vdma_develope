/*
 * Copyright (C) 2025 Eyenix Corporation
 * Eyenix <support6@eyenix.com>
 */


#ifndef __DEMO_VDMA_H__
#define __DEMO_VDMA_H__

#include <stdio.h>
#include <linux/types.h>

#include "../../enx_vdma.h"

#define ENX_SUCCESS				0
#define ENX_FAIL				1

static const char * const gVdmaMenu[] = {
	"========== [VDMA] ==========",
	"1. VDMA Scaler Control",
	"2. VDMA Font Control",
	"3. VDMA JPEG-ENC Control",
	"4. VDMA JPEG-DEC Control",
	"5. VDMA NPU Control",
	"6. Roundtrip (JpegEnc -> JpegDec + PSNR)",
	"7. Stress Test (loop / regression)",
	"8. Pipeline (cross-device buffer share)",
	"0. Exit",
	"============================",
};

static const char * const gVdmaStressMenu[] = {
	"======= [VDMA Stress] =======",
	"1. Roundtrip loop (JpegEnc -> JpegDec)",
	"2. JpegEnc loop",
	"3. JpegDec loop",
	"4. Scaler loop",
	"5. Font loop",
	"6. NPU loop",
	"7. Alloc/Free churn  (lifecycle, no Execute)",
	"8. Quota exhaustion",
	"9. Init/Exit cycle",
	"10. Concurrent (pthread, race verification)",
	"0. Exit",
	"=============================",
};

static const char * const gVdmaPipelineMenu[] = {
    "===== [VDMA Pipeline] =====",
    "1. Roundtrip via Connect",
    "2. Font  -> JpegEnc",
    "3. Scaler -> JpegEnc",
    "4. JpegDec -> Scaler",
    "5. Export_Fd / Import_Fd (same-process sanity)",
    "6. Fork + SCM_RIGHTS (cross-process)",
    "0. Exit",
    "===========================",
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

static const char * const gVdmaNpuMenu[] = {
	"====== [VDMA NPU] =======",
	"1. NPU Init",
	"2. NPU Exec",
	"0. Exit",
	"============================",
};

#endif /* __DEMO_VDMA_H__ */