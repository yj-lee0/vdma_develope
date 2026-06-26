include $(PWD)/Config.mk

KER_MODULE 	:= enx_vdma
#BUILD_DIR	:= $(PWD)/../build/drv
BUILD_DIR	:= ~/work/share/en683/vdma/new

BUILD_TS := $(shell date +"%Y-%m-%d,%H:%M:%S")
BUILD_TZ := KST

# Core .ko
obj-m := $(KER_MODULE).o
$(KER_MODULE)-y := enx-vdma-core.o

# Per-HW driver modules. Add new drivers here (each must end with '/').
#   e.g. DRV_MODULES := dz-drv/ rot-drv/ font-drv/ jenc-drv/ jdec-drv/
DRV_MODULES ?= dz-drv/ font-drv/ jpegenc-drv/ jpegdec-drv/ npu-drv/

obj-y += $(DRV_MODULES)

EXTRA_CFLAGS += -Wall -I$(src) -I$(src)/include-uapi/ -DPKG_BUILD_VER=\"$(VER)\" -DPKG_BUILD_TS=\"$(BUILD_TS)\" -DPKG_BUILD_TZ=\"$(BUILD_TZ)\"

# Strip trailing slash for shell globbing (font-drv/ -> font-drv)
DRV_DIRS := $(DRV_MODULES:/=)

# module strip only debug symbol
modules:
	@echo \# $(KER_MODULE) + drivers [$(DRV_DIRS)]: Building modules
	$(MAKE) -C $(LINUX_ROOT) M=$(PWD) modules
	$(CROSS_COMPILE)strip --strip-debug $(KER_MODULE).ko
	@for d in $(DRV_DIRS); do \
	  for ko in $$d/*.ko; do \
	    [ -e "$$ko" ] && $(CROSS_COMPILE)strip --strip-debug $$ko; \
	  done; \
	done

install:
	mkdir -p $(BUILD_DIR)
	cp -r $(KER_MODULE).ko $(BUILD_DIR)/
	@for d in $(DRV_DIRS); do \
	  for ko in $$d/*.ko; do \
	    [ -e "$$ko" ] && cp -r $$ko $(BUILD_DIR)/; \
	  done; \
	done
ifneq (,$(wildcard $(BUILD_INC_DIR)))
	cp -r *.h $(BUILD_INC_DIR)/
endif

clean:
ifeq (,$(wildcard $(LINUX_ROOT)))
	@echo "-- please compile the kernel first to clean make..."
else
	@echo \# $(KER_MODULE) + drivers: Deleting temporary files
	@make -C $(LINUX_ROOT) M=$(PWD) clean
endif
