// The variant chosen at build time (-DMACROPAD_VARIANT_<NAME>).
#pragma once

#include "core.h"

namespace mp {

struct Pins {
  uint8_t keys[MAX_KEYS];
  uint8_t buttons[MAX_BUTTONS];  // in the variant's button order
  uint8_t enc_a, enc_b;          // used when the variant has an encoder
};

extern const Variant VARIANT;
extern const Pins PINS;

}  // namespace mp
