// The config record: one pad's whole setup, v1.0, exactly 1355 bytes.
// Pure: no Arduino headers. Layout, rules and error codes: docs/protocol.md.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tools/schema.h"

namespace mp {

constexpr uint16_t CFG_SIZE = 1355;
constexpr uint8_t CFG_MAJOR = 1, CFG_MINOR = 0;
constexpr uint8_t CFG_KEYS = 8, CFG_SETS = 6, CFG_TOOLS = 8, CFG_BUTTONS = 2;
constexpr uint8_t PIN_CODES = 16;  // 0-10 D2-D12, 11 A0, 12 A1, 13 A3, 14 A6, 15 A7
constexpr uint8_t PIN_NONE = 0xFF;

// Section offsets.
constexpr uint16_t OFF_HARDWARE = 16, OFF_SETS = 67, OFF_TOOLS = 1082, OFF_STATE = 1351;
constexpr uint16_t SET_SIZE = 169, BINDING_SIZE = 13;

enum CfgRole : uint8_t { ROLE_NONE = 0, ROLE_KILL = 1, ROLE_NAV = 2, ROLE_BIND = 3 };
enum BindKind : uint8_t { BK_NONE = 0, BK_KEY = 1, BK_CONSUMER = 2, BK_TOGGLE = 3 };
enum SetFlags : uint8_t { SET_LEGEND = 1, SET_LIVE = 2 };
enum EncFlags : uint8_t { ENC_ENABLED = 1 };

// Error codes, frozen and append-only (shared with the protocol).
enum ErrCode : uint8_t {
  E_OK = 0,
  E_MAGIC = 1, E_VERSION = 2, E_LENGTH = 3, E_CRC = 4, E_RESERVED = 5, E_FLAGS = 6,
  E_STRING = 7, E_ID = 8, E_COUNT = 9, E_PIN = 10, E_ROLE = 11, E_BINDING = 12,
  E_ENCODER = 13, E_DEBOUNCE = 14, E_TOOLKIND = 15, E_PARAM = 16, E_UNUSED = 17,
  E_CLAMP = 20,
  E_DUP_PIN = 30, E_DUP_SET = 31, E_DUP_TOOL = 32, E_TOGGLE = 33, E_SELECTED = 34,
  E_NO_NAV = 35, E_TWO_NAV = 36, E_TWO_KILL = 37, E_CONTROLS = 38,
  E_LINE = 100, E_MODE = 101, E_ARGS = 102, E_RANGE = 103, E_HEX = 104, E_WRITE = 105,
  E_VERIFY = 106,
};
const char* err_name(uint8_t code);

struct CfgError {
  uint8_t code = E_OK;
  uint16_t offset = 0;
};

struct CfgBinding {
  uint8_t kind, mods, usage;
  char label[9];
};
struct CfgButton {
  uint8_t pin, role;
  CfgBinding bind;
};
struct CfgSet {
  char id[7];
  char name[13];
  uint8_t flags;
  CfgBinding keys[CFG_KEYS];
  CfgBinding enc[3];  // cw, ccw, push
};

// The record in RAM. Reserved bytes aren't stored: they're always 0.
struct Config {
  uint8_t key_count;
  uint8_t key_pins[CFG_KEYS];
  CfgButton buttons[CFG_BUTTONS];
  uint8_t enc_flags;
  uint8_t enc_pins[3];  // A, B, push
  uint8_t detent;
  uint8_t release_ms;
  uint8_t set_count;
  CfgSet sets[CFG_SETS];
  uint8_t tool_count;
  uint8_t tool_kinds[CFG_TOOLS];
  int32_t params[PARAM_SLOTS][PARAM_WORDS];
  uint8_t selected;
};

uint32_t crc32(const uint8_t* data, size_t len);  // CRC-32/ISO-HDLC (zlib)

// Encode with header and CRC. `out` must hold CFG_SIZE bytes.
void config_encode(const Config& c, uint8_t* out);
// Decode without checking; run config_validate first.
void config_decode(const uint8_t* in, Config& c);
// The only validator: format, single-field rules in byte order, clamp
// identity, then cross-field rules. The first failure wins.
CfgError config_validate(const uint8_t* in, size_t len);
// The CRC stored in an encoded record's header.
uint32_t config_crc(const uint8_t* in);

// The compiled defaults: the normal build's pad, and the self-test fixture.
void config_default(Config& c);
void config_selftest(Config& c);
// The deck preset (8 keys, encoder, bound button, no tools).
void config_deck(Config& c);

}  // namespace mp
