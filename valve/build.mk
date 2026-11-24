PROJECT_NAME := BT3L-Valve-Zigbee
SRC_DIR := /valve
APP_MAKEFILE := valve/objects.mk
OUT_PATH := ./build-valve
BIN_PATH := ./out-valve
PYTHON := python3
include makefile
.DEFAULT_GOAL := firmware

$(OBJ_LIST): valve/app_cfg.h valve/version_cfg.h
$(ELF_FILE): valve/boot_z.link

.PHONY: firmware
firmware: pre-build
	$(MAKE) -f valve/build.mk $(BIN_FILE)
