#ifdef MACROPAD_SELFTEST

#include "selftest.h"

#include <Arduino.h>
#include <Preferences.h>
#include <stdarg.h>
#include <string.h>

#include "app.h"
#include "cdc.h"
#include "display.h"
#include "pins.h"
#include "store.h"
#include "tools/tools.h"
#include "usb.h"

namespace mp {
namespace {

// Protothread-style script: WAIT() returns and resumes at the same point on a
// later call. Only static variables survive across waits.
int pc = 0;
uint32_t wake = 0;
#define WAIT_(ms, n)   \
  do {                 \
    wake = now + (ms); \
    pc = n;            \
    return;            \
    case n:;           \
  } while (0)
#define WAIT(ms) WAIT_(ms, __COUNTER__ + 1)
#define PRESS(code) pins_drive(code, true)
#define LIFT(code) pins_drive(code, false)
#define TAP(code) \
  PRESS(code);    \
  WAIT(60);       \
  LIFT(code);     \
  WAIT(60)
#define HOLD(code) \
  PRESS(code);     \
  WAIT(800);       \
  LIFT(code);      \
  WAIT(100)
constexpr int REPORT = 9000;  // the report step, jumped to on an abort
constexpr int DONE = 9999;

struct Check {
  const char* name;
  bool pass;
};
Check checks[64];
uint8_t nchecks = 0;
void check(const char* name, bool pass) {
  if (nchecks < 64) checks[nchecks++] = Check{name, pass};
}

// Keyboard report hook: latency, chords, modifiers, F13 and F17.
constexpr int TRIALS = 200;
uint32_t lat[TRIALS];
int nlat = 0;
bool lat_pending = false;
uint8_t lat_usage = 0;
uint32_t lat_t0 = 0;
bool chord_pending = false;
uint8_t chord_usages[4];
int chord_ok = 0, chord_split = 0;
uint32_t kb_reports = 0, mods_seen = 0, f17_reports = 0, f13_reports = 0;

void on_keys(const Keys& k) {
  kb_reports++;
  if (k.mods) mods_seen++;
  if (k.has(0x6C)) f17_reports++;
  if (k.has(0x68)) f13_reports++;
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

uint8_t osu(int k) { return (uint8_t)app_runtime().keymaps[0].keys[k].usage; }

int32_t stored_param(uint8_t kind, uint8_t j) {
  static uint8_t buf[CFG_SIZE];
  size_t n = store_read(buf);
  if (!n || config_validate(buf, n).code) return -1;
  static Config c;
  config_decode(buf, c);
  return c.params[kind - 1][j];
}

// The report survives the restart that ends a full run: the USB-loss check
// uses a soft detach, after which this core's USB driver is unreliable for CDC
// traffic, so the script saves its report to NVS and restarts.
constexpr const char* REPORT_NS = "mp-selftest";
char saved[3072];
uint16_t saved_len = 0;
bool to_saved = false;

void rep(const char* fmt, ...) {
  char line[Link::MAX_LINE];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (!to_saved) {
    text_printf("%s", line);
    return;
  }
  size_t len = strlen(line);
  if (saved_len + len + 1 > sizeof saved) return;
  memcpy(saved + saved_len, line, len + 1);
  saved_len += len + 1;
}

void report() {
  rep("=== selftest results ===");
  int fails = 0;
  for (uint8_t i = 0; i < nchecks; i++) {
    rep("%s %s", checks[i].pass ? "PASS" : "FAIL", checks[i].name);
    fails += !checks[i].pass;
  }
  if (nlat)
    rep("latency_us n=%d min=%lu p50=%lu p99=%lu max=%lu", nlat, (unsigned long)lat[0],
                (unsigned long)lat[nlat / 2], (unsigned long)p99(), (unsigned long)lat[nlat - 1]);
  rep("chords ok=%d split=%d", chord_ok, chord_split);
  rep("kb_reports=%lu mods_seen=%lu f17_reports=%lu", (unsigned long)kb_reports,
              (unsigned long)mods_seen, (unsigned long)f17_reports);
  rep("loop max_gap_us=%lu gaps_over_2ms=%lu loops=%lu", (unsigned long)stats.max_gap_us,
              (unsigned long)stats.gaps_over_2ms, (unsigned long)stats.loops);
  rep("input_dropped=%lu hid_overflows=%lu display_online=%d", (unsigned long)input.dropped(),
              (unsigned long)hid.overflows(), display_online());
  rep("=== %d checks, %d failed ===", nchecks, fails);
}

}  // namespace

void selftest_begin() {
  hid.on_keys_sent = on_keys;
  text_printf("selftest: boot, reset reason %d", (int)esp_reset_reason());
  // A report saved before the restart that ended the last run: hand it out
  // at the next knock instead of running again.
  Preferences p;
  if (!p.begin(REPORT_NS, false)) return;
  size_t n = p.getBytesLength("report");
  if (n && n <= sizeof saved && p.getBytes("report", saved, n) == n) {
    for (size_t at = 0; at < n;) {
      size_t len = strnlen(saved + at, n - at);
      text_printf("%.*s", (int)len, saved + at);
      at += len + 1;
    }
    text_printf("selftest: done (knock to read)");
    pc = DONE;
  }
  if (n) p.remove("report");
  p.end();
}

void selftest_restart() {
  pc = 0;
  wake = 0;
  nchecks = 0;
  nlat = 0;
  lat_pending = chord_pending = false;
  chord_ok = chord_split = 0;
  kb_reports = mods_seen = f17_reports = f13_reports = 0;
  stats.measuring = false;
  stats.max_gap_us = stats.gaps_over_2ms = stats.loops = 0;
}

void selftest_step(uint32_t now) {
  static uint8_t K[4], KILL, NAV;
  static int i, j;
  static int32_t v;
  static uint32_t t, calls;
  static bool running = false;

  // Never drive pins outside normal mode: stop and release everything.
  if (running && core.config_mode()) {
    pins_release_all();
    running = false;
    check("script ran to the end (stopped: mode changed)", false);
    pc = REPORT;
    wake = now;
  }
  if (!due(now, wake)) return;
  switch (pc) {
    case 0:
      if (!usb_up()) return;
      WAIT(3000);
      if (!app_fixture_running() || core.config_mode()) {
        text_printf("selftest: fixture not running in normal mode; no pins driven");
        pc = DONE;
        return;
      }
      running = true;
      for (i = 0; i < 4; i++) K[i] = app_runtime().key_pins[i];
      KILL = app_runtime().button_pins[0];
      NAV = app_runtime().button_pins[1];
      check("boot: osu shown, not killed, tools off",
            core.screen() == Core::Screen::Keys && core.set() == 0 && !core.killed() && !core.enabled(0));

      // --- Arming: killswitch on, arm autoclicker and jitter, killswitch off ---
      TAP(KILL);
      check("killswitch engages on press", core.killed());
      HOLD(NAV);
      check("nav hold opens the menu", core.in_menu() && core.cursor() == 0);
      TAP(K[1]);
      TAP(K[2]);
      check("menu: open the autoclicker", core.screen() == Core::Screen::Tool && core.tool() == 0);
      TAP(NAV);
      check("arming: autoclicker enabled but not running", core.enabled(0) && !core.active(0));
      HOLD(NAV);
      TAP(K[1]);
      TAP(K[2]);
      TAP(NAV);
      check("arming: jitter enabled but not running", core.enabled(1) && !core.active(1));
      HOLD(NAV);
      TAP(K[0]);
      TAP(K[0]);
      TAP(K[2]);  // Keys: the set list
      check("Keys opens the set list", core.screen() == Core::Screen::SetList && core.list_cursor() == 0);
      TAP(K[2]);
      check("set list opens osu", core.screen() == Core::Screen::Keys && core.set() == 0);
      TAP(KILL);
      WAIT(50);
      check("killswitch tap starts both armed tools", !core.killed() && core.active(0) && core.active(1));

      // --- Live keys option ---
      check("live keys on by default", core.live(0));
      TAP(NAV);
      check("nav tap switches osu to low latency", !core.live(0));
      TAP(NAV);
      check("and back to live keys", core.live(0));
      PRESS(K[1]);
      PRESS(K[3]);
      WAIT(150);
      check("live keys: 2 and 4 shown as held", core.held_keys() == 0b1010);
      LIFT(K[1]);
      LIFT(K[3]);
      WAIT(500);

      // --- Latency and chords on osu, jitter and autoclicker running ---
      stats.max_gap_us = stats.gaps_over_2ms = stats.loops = 0;
      stats.measuring = true;
      calls = cdc_call_count();
      for (i = 0; i < TRIALS; i++) {
        stats.force_redraw = true;
        lat_usage = osu(i % 4);
        lat_pending = true;
        lat_t0 = micros();
        PRESS(K[i % 4]);
        WAIT(30);
        LIFT(K[i % 4]);
        WAIT(30);
      }
      for (i = 0; i < 20; i++) {
        stats.force_redraw = true;
        for (j = 0; j < 4; j++) chord_usages[j] = osu(j);
        chord_pending = true;
        for (j = 0; j < 4; j++) PRESS(K[j]);
        WAIT(40);
        for (j = 0; j < 4; j++) LIFT(K[j]);
        WAIT(40);
      }
      stats.measuring = false;
      sort_latency();
      check("no CDC function ran in normal mode", cdc_call_count() == calls);
      check("every osu press reached a report", nlat == TRIALS);
      check("latency p99 <= 1 ms (pin to report accepted)", nlat && p99() <= 1000);
      check("no loop gap over 2 ms", stats.gaps_over_2ms == 0);
      check("every chord in one report", chord_ok == 20 && chord_split == 0);
      check("tools still running after the trials", core.active(0) && core.active(1));
      check("autoclicker clicked during the trials", ((Autoclicker*)tool_for_kind(TK_CLICK))->clicks > 10);

      // --- Killswitch while an osu key is held ---
      PRESS(K[0]);
      WAIT(100);
      check("held osu key is sent", hid.sent_keys().has(osu(0)));
      TAP(KILL);
      WAIT(20);
      check("killswitch clears the held key",
            core.killed() && hid.sent_keys().empty() && hid.sent_buttons() == 0 && hid.keyboard_idle());
      TAP(KILL);
      WAIT(100);
      check("still-held key stays silent after disengaging", !core.killed() && hid.sent_keys().empty());
      LIFT(K[0]);
      WAIT(60);
      PRESS(K[0]);
      WAIT(60);
      check("fresh press works again", hid.sent_keys().has(osu(0)));
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
      check("killswitch hold: all tools off, still killed",
            core.killed() && !core.enabled(0) && !core.enabled(1));
      TAP(KILL);
      check("tap disengages after a hold", !core.killed());

      // --- Key set switching ---
      HOLD(NAV);
      TAP(K[2]);  // Keys
      TAP(K[1]);  // stream
      TAP(K[2]);
      check("set list opens the stream set", core.screen() == Core::Screen::Keys && core.set() == 1);
      t = f13_reports;
      TAP(K[0]);  // F13
      WAIT(50);
      check("stream key 1 sends F13", f13_reports > t);
      HOLD(NAV);
      TAP(K[2]);
      TAP(K[0]);
      TAP(K[2]);
      check("back on osu", core.set() == 0);

      // --- Mouse hold, through the killswitch ---
      HOLD(NAV);
      for (i = 0; i < 4; i++) {
        TAP(K[1]);
      }
      TAP(K[2]);
      TAP(NAV);
      WAIT(100);
      check("mouse hold holds its button", core.active(3) && (hid.sent_buttons() & 0x80));
      TAP(KILL);
      WAIT(20);
      check("killswitch releases the held button", hid.sent_buttons() == 0);
      TAP(KILL);
      WAIT(100);
      check("disengaging presses it again", core.active(3) && (hid.sent_buttons() & 0x80));

      // --- Settings: an edit is saved in the record after 1 s off key screens ---
      HOLD(NAV);
      for (i = 0; i < 3; i++) {
        TAP(K[0]);
      }
      TAP(K[2]);  // autoclicker
      TAP(K[3]);
      TAP(K[3]);  // min +10
      WAIT(1500);
      check("edited setting saved in the record", stored_param(TK_CLICK, 0) == 92);
      TAP(K[2]);
      TAP(K[2]);  // back to 82
      WAIT(1500);
      check("restored setting saved in the record", stored_param(TK_CLICK, 0) == 82);

      // --- Keyspam ---
      HOLD(NAV);
      TAP(K[1]);
      TAP(K[1]);
      TAP(K[2]);  // keyspam
      t = f17_reports;
      TAP(NAV);
      WAIT(1500);
      check("keyspam sends its key", core.active(2) && f17_reports >= t + 4);
      TAP(NAV);
      WAIT(100);

      // --- USB loss: soft disconnect, then reconnect ---
      check("mouse hold running before USB loss", core.active(3));
      usb_detach(true);
      WAIT(1500);
      usb_detach(false);
      for (i = 0; i < 100 && !usb_up(); i++) {
        WAIT(100);
      }
      WAIT(1000);
      check("after USB returns: killed, mouse hold still armed",
            core.killed() && core.enabled(3) && !core.active(3));
      TAP(KILL);
      WAIT(100);
      check("a tap resumes it", !core.killed() && core.active(3) && (hid.sent_buttons() & 0x80));

      // --- Clean up: everything off, back on osu ---
      PRESS(KILL);
      WAIT(800);
      LIFT(KILL);
      WAIT(100);
      TAP(KILL);
      HOLD(NAV);
      // Up to Keys, however many menu items there are (Configure is last).
      for (i = 0; i < 12 && core.cursor() != 0; i++) {
        TAP(K[0]);
      }
      TAP(K[2]);
      TAP(K[2]);
      WAIT(1500);
      check("clean up: nothing held, osu shown", hid.sent_buttons() == 0 && hid.sent_keys().empty() &&
                                                    core.screen() == Core::Screen::Keys && core.set() == 0);
      check("no modifier ever sent", mods_seen == 0);
      running = false;
      pins_release_all();
      // Save the report and restart (see saved_magic); the next boot hands
      // it out when the host knocks.
      to_saved = true;
      saved_len = 0;
      report();
      {
        Preferences p;
        if (p.begin(REPORT_NS, false)) {
          p.putBytes("report", saved, saved_len);
          p.end();
        }
      }
      ESP.restart();
      return;
    case REPORT:  // stopped early by a mode change: report in config mode
      report();
      text_printf("--- last frame (for a human to look at) ---");
      display_dump();
      text_printf("selftest: done (knock to read)");
      pc = DONE;
      return;
    case DONE:
      return;
  }
}

}  // namespace mp

#endif
