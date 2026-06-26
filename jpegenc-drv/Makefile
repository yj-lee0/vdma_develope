# SPDX-License-Identifier: GPL-2.0
#
# Makefile for Eyenix Video-DMA JPEG Encoder driver
#

JENC_DRV := enx_vdma_jenc

obj-m += $(JENC_DRV).o

$(JENC_DRV)-y := en683-jpegenc.o

VER := $(shell awk '{getline d; print $$0}' $(sdk_root)/sdk_version.build 2>/dev/null)

EXTRA_CFLAGS += -Wall \
	-I$(src)/.. -I$(src) -I$(src)/../include-uapi \
	-DPKG_BUILD_VER=\"$(VER)\"
