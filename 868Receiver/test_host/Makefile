# Host unit tests of the gateway's security-critical logic (run on Linux with g++, not on the
# ESP32). Needs mbedTLS: sudo apt-get install libmbedtls-dev g++ make
#
#   make -C 868Receiver/test_host            build and run all tests
#   make -C 868Receiver/test_host run T=kdf  only the tests whose name contains "kdf"
#   HOST_TEST_VERBOSE=1 make ...             also show the firmware's Serial output
#
# Another mbedTLS: make MBEDTLS_CFLAGS=-I<dir>/include MBEDTLS_LIBS="-L<dir> -lmbedcrypto"

CXX            ?= g++
SANITIZE       ?= -fsanitize=address,undefined -fno-omit-frame-pointer
CXXFLAGS       ?= -std=gnu++17 -O1 -g -Wall -Wextra
MBEDTLS_CFLAGS ?=
MBEDTLS_LIBS   ?= -lmbedcrypto

SRC_DIR  := ../src
BUILD    := build
CPPFLAGS := -DHOST_TEST -Istubs -I$(SRC_DIR) -I. $(MBEDTLS_CFLAGS)

SOURCES := test_main.cpp test_crypto.cpp test_portal_util.cpp test_config.cpp \
           stubs/host_stubs.cpp $(SRC_DIR)/crypto.cpp $(SRC_DIR)/config_manager.cpp
OBJECTS := $(patsubst %.cpp,$(BUILD)/%.o,$(subst ../src/,src/,$(SOURCES)))
DEPS    := $(OBJECTS:.o=.d)

.PHONY: all run clean
all: run

run: $(BUILD)/gateway_tests
	./$(BUILD)/gateway_tests $(T)

$(BUILD)/gateway_tests: $(OBJECTS)
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
