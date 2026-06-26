# SPDX-License-Identifier: MIT
#
# userapp/scaler/Makefile — libvdma_scaler + examples
#
# 빌드 산출물:
#   example_vdma_scaler   DZ (scaler) 동작 예제 (downscale / format / crop+flip)
#   libvdma_scaler.so     (옵션) 공유 라이브러리
#
# 사용 예:
#   make                    # 예제 빌드
#   make so                 # 공유 라이브러리 까지
#   make CROSS_COMPILE=aarch64-linux-gnu-
#   make install PREFIX=/usr/local
#   make clean

# ─────────────────────────────────────────────────────────────────────────
# Toolchain
# ─────────────────────────────────────────────────────────────────────────

CROSS_COMPILE	?=
CC		= $(CROSS_COMPILE)gcc
AR		= $(CROSS_COMPILE)ar

PREFIX		?= /usr/local
BINDIR		?= $(PREFIX)/bin
LIBDIR		?= $(PREFIX)/lib
INCDIR		?= $(PREFIX)/include

# ─────────────────────────────────────────────────────────────────────────
# Compile flags
# ─────────────────────────────────────────────────────────────────────────

CFLAGS		?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS		?= -lpthread

# UAPI header paths: common in ../../, DZ-specific in ../../dz-drv/
INCLUDES	= -I../.. -I../../include-uapi

# ─────────────────────────────────────────────────────────────────────────
# Targets
# ─────────────────────────────────────────────────────────────────────────

EXAMPLES	= example_vdma_scaler
LIB_SHARED	= libvdma_scaler.so
LIB_STATIC	= libvdma_scaler.a

LIB_SRC		= libvdma_scaler.c
LIB_OBJ		= libvdma_scaler.o

.PHONY: all so static install clean help

all: $(EXAMPLES)

so: $(LIB_SHARED)

static: $(LIB_STATIC)

# ─────────────────────────────────────────────────────────────────────────
# Build rules
# ─────────────────────────────────────────────────────────────────────────

# 예제는 libvdma_scaler.c 를 직접 같이 컴파일 (single .so 의존성 없이 standalone)
example_vdma_scaler: example_vdma_scaler.c $(LIB_SRC) libvdma_scaler.h
	$(CC) $(CFLAGS) $(INCLUDES) example_vdma_scaler.c $(LIB_SRC) $(LDFLAGS) -o $@

# 공유 라이브러리 (앱이 -lvdma_scaler 로 링크)
$(LIB_SHARED): $(LIB_SRC) libvdma_scaler.h
	$(CC) $(CFLAGS) $(INCLUDES) -fPIC -shared $(LIB_SRC) $(LDFLAGS) -o $@

# 정적 라이브러리
$(LIB_OBJ): $(LIB_SRC) libvdma_scaler.h
	$(CC) $(CFLAGS) $(INCLUDES) -fPIC -c $(LIB_SRC) -o $@

$(LIB_STATIC): $(LIB_OBJ)
	$(AR) rcs $@ $^

# ─────────────────────────────────────────────────────────────────────────
# Install / clean
# ─────────────────────────────────────────────────────────────────────────

install: $(EXAMPLES)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(EXAMPLES) $(DESTDIR)$(BINDIR)/
	install -d $(DESTDIR)$(INCDIR)
	install -m 0644 libvdma_scaler.h $(DESTDIR)$(INCDIR)/
	@if [ -f $(LIB_SHARED) ]; then \
	  install -d $(DESTDIR)$(LIBDIR); \
	  install -m 0755 $(LIB_SHARED) $(DESTDIR)$(LIBDIR)/; \
	fi
	@if [ -f $(LIB_STATIC) ]; then \
	  install -d $(DESTDIR)$(LIBDIR); \
	  install -m 0644 $(LIB_STATIC) $(DESTDIR)$(LIBDIR)/; \
	fi

clean:
	rm -f $(EXAMPLES) $(LIB_SHARED) $(LIB_STATIC) $(LIB_OBJ)

help:
	@echo "userapp/scaler build targets:"
	@echo "  all (default)    Build $(EXAMPLES)"
	@echo "  so               Build $(LIB_SHARED)"
	@echo "  static           Build $(LIB_STATIC)"
	@echo "  install          Install to \$$PREFIX (= $(PREFIX))"
	@echo "  clean            Remove all build artifacts"
	@echo
	@echo "Environment:"
	@echo "  CROSS_COMPILE  = $(CROSS_COMPILE)"
	@echo "  CC             = $(CC)"
	@echo "  CFLAGS         = $(CFLAGS)"
	@echo "  INCLUDES       = $(INCLUDES)"
	@echo "  PREFIX         = $(PREFIX)"
