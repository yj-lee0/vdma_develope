/*
 * Eyenix, DMA-Font test application
 *
 * enx_vdma_font - DMA-Font test app
 *
 * Pipeline:
 *   1. Take a text string from the command line.
 *   2. Render it into a Y-plane (grayscale) glyph bitmap using the FreeType library.
 *   3. Store that bitmap into a SRC buffer allocated by ENX_DMA_Font_Buffer_Alloc().
 *   4. Use the DMA-Font HW (kernel mode) to composite the glyph onto a
 *      DST_W x DST_H NV12 (YUV420SP) destination buffer.
 *   5. Save the resulting DST_W x DST_H NV12 image to a file.
 *
 * Usage:
 *   ./enx_vdma_font -f <font.ttf> -i <text> [-o out.nv12] [-fs size]
 *                   [-ow outline] [-ls spacing] [-xy x y]
 */

#include <linux/types.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_STROKER_H
#include FT_OUTLINE_H

#include "../../enx_vdma.h"

/*******************************************************************************
 *
 * Destination (NV12) geometry — fixed DST_W x DST_H per the test requirement.
 *
*******************************************************************************/
#define DST_W			320
#define DST_H			240
#define DST_SIZE		((DST_W) * (DST_H) * 3 / 2)	/* NV12: Y + UV/2 */

/*******************************************************************************
 * FreeType glyph-bitmap generation parameters (mirror of font_gen.c).
 *
 * The SRC bitmap is "binary": every glyph pixel is written as FONT_VALUE or
 * OUTLINE_VALUE, never an anti-aliased gradient. The DMA-Font HW then matches
 * those exact Y values to pick the font / outline color.
*******************************************************************************/
#define LETTER_SPACE	0
#define FONT_VALUE		255		/* SRC value that marks the font interior  */
#define OUTLINE_VALUE	128		/* SRC value that marks the outline        */
#define FONT_SIZE		32
#define OUTLINE_WIDTH	4
#define MIN_FONT_SIZE	8		/* auto-shrink floor when text won't fit   */

/*
 * Binarization (coverage) threshold.
 *
 * FreeType renders anti-aliased glyphs: edge pixels carry partial coverage
 * (1..254). Since the DMA-Font HW only understands the discrete FONT_VALUE /
 * OUTLINE_VALUE levels, we must binarize. Thresholding at >0 (the old behavior)
 * turned every faintly-touched edge pixel solid, fattening each stroke by ~1px
 * all around — which fills in small counters (the holes in e/a/o) and merges
 * neighbouring strokes at small sizes ("뭉개짐"). Keeping only pixels at >=50%
 * coverage tracks the true glyph contour, so small text stays separated.
 * Lower it (e.g. -th 96) if thin stems start to break up.
 */
#define COVERAGE_THRESHOLD	128

/* Outline width auto-scales with font size unless the user passes -ow.
 * A fixed 4px outline is huge at small sizes and is the main cause of adjacent
 * glyphs merging, so default to ~size/16 (min 1). */
#define OUTLINE_RATIO	16

/* DMA-Font color / blend defaults (YCbCr) */
#define FONT_COLOR_Y		0xEB	/* white  */
#define FONT_COLOR_CB		0x80
#define FONT_COLOR_CR		0x80
#define OUTLINE_COLOR_Y		0x10	/* black  */
#define OUTLINE_COLOR_CB	0x80
#define OUTLINE_COLOR_CR	0x80
/* Light pastel-blue background so the dark outline stands out clearly.
 * (YCbCr ~ RGB(180,200,230)) */
#define BG_Y				0xC5	/* light luma   */
#define BG_CB				0x92	/* slight blue tint */
#define BG_CR				0x74
#define FONT_ALPHA			256		/* 0~256 (opaque) */

static int width = 0;				/* generated glyph-bitmap width  (= SRC width)  */
static int height = 0;				/* generated glyph-bitmap height (= SRC height) */
static int font_size = FONT_SIZE;
static int outline_width = OUTLINE_WIDTH;
static int is_ow_set = 0;			/* user gave -ow -> don't auto-scale outline   */
static int coverage_thresh = COVERAGE_THRESHOLD;	/* binarization threshold  */
/*
 * The DMA-Font HW consumes a hard 1-bit/threshold glyph (it has no per-pixel
 * alpha — font_alpha is a single global value), so anti-aliasing cannot survive
 * to the screen. Given that, render the glyph with FreeType's dedicated
 * monochrome rasterizer (the mono hinter snaps stems to the pixel grid for the
 * cleanest possible 1-bit shape) instead of thresholding a grayscale bitmap.
 * -gray switches back to the grayscale+threshold path (tunable via -th).
 */
static int render_mono = 1;
/*
 * Slight emboldening (in 26.6 px) to thicken thin stems so they survive 1-bit
 * binarization at small sizes — set via -bold. 0 = off. Applied to the glyph
 * outline before both the body and the stroked outline are derived, so the
 * outline_width inset is preserved.
 */
static int embolden_str = 0;
static int x_pos = 0;			/* text start position inside the glyph bitmap */
static int y_pos = 0;			/* text bottom margin inside the glyph bitmap */
static int letter_spacing = LETTER_SPACE;

/* Optional override for the overlay position inside the 320x240 frame. */
static int dst_xpos = -1;		/* -1 => auto-center */
static int dst_ypos = -1;		/* -1 => auto-center */

/* Required-flag tracking */
static int is_ref_font = 0, is_input_font = 0, is_out_filename = 0;

/*******************************************************************************
 *
 * FreeType glyph rendering (adapted from font_gen.c)
 *
*******************************************************************************/

/*
 * Walk the text with proper layout (26.6 fractional pen advance + kerning +
 * per-glyph left side bearing) so spacing is even and correct, and report the
 * SRC bitmap extent. A left pad of outline_width keeps the (outward-grown)
 * outline of the first glyph from landing at negative x.
 */
static void measure_text_extent(FT_Face face, FT_Stroker stroker,
				const char *text, int *out_width, int *out_height, int *offset)
{
	FT_Int32 load = FT_LOAD_NO_BITMAP | (render_mono ? FT_LOAD_TARGET_MONO : 0);
	FT_Bool use_kerning = FT_HAS_KERNING(face);
	long pen = (long)(x_pos + outline_width) << 6;	/* 26.6 */
	FT_UInt prev = 0;
	int max_top = 0, max_bottom = 0, right = 0;

	for (const char *p = text; *p; p++) {
		FT_UInt idx = FT_Get_Char_Index(face, (unsigned char)*p);

		if (use_kerning && prev && idx) {
			FT_Vector dk;
			FT_Get_Kerning(face, prev, idx, FT_KERNING_DEFAULT, &dk);
			pen += dk.x;
		}

		if (FT_Load_Char(face, (unsigned char)*p, load)) { prev = idx; continue; }
		if (embolden_str) FT_Outline_Embolden(&face->glyph->outline, embolden_str);

		FT_Glyph glyph;
		if (FT_Get_Glyph(face->glyph, &glyph)) { prev = idx; continue; }

		FT_Glyph outline_glyph;
		if (!FT_Glyph_Copy(glyph, &outline_glyph)) {
			if (!FT_Glyph_StrokeBorder(&outline_glyph, stroker, 0, 1) &&
			    !FT_Glyph_To_Bitmap(&outline_glyph, FT_RENDER_MODE_NORMAL, 0, 1)) {
				FT_BitmapGlyph bg = (FT_BitmapGlyph)outline_glyph;
				int pen_int = (int)((pen + 32) >> 6);
				if (bg->top > max_top) max_top = bg->top;
				if ((int)bg->bitmap.rows - bg->top > max_bottom)
					max_bottom = (int)bg->bitmap.rows - bg->top;
				if (pen_int + bg->left + (int)bg->bitmap.width > right)
					right = pen_int + bg->left + (int)bg->bitmap.width;
			}
			FT_Done_Glyph(outline_glyph);
		}

		pen += face->glyph->advance.x + (letter_spacing << 6) + (outline_width << 6);
		FT_Done_Glyph(glyph);
		prev = idx;
	}

	*out_width = right + outline_width;	/* right already includes glyph extent */
	*out_height = max_top + max_bottom + y_pos;
	*offset = max_bottom % 2 ? max_bottom + 1 : max_bottom;

	/* Even-aligned image (NV12 / HW requires even width & height) */
	if (*out_width % 2) (*out_width)++;
	if (*out_height % 2) (*out_height)++;
}

static void draw_bitmap(FT_Bitmap *bitmap, FT_Int x, FT_Int y,
				unsigned char *Y, unsigned char intensity)
{
	for (int i = 0; i < (int)bitmap->rows; i++) {
		for (int j = 0; j < (int)bitmap->width; j++) {
			int xi = x + j;
			int yi = y - bitmap->rows + i;
			if (xi < 0 || xi >= width || yi < 0 || yi >= height) continue;

			/* MONO bitmaps are 1bpp (MSB-first); GRAY bitmaps are 8bpp. */
			unsigned char val;
			if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO)
				val = (bitmap->buffer[i * bitmap->pitch + (j >> 3)]
						>> (7 - (j & 7))) & 1 ? 255 : 0;
			else
				val = bitmap->buffer[i * bitmap->pitch + j];
			/* Binarize on coverage: only pixels at/above the threshold become
			 * part of the glyph. This keeps strokes at their true width so
			 * small text doesn't fatten and merge. (MONO is already 0/255.) */
			if (val >= coverage_thresh) {
				/* Don't let the outline overwrite the solid font body. */
				if (intensity == OUTLINE_VALUE && Y[yi * width + xi] >= 200)
					continue;
				Y[yi * width + xi] = intensity;
			}
		}
	}
}

static int process_font(FT_Face face, FT_Stroker stroker,
				const char *text, unsigned char *Y, int y_offset)
{
	int baseline_y = height - y_offset;

	/* Mono: hint+rasterize for 1-bit. Gray: anti-aliased, binarized later. */
	FT_Int32 load = FT_LOAD_NO_BITMAP | (render_mono ? FT_LOAD_TARGET_MONO : 0);
	FT_Render_Mode rmode = render_mono ? FT_RENDER_MODE_MONO : FT_RENDER_MODE_NORMAL;
	FT_Bool use_kerning = FT_HAS_KERNING(face);
	long pen = (long)(x_pos + outline_width) << 6;	/* 26.6, matches measure */
	FT_UInt prev = 0;

	for (const char *p = text; *p; p++) {
		FT_Glyph glyph = NULL, outline_glyph = NULL;
		FT_UInt idx = FT_Get_Char_Index(face, (unsigned char)*p);

		if (use_kerning && prev && idx) {
			FT_Vector dk;
			FT_Get_Kerning(face, prev, idx, FT_KERNING_DEFAULT, &dk);
			pen += dk.x;
		}

		if (FT_Load_Char(face, (unsigned char)*p, load)) {
			fprintf(stderr, "Could not load character '%c'\n", *p);
			prev = idx;
			continue;
		}
		/* Embolden once on the outline; both the body and the stroked outline
		 * are derived from it, so the outline_width inset is preserved. */
		if (embolden_str) FT_Outline_Embolden(&face->glyph->outline, embolden_str);

		if (FT_Get_Glyph(face->glyph, &glyph)) goto glyph_cleanup;

		int pen_int = (int)((pen + 32) >> 6);

		/* Outline: copy + stroke + rasterize, drawn first. */
		if (!FT_Glyph_Copy(glyph, &outline_glyph) &&
		    !FT_Glyph_StrokeBorder(&outline_glyph, stroker, 0, 1) &&
		    !FT_Glyph_To_Bitmap(&outline_glyph, rmode, 0, 1)) {
			FT_BitmapGlyph bg = (FT_BitmapGlyph)outline_glyph;
			draw_bitmap(&bg->bitmap, pen_int + bg->left,
				baseline_y - bg->top + bg->bitmap.rows, Y, OUTLINE_VALUE);
		}

		/* Body: rasterize the (same, embolded) glyph on top. */
		if (!FT_Glyph_To_Bitmap(&glyph, rmode, 0, 1)) {
			FT_BitmapGlyph bb = (FT_BitmapGlyph)glyph;
			draw_bitmap(&bb->bitmap, pen_int + bb->left,
				baseline_y - bb->top + bb->bitmap.rows, Y, FONT_VALUE);
		}

glyph_cleanup:
		if (outline_glyph) FT_Done_Glyph(outline_glyph);
		if (glyph) FT_Done_Glyph(glyph);

		pen += face->glyph->advance.x + (letter_spacing << 6) + (outline_width << 6);
		prev = idx;
	}

	return 0;
}

/*******************************************************************************
 *
 * NV12 helpers
 *
*******************************************************************************/

/* Fill the whole NV12 buffer with a flat background color.
 * NV12 chroma is an interleaved Cb,Cr,Cb,Cr,... plane, so a single memset
 * only works for neutral chroma (Cb==Cr); fill the two components separately. */
static void nv12_fill_bg(unsigned char *dst, int w, int h)
{
	memset(dst, BG_Y, (size_t)w * h);		/* Y plane */

	unsigned char *c = dst + (size_t)w * h;		/* interleaved CbCr plane */
	size_t cbytes = (size_t)w * h / 2;
	for (size_t i = 0; i + 1 < cbytes; i += 2) {
		c[i]     = BG_CB;	/* Cb */
		c[i + 1] = BG_CR;	/* Cr */
	}
}

static int save_nv12(const char *filename, const unsigned char *buf, size_t size)
{
	FILE *f = fopen(filename, "wb");
	if (!f) {
		fprintf(stderr, "Could not open output file '%s'\n", filename);
		return -1;
	}
	size_t n = fwrite(buf, 1, size, f);
	fclose(f);
	if (n != size) {
		fprintf(stderr, "Short write: %zu / %zu bytes\n", n, size);
		return -1;
	}
	return 0;
}

/*******************************************************************************
 *
 * CLI
 *
*******************************************************************************/

static void print_help(void)
{
	printf("DMA-Font test: renders text (FreeType) into a SRC buffer and\n");
	printf("composites it onto a %dx%d NV12 image using the DMA-Font HW.\n\n", DST_W, DST_H);
	printf("  %-18s%-12s%s\n", "-f [filename]", "[Required]", "TTF(font) file to reference");
	printf("  %-18s%-12s%s\n", "-i [string]",   "[Required]", "Text to render");
	printf("  %-18s%-12s%s\n", "-o [filename]",  "[Optional]", "Output NV12 file name");
	printf("  %-18s%-12s%s\n", "-fs [size]",     "[Optional]", "Font size (default 32, auto-shrinks to fit)");
	printf("  %-18s%-12s%s\n", "-ow [width]",    "[Optional]", "Outline width (default: auto ~size/16)");
	printf("  %-18s%-12s%s\n", "-gray",          "[Optional]", "Grayscale+threshold render (default: mono)");
	printf("  %-18s%-12s%s\n", "-th [1..255]",   "[Optional]", "Coverage threshold for -gray (default 128; lower=bolder)");
	printf("  %-18s%-12s%s\n", "-bold [0..30]",  "[Optional]", "Embolden stems, tenths of px (default 0.3 if given)");
	printf("  %-18s%-12s%s\n", "-ls [size]",     "[Optional]", "Letter spacing (default 0)");
	printf("  %-18s%-12s%s\n", "-xy [x] [y]",    "[Optional]", "Overlay position in frame (default: centered)");
	printf("  %-18s%-12s%s\n", "-h", "", "This help");
}

int main(int argc, char *argv[])
{
	FT_Library library = NULL;
	FT_Face face = NULL;
	FT_Stroker stroker = NULL;

	int i, y_offset = 0;
	char text[256] = {0};
	char font_file[256] = {0};
	char out_file[256] = {0};

	void *src = NULL, *dst = NULL;
	size_t src_size = 0;
	VDMA_FONT_CONFIG_S *cfg = NULL;
	VDMA_FONT_INFO_S *fi = NULL;
	Int32 rc;
	int ret = -1;
	int font_inited = 0, vdma_inited = 0;
	int xpos = 0, ypos = 0;

	/* Argument parsing */
	if (argc == 1) { print_help(); return 0; }

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			print_help();
			return 0;
		} else if (!strcmp(argv[i], "-f")) {
			if (i + 1 >= argc || argv[i + 1][0] == '-') {
				printf("Error, Font(TTF) file name not input.\n"); return -1;
			}
			snprintf(font_file, sizeof(font_file), "%s", argv[++i]);
			is_ref_font = 1;
		} else if (!strcmp(argv[i], "-i")) {
			if (i + 1 >= argc) { printf("Error, Text not input.\n"); return -1; }
			snprintf(text, sizeof(text), "%s", argv[++i]);
			is_input_font = 1;
		} else if (!strcmp(argv[i], "-o")) {
			if (i + 1 >= argc || argv[i + 1][0] == '-') {
				printf("Error, Output file name not input.\n"); return -1;
			}
			snprintf(out_file, sizeof(out_file), "%s", argv[++i]);
			is_out_filename = 1;
		} else if (!strcmp(argv[i], "-fs")) {
			if (i + 1 >= argc) { printf("Error, Font size not input.\n"); return -1; }
			font_size = atoi(argv[++i]);
			if (font_size <= 0) { printf("Error, Wrong font size\n"); return -1; }
		} else if (!strcmp(argv[i], "-ow")) {
			if (i + 1 >= argc) { printf("Error, Outline width not input.\n"); return -1; }
			outline_width = atoi(argv[++i]);
			if (outline_width < 0) { printf("Error, Wrong outline width\n"); return -1; }
			is_ow_set = 1;
		} else if (!strcmp(argv[i], "-th")) {
			if (i + 1 >= argc) { printf("Error, Threshold not input.\n"); return -1; }
			coverage_thresh = atoi(argv[++i]);
			if (coverage_thresh < 1 || coverage_thresh > 255) {
				printf("Error, Threshold must be 1..255\n"); return -1;
			}
		} else if (!strcmp(argv[i], "-gray")) {
			render_mono = 0;	/* anti-aliased + threshold instead of mono */
		} else if (!strcmp(argv[i], "-bold")) {
			/* optional strength in tenths of a pixel (default 0.3px). */
			int tenths = 3;
			if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
				tenths = atoi(argv[++i]);
			if (tenths < 0 || tenths > 30) {
				printf("Error, -bold strength must be 0..30 (tenths of px)\n");
				return -1;
			}
			embolden_str = tenths * 64 / 10;	/* -> 26.6 px */
		} else if (!strcmp(argv[i], "-ls")) {
			if (i + 1 >= argc) { printf("Error, Letter spacing not input.\n"); return -1; }
			letter_spacing = atoi(argv[++i]);
			if (letter_spacing < 0) { printf("Error, Wrong letter spacing\n"); return -1; }
		} else if (!strcmp(argv[i], "-xy")) {
			if (i + 2 >= argc) { printf("Error, -xy needs x and y.\n"); return -1; }
			dst_xpos = atoi(argv[++i]);
			dst_ypos = atoi(argv[++i]);
			if (dst_xpos < 0 || dst_ypos < 0) { printf("Error, Wrong overlay position\n"); return -1; }
		} else {
			printf("Invalid parameter. [%s]\n", argv[i]);
			return -1;
		}
	}

	if (!is_ref_font)            { printf("Error, Font(TTF) file not input.\n"); return -1; }
	if (access(font_file, F_OK)) { printf("Error, Font file does not exist.\n"); return -1; }
	if (!is_input_font)          { printf("Error, Text not input.\n"); return -1; }

	/* FreeType init + auto-fit glyph bitmap into the 320x240 frame */
	if (FT_Init_FreeType(&library)) {
		fprintf(stderr, "Could not init FreeType library\n");
		return -1;
	}
	if (FT_New_Face(library, font_file, 0, &face)) {
		fprintf(stderr, "Could not open font %s\n", font_file);
		goto done;
	}
	if (FT_Stroker_New(library, &stroker)) {
		fprintf(stderr, "Could not create stroker\n");
		goto done;
	}

	/* Measure; if the rendered text is wider/taller than the frame,
	 * shrink the font size and re-measure until it fits (or hit the floor). */
	for (;;) {
		/* Scale the outline to the (current) font size unless the user fixed
		 * it with -ow. A thin outline at small sizes keeps glyphs apart. */
		if (!is_ow_set) {
			outline_width = font_size / OUTLINE_RATIO;
			if (outline_width < 1) outline_width = 1;
		}

		FT_Set_Pixel_Sizes(face, 0, font_size);
		FT_Stroker_Set(stroker, outline_width * 64,
				FT_STROKER_LINECAP_ROUND, FT_STROKER_LINEJOIN_ROUND, 0);

		measure_text_extent(face, stroker, text, &width, &height, &y_offset);
		if (width <= 0 || height <= 0) {
			fprintf(stderr, "Error, Invalid font size / empty text\n");
			goto done;
		}
		if ((width <= DST_W && height <= DST_H) || font_size <= MIN_FONT_SIZE)
			break;

		font_size -= 2;
		printf("[*] Text too large for %dx%d — retrying with font size %d\n",
			DST_W, DST_H, font_size);
	}

	if (width > DST_W || height > DST_H) {
		fprintf(stderr,
			"Error, Rendered text (%dx%d) does not fit in %dx%d even at min font size %d.\n",
			width, height, DST_W, DST_H, MIN_FONT_SIZE);
		goto done;
	}
	if (render_mono)
		printf("[+] Glyph bitmap: %dx%d (font size %d, outline %d, mono, bold %d/64px)\n",
			width, height, font_size, outline_width, embolden_str);
	else
		printf("[+] Glyph bitmap: %dx%d (font size %d, outline %d, gray th=%d, bold %d/64px)\n",
			width, height, font_size, outline_width, coverage_thresh, embolden_str);

	/* A thick outline relative to the font size grows each glyph's footprint
	 * until neighbouring outlines touch and merge into blobs. Warn the user. */
	if (is_ow_set && outline_width * 8 > font_size) {
		printf("[!] Warning: outline width %d is too large for font size %d — "
			"glyph outlines will merge.\n"
			"    Use a thinner outline (e.g. -ow 1) or omit -ow for auto (~size/16).\n",
			outline_width, font_size);
	}

	/* DMA-Font init + buffer allocation */
	rc = ENX_DMA_Font_Init(ENX_VDMA_INIT_TYPE_KERNEL);
	if (rc) { fprintf(stderr, "ENX_DMA_Font_Init failed (%d)\n", rc); goto done; }
	vdma_inited = 1;

	dst = ENX_DMA_Font_Buffer_Alloc(VDMA_KIND_DST, DST_SIZE);
	if (!dst) { fprintf(stderr, "Buffer_Alloc(DST) failed\n"); goto done; }

	src_size = (size_t)width * height;	/* Y-only glyph bitmap */
	src = ENX_DMA_Font_Buffer_Alloc(VDMA_KIND_SRC, src_size);
	if (!src) { fprintf(stderr, "Buffer_Alloc(SRC) failed\n"); goto done; }

	/* Render the text directly into the allocated SRC buffer. */
	memset(src, 0, src_size);
	process_font(face, stroker, text, (unsigned char *)src, y_offset);

	/* Build the font config (one blit = whole text bitmap) */
	cfg = ENX_DMA_Font_CfgAlloc(/*blit_count=*/1, /*yc_index=*/0);
	if (!cfg) { fprintf(stderr, "Font_CfgAlloc failed\n"); goto done; }

	cfg->dst_width         = DST_W;
	cfg->dst_height        = DST_H;
	cfg->dst_type_index    = ENX_VDMA_ADDR_TYPE_VIRT;	/* dst given by virt addr */
	cfg->background_overlay = 1;

	if (cfg->background_overlay == 1) {
		/* Prepare the NV12 destination background. */
		nv12_fill_bg((unsigned char *)dst, DST_W, DST_H);
	}

	/* Center the overlay (or use -xy override); keep positions even. */
	xpos = (dst_xpos >= 0) ? dst_xpos : (DST_W - width) / 2;
	ypos = (dst_ypos >= 0) ? dst_ypos : (DST_H - height) / 2;
	if (xpos < 0) xpos = 0;
	if (ypos < 0) ypos = 0;
	xpos &= ~1;
	ypos &= ~1;
	if (xpos + width  > DST_W) xpos = DST_W - width;
	if (ypos + height > DST_H) ypos = DST_H - height;

	fi = &cfg->font_info[0];
	fi->src_addr_flag		= ENX_VDMA_ADDR_TYPE_VIRT;
	fi->src_addr			= (UInt64)(uintptr_t)src;
	fi->src_width			= (UInt16)width;
	fi->src_height			= (UInt16)height;
	fi->font_xpos			= (UInt16)xpos;
	fi->font_ypos			= (UInt16)ypos;
	fi->dst_addr_y			= (UInt64)(uintptr_t)dst;
	fi->dst_addr_c			= (UInt64)(uintptr_t)dst + (UInt64)DST_W * DST_H;
	fi->font_color_y		= FONT_COLOR_Y;
	fi->font_color_cb		= FONT_COLOR_CB;
	fi->font_color_cr		= FONT_COLOR_CR;
	fi->font_value			= FONT_VALUE;
	fi->outline_color_y		= OUTLINE_COLOR_Y;
	fi->outline_color_cb	= OUTLINE_COLOR_CB;
	fi->outline_color_cr	= OUTLINE_COLOR_CR;
	fi->outline_value		= OUTLINE_VALUE;
	fi->bg_color_y			= BG_Y;
	fi->bg_color_cb			= BG_CB;
	fi->bg_color_cr			= BG_CR;
	fi->font_wr_mode		= 0;	/* YC read-write (blend) */
	fi->font_color_tone		= 1;	/* use the configured font/outline colors */
	fi->font_alpha			= FONT_ALPHA;
	fi->font_threshold		= 0;

	printf("[+] Overlay at (%d, %d) on %dx%d NV12\n", xpos, ypos, DST_W, DST_H);

	/* Run the DMA-Font HW */
	rc = ENX_DMA_Font_Update(cfg);
	if (rc) { fprintf(stderr, "ENX_DMA_Font_Update failed (%d)\n", rc); goto done; }

	/* Save the resulting NV12 image */
	if (!is_out_filename)
		snprintf(out_file, sizeof(out_file), "output_%dx%d.nv12", DST_W, DST_H);

	if (save_nv12(out_file, (const unsigned char *)dst, DST_SIZE) == 0) {
		printf("[+] Saved %s (%dx%d NV12, %d bytes)\n",
			out_file, DST_W, DST_H, DST_SIZE);
		ret = 0;
	}

done:
	if (cfg)  ENX_DMA_Font_CfgFree();
	if (src)  ENX_DMA_Font_Buffer_Free(src, src_size);
	if (dst)  ENX_DMA_Font_Buffer_Free(dst, DST_SIZE);
	if (vdma_inited) ENX_DMA_Font_Exit();

	if (stroker) FT_Stroker_Done(stroker);
	if (face)    FT_Done_Face(face);
	if (library) FT_Done_FreeType(library);

	(void)font_inited;
	return ret;
}
