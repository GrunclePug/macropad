#include "schema.h"

namespace mp {
namespace {

const char* const BUTTONS[] = {"Left", "Right", "Middle", nullptr};
const char* const HOLD_BUTTONS[] = {"Left", "Right", "Both", nullptr};

void clamp_min_max(int32_t* v, int lo, int hi) {
  if (v[hi] < v[lo]) v[hi] = v[lo];
}

const ParamDef CLICK[] = {
    {"min", "Min", P_NUMBER, 20, 5000, 5, 82, "ms", nullptr},
    {"max", "Max", P_NUMBER, 20, 5000, 5, 103, "ms", nullptr},
    {"btn", "Button", P_CHOICE, 0, 2, 1, 0, "", BUTTONS},
};
void clamp_click(int32_t* v) { clamp_min_max(v, 0, 1); }

const ParamDef JITTER[] = {
    {"dist", "Distance", P_NUMBER, 10, 200, 10, 100, "px", nullptr},
    {"min", "Min pause", P_NUMBER, 100, 60000, 100, 4000, "ms", nullptr},
    {"max", "Max pause", P_NUMBER, 100, 60000, 100, 7000, "ms", nullptr},
};
void clamp_jitter(int32_t* v) { clamp_min_max(v, 1, 2); }

const ParamDef SPAM[] = {
    {"key", "Key", P_KEY, 0x04, 0x7F, 1, 0x2C, "", nullptr},
    {"min", "Min", P_NUMBER, 20, 10000, 10, 202, "ms", nullptr},
    {"max", "Max", P_NUMBER, 20, 10000, 10, 305, "ms", nullptr},
    {"hold", "Hold", P_NUMBER, 10, 500, 10, 50, "ms", nullptr},
};
void clamp_spam(int32_t* v) { clamp_min_max(v, 1, 2); }

const ParamDef HOLD[] = {
    {"btn", "Button", P_CHOICE, 0, 2, 1, 0, "", HOLD_BUTTONS},
};
void clamp_none(int32_t*) {}

const ToolSchema SCHEMAS[TOOL_KINDS] = {
    {TK_CLICK, "click", "Autoclicker", 3, CLICK, clamp_click},
    {TK_JITTER, "jitter", "Jitter", 3, JITTER, clamp_jitter},
    {TK_SPAM, "spam", "Keyspam", 4, SPAM, clamp_spam},
    {TK_HOLD, "hold", "Mouse hold", 1, HOLD, clamp_none},
};

}  // namespace

const ToolSchema* tool_schema(uint8_t kind) {
  return kind >= 1 && kind <= TOOL_KINDS ? &SCHEMAS[kind - 1] : nullptr;
}

const KeyName KEY_NAMES[] = {
    {0x04, "A"}, {0x05, "B"}, {0x06, "C"}, {0x07, "D"}, {0x08, "E"}, {0x09, "F"}, {0x0A, "G"},
    {0x0B, "H"}, {0x0C, "I"}, {0x0D, "J"}, {0x0E, "K"}, {0x0F, "L"}, {0x10, "M"}, {0x11, "N"},
    {0x12, "O"}, {0x13, "P"}, {0x14, "Q"}, {0x15, "R"}, {0x16, "S"}, {0x17, "T"}, {0x18, "U"},
    {0x19, "V"}, {0x1A, "W"}, {0x1B, "X"}, {0x1C, "Y"}, {0x1D, "Z"},
    {0x1E, "1"}, {0x1F, "2"}, {0x20, "3"}, {0x21, "4"}, {0x22, "5"}, {0x23, "6"}, {0x24, "7"},
    {0x25, "8"}, {0x26, "9"}, {0x27, "0"},
    {0x28, "Enter"}, {0x29, "Esc"}, {0x2A, "Backspace"}, {0x2B, "Tab"}, {0x2C, "Space"},
    {0x3A, "F1"}, {0x3B, "F2"}, {0x3C, "F3"}, {0x3D, "F4"}, {0x3E, "F5"}, {0x3F, "F6"},
    {0x40, "F7"}, {0x41, "F8"}, {0x42, "F9"}, {0x43, "F10"}, {0x44, "F11"}, {0x45, "F12"},
    {0x68, "F13"}, {0x69, "F14"}, {0x6A, "F15"}, {0x6B, "F16"}, {0x6C, "F17"}, {0x6D, "F18"},
    {0x6E, "F19"}, {0x6F, "F20"}, {0x70, "F21"}, {0x71, "F22"}, {0x72, "F23"}, {0x73, "F24"},
};
const uint8_t KEY_NAMES_COUNT = sizeof KEY_NAMES / sizeof KEY_NAMES[0];

int key_index(uint8_t usage) {
  for (uint8_t i = 0; i < KEY_NAMES_COUNT; i++)
    if (KEY_NAMES[i].usage == usage) return i;
  return -1;
}

const char* key_name(uint8_t usage) {
  int i = key_index(usage);
  return i < 0 ? nullptr : KEY_NAMES[i].name;
}

}  // namespace mp
