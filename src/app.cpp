#include "app.h"

#include <Arduino.h>
#include <esp32s3/rom/cache.h>
#include <double_tap.h>  // the core's (variants/arduino_nano_nora)
#include <esp32s3/rom/rtc.h>
#include <esp32s3/spiram.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <soc/soc.h>

#include "boot.h"
#include "cdc.h"
#include "display.h"
#include "knock.h"
#include "led.h"
#include "pins.h"
#include "proto.h"
#include "selftest.h"
#include "store.h"
#include "usb.h"
#include "version.h"

namespace mp {

Input input;
Arbiter hid;
Core core;
LoopStats stats;

namespace {

Rng rng;
Config cfg;
Runtime rt;
BootDecision boot;
uint8_t blob[CFG_SIZE];
uint32_t last_loop_us = 0;
char boot_text[24];
char cfg_status[24];
char status_text[40];

ProtoEnv env;
Proto proto(env);
Knock knock;

// Leaving config mode: wait for a CDC IN transfer still in flight before key
// output resumes (the core's USB driver can't interleave CDC and HID). If the
// host never reads it, restart: a soft USB detach would clear the endpoint,
// but leaves this core's driver unreliable for later CDC traffic.
enum class Leave : uint8_t { None, Draining } leaving = Leave::None;
uint32_t leave_at = 0;
const char* leave_msg = "";
constexpr uint32_t DRAIN_MS = 500;
constexpr uint32_t IDLE_CANCEL_MS = 60000;
// Config mode entered by a knock leaves if no app speaks within this time.
constexpr uint32_t HANDSHAKE_MS = 10000;
bool knock_entry = false;
uint32_t entered_ms = 0;

bool restart_armed = false;
uint32_t restart_at = 0;

void stage_default() {
  Config d;
#ifdef MACROPAD_SELFTEST
  config_selftest(d);
#else
  config_default(d);
#endif
  config_encode(d, env.staging);
}

// What this boot has seen of flash (see FlashState): set at boot, by a
// save, and by every read; never cleared before a restart.
FlashState flash_state = {false, false};

// Flash no longer holds what it should (unreadable, or a verified record
// gone): `cfg ok` with `crc none` (`cfg ok default` on a safe or crash boot,
// which runs the compiled default), which the client refuses to write over,
// and no pad-side saves until a restart sorts it out.
void flash_fault() {
  env.has_crc = false;
  env.stored_crc = 0;
  if (!strncmp(cfg_status, "default", 7) || !strcmp(cfg_status, "ok default"))
    strcpy(cfg_status, "ok default");
  else
    strcpy(cfg_status, "ok");
  boot.persist = false;
  core.set_unsaved(true);
}

// One read of the stored record (`rec`, `n` from store_read). `want`: the
// record just written, if any.
Flash classify(const uint8_t* rec, size_t n, const uint8_t* want) {
  if (!n) return Flash::None;
  if (n != CFG_SIZE || config_validate(rec, n).code) return Flash::Unreadable;
  return want && !memcmp(rec, want, CFG_SIZE) ? Flash::Exact : Flash::Valid;
}

// HELLO's crc from a decision. `stored`: the record read from flash;
// `staged`: the staged record.
void apply(const FlashView& v, const uint8_t* stored, const uint8_t* staged) {
  flash_state = v.state;
  if (v.fault) {
    flash_fault();
    return;
  }
  switch (v.crc) {
    case CrcFrom::None:  // the boot row stands
      env.has_crc = false;
      env.stored_crc = 0;
      break;
    case CrcFrom::Stored:
      env.has_crc = true;
      env.stored_crc = config_crc(stored);
      if (!strcmp(cfg_status, "none")) strcpy(cfg_status, "ok");
      break;
    case CrcFrom::Staged:
      env.has_crc = true;
      env.stored_crc = config_crc(staged);
      break;
  }
}

uint8_t do_commit(const uint8_t* rec) {
  uint8_t r = store_write(rec);
  if (r == E_VERIFY || r == E_WRITE) {
    // Read back once more: if flash does hold exactly this record, the save
    // happened, and it ends the normal way (OK and a restart). E_WRITE too:
    // Preferences reports any NVS error as a failed write, including one
    // after the new blob was already written.
    static uint8_t again[CFG_SIZE];
    size_t n = store_read(again);
    FlashView v = after_failed_write(classify(again, n, rec), flash_state, false);
    if (v.landed)
      r = E_OK;
    else if (!core.recovery())
      apply(v, again, nullptr);  // the same read decides
    else
      flash_state.verified = v.state.verified;  // recovery keeps "none", but remembers a valid read
  }
  if (r == E_OK) {
    core.freeze();  // terminal: no dispatch, no pad input until the restart
    restart_armed = true;
    restart_at = millis() + 200;
    core.set_status("Saving...");
  }
  return r;
}

void begin_leave(const char* msg) {
  if (leaving != Leave::None || restart_armed) return;
  leaving = Leave::Draining;
  leave_at = millis();
  leave_msg = msg;
  // Keep a queued reply (EXIT's OK goes out during the drain); drop logs.
  cdc_link().drop_logs();
}

void do_exit() { begin_leave("Config closed"); }

void plant_double_tap();  // below, with the guard

void do_test(const char* what) {
#ifdef MACROPAD_SELFTEST
  if (!strcmp(what, "crash")) abort();
  if (!strcmp(what, "corrupt")) {
    static uint8_t bad[CFG_SIZE];
    config_encode(cfg, bad);
    bad[12] ^= 0xFF;  // break the CRC
    store_write(bad);
    restart_armed = true;
    restart_at = millis() + 200;
  }
  if (!strcmp(what, "doubletap")) {
    plant_double_tap();  // what a reset in the core's boot window leaves behind
    restart_armed = true;
    restart_at = millis() + 200;
  }
  if (!strcmp(what, "run")) {
    selftest_restart();
    begin_leave("Self-test");
  }
  if (!strcmp(what, "tap")) selftest_request_nav_tap();  // after the next leave
  if (!strcmp(what, "ack")) selftest_ack_report();
  if (!strncmp(what, "erase", 5)) {  // a fresh pad: no record, then restart
    store_erase();
    if (!strcmp(what, "erase-norun")) selftest_skip_next_run();
    restart_armed = true;
    restart_at = millis() + 200;
  }
#else
  (void)what;
#endif
}

// HELLO reports the stored record's crc: keep it current after pad-side saves.
// From the record store_write() just verified, not a second read.
void note_stored() {
  flash_state.verified = true;
  env.has_crc = true;
  env.stored_crc = store_saved_crc();
  // A fresh pad's first save creates the record (pad-side saves only run on
  // the "none" and "ok" boot rows).
  if (env.has_crc && !strcmp(cfg_status, "none")) strcpy(cfg_status, "ok");
}

void enter_config(bool recovery, bool by_knock = false) {
  knock_entry = by_knock;
  entered_ms = millis();
  core.enter_config(recovery);
  cdc_link().start();
  proto.reset();
  knock.reset();
  env.recovery = recovery;
#ifdef MACROPAD_SELFTEST
  selftest_on_config_entry();  // re-sends an unacknowledged report
#endif
  // Pad-side edits not yet saved go to flash first, so Connect shows them and
  // a Save can't overwrite them. If that write fails, staging is the running
  // config (with the edits), and the read-back decides what HELLO reports.
  if (!recovery && boot.persist && core.dirty()) {
    config_encode(cfg, env.staging);
    if (store_write(env.staging) == E_OK) {
      core.clean();
      note_stored();
    } else {
      static uint8_t back[CFG_SIZE];
      size_t m = store_read(back);
      FlashView v = after_failed_write(classify(back, m, env.staging), flash_state, true);
      apply(v, back, env.staging);
      if (!v.landed) return;  // staging stays the running config
      core.clean();
    }
  }
  if (recovery) {
    stage_default();
    return;
  }
  // Staging: the stored blob if it validates, otherwise the running config.
  // When it's the stored record, HELLO reports exactly its crc, so what a
  // client reads and the crc it's told always agree.
  size_t n = store_read(env.staging);
  FlashView v = at_entry(classify(env.staging, n, nullptr), flash_state);
  if (v.crc != CrcFrom::Stored) config_encode(cfg, env.staging);
  apply(v, env.staging, nullptr);
}

void serve_config(uint32_t now) {
  Link& l = cdc_link();
  if (l.watch()) {  // raw DTR fell: a session break
    if (core.recovery())
      stage_default();
    else if (proto.got_valid())
      begin_leave("Cancelled");
    proto.reset();
  }
  proto.set_now(now);
  if (leaving == Leave::None && !restart_armed) proto.step(l);
  // due(): signed, so a leave_at or entered_ms sampled after `now` can't wrap.
  if (!core.recovery() && proto.got_valid() && due(now, proto.last_valid_ms() + IDLE_CANCEL_MS))
    begin_leave("Timed out");
  // A knock with no app behind it (a crashed client, a stray line toggle).
  if (knock_entry && !proto.got_valid() && due(now, entered_ms + HANDSHAKE_MS)) begin_leave("No app");
  if (!restart_armed) {
    if (core.recovery()) {
      snprintf(status_text, sizeof status_text, "Error %s: open the app", cfg_status + 8);
      core.set_status(status_text);
    } else {
      core.set_status(proto.got_valid() ? "App connected" : "Waiting for app");
    }
  }
  if (leaving == Leave::None) text_service();  // a leave drains, it doesn't refill
  l.service();
}

void finish_leave(uint32_t now) {
  if (leaving == Leave::Draining) {
    if (!cdc_tx_busy() && !cdc_link().output_pending()) {
      leaving = Leave::None;
      core.leave_config();
      knock.reset();
      core.notice(leave_msg, now, 1500);
    } else if (due(now, leave_at + DRAIN_MS)) {
      ESP.restart();  // boots with every tool off
    }
  }
}

// The core's double-tap reset (variants/arduino_nano_nora/variant.cpp and
// double_tap.c) keeps three magic words in PSRAM, 0x20 bytes below its end.
// A global constructor (init_priority 101) sets them for ~0.9 s at every boot
// and switches to the factory recovery image when it finds them. A reset in
// that window leaves them behind, and on this board the recovery image doesn't
// see them: every boot then went to the recovery image and back until a power
// cycle (PLAN 9ze). So clear them before the core's check (priority 100 runs
// first: the linker sorts .ctors.* and IDF runs the table from the end; that
// also puts the core's constructor after every default-priority one, harmless
// since constructors never touch hardware). Only after a software restart:
// every boot of the loop is one, and a real double tap is a hardware reset, so
// it still reaches recovery mode on a board where that works. The spot is
// inside the PSRAM heap, so only the exact words are cleared, and the line is
// written out to PSRAM.
const uint32_t DOUBLE_TAP_AT = SOC_EXTRAM_DATA_HIGH - 0x20 - 3 * sizeof(uint32_t);
const uint32_t DOUBLE_TAP_WORDS[3] = {0xf01681de, 0xbd729b29, 0xd359be7a};  // double_tap.c

// True if it found the words and cleared them.
bool clear_double_tap() {
  if (!esp_spiram_is_initialized()) return false;
  volatile uint32_t* m = (volatile uint32_t*)DOUBLE_TAP_AT;
  bool found = m[0] == DOUBLE_TAP_WORDS[0] && m[1] == DOUBLE_TAP_WORDS[1] && m[2] == DOUBLE_TAP_WORDS[2];
  if (found) m[0] = m[1] = m[2] = 0;
  Cache_WriteBack_Addr(DOUBLE_TAP_AT, sizeof DOUBLE_TAP_WORDS);
  return found;
}

// The words as the core leaves them, by the core's own code: TESTDOUBLETAP.
void plant_double_tap() {
  if (esp_spiram_is_initialized()) double_tap_mark();
}

int64_t double_tap_guard_us = -1;  // when the guard ran (the self-test checks it ran first)
bool double_tap_cleared = false;   // this boot's guard found the words (TESTDOUBLETAP's proof)
int64_t setup_us = -1;

struct DoubleTapGuard {
  DoubleTapGuard() {
    if (rtc_get_reset_reason(0) == RTC_SW_CPU_RESET) double_tap_cleared = clear_double_tap();
    double_tap_guard_us = esp_timer_get_time();
  }
};
DoubleTapGuard double_tap_guard __attribute__((init_priority(100)));

bool crash_reset() {
  switch (esp_reset_reason()) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
      return true;
    default:
      return false;
  }
}

// Safe boot: a pin held for 1 s at power-up. The first key pin of a valid
// stored layout, and D2 when the blob is missing or invalid or D2 is a key or
// button pin there. Only if a pin reads low at the first sample do we wait.
bool safe_held(bool valid, const Config& stored) {
  uint8_t pins[2];
  uint8_t n = 0;
  bool d2 = !valid;
  if (valid) {
    pins[n++] = stored.key_pins[0];
    for (uint8_t i = 0; i < stored.key_count; i++) d2 |= stored.key_pins[i] == 0;
    for (uint8_t b = 0; b < CFG_BUTTONS; b++) d2 |= stored.buttons[b].role && stored.buttons[b].pin == 0;
  }
  if (d2 && !(n && pins[0] == 0)) pins[n++] = 0;
  for (uint8_t i = 0; i < n; i++) {
    if (!pin_low(pins[i])) continue;
    uint32_t t0 = millis();
    while (pin_low(pins[i])) {
      if (millis() - t0 >= 1000) return true;
      delay(10);
    }
  }
  return false;
}

}  // namespace

const Runtime& app_runtime() { return rt; }
Config& app_config() { return cfg; }
bool app_fixture_running() {
  static uint8_t a[CFG_SIZE], b[CFG_SIZE];
  Config f;
  config_selftest(f);
  config_encode(f, a);
  config_encode(cfg, b);
  return !memcmp(a, b, CFG_SIZE) && (boot.status == CfgStatus::None || boot.status == CfgStatus::Ok) &&
         boot.mode == BootMode::Normal;
}

void app_setup() {
  setup_us = esp_timer_get_time();
  cdc_begin();
  uint32_t nonce = esp_random();
  led_begin();
  store_begin();

  // Boot decision.
  size_t n = store_read(blob);
  BootInput bi{};
  static Config stored;
  bool valid = false;
  if (!n) {
    bi.blob = BlobState::None;
  } else {
    CfgError e = config_validate(blob, n);
    valid = !e.code;
    bi.blob = valid ? BlobState::Valid : BlobState::Invalid;
    bi.invalid_code = e.code;
    if (valid) config_decode(blob, stored);
  }
  bi.crash_reset = crash_reset();
  bi.safe_held = safe_held(valid, stored);
  boot = boot_decide(bi);
  if (boot.use_blob) {
    cfg = stored;
  } else {
#ifdef MACROPAD_SELFTEST
    config_selftest(cfg);
#else
    config_default(cfg);
#endif
  }
  switch (boot.status) {
    case CfgStatus::Ok: strcpy(cfg_status, "ok"); break;
    case CfgStatus::None: strcpy(cfg_status, "none"); break;
    case CfgStatus::Invalid: snprintf(cfg_status, sizeof cfg_status, "invalid %u", boot.invalid_code); break;
    case CfgStatus::DefaultCrash: strcpy(cfg_status, "default crash"); break;
    case CfgStatus::DefaultSafe: strcpy(cfg_status, "default safe"); break;
  }

  config_to_variant(cfg, rt);
  pins_begin(rt);
  rng.seed(esp_random());
  core.begin(rt.v, cfg, input, hid, rng);
  core.set_unsaved(!boot.persist);

  env.fw = MACROPAD_VERSION;
  env.sn = usb_serial();
  env.nonce = nonce;
  env.cfg_status = cfg_status;
  env.has_crc = valid;
  flash_state = {valid, false};
  env.stored_crc = valid ? config_crc(blob) : 0;
  env.commit = do_commit;
  env.exit = do_exit;
  env.test = do_test;
#ifdef MACROPAD_SELFTEST
  env.selftest = true;
  env.double_tap_cleared = app_double_tap_cleared();
#endif
  if (boot.mode == BootMode::Recovery) enter_config(true);

  display_begin();
  usb_begin();
  enableLoopWDT();
  snprintf(boot_text, sizeof boot_text, "macropad %s", MACROPAD_VERSION);
  core.notice(boot_text, millis(), 1500);
#ifdef MACROPAD_SELFTEST
  selftest_begin();
#endif
}

void app_loop() {
  uint32_t us = micros();
  if (stats.measuring && last_loop_us) {
    uint32_t gap = us - last_loop_us;
    if (gap > stats.max_gap_us) stats.max_gap_us = gap;
    if (gap > 2000) stats.gaps_over_2ms++;
    stats.loops++;
  }
  last_loop_us = us;
  uint32_t now = millis();

#ifdef MACROPAD_SELFTEST
  selftest_step(now);
#endif

  uint8_t keys, buttons, enc;
  pins_read(keys, buttons, enc);
  input.sample(keys, buttons, enc, now);
  bool disturbed = usb_take_disturbed();
  core.link(usb_up(), disturbed);
  if (disturbed && core.config_mode()) {
    cdc_link().usb_break();
    proto.reset();  // no half-sent PINS/TOOLS reply carries into the next session
    if (core.recovery()) stage_default();
  }

  Event e;
  while (input.pop(e)) core.handle(e, now);
  if (core.take_config_request()) enter_config(false);

  if (!core.config_mode()) {
    // Normal mode touches no CDC endpoint: only the line state, for the knock.
    bool dtr, rts;
    cdc_line_state(dtr, rts);
    if (knock.feed(dtr, rts, now))
      enter_config(false, true);
    else if (store_service(core, cfg, boot.persist, now))  // before a newly shown key screen goes live
      note_stored();
  } else {
    serve_config(now);
  }
  if (core.take_cancel()) begin_leave("Cancelled");
  finish_leave(now);

  core.update(now);
  hid.collect();
  hid.service(usb_port());
  cdc_set_hid_idle(hid.idle());

  if (restart_armed && due(now, restart_at)) ESP.restart();

  display_service(core, now, stats.force_redraw);
  stats.force_redraw = false;
  led_update(core);
}

#ifdef MACROPAD_SELFTEST
// Plants the double-tap words with the core's own code, runs our clear, and
// lets the core put the old contents back (it's heap memory). True if the core
// saw them and then didn't: our address and words are the core's.
bool app_double_tap_check() {
  if (!esp_spiram_is_initialized()) return false;
  double_tap_mark();
  bool planted = double_tap_check_match();
  clear_double_tap();
  bool cleared = !double_tap_check_match();
  double_tap_invalidate();
  return planted && cleared;
}

// The guard ran before the core's check: the core's ~0.9 s LED fade lies
// between the two.
bool app_double_tap_guard_first() {
  return double_tap_guard_us >= 0 && setup_us - double_tap_guard_us >= 800000;
}

bool app_double_tap_cleared() { return double_tap_cleared; }
#endif

}  // namespace mp
