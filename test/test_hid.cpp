#include "../src/hid.h"
#include "check.h"
#include "fake_port.h"

using namespace mp;

static bool key_in(const Report& r, uint8_t usage) {
  return r.id == REPORT_KEYBOARD && (r.data[1 + (usage >> 3)] & (1u << (usage & 7)));
}

TEST(chord_goes_out_in_one_report) {
  Arbiter a;
  Output o;
  a.add(&o);
  FakePort p;
  o.press(0, 0x07);
  o.press(0, 0x09);
  o.press(0, 0x0D);
  o.press(0, 0x0E);
  a.collect();
  p.drain(a);
  CHECK(p.sent.size() == 1);
  CHECK(key_in(p.sent[0], 0x07) && key_in(p.sent[0], 0x0E) && p.sent[0].data[0] == 0);
}

TEST(tap_within_busy_endpoint_is_not_lost) {
  Arbiter a;
  Output o;
  a.add(&o);
  FakePort p;
  p.busy = true;
  o.press(0, 0x07);
  a.collect();
  a.service(p);
  o.clear();
  a.collect();
  a.service(p);
  CHECK(p.sent.empty());
  p.busy = false;
  p.drain(a);
  CHECK(p.sent.size() == 2);
  CHECK(key_in(p.sent[0], 0x07) && !key_in(p.sent[1], 0x07));
}

TEST(neutral_reports_go_first) {
  Arbiter a;
  Output o;
  a.add(&o);
  FakePort p;
  o.press(0, 0x07);
  o.press_button(1);
  a.collect();
  a.neutral();
  a.collect();  // owners still hold state: re-queued after the neutral reports
  p.drain(a);
  CHECK(p.sent.size() >= 3);
  CHECK(p.sent[0].id == REPORT_KEYBOARD && p.sent[1].id == REPORT_MOUSE && p.sent[2].id == REPORT_CONSUMER);
  bool zero = true;
  for (int i = 0; i < 3; i++)
    for (int b = 0; b < p.sent[i].len; b++) zero &= p.sent[i].data[b] == 0;
  CHECK(zero);
}

TEST(gate_closed_sends_only_neutral) {
  Arbiter a;
  Output o;
  a.add(&o);
  FakePort p;
  o.press(0, 0x07);
  a.collect();
  p.drain(a);
  p.sent.clear();
  a.set_gate(false);
  a.move(5, 5);
  a.pulse_key(0, 0x09);
  a.collect();
  p.drain(a);
  CHECK(p.sent.size() == 3);
  CHECK(a.sent_keys().empty());
  a.set_gate(true);
  a.collect();
  p.drain(a);
  CHECK(a.sent_keys().has(0x07));
}

TEST(keyboard_before_mouse) {
  Arbiter a;
  Output keys, mouse;
  a.add(&keys);
  a.add(&mouse);
  FakePort p;
  mouse.press_button(1);
  a.move(3, 0);
  keys.press(0, 0x07);
  a.collect();
  p.drain(a);
  CHECK(p.sent.size() == 3);
  CHECK(p.sent[0].id == REPORT_KEYBOARD && p.sent[1].id == REPORT_MOUSE && p.sent[2].id == REPORT_MOUSE);
  CHECK(p.sent[1].data[0] == 1 && p.sent[1].data[1] == 0);  // button edge first
  CHECK(p.sent[2].data[0] == 1 && p.sent[2].data[1] == 3);  // then movement, button still held
}

TEST(movement_is_clamped_and_carried) {
  Arbiter a;
  FakePort p;
  a.move(300, -5);
  p.drain(a);
  CHECK(p.sent.size() == 3);
  int sum = 0;
  for (auto& r : p.sent) sum += (int8_t)r.data[1];
  CHECK(sum == 300);
  CHECK((int8_t)p.sent[0].data[1] == 127 && (int8_t)p.sent[0].data[2] == -5);
}

TEST(owners_are_ored_and_released_separately) {
  Arbiter a;
  Output one, two;
  a.add(&one);
  a.add(&two);
  FakePort p;
  one.press_button(1);
  two.press_button(1);
  a.collect();
  p.drain(a);
  one.clear();
  a.collect();
  p.drain(a);
  CHECK(a.sent_buttons() == 1);
  CHECK(p.sent.size() == 1);
}

TEST(pulse_sends_press_then_release) {
  Arbiter a;
  FakePort p;
  a.pulse_consumer(CC_VOL_UP);
  a.pulse_consumer(CC_VOL_UP);
  p.drain(a);
  CHECK(p.sent.size() == 4);
  CHECK(p.sent[0].id == REPORT_CONSUMER && p.sent[0].data[0] == 1 && p.sent[1].data[0] == 0);
}

void run_hid_tests() {
  RUN(chord_goes_out_in_one_report);
  RUN(tap_within_busy_endpoint_is_not_lost);
  RUN(neutral_reports_go_first);
  RUN(gate_closed_sends_only_neutral);
  RUN(keyboard_before_mouse);
  RUN(movement_is_clamped_and_carried);
  RUN(owners_are_ored_and_released_separately);
  RUN(pulse_sends_press_then_release);
}
