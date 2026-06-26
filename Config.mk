ifeq ($(PLATFORM_NAME),iplat2)
ifneq ($(CHIPSET),en683)
export CHIPSET=en683
export CHIPSET_U=EN683
CPU_CFLAGS += -D_$(CHIPSET_U)_$(CHIP_LEVEL)_
endif
endif

CPU_CFLAGS += -mcmodel=medany
export CPU_CFLAGS

# absolute path
SDK_ROOT		:= $(sdk_root)
MOD_ROOT		:= $(shell pwd)
OBJ_DIR 		:= $(MOD_ROOT)/build
LIB_ROOT		:= $(MOD_ROOT)/../libs
LIB_DIR			:= $(LIB_ROOT)/$(TOOLCHAIN_VER)
LIB_INCLUDE		:= $(LIB_ROOT)/include
API_INC_PATH	:= $(SDK_ROOT)/api/include
API_LIB_PATH	:= $(SDK_ROOT)/api/lib
LINUX_ROOT 		?= $(SDK_ROOT)/work/linux

CC				:= $(CROSS_COMPILE)gcc $(CPU_CFLAGS) -DNO_OPENSSL -I$(MOD_ROOT)/include -I$(LIB_INCLUDE) -g -Wall -lpthread -L$(LIB_DIR)
CXX				:= $(CROSS_COMPILE)g++ $(CPU_CFLAGS) -DNO_OPENSSL -I$(MOD_ROOT)/include -I$(LIB_INCLUDE) -g -Wall -lpthread -L$(LIB_DIR)
LD				:= ${CROSS_COMPILE}ld
AR				:= ${CROSS_COMPILE}ar
STRIP			:= ${CROSS_COMPILE}strip

INCLUDE_LIVE 	:= -I$(LIB_INCLUDE)/liveMedia -I$(LIB_INCLUDE)/BasicUsageEnvironment -I$(LIB_INCLUDE)/groupsock -I$(LIB_INCLUDE)/UsageEnvironment -I$(LIB_INCLUDE)/libRTSP
LD_FLAGS_LIVE 	:= -lliveMedia -lgroupsock -lBasicUsageEnvironment -lUsageEnvironment -lenxrtsp
LD_FLAGS_FFMPEG	:= -lavformat -lavcodec -lavutil -lswscale -lswresample -lmp3lame -Wl,-rpath-link,$(LIB_DIR)

#RELEASE_MODE ?= 1
ifdef RELEASE_MODE
CC += -DRELEASE_MODE=1
export CC
CXX += -DRELEASE_MODE=1
export CXX
endif

ifdef APP_PACKAGE_NAME
CC += -DAPP_PACKAGE_NAME=$(APP_PACKAGE_NAME) -DAPP_MODEL_INFO=$(APP_MODEL_INFO) -DCOMPANY_NAME=$(COMPANY_NAME)
CXX += -DAPP_PACKAGE_NAME=$(APP_PACKAGE_NAME) -DAPP_MODEL_INFO=$(APP_MODEL_INFO) -DCOMPANY_NAME=$(COMPANY_NAME)
endif

#LD_FLAGS_FFMPEG += -lfdk-aac

# api related.

