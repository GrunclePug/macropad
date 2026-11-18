#include "hid.h"

namespace mp {

void Arbiter::add(Output* o) {
  if (nowners_ < MAX_OWNERS) owners_[nowners_++] = o;
}

void Arbiter::set_gate(bool open) {
  if (open == gate_) return;
  gate_ = open;
  if (!open) neutral();
}

void Arbiter::neutral() {
  kq_head_ = kq_count_ = 0;
  cq_.clear();
  mq_.clear();
  dx_ = dy_ = dw_ = 0;
  k_last_ = Keys{};
  c_last_ = b_last_ = 0;
  neutral_pending_ = (1u << REPORT_KEYBOARD) | (1u << REPORT_MOUSE) | (1u << REPORT_CONSUMER);
}

void Arbiter::move(int dx, int dy, int wheel) {
  if (!gate_) return;
  dx_ += dx;
  dy_ += dy;
  dw_ += wheel;
}

void Arbiter::push_keys(const Keys& k) {
  if (kq_count_ == QLEN) {
    // Collapse to the current state rather than drop the newest change.
    overflows_++;
    kq_head_ = kq_count_ = 0;
  }
  kq_[(kq_head_ + kq_count_) % QLEN] = k;
  kq_count_++;
  k_last_ = k;
}

void Arbiter::push_consumer(uint8_t c) {
  if (!cq_.push(c)) {
    overflows_++;
    cq_.clear();
    cq_.push(c);
  }
  c_last_ = c;
}

void Arbiter::push_buttons(uint8_t b) {
  if (!mq_.push(b)) {
    overflows_++;
    mq_.clear();
    mq_.push(b);
  }
  b_last_ = b;
}

void Arbiter::pulse_key(uint8_t mods, uint8_t usage) {
  if (!gate_) return;
  Keys base = k_last_;
  Keys k = base;
  k.mods |= mods;
  if (usage) k.set(usage);
  if (k == base) return;  // already held by someone
  push_keys(k);
  push_keys(base);
}

void Arbiter::pulse_consumer(uint8_t bit) {
  if (!gate_) return;
  uint8_t base = c_last_;
  uint8_t c = base | (1u << bit);
  if (c == base) return;
  push_consumer(c);
  push_consumer(base);
}

void Arbiter::collect() {
  Keys k;
  uint8_t b = 0, c = 0;
  if (gate_) {
    for (uint8_t i = 0; i < nowners_; i++) {
      k.merge(owners_[i]->keys);
      b |= owners_[i]->buttons;
      c |= owners_[i]->consumer;
    }
  }
  if (k != k_last_) push_keys(k);
  if (c != c_last_) push_consumer(c);
  if (b != b_last_) push_buttons(b);
}

static int8_t take(int32_t& v) {
  int32_t s = v > 127 ? 127 : v < -127 ? -127 : v;
  return (int8_t)s;
}

bool Arbiter::service(UsbPort& port) {
  if (!port.ready()) return false;

  if (neutral_pending_) {
    uint8_t zero[17] = {};
    uint8_t id = (neutral_pending_ & (1u << REPORT_KEYBOARD))  ? REPORT_KEYBOARD
                 : (neutral_pending_ & (1u << REPORT_MOUSE)) ? REPORT_MOUSE
                                                             : REPORT_CONSUMER;
    uint8_t len = id == REPORT_KEYBOARD ? 17 : id == REPORT_MOUSE ? 4 : 1;
    if (!port.submit(id, zero, len)) return false;
    neutral_pending_ &= ~(1u << id);
    if (id == REPORT_KEYBOARD) sent_keys_ = Keys{};
    if (id == REPORT_MOUSE) sent_buttons_ = 0;
    if (id == REPORT_CONSUMER) sent_consumer_ = 0;
    return true;
  }

  if (kq_count_) {
    const Keys& k = kq_[kq_head_];
    uint8_t r[17];
    r[0] = k.mods;
    memcpy(r + 1, k.bits, 16);
    if (!port.submit(REPORT_KEYBOARD, r, sizeof r)) return false;
    sent_keys_ = k;
    kq_head_ = (kq_head_ + 1) % QLEN;
    kq_count_--;
    if (on_keys_sent) on_keys_sent(sent_keys_);
    return true;
  }

  if (cq_.count) {
    uint8_t c = cq_.front();
    if (!port.submit(REPORT_CONSUMER, &c, 1)) return false;
    sent_consumer_ = c;
    cq_.pop();
    return true;
  }

  if (mq_.count) {
    uint8_t r[4] = {mq_.front(), 0, 0, 0};
    if (!port.submit(REPORT_MOUSE, r, sizeof r)) return false;
    sent_buttons_ = r[0];
    mq_.pop();
    return true;
  }

  if (dx_ || dy_ || dw_) {
    int8_t x = take(dx_), y = take(dy_), w = take(dw_);
    uint8_t r[4] = {sent_buttons_, (uint8_t)x, (uint8_t)y, (uint8_t)w};
    if (!port.submit(REPORT_MOUSE, r, sizeof r)) return false;
    dx_ -= x;
    dy_ -= y;
    dw_ -= w;
    return true;
  }
  return false;
}

}  // namespace mp
