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
int over_1ms() {
  int n = 0;
  for (int i = 0; i < nlat; i++) n += lat[i] > 1000;
  return n;
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
constexpr const char* REPORT_NS = "macropad-test";  // the self-test's namespace
bool report_from_nvs = false;  // remove it only once it has gone out
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

int cleanup_state[7] = {-1, -1, -1, -1, -1, -1, -1};
int cleanup_holds = 0;        // killswitch holds the clean-up had to repeat
int cleanup_unexplained = 0;  // of those, repeats with no link change during the hold

bool any_enabled() {
  for (uint8_t k = 0; k < core.variant().ntools; k++)
    if (core.enabled(k)) return true;
  return false;
}

void report() {
  sort_latency();  // the abort path reaches here without the trials' sort
  rep("=== selftest results ===");
  int fails = 0;
  for (uint8_t i = 0; i < nchecks; i++) {
    rep("%s %s", checks[i].pass ? "PASS" : "FAIL", checks[i].name);
    fails += !checks[i].pass;
  }
  if (nlat)
    rep("latency_us n=%d min=%lu p50=%lu p99=%lu max=%lu over_1ms=%d", nlat, (unsigned long)lat[0],
                (unsigned long)lat[nlat / 2], (unsigned long)p99(), (unsigned long)lat[nlat - 1], over_1ms());
  rep("chords ok=%d split=%d", chord_ok, chord_split);
  rep("cleanup screen=%d set=%d menu=%d cursor=%d buttons=%x keys=%d killed=%d repeated_holds=%d", cleanup_state[0],
      cleanup_state[1], cleanup_state[2], cleanup_state[3], cleanup_state[4], cleanup_state[5], cleanup_state[6],
      cleanup_holds);
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
  cdc_link().keep_logs(true);
  text_printf("selftest: boot, reset reason %d, double-tap words cleared %d", (int)esp_reset_reason(),
              (int)app_double_tap_cleared());
  // A report saved before the restart that ended the last run: hand it out
  // at the next knock instead of running again.
  // It is valid only on the boot right after the run that saved it (marked
  // "fresh"): any later boot, a re-flash included, drops it, so a stale
  // report can never pass for a new run.
  Preferences p;
  if (!p.begin(REPORT_NS, false)) return;
  bool fresh = p.getUChar("fresh", 0) == 1;
  p.putUChar("fresh", 0);
  if (p.getUChar("norun", 0)) {
    p.putUChar("norun", 0);
    pc = DONE;
    text_printf("selftest: skipped on this boot (TESTERASE norun)");
  }
  size_t n = p.getBytesLength("report");
  if (fresh && n && n <= sizeof saved && p.getBytes("report", saved, n) == n) {
    saved_len = (uint16_t)n;
    pc = DONE;
    report_from_nvs = true;  // queued at each config entry until TESTACK
  } else if (n) {
    p.remove("report");  // stale or unreadable
  }
  p.end();
}

static uint8_t tap_state = 0;
static uint32_t tap_at = 0;

void selftest_request_nav_tap() { tap_state = 1; }

void selftest_on_config_entry() {
  if (!report_from_nvs) return;
  text_clear();
  for (size_t at = 0; at < saved_len;) {
    size_t len = strnlen(saved + at, saved_len - at);
    text_printf("%.*s", (int)len, saved + at);
    at += len + 1;
  }
  text_printf("selftest: done (send TESTACK once read)");
}

void selftest_skip_next_run() {
  Preferences p;
  if (p.begin(REPORT_NS, false)) {
    p.putUChar("norun", 1);
    p.end();
  }
}

void selftest_ack_report() {
  if (!report_from_nvs) return;
  Preferences p;
  if (p.begin(REPORT_NS, false)) {
    p.remove("report");
    p.end();
  }
  report_from_nvs = false;
}

void selftest_restart() {
  text_clear();  // no tail of an earlier report reaches the next entry
  // A new run retires an unacknowledged report: it must never pass for this
  // run's result.
  if (report_from_nvs) {
    Preferences p;
    if (p.begin(REPORT_NS, false)) {
      p.remove("report");
      p.end();
    }
    report_from_nvs = false;
  }
  pc = 0;
  wake = 0;
  nchecks = 0;
  nlat = 0;
  lat_pending = chord_pending = false;
  chord_ok = chord_split = 0;
  cleanup_holds = cleanup_unexplained = 0;
  kb_reports = mods_seen = f17_reports = f13_reports = 0;
  stats.measuring = false;
  stats.max_gap_us = stats.gaps_over_2ms = stats.loops = 0;
}

void selftest_step(uint32_t now) {
  static uint8_t K[4], KILL, NAV;
  static int i, j;
  static int32_t v;
  static uint32_t t, calls, gen0;
  static bool running = false;

  // Never drive pins outside normal mode: stop and release everything.
  if (running && core.config_mode()) {
    pins_release_all();
    running = false;
    check("script ran to the end (stopped: mode changed)", false);
    pc = REPORT;
    wake = now;
  }
  // Only a Nav button on a key screen: slot 1 may be a bound host shortcut.
  if (tap_state == 1 && !core.config_mode() &&
      (core.variant().buttons[1].role != Role::Nav || core.screen() != Core::Screen::Keys)) {
    tap_state = 0;
    text_printf("selftest: TESTTAP skipped (button 2 isn't Nav, or no key screen)");
  }
  if (!running && tap_state == 1 && !core.config_mode()) {
    pins_drive(app_runtime().button_pins[1], true);
    tap_at = now;
    tap_state = 2;
  } else if (tap_state == 2 && now - tap_at >= 80) {
    pins_drive(app_runtime().button_pins[1], false);
    tap_state = 0;
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
      check("boot: planted double-tap words are cleared", app_double_tap_check());
      check("boot: the double-tap guard ran before the core's check", app_double_tap_guard_first());

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
      // Pin to report accepted. A report either makes the next USB frame or
      // waits one more (the shared endpoint), so p99 (the third-largest of
      // 200) lands near 0.6 or 1.1 ms run to run. Check the median, the share
      // that waited, p99, and that even the slowest waited one frame at most.
      check("latency median <= 0.75 ms", nlat && lat[nlat / 2] <= 750);
      // 4 % per run: at the usual ~1 % one-frame rate, 2 % of 200 fails one run in
      // 20 by chance. run.sh holds the pooled count of its runs to 2 %.
      check("latency: at most 4 % of presses over 1 ms", nlat && over_1ms() * 25 <= nlat);
      check("latency p99 <= 1.25 ms (one extra USB frame at most)", nlat && p99() <= 1250);
      check("latency: slowest press <= 1.5 ms", nlat && lat[nlat - 1] <= 1500);
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
      // A link change during the hold (the host may suspend the pad after the
      // reconnect above) discards it, by design: hold again until it took.
      for (j = 0; j < 4; j++) {
        gen0 = core.kill_holds_discarded();
        PRESS(KILL);
        WAIT(800);
        LIFT(KILL);
        WAIT(100);
        if (!any_enabled()) break;
        cleanup_holds++;
        // The core counts a hold it ignored because the link changed during
        // the press; any other missed hold is unexplained.
        if (core.kill_holds_discarded() == gen0) cleanup_unexplained++;
      }
      TAP(KILL);
      HOLD(NAV);
      // Up to Keys, however many menu items there are (Configure is last).
      for (i = 0; i < 12 && core.cursor() != 0; i++) {
        TAP(K[0]);
      }
      TAP(K[2]);
      TAP(K[2]);
      WAIT(1500);
      check("clean up: every tool off, nothing held, osu shown",
            !any_enabled() && hid.sent_buttons() == 0 && hid.sent_keys().empty() &&
                core.screen() == Core::Screen::Keys && core.set() == 0);
      check("clean up: a hold was repeated only after a link change", cleanup_unexplained == 0);
      cleanup_state[0] = (int)core.screen();
      cleanup_state[1] = core.set();
      cleanup_state[2] = core.in_menu();
      cleanup_state[3] = core.cursor();
      cleanup_state[4] = hid.sent_buttons();
      cleanup_state[5] = !hid.sent_keys().empty();
      cleanup_state[6] = core.killed();
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
          p.putUChar("fresh", 1);
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
