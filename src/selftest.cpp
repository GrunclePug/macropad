#ifdef MACROPAD_SELFTEST

#include "selftest.h"

#include <Arduino.h>
#include <string.h>

#include "app.h"
#include "cdc.h"
#include "display.h"
#include "pins.h"
#include "tools/tools.h"
#include "settings.h"
#include "usb.h"
#include "variant.h"

namespace mp {
namespace {

// Protothread-style script: WAIT() returns and resumes at the same point on a
// later call. Only static variables survive across waits.
int pc = 0;
uint32_t wake = 0;
#define WAIT_(ms, n) \
  do {               \
    wake = now + (ms); \
    pc = n;          \
    return;          \
    case n:;         \
  } while (0)
#define WAIT(ms) WAIT_(ms, __COUNTER__ + 1)
#define PRESS(pin) pins_drive(pin, true)
#define LIFT(pin) pins_drive(pin, false)
#define TAP(pin)  \
  PRESS(pin);     \
  WAIT(60);       \
  LIFT(pin);      \
  WAIT(60)

struct Check {
  const char* name;
  bool pass;
};
Check checks[48];
uint8_t nchecks = 0;
void check(const char* name, bool pass) {
  if (nchecks < 48) checks[nchecks++] = Check{name, pass};
}

// Keyboard report hook: latency, chords, shift, F17.
constexpr int TRIALS = 200;
uint32_t lat[TRIALS];
int nlat = 0;
bool lat_pending = false;
uint8_t lat_usage = 0;
uint32_t lat_t0 = 0;
bool chord_pending = false;
uint8_t chord_usages[4];
int chord_ok = 0, chord_split = 0;
uint32_t kb_reports = 0, mods_seen = 0, f17_reports = 0;

void on_keys(const Keys& k) {
  kb_reports++;
  if (k.mods) mods_seen++;
  if (k.has(0x6C)) f17_reports++;  // F17
  if (lat_pending && k.has(lat_usage)) {
    if (nlat < TRIALS) lat[nlat++] = micros() - lat_t0;
    lat_pending = false;
  }
  if (chord_pending) {
    int n = 0;
    for (uint8_t u : chord_usages) n += k.has(u);
    if (n) {
      (n == 4 ? chord_ok : chord_split)++;
      chord_pending = false;
    }
  }
}

void sort_latency() {
  for (int i = 1; i < nlat; i++)
    for (int j = i; j > 0 && lat[j] < lat[j - 1]; j--) {
      uint32_t t = lat[j];
      lat[j] = lat[j - 1];
      lat[j - 1] = t;
    }
}

uint32_t p99() { return nlat ? lat[(nlat * 99 + 99) / 100 - 1] : 0; }

int tool_index(const char* id) {
  for (uint8_t i = 0; i < VARIANT.ntools; i++)
    if (!strcmp(VARIANT.tools[i]->id, id)) return i;
  return -1;
}

void report() {
  text_printf("%s", "=== selftest results ===");
  int fails = 0;
  for (uint8_t i = 0; i < nchecks; i++) {
    text_printf("%s %s\n", checks[i].pass ? "PASS" : "FAIL", checks[i].name);
    fails += !checks[i].pass;
  }
  if (nlat)
    text_printf("latency_us n=%d min=%lu p50=%lu p99=%lu max=%lu\n", nlat, (unsigned long)lat[0],
                  (unsigned long)lat[nlat / 2], (unsigned long)p99(), (unsigned long)lat[nlat - 1]);
  text_printf("chords ok=%d split=%d\n", chord_ok, chord_split);
  text_printf("kb_reports=%lu mods_seen=%lu f17_reports=%lu\n", (unsigned long)kb_reports,
                (unsigned long)mods_seen, (unsigned long)f17_reports);
  text_printf("loop max_gap_us=%lu gaps_over_2ms=%lu loops=%lu\n", (unsigned long)stats.max_gap_us,
                (unsigned long)stats.gaps_over_2ms, (unsigned long)stats.loops);
  text_printf("tx_stalls_during_trials=%lu\n", (unsigned long)cdc_link().stalls());
  text_printf("input_dropped=%lu hid_overflows=%lu display_online=%d\n",
                (unsigned long)input.dropped(), (unsigned long)hid.overflows(), display_online());
  text_printf("=== %d checks, %d failed ===\n", nchecks, fails);
}

}  // namespace

void selftest_begin() { hid.on_keys_sent = on_keys; }

void selftest_step(uint32_t now) {
  static const uint8_t* K = PINS.keys;
  static const uint8_t KILL = PINS.buttons[0], NAV = PINS.buttons[1];
  static int i, j, click, jit, spam, hold;
  static int32_t v;
  static uint32_t t;

  if (!due(now, wake)) return;
  switch (pc) {
    case 0:
      if (!usb_up()) return;
      WAIT(3000);
      text_printf("%s", "selftest: start");
      click = tool_index("click");
      jit = tool_index("jitter");
      spam = tool_index("spam");
      hold = tool_index("hold");
      check("boot: osu shown, not killed, tools off",
            !core.in_menu() && core.shown() == 0 && !core.killed() && !core.enabled(click));

      // --- Arming workflow through the real pins ---
      TAP(KILL);
      check("killswitch engages on press", core.killed());
      PRESS(NAV);
      WAIT(800);
      LIFT(NAV);
      WAIT(100);
      check("nav hold opens the menu", core.in_menu());
      TAP(K[1]);
      TAP(K[1]);
      TAP(K[2]);
      check("menu: open autoclicker", !core.in_menu() && core.shown() == 2);
      TAP(NAV);
      check("arming: autoclicker enabled but not running", core.enabled(click) && !core.active(click));
      PRESS(NAV);
      WAIT(800);
      LIFT(NAV);
      WAIT(100);
      TAP(K[1]);
      TAP(K[2]);
      TAP(NAV);
      check("arming: jitter enabled but not running", core.enabled(jit) && !core.active(jit));
      PRESS(NAV);
      WAIT(800);
      LIFT(NAV);
      WAIT(100);
      TAP(K[0]);
      TAP(K[0]);
      TAP(K[0]);
      TAP(K[2]);
      check("menu: back on osu", !core.in_menu() && core.shown() == 0);
      TAP(KILL);
      WAIT(50);
      check("killswitch tap starts both armed tools", !core.killed() && core.active(click) && core.active(jit));

      // --- Live keys option: nav tap on osu toggles it (on by default) ---
      check("live keys on by default", core.live(0));
      TAP(NAV);
      check("nav tap switches osu to low latency", !core.live(0));
      TAP(NAV);
      check("and back to live keys", core.live(0));
      PRESS(K[1]);
      PRESS(K[3]);
      WAIT(150);
      check("live keys: 2 and 4 shown as held", core.held_keys() == 0b1010);
      text_printf("%s", "--- osu frame, keys 2 and 4 held ---");
      display_dump();
      LIFT(K[1]);
      LIFT(K[3]);
      WAIT(100);

      // --- Latency and chords on osu (live keys: redraws on every press),
      // with jitter and autoclicker running ---
      // Let the self-test's own output drain first: in real use nothing is sent
      // over serial while keys are played (up to 20 s if a host is reading).
      for (i = 0; i < 2000 && cdc_link().connected() && (!text_idle() || cdc_link().reply_pending()); i++) {
        WAIT(10);
      }
      WAIT(500);
      stats.max_gap_us = stats.gaps_over_2ms = stats.loops = 0;
      stats.measuring = true;
      cdc_link().reset_stalls();
      cdc_link().count_stalls(true);
      for (i = 0; i < TRIALS; i++) {
        stats.force_redraw = true;
        lat_usage = VARIANT.keymaps[0].keys[i % 4].usage;
        lat_pending = true;
        lat_t0 = micros();
        PRESS(K[i % 4]);
        WAIT(30);
        LIFT(K[i % 4]);
        WAIT(30);
      }
      for (i = 0; i < 20; i++) {
        stats.force_redraw = true;
        for (j = 0; j < 4; j++) chord_usages[j] = VARIANT.keymaps[0].keys[j].usage;
        chord_pending = true;
        for (j = 0; j < 4; j++) PRESS(K[j]);
        WAIT(40);
        for (j = 0; j < 4; j++) LIFT(K[j]);
        WAIT(40);
      }
      stats.measuring = false;
      cdc_link().count_stalls(false);
      sort_latency();
      check("every osu press reached a report", nlat == TRIALS);
      check("latency p99 <= 1 ms (pin to report accepted)", nlat && p99() <= 1000);
      check("no loop gap over 2 ms", stats.gaps_over_2ms == 0);
      check("every chord in one report", chord_ok == 20 && chord_split == 0);
      check("tools still running after the trials", core.active(click) && core.active(jit));
      check("autoclicker clicked during the trials", ((Autoclicker*)VARIANT.tools[click])->clicks > 10);

      // --- Killswitch while an osu key is held ---
      PRESS(K[0]);
      WAIT(100);
      check("held osu key is sent", hid.sent_keys().has(VARIANT.keymaps[0].keys[0].usage));
      TAP(KILL);
      WAIT(20);
      check("killswitch clears the held key", core.killed() && hid.sent_keys().empty() &&
                                                  hid.sent_buttons() == 0 && hid.keyboard_idle());
      TAP(KILL);
      WAIT(100);
      check("still-held key stays silent after disengaging", !core.killed() && hid.sent_keys().empty());
      LIFT(K[0]);
      WAIT(60);
      PRESS(K[0]);
      WAIT(60);
      check("fresh press works again", hid.sent_keys().has(VARIANT.keymaps[0].keys[0].usage));
      LIFT(K[0]);
      WAIT(60);

      // --- Killswitch during autoclicker clicks: press it while a click is down ---
      v = 0;
      t = 0;
      for (i = 0; i < 20; i++) {
        for (j = 0; j < 500 && !(hid.sent_buttons() & 0x80); j++) {
          WAIT(1);
        }
        if (hid.sent_buttons() & 0x80) t++;
        TAP(KILL);
        WAIT(20);
        if (!(core.killed() && hid.sent_buttons() == 0 && hid.mouse_idle())) v++;
        TAP(KILL);
      }
      check("killswitch during a click leaves no button down (20 tries)", v == 0 && t == 20);

      // --- Killswitch hold switches every tool off ---
      PRESS(KILL);
      WAIT(800);
      LIFT(KILL);
      WAIT(100);
      check("killswitch hold: all tools off, still killed", core.killed() && !core.enabled(click) && !core.enabled(jit));
      TAP(KILL);
      check("tap disengages after a hold", !core.killed());

      // --- Mouse hold, through the killswitch ---
      PRESS(NAV);
      WAIT(800);
      LIFT(NAV);
      WAIT(100);
      for (i = 0; i < 5; i++) {
        TAP(K[1]);
      }
      TAP(K[2]);
      TAP(NAV);
      WAIT(100);
      check("mouse hold holds its button", core.active(hold) && (hid.sent_buttons() & 0x80));
      TAP(KILL);
      WAIT(20);
      check("killswitch releases the held button", hid.sent_buttons() == 0);
      TAP(KILL);
      WAIT(100);
      check("disengaging presses it again", core.active(hold) && (hid.sent_buttons() & 0x80));

      // --- Settings: edit, saved after 1 s idle ---
      PRESS(NAV);
      WAIT(800);
      LIFT(NAV);
      WAIT(100);
      TAP(K[0]);
      TAP(K[0]);
      TAP(K[0]);
      TAP(K[2]);  // autoclicker
      TAP(K[3]);
      TAP(K[3]);  // min +10
      WAIT(1500);
      v = -1;
      check("edited setting written to NVS", settings_read("click", "min", v) && v == 92);
      TAP(K[2]);
      TAP(K[2]);  // back to 82
      WAIT(1500);
      check("restored setting written to NVS", settings_read("click", "min", v) && v == 82);

      // --- Keyspam ---
      PRESS(NAV);
      WAIT(800);
      LIFT(NAV);
      WAIT(100);
      TAP(K[1]);
      TAP(K[1]);
      TAP(K[2]);  // keyspam
      t = f17_reports;
      TAP(NAV);
      WAIT(1500);
      check("keyspam sends its keys", core.active(spam) && f17_reports >= t + 4);
      TAP(NAV);
      WAIT(100);

      // --- USB loss: soft disconnect, then reconnect ---
      check("mouse hold running before USB loss", core.active(hold));
#ifndef MACROPAD_SELFTEST_NODETACH
      usb_detach(true);
      WAIT(1500);
      usb_detach(false);
#endif
      for (i = 0; i < 100 && !usb_up(); i++) {
        WAIT(100);
      }
      WAIT(1000);
      check("after USB returns: killed, mouse hold still armed",
            core.killed() && core.enabled(hold) && !core.active(hold));
      TAP(KILL);
      WAIT(100);
      check("a tap resumes it", !core.killed() && core.active(hold) && (hid.sent_buttons() & 0x80));

      // --- Clean up and report ---
      PRESS(KILL);
      WAIT(800);
      LIFT(KILL);
      WAIT(100);
      TAP(KILL);
      check("clean up: nothing held", hid.sent_buttons() == 0 && hid.sent_keys().empty());
      check("no modifier ever sent", mods_seen == 0);
      display_service(core, now, true);
      // The serial port went away with the USB test: wait for the host to
      // reopen it, so the one-time frame dump isn't lost.
      for (i = 0; i < 600 && !cdc_link().connected(); i++) {
        WAIT(100);
      }
      WAIT(500);
      report();
      text_printf("%s", "--- last frame (for a human to look at) ---");
      display_dump();
      for (;;) {
        WAIT(5000);
        report();
      }
  }
}

}  // namespace mp

#else
// Normal builds: the self-test isn't compiled in.
#endif
