// On-device self-test (build with SELFTEST=1). It drives the real input pins
// low through open-drain outputs, the way the switches do, and checks what the
// firmware sends. Uses only harmless outputs (F13-F17, mouse button 8).
#pragma once

#include <stdint.h>

namespace mp {

void selftest_begin();
void selftest_step(uint32_t now);

}  // namespace mp
