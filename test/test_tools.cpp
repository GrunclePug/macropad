#include "../src/tools/tools.h"
#include "check.h"
#include "fake_port.h"

using namespace mp;

TEST(autoclicker_hold_counts_from_acceptance) {
  Arbiter a;
  Rng rng;
  ToolCtx c{a, rng};
  Autoclicker t;
  int32_t slot[8] = {50, 50, 0};
  t.bind(slot);
  a.add(&t.out);
  FakePort p;
  t.start(c, 0);
  uint32_t down_at = 0, up_at = 0;
  for (uint32_t now = 0; now < 200 && !up_at; now++) {
    p.busy = now >= 50 && now < 80;  // endpoint stalls right as the click is due
    t.tick(c, now);
    a.collect();
    size_t before = p.sent.size();
    a.service(p);
    if (p.sent.size() > before) {
      const Report& r = p.sent.back();
      if (r.id == REPORT_MOUSE && r.data[0] && !down_at) down_at = now;
      if (r.id == REPORT_MOUSE && !r.data[0] && down_at) up_at = now;
    }
  }
  CHECK(down_at >= 80);
  CHECK(up_at - down_at >= Autoclicker::HOLD_MS);
}

TEST(autoclicker_keeps_one_click_in_flight) {
  Arbiter a;
  Rng rng;
  ToolCtx c{a, rng};
  Autoclicker t;
  int32_t slot[8] = {20, 20, 0};
  t.bind(slot);
  a.add(&t.out);
  FakePort p;
  p.busy = true;
  t.start(c, 0);
  for (uint32_t now = 0; now < 500; now++) {
    t.tick(c, now);
    a.collect();
    a.service(p);
  }
  p.busy = false;
  p.drain(a);
  CHECK(p.sent.size() == 1);  // only the one pending button-down
}

TEST(autoclicker_button_change_mid_click_releases_the_old_button) {
  Arbiter a;
  Rng rng;
  ToolCtx c{a, rng};
  Autoclicker t;
  int32_t slot[8] = {50, 50, 0};
  t.bind(slot);
  a.add(&t.out);
  FakePort p;
  t.start(c, 0);
  for (uint32_t now = 0; now < 300; now++) {
    if (now == 52) slot[2] = 1;  // Right, while Left is down
    t.tick(c, now);
    a.collect();
    a.service(p);
  }
  CHECK(t.out.buttons == 0 || t.out.buttons == 2);
  CHECK(!(t.out.buttons & 1));
}

TEST(autoclicker_skips_a_click_that_fell_due_mid_click) {
  Arbiter a;
  Rng rng;
  ToolCtx c{a, rng};
  Autoclicker t;
  int32_t slot[8] = {50, 50, 0};
  t.bind(slot);
  a.add(&t.out);
  FakePort p;
  t.start(c, 0);
  uint32_t up_at = 0, next_down = 0;
  for (uint32_t now = 0; now < 400 && !next_down; now++) {
    p.busy = now >= 51 && now < 120;  // the click takes longer than the interval
    t.tick(c, now);
    a.collect();
    size_t before = p.sent.size();
    a.service(p);
    if (p.sent.size() > before) {
      uint8_t b = p.sent.back().data[0];
      if (!b && !up_at && p.sent.size() > 1) up_at = now;
      if (b && up_at) next_down = now;
    }
  }
  CHECK(up_at && next_down > up_at && next_down - up_at >= 40);
}

TEST(jitter_edit_mid_cycle_still_returns) {
  Arbiter a;
  Rng rng;
  ToolCtx c{a, rng};
  Jitter t;
  int32_t slot[8] = {100, 1000, 1000};
  t.bind(slot);
  FakePort p;
  t.start(c, 0);
  for (uint32_t now = 0; now < 500; now++) {
    if (now == 21) slot[0] = 50;  // during the outward leg's end
    t.tick(c, now);
    a.service(p);
  }
  p.drain(a);
  int x = 0;
  for (auto& r : p.sent) x += (int8_t)r.data[1];
  CHECK(x == 0);
}

TEST(jitter_returns_to_where_it_started) {
  Arbiter a;
  Rng rng;
  ToolCtx c{a, rng};
  Jitter t;
  int32_t slot[8] = {100, 1000, 1000};
  t.bind(slot);
  FakePort p;
  t.start(c, 0);
  for (uint32_t now = 0; now < 500; now++) {
    t.tick(c, now);
    a.service(p);
  }
  p.drain(a);
  int x = 0, peak = 0;
  for (auto& r : p.sent) {
    x += (int8_t)r.data[1];
    if (x > peak) peak = x;
  }
  CHECK(x == 0 && peak == 100);
}

TEST(keyspam_presses_and_releases_its_set) {
  Arbiter a;
  Rng rng;
  ToolCtx c{a, rng};
  Keyspam t;
  int32_t slot[8] = {0x04, 100, 100, 50};  // A
  t.bind(slot);
  a.add(&t.out);
  FakePort p;
  t.start(c, 0);
  for (uint32_t now = 0; now < 1000; now++) {
    t.tick(c, now);
    a.collect();
    a.service(p);
  }
  int presses = 0;
  for (auto& r : p.sent)
    if (r.data[1] & 0x10) presses++;  // usage 0x04: byte 0, bit 4
  CHECK(presses >= 8 && presses <= 10);
  CHECK(a.sent_keys().empty() || t.out.keys.has(0x04));
}

TEST(mousehold_follows_its_setting) {
  Arbiter a;
  Rng rng;
  ToolCtx c{a, rng};
  Mousehold t;
  int32_t slot[8] = {0};
  t.bind(slot);
  t.start(c, 0);
  CHECK(t.out.buttons == 1);
  slot[0] = 2;  // Both
  t.tick(c, 1);
  CHECK(t.out.buttons == 3);
}

void run_tool_tests() {
  RUN(autoclicker_hold_counts_from_acceptance);
  RUN(autoclicker_keeps_one_click_in_flight);
  RUN(autoclicker_button_change_mid_click_releases_the_old_button);
  RUN(autoclicker_skips_a_click_that_fell_due_mid_click);
  RUN(jitter_edit_mid_cycle_still_returns);
  RUN(jitter_returns_to_where_it_started);
  RUN(keyspam_presses_and_releases_its_set);
  RUN(mousehold_follows_its_setting);
}
