#include "runtime.h"

#include <string.h>

namespace mp {
namespace {

Binding convert(const CfgBinding& b, const Config& cfg) {
  Binding r{Binding::NONE, 0, 0, b.label};
  switch (b.kind) {
    case BK_KEY:
      r.kind = Binding::KEY;
      r.mods = b.mods;
      r.usage = b.usage;
      break;
    case BK_CONSUMER:
      r.kind = Binding::CONSUMER;
      r.usage = b.usage;
      break;
    case BK_TOGGLE:
      for (uint8_t i = 0; i < cfg.tool_count; i++)
        if (cfg.tool_kinds[i] == b.usage) {
          r.kind = Binding::TOGGLE_TOOL;
          r.usage = i;
        }
      break;
    default:
      break;
  }
  return r;
}

Role role(uint8_t r) {
  switch (r) {
    case ROLE_KILL:
      return Role::Kill;
    case ROLE_NAV:
      return Role::Nav;
    case ROLE_BIND:
      return Role::Bind;
    default:
      return Role::None;
  }
}

}  // namespace

void config_to_variant(Config& cfg, Runtime& rt) {
  memset(&rt, 0, sizeof rt);
  for (uint8_t s = 0; s < cfg.set_count; s++) {
    const CfgSet& cs = cfg.sets[s];
    Keymap& km = rt.keymaps[s];
    km.id = cs.id;
    km.name = cs.name;
    for (uint8_t k = 0; k < CFG_KEYS; k++) km.keys[k] = convert(cs.keys[k], cfg);
    km.enc_cw = convert(cs.enc[0], cfg);
    km.enc_ccw = convert(cs.enc[1], cfg);
    km.enc_push = convert(cs.enc[2], cfg);
    km.legend = cs.flags & SET_LEGEND;
  }
  for (uint8_t k = 1; k <= TOOL_KINDS; k++) tool_for_kind(k)->bind(cfg.params[k - 1]);
  for (uint8_t i = 0; i < cfg.tool_count; i++) rt.tools[i] = tool_for_kind(cfg.tool_kinds[i]);

  Variant& v = rt.v;
  v.keys = cfg.key_count;
  v.encoder = cfg.enc_flags & ENC_ENABLED;
  v.detent = v.encoder ? cfg.detent : 4;
  v.release_ms = cfg.release_ms;
  v.keymaps = rt.keymaps;
  v.nkeymaps = cfg.set_count;
  v.tools = rt.tools;
  v.ntools = cfg.tool_count;
  for (uint8_t b = 0; b < CFG_BUTTONS; b++) {
    v.buttons[b].role = role(cfg.buttons[b].role);
    v.buttons[b].bind = convert(cfg.buttons[b].bind, cfg);
    rt.button_pins[b] = cfg.buttons[b].pin;
  }
  v.nbuttons = CFG_BUTTONS;
  rt.button_pins[2] = PIN_NONE;
  if (v.encoder && cfg.enc_pins[2] != PIN_NONE) {
    v.buttons[2].role = Role::EncPush;
    rt.button_pins[2] = cfg.enc_pins[2];
    v.nbuttons = 3;
  }
  for (uint8_t k = 0; k < CFG_KEYS; k++) rt.key_pins[k] = cfg.key_pins[k];
  rt.enc_a = v.encoder ? cfg.enc_pins[0] : PIN_NONE;
  rt.enc_b = v.encoder ? cfg.enc_pins[1] : PIN_NONE;
}

}  // namespace mp
