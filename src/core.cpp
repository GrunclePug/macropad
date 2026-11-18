#include "core.h"

namespace mp {

static constexpr uint32_t BANNER_MS = 1500;

void Core::begin(const Variant& v, Input& in, Arbiter& hid, Rng& rng) {
  v_ = &v;
  in_ = &in;
  hid_ = &hid;
  rng_ = &rng;

  InputConfig c;
  c.keys = v.keys;
  c.buttons = v.nbuttons;
  c.encoder = v.encoder;
  c.detent = v.detent;
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

Tool* Core::shown_tool() const {
  const Entry& e = v_->entries[shown_];
  return e.tool ? v_->tools[e.index] : nullptr;
}

const char* Core::entry_name(uint8_t e) const {
  const Entry& en = v_->entries[e];
  return en.tool ? v_->tools[en.index]->name : v_->keymaps[en.index].name;
}

bool Core::any_dirty() const {
  for (uint8_t i = 0; i < v_->ntools; i++)
    if (dirty_[i]) return true;
  return false;
}

bool Core::take_flush() {
  bool f = flush_;
  flush_ = false;
  return f;
}

// --- Killswitch and link ---

void Core::engage_kill() {
  if (killed_) return;
  killed_ = true;
  hid_->set_gate(false);  // also queues neutral reports
  owner_change();
  version_++;
}

void Core::disengage_kill() {
  killed_ = false;
  hid_->set_gate(true);
  owner_change();
  version_++;
}

void Core::disable_all(uint32_t now) {
  for (uint8_t i = 0; i < v_->ntools; i++) enabled_[i] = false;
  notice("All tools off", now, BANNER_MS);
}

void Core::notice(const char* text, uint32_t now, uint32_t ms) {
  banner_ = text;
  banner_until_ = now + ms;
  if (!banner_until_) banner_until_ = 1;
  version_++;
}

void Core::link(bool up, bool disturbed) {
  if (!link_init_) {
    // The first poll sets the state: enumerating at boot isn't a loss.
    link_init_ = true;
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
  cursor_ = shown_;
  version_++;
}

void Core::open(uint8_t entry) {
  owner_change();
  in_menu_ = false;
  shown_ = entry;
  focus_ = 0;
  if (!v_->entries[entry].tool) flush_ = true;
  version_++;
}

void Core::back() {
  owner_change();
  in_menu_ = false;
  if (keymap_live()) flush_ = true;
  version_++;
}

// --- Events ---

void Core::handle(const Event& e, uint32_t now) {
  now_ = now;
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

void Core::key_event(const Event& e) {
  if (e.index >= v_->keys) return;
  bool press = e.type == Ev::Press;
  bool step = press || e.type == Ev::Repeat;

  if (in_menu_) {
    if (!step) return;
    uint8_t n = v_->nentries;
    if (e.index == 0) cursor_ = (cursor_ + n - 1) % n;
    if (e.index == 1) cursor_ = (cursor_ + 1) % n;
    if (e.index == 2 && press) open(cursor_);
    if (e.index == 3 && press) back();
    version_++;
    return;
  }

  if (Tool* t = shown_tool()) {
    if (!step) return;
    size_t n;
    t->params(n);
    if (!n) return;
    if (e.index == 0) focus_ = (focus_ + n - 1) % n;
    if (e.index == 1) focus_ = (focus_ + 1) % n;
    if (e.index == 2) edit(-1);
    if (e.index == 3) edit(1);
    version_++;
    return;
  }

  const Binding& b = v_->keymaps[v_->entries[shown_].index].keys[e.index];
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
  switch (d.role) {
    case Role::Kill:
      if (e.type == Ev::Press) {
        kill_press_gen_ = link_gen_;
        kill_press_engaged_ = !killed_;
        engage_kill();
      } else if (e.type == Ev::Hold) {
        disable_all(now);
      } else if (e.type == Ev::Tap) {
        // Only a fresh tap, begun while already killed and with the link
        // unchanged since, turns the killswitch off.
        if (!kill_press_engaged_ && killed_ && usb_up_ && kill_press_gen_ == link_gen_)
          disengage_kill();
      }
      break;
    case Role::Nav:
      if (e.type == Ev::Tap) {
        if (in_menu_) {
          open(cursor_);
        } else if (v_->entries[shown_].tool) {
          uint8_t i = v_->entries[shown_].index;
          enabled_[i] = !enabled_[i];
          version_++;
        }
      } else if (e.type == Ev::Hold) {
        if (in_menu_)
          back();
        else
          open_menu();
      }
      break;
    case Role::EncPush:
      if (e.type != Ev::Tap) break;
      if (in_menu_) {
        open(cursor_);
      } else if (Tool* t = shown_tool()) {
        size_t n;
        t->params(n);
        if (n) focus_ = (focus_ + 1) % n;
        version_++;
      } else {
        fire(v_->keymaps[v_->entries[shown_].index].enc_push);
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
  if (in_menu_) {
    uint8_t n = v_->nentries;
    cursor_ = (cursor_ + n + (delta > 0 ? 1 : n - 1)) % n;
    version_++;
  } else if (shown_tool()) {
    edit(delta);
    version_++;
  } else {
    const Keymap& km = v_->keymaps[v_->entries[shown_].index];
    fire(delta > 0 ? km.enc_cw : km.enc_ccw);
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
  Tool* t = shown_tool();
  if (!t) return;
  size_t n;
  const Param* p = t->params(n);
  if (focus_ >= n) return;
  int32_t before[16];
  for (size_t i = 0; i < n && i < 16; i++) before[i] = *p[i].value;

  const Param& f = p[focus_];
  int32_t v = *f.value + steps * f.step;
  if (v < f.min) v = f.min;
  if (v > f.max) v = f.max;
  *f.value = v;
  t->clamp();

  uint8_t ti = v_->entries[shown_].index;
  for (size_t i = 0; i < n && i < 16; i++)
    if (*p[i].value != before[i]) dirty_[ti] |= 1u << i;
  last_edit_ = now_;
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
    bool a = enabled_[i] && !killed_ && usb_up_;
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
    const Keymap& km = v_->keymaps[v_->entries[shown_].index];
    for (uint8_t k = 0; k < v_->keys; k++) {
      if (!(held_ & (1u << k))) continue;
      const Binding& b = km.keys[k];
      if (b.kind == Binding::KEY) keymap_out_.press(b.mods, b.usage);
      if (b.kind == Binding::CONSUMER) keymap_out_.press_consumer(b.usage);
    }
  }

  button_out_.clear();
  if (!killed_) {
    for (uint8_t i = 0; i < v_->nbuttons; i++) {
      if (!(button_held_ & (1u << i))) continue;
      const Binding& b = v_->buttons[i].bind;
      if (b.kind == Binding::KEY) button_out_.press(b.mods, b.usage);
      if (b.kind == Binding::CONSUMER) button_out_.press_consumer(b.usage);
    }
  }
}

}  // namespace mp
