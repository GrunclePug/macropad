# Build, flash and test. Firmware builds use arduino-cli and the profile in
# sketch.yaml; tests build the pure logic natively with the host's g++.
#   make                          build the firmware
#   make upload PORT=/dev/ttyACM0 build and flash
#   make SELFTEST=1 upload ...    on-device self-test build
#   make test                     native tests
#   make clean

DEFINES := $(if $(SELFTEST),-DMACROPAD_SELFTEST)
BUILD   := .build/$(if $(SELFTEST),selftest,firmware)
CLI     := arduino-cli compile --warnings all --build-path $(BUILD) \
           --build-property "compiler.cpp.extra_flags=$(DEFINES)"

TEST_SRC := $(wildcard test/*.cpp) src/input.cpp src/hid.cpp src/core.cpp src/link.cpp \
            src/config.cpp src/boot.cpp src/runtime.cpp src/knock.cpp src/proto.cpp \
            src/tools/tools.cpp src/tools/schema.cpp

.PHONY: build upload test vectors clean

build:
	$(CLI) .

upload:
ifndef PORT
	$(error set PORT to the board's real device, e.g. PORT=/dev/ttyACM0)
endif
	$(CLI) --upload -p $(PORT) .

test:
	@mkdir -p .build
	$(CXX) -std=c++11 -Wall -Wextra -Werror -O1 -o .build/tests $(TEST_SRC)
	./.build/tests

vectors:
	@mkdir -p .build test/vectors
	$(CXX) -std=c++11 -Wall -Wextra -Werror -O1 -o .build/gen_vectors test/gen/gen_vectors.cpp \
		src/config.cpp src/tools/schema.cpp
	./.build/gen_vectors

clean:
	rm -rf .build
