// On-device self-test (build with SELFTEST=1). It drives the real input pins
// low through open-drain outputs, the way the switches do, and checks what the
// firmware sends. Uses only harmless outputs (F13-F17, mouse button 8).
#pragma once

#include <stdint.h>

namespace mp {

void selftest_begin();
void selftest_step(uint32_t now);
// Run the script again from the start (the TESTRUN command), once the pad is
// back in normal mode.
void selftest_restart();
// Tap Nav once the pad is back in normal mode (the TESTTAP command): on a
// live-keys set that toggles live keys, an unsaved pad edit a key screen
// keeps from autosaving.
void selftest_request_nav_tap();
// Entering config mode re-sends a saved report until TESTACK confirms it was
// read; TESTACK then removes it.
void selftest_on_config_entry();
void selftest_ack_report();
// The next boot doesn't run the script (TESTERASE norun: a fresh pad the
// hardware proof edits by hand).
void selftest_skip_next_run();

}  // namespace mp
