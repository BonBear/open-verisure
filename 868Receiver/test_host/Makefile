# Host unit tests of the gateway's security-critical logic (run on Linux with g++, not on the
# ESP32). Needs mbedTLS: sudo apt-get install libmbedtls-dev g++ make
#
#   make -C 868Receiver/test_host            build and run all tests (unit and fuzz)
#   make -C 868Receiver/test_host run T=kdf  only the tests whose name contains "kdf"
#   make -C 868Receiver/test_host fuzz       only the fuzz/property tests (fuzz_*.cpp)
#   HOST_TEST_VERBOSE=1 make ...             also show the firmware's Serial output
#   FUZZ_SCALE=50 FUZZ_SEED=7 make ... fuzz  50 times the fuzz iterations, other seeds
#
# Two binaries, one runner (test_main.cpp): gateway_tests (test_*.cpp, with a stand-in for the
# image receiver) and fuzz_tests (fuzz_*.cpp, with the real image receiver, jpeg_splice.cpp and an
# MQTT recorder). Sanitizer reports abort the run (-fno-sanitize-recover).
#
# Another mbedTLS: make MBEDTLS_CFLAGS=-I<dir>/include MBEDTLS_LIBS="-L<dir> -lmbedcrypto"

CXX            ?= g++
SANITIZE       ?= -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
CXXFLAGS       ?= -std=gnu++17 -O1 -g -Wall -Wextra
MBEDTLS_CFLAGS ?=
MBEDTLS_LIBS   ?= -lmbedcrypto

SRC_DIR  := ../src
BUILD    := build
CPPFLAGS := -DHOST_TEST -Istubs -I$(SRC_DIR) -I. $(MBEDTLS_CFLAGS)

COMMON       := test_main.cpp stubs/host_stubs.cpp $(SRC_DIR)/crypto.cpp $(SRC_DIR)/downlink.cpp
UNIT_SOURCES := $(COMMON) test_crypto.cpp test_portal_util.cpp test_config.cpp test_downlink.cpp \
                test_ota_image.cpp $(SRC_DIR)/config_manager.cpp
FUZZ_SOURCES := $(COMMON) fuzz_jpeg_splice.cpp fuzz_image_receiver.cpp fuzz_crypto.cpp \
                stubs/host_mqtt.cpp $(SRC_DIR)/image_receiver.cpp $(SRC_DIR)/jpeg_splice.cpp

objs = $(patsubst %.cpp,$(BUILD)/%.o,$(subst ../src/,src/,$(1)))
UNIT_OBJECTS := $(call objs,$(UNIT_SOURCES))
FUZZ_OBJECTS := $(call objs,$(FUZZ_SOURCES))
DEPS         := $(sort $(UNIT_OBJECTS:.o=.d) $(FUZZ_OBJECTS:.o=.d))

# The runner exits with 77 when no test matches T: only an error if neither binary has one.
.PHONY: all run fuzz clean
all: run

run: $(BUILD)/gateway_tests $(BUILD)/fuzz_tests
	@a=0; ./$(BUILD)/gateway_tests $(T) || a=$$?; \
	 b=0; ./$(BUILD)/fuzz_tests $(T) || b=$$?; \
	 if { [ $$a -ne 0 ] && [ $$a -ne 77 ]; } || { [ $$b -ne 0 ] && [ $$b -ne 77 ]; } || \
	    { [ $$a -eq 77 ] && [ $$b -eq 77 ]; }; then echo "HOST TESTS FAILED"; exit 1; fi

fuzz: $(BUILD)/fuzz_tests
	./$(BUILD)/fuzz_tests $(T)

$(BUILD)/gateway_tests: $(UNIT_OBJECTS)
	$(CXX) $(SANITIZE) -o $@ $^ $(MBEDTLS_LIBS)

$(BUILD)/fuzz_tests: $(FUZZ_OBJECTS)
	$(CXX) $(SANITIZE) -o $@ $^ $(MBEDTLS_LIBS)

$(BUILD)/src/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SANITIZE) -MMD -MP -c -o $@ $<

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SANITIZE) -MMD -MP -c -o $@ $<

clean:
	rm -rf $(BUILD)

-include $(DEPS)
