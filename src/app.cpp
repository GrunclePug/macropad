#include "app.h"

#include <Arduino.h>

#include "display.h"
#include "led.h"
#include "pins.h"
#include "selftest.h"
#include "settings.h"
#include "usb.h"
#include "variant.h"
#include "version.h"

namespace mp {

Input input;
Arbiter hid;
Core core;
LoopStats stats;

namespace {
Rng rng;
uint32_t last_loop_us = 0;
char boot_text[24];
}  // namespace

void app_setup() {
  Serial.begin(115200);
  led_begin();
  pins_begin();
  rng.seed(esp_random());
  core.begin(VARIANT, input, hid, rng);
  settings_begin(VARIANT);
  display_begin();
  usb_begin();
  enableLoopWDT();

  snprintf(boot_text, sizeof boot_text, "%s %s", VARIANT.name, MACROPAD_VERSION);
  core.notice(boot_text, millis(), 1500);
  Serial.printf("macropad %s, variant %s\n", MACROPAD_VERSION, VARIANT.name);
#ifdef MACROPAD_SELFTEST
  selftest_begin();
#endif
}

void app_loop() {
  uint32_t us = micros();
  if (stats.measuring && last_loop_us) {
    uint32_t gap = us - last_loop_us;
    if (gap > stats.max_gap_us) stats.max_gap_us = gap;
    if (gap > 2000) stats.gaps_over_2ms++;
    stats.loops++;
  }
  last_loop_us = us;
  uint32_t now = millis();

#ifdef MACROPAD_SELFTEST
  selftest_step(now);
#endif

  uint8_t keys, buttons, enc;
  pins_read(keys, buttons, enc);
  input.sample(keys, buttons, enc, now);
  core.link(usb_up(), usb_take_disturbed());

  Event e;
  while (input.pop(e)) core.handle(e, now);
  settings_service(core, now);  // before a newly shown keymap goes live
  core.update(now);
  hid.collect();
  hid.service(usb_port());

  display_service(core, now, stats.force_redraw);
  stats.force_redraw = false;
  led_update(core);
}

}  // namespace mp
