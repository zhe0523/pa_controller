# Makefile for PA controller application

TOOLCHAIN_DIR ?= /home/zhe/sdk/kosmo2/pango_config/misc/toolchain/gcc-linaro-7.5.0-2019.12-i686_aarch64-linux-gnu/bin
CROSS_COMPILE ?= $(TOOLCHAIN_DIR)/aarch64-linux-gnu-
ifeq ($(origin CC),default)
CC = $(CROSS_COMPILE)gcc
endif
STRIP ?= $(CROSS_COMPILE)strip

TARGET = pa_controller
BUILD_DIR = build
BIN_DIR = $(BUILD_DIR)/bin

ARCH ?= armv8-a
PA_PU_BASE_ADDR ?= 0x43c10000
PA_PU_UIO_DEVICE ?= /dev/uio0
RS422_DEVICE ?= /dev/ttyS1
RS422_BAUD ?= 115200
DEVICE_WIDTH ?= 3072
DEVICE_HEIGHT ?= 7716
IMAGE_WIDTH ?= 3072
IMAGE_HEIGHT ?= 7716
COL_OFFSET ?= 0
ROW_OFFSET ?= 0

CFLAGS = -Wall -Wextra -O2 -g -std=c11 -D_GNU_SOURCE
CFLAGS += -march=$(ARCH)
CFLAGS += -DPA_PU_BASE_ADDR=$(PA_PU_BASE_ADDR)
CFLAGS += -DPA_PU_UIO_DEVICE='"$(PA_PU_UIO_DEVICE)"'
CFLAGS += -DRS422_DEVICE='"$(RS422_DEVICE)"'
CFLAGS += -DRS422_BAUD=$(RS422_BAUD)
CFLAGS += -DDEVICE_WIDTH=$(DEVICE_WIDTH) -DDEVICE_HEIGHT=$(DEVICE_HEIGHT)
CFLAGS += -DIMAGE_WIDTH=$(IMAGE_WIDTH) -DIMAGE_HEIGHT=$(IMAGE_HEIGHT)
CFLAGS += -DCOL_OFFSET=$(COL_OFFSET) -DROW_OFFSET=$(ROW_OFFSET)
LDFLAGS = -lpthread

SRCS = src/main.c \
       src/log.c \
       src/fpga_mem.c \
       src/pa_pu.c \
       src/rs422.c \
       src/template_builder.c \
       src/command_handler.c

OBJS = $(SRCS:%.c=$(BUILD_DIR)/%.o)

all: $(BIN_DIR)/$(TARGET)

$(BIN_DIR):
	mkdir -p $@

$(BUILD_DIR)/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

deploy: $(BIN_DIR)/$(TARGET)
	$(STRIP) $(BIN_DIR)/$(TARGET) -o $(BIN_DIR)/$(TARGET)_deploy

config:
	@echo "CC = $(CC)"
	@echo "STRIP = $(STRIP)"
	@echo "CFLAGS = $(CFLAGS)"
	@echo "LDFLAGS = $(LDFLAGS)"
	@echo "PA_PU_BASE_ADDR = $(PA_PU_BASE_ADDR)"
	@echo "PA_PU_UIO_DEVICE = $(PA_PU_UIO_DEVICE)"
	@echo "RS422_DEVICE = $(RS422_DEVICE)"
	@echo "RS422_BAUD = $(RS422_BAUD)"
	@echo "DEVICE = $(DEVICE_WIDTH)x$(DEVICE_HEIGHT)"
	@echo "IMAGE = $(IMAGE_WIDTH)x$(IMAGE_HEIGHT)"
	@echo "OFFSET = ($(COL_OFFSET),$(ROW_OFFSET))"

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean config deploy
