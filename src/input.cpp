#include "input.h"

namespace mp {

void Input::begin(const InputConfig& c) {
  cfg_ = c;
  for (auto& d : keys_) d = Deb{};
  for (auto& d : buttons_) d = Deb{};
  enc_init_ = false;
  enc_acc_ = 0;
  head_ = count_ = 0;
}

Input::Edge Input::step(Deb& d, bool raw, uint32_t now) {
  if (!d.down) {
    if (!raw) {
      d.agree = 0;
      return NONE;
    }
    if (++d.agree < 2) return NONE;
    d.agree = 0;
    d.down = true;
    d.releasing = false;
    return PRESS;
  }
  if (raw) {
    d.releasing = false;
    return NONE;
  }
  if (!d.releasing) {
    d.releasing = true;
    d.since = now;
  }
  if (now - d.since < cfg_.release_ms) return NONE;
  d.down = false;
  d.releasing = false;
  return RELEASE;
}

void Input::sample(uint8_t key_mask, uint8_t button_mask, uint8_t enc_ab, uint32_t now) {
  for (uint8_t i = 0; i < cfg_.keys; i++) {
    Deb& d = keys_[i];
    switch (step(d, key_mask & (1u << i), now)) {
      case PRESS:
        d.suppressed = false;
        d.pressed_at = now;
        d.next_repeat = now + cfg_.repeat_delay_ms;
        push(Ev::Press, Src::Key, i);
        break;
      case RELEASE:
        if (d.suppressed)
          d.suppressed = false;
        else
          push(Ev::Release, Src::Key, i);
        break;
      case NONE:
        if (d.down && !d.suppressed && due(now, d.next_repeat)) {
          d.next_repeat = now + cfg_.repeat_ms;
          push(Ev::Repeat, Src::Key, i);
        }
        break;
    }
  }

  for (uint8_t i = 0; i < cfg_.buttons; i++) {
    Deb& d = buttons_[i];
    switch (step(d, button_mask & (1u << i), now)) {
      case PRESS:
        d.pressed_at = now;
        d.hold_fired = false;
        push(Ev::Press, Src::Button, i);
        break;
      case RELEASE:
        if (!d.hold_fired) push(Ev::Tap, Src::Button, i);
        push(Ev::Release, Src::Button, i);
        break;
      case NONE:
        if (d.down && !d.hold_fired && now - d.pressed_at >= cfg_.hold_ms) {
          d.hold_fired = true;
          push(Ev::Hold, Src::Button, i);
        }
        break;
    }
  }

  if (cfg_.encoder) {
    // Quadrature decoding: index (previous << 2 | current) gives the step.
    static const int8_t table[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
    uint8_t cur = enc_ab & 3;
    if (!enc_init_) {
      enc_init_ = true;
      enc_prev_ = cur;
    }
    enc_acc_ += table[(enc_prev_ << 2) | cur];
    enc_prev_ = cur;
    int8_t detent = cfg_.detent ? cfg_.detent : 4;
    if (enc_acc_ >= detent) {
      enc_acc_ -= detent;
      push(Ev::Turn, Src::Encoder, 0, 1);
    } else if (enc_acc_ <= -detent) {
      enc_acc_ += detent;
      push(Ev::Turn, Src::Encoder, 0, -1);
    }
  }
}

void Input::push(Ev t, Src s, uint8_t i, int8_t delta) {
  if (count_ == QLEN) {
    dropped_++;
    return;
  }
  q_[(head_ + count_) % QLEN] = Event{t, s, i, delta};
  count_++;
}

bool Input::pop(Event& e) {
  while (count_) {
    e = q_[head_];
    head_ = (head_ + 1) % QLEN;
    count_--;
    // A key suppressed after its event was queued (two keys in one scan, the
    // first changing screens) must not act on the new screen.
    if (e.src == Src::Key && e.type != Ev::Release && keys_[e.index].suppressed) continue;
    return true;
  }
  return false;
}

void Input::suppress_held() {
  for (uint8_t i = 0; i < cfg_.keys; i++)
    if (keys_[i].down) keys_[i].suppressed = true;
}

uint8_t Input::keys_down() const {
  uint8_t m = 0;
  for (uint8_t i = 0; i < cfg_.keys; i++)
    if (keys_[i].down) m |= 1u << i;
  return m;
}

}  // namespace mp
