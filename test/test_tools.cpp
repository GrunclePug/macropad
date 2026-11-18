#include "../src/tools/tools.h"
#include "check.h"
#include "fake_port.h"

using namespace mp;

TEST(autoclicker_hold_counts_from_acceptance) {
  Arbiter a;
  Rng rng;
  ToolCtx c{a, rng};
  Autoclicker t(50, 50);
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
  Autoclicker t(20, 20);
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

TEST(jitter_returns_to_where_it_started) {
  Arbiter a;
  Rng rng;
  ToolCtx c{a, rng};
  Jitter t(100, 1000, 1000);
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
  static const KeySet sets[] = {{"A", 0, {0x04, 0x05}}};
  static const char* const names[] = {"A"};
  Arbiter a;
  Rng rng;
  ToolCtx c{a, rng};
  Keyspam t(sets, names, 1, 100, 100, 50);
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
    if (r.data[1] & 0x30) presses++;  // usages 0x04 and 0x05: byte 0, bits 4-5
  CHECK(presses >= 8 && presses <= 10);
  CHECK(a.sent_keys().empty() || t.out.keys.has(0x04));
}

TEST(mousehold_follows_its_setting) {
  Arbiter a;
  Rng rng;
  ToolCtx c{a, rng};
  Mousehold t;
  t.start(c, 0);
  CHECK(t.out.buttons == 1);
  size_t n;
  *t.params(n)[0].value = 2;  // Both
  t.tick(c, 1);
  CHECK(t.out.buttons == 3);
}

void run_tool_tests() {
  RUN(autoclicker_hold_counts_from_acceptance);
  RUN(autoclicker_keeps_one_click_in_flight);
  RUN(jitter_returns_to_where_it_started);
  RUN(keyspam_presses_and_releases_its_set);
  RUN(mousehold_follows_its_setting);
}
