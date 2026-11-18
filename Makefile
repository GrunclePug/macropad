# Build, flash and test. Firmware builds use arduino-cli and the profile in
# sketch.yaml; tests build the pure logic natively with the host's g++.
#   make                          build the default variant
#   make VARIANT=deck             build another variant
#   make upload PORT=/dev/ttyACM0 build and flash
#   make SELFTEST=1 upload ...    on-device self-test build
#   make test                     native tests
#   make clean

VARIANTS := pad deck
VARIANT  ?= pad

ifeq ($(filter $(VARIANT),$(VARIANTS)),)
$(error unknown VARIANT '$(VARIANT)'; known: $(VARIANTS))
endif

ifneq ($(SELFTEST),)
ifneq ($(VARIANT),pad)
$(error the self-test is written for VARIANT=pad)
endif
endif

DEFINES := -DMACROPAD_VARIANT_$(shell echo $(VARIANT) | tr a-z A-Z)$(if $(SELFTEST), -DMACROPAD_SELFTEST)
BUILD   := .build/$(VARIANT)$(if $(SELFTEST),-selftest)
CLI     := arduino-cli compile --warnings all --build-path $(BUILD) \
           --build-property "compiler.cpp.extra_flags=$(DEFINES)"

TEST_SRC := $(wildcard test/*.cpp) src/input.cpp src/hid.cpp src/core.cpp \
            src/tools/tools.cpp src/settings_key.cpp

.PHONY: build upload test clean

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

clean:
	rm -rf .build
