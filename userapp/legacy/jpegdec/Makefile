# SPDX-License-Identifier: MIT
#
# userapp/jpegdec/Makefile — libvdma_jpegdec + examples

CROSS_COMPILE	?=
CC		= $(CROSS_COMPILE)gcc
AR		= $(CROSS_COMPILE)ar

PREFIX		?= /usr/local
BINDIR		?= $(PREFIX)/bin
LIBDIR		?= $(PREFIX)/lib
INCDIR		?= $(PREFIX)/include

CFLAGS		?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS		?= -lpthread

INCLUDES	= -I../.. -I../../include-uapi

EXAMPLES	= example_vdma_jpegdec
LIB_SHARED	= libvdma_jpegdec.so
LIB_STATIC	= libvdma_jpegdec.a

LIB_SRC		= libvdma_jpegdec.c
LIB_OBJ		= libvdma_jpegdec.o

.PHONY: all so static install clean help

all: $(EXAMPLES)

so: $(LIB_SHARED)

static: $(LIB_STATIC)

example_vdma_jpegdec: example_vdma_jpegdec.c $(LIB_SRC) libvdma_jpegdec.h
	$(CC) $(CFLAGS) $(INCLUDES) example_vdma_jpegdec.c $(LIB_SRC) $(LDFLAGS) -o $@

$(LIB_SHARED): $(LIB_SRC) libvdma_jpegdec.h
	$(CC) $(CFLAGS) $(INCLUDES) -fPIC -shared $(LIB_SRC) $(LDFLAGS) -o $@

$(LIB_OBJ): $(LIB_SRC) libvdma_jpegdec.h
	$(CC) $(CFLAGS) $(INCLUDES) -fPIC -c $(LIB_SRC) -o $@

$(LIB_STATIC): $(LIB_OBJ)
	$(AR) rcs $@ $^

install: $(EXAMPLES)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(EXAMPLES) $(DESTDIR)$(BINDIR)/
	install -d $(DESTDIR)$(INCDIR)
	install -m 0644 libvdma_jpegdec.h $(DESTDIR)$(INCDIR)/
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
	@echo "userapp/jpegdec build targets:"
	@echo "  all (default)    Build $(EXAMPLES)"
	@echo "  so               Build $(LIB_SHARED)"
	@echo "  static           Build $(LIB_STATIC)"
	@echo "  install          Install to \$$PREFIX (= $(PREFIX))"
	@echo "  clean            Remove all build artifacts"
