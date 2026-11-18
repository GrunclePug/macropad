// Core behaviour on the compiled default config.
#include <string.h>

#include "check.h"
#include "rig.h"

using namespace mp;

enum { KILL = 0, NAV = 1 };
enum { M_KEYS = 0, M_CLICK = 1, M_JITTER = 2, M_SPAM = 3, M_HOLD = 4, M_CONFIGURE = 5 };
enum { T_CLICK = 0, T_JITTER = 1, T_SPAM = 2, T_HOLD = 3 };

static Config def() {
  Config c;
  config_default(c);
  return c;
}

static void open_item(Rig& r, int item) {
  r.hold_button(NAV);
  for (int i = 0; i < 10 && r.core.cursor() != item; i++) r.tap_key(1);
  r.tap_key(2);
}

static void open_set(Rig& r, int s) {
  open_item(r, M_KEYS);
  for (int i = 0; i < 10 && r.core.list_cursor() != s; i++) r.tap_key(1);
  r.tap_key(2);
}

TEST(boot_is_not_a_loss) {
  Rig r(def(), false);
  CHECK(!r.core.killed());
  r.up = true;
  r.run(10);
  CHECK(!r.core.killed() && r.core.usb_up());
  CHECK(r.core.screen() == Core::Screen::Keys && r.core.set() == 0 && !r.core.in_menu());
}

TEST(killswitch_press_engages_and_its_release_does_not_disengage) {
  Rig r(def());
  r.buttons = 1;
  r.run(5);
  CHECK(r.core.killed());
  r.buttons = 0;
  r.run(20);
  CHECK(r.core.killed());
  r.tap_button(KILL);
  CHECK(!r.core.killed());
}

TEST(arming_workflow) {
  Rig r(def());
  r.tap_button(KILL);
  open_item(r, M_CLICK);
  CHECK(r.core.screen() == Core::Screen::Tool && r.core.tool() == T_CLICK);
  r.tap_button(NAV);
  open_item(r, M_JITTER);
  r.tap_button(NAV);
  CHECK(r.core.enabled(T_CLICK) && r.core.enabled(T_JITTER));
  CHECK(!r.core.active(T_CLICK) && !r.core.active(T_JITTER));
  r.tap_button(KILL);
  CHECK(r.core.active(T_CLICK) && r.core.active(T_JITTER));
}

TEST(killswitch_hold_switches_everything_off) {
  Rig r(def());
  open_item(r, M_CLICK);
  r.tap_button(NAV);
  CHECK(r.core.active(T_CLICK));
  r.hold_button(KILL);
  CHECK(r.core.killed() && !r.core.enabled(T_CLICK));
  CHECK(r.core.banner(r.t) != nullptr);
}

TEST(usb_loss_engages_killswitch_and_keeps_tools_armed) {
  Rig r(def());
  open_item(r, M_HOLD);
  r.tap_button(NAV);
  CHECK(r.core.active(T_HOLD) && r.hid.sent_buttons());
  r.up = false;
  r.run(10);
  CHECK(r.core.killed() && r.core.enabled(T_HOLD) && !r.core.active(T_HOLD));
  r.tap_button(KILL);  // ignored while USB is down
  CHECK(r.core.killed());
  r.up = true;
  r.run(10);
  r.tap_button(KILL);
  CHECK(!r.core.killed() && r.core.active(T_HOLD) && r.hid.sent_buttons());
}

TEST(press_spanning_a_link_change_is_ignored) {
  Rig r(def());
  r.tap_button(KILL);
  r.buttons = 1;
  r.run(30);
  r.disturbed = true;
  r.run(30);
  r.buttons = 0;
  r.run(30);
  CHECK(r.core.killed());
  r.tap_button(KILL);
  CHECK(!r.core.killed());
}

TEST(held_osu_key_cleared_by_killswitch_and_silent_after) {
  Rig r(def());
  r.keys = 1;
  r.run(20);
  CHECK(r.hid.sent_keys().has(0x07));
  r.tap_button(KILL);
  CHECK(r.hid.sent_keys().empty());
  r.tap_button(KILL);
  r.run(50);
  CHECK(!r.core.killed() && r.hid.sent_keys().empty());
  r.keys = 0;
  r.run(20);
  r.keys = 1;
  r.run(20);
  CHECK(r.hid.sent_keys().has(0x07));
}

TEST(key_that_opens_a_set_does_not_type) {
  Rig r(def());
  open_item(r, M_KEYS);
  CHECK(r.core.screen() == Core::Screen::SetList);
  r.keys = 1u << 2;  // "open", which is also osu's third key
  r.run(100);
  CHECK(r.core.screen() == Core::Screen::Keys && r.hid.sent_keys().empty());
  r.keys = 0;
  r.run(20);
  r.keys = 1u << 2;
  r.run(20);
  CHECK(r.hid.sent_keys().has(0x0D));
}

TEST(key_that_opens_a_tool_does_not_repeat_into_it) {
  Rig r(def());
  r.hold_button(NAV);
  for (int i = 0; i < 10 && r.core.cursor() != M_CLICK; i++) r.tap_key(1);
  int32_t before = r.cfg.params[TK_CLICK - 1][0];
  r.keys = 1u << 2;
  r.run(1500);
  r.keys = 0;
  r.run(20);
  CHECK(r.core.screen() == Core::Screen::Tool && r.cfg.params[TK_CLICK - 1][0] == before);
}

TEST(two_keys_in_one_scan_from_the_menu_leave_nothing_held) {
  Rig r(def());
  r.hold_button(NAV);
  r.keys = (1u << 2) | (1u << 3);
  r.run(50);
  r.keys = 0;
  r.run(50);
  CHECK(r.hid.sent_keys().empty());
}

TEST(nav_tap_on_osu_toggles_live_keys_in_the_config) {
  Rig r(def());
  CHECK(r.core.live(0));
  r.tap_button(NAV);
  CHECK(!(r.cfg.sets[0].flags & SET_LIVE) && r.core.dirty());
  r.tap_button(NAV);
  CHECK(r.core.live(0));
}

TEST(switching_sets_saves_the_selection_and_flushes) {
  Rig r(def());
  open_set(r, 2);
  CHECK(r.core.screen() == Core::Screen::Keys && r.core.set() == 2 && r.cfg.selected == 2);
  CHECK(r.core.dirty() && r.core.take_flush());
  r.tap_key(3);  // media: mute, a consumer key
  bool consumer = false;
  for (auto& rep : r.port.sent) consumer |= rep.id == REPORT_CONSUMER && rep.data[0] == (1u << CC_MUTE);
  CHECK(consumer);
}

TEST(tool_edit_changes_the_running_config) {
  Rig r(def());
  open_item(r, M_CLICK);
  r.tap_key(3);
  CHECK(r.cfg.params[TK_CLICK - 1][0] == 87 && r.core.dirty());
  // min past max pushes max up (the schema's clamp)
  for (int i = 0; i < 10; i++) r.tap_key(3);
  CHECK(r.cfg.params[TK_CLICK - 1][1] >= r.cfg.params[TK_CLICK - 1][0]);
}

TEST(keyspam_key_steps_through_named_keys) {
  Rig r(def());
  open_item(r, M_SPAM);
  CHECK(r.cfg.params[TK_SPAM - 1][0] == 0x2C);  // Space
  r.tap_key(3);
  CHECK(r.cfg.params[TK_SPAM - 1][0] == 0x3A);  // F1: the next named key, not 0x2D
}

TEST(stream_toggle_arms_while_killed) {
  Rig r(def());
  open_set(r, 1);
  r.tap_button(KILL);
  r.tap_key(2);  // toggles the autoclicker
  CHECK(r.core.enabled(T_CLICK) && !r.core.active(T_CLICK));
  r.tap_key(0);  // F13, dropped while killed
  CHECK(r.hid.sent_keys().empty());
  r.tap_button(KILL);
  CHECK(r.core.active(T_CLICK));
}

TEST(config_mode_gates_output_and_cancels) {
  Rig r(def());
  open_item(r, M_CONFIGURE);
  CHECK(r.core.config_mode() && r.core.screen() == Core::Screen::Configure);
  CHECK(!r.core.killed());  // no tool was enabled: the killswitch is left alone
  r.keys = 1;
  r.run(30);
  CHECK(r.hid.sent_keys().empty());
  r.keys = 0;
  r.run(20);
  r.tap_button(KILL);  // cancel
  CHECK(!r.core.config_mode() && r.core.screen() == Core::Screen::Keys);
}

TEST(config_entry_with_a_tool_on_engages_the_killswitch) {
  Rig r(def());
  open_item(r, M_HOLD);
  r.tap_button(NAV);
  CHECK(r.core.active(T_HOLD));
  open_item(r, M_CONFIGURE);
  CHECK(r.core.config_mode() && r.core.killed() && !r.core.active(T_HOLD) && !r.hid.sent_buttons());
  r.hold_button(NAV);  // cancel
  CHECK(!r.core.config_mode() && r.core.killed());  // leaving never clears it
}

TEST(frozen_core_ignores_input) {
  Rig r(def());
  r.core.freeze();
  r.tap_button(KILL);
  r.hold_button(NAV);
  CHECK(!r.core.killed() && !r.core.in_menu());
}

// The smallest accepted layout: one key, Nav, an encoder with push.
static Config tiny() {
  Config c;
  config_default(c);
  c.key_count = 1;
  for (int i = 1; i < CFG_KEYS; i++) c.key_pins[i] = PIN_NONE;
  c.buttons[0].role = ROLE_NONE;
  c.buttons[0].pin = PIN_NONE;
  c.enc_flags = ENC_ENABLED;
  c.enc_pins[0] = 6;
  c.enc_pins[1] = 7;
  c.enc_pins[2] = 8;
  c.detent = 4;
  return c;
}

TEST(smallest_layout_validates_and_works) {
  Config c = tiny();
  uint8_t rec[CFG_SIZE];
  config_encode(c, rec);
  CHECK(config_validate(rec, CFG_SIZE).code == E_OK);
  Rig r(c);
  const int PUSH = 2;  // button slot 2
  // Menu with a push hold, then the wheel down to the autoclicker.
  r.hold_button(PUSH);
  CHECK(r.core.in_menu());
  r.turn(true);
  r.tap_button(PUSH);
  CHECK(r.core.screen() == Core::Screen::Tool && r.core.tool() == T_CLICK);
  int32_t before = r.cfg.params[TK_CLICK - 1][0];
  r.turn(true);
  CHECK(r.cfg.params[TK_CLICK - 1][0] == before + 5);
  r.tap_button(PUSH);  // next param
  CHECK(r.core.focus() == 1);
  // Back to the menu, pick a set with the wheel.
  r.hold_button(PUSH);
  for (int i = 0; i < 10 && r.core.cursor() != 0; i++) r.turn(false);
  r.tap_button(PUSH);
  CHECK(r.core.screen() == Core::Screen::SetList);
  r.turn(true);
  r.tap_button(PUSH);
  CHECK(r.core.screen() == Core::Screen::Keys && r.core.set() == 1);
  // And Configure.
  r.hold_button(PUSH);
  for (int i = 0; i < 10 && r.core.cursor() != M_CONFIGURE; i++) r.turn(true);
  r.tap_button(PUSH);
  CHECK(r.core.config_mode());
  r.hold_button(PUSH);  // cancel
  CHECK(!r.core.config_mode());
}

// A config without a killswitch button.
TEST(no_killswitch_layout_switches_tools_off_on_loss) {
  Config c;
  config_default(c);
  c.buttons[0].role = ROLE_NONE;
  c.buttons[0].pin = PIN_NONE;
  Rig r(c);
  open_item(r, M_HOLD);
  r.tap_button(NAV);
  CHECK(r.core.active(T_HOLD));
  r.up = false;
  r.run(10);
  r.up = true;
  r.run(10);
  CHECK(!r.core.killed() && !r.core.enabled(T_HOLD));
}

void run_core_tests() {
  RUN(boot_is_not_a_loss);
  RUN(killswitch_press_engages_and_its_release_does_not_disengage);
  RUN(arming_workflow);
  RUN(killswitch_hold_switches_everything_off);
  RUN(usb_loss_engages_killswitch_and_keeps_tools_armed);
  RUN(press_spanning_a_link_change_is_ignored);
  RUN(held_osu_key_cleared_by_killswitch_and_silent_after);
  RUN(key_that_opens_a_set_does_not_type);
  RUN(key_that_opens_a_tool_does_not_repeat_into_it);
  RUN(two_keys_in_one_scan_from_the_menu_leave_nothing_held);
  RUN(nav_tap_on_osu_toggles_live_keys_in_the_config);
  RUN(switching_sets_saves_the_selection_and_flushes);
  RUN(tool_edit_changes_the_running_config);
  RUN(keyspam_key_steps_through_named_keys);
  RUN(stream_toggle_arms_while_killed);
  RUN(config_mode_gates_output_and_cancels);
  RUN(config_entry_with_a_tool_on_engages_the_killswitch);
  RUN(frozen_core_ignores_input);
  RUN(smallest_layout_validates_and_works);
  RUN(no_killswitch_layout_switches_tools_off_on_loss);
}
