// The automation tools. Pure: no Arduino headers.
#pragma once

#include "../tool.h"

namespace mp {

// Clicks a mouse button at a random interval between min and max. Each click
// is held for HOLD_MS, timed from when the button-down report is accepted, and
// only one click is in flight at a time.
class Autoclicker : public Tool {
 public:
  static constexpr uint32_t HOLD_MS = 10;
  // test_mask, when set, replaces the chosen button (self-test only).
  Autoclicker(int32_t min_ms, int32_t max_ms, uint8_t test_mask = 0);
  const Param* params(size_t& n) override;
  void clamp() override;
  void start(ToolCtx& c, uint32_t now) override;
  void stop() override;
  void tick(ToolCtx& c, uint32_t now) override;

  uint32_t clicks = 0;

 private:
  uint8_t mask() const;
  enum State : uint8_t { IDLE, DOWN_QUEUED, HOLDING, UP_QUEUED } state_ = IDLE;
  int32_t min_, max_, button_ = 0;
  uint8_t test_mask_;
  uint32_t next_ = 0, down_at_ = 0;
  Param params_[3];
};

// Moves the pointer out and back by `dist` pixels, then pauses for a random
// time between min and max.
class Jitter : public Tool {
 public:
  static constexpr int STEP_PX = 10;
  static constexpr uint32_t STEP_MS = 2;
  Jitter(int32_t dist, int32_t min_ms, int32_t max_ms);
  const Param* params(size_t& n) override;
  void clamp() override;
  void start(ToolCtx& c, uint32_t now) override;
  void tick(ToolCtx& c, uint32_t now) override;

 private:
  enum Phase : uint8_t { OUT, BACK, PAUSE } phase_ = OUT;
  int32_t dist_, min_, max_;
  int32_t cycle_ = 0;  // distance for the current cycle, so both legs match
  int32_t moved_ = 0;
  uint32_t next_ = 0;
  Param params_[3];
};

// One key set for Keyspam: up to four keys pressed together.
struct KeySet {
  const char* name;
  uint8_t mods;
  uint8_t usages[4];
};

// Presses a key set at a random interval between min and max, holding it for
// `hold` ms.
class Keyspam : public Tool {
 public:
  Keyspam(const KeySet* sets, const char* const* names, uint8_t nsets, int32_t min_ms,
          int32_t max_ms, int32_t hold_ms);
  const Param* params(size_t& n) override;
  void clamp() override;
  void start(ToolCtx& c, uint32_t now) override;
  void stop() override;
  void tick(ToolCtx& c, uint32_t now) override;

 private:
  const KeySet* sets_;
  bool held_ = false;
  int32_t set_ = 0, min_, max_, hold_;
  uint32_t next_ = 0, release_at_ = 0;
  Param params_[4];
};

// Holds a mouse button down while active.
class Mousehold : public Tool {
 public:
  explicit Mousehold(uint8_t test_mask = 0);
  const Param* params(size_t& n) override;
  void start(ToolCtx& c, uint32_t now) override;
  void tick(ToolCtx& c, uint32_t now) override;

 private:
  uint8_t mask() const;
  int32_t button_ = 0;
  uint8_t test_mask_;
  Param params_[1];
};

}  // namespace mp
