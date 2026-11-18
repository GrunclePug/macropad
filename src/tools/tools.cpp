#include "tools.h"

namespace mp {

static const char* const BUTTONS[] = {"Left", "Right", "Middle"};
static const uint8_t BUTTON_MASKS[] = {0x01, 0x02, 0x04};
static const char* const HOLD_BUTTONS[] = {"Left", "Right", "Both"};
static const uint8_t HOLD_MASKS[] = {0x01, 0x02, 0x03};

// --- Autoclicker ---

Autoclicker::Autoclicker(int32_t min_ms, int32_t max_ms, uint8_t test_mask)
    : Tool("click", "Autoclicker"), min_(min_ms), max_(max_ms), test_mask_(test_mask) {
  params_[0] = Param{"min", "Min", &min_, 20, 5000, 5, "ms", nullptr};
  params_[1] = Param{"max", "Max", &max_, 20, 5000, 5, "ms", nullptr};
  params_[2] = Param{"btn", "Button", &button_, 0, 2, 1, "", BUTTONS};
}

const Param* Autoclicker::params(size_t& n) {
  n = 3;
  return params_;
}

void Autoclicker::clamp() {
  if (max_ < min_) max_ = min_;
}

uint8_t Autoclicker::mask() const { return test_mask_ ? test_mask_ : BUTTON_MASKS[button_]; }

void Autoclicker::start(ToolCtx& c, uint32_t now) {
  state_ = IDLE;
  next_ = now + c.rng.between(min_, max_);
}

void Autoclicker::stop() { state_ = IDLE; }

void Autoclicker::tick(ToolCtx& c, uint32_t now) {
  switch (state_) {
    case IDLE:
      if (!due(now, next_)) return;
      next_ = now + c.rng.between(min_, max_);
      if (!c.hid.mouse_idle()) return;  // previous traffic still queued: skip
      out.press_button(mask());
      state_ = DOWN_QUEUED;
      return;
    case DOWN_QUEUED:
      // The queue drains once the button-down report is accepted (or right
      // away if another owner already holds this button).
      if (!c.hid.mouse_idle()) return;
      down_at_ = now;
      state_ = HOLDING;
      return;
    case HOLDING:
      if (now - down_at_ < HOLD_MS) return;
      out.release_button(mask());
      state_ = UP_QUEUED;
      return;
    case UP_QUEUED:
      if (!c.hid.mouse_idle()) return;
      clicks++;
      state_ = IDLE;
      return;
  }
}

// --- Jitter ---

Jitter::Jitter(int32_t dist, int32_t min_ms, int32_t max_ms)
    : Tool("jitter", "Jitter"), dist_(dist), min_(min_ms), max_(max_ms) {
  params_[0] = Param{"dist", "Distance", &dist_, 10, 200, 10, "px", nullptr};
  params_[1] = Param{"min", "Min pause", &min_, 100, 60000, 100, "ms", nullptr};
  params_[2] = Param{"max", "Max pause", &max_, 100, 60000, 100, "ms", nullptr};
}

const Param* Jitter::params(size_t& n) {
  n = 3;
  return params_;
}

void Jitter::clamp() {
  if (max_ < min_) max_ = min_;
}

void Jitter::start(ToolCtx&, uint32_t now) {
  phase_ = OUT;
  moved_ = 0;
  next_ = now;
}

void Jitter::tick(ToolCtx& c, uint32_t now) {
  if (!due(now, next_)) return;
  switch (phase_) {
    case OUT:
    case BACK: {
      int step = dist_ - moved_ < STEP_PX ? dist_ - moved_ : STEP_PX;
      c.hid.move(phase_ == OUT ? step : -step, 0);
      moved_ += step;
      next_ = now + STEP_MS;
      if (moved_ >= dist_) {
        moved_ = 0;
        if (phase_ == OUT) {
          phase_ = BACK;
        } else {
          phase_ = PAUSE;
          next_ = now + c.rng.between(min_, max_);
        }
      }
      return;
    }
    case PAUSE:
      phase_ = OUT;
      return;
  }
}

// --- Keyspam ---

Keyspam::Keyspam(const KeySet* sets, const char* const* names, uint8_t nsets, int32_t min_ms,
                 int32_t max_ms, int32_t hold_ms)
    : Tool("spam", "Keyspam"), sets_(sets), min_(min_ms), max_(max_ms), hold_(hold_ms) {
  params_[0] = Param{"set", "Keys", &set_, 0, nsets - 1, 1, "", names};
  params_[1] = Param{"min", "Min", &min_, 20, 10000, 10, "ms", nullptr};
  params_[2] = Param{"max", "Max", &max_, 20, 10000, 10, "ms", nullptr};
  params_[3] = Param{"hold", "Hold", &hold_, 10, 500, 10, "ms", nullptr};
}

const Param* Keyspam::params(size_t& n) {
  n = 4;
  return params_;
}

void Keyspam::clamp() {
  if (max_ < min_) max_ = min_;
}

void Keyspam::start(ToolCtx& c, uint32_t now) {
  held_ = false;
  next_ = now + c.rng.between(min_, max_);
}

void Keyspam::stop() { held_ = false; }

void Keyspam::tick(ToolCtx& c, uint32_t now) {
  if (held_) {
    if (!due(now, release_at_)) return;
    out.clear();
    held_ = false;
    return;
  }
  if (!due(now, next_)) return;
  const KeySet& s = sets_[set_];
  out.press(s.mods, 0);
  for (uint8_t u : s.usages)
    if (u) out.press(0, u);
  held_ = true;
  release_at_ = now + hold_;
  next_ = now + c.rng.between(min_, max_);
  if (!due(next_, release_at_ + 1)) next_ = release_at_ + 1;
}

// --- Mousehold ---

Mousehold::Mousehold(uint8_t test_mask) : Tool("hold", "Mouse hold"), test_mask_(test_mask) {
  params_[0] = Param{"btn", "Button", &button_, 0, 2, 1, "", HOLD_BUTTONS};
}

const Param* Mousehold::params(size_t& n) {
  n = 1;
  return params_;
}

uint8_t Mousehold::mask() const { return test_mask_ ? test_mask_ : HOLD_MASKS[button_]; }

void Mousehold::start(ToolCtx&, uint32_t) { out.press_button(mask()); }

void Mousehold::tick(ToolCtx&, uint32_t) {
  // Follow a change of the button setting while running.
  if (out.buttons != mask()) {
    out.release_button(0xFF);
    out.press_button(mask());
  }
}

}  // namespace mp
