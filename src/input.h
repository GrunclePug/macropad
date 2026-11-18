// Debouncing, tap/hold detection and encoder decoding. Pure: no Arduino headers.
//
// Keys emit Press, Release and Repeat. Buttons emit Press, Hold, Tap and
// Release. The encoder emits Turn. A press needs two equal samples in a row (a
// glitch filter) and then counts at once; a release needs the input to stay up
// for release_ms.
#pragma once

#include "event.h"

namespace mp {

struct InputConfig {
  uint8_t keys = 4;
  uint8_t buttons = 2;
  bool encoder = false;
  uint8_t detent = 4;  // encoder transitions per detent
  uint16_t release_ms = 5;
  uint16_t hold_ms = 600;
  uint16_t repeat_delay_ms = 400;
  uint16_t repeat_ms = 80;
};

class Input {
 public:
  void begin(const InputConfig& c);

  // One scan. Bit i of key_mask / button_mask is set when that input reads
  // pressed. enc_ab is the encoder's A (bit 1) and B (bit 0) levels.
  void sample(uint8_t key_mask, uint8_t button_mask, uint8_t enc_ab, uint32_t now_ms);

  bool pop(Event& e);

  // Keys that are down now emit nothing more (no Repeat, no Release) until
  // they come up; the next press is normal.
  void suppress_held();

  uint8_t keys_down() const;
  uint32_t dropped() const { return dropped_; }

 private:
  struct Deb {
    uint8_t agree = 0;
    bool down = false;
    bool releasing = false;
    uint32_t since = 0;
    uint32_t pressed_at = 0;
    uint32_t next_repeat = 0;
    bool hold_fired = false;
    bool suppressed = false;
  };
  enum Edge : uint8_t { NONE, PRESS, RELEASE };

  Edge step(Deb& d, bool raw, uint32_t now);
  void push(Ev t, Src s, uint8_t i, int8_t delta = 0);

  InputConfig cfg_;
  Deb keys_[MAX_KEYS];
  Deb buttons_[MAX_BUTTONS];
  uint8_t enc_prev_ = 0;
  int8_t enc_acc_ = 0;
  bool enc_init_ = false;

  static constexpr uint8_t QLEN = 32;
  Event q_[QLEN];
  uint8_t head_ = 0, count_ = 0;
  uint32_t dropped_ = 0;
};

}  // namespace mp
