// Input events and shared limits. Pure: no Arduino headers.
#pragma once

#include <stdint.h>

namespace mp {

constexpr uint8_t MAX_KEYS = 8;     // main key switches
constexpr uint8_t MAX_BUTTONS = 3;  // small buttons plus the encoder push
constexpr uint8_t MAX_TOOLS = 8;
constexpr uint8_t MAX_KEYMAPS = 6;

enum class Ev : uint8_t {
  Press,    // key or button went down
  Release,  // key or button came up
  Repeat,   // key still held (fixed rate)
  Tap,      // button released before the hold threshold
  Hold,     // button held past the threshold (fires once, while held)
  Turn,     // encoder detent, delta is +1 or -1
};

enum class Src : uint8_t { Key, Button, Encoder };

struct Event {
  Ev type;
  Src src;
  uint8_t index;  // key or button index
  int8_t delta;   // Turn only
};

// Wrap-safe "now is at or after t" for millisecond and microsecond clocks.
inline bool due(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

}  // namespace mp
