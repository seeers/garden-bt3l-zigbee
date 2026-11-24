OUT_DIR += /valve /patches /proj/drivers /platform/chip_8258/flash
INCLUDE_PATHS += -I$(SDK_PATH) -I./src/includes
OBJS += $(OUT_PATH)/valve/app.o $(OUT_PATH)/valve/tuya_uart.o \
	$(OUT_PATH)/patches/cstartup_8258.o $(OUT_PATH)/patches/mac_phy.o \
	$(OUT_PATH)/platform/chip_8258/flash.o $(OUT_PATH)/platform/chip_8258/flash/flash_common.o $(OUT_PATH)/platform/chip_8258/adc.o $(OUT_PATH)/patches/flash_drv.o \
	$(OUT_PATH)/patches/random.o \
	$(OUT_PATH)/proj/drivers/drv_hw.o $(OUT_PATH)/proj/drivers/drv_calibration.o \
	$(OUT_PATH)/proj/drivers/drv_nv.o $(OUT_PATH)/proj/drivers/drv_adc.o $(OUT_PATH)/proj/os/ev_rtc.o

OBJS += $(patsubst $(SDK_PATH)/%.c,$(OUT_PATH)/%.o,$(wildcard $(SDK_PATH)/platform/chip_8258/flash/flash_mid*.c))

$(OUT_PATH)/valve/%.o: valve/%.c
	$(TC32_PATH)tc32-elf-gcc $(GCC_FLAGS) $(INCLUDE_PATHS) -MMD -MP -c -o "$@" "$<"

$(OUT_PATH)/patches/%.o: src/patch_z_sdk/%.c
	$(TC32_PATH)tc32-elf-gcc $(GCC_FLAGS) $(INCLUDE_PATHS) -MMD -MP -c -o "$@" "$<"

$(OUT_PATH)/patches/%.o: src/patch_z_sdk/%.S
	$(TC32_PATH)tc32-elf-gcc $(GCC_FLAGS) $(ASM_FLAGS) $(INCLUDE_PATHS) -c -o "$@" "$<"

-include $(wildcard $(OUT_PATH)/valve/*.d $(OUT_PATH)/patches/*.d)
