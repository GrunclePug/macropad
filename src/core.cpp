#include "core.h"

namespace mp {

static constexpr uint32_t BANNER_MS = 1500;

void Core::begin(const Variant& v, Config& cfg, Input& in, Arbiter& hid, Rng& rng) {
  v_ = &v;
  cfg_ = &cfg;
  in_ = &in;
  hid_ = &hid;
  rng_ = &rng;
  set_ = cfg.selected < v.nkeymaps ? cfg.selected : 0;

  InputConfig c;
  c.keys = v.keys;
  c.buttons = v.nbuttons;
  c.encoder = v.encoder;
  c.detent = v.detent;
  c.release_ms = v.release_ms;
  in.begin(c);

  hid.add(&keymap_out_);
  hid.add(&button_out_);
  for (uint8_t i = 0; i < v.ntools; i++) {
    v.tools[i]->out.clear();
    hid.add(&v.tools[i]->out);
  }
}

bool Core::has_kill() const {
  for (uint8_t i = 0; i < v_->nbuttons; i++)
    if (v_->buttons[i].role == Role::Kill) return true;
  return false;
}

const char* Core::menu_name(uint8_t i) const {
  if (i == 0) return "Keys";
  if (i <= v_->ntools) return v_->tools[i - 1]->schema().name;
  return "Configure";
}

bool Core::take_flush() {
  bool f = flush_;
  flush_ = false;
  return f;
}

bool Core::take_config_request() {
  bool r = config_request_;
  config_request_ = false;
  return r;
}

bool Core::take_cancel() {
  bool c = cancel_;
  cancel_ = false;
  return c;
}

void Core::touched() {
  dirty_ = true;
  last_edit_ = now_;
  version_++;
}

void Core::notice(const char* text, uint32_t now, uint32_t ms) {
  banner_ = text;
  banner_until_ = now + ms;
  if (!banner_until_) banner_until_ = 1;
  version_++;
}

// --- Killswitch, config mode and link ---

void Core::gate() { hid_->set_gate(!killed_ && !config_); }

void Core::engage_kill() {
  if (killed_) return;
  killed_ = true;
  gate();  // closing it queues neutral reports
  owner_change();
  version_++;
}

void Core::disengage_kill() {
  killed_ = false;
  gate();
  owner_change();
  version_++;
}

void Core::disable_all(uint32_t now) {
  for (uint8_t i = 0; i < v_->ntools; i++) enabled_[i] = false;
  notice("All tools off", now, BANNER_MS);
}

void Core::enter_config(bool recovery) {
  if (config_) return;
  bool any = false;
  for (uint8_t i = 0; i < v_->ntools; i++) any |= enabled_[i];
  if (any) {
    if (has_kill())
      engage_kill();
    else
      for (uint8_t i = 0; i < v_->ntools; i++) enabled_[i] = false;
  }
  config_ = true;
  recovery_ = recovery;
  before_config_ = screen_ == Screen::Configure ? Screen::Keys : screen_;
  screen_ = Screen::Configure;
  in_menu_ = false;
  gate();
  owner_change();
  version_++;
}

void Core::leave_config() {
  if (!config_) return;
  config_ = false;
  recovery_ = false;
  screen_ = before_config_;
  gate();
  owner_change();
  if (screen_ == Screen::Keys) flush_ = true;
  version_++;
}

void Core::link(bool up, bool disturbed) {
  if (!link_init_) {
    link_init_ = true;  // the first poll sets the state: enumerating at boot isn't a loss
    usb_up_ = up;
    return;
  }
  bool lost = usb_up_ && (!up || disturbed);
  bool back = up && (!usb_up_ || disturbed);
  if (lost) {
    link_gen_++;
    if (has_kill())
      engage_kill();
    else
      disable_all(now_);
    owner_change();
    if (config_ && !recovery_) cancel_ = true;
  }
  if (back) {
    link_gen_++;
    hid_->neutral();
    owner_change();
  }
  if (up != usb_up_) version_++;
  usb_up_ = up;
}

// --- Screens ---

void Core::owner_change() {
  held_ = 0;
  button_held_ = 0;
  in_->suppress_held();
}

void Core::open_menu() {
  owner_change();
  in_menu_ = true;
  cursor_ = screen_ == Screen::Tool ? tool_ + 1 : 0;
  version_++;
}

void Core::open_item(uint8_t item) {
  owner_change();
  in_menu_ = false;
  if (item == 0) {
    if (v_->nkeymaps == 1) {
      open_set(0);
      return;
    }
    screen_ = Screen::SetList;
    list_cursor_ = set_;
  } else if (item <= v_->ntools) {
    screen_ = Screen::Tool;
    tool_ = item - 1;
    focus_ = 0;
  } else {
    config_request_ = true;  // the app enters config mode
  }
  version_++;
}

void Core::open_set(uint8_t s) {
  owner_change();
  in_menu_ = false;
  screen_ = Screen::Keys;
  if (s != set_ || cfg_->selected != s) {
    set_ = s;
    cfg_->selected = s;
    touched();
  }
  flush_ = true;
  version_++;
}

void Core::back() {
  owner_change();
  in_menu_ = false;
  if (screen_ == Screen::Keys) flush_ = true;
  version_++;
}

// --- Events ---

void Core::handle(const Event& e, uint32_t now) {
  now_ = now;
  if (frozen_) return;
  switch (e.src) {
    case Src::Key:
      key_event(e);
      break;
    case Src::Button:
      button_event(e, now);
      break;
    case Src::Encoder:
      turn(e.delta);
      break;
  }
}

static uint8_t wrap(int v, uint8_t n) { return (uint8_t)((v % n + n) % n); }

void Core::key_event(const Event& e) {
  if (e.index >= v_->keys || config_) return;
  bool press = e.type == Ev::Press;
  bool step = press || e.type == Ev::Repeat;

  if (in_menu_) {
    if (!step) return;
    uint8_t n = menu_items();
    if (e.index == 0) cursor_ = wrap(cursor_ - 1, n);
    if (e.index == 1) cursor_ = wrap(cursor_ + 1, n);
    if (e.index == 2 && press) open_item(cursor_);
    if (e.index == 3 && press) back();
    version_++;
    return;
  }

  switch (screen_) {
    case Screen::SetList:
      if (!step) return;
      if (e.index == 0) list_cursor_ = wrap(list_cursor_ - 1, v_->nkeymaps);
      if (e.index == 1) list_cursor_ = wrap(list_cursor_ + 1, v_->nkeymaps);
      if (e.index == 2 && press) open_set(list_cursor_);
      if (e.index == 3 && press) open_menu();
      version_++;
      return;
    case Screen::Tool: {
      if (!step) return;
      uint8_t n = v_->tools[tool_]->schema().nparams;
      if (e.index == 0) focus_ = wrap(focus_ - 1, n);
      if (e.index == 1) focus_ = wrap(focus_ + 1, n);
      if (e.index == 2) edit(-1);
      if (e.index == 3) edit(1);
      version_++;
      return;
    }
    case Screen::Configure:
      return;
    case Screen::Keys:
      break;
  }

  const Binding& b = v_->keymaps[set_].keys[e.index];
  uint8_t bit = 1u << e.index;
  if (press) {
    if (b.kind == Binding::TOGGLE_TOOL)
      fire(b);
    else if (b.kind != Binding::NONE)
      held_ |= bit;
  } else if (e.type == Ev::Release) {
    held_ &= ~bit;
  }
}

void Core::button_event(const Event& e, uint32_t now) {
  if (e.index >= v_->nbuttons) return;
  const ButtonDef& d = v_->buttons[e.index];
  bool nav_hold = (d.role == Role::Nav || d.role == Role::EncPush) && e.type == Ev::Hold;

  if (config_) {
    // Only cancel works: a Kill tap, or a Nav or push hold.
    if ((d.role == Role::Kill && e.type == Ev::Tap) || nav_hold) cancel_ = true;
    return;
  }

  switch (d.role) {
    case Role::Kill:
      if (e.type == Ev::Press) {
        kill_press_gen_ = link_gen_;
        kill_press_engaged_ = !killed_;
        engage_kill();
      } else if (e.type == Ev::Hold) {
        if (kill_press_gen_ == link_gen_)
          disable_all(now);
        else
          kill_holds_discarded_++;
      } else if (e.type == Ev::Tap) {
        // Only a fresh tap, begun while already killed and with the link
        // unchanged since, turns the killswitch off.
        if (!kill_press_engaged_ && killed_ && usb_up_ && kill_press_gen_ == link_gen_)
          disengage_kill();
      }
      break;
    case Role::Nav:
    case Role::EncPush:
      if (nav_hold) {
        if (in_menu_)
          back();
        else
          open_menu();
        break;
      }
      if (e.type != Ev::Tap) break;
      if (in_menu_) {
        open_item(cursor_);
      } else if (screen_ == Screen::SetList) {
        open_set(list_cursor_);
      } else if (screen_ == Screen::Tool) {
        if (d.role == Role::Nav) {
          enabled_[tool_] = !enabled_[tool_];
        } else {
          focus_ = wrap(focus_ + 1, v_->tools[tool_]->schema().nparams);
        }
        version_++;
      } else if (screen_ == Screen::Keys) {
        const Keymap& km = v_->keymaps[set_];
        if (d.role == Role::EncPush) {
          fire(km.enc_push);
        } else if (!km.legend) {
          cfg_->sets[set_].flags ^= SET_LIVE;
          touched();
        }
      }
      break;
    case Role::Bind:
      if (e.type == Ev::Press) {
        if (d.bind.kind == Binding::TOGGLE_TOOL)
          fire(d.bind);
        else
          button_held_ |= 1u << e.index;
      } else if (e.type == Ev::Release) {
        button_held_ &= ~(1u << e.index);
      }
      break;
    case Role::None:
      break;
  }
}

void Core::turn(int8_t delta) {
  if (config_) return;
  if (in_menu_) {
    cursor_ = wrap(cursor_ + (delta > 0 ? 1 : -1), menu_items());
    version_++;
    return;
  }
  switch (screen_) {
    case Screen::SetList:
      list_cursor_ = wrap(list_cursor_ + (delta > 0 ? 1 : -1), v_->nkeymaps);
      version_++;
      break;
    case Screen::Tool:
      edit(delta);
      break;
    case Screen::Keys: {
      const Keymap& km = v_->keymaps[set_];
      fire(delta > 0 ? km.enc_cw : km.enc_ccw);
      break;
    }
    case Screen::Configure:
      break;
  }
}

void Core::fire(const Binding& b) {
  switch (b.kind) {
    case Binding::KEY:
      hid_->pulse_key(b.mods, b.usage);
      break;
    case Binding::CONSUMER:
      hid_->pulse_consumer(b.usage);
      break;
    case Binding::TOGGLE_TOOL:
      if (b.usage < v_->ntools) {
        enabled_[b.usage] = !enabled_[b.usage];
        version_++;
      }
      break;
    case Binding::NONE:
      break;
  }
}

void Core::edit(int32_t steps) {
  Tool* t = v_->tools[tool_];
  const ToolSchema& s = t->schema();
  if (focus_ >= s.nparams || !t->slot()) return;
  int32_t* slot = t->slot();
  const ParamDef& d = s.params[focus_];
  if (d.kind == P_KEY) {
    int i = key_index((uint8_t)slot[focus_]);
    if (i < 0) i = 0;
    i += steps;
    if (i < 0) i = 0;
    if (i >= KEY_NAMES_COUNT) i = KEY_NAMES_COUNT - 1;
    slot[focus_] = KEY_NAMES[i].usage;
  } else {
    int32_t v = slot[focus_] + steps * d.step;
    if (v < d.min) v = d.min;
    if (v > d.max) v = d.max;
    slot[focus_] = v;
  }
  s.clamp(slot);
  touched();
}

// --- Per loop ---

void Core::update(uint32_t now) {
  now_ = now;
  if (banner_until_ && due(now, banner_until_)) {
    banner_until_ = 0;
    version_++;
  }

  ToolCtx ctx{*hid_, *rng_};
  for (uint8_t i = 0; i < v_->ntools; i++) {
    Tool* t = v_->tools[i];
    bool a = enabled_[i] && !killed_ && !config_ && usb_up_;
    if (a != active_[i]) {
      t->out.clear();
      if (a)
        t->start(ctx, now);
      else
        t->stop();
      active_[i] = a;
      version_++;
    }
    if (a) t->tick(ctx, now);
  }

  keymap_out_.clear();
  if (keymap_live() && !killed_) {
    const Keymap& km = v_->keymaps[set_];
    for (uint8_t k = 0; k < v_->keys; k++) {
      if (!(held_ & (1u << k))) continue;
      const Binding& b = km.keys[k];
      if (b.kind == Binding::KEY) keymap_out_.press(b.mods, b.usage);
      if (b.kind == Binding::CONSUMER) keymap_out_.press_consumer(b.usage);
    }
  }

  button_out_.clear();
  if (!killed_ && !config_) {
    for (uint8_t i = 0; i < v_->nbuttons; i++) {
      if (!(button_held_ & (1u << i))) continue;
      const Binding& b = v_->buttons[i].bind;
      if (b.kind == Binding::KEY) button_out_.press(b.mods, b.usage);
      if (b.kind == Binding::CONSUMER) button_out_.press_consumer(b.usage);
    }
  }
}

}  // namespace mp
