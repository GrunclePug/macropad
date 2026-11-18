// Onboard RGB LED status: red while killed, green while a tool runs. ESP32 only.
#pragma once

#include "core.h"

namespace mp {

void led_begin();
void led_update(const Core& c);

}  // namespace mp
