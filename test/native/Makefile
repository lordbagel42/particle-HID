# Native tests for the portable core: make -C test/native
CXX ?= g++
CXXFLAGS ?= -std=gnu++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined
SRC := $(filter-out ../../src/phid/ble_hid.cpp ../../src/phid/platform_particle.cpp,$(wildcard ../../src/phid/*.cpp))
TESTS := $(wildcard *.cpp)
BUILD := build

$(BUILD)/tests: $(SRC) $(TESTS) $(wildcard *.h) $(wildcard ../../src/phid/*.h)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(SRC) $(TESTS) -lcrypto -o $@

test: $(BUILD)/tests
	./$(BUILD)/tests

clean:
	rm -rf $(BUILD)

.PHONY: test clean
.DEFAULT_GOAL := test
