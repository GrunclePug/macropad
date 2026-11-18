// OLED: render on the main loop (core 1), flush from a task on core 0 that
// owns I2C. ESP32 only.
#pragma once

#include "core.h"

namespace mp {

void display_begin();
// Renders when the core's view changed, at most about 30 times a second.
void display_service(const Core& c, uint32_t now, bool force = false);
bool display_online();
void display_dump();  // self-test: print the last rendered frame as text

}  // namespace mp
