# Host tests of the CC1110 persistent packet counter log (cc1110_common/ctr_log.c and each
# device's ctr_boot) on an emulated CC1110 flash. Linux x86-64 (the firmware's __code pointers
# read the emulated flash through the GS segment); needs gcc, g++, make and python3.
#
#   make -C cc1110_common/test_host            build and run all tests
#   make -C cc1110_common/test_host run T=cut  only the tests whose name contains "cut"
#
# The firmware code is not copied: extract_ctr.py cuts ctr_boot() and the counter rule before a
# packet out of VerisureDoorSensor/main.c and MD_CAM/firmware/main.c at every build, and the
# real ctr_log.c is included from there. ctr_glue.c is built once per variant below.

CC        ?= gcc
CXX       ?= g++
PYTHON    ?= python3
SANITIZE  ?= -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS    ?= -std=gnu11 -O1 -g -Wall -Wextra
CXXFLAGS  ?= -std=gnu++17 -O1 -g -Wall -Wextra

ROOT     := ../..
DOOR_SRC := $(ROOT)/VerisureDoorSensor/main.c
CAM_SRC  := $(ROOT)/MD_CAM/firmware/main.c
BUILD    := build

# The firmware casts XDATA pointers to 16-bit addresses (DMA descriptor) and flash addresses to
# __code pointers; on the host these are only low address bits / GS offsets, by design.
FW_CFLAGS := $(CFLAGS) -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-unused-variable \
             -Wno-unused-function -Istubs -I. -I$(BUILD)

# variant name, device (door/cam), extra macros
VARIANTS := door door4 door_seed door_seed0 door_seedbig cam cam_seed
door_DEV         := door
door4_DEV        := door
door4_DEFS       := -DCTR_ENTRIES=4
door_seed_DEV    := door
door_seed_DEFS   := -DCTR_SEED=0x123456UL
door_seed0_DEV   := door
door_seed0_DEFS  := -DCTR_SEED=0UL
door_seedbig_DEV := door
door_seedbig_DEFS := -DCTR_SEED=0xFF0000UL
cam_DEV          := cam
cam_seed_DEV     := cam
cam_seed_DEFS    := -DCTR_SEED=0x123456UL
door_IQUOTE := -iquote $(ROOT)/VerisureDoorSensor
cam_IQUOTE  := -iquote $(ROOT)/MD_CAM/firmware
cam_DEVDEF  := -DDEV_CAM

TESTS   := test_main.cpp test_ctr_log.cpp test_ctr_random.cpp
OBJECTS := $(patsubst %.cpp,$(BUILD)/%.o,$(TESTS)) $(BUILD)/emu.o \
           $(foreach v,$(VARIANTS),$(BUILD)/glue_$(v).o)

.PHONY: all run clean check-arch
all: run

check-arch:
	@test "$$(uname -m)" = x86_64 || { echo "these tests need Linux on x86-64 (GS segment)"; exit 1; }

run: check-arch $(BUILD)/ctr_log_tests
	./$(BUILD)/ctr_log_tests $(T)

$(BUILD)/ctr_log_tests: $(OBJECTS)
	$(CXX) $(SANITIZE) -o $@ $^

$(BUILD)/door_ctr.inc: $(DOOR_SRC) $(ROOT)/cc1110_common/delay.c extract_ctr.py
	@mkdir -p $(BUILD)
	$(PYTHON) extract_ctr.py $< $@

$(BUILD)/cam_ctr.inc: $(CAM_SRC) $(ROOT)/cc1110_common/delay.c extract_ctr.py
	@mkdir -p $(BUILD)
	$(PYTHON) extract_ctr.py $< $@

define VARIANT_RULE
$(BUILD)/glue_$(1).o: ctr_glue.c ctr_dev.h emu.h pic_stubs.h stubs/cc1110.h $(BUILD)/$($(1)_DEV)_ctr.inc \
                      $(ROOT)/cc1110_common/ctr_log.c
	@mkdir -p $(BUILD)
	$(CC) $(FW_CFLAGS) $(SANITIZE) $($($(1)_DEV)_IQUOTE) $($($(1)_DEV)_DEVDEF) $($(1)_DEFS) \
	    -DDEV_SYM=dev_$(1) -DDEV_NAME='"$(1)"' -c -o $$@ $$<
endef
$(foreach v,$(VARIANTS),$(eval $(call VARIANT_RULE,$(v))))

$(BUILD)/emu.o: emu.c emu.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SANITIZE) -c -o $@ $<

$(BUILD)/%.o: %.cpp test.h ctr_dev.h emu.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(SANITIZE) -I. -c -o $@ $<

clean:
	rm -rf $(BUILD)
