# GW-SD3 smoke detector firmware (EFR32FG14P231F256GM48), bare-metal arm-none-eabi-gcc.
#
#   make GECKO_SDK=/path/to/gecko_sdk          # build build/smoke.{elf,hex,bin}
#   make GECKO_SDK=... flash                   # flash with pyOCD (CMSIS-DAP probe)
#
# GECKO_SDK must point at a checkout of SiliconLabs/gecko_sdk v4.4.6
# (only platform/Device/SiliconLabs/EFR32FG14P, platform/CMSIS/Core/Include
# and platform/common/inc are needed; see README.md).

GECKO_SDK ?= C:/Tools/gecko_sdk_4.4.6
DEVICE    := EFR32FG14P231F256GM48
CROSS     ?= arm-none-eabi-
PYTHON    ?= py -3.12

CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size

DEV_DIR := $(GECKO_SDK)/platform/Device/SiliconLabs/EFR32FG14P
BUILD   := build
TARGET  := $(BUILD)/smoke

SRCS := src/main.c \
        $(DEV_DIR)/Source/startup_efr32fg14p.c \
        $(DEV_DIR)/Source/system_efr32fg14p.c
OBJS := $(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o)))
vpath %.c $(sort $(dir $(SRCS)))

CPUFLAGS := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
CFLAGS   := $(CPUFLAGS) -std=c99 -Os -g3 -Wall -Wextra -Werror \
            -ffunction-sections -fdata-sections \
            -D$(DEVICE) -D__START=main \
            -Isrc -I$(DEV_DIR)/Include \
            -I$(GECKO_SDK)/platform/CMSIS/Core/Include \
            -I$(GECKO_SDK)/platform/common/inc
LDFLAGS  := $(CPUFLAGS) -T$(DEV_DIR)/Source/GCC/efr32fg14p.ld \
            --specs=nano.specs -nostartfiles \
            -Wl,--gc-sections -Wl,--no-warn-rwx-segments -Wl,-Map=$(TARGET).map -Wl,--print-memory-usage

.PHONY: all clean flash
all: $(TARGET).hex $(TARGET).bin

$(BUILD):
	mkdir -p $@

# SDK sources are compiled without -Werror/-Wextra (vendor code).
$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(if $(filter src/%,$<),$(CFLAGS),$(filter-out -Werror -Wextra,$(CFLAGS))) -c $< -o $@

$(TARGET).elf: $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@
	$(SIZE) $@

$(TARGET).hex: $(TARGET).elf
	$(OBJCOPY) -O ihex $< $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

flash: $(TARGET).hex
	$(PYTHON) tools/flash.py $(TARGET).hex

clean:
	rm -rf $(BUILD)
