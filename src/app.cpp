#include "app.h"

#include <Arduino.h>

#include "cdc.h"
#include "display.h"
#include "led.h"
#include "pins.h"
#include "selftest.h"
#include "settings.h"
#include "usb.h"
#include "variant.h"
#include "version.h"

namespace mp {

Input input;
Arbiter hid;
Core core;
LoopStats stats;

namespace {
Rng rng;
uint32_t last_loop_us = 0;
char boot_text[24];
uint32_t boot_nonce = 0;
uint32_t next_serial_poll = 0;
uint32_t next_reply = 0;

// Normal mode answers only HELLO and CONFIG; everything else is dropped.
void serve_serial(uint32_t now) {
  Link& l = cdc_link();
  if (!due(now, next_serial_poll)) return;
  next_serial_poll = now + 10;
  l.poll();
  l.take_overflow();
  const char* line = l.take_line();
  if (!line) return;
  // At most one reply per 250 ms in normal mode; extra lines are dropped.
  if (!due(now, next_reply)) return;
  if (!strcmp(line, "HELLO") || !strcmp(line, "CONFIG")) next_reply = now + 250;
  if (!strcmp(line, "HELLO")) {
    char r[Link::MAX_LINE];
    snprintf(r, sizeof r, "OK\tmacropad\t%s\tproto 1\tmode normal\tcfg none\tcrc none\tboot %08lx",
             MACROPAD_VERSION, (unsigned long)boot_nonce);
    l.reply(r);
  } else if (!strcmp(line, "CONFIG")) {
    l.reply("OK");  // phase 2: enter config mode
  }
}
}  // namespace

void app_setup() {
  cdc_begin();
  boot_nonce = esp_random();
  led_begin();
  pins_begin();
  rng.seed(esp_random());
  core.begin(VARIANT, input, hid, rng);
  settings_begin(core);
  display_begin();
  usb_begin();
  enableLoopWDT();

  snprintf(boot_text, sizeof boot_text, "%s %s", VARIANT.name, MACROPAD_VERSION);
  core.notice(boot_text, millis(), 1500);
  cdc_link().log(boot_text);  // dropped unless a host is already connected
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
  Link& l = cdc_link();
  if (disturbed) l.usb_break();
  l.watch();
  serve_serial(now);

  Event e;
  while (input.pop(e)) core.handle(e, now);
  settings_service(core, now);  // before a newly shown keymap goes live
  core.update(now);
  hid.collect();
  hid.service(usb_port());
  cdc_set_hid_idle(hid.idle());

  text_service();
  l.service();

  display_service(core, now, stats.force_redraw);
  stats.force_redraw = false;
  led_update(core);
}

}  // namespace mp
