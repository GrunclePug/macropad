// HID output owners and the report arbiter. Pure: no Arduino headers.
//
// Every keymap and tool owns an Output. The arbiter ORs all owners into one
// keyboard, mouse and consumer state, queues each change in order, and submits
// at most one report per call, never blocking. Neutral reports go first, then
// keyboard and consumer changes, then mouse button edges, then accumulated
// mouse movement.
#pragma once

#include <stdint.h>
#include <string.h>

namespace mp {

enum : uint8_t { REPORT_KEYBOARD = 1, REPORT_MOUSE = 2, REPORT_CONSUMER = 3 };

// Consumer controls are sent as one bit per usage, in this order.
enum : uint8_t { CC_VOL_UP, CC_VOL_DOWN, CC_MUTE, CC_PLAY_PAUSE, CC_NEXT, CC_PREV, CC_STOP, CC_COUNT };

// Keyboard state: modifier bits plus a bitmap of usages 0x00-0x7F.
struct Keys {
  uint8_t mods = 0;
  uint8_t bits[16] = {};

  void set(uint8_t usage) {
    if (usage < 0x80) bits[usage >> 3] |= 1u << (usage & 7);
  }
  bool has(uint8_t usage) const { return usage < 0x80 && (bits[usage >> 3] & (1u << (usage & 7))); }
  bool empty() const {
    if (mods) return false;
    for (uint8_t b : bits)
      if (b) return false;
    return true;
  }
  void merge(const Keys& o) {
    mods |= o.mods;
    for (int i = 0; i < 16; i++) bits[i] |= o.bits[i];
  }
  bool operator==(const Keys& o) const { return mods == o.mods && memcmp(bits, o.bits, 16) == 0; }
  bool operator!=(const Keys& o) const { return !(*this == o); }
};

class Output {
 public:
  void press(uint8_t mods, uint8_t usage) {
    keys.mods |= mods;
    if (usage) keys.set(usage);
  }
  void press_button(uint8_t mask) { buttons |= mask; }
  void release_button(uint8_t mask) { buttons &= ~mask; }
  void press_consumer(uint8_t bit) { consumer |= 1u << bit; }
  void clear() {
    keys = Keys{};
    buttons = 0;
    consumer = 0;
  }
  bool empty() const { return keys.empty() && !buttons && !consumer; }

  Keys keys;
  uint8_t buttons = 0;
  uint8_t consumer = 0;
};

// The USB side, faked in tests.
struct UsbPort {
  virtual bool ready() = 0;
  virtual bool submit(uint8_t report_id, const uint8_t* data, uint8_t len) = 0;
};

class Arbiter {
 public:
  static constexpr uint8_t MAX_OWNERS = 16;
  static constexpr uint8_t QLEN = 16;

  void add(Output* o);

  // Closed: owners count as empty and moves and pulses are dropped. Closing
  // also queues neutral reports.
  void set_gate(bool open);
  bool gate() const { return gate_; }

  // Drop everything queued and send one neutral report per report ID before
  // any other traffic.
  void neutral();

  void move(int dx, int dy, int wheel = 0);
  void pulse_key(uint8_t mods, uint8_t usage);  // press then release
  void pulse_consumer(uint8_t bit);

  // Queue any change in the owners' combined state. Call once per loop, after
  // the owners have been updated.
  void collect();

  // Submit at most one report. Returns true if one was accepted.
  bool service(UsbPort& port);

  bool mouse_idle() const { return mq_.count == 0; }
  bool keyboard_idle() const { return kq_count_ == 0; }
  const Keys& sent_keys() const { return sent_keys_; }
  uint8_t sent_buttons() const { return sent_buttons_; }
  uint8_t sent_consumer() const { return sent_consumer_; }
  uint32_t overflows() const { return overflows_; }

  // Debug hook: called after each accepted keyboard report.
  void (*on_keys_sent)(const Keys& k) = nullptr;

 private:
  struct ByteQueue {
    uint8_t v[QLEN];
    uint8_t head = 0, count = 0;
    bool push(uint8_t x) {
      if (count == QLEN) return false;
      v[(head + count) % QLEN] = x;
      count++;
      return true;
    }
    uint8_t front() const { return v[head]; }
    void pop() {
      head = (head + 1) % QLEN;
      count--;
    }
    void clear() { head = count = 0; }
  };

  void push_keys(const Keys& k);
  void push_consumer(uint8_t c);
  void push_buttons(uint8_t b);

  Output* owners_[MAX_OWNERS] = {};
  uint8_t nowners_ = 0;
  bool gate_ = true;
  uint8_t neutral_pending_ = 0;  // bit per report ID

  Keys kq_[QLEN];
  uint8_t kq_head_ = 0, kq_count_ = 0;
  Keys k_last_;  // state as of the last queued keyboard change
  ByteQueue cq_, mq_;
  uint8_t c_last_ = 0, b_last_ = 0;
  int32_t dx_ = 0, dy_ = 0, dw_ = 0;

  Keys sent_keys_;
  uint8_t sent_buttons_ = 0, sent_consumer_ = 0;
  uint32_t overflows_ = 0;
};

}  // namespace mp
