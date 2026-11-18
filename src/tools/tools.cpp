#include "tools.h"

namespace mp {

static const uint8_t BUTTON_MASKS[] = {0x01, 0x02, 0x04};
static const uint8_t HOLD_MASKS[] = {0x01, 0x02, 0x03};

static uint8_t pick(const uint8_t* table, int32_t i) { return table[i >= 0 && i <= 2 ? i : 0]; }

// --- Autoclicker ---

uint8_t Autoclicker::mask() const { return test_mask_ ? test_mask_ : pick(BUTTON_MASKS, p_[2]); }

void Autoclicker::start(ToolCtx& c, uint32_t now) {
  state_ = IDLE;
  next_ = now + c.rng.between(p_[0], p_[1]);
}

void Autoclicker::stop() { state_ = IDLE; }

void Autoclicker::tick(ToolCtx& c, uint32_t now) {
  switch (state_) {
    case IDLE:
      if (!due(now, next_)) return;
      next_ = now + c.rng.between(p_[0], p_[1]);
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
      out.release_button(0xFF);  // also right if the button setting changed mid-click
      state_ = UP_QUEUED;
      return;
    case UP_QUEUED:
      if (!c.hid.mouse_idle()) return;
      clicks++;
      state_ = IDLE;
      if (due(now, next_)) next_ = now + c.rng.between(p_[0], p_[1]);  // fell due mid-click: skip it
      return;
  }
}

// --- Jitter ---

void Jitter::start(ToolCtx&, uint32_t now) {
  phase_ = OUT;
  moved_ = 0;
  cycle_ = p_[0];
  next_ = now;
}

void Jitter::tick(ToolCtx& c, uint32_t now) {
  if (!due(now, next_)) return;
  switch (phase_) {
    case OUT:
    case BACK: {
      int step = cycle_ - moved_ < STEP_PX ? cycle_ - moved_ : STEP_PX;
      c.hid.move(phase_ == OUT ? step : -step, 0);
      moved_ += step;
      next_ = now + STEP_MS;
      if (moved_ >= cycle_) {
        moved_ = 0;
        if (phase_ == OUT) {
          phase_ = BACK;
        } else {
          phase_ = PAUSE;
          next_ = now + c.rng.between(p_[1], p_[2]);
        }
      }
      return;
    }
    case PAUSE:
      phase_ = OUT;
      cycle_ = p_[0];
      return;
  }
}

// --- Keyspam ---

void Keyspam::start(ToolCtx& c, uint32_t now) {
  held_ = false;
  next_ = now + c.rng.between(p_[1], p_[2]);
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
  out.press(0, (uint8_t)p_[0]);
  held_ = true;
  release_at_ = now + p_[3];
  next_ = now + c.rng.between(p_[1], p_[2]);
  if (!due(next_, release_at_ + 1)) next_ = release_at_ + 1;
}

// --- Mousehold ---

uint8_t Mousehold::mask() const { return test_mask_ ? test_mask_ : pick(HOLD_MASKS, p_[0]); }

void Mousehold::start(ToolCtx&, uint32_t) { out.press_button(mask()); }

void Mousehold::tick(ToolCtx&, uint32_t) {
  // Follow a change of the button setting while running.
  if (out.buttons != mask()) {
    out.release_button(0xFF);
    out.press_button(mask());
  }
}

// --- Registry ---

#ifdef MACROPAD_SELFTEST
constexpr uint8_t TEST_BUTTON = 0x80;  // mouse button 8: harmless in the user's session
#else
constexpr uint8_t TEST_BUTTON = 0;
#endif

Tool* tool_for_kind(uint8_t kind) {
  static Autoclicker click(TEST_BUTTON);
  static Jitter jitter;
  static Keyspam spam;
  static Mousehold hold(TEST_BUTTON);
  switch (kind) {
    case TK_CLICK:
      return &click;
    case TK_JITTER:
      return &jitter;
    case TK_SPAM:
      return &spam;
    case TK_HOLD:
      return &hold;
    default:
      return nullptr;
  }
}

}  // namespace mp
