// The deck's planned wiring. Include from exactly one file.
#pragma once

#include <Arduino.h>

#include "../variant.h"

namespace mp {

const Pins PINS = {
    {D2, D3, D4, D5, D6, D7, D8, D9},  // keys 1-8
    {D10, D11, A1},                    // bound button, nav, encoder push
    D12, A0,                           // encoder A, B
};

}  // namespace mp
