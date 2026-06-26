# ─────────────────────────────────────────────────────────────────────────
# Toolchain
# ─────────────────────────────────────────────────────────────────────────

CROSS_COMPILE	?=
CC		= $(CROSS_COMPILE)gcc
AR		= $(CROSS_COMPILE)ar

PREFIX		?= /usr/local
LIBDIR		?= $(PREFIX)/lib
INCDIR		?= $(PREFIX)/include

# ─────────────────────────────────────────────────────────────────────────
# Compile flags
# ─────────────────────────────────────────────────────────────────────────

CFLAGS		?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS		?= -lpthread

# UAPI 헤더: skeleton/include-uapi/
INCLUDES	= -I../../include-uapi

# FreeType — used only by the font_test binary. Built locally by
# font_test/build_freetype.sh, which installs into $(FREETYPE_DIR).
# Override FREETYPE_DIR (or FT_CFLAGS / FT_LIBS directly) for other layouts.
#
# Linking the shared libfreetype.so by name (-lfreetype) is intentional: it
# carries its own DT_NEEDED for any deps (e.g. zlib), so we don't list them
# here. To static-link instead, override e.g.:
#   make font FT_LIBS="-L$(FREETYPE_DIR)/lib -l:libfreetype.a -lz"
FREETYPE_DIR	?= font_test/libs
FT_CFLAGS	?= -I$(FREETYPE_DIR)/include/freetype2
FT_LIBS		?= -L$(FREETYPE_DIR)/lib -lfreetype

# ─────────────────────────────────────────────────────────────────────────
# Targets
# ─────────────────────────────────────────────────────────────────────────

LIB_SHARED	= enx_vdma.so
LIB_STATIC	= enx_vdma.a

LIB_SRC		= enx_vdma.c
LIB_OBJ		= enx_vdma.o
LIB_OBJ_PIC	= enx_vdma.pic.o
LIB_HDR		= enx_vdma.h

DEMO_BIN	= demo/enx_vdma_demo
DEMO_SRC	= demo/enx_vdma_demo.c

LOOP_BIN	= demo/enx_vdma_loop
LOOP_SRC	= demo/enx_vdma_loop.c

DEMOV_BIN	= demo/Demo_vdma
DEMOV_SRC	= demo/Demo_vdma.c
DEMOV_HDR	= demo/Demo_vdma.h

FONT_BIN	= font_test/enx_vdma_font
FONT_SRC	= font_test/enx_vdma_font.c

.PHONY: all so static demo loop demov tools install clean help

all: $(LIB_SHARED) $(LIB_STATIC)

so: $(LIB_SHARED)

static: $(LIB_STATIC)

demo: $(DEMO_BIN)

loop: $(LOOP_BIN)

demov: $(DEMOV_BIN)

font: $(FONT_BIN)

tools: $(DEMO_BIN) $(LOOP_BIN) $(DEMOV_BIN) $(FONT_BIN)

# ─────────────────────────────────────────────────────────────────────────
# Build rules
# ─────────────────────────────────────────────────────────────────────────

$(LIB_OBJ_PIC): $(LIB_SRC) $(LIB_HDR)
	$(CC) $(CFLAGS) $(INCLUDES) -fPIC -c $(LIB_SRC) -o $@

$(LIB_OBJ): $(LIB_SRC) $(LIB_HDR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $(LIB_SRC) -o $@

$(LIB_SHARED): $(LIB_OBJ_PIC)
	$(CC) -shared -o $@ $^ $(LDFLAGS)

$(LIB_STATIC): $(LIB_OBJ)
	$(AR) rcs $@ $^

# Demo / test binary — links statically against $(LIB_STATIC)
$(DEMO_BIN): $(DEMO_SRC) $(LIB_HDR) $(LIB_STATIC)
	$(CC) $(CFLAGS) $(INCLUDES) $(DEMO_SRC) $(LIB_STATIC) -o $@ $(LDFLAGS)

# Loop benchmark / stress binary — links statically against $(LIB_STATIC)
$(LOOP_BIN): $(LOOP_SRC) $(LIB_HDR) $(LIB_STATIC)
	$(CC) $(CFLAGS) $(INCLUDES) $(LOOP_SRC) $(LIB_STATIC) -o $@ $(LDFLAGS)

# Interactive Scaler / Font / JPEG demo — links statically against $(LIB_STATIC)
$(DEMOV_BIN): $(DEMOV_SRC) $(DEMOV_HDR) $(LIB_HDR) $(LIB_STATIC)
	$(CC) $(CFLAGS) $(INCLUDES) $(DEMOV_SRC) $(LIB_STATIC) -o $@ $(LDFLAGS)

# Font test binary — links statically against $(LIB_STATIC) + FreeType
$(FONT_BIN): $(FONT_SRC) $(LIB_HDR) $(LIB_STATIC)
	$(CC) $(CFLAGS) $(INCLUDES) $(FT_CFLAGS) $(FONT_SRC) $(LIB_STATIC) -o $@ $(LDFLAGS) $(FT_LIBS)

# ─────────────────────────────────────────────────────────────────────────
# Install / clean
# ─────────────────────────────────────────────────────────────────────────

install: all
	install -d $(DESTDIR)$(INCDIR)
	install -m 0644 $(LIB_HDR) $(DESTDIR)$(INCDIR)/
	install -d $(DESTDIR)$(LIBDIR)
	install -m 0755 $(LIB_SHARED) $(DESTDIR)$(LIBDIR)/
	install -m 0644 $(LIB_STATIC) $(DESTDIR)$(LIBDIR)/

clean:
	rm -f $(LIB_SHARED) $(LIB_STATIC) $(LIB_OBJ) $(LIB_OBJ_PIC) $(DEMO_BIN) $(LOOP_BIN) $(DEMOV_BIN) $(FONT_BIN)

help:
	@echo "userapp/lib build targets:"
	@echo "  all (default)    Build $(LIB_SHARED) + $(LIB_STATIC)"
	@echo "  so               Build $(LIB_SHARED)"
	@echo "  static           Build $(LIB_STATIC)"
	@echo "  demo             Build $(DEMO_BIN) (verification / unit test)"
	@echo "  loop             Build $(LOOP_BIN) (submit-loop benchmark)"
	@echo "  demov            Build $(DEMOV_BIN) (interactive Scaler/Font/JPEG demo)"
	@echo "  font             Build $(FONT_BIN) (font test)"
	@echo "  tools            Build demo + loop + demov + font"
	@echo "  install          Install to \$$PREFIX (= $(PREFIX))"
	@echo "  clean            Remove all build artifacts"
	@echo
	@echo "Environment:"
	@echo "  CROSS_COMPILE  = $(CROSS_COMPILE)"
	@echo "  CC             = $(CC)"
	@echo "  CFLAGS         = $(CFLAGS)"
	@echo "  INCLUDES       = $(INCLUDES)"
	@echo "  PREFIX         = $(PREFIX)"
