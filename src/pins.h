// Reading the inputs. ESP32 only. Pins are pin codes (config.h).
#pragma once

#include <stdint.h>

#include "runtime.h"

namespace mp {

uint8_t pin_arduino(uint8_t code);  // Arduino pin number for a pin code
void pins_begin(const Runtime& rt);
// Bit i set when key / button i reads pressed (low). enc gets A (bit 1), B (bit 0).
void pins_read(uint8_t& keys, uint8_t& buttons, uint8_t& enc);
bool pin_low(uint8_t code);  // one raw read, for the safe-boot check

// Self-test: drive an input low as if pressed, or let it float back up.
// Open drain is safe here because the switches only ever connect to GND.
void pins_drive(uint8_t code, bool pressed);
void pins_release_all();

}  // namespace mp
