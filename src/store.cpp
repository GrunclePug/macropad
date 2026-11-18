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
uint32_t saved_crc = 0;

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
  if (n > CFG_SIZE || n == 0) {
    // Too long (or unreadable length): invalid, never "absent", which would
    // allow a save over it. The zeroed buffer fails validation.
    memset(buf, 0, CFG_SIZE);
    return n ? n : 1;
  }
  if (prefs.getBytes("cfg", buf, n) != n) {
    // The record is there but couldn't be read: invalid (recovery), not
    // "absent".
    memset(buf, 0, CFG_SIZE);
    return n;
  }
  return n;
}

uint8_t store_write(const uint8_t* rec) {
  if (!open_ || prefs.putBytes("cfg", rec, CFG_SIZE) != CFG_SIZE) return E_WRITE;
  static uint8_t back[CFG_SIZE];
  if (prefs.getBytes("cfg", back, CFG_SIZE) != CFG_SIZE || memcmp(back, rec, CFG_SIZE)) return E_VERIFY;
  saved_crc = config_crc(rec);
  if (!old_keys_cleared) {
    for (const char* k : OLD_KEYS)
      if (prefs.isKey(k)) prefs.remove(k);
    old_keys_cleared = true;
  }
  return E_OK;
}

uint32_t store_saved_crc() { return saved_crc; }

void store_erase() {
  if (open_) prefs.remove("cfg");
}

bool store_service(Core& core, const Config& cfg, bool persist, uint32_t now) {
  bool flush = core.take_flush();
  if (!persist || !core.dirty()) return false;
  if (!flush) {
    // No writes while a key screen is live, and not until edits settle.
    if (core.keymap_live()) return false;
    if (now - core.last_edit() < IDLE_MS) return false;
    if (retrying && !due(now, retry_at)) return false;
  }
  static uint8_t rec[CFG_SIZE];
  config_encode(cfg, rec);
  if (store_write(rec) == E_OK) {
    core.clean();
    retrying = false;
    return true;
  }
  retrying = true;
  retry_at = now + RETRY_MS;
  return false;
}

}  // namespace mp
