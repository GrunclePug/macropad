// Screens, menu, killswitch, config mode, USB link and the tool lifecycle.
// Pure: no Arduino headers.
#pragma once

#include "config.h"
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

// Runtime tables built from the config record (see runtime.h).
struct Variant {
  uint8_t keys;
  bool encoder;
  uint8_t detent;
  uint8_t release_ms;
  const Keymap* keymaps;  // the key sets
  uint8_t nkeymaps;
  Tool* const* tools;  // shown tools, menu order
  uint8_t ntools;
  ButtonDef buttons[MAX_BUTTONS];  // slot 2 is the encoder push
  uint8_t nbuttons;
};

class Core {
 public:
  enum class Screen : uint8_t { Keys, SetList, Tool, Configure };

  // `cfg` is the running config: the core edits its selected set, live flags
  // and (through the tools' slots) tool params.
  void begin(const Variant& v, Config& cfg, Input& in, Arbiter& hid, Rng& rng);

  // USB state, every loop. `disturbed` is a latched loss or re-enumeration.
  void link(bool up, bool disturbed);
  void handle(const Event& e, uint32_t now);
  // Lifecycle edges, tool ticks and the keymap's output. Call after handle().
  void update(uint32_t now);

  // Edits to the running config made on the pad.
  bool dirty() const { return dirty_; }
  void clean() { dirty_ = false; }
  uint32_t last_edit() const { return last_edit_; }
  bool take_flush();  // a key screen is about to go live: save now

  // Config mode. The pad asks for it from its Configure entry
  // (take_config_request); the app enters and leaves it.
  bool take_config_request();
  void enter_config(bool recovery);
  void leave_config();  // cancel: back to the screen before
  bool config_mode() const { return config_; }
  bool recovery() const { return recovery_; }
  bool take_cancel();  // the pad cancelled config mode (Kill tap, Nav hold)
  void freeze() { frozen_ = true; }  // a verified commit: ignore all input

  // A short message in place of the screen body.
  void notice(const char* text, uint32_t now, uint32_t ms);
  // The Configure screen's status line, and "Default (not saved)" in the top
  // bar when the pad can't save its own edits. Set by the app.
  void set_status(const char* s) {
    if (s != status_) {
      status_ = s;
      version_++;
    }
  }
  const char* status() const { return status_; }
  void set_unsaved(bool on) {
    if (on != unsaved_) {
      unsaved_ = on;
      version_++;
    }
  }
  bool unsaved() const { return unsaved_; }

  // View, for the display and LED.
  const Variant& variant() const { return *v_; }
  const Config& config() const { return *cfg_; }
  Screen screen() const { return screen_; }
  bool in_menu() const { return in_menu_; }
  uint8_t cursor() const { return cursor_; }
  uint8_t menu_items() const { return v_->ntools + 2; }  // Keys, tools, Configure
  const char* menu_name(uint8_t i) const;
  uint8_t set() const { return set_; }
  uint8_t tool() const { return tool_; }
  uint8_t list_cursor() const { return list_cursor_; }
  uint8_t focus() const { return focus_; }
  bool keymap_live() const { return !in_menu_ && screen_ == Screen::Keys && !config_; }
  bool live(uint8_t set) const { return cfg_->sets[set].flags & SET_LIVE; }
  uint8_t held_keys() const { return killed_ || config_ ? 0 : held_; }
  bool killed() const { return killed_; }
  uint32_t link_gen() const { return link_gen_; }  // counts link losses and returns
  bool usb_up() const { return usb_up_; }
  bool enabled(uint8_t t) const { return enabled_[t]; }
  bool active(uint8_t t) const { return active_[t]; }
  const char* banner(uint32_t now) const {
    return banner_until_ && !due(now, banner_until_) ? banner_ : nullptr;
  }
  uint32_t version() const { return version_; }

 private:
  void engage_kill();
  void disengage_kill();
  void disable_all(uint32_t now);
  void owner_change();
  void gate();
  void open_menu();
  void open_item(uint8_t item);
  void open_set(uint8_t set);
  void back();
  void key_event(const Event& e);
  void button_event(const Event& e, uint32_t now);
  void turn(int8_t delta);
  void edit(int32_t steps);
  void fire(const Binding& b);
  bool has_kill() const;
  void touched();

  const Variant* v_ = nullptr;
  Config* cfg_ = nullptr;
  Input* in_ = nullptr;
  Arbiter* hid_ = nullptr;
  Rng* rng_ = nullptr;

  Output keymap_out_;
  Output button_out_;
  uint8_t held_ = 0;
  uint8_t button_held_ = 0;

  Screen screen_ = Screen::Keys;
  Screen before_config_ = Screen::Keys;
  bool in_menu_ = false;
  uint8_t cursor_ = 0, set_ = 0, tool_ = 0, list_cursor_ = 0, focus_ = 0;

  bool killed_ = false;
  bool kill_press_engaged_ = false;
  uint32_t kill_press_gen_ = 0;

  bool config_ = false, recovery_ = false, config_request_ = false, cancel_ = false;
  bool frozen_ = false;

  bool link_init_ = false;
  bool usb_up_ = false;
  uint32_t link_gen_ = 0;

  bool enabled_[MAX_TOOLS] = {};
  bool active_[MAX_TOOLS] = {};
  bool dirty_ = false;
  uint32_t last_edit_ = 0;
  bool flush_ = false;

  const char* banner_ = "";
  const char* status_ = "";
  bool unsaved_ = false;
  uint32_t banner_until_ = 0;
  uint32_t now_ = 0;
  uint32_t version_ = 1;
};

}  // namespace mp
