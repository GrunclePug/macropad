// Key layouts as data. Pure: no Arduino headers.
#pragma once

#include <stdint.h>

#include "event.h"

namespace mp {

// HID keyboard usages (USB HID Usage Tables, page 0x07). Only usages below
// 0x80 fit the NKRO bitmap; modifiers go through the mods byte.
namespace key {
constexpr uint8_t A = 0x04, B = 0x05, C = 0x06, D = 0x07, E = 0x08, F = 0x09, G = 0x0A, H = 0x0B,
                  I = 0x0C, J = 0x0D, K = 0x0E, L = 0x0F, M = 0x10, N = 0x11, O = 0x12, P = 0x13,
                  Q = 0x14, R = 0x15, S = 0x16, T = 0x17, U = 0x18, V = 0x19, W = 0x1A, X = 0x1B,
                  Y = 0x1C, Z = 0x1D;
constexpr uint8_t N1 = 0x1E, N2 = 0x1F, N3 = 0x20, N4 = 0x21, N5 = 0x22, N6 = 0x23, N7 = 0x24,
                  N8 = 0x25, N9 = 0x26, N0 = 0x27;
constexpr uint8_t ENTER = 0x28, ESC = 0x29, BACKSPACE = 0x2A, TAB = 0x2B, SPACE = 0x2C;
constexpr uint8_t F1 = 0x3A, F2 = 0x3B, F3 = 0x3C, F4 = 0x3D, F5 = 0x3E, F6 = 0x3F, F7 = 0x40,
                  F8 = 0x41, F9 = 0x42, F10 = 0x43, F11 = 0x44, F12 = 0x45;
constexpr uint8_t F13 = 0x68, F14 = 0x69, F15 = 0x6A, F16 = 0x6B, F17 = 0x6C, F18 = 0x6D,
                  F19 = 0x6E, F20 = 0x6F, F21 = 0x70, F22 = 0x71, F23 = 0x72, F24 = 0x73;
}  // namespace key

namespace mod {
constexpr uint8_t LCTRL = 0x01, LSHIFT = 0x02, LALT = 0x04, LGUI = 0x08;
constexpr uint8_t RCTRL = 0x10, RSHIFT = 0x20, RALT = 0x40, RGUI = 0x80;
}  // namespace mod

// An empty Binding ({}) is NONE with no label (label may be null).
struct Binding {
  enum Kind : uint8_t { NONE, KEY, CONSUMER, TOGGLE_TOOL } kind;
  uint8_t mods;
  uint8_t usage;  // keyboard usage, consumer bit (CC_*) or tool index
  const char* label;
};

constexpr Binding bind_key(uint8_t usage, const char* label, uint8_t mods = 0) {
  return Binding{Binding::KEY, mods, usage, label};
}
constexpr Binding bind_consumer(uint8_t bit, const char* label) {
  return Binding{Binding::CONSUMER, 0, bit, label};
}
constexpr Binding bind_tool(uint8_t tool, const char* label) {
  return Binding{Binding::TOGGLE_TOOL, 0, tool, label};
}

struct Keymap {
  const char* id;
  const char* name;
  Binding keys[MAX_KEYS];
  Binding enc_cw, enc_ccw, enc_push;
  bool legend;  // show the key labels on screen
};

}  // namespace mp
