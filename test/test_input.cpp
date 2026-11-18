#include "../src/input.h"
#include "check.h"

using namespace mp;

static int count(Input& in, Ev type, Src src = Src::Key, int index = -1) {
  int n = 0;
  Event e;
  while (in.pop(e))
    if (e.type == type && e.src == src && (index < 0 || e.index == index)) n++;
  return n;
}

static Input make(uint8_t keys = 4, uint8_t buttons = 2, bool encoder = false) {
  Input in;
  InputConfig c;
  c.keys = keys;
  c.buttons = buttons;
  c.encoder = encoder;
  in.begin(c);
  return in;
}

TEST(glitch_filter_needs_two_samples) {
  Input in = make();
  in.sample(1, 0, 0, 0);
  in.sample(0, 0, 0, 1);
  CHECK(count(in, Ev::Press) == 0);
  in.sample(1, 0, 0, 2);
  in.sample(1, 0, 0, 3);
  CHECK(count(in, Ev::Press) == 1);
}

TEST(release_waits_for_debounce) {
  Input in = make();
  in.sample(1, 0, 0, 0);
  in.sample(1, 0, 0, 0);
  count(in, Ev::Press);
  in.sample(0, 0, 0, 10);
  in.sample(1, 0, 0, 12);  // bounce
  in.sample(0, 0, 0, 13);
  in.sample(0, 0, 0, 17);
  CHECK(count(in, Ev::Release) == 0);
  in.sample(0, 0, 0, 18);
  CHECK(count(in, Ev::Release) == 1);
}

TEST(repeat_after_delay) {
  Input in = make();
  in.sample(2, 0, 0, 0);
  in.sample(2, 0, 0, 0);
  for (uint32_t t = 1; t <= 600; t++) in.sample(2, 0, 0, t);
  // Repeats at 400, 480 and 560 ms.
  CHECK(count(in, Ev::Repeat, Src::Key, 1) == 3);
}

TEST(suppressed_key_is_silent_until_released) {
  Input in = make();
  in.sample(1, 0, 0, 0);
  in.sample(1, 0, 0, 0);
  count(in, Ev::Press);
  in.suppress_held();
  for (uint32_t t = 1; t < 1000; t++) in.sample(1, 0, 0, t);
  in.sample(0, 0, 0, 1000);
  in.sample(0, 0, 0, 1010);
  Event e;
  int n = 0;
  while (in.pop(e)) n++;
  CHECK(n == 0);
  in.sample(1, 0, 0, 1020);
  in.sample(1, 0, 0, 1021);
  CHECK(count(in, Ev::Press) == 1);
}

TEST(button_tap_and_hold) {
  Input in = make();
  in.sample(0, 1, 0, 0);
  in.sample(0, 1, 0, 0);
  in.sample(0, 0, 0, 100);
  in.sample(0, 0, 0, 110);
  CHECK(count(in, Ev::Tap, Src::Button, 0) == 1);

  in.sample(0, 1, 0, 200);
  in.sample(0, 1, 0, 200);
  in.sample(0, 1, 0, 799);
  CHECK(count(in, Ev::Hold, Src::Button) == 0);
  in.sample(0, 1, 0, 800);
  in.sample(0, 1, 0, 900);
  in.sample(0, 0, 0, 1000);
  in.sample(0, 0, 0, 1010);
  int holds = 0, taps = 0, releases = 0;
  Event e;
  while (in.pop(e)) {
    holds += e.type == Ev::Hold;
    taps += e.type == Ev::Tap;
    releases += e.type == Ev::Release;
  }
  CHECK(holds == 1 && taps == 0 && releases == 1);
}

TEST(encoder_detents) {
  Input in = make(4, 2, true);
  // One full clockwise cycle: 00 -> 01 -> 11 -> 10 -> 00 (A is bit 1).
  const uint8_t cw[] = {0, 1, 3, 2, 0};
  for (uint8_t s : cw) in.sample(0, 0, s, 0);
  Event e;
  int sum = 0, n = 0;
  while (in.pop(e))
    if (e.type == Ev::Turn) {
      sum += e.delta;
      n++;
    }
  CHECK(n == 1 && (sum == 1 || sum == -1));
  int first = sum;
  const uint8_t ccw[] = {0, 2, 3, 1, 0};
  for (uint8_t s : ccw) in.sample(0, 0, s, 0);
  sum = 0;
  while (in.pop(e))
    if (e.type == Ev::Turn) sum += e.delta;
  CHECK(sum == -first);
}

void run_input_tests() {
  RUN(glitch_filter_needs_two_samples);
  RUN(release_waits_for_debounce);
  RUN(repeat_after_delay);
  RUN(suppressed_key_is_silent_until_released);
  RUN(button_tap_and_hold);
  RUN(encoder_detents);
}
