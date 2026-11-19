#include "pins.h"

#include <Arduino.h>

namespace mp {
namespace {

// Pin codes 0-15. Excluded pins (A2 strapping, D0/D1, A4/A5 I2C, D13, the
// RGB LED and boot pins) have no code.
const uint8_t PIN_TABLE[PIN_CODES] = {D2, D3, D4, D5, D6, D7, D8, D9, D10, D11, D12, A0, A1, A3, A6, A7};

const Runtime* rt_ = nullptr;
uint16_t driven = 0;  // codes driven by the self-test

}  // namespace

uint8_t pin_arduino(uint8_t code) { return code < PIN_CODES ? PIN_TABLE[code] : 0xFF; }

static void input(uint8_t code) {
  if (code < PIN_CODES) pinMode(PIN_TABLE[code], INPUT_PULLUP);
}

void pins_begin(const Runtime& rt) {
  rt_ = &rt;
  for (uint8_t i = 0; i < rt.v.keys; i++) input(rt.key_pins[i]);
  for (uint8_t i = 0; i < rt.v.nbuttons; i++) input(rt.button_pins[i]);
  input(rt.enc_a);
  input(rt.enc_b);
}

static bool low(uint8_t code) { return code < PIN_CODES && digitalRead(PIN_TABLE[code]) == LOW; }

bool pin_low(uint8_t code) {
  if (code >= PIN_CODES) return false;
  pinMode(PIN_TABLE[code], INPUT_PULLUP);
  return digitalRead(PIN_TABLE[code]) == LOW;
}

void pins_read(uint8_t& keys, uint8_t& buttons, uint8_t& enc) {
  keys = buttons = enc = 0;
  const Runtime& rt = *rt_;
  for (uint8_t i = 0; i < rt.v.keys; i++)
    if (low(rt.key_pins[i])) keys |= 1u << i;
  for (uint8_t i = 0; i < rt.v.nbuttons; i++)
    if (low(rt.button_pins[i])) buttons |= 1u << i;
  if (rt.v.encoder) enc = (low(rt.enc_a) ? 0 : 2) | (low(rt.enc_b) ? 0 : 1);
}

void pins_drive(uint8_t code, bool pressed) {
  if (code >= PIN_CODES) return;
  uint8_t pin = PIN_TABLE[code];
  if (!(driven & (1u << code))) {
    digitalWrite(pin, HIGH);
    pinMode(pin, OUTPUT_OPEN_DRAIN | PULLUP);  // input stays enabled
    driven |= 1u << code;
  }
  digitalWrite(pin, pressed ? LOW : HIGH);
}

void pins_release_all() {
  for (uint8_t code = 0; code < PIN_CODES; code++)
    if (driven & (1u << code)) {
      digitalWrite(PIN_TABLE[code], HIGH);
      pinMode(PIN_TABLE[code], INPUT_PULLUP);
    }
  driven = 0;
}

}  // namespace mp
