# Build and flash with arduino-cli and the profile in sketch.yaml.
#   make                     build the default variant
#   make VARIANT=pad         build a variant
#   make upload PORT=/dev/ttyACM0 [VARIANT=pad]
#   make clean

VARIANTS := pad
VARIANT  ?= pad

ifeq ($(filter $(VARIANT),$(VARIANTS)),)
$(error unknown VARIANT '$(VARIANT)'; known: $(VARIANTS))
endif

DEFINE := MACROPAD_VARIANT_$(shell echo $(VARIANT) | tr a-z A-Z)
BUILD  := .build/$(VARIANT)
CLI    := arduino-cli compile --build-path $(BUILD) \
          --build-property "compiler.cpp.extra_flags=-D$(DEFINE)"

.PHONY: build upload clean

build:
	$(CLI) .

upload:
ifndef PORT
	$(error set PORT to the board's real device, e.g. PORT=/dev/ttyACM0)
endif
	$(CLI) --upload -p $(PORT) .

clean:
	rm -rf .build
