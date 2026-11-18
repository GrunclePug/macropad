#include "store.h"

#include <Arduino.h>
#include <Preferences.h>

namespace mp {
namespace {

constexpr uint32_t IDLE_MS = 1000;
constexpr uint32_t RETRY_MS = 5000;

Preferences prefs;
bool open_ = false;
bool retrying = false;
uint32_t retry_at = 0;
bool old_keys_cleared = false;

// Per-parameter keys written by firmware 0.2.0, erased after the first save.
const char* const OLD_KEYS[] = {"click.min", "click.max", "click.btn", "jitter.dist", "jitter.min",
                                "jitter.max", "spam.set",  "spam.min",  "spam.max",    "spam.hold",
                                "hold.btn",   "osu.live",  "deck.live"};

}  // namespace

void store_begin() {
#ifdef MACROPAD_SELFTEST
  open_ = prefs.begin("macropad-test", false);
#else
  open_ = prefs.begin("macropad", false);
#endif
}

size_t store_read(uint8_t* buf) {
  if (!open_ || !prefs.isKey("cfg")) return 0;
  size_t n = prefs.getBytesLength("cfg");
  if (n > CFG_SIZE) n = CFG_SIZE;
  return prefs.getBytes("cfg", buf, n);
}

uint8_t store_write(const uint8_t* rec) {
  if (!open_ || prefs.putBytes("cfg", rec, CFG_SIZE) != CFG_SIZE) return E_WRITE;
  static uint8_t back[CFG_SIZE];
  if (prefs.getBytes("cfg", back, CFG_SIZE) != CFG_SIZE || memcmp(back, rec, CFG_SIZE)) return E_VERIFY;
  if (!old_keys_cleared) {
    for (const char* k : OLD_KEYS)
      if (prefs.isKey(k)) prefs.remove(k);
    old_keys_cleared = true;
  }
  return E_OK;
}

void store_service(Core& core, const Config& cfg, bool persist, uint32_t now) {
  bool flush = core.take_flush();
  if (!persist || !core.dirty()) return;
  if (!flush) {
    // No writes while a key screen is live, and not until edits settle.
    if (core.keymap_live()) return;
    if (now - core.last_edit() < IDLE_MS) return;
    if (retrying && !due(now, retry_at)) return;
  }
  static uint8_t rec[CFG_SIZE];
  config_encode(cfg, rec);
  if (store_write(rec) == E_OK) {
    core.clean();
    retrying = false;
  } else {
    retrying = true;
    retry_at = now + RETRY_MS;
  }
}

}  // namespace mp
