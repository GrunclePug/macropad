// Sources of the shared test vectors (test/vectors/), used by the generator
// and the C++ tests. The Rust client's tests read the same files.
#pragma once

#include <string.h>

#include <string>
#include <vector>

#include "../src/config.h"

namespace vec {

using namespace mp;

inline CfgBinding b(uint8_t kind, uint8_t mods, uint8_t usage, const char* label) {
  CfgBinding x{kind, mods, usage, {}};
  strncpy(x.label, label, 8);
  return x;
}

inline uint8_t mod_all() { return 0xA5; }

// Every field set and distinct, within valid limits.
inline void config_full(Config& c) {
  memset(&c, 0, sizeof c);
  c.key_count = 8;
  for (uint8_t i = 0; i < 8; i++) c.key_pins[i] = 15 - i;  // A7 down to D9
  c.buttons[0].pin = 0;  // D2
  c.buttons[0].role = ROLE_BIND;
  c.buttons[0].bind = b(BK_KEY, mod_all(), 0x73, "F24");
  c.buttons[1].pin = 1;  // D3
  c.buttons[1].role = ROLE_NAV;
  c.enc_flags = ENC_ENABLED;
  c.enc_pins[0] = 2;
  c.enc_pins[1] = 3;
  c.enc_pins[2] = 4;
  c.detent = 2;
  c.release_ms = 7;
  c.set_count = 6;
  const char* ids[] = {"a1", "b22", "c333", "d4444", "e55555", "f6"};
  const char* names[] = {"First", "Second set", "Third!", "4th: mods", "Fifth #5", "Sixth set 12"};
  for (uint8_t s = 0; s < 6; s++) {
    CfgSet& set = c.sets[s];
    strncpy(set.id, ids[s], 6);
    strncpy(set.name, names[s], 12);
    set.flags = (uint8_t)(s % 4);
    for (uint8_t k = 0; k < 8; k++) {
      uint8_t n = (uint8_t)(s * 11 + k);
      switch (n % 4) {
        case 0: set.keys[k] = b(BK_KEY, (uint8_t)(n * 7), (uint8_t)(0x04 + n), "k"); break;
        case 1: set.keys[k] = b(BK_CONSUMER, 0, (uint8_t)(n % 7), "cons"); break;
        case 2: set.keys[k] = b(BK_TOGGLE, 0, (uint8_t)(1 + n % 4), "toggle!"); break;
        default: set.keys[k] = b(BK_NONE, 0, 0, ""); break;
      }
    }
    set.enc[0] = b(BK_CONSUMER, 0, 0, "Vol+");
    set.enc[1] = b(BK_CONSUMER, 0, 1, "Vol-");
    set.enc[2] = b(BK_KEY, 0x01, 0x68, "Ctl F13");
  }
  c.tool_count = 4;
  c.tool_kinds[0] = TK_HOLD;
  c.tool_kinds[1] = TK_SPAM;
  c.tool_kinds[2] = TK_CLICK;
  c.tool_kinds[3] = TK_JITTER;
  int32_t click[] = {40, 300, 2}, jitter[] = {150, 1000, 9000}, spam[] = {0x6E, 30, 9990, 20}, hold[] = {1};
  memcpy(c.params[0], click, sizeof click);
  memcpy(c.params[1], jitter, sizeof jitter);
  memcpy(c.params[2], spam, sizeof spam);
  memcpy(c.params[3], hold, sizeof hold);
  c.selected = 5;
}

struct Invalid {
  std::string name;
  std::vector<uint8_t> rec;
  uint8_t code;
  uint16_t offset;
};

inline void recrc(std::vector<uint8_t>& r) {
  memset(&r[12], 0, 4);
  uint32_t c = crc32(r.data(), r.size());
  for (int i = 0; i < 4; i++) r[12 + i] = (uint8_t)(c >> (8 * i));
}

inline std::vector<uint8_t> enc(const Config& c) {
  std::vector<uint8_t> r(CFG_SIZE);
  config_encode(c, r.data());
  return r;
}

// One case per rule: a valid record with one field broken (CRC fixed up).
inline std::vector<Invalid> invalid_cases() {
  Config d;
  config_default(d);
  std::vector<Invalid> out;
  auto add = [&](const char* name, std::vector<uint8_t> r, uint8_t code, uint16_t off, bool fix = true) {
    if (fix) recrc(r);
    out.push_back(Invalid{name, r, code, off});
  };
  auto def = [&]() { return enc(d); };
  std::vector<uint8_t> r;
  r = def(); r[0] = 'X'; add("magic", r, E_MAGIC, 0, false);
  r = def(); r[4] = 2; add("major", r, E_VERSION, 4);
  r = def(); r[5] = 1; add("newer minor", r, E_VERSION, 5);
  r = def(); r[6] = 0x4A; add("length", r, E_LENGTH, 6);
  r = def(); r[8] = 1; add("header reserved", r, E_RESERVED, 8);
  r = def(); r[12] ^= 1; add("crc", r, E_CRC, 12, false);
  r = def(); r[16] = 0; add("key count", r, E_COUNT, 16);
  r = def(); r[17] = 16; add("key pin", r, E_PIN, 17);
  r = def(); r[21] = 6; add("unused key pin", r, E_UNUSED, 21);
  r = def(); r[25] = PIN_NONE; add("button pin", r, E_PIN, 25);
  r = def(); r[26] = 4; add("button role", r, E_ROLE, 26);
  r = def(); r[27] = 1; add("button reserved", r, E_RESERVED, 27);
  r = def(); r[28] = 1; add("binding on a non-bind button", r, E_UNUSED, 28);
  r = def(); r[57] = 2; add("encoder flags", r, E_FLAGS, 57);
  r = def(); r[58] = 0; add("disabled encoder pin", r, E_UNUSED, 58);
  r = def(); r[61] = 1; add("disabled encoder detent", r, E_ENCODER, 61);
  r = def(); r[62] = 0; add("release debounce", r, E_DEBOUNCE, 62);
  r = def(); r[63] = 1; add("hardware reserved", r, E_RESERVED, 63);
  r = def(); r[67] = 0; add("set count", r, E_COUNT, 67);
  r = def(); r[68] = 0; add("empty set id", r, E_ID, 68);
  r = def(); r[68] = 'O'; add("set id charset", r, E_ID, 68);
  r = def(); r[75] = 0x07; add("set name charset", r, E_STRING, 75);
  r = def(); r[88] = 4; add("set flags", r, E_FLAGS, 88);
  r = def(); r[89] = 1; add("set reserved", r, E_RESERVED, 89);
  r = def(); r[90] = 4; add("binding kind", r, E_BINDING, 90);
  r = def(); r[92] = 0x03; add("key usage", r, E_BINDING, 92);
  r = def(); r[93] = 1; add("binding reserved", r, E_RESERVED, 93);
  r = def(); r[94] = 0x01; add("label charset", r, E_STRING, 94);
  r = def(); r[429] = 1; add("consumer mods", r, E_BINDING, 429);
  r = def(); r[233] = 1; add("set tail reserved", r, E_RESERVED, 233);
  r = def(); r[575] = 1; add("unused set slot", r, E_UNUSED, 575);
  r = def(); r[1082] = 5; add("tool count", r, E_COUNT, 1082);
  r = def(); r[1083] = 9; add("tool kind", r, E_TOOLKIND, 1083);
  r = def(); r[1087] = 1; add("unused tool kind", r, E_UNUSED, 1087);
  r = def(); r[1091] = 1; r[1092] = 0; add("param range", r, E_PARAM, 1091);
  r = def(); r[1155] = 0x2D; add("key param not named", r, E_PARAM, 1155);
  r = def(); r[1103] = 1; add("unused param word", r, E_UNUSED, 1103);
  r = def(); r[1219] = 1; add("unused param slot", r, E_UNUSED, 1219);
  r = def(); r[1347] = 1; add("tools reserved", r, E_RESERVED, 1347);
  r = def(); r[1352] = 1; add("state reserved", r, E_RESERVED, 1352);
  {
    Config c = d;
    c.params[0][0] = 500;
    c.params[0][1] = 100;
    add("clamp min > max", enc(c), E_CLAMP, 1095);
  }
  {
    Config c = d;
    c.key_pins[1] = c.key_pins[0];
    add("duplicate pin", enc(c), E_DUP_PIN, 18);
  }
  {
    Config c = d;
    strcpy(c.sets[1].id, "osu");
    add("duplicate set id", enc(c), E_DUP_SET, 237);
  }
  {
    Config c = d;
    c.tool_kinds[1] = TK_CLICK;
    add("duplicate tool kind", enc(c), E_DUP_TOOL, 1084);
  }
  {
    Config c = d;
    c.tool_count = 1;
    c.tool_kinds[1] = c.tool_kinds[2] = c.tool_kinds[3] = 0;
    add("toggle target not shown", enc(c), E_TOGGLE, 68 + 169 + 22 + 3 * 13);
  }
  {
    Config c = d;
    c.selected = 3;
    add("selected set", enc(c), E_SELECTED, 1351);
  }
  {
    Config c = d;
    c.buttons[1].role = ROLE_NONE;
    c.buttons[1].pin = PIN_NONE;
    add("no nav", enc(c), E_NO_NAV, 16);
  }
  {
    Config c = d;
    c.buttons[0].role = ROLE_NAV;
    add("two nav", enc(c), E_TWO_NAV, 42);
  }
  {
    Config c = d;
    c.key_count = 3;
    c.key_pins[3] = PIN_NONE;
    add("too few controls", enc(c), E_CONTROLS, 16);
  }
  return out;
}

inline std::string hex(const std::vector<uint8_t>& r) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (uint8_t x : r) {
    s += d[x >> 4];
    s += d[x & 15];
  }
  return s;
}

}  // namespace vec
