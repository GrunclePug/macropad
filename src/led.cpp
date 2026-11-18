#include "led.h"

#include <Arduino.h>

namespace mp {
namespace {
uint8_t shown = 0xFF;
}

void led_begin() {
  // Active-low: HIGH is off.
  for (uint8_t p : {LED_RED, LED_GREEN, LED_BLUE}) {
    pinMode(p, OUTPUT);
    digitalWrite(p, HIGH);
  }
}

void led_update(const Core& c) {
  bool running = false;
  for (uint8_t t = 0; t < c.variant().ntools; t++) running |= c.active(t);
  uint8_t s = c.killed() ? 1 : running ? 2 : 0;
  if (s == shown) return;
  shown = s;
  digitalWrite(LED_RED, s == 1 ? LOW : HIGH);
  digitalWrite(LED_GREEN, s == 2 ? LOW : HIGH);
}

}  // namespace mp
