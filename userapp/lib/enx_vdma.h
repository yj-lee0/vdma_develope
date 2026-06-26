/*
 * Copyright (C) 2026 Eyenix Corporation All rights reserved.
 * Eyenix <support6@eyenix.com>
 *
 * enx_vdma - API headers for user-space applications
 */
#ifndef __ENX_VDMA_H__
#define __ENX_VDMA_H__

#ifdef __cplusplus
extern "C"{
#endif /* __cplusplus */

#define C_RESET  "\x1b[0m"
#define C_GREEN  "\x1b[32m"
#define C_RED    "\x1b[31m"
#define C_YELL   "\x1b[33m"
#define C_CYAN   "\x1b[36m"
#define C_DIM    "\x1b[2m"

#define MAX_VISP_YC_NUM 12

#define EPRINTF(...) do { fprintf(stderr, "%s[E]%s ", C_RED, C_RESET); fprintf(stderr, __VA_ARGS__); } while (0)
#define HPRINTF(...) do { printf("%s[+]%s ", C_GREEN, C_RESET); printf(__VA_ARGS__); } while (0)
#define MPRINTF(...) do { printf("%s[*]%s ", C_CYAN, C_RESET); printf(__VA_ARGS__); } while (0)

#include <stdint.h>
#include <stddef.h>

typedef __u32 u32;
typedef __u64 u64;
typedef __s32 s32;
typedef __u16 u16;
typedef __u8  u8;

typedef int8_t Int8;
typedef uint8_t UInt8;
typedef int16_t Int16;
typedef uint16_t UInt16;
typedef int32_t Int32;
typedef uint32_t UInt32;
typedef int64_t Int64;
typedef uint64_t UInt64;

typedef enum {
	ENX_VDMA_SCALER0 = 0,
	ENX_VDMA_SCALER1,
	ENX_VDMA_ROTATION,
	ENX_VDMA_FONT,
	ENX_VDMA_JPEG_ENC,
	ENX_VDMA_JPEG_DEC,
	ENX_VDMA_NPU,
	ENX_VDMA_FUNC_END
} ENX_VDMA_FUNC_TYPE_E;

typedef enum {
	ENX_VDMA_INIT_TYPE_NONE,
	ENX_VDMA_INIT_TYPE_KERNEL,
	ENX_VDMA_INIT_TYPE_VIDEO_CORE,
} ENX_VDMA_INIT_TYPE_E;

typedef enum {
	eFmt_RGB,
	eFmt_RBG,
	eFmt_GRB,
	eFmt_GBR,
	eFmt_BRG,
	eFmt_BGR,

	eFmt_RRR,
	eFmt_GGG,
	eFmt_BBB,
	eFmt_Rzz,
	eFmt_zGz,
	eFmt_zzB,

	eFmt_YUV444,
	eFmt_YUV444_YVU,
	eFmt_YUV444_UVY,
	eFmt_YUV444_UYV,
	eFmt_YUV444_VUY,
	eFmt_YUV444_VYU,
	eFmt_YUV444_Y,

	eFmt_YUV422_YUYV,
	eFmt_YUV422_YVYU,
	eFmt_YUV422_UYVY,
	eFmt_YUV422_VYUY,
	eFmt_YUV422_Y,

	eFmt_YUV420SP_NV12,
	eFmt_YVU420SP_NV21,
	eFmt_YUV420SP_NV_Y,

	eFmt_YUV400,
} ENX_VDMA_FORMAT_E;

/* Address flag type — Types of addr flags to use in hw_run */
typedef enum {
	ENX_VDMA_ADDR_TYPE_ID = 0,
	ENX_VDMA_ADDR_TYPE_PHYS,
	ENX_VDMA_ADDR_TYPE_VIRT,
} ENX_VDMA_ADDR_TYPE_E;

/* Buffer kind — alloc type */
typedef enum {
	VDMA_KIND_SRC = 1,
	VDMA_KIND_DST = 2,
} ENX_VDMA_BUF_KIND_E;

/*****************************************************************************
 *  [NOTE] src/dst_data_offset : byte offset into the allocated buffer
 *
 *  Each device's config struct may expose optional src_data_offset and/or
 *  dst_data_offset fields. They specify, in bytes, where the linear data
 *  region for this operation begins inside the underlying buffer.
 *
 *  Format-agnostic — the same field works for NV12, YUV444, RGB, JPEG
 *  bitstream, NPU tensors, etc. The driver treats it strictly as a byte
 *  offset added to the buffer base address before HW programming.
 *
 *  Default 0 keeps the legacy "use whole buffer from start" behavior, so
 *  existing callers and devices that don't expose the field are unaffected.
 *  Setting a non-zero value selects a sub-region of the backing.
 *
 *  Orthogonal to spatial positioning:
 *	data_offset    — WHICH frame / region (byte-level, format-agnostic)
 *	Font xpos/ypos — WHERE in the frame the overlay lands (pixel-level)
 *	Scaler src_width_pos / src_height_pos — crop ROI within the frame
 *  These remain separate concepts; mixing them in one field would force the
 *  user to track units (byte vs pixel) at every call site.
 *
 *  Primary use case — ring buffer of N frames in a single allocation:
 *	src_data_offset = frame_index * frame_size;
 *  Other uses: plane offsets within a packed buffer, sub-pool allocation
 *  from a shared backing, in-place stitching of multiple outputs into one
 *  destination buffer.
 *
 *  Validation split:
 *	- Bounds (offset + op_size <= buf_size) enforced by the kernel core
 *	  uniformly for every device — security-critical, never skipped.
 *	- Alignment (Scaler 16B, JpegEnc MCU boundary, NPU stride, …) is
 *	  enforced by the per-device driver according to its HW spec.
*******************************************************************************/

/* DMA-Scaler config data */
typedef struct {
	/* Depending on the flag, the src/dst_addr values ​​are determined to be addresses or offsets. */
	UInt32 src_addr_flag;	// 0: offset(src alloc id), 1: address(src phys addr), 2: address(src virt addr)
	UInt32 dst_addr_flag;	// 0: offset(dst alloc id), 1: address(dst phys addr), 2: address(dst virt addr)

	UInt64 src_addr;
	UInt64 dst_addr;

	/* data_offset */
	UInt32 src_data_offset;	// byte offset into the source buffer (default 0)
	UInt32 dst_data_offset;	// byte offset into the destination buffer (default 0)

	/* source info */
	UInt16 src_width;
	UInt16 src_width_total;
	UInt16 src_height;
	UInt16 src_height_total;
	ENX_VDMA_FORMAT_E src_format;

	/* source crop info */
	UInt16 src_width_pos;
	UInt16 src_height_pos;

	/* destination info */
	UInt16 dst_width;
	UInt16 dst_width_total;
	UInt16 dst_height;
	ENX_VDMA_FORMAT_E dst_format;

	/* flip */
	UInt8 vflip;
	UInt8 hflip;

	/* cache flush */
	UInt32 src_cache_flush;	// source cache flush
	UInt32 dst_cache_flush;	// destination cache flush
} VDMA_DZ_CONFIG_S;

/* DMA-FONT control info */
typedef struct {
	/* Depending on the flag, the src_addr values ​​are determined to be addresses or offsets. */
	UInt32 src_addr_flag;	// 0: offset(src alloc id), 1: address(phys addr), 2: address(virtual addr)

	/* font img start addr */
	UInt64 src_addr;
	UInt32 src_data_offset;	// byte offset into the source buffer (default 0)

	/* font img width / height (only even) */
	UInt16 src_width;
	UInt16 src_height;

	/* font img start position */
	UInt16 font_xpos;
	UInt16 font_ypos;

	/* font dst YC address */
	UInt64 dst_addr_y;
	UInt64 dst_addr_c;
	UInt32 reserved;

	/* font img color(YCbCr) */
	UInt8 font_color_y;
	UInt8 font_color_cb;
	UInt8 font_color_cr;

	/* It is recognized as the font value of font img based on the set value (Y). */
	UInt8 font_value;

	/* font img outline color(YCbCr) */
	UInt8 outline_color_y;
	UInt8 outline_color_cb;
	UInt8 outline_color_cr;

	/* It is recognized as the outline value of font img based on the set value (Y). */
	UInt8 outline_value;

	/* font img background color(YCbCr) */
	UInt8 bg_color_y;
	UInt8 bg_color_cb;
	UInt8 bg_color_cr;

	/*
	 * font write mode :
	 * If you select Wonly mode, it will be applied with the set background color without blending.
	 */
	UInt8 font_wr_mode; // 0: YC(RW), 1: Y(RW), 2: YC(Wonly), 3: Y(Wonly)

	/*
	 * font color tone :
	 * Determines whether to use the color of font img as the value of font_color
	 */
	UInt8 font_color_tone; // 0: bitmap color, 1: font_color

	/* font alpha blending*/
	UInt16 font_alpha; // 0~256 (0: transparent, 256: opaque)

	/*
	 * font threshold :
	 * Recognizes values ​​greater than the threshold as font img based on the threshold value
	 */
	UInt8 font_threshold;
} VDMA_FONT_INFO_S;

/* DMA-FONT config data */
typedef struct {
	/*
	 * The init_type determines the destination type to which the font will be applied.
	 * - kernel     : Memory      -> 0 : id, 1 : physical address, 2: virtual address
	 * - videocore  : YC channel index.
	 */
	UInt32 dst_type_index;
	UInt32 dst_data_offset;		// byte offset into the destination buffer (default 0)

	/* Used only when init_type is kernel. */
	UInt16 dst_width;			// Horizontal line offset
	UInt16 dst_height;			// calcul c addr
	UInt32 background_overlay;	// 0: no overlay(dst_buf memset 0), 1: Overlay on background image

	UInt32 font_number;
	VDMA_FONT_INFO_S font_info[]; 		// flexible array -> [font_number]
} VDMA_FONT_CONFIG_S;

/* DMA-JPEG Encoder config data */
typedef struct {
	/* Depending on the flag, the src/dst_addr values ​​are determined to be addresses or offsets. */
	UInt32 src_addr_flag;	// 0: offset(src alloc id), 1: address(src phys addr), 2: address(dst virt addr)
	UInt32 dst_addr_flag;	// 0: offset(dst alloc id), 1: address(dst phys addr), 2: address(dst virt addr)

	UInt64 src_addr;
	UInt64 dst_addr;

	/* data_offset */
	UInt32 src_data_offset;	// byte offset into the source buffer (default 0)
	UInt32 dst_data_offset;	// byte offset into the destination buffer (default 0)

	/* source info */
	UInt16 src_width_total;
	UInt16 src_height_total;
	ENX_VDMA_FORMAT_E src_format;

	/* crop info */
	UInt16 src_width;
	UInt16 src_height;
	UInt16 src_width_pos;
	UInt16 src_height_pos;

	/* jpeg option */
	UInt32 ovf_threshold;		// JPEG size overflow threshold
	UInt32 quality;			// JPEG quality (0~100)

	/* cache flush */
	UInt32 src_cache_flush;	// source cache flush
	UInt32 dst_cache_flush;	// destination cache flush
} VDMA_JPEG_ENC_CONFIG_S;

/* DMA-JPEG Decoder config data */
typedef struct {
	/* Depending on the flag, the src/dst_addr values ​​are determined to be addresses or offsets. */
	UInt32 src_addr_flag;	// 0: offset(src alloc id), 1: address(src phys addr), 2: address(dst virt addr)
	UInt32 dst_addr_flag;	// 0: offset(dst alloc id), 1: address(dst phys addr), 2: address(dst virt addr)

	UInt64 src_addr;
	UInt64 dst_addr;

	/* data_offset */
	UInt32 src_data_offset;	// byte offset into the source buffer (default 0)
	UInt32 dst_data_offset;	// byte offset into the destination buffer (default 0)

	/* jpeg img size */
	UInt32 src_jpeg_size;

	/*
	 * dst_buf_size — destination buffer capacity in bytes. REQUIRED.
	 *
	 * While not strictly required by the HW logic itself, always provide
	 * the pre-calculated size here. This prevents unexpected malfunctions
	 * that can occur if the generated output exceeds the allocated
	 * destination buffer.
	 */
	UInt32 dst_buf_size;

	/* cache flush */
	UInt32 src_cache_flush;	// source cache flush
	UInt32 dst_cache_flush;	// destination cache flush
} VDMA_JPEG_DEC_CONFIG_S;

/* DMA-NPU config data */
typedef struct {
	/* Depending on the flag, the src/dst_addr values ​​are determined to be addresses or offsets. */
	UInt32 src_addr_flag;	// 0: offset(src alloc index), 1: address(src phys addr)
	UInt32 dst_addr_flag;	// 0: offset(dst alloc index), 1: address(dst phys addr)

	/* phys_addr */
	UInt64 src_addr;
	UInt64 dst_addr;

	/* data_offset */
	UInt32 src_data_offset;	// byte offset into the source buffer (default 0)
	UInt32 dst_data_offset;	// byte offset into the destination buffer (default 0)

	/* source info */
	UInt16 src_width; // must be 32-byte aligned
	UInt16 src_height;

	/* Specifies the alignment of the NPU Operation Results. */
	UInt8 read_mode; // 0: 4Byte, 1: 8Byte, 2: 16Byte, 3: 32Byte

	/*
	 * Setting Data Scale When Reading NPU Operation Results.
	 * 0 : Bypass
	 * 1 : x2
	 * 2 : unsigned mode (Invert the 7th bit value of the NPU operation result and use it)
	 */
	UInt8 rgb_scale;

	UInt8 color_invert; // 0: normal, 1: invert

	/* cache flush */
	UInt32 src_cache_flush;	// source cache flush
	UInt32 dst_cache_flush;	// destination cache flush
} VDMA_NPU_CONFIG_S;

/* DMA-Scaler */
Int32 ENX_DMA_Scaler_Init(Int32 ch);
void ENX_DMA_Scaler_Exit(Int32 ch);
void* ENX_DMA_Scaler_Buffer_Alloc(Int32 ch, ENX_VDMA_BUF_KIND_E kind, size_t src_size);
void* ENX_DMA_Scaler_Buffer_Alloc_EX(Int32 ch, ENX_VDMA_BUF_KIND_E kind, size_t src_size, UInt32 *buf_id_out);
Int32 ENX_DMA_Scaler_Buffer_Free(Int32 ch, void *pBuf, size_t buf_size);
Int32 ENX_DMA_Scaler_Execute(Int32 ch, VDMA_DZ_CONFIG_S *gVdmaDzCfg);

/* DMA-Font */
Int32 ENX_DMA_Font_Init(ENX_VDMA_INIT_TYPE_E init_type);
void ENX_DMA_Font_Exit(void);
void* ENX_DMA_Font_Buffer_Alloc(ENX_VDMA_BUF_KIND_E kind, size_t src_size);
void* ENX_DMA_Font_Buffer_Alloc_EX(ENX_VDMA_BUF_KIND_E kind, size_t src_size, UInt32 *buf_id_out);
Int32 ENX_DMA_Font_Buffer_Free(void *pBuf, size_t buf_size);
VDMA_FONT_CONFIG_S* ENX_DMA_Font_CfgAlloc(UInt32 blit_count, UInt32 yc_index);
VDMA_FONT_CONFIG_S* ENX_DMA_Font_CfgRealloc(VDMA_FONT_CONFIG_S* VdmaFontConfig, UInt32 blit_count, UInt32 yc_index);
void ENX_DMA_Font_CfgFree(void);
Int32 ENX_DMA_Font_Update(VDMA_FONT_CONFIG_S* gVdmaFontConfig);
Int32 ENX_DMA_Font_Stop(UInt32 yc_num);
Int32 ENX_DMA_Font_Stop_All(void);

/* DMA-JPEG ENC */
Int32 ENX_DMA_JpegEnc_Init();
void ENX_DMA_JpegEnc_Exit(void);
void* ENX_DMA_JpegEnc_Buffer_Alloc(ENX_VDMA_BUF_KIND_E kind, size_t size);
void* ENX_DMA_JpegEnc_Buffer_Alloc_EX(ENX_VDMA_BUF_KIND_E kind, size_t size, UInt32 *buf_id_out);
Int32 ENX_DMA_JpegEnc_Buffer_Free(void *pBuf, size_t buf_size);
Int32 ENX_DMA_JpegEnc_Execute(VDMA_JPEG_ENC_CONFIG_S* gVdmaJpegEncCfg, size_t *jpeg_size_out);
void ENX_DMA_JpegEnc_Discard(void);

/* DMA-JPEG DEC */
Int32 ENX_DMA_JpegDec_Init();
void ENX_DMA_JpegDec_Exit(void);
void* ENX_DMA_JpegDec_Buffer_Alloc(ENX_VDMA_BUF_KIND_E kind, size_t size);
void* ENX_DMA_JpegDec_Buffer_Alloc_EX(ENX_VDMA_BUF_KIND_E kind, size_t size, UInt32 *buf_id_out);
Int32 ENX_DMA_JpegDec_Buffer_Free(void *pBuf, size_t buf_size);
Int32 ENX_DMA_JpegDec_Execute(VDMA_JPEG_DEC_CONFIG_S* gVdmaJpegDecCfg, size_t *yuv_size_out);
void ENX_DMA_JpegDec_Discard(void);

/* DMA-NPU */
Int32 ENX_DMA_Npu_Init();
void ENX_DMA_Npu_Exit(void);
void* ENX_DMA_Npu_Buffer_Alloc(ENX_VDMA_BUF_KIND_E kind, size_t size);
void* ENX_DMA_Npu_Buffer_Alloc_EX(ENX_VDMA_BUF_KIND_E kind, size_t size, UInt32 *buf_id_out);
Int32 ENX_DMA_Npu_Buffer_Free(void *pBuf, size_t buf_size);
Int32 ENX_DMA_Npu_Execute(VDMA_NPU_CONFIG_S* gVdmaNpuCfg);

/*****************************************************************************
 * Cross-device buffer sharing
 *
 * Two complementary APIs:
 *  - Buffer_Connect()        : single-process one-shot. Caller has a buf
 *                              owned by src_func; gets back a new buf
 *                              owned by dst_func that shares the same
 *                              backing. No fd plumbing.
 *
 *  - Buffer_Export_Fd()      : provider side, cross-process. Returns an
 *  + Buffer_Import_Fd()        anon_inode fd suitable for SCM_RIGHTS over
 *                              a UDS. The consumer process calls
 *                              Buffer_Import_Fd() with the received fd.
 *
 * In all cases the returned buf is freed with the regular ENX_DMA_*_Buffer_Free();
 * no special "disconnect" / "release" is needed. The returned fd from
 * Export_Fd() is caller-owned and must be close()'d by the caller.
 *
 * NOTE on synchronization:
 *	ENX_DMA_Buffer_Connect/Export/Import provide memory sharing only.
 *	Temporal synchronization between provider and consumer Execute is
 *	the caller's responsibility. Within a single thread, calling them
 *	sequentially is sufficient — Execute is synchronous and returns
 *	only after HW completes.
 *
 *	For concurrent producer-consumer flows, use application-level
 *	primitives (eventfd, pthread cond, UDS messages, etc.) to enforce
 *	the ordering between Execute calls.
 *****************************************************************************/

/*
 * Same-process — connect src_buf (owned by src_func) into dst_func as a
 * new buf of @kind. The src_buf remains valid; the returned dst_buf
 * shares the same physical backing.
 */
Int32 ENX_DMA_Buffer_Connect(ENX_VDMA_FUNC_TYPE_E src_func, void * pSrc_buf,
	ENX_VDMA_FUNC_TYPE_E dst_func, ENX_VDMA_BUF_KIND_E kind, void **pDst_buf_out);

/*
 * Cross-process — publish @buf (owned by @func) as an anon_inode fd.
 * The caller owns @fd_out and must close() it when done.
 */
Int32 ENX_DMA_Buffer_Export_Fd(ENX_VDMA_FUNC_TYPE_E func, void *pBuf, int *fd_out);

/* Cross-process — consume an exported fd into @func as a new buf of @kind. */
Int32 ENX_DMA_Buffer_Import_Fd(ENX_VDMA_FUNC_TYPE_E func, ENX_VDMA_BUF_KIND_E kind, int fd, void **pBuf_out);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __ENX_VDMA_H__ */
