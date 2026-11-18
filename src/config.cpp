#include "config.h"

#include <string.h>

namespace mp {

// --- CRC ---

uint32_t crc32(const uint8_t* d, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return c ^ 0xFFFFFFFFu;
}

const char* err_name(uint8_t code) {
  switch (code) {
    case E_OK: return "OK";
    case E_MAGIC: return "E_MAGIC";
    case E_VERSION: return "E_VERSION";
    case E_LENGTH: return "E_LENGTH";
    case E_CRC: return "E_CRC";
    case E_RESERVED: return "E_RESERVED";
    case E_FLAGS: return "E_FLAGS";
    case E_STRING: return "E_STRING";
    case E_ID: return "E_ID";
    case E_COUNT: return "E_COUNT";
    case E_PIN: return "E_PIN";
    case E_ROLE: return "E_ROLE";
    case E_BINDING: return "E_BINDING";
    case E_ENCODER: return "E_ENCODER";
    case E_DEBOUNCE: return "E_DEBOUNCE";
    case E_TOOLKIND: return "E_TOOLKIND";
    case E_PARAM: return "E_PARAM";
    case E_UNUSED: return "E_UNUSED";
    case E_CLAMP: return "E_CLAMP";
    case E_DUP_PIN: return "E_DUP_PIN";
    case E_DUP_SET: return "E_DUP_SET";
    case E_DUP_TOOL: return "E_DUP_TOOL";
    case E_TOGGLE: return "E_TOGGLE";
    case E_SELECTED: return "E_SELECTED";
    case E_NO_NAV: return "E_NO_NAV";
    case E_TWO_NAV: return "E_TWO_NAV";
    case E_TWO_KILL: return "E_TWO_KILL";
    case E_CONTROLS: return "E_CONTROLS";
    case E_LINE: return "E_LINE";
    case E_MODE: return "E_MODE";
    case E_ARGS: return "E_ARGS";
    case E_RANGE: return "E_RANGE";
    case E_HEX: return "E_HEX";
    case E_WRITE: return "E_WRITE";
    case E_VERIFY: return "E_VERIFY";
    default: return "E_UNKNOWN";
  }
}

// --- Encode / decode ---

namespace {

struct W {
  explicit W(uint8_t* out) : p(out) {}
  uint8_t* p;
  uint16_t at = 0;
  void u8(uint8_t v) { p[at++] = v; }
  void zero(uint16_t n) {
    memset(p + at, 0, n);
    at += n;
  }
  void str(const char* s, uint16_t field) {
    size_t n = strnlen(s, field - 1);
    memcpy(p + at, s, n);
    memset(p + at + n, 0, field - n);
    at += field;
  }
  void i32(int32_t v) {
    uint32_t u = (uint32_t)v;
    for (int i = 0; i < 4; i++) u8((uint8_t)(u >> (8 * i)));
  }
  void binding(const CfgBinding& b) {
    u8(b.kind);
    u8(b.mods);
    u8(b.usage);
    u8(0);
    str(b.label, 9);
  }
};

struct R {
  explicit R(const uint8_t* in) : p(in) {}
  const uint8_t* p;
  uint16_t at = 0;
  uint8_t u8() { return p[at++]; }
  void skip(uint16_t n) { at += n; }
  void str(char* s, uint16_t field) {
    memcpy(s, p + at, field);
    s[field - 1] = 0;
    at += field;
  }
  int32_t i32() {
    uint32_t u = 0;
    for (int i = 0; i < 4; i++) u |= (uint32_t)u8() << (8 * i);
    return (int32_t)u;
  }
  void binding(CfgBinding& b) {
    b.kind = u8();
    b.mods = u8();
    b.usage = u8();
    skip(1);
    str(b.label, 9);
  }
};

}  // namespace

void config_encode(const Config& c, uint8_t* out) {
  W w(out);
  w.u8('M'), w.u8('P'), w.u8('C'), w.u8('1');
  w.u8(CFG_MAJOR);
  w.u8(CFG_MINOR);
  w.u8(CFG_SIZE & 0xFF);
  w.u8(CFG_SIZE >> 8);
  w.zero(4);  // reserved
  w.zero(4);  // crc, filled below
  // Hardware
  w.u8(c.key_count);
  for (uint8_t i = 0; i < CFG_KEYS; i++) w.u8(c.key_pins[i]);
  for (uint8_t i = 0; i < CFG_BUTTONS; i++) {
    w.u8(c.buttons[i].pin);
    w.u8(c.buttons[i].role);
    w.u8(0);
    w.binding(c.buttons[i].bind);
  }
  w.u8(c.enc_flags);
  for (uint8_t i = 0; i < 3; i++) w.u8(c.enc_pins[i]);
  w.u8(c.detent);
  w.u8(c.release_ms);
  w.zero(4);
  // Key sets
  w.u8(c.set_count);
  for (uint8_t s = 0; s < CFG_SETS; s++) {
    if (s >= c.set_count) {
      w.zero(SET_SIZE);
      continue;
    }
    const CfgSet& set = c.sets[s];
    w.str(set.id, 7);
    w.str(set.name, 13);
    w.u8(set.flags);
    w.u8(0);
    for (uint8_t k = 0; k < CFG_KEYS; k++) w.binding(set.keys[k]);
    for (uint8_t k = 0; k < 3; k++) w.binding(set.enc[k]);
    w.zero(4);
  }
  // Tools
  w.u8(c.tool_count);
  for (uint8_t i = 0; i < CFG_TOOLS; i++) w.u8(c.tool_kinds[i]);
  for (uint8_t s = 0; s < PARAM_SLOTS; s++)
    for (uint8_t j = 0; j < PARAM_WORDS; j++) w.i32(c.params[s][j]);
  w.zero(4);
  // State
  w.u8(c.selected);
  w.zero(3);
  uint32_t crc = crc32(out, CFG_SIZE);  // crc field is still 0
  for (int i = 0; i < 4; i++) out[12 + i] = (uint8_t)(crc >> (8 * i));
}

uint32_t config_crc(const uint8_t* in) {
  return (uint32_t)in[12] | (uint32_t)in[13] << 8 | (uint32_t)in[14] << 16 | (uint32_t)in[15] << 24;
}

void config_decode(const uint8_t* in, Config& c) {
  memset(&c, 0, sizeof c);
  R r(in);
  r.skip(16);
  c.key_count = r.u8();
  for (uint8_t i = 0; i < CFG_KEYS; i++) c.key_pins[i] = r.u8();
  for (uint8_t i = 0; i < CFG_BUTTONS; i++) {
    c.buttons[i].pin = r.u8();
    c.buttons[i].role = r.u8();
    r.skip(1);
    r.binding(c.buttons[i].bind);
  }
  c.enc_flags = r.u8();
  for (uint8_t i = 0; i < 3; i++) c.enc_pins[i] = r.u8();
  c.detent = r.u8();
  c.release_ms = r.u8();
  r.skip(4);
  c.set_count = r.u8();
  for (uint8_t s = 0; s < CFG_SETS; s++) {
    CfgSet& set = c.sets[s];
    r.str(set.id, 7);
    r.str(set.name, 13);
    set.flags = r.u8();
    r.skip(1);
    for (uint8_t k = 0; k < CFG_KEYS; k++) r.binding(set.keys[k]);
    for (uint8_t k = 0; k < 3; k++) r.binding(set.enc[k]);
    r.skip(4);
  }
  c.tool_count = r.u8();
  for (uint8_t i = 0; i < CFG_TOOLS; i++) c.tool_kinds[i] = r.u8();
  for (uint8_t s = 0; s < PARAM_SLOTS; s++)
    for (uint8_t j = 0; j < PARAM_WORDS; j++) c.params[s][j] = r.i32();
  r.skip(4);
  c.selected = r.u8();
}

// --- Validation ---

namespace {

struct V {
  const uint8_t* p;
  CfgError err;
  bool fail(uint8_t code, uint16_t off) {
    if (!err.code) {
      err.code = code;
      err.offset = off;
    }
    return false;
  }
  bool zeros(uint16_t off, uint16_t n, uint8_t code) {
    for (uint16_t i = 0; i < n; i++)
      if (p[off + i]) return fail(code, off + i);
    return true;
  }
  // A NUL-padded string field: `max` chars at most, chars accepted by `ok`.
  template <class F>
  bool str(uint16_t off, uint16_t field, bool nonempty, uint8_t code, F ok) {
    uint16_t n = 0;
    while (n < field && p[off + n]) n++;
    if (n == field) return fail(code, off + field - 1);  // no NUL
    if (nonempty && n == 0) return fail(code, off);
    for (uint16_t i = 0; i < n; i++)
      if (!ok(p[off + i])) return fail(code, off + i);
    return zeros(off + n, field - n, code);
  }
  static bool printable(uint8_t c) { return c >= 0x20 && c <= 0x7E; }
  static bool idchar(uint8_t c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

  bool binding(uint16_t off) {
    uint8_t kind = p[off], mods = p[off + 1], usage = p[off + 2];
    if (kind > BK_TOGGLE) return fail(E_BINDING, off);
    if (kind == BK_NONE) return zeros(off, BINDING_SIZE, E_BINDING);
    if (kind != BK_KEY && mods) return fail(E_BINDING, off + 1);
    if (kind == BK_KEY && (usage < 0x04 || usage > 0x7F)) return fail(E_BINDING, off + 2);
    if (kind == BK_CONSUMER && usage > 6) return fail(E_BINDING, off + 2);
    if (kind == BK_TOGGLE && (usage < 1 || usage > TOOL_KINDS)) return fail(E_BINDING, off + 2);
    if (p[off + 3]) return fail(E_RESERVED, off + 3);
    return str(off + 4, 9, false, E_STRING, printable);
  }
};

}  // namespace

CfgError config_validate(const uint8_t* p, size_t len) {
  V v{p, {}};
  // Header
  if (len < 4 || memcmp(p, "MPC1", 4) != 0) {
    v.fail(E_MAGIC, 0);
    return v.err;
  }
  if (len < 6 || p[4] != CFG_MAJOR) {
    v.fail(E_VERSION, 4);
    return v.err;
  }
  if (p[5] > CFG_MINOR) {
    v.fail(E_VERSION, 5);
    return v.err;
  }
  if (len < CFG_SIZE || (p[6] | p[7] << 8) != CFG_SIZE) {
    v.fail(E_LENGTH, 6);
    return v.err;
  }
  if (!v.zeros(8, 4, E_RESERVED)) return v.err;
  {
    uint8_t tmp[CFG_SIZE];
    memcpy(tmp, p, CFG_SIZE);
    memset(tmp + 12, 0, 4);
    if (crc32(tmp, CFG_SIZE) != config_crc(p)) {
      v.fail(E_CRC, 12);
      return v.err;
    }
  }

  // Hardware, byte order.
  uint8_t keys = p[16];
  if (keys < 1 || keys > CFG_KEYS) return v.fail(E_COUNT, 16), v.err;
  for (uint8_t i = 0; i < CFG_KEYS; i++) {
    uint8_t pin = p[17 + i];
    if (i < keys && pin >= PIN_CODES) return v.fail(E_PIN, 17 + i), v.err;
    if (i >= keys && pin != PIN_NONE) return v.fail(E_UNUSED, 17 + i), v.err;
  }
  for (uint8_t b = 0; b < CFG_BUTTONS; b++) {
    uint16_t off = 25 + b * 16;
    uint8_t pin = p[off], role = p[off + 1];
    if (role != ROLE_NONE && pin >= PIN_CODES) return v.fail(E_PIN, off), v.err;
    if (role == ROLE_NONE && pin != PIN_NONE) return v.fail(E_UNUSED, off), v.err;
    if (role > ROLE_BIND) return v.fail(E_ROLE, off + 1), v.err;
    if (p[off + 2]) return v.fail(E_RESERVED, off + 2), v.err;
    if (role == ROLE_BIND) {
      if (p[off + 3] == BK_NONE) return v.fail(E_BINDING, off + 3), v.err;
      if (!v.binding(off + 3)) return v.err;
    } else if (!v.zeros(off + 3, BINDING_SIZE, E_UNUSED)) {
      return v.err;
    }
  }
  uint8_t ef = p[57];
  if (ef & ~ENC_ENABLED) return v.fail(E_FLAGS, 57), v.err;
  bool enc = ef & ENC_ENABLED;
  for (uint8_t i = 0; i < 3; i++) {
    uint8_t pin = p[58 + i];
    if (!enc && pin != PIN_NONE) return v.fail(E_UNUSED, 58 + i), v.err;
    if (enc && pin >= PIN_CODES && !(i == 2 && pin == PIN_NONE)) return v.fail(E_PIN, 58 + i), v.err;
  }
  uint8_t det = p[61];
  if (enc ? !(det == 1 || det == 2 || det == 4) : det != 0) return v.fail(E_ENCODER, 61), v.err;
  if (p[62] < 1 || p[62] > 30) return v.fail(E_DEBOUNCE, 62), v.err;
  if (!v.zeros(63, 4, E_RESERVED)) return v.err;

  // Key sets
  uint8_t sets = p[67];
  if (sets < 1 || sets > CFG_SETS) return v.fail(E_COUNT, 67), v.err;
  for (uint8_t s = 0; s < CFG_SETS; s++) {
    uint16_t off = 68 + s * SET_SIZE;
    if (s >= sets) {
      if (!v.zeros(off, SET_SIZE, E_UNUSED)) return v.err;
      continue;
    }
    if (!v.str(off, 7, true, E_ID, V::idchar)) return v.err;
    if (!v.str(off + 7, 13, true, E_STRING, V::printable)) return v.err;
    if (p[off + 20] & ~(SET_LEGEND | SET_LIVE)) return v.fail(E_FLAGS, off + 20), v.err;
    if (p[off + 21]) return v.fail(E_RESERVED, off + 21), v.err;
    for (uint8_t k = 0; k < 11; k++)
      if (!v.binding(off + 22 + k * BINDING_SIZE)) return v.err;
    if (!v.zeros(off + 165, 4, E_RESERVED)) return v.err;
  }

  // Tools
  uint8_t tools = p[1082];
  if (tools > TOOL_KINDS) return v.fail(E_COUNT, 1082), v.err;
  for (uint8_t i = 0; i < CFG_TOOLS; i++) {
    uint8_t k = p[1083 + i];
    if (i < tools && !tool_schema(k)) return v.fail(E_TOOLKIND, 1083 + i), v.err;
    if (i >= tools && k) return v.fail(E_UNUSED, 1083 + i), v.err;
  }
  int32_t params[PARAM_SLOTS][PARAM_WORDS];
  for (uint8_t s = 0; s < PARAM_SLOTS; s++)
    for (uint8_t j = 0; j < PARAM_WORDS; j++) {
      uint16_t off = 1091 + (s * PARAM_WORDS + j) * 4;
      int32_t x = (int32_t)((uint32_t)p[off] | (uint32_t)p[off + 1] << 8 | (uint32_t)p[off + 2] << 16 |
                            (uint32_t)p[off + 3] << 24);
      params[s][j] = x;
      const ToolSchema* ts = tool_schema(s + 1);
      if (!ts || j >= ts->nparams) {
        if (x) return v.fail(E_UNUSED, off), v.err;
        continue;
      }
      const ParamDef& d = ts->params[j];
      bool bad;
      if (d.kind == P_KEY)
        bad = x < 0 || x > 255 || key_index((uint8_t)x) < 0;  // named keys only
      else
        bad = x < d.min || x > d.max;
      if (bad) return v.fail(E_PARAM, off), v.err;
    }
  if (!v.zeros(1347, 4, E_RESERVED)) return v.err;
  // State
  if (!v.zeros(1352, 3, E_RESERVED)) return v.err;

  // Clamp identity over slots 0..3.
  for (uint8_t s = 0; s < TOOL_KINDS; s++) {
    int32_t copy[PARAM_WORDS];
    memcpy(copy, params[s], sizeof copy);
    tool_schema(s + 1)->clamp(copy);
    for (uint8_t j = 0; j < PARAM_WORDS; j++)
      if (copy[j] != params[s][j]) return v.fail(E_CLAMP, 1091 + (s * PARAM_WORDS + j) * 4), v.err;
  }

  // Cross-field 1: duplicate pins (keys, buttons, encoder), later offset.
  {
    bool seen[PIN_CODES] = {};
    uint16_t offs[CFG_KEYS + CFG_BUTTONS + 3];
    uint8_t n = 0;
    for (uint8_t i = 0; i < keys; i++) offs[n++] = 17 + i;
    for (uint8_t b = 0; b < CFG_BUTTONS; b++)
      if (p[26 + b * 16] != ROLE_NONE) offs[n++] = 25 + b * 16;
    if (enc)
      for (uint8_t i = 0; i < 3; i++)
        if (p[58 + i] != PIN_NONE) offs[n++] = 58 + i;
    for (uint8_t i = 0; i < n; i++) {
      uint8_t pin = p[offs[i]];
      if (seen[pin]) return v.fail(E_DUP_PIN, offs[i]), v.err;
      seen[pin] = true;
    }
  }
  // 2: duplicate set ids.
  for (uint8_t a = 1; a < sets; a++)
    for (uint8_t b = 0; b < a; b++)
      if (!memcmp(p + 68 + a * SET_SIZE, p + 68 + b * SET_SIZE, 7)) return v.fail(E_DUP_SET, 68 + a * SET_SIZE), v.err;
  // 3: duplicate tool kinds.
  for (uint8_t a = 1; a < tools; a++)
    for (uint8_t b = 0; b < a; b++)
      if (p[1083 + a] == p[1083 + b]) return v.fail(E_DUP_TOOL, 1083 + a), v.err;
  // 4: toggle targets shown, in byte order.
  {
    uint16_t offs[CFG_BUTTONS + CFG_SETS * 11];
    uint8_t n = 0;
    for (uint8_t b = 0; b < CFG_BUTTONS; b++) offs[n++] = 28 + b * 16;
    for (uint8_t s = 0; s < sets; s++)
      for (uint8_t k = 0; k < 11; k++) offs[n++] = 68 + s * SET_SIZE + 22 + k * BINDING_SIZE;
    for (uint8_t i = 0; i < n; i++) {
      if (p[offs[i]] != BK_TOGGLE) continue;
      bool shown = false;
      for (uint8_t t = 0; t < tools; t++) shown |= p[1083 + t] == p[offs[i] + 2];
      if (!shown) return v.fail(E_TOGGLE, offs[i]), v.err;
    }
  }
  // 5: selected set.
  if (p[1351] >= sets) return v.fail(E_SELECTED, 1351), v.err;
  // 6: controls.
  {
    uint8_t nav = 0, kill = 0;
    uint16_t nav2 = 0, kill2 = 0;
    for (uint8_t b = 0; b < CFG_BUTTONS; b++) {
      uint8_t role = p[26 + b * 16];
      if (role == ROLE_NAV && ++nav == 2) nav2 = 26 + b * 16;
      if (role == ROLE_KILL && ++kill == 2) kill2 = 26 + b * 16;
    }
    if (nav == 0) return v.fail(E_NO_NAV, 16), v.err;
    if (nav > 1) return v.fail(E_TWO_NAV, nav2), v.err;
    if (kill > 1) return v.fail(E_TWO_KILL, kill2), v.err;
    bool wheel = enc && p[60] != PIN_NONE;
    if (keys < 4 && !wheel) return v.fail(E_CONTROLS, 16), v.err;
  }
  return v.err;
}

// --- Defaults ---

namespace {

CfgBinding key(uint8_t usage, const char* label, uint8_t mods = 0) {
  CfgBinding b{BK_KEY, mods, usage, {}};
  strncpy(b.label, label, 8);
  return b;
}
CfgBinding consumer(uint8_t code, const char* label) {
  CfgBinding b{BK_CONSUMER, 0, code, {}};
  strncpy(b.label, label, 8);
  return b;
}
CfgBinding toggle(uint8_t kind, const char* label) {
  CfgBinding b{BK_TOGGLE, 0, kind, {}};
  strncpy(b.label, label, 8);
  return b;
}
void set_strings(CfgSet& s, const char* id, const char* name) {
  strncpy(s.id, id, 6);
  strncpy(s.name, name, 12);
}
void tool_defaults(Config& c) {
  for (uint8_t k = 1; k <= TOOL_KINDS; k++) {
    const ToolSchema* t = tool_schema(k);
    for (uint8_t j = 0; j < t->nparams; j++) c.params[k - 1][j] = t->params[j].def;
  }
}

}  // namespace

void config_default(Config& c) {
  memset(&c, 0, sizeof c);
  c.key_count = 4;
  for (uint8_t i = 0; i < CFG_KEYS; i++) c.key_pins[i] = i < 4 ? i : PIN_NONE;  // D2-D5
  c.buttons[0].pin = 4;  // D6
  c.buttons[0].role = ROLE_KILL;
  c.buttons[1].pin = 5;  // D7
  c.buttons[1].role = ROLE_NAV;
  c.enc_pins[0] = c.enc_pins[1] = c.enc_pins[2] = PIN_NONE;
  c.release_ms = 5;

  c.set_count = 3;
  CfgSet& osu = c.sets[0];
  set_strings(osu, "osu", "osu!mania");
  osu.flags = SET_LIVE;
  osu.keys[0] = key(0x07, "D");
  osu.keys[1] = key(0x09, "F");
  osu.keys[2] = key(0x0D, "J");
  osu.keys[3] = key(0x0E, "K");
  CfgSet& stream = c.sets[1];
  set_strings(stream, "stream", "Stream");
  stream.flags = SET_LEGEND;
  stream.keys[0] = key(0x68, "F13");
  stream.keys[1] = key(0x69, "F14");
  stream.keys[2] = toggle(TK_CLICK, "Clicker");
  stream.keys[3] = toggle(TK_JITTER, "Jitter");
  CfgSet& media = c.sets[2];
  set_strings(media, "media", "Media");
  media.flags = SET_LEGEND;
  media.keys[0] = consumer(3, "Play");
  media.keys[1] = consumer(5, "Prev");
  media.keys[2] = consumer(4, "Next");
  media.keys[3] = consumer(2, "Mute");

  c.tool_count = 4;
  c.tool_kinds[0] = TK_CLICK;
  c.tool_kinds[1] = TK_JITTER;
  c.tool_kinds[2] = TK_SPAM;
  c.tool_kinds[3] = TK_HOLD;
  tool_defaults(c);
  c.selected = 0;
}

void config_selftest(Config& c) {
  config_default(c);
  // Harmless outputs: F13-F17 and no consumer codes (mouse button 8 is a
  // compile-time override in the tools).
  CfgSet& osu = c.sets[0];
  osu.keys[0] = key(0x68, "D");
  osu.keys[1] = key(0x69, "F");
  osu.keys[2] = key(0x6A, "J");
  osu.keys[3] = key(0x6B, "K");
  CfgSet& media = c.sets[2];
  set_strings(media, "media", "Media");
  media.keys[0] = key(0x6C, "F17");
  media.keys[1] = key(0x6C, "F17");
  media.keys[2] = key(0x6C, "F17");
  media.keys[3] = key(0x6C, "F17");
  c.params[TK_JITTER - 1][0] = 10;   // 10 px
  c.params[TK_SPAM - 1][0] = 0x6C;   // F17
}

void config_deck(Config& c) {
  memset(&c, 0, sizeof c);
  c.key_count = 8;
  for (uint8_t i = 0; i < CFG_KEYS; i++) c.key_pins[i] = i;  // D2-D9
  c.buttons[0].pin = 8;  // D10
  c.buttons[0].role = ROLE_BIND;
  c.buttons[0].bind = key(0x70, "F21");
  c.buttons[1].pin = 9;  // D11
  c.buttons[1].role = ROLE_NAV;
  c.enc_flags = ENC_ENABLED;
  c.enc_pins[0] = 10;  // D12
  c.enc_pins[1] = 11;  // A0
  c.enc_pins[2] = 12;  // A1
  c.detent = 4;
  c.release_ms = 5;
  c.set_count = 1;
  CfgSet& d = c.sets[0];
  set_strings(d, "deck", "Deck");
  d.flags = SET_LEGEND;
  const char* labels[] = {"F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20"};
  for (uint8_t k = 0; k < 8; k++) d.keys[k] = key(0x68 + k, labels[k]);
  d.enc[0] = consumer(0, "Vol+");
  d.enc[1] = consumer(1, "Vol-");
  d.enc[2] = consumer(2, "Mute");
  c.tool_count = 0;
  tool_defaults(c);
  c.selected = 0;
}

}  // namespace mp
