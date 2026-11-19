// Tool schemas: each tool kind's settings, as static data. Pure.
//
// The config record stores each kind's params in slot (kind - 1), in this
// order. Kinds and param order are append-only (docs/protocol.md).
#pragma once

#include <stdint.h>

namespace mp {

enum ToolKind : uint8_t { TK_NONE = 0, TK_CLICK = 1, TK_JITTER = 2, TK_SPAM = 3, TK_HOLD = 4 };
constexpr uint8_t TOOL_KINDS = 4;   // known kinds: 1..TOOL_KINDS
constexpr uint8_t PARAM_SLOTS = 8;  // slots in the record
constexpr uint8_t PARAM_WORDS = 8;  // words per slot

enum ParamKind : uint8_t { P_NUMBER = 0, P_CHOICE = 1, P_KEY = 2 };

struct ParamDef {
  const char* id;  // short ASCII, unique within the tool
  const char* label;
  uint8_t kind;
  int32_t min, max, step, def;
  const char* unit;
  const char* const* names;  // P_CHOICE: labels, value = index
};

struct ToolSchema {
  uint8_t kind;
  const char* id;  // at most 6 bytes
  const char* name;
  uint8_t nparams;
  const ParamDef* params;
  // Fix rules across params (min <= max). Validation runs it on a copy and
  // rejects a record it would change.
  void (*clamp)(int32_t* v);
};

const ToolSchema* tool_schema(uint8_t kind);  // null for unknown kinds

// Named keys for P_KEY params, in picker order. Usages are HID keyboard
// usages; anything not in the table is shown as 0xNN.
struct KeyName {
  uint8_t usage;
  const char* name;
};
extern const KeyName KEY_NAMES[];
extern const uint8_t KEY_NAMES_COUNT;
int key_index(uint8_t usage);  // -1 if not named
const char* key_name(uint8_t usage);  // null if not named

}  // namespace mp
