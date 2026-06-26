#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_STROKER_H

#define LETTER_SPACE	0 // 글자 간격

#define FONT_VALUE      255
#define OUTLINE_VALUE   128

#define FONT_SIZE       32
#define OUTLINE_WIDTH   4

static int width = 0;
static int height = 0;
static int font_size = FONT_SIZE;
static int outline_width = OUTLINE_WIDTH;
static int x_pos = 0;   // text(outline) start position
static int y_pos = 0;   // text(outline) bottom position
static int letter_spacing = LETTER_SPACE;

/* Required flag */
static int is_ref_font = 0, is_input_font = 0, is_out_filename = 0;

void measure_text_extent(FT_Library library, FT_Face face, FT_Stroker stroker, const char* font_file, const char* text, int* out_width, int* out_height, int* offset)
{
	int total_width = 0;
	int max_top = 0, max_bottom = 0;

	for (const char* p = text; *p; p++) {
		if (FT_Load_Char(face, *p, FT_LOAD_NO_BITMAP)) continue;

		FT_Glyph glyph;
		if (FT_Get_Glyph(face->glyph, &glyph)) continue;

		// Copy and stroke
		FT_Glyph outline_glyph;
		if (FT_Glyph_Copy(glyph, &outline_glyph)) {
			FT_Done_Glyph(glyph);
			continue;
		}

		if (FT_Glyph_StrokeBorder(&outline_glyph, stroker, 0, 1)) {
			FT_Done_Glyph(glyph);
			FT_Done_Glyph(outline_glyph);
			continue;
		}

		if (FT_Glyph_To_Bitmap(&outline_glyph, FT_RENDER_MODE_NORMAL, 0, 1)) {
			FT_Done_Glyph(glyph);
			FT_Done_Glyph(outline_glyph);
			continue;
		}

		FT_BitmapGlyph bitmap_glyph = (FT_BitmapGlyph)outline_glyph;
		FT_Bitmap* bitmap = &bitmap_glyph->bitmap;

		int top = bitmap_glyph->top;
		int bottom = bitmap->rows - top;

		if (top > max_top) max_top = top;
		if (bottom > max_bottom) max_bottom = bottom;

		total_width += (face->glyph->advance.x >> 6) + letter_spacing + outline_width;

		FT_Done_Glyph(outline_glyph);
		FT_Done_Glyph(glyph);
	}

	*out_width = total_width + outline_width + x_pos - letter_spacing;
	*out_height = max_top + max_bottom + y_pos;
	*offset = max_bottom % 2 ? max_bottom+1 : max_bottom;
	printf("max_top=%d, max_bottom=%d\n", max_top, max_bottom);
	// *out_height = max_top + max_bottom + (outline_width * 2) + y_pos;

	/* Even-aligned image */
	if (*out_width %2) (*out_width)++;
	if (*out_height %2) (*out_height)++;
}

void file_newname(char* filename)
{
	FILE* fp;
	char tempName[256];
	int count = 1;
	char* extension = strrchr(filename, '.');

	if ((fp = fopen(filename, "r")) != NULL) {
		fclose(fp);
		do {
			snprintf(tempName, sizeof(tempName), "%.*s_%04d%s", (int)(extension - filename), filename, count, extension);
			count++;
		} while ((fp = fopen(tempName, "r")) != NULL);
		strcpy(filename, tempName);
	}
}

void save_file(const char* filename, unsigned char* Y, int width, int height) {
	FILE* f = fopen(filename, "wb");
	fwrite(Y, 1, width * height, f);
	fclose(f);
}

void draw_bitmap(FT_Bitmap* bitmap, FT_Int x, FT_Int y, unsigned char* Y, unsigned char intensity) {
	for (int i = 0; i < bitmap->rows; i++) {
		for (int j = 0; j < bitmap->width; j++) {
			int xi = x + j;
			int yi = y - bitmap->rows + i;
			if (xi < 0 || xi >= width || yi < 0 || yi >= height) continue;

			unsigned char val = bitmap->buffer[i * bitmap->pitch + j];
			if (val > 0) {
				if (intensity == OUTLINE_VALUE && Y[yi * width + xi] >= 200)
					continue;

				Y[yi * width + xi] = intensity;
			}
		}
	}
}

int process_font(FT_Library library, FT_Face face, FT_Stroker stroker, const char* font_file, const char* text, unsigned char* Y, int y_offset)
{
	int pen_x = x_pos;
    int baseline_y = height - y_offset;
	int font_x = 0, font_y = 0, outline_x = 0, outline_y = 0;

	for (const char* p = text; *p; p++) {
		if (FT_Load_Char(face, *p, FT_LOAD_NO_BITMAP)) {
			fprintf(stderr, "Could not load character '%c'\n", *p);
			continue;
		}

		// Get original glyph
		FT_Glyph glyph, outline_glyph;
		if (FT_Get_Glyph(face->glyph, &glyph)) goto glyph_cleanup;

		// Copy and stroke to generate
		FT_Glyph_Copy(glyph, &outline_glyph);
		if (FT_Glyph_StrokeBorder(&outline_glyph, stroker, 0, 1)) goto glyph_cleanup;

		// Convert stroked glyph to bitmap
		if (FT_Glyph_To_Bitmap(&outline_glyph, FT_RENDER_MODE_NORMAL, 0, 1)) goto glyph_cleanup;
		FT_BitmapGlyph bitmap_glyph = (FT_BitmapGlyph)outline_glyph;

		outline_x = pen_x;
		outline_y = baseline_y - bitmap_glyph->top + bitmap_glyph->bitmap.rows;

		// Draw outline
		draw_bitmap(&bitmap_glyph->bitmap, outline_x, outline_y, Y, OUTLINE_VALUE);

		// Reload and render original glyph
		if (FT_Load_Char(face, *p, FT_LOAD_RENDER)) goto glyph_cleanup;

		font_x = pen_x + outline_width;
		font_y = outline_y - outline_width;

		// Draw font
		draw_bitmap(&face->glyph->bitmap, font_x, font_y, Y, FONT_VALUE);

glyph_cleanup:
		if (outline_glyph) FT_Done_Glyph(outline_glyph);
		if (glyph) FT_Done_Glyph(glyph);

		pen_x += (face->glyph->advance.x >> 6) + letter_spacing + outline_width;
	}

	return 0;
}

int main(int argc, char *argv[])
{
	FT_Library library;
	FT_Face face;
	FT_Stroker stroker;

	int i = 0, y_offset = 0;
	unsigned char *Y;  // Y plane only
	char text[256] = {0,};
	char font_file[256] = {0,};
	char out_file[256] = {0,};

	/* check help and type */
	for (i = 0; argc > i; i++) {
		if (argc == 1 || !strcmp(argv[i], "-h") || !strcmp(argv[i], "--help") || !strcmp(argv[i], "-help")) {
			printf("Provides generation of Font images using Freetype library functions.\n");
			printf("\n");
			printf("  %-20s%10s %s\n", "-f [filename]", "[Required]", "Refer to the ttf(font) file");
			printf("  %-20s%10s %s\n", "-i [string]", "[Required]", "Font characters to generate");
			printf("  %-20s%10s %s\n", "-o [filename]", "[Optional]", "output file name");
			printf("  %-20s%10s %s\n", "-fs [fontsize]", "[Optional]", "font size of the font image");
			printf("  %-20s%10s %s\n", "", "", "The default setting is 24");
			printf("  %-20s%10s %s\n", "-ow [outline]", "[Optional]", "outline width of the font image");
			printf("  %-20s%10s %s\n", "", "", "The default setting is 4");
			printf("  %-20s%10s %s\n", "-xy [x] [y]", "[Optional]", "font image x / y position");
			printf("  %-20s%10s %s\n", "", "", "The default setting is 0, 0");
			printf("  %-20s%10s %s\n", "-ls [size]", "[Optional]", "letter spaceing of the font image");
			printf("  %-20s%10s %s\n", "", "", "The default setting is 0");

			printf("help option:\n");
			printf("  %-20s%10s %s\n", "-h", "Prints help and usage information", "");
			printf("  %-20s%10s %s\n", "-v", "Displays detailed progress information during the execution of the program", "");
			return 0;
		}

		if (strncmp(argv[i], "-f", 3) == 0) { // 참조 폰트 파일 이름
			if ((i + 1 == argc) || (strncmp(argv[i+1], "-", 1) == 0)) {
				printf("Error, Font(TTF) file name not input.\n");
				return -1;
			}
			i++;
		} else if (strncmp(argv[i], "-i", 2) == 0) { // 입력 문자열
			if ((i + 1 == argc) || (strncmp(argv[i+1], "-", 1) == 0)) {
				printf("Error, Font string not input.\n");
				return -1;
			}
			i++;
		} else if (strncmp(argv[i], "-o", 3) == 0) { // 출력 파일 이름
			if ((i + 1 == argc) || (strncmp(argv[i+1], "-", 1) == 0)) {
				printf("Error, File(dst) name not input.\n");
				return -1;
			}
			i++;
		} else if (strncmp(argv[i], "-fs", 3) == 0) { // 폰트 크기
			if ((i + 1 == argc) || (strncmp(argv[i+1], "-", 1) == 0)) {
				printf("Error, Font size not input.\n");
				return -1;
			}
			i++;
		} else if (strncmp(argv[i], "-ow", 3) == 0) { // 윤곽선 넓이
			if ((i + 1 == argc) || (strncmp(argv[i+1], "-", 1) == 0)) {
				printf("Error, Outline width not input.\n");
				return -1;
			}
			i++;
		} else if (strncmp(argv[i], "-xy", 3) == 0) { // 폰트 위치
			if ((i + 1 == argc) || (strncmp(argv[i+1], "-", 1) == 0)) {
				printf("Error, Font x offset not input.\n");
				return -1;
			} else if ((i + 2 == argc) || (strncmp(argv[i+2], "-", 1) == 0)) {
				printf("Error, Font y offset not input.\n");
				return -1;
			}
			i+=2;
		} else if (strncmp(argv[i], "-ls", 3) == 0) { // 윤곽선 넓이
			if ((i + 1 == argc) || (strncmp(argv[i+1], "-", 1) == 0)) {
				printf("Error, letter spaceing not input.\n");
				return -1;
			}
			i++;
		} else if (i != 0) {
			printf("Invalid parameter. [%s]\n", argv[i]);
			return -1;
		}
	}

    /* insert option */
    for (i = 1; argc > i; i++) {
		if (strncmp(argv[i], "-f", 3) == 0) { // 참조 폰트 파일 이름
			sscanf(argv[i+1], "%s", font_file);
			is_ref_font = 1;
			i++;
		} else if (strncmp(argv[i], "-i", 2) == 0) { // 입력 문자열
			sscanf(argv[i+1], "%s", text);
			is_input_font = 1;
			i++;
		} else if (strncmp(argv[i], "-o", 3) == 0) { // 출력 파일 이름
			sscanf(argv[i+1], "%s", out_file);
			is_out_filename = 1;
			i++;
		} else if (strncmp(argv[i], "-fs", 3) == 0) { // 폰트 크기 
			sscanf(argv[i+1], "%d", &font_size);
			if (font_size == 0 || font_size < 0) {
				printf("Error, Wrong font size\n");
				return -1;
			}
			i++;
		} else if (strncmp(argv[i], "-ow", 3) == 0) { // 윤곽선 넓이
			sscanf(argv[i+1], "%d", &outline_width);
			if (outline_width < 0) {
				printf("Error, Wrong font size\n");
				return -1;
			}
			i++;
		} else if (strncmp(argv[i], "-xy", 3) == 0) { // 폰트 위치
			sscanf(argv[i+1], "%d", &x_pos);
			if (x_pos < 0) {
				printf("Error, Wrong horizontal position\n");
				return -1;
			}
			sscanf(argv[i+2], "%d", &y_pos);
			if (y_pos < 0) {
				printf("Error, Wrong vertical position\n");
				return -1;
			}
			i += 2;
		} else if (strncmp(argv[i], "-ls", 3) == 0) { // 윤곽선 넓이
			sscanf(argv[i+1], "%d", &letter_spacing);
			if (letter_spacing < 0) {
				printf("Error, Wrong font size\n");
				return -1;
			}
			i++;
		}
	}

    /* check required option */
    if (is_ref_font == 0) {
		printf("Error, File(src) name not input.\n");
		return -1;
	} else if (access(font_file, F_OK) == -1) {
		printf("Error, File does not exist.\n");
		return -1;
	} else if (is_input_font == 0) {
		printf("Error, Target font string not input.\n");
		return -1;
	}

	/* Initailize Freetype */
	if (FT_Init_FreeType(&library)) {
		fprintf(stderr, "Could not init FreeType library\n");
		return -1;
	}

	if (FT_New_Face(library, font_file, 0, &face)) {
		fprintf(stderr, "Could not open font %s\n", font_file);
		FT_Done_FreeType(library);
		return -1;
	}

	if (FT_Stroker_New(library, &stroker)) {
		fprintf(stderr, "Could not create stroker\n");
		FT_Done_Face(face);
		FT_Done_FreeType(library);
		return -1;
	}

	FT_Set_Pixel_Sizes(face, 0, font_size);

	FT_Stroker_Set(stroker,
				outline_width * 64,
				FT_STROKER_LINECAP_ROUND,
				FT_STROKER_LINEJOIN_ROUND,
				0);

    /* set resolution */
    measure_text_extent(library, face, stroker, font_file, text, &width, &height, &y_offset);
    // printf("calculated width=%d, height=%d\n", width, height);
	if (width <= 0 || height <= 0) {
		printf("Error, Invalid font size\n");
		goto done;
	}

	if (is_out_filename == 0) {
		snprintf(out_file, sizeof(out_file), "output_%dx%d_%c",
			width, height, text[0]);

		strcat(out_file, ".yuv");
//		printf("filename: [%s]\n", out_file);

		file_newname(out_file);
	}

	/* output buffer alloc */
	Y = (unsigned char*)malloc(width * height);
	if (Y == NULL) {
		fprintf(stderr, "Could not allocate memory for Y plane\n");
		goto done;
	}
	memset(Y, 0, width * height);  // Initialize out buffer

	/* process font */
	if (process_font(library, face, stroker, font_file, text, Y, y_offset) < 0) {
		fprintf(stderr, "Error processing font\n");
		goto done;
	}

	save_file(out_file, Y, width, height);

done:
	if(Y) free(Y);
	FT_Stroker_Done(stroker);
	FT_Done_Face(face);
	FT_Done_FreeType(library);
	return 0;
}
