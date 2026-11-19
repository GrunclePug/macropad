// The automation tools. Pure: no Arduino headers. Settings are read from the
// bound slot (see schema.cpp for each kind's order and ranges).
#pragma once

#include "../tool.h"

namespace mp {

// Clicks a mouse button at a random interval between min and max. Each click
// is held for HOLD_MS, timed from when the button-down report is accepted, and
// only one click is in flight at a time.
// Slot: 0 min ms, 1 max ms, 2 button (0 left, 1 right, 2 middle).
class Autoclicker : public Tool {
 public:
  static constexpr uint32_t HOLD_MS = 10;
  // test_mask, when set, replaces the chosen button (self-test only).
  explicit Autoclicker(uint8_t test_mask = 0) : Tool(TK_CLICK), test_mask_(test_mask) {}
  void start(ToolCtx& c, uint32_t now) override;
  void stop() override;
  void tick(ToolCtx& c, uint32_t now) override;

  uint32_t clicks = 0;

 private:
  uint8_t mask() const;
  enum State : uint8_t { IDLE, DOWN_QUEUED, HOLDING, UP_QUEUED } state_ = IDLE;
  uint8_t test_mask_;
  uint32_t next_ = 0, down_at_ = 0;
};

// Moves the pointer out and back, then pauses for a random time.
// Slot: 0 distance px, 1 min pause ms, 2 max pause ms.
class Jitter : public Tool {
 public:
  static constexpr int STEP_PX = 10;
  static constexpr uint32_t STEP_MS = 2;
  Jitter() : Tool(TK_JITTER) {}
  void start(ToolCtx& c, uint32_t now) override;
  void tick(ToolCtx& c, uint32_t now) override;

 private:
  enum Phase : uint8_t { OUT, BACK, PAUSE } phase_ = OUT;
  int32_t cycle_ = 0;  // distance for the current cycle, so both legs match
  int32_t moved_ = 0;
  uint32_t next_ = 0;
};

// Presses a key at a random interval, holding it for `hold` ms.
// Slot: 0 key (HID usage), 1 min ms, 2 max ms, 3 hold ms.
class Keyspam : public Tool {
 public:
  Keyspam() : Tool(TK_SPAM) {}
  void start(ToolCtx& c, uint32_t now) override;
  void stop() override;
  void tick(ToolCtx& c, uint32_t now) override;

 private:
  bool held_ = false;
  uint32_t next_ = 0, release_at_ = 0;
};

// Holds a mouse button down while active.
// Slot: 0 button (0 left, 1 right, 2 both).
class Mousehold : public Tool {
 public:
  explicit Mousehold(uint8_t test_mask = 0) : Tool(TK_HOLD), test_mask_(test_mask) {}
  void start(ToolCtx& c, uint32_t now) override;
  void tick(ToolCtx& c, uint32_t now) override;

 private:
  uint8_t mask() const;
  uint8_t test_mask_;
};

// One static instance per kind (the self-test build swaps in mouse button 8).
Tool* tool_for_kind(uint8_t kind);

}  // namespace mp
