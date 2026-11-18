#include "app.h"

#include <Arduino.h>

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

uint8_t do_commit(const uint8_t* rec) {
  uint8_t r = store_write(rec);
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
  if (!strcmp(what, "detach")) {
    usb_detach(true);
    delay(1500);
    usb_detach(false);
  }
  if (!strcmp(what, "run")) {
    selftest_restart();
    begin_leave("Self-test");
  }
#else
  (void)what;
#endif
}

void enter_config(bool recovery, bool by_knock = false) {
  knock_entry = by_knock;
  entered_ms = millis();
  core.enter_config(recovery);
  cdc_link().start();
  proto.reset();
  knock.reset();
  env.recovery = recovery;
  // Staging: the stored blob if it validates, otherwise the running config
  // (recovery: the compiled default).
  size_t n = recovery ? 0 : store_read(env.staging);
  if (recovery)
    stage_default();
  else if (!n || config_validate(env.staging, n).code)
    config_encode(cfg, env.staging);
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
  if (!core.recovery() && proto.got_valid() && now - proto.last_valid_ms() > IDLE_CANCEL_MS)
    begin_leave("Timed out");
  // A knock with no app behind it (a crashed client, a stray line toggle).
  if (knock_entry && !proto.got_valid() && now - entered_ms > HANDSHAKE_MS) begin_leave("No app");
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
    } else if (now - leave_at > DRAIN_MS) {
      ESP.restart();  // boots with every tool off
    }
  }
}

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
  env.nonce = nonce;
  env.cfg_status = cfg_status;
  env.has_crc = valid;
  env.stored_crc = valid ? config_crc(blob) : 0;
  env.commit = do_commit;
  env.exit = do_exit;
  env.test = do_test;
#ifdef MACROPAD_SELFTEST
  env.selftest = true;
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
    if (core.recovery()) stage_default();
  }

  Event e;
  while (input.pop(e)) core.handle(e, now);
  if (core.take_config_request()) enter_config(false);

  if (!core.config_mode()) {
    // Normal mode touches no CDC endpoint: only the line state, for the knock.
    bool dtr, rts;
    cdc_line_state(dtr, rts);
    if (knock.feed(dtr, rts, now)) enter_config(false, true);
    store_service(core, cfg, boot.persist, now);  // before a newly shown key screen goes live
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

}  // namespace mp
