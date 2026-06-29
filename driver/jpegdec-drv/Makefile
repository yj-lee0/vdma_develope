# SPDX-License-Identifier: GPL-2.0
#
# Makefile for Eyenix Video-DMA JPEG Decoder driver
#

JDEC_DRV := enx_vdma_jdec

obj-m += $(JDEC_DRV).o

$(JDEC_DRV)-y := en683-jpegdec.o

VER := $(shell awk '{getline d; print $$0}' $(sdk_root)/sdk_version.build 2>/dev/null)

EXTRA_CFLAGS += -Wall \
	-I$(src)/.. -I$(src) -I$(src)/../include-uapi \
	-I$(src)/../../common \
	-I$(src)/../../linkreg \
	-I$(src)/../../api/inc \
	-I$(srctree)/drivers/soc/eyenix \
	-I$(srctree)/drivers/memory/eyenix \
	-DPKG_BUILD_VER=\"$(VER)\"
