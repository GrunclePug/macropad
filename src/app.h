// The main loop. ESP32 only.
#pragma once

#include "config.h"
#include "core.h"
#include "runtime.h"

namespace mp {

void app_setup();
void app_loop();

// Shared with the self-test.
extern Input input;
extern Arbiter hid;
extern Core core;

struct LoopStats {
  uint32_t max_gap_us = 0;
  uint32_t gaps_over_2ms = 0;
  uint32_t loops = 0;
  bool measuring = false;
  bool force_redraw = false;
};
extern LoopStats stats;

const Runtime& app_runtime();
Config& app_config();
// The self-test may drive pins only on a normal boot running its fixture.
bool app_fixture_running();

}  // namespace mp
