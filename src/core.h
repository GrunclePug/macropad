// Screen state, menu, killswitch, USB link and the tool lifecycle. Pure: no
// Arduino headers.
#pragma once

#include "event.h"
#include "hid.h"
#include "input.h"
#include "keymap.h"
#include "sched.h"
#include "tool.h"

namespace mp {

enum class Role : uint8_t { None, Kill, Nav, EncPush, Bind };

struct ButtonDef {
  Role role;
  Binding bind;  // Role::Bind only
};

struct Entry {
  bool tool;      // false: keymap
  uint8_t index;  // into keymaps or tools
};

struct Variant {
  const char* name;
  uint8_t keys;
  bool encoder;
  uint8_t detent;
  const Keymap* keymaps;
  uint8_t nkeymaps;
  Tool* const* tools;
  uint8_t ntools;
  const Entry* entries;  // menu order; the first is shown at boot
  uint8_t nentries;
  ButtonDef buttons[MAX_BUTTONS];
  uint8_t nbuttons;
};

class Core {
 public:
  void begin(const Variant& v, Input& in, Arbiter& hid, Rng& rng);

  // USB state, every loop. `disturbed` is a latched loss or re-enumeration
  // seen by the USB event handler since the last call.
  void link(bool up, bool disturbed);
  void handle(const Event& e, uint32_t now);
  // Lifecycle edges, tool ticks and the keymap's output. Call after handle().
  void update(uint32_t now);

  // Settings: dirty parameters, per tool, as a bit per parameter.
  uint16_t dirty(uint8_t tool) const { return dirty_[tool]; }
  void clean(uint8_t tool, uint16_t bits) { dirty_[tool] &= ~bits; }
  bool any_dirty() const;
  // Keymaps without a legend (osu) have a "live keys" option: show which keys
  // are down, or keep the screen static for the lowest latency.
  bool live(uint8_t keymap) const { return live_[keymap]; }
  void set_live(uint8_t keymap, bool on) { live_[keymap] = on; }
  uint8_t live_dirty() const { return live_dirty_; }
  void clean_live(uint8_t bits) { live_dirty_ &= ~bits; }
  uint32_t last_edit() const { return last_edit_; }
  bool take_flush();  // a keymap is about to go live: write settings now

  // A short message in place of the screen body (boot, "All tools off").
  void notice(const char* text, uint32_t now, uint32_t ms);

  // View, for the display and LED.
  const Variant& variant() const { return *v_; }
  bool in_menu() const { return in_menu_; }
  uint8_t cursor() const { return cursor_; }
  uint8_t shown() const { return shown_; }
  bool keymap_live() const { return !in_menu_ && !v_->entries[shown_].tool; }
  uint8_t focus() const { return focus_; }
  bool killed() const { return killed_; }
  bool usb_up() const { return usb_up_; }
  bool enabled(uint8_t t) const { return enabled_[t]; }
  uint8_t held_keys() const { return killed_ ? 0 : held_; }  // keymap keys being sent
  bool active(uint8_t t) const { return active_[t]; }
  const char* banner(uint32_t now) const {
    return banner_until_ && !due(now, banner_until_) ? banner_ : nullptr;
  }
  const char* entry_name(uint8_t e) const;
  uint32_t version() const { return version_; }

 private:
  void engage_kill();
  void disengage_kill();
  void disable_all(uint32_t now);
  void owner_change();
  void open_menu();
  void open(uint8_t entry);
  void back();
  void key_event(const Event& e);
  void button_event(const Event& e, uint32_t now);
  void turn(int8_t delta);
  void edit(int32_t steps);
  void fire(const Binding& b);  // one-shot binding (encoder, toggle)
  Tool* shown_tool() const;
  bool has_kill() const;

  const Variant* v_ = nullptr;
  Input* in_ = nullptr;
  Arbiter* hid_ = nullptr;
  Rng* rng_ = nullptr;

  Output keymap_out_;  // the shown keymap's held keys
  Output button_out_;  // small buttons bound to keys (Role::Bind)
  uint8_t held_ = 0;         // keys pressed on the current keymap screen
  uint8_t button_held_ = 0;  // Role::Bind buttons down

  bool in_menu_ = false;
  uint8_t shown_ = 0, cursor_ = 0, focus_ = 0;

  bool killed_ = false;
  bool kill_press_engaged_ = false;
  uint32_t kill_press_gen_ = 0;

  bool link_init_ = false;
  bool usb_up_ = false;
  uint32_t link_gen_ = 0;

  bool enabled_[MAX_TOOLS] = {};
  bool active_[MAX_TOOLS] = {};
  uint16_t dirty_[MAX_TOOLS] = {};
  bool live_[MAX_KEYMAPS] = {};
  uint8_t live_dirty_ = 0;
  uint32_t last_edit_ = 0;
  bool flush_ = false;

  const char* banner_ = "";
  uint32_t banner_until_ = 0;
  uint32_t now_ = 0;
  uint32_t version_ = 1;
};

}  // namespace mp
