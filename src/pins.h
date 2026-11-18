// Reading the inputs. ESP32 only.
#pragma once

#include <stdint.h>

#include "variant.h"

namespace mp {

void pins_begin();
// Bit i set when key / button i reads pressed (low). enc gets A (bit 1), B (bit 0).
void pins_read(uint8_t& keys, uint8_t& buttons, uint8_t& enc);

// Self-test: drive an input pin low as if pressed, or let it float back up.
// Open drain is safe here because the switches only ever connect to GND.
void pins_drive(uint8_t pin, bool pressed);

}  // namespace mp
