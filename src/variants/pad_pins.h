// The user's pad, current breadboard wiring. Include from exactly one file.
#pragma once

#include <Arduino.h>

#include "../variant.h"

namespace mp {

const Pins PINS = {
    {D2, D3, D4, D5},  // keys 1-4
    {D6, D7},          // killswitch, nav
    0, 0,              // no encoder
};

}  // namespace mp
