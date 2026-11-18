// Core behaviour on the real `pad` variant tables.
#include "../src/variant.h"
#include "../src/variants/pad.h"
#include "check.h"
#include "rig.h"

using namespace mp;

enum { KILL = 0, NAV = 1 };
enum { E_OSU = 0, E_DECK = 1, E_CLICK = 2, E_JITTER = 3, E_SPAM = 4, E_HOLD = 5 };

static void open_entry(Rig& r, int entry) {
  r.hold_button(NAV);
  while (r.core.cursor() != entry) r.tap_key(1);
  r.tap_key(2);
}

TEST(boot_is_not_a_loss) {
  Rig r(VARIANT, false);
  CHECK(!r.core.killed());
  r.up = true;
  r.run(10);
  CHECK(!r.core.killed() && r.core.usb_up());
  CHECK(r.core.shown() == E_OSU && !r.core.in_menu());
}

TEST(killswitch_press_engages_and_its_release_does_not_disengage) {
  Rig r(VARIANT);
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
  Rig r(VARIANT);
  r.tap_button(KILL);
  open_entry(r, E_CLICK);
  CHECK(r.core.shown() == E_CLICK);
  r.tap_button(NAV);
  open_entry(r, E_JITTER);
  r.tap_button(NAV);
  CHECK(r.core.enabled(pad::T_CLICK) && r.core.enabled(pad::T_JITTER));
  CHECK(!r.core.active(pad::T_CLICK) && !r.core.active(pad::T_JITTER));
  r.tap_button(KILL);
  CHECK(r.core.active(pad::T_CLICK) && r.core.active(pad::T_JITTER));
}

TEST(killswitch_hold_switches_everything_off) {
  Rig r(VARIANT);
  open_entry(r, E_CLICK);
  r.tap_button(NAV);
  CHECK(r.core.active(pad::T_CLICK));
  r.hold_button(KILL);
  CHECK(r.core.killed() && !r.core.enabled(pad::T_CLICK));
  CHECK(r.core.banner(r.t) != nullptr);
  // Also when already killed.
  open_entry(r, E_HOLD);
  r.tap_button(NAV);
  CHECK(r.core.enabled(pad::T_HOLD));
  r.hold_button(KILL);
  CHECK(r.core.killed() && !r.core.enabled(pad::T_HOLD));
}

TEST(usb_loss_engages_killswitch_and_keeps_tools_armed) {
  Rig r(VARIANT);
  open_entry(r, E_HOLD);
  r.tap_button(NAV);
  CHECK(r.core.active(pad::T_HOLD) && r.hid.sent_buttons());
  r.up = false;
  r.run(10);
  CHECK(r.core.killed() && r.core.enabled(pad::T_HOLD) && !r.core.active(pad::T_HOLD));
  r.tap_button(KILL);  // ignored while USB is down
  CHECK(r.core.killed());
  r.up = true;
  r.run(10);
  CHECK(r.core.killed() && !r.core.active(pad::T_HOLD));
  r.tap_button(KILL);
  CHECK(!r.core.killed() && r.core.active(pad::T_HOLD) && r.hid.sent_buttons());
}

TEST(reenumeration_counts_as_a_loss) {
  Rig r(VARIANT);
  r.disturbed = true;
  r.run(5);
  CHECK(r.core.killed());
  // Neutral reports went out after the return.
  CHECK(r.port.sent.size() >= 3);
}

TEST(press_spanning_a_link_change_is_ignored) {
  Rig r(VARIANT);
  r.tap_button(KILL);
  CHECK(r.core.killed());
  r.buttons = 1;
  r.run(30);
  r.disturbed = true;  // link bounces while the button is down
  r.run(30);
  r.buttons = 0;
  r.run(30);
  CHECK(r.core.killed());
  r.tap_button(KILL);
  CHECK(!r.core.killed());
}

TEST(held_osu_key_cleared_by_killswitch_and_silent_after) {
  Rig r(VARIANT);
  uint8_t d = pad::OSU[0];
  r.keys = 1;
  r.run(20);
  CHECK(r.hid.sent_keys().has(d));
  r.tap_button(KILL);
  CHECK(r.hid.sent_keys().empty());
  r.tap_button(KILL);
  r.run(50);
  CHECK(!r.core.killed() && r.hid.sent_keys().empty());
  r.keys = 0;
  r.run(20);
  r.keys = 1;
  r.run(20);
  CHECK(r.hid.sent_keys().has(d));
}

TEST(key_that_opens_a_keymap_does_not_type) {
  Rig r(VARIANT);
  r.hold_button(NAV);
  CHECK(r.core.in_menu() && r.core.cursor() == E_OSU);
  r.keys = 1u << 2;  // "open", which is also osu's third key
  r.run(100);
  CHECK(!r.core.in_menu() && r.core.shown() == E_OSU);
  CHECK(r.hid.sent_keys().empty());
  r.keys = 0;
  r.run(20);
  r.keys = 1u << 2;
  r.run(20);
  CHECK(r.hid.sent_keys().has(pad::OSU[2]));
}

TEST(key_that_opens_a_tool_does_not_repeat_into_it) {
  Rig r(VARIANT);
  r.hold_button(NAV);
  while (r.core.cursor() != E_CLICK) r.tap_key(1);
  size_t n;
  int32_t before = *pad::clicker.params(n)[0].value;
  r.keys = 1u << 2;  // "open" here, "minus" on the tool screen
  r.run(1500);       // well past the repeat delay
  r.keys = 0;
  r.run(20);
  CHECK(r.core.shown() == E_CLICK && *pad::clicker.params(n)[0].value == before);
}

TEST(two_keys_in_one_scan_from_the_menu_leave_nothing_held) {
  Rig r(VARIANT);
  r.hold_button(NAV);
  r.keys = (1u << 2) | (1u << 3);  // "open" and "back" in the same scan
  r.run(50);
  r.keys = 0;
  r.run(50);
  CHECK(r.hid.sent_keys().empty());
}

TEST(killswitch_hold_spanning_a_link_change_keeps_tools_armed) {
  Rig r(VARIANT);
  open_entry(r, E_CLICK);
  r.tap_button(KILL);
  r.tap_button(NAV);  // armed
  r.buttons = 1;
  r.run(100);
  r.disturbed = true;
  r.run(900);  // past the hold threshold
  r.buttons = 0;
  r.run(20);
  CHECK(r.core.killed() && r.core.enabled(pad::T_CLICK));
}

TEST(nav_tap_on_osu_toggles_live_keys) {
  Rig r(VARIANT);
  CHECK(r.core.live(0));  // on by default
  r.tap_button(NAV);
  CHECK(!r.core.live(0) && (r.core.live_dirty() & 1));
  r.tap_button(NAV);
  CHECK(r.core.live(0));
}

TEST(held_keys_shows_what_is_sent) {
  Rig r(VARIANT);
  r.keys = 0b1010;
  r.run(20);
  CHECK(r.core.held_keys() == 0b1010);
  r.tap_button(KILL);
  CHECK(r.core.held_keys() == 0);
  r.keys = 0;
  r.run(20);
  r.tap_button(KILL);
}

TEST(held_key_released_when_menu_opens) {
  Rig r(VARIANT);
  r.keys = 1;
  r.run(20);
  CHECK(!r.hid.sent_keys().empty());
  r.hold_button(NAV);
  CHECK(r.core.in_menu() && r.hid.sent_keys().empty());
}

TEST(settings_edit_marks_dirty_and_keymap_flushes) {
  Rig r(VARIANT);
  open_entry(r, E_CLICK);
  r.core.take_flush();
  int32_t before;
  size_t n;
  before = *pad::clicker.params(n)[0].value;
  r.tap_key(3);
  CHECK(*pad::clicker.params(n)[0].value == before + 5);
  CHECK(r.core.dirty(pad::T_CLICK) & 1);
  CHECK(!r.core.take_flush());
  open_entry(r, E_OSU);
  CHECK(r.core.take_flush());
  r.core.clean(pad::T_CLICK, 0xFFFF);
  *pad::clicker.params(n)[0].value = before;
}

TEST(deck_toggle_arms_while_killed) {
  Rig r(VARIANT);
  open_entry(r, E_DECK);
  r.tap_button(KILL);
  r.tap_key(2);  // toggles the autoclicker
  CHECK(r.core.enabled(pad::T_CLICK) && !r.core.active(pad::T_CLICK));
  r.tap_key(0);  // F13, dropped while killed
  CHECK(r.hid.sent_keys().empty());
  r.tap_button(KILL);
  CHECK(r.core.active(pad::T_CLICK));
  r.hold_button(KILL);
}

// A variant without a killswitch button.
static Mousehold nk_hold;
static Tool* const NK_TOOLS[] = {&nk_hold};
static const Keymap NK_KEYMAPS[] = {{"k", "K", {}, {}, {}, {}, true}};
static const Entry NK_ENTRIES[] = {{false, 0}, {true, 0}};
static const Variant NO_KILL = {"nokill", 4, false, 4, NK_KEYMAPS, 1, NK_TOOLS, 1, NK_ENTRIES, 2,
                                {{Role::Nav, {}}, {}, {}}, 1};

TEST(no_killswitch_variant_switches_tools_off_on_loss) {
  Rig r(NO_KILL);
  r.hold_button(0);
  r.tap_key(1);
  r.tap_key(2);
  r.tap_button(0);
  CHECK(r.core.active(0));
  r.up = false;
  r.run(10);
  r.up = true;
  r.run(10);
  CHECK(!r.core.killed() && !r.core.enabled(0) && !r.core.active(0));
}

void run_core_tests() {
  RUN(boot_is_not_a_loss);
  RUN(killswitch_press_engages_and_its_release_does_not_disengage);
  RUN(arming_workflow);
  RUN(killswitch_hold_switches_everything_off);
  RUN(usb_loss_engages_killswitch_and_keeps_tools_armed);
  RUN(reenumeration_counts_as_a_loss);
  RUN(press_spanning_a_link_change_is_ignored);
  RUN(held_osu_key_cleared_by_killswitch_and_silent_after);
  RUN(key_that_opens_a_keymap_does_not_type);
  RUN(key_that_opens_a_tool_does_not_repeat_into_it);
  RUN(two_keys_in_one_scan_from_the_menu_leave_nothing_held);
  RUN(killswitch_hold_spanning_a_link_change_keeps_tools_armed);
  RUN(nav_tap_on_osu_toggles_live_keys);
  RUN(held_keys_shows_what_is_sent);
  RUN(held_key_released_when_menu_opens);
  RUN(settings_edit_marks_dirty_and_keymap_flushes);
  RUN(deck_toggle_arms_while_killed);
  RUN(no_killswitch_variant_switches_tools_off_on_loss);
}
