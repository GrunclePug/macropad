#include "pins.h"

#include <Arduino.h>

namespace mp {

void pins_begin() {
  for (uint8_t i = 0; i < VARIANT.keys; i++) pinMode(PINS.keys[i], INPUT_PULLUP);
  for (uint8_t i = 0; i < VARIANT.nbuttons; i++) pinMode(PINS.buttons[i], INPUT_PULLUP);
  if (VARIANT.encoder) {
    pinMode(PINS.enc_a, INPUT_PULLUP);
    pinMode(PINS.enc_b, INPUT_PULLUP);
  }
}

void pins_read(uint8_t& keys, uint8_t& buttons, uint8_t& enc) {
  keys = buttons = enc = 0;
  for (uint8_t i = 0; i < VARIANT.keys; i++)
    if (digitalRead(PINS.keys[i]) == LOW) keys |= 1u << i;
  for (uint8_t i = 0; i < VARIANT.nbuttons; i++)
    if (digitalRead(PINS.buttons[i]) == LOW) buttons |= 1u << i;
  if (VARIANT.encoder)
    enc = (digitalRead(PINS.enc_a) == HIGH ? 2 : 0) | (digitalRead(PINS.enc_b) == HIGH ? 1 : 0);
}

void pins_drive(uint8_t pin, bool pressed) {
  // Input stays enabled in open-drain mode, so digitalRead() sees the pad.
  static uint64_t configured = 0;
  if (pin < 64 && !(configured & (1ull << pin))) {
    digitalWrite(pin, HIGH);
    pinMode(pin, OUTPUT_OPEN_DRAIN | PULLUP);
    configured |= 1ull << pin;
  }
  digitalWrite(pin, pressed ? LOW : HIGH);
}

}  // namespace mp
