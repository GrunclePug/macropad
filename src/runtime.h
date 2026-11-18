// Runtime tables built from the config record. Pure.
#pragma once

#include "config.h"
#include "core.h"
#include "tools/tools.h"

namespace mp {

struct Runtime {
  Keymap keymaps[CFG_SETS];
  Tool* tools[CFG_TOOLS];
  Variant v;
  // Pin codes (see config.h); PIN_NONE where unused. pins.cpp maps them.
  uint8_t key_pins[CFG_KEYS];
  uint8_t button_pins[MAX_BUTTONS];
  uint8_t enc_a, enc_b;
};

// Fill `rt` from a validated config, and bind each tool kind's instance to its
// params slot in `cfg` (so edits on the pad change the running config).
// Toggle-tool bindings are translated from tool kind to menu index.
void config_to_variant(Config& cfg, Runtime& rt);

}  // namespace mp
