#include "settings.h"

#include <Arduino.h>
#include <Preferences.h>

#include "cdc.h"

namespace mp {
namespace {

constexpr uint32_t IDLE_MS = 1000;
constexpr uint32_t RETRY_MS = 5000;

Preferences prefs;
bool open_ = false;
uint32_t retry_at = 0;
bool retrying = false;

}  // namespace

void settings_begin(Core& c) {
  const Variant& v = c.variant();
#ifdef MACROPAD_SELFTEST
  open_ = prefs.begin("macropad-test", false);  // never touch the real settings
#else
  open_ = prefs.begin("macropad", false);
#endif
  if (!open_) {
    cdc_link().log("settings: NVS unavailable, using defaults");
    return;
  }
  char key[16];
  for (uint8_t t = 0; t < v.ntools; t++) {
    size_t n;
    const Param* p = v.tools[t]->params(n);
    for (size_t i = 0; i < n; i++) {
      if (!settings_key(key, v.tools[t]->id, p[i].id) || !prefs.isKey(key)) continue;
      int32_t x = prefs.getInt(key, *p[i].value);
      if (x < p[i].min) x = p[i].min;
      if (x > p[i].max) x = p[i].max;
      *p[i].value = x;
    }
    v.tools[t]->clamp();
  }
  for (uint8_t k = 0; k < v.nkeymaps; k++)
    if (settings_key(key, v.keymaps[k].id, "live") && prefs.isKey(key))
      c.set_live(k, prefs.getInt(key, 1) != 0);
}

void settings_service(Core& c, uint32_t now) {
  bool flush = c.take_flush();
  if (!open_ || !c.any_dirty()) return;
  if (!flush) {
    // No writes while a keymap is live, and not until edits settle.
    if (c.keymap_live()) return;
    if (now - c.last_edit() < IDLE_MS) return;
    if (retrying && !due(now, retry_at)) return;
  }
  const Variant& v = c.variant();
  bool failed = false;
  char key[16];
  for (uint8_t t = 0; t < v.ntools; t++) {
    uint16_t d = c.dirty(t);
    if (!d) continue;
    size_t n;
    const Param* p = v.tools[t]->params(n);
    for (size_t i = 0; i < n; i++) {
      if (!(d & (1u << i))) continue;
      if (settings_key(key, v.tools[t]->id, p[i].id) && prefs.putInt(key, *p[i].value) == 4)
        c.clean(t, 1u << i);
      else
        failed = true;
    }
  }
  uint8_t ld = c.live_dirty();
  for (uint8_t k = 0; k < v.nkeymaps; k++) {
    if (!(ld & (1u << k))) continue;
    if (settings_key(key, v.keymaps[k].id, "live") && prefs.putInt(key, c.live(k) ? 1 : 0) == 4)
      c.clean_live(1u << k);
    else
      failed = true;
  }
  retrying = failed;
  if (failed) retry_at = now + RETRY_MS;
}

bool settings_read(const char* tool, const char* param, int32_t& value) {
  char key[16];
  if (!open_ || !settings_key(key, tool, param) || !prefs.isKey(key)) return false;
  value = prefs.getInt(key, 0);
  return true;
}

}  // namespace mp
